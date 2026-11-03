// ITU-R BS.1770-4 / EBU R128 loudness. See loudness.h.

#include "loudness.h"

#include <algorithm>
#include <cmath>

namespace neiro {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kOffset = -0.691;

double toLufs(double z) {
    if (!(z > 0.0)) return -200.0;
    return kOffset + 10.0 * std::log10(z);
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return -200.0;
    std::sort(values.begin(), values.end());
    const double pos = fraction * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

}  // namespace

bool LoudnessMeter::configure(uint32_t channels, uint32_t sampleRate) {
    if (channels == 0 || sampleRate < 8000) return false;
    channels_ = channels;
    sampleRate_ = sampleRate;

    shelf_.assign(channels, Biquad{});
    highpass_.assign(channels, Biquad{});

    // Stage 1: high shelf. Parameters from the BS.1770 reference implementation.
    {
        const double f0 = 1681.974450955533;
        const double G = 3.999843853973347;
        const double Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / static_cast<double>(sampleRate));
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        Biquad f;
        f.b0 = (Vh + Vb * K / Q + K * K) / a0;
        f.b1 = 2.0 * (K * K - Vh) / a0;
        f.b2 = (Vh - Vb * K / Q + K * K) / a0;
        f.a1 = 2.0 * (K * K - 1.0) / a0;
        f.a2 = (1.0 - K / Q + K * K) / a0;
        for (auto& s : shelf_) s = f;
    }

    // Stage 2: RLB high-pass.
    {
        const double f0 = 38.13547087602444;
        const double Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / static_cast<double>(sampleRate));
        const double a0 = 1.0 + K / Q + K * K;
        Biquad f;
        f.b0 = 1.0;
        f.b1 = -2.0;
        f.b2 = 1.0;
        f.a1 = 2.0 * (K * K - 1.0) / a0;
        f.a2 = (1.0 - K / Q + K * K) / a0;
        for (auto& s : highpass_) s = f;
    }

    // Channel weights in this engine's bit order: L, R, C = 1; LFE = 0;
    // every surround (BL, BR, BC, SL, SR, ...) = 1.41. Special-casing only
    // indices 4 and 5 left 7.1's back/side channels at 1.0 and biased
    // integrated loudness low on those layouts.
    channelWeight_.assign(channels, 1.0);
    if (channels >= 6) {
        channelWeight_[3] = 0.0;  // LFE is excluded by BS.1770
        for (uint32_t c = 4; c < channels; ++c) channelWeight_[c] = 1.41;
    }

    segmentLength_ = static_cast<uint32_t>(
        std::max<long long>(1, std::llround(sampleRate * kSegmentMs / 1000.0)));
    segmentEnergy_.assign(channels, {});
    segmentAcc_.assign(channels, 0.0);
    segmentFill_ = 0;
    return true;
}

void LoudnessMeter::process(const float* interleaved, size_t frames, uint32_t channels) {
    if (!segmentLength_ || channels == 0) return;
    const uint32_t nch = std::min(channels, channels_);
    for (size_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < nch; ++c) {
            const double x = static_cast<double>(interleaved[f * channels + c]);
            const double y = highpass_[c].tick(shelf_[c].tick(x));
            segmentAcc_[c] += y * y;
        }
        if (++segmentFill_ >= segmentLength_) {
            for (uint32_t c = 0; c < nch; ++c) {
                segmentEnergy_[c].push_back(segmentAcc_[c] /
                                            static_cast<double>(segmentLength_));
                segmentAcc_[c] = 0.0;
            }
            segmentFill_ = 0;
        }
    }
}

LoudnessMeter::Result LoudnessMeter::finish() const {
    Result r;
    if (segmentEnergy_.empty() || segmentEnergy_[0].size() < kBlockSegments) return r;
    const size_t segments = segmentEnergy_[0].size();
    const uint32_t nch = static_cast<uint32_t>(segmentEnergy_.size());

    // z for a run of segments: weighted sum of per-channel mean squares.
    const auto energy = [&](size_t from, size_t count) {
        double z = 0.0;
        for (uint32_t c = 0; c < nch; ++c) {
            double sum = 0.0;
            for (size_t k = from; k < from + count; ++k) {
                sum += segmentEnergy_[c][k];
            }
            const double mean = sum / static_cast<double>(count);
            z += channelWeight_[c] * mean;
        }
        return z;
    };

    // 400 ms blocks, 75% overlap (100 ms hop).
    std::vector<double> blockZ;
    blockZ.reserve(segments - kBlockSegments + 1);
    for (size_t k = 0; k + kBlockSegments <= segments; ++k) {
        blockZ.push_back(energy(k, kBlockSegments));
    }
    r.totalBlocks = static_cast<uint32_t>(blockZ.size());
    r.momentaryMaxLufs = -200.0;
    for (double z : blockZ) {
        r.momentaryMaxLufs = std::max(r.momentaryMaxLufs, toLufs(z));
    }

    // Integrated: absolute gate at -70 LUFS, then relative gate 10 LU below the
    // loudness of what passed the absolute gate.
    std::vector<double> absolute;
    for (double z : blockZ) {
        if (toLufs(z) > -70.0) absolute.push_back(z);
    }
    if (!absolute.empty()) {
        double sum = 0.0;
        for (double z : absolute) sum += z;
        const double relativeGate = toLufs(sum / static_cast<double>(absolute.size())) - 10.0;
        double gatedSum = 0.0;
        uint32_t gated = 0;
        for (double z : absolute) {
            if (toLufs(z) > relativeGate) {
                gatedSum += z;
                ++gated;
            }
        }
        if (gated > 0) {
            r.integratedLufs = toLufs(gatedSum / static_cast<double>(gated));
            r.gatedBlocks = gated;
            r.valid = true;
        }
    }

    // Short-term windows: 3 s, 1 s hop, for short-term max and LRA.
    std::vector<double> shortTermZ;
    for (size_t k = 0; k + kShortTermSegments <= segments; k += kShortTermHop) {
        shortTermZ.push_back(energy(k, kShortTermSegments));
    }
    r.shortTermMaxLufs = 0.0;
    for (double z : shortTermZ) {
        if (!r.shortTermValid) {
            r.shortTermMaxLufs = toLufs(z);
            r.shortTermValid = true;
        } else {
            r.shortTermMaxLufs = std::max(r.shortTermMaxLufs, toLufs(z));
        }
    }

    // EBU Tech 3342: absolute gate -70 LUFS, relative gate -20 LU, then
    // P95 - P10 of the surviving short-term loudness values.
    std::vector<double> stAbsolute;
    for (double z : shortTermZ) {
        if (toLufs(z) > -70.0) stAbsolute.push_back(z);
    }
    if (!stAbsolute.empty()) {
        double sum = 0.0;
        for (double z : stAbsolute) sum += z;
        const double relativeGate = toLufs(sum / static_cast<double>(stAbsolute.size())) - 20.0;
        std::vector<double> kept;
        for (double z : stAbsolute) {
            const double l = toLufs(z);
            if (l > relativeGate) kept.push_back(l);
        }
        if (kept.size() >= 2) {
            r.rangeLu = percentile(kept, 0.95) - percentile(kept, 0.10);
        }
    }
    return r;
}

}  // namespace neiro
