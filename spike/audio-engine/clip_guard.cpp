#include "clip_guard.h"

#include <algorithm>
#include <cmath>

namespace neiro {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

double ClipGuard::peak4x(const float* frame, uint32_t channels) {
    double peak = 0.0;
    for (uint32_t c = 0; c < channels; ++c) {
        const size_t taps = kTapsPerPhase;
        const size_t ring = history_[c].size();
        const size_t pos = historyPos_[c];
        for (uint32_t phase = 1; phase < kPhases; ++phase) {
            double acc = 0.0;
            for (size_t k = 0; k < taps; ++k) {
                const size_t idx = (pos + ring - taps + k) % ring;
                acc += coeff_[(phase - 1) * taps + k] * history_[c][idx];
            }
            peak = std::max(peak, std::fabs(acc));
        }
    }
    return peak;
}

bool ClipGuard::configure(uint32_t channels, uint32_t sampleRate, double ceilingDbTp,
                          double lookaheadMs, double releaseMs) {
    if (channels == 0 || channels > 32 || sampleRate == 0) return false;
    if (!std::isfinite(ceilingDbTp) || !std::isfinite(lookaheadMs) ||
        !std::isfinite(releaseMs)) {
        return false;
    }
    if (ceilingDbTp > 0.0 || ceilingDbTp < -24.0) return false;
    if (lookaheadMs < 0.0 || lookaheadMs > 50.0) return false;
    if (releaseMs < 1.0 || releaseMs > 1000.0) return false;

    channels_ = channels;
    rate_ = sampleRate;
    ceiling_ = std::pow(10.0, ceilingDbTp / 20.0);
    lookahead_ = static_cast<size_t>(std::lround(lookaheadMs * sampleRate / 1000.0));
    if (lookahead_ == 0) lookahead_ = 1;
    // Attack over the lookahead window; release from the requested time.
    releaseRate_ = 1.0 / std::max(1.0, releaseMs * sampleRate / 1000.0);
    hold_ = 0;
    delay_.assign(lookahead_ * channels_, 0.0f);
    coeff_.assign((kPhases - 1) * kTapsPerPhase, 0.0);
    for (uint32_t phase = 1; phase < kPhases; ++phase) {
        const double frac = static_cast<double>(phase) / kPhases;
        double sum = 0.0;
        for (uint32_t k = 0; k < kTapsPerPhase; ++k) {
            const double t = static_cast<double>(k) - (kTapsPerPhase / 2 - 1) - frac;
            const double sinc = t == 0.0 ? 1.0
                                         : std::sin(3.14159265358979323846 * t) /
                                               (3.14159265358979323846 * t);
            const double w = 0.54 - 0.46 * std::cos(
                2.0 * 3.14159265358979323846 * (k + 0.5) / kTapsPerPhase);
            const double v = sinc * w;
            coeff_[(phase - 1) * kTapsPerPhase + k] = v;
            sum += v;
        }
        if (std::fabs(sum) > 1e-9) {
            for (uint32_t k = 0; k < kTapsPerPhase; ++k) {
                coeff_[(phase - 1) * kTapsPerPhase + k] /= sum;
            }
        }
    }
    history_.assign(channels_, std::vector<double>(kTapsPerPhase, 0.0));
    historyPos_.assign(channels_, 0);
    write_ = 0;
    gain_ = 1.0;
    active_ = true;
    return true;
}

void ClipGuard::reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    for (auto& h : history_) std::fill(h.begin(), h.end(), 0.0);
    std::fill(historyPos_.begin(), historyPos_.end(), 0);
    write_ = 0;
    gain_ = 1.0;
    hold_ = 0;
}

void ClipGuard::process(float* samples, size_t frames) {
    if (!active_ || !samples || channels_ == 0) return;
    for (size_t f = 0; f < frames; ++f) {
        float* frame = samples + f * channels_;
        double peak = 0.0;
        for (uint32_t c = 0; c < channels_; ++c) {
            peak = std::max(peak, std::fabs(static_cast<double>(frame[c])));
        }
        if (truePeak_) {
            for (uint32_t c = 0; c < channels_; ++c) {
                history_[c][historyPos_[c]] = static_cast<double>(frame[c]);
                historyPos_[c] = (historyPos_[c] + 1) % history_[c].size();
            }
            peak = std::max(peak, peak4x(frame, channels_));
        }
        const double target = peak > ceiling_ ? ceiling_ / peak : 1.0;
        // Attack is immediate, and the gain is held for the lookahead: the
        // sample that caused the reduction is still in the delay line for that
        // many frames, so releasing before then lets the transient out
        // unattenuated (measured with 50 ms lookahead and 1 ms release).
        if (target < gain_) {
            gain_ = target;
            hold_ = static_cast<uint32_t>(lookahead_);
        } else if (hold_ > 0) {
            --hold_;
        } else {
            gain_ += (target - gain_) * std::min(1.0, releaseRate_);
        }
        for (uint32_t c = 0; c < channels_; ++c) {
            const size_t idx = write_ * channels_ + c;
            const float delayed = delay_[idx];
            // Store the *input* sample before the output overwrites the frame;
            // the interpolation history must see the signal, not the limited
            // output.
            const float input = frame[c];
            delay_[idx] = input;
            frame[c] = static_cast<float>(static_cast<double>(delayed) * gain_);
        }
        write_ = (write_ + 1) % lookahead_;
    }
}

double ClipGuard::reduction_db() const {
    if (gain_ <= 0.0) return 0.0;
    return 20.0 * std::log10(gain_);
}

}  // namespace neiro
