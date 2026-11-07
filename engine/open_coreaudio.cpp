// The macOS device open, on its own.
//
// It used to be the `#else` half of an `#ifdef _WIN32` block in `nae.cpp`, two
// thousand lines from the WASAPI half. Every change here had to be checked on
// both platforms because the other half was in the same file, and the compiler
// could not say which half a mistake was in. Now the file boundary is the
// platform boundary: this translation unit is not compiled on Windows at all.
#include "nae_internal.h"

#ifndef _WIN32

// macOS. Same contract, filled from what the device advertises.
//
// `bitPerfect` follows the definition the listener-facing guide uses -- no rate
// conversion and no requantisation -- but this platform has one route and it is
// shared, so it is forced to zero with `SharedMode`, the same rule the Windows
// path enforces. The container the samples end up in is a separate question and
// is reported through `isFloat` and `containerBits`.
extern "C" int32_t openExclusiveImpl(const char* deviceId, int32_t sourceRate, int32_t outputMode,
                          int32_t fixedRate, int32_t quality, AUDCLNT_SHAREMODE shareMode,
                          NAE_Handle** out, int32_t* outSampleRate, int32_t* outSourceRate,
                          int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                          int32_t* isFloat, int32_t* resampling, int32_t* dithered,
                          int32_t* bitPerfect, double* latencyMs, int32_t bufferMode,
                          uint32_t bufferMs) {
    (void)shareMode;
    (void)bufferMode;
    if (!out) return NAE_ERR_INVALID_ARGUMENT;
    // Every out-parameter starts at zero, as the Windows implementation does: a
    // caller that ignores a failure must not be left reading its own stale
    // values as though the engine had produced them.
    *out = nullptr;
    if (outSampleRate) *outSampleRate = 0;
    if (outSourceRate) *outSourceRate = 0;
    if (channels) *channels = 0;
    if (bitsPerSample) *bitsPerSample = 0;
    if (validBits) *validBits = 0;
    if (isFloat) *isFloat = 0;
    if (resampling) *resampling = 0;
    if (dithered) *dithered = 0;
    if (bitPerfect) *bitPerfect = 0;
    if (latencyMs) *latencyMs = 0.0;

    auto backend = neiro::createPlatformBackend();
    if (!backend) {
        g_lastErrorText = "no audio backend on this platform";
        return NAE_ERR_NO_DEVICE;
    }
    NAE_DeviceCaps caps{};
    caps.structSize = sizeof(caps);
    std::string error;
    if (!backend->queryCaps(deviceId ? deviceId : "", NAE_SHARE_MODE_EXCLUSIVE, &caps, &error)) {
        g_lastErrorText = error.empty() ? "no output device" : error;
        return NAE_ERR_NO_DEVICE;
    }

    uint32_t rate = caps.nativeRate ? caps.nativeRate : 48000;
    if (outputMode == NAE_OUTPUT_FIXED && fixedRate > 0) {
        rate = static_cast<uint32_t>(fixedRate);
    } else if (outputMode == NAE_OUTPUT_NATIVE && sourceRate > 0) {
        rate = static_cast<uint32_t>(sourceRate);
    }

    const uint32_t ch = caps.nativeChannels ? caps.nativeChannels : 2;
    const uint32_t container = caps.nativeContainerBits ? caps.nativeContainerBits : 32;
    const uint32_t valid = caps.nativeValidBits ? caps.nativeValidBits : container;
    const bool deviceIsFloat = caps.nativeFormat == NAE_SAMPLE_FORMAT_F32 ||
                               caps.nativeFormat == NAE_SAMPLE_FORMAT_F64;

    neiro::StreamFormatRequest want;
    want.deviceId = deviceId ? deviceId : "";
    want.rate = rate;
    want.channels = ch;
    want.containerBits = container;
    want.validBits = valid;
    want.isFloat = deviceIsFloat;
    // The one thing the backend needs the share mode for: exclusive takes the
    // device away from other processes (hog), shared must not.
    want.shareMode = (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? NAE_SHARE_MODE_EXCLUSIVE
                                                               : NAE_SHARE_MODE_SHARED;
    want.bufferFrames = bufferMs ? (rate * bufferMs) / 1000u : 0;

    auto stream = neiro::openCoreAudioStream(want, 8192, &error);
    if (!stream) {
        g_lastErrorText = error.empty() ? "the device refused to open" : error;
        return NAE_ERR_INITIALIZE;
    }

    NAE_Handle* handle = nullptr;
    try {
        handle = new NAE_Handle();
    } catch (...) {
        g_lastErrorText = "no memory for the handle";
        return NAE_ERR_NO_MEMORY;
    }
    handle->stream = stream;
    handle->fmt.rate = rate;
    handle->fmt.channels = ch;
    handle->fmt.containerBits = container;
    handle->fmt.validBits = valid;
    handle->fmt.blockAlign = ch * ((container + 7u) / 8u);
    handle->fmt.isFloat = deviceIsFloat;
    handle->bufferFrames = stream->capacityFrames();
    handle->latencyMs = stream->latencyMs();
    handle->sourceRate = sourceRate > 0 ? static_cast<uint32_t>(sourceRate) : rate;
    // The converter is the difference between "the device runs at 96 kHz" and
    // "the device is fed 44.1k samples as though they were 96k", and only the
    // first one is playback. This was missing here while `resampling` below was
    // still computed from the rates, so the report said a conversion had happened
    // and the samples had gone through untouched -- a tone that should last
    // 500 ms finished in 265. Prepare it once, at open, the same way the WASAPI
    // branch does.
    handle->resampling = handle->sourceRate != handle->fmt.rate;
    if (handle->resampling) {
        std::string err;
        const auto q = static_cast<neiro::ResamplerQuality>(
            quality >= 0 && quality <= 2 ? quality : static_cast<int32_t>(neiro::ResamplerQuality::High));
        if (!handle->resampler.prepare(static_cast<int>(ch), handle->sourceRate,
                                      handle->fmt.rate, q, &err)) {
            g_lastErrorText = err.empty() ? "no resampler" : err;
            delete handle;
            return NAE_ERR_INITIALIZE;
        }
    }
    // What was asked for and what the device gave. Both are known here; leaving
    // them at zero made the signal path read "the device granted 0 bits".
    handle->requestedBits = static_cast<uint16_t>(container);
    handle->grantedBits = static_cast<uint16_t>(valid);
    // This platform has one route and it is shared. Saying otherwise here would
    // contradict `caps.exclusiveSupported`, which reports 0 for the same reason.
    handle->exclusive = false;

    const int32_t didResample = handle->resampling ? 1 : 0;
    if (outSampleRate) *outSampleRate = static_cast<int32_t>(rate);
    if (outSourceRate) *outSourceRate = static_cast<int32_t>(handle->sourceRate);
    if (channels) *channels = static_cast<int32_t>(ch);
    if (bitsPerSample) *bitsPerSample = static_cast<int32_t>(container);
    if (validBits) *validBits = static_cast<int32_t>(valid);
    if (isFloat) *isFloat = deviceIsFloat ? 1 : 0;
    if (resampling) *resampling = didResample;
    if (dithered) *dithered = 0;
    // Shared is not bit-perfect, whatever the rate negotiation concluded -- the
    // same rule the Windows path enforces, and the one nae.h states.
    if (bitPerfect) *bitPerfect = 0;
    handle->reason = neiro::PerfectReason::SharedMode;
    if (latencyMs) *latencyMs = handle->latencyMs;

    *out = handle;
    return NAE_OK;
}

/// The device the system chose, in the same shape the Windows path returns.
///
/// Here rather than in `nae.cpp` for the same reason as the open: it is the
/// platform's device knowledge, and a change to it should not be able to touch
/// the Windows build.
extern "C" char* NAE_GetDefaultDeviceId(void) NAE_NOEXCEPT {
    // The backend enumerates and says which device the system chose.
    auto backend = neiro::createPlatformBackend();
    if (!backend) return nullptr;
    std::string error;
    const auto devices = backend->enumerate(&error);
    const neiro::BackendDevice* pick = nullptr;
    for (const auto& d : devices) {
        if (d.isDefault && !d.id.empty()) {
            pick = &d;
            break;
        }
    }
    // No fallback to the first device. This function promises the *default*
    // one; returning an arbitrary device would let a caller act on a premise
    // that is false. A system with no default output is a real state.
    if (!pick) {
        g_lastErrorText = "the system has no default output device";
        return nullptr;
    }
    auto* out = static_cast<char*>(std::malloc(pick->id.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, pick->id.c_str(), pick->id.size() + 1);
    return out;
}

#endif  // !_WIN32
