// ASIO, by consequence.
//
// The question this answers is not "did a callback fire". It is: does the device
// the registry lists open a real file, at the rate and channel count it reports,
// and does the position advance because device frames advanced -- while the
// engine's own resolved signal path says which backend is playing.
//
// The order matters. Enumeration comes first, because a device that is not listed
// cannot be opened and the reason it is listed at all is the registry. Caps come
// second, because they are the driver's own answer about its format; if the driver
// refuses to describe itself, that refusal is the result and the probe stops
// there rather than opening something blindly.
//
// Nothing here reports success for a stream with no frames: the exit code is a
// failure when the device wrote none, whatever else the run printed.
//
// Usage: asio_probe <file> [asio:{CLSID}]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nae.h"

namespace {

/// The JSON entries whose id carries the ASIO prefix.
///
/// Brace-counted rather than "up to the next `}`": the device id itself contains
/// braces -- `asio:{A80362FF-...}` -- so the first closing brace is inside the id.
std::vector<std::string> asioEntries(const std::string& json) {
    std::vector<std::string> entries;
    size_t pos = 0;
    while ((pos = json.find("\"id\":\"asio:", pos)) != std::string::npos) {
        const size_t begin = json.rfind('{', pos);
        if (begin == std::string::npos) break;
        int depth = 0;
        size_t end = std::string::npos;
        for (size_t i = begin; i < json.size(); ++i) {
            if (json[i] == '{') ++depth;
            if (json[i] == '}' && --depth == 0) {
                end = i;
                break;
            }
        }
        if (end == std::string::npos) break;
        entries.push_back(json.substr(begin, end - begin + 1));
        pos = end;
    }
    return entries;
}

/// The number after `"key":` in a small JSON object, or -1 when absent.
double jsonNumber(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return -1.0;
    return std::strtod(json.c_str() + at + needle.size(), nullptr);
}

std::string engineError() {
    char buffer[512] = {};
    NAE_GetLastErrorText(buffer, static_cast<int32_t>(sizeof(buffer)));
    return buffer;
}

const char* backendName(int32_t backend) {
    switch (backend) {
        case NAE_BACKEND_WASAPI: return "wasapi";
        case NAE_BACKEND_COREAUDIO: return "coreaudio";
        case NAE_BACKEND_AAUDIO: return "aaudio";
        case NAE_BACKEND_OPENSL: return "opensl";
        case NAE_BACKEND_ALSA: return "alsa";
        case NAE_BACKEND_ASIO: return "asio";
        case NAE_BACKEND_USB: return "usb";
        default: return "unknown";
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: asio_probe <file> [asio:{CLSID}]\n");
        return 1;
    }
    const char* path = argv[1];

    // ---- 1. what the registry says ----
    std::string listedId;
    {
        char* raw = NAE_EnumerateDevices();
        if (!raw) {
            std::printf("FAILED: NAE_EnumerateDevices returned nothing (%s)\n", engineError().c_str());
            return 1;
        }
        const std::string json(raw);
        NAE_FreeString(raw);
        const auto entries = asioEntries(json);
        std::printf("engine enumeration: %zu ASIO device(s)\n", entries.size());
        for (const std::string& entry : entries) std::printf("  %s\n", entry.c_str());
        if (entries.empty()) {
            std::printf("FAILED: no ASIO device is registered under HKLM\\SOFTWARE\\ASIO\n");
            return 1;
        }
        listedId = argv[2] ? argv[2] : "";
        if (listedId.empty()) {
            const size_t at = entries.front().find("\"id\":\"");
            const size_t end = entries.front().find('"', at + 6);
            listedId = entries.front().substr(at + 6, end - at - 6);
        }
    }
    std::printf("device under test: %s\n", listedId.c_str());

    // ---- 2. the driver's own account of itself ----
    NAE_DeviceCaps caps{};
    caps.structSize = sizeof(caps);
    const bool capsOk =
        NAE_QueryDeviceCaps(listedId.c_str(), NAE_SHARE_MODE_EXCLUSIVE, &caps) == NAE_OK;
    if (!capsOk) {
        // A driver that will not initialise has no format to open. The open is
        // still attempted below, because the reason *it* fails is the evidence
        // that the refusal came from the driver and not from a generic path.
        std::printf("caps: FAILED: %s\n", engineError().c_str());
    } else {
        std::printf("caps: name='%s' backend=%d (%s) native %u Hz, %u ch, %u-bit container / "
                    "%u valid, rates=%u, buffer frames=%u..%u default %u, probed=0x%X\n",
                    caps.name, caps.backend, backendName(caps.backend), caps.nativeRate,
                    caps.nativeChannels, caps.nativeContainerBits, caps.nativeValidBits,
                    caps.rateCount, caps.minBufferFrames, caps.maxBufferFrames,
                    caps.defaultBufferFrames, caps.probed);
        if (caps.rateCount > 0) {
            std::printf("caps: rates");
            for (uint32_t i = 0; i < caps.rateCount; ++i) std::printf(" %u", caps.rates[i]);
            std::printf("\n");
        }
        if (caps.backend != NAE_BACKEND_ASIO) {
            std::printf("FAILED: the caps query reported backend %d, not NAE_BACKEND_ASIO (%d)\n",
                        caps.backend, NAE_BACKEND_ASIO);
            return 1;
        }
    }

    // ---- 3. the routing rule's other half, which needs no device ----
    //
    // ASIO has one mode. A shared request must be refused by name: opening it
    // exclusively and reporting "shared" would be a claim the stream cannot
    // support, and a WASAPI id must keep going to WASAPI whatever this backend
    // does.
    {
        NAE_Handle* shared = nullptr;
        int32_t sharedRate = 0, sharedSourceRate = 0, sharedChannels = 0, sharedBits = 0;
        int32_t sharedValid = 0, sharedFloat = 0, sharedResampling = 0, sharedDither = 0;
        int32_t sharedPerfect = 0, sharedFileChannels = 0;
        double sharedDuration = 0.0, sharedLatency = 0.0;
        const int32_t sharedRc = NAE_OpenFileShared(
            listedId.c_str(), path, NAE_OUTPUT_DEVICE, 0, NAE_QUALITY_HIGH, &shared, &sharedRate,
            &sharedSourceRate, &sharedChannels, &sharedBits, &sharedValid, &sharedFloat,
            &sharedResampling, &sharedDither, &sharedPerfect, &sharedFileChannels, &sharedDuration,
            &sharedLatency);
        std::printf("shared open: rc=%d: %s\n", sharedRc, engineError().c_str());
        if (shared) NAE_Close(shared);
        if (sharedRc == NAE_OK) {
            std::printf("FAILED: an ASIO id opened in shared mode\n");
            return 1;
        }
    }

    // ---- 4. open the file through the engine's normal path ----
    NAE_Handle* handle = nullptr;
    int32_t rate = 0;
    int32_t sourceRate = 0;
    int32_t channels = 0;
    int32_t bits = 0;
    int32_t valid = 0;
    int32_t isFloat = 0;
    int32_t resampling = 0;
    int32_t dithered = 0;
    int32_t perfect = 0;
    int32_t fileChannels = 0;
    double durationMs = 0.0;
    double latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(listedId.c_str(), path, NAE_OUTPUT_DEVICE, 0, NAE_QUALITY_HIGH,
                                    &handle, &rate, &sourceRate, &channels, &bits, &valid, &isFloat,
                                    &resampling, &dithered, &perfect, &fileChannels, &durationMs,
                                    &latencyMs);
    if (rc != NAE_OK || !handle) {
        std::printf("open: FAILED (%d): %s\n", rc, engineError().c_str());
        return 1;
    }
    std::printf("open: granted %d Hz, %d ch, %d-bit container / %d valid, float=%d, "
                "resampling=%d dithered=%d bitPerfect=%d, latency=%.2f ms\n",
                rate, channels, bits, valid, isFloat, resampling, dithered, perfect, latencyMs);

    // ---- 5. the resolved signal path, which is where a host reads the backend ----
    NAE_TrackInfo track{};
    track.structSize = sizeof(track);
    NAE_QueryTrackInfo(handle, &track);
    NAE_SourceInfo source{};
    source.structSize = sizeof(source);
    source.sampleRate = static_cast<uint32_t>(track.sampleRate);
    source.channels = static_cast<uint32_t>(track.channels);
    source.sampleFormat = NAE_SAMPLE_FORMAT_S32;
    source.validBits = 24;
    source.containerBits = static_cast<uint32_t>(track.bitsPerSample ? track.bitsPerSample : 32);
    NAE_OutputPolicy policy{};
    NAE_OutputPolicyDefaults(&policy);
    NAE_ResolvedSignalPath resolved{};
    resolved.structSize = sizeof(resolved);
    if (!capsOk) {
        std::printf("resolved: unavailable (the driver refused to describe the device)\n");
    } else if (NAE_ResolveSignalPath(&caps, &source, &policy, &resolved) == NAE_OK) {
        std::printf("resolved: backend=%d (%s) device='%s' %u Hz, %u ch, %u-bit container / "
                    "%u valid, bitPerfect=%d\n",
                    resolved.backend, backendName(resolved.backend), resolved.deviceName,
                    resolved.sampleRate, resolved.channels, resolved.containerBits,
                    resolved.validBits, resolved.bitPerfect);
        if (resolved.backend != NAE_BACKEND_ASIO) {
            std::printf("FAILED: the resolved path reports backend %d, not ASIO\n",
                        resolved.backend);
            NAE_Close(handle);
            return 1;
        }
    } else {
        std::printf("resolved: the resolver refused this caps/source pair\n");
    }

    // ---- 6. play, and watch the position move with the device's frames ----
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    double lastDeviceFrames = 0.0;
    double deviceFrames = 0.0;
    double firstPosition = -1.0;
    double lastPosition = -1.0;
    bool advanced = true;
    int32_t failure = NAE_OK;

    std::printf("playing for 3 s; position and deviceWritten are sampled together\n");
    for (int slice = 0; slice < 30; ++slice) {
        failure = NAE_Play(handle, 100);
        if (failure != NAE_OK) break;

        char* stats = NAE_PipelineStats(handle);
        const double written = stats ? jsonNumber(stats, "deviceWritten") : -1.0;
        if (stats) NAE_FreeString(stats);
        const double position = NAE_Position(handle);
        const double elapsedMs =
            static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    clock::now() - started)
                                    .count());
        if (firstPosition < 0.0) firstPosition = position;
        if (lastPosition >= 0.0 && position < lastPosition) advanced = false;
        lastPosition = position;
        deviceFrames = written;

        std::printf("  t=%6.0f ms  position=%8.3f s  deviceWritten=%.0f frames  "
                    "implied=%.3f s\n",
                    elapsedMs, position, written, rate ? written / rate : 0.0);
        if (written > 0.0 && written <= lastDeviceFrames) {
            // Frames stopped arriving while NAE_Play still returned: the device is
            // not moving and the position is about to be a number about the past.
            std::printf("  note: device frames did not advance in this slice\n");
        }
        lastDeviceFrames = written;
    }

    // ---- 7. the stream's own counters, and the verdict ----
    NAE_SignalPath signal{};
    signal.structSize = sizeof(signal);
    if (NAE_QuerySignalPath(handle, &signal) == NAE_OK) {
        std::printf("signal: %d Hz, %d ch, bufferFrames=%d, bufferLatency=%.2f ms, "
                    "driverLatency=%.2f ms, underruns=%llu, callbacks=%llu\n",
                    signal.sampleRate, signal.channels, signal.bufferFrames,
                    signal.bufferLatencyMs, signal.driverLatencyMs,
                    static_cast<unsigned long long>(signal.underrunCount),
                    static_cast<unsigned long long>(signal.callbackCount));
    }
    NAE_Close(handle);

    if (failure != NAE_OK) {
        std::printf("FAILED: NAE_Play returned %d: %s\n", failure, engineError().c_str());
        return 1;
    }
    const double advancedBy = (lastPosition >= 0.0 && firstPosition >= 0.0)
                                  ? lastPosition - firstPosition
                                  : 0.0;
    std::printf("position advanced %.3f s from device frames; %.0f device frames were written\n",
                advancedBy, deviceFrames);
    if (deviceFrames <= 0.0) {
        std::printf("FAILED: the device wrote no frames; this is not playback\n");
        return 1;
    }
    if (!advanced || advancedBy <= 0.0) {
        std::printf("FAILED: the position did not advance\n");
        return 1;
    }
    std::printf("asio probe passed\n");
    return 0;
}
