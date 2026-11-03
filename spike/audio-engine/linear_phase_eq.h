// Linear-phase, per-channel equaliser.
//
// The minimum-phase path runs RBJ biquads per sample. This path builds one
// symmetric FIR per channel from that channel's band set, then convolves with a
// uniform-partitioned FFT. The FIR is designed by sampling the cascade's
// magnitude response and windowing its zero-phase impulse response, so the
// phase is linear and the group delay is the same for every frequency.
//
// All channels are delayed by the same amount, including channels with no
// bands (their FIR is a centred delta), so a per-channel setting cannot
// misalign the image.
//
// The FIR length follows the sample rate: a fixed 2048 taps is 42.7 ms at
// 48 kHz but only 10.7 ms at 192 kHz, and a 1 kHz Q=1 band was measured 1.26 dB
// off at the higher rate. The design now keeps roughly 50 Hz of bin spacing,
// clamped to 2048..8192 taps.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace neiro {

struct LinearPhaseBandSpec {
    int32_t type = 0;  // NAE_EQ_* value
    double freqHz = 1000.0;
    double gainDb = 0.0;
    double q = 0.707;
};

class LinearPhaseEq {
  public:
    LinearPhaseEq();
    ~LinearPhaseEq();
    LinearPhaseEq(LinearPhaseEq&&) noexcept;
    LinearPhaseEq& operator=(LinearPhaseEq&&) noexcept;

    // `channelBands[c]` is the list of bands that apply to channel c.
    // `preamp` is a flat linear gain applied after convolution.
    // Returns false for a configuration that cannot be realised.
    bool configure(uint32_t channels, uint32_t sampleRate,
                   const std::vector<std::vector<LinearPhaseBandSpec>>& channelBands,
                   double preamp);

    // In place, interleaved. Output is delayed by `latencyFrames()`.
    void process(float* interleaved, size_t frames, uint32_t channels);
    void reset();

    static constexpr size_t blockSize() { return 256; }
    /** FIR length chosen for this sample rate; a power of two. */
    size_t taps() const;
    /** FIR group delay plus the block the partitioned convolver holds back. */
    size_t latencyFrames() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neiro
