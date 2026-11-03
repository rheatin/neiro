#include "routing.h"
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
}  // namespace

int main() {
    NAE_RoutingMatrix matrix{};
    const int32_t rc = NAE_BuildRoutingMatrix(NAE_ROUTING_DOWNMIX, NAE_DOWNMIX_LTRT,
                                               NAE_LAYOUT_5_1, NAE_LAYOUT_STEREO, &matrix);
    check(rc == NAE_OK, "LtRt matrix builds");
    neiro::RoutingProcessor routing;
    std::string error;
    check(routing.configure(matrix, 512, &error), "routing configures", error);
    check(routing.source_channels() == 6 && routing.target_channels() == 2,
          "routing channel counts");
    check(routing.latency_frames() == 0, "IIR LtRt needs no alignment latency",
          std::to_string(routing.latency_frames()));

    // Alignment delay applies to every target, not only the first. With one
    // source feeding two targets, both direct paths must land at the same
    // index; before the fix only target 0 was delayed.
    {
        NAE_RoutingMatrix mono{};
        mono.sourceChannels = 1;
        mono.targetChannels = 2;
        mono.sourceMask = NAE_LAYOUT_MONO;
        mono.targetMask = NAE_LAYOUT_STEREO;
        mono.gain[0][0].real = 1.0f;
        mono.gain[1][0].real = 1.0f;
        mono.phaseMode = NAE_PHASE_FIR_HILBERT;
        mono.phaseTaps = 255;
        mono.alignPhaseLatency = 1;
        neiro::RoutingProcessor p;
        std::string e;
        check(p.configure(mono, 512, &e), "mono-to-stereo FIR routing configures", e);
        std::vector<float> impulse(512, 0.0f);
        impulse[0] = 1.0f;
        std::vector<float> stereo(512 * 2, 0.0f);
        p.process(impulse.data(), 512, stereo.data());
        size_t peakL = 0, peakR = 0;
        for (size_t f = 0; f < 512; ++f) {
            if (std::fabs(stereo[f * 2]) > std::fabs(stereo[peakL * 2])) peakL = f;
            if (std::fabs(stereo[f * 2 + 1]) > std::fabs(stereo[peakR * 2 + 1])) peakR = f;
        }
        check(peakL == 127 && peakR == 127,
              "both targets carry the same alignment delay",
              std::to_string(peakL) + " / " + std::to_string(peakR));
    }

    // An even FIR length is rejected rather than silently producing no
    // quadrature at all.
    {
        NAE_RoutingMatrix badFir = matrix;
        badFir.phaseMode = NAE_PHASE_FIR_HILBERT;
        badFir.phaseTaps = 256;
        neiro::RoutingProcessor rejected;
        std::string badError;
        check(!rejected.configure(badFir, 512, &badError),
              "an even FIR Hilbert length is rejected", badError);
    }

    // An unsupported IIR order is rejected rather than ignored.
    {
        NAE_RoutingMatrix wrong = matrix;
        wrong.phaseOrder = 4;
        neiro::RoutingProcessor rejected;
        std::string wrongError;
        check(!rejected.configure(wrong, 512, &wrongError),
              "an undesigned IIR order is rejected", wrongError);
    }

    // The FIR mode is still selectable and still reports its group delay.
    {
        NAE_RoutingMatrix fir = matrix;
        fir.phaseMode = NAE_PHASE_FIR_HILBERT;
        fir.phaseTaps = 255;
        fir.alignPhaseLatency = 1;
        neiro::RoutingProcessor firRouting;
        std::string firError;
        check(firRouting.configure(fir, 512, &firError), "FIR routing configures", firError);
        check(firRouting.latency_frames() == 127, "FIR routing latency 127",
              std::to_string(firRouting.latency_frames()));
    }

    constexpr size_t kFrames = 512;
    constexpr double kRate = 48000.0;
    constexpr double kFreq = 1000.0;
    std::vector<float> in(kFrames * 6, 0.0f);
    std::vector<float> out(kFrames * 2, 0.0f);
    for (size_t f = 0; f < kFrames; ++f) {
        const double t = static_cast<double>(f) / kRate;
        const float s = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * kFreq * t));
        in[f * 6 + 4] = s;  // BL/Ls
        in[f * 6 + 5] = s;  // BR/Rs
    }
    routing.process(in.data(), kFrames, out.data());

    double lt = 0.0, rt = 0.0;
    for (size_t f = 200; f + 200 < kFrames; ++f) {
        lt = std::max(lt, std::fabs(static_cast<double>(out[f * 2])));
        rt = std::max(rt, std::fabs(static_cast<double>(out[f * 2 + 1])));
    }
    check(lt > 0.5, "Lt has Ls signal", std::to_string(lt));
    check(rt > 0.5, "Rt has Rs signal", std::to_string(rt));

    // Direct path (left input only) must remain real.
    std::fill(in.begin(), in.end(), 0.0f);
    for (size_t f = 0; f < kFrames; ++f) in[f * 6] = 0.5f;
    routing.process(in.data(), kFrames, out.data());
    double maxDirect = 0.0;
    for (size_t f = 200; f + 200 < kFrames; ++f) {
        maxDirect = std::max(maxDirect, std::fabs(static_cast<double>(out[f * 2])));
    }
    check(std::fabs(maxDirect - 0.5) < 0.01, "direct L path stays 0.5",
          std::to_string(maxDirect));

    std::printf("\n%s\n", failures == 0 ? "All routing checks passed."
                                        : "Some routing checks FAILED.");
    return failures == 0 ? 0 : 1;
}
