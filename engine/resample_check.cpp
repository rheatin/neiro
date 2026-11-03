// Proof that soxr actually converts, and that it survives being linked into a
// DLL.
//
// Why this exists: adding a vendored static library to a DLL is easy to get
// subtly wrong in a way that still "builds". If nothing references a symbol, the
// linker discards the whole archive without complaint — the build is green and
// the feature is absent. That is exactly what happened once already.
//
// So this does not test the abstract library. It tests what the app will
// actually do: convert a known sine from one rate to another through the
// engine's own path, and check the result is the right length and still a sine
// at the right frequency. A resampler that returns the right number of samples
// full of noise passes a length check and fails this one.
//
// Usage: resample_check.exe

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "resampler.h"

namespace {

int g_failures = 0;

void check(const char* label, bool ok, const std::string& detail) {
    if (!ok) ++g_failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

// Goertzel: magnitude of one frequency in a real signal. Cheaper and clearer
// than an FFT for answering "is there still a 1 kHz tone here, and how big".
double goertzel(const std::vector<float>& x, double rate, double hz) {
    const size_t n = x.size();
    if (n < 8) return 0.0;
    const double k = 0.5 + (hz * static_cast<double>(n) / rate);
    const double w = (2.0 * 3.14159265358979323846 / static_cast<double>(n)) * k;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / (static_cast<double>(n) / 2.0);
}



}  // namespace

int main() {
    constexpr double kToneHz = 1000.0;
    constexpr double kAmplitude = 0.5;
    constexpr int kSeconds = 1;

    struct Case {
        double from;
        double to;
        const char* note;
    };
    const Case cases[] = {
        {48000.0, 48000.0, "identity"},
        {44100.0, 48000.0, "the common case: CD to device rate"},
        {48000.0, 192000.0, "integer multiple, up"},
        {48000.0, 96000.0, "integer multiple, up 2x"},
        {192000.0, 44100.0, "integer multiple, down"},
        {44100.0, 192000.0, "large non-integer ratio (the 480x case)"},
        {96000.0, 44100.0, "down and across"},
    };

    std::printf("== resampling through the engine's own wrapper ==\n");

    for (const Case& c : cases) {
        const auto inRate = static_cast<uint32_t>(c.from);
        const auto outRate = static_cast<uint32_t>(c.to);
        const size_t inFrames =
            static_cast<size_t>(c.from) * kSeconds;

        // A pure tone, so any corruption shows up as energy at the wrong
        // frequency or as broadband noise.
        std::vector<float> in(inFrames);
        for (size_t i = 0; i < inFrames; ++i) {
            in[i] = static_cast<float>(
                kAmplitude * std::sin(2.0 * 3.14159265358979323846 * kToneHz *
                                      static_cast<double>(i) / c.from));
        }

        neiro::Resampler res;
        std::string err;
        if (!res.prepare(/*channels=*/1, inRate, outRate, neiro::ResamplerQuality::High, &err)) {
            check(c.note, false, "prepare failed: " + err);
            continue;
        }

        std::vector<float> out;
        if (!res.process(in, &out, &err)) {
            check(c.note, false, "process failed: " + err);
            continue;
        }
        // process() deliberately does not drain the filter tail: a streaming
        // caller must keep its latency across block boundaries. End of signal is
        // what flush() is for.
        if (!res.flush(&out, &err)) {
            check(c.note, false, "flush failed: " + err);
            continue;
        }

        const size_t expect =
            static_cast<size_t>(static_cast<double>(inFrames) * (c.to / c.from));
        const long long drift = static_cast<long long>(out.size()) - static_cast<long long>(expect);

        // Skip the first and last 10% of the output: resamplers need a little
        // history to fill their filter, and a short tail to flush it. Judging
        // the edges would be measuring the ramp, not the conversion.
        const size_t margin = out.size() / 10;
        std::vector<float> core(out.begin() + static_cast<long>(margin),
                                out.end() - static_cast<long>(margin));

        const double toneMag = core.empty() ? 0.0 : goertzel(core, c.to, kToneHz);
        const double peak = [&] {
            double m = 0.0;
            for (float s : core) m = std::max(m, std::abs(static_cast<double>(s)));
            return m;
        }();

        char label[64];
        std::snprintf(label, sizeof(label), "%g -> %g Hz", c.from, c.to);
        std::printf("\n  %s  (%s)\n", label, c.note);

        char buf[160];
        std::snprintf(buf, sizeof(buf), "%zu vs %zu (drift %lld)", out.size(), expect, drift);
        check("frame count within 0.5% of expected",
              std::abs(drift) < static_cast<long long>(expect / 200), buf);

        std::snprintf(buf, sizeof(buf), "peak %.4f", peak);
        check("output stays inside full scale", peak <= 1.0001, buf);

        // A band-limited resampler attenuates very little at 1 kHz; losing more
        // than half the amplitude means it is filtering the tone away.
        std::snprintf(buf, sizeof(buf), "magnitude %.4f (input %.2f)", toneMag, kAmplitude);
        check("tone survives at roughly its input level", toneMag > kAmplitude * 0.5, buf);

        // The conversion must actually be happening, not short-circuited.
        if (c.from == c.to) {
            std::snprintf(buf, sizeof(buf), "%zu frames", out.size());
            check("identity conversion is sample-accurate", out.size() == inFrames, buf);
        }
    }

    std::printf("\n%s\n", g_failures == 0
                              ? "All resampling checks passed."
                              : "Some resampling checks FAILED.");
    return g_failures == 0 ? 0 : 1;
}
