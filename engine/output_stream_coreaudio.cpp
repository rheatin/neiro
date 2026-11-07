// CoreAudio implementation of OutputStream.
//
// The AudioUnit calls us on its own realtime thread and expects us to fill a
// buffer. `nae.cpp`'s render loop has the opposite shape: it asks the device for
// room, writes, and commits. This file is the adapter between the two -- a ring
// the AudioUnit drains, presented to the caller as "room, window, commit".
//
// Why a ring rather than driving the loop from the callback: the loop does far
// more than copy. It decodes, resamples, dithers and updates the counters a host
// reads, none of which may run on a realtime thread. Inverting that would put the
// decoder on the audio thread -- the fault this engine's own notes record against
// a reference implementation. The ring keeps the work where it is and leaves the
// callback with a memcpy.
//
// The cost is one copy per period on macOS. That is the standard shape on this
// platform, and the copy is not on the measured path: the ring absorbs the loop's
// jitter, so the device never runs dry because the loop was late.
#include "audio_backend.h"
#include "output_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace neiro {

namespace {

class CoreAudioOutputStream final : public OutputStream {
  public:
    CoreAudioOutputStream(uint32_t ringFramesHint, uint32_t channels, uint32_t bytesPerFrame,
                          uint32_t rate)
        : ringFrames_([ringFramesHint] {
              uint32_t f = 1;
              while (f < ringFramesHint) f <<= 1;  // the wrap is a mask, on the RT thread
              return f;
          }()),
          mask_(ringFrames_ - 1),
          ring_(static_cast<size_t>(ringFrames_) * bytesPerFrame, 0),
          channels_(channels),
          bytesPerFrame_(bytesPerFrame),
          rate_(rate) {}

    /// Attach the open device. Separate from construction because the render
    /// callback has to exist before `open`, and it needs an object to call.
    void attach(std::unique_ptr<AudioBackend> backend, std::unique_ptr<BackendStream> stream) {
        backend_ = std::move(backend);
        stream_ = std::move(stream);
    }

    uint32_t capacityFrames() const override { return ringFrames_; }
    uint32_t bytesPerFrame() const override { return bytesPerFrame_; }
    uint32_t channels() const override { return channels_; }
    uint32_t rate() const override { return rate_; }
    double latencyMs() const override { return stream_ ? stream_->latencyMs() : 0.0; }
    int64_t framesWritten() const override { return pulled_.load(std::memory_order_relaxed); }
    bool running() const override { return running_.load(std::memory_order_relaxed); }
    uint64_t underruns() const override { return underruns_.load(std::memory_order_relaxed); }
    const char* lastError() const override { return error_.c_str(); }

    StreamStatus acquire(uint32_t want, uint8_t** data, uint32_t* frames) override {
        if (!data || !frames) return StreamStatus::Error;
        if (want > ringFrames_) return StreamStatus::TooLarge;
        const uint64_t write = writePos_.load(std::memory_order_relaxed);
        const uint64_t read = readPos_.load(std::memory_order_acquire);
        const uint64_t freeFrames = ringFrames_ - (write - read);
        if (freeFrames == 0) {
            *frames = 0;
            return StreamStatus::NoRoom;
        }
        // Only the contiguous run to the end of the ring; a caller wanting more
        // asks again after committing. That keeps the window one span.
        const uint32_t offset = static_cast<uint32_t>(write & mask_);
        const uint32_t toEnd = ringFrames_ - offset;
        const uint32_t n = static_cast<uint32_t>(
            std::min<uint64_t>(std::min<uint32_t>(want, toEnd), freeFrames));
        *data = ring_.data() + static_cast<size_t>(offset) * bytesPerFrame_;
        *frames = n;
        return StreamStatus::Ok;
    }

    StreamStatus commit(uint32_t frames, bool silent) override {
        if (frames == 0) return StreamStatus::Ok;
        const uint64_t write = writePos_.load(std::memory_order_relaxed);
        if (silent) {
            const uint32_t offset = static_cast<uint32_t>(write & mask_);
            std::memset(ring_.data() + static_cast<size_t>(offset) * bytesPerFrame_, 0,
                        static_cast<size_t>(frames) * bytesPerFrame_);
        }
        writePos_.store(write + frames, std::memory_order_release);
        room_.notify_one();
        return StreamStatus::Ok;
    }

    bool waitForRoom(uint32_t timeoutMs) override {
        std::unique_lock<std::mutex> lock(mutex_);
        return room_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            const uint64_t write = writePos_.load(std::memory_order_acquire);
            const uint64_t read = readPos_.load(std::memory_order_acquire);
            return ringFrames_ - (write - read) > 0;
        });
    }

    StreamStatus start() override {
        std::string error;
        if (!stream_ || !stream_->start(&error)) {
            error_ = error.empty() ? "the device refused to start" : error;
            return StreamStatus::Error;
        }
        running_.store(true, std::memory_order_relaxed);
        return StreamStatus::Ok;
    }

    StreamStatus stop() override {
        if (stream_) stream_->stop();
        running_.store(false, std::memory_order_relaxed);
        return StreamStatus::Ok;
    }

    /// The AudioUnit's thread. No allocation, no logging, no blocking.
    size_t pullBytes(void* out, size_t frames) {
        const uint64_t read = readPos_.load(std::memory_order_relaxed);
        const uint64_t write = writePos_.load(std::memory_order_acquire);
        const uint64_t available = write - read;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(frames, available));
        auto* dst = static_cast<uint8_t*>(out);
        if (n > 0) {
            const uint32_t offset = static_cast<uint32_t>(read & mask_);
            const size_t first = std::min<size_t>(n, ringFrames_ - offset);
            std::memcpy(dst, ring_.data() + static_cast<size_t>(offset) * bytesPerFrame_,
                        first * bytesPerFrame_);
            if (first < n) {
                std::memcpy(dst + first * bytesPerFrame_, ring_.data(),
                            (n - first) * bytesPerFrame_);
            }
            readPos_.store(read + n, std::memory_order_release);
        }
        if (n < frames) {
            std::memset(dst + n * bytesPerFrame_, 0, (frames - n) * bytesPerFrame_);
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        pulled_.fetch_add(static_cast<int64_t>(n), std::memory_order_relaxed);
        room_.notify_one();
        return n;
    }

  private:
    std::unique_ptr<AudioBackend> backend_;
    std::unique_ptr<BackendStream> stream_;
    uint32_t ringFrames_ = 0;
    uint32_t mask_ = 0;
    std::vector<uint8_t> ring_;
    uint32_t channels_ = 0;
    uint32_t bytesPerFrame_ = 0;
    uint32_t rate_ = 0;

    std::atomic<uint64_t> writePos_{0};
    std::atomic<uint64_t> readPos_{0};
    std::atomic<int64_t> pulled_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::condition_variable room_;
    std::string error_;
};

}  // namespace

std::shared_ptr<OutputStream> openCoreAudioStream(const StreamFormatRequest& want,
                                                  uint32_t ringFrames, std::string* error) {
    auto backend = createPlatformBackend();
    if (!backend) {
        if (error) *error = "no CoreAudio backend";
        return nullptr;
    }

    const uint32_t bytesPerFrame = want.channels * ((want.containerBits + 7) / 8);
    auto stream = std::make_shared<CoreAudioOutputStream>(ringFrames ? ringFrames : 8192,
                                                          want.channels, bytesPerFrame,
                                                          want.rate);
    std::weak_ptr<CoreAudioOutputStream> weak = stream;

    BackendOpenRequest request;
    request.deviceId = want.deviceId;
    request.resolved.sampleRate = static_cast<int32_t>(want.rate);
    request.resolved.channels = static_cast<int32_t>(want.channels);
    request.resolved.containerBits = want.containerBits;
    request.resolved.validBits = want.validBits;
    request.resolved.isFloat = want.isFloat ? 1 : 0;
    request.resolved.shareMode = want.shareMode;
    request.resolved.sampleFormat =
        want.isFloat ? NAE_SAMPLE_FORMAT_F32
                     : (want.containerBits == 24 ? NAE_SAMPLE_FORMAT_S24
                                                 : (want.containerBits == 16
                                                        ? NAE_SAMPLE_FORMAT_S16
                                                        : NAE_SAMPLE_FORMAT_S32));
    request.bufferFrames = want.bufferFrames;
    request.render = [weak](void* out, size_t frames) -> size_t {
        if (auto self = weak.lock()) return self->pullBytes(out, frames);
        return 0;
    };

    auto deviceStream = backend->open(request, error);
    if (!deviceStream) return nullptr;

    stream->attach(std::move(backend), std::move(deviceStream));
    return stream;
}

}  // namespace neiro
