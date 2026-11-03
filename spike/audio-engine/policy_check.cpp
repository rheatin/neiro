#include "nae.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;

void check(bool ok, const char* label, const char* detail = "") {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail);
}

NAE_DeviceCaps caps() {
    NAE_DeviceCaps c{};
    c.structSize = sizeof(c);
    c.version = NAE_DEVICE_CAPS_VERSION;
    c.backend = NAE_BACKEND_WASAPI;
    c.shareMode = NAE_SHARE_MODE_EXCLUSIVE;
    c.probed = NAE_CAPS_PROBED_NATIVE | NAE_CAPS_PROBED_RATES |
               NAE_CAPS_PROBED_FORMATS | NAE_CAPS_PROBED_LAYOUTS |
               NAE_CAPS_PROBED_BUFFER | NAE_CAPS_PROBED_DOP |
               NAE_CAPS_PROBED_NATIVE_DSD | NAE_CAPS_PROBED_SHARE;
    std::snprintf(c.id, sizeof(c.id), "test-device");
    std::snprintf(c.name, sizeof(c.name), "Test endpoint");
    c.nativeFormat = NAE_SAMPLE_FORMAT_S24_IN_32;
    c.nativeValidBits = 24;
    c.nativeContainerBits = 32;
    c.nativeRate = 192000;
    c.nativeChannels = 2;
    c.nativeChannelMask = NAE_LAYOUT_STEREO;
    c.maxChannels = 8;
    c.rateCount = 4;
    c.rates[0] = 44100;
    c.rates[1] = 48000;
    c.rates[2] = 96000;
    c.rates[3] = 192000;
    c.formatCount = 1;
    c.formats[0] = {NAE_SAMPLE_FORMAT_S24_IN_32, 24, 32};
    c.layoutCount = 4;
    c.layouts[0] = NAE_LAYOUT_MONO;
    c.layouts[1] = NAE_LAYOUT_STEREO;
    c.layouts[2] = NAE_LAYOUT_5_1;
    c.layouts[3] = NAE_LAYOUT_7_1;
    c.minBufferFrames = 128;
    c.maxBufferFrames = 9600;
    c.defaultBufferFrames = 2048;
    c.minPeriodFrames = 128;
    c.maxPeriodFrames = 1920;
    c.eventDriven = 1;
    c.reportedLatencyMs = 10.0;
    c.dopSupported = 1;
    c.dopCarrierCount = 2;
    c.dopCarriers[0] = 176400;
    c.dopCarriers[1] = 352800;
    c.nativeDsdSupported = 0;
    c.exclusiveSupported = 1;
    c.sharedSupported = 1;
    c.supportedShareModes = (1u << 1) | (1u << 2);
    return c;
}

NAE_SourceInfo pcmSource(uint32_t rate, uint32_t channels, uint32_t mask) {
    NAE_SourceInfo s{};
    s.structSize = sizeof(s);
    s.sampleRate = rate;
    s.channels = channels;
    s.channelMask = mask;
    s.sampleFormat = NAE_SAMPLE_FORMAT_F32;
    s.validBits = 32;
    s.containerBits = 32;
    s.isDsd = 0;
    return s;
}

NAE_SourceInfo dsdSource() {
    NAE_SourceInfo s{};
    s.structSize = sizeof(s);
    s.channels = 2;
    s.channelMask = NAE_LAYOUT_STEREO;
    s.sampleFormat = NAE_SAMPLE_FORMAT_DSD_U8;
    s.validBits = 1;
    s.containerBits = 8;
    s.isDsd = 1;
    s.dsdRate = 2822400;
    return s;
}

NAE_ResolvedSignalPath resolved() {
    NAE_ResolvedSignalPath r{};
    r.structSize = sizeof(r);
    return r;
}
}  // namespace

int main() {
    const NAE_DeviceCaps c = caps();
    NAE_OutputPolicy p{};
    NAE_OutputPolicyDefaults(&p);
    std::printf("output policy version %u, struct %zu\n", p.version, sizeof(p));

    // Native rate + no DSP => bit-perfect.
    {
        NAE_SourceInfo s = pcmSource(44100, 2, NAE_LAYOUT_STEREO);
        NAE_ResolvedSignalPath r = resolved();
        const int32_t rc = NAE_ResolveSignalPath(&c, &s, &p, &r);
        check(rc == NAE_OK, "44.1k resolves");
        check(r.sampleRate == 44100, "44.1k stays native", std::to_string(r.sampleRate).c_str());
        check(r.bitPerfect == 1, "44.1k is bit-perfect");
        check(r.diagnosticCount > 0, "auto diagnostics exist");
        check(r.dsdModeResolved == NAE_DSD_MODE_PCM, "PCM source stays PCM");
    }

    // Unsupported source rate => Auto nearest, coercision diagnostic.
    {
        NAE_SourceInfo s = pcmSource(88200, 2, NAE_LAYOUT_STEREO);
        NAE_ResolvedSignalPath r = resolved();
        const int32_t rc = NAE_ResolveSignalPath(&c, &s, &p, &r);
        check(rc == NAE_OK, "88.2k resolves");
        check(r.sampleRate == 192000, "88.2k coerced to device native 192k",
              std::to_string(r.sampleRate).c_str());
        check(r.coerced == 1, "coercion is reported");
        check(r.resampling == 1, "resampling is reported");
        check(r.bitPerfect == 0, "converted path is not bit-perfect");
        check(r.diagnosticCount >= 2, "fallback diagnostics exist");
    }

    // DSD64 + DoP => bit-perfect DSD.
    {
        NAE_SourceInfo s = dsdSource();
        NAE_ResolvedSignalPath r = resolved();
        const int32_t rc = NAE_ResolveSignalPath(&c, &s, &p, &r);
        check(rc == NAE_OK, "DSD64 DoP resolves");
        check(r.dsdModeResolved == NAE_DSD_MODE_DSD, "DSD stays DSD");
        check(r.dsdTransportResolved == NAE_DSD_TRANSPORT_DOP, "DSD chooses DoP");
        check(r.dsdCarrierRate == 176400, "DSD64 carrier is 176400",
              std::to_string(r.dsdCarrierRate).c_str());
        check(r.sampleRate == 176400, "DoP output rate");
        check(r.bitPerfect == 1, "exact DoP is bit-perfect");
    }

    // LtRt uses the complex routing matrix and phase stage.
    {
        NAE_SourceInfo s = pcmSource(48000, 6, NAE_LAYOUT_5_1);
        p.routingMode = NAE_ROUTING_DOWNMIX;
        p.downmixPreset = NAE_DOWNMIX_LTRT;
        p.targetChannelMask = NAE_LAYOUT_STEREO;
        p.ratePolicy = NAE_RATE_NATIVE;
        NAE_ResolvedSignalPath r = resolved();
        const int32_t rc = NAE_ResolveSignalPath(&c, &s, &p, &r);
        check(rc == NAE_OK, "LtRt resolves");
        check(r.channels == 2, "LtRt target is stereo");
        check(r.phaseShiftActive == 1, "LtRt phase stage is active");
        check(r.bitPerfect == 0, "LtRt is not bit-perfect");
        check(r.routingReason == NAE_RESOLVE_REASON_ROUTING_DOWNMIX, "LtRt routing reason");
    }

    // The builder itself: Ls/Rs carry quadrature, L/R/C are real.
    {
        NAE_RoutingMatrix m{};
        const int32_t rc = NAE_BuildRoutingMatrix(NAE_ROUTING_DOWNMIX, NAE_DOWNMIX_LTRT,
                                                   NAE_LAYOUT_5_1, NAE_LAYOUT_STEREO, &m);
        check(rc == NAE_OK, "LtRt matrix builds");
        check(m.phaseMode == NAE_PHASE_IIR_ALLPASS_90, "LtRt phase mode");
        check(m.phaseOrder == 6, "LtRt IIR order 6");
        const int lt = 0;  // FL row
        const int ls = 4;  // BL column in 5.1 bit order: FL FR FC LFE BL
        check(m.gain[lt][ls].quadrature > 0.7f, "Lt has +Ls quadrature",
              std::to_string(m.gain[lt][ls].quadrature).c_str());
        check(m.gain[lt][0].real == 1.0f, "Lt has direct L");
    }

    // DSD without a carrier must fall back to PCM only when permitted.
    {
        NAE_DeviceCaps noDop = c;
        noDop.dopSupported = 0;
        noDop.dopCarrierCount = 0;
        noDop.probed &= ~NAE_CAPS_PROBED_DOP;
        noDop.nativeDsdSupported = 0;
        noDop.nativeDsdRateCount = 0;
        noDop.probed &= ~NAE_CAPS_PROBED_NATIVE_DSD;
        NAE_SourceInfo s = dsdSource();
        NAE_OutputPolicy fallback{};
        NAE_OutputPolicyDefaults(&fallback);
        NAE_ResolvedSignalPath r = resolved();
        const int32_t rc = NAE_ResolveSignalPath(&noDop, &s, &fallback, &r);
        check(rc == NAE_OK, "DSD fallback resolves when allowed");
        check(r.dsdPcmFallback == 1, "DSD fallback is marked");
        check(r.bitPerfect == 0, "DSD fallback is not bit-perfect");
        check(r.diagnosticCount > 0, "DSD fallback diagnostic exists");

        fallback.allowPcmFallback = 0;
        r = resolved();
        const int32_t denied = NAE_ResolveSignalPath(&noDop, &s, &fallback, &r);
        check(denied != NAE_OK, "DSD fallback denial fails");
        check(r.status == NAE_RESOLVE_DSD_UNAVAILABLE, "DSD denial status");
    }

    // Registered matrices round-trip and drive the Session policy path.
    {
        NAE_RoutingMatrix m{};
        NAE_BuildRoutingMatrix(NAE_ROUTING_DOWNMIX, NAE_DOWNMIX_LTRT,
                               NAE_LAYOUT_5_1, NAE_LAYOUT_STEREO, &m);
        uint32_t id = 0;
        check(NAE_RegisterRoutingMatrix(&m, &id) == NAE_OK, "routing matrix registers");
        NAE_RoutingMatrix got{};
        got.structSize = sizeof(got);
        check(NAE_GetRoutingMatrix(id, &got) == NAE_OK, "routing matrix reads back");
        check(got.phaseMode == NAE_PHASE_IIR_ALLPASS_90 && got.phaseOrder == 6,
              "routing matrix preserves phase fields");
        NAE_ReleaseRoutingMatrix(id);
        got = NAE_RoutingMatrix{};
        got.structSize = sizeof(got);
        check(NAE_GetRoutingMatrix(id, &got) != NAE_OK, "released matrix is gone");
    }

    // Fixed unsupported rate fails and explains.
    {
        NAE_SourceInfo s = pcmSource(44100, 2, NAE_LAYOUT_STEREO);
        p.ratePolicy = NAE_RATE_FIXED;
        p.fixedRate = 384000;
        NAE_ResolvedSignalPath r = resolved();
        const int32_t rc = NAE_ResolveSignalPath(&c, &s, &p, &r);
        check(rc != NAE_OK, "unsupported fixed rate fails");
        check(r.resolved == 0, "failure is not resolved");
        check(r.status == NAE_RESOLVE_UNSUPPORTED_RATE, "failure status");
        check(r.diagnosticCount > 0, "failure diagnostic exists");
    }

    std::printf("\n%s\n", failures == 0 ? "All policy checks passed."
                                        : "Some policy checks FAILED.");
    return failures == 0 ? 0 : 1;
}
