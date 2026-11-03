// Dither: the last step before float samples become integer PCM.
//
// # Why it exists
//
// A resampler works in floating point and outputs more precision than any DAC
// can carry. Converting 24 valid bits down to 16 is quantisation, and plain
// quantisation is not just lossy — it is *correlated*. Once the signal sits
// below the step size, successive samples round to the same value and the error
// stops being noise. That correlated error is audible as distortion, and it is
// worse than the noise dither trades it for.
//
// Adding a small amount of shaped noise before rounding decorrelates the error.
// The noise is the point: it is inaudible, and what it buys is the removal of
// something that is not.
//
// # TPDF, and why not rectangular
//
// Rectangular dither (one uniform sample) leaves a tiny residual correlation.
// TPDF — two independent uniform samples summed — gives a triangular
// distribution, which is provably first-order decorrelated. It costs one extra
// random number and one addition.
//
// The trade is real and worth stating: dither adds noise at roughly the
// quantisation floor, about -96 dBFS for 16-bit. That is not free, and for a
// 24-in-32 endpoint it is not needed at all, which is why dithering is decided
// per-path rather than always on.
//
// # Modes
//
// Off, TPDF, rectangular and first-order noise-shaped are implemented. Noise
// shaping pushes quantisation error above the audible band at the cost of a
// raised noise floor; it is an explicit choice, never an Auto default.

#pragma once

#include <cstdint>

namespace neiro {

// TPDF dither, stateful so the random stream is continuous across blocks.
//
// A fresh generator per block would restart the sequence and produce a
// repeating pattern at block boundaries, which is precisely the correlation this
// is meant to remove.
class Dither {
  public:
    enum class Mode { Off, TPDF, Rectangular, NoiseShaped };
    // `validBits` is the number of bits that will actually carry signal, not the
    // container size. A 24-in-32 path has validBits == 24 and must not dither
    // to 24 unless the *device* is 24-bit.
    explicit Dither(uint32_t validBits);

    void set_valid_bits(uint32_t validBits);

    uint32_t valid_bits() const { return validBits_; }

    // True when dithering is actually needed and switched on.
    //
    // Required whenever the target depth is narrower than the precision the
    // pipeline carries. Dithering a path that is already wider than its output
    // would add noise that serves no purpose.
    bool active() const { return mode_ != Mode::Off && validBits_ < kInternalBits; }

    void set_mode(Mode mode) { mode_ = mode; }
    Mode mode() const { return mode_; }

    // Round `in` (nominal range -1..1) to an integer sample of `validBits`,
    // adding dither on the way.
    //
    // NOT const, and that is load-bearing: it advances the random stream. A
    // const version would either not compile or, if the state were mutable, look
    // re-entrant while silently producing a repeating sequence — the exact
    // correlation dither exists to remove.
    int32_t quantize(double in);

    // Scale factor from a normalised float to one LSB at the target depth.
    double lsb() const;

    void set_enabled(bool on) { mode_ = on ? Mode::TPDF : Mode::Off; }
    bool enabled() const { return mode_ != Mode::Off; }

    // The internal precision the pipeline carries before quantisation.
    // 24 bits is the working depth of the whole engine; anything narrower than
    // the target must be dithered.
    static constexpr uint32_t kInternalBits = 24;

  private:
    double uniform();  // [-1, 1)

    uint32_t validBits_ = 0;
    Mode mode_ = Mode::TPDF;
    double error1_ = 0.0;

    // xorshift64*, seeded per instance. Not cryptographic and not meant to be —
    // it needs to be fast, allocation-free and decorrelate, nothing more.
    uint64_t state_ = 0x9E3779B97F4A7C15ull;
};

}  // namespace neiro
