// Does the macOS device path actually move frames?
//
// Opens the output stream, offers it a second of audio in the same "ask for
// room, write, commit" shape the engine's render loop uses, and checks that the
// device consumed it. Measured on the counter the AudioUnit increments, not on a
// callback having fired: `framesWritten` is what the device took.
//
// A shortfall is the interesting case. If the device pulls fewer frames than were
// committed, either the format is wrong, the stream never started, or the ring
// and the AudioUnit disagree about the frame width -- all of which are silent in
// a program that only checks "the callback ran".
#include "output_stream.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const uint32_t seconds = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 2;
    neiro::StreamFormatRequest want;
    want.rate = 48000;
    want.channels = 2;
    want.containerBits = 32;
    want.validBits = 32;
    want.isFloat = true;  // the built-in device's only physical format

    std::string error;
    auto stream = neiro::openCoreAudioStream(want, 8192, &error);
    if (!stream) {
        std::printf("open failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("opened %u Hz, %u ch, %u-bit %s, ring %u frames, latency %.1f ms\n",
                stream->rate(), stream->channels(), stream->bytesPerFrame() * 8 / stream->channels(),
                want.isFloat ? "float" : "int", stream->capacityFrames(), stream->latencyMs());

    if (stream->start() != neiro::StreamStatus::Ok) {
        std::printf("start failed: %s\n", stream->lastError());
        return 1;
    }

    const uint32_t total = want.rate * seconds;  // how much audio to feed in all
    const auto started = std::chrono::steady_clock::now();
    int64_t lastSample = 0;
    auto lastSampleAt = started;
    uint32_t committed = 0;
    std::vector<uint8_t> scratch(stream->bytesPerFrame() * 512);

    // Feed for `seconds` of wall clock, not `seconds` worth of frames.
    //
    // The difference is the measurement. Handing over a second of audio and
    // watching the device take it says nothing about the rate: the HAL may pull
    // several periods ahead, so the counter jumps and then stops. What has to be
    // true is that the device consumes about `rate` frames per second of wall
    // clock while the stream is running -- that is the clock the listener hears.
    const double wallSeconds = static_cast<double>(seconds);
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() <
           wallSeconds) {
        uint8_t* window = nullptr;
        uint32_t frames = 0;
        const auto status = stream->acquire(512, &window, &frames);
        if (status == neiro::StreamStatus::NoRoom) {
            if (!stream->waitForRoom(200)) {
                std::printf("the device stopped taking room after %u frames\n", committed);
                break;
            }
            continue;
        }
        if (status != neiro::StreamStatus::Ok) {
            std::printf("acquire failed: status %d\n", static_cast<int>(status));
            break;
        }
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t c = 0; c < stream->channels(); ++c) {
                const float v = static_cast<float>((committed + f + c) % 1000) / 1000.0f;
                std::memcpy(window + (f * stream->channels() + c) * sizeof(float), &v,
                            sizeof(float));
            }
        }
        stream->commit(frames, false);
        committed += frames;
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const int64_t consumed = stream->framesWritten();
    const double fps = elapsed > 0.0 ? static_cast<double>(consumed) / elapsed : 0.0;
    std::printf("fed %.2f s of audio in %.2f s of wall clock\n", static_cast<double>(committed) / want.rate,
                elapsed);
    std::printf("device consumed %.0f frames/s against a %.0f frame/s format (%+.1f%%)\n", fps,
                static_cast<double>(want.rate), (fps / want.rate - 1.0) * 100.0);
    // Timeline: a device that stopped pulling and a counter that stopped being
    // read look identical at one instant.
    int64_t previous = -1;
    for (int i = 0; i < 10; ++i) {
        const int64_t now = stream->framesWritten();
        std::printf("  t=%d00 ms  taken %lld%s\n", i, static_cast<long long>(now),
                    (previous >= 0 && now == previous) ? "  (stalled)" : "");
        previous = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const int64_t taken = stream->framesWritten();
    stream->stop();

    std::printf("committed %u frames, device took %lld\n", committed,
                static_cast<long long>(taken));
    // The device has to be within 5% of its own clock. A burst that outruns
    // wall clock means nothing is actually being clocked, and a starved one means
    // the ring is the bottleneck; both are faults, and both read as "it passed"
    // if the only question asked is whether the counter moved.
    const bool ok = fps > want.rate * 0.95 && fps < want.rate * 1.05;
    std::printf("%s\n", ok ? "output stream check passed" : "output stream check FAILED");
    return ok ? 0 : 1;
}
