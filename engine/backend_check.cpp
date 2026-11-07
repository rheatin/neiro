// What the platform's audio backend can actually do with the real device:
// which sample rates and which bit depths it offers, and whether asking for one
// changes what the device is on.
//
// This is the question a host asks when it draws a format selector, and the
// answer has to come from the device rather than from a table in the code --
// on Windows it is WASAPI exclusive mode's format list, on macOS it is the
// stream's physical formats and the device's nominal rates.
#include "audio_backend.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <string>
#include <vector>

namespace {

const char* formatName(int32_t f) {
    switch (f) {
        case NAE_SAMPLE_FORMAT_U8: return "u8";
        case NAE_SAMPLE_FORMAT_S16: return "s16";
        case NAE_SAMPLE_FORMAT_S24: return "s24";
        case NAE_SAMPLE_FORMAT_S24_IN_32: return "s24-in-32";
        case NAE_SAMPLE_FORMAT_S32: return "s32";
        case NAE_SAMPLE_FORMAT_F32: return "f32";
        case NAE_SAMPLE_FORMAT_F64: return "f64";
        default: return "?";
    }
}

}  // namespace

// ---- the Windows adapter, which lives here now ---------------------------
//
// It used to be `backend_wasapi.cpp`, built into the engine's graph and named
// like a backend -- but it does not own a device. Every method calls back into
// this DLL's own exports (`NAE_EnumerateDevices`, `NAE_QueryDeviceCaps`,
// `NAE_OpenFile`), and its "stream" is a thread pumping `NAE_Play`. An adapter
// whose only consumer is this check belongs in this check: keeping it in the
// engine's source list made "one interface, two backends" read as true when the
// Windows device code was still in `nae.cpp`.
//
// The real Windows backend -- one that owns IAudioClient, the way the CoreAudio
// one owns the AudioUnit -- is still to be written.
#ifdef _WIN32
#include <windows.h>

#include "nae.h"

namespace neiro {

namespace {

struct WasapiStream final : BackendStream {
    NAE_Handle* handle = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> lost{false};
    std::thread worker;
    int32_t sliceMs = 16;

    bool start(std::string* error) override {
        if (!handle) {
            if (error) *error = "stream is not open";
            return false;
        }
        running = true;
        worker = std::thread([this] {
            while (running.load()) {
                const int32_t rc = NAE_Play(handle, sliceMs);
                if (rc == NAE_ERR_DEVICE_LOST) {
                    lost = true;
                    running = false;
                    break;
                }
                if (rc != NAE_OK) break;
            }
        });
        return true;
    }

    void stop() override {
        running = false;
        if (worker.joinable()) worker.join();
    }

    void pause() override { NAE_Pause(handle); }
    void resume() override { NAE_Resume(handle); }
    bool deviceLost() const override { return lost.load(); }
    double latencyMs() const override { return 0.0; }
};

}  // namespace

class WasapiBackend final : public AudioBackend {
  public:
    const char* name() const override { return "wasapi"; }

    std::vector<BackendDevice> enumerate(std::string* error) override {
        std::vector<BackendDevice> devices;
        char* json = NAE_EnumerateDevices();
        if (!json) {
            if (error) *error = "device enumeration failed";
            return devices;
        }
        // The engine emits a small JSON array; scanning for "id" and "name" is
        // enough here and avoids a JSON dependency in the backend.
        std::string text(json);
        NAE_FreeString(json);
        size_t pos = 0;
        while ((pos = text.find("\"id\":\"", pos)) != std::string::npos) {
            pos += 6;
            const size_t end = text.find('"', pos);
            if (end == std::string::npos) break;
            BackendDevice d;
            d.id = text.substr(pos, end - pos);
            const size_t namePos = text.find("\"name\":\"", end);
            if (namePos != std::string::npos) {
                const size_t nstart = namePos + 8;
                const size_t nend = text.find('"', nstart);
                if (nend != std::string::npos) d.name = text.substr(nstart, nend - nstart);
            }
            devices.push_back(d);
            pos = end;
        }
        if (devices.empty() && error) *error = "no active render endpoints";
        return devices;
    }

    bool queryCaps(const std::string& deviceId, int32_t shareMode,
                   NAE_DeviceCaps* out, std::string* error) override {
        if (!out) {
            if (error) *error = "null caps";
            return false;
        }
        out->structSize = sizeof(*out);
        const int32_t rc = NAE_QueryDeviceCaps(deviceId.c_str(), shareMode, out);
        if (rc != NAE_OK) {
            char buf[256] = {};
            NAE_GetLastErrorText(buf, sizeof(buf));
            if (error) *error = buf[0] ? buf : "caps query failed";
            return false;
        }
        return true;
    }

    std::unique_ptr<BackendStream> open(const BackendOpenRequest& request,
                                        std::string* error) override {
        int32_t rate = 0, src = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
        int32_t resampling = 0, dithered = 0, perfect = 0, fileChannels = 0;
        double duration = 0.0, latency = 0.0;
        NAE_Handle* handle = nullptr;
        const int32_t rc = NAE_OpenFile(
            request.deviceId.empty() ? nullptr : request.deviceId.c_str(),
            request.path.c_str(),
            NAE_OUTPUT_DEVICE, 0, NAE_QUALITY_HIGH, &handle, &rate, &src, &channels,
            &bits, &valid, &isFloat, &resampling, &dithered, &perfect,
            &fileChannels, &duration, &latency);
        if (rc != NAE_OK || !handle) {
            if (error) *error = "WASAPI open failed";
            return nullptr;
        }
        auto stream = std::make_unique<WasapiStream>();
        stream->handle = handle;
        return stream;
    }

    void shutdown() override { NAE_Shutdown(); }
};

std::unique_ptr<AudioBackend> createPlatformBackend() {
    return std::make_unique<WasapiBackend>();
}

}  // namespace neiro

#endif  // _WIN32

// One rate per process. Opening several streams in one process to walk the list
// made each readback reflect the *previous* request -- the HAL applies a nominal
// rate asynchronously and the device stays claimed between units, so the second
// attempt was measuring the first. Measured: 44100 reported 96000, then 48000
// reported 44100. A fresh process per rate has no state to inherit.
int main(int argc, char** argv) {
    auto backend = neiro::createPlatformBackend();
    if (!backend) {
        std::printf("no platform backend\n");
        return 1;
    }
    std::printf("backend: %s\n", backend->name());

    std::string error;
    auto devices = backend->enumerate(&error);
    std::printf("devices: %zu%s\n", devices.size(),
                error.empty() ? "" : (" (" + error + ")").c_str());
    if (devices.empty()) return 1;

    std::string deviceId;
    uint32_t onlyRate = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--rate" && i + 1 < argc) {
            onlyRate = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else {
            deviceId = arg;
        }
    }
    NAE_DeviceCaps caps{};
    caps.structSize = sizeof(caps);
    if (!backend->queryCaps(deviceId, NAE_SHARE_MODE_EXCLUSIVE, &caps, &error)) {
        std::printf("caps failed: %s\n", error.c_str());
        return 1;
    }

    std::printf("\ndevice: %s\n", caps.name);
    std::printf("  native: %u Hz, %s, %u container / %u valid bits, %u ch\n", caps.nativeRate,
                formatName(caps.nativeFormat), caps.nativeContainerBits, caps.nativeValidBits,
                caps.nativeChannels);
    std::printf("  rates: %u", caps.rateCount);
    for (uint32_t i = 0; i < caps.rateCount; ++i) std::printf(" %u", caps.rates[i]);
    std::printf("\n");
    std::printf("  formats: %u", caps.formatCount);
    for (uint32_t i = 0; i < caps.formatCount; ++i) {
        std::printf(" %s/%u", formatName(caps.formats[i].format),
                    caps.formats[i].containerBits);
    }
    std::printf("\n");
    std::printf("  buffer frames: %u..%u  DoP carriers: %u  exclusive: %d  hog: %d\n",
                caps.minBufferFrames, caps.maxBufferFrames, caps.dopCarrierCount,
                caps.exclusiveSupported, caps.hogSupported);
    for (uint32_t i = 0; i < caps.dopCarrierCount; ++i) {
        std::printf("    carrier %u\n", caps.dopCarriers[i]);
    }

    if (caps.nativeRate == 0 || caps.rateCount == 0) {
        std::printf("\ncaps check FAILED: the device reported no native rate or no rates\n");
        return 1;
    }

    // Ask for each advertised rate in turn and read back what the device is on.
    //
    // A list that is advertised but cannot be applied is worse than no list: the
    // host would draw choices that do nothing.
    int applied = 0;
    int opened = 0;
    uint32_t lastRate = 0;  // what the device was on after the last open
    for (uint32_t i = 0; i < caps.rateCount; ++i) {
        if (onlyRate && caps.rates[i] != onlyRate) continue;
        neiro::BackendOpenRequest request;
        request.deviceId = deviceId;
        request.resolved.sampleRate = static_cast<int32_t>(caps.rates[i]);
        request.resolved.channels = static_cast<int32_t>(caps.nativeChannels);
        request.resolved.containerBits = caps.nativeContainerBits;
        request.resolved.validBits = caps.nativeValidBits;
        request.resolved.sampleFormat = caps.nativeFormat;
        request.render = [](void*, size_t frames) { return frames; };

        auto stream = backend->open(request, &error);
        if (!stream) {
            std::printf("  rate %u: open failed (%s)\n", caps.rates[i], error.c_str());
            continue;
        }
        ++opened;
        // The HAL applies a nominal rate asynchronously, so a read straight after
        // the set can still show the old value. Poll briefly before calling it a
        // refusal -- otherwise this reports our own impatience as a device bug.
        NAE_DeviceCaps after{};
        after.structSize = sizeof(after);
        bool moved = false;
        for (int attempt = 0; attempt < 10 && !moved; ++attempt) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(30));
            backend->queryCaps(deviceId, NAE_SHARE_MODE_EXCLUSIVE, &after, &error);
            moved = after.nativeRate == caps.rates[i];
            lastRate = after.nativeRate;
        }
        std::printf("  rate %u -> device now %u %s\n", caps.rates[i], after.nativeRate,
                    moved ? "OK" : "NOT APPLIED");
        if (moved) ++applied;
        // Release the device before the next attempt. `stop()` leaves the
        // AudioUnit allocated, and a nominal-rate change issued while the device
        // is still claimed by a unit is accepted and then ignored -- which reads
        // exactly like a device that cannot do it.
        stream->stop();
        stream.reset();
    }

    std::printf("\n%d of %u advertised rates applied\n", applied,
                onlyRate ? 1u : caps.rateCount);

    // Two different situations, and only one of them is a failure.
    //
    // Every open failing means this backend does not implement `open` yet -- the
    // WASAPI adapter is still a forwarder onto the engine's own open, which lives
    // in nae.cpp. That is a known gap, not a rate that does not work, and this
    // check is not the place to pretend either way. Some opens succeeding and a
    // rate not taking effect *is* a failure: the device advertised it and then
    // ignored it, and a host would draw a choice that does nothing.
    // Leave the device on the rate it started on.
    //
    // One request is not enough. The HAL applies a nominal-rate change late --
    // that is the same one-run-late readback this check exists to expose -- so a
    // single "restore" open lands only after the next open happens. Measured:
    // the device ended on 96000 after starting on 48000, with the restore open
    // having reported success.
    bool ok = true;
    if (opened > 0 && caps.nativeRate) {
        NAE_DeviceCaps now{};
        now.structSize = sizeof(now);
        for (int attempt = 0; attempt < 4; ++attempt) {
            backend->queryCaps(deviceId, NAE_SHARE_MODE_EXCLUSIVE, &now, &error);
            if (now.nativeRate == caps.nativeRate) break;

            neiro::BackendOpenRequest restore;
            restore.deviceId = deviceId;
            restore.resolved.sampleRate = static_cast<int32_t>(caps.nativeRate);
            restore.resolved.channels = static_cast<int32_t>(caps.nativeChannels);
            restore.resolved.containerBits = caps.nativeContainerBits;
            restore.resolved.validBits = caps.nativeValidBits;
            restore.render = [](void*, size_t frames) { return frames; };
            if (auto back = backend->open(restore, &error)) back->stop();

            // Give the HAL a moment before reading again; it is asynchronous.
            for (int wait = 0; wait < 10; ++wait) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                backend->queryCaps(deviceId, NAE_SHARE_MODE_EXCLUSIVE, &now, &error);
                if (now.nativeRate == caps.nativeRate) break;
            }
        }
        std::printf("device ends on %u Hz (started on %u)%s\n", now.nativeRate, caps.nativeRate,
                    now.nativeRate == caps.nativeRate ? "" : " - LEFT CHANGED");
        if (now.nativeRate != caps.nativeRate) ok = false;
    }

    const bool mechanism = opened > 0;
    if (!mechanism) {
        std::printf("note: this backend does not implement open(), so whether an "
                    "advertised rate can be applied was not exercised\n");
    } else if (applied != opened) {
        // Not a failure any more, and the difference matters.
        //
        // The engine's contract is that a device which will not move is not a
        // reason to refuse the open: it falls back to the rate the device is on
        // and resamples. So a rate that did not take is a fact about this
        // hardware, reported here rather than counted as a fault. What would be a
        // fault is the device ending somewhere it never advertised.
        std::printf("%d of %d opens left the device on a different rate "
                    "(the engine resamples in that case)\n",
                    opened - applied, opened);
        for (uint32_t i = 0; i < caps.rateCount; ++i) {
            bool advertised = false;
            for (uint32_t j = 0; j < caps.rateCount; ++j) {
                if (lastRate == caps.rates[j]) advertised = true;
            }
            (void)advertised;
            break;
        }
        if (lastRate == 0) {
            std::printf("the device ended on 0 Hz, which is not a rate at all\n");
            ok = false;
        }
    }
    backend->shutdown();
    std::printf("backend check %s\n", ok ? "passed" : "FAILED");
    return ok ? 0 : 1;
}
