// True-peak-ish limiter for the clip guard policy.
//
// Platform-neutral. The render loop calls process() on interleaved frames; no
// allocation happens inside process().
//
// This is a feed-forward limiter with lookahead: the gain envelope is computed
// from the incoming peak and applied to the delayed signal, so a peak that needs
// reduction reaches the output already attenuated.
//
// True peak: the detector oversamples by 4x with a windowed-sinc polyphase FIR
// before taking the peak, so a peak between samples is seen and reduced. Cubic
// interpolation is not enough: at fs/4 it reconstructs the samples, not the
// waveform. The ceiling is a dBTP value; 0 dBTP is full scale.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neiro {

class ClipGuard {
  public:
    // Returns false for invalid settings (non-finite or out-of-range values).
    bool configure(uint32_t channels, uint32_t sampleRate, double ceilingDbTp,
                   double lookaheadMs, double releaseMs);
    bool active() const { return active_; }
    void set_active(bool on) { active_ = on; }
    void set_true_peak(bool on) { truePeak_ = on; }
    /// The delay between detection and output. Reported in the signal path.
    size_t lookahead_frames() const { return lookahead_; }
    void reset();

    // In-place, interleaved. `frames` may be any size.
    void process(float* samples, size_t frames);

    // Current gain reduction in dB (0 = none). For telemetry.
    double reduction_db() const;

  private:
    double peak4x(const float* frame, uint32_t channels);
    bool active_ = false;
    uint32_t channels_ = 0;
    bool truePeak_ = true;
    uint32_t rate_ = 0;
    double ceiling_ = 1.0;
    size_t lookahead_ = 0;
    double releaseRate_ = 1.0;
    /// Samples left before release may move the gain after a reduction. The
    /// peak that caused the reduction is still in the delay line for the whole
    /// lookahead, so releasing earlier lets it through.
    uint32_t hold_ = 0;
    double gain_ = 1.0;
    std::vector<float> delay_;
    // Polyphase 4x interpolation: kTaps per phase, 3 fractional phases.
    static constexpr uint32_t kPhases = 4;
    static constexpr uint32_t kTapsPerPhase = 16;
    std::vector<double> coeff_;  // (kPhases-1) * kTapsPerPhase
    std::vector<std::vector<double>> history_;  // per channel ring
    std::vector<size_t> historyPos_;
    size_t write_ = 0;
};

}  // namespace neiro
