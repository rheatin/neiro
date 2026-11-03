#include "hilbert.h"
#include "nae.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* label, const std::string& detail = std::string()) {
    if (!ok) ++failures;
    std::printf("  [%s] %-42s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}
}  // namespace

int main() {
    constexpr double fs = 48000.0;
    constexpr double f = 1000.0;
    const size_t frames = 48000;
    std::vector<float> in(frames), out(frames);
    for (size_t i = 0; i < frames; ++i)
        in[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * f * i / fs));

    neiro::Quadrature h(255);
    check(h.taps() == 255 && h.latency_frames() == 127, "Hilbert 255 taps, 127 latency");
    h.process(in.data(), out.data(), frames);

    double ss = 0.0, cc = 0.0, sc = 0.0, sy = 0.0, cy = 0.0;
    for (size_t i = 500; i + 500 < frames; ++i) {
        const double t = static_cast<double>(i) - h.latency_frames();
        const double s = std::sin(2.0 * 3.14159265358979323846 * f * t / fs);
        const double c = std::cos(2.0 * 3.14159265358979323846 * f * t / fs);
        ss += s * s; cc += c * c; sc += s * c;
        sy += s * out[i]; cy += c * out[i];
    }
    const double det = ss * cc - sc * sc;
    const double a = (sy * cc - cy * sc) / det;
    const double b = (cy * ss - sy * sc) / det;
    const double amp = std::hypot(a, b);
    const double phase = std::atan2(b, a) * 180.0 / 3.14159265358979323846;
    char detail[96];
    std::snprintf(detail, sizeof(detail), "amp %.5f phase %.2f deg", amp, phase);
    check(std::fabs(amp - 1.0) < 0.01, "Hilbert amplitude at 1 kHz", detail);
    // Signed, not absolute: the routing matrix's quadrature gains were written
    // for the -90 degree convention, and a sign flip would invert the surround
    // orientation. The IIR mode is pinned to the same sign.
    check(std::fabs(phase + 90.0) < 1.0, "Hilbert quadrature at 1 kHz", detail);

    // IIR all-pass pair: both outputs are all-pass, and the phase difference is
    // the quantity the matrix consumes.
    {
        constexpr double fs2 = 48000.0;
        std::vector<float> sig(frames), realOut(frames), imagOut(frames);
        for (size_t i = 0; i < frames; ++i)
            sig[i] = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * f * i / fs2));
        neiro::IirQuadrature q;
        check(q.configure(fs2), "IIR quadrature configures");
        check(q.sectionsPerPath() == 6, "IIR has 6 sections per path",
              std::to_string(q.sectionsPerPath()));
        q.process(sig.data(), realOut.data(), imagOut.data(), frames);

        const auto tone = [&](const std::vector<float>& x, double* amp, double* phase) {
            double ss = 0, cc = 0, sc = 0, sy = 0, cy = 0;
            for (size_t i = 500; i + 500 < frames; ++i) {
                const double t = static_cast<double>(i);
                const double s = std::sin(2.0 * 3.14159265358979323846 * f * t / fs2);
                const double c = std::cos(2.0 * 3.14159265358979323846 * f * t / fs2);
                ss += s * s; cc += c * c; sc += s * c;
                sy += s * x[i]; cy += c * x[i];
            }
            const double det = ss * cc - sc * sc;
            const double a = (sy * cc - cy * sc) / det;
            const double b = (cy * ss - sy * sc) / det;
            *amp = std::hypot(a, b);
            *phase = std::atan2(b, a) * 180.0 / 3.14159265358979323846;
        };
        double aReal = 0, pReal = 0, aImag = 0, pImag = 0;
        tone(realOut, &aReal, &pReal);
        tone(imagOut, &aImag, &pImag);
        char detail[128];
        std::snprintf(detail, sizeof(detail), "amps %.4f/%.4f", aReal, aImag);

        check(std::fabs(aReal - 1.0) < 0.001 && std::fabs(aImag - 1.0) < 0.001,
              "IIR paths are flat (all-pass)", detail);
        double diff = pImag - pReal;
        while (diff <= -180.0) diff += 360.0;
        while (diff > 180.0) diff -= 360.0;
        std::snprintf(detail, sizeof(detail), "phase diff %.2f deg", diff);
        check(std::fabs(diff + 90.0) < 1.0, "IIR quadrature at 1 kHz", detail);

        // Worst case over the design band, measured at 1/3-octave spacing.
        double worst = 0.0;
        double worstHz = 0.0;
        for (double hz = 20.0; hz <= 20000.0; hz *= 1.12) {
            std::vector<float> x(frames), rr(frames), ii(frames);
            for (size_t i = 0; i < frames; ++i)
                x[i] = static_cast<float>(
                    std::sin(2.0 * 3.14159265358979323846 * hz * i / fs2));
            neiro::IirQuadrature probe;
            probe.configure(fs2);
            probe.process(x.data(), rr.data(), ii.data(), frames);
            double ar = 0, pr = 0, ai = 0, pi = 0;
            const auto measure = [&](const std::vector<float>& y, double* amp, double* ph) {
                double sS = 0, cC = 0, sC = 0, sY = 0, cY = 0;
                for (size_t i = 500; i + 500 < frames; ++i) {
                    const double t = static_cast<double>(i);
                    const double s = std::sin(2.0 * 3.14159265358979323846 * hz * t / fs2);
                    const double c = std::cos(2.0 * 3.14159265358979323846 * hz * t / fs2);
                    sS += s * s; cC += c * c; sC += s * c;
                    sY += s * y[i]; cY += c * y[i];
                }
                const double det = sS * cC - sC * sC;
                const double aa = (sY * cC - cY * sC) / det;
                const double bb = (cY * sS - sY * sC) / det;
                *amp = std::hypot(aa, bb);
                *ph = std::atan2(bb, aa) * 180.0 / 3.14159265358979323846;
            };
            measure(rr, &ar, &pr);
            measure(ii, &ai, &pi);
            double d = pi - pr;
            while (d <= -180.0) d += 360.0;
            while (d > 180.0) d -= 360.0;
            if (std::fabs(d + 90.0) > worst) {
                worst = std::fabs(d + 90.0);
                worstHz = hz;
            }
        }
        std::snprintf(detail, sizeof(detail), "worst %.2f deg at %.0f Hz", worst, worstHz);
        check(worst < 1.0, "IIR quadrature over 20 Hz - 20 kHz", detail);
    }

    NAE_RoutingMatrix m{};
    const int32_t rc = NAE_BuildRoutingMatrix(NAE_ROUTING_DOWNMIX, NAE_DOWNMIX_LTRT,
                                               NAE_LAYOUT_5_1, NAE_LAYOUT_STEREO, &m);
    check(rc == NAE_OK, "LtRt matrix builds");
    check(m.phaseMode == NAE_PHASE_IIR_ALLPASS_90, "LtRt uses IIR all-pass Hilbert");
    check(m.phaseOrder == 6 && m.alignPhaseLatency == 0,
          "LtRt asks for 6 sections and no alignment delay");
    check(m.gain[0][4].quadrature > 0.7f, "Lt has +Ls quadrature");

    std::printf("\n%s\n", failures == 0 ? "All Hilbert checks passed."
                                        : "Some Hilbert checks FAILED.");
    return failures == 0 ? 0 : 1;
}
