// Output-policy resolution and diagnostics.
//
// This file is deliberately platform-neutral: no Windows headers, no device
// handles. It can be compiled on macOS/Linux and tested with synthetic caps.
// The backend supplies NAE_DeviceCaps; this resolver decides and explains.

#include "nae.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {

constexpr uint32_t kVersion = NAE_OUTPUT_POLICY_VERSION;

uint32_t bitCount(uint32_t mask) {
    uint32_t n = 0;
    while (mask) {
        mask &= mask - 1;
        ++n;
    }
    return n;
}

int bitIndex(uint32_t mask, uint32_t bit) {
    if (!(mask & bit)) return -1;
    int index = 0;
    for (uint32_t b = 1; b != bit; b <<= 1) {
        if (mask & b) ++index;
    }
    return index;
}

uint32_t channelBitByIndex(uint32_t mask, int index) {
    int n = 0;
    for (uint32_t b = 1; b != 0; b <<= 1) {
        if (mask & b) {
            if (n == index) return b;
            ++n;
        }
    }
    return 0;
}

bool hasChannel(uint32_t mask, uint32_t bit) {
    return (mask & bit) != 0;
}

void setGain(NAE_RoutingMatrix* m, uint32_t targetMask, uint32_t targetBit,
             uint32_t sourceMask, uint32_t sourceBit, float real, float quad = 0.0f) {
    const int t = bitIndex(targetMask, targetBit);
    const int s = bitIndex(sourceMask, sourceBit);
    if (t < 0 || s < 0) return;
    m->gain[t][s].real = real;
    m->gain[t][s].quadrature = quad;
}

uint32_t defaultTargetForPreset(int32_t preset) {
    switch (preset) {
        case NAE_UPMIX_MONO_TO_STEREO: return NAE_LAYOUT_STEREO;
        case NAE_UPMIX_MONO_TO_CENTER: return NAE_LAYOUT_5_1;
        case NAE_UPMIX_MONO_TO_ALL: return NAE_LAYOUT_5_1;
        case NAE_UPMIX_STEREO_TO_3_0: return NAE_LAYOUT_3_0;
        case NAE_UPMIX_STEREO_TO_4_0: return NAE_LAYOUT_4_0;
        case NAE_UPMIX_STEREO_TO_5_0: return NAE_LAYOUT_5_0;
        case NAE_UPMIX_STEREO_TO_5_1: return NAE_LAYOUT_5_1;
        case NAE_UPMIX_STEREO_TO_5_1_BASS: return NAE_LAYOUT_5_1;
        case NAE_UPMIX_STEREO_TO_6_1: return NAE_LAYOUT_6_1;
        case NAE_UPMIX_STEREO_TO_7_1: return NAE_LAYOUT_7_1;
        case NAE_UPMIX_5_1_TO_6_1: return NAE_LAYOUT_6_1;
        case NAE_UPMIX_5_1_TO_7_1: return NAE_LAYOUT_7_1;
        case NAE_UPMIX_STEREO_TO_5_1_2: return NAE_LAYOUT_5_1_2;
        case NAE_UPMIX_STEREO_TO_5_1_4: return NAE_LAYOUT_5_1_4;
        case NAE_UPMIX_STEREO_TO_7_1_2: return NAE_LAYOUT_7_1_2;
        case NAE_UPMIX_STEREO_TO_7_1_4: return NAE_LAYOUT_7_1_4;
        case NAE_UPMIX_5_1_TO_5_1_2: return NAE_LAYOUT_5_1_2;
        case NAE_UPMIX_5_1_TO_5_1_4: return NAE_LAYOUT_5_1_4;
        case NAE_UPMIX_7_1_TO_7_1_2: return NAE_LAYOUT_7_1_2;
        case NAE_UPMIX_7_1_TO_7_1_4: return NAE_LAYOUT_7_1_4;
        default: return 0;
    }
}

uint32_t defaultTargetForDownmix(int32_t preset) {
    switch (preset) {
        case NAE_DOWNMIX_5_1_TO_3_0:
        case NAE_DOWNMIX_7_1_TO_3_0: return NAE_LAYOUT_3_0;
        case NAE_DOWNMIX_5_1_TO_2_1: return NAE_LAYOUT_2_1;
        case NAE_DOWNMIX_STANDARD:
        case NAE_DOWNMIX_5_1_TO_STEREO:
        case NAE_DOWNMIX_7_1_TO_STEREO:
        case NAE_DOWNMIX_ITU_R_775:
        case NAE_DOWNMIX_LORO:
        case NAE_DOWNMIX_LTRT: return NAE_LAYOUT_STEREO;
        case NAE_DOWNMIX_7_1_TO_5_1: return NAE_LAYOUT_5_1;
        case NAE_DOWNMIX_5_1_2_TO_5_1:
        case NAE_DOWNMIX_5_1_4_TO_5_1: return NAE_LAYOUT_5_1;
        case NAE_DOWNMIX_7_1_2_TO_7_1:
        case NAE_DOWNMIX_7_1_4_TO_7_1: return NAE_LAYOUT_7_1;
        default: return 0;
    }
}

void addDiag(NAE_ResolvedSignalPath* out, int32_t severity, int32_t code,
             uint32_t field, int32_t requested, int32_t resolved, uint32_t value,
             const std::string& text) {
    if (out->diagnosticCount >= NAE_MAX_DIAGNOSTICS) return;
    NAE_Diagnostic& d = out->diagnostics[out->diagnosticCount++];
    d.structSize = sizeof(d);
    d.severity = severity;
    d.code = code;
    d.field = field;
    d.requested = requested;
    d.resolved = resolved;
    d.value = value;
    std::snprintf(d.message, sizeof(d.message), "%s", text.c_str());
}

void addAuto(NAE_ResolvedSignalPath* out, uint32_t field, int32_t choice,
             int32_t reason, uint32_t value, const std::string& text) {
    if (out->autoDecisionCount < 16) {
        NAE_AutoDecision& a = out->autoDecisions[out->autoDecisionCount++];
        a.field = field;
        a.choice = choice;
        a.reason = reason;
        a.value = value;
    }
    addDiag(out, NAE_DIAG_AUTO, NAE_DIAG_AUTO_CHOICE, field, -1, choice, value, text);
}

bool rateProbed(const NAE_DeviceCaps& c) {
    return (c.probed & NAE_CAPS_PROBED_RATES) != 0;
}

bool hasRate(const NAE_DeviceCaps& c, uint32_t rate) {
    for (uint32_t i = 0; i < c.rateCount && i < NAE_MAX_DEVICE_RATES; ++i) {
        if (c.rates[i] == rate) return true;
    }
    return false;
}

uint32_t nearestRate(const NAE_DeviceCaps& c, uint32_t want) {
    uint32_t best = 0;
    double bestDistance = 0.0;
    for (uint32_t i = 0; i < c.rateCount && i < NAE_MAX_DEVICE_RATES; ++i) {
        const uint32_t r = c.rates[i];
        if (r == 0) continue;
        const double d = std::fabs(std::log(static_cast<double>(want) /
                                            static_cast<double>(r)));
        if (best == 0 || d < bestDistance) {
            best = r;
            bestDistance = d;
        }
    }
    return best;
}

bool hasDopCarrier(const NAE_DeviceCaps& c, uint32_t carrier) {
    for (uint32_t i = 0; i < c.dopCarrierCount && i < NAE_MAX_DOP_CARRIERS; ++i) {
        if (c.dopCarriers[i] == carrier) return true;
    }
    return false;
}

bool hasNativeDsd(const NAE_DeviceCaps& c, uint32_t rate) {
    for (uint32_t i = 0; i < c.nativeDsdRateCount && i < NAE_MAX_NATIVE_DSD_RATES; ++i) {
        if (c.nativeDsdRates[i] == rate) return true;
    }
    return false;
}

bool choosePcmRate(const NAE_DeviceCaps& c, const NAE_OutputPolicy& p,
                   uint32_t sourceRate, uint32_t* outRate, int32_t* reason,
                   bool* coerced, NAE_ResolvedSignalPath* r) {
    *coerced = false;
    if (!rateProbed(c)) {
        r->status = NAE_RESOLVE_UNPROBED;
        std::snprintf(r->failure, sizeof(r->failure), "device rates were not probed");
        addDiag(r, NAE_DIAG_ERROR, NAE_DIAG_UNPROBED, NAE_AUTO_FIELD_RATE, -1, -1, 0,
                "Cannot choose a rate: the device rates were not probed.");
        return false;
    }
    switch (p.ratePolicy) {
        case NAE_RATE_NATIVE:
            if (!hasRate(c, sourceRate)) {
                r->status = NAE_RESOLVE_UNSUPPORTED_RATE;
                std::snprintf(r->failure, sizeof(r->failure),
                              "source rate %u is not in the device's probed rates",
                              sourceRate);
                addDiag(r, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_RATE,
                        NAE_RATE_NATIVE, -1, sourceRate,
                        "Native output requested, but the device does not list the source rate.");
                return false;
            }
            *outRate = sourceRate;
            *reason = NAE_RESOLVE_REASON_SOURCE_RATE_SUPPORTED;
            return true;
        case NAE_RATE_FIXED:
            if (p.fixedRate == 0 || !hasRate(c, p.fixedRate)) {
                r->status = NAE_RESOLVE_UNSUPPORTED_RATE;
                std::snprintf(r->failure, sizeof(r->failure),
                              "fixed rate %u is not supported", p.fixedRate);
                addDiag(r, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_RATE,
                        NAE_RATE_FIXED, -1, p.fixedRate,
                        "The fixed output rate is not supported; it is not silently coerced.");
                return false;
            }
            *outRate = p.fixedRate;
            *reason = NAE_RESOLVE_REASON_FIXED_RATE;
            return true;
        case NAE_RATE_DEVICE:
            if (c.nativeRate == 0 || !hasRate(c, c.nativeRate)) {
                r->status = NAE_RESOLVE_UNSUPPORTED_RATE;
                std::snprintf(r->failure, sizeof(r->failure),
                              "device native rate %u is not supported", c.nativeRate);
                addDiag(r, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_RATE,
                        NAE_RATE_DEVICE, -1, c.nativeRate,
                        "The device native rate is not available.");
                return false;
            }
            *outRate = c.nativeRate;
            *reason = NAE_RESOLVE_REASON_DEVICE_NATIVE_RATE;
            return true;
        case NAE_RATE_AUTO:
        default:
            if (hasRate(c, sourceRate)) {
                *outRate = sourceRate;
                *reason = NAE_RESOLVE_REASON_SOURCE_RATE_SUPPORTED;
                return true;
            }
            if (c.nativeRate && hasRate(c, c.nativeRate)) {
                *outRate = c.nativeRate;
                *reason = NAE_RESOLVE_REASON_DEVICE_NATIVE_RATE;
                *coerced = true;
                return true;
            }
            *outRate = nearestRate(c, sourceRate);
            *reason = NAE_RESOLVE_REASON_NEAREST_SUPPORTED_RATE;
            *coerced = true;
            return *outRate != 0;
    }
}

int32_t reasonForFallback(int32_t dsdRatePolicy) {
    switch (dsdRatePolicy) {
        case NAE_DSD_RATE_PCM_FALLBACK: return NAE_RESOLVE_REASON_DSD_PCM_FALLBACK;
        case NAE_DSD_RATE_DOWNRATE: return NAE_RESOLVE_REASON_DSD_DOWNRATED;
        case NAE_DSD_RATE_EXTRACT: return NAE_RESOLVE_REASON_DSD_EXTRACTED;
        default: return NAE_RESOLVE_REASON_NONE;
    }
}

}  // namespace

extern "C" void NAE_OutputPolicyDefaults(NAE_OutputPolicy* out) NAE_POLICY_NOEXCEPT {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->version = kVersion;
    out->ratePolicy = NAE_RATE_AUTO;
    out->resampleAlgorithm = NAE_RESAMPLE_ALGORITHM_AUTO;
    out->resampleQuality = NAE_RESAMPLE_QUALITY_AUTO;
    out->ditherMode = NAE_DITHER_AUTO;
    out->dsdMode = NAE_DSD_MODE_AUTO;
    out->dsdTransport = NAE_DSD_TRANSPORT_AUTO;
    out->dsdRatePolicy = NAE_DSD_RATE_AUTO;
    out->allowPcmFallback = 1;
    out->allowDownrate = 0;
    out->allowExtractRate = 1;
    out->bufferMode = NAE_BUFFER_AUTO;
    out->routingMode = NAE_ROUTING_AUTO;
    out->upmixPreset = NAE_UPMIX_NONE;
    out->downmixPreset = NAE_DOWNMIX_ITU_R_775;
    out->bassManagement = NAE_BASS_AUTO;
    out->lfeMode = NAE_LFE_AUTO;
    out->lfeCrossoverHz = 80.0;
    out->channelMode = NAE_CHANNEL_MODE_NORMAL;
    out->clipGuardMode = NAE_CLIP_GUARD_AUTO;
    out->clipCeilingDbTp = -1.0;
    out->clipLookaheadMs = 5.0;
    out->clipReleaseMs = 100.0;
}

extern "C" int32_t NAE_BuildRoutingMatrix(int32_t routingMode, int32_t preset,
                                          uint32_t sourceMask, uint32_t targetMask,
                                          NAE_RoutingMatrix* out) NAE_POLICY_NOEXCEPT {
    if (!out || sourceMask == 0) return NAE_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->version = kVersion;
    out->sourceMask = sourceMask;
    out->targetMask = targetMask;
    out->sourceChannels = bitCount(sourceMask);
    out->targetChannels = bitCount(targetMask);
    out->normalize = NAE_ROUTING_NORMALIZE_NONE;
    out->phaseMode = NAE_PHASE_NONE;
    out->alignPhaseLatency = 1;
    out->bassManagement = NAE_BASS_AUTO;
    out->lfeMode = NAE_LFE_AUTO;
    out->lfeCrossoverHz = 0.0;
    out->lfeGainDb = 0.0;

    const bool isUpmix = routingMode == NAE_ROUTING_UPMIX;
    const bool isDownmix = routingMode == NAE_ROUTING_DOWNMIX;
    if (!isUpmix && !isDownmix) return NAE_ERR_INVALID_ARGUMENT;

    if (targetMask == 0) {
        targetMask = isUpmix ? defaultTargetForPreset(preset) : defaultTargetForDownmix(preset);
        out->targetMask = targetMask;
        out->targetChannels = bitCount(targetMask);
    }
    if (targetMask == 0) return NAE_ERR_INVALID_ARGUMENT;

    const uint32_t L = NAE_CHANNEL_FL, R = NAE_CHANNEL_FR, C = NAE_CHANNEL_FC;
    const uint32_t LFE = NAE_CHANNEL_LFE, BL = NAE_CHANNEL_BL, BR = NAE_CHANNEL_BR;
    const uint32_t BC = NAE_CHANNEL_BC, SL = NAE_CHANNEL_SL, SR = NAE_CHANNEL_SR;
    const uint32_t TFL = NAE_CHANNEL_TFL, TFR = NAE_CHANNEL_TFR;
    const uint32_t TBL = NAE_CHANNEL_TBL, TBR = NAE_CHANNEL_TBR;
    const float k3 = 0.70710678f;

    if (isUpmix) {
        const int sourceChannels = static_cast<int>(out->sourceChannels);
        if (preset == NAE_UPMIX_MONO_TO_STEREO && sourceChannels == 1) {
            setGain(out, targetMask, L, sourceMask, channelBitByIndex(sourceMask, 0), 1.0f);
            setGain(out, targetMask, R, sourceMask, channelBitByIndex(sourceMask, 0), 1.0f);
            return NAE_OK;
        }
        if (preset == NAE_UPMIX_MONO_TO_CENTER) {
            setGain(out, targetMask, C, sourceMask, channelBitByIndex(sourceMask, 0), 1.0f);
            return NAE_OK;
        }
        if (preset == NAE_UPMIX_MONO_TO_ALL) {
            uint32_t nonLfe = 0;
            for (uint32_t b = 1; b != 0; b <<= 1) {
                if ((targetMask & b) && b != LFE) ++nonLfe;
            }
            if (nonLfe == 0) return NAE_ERR_INVALID_ARGUMENT;
            const float g = 1.0f / std::sqrt(static_cast<float>(nonLfe));
            for (uint32_t b = 1; b != 0; b <<= 1) {
                if ((targetMask & b) && b != LFE) {
                    setGain(out, targetMask, b, sourceMask, channelBitByIndex(sourceMask, 0), g);
                }
            }
            return NAE_OK;
        }
        if (preset == NAE_UPMIX_5_1_TO_6_1 || preset == NAE_UPMIX_5_1_TO_7_1 ||
            preset == NAE_UPMIX_5_1_TO_5_1_2 || preset == NAE_UPMIX_5_1_TO_5_1_4 ||
            preset == NAE_UPMIX_7_1_TO_7_1_2 || preset == NAE_UPMIX_7_1_TO_7_1_4) {
            setGain(out, targetMask, L, sourceMask, L, 1.0f);
            setGain(out, targetMask, R, sourceMask, R, 1.0f);
            if (targetMask & C) setGain(out, targetMask, C, sourceMask, C, 1.0f);
            if (targetMask & LFE) setGain(out, targetMask, LFE, sourceMask, LFE, 1.0f);
            if (targetMask & BL) setGain(out, targetMask, BL, sourceMask, BL, 1.0f);
            if (targetMask & BR) setGain(out, targetMask, BR, sourceMask, BR, 1.0f);
            if (targetMask & SL) {
                if (hasChannel(sourceMask, SL)) setGain(out, targetMask, SL, sourceMask, SL, 1.0f);
                else if (preset == NAE_UPMIX_5_1_TO_7_1) setGain(out, targetMask, SL, sourceMask, BL, 1.0f);
            }
            if (targetMask & SR) {
                if (hasChannel(sourceMask, SR)) setGain(out, targetMask, SR, sourceMask, SR, 1.0f);
                else if (preset == NAE_UPMIX_5_1_TO_7_1) setGain(out, targetMask, SR, sourceMask, BR, 1.0f);
            }
            if (targetMask & BC && preset == NAE_UPMIX_5_1_TO_6_1) {
                setGain(out, targetMask, BC, sourceMask, BL, k3);
                setGain(out, targetMask, BC, sourceMask, BR, k3);
            }
            if (targetMask & TFL) setGain(out, targetMask, TFL, sourceMask, L, 0.5f);
            if (targetMask & TFR) setGain(out, targetMask, TFR, sourceMask, R, 0.5f);
            if (targetMask & TBL) setGain(out, targetMask, TBL, sourceMask, L, 0.5f);
            if (targetMask & TBR) setGain(out, targetMask, TBR, sourceMask, R, 0.5f);
            return NAE_OK;
        }
        if (sourceChannels != 2) return NAE_ERR_INVALID_ARGUMENT;
        const uint32_t l = L, r = R;
        setGain(out, targetMask, L, sourceMask, l, 1.0f);
        setGain(out, targetMask, R, sourceMask, r, 1.0f);
        if (targetMask & C) {
            setGain(out, targetMask, C, sourceMask, l, k3);
            setGain(out, targetMask, C, sourceMask, r, k3);
        }
        if (targetMask & BL) setGain(out, targetMask, BL, sourceMask, l, k3);
        if (targetMask & BR) setGain(out, targetMask, BR, sourceMask, r, k3);
        if (targetMask & SL) setGain(out, targetMask, SL, sourceMask, l, 0.5f);
        if (targetMask & SR) setGain(out, targetMask, SR, sourceMask, r, 0.5f);
        if (targetMask & BC) setGain(out, targetMask, BC, sourceMask, l, 0.5f);
        if (targetMask & BC) setGain(out, targetMask, BC, sourceMask, r, 0.5f);
        if (targetMask & TFL) setGain(out, targetMask, TFL, sourceMask, l, 0.5f);
        if (targetMask & TFR) setGain(out, targetMask, TFR, sourceMask, r, 0.5f);
        if (targetMask & TBL) setGain(out, targetMask, TBL, sourceMask, l, 0.5f);
        if (targetMask & TBR) setGain(out, targetMask, TBR, sourceMask, r, 0.5f);
        if (targetMask & LFE && preset == NAE_UPMIX_STEREO_TO_5_1_BASS) {
            // The actual 80 Hz low-pass is a runtime stage; the gain is marked
            // by the lfeMode/bass management policy, not a second matrix tap.
        }
        if (preset == NAE_UPMIX_5_1_TO_6_1) {
            setGain(out, targetMask, BC, sourceMask, BL, k3);
            setGain(out, targetMask, BC, sourceMask, BR, k3);
        }
        if (preset == NAE_UPMIX_5_1_TO_7_1) {
            setGain(out, targetMask, SL, sourceMask, BL, 1.0f);
            setGain(out, targetMask, SR, sourceMask, BR, 1.0f);
        }
        return NAE_OK;
    }

    // Downmix.
    const bool has6 = hasChannel(sourceMask, BL) && hasChannel(sourceMask, BR);
    const bool has7 = has6 && hasChannel(sourceMask, SL) && hasChannel(sourceMask, SR);
    if (preset == NAE_DOWNMIX_LTRT) {
        if (!has6 || !hasChannel(sourceMask, L) || !hasChannel(sourceMask, R) ||
            !hasChannel(sourceMask, C)) {
            return NAE_ERR_INVALID_ARGUMENT;
        }
        setGain(out, targetMask, L, sourceMask, L, 1.0f);
        setGain(out, targetMask, R, sourceMask, R, 1.0f);
        setGain(out, targetMask, L, sourceMask, C, k3);
        setGain(out, targetMask, R, sourceMask, C, k3);
        if (has7) {
            setGain(out, targetMask, L, sourceMask, BL, 0.0f, 0.5f);
            setGain(out, targetMask, L, sourceMask, SL, 0.0f, 0.5f);
            setGain(out, targetMask, R, sourceMask, BR, 0.0f, -0.5f);
            setGain(out, targetMask, R, sourceMask, SR, 0.0f, -0.5f);
        } else {
            setGain(out, targetMask, L, sourceMask, BL, 0.0f, k3);
            setGain(out, targetMask, R, sourceMask, BR, 0.0f, -k3);
        }
        // The parallel all-pass pair: 12 first-order sections, no group delay,
        // 0.586 degrees worst-case over 20 Hz to 20 kHz. The 255-tap FIR is
        // still selectable by setting phaseMode/phaseTaps on the matrix, but its
        // accuracy collapses below about 190 Hz (measured 21 dB amplitude error
        // at 20 Hz), so it is not the default for an LtRt downmix.
        out->phaseMode = NAE_PHASE_IIR_ALLPASS_90;
        out->phaseOrder = 6;
        out->phaseTaps = 0;
        out->alignPhaseLatency = 0;
        return NAE_OK;
    }
    if (preset == NAE_DOWNMIX_5_1_TO_3_0 || preset == NAE_DOWNMIX_7_1_TO_3_0) {
        setGain(out, targetMask, L, sourceMask, L, 1.0f);
        setGain(out, targetMask, R, sourceMask, R, 1.0f);
        if (hasChannel(sourceMask, C)) setGain(out, targetMask, C, sourceMask, C, 1.0f);
        if (has6) {
            setGain(out, targetMask, L, sourceMask, BL, k3);
            setGain(out, targetMask, R, sourceMask, BR, k3);
        }
        return NAE_OK;
    }
    if (preset == NAE_DOWNMIX_STANDARD || preset == NAE_DOWNMIX_5_1_TO_2_1 ||
        preset == NAE_DOWNMIX_5_1_TO_STEREO || preset == NAE_DOWNMIX_7_1_TO_STEREO ||
        preset == NAE_DOWNMIX_ITU_R_775 || preset == NAE_DOWNMIX_LORO) {
        setGain(out, targetMask, L, sourceMask, L, 1.0f);
        setGain(out, targetMask, R, sourceMask, R, 1.0f);
        if (hasChannel(sourceMask, C)) {
            setGain(out, targetMask, L, sourceMask, C, k3);
            setGain(out, targetMask, R, sourceMask, C, k3);
        }
        if (has6) {
            setGain(out, targetMask, L, sourceMask, BL, k3);
            setGain(out, targetMask, R, sourceMask, BR, k3);
        }
        if (has7) {
            setGain(out, targetMask, L, sourceMask, SL, 0.5f);
            setGain(out, targetMask, R, sourceMask, SR, 0.5f);
        }
        if (preset == NAE_DOWNMIX_5_1_TO_2_1 && hasChannel(sourceMask, LFE) &&
            (targetMask & LFE)) {
            setGain(out, targetMask, LFE, sourceMask, LFE, 1.0f);
        }
        return NAE_OK;
    }
    if (preset == NAE_DOWNMIX_7_1_TO_5_1) {
        setGain(out, targetMask, L, sourceMask, L, 1.0f);
        setGain(out, targetMask, R, sourceMask, R, 1.0f);
        if (hasChannel(sourceMask, C)) setGain(out, targetMask, C, sourceMask, C, 1.0f);
        if (hasChannel(sourceMask, LFE)) setGain(out, targetMask, LFE, sourceMask, LFE, 1.0f);
        if (has7) {
            setGain(out, targetMask, BL, sourceMask, BL, k3);
            setGain(out, targetMask, BL, sourceMask, SL, k3);
            setGain(out, targetMask, BR, sourceMask, BR, k3);
            setGain(out, targetMask, BR, sourceMask, SR, k3);
        }
        return NAE_OK;
    }
    // Immersive fold-downs.
    if (preset == NAE_DOWNMIX_5_1_2_TO_5_1 || preset == NAE_DOWNMIX_5_1_4_TO_5_1 ||
        preset == NAE_DOWNMIX_7_1_2_TO_7_1 || preset == NAE_DOWNMIX_7_1_4_TO_7_1) {
        setGain(out, targetMask, L, sourceMask, L, 1.0f);
        setGain(out, targetMask, R, sourceMask, R, 1.0f);
        if (hasChannel(sourceMask, C)) setGain(out, targetMask, C, sourceMask, C, 1.0f);
        if (hasChannel(sourceMask, LFE)) setGain(out, targetMask, LFE, sourceMask, LFE, 1.0f);
        if (has6) {
            setGain(out, targetMask, BL, sourceMask, BL, 1.0f);
            setGain(out, targetMask, BR, sourceMask, BR, 1.0f);
        }
        if (has7) {
            setGain(out, targetMask, SL, sourceMask, SL, 1.0f);
            setGain(out, targetMask, SR, sourceMask, SR, 1.0f);
        }
        setGain(out, targetMask, L, sourceMask, TFL, 0.5f);
        setGain(out, targetMask, R, sourceMask, TFR, 0.5f);
        setGain(out, targetMask, L, sourceMask, TBL, 0.5f);
        setGain(out, targetMask, R, sourceMask, TBR, 0.5f);
        return NAE_OK;
    }
    return NAE_ERR_INVALID_ARGUMENT;
}

namespace {
std::mutex g_matrixMutex;
std::map<uint32_t, NAE_RoutingMatrix> g_matrices;
uint32_t g_nextMatrixId = 1;
}  // namespace

extern "C" int32_t NAE_RegisterRoutingMatrix(const NAE_RoutingMatrix* matrix,
                                             uint32_t* outId) NAE_POLICY_NOEXCEPT {
    if (!matrix || !outId || matrix->structSize != sizeof(NAE_RoutingMatrix)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (matrix->sourceChannels == 0 || matrix->targetChannels == 0 ||
        matrix->sourceChannels > NAE_MAX_MATRIX_CHANNELS ||
        matrix->targetChannels > NAE_MAX_MATRIX_CHANNELS) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_matrixMutex);
    const uint32_t id = g_nextMatrixId++;
    g_matrices[id] = *matrix;
    *outId = id;
    return NAE_OK;
}

extern "C" void NAE_ReleaseRoutingMatrix(uint32_t matrixId) NAE_POLICY_NOEXCEPT {
    std::lock_guard<std::mutex> lock(g_matrixMutex);
    g_matrices.erase(matrixId);
}

extern "C" int32_t NAE_GetRoutingMatrix(uint32_t matrixId,
                                        NAE_RoutingMatrix* out) NAE_POLICY_NOEXCEPT {
    if (!out || out->structSize != sizeof(NAE_RoutingMatrix)) return NAE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_matrixMutex);
    auto it = g_matrices.find(matrixId);
    if (it == g_matrices.end()) return NAE_ERR_NOT_OPEN;
    *out = it->second;
    return NAE_OK;
}

extern "C" int32_t NAE_ResolveSignalPath(const NAE_DeviceCaps* caps,
                                         const NAE_SourceInfo* source,
                                         const NAE_OutputPolicy* policy,
                                         NAE_ResolvedSignalPath* out) NAE_POLICY_NOEXCEPT {
    if (!caps || !source || !policy || !out) return NAE_ERR_INVALID_ARGUMENT;
    if (caps->structSize != sizeof(NAE_DeviceCaps) ||
        source->structSize != sizeof(NAE_SourceInfo) ||
        policy->structSize != sizeof(NAE_OutputPolicy) ||
        out->structSize != sizeof(NAE_ResolvedSignalPath)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->version = NAE_RESOLVED_SIGNAL_PATH_VERSION;
    out->capsGeneration = caps->capsGeneration;
    out->backend = caps->backend;
    out->shareMode = caps->shareMode;
    std::snprintf(out->deviceId, sizeof(out->deviceId), "%s", caps->id);
    std::snprintf(out->deviceName, sizeof(out->deviceName), "%s", caps->name);

    out->sourceRate = source->sampleRate;
    out->sourceChannels = source->channels;
    out->sourceChannelMask = source->channelMask;
    out->sourceFormat = source->sampleFormat;
    out->sourceValidBits = source->validBits;
    out->sourceContainerBits = source->containerBits;
    out->sourceIsDsd = source->isDsd ? 1 : 0;
    out->sourceDsdRate = source->dsdRate;
    out->dsdModeRequested = policy->dsdMode;
    out->dsdTransportRequested = policy->dsdTransport;
    out->dsdRatePolicy = policy->dsdRatePolicy;

    // A source layout of 0 is not a usable routing input. Derive the common
    // layouts rather than inventing a mask.
    uint32_t sourceMask = source->channelMask;
    if (sourceMask == 0) {
        if (source->channels == 1) sourceMask = NAE_LAYOUT_MONO;
        else if (source->channels == 2) sourceMask = NAE_LAYOUT_STEREO;
        else if (source->channels == 6) sourceMask = NAE_LAYOUT_5_1;
        else if (source->channels == 8) sourceMask = NAE_LAYOUT_7_1;
    }
    out->sourceChannelMask = sourceMask;
    if (sourceMask == 0 || source->channels == 0) {
        out->status = NAE_RESOLVE_UNSUPPORTED_CHANNELS;
        std::snprintf(out->failure, sizeof(out->failure), "source has no usable channel layout");
        addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_ROUTING, -1, -1, 0,
                "The source channel layout is unknown; no routing decision is possible.");
        return NAE_ERR_INVALID_ARGUMENT;
    }

    uint32_t outputRate = 0;
    uint32_t outputChannels = source->channels;
    uint32_t outputMask = sourceMask;
    int32_t outputFormat = caps->nativeFormat ? caps->nativeFormat : NAE_SAMPLE_FORMAT_S24_IN_32;
    uint32_t validBits = caps->nativeValidBits ? caps->nativeValidBits : 24;
    uint32_t containerBits = caps->nativeContainerBits ? caps->nativeContainerBits : 32;
    bool isFloat = outputFormat == NAE_SAMPLE_FORMAT_F32 || outputFormat == NAE_SAMPLE_FORMAT_F64;
    bool dsdOutput = false;
    bool dsdCarrier = false;

    // ---- source-domain / DSD decision ------------------------------------
    int32_t dsdMode = policy->dsdMode;
    if (source->isDsd) {
        if (dsdMode == NAE_DSD_MODE_AUTO) {
            const bool native = (caps->probed & NAE_CAPS_PROBED_NATIVE_DSD) &&
                                caps->nativeDsdSupported &&
                                hasNativeDsd(*caps, source->dsdRate);
            const uint32_t carrier = source->dsdRate / 16;
            const bool dop = (caps->probed & NAE_CAPS_PROBED_DOP) && caps->dopSupported &&
                             hasDopCarrier(*caps, carrier);
            if (native) {
                dsdMode = NAE_DSD_MODE_DSD;
                out->dsdTransportResolved = NAE_DSD_TRANSPORT_NATIVE;
                out->dsdReason = NAE_RESOLVE_REASON_DSD_NATIVE;
            } else if (dop) {
                dsdMode = NAE_DSD_MODE_DSD;
                out->dsdTransportResolved = NAE_DSD_TRANSPORT_DOP;
                out->dsdReason = NAE_RESOLVE_REASON_DSD_DOP;
            } else if (policy->allowPcmFallback) {
                dsdMode = NAE_DSD_MODE_PCM;
                out->dsdReason = NAE_RESOLVE_REASON_DSD_PCM_FALLBACK;
                addDiag(out, NAE_DIAG_FALLBACK, NAE_DIAG_DSD_PCM_FALLBACK,
                        NAE_AUTO_FIELD_DSD_MODE, NAE_DSD_MODE_AUTO, NAE_DSD_MODE_PCM,
                        0, "DSD transport is unavailable; Auto fell back to PCM.");
            } else {
                out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure),
                              "no DoP carrier or native DSD path and PCM fallback is forbidden");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_MODE,
                        NAE_DSD_MODE_AUTO, -1, 0,
                        "No DSD transport is available and PCM fallback is disabled.");
                return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
            }
        }

        if (dsdMode == NAE_DSD_MODE_PCM_TO_DSD) {
            out->status = NAE_RESOLVE_INVALID_POLICY;
            std::snprintf(out->failure, sizeof(out->failure),
                          "PCM-to-DSD is not valid for a DSD source");
            addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_MODE,
                    NAE_DSD_MODE_PCM_TO_DSD, -1, 0,
                    "A DSD source cannot be converted with the PCM-to-DSD modulator.");
            return NAE_ERR_INVALID_ARGUMENT;
        }

        if (dsdMode == NAE_DSD_MODE_PCM) {
            out->dsdPcmFallback = 1;
            out->dsdTransportResolved = NAE_DSD_TRANSPORT_NONE;
            if (out->dsdReason == NAE_RESOLVE_REASON_NONE)
                out->dsdReason = NAE_RESOLVE_REASON_DSD_PCM_FALLBACK;
            char text[NAE_MAX_DIAGNOSTIC_MESSAGE];
            std::snprintf(text, sizeof(text),
                          "DSD %u Hz is decoded to PCM for this output path.",
                          source->dsdRate);
            addDiag(out, NAE_DIAG_FALLBACK, NAE_DIAG_DSD_PCM_FALLBACK,
                    NAE_AUTO_FIELD_DSD_MODE, policy->dsdMode, NAE_DSD_MODE_PCM,
                    source->dsdRate, text);
            // Fall through to the PCM rate decision.
        } else {
            dsdOutput = true;
            if (out->dsdTransportResolved == NAE_DSD_TRANSPORT_NONE) {
                if (policy->dsdTransport == NAE_DSD_TRANSPORT_DOP) {
                    out->dsdTransportResolved = NAE_DSD_TRANSPORT_DOP;
                } else if (policy->dsdTransport == NAE_DSD_TRANSPORT_NATIVE) {
                    out->dsdTransportResolved = NAE_DSD_TRANSPORT_NATIVE;
                } else {
                    const bool native = (caps->probed & NAE_CAPS_PROBED_NATIVE_DSD) &&
                                        caps->nativeDsdSupported &&
                                        hasNativeDsd(*caps, source->dsdRate);
                    const uint32_t carrier = source->dsdRate / 16;
                    const bool dop = (caps->probed & NAE_CAPS_PROBED_DOP) &&
                                     caps->dopSupported && hasDopCarrier(*caps, carrier);
                    out->dsdTransportResolved = native ? NAE_DSD_TRANSPORT_NATIVE
                                                       : (dop ? NAE_DSD_TRANSPORT_DOP
                                                              : NAE_DSD_TRANSPORT_NONE);
                }
            }
            if (out->dsdTransportResolved == NAE_DSD_TRANSPORT_DOP) {
                const uint32_t carrier = source->dsdRate / 16;
                if (!(caps->probed & NAE_CAPS_PROBED_DOP) || !caps->dopSupported ||
                    !hasDopCarrier(*caps, carrier)) {
                    out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
                    std::snprintf(out->failure, sizeof(out->failure),
                                  "DoP carrier %u is unavailable", carrier);
                    addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_TRANSPORT,
                            NAE_DSD_TRANSPORT_DOP, -1, carrier,
                            "The requested DoP carrier is unavailable.");
                    return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
                }
            } else if (out->dsdTransportResolved == NAE_DSD_TRANSPORT_NATIVE) {
                if (!(caps->probed & NAE_CAPS_PROBED_NATIVE_DSD) || !caps->nativeDsdSupported ||
                    !hasNativeDsd(*caps, source->dsdRate)) {
                    out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
                    std::snprintf(out->failure, sizeof(out->failure),
                                  "native DSD rate %u is unavailable", source->dsdRate);
                    addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_TRANSPORT,
                            NAE_DSD_TRANSPORT_NATIVE, -1, source->dsdRate,
                            "The requested native DSD rate is unavailable.");
                    return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
                }
            }
            if (out->dsdTransportResolved == NAE_DSD_TRANSPORT_NONE) {
                out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure),
                              "requested DSD transport is unavailable");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_MODE,
                        policy->dsdMode, -1, 0,
                        "The requested DSD transport is unavailable.");
                return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
            }
            uint32_t dsdRate = source->dsdRate;
            if (policy->targetDsdRate) dsdRate = policy->targetDsdRate;
            if (dsdRate != source->dsdRate) {
                if (dsdRate > source->dsdRate && !policy->allowDownrate) {
                    out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
                    std::snprintf(out->failure, sizeof(out->failure),
                                  "DSD target rate is not available");
                    addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_RATE,
                            policy->dsdRatePolicy, -1, dsdRate,
                            "The requested DSD rate is not available.");
                    return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
                }
                if (dsdRate < source->dsdRate) {
                    out->dsdDownrated = 1;
                    addDiag(out, NAE_DIAG_FALLBACK, NAE_DIAG_DSD_DOWNRATE,
                            NAE_AUTO_FIELD_DSD_RATE, policy->dsdRatePolicy,
                            NAE_DSD_RATE_DOWNRATE, dsdRate,
                            "DSD rate was lowered to the nearest available rate.");
                }
            }
            out->dsdRateResolved = dsdRate;
            if (out->dsdTransportResolved == NAE_DSD_TRANSPORT_DOP) {
                out->dsdCarrierRate = dsdRate / 16;
                outputRate = out->dsdCarrierRate;
                outputFormat = NAE_SAMPLE_FORMAT_S24;
                validBits = 24;
                containerBits = 24;
            } else {
                out->dsdCarrierRate = 0;
                outputRate = dsdRate;
                outputFormat = NAE_SAMPLE_FORMAT_DSD_U8;
                validBits = 1;
                containerBits = 8;
            }
            outputChannels = source->channels;
            outputMask = sourceMask;
            isFloat = false;
            dsdCarrier = out->dsdTransportResolved == NAE_DSD_TRANSPORT_DOP;
        }
    } else if (dsdMode == NAE_DSD_MODE_DSD) {
        out->status = NAE_RESOLVE_INVALID_POLICY;
        std::snprintf(out->failure, sizeof(out->failure),
                      "DSD mode is not valid for a PCM source");
        addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_MODE,
                NAE_DSD_MODE_DSD, -1, 0,
                "A PCM source cannot use DSD mode without the PCM-to-DSD modulator.");
        return NAE_ERR_INVALID_ARGUMENT;
    } else if (dsdMode == NAE_DSD_MODE_PCM_TO_DSD) {
        const uint32_t target = policy->targetDsdRate ? policy->targetDsdRate : 2822400u;
        out->dsdModeResolved = NAE_DSD_MODE_PCM_TO_DSD;
        out->dsdRateResolved = target;
        const uint32_t carrier = target / 16;
        const bool dop = (caps->probed & NAE_CAPS_PROBED_DOP) && caps->dopSupported &&
                         hasDopCarrier(*caps, carrier);
        const bool native = (caps->probed & NAE_CAPS_PROBED_NATIVE_DSD) &&
                            caps->nativeDsdSupported && hasNativeDsd(*caps, target);
        if (policy->dsdTransport == NAE_DSD_TRANSPORT_DOP) {
            if (!dop) {
                out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure), "no DoP carrier for PCM-to-DSD");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_TRANSPORT,
                        NAE_DSD_TRANSPORT_DOP, -1, carrier,
                        "PCM-to-DSD requested DoP, but the carrier is unavailable.");
                return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
            }
            out->dsdTransportResolved = NAE_DSD_TRANSPORT_DOP;
        } else if (policy->dsdTransport == NAE_DSD_TRANSPORT_NATIVE) {
            if (!native) {
                out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure), "native DSD target unavailable");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_TRANSPORT,
                        NAE_DSD_TRANSPORT_NATIVE, -1, target,
                        "PCM-to-DSD requested native DSD, but the target is unavailable.");
                return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
            }
            out->dsdTransportResolved = NAE_DSD_TRANSPORT_NATIVE;
        } else if (native) {
            out->dsdTransportResolved = NAE_DSD_TRANSPORT_NATIVE;
        } else if (dop) {
            out->dsdTransportResolved = NAE_DSD_TRANSPORT_DOP;
        } else {
            out->status = NAE_RESOLVE_DSD_UNAVAILABLE;
            std::snprintf(out->failure, sizeof(out->failure), "no DSD transport for PCM-to-DSD");
            addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_DSD_TRANSPORT,
                    NAE_DSD_TRANSPORT_AUTO, -1, target,
                    "PCM-to-DSD has no native or DoP output path.");
            return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
        }
        if (out->dsdTransportResolved == NAE_DSD_TRANSPORT_DOP) {
            out->dsdCarrierRate = carrier;
            outputRate = carrier;
            outputFormat = NAE_SAMPLE_FORMAT_S24;
            validBits = 24;
            containerBits = 24;
        } else {
            outputRate = target;
            outputFormat = NAE_SAMPLE_FORMAT_DSD_U8;
            validBits = 1;
            containerBits = 8;
        }
        out->dsdModeRequested = dsdMode;
        dsdOutput = true;
        dsdCarrier = out->dsdTransportResolved == NAE_DSD_TRANSPORT_DOP;
        addDiag(out, NAE_DIAG_INFO, NAE_DIAG_AUTO_CHOICE, NAE_AUTO_FIELD_DSD_MODE,
                NAE_DSD_MODE_PCM_TO_DSD, NAE_DSD_MODE_PCM_TO_DSD, target,
                "PCM is modulated to DSD; this path is not bit-perfect.");
    } else {
        dsdMode = NAE_DSD_MODE_PCM;
        out->dsdModeResolved = NAE_DSD_MODE_PCM;
        out->dsdTransportResolved = NAE_DSD_TRANSPORT_NONE;
        if (policy->dsdMode == NAE_DSD_MODE_AUTO) {
            addAuto(out, NAE_AUTO_FIELD_DSD_MODE, NAE_DSD_MODE_PCM,
                    NAE_RESOLVE_REASON_NONE, 0,
                    "Auto kept PCM because the source is PCM.");
        }
    }

    // ---- PCM rate / resampler / dither ------------------------------------
    if (!dsdOutput) {
        int32_t rateReason = NAE_RESOLVE_REASON_NONE;
        bool coerced = false;
        if (!choosePcmRate(*caps, *policy, source->sampleRate, &outputRate, &rateReason,
                           &coerced, out)) {
            return NAE_ERR_INVALID_ARGUMENT;
        }
        out->requestedRate = policy->ratePolicy == NAE_RATE_FIXED ? policy->fixedRate
                                                                  : outputRate;
        out->ratePolicy = policy->ratePolicy;
        out->rateReason = rateReason;
        out->coerced = coerced ? 1 : 0;
        if (coerced) {
            addDiag(out, NAE_DIAG_FALLBACK, NAE_DIAG_RATE_COERCED, NAE_AUTO_FIELD_RATE,
                    policy->ratePolicy, NAE_RATE_AUTO, outputRate,
                    "Auto chose the nearest supported output rate; the source rate is not available.");
        }
        if (policy->ratePolicy == NAE_RATE_AUTO) {
            addAuto(out, NAE_AUTO_FIELD_RATE, NAE_RATE_AUTO, rateReason, outputRate,
                    "Auto selected " + std::to_string(outputRate) + " Hz.");
        }
        const bool needResample = outputRate != source->sampleRate;
        out->resampling = needResample ? 1 : 0;
        out->resamplerAlgorithm = policy->resampleAlgorithm;
        out->resamplerQuality = policy->resampleQuality;
        out->resampleRatio = static_cast<double>(outputRate) /
                             static_cast<double>(source->sampleRate);
        out->resampleReason = needResample ? NAE_RESOLVE_REASON_RESAMPLE_REQUIRED
                                           : NAE_RESOLVE_REASON_NO_RESAMPLE;
        if (needResample) {
            if (out->resamplerAlgorithm == NAE_RESAMPLE_ALGORITHM_AUTO)
                out->resamplerAlgorithm = NAE_RESAMPLE_ALGORITHM_SOXR;
            if (out->resamplerQuality == NAE_RESAMPLE_QUALITY_AUTO)
                out->resamplerQuality = NAE_RESAMPLE_QUALITY_HIGH;
            addDiag(out, NAE_DIAG_INFO, NAE_DIAG_RESAMPLER, NAE_AUTO_FIELD_RESAMPLER,
                    policy->resampleAlgorithm, out->resamplerAlgorithm, outputRate,
                    "Resampling is required; Auto selected soxr.");
        } else {
            addDiag(out, NAE_DIAG_INFO, NAE_DIAG_RESAMPLER, NAE_AUTO_FIELD_RESAMPLER,
                    policy->resampleAlgorithm, NAE_RESAMPLE_ALGORITHM_SOXR, 0,
                    "No resampling is needed.");
        }

        if (isFloat) {
            out->ditherMode = NAE_DITHER_OFF;
        } else if (validBits >= 24) {
            out->ditherMode = NAE_DITHER_OFF;
        } else if (policy->ditherMode == NAE_DITHER_AUTO) {
            out->ditherMode = NAE_DITHER_TPDF;
        } else {
            out->ditherMode = policy->ditherMode;
        }
        out->ditherBits = validBits;
        out->ditherActive = out->ditherMode != NAE_DITHER_OFF && out->ditherMode != NAE_DITHER_AUTO;
        out->ditherReason = out->ditherActive ? NAE_RESOLVE_REASON_DITHER_REQUIRED
                                              : NAE_RESOLVE_REASON_NO_DITHER;
        if (out->ditherActive) {
            addDiag(out, NAE_DIAG_INFO, NAE_DIAG_DITHER, NAE_AUTO_FIELD_DITHER,
                    policy->ditherMode, out->ditherMode, validBits,
                    "Dither is active for the target bit depth.");
        }
    }

    // ---- routing ----------------------------------------------------------
    uint32_t targetMask = policy->targetChannelMask;
    if (targetMask == 0) {
        targetMask = sourceMask;
        if (policy->routingMode == NAE_ROUTING_DOWNMIX) {
            targetMask = defaultTargetForDownmix(policy->downmixPreset);
        } else if (policy->routingMode == NAE_ROUTING_UPMIX) {
            targetMask = defaultTargetForPreset(policy->upmixPreset);
        }
    }
    if (targetMask == 0) {
        out->status = NAE_RESOLVE_UNSUPPORTED_CHANNELS;
        std::snprintf(out->failure, sizeof(out->failure), "no target channel layout");
        addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_ROUTING, -1, -1, 0,
                "No target channel layout was chosen.");
        return NAE_ERR_INVALID_ARGUMENT;
    }
    out->channelMask = targetMask;
    out->channels = bitCount(targetMask);

    const uint32_t sourceCount = bitCount(sourceMask);
    const uint32_t targetCount = bitCount(targetMask);
    out->routingMode = policy->routingMode;
    out->upmixPreset = policy->upmixPreset;
    out->downmixPreset = policy->downmixPreset;
    out->matrixId = policy->matrixId;

    if (sourceMask != targetMask) {
        out->channelConversion = 1;
        int32_t mode = policy->routingMode;
        int32_t preset = 0;
        if (mode == NAE_ROUTING_AUTO) {
            if (targetCount < sourceCount) {
                mode = NAE_ROUTING_DOWNMIX;
                preset = NAE_DOWNMIX_ITU_R_775;
                addDiag(out, NAE_DIAG_INFO, NAE_DIAG_ROUTING_DOWNMIX,
                        NAE_AUTO_FIELD_ROUTING, NAE_ROUTING_AUTO, mode, targetMask,
                        "Auto downmixes because the target has fewer channels.");
            } else if (targetCount > sourceCount) {
                out->status = NAE_RESOLVE_ROUTING_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure),
                              "Auto never upmixes; choose an upmix preset or matrix");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_ROUTING,
                        NAE_ROUTING_AUTO, -1, targetMask,
                        "Auto never upmixes; an explicit upmix preset or matrix is required.");
                return NAE_ERR_INVALID_ARGUMENT;
            } else {
                mode = NAE_ROUTING_MATRIX;
                out->status = NAE_RESOLVE_ROUTING_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure),
                              "channel masks differ but no routing mode was selected");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_ROUTING,
                        NAE_ROUTING_AUTO, -1, targetMask,
                        "The channel masks differ; choose a downmix, upmix or matrix.");
                return NAE_ERR_INVALID_ARGUMENT;
            }
        } else {
            preset = mode == NAE_ROUTING_UPMIX ? policy->upmixPreset
                                               : policy->downmixPreset;
        }
        if (mode == NAE_ROUTING_MATRIX) {
            if (policy->matrixId == 0) {
                out->status = NAE_RESOLVE_ROUTING_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure), "MATRIX requires matrixId");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_ROUTING,
                        NAE_ROUTING_MATRIX, -1, 0,
                        "The matrix routing mode requires a registered matrixId.");
                return NAE_ERR_INVALID_ARGUMENT;
            }
            std::lock_guard<std::mutex> lock(g_matrixMutex);
            auto it = g_matrices.find(policy->matrixId);
            if (it == g_matrices.end() || it->second.sourceChannels != sourceCount ||
                it->second.targetChannels != targetCount ||
                it->second.sourceMask != sourceMask || it->second.targetMask != targetMask) {
                out->status = NAE_RESOLVE_ROUTING_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure), "routing matrix does not match");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_ROUTING,
                        NAE_ROUTING_MATRIX, -1, policy->matrixId,
                        "The registered matrix does not match the source/target layout.");
                return NAE_ERR_INVALID_ARGUMENT;
            }
            out->phaseShiftActive = it->second.phaseMode != NAE_PHASE_NONE ? 1 : 0;
            out->phaseFilterOrder = it->second.phaseOrder;
            out->phaseFilterTaps = it->second.phaseTaps;
            out->phaseLatencyFrames = it->second.phaseTaps ? (it->second.phaseTaps - 1) / 2 : 0;
        } else {
            NAE_RoutingMatrix matrix{};
            const int32_t rc = NAE_BuildRoutingMatrix(
                mode, preset, sourceMask, targetMask, &matrix);
            if (rc != NAE_OK) {
                out->status = NAE_RESOLVE_ROUTING_UNAVAILABLE;
                std::snprintf(out->failure, sizeof(out->failure), "routing preset cannot be built");
                addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_ROUTING,
                        mode, preset, targetMask,
                        "The routing preset cannot be built for this source/target layout.");
                return rc;
            }
            out->phaseShiftActive = matrix.phaseMode != NAE_PHASE_NONE ? 1 : 0;
            out->phaseFilterOrder = matrix.phaseOrder;
            out->phaseFilterTaps = matrix.phaseTaps;
            out->phaseLatencyFrames = matrix.phaseTaps ? (matrix.phaseTaps - 1) / 2 : 0;
        }
        out->routingReason = preset == NAE_DOWNMIX_LTRT
                                 ? NAE_RESOLVE_REASON_ROUTING_DOWNMIX
                                 : (mode == NAE_ROUTING_UPMIX
                                        ? NAE_RESOLVE_REASON_ROUTING_UPMIX
                                        : NAE_RESOLVE_REASON_ROUTING_DOWNMIX);
        out->channelConversion = 1;
    } else {
        out->routingMode = NAE_ROUTING_PASS_THROUGH;
        out->routingReason = NAE_RESOLVE_REASON_ROUTING_PASS_THROUGH;
        out->channelConversion = 0;
    }
    if (out->phaseShiftActive) {
        addDiag(out, NAE_DIAG_INFO, NAE_DIAG_PHASE_SHIFT, NAE_AUTO_FIELD_ROUTING,
                policy->downmixPreset, NAE_DSD_MODE_AUTO, out->phaseFilterTaps,
                "Lt/Rt uses a 90-degree Hilbert phase stage.");
    }

    // ---- bass management --------------------------------------------------
    const bool targetHasLfe = (targetMask & NAE_CHANNEL_LFE) != 0;
    const bool sourceHasLfe = (sourceMask & NAE_CHANNEL_LFE) != 0;
    int32_t bass = policy->bassManagement;
    if (bass == NAE_BASS_AUTO) bass = (targetHasLfe && !sourceHasLfe) ? NAE_BASS_ON : NAE_BASS_OFF;
    out->bassManagement = bass;
    if (bass == NAE_BASS_ON) {
        out->lfeMode = policy->lfeMode == NAE_LFE_AUTO ? NAE_LFE_LOW_PASS_SUM : policy->lfeMode;
        out->lfeCrossoverHz = policy->lfeCrossoverHz > 0 ? policy->lfeCrossoverHz : 80.0;
        out->lfeGainDb = policy->lfeGainDb;
        out->bypassedStages &= ~NAE_STAGE_BASS_MANAGEMENT;
        out->activeStages |= NAE_STAGE_BASS_MANAGEMENT;
        addDiag(out, NAE_DIAG_INFO, NAE_DIAG_BASS_MANAGEMENT, NAE_AUTO_FIELD_ROUTING,
                policy->bassManagement, bass, static_cast<uint32_t>(out->lfeCrossoverHz),
                "Bass management derives the LFE from the routed sum.");
    }

    // ---- buffer -----------------------------------------------------------
    out->bufferMode = policy->bufferMode;
    if (policy->bufferMode == NAE_BUFFER_FIXED) {
        if (!(caps->probed & NAE_CAPS_PROBED_BUFFER) || policy->bufferMs == 0 ||
            policy->bufferMs * outputRate / 1000 < caps->minBufferFrames ||
            (caps->maxBufferFrames && policy->bufferMs * outputRate / 1000 > caps->maxBufferFrames)) {
            out->status = NAE_RESOLVE_BUFFER_UNAVAILABLE;
            std::snprintf(out->failure, sizeof(out->failure), "fixed buffer is outside device limits");
            addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_FAILURE, NAE_AUTO_FIELD_BUFFER,
                    policy->bufferMode, -1, policy->bufferMs,
                    "The fixed buffer is outside the device's supported range.");
            return NAE_ERR_INVALID_ARGUMENT;
        }
        out->bufferFrames = static_cast<uint32_t>(policy->bufferMs) * outputRate / 1000;
        out->bufferReason = NAE_RESOLVE_REASON_BUFFER_FIXED;
    } else if (policy->bufferMode == NAE_BUFFER_LOW_LATENCY) {
        if (!(caps->probed & NAE_CAPS_PROBED_BUFFER)) {
            out->status = NAE_RESOLVE_UNPROBED;
            std::snprintf(out->failure, sizeof(out->failure), "buffer limits were not probed");
            addDiag(out, NAE_DIAG_ERROR, NAE_DIAG_UNPROBED, NAE_AUTO_FIELD_BUFFER,
                    policy->bufferMode, -1, 0,
                    "Cannot choose a low-latency buffer: the device was not probed.");
            return NAE_ERR_INVALID_ARGUMENT;
        }
        out->bufferFrames = caps->minBufferFrames;
        out->bufferReason = NAE_RESOLVE_REASON_BUFFER_LOW_LATENCY;
    } else {
        out->bufferFrames = caps->defaultBufferFrames;
        out->bufferReason = NAE_RESOLVE_REASON_BUFFER_BACKEND_DEFAULT;
    }
    out->periodFrames = caps->minPeriodFrames;
    out->eventDriven = caps->eventDriven;
    out->bufferLatencyMs = caps->reportedLatencyMs;
    out->driverLatencyMs = caps->reportedLatencyMs;
    out->totalLatencyMs = caps->reportedLatencyMs;
    out->bufferMode = policy->bufferMode;
    if (policy->bufferMode == NAE_BUFFER_AUTO) {
        addAuto(out, NAE_AUTO_FIELD_BUFFER, NAE_BUFFER_AUTO,
                NAE_RESOLVE_REASON_BUFFER_BACKEND_DEFAULT, out->bufferFrames,
                "Auto uses the backend's default buffer.");
    }

    // ---- channel / clip ---------------------------------------------------
    out->channelMode = policy->channelMode;
    out->balance = policy->balance;
    out->polarityMask = policy->polarityMask;
    for (uint32_t i = 0; i < NAE_MAX_CHANNELS; ++i) {
        out->delayFrames[i] = policy->delayFrames[i];
        out->trimDb[i] = policy->trimDb[i];
    }
    out->channelReason = NAE_RESOLVE_REASON_CHANNEL_AUTO;
    // Auto clip guard is decided after the stage bits are known: an exact
    // path stays exact, a processed path gets the limiter.
    out->clipGuardMode = policy->clipGuardMode == NAE_CLIP_GUARD_AUTO
                             ? NAE_CLIP_GUARD_OFF
                             : policy->clipGuardMode;
    out->clipCeilingDbTp = policy->clipCeilingDbTp;
    out->clipLookaheadMs = policy->clipLookaheadMs;
    out->clipReason = NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_OFF;

    out->replayGainMode = NAE_RG_OFF;
    out->replayGainAppliedDb = 0.0;
    out->replayGainReason = NAE_RESOLVE_REASON_NONE;

    // ---- bit-perfect and stage bits --------------------------------------
    out->bitPerfect = 1;
    out->perfectReason = NAE_REASON_BIT_PERFECT;
    if (out->resampling) {
        out->bitPerfect = 0;
        out->perfectReason = out->coerced ? NAE_REASON_RESAMPLED_TO_SUPPORTED_RATE
                                         : NAE_REASON_RESAMPLED_TO_DEVICE_RATE;
        out->activeStages |= NAE_STAGE_RESAMPLE;
    }
    if (out->ditherActive) {
        out->bitPerfect = 0;
        if (out->perfectReason == NAE_REASON_BIT_PERFECT) out->perfectReason = NAE_REASON_DITHERED;
        out->activeStages |= NAE_STAGE_DITHER;
    }
    if (out->channelConversion) {
        out->bitPerfect = 0;
        if (out->perfectReason == NAE_REASON_BIT_PERFECT) out->perfectReason = NAE_REASON_CHANNEL_CONVERTED;
        out->activeStages |= NAE_STAGE_ROUTING;
    }
    if (out->phaseShiftActive) {
        out->bitPerfect = 0;
        if (out->perfectReason == NAE_REASON_BIT_PERFECT) out->perfectReason = NAE_REASON_PHASE;
        out->activeStages |= NAE_STAGE_PHASE_SHIFT;
    }
    if (out->bassManagement == NAE_BASS_ON) {
        out->bitPerfect = 0;
        if (out->perfectReason == NAE_REASON_BIT_PERFECT) out->perfectReason = NAE_REASON_ROUTING;
    }
    if (out->clipGuardActive) {
        out->bitPerfect = 0;
        if (out->perfectReason == NAE_REASON_BIT_PERFECT) out->perfectReason = NAE_REASON_CLIP_GUARD;
        out->activeStages |= NAE_STAGE_CLIP_GUARD;
    }
    if (dsdOutput) {
        out->activeStages |= NAE_STAGE_DSD_TRANSPORT;
        if (out->dsdModeResolved == NAE_DSD_MODE_PCM_TO_DSD) {
            out->bitPerfect = 0;
            out->perfectReason = NAE_REASON_PCM_TO_DSD;
            out->activeStages |= NAE_STAGE_DSD_MODULATOR;
        }
    }
    if (out->dsdPcmFallback || out->dsdDownrated || out->dsdExtracted) {
        out->bitPerfect = 0;
        if (out->perfectReason == NAE_REASON_BIT_PERFECT)
            out->perfectReason = NAE_REASON_DSD_TRANSPORT;
    }
    if (caps->shareMode == NAE_SHARE_MODE_SHARED) {
        out->bitPerfect = 0;
        if (out->perfectReason == NAE_REASON_BIT_PERFECT) out->perfectReason = NAE_REASON_SHARED_MODE;
    }
    if (policy->clipGuardMode == NAE_CLIP_GUARD_AUTO && !dsdOutput && !out->bitPerfect) {
        out->clipGuardMode = NAE_CLIP_GUARD_LIMITER;
        out->clipGuardActive = 1;
        out->clipReason = NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_ON;
        out->activeStages |= NAE_STAGE_CLIP_GUARD;
        addAuto(out, NAE_AUTO_FIELD_CLIP_GUARD, NAE_CLIP_GUARD_AUTO,
                NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_ON, 0,
                "Auto enables the clip guard for this processed path.");
    } else if (policy->clipGuardMode == NAE_CLIP_GUARD_AUTO) {
        addAuto(out, NAE_AUTO_FIELD_CLIP_GUARD, NAE_CLIP_GUARD_AUTO,
                NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_OFF, 0,
                "Auto leaves the clip guard off for this exact path.");
    }
    if (out->bitPerfect) {
        addDiag(out, NAE_DIAG_INFO, NAE_DIAG_BIT_PERFECT, NAE_AUTO_FIELD_BIT_PERFECT,
                -1, -1, 0, "The path is bit-perfect.");
    } else {
        char text[NAE_MAX_DIAGNOSTIC_MESSAGE];
        std::snprintf(text, sizeof(text), "The path is not bit-perfect (reason %d).",
                      out->perfectReason);
        addDiag(out, NAE_DIAG_INFO, NAE_DIAG_BIT_PERFECT, NAE_AUTO_FIELD_BIT_PERFECT,
                -1, out->perfectReason, 0, text);
    }

    out->sampleRate = outputRate;
    out->sampleFormat = outputFormat;
    out->validBits = validBits;
    out->containerBits = containerBits;
    out->isFloat = isFloat ? 1 : 0;
    out->dsdModeResolved = dsdMode;
    out->resolved = 1;
    out->status = NAE_RESOLVE_OK;
    return NAE_OK;
}
