// The device half of the render loop, in one shape on every platform.
//
// `nae.cpp` drives playback: it waits for the device to have room, fills the
// window it is handed, and commits it. WASAPI offers that shape directly
// (`GetBuffer` / `ReleaseBuffer`). CoreAudio does not -- it calls *you* from its
// own thread -- so the macOS implementation keeps a ring that the AudioUnit
// drains and presents the same "room, window, commit" shape on top of it.
//
// The engine code above this header therefore has no platform branches: the same
// loop, the same dither, the same position accounting, and the same counters run
// on both. That is the whole point. A second render loop per platform would be a
// second copy of the DSP, and the two would drift.
//
// What the interface deliberately does NOT do:
//   * It does not convert formats. The caller writes final device bytes, in the
//     format the stream was opened with, exactly as `RenderCallback` documents.
//   * It does not decide silence. `commit(frames, silent)` says the device may
//     play those frames without the caller having written them, which is what
//     AUDCLNT_BUFFERFLAGS_SILENT means and what a pause needs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace neiro {

enum class StreamStatus {
    Ok,
    /// Nothing free right now. Wait, then try again; this is not an error.
    NoRoom,
    /// Asked for more frames than this device can ever hold. A caller bug.
    TooLarge,
    /// The endpoint is gone: unplugged, or taken by another program in exclusive
    /// mode. Playback pauses; the handle, decoder and position stay valid.
    DeviceLost,
    Error,
};

class OutputStream {
  public:
    virtual ~OutputStream() = default;

    virtual uint32_t capacityFrames() const = 0;
    virtual uint32_t bytesPerFrame() const = 0;
    virtual uint32_t channels() const = 0;
    /// The format actually in force, for the counters and the signal path.
    virtual uint32_t rate() const = 0;

    /// Non-blocking. Hands back a writable window of at most `want` frames and
    /// sets `*frames` to how many it got. The window is contiguous; a caller
    /// that needs more calls again after committing.
    virtual StreamStatus acquire(uint32_t want, uint8_t** data, uint32_t* frames) = 0;

    /// Commits what the caller wrote. `silent` says the device may play these
    /// frames without the data having been filled in; the region is then
    /// undefined and the device must not read it.
    virtual StreamStatus commit(uint32_t frames, bool silent) = 0;

    /// Blocks until the device has consumed at least one frame, or the timeout
    /// expires. False means nothing moved in time -- the caller counts that, it
    /// is not an error by itself.
    virtual bool waitForRoom(uint32_t timeoutMs) = 0;

    virtual StreamStatus start() = 0;
    virtual StreamStatus stop() = 0;
    virtual bool running() const = 0;

    /// The buffer's own duration: how much audio the device holds on our behalf.
    virtual double latencyMs() const = 0;

    /// Latency downstream of the buffer, as the driver reports it right now.
    ///
    /// Read per call rather than cached at open, because the device is allowed to
    /// change it while the stream runs. Zero where the platform has no such figure
    /// -- that is "not knowable here", not "no latency"; the caller reports the
    /// two differently.
    virtual double driverLatencyMs() const { return 0.0; }

    /// Frames the device still has queued for playback, when the backend can say.
    ///
    /// WASAPI tracks its own committed frames per buffer, so the render loop asks
    /// for `capacityFrames() - queuedFrames()` and gets the room that is actually
    /// free. A backend that cannot answer returns zero, which the loop reads as a
    /// whole free buffer rather than inventing a figure.
    virtual uint32_t queuedFrames() const { return 0; }

    /// Device frames handed over since start, including silence. This is the one
    /// quantity a position and a rate window are both derived from.
    virtual int64_t framesWritten() const = 0;

    /// Times the device asked for frames the stream did not have. A non-zero
    /// value means the listener may have heard a gap, so it has to be visible to
    /// the host rather than counted and dropped.
    virtual uint64_t underruns() const = 0;

    /// Human-readable reason for the last non-Ok status.
    virtual const char* lastError() const = 0;
};

/// What to open: the format the resolver chose, and how much ring to keep.
struct StreamFormatRequest {
    std::string deviceId;
    uint32_t rate = 48000;
    uint32_t channels = 2;
    uint32_t containerBits = 24;
    uint32_t validBits = 24;
    bool isFloat = false;
    /// Buffer to ask for, in frames at `rate`. Zero asks for the backend's own
    /// default, which is what every ordinary open does: an explicit period on
    /// this hardware measures worse, not better.
    uint32_t bufferFrames = 0;
    /// `NAE_SHARE_MODE_*`. On macOS this is what decides whether the device is
    /// taken away from other processes: exclusive is `kAudioDevicePropertyHogMode`,
    /// and a shared stream must leave the device alone so every other program can
    /// still make sound.
    int32_t shareMode = 2;  // NAE_SHARE_MODE_SHARED
};

/// macOS: the ring-backed stream over CoreAudio.
std::shared_ptr<OutputStream> openCoreAudioStream(const StreamFormatRequest& want,
                                                  uint32_t ringFrames, std::string* error);

/// What the device granted, measured after the format was actually negotiated.
///
/// The engine fills its handle from this, so the answer the listener is shown is
/// the device's own rather than a copy of what was asked for. `latencyMs` is the
/// buffer's duration (`bufferFrames / rate`), which is the figure the signal path
/// reports as the buffer part of the total.
struct OpenedFormat {
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint32_t containerBits = 0;
    /// As the device reported it. Zero means the format carried no valid-bits
    /// field; the caller falls back to `containerBits`, which is how the device
    /// will interpret the samples.
    uint32_t validBits = 0;
    bool isFloat = false;
    uint32_t bufferFrames = 0;
    double latencyMs = 0.0;

    /// True when the device signals the next period instead of the caller
    /// polling a timer. It is reported rather than assumed: on an endpoint that
    /// refuses event callbacks the stream still plays, and the only way to know
    /// which pacing a run used is to ask.
    bool eventDriven = false;
    /// Why event-driven pacing was not used. Empty when it was.
    std::string pacingNote;
};

/// Windows: the WASAPI stream, opened with the format the resolver chose.
///
/// The stream owns the COM clients, the render event and the format; nothing in
/// `nae.cpp` reaches for `IAudioClient` any more. A null return means the device
/// refused and `*error` says why, in the words the listener should read.
///
/// `want.bufferFrames` is honoured as an explicit buffer duration; zero asks for
/// the device's own default. A refused explicit duration falls back to the default
/// and is reported through `*error` even on success, because it is a fact about
/// the stream rather than a failure.
std::shared_ptr<OutputStream> openWasapiStream(const StreamFormatRequest& want,
                                               OpenedFormat* opened, std::string* error);

}  // namespace neiro
