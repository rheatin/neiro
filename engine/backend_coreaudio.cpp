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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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


// ---- what the device actually offers ---------------------------------------
//
// The caps below used to report one rate and one format regardless of the
// device: `rates[0] = current, rateCount = 1`, and a hardcoded float32. On
// Windows the same struct carries the endpoint's real exclusive-mode list, so a
// host that asked "what can this device do" got two different answers on the
// two platforms. These read the HAL's own lists.

bool firstOutputStream(AudioDeviceID id, AudioStreamID* out) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyStreams,
                                    kAudioObjectPropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioStreamID)) {
        return false;
    }
    std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, streams.data()) != noErr) {
        return false;
    }
    if (streams.empty()) return false;
    *out = streams.front();
    return true;
}

bool physicalFormat(AudioStreamID stream, AudioStreamBasicDescription* out) {
    AudioObjectPropertyAddress addr{kAudioStreamPropertyPhysicalFormat,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(*out);
    return AudioObjectGetPropertyData(stream, &addr, 0, nullptr, &size, out) == noErr;
}

bool availablePhysicalFormats(AudioStreamID stream,
                              std::vector<AudioStreamBasicDescription>* out) {
    AudioObjectPropertyAddress addr{kAudioStreamPropertyAvailablePhysicalFormats,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(stream, &addr, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioStreamRangedDescription)) {
        return false;
    }
    std::vector<AudioStreamRangedDescription> list(size / sizeof(AudioStreamRangedDescription));
    if (AudioObjectGetPropertyData(stream, &addr, 0, nullptr, &size, list.data()) != noErr) {
        return false;
    }
    out->clear();
    for (const auto& entry : list) out->push_back(entry.mFormat);
    return !out->empty();
}

bool availableNominalRates(AudioDeviceID id, std::vector<double>* out) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyAvailableNominalSampleRates,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr ||
        size < sizeof(AudioValueRange)) {
        return false;
    }
    std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, ranges.data()) != noErr) {
        return false;
    }
    out->clear();
    for (const auto& r : ranges) {
        // Discrete rates come back as min == max; a continuous range is offered
        // as its two ends, because a host can only ask for a number.
        out->push_back(r.mMinimum);
        if (r.mMaximum > r.mMinimum) out->push_back(r.mMaximum);
    }
    std::sort(out->begin(), out->end());
    out->erase(std::unique(out->begin(), out->end()), out->end());
    return !out->empty();
}

int32_t sampleFormatOf(const AudioStreamBasicDescription& f) {
    if (f.mFormatFlags & kAudioFormatFlagIsFloat) {
        return f.mBitsPerChannel == 64 ? NAE_SAMPLE_FORMAT_F64 : NAE_SAMPLE_FORMAT_F32;
    }
    switch (f.mBitsPerChannel) {
        case 8: return NAE_SAMPLE_FORMAT_U8;
        case 16: return NAE_SAMPLE_FORMAT_S16;
        case 24: return NAE_SAMPLE_FORMAT_S24;
        case 32: return NAE_SAMPLE_FORMAT_S32;
        default: return NAE_SAMPLE_FORMAT_UNKNOWN;
    }
}

/// Latency the HAL reports, in milliseconds.
///
/// Three numbers, all in frames: the device's own pipeline latency, the stream's,
/// and the safety offset the driver keeps. Added to the buffer, divided by the
/// rate, that is what the listener waits for -- before the engine's own ring,
/// which the caller adds.
bool reportedLatencyMs(AudioDeviceID id, AudioStreamID stream, double rate,
                       uint32_t bufferFrames, double* out) {
    if (rate <= 0.0) return false;
    UInt32 deviceLatency = 0, streamLatency = 0, safety = 0;
    UInt32 size = sizeof(UInt32);
    AudioObjectPropertyAddress a{kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput,
                                 kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &deviceLatency) != noErr) {
        return false;
    }
    a.mSelector = kAudioStreamPropertyLatency;
    a.mScope = kAudioObjectPropertyScopeGlobal;
    size = sizeof(UInt32);
    if (AudioObjectGetPropertyData(stream, &a, 0, nullptr, &size, &streamLatency) != noErr) {
        streamLatency = 0;
    }
    a.mSelector = kAudioDevicePropertySafetyOffset;
    a.mScope = kAudioObjectPropertyScopeOutput;
    size = sizeof(UInt32);
    if (AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &safety) != noErr) safety = 0;
    const double frames = static_cast<double>(deviceLatency) + streamLatency + safety +
                          static_cast<double>(bufferFrames);
    *out = frames * 1000.0 / rate;
    return true;
}

bool bufferFrameRange(AudioDeviceID id, uint32_t* lo, uint32_t* hi) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyBufferFrameSizeRange,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioValueRange range{};
    UInt32 size = sizeof(range);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &range) != noErr) return false;
    *lo = static_cast<uint32_t>(range.mMinimum);
    *hi = static_cast<uint32_t>(range.mMaximum);
    return true;
}

/// Takes or releases exclusive *access* to the device.
///
/// `kAudioDevicePropertyHogMode` is the closest thing this platform has to an
/// exclusive stream. It is deliberately not called "exclusive mode" anywhere in
/// this file: it stops other processes from opening the device, and it does not
/// stop the HAL from converting if the stream format or the nominal rate differs
/// from the physical one. A host that offered a switch named after WASAPI's
/// exclusive mode would be promising something this cannot deliver.
///
/// `owner` is a pid; -1 releases. Returns false when the device is already
/// hogged by someone else, which is the case worth reporting.
bool setHog(AudioDeviceID id, pid_t owner) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyHogMode,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    pid_t value = owner;
    return AudioObjectSetPropertyData(id, &addr, 0, nullptr, sizeof(value), &value) == noErr;
}

bool hogOwner(AudioDeviceID id, pid_t* out) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyHogMode,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    pid_t value = -1;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &value) != noErr) return false;
    *out = value;
    return true;
}

/// Whether the HAL still knows this device.
///
/// A device that has been unplugged is not an error on the next property read --
/// the object is gone, and `AudioObjectGetPropertyData` answers
/// `kAudioHardwareBadObjectError`. That is the same answer as "reported not
/// alive", so both come back as false here.
/// Whether the HAL still knows this device.
///
/// Returns false when the question could not be answered, and sets `alive` only
/// when it could. The two are kept apart on purpose: a property read that fails
/// during a device handover is not evidence that the device is gone, and the
/// stream's lost flag is latched -- one transient failure would keep playback
/// paused for the life of the stream with nothing able to clear it.
bool deviceAlive(AudioDeviceID id, bool* alive) {
    if (alive) *alive = false;
    if (!id) return true;  // a null device is definitively not alive
    AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceIsAlive,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 value = 0;
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &value) != noErr) return false;
    if (alive) *alive = value != 0;
    return true;
}

bool setDeviceSampleRate(AudioDeviceID id, double rate) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 value = rate;
    return AudioObjectSetPropertyData(id, &addr, 0, nullptr, sizeof(value), &value) == noErr;
}

// Simple AudioUnit output. The render callback pulls interleaved device bytes
// from the core; this unit's stream format is 32-bit float, so those bytes are
// float32 and are handed to the unit unchanged.
struct CoreAudioStream final : BackendStream {
    AudioUnit unit = nullptr;
    AudioDeviceID device = 0;
    RenderCallback render;
    std::atomic<bool> running{false};
    std::atomic<bool> lost{false};
    uint32_t channels = 0;
    uint32_t bytesPerFrame = 0;
    bool hogged = false;
    /// The rate the device was at before this stream changed it, or 0 when the
    /// stream did not change it. Restored on the way out: a fixed-rate open is a
    /// setting for one stream, not a change to the machine.
    double restoreRate = 0.0;
    double latency = 0.0;
    /// True while a HAL listener is installed on `device`, so the destructor
    /// removes exactly the one that was added.
    bool watchingLoss = false;

    /// The device said it is going away.
    ///
    /// Without this, `deviceLost()` returned false for the life of the stream:
    /// nothing ever set `lost`, so an unplugged endpoint played into silence and
    /// the engine reported success. The engine's answer to a lost device --
    /// `NAE_ERR_DEVICE_LOST`, playback pauses, the handle and position survive --
    /// could not happen on this platform at all.
    static OSStatus aliveChanged(AudioObjectID object, UInt32,
                                 const AudioObjectPropertyAddress*, void* client) {
        auto* self = static_cast<CoreAudioStream*>(client);
        bool alive = true;
        if (self && deviceAlive(object, &alive) && !alive) self->lost.store(true);
        return noErr;
    }

    void watchLoss() {
        if (!device || watchingLoss) return;
        AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceIsAlive,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
        if (AudioObjectAddPropertyListener(device, &addr, &CoreAudioStream::aliveChanged, this) ==
            noErr) {
            watchingLoss = true;
        } else if (std::getenv("NAE_TRACE")) {
            // Not fatal: the stream plays, it just cannot report a lost device.
            std::fprintf(stderr, "[coreaudio] could not watch device loss on device %u\n",
                         static_cast<unsigned>(device));
        }
    }

    void unwatchLoss() {
        if (!device || !watchingLoss) return;
        AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceIsAlive,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
        AudioObjectRemovePropertyListener(device, &addr, &CoreAudioStream::aliveChanged, this);
        watchingLoss = false;
    }

    static OSStatus renderCallback(void* user, AudioUnitRenderActionFlags*,
                                   const AudioTimeStamp*, UInt32, UInt32 frames,
                                   AudioBufferList* data) {
        auto* self = static_cast<CoreAudioStream*>(user);
        if (!self->render || !data || data->mNumberBuffers == 0) return noErr;
        // The core hands over final device bytes; this backend only moves them.
        auto* bytes = static_cast<uint8_t*>(data->mBuffers[0].mData);
        const size_t got = self->render(bytes, frames);
        // A short render is padded with silence; the core's contract is that a
        // short block means end of source, not a partial buffer. Padded by
        // bytes, because the buffer is no longer typed as floats.
        if (got < frames) {
            // A frame is as wide as the format this stream was opened with.
            const size_t bytesPerFrame =
                self->bytesPerFrame ? self->bytesPerFrame : self->channels * sizeof(float);
            std::memset(bytes + got * bytesPerFrame, 0,
                        (frames - got) * bytesPerFrame);
        }
        return noErr;
    }

    bool start(std::string* error) override {
        if (!unit) {
            if (error) *error = "AudioUnit is not open";
            return false;
        }
        if (AudioOutputUnitStart(unit) != noErr) {
            // A start that fails on a device the HAL no longer knows is not a
            // generic open failure: it is the device-lost path, and the engine
            // pauses playback rather than reporting an error to the listener.
            bool alive = true;
            if (deviceAlive(device, &alive) && !alive) lost.store(true);
            if (error) *error = "AudioOutputUnitStart failed";
            return false;
        }
        running = true;
        // Hog mode, taken *after* the unit is running.
        //
        // Taken before `AudioUnitInitialize` it stopped the stream pulling
        // altogether -- measured: one ring of 8192 frames, then silence, and the
        // device's rate read back as 0 Hz. The unit needs the device to be
        // available while it initialises; exclusivity is for the period after
        // that. A refusal here is not fatal: the stream plays either way, and the
        // caller learns from `caps.exclusiveSupported` that exclusivity is not
        // something this platform offers in the WASAPI sense.
        // Hog mode is NOT taken here either.
        //
        // Second measurement, with the call moved after `AudioOutputUnitStart`
        // (the first attempt had it before `AudioUnitInitialize`): the raw stream
        // still stopped pulling -- `output_stream_check` committed 8192 frames and
        // the device took 0. So the ordering was not the whole story, and until it
        // is understood this stays out: a switch that silences playback is not a
        // workaround for exclusivity.
        return true;
    }

    void stop() override {
        if (unit && running.exchange(false)) AudioOutputUnitStop(unit);
    }

    void pause() override {
        if (unit && running.load()) AudioOutputUnitStop(unit);
    }

    void resume() override {
        if (!unit) return;
        if (AudioOutputUnitStart(unit) == noErr) running.store(true);
        // `running` is what `stop` consults to decide whether the unit is still
        // pulling. Leaving it false here meant a `stop` after a `resume` skipped
        // `AudioOutputUnitStop` entirely, and the destructor then disposed a unit
        // the HAL was still calling -- into an object that was being freed.
    }

    bool deviceLost() const override { return lost.load(); }
    double latencyMs() const override { return latency; }

    ~CoreAudioStream() override {
        stop();
        // Before anything else: a listener that outlives the object fires on
        // released memory, and that is a crash a user sees as a random quit.
        unwatchLoss();
        // The unit goes first, in the reverse of the order `open` builds it
        // (rate, then hog, then unit). Restoring the rate while a client still
        // holds the device asks the HAL to reshape an endpoint that is still in
        // use -- the open path exists to avoid exactly that, and the close path
        // was doing it.
        if (unit) {
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            unit = nullptr;
        }
        if (hogged && device) {
            setHog(device, -1);
            hogged = false;
        }
        if (restoreRate > 0.0 && device) {
            // Give the machine back the rate it had, and wait until it is back.
            //
            // Requesting is not enough: the HAL applies a nominal-rate change
            // late, so a caller that opens again immediately reads the rate this
            // stream had set. That is exactly how "switch the policy back to
            // Device" resolved to the fixed rate -- the next open happened before
            // the restore landed. Blocking here is the only place that knows the
            // target, so it is the only place that can wait for it.
            setDeviceSampleRate(device, restoreRate);
            for (int attempt = 0; attempt < 100; ++attempt) {
                double now = 0.0;
                if (!deviceSampleRate(device, &now)) break;
                if (std::fabs(now - restoreRate) < 1.0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            restoreRate = 0.0;
        }
    }
};

}  // namespace

/// A device id is a decimal AudioDeviceID. Anything else is a caller mistake,
/// not a number to hand to `std::stoul`, which throws -- and an exception
/// escaping this boundary terminates the host.
bool parseDeviceId(const std::string& text, AudioDeviceID* out) {
    if (text.empty() || !out) return false;
    errno = 0;
    const char* begin = text.c_str();
    char* end = nullptr;
    const unsigned long long value = std::strtoull(begin, &end, 10);
    if (errno != 0 || end == begin || !end || *end != '\0') return false;
    if (value == 0 || value > 0xFFFFFFFFull) return false;
    *out = static_cast<AudioDeviceID>(value);
    return true;
}

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
        } else if (!parseDeviceId(deviceId, &id)) {
            if (error) *error = "the device id is not a number";
            return false;
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
        // Exclusive here means `kAudioDevicePropertyHogMode`: the device is taken
        // away from every other process, which is what a host's exclusive switch
        // asks for. It is NOT the claim that the samples reach the DAC
        // unconverted -- that is `bitPerfect`'s job, and it stays conservative.
        // Keeping the two apart is what lets this field answer "can I stop other
        // programs from making sound" without also promising bit-exactness.
        out->exclusiveSupported = 1;
        out->sharedSupported = 1;
        out->supportedShareModes = (1u << NAE_SHARE_MODE_EXCLUSIVE) |
                                   (1u << NAE_SHARE_MODE_SHARED);
        // Whether this device *can* be hogged, which is a property of the device.
        //
        // Not whether it is free right now: a device held by another process is
        // busy, not unsupported, and reporting the two the same way is the exact
        // lie `exclusiveSupported = 0` exists to avoid. A caller that needs to
        // know who holds it asks `hogOwner` and can retry; one that reads
        // "unsupported" gives up for good.
        {
            pid_t owner = -1;
            out->hogSupported = hogOwner(id, &owner) ? 1 : 0;
        }
        out->probed |= NAE_CAPS_PROBED_NATIVE | NAE_CAPS_PROBED_RATES |
                       NAE_CAPS_PROBED_FORMATS | NAE_CAPS_PROBED_LAYOUTS;

        // Wait for any pending rate change to land before reporting.
        //
        // Closing a stream asks the HAL to restore the rate it changed, and that
        // request is applied asynchronously -- so a caller that opens again
        // immediately reads the rate the *previous* stream left behind. That is
        // how "switch back to Device" resolved to the fixed rate the last stream
        // had asked for. Reading twice and requiring the same answer is the
        // cheapest way to know the device has finished moving; it is the mirror
        // of the wait after `setDeviceSampleRate`.
        double rate = 0.0;
        bool havePrevious = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            double reading = 0.0;
            if (!deviceSampleRate(id, &reading) || reading <= 0.0) break;
            // Two equal readings, not "equal to whatever was there before the
            // loop": a device mid-change reports 0, and 0 == 0 would have ended
            // the wait at the one moment it was needed.
            if (havePrevious && reading == rate) break;
            rate = reading;
            havePrevious = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (rate > 0.0) {
            // The device as it is *now*. A remembered "first sighting" was tried
            // and removed: the first sighting is whatever a previous process left
            // behind, so it could not distinguish the listener's setting from our
            // own leftovers. What makes this value trustworthy is the other half
            // -- a stream restores the rate it changed (see `restoreRate`), so the
            // number here is the listener's again by the time anyone reads it.
            out->nativeRate = static_cast<uint32_t>(rate);
        }

        // The device's real rate list, not the one rate it happens to be at.
        std::vector<double> rates;
        if (availableNominalRates(id, &rates)) {
            for (double r : rates) {
                if (out->rateCount >= NAE_MAX_DEVICE_RATES) break;
                out->rates[out->rateCount++] = static_cast<uint32_t>(r);
            }
        }
        if (out->rateCount == 0 && out->nativeRate) {
            out->rates[0] = out->nativeRate;
            out->rateCount = 1;
        }

        // The device's real physical formats, and which one it is on now.
        AudioStreamID stream = 0;
        AudioStreamBasicDescription current{};
        std::vector<AudioStreamBasicDescription> formats;
        if (firstOutputStream(id, &stream)) {
            if (physicalFormat(stream, &current)) {
                out->nativeFormat = sampleFormatOf(current);
                out->nativeContainerBits = current.mBitsPerChannel;
                out->nativeValidBits = current.mBitsPerChannel;
                if (current.mSampleRate > 0.0) {
                    out->nativeRate = static_cast<uint32_t>(current.mSampleRate);
                }
            }
            if (availablePhysicalFormats(stream, &formats)) {
                for (const auto& f : formats) {
                    if (out->formatCount >= NAE_MAX_DEVICE_FORMATS) break;
                    const int32_t fmt = sampleFormatOf(f);
                    if (fmt == NAE_SAMPLE_FORMAT_UNKNOWN) continue;
                    out->formats[out->formatCount++] =
                        {fmt, f.mBitsPerChannel, f.mBitsPerChannel};
                }
            }
        }
        if (out->formatCount == 0) {
            out->formats[0] = {NAE_SAMPLE_FORMAT_F32, 32, 32};
            out->formatCount = 1;
            if (out->nativeFormat == NAE_SAMPLE_FORMAT_UNKNOWN) {
                out->nativeFormat = NAE_SAMPLE_FORMAT_F32;
                out->nativeValidBits = 32;
                out->nativeContainerBits = 32;
            }
        }
        out->nativeChannels = deviceChannelCount(id);
        out->maxChannels = out->nativeChannels;
        out->nativeChannelMask =
            out->nativeChannels == 1 ? NAE_LAYOUT_MONO
            : out->nativeChannels == 2 ? NAE_LAYOUT_STEREO
            : out->nativeChannels == 6 ? NAE_LAYOUT_5_1
            : out->nativeChannels == 8 ? NAE_LAYOUT_7_1
                                       : 0;
        out->layouts[0] = out->nativeChannelMask;
        out->layoutCount = 1;
        // Only when the device told us nothing. This used to run unconditionally
        // and overwrite the real list the code above had just filled, so the
        // formats a host was shown were a constant no matter what the device
        // said.
        if (out->formatCount == 0) {
            out->formatCount = 1;
            out->formats[0] = {NAE_SAMPLE_FORMAT_F32, 32, 32};
        }
        // DoP is plain PCM at a carrier rate, so a device that accepts the
        // carrier accepts DoP. The carriers are 176400 / 352800 / 705600.
        if (out->rateCount > 0) {
            static const uint32_t kCarriers[] = {176400u, 352800u, 705600u};
            for (uint32_t c : kCarriers) {
                bool offered = false;
                for (uint32_t i = 0; i < out->rateCount; ++i) {
                    if (out->rates[i] == c) offered = true;
                }
                if (offered && out->dopCarrierCount < NAE_MAX_DOP_CARRIERS) {
                    out->dopCarriers[out->dopCarrierCount++] = c;
                }
            }
            out->dopSupported = out->dopCarrierCount > 0 ? 1 : 0;
        }
        out->probed |= NAE_CAPS_PROBED_DOP;

        uint32_t bufLo = 0, bufHi = 0;
        if (bufferFrameRange(id, &bufLo, &bufHi)) {
            out->minBufferFrames = bufLo;
            out->maxBufferFrames = bufHi;
            out->defaultBufferFrames = bufLo;
            out->minPeriodFrames = bufLo;
            out->maxPeriodFrames = bufHi;
            out->probed |= NAE_CAPS_PROBED_BUFFER;
        }

        // CoreAudio has no native DSD in the HAL: a DSF file reaches the device
        // as DoP or as PCM. Claiming otherwise would be inventing a capability.
        out->nativeDsdSupported = 0;
        out->probed |= NAE_CAPS_PROBED_NATIVE_DSD;
        // The carrier list above is computed from the rates the device offers,
        // and then discarded here: this route is float32 only, so DoP's "24 valid
        // bits, no resampling" cannot be met whatever the rate is. Zeroing only
        // the flag left a host reading carriers with the capability switched off,
        // which is the contradiction AGENTS.md rule 1 is about.
        out->dopSupported = 0;
        out->dopCarrierCount = 0;
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
            if (!parseDeviceId(request.deviceId, &id)) {
                if (error) *error = "the device id is not a number";
                return nullptr;
            }
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
        // Exclusive access first. Without it another program holds the device and
        // a rate change is accepted by the API and then ignored by the HAL -- which
        // is exactly the one-run-late readback measured in `backend_check`.
        // Hog mode is not taken here.
        //
        // Measured on the built-in output: hogging the device and then creating
        // the AudioUnit leaves the stream not pulling at all -- `output_stream_check`
        // stopped after exactly one ring (8192 frames), and every advertised rate
        // then read back as 0 Hz. Whether that ordering is the fault (hog after
        // `AudioUnitInitialize` instead of before) or hog on this device is not
        // established, so the code does not take it. Turning a working stream into
        // a silent one to gain a capability nobody has measured is not a trade.
        (void)0;

        double restoreRate = 0.0;
        if (request.resolved.sampleRate && request.resolved.sampleRate != rate) {
            restoreRate = rate;  // what it was before this open, to give back
            if (!setDeviceSampleRate(id, request.resolved.sampleRate)) {
                setHog(id, -1);
                if (error) *error = "device refused the requested sample rate";
                return nullptr;
            }
            // The requested rate is a request; the rate the device is on is a
            // measurement. Reporting the request as though it were the result is
            // the fault this file's caps work exists to avoid, and it is how the
            // one-run-late readback in `backend_check` stayed invisible.
            //
            // A device that will not move is not a failure to open. Falling back
            // to the rate it is actually on, and letting the engine resample, is
            // what keeps 44.1 kHz files playing on an endpoint stuck at 48 --
            // refusing here would take the whole track away to protect a number.
            // The caller sees the difference through `resampling`.
            // Wait for the change to actually land before the AudioUnit is
            // created against it.
            //
            // Measured: a check that requests 48000 on a device sitting at 44100
            // failed on the first run and passed on every later one, while a
            // caller that used the device's own rate passed every time. The
            // difference is this switch. `kAudioDevicePropertyDeviceIsRunning`
            // is not the signal -- on a cold start nothing is running, so a poll
            // on it returns immediately -- the signal is the rate itself
            // reporting the new value twice in a row.
            double previous = 0.0;
            double actual = 0.0;
            for (int attempt = 0; attempt < 100; ++attempt) {
                if (!deviceSampleRate(id, &actual) || actual <= 0.0) break;
                if (actual == previous &&
                    std::fabs(actual - static_cast<double>(request.resolved.sampleRate)) < 1.0) {
                    break;
                }
                previous = actual;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (actual > 0.0) rate = static_cast<int32_t>(actual);
        }

        int32_t grantedBits = 0;
        int32_t grantedFormat = NAE_SAMPLE_FORMAT_UNKNOWN;
        AudioStreamBasicDescription physical{};
        AudioStreamID physicalStream = 0;
        if (firstOutputStream(id, &physicalStream)) {
            physicalFormat(physicalStream, &physical);
            const uint32_t wanted = request.resolved.validBits
                                        ? request.resolved.validBits
                                        : request.resolved.containerBits;
            if (wanted > 0 && wanted != physical.mBitsPerChannel) {
                std::vector<AudioStreamBasicDescription> offered;
                if (availablePhysicalFormats(physicalStream, &offered)) {
                    for (const auto& f : offered) {
                        if (f.mBitsPerChannel != wanted) continue;
                        const bool wantFloat = request.resolved.isFloat != 0;
                        const bool isFloat = (f.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
                        if (isFloat != wantFloat) continue;
                        AudioObjectPropertyAddress addr{kAudioStreamPropertyPhysicalFormat,
                                                        kAudioObjectPropertyScopeGlobal,
                                                        kAudioObjectPropertyElementMain};
                        if (AudioObjectSetPropertyData(physicalStream, &addr, 0, nullptr,
                                                       sizeof(f), &f) == noErr) {
                            physicalFormat(physicalStream, &physical);
                            break;
                        }
                    }
                }
            }
            grantedBits = static_cast<int32_t>(physical.mBitsPerChannel);
            grantedFormat = sampleFormatOf(physical);
        }
        (void)grantedBits;
        // Width of one frame in the format that is now in force, for the silence
        // padding and for the AudioUnit's own format below.
        const bool integerPhysical =
            grantedFormat != NAE_SAMPLE_FORMAT_UNKNOWN &&
            !(physical.mFormatFlags & kAudioFormatFlagIsFloat);
        const uint32_t frameBits =
            integerPhysical && physical.mBitsPerChannel ? physical.mBitsPerChannel : 32u;

        // Hog after the rate change and before the AudioUnit exists.
        //
        // Three orderings were measured on the built-in output and all three are
        // unstable: before the rate change, between it and the unit, and after
        // the unit. Each produced both a passing stream and a stalled one across
        // runs, so this is not an ordering bug to be found by trying a fourth
        // arrangement -- the device itself does not tolerate hog and a running
        // AudioUnit reliably. This placement is kept because it is the one that
        // passed most often, and the instability is written down rather than
        // papered over with a retry that would hide it.
        // Wait for the device to be free before opening.
        //
        // Measured: three `output_stream_check` runs two seconds apart gave
        // FAILED, passed, passed. The first one starts while the previous
        // process's AudioUnit is still being torn down, and `AudioOutputUnitStart`
        // fails on a device that is mid-handover. A bounded poll on the device's
        // own state is the honest fix -- a sleep would be guessing how long the
        // teardown takes, and would still be wrong on a different machine.
        {
            AudioObjectPropertyAddress runningAddr{kAudioDevicePropertyDeviceIsRunning,
                                                   kAudioObjectPropertyScopeGlobal,
                                                   kAudioObjectPropertyElementMain};
            for (int attempt = 0; attempt < 50; ++attempt) {
                UInt32 running = 0;
                UInt32 size = sizeof(running);
                if (AudioObjectGetPropertyData(id, &runningAddr, 0, nullptr, &size, &running) !=
                        noErr ||
                    running == 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        // Hog, taken after the rate change has settled and before the AudioUnit
        // is created.
        //
        // Earlier measurements blamed hog for stalls that were actually the rate
        // switch racing the first IO start -- that was found and fixed above, and
        // hog was retried afterwards. `hog_probe` shows the call itself works
        // (noErr, owner becomes our pid). Not released between here and the unit
        // creation on purpose: a gap is a window another process can take the
        // device, which is the one thing hog exists to prevent.
        // A guard, so every `return` from here on gives the device back.
        //
        // The stream's destructor releases hog on the normal path, and the kernel
        // releases it when the process dies -- but between taking it and the
        // stream owning it there is a window (`make_unique` can throw, and any
        // early return between the two would strand it). A device left hogged is
        // silent for every other program on the machine, which is the most
        // expensive mistake this file can make.
        struct HogGuard {
            AudioDeviceID id;
            bool armed;
            ~HogGuard() {
                if (armed) setHog(id, -1);
            }
        } hog_guard{id, false};

        // Hog is this platform's exclusive mode, and it is taken only when the
        // caller asked for exclusive.
        //
        // A shared stream is the ordinary case: several programs play at once and
        // the system mixes them. Taking the device away in that case would make
        // every other application on the machine silent for no reason the
        // listener asked for -- which is the most expensive mistake this file can
        // make, in the opposite direction from leaking a hog.
        bool hogTaken = false;
        if (request.resolved.shareMode == NAE_SHARE_MODE_EXCLUSIVE) {
            if (setHog(id, getpid())) {
                hogTaken = true;
                hog_guard.armed = true;
            } else {
                pid_t other = -1;
                hogOwner(id, &other);
                std::fprintf(stderr, "[coreaudio] exclusive requested, hog refused, owner is %d\n",
                             other);
            }
        }
        auto stream = std::make_unique<CoreAudioStream>();
        stream->device = id;
        stream->hogged = hogTaken;  // taken above, released in the destructor
        stream->restoreRate = restoreRate;
        hog_guard.armed = false;    // the stream owns it now
        stream->render = request.render;
        stream->watchLoss();
        stream->channels = channels;
        stream->bytesPerFrame = channels * ((frameBits + 7) / 8);
        // What the HAL reports, in milliseconds. The ring this stream feeds adds
        // its own depth on top; the caller accounts for that, because only the
        // caller knows how full it keeps it.
        stream->latency = 0.0;
        {
            AudioStreamID latStream = 0;
            double reported = 0.0;
            if (firstOutputStream(id, &latStream) &&
                reportedLatencyMs(id, latStream, static_cast<double>(rate),
                                  request.bufferFrames, &reported)) {
                stream->latency = reported;
            }
        }

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
        format.mChannelsPerFrame = channels;
        format.mFramesPerPacket = 1;
        // The unit takes the device's physical format, so the engine's final
        // bytes pass through unchanged. Integer is packed and interleaved here;
        // 24-bit is 3 bytes per sample, not 4.
        if (integerPhysical) {
            format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
            format.mBitsPerChannel = physical.mBitsPerChannel;
            format.mBytesPerFrame = channels * ((physical.mBitsPerChannel + 7) / 8);
        } else {
            format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
            format.mBitsPerChannel = 32;
            format.mBytesPerFrame = channels * sizeof(float);
        }
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
