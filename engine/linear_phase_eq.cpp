// Linear-phase, per-channel equaliser. See linear_phase_eq.h.

#include "linear_phase_eq.h"

#include "eq_coeffs.h"
#include "nae.h"
#include "partitioned_convolver.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace neiro {
namespace {

constexpr double kPi = 3.14159265358979323846;

void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        for (auto& v : a) v /= static_cast<double>(n);
    }
}

std::complex<double> biquadResponse(const EqBiquad& q, double w) {
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    return (static_cast<double>(q.b0) + static_cast<double>(q.b1) * z1 +
            static_cast<double>(q.b2) * z2) /
           (1.0 + static_cast<double>(q.a1) * z1 + static_cast<double>(q.a2) * z2);
}

// Design one symmetric FIR from a magnitude response. The impulse is returned
// centred at taps()/2, so the group delay is exactly taps()/2 for every
// frequency.
bool designFir(const std::vector<LinearPhaseBandSpec>& bands, double fs, size_t n,
               std::vector<float>* fir) {
    if (!fir || (n & (n - 1)) != 0) return false;
    fir->assign(n, 0.0f);

    std::vector<EqBiquad> biquads;
    biquads.reserve(bands.size());
    for (const auto& band : bands) {
        NAE_EqBand cb{};
        cb.type = band.type;
        cb.freqHz = band.freqHz;
        cb.gainDb = band.gainDb;
        cb.q = band.q;
        cb.enabled = 1;
        EqBiquad q;
        if (!computeEqBiquad(cb, fs, &q)) return false;
        biquads.push_back(q);
    }

    if (biquads.empty()) {
        // Identity: a centred delta, so this channel carries exactly the same
        // delay as the filtered ones.
        (*fir)[n / 2] = 1.0f;
        return true;
    }

    // Sample the cascade magnitude on the FFT grid.
    std::vector<std::complex<double>> spectrum(n);
    std::vector<double> targetMagnitude(n, 0.0);
    size_t anchorBin = 0;
    double anchorMagnitude = 0.0;
    for (size_t k = 0; k < n; ++k) {
        const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(n);
        std::complex<double> h(1.0, 0.0);
        for (const auto& q : biquads) h *= biquadResponse(q, w);
        targetMagnitude[k] = std::abs(h);
        if (targetMagnitude[k] > anchorMagnitude) {
            anchorMagnitude = targetMagnitude[k];
            anchorBin = k;
        }
        spectrum[k] = std::complex<double>(targetMagnitude[k], 0.0);
    }
    fft(spectrum, true);

    // Centre the impulse and window it. The Blackman window is symmetric, so
    // the result stays linear-phase.
    std::vector<double> centred(n);
    for (size_t i = 0; i < n; ++i) {
        centred[i] = spectrum[(i + n / 2) % n].real();
    }
    // Periodic (not symmetric) Blackman: it is symmetric about index n/2, the
    // same point the impulse is centred on. The symmetric variant is centred
    // on (n-1)/2, which would tilt a half-sample of the phase response.
    // Computed once and reused: it was recomputed inside windowedMagnitudeAt and
    // again in the final loop, for the same numbers.
    std::vector<double> window(n);
    for (size_t i = 0; i < n; ++i) {
        window[i] = 0.42 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                          static_cast<double>(n)) +
                    0.08 * std::cos(4.0 * kPi * static_cast<double>(i) /
                                    static_cast<double>(n));
        centred[i] *= window[i];
    }

    // Anchor the windowed response where the requested magnitude is largest.
    // Normalising by the window's mean instead is wrong: the ideal impulse of a
    // narrow band is concentrated near the centre, where the window is close to
    // 1, so the mean over the whole window over-scales it (measured: a -12 dB
    // band produced 2.38x the requested level).
    const auto windowedMagnitudeAt = [&](size_t bin) {
        const double w = 2.0 * kPi * static_cast<double>(bin) / static_cast<double>(n);
        double re = 0.0;
        double im = 0.0;
        for (size_t i = 0; i < n; ++i) {
            re += centred[i] * std::cos(-w * static_cast<double>(i));
            im += centred[i] * std::sin(-w * static_cast<double>(i));
        }
        return std::hypot(re, im);
    };
    const double measured = windowedMagnitudeAt(anchorBin);
    const double scale = measured > 1e-12 ? anchorMagnitude / measured : 1.0;
    for (size_t i = 0; i < n; ++i) {
        (*fir)[i] = static_cast<float>(centred[i] * scale);
    }
    return true;
}

}  // namespace

struct LinearPhaseEq::Impl {
    uint32_t channels = 0;
    size_t taps = 0;
    std::vector<std::vector<float>> ir;
    std::vector<std::unique_ptr<PartitionedConvolver>> conv;
    std::vector<float> mono;
    double preamp = 1.0;
    bool ready = false;
};

LinearPhaseEq::LinearPhaseEq() : impl_(new Impl) {}
LinearPhaseEq::~LinearPhaseEq() = default;
LinearPhaseEq::LinearPhaseEq(LinearPhaseEq&&) noexcept = default;
LinearPhaseEq& LinearPhaseEq::operator=(LinearPhaseEq&&) noexcept = default;

bool LinearPhaseEq::configure(
    uint32_t channels, uint32_t sampleRate,
    const std::vector<std::vector<LinearPhaseBandSpec>>& channelBands, double preamp) {
    // No channel cap: the engine accepts up to NAE_MAX_CHANNELS, and a lower
    // limit here reported "could not be built at this rate" for a channel-count
    // problem. Memory is one FIR and one convolver per channel.
    if (channels == 0 || channels > NAE_MAX_CHANNELS || sampleRate < 8000) return false;
    impl_->ready = false;
    impl_->channels = channels;
    impl_->preamp = preamp;
    // About 50 Hz between bins, power of two, within the partition limit.
    size_t taps = 1;
    while (taps < static_cast<size_t>(sampleRate / 50)) taps <<= 1;
    taps = std::max<size_t>(2048, std::min<size_t>(8192, taps));
    impl_->taps = taps;
    impl_->ir.assign(channels, {});
    impl_->conv.clear();
    impl_->conv.reserve(channels);
    for (uint32_t c = 0; c < channels; ++c) {
        impl_->conv.push_back(std::make_unique<PartitionedConvolver>());
    }
    impl_->mono.assign(1, 0.0f);
    for (uint32_t c = 0; c < channels; ++c) {
        const std::vector<LinearPhaseBandSpec> empty;
        const auto& bands = c < channelBands.size() ? channelBands[c] : empty;
        if (!designFir(bands, static_cast<double>(sampleRate), taps, &impl_->ir[c])) {
            return false;
        }
        // One channel per convolver: this class filters one channel at a time
        // through its own instance, so the two-channel form would build a
        // second state set that is never read.
        if (!impl_->conv[c]->configure({impl_->ir[c]})) return false;
    }
    impl_->ready = true;
    return true;
}

size_t LinearPhaseEq::taps() const { return impl_->taps; }

size_t LinearPhaseEq::latencyFrames() const {
    return impl_->taps / 2 + blockSize();
}

void LinearPhaseEq::reset() {
    for (auto& c : impl_->conv) c->reset();
}

void LinearPhaseEq::process(float* interleaved, size_t frames, uint32_t channels) {
    if (!impl_->ready) return;
    const uint32_t nch = std::min(channels, impl_->channels);
    for (size_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < nch; ++c) {
            impl_->mono[0] = interleaved[f * channels + c];
            impl_->conv[c]->process(impl_->mono.data(), 1, 1);
            interleaved[f * channels + c] =
                static_cast<float>(static_cast<double>(impl_->mono[0]) * impl_->preamp);
        }
    }
}

}  // namespace neiro
