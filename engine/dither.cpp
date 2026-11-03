#include "dither.h"

#include <cmath>

namespace neiro {

namespace {

// Full-scale magnitude for *signed* PCM at a given depth.
//
// The obvious expression, (1 << bits) - 1, is the UNSIGNED maximum: 65535 at 16
// bits, 4294967295 at 32. Signed PCM peaks at 2^(bits-1) - 1, so using the
// unsigned form lets the quantiser emit samples twice the device can represent
// — the top half of the range simply clips at the DAC, which is the harshest
// possible distortion and produces a click on every track boundary where the
// dither pushes a full-scale sample over.
//
// The same mistake makes 32-bit output unrepresentable in the int32_t return
// type, which is a second way it shows up.
constexpr double fullScale(uint32_t validBits) {
    return static_cast<double>((uint64_t{1} << (validBits - 1)) - 1);
}

}  // namespace

Dither::Dither(uint32_t validBits) : validBits_(validBits) {
    if (validBits_ == 0) validBits_ = 1;
    if (validBits_ > 32) validBits_ = 32;
}

void Dither::set_valid_bits(uint32_t validBits) {
    validBits_ = validBits ? validBits : 1;
    if (validBits_ > 32) validBits_ = 32;
}

double Dither::lsb() const {
    // One step of the target depth in the normalised [-1, 1] domain.
    return 1.0 / fullScale(validBits_);
}

double Dither::uniform() {
    // xorshift64*: three shifts, no multiply, no division, no allocation.
    // Adequate for audio dither — the requirement is that successive values are
    // not correlated in a way the ear can find, not unpredictability.
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    const uint64_t v = state_ * 0x2545F4914F6CDD1Dull;
    // Map the top 53 bits to [-1, 1). Using the high bits because xorshift's
    // low bits are its weakest.
    const double unit = static_cast<double>(v >> 11) / static_cast<double>(1ull << 53);
    return unit * 2.0 - 1.0;
}

int32_t Dither::quantize(double in) {
    if (in > 1.0) in = 1.0;
    if (in < -1.0) in = -1.0;

    const double full = fullScale(validBits_);
    double scaled = in * full;

    if (active()) {
        if (mode_ == Mode::Rectangular) {
            // One uniform sample. Retained as an explicit choice, not a default:
            // it leaves a small residual correlation.
            scaled += uniform() * 0.5;
        } else {
            // TPDF: the sum of two independent uniforms is triangular, which is
            // what decorrelates the quantisation error.
            const double tpdf = (uniform() + uniform()) * 0.5;
            scaled += tpdf;
            if (mode_ == Mode::NoiseShaped) {
                // First-order error feedback: subtract the previous quantisation
                // error, moving noise toward high frequencies. The previous error
                // is in the same LSB-scaled domain as `scaled`.
                scaled -= error1_;
            }
        }
    }

    // Round half away from zero, then clamp. std::llround would do, but being
    // explicit keeps the behaviour identical on every toolchain we build with.
    const double rounded = (scaled >= 0.0) ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    if (active() && mode_ == Mode::NoiseShaped) {
        error1_ = rounded - scaled;
    }

    // Clamp to the signed range. Note the asymmetry: negative full scale is one
    // larger in magnitude (-32768 at 16 bit) and int32_t holds it, so the
    // negative bound is the power of two, not the same expression.
    const double negBound = -static_cast<double>(uint64_t{1} << (validBits_ - 1));
    if (rounded > full) return static_cast<int32_t>(full);
    if (rounded < negBound) return static_cast<int32_t>(negBound);
    return static_cast<int32_t>(rounded);
}

}  // namespace neiro
