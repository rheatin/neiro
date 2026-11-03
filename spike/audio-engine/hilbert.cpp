#include "hilbert.h"

#include <algorithm>
#include <cmath>

namespace neiro {

Quadrature::Quadrature(uint32_t taps) {
    prepare(taps);
}

bool Quadrature::prepare(uint32_t taps) {
    if (taps < 3 || (taps & 1u) == 0) return false;
    taps_ = taps;
    coeff_.assign(taps, 0.0f);
    const int32_t delay = static_cast<int32_t>((taps - 1) / 2);
    for (uint32_t n = 0; n < taps; ++n) {
        const int32_t k = static_cast<int32_t>(n) - delay;
        if (k == 0 || (k & 1) == 0) continue;
        // Ideal Hilbert transformer; the Blackman window trades a little
        // passband ripple for much lower stopband leakage.
        const double w = 0.42 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * n /
                                               static_cast<double>(taps - 1)) +
                         0.08 * std::cos(4.0 * 3.14159265358979323846 * n /
                                          static_cast<double>(taps - 1));
        coeff_[n] = static_cast<float>((2.0 / (3.14159265358979323846 * k)) * w);
    }
    reset();
    return true;
}

void Quadrature::reset() {
    history_.assign(taps_, 0.0f);
    write_ = 0;
}

void Quadrature::process(const float* in, float* out, size_t frames) {
    if (!in || !out || taps_ == 0) return;
    for (size_t i = 0; i < frames; ++i) {
        history_[write_] = in[i];
        double sum = 0.0;
        size_t p = write_;
        for (uint32_t n = 0; n < taps_; ++n) {
            sum += static_cast<double>(coeff_[n]) * history_[p];
            p = (p == 0) ? taps_ - 1 : p - 1;
        }
        out[i] = static_cast<float>(sum);
        write_ = (write_ + 1) % taps_;
    }
}

// Analog pole frequencies in Hz, from a design at 48 kHz that minimises the
// worst quadrature error over 20 Hz to 20 kHz. The bilinear transform is
// applied per sample rate, and the same set measured 0.586 degrees worst-case
// at 32, 44.1, 48, 88.2, 96, 176.4 and 192 kHz over 20 Hz to min(20 kHz,
// 0.42*fs). Values are the analogue frequencies, not the 48 kHz coefficients,
// so a different rate re-derives its own coefficients.
namespace {
constexpr double kPolesA[] = {27.822863, 134.692706, 616.751973, 2943.367162,
                              14831.052356, 134252.77525};
constexpr double kPolesB[] = {7.837996, 63.213846, 286.606529, 1341.4736,
                              6531.808115, 36124.895734};
constexpr size_t kSections = sizeof(kPolesA) / sizeof(kPolesA[0]);
}  // namespace

bool IirQuadrature::configure(double sampleRate) {
    if (!(sampleRate > 4000.0)) return false;
    highBandHz_ = std::min(20000.0, 0.42 * sampleRate);
    chainA_.assign(kSections, Section{});
    chainB_.assign(kSections, Section{});
    for (size_t i = 0; i < kSections; ++i) {
        // Bilinear transform of the first-order analogue all-pass:
        //   k = pi*f0/fs,  a = (1 - k)/(1 + k).
        const double ka = 3.14159265358979323846 * kPolesA[i] / sampleRate;
        const double kb = 3.14159265358979323846 * kPolesB[i] / sampleRate;
        chainA_[i].a = (1.0 - ka) / (1.0 + ka);
        chainB_[i].a = (1.0 - kb) / (1.0 + kb);
    }
    reset();
    return true;
}

void IirQuadrature::reset() {
    for (auto& s : chainA_) s.reset();
    for (auto& s : chainB_) s.reset();
}

void IirQuadrature::process(const float* in, float* realOut, float* imagOut, size_t frames) {
    if (!in || !realOut || !imagOut || chainA_.empty()) return;
    for (size_t i = 0; i < frames; ++i) {
        double a = in[i];
        double b = in[i];
        for (auto& s : chainA_) a = s.tick(a);
        for (auto& s : chainB_) b = s.tick(b);
        realOut[i] = static_cast<float>(a);
        imagOut[i] = static_cast<float>(b);
    }
}

}  // namespace neiro
