// CoreAudio backend for macOS.
//
// Implements the same AudioBackend interface as WASAPI. It does not contain DSP,
// routing, policy or diagnostics: caps come from the core and the stream moves
// frames.
//
// This file is only compiled on Apple platforms. It is written against the
// AudioToolbox/CoreAudio HAL, not AVAudioEngine, because the engine needs the
// device's real format and buffer, not an app-level mix graph.

#include "audio_backend.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace neiro {

namespace {

std::string fourCcToString(OSType type) {
    char c[5] = {static_cast<char>(type >> 24), static_cast<char>(type >> 16),
                 static_cast<char>(type >> 8), static_cast<char>(type), '\0'};
    return std::string(c);
}

bool getDefaultOutputDevice(AudioDeviceID* out) {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioDeviceID id = 0;
    UInt32 size = sizeof(id);
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0,
                                                       nullptr, &size, &id);
    if (status != noErr || id == 0) return false;
    *out = id;
    return true;
}

bool deviceName(AudioDeviceID id, std::string* out) {
    AudioObjectPropertyAddress addr{kAudioObjectPropertyName,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &name) != noErr || !name) {
        return false;
    }
    char buf[256] = {};
    const bool ok = CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(name);
    if (ok) *out = buf;
    return ok;
}

uint32_t deviceChannelCount(AudioDeviceID id) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration,
                                    kAudioObjectPropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    std::vector<char> storage(size);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, list) != noErr) return 0;
    uint32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

bool deviceSampleRate(AudioDeviceID id, double* out) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &rate) != noErr) return false;
    *out = rate;
    return true;
}

bool setDeviceSampleRate(AudioDeviceID id, double rate) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 value = rate;
    return AudioObjectSetPropertyData(id, &addr, 0, nullptr, sizeof(value), &value) == noErr;
}

// Simple AudioUnit output. The render callback pulls from the core and returns
// interleaved float32 at the unit's own stream format.
struct CoreAudioStream final : BackendStream {
    AudioUnit unit = nullptr;
    AudioDeviceID device = 0;
    RenderCallback render;
    std::atomic<bool> running{false};
    std::atomic<bool> lost{false};
    uint32_t channels = 0;
    double latency = 0.0;

    static OSStatus renderCallback(void* user, AudioUnitRenderActionFlags*,
                                   const AudioTimeStamp*, UInt32, UInt32 frames,
                                   AudioBufferList* data) {
        auto* self = static_cast<CoreAudioStream*>(user);
        if (!self->render || !data || data->mNumberBuffers == 0) return noErr;
        auto* first = static_cast<float*>(data->mBuffers[0].mData);
        const size_t got = self->render(first, frames);
        // A short render is padded with silence; the core's contract is that a
        // short block means end of source, not a partial buffer.
        if (got < frames) {
            std::memset(first + got * self->channels, 0,
                        (frames - got) * self->channels * sizeof(float));
        }
        return noErr;
    }

    bool start(std::string* error) override {
        if (!unit) {
            if (error) *error = "AudioUnit is not open";
            return false;
        }
        if (AudioOutputUnitStart(unit) != noErr) {
            if (error) *error = "AudioOutputUnitStart failed";
            return false;
        }
        running = true;
        return true;
    }

    void stop() override {
        if (unit && running.exchange(false)) AudioOutputUnitStop(unit);
    }

    void pause() override {
        if (unit && running.load()) AudioOutputUnitStop(unit);
    }

    void resume() override {
        if (unit) AudioOutputUnitStart(unit);
    }

    bool deviceLost() const override { return lost.load(); }
    double latencyMs() const override { return latency; }

    ~CoreAudioStream() override {
        stop();
        if (unit) {
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
        }
    }
};

}  // namespace

class CoreAudioBackend final : public AudioBackend {
  public:
    const char* name() const override { return "coreaudio"; }

    std::vector<BackendDevice> enumerate(std::string* error) override {
        std::vector<BackendDevice> devices;
        AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr,
                                           &size) != noErr) {
            if (error) *error = "cannot list CoreAudio devices";
            return devices;
        }
        const uint32_t count = size / sizeof(AudioDeviceID);
        std::vector<AudioDeviceID> ids(count);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                       ids.data()) != noErr) {
            if (error) *error = "cannot read CoreAudio device list";
            return devices;
        }
        AudioDeviceID defaultId = 0;
        getDefaultOutputDevice(&defaultId);
        for (AudioDeviceID id : ids) {
            if (deviceChannelCount(id) == 0) continue;  // input-only
            BackendDevice d;
            d.id = std::to_string(id);
            deviceName(id, &d.name);
            d.isDefault = id == defaultId;
            devices.push_back(d);
        }
        if (devices.empty() && error) *error = "no CoreAudio output device";
        return devices;
    }

    bool queryCaps(const std::string& deviceId, int32_t shareMode,
                   NAE_DeviceCaps* out, std::string* error) override {
        if (!out) {
            if (error) *error = "null caps";
            return false;
        }
        AudioDeviceID id = 0;
        if (deviceId.empty()) {
            if (!getDefaultOutputDevice(&id)) {
                if (error) *error = "no default output device";
                return false;
            }
        } else {
            id = static_cast<AudioDeviceID>(std::stoul(deviceId));
        }

        std::memset(out, 0, sizeof(*out));
        out->structSize = sizeof(*out);
        out->version = NAE_DEVICE_CAPS_VERSION;
        out->backend = NAE_BACKEND_COREAUDIO;
        out->shareMode = shareMode == NAE_SHARE_MODE_SHARED ? NAE_SHARE_MODE_SHARED
                                                           : NAE_SHARE_MODE_EXCLUSIVE;
        std::snprintf(out->id, sizeof(out->id), "%s", deviceId.c_str());
        std::string name;
        deviceName(id, &name);
        std::snprintf(out->name, sizeof(out->name), "%s", name.c_str());
        out->isDefault = deviceId.empty() ? 1 : 0;
        out->hotplug = 1;
        out->exclusiveSupported = 1;
        out->sharedSupported = 1;
        out->supportedShareModes = (1u << NAE_SHARE_MODE_EXCLUSIVE) |
                                   (1u << NAE_SHARE_MODE_SHARED);
        out->probed |= NAE_CAPS_PROBED_NATIVE | NAE_CAPS_PROBED_RATES |
                       NAE_CAPS_PROBED_FORMATS | NAE_CAPS_PROBED_LAYOUTS;

        double rate = 0.0;
        if (deviceSampleRate(id, &rate)) {
            out->nativeRate = static_cast<uint32_t>(rate);
            out->rates[0] = out->nativeRate;
            out->rateCount = 1;
        }
        out->nativeChannels = deviceChannelCount(id);
        out->maxChannels = out->nativeChannels;
        out->nativeFormat = NAE_SAMPLE_FORMAT_F32;
        out->nativeValidBits = 32;
        out->nativeContainerBits = 32;
        out->nativeChannelMask =
            out->nativeChannels == 1 ? NAE_LAYOUT_MONO
            : out->nativeChannels == 2 ? NAE_LAYOUT_STEREO
            : out->nativeChannels == 6 ? NAE_LAYOUT_5_1
            : out->nativeChannels == 8 ? NAE_LAYOUT_7_1
                                       : 0;
        out->layouts[0] = out->nativeChannelMask;
        out->layoutCount = 1;
        out->formatCount = 1;
        out->formats[0] = {NAE_SAMPLE_FORMAT_F32, 32, 32};
        // CoreAudio does not advertise native DSD through this property; a
        // device-specific driver would have to report it. DoP is not claimed
        // without a probe of the carrier rate.
        out->nativeDsdSupported = 0;
        out->dopSupported = 0;
        return true;
    }

    std::unique_ptr<BackendStream> open(const BackendOpenRequest& request,
                                        std::string* error) override {
        AudioDeviceID id = 0;
        if (request.deviceId.empty()) {
            if (!getDefaultOutputDevice(&id)) {
                if (error) *error = "no default output device";
                return nullptr;
            }
        } else {
            id = static_cast<AudioDeviceID>(std::stoul(request.deviceId));
        }

        const uint32_t channels = deviceChannelCount(id);
        if (channels == 0) {
            if (error) *error = "device has no output channels";
            return nullptr;
        }
        double rate = 0.0;
        if (!deviceSampleRate(id, &rate)) {
            if (error) *error = "device reports no sample rate";
            return nullptr;
        }
        if (request.resolved.sampleRate && request.resolved.sampleRate != rate) {
            if (!setDeviceSampleRate(id, request.resolved.sampleRate)) {
                if (error) *error = "device refused the requested sample rate";
                return nullptr;
            }
            rate = request.resolved.sampleRate;
        }

        auto stream = std::make_unique<CoreAudioStream>();
        stream->device = id;
        stream->render = request.render;
        stream->channels = channels;
        stream->latency = 0.0;

        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_HALOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) {
            if (error) *error = "no HAL output AudioUnit";
            return nullptr;
        }
        if (AudioComponentInstanceNew(comp, &stream->unit) != noErr || !stream->unit) {
            if (error) *error = "cannot create the HAL output AudioUnit";
            return nullptr;
        }
        const UInt32 enable = 1;
        AudioUnitSetProperty(stream->unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 1, &enable, sizeof(enable));
        AudioUnitSetProperty(stream->unit, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &id, sizeof(id));

        AudioStreamBasicDescription format{};
        format.mSampleRate = rate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mChannelsPerFrame = channels;
        format.mBitsPerChannel = 32;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = channels * sizeof(float);
        format.mBytesPerPacket = format.mBytesPerFrame;
        AudioUnitSetProperty(stream->unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &format, sizeof(format));

        AURenderCallbackStruct callback{};
        callback.inputProc = CoreAudioStream::renderCallback;
        callback.inputProcRefCon = stream.get();
        if (AudioUnitSetProperty(stream->unit, kAudioUnitProperty_SetRenderCallback,
                                 kAudioUnitScope_Input, 0, &callback,
                                 sizeof(callback)) != noErr) {
            if (error) *error = "cannot set the render callback";
            return nullptr;
        }
        if (AudioUnitInitialize(stream->unit) != noErr) {
            if (error) *error = "AudioUnitInitialize failed";
            return nullptr;
        }
        return stream;
    }

    void shutdown() override {}
};

std::unique_ptr<AudioBackend> createPlatformBackend() {
    return std::make_unique<CoreAudioBackend>();
}

}  // namespace neiro
