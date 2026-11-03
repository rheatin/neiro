// RBJ cookbook biquad coefficients, shared by the minimum-phase render path and
// the linear-phase FIR designer. One implementation: a second copy of these
// formulas is how a band ends up meaning two different things.

#pragma once

#include "nae.h"

#include <algorithm>
#include <cmath>

namespace neiro {

// One normalised biquad. Coefficients are computed when the cascade changes,
// never in the render loop.
struct EqBiquad {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// RBJ cookbook coefficients, normalised by a0. Returns false for a band that
// cannot be realised at this rate rather than silently moving it.
inline bool computeEqBiquad(const NAE_EqBand& band, double fs, EqBiquad* out) {
    if (!out || fs <= 0.0) return false;
    if (!std::isfinite(band.freqHz) || band.freqHz <= 0.0 || band.freqHz >= fs * 0.49) {
        return false;
    }
    if (!std::isfinite(band.q) || band.q <= 0.0) return false;
    // No clamping: the header promises that a band which cannot be realised is
    // rejected, and silently moving q makes the realised filter differ from the
    // requested one. The RBJ formulas are stable for any positive q in double.
    const double q = band.q;
    const double w0 = 2.0 * 3.14159265358979323846 * band.freqHz / fs;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    const double A = std::pow(10.0, band.gainDb / 40.0);

    switch (band.type) {
        case NAE_EQ_PEAKING:
            b0 = 1.0 + alpha * A;
            b1 = -2.0 * cosw0;
            b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha / A;
            break;
        case NAE_EQ_LOW_SHELF: {
            const double shelfAlpha = sinw0 / 2.0 * std::sqrt(2.0);
            const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * shelfAlpha;
            b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAAlpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
            b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAAlpha);
            a0 = (A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAAlpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0);
            a2 = (A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAAlpha;
            break;
        }
        case NAE_EQ_HIGH_SHELF: {
            const double shelfAlpha = sinw0 / 2.0 * std::sqrt(2.0);
            const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * shelfAlpha;
            b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAAlpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
            b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAAlpha);
            a0 = (A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAAlpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
            a2 = (A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAAlpha;
            break;
        }
        case NAE_EQ_HIGH_PASS:
            b0 = (1.0 + cosw0) / 2.0;
            b1 = -(1.0 + cosw0);
            b2 = (1.0 + cosw0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha;
            break;
        case NAE_EQ_LOW_PASS:
            b0 = (1.0 - cosw0) / 2.0;
            b1 = 1.0 - cosw0;
            b2 = (1.0 - cosw0) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha;
            break;
        case NAE_EQ_NOTCH:
            b0 = 1.0;
            b1 = -2.0 * cosw0;
            b2 = 1.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha;
            break;
        case NAE_EQ_ALL_PASS:
            b0 = 1.0 - alpha;
            b1 = -2.0 * cosw0;
            b2 = 1.0 + alpha;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 = 1.0 - alpha;
            break;
        default:
            return false;
    }

    out->b0 = b0 / a0;
    out->b1 = b1 / a0;
    out->b2 = b2 / a0;
    out->a1 = a1 / a0;
    out->a2 = a2 / a0;
    return true;
}

}  // namespace neiro
