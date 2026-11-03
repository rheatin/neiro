// AAudio backend for Android.
//
// Implements the same AudioBackend interface. Caps come from AAudio; the stream
// pulls interleaved float32 from the core render callback. This file is only
// compiled for Android.

#include "audio_backend.h"

#include <aaudio/AAudio.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace neiro {

namespace {

struct AAudioStream final : BackendStream {
    AAudioStreamBuilder* builder = nullptr;
    AAudioStream* stream = nullptr;
    RenderCallback render;
    uint32_t channels = 0;
    double latency = 0.0;

    static aaudio_data_callback_result_t dataCallback(AAudioStream*,
                                                      void* user,
                                                      void* audioData,
                                                      int32_t numFrames) {
        auto* self = static_cast<AAudioStream*>(user);
        if (!self || !self->render) return AAUDIO_CALLBACK_RESULT_CONTINUE;
        auto* out = static_cast<float*>(audioData);
        const size_t got = self->render(out, static_cast<size_t>(numFrames));
        if (got < static_cast<size_t>(numFrames)) {
            std::memset(out + got * self->channels, 0,
                        (static_cast<size_t>(numFrames) - got) * self->channels * sizeof(float));
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    bool start(std::string* error) override {
        if (!stream) {
            if (error) *error = "AAudio stream is not open";
            return false;
        }
        if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
            if (error) *error = "AAudioStream_requestStart failed";
            return false;
        }
        return true;
    }

    void stop() override {
        if (stream) AAudioStream_requestStop(stream);
    }
    void pause() override { stop(); }
    void resume() override { start(nullptr); }
    bool deviceLost() const override { return false; }
    double latencyMs() const override { return latency; }

    ~AAudioStream() override {
        if (stream) {
            AAudioStream_requestStop(stream);
            AAudioStream_close(stream);
        }
        if (builder) AAudioStreamBuilder_delete(builder);
    }
};

}  // namespace

class AAudioBackend final : public AudioBackend {
  public:
    const char* name() const override { return "aaudio"; }

    std::vector<BackendDevice> enumerate(std::string* error) override {
        std::vector<BackendDevice> devices;
        // AAudio exposes the default output device and lets a stream specify
        // its id; there is no public enumeration API. Report the default with
        // its resolved id so the caller can open it explicitly.
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
            if (error) *error = "cannot create an AAudio builder";
            return devices;
        }
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStream* stream = nullptr;
        if (AAudioStreamBuilder_openStream(builder, &stream) == AAUDIO_OK && stream) {
            BackendDevice d;
            const int32_t id = AAudioStream_getDeviceId(stream);
            d.id = std::to_string(id);
            d.name = "AAudio default output";
            d.isDefault = true;
            devices.push_back(d);
            AAudioStream_close(stream);
        }
        AAudioStreamBuilder_delete(builder);
        if (devices.empty() && error) *error = "no AAudio output device";
        return devices;
    }

    bool queryCaps(const std::string& deviceId, int32_t shareMode,
                   NAE_DeviceCaps* out, std::string* error) override {
        if (!out) {
            if (error) *error = "null caps";
            return false;
        }
        std::memset(out, 0, sizeof(*out));
        out->structSize = sizeof(*out);
        out->version = NAE_DEVICE_CAPS_VERSION;
        out->backend = NAE_BACKEND_AAUDIO;
        out->shareMode = shareMode == NAE_SHARE_MODE_SHARED ? NAE_SHARE_MODE_SHARED
                                                           : NAE_SHARE_MODE_EXCLUSIVE;
        std::snprintf(out->id, sizeof(out->id), "%s", deviceId.c_str());
        std::snprintf(out->name, sizeof(out->name), "AAudio output");
        out->isDefault = deviceId.empty() ? 1 : 0;
        out->hotplug = 1;
        out->sharedSupported = 1;
        out->supportedShareModes = (1u << NAE_SHARE_MODE_SHARED);
        out->probed |= NAE_CAPS_PROBED_NATIVE | NAE_CAPS_PROBED_RATES |
                       NAE_CAPS_PROBED_FORMATS | NAE_CAPS_PROBED_LAYOUTS;

        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
            if (error) *error = "cannot create an AAudio builder";
            return false;
        }
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        if (!deviceId.empty()) {
            AAudioStreamBuilder_setDeviceId(builder, std::stoi(deviceId));
        }
        AAudioStream* stream = nullptr;
        const AAudioResult rc = AAudioStreamBuilder_openStream(builder, &stream);
        AAudioStreamBuilder_delete(builder);
        if (rc != AAUDIO_OK || !stream) {
            if (error) *error = "cannot open an AAudio stream for caps";
            return false;
        }
        out->nativeRate = static_cast<uint32_t>(AAudioStream_getSampleRate(stream));
        out->rates[0] = out->nativeRate;
        out->rateCount = 1;
        out->nativeChannels = static_cast<uint32_t>(AAudioStream_getChannelCount(stream));
        out->maxChannels = out->nativeChannels;
        out->nativeFormat = NAE_SAMPLE_FORMAT_F32;
        out->nativeValidBits = 32;
        out->nativeContainerBits = 32;
        out->nativeChannelMask = out->nativeChannels == 1 ? NAE_LAYOUT_MONO
                                 : out->nativeChannels == 2 ? NAE_LAYOUT_STEREO
                                                           : 0;
        out->layouts[0] = out->nativeChannelMask;
        out->layoutCount = 1;
        out->formatCount = 1;
        out->formats[0] = {NAE_SAMPLE_FORMAT_F32, 32, 32};
        out->defaultBufferFrames = static_cast<uint32_t>(AAudioStream_getBufferSizeInFrames(stream));
        out->minBufferFrames = static_cast<uint32_t>(AAudioStream_getBufferCapacityInFrames(stream));
        out->eventDriven = 1;
        // AAudio does not expose a single latency number through this API;
        // leave it at 0 rather than inventing one. The stream reports its own
        // timing once open.
        AAudioStream_close(stream);
        return true;
    }

    std::unique_ptr<BackendStream> open(const BackendOpenRequest& request,
                                        std::string* error) override {
        auto stream = std::make_unique<AAudioStream>();
        stream->render = request.render;
        stream->channels = request.resolved.channels ? request.resolved.channels : 2;
        if (AAudio_createStreamBuilder(&stream->builder) != AAUDIO_OK || !stream->builder) {
            if (error) *error = "cannot create an AAudio builder";
            return nullptr;
        }
        AAudioStreamBuilder_setDirection(stream->builder, AAUDIO_DIRECTION_OUTPUT);
        if (!request.deviceId.empty()) {
            AAudioStreamBuilder_setDeviceId(stream->builder, std::stoi(request.deviceId));
        }
        AAudioStreamBuilder_setSampleRate(stream->builder,
                                          static_cast<int32_t>(request.resolved.sampleRate));
        AAudioStreamBuilder_setChannelCount(stream->builder,
                                            static_cast<int32_t>(stream->channels));
        AAudioStreamBuilder_setFormat(stream->builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setPerformanceMode(stream->builder,
                                               AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setDataCallback(stream->builder, AAudioStream::dataCallback,
                                            stream.get());
        if (AAudioStreamBuilder_openStream(stream->builder, &stream->stream) != AAUDIO_OK ||
            !stream->stream) {
            if (error) *error = "AAudioStreamBuilder_openStream failed";
            return nullptr;
        }
        stream->latency = 0.0;
        return stream;
    }

    void shutdown() override {}
};

std::unique_ptr<AudioBackend> createPlatformBackend() {
    return std::make_unique<AAudioBackend>();
}

}  // namespace neiro
