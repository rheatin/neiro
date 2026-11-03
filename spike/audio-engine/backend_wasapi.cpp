// WASAPI adapter for the backend interface.
//
// The engine's existing WASAPI path is push-driven (NAE_Play). This adapter
// exposes the common interface to hosts that want it, and owns a thread that
// calls NAE_Play in slices. It deliberately does not duplicate device
// negotiation: caps come from NAE_QueryDeviceCaps and the stream comes from
// NAE_OpenFile.

#include "audio_backend.h"
#include "nae.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

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
