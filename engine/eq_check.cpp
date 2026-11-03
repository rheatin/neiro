// The equaliser changes the samples, so the check measures the spectrum.
//
// A constant 1 kHz tone is the clean instrument: a peaking band at 1 kHz with
// -12 dB must drop the measured bin to 10^(-12/20) = 0.2512 of its unity value.
// The cascade is then disabled and the same measurement must come back, which is
// what separates "the filter works" from "the filter is stuck on".

#include "nae.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

bool playAndMeasure(NAE_Handle* handle, double* peak, int32_t* bitPerfect, int32_t* reason,
                    double* rms = nullptr) {
    for (int i = 0; i < 25; ++i) NAE_PlaySine(handle, 20);

    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(info);
    std::vector<float> bins(4096, 0.0f);
    if (NAE_QuerySpectrum(handle, &info, bins.data(),
                          static_cast<int32_t>(bins.size())) != NAE_OK) {
        return false;
    }
    *peak = info.peak;
    if (rms) *rms = info.rms;

    NAE_SignalPath path{};
    path.structSize = sizeof(path);
    const int32_t rc = NAE_QuerySignalPath(handle, &path);
    *bitPerfect = rc == NAE_OK ? path.bitPerfect : 0;
    *reason = rc == NAE_OK ? path.reason : -1;
    return true;
}

// Peak is the wrong instrument for a linear-phase filter: its impulse response
// rings before the transient, and a ring holding a stream boundary reads a
// value that is not the steady-state gain. RMS over several queries, taking the
// best window, measures the level the filter settles at.
bool playAndMeasureSteady(NAE_Handle* handle, double* rms, int32_t* bitPerfect,
                          int32_t* reason) {
    double best = 0.0;
    bool ran = false;
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < 10; ++i) NAE_PlaySine(handle, 20);
        NAE_SpectrumInfo info{};
        info.struct_size = sizeof(info);
        std::vector<float> bins(4096, 0.0f);
        if (NAE_QuerySpectrum(handle, &info, bins.data(),
                              static_cast<int32_t>(bins.size())) != NAE_OK) {
            continue;
        }
        ran = true;
        if (info.rms > best) best = info.rms;
        NAE_SignalPath path{};
        path.structSize = sizeof(path);
        const int32_t rc = NAE_QuerySignalPath(handle, &path);
        *bitPerfect = rc == NAE_OK ? path.bitPerfect : 0;
        *reason = rc == NAE_OK ? path.reason : -1;
    }
    *rms = best;
    return ran;
}

}  // namespace

int main() {
    NAE_Handle* handle = nullptr;
    int32_t rate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    double latencyMs = 0.0;
    if (NAE_OpenExclusiveSine(nullptr, &handle, &rate, &channels, &bits, &valid, &isFloat,
                              &latencyMs) != NAE_OK ||
        !handle) {
        std::printf("open failed\n");
        return 1;
    }


    double unityPeak = 0.0, eqPeak = 0.0, restoredPeak = 0.0;
    int32_t bpUnity = 0, reasonUnity = -1, bpEq = 0, reasonEq = -1, bpRestored = 0,
            reasonRestored = -1;
    double unityRms = 0.0;
    check(playAndMeasure(handle, &unityPeak, &bpUnity, &reasonUnity, &unityRms),
          "unity measurement runs");
    check(unityPeak > 0.001, "unity tone has signal", std::to_string(unityPeak));

    NAE_EqParams params;
    NAE_EqDefaults(&params);
    check(params.structSize == sizeof(params) && params.enabled == 0 &&
              params.bandCount == 0,
          "defaults are disabled and empty");

    params.enabled = 1;
    params.preampDb = 0.0;
    params.bandCount = 1;
    params.bands[0].type = NAE_EQ_PEAKING;
    params.bands[0].freqHz = 1000.0;
    params.bands[0].gainDb = -12.0;
    params.bands[0].q = 1.0;
    params.bands[0].enabled = 1;

    check(NAE_SetEq(handle, &params) == NAE_OK, "EQ is accepted");
    check(playAndMeasure(handle, &eqPeak, &bpEq, &reasonEq), "EQ measurement runs");
    const double ratio = unityPeak > 0.0 ? eqPeak / unityPeak : 0.0;
    check(std::fabs(ratio - 0.2512) < 0.02, "-12 dB at the tone's bin",
          "ratio " + std::to_string(ratio));
    if (bpUnity) {
        check(bpEq == 0 && reasonEq == NAE_REASON_EQ, "EQ reports not bit-perfect",
              "bitPerfect=" + std::to_string(bpEq) + " reason=" +
                  std::to_string(reasonEq));
    }

    NAE_EqParams readback;
    NAE_EqDefaults(&readback);
    check(NAE_GetEq(handle, &readback) == NAE_OK && readback.bandCount == 1 &&
              readback.bands[0].gainDb == -12.0,
          "EQ readback matches what was set");

    params.enabled = 0;
    check(NAE_SetEq(handle, &params) == NAE_OK, "EQ can be disabled");
    check(playAndMeasure(handle, &restoredPeak, &bpRestored, &reasonRestored),
          "restored measurement runs");
    const double restored = unityPeak > 0.0 ? restoredPeak / unityPeak : 0.0;
    check(std::fabs(restored - 1.0) < 0.05, "disabling restores the level",
          "ratio " + std::to_string(restored));
    if (bpUnity) {
        check(bpRestored == 1 && reasonRestored == 0,
              "disabling restores bit-perfect",
              "bitPerfect=" + std::to_string(bpRestored) + " reason=" +
                  std::to_string(reasonRestored));
    }

    // The same band, linear phase. Same magnitude, built as a symmetric FIR
    // per channel instead of a per-sample biquad.
    params.enabled = 1;
    params.bands[0].freqHz = 1000.0;
    params.phase = NAE_EQ_PHASE_MINIMUM;
    check(NAE_SetEq(handle, &params) == NAE_OK, "minimum-phase EQ reaccepted");
    NAE_SignalPath minPath{};
    minPath.structSize = sizeof(minPath);
    check(NAE_QuerySignalPath(handle, &minPath) == NAE_OK, "minimum-phase path readable");
    params.phase = NAE_EQ_PHASE_LINEAR;
    check(NAE_SetEq(handle, &params) == NAE_OK, "linear-phase EQ is accepted");
    NAE_SignalPath linPath{};
    linPath.structSize = sizeof(linPath);
    check(NAE_QuerySignalPath(handle, &linPath) == NAE_OK, "linear-phase path readable");
    check(linPath.totalLatencyMs - minPath.totalLatencyMs > 5.0,
          "linear phase adds its FIR latency to the reported total",
          std::to_string(linPath.totalLatencyMs - minPath.totalLatencyMs) + " ms");
    double linearRms = 0.0;
    int32_t bpLinear = 0, reasonLinear = -1;
    check(playAndMeasureSteady(handle, &linearRms, &bpLinear, &reasonLinear),
          "linear-phase measurement runs");
    const double linearRatio = unityRms > 0.0 ? linearRms / unityRms : 0.0;
    check(linearRatio > 0.22 && linearRatio < 0.30,
          "linear phase gives about -12 dB at the tone's bin",
          "rms ratio " + std::to_string(linearRatio));
    check(bpLinear == 0 && reasonLinear == NAE_REASON_EQ,
          "linear phase reports not bit-perfect",
          "bitPerfect=" + std::to_string(bpLinear) + " reason=" +
              std::to_string(reasonLinear));

    NAE_EqParams readbackLinear;
    NAE_EqDefaults(&readbackLinear);
    check(NAE_GetEq(handle, &readbackLinear) == NAE_OK &&
              readbackLinear.phase == NAE_EQ_PHASE_LINEAR,
          "linear phase is reported back");

    // Per-channel targeting: a band on channel 0 only must still move the
    // measured (left) bin, and is reported back with its mask.
    params.bands[0].channelMask = 1u;
    check(NAE_SetEq(handle, &params) == NAE_OK, "per-channel band is accepted");
    double maskedRms = 0.0;
    int32_t bpMasked = 0, reasonMasked = -1;
    check(playAndMeasureSteady(handle, &maskedRms, &bpMasked, &reasonMasked),
          "per-channel measurement runs");
    const double maskedRatio = unityRms > 0.0 ? maskedRms / unityRms : 0.0;
    check(maskedRatio > 0.22 && maskedRatio < 0.30,
          "a left-only band changes the left channel",
          "rms ratio " + std::to_string(maskedRatio));
    NAE_EqParams readbackMask;
    NAE_EqDefaults(&readbackMask);
    check(NAE_GetEq(handle, &readbackMask) == NAE_OK &&
              readbackMask.bands[0].channelMask == 1u,
          "channel mask is reported back");

    // The same mask on the minimum-phase path. Both paths read the mask from
    // the same field, so the left channel must move here too.
    params.phase = NAE_EQ_PHASE_MINIMUM;
    check(NAE_SetEq(handle, &params) == NAE_OK, "minimum-phase masked band is accepted");
    double maskedMinRms = 0.0;
    int32_t bpMaskedMin = 0, reasonMaskedMin = -1;
    check(playAndMeasureSteady(handle, &maskedMinRms, &bpMaskedMin, &reasonMaskedMin),
          "minimum-phase masked measurement runs");
    const double maskedMinRatio = unityRms > 0.0 ? maskedMinRms / unityRms : 0.0;
    check(std::fabs(maskedMinRatio - 0.2512) < 0.03,
          "a left-only band works in minimum phase too",
          "rms ratio " + std::to_string(maskedMinRatio));

    params.bands[0].channelMask = 0;
    params.enabled = 1;
    params.bands[0].freqHz = 0.0;
    check(NAE_SetEq(handle, &params) == NAE_ERR_INVALID_ARGUMENT,
          "an unrealisable band is rejected");

    // The whole array: 32 bands must be accepted and read back, 33 must not.
    params.bandCount = NAE_EQ_MAX_BANDS;
    for (int32_t i = 0; i < NAE_EQ_MAX_BANDS; ++i) {
        params.bands[i].type = NAE_EQ_PEAKING;
        params.bands[i].freqHz = 100.0 * static_cast<double>(i + 1);
        params.bands[i].gainDb = 0.0;
        params.bands[i].q = 1.0;
        params.bands[i].enabled = 0;  // configured but bypassed: a size test
    }
    check(NAE_SetEq(handle, &params) == NAE_OK, "32 bands are accepted");
    NAE_EqParams readback32;
    NAE_EqDefaults(&readback32);
    const int32_t gotBands = NAE_GetEq(handle, &readback32) == NAE_OK
                                 ? readback32.bandCount
                                 : -1;
    check(gotBands == NAE_EQ_MAX_BANDS, "32 bands are reported back",
          std::to_string(gotBands));
    // q is used as asked, not clamped: the header promises a rejected band, not
    // a silently different filter.
    params.bandCount = 1;
    params.phase = NAE_EQ_PHASE_MINIMUM;
    params.bands[0].type = NAE_EQ_PEAKING;
    params.bands[0].freqHz = 1000.0;
    params.bands[0].gainDb = -6.0;
    params.bands[0].q = 0.001;
    params.bands[0].enabled = 1;
    check(NAE_SetEq(handle, &params) == NAE_OK, "an extreme but valid q is accepted");
    params.bands[0].q = -1.0;
    check(NAE_SetEq(handle, &params) == NAE_ERR_INVALID_ARGUMENT,
          "a non-positive q is rejected");

    params.bandCount = NAE_EQ_MAX_BANDS + 1;
    check(NAE_SetEq(handle, &params) == NAE_ERR_INVALID_ARGUMENT,
          "one band over the array is rejected");

    NAE_Close(handle);
    NAE_Shutdown();

    std::printf("\n%s\n", failures == 0 ? "All EQ checks passed."
                                        : "Some EQ checks FAILED.");
    return failures == 0 ? 0 : 1;
}
