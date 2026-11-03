// 90-degree Hilbert FIR for the Lt/Rt routing matrix.
//
// Platform-neutral: no Windows, no device, no allocation in process(). The
// filter is an odd-length, windowed ideal Hilbert transformer. Its group delay
// is (taps - 1) / 2; the routing matrix delays direct paths by the same amount
// when alignPhaseLatency is set.
//
// Two implementations feed the same NAE_ComplexGain:
//   Quadrature      - windowed FIR Hilbert, constant group delay.
//   IirQuadrature   - frequency-warped parallel all-pass pair (Harris/Berdahl/
//                     Abel). Both outputs are filtered, and their phase differs
//                     by 90 degrees; there is no group delay to align.
//
// The IIR pole set was designed for 20 Hz to min(20 kHz, 0.42*fs) and measured
// 0.586 degrees worst-case at 32, 44.1, 48, 88.2, 96, 176.4 and 192 kHz. Its
// cost is 12 first-order sections per source channel with no window length.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace neiro {

class Quadrature {
  public:
    explicit Quadrature(uint32_t taps = 255);

    // Rebuild for `taps` (odd, >= 3). Returns false for an invalid size.
    bool prepare(uint32_t taps);
    void reset();

    // Process `frames` samples from `in` to `out`, both mono contiguous.
    // `out` is the Hilbert output at the same index as the filter's centre tap;
    // it is delayed by latency_frames() relative to the input.
    void process(const float* in, float* out, size_t frames);

    uint32_t taps() const { return taps_; }
    uint32_t latency_frames() const { return (taps_ - 1) / 2; }

  private:
    uint32_t taps_ = 0;
    std::vector<float> coeff_;
    std::vector<float> history_;
    size_t write_ = 0;
};

/**
 * Frequency-warped parallel all-pass Hilbert.
 *
 * The real output is chain A, the imaginary output is chain B, and the design
 * makes arg A - arg B = +90 degrees over the audio band. B lags A by 90
 * degrees, which is the same convention as the FIR (its response is -j), so a
 * quadrature gain keeps its sign when the mode changes.
 */
class IirQuadrature {
  public:
    bool configure(double sampleRate);

    // `realOut` is A(input); `imagOut` is B(input), 90 degrees behind A.
    void process(const float* in, float* realOut, float* imagOut, size_t frames);
    void reset();

    double highBandHz() const { return highBandHz_; }
    uint32_t sectionsPerPath() const { return static_cast<uint32_t>(chainA_.size()); }

  private:
    struct Section {
        double a = 0.0;
        double x1 = 0.0;
        double y1 = 0.0;
        // H(z) = (a - z^-1)/(1 - a z^-1), the bilinear image of the
        // first-order analogue all-pass used in the pole design.
        double tick(double x) {
            const double y = a * x - x1 + a * y1;
            x1 = x;
            y1 = y;
            return y;
        }
        void reset() { x1 = 0.0; y1 = 0.0; }
    };
    std::vector<Section> chainA_;
    std::vector<Section> chainB_;
    double highBandHz_ = 0.0;
};

}  // namespace neiro
