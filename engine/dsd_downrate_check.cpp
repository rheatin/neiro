// DSD bitstream downrate. The claim is narrow and measurable: halving the DSD
// rate must keep the tone at the same level and must not fold the shaped noise
// into the audio band.

#include "dsd_downrate.h"
#include "dsd_filter.h"

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

// A second-order sigma-delta, interleaved by byte: byte i belongs to channel
// i % channels, LSB first.
std::vector<uint8_t> makeDsd(double freq, double amplitude, double seconds,
                             uint32_t rate, uint32_t channels) {
    const size_t bits = static_cast<size_t>(std::llround(seconds * rate));
    const size_t bytes = bits / 8;
    std::vector<uint8_t> out(bytes * channels, 0);
    std::vector<double> i1(channels, 0.0), i2(channels, 0.0), prev(channels, 1.0);
    for (size_t n = 0; n < bits; ++n) {
        const double t = static_cast<double>(n) / rate;
        const double x = amplitude * std::sin(2.0 * kPi * freq * t);
        const size_t byteIndex = n / 8;
        const uint32_t bit = static_cast<uint32_t>(n % 8);
        for (uint32_t c = 0; c < channels; ++c) {
            i1[c] += x - prev[c];
            i1[c] = std::max(-4.0, std::min(4.0, i1[c]));
            i2[c] += i1[c] - prev[c];
            i2[c] = std::max(-8.0, std::min(8.0, i2[c]));
            const double y = i2[c] >= 0.0 ? 1.0 : -1.0;
            prev[c] = y;
            if (y > 0.0) {
                out[byteIndex * channels + c] = static_cast<uint8_t>(
                    out[byteIndex * channels + c] | (1u << bit));
            }
        }
    }
    return out;
}

struct Measurement {
    double tone = 0.0;   // amplitude at 1 kHz
    double residual = 0.0;
};

Measurement measure(const std::vector<uint8_t>& dsd, uint32_t dsdRate) {
    neiro::dsd::Cascade cascade;
    std::string err;
    double audioRate = 0.0;
    Measurement m;
    if (!cascade.configure(dsdRate, 2, &audioRate, &err)) return m;
    std::vector<float> pcm;
    const size_t positions = dsd.size() / 2;
    cascade.feedInterleaved(dsd.data(), positions, &pcm, 1u << 22);
    cascade.drainPending(&pcm, 1u << 22);
    const size_t frames = pcm.size() / 2;
    if (frames < 10000) return m;
    const size_t start = 4000;
    const size_t count = std::min<size_t>(frames - start, 40000);
    double re = 0.0, im = 0.0, sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double v = pcm[(start + i) * 2];
        const double w = 2.0 * kPi * 1000.0 * static_cast<double>(i) / audioRate;
        re += v * std::cos(w);
        im -= v * std::sin(w);
        sum += v * v;
    }
    m.tone = 2.0 * std::hypot(re, im) / static_cast<double>(count);
    const double rms = std::sqrt(sum / static_cast<double>(count));
    const double toneRms = m.tone / std::sqrt(2.0);
    m.residual = std::sqrt(std::max(0.0, rms * rms - toneRms * toneRms));
    return m;
}

}  // namespace

int main() {
    constexpr uint32_t dsd64 = 2822400;
    constexpr uint32_t dsd128 = 5644800;
    const auto source = makeDsd(1000.0, 0.5, 0.5, dsd128, 2);
    const auto reference = makeDsd(1000.0, 0.5, 0.5, dsd64, 2);

    neiro::dsd::Downrate down;
    std::string err;
    check(down.configure(dsd128, dsd64, 2, false, &err), "DSD128 to DSD64 configures");
    std::vector<uint8_t> halved;
    down.feed(source.data(), source.size(), &halved);

    const size_t expected = source.size() / 2;
    check(halved.size() + 1 >= expected && halved.size() <= expected,
          "the downrated stream has half the bytes",
          std::to_string(halved.size()) + " vs " + std::to_string(expected));

    const Measurement sourceM = measure(source, dsd128);
    const Measurement refM = measure(reference, dsd64);
    const Measurement outM = measure(halved, dsd64);

    check(sourceM.tone > 0.3, "the source decodes to a tone", std::to_string(sourceM.tone));
    check(std::fabs(outM.tone - sourceM.tone) < 0.01,
          "the downrated tone keeps its level",
          std::to_string(outM.tone) + " vs " + std::to_string(sourceM.tone));
    check(std::fabs(outM.tone - refM.tone) < 0.01,
          "it matches a native DSD64 encoding of the same tone",
          std::to_string(outM.tone) + " vs " + std::to_string(refM.tone));
    const double residualDb = 20.0 * std::log10(std::max(1e-12, outM.residual) /
                                               std::max(1e-12, refM.residual));
    check(std::fabs(residualDb) < 6.0, "in-band noise is within 6 dB of native DSD64",
          std::to_string(residualDb) + " dB");

    // Headroom: a near-full-scale stream must survive the requantiser. The
    // modulator input is clamped at 1.5; a lower clamp (0.9) was measured
    // costing 0.6 dB at 0.99, which is a level error on loud masters.
    {
        const auto hot = makeDsd(1000.0, 0.99, 0.5, dsd128, 2);
        neiro::dsd::Downrate down2;
        std::string err2;
        down2.configure(dsd128, dsd64, 2, false, &err2);
        std::vector<uint8_t> out2;
        down2.feed(hot.data(), hot.size(), &out2);
        const Measurement hotSrc = measure(hot, dsd128);
        const Measurement hotOut = measure(out2, dsd64);
        check(std::fabs(hotOut.tone - hotSrc.tone) < 0.01,
              "a 0.99 full-scale tone keeps its level",
              std::to_string(hotOut.tone) + " vs " + std::to_string(hotSrc.tone));
    }

    // Factor 1 is a passthrough: no filtering, no requantisation, byte-identical.
    {
        neiro::dsd::Downrate same;
        std::string e;
        check(same.configure(dsd64, dsd64, 2, false, &e), "factor 1 configures");
        std::vector<uint8_t> kept;
        same.feed(reference.data(), reference.size(), &kept);
        check(kept == reference, "factor 1 is byte-identical");
    }

    // Rejections.
    {
        neiro::dsd::Downrate bad;
        std::string e;
        check(!bad.configure(dsd64, dsd128, 2, false, &e), "upsampling is rejected");
        check(!bad.configure(dsd128, 1000000, 2, false, &e), "a non-integer factor is rejected");
        check(!bad.configure(dsd128, dsd64, 0, false, &e), "zero channels is rejected");
    }

    if (failures == 0) {
        std::printf("\nAll DSD downrate checks passed.\n");
        return 0;
    }
    std::printf("\nSome DSD downrate checks FAILED.\n");
    return 1;
}
