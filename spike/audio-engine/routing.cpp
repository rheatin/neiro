#include "routing.h"

#include <algorithm>
#include <cmath>

namespace neiro {

namespace {
uint32_t bitByIndex(uint32_t mask, int index) {
    int n = 0;
    for (uint32_t b = 1; b != 0; b <<= 1) {
        if (mask & b) {
            if (n == index) return b;
            ++n;
        }
    }
    return 0;
}
int indexOfBit(uint32_t mask, uint32_t bit) {
    if (!(mask & bit)) return -1;
    int n = 0;
    for (uint32_t b = 1; b != bit; b <<= 1) {
        if (mask & b) ++n;
    }
    return n;
}
}  // namespace

bool RoutingProcessor::configure(const NAE_RoutingMatrix& matrix, size_t maxFrames,
                                 std::string* error) {
    ready_ = false;
    if (matrix.sourceChannels == 0 || matrix.targetChannels == 0 ||
        matrix.sourceChannels > NAE_MAX_MATRIX_CHANNELS ||
        matrix.targetChannels > NAE_MAX_MATRIX_CHANNELS) {
        if (error) *error = "invalid routing channel counts";
        return false;
    }
    if (matrix.phaseMode != NAE_PHASE_NONE && matrix.phaseMode != NAE_PHASE_FIR_HILBERT &&
        matrix.phaseMode != NAE_PHASE_IIR_ALLPASS_90) {
        if (error) *error = "unknown phase mode";
        return false;
    }

    sourceChannels_ = matrix.sourceChannels;
    targetChannels_ = matrix.targetChannels;
    maxFrames_ = maxFrames ? maxFrames : 1;
    iir_ = matrix.phaseMode == NAE_PHASE_IIR_ALLPASS_90;
    if (iir_) {
        // One pole set was designed, six sections per path. Accepting another
        // order and silently using six would make phaseOrder a number with no
        // effect, which is worse than rejecting it.
        const uint32_t order = matrix.phaseOrder == 0 ? 6 : matrix.phaseOrder;
        if (order != 6) {
            if (error) *error = "the IIR Hilbert is designed for 6 sections per path";
            return false;
        }
    }
    // The IIR pair filters both paths, so there is no group delay to align and
    // delaying the direct path would break the quadrature relation.
    align_ = !iir_ && matrix.alignPhaseLatency != 0;
    phase_ = iir_ || matrix.phaseMode == NAE_PHASE_FIR_HILBERT;
    latencyFrames_ = iir_    ? 0
                     : phase_ && matrix.phaseTaps ? (matrix.phaseTaps - 1) / 2
                     : phase_                    ? 127
                                                 : 0;

    directFrame_.assign(sourceChannels_, 0.0f);
    sourceBits_.resize(sourceChannels_);
    for (size_t s = 0; s < sourceChannels_; ++s) sourceBits_[s] = bitByIndex(matrix.sourceMask, static_cast<int>(s));
    targetBits_.resize(targetChannels_);
    for (size_t t = 0; t < targetChannels_; ++t) targetBits_[t] = bitByIndex(matrix.targetMask, static_cast<int>(t));

    real_.assign(targetChannels_ * sourceChannels_, 0.0f);
    quad_.assign(targetChannels_ * sourceChannels_, 0.0f);
    for (size_t t = 0; t < targetChannels_; ++t) {
        for (size_t s = 0; s < sourceChannels_; ++s) {
            real_[t * sourceChannels_ + s] = matrix.gain[t][s].real;
            quad_[t * sourceChannels_ + s] = matrix.gain[t][s].quadrature;
        }
    }

    hilbert_.clear();
    hilbertOut_.assign(sourceChannels_, {});
    iirFilters_.clear();
    iirReal_.assign(sourceChannels_, {});
    iirImag_.assign(sourceChannels_, {});
    delay_.assign(sourceChannels_, {});
    delayPos_.assign(sourceChannels_, 0);
    if (iir_) {
        const double rate = matrix.sampleRate > 0.0 ? matrix.sampleRate : 48000.0;
        for (size_t s = 0; s < sourceChannels_; ++s) {
            bool needsQuad = false;
            for (size_t t = 0; t < targetChannels_; ++t) {
                if (std::fabs(quad_[t * sourceChannels_ + s]) > 1e-8f) needsQuad = true;
            }
            if (!needsQuad) continue;
            IirQuadrature q;
            if (!q.configure(rate)) {
                if (error) *error = "IIR Hilbert cannot be configured at this rate";
                return false;
            }
            iirFilters_.push_back(q);
            iirReal_[s].assign(maxFrames_, 0.0f);
            iirImag_[s].assign(maxFrames_, 0.0f);
        }
    } else if (phase_) {
        const uint32_t taps = matrix.phaseTaps ? matrix.phaseTaps : 255;
        // The FIR designer needs an odd length of at least 63; without this
        // check an even value leaves taps_ == 0 and the quadrature path
        // silently outputs nothing while configure() reports success.
        if (taps < 63 || (taps & 1u) == 0) {
            if (error) *error = "FIR Hilbert taps must be odd and at least 63";
            return false;
        }
        for (size_t s = 0; s < sourceChannels_; ++s) {
            bool needsQuad = false;
            for (size_t t = 0; t < targetChannels_; ++t) {
                if (std::fabs(quad_[t * sourceChannels_ + s]) > 1e-8f) needsQuad = true;
            }
            if (!needsQuad) continue;
            hilbert_.resize(hilbert_.size() + 1);
            hilbert_.back().prepare(taps);
            hilbertOut_[s].assign(maxFrames_, 0.0f);
        }
    }
    if (align_ && latencyFrames_ > 0) {
        for (size_t s = 0; s < sourceChannels_; ++s) delay_[s].assign(latencyFrames_, 0.0f);
    }

    // Bass management. The LFE row already exists in the matrix; when enabled,
    // the LFE channel is derived from a low-passed sum of the routed channels
    // instead of the matrix row. That keeps one owner for the LFE value.
    bass_ = false;
    lpState_ = 0.0;
    lfeGain_ = std::pow(10.0, matrix.lfeGainDb / 20.0);
    if (matrix.bassManagement == NAE_BASS_ON) {
        for (size_t t = 0; t < targetChannels_; ++t) {
            if (targetBits_[t] == NAE_CHANNEL_LFE) {
                bass_ = true;
                lfeIndex_ = t;
                break;
            }
        }
        if (bass_) {
            const double crossover = matrix.lfeCrossoverHz > 0 ? matrix.lfeCrossoverHz : 80.0;
            const double rate = matrix.sampleRate;
            if (rate > 0.0) {
                const double rc = 1.0 / (2.0 * 3.14159265358979323846 * crossover);
                const double dt = 1.0 / rate;
                lpAlpha_ = dt / (rc + dt);
            } else {
                bass_ = false;
            }
        }
    }
    ready_ = true;
    return true;
}

void RoutingProcessor::reset() {
    for (auto& h : hilbert_) h.reset();
    for (auto& q : iirFilters_) q.reset();
    for (auto& q : iirReal_) std::fill(q.begin(), q.end(), 0.0f);
    for (auto& q : iirImag_) std::fill(q.begin(), q.end(), 0.0f);
    lpState_ = 0.0;
    for (auto& d : delay_) std::fill(d.begin(), d.end(), 0.0f);
    for (auto& q : hilbertOut_) std::fill(q.begin(), q.end(), 0.0f);
    std::fill(delayPos_.begin(), delayPos_.end(), 0);
}

void RoutingProcessor::process(const float* in, size_t frames, float* out) {
    if (!ready_ || !in || !out || frames > maxFrames_) return;
    if (phase_) {
        scratch_.resize(maxFrames_);
        if (iir_) {
            size_t qi = 0;
            for (size_t s = 0; s < sourceChannels_; ++s) {
                if (iirReal_[s].empty()) continue;
                for (size_t f = 0; f < frames; ++f) scratch_[f] = in[f * sourceChannels_ + s];
                iirFilters_[qi++].process(scratch_.data(), iirReal_[s].data(), iirImag_[s].data(),
                                  frames);
            }
        } else {
            size_t hi = 0;
            for (size_t s = 0; s < sourceChannels_; ++s) {
                if (hilbertOut_[s].empty()) continue;
                for (size_t f = 0; f < frames; ++f) scratch_[f] = in[f * sourceChannels_ + s];
                hilbert_[hi++].process(scratch_.data(), hilbertOut_[s].data(), frames);
            }
        }
    }

    for (size_t f = 0; f < frames; ++f) {
        // Resolve each source's direct sample once per frame. The delay ring
        // used to be written inside the target loop, so target 0 read the
        // delayed value and every later target read the current one.
        for (size_t s = 0; s < sourceChannels_; ++s) {
            float direct = iir_ && !iirReal_[s].empty()
                               ? iirReal_[s][f]
                               : in[f * sourceChannels_ + s];
            if (align_ && latencyFrames_ > 0 && !delay_[s].empty()) {
                const size_t pos = delayPos_[s];
                direct = delay_[s][pos];
                delay_[s][pos] = in[f * sourceChannels_ + s];
                delayPos_[s] = (pos + 1) % latencyFrames_;
            }
            directFrame_[s] = direct;
        }
        for (size_t t = 0; t < targetChannels_; ++t) {
            double sum = 0.0;
            for (size_t s = 0; s < sourceChannels_; ++s) {
                const float direct = directFrame_[s];
                sum += direct * real_[t * sourceChannels_ + s];
                if (phase_) {
                    if (iir_ && !iirImag_[s].empty()) {
                        sum += iirImag_[s][f] * quad_[t * sourceChannels_ + s];
                    } else if (!hilbertOut_[s].empty()) {
                        sum += hilbertOut_[s][f] * quad_[t * sourceChannels_ + s];
                    }
                }
            }
            out[f * targetChannels_ + t] = static_cast<float>(sum);
        }
        if (bass_) {
            double sum = 0.0;
            for (size_t t = 0; t < targetChannels_; ++t) {
                if (t == lfeIndex_) continue;
                sum += out[f * targetChannels_ + t];
            }
            const double nonLfe = static_cast<double>(targetChannels_ > 1 ? targetChannels_ - 1 : 1);
            const double scaled = nonLfe > 0.0 ? sum / nonLfe : sum;
            lpState_ += lpAlpha_ * (scaled - lpState_);
            out[f * targetChannels_ + lfeIndex_] =
                static_cast<float>(lpState_ * lfeGain_);
        }
    }
}

}  // namespace neiro
