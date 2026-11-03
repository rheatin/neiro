// Digital volume changes the samples, so the check has to measure the samples.
//
// The tone path is used on purpose: its source is a constant 1 kHz sine, so two
// fresh handles have the same peak at the same point in the stream. At unity the
// measurement is the reference; at 0.5 it must halve. A file would put a
// different musical segment in the last FFT window and make the ratio a property
// of the song rather than of the gain.
//
// The bit-perfect flag is checked on the same two handles: unity keeps it, 0.5
// turns it off with the volume reason.

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

bool measure(double volume, double* peak, int32_t* bitPerfect, int32_t* reason) {
    NAE_Handle* handle = nullptr;
    int32_t rate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    double latencyMs = 0.0;

    const int32_t rc = NAE_OpenExclusiveSine(nullptr, &handle, &rate, &channels, &bits,
                                             &valid, &isFloat, &latencyMs);
    if (rc != NAE_OK || !handle) {
        std::printf("open failed: rc=%d\n", rc);
        return false;
    }
    if (volume == 1.0) {
        check(NAE_SetVolume(handle, -0.1) == NAE_ERR_INVALID_ARGUMENT,
              "volume below zero is rejected");
        check(NAE_SetVolume(handle, 1.1) == NAE_ERR_INVALID_ARGUMENT,
              "volume above one is rejected");
    }
    if (volume != 1.0 && NAE_SetVolume(handle, volume) != NAE_OK) {
        NAE_Close(handle);
        return false;
    }

    // 500 ms: the 8 ms start fade and the 20 ms volume ramp are both long settled.
    for (int i = 0; i < 25; ++i) NAE_PlaySine(handle, 20);

    // The spectrum tap is per-window, so it does not keep the peak from the
    // first buffer while the volume ramp was still moving. On a constant tone
    // the last window is the steady state.
    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(info);
    std::vector<float> bins(4096, 0.0f);
    const int32_t spectrumRc = NAE_QuerySpectrum(handle, &info, bins.data(),
                                                 static_cast<int32_t>(bins.size()));
    *peak = spectrumRc == NAE_OK ? info.peak : 0.0;

    NAE_SignalPath path{};
    path.structSize = sizeof(path);
    const int32_t spRc = NAE_QuerySignalPath(handle, &path);
    *bitPerfect = spRc == NAE_OK ? path.bitPerfect : 0;
    *reason = spRc == NAE_OK ? path.reason : -1;

    NAE_Close(handle);
    NAE_Shutdown();
    return true;
}

}  // namespace

int main() {
    double unityPeak = 0.0, halfPeak = 0.0;
    int32_t bpUnity = 0, reasonUnity = -1, bpHalf = 0, reasonHalf = -1;
    const bool okUnity = measure(1.0, &unityPeak, &bpUnity, &reasonUnity);
    const bool okHalf = measure(0.5, &halfPeak, &bpHalf, &reasonHalf);
    check(okUnity && okHalf, "both handles played and reported",
          std::to_string(unityPeak) + " / " + std::to_string(halfPeak));
    if (!okUnity || !okHalf) return 1;

    check(unityPeak > 0.001, "the unity window has signal", std::to_string(unityPeak));
    const double ratio = unityPeak > 0.0 ? halfPeak / unityPeak : 0.0;
    check(std::fabs(ratio - 0.5) < 0.02, "half volume halves the measured peak",
          "ratio " + std::to_string(ratio));

    if (bpUnity) {
        check(bpHalf == 0 && reasonHalf == NAE_REASON_VOLUME_NOT_UNITY,
              "half volume reports not bit-perfect",
              "bitPerfect=" + std::to_string(bpHalf) + " reason=" +
                  std::to_string(reasonHalf));
    } else {
        std::printf("  [SKIP] unity was already not bit-perfect (reason %d)\n", reasonUnity);
    }

    std::printf("\n%s\n", failures == 0 ? "All volume checks passed."
                                        : "Some volume checks FAILED.");
    return failures == 0 ? 0 : 1;
}
