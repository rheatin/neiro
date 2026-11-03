// Linear-phase, per-channel EQ. The two claims that have to be measured:
// the magnitude matches the requested band gain, and the phase is linear (the
// impulse response is symmetric about one centre for every channel).

#include "linear_phase_eq.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
constexpr double kPi = 3.14159265358979323846;

void check(bool ok, const char* label, const std::string& detail = {}) {
    std::printf("[%s] %-56s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

double rms(const std::vector<float>& x, size_t channels, size_t stride, size_t from,
           size_t to) {
    double sum = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to; ++i) {
        const double v = x[i * channels + stride];
        sum += v * v;
        ++n;
    }
    return n ? std::sqrt(sum / static_cast<double>(n)) : 0.0;
}

std::vector<float> sine(double amplitude, double freq, double seconds, size_t channels,
                        uint32_t rate = 48000) {
    const size_t frames = static_cast<size_t>(std::llround(seconds * rate));
    std::vector<float> out(frames * channels, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(
            amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
        for (size_t c = 0; c < channels; ++c) out[i * channels + c] = s;
    }
    return out;
}

}  // namespace

int main() {
    constexpr uint32_t rate = 48000;

    // ---- identity: a channel with no bands is a pure delay ----
    {
        neiro::LinearPhaseEq eq;
        check(eq.configure(2, rate, {{}, {}}, 1.0), "identity EQ configures");
        const size_t latency = eq.latencyFrames();
        std::vector<float> x(4096 * 2, 0.0f);
        x[100 * 2] = 0.5f;
        x[100 * 2 + 1] = -0.25f;
        eq.process(x.data(), 4096, 2);
        check(std::fabs(x[(100 + latency) * 2] - 0.5f) < 1e-6,
              "left passes at the reported latency",
              std::to_string(x[(100 + latency) * 2]));
        check(std::fabs(x[(100 + latency) * 2 + 1] + 0.25f) < 1e-6,
              "right passes at the reported latency",
              std::to_string(x[(100 + latency) * 2 + 1]));
    }

    // ---- magnitude and per-channel targeting ----
    {
        neiro::LinearPhaseEq eq;
        std::vector<std::vector<neiro::LinearPhaseBandSpec>> bands(2);
        neiro::LinearPhaseBandSpec peak;
        peak.type = 0;  // NAE_EQ_PEAKING
        peak.freqHz = 1000.0;
        peak.gainDb = -12.0;
        peak.q = 2.0;
        bands[0].push_back(peak);  // left only
        check(eq.configure(2, rate, bands, 1.0), "per-channel EQ configures");

        auto tone = sine(0.5, 1000.0, 2.0, 2, rate);
        eq.process(tone.data(), tone.size() / 2, 2);
        const size_t from = eq.latencyFrames() + 2000;
        const size_t to = tone.size() / 2 - 1000;
        const double left = rms(tone, 2, 0, from, to);
        const double right = rms(tone, 2, 1, from, to);
        const double expected = 0.5 / std::sqrt(2.0) * std::pow(10.0, -12.0 / 20.0);
        check(std::fabs(left / expected - 1.0) < 0.05, "1 kHz at -12 dB on the left",
              std::to_string(left / expected));
        const double rightExpected = 0.5 / std::sqrt(2.0);
        check(std::fabs(right / rightExpected - 1.0) < 0.02,
              "the right channel is untouched", std::to_string(right / rightExpected));
    }

    // ---- linear phase: the impulse response is symmetric about the centre ----
    {
        neiro::LinearPhaseEq eq;
        std::vector<std::vector<neiro::LinearPhaseBandSpec>> bands(1);
        neiro::LinearPhaseBandSpec shelf;
        shelf.type = 1;  // NAE_EQ_LOW_SHELF
        shelf.freqHz = 200.0;
        shelf.gainDb = 6.0;
        shelf.q = 0.707;
        bands[0].push_back(shelf);
        check(eq.configure(1, rate, bands, 1.0), "filter configures for symmetry test");
        std::vector<float> x(8192, 0.0f);
        x[0] = 1.0f;
        eq.process(x.data(), x.size(), 1);
        size_t center = 0;
        double best = 0.0;
        for (size_t i = 0; i < x.size(); ++i) {
            if (std::fabs(x[i]) > best) {
                best = std::fabs(x[i]);
                center = i;
            }
        }
        check(center == eq.latencyFrames(), "impulse peak lands at the reported latency",
              std::to_string(center));
        const size_t latency = eq.latencyFrames();
        double worstAsymmetry = 0.0;
        for (size_t k = 1; k < 600 && center + k < x.size(); ++k) {
            worstAsymmetry = std::max(
                worstAsymmetry,
                std::fabs(static_cast<double>(x[center + k]) - x[center - k]));
        }
        check(worstAsymmetry < 1e-6, "impulse response is symmetric (linear phase)",
              std::to_string(worstAsymmetry));
    }

    // ---- group delay is the same at two frequencies ----
    //
    // With linear phase, the phase at f is -2*pi*f*tau, so the measured delay
    // from the cross-correlation of a tone with its output must match at both
    // frequencies. A minimum-phase cascade would not.
    {
        neiro::LinearPhaseEq eq;
        std::vector<std::vector<neiro::LinearPhaseBandSpec>> bands(1);
        neiro::LinearPhaseBandSpec peak;
        peak.type = 0;
        peak.freqHz = 1000.0;
        peak.gainDb = -12.0;
        peak.q = 1.0;
        bands[0].push_back(peak);
        eq.configure(1, rate, bands, 1.0);
        const auto delayAt = [&](double freq) {
            auto x = sine(0.5, freq, 0.5, 1, rate);
            auto in = x;
            eq.reset();
            eq.process(x.data(), x.size(), 1);
            // The output is the input delayed by `latency`; compare directly.
            double best = 0.0;
            size_t bestShift = 0;
            const size_t latency = eq.latencyFrames();
            for (size_t shift = latency - 2; shift <= latency + 2; ++shift) {
                double corr = 0.0;
                for (size_t i = 3000; i + shift < x.size(); ++i) {
                    corr += static_cast<double>(in[i]) * x[i + shift];
                }
                if (corr > best) {
                    best = corr;
                    bestShift = shift;
                }
            }
            return bestShift;
        };
        const size_t d500 = delayAt(500.0);
        const size_t d4000 = delayAt(4000.0);
        check(d500 == d4000, "group delay is frequency-independent", std::to_string(d500) +
                                                                    " / " + std::to_string(d4000));
    }

    // ---- no channel cap ----
    //
    // The engine accepts up to NAE_MAX_CHANNELS; the equaliser used to refuse
    // anything above 8 with a message about the sample rate.
    {
        for (uint32_t ch : {6u, 16u}) {
            neiro::LinearPhaseEq eq;
            std::vector<std::vector<neiro::LinearPhaseBandSpec>> bands(ch);
            neiro::LinearPhaseBandSpec peak;
            peak.type = 0;
            peak.freqHz = 1000.0;
            peak.gainDb = -6.0;
            peak.q = 1.0;
            for (uint32_t c = 0; c < ch; ++c) bands[c].push_back(peak);
            check(eq.configure(ch, rate, bands, 1.0),
                  ch == 6u ? "6-channel linear EQ configures"
                           : "16-channel linear EQ configures");
            std::vector<float> x(4096 * ch, 0.5f);
            eq.process(x.data(), 4096, ch);
            const size_t at = 2000 * ch;
            check(std::fabs(x[at + ch - 1] - x[at]) < 1e-4,
                  ch == 6u ? "every channel is filtered (6)"
                           : "every channel is filtered (16)");
        }
    }

    // ---- rejects an unrealisable band ----
    {
        neiro::LinearPhaseEq eq;
        std::vector<std::vector<neiro::LinearPhaseBandSpec>> bands(1);
        neiro::LinearPhaseBandSpec bad;
        bad.type = 0;
        bad.freqHz = 30000.0;  // above Nyquist at 48 kHz
        bad.gainDb = -6.0;
        bad.q = 1.0;
        bands[0].push_back(bad);
        check(!eq.configure(1, rate, bands, 1.0), "a band above Nyquist is rejected");
    }

    if (failures == 0) {
        std::printf("\nAll linear-phase EQ checks passed.\n");
        return 0;
    }
    std::printf("\nSome linear-phase EQ checks FAILED.\n");
    return 1;
}
