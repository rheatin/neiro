#include "clip_guard.h"

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
    neiro::ClipGuard guard;
    std::string error;
    check(guard.configure(2, 48000, -1.0, 5.0, 100.0), "clip guard configures");
    check(guard.active(), "clip guard is active after configure");

    constexpr size_t frames = 48000;
    std::vector<float> buf(frames * 2, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(2.0 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * i / 48000.0));
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }
    guard.process(buf.data(), frames);
    double peak = 0.0;
    for (size_t i = 1000; i + 1000 < frames; ++i) {
        peak = std::max(peak, static_cast<double>(std::fabs(buf[i * 2])));
    }
    check(peak <= 0.9, "limiter holds a 2.0 signal under the -1 dBFS ceiling",
          std::to_string(peak));
    check(guard.reduction_db() < 0.0, "gain reduction is reported",
          std::to_string(guard.reduction_db()));

    // reset() must clear the interpolation history as well as the delay: a
    // stale pre-reset peak would reduce a fresh, quiet stream.
    {
        neiro::ClipGuard g;
        g.configure(2, 48000, -1.0, 5.0, 100.0);
        std::vector<float> loud(2048 * 2, 0.0f);
        for (size_t i = 0; i < 2048; ++i) {
            loud[i * 2] = 2.0f;
            loud[i * 2 + 1] = 2.0f;
        }
        g.process(loud.data(), 2048);
        g.reset();
        const size_t latency = 240;  // 5 ms at 48 kHz
        std::vector<float> quiet(4096 * 2, 0.0f);
        quiet[0] = 0.5f;
        quiet[1] = 0.5f;
        g.process(quiet.data(), 4096);
        check(std::fabs(quiet[latency * 2] - 0.5f) < 0.01,
              "reset clears the true-peak history",
              std::to_string(quiet[latency * 2]));
    }

    // Lookahead longer than release: the transient must still be limited.
    {
        neiro::ClipGuard slow;
        check(slow.configure(2, 48000, -1.0, 50.0, 1.0), "long-lookahead guard configures");
        std::vector<float> impulse(48000 * 2, 0.0f);
        impulse[10 * 2] = 2.0f;
        impulse[10 * 2 + 1] = 2.0f;
        slow.process(impulse.data(), 48000);
        double worst = 0.0;
        for (float v : impulse) worst = std::max(worst, static_cast<double>(std::fabs(v)));
        check(worst <= 0.900, "50 ms lookahead with 1 ms release still limits",
              std::to_string(worst));
    }

    // A genuine inter-sample overshoot: fs/4 sine sampled at 45 degrees.
    // Its samples sit at 0.707 * amplitude, but the waveform between them
    // reaches the full amplitude. A sample-peak limiter therefore sees
    // 0.707*A; a true-peak limiter sees A. With A = 1.5 and a -1 dBFS
    // (0.891) ceiling the two detectors must reduce by different amounts:
    // sample peak is 0.778 (under the ceiling), true peak is 1.10 (over it).
    {
        neiro::ClipGuard tp;
        check(tp.configure(2, 48000, -1.0, 5.0, 100.0), "true-peak guard configures");
        std::vector<float> buf(2048 * 2, 0.0f);
        for (size_t i = 0; i < 2048; ++i) {
            const double phase = 2.0 * 3.14159265358979323846 * 12000.0 * i / 48000.0 +
                                 3.14159265358979323846 / 4.0;
            const float s = static_cast<float>(1.10 * std::sin(phase));
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
        }
        tp.process(buf.data(), 2048);
        double tpPeak = 0.0;
        for (size_t i = 100; i + 100 < 2048; ++i) {
            tpPeak = std::max(tpPeak, static_cast<double>(std::fabs(buf[i * 2])));
        }

        neiro::ClipGuard sampleOnly;
        sampleOnly.configure(2, 48000, -1.0, 5.0, 100.0);
        sampleOnly.set_true_peak(false);
        std::vector<float> control(2048 * 2, 0.0f);
        for (size_t i = 0; i < 2048; ++i) {
            const double phase = 2.0 * 3.14159265358979323846 * 12000.0 * i / 48000.0 +
                                 3.14159265358979323846 / 4.0;
            const float s = static_cast<float>(1.10 * std::sin(phase));
            control[i * 2] = s;
            control[i * 2 + 1] = s;
        }
        sampleOnly.process(control.data(), 2048);
        double spPeak = 0.0;
        for (size_t i = 100; i + 100 < 2048; ++i) {
            spPeak = std::max(spPeak, static_cast<double>(std::fabs(control[i * 2])));
        }
        check(tpPeak < spPeak - 0.05, "true-peak reduces more than sample-peak",
              "tp=" + std::to_string(tpPeak) + " sp=" + std::to_string(spPeak));
        check(tpPeak < 0.72, "true-peak output sits below the sample-only result",
              std::to_string(tpPeak));
    }

    neiro::ClipGuard bad;
    check(!bad.configure(2, 48000, 3.0, 5.0, 100.0), "positive ceiling is rejected");
    check(!bad.configure(0, 48000, -1.0, 5.0, 100.0), "zero channels is rejected");
    check(!bad.configure(2, 48000, -1.0, 80.0, 100.0), "excess lookahead is rejected");

    std::printf("\n%s\n", failures == 0 ? "All clip guard checks passed."
                                        : "Some clip guard checks FAILED.");
    return failures == 0 ? 0 : 1;
}
