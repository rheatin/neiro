// ITU-R BS.1770-4 / EBU R128 loudness measurement.
//
// K-weighting is the standard two-stage filter (high shelf + RLB high-pass),
// designed from the analogue prototype for the actual sample rate rather than
// using the 48 kHz coefficients at every rate. Block loudness is
// -0.691 + 10 log10(weighted mean square) over 400 ms with 100 ms hop; the
// integrated value applies the absolute (-70 LUFS) and relative (-10 LU)
// gates. Loudness range follows EBU Tech 3342.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neiro {

class LoudnessMeter {
  public:
    struct Result {
        bool valid = false;
        /** Gated integrated loudness, LUFS. */
        double integratedLufs = 0.0;
        /** Loudest 400 ms block, LUFS. Ungated. */
        double momentaryMaxLufs = 0.0;
        /** Loudest 3 s window, LUFS. Ungated. */
        double shortTermMaxLufs = 0.0;
        /**
         * EBU Tech 3342 loudness range, LU.
         *
         * Only meaningful when `shortTermValid` is set: the range is computed
         * from 3 s windows, and a file shorter than 3 s has none.
         */
        double rangeLu = 0.0;
        /**
         * Whether a 3 s window existed. `shortTermMaxLufs` and `rangeLu` are
         * 0 when this is false, and 0 is not a measurement -- a caller that
         * draws them without checking this paints "0 LUFS" for a jingle.
         */
        bool shortTermValid = false;
        /** Number of blocks that passed both gates. */
        uint32_t gatedBlocks = 0;
        /** Number of 400 ms blocks measured. */
        uint32_t totalBlocks = 0;
    };

    bool configure(uint32_t channels, uint32_t sampleRate);
    void process(const float* interleaved, size_t frames, uint32_t channels);
    Result finish() const;

  private:
    struct Biquad {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        double tick(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0.0; }
    };

    uint32_t channels_ = 0;
    uint32_t sampleRate_ = 0;
    std::vector<Biquad> shelf_;
    std::vector<Biquad> highpass_;

    // 100 ms segments. `segmentEnergy_[c][k]` is the mean square of channel c
    // in segment k; `segmentAcc_` holds the partial segment being filled.
    std::vector<std::vector<double>> segmentEnergy_;
    std::vector<double> segmentAcc_;
    uint32_t segmentFill_ = 0;
    uint32_t segmentLength_ = 0;

    std::vector<double> channelWeight_;

    static constexpr uint32_t kSegmentMs = 100;
    static constexpr uint32_t kBlockSegments = 4;   // 400 ms
    static constexpr uint32_t kShortTermSegments = 30;  // 3 s
    static constexpr uint32_t kShortTermHop = 10;   // 1 s
};

}  // namespace neiro
