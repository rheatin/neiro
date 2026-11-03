// Verifies dither does the one job it is for.
//
// Dither is random, so nothing here can check exact sample values. What it can
// check is the property that distinguishes dither from plain rounding:
//
//   * a constant low-level signal must produce a *variety* of output values,
//     because that is what decorrelates the error
//   * without dither the same signal produces one repeated value, which is the
//     distortion dither exists to remove
//   * the added noise must sit at the quantisation floor and no higher
//   * it must not add a bias, or it becomes a DC offset
//
// A dither implementation that is subtly wrong still sounds like music, which is
// why this measures rather than listens.
//
// Usage: dither_check.exe

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <map>
#include <set>
#include <vector>

// `wasapi_compat.hpp` declares interfaces against the Windows audio headers, so
// windows.h has to arrive first. MinGW's audioclient.h uses REFERENCE_TIME without
// including its own definition.
#include <windows.h>

#include "dither.h"
#include "wasapi_compat.hpp"

using neiro::wasapi::placeInContainer;

namespace {

int g_failures = 0;

void check(const char* label, bool ok, const std::string& detail) {
    if (!ok) ++g_failures;
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

std::string fmt(const char* format, ...) {
    char buf[192];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return buf;
}

// Quantise without dither, for the "what we are avoiding" comparison. Same
// rounding rule as the Dither class, minus the noise.
int32_t roundOnly(double in, uint32_t validBits) {
    if (in > 1.0) in = 1.0;
    if (in < -1.0) in = -1.0;
    // Signed PCM full scale, matching Dither: 2^(bits-1) - 1, not (1<<bits)-1.
    const double full = static_cast<double>((uint64_t{1} << (validBits - 1)) - 1);
    const double scaled = in * full;
    const double r = (scaled >= 0.0) ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
    return static_cast<int32_t>(std::max(-full, std::min(full, r)));
}

}  // namespace

int main() {
    std::printf("== 1. dither decorrelates where plain rounding does not ==\n");

    // A constant signal sitting between two representable levels — exactly the
    // case where plain quantisation collapses to a single repeated value.
    constexpr double kTinySignal = 0.5 / 32767.0;  // half an LSB at 16 bit
    constexpr uint32_t kBits = 16;
    const size_t kN = 4096;

    {
        std::set<int32_t> distinct;
        for (size_t i = 0; i < kN; ++i) {
            distinct.insert(roundOnly(kTinySignal, kBits));
        }
        check("plain rounding collapses to one value", distinct.size() == 1,
              fmt("%.0f distinct value(s)", (double)distinct.size()));
    }

    {
        neiro::Dither d(kBits);
        std::set<int32_t> distinct;
        std::vector<int32_t> values;
        for (size_t i = 0; i < kN; ++i) {
            const int32_t v = d.quantize(kTinySignal);
            distinct.insert(v);
            values.push_back(v);
        }

        // Two distinct values is the CORRECT answer here, and the exact
        // reason this test is worth having.
        //
        // The signal sits at exactly 0.5 LSB — a perfect tie. Plain
        // rounding resolves the tie the same way every time, which is the
        // correlated error. TPDF noise spans one LSB peak-to-peak, so the
        // sum lands anywhere in [-0.5, +1.5) and rounds to either 0 or 1.
        //
        // The dither turns a biased coin into a fair one. An earlier version
        // of this check demanded three or more distinct values, which would
        // have failed the correct implementation.
        check("dithered rounding resolves the tie into two values",
              distinct.size() == 2,
              fmt("%.0f distinct values", (double)distinct.size()));

        // And the tie must be resolved *fairly*: the mean of the dithered
        // output converges on the true scaled value (0.5), not on the
        // rounded one (1). Dither's entire purpose is that shift;
        // asserting it against the rounded value would assert the opposite.
        int64_t sum = 0;
        for (int32_t v : values) sum += v;
        const double mean = static_cast<double>(sum) / static_cast<double>(values.size());
        const double trueScaled = kTinySignal * 32767.0;  // exactly 0.5
        check("mean converges on the true value, not the rounded one",
              std::abs(mean - trueScaled) < 0.10,
              fmt("mean %.4f, true %.4f, rounded %d", mean, trueScaled,
                  roundOnly(kTinySignal, kBits)));
    }

    std::printf("\n== 2. added noise sits at the quantisation floor ==\n");
    {
        // Measure the error of a full-scale sine with and without dither. The
        // dithered error should be close to the TPDF ideal of one LSB rms and
        // definitely not orders of magnitude above it.
        constexpr double kRms = 0.5;  // full-scale sine
        const size_t kSamples = 65536;
        neiro::Dither on(kBits);
        neiro::Dither off(kBits);
        off.set_enabled(false);

        double sumOn = 0.0, sumOn2 = 0.0, sumOff = 0.0, sumOff2 = 0.0;
        for (size_t i = 0; i < kSamples; ++i) {
            const double t = static_cast<double>(i) / 48000.0;
            const double s = kRms * std::sin(2.0 * 3.14159265358979323846 * 440.0 * t);
            const double full = static_cast<double>((uint64_t{1} << (kBits - 1)) - 1);
            const double eOn = (on.quantize(s) / full) - s;
            const double eOff = (static_cast<double>(roundOnly(s, kBits)) / full) - s;
            sumOn += eOn;  sumOn2 += eOn * eOn;
            sumOff += eOff; sumOff2 += eOff * eOff;
        }
        const double rmsOn = std::sqrt(sumOn2 / kSamples);
        const double rmsOff = std::sqrt(sumOff2 / kSamples);
        const double meanOn = sumOn / kSamples;
        const double lsb = 1.0 / 32767.0;  // signed 16-bit full scale

        std::printf("  rms error: dithered %.2e, plain %.2e (1 LSB = %.2e)\n",
                    rmsOn, rmsOff, lsb);
        check("dithered error is within a small multiple of 1 LSB",
              rmsOn < lsb * 1.2, fmt("%.2e vs 1 LSB %.2e", rmsOn, lsb));
        check("dithered error is not *below* plain rounding by much",
              rmsOn > rmsOff * 0.5, fmt("%.2e vs %.2e", rmsOn, rmsOff));
        check("dithered error has no DC component", std::abs(meanOn) < lsb * 0.1,
              fmt("mean %.2e", meanOn));
    }

    std::printf("\n== 3. dither is skipped when the path is already wide ==\n");
    {
        neiro::Dither wide(32);
        check("inactive at 32 bits", wide.active() == false, "");
        neiro::Dither narrow(16);
        check("active at 16 bits", narrow.active() == true, "");
        neiro::Dither edge(neiro::Dither::kInternalBits);
        check("inactive at exactly the internal depth", edge.active() == false, "");

        // An inactive dither must be bit-identical to plain rounding, or
        // "automatic" would quietly alter every wide path.
        neiro::Dither d(32);
        bool identical = true;
        for (size_t i = 0; i < 1024; ++i) {
            const double s = 0.3 * std::sin(static_cast<double>(i) * 0.01);
            const double full = 2147483647.0;  // signed 32-bit full scale, 2^31 - 1
            const int64_t mine = d.quantize(s);
            const int64_t ref = static_cast<int64_t>(std::llround(s * full));
            if (mine != ref) { identical = false; break; }
        }
        check("inactive dither is bit-identical to plain rounding", identical, "");
    }

    std::printf("\n== 4. output never leaves the target range ==\n");
    {
        neiro::Dither d(16);
        bool inRange = true;
        const double full = 32767.0;
        // Overdrive well past full scale, with dither active, and confirm the
        // clamp holds. Dither pushing a sample outside the range is how a
        // resampler ends up with a click at the track boundary.
        for (size_t i = 0; i < 8192; ++i) {
            const double s = 1.0 + 0.5 * std::sin(static_cast<double>(i) * 0.3);
            const int32_t v = d.quantize(s);
            if (v > static_cast<int32_t>(full) || v < -static_cast<int32_t>(full)) {
                inRange = false;
                break;
            }
        }
        check("clamped to the target range under overdrive", inRange, "");
    }

    std::printf("\n== 5. the random stream does not repeat across blocks ==\n");
    {
        // A dither seeded per block would restart and produce an audible
        // pattern at block boundaries — the correlation it exists to remove.
        neiro::Dither d(16);
        std::vector<int32_t> first, second;
        for (size_t i = 0; i < 512; ++i) first.push_back(d.quantize(kTinySignal));
        for (size_t i = 0; i < 512; ++i) second.push_back(d.quantize(kTinySignal));
        bool same = true;
        for (size_t i = 0; i < 512; ++i) {
            if (first[i] != second[i]) { same = false; break; }
        }
        check("consecutive blocks differ", !same, "");
    }

    // ---- placing the sample in the container ----
    //
    // This is where the quiet bug lived, and it belonged in this file because the
    // dither already produces the right *number* -- `quantize` at 24 valid bits
    // returns +-8388607, correctly scaled. The fault was entirely in how that
    // number became bytes.
    //
    // A 24-in-32 format means 24 significant bits at the top of a 32-bit word.
    // Writing the value straight through leaves it at the bottom, and the device
    // reads a waveform that is intact and eight bits low: 256x too quiet. Measured
    // as 0.9194 out of the fifo becoming 0.0036 at the device, which is why it
    // presented as "some songs are silent" rather than as an arithmetic mistake.
    {
        std::printf("\n-- sample placement in the container --\n");

        // 16-in-16 and 24-in-24: nothing to do, and these must stay untouched.
        check("16-in-16 is unchanged", placeInContainer(1234, 16, 16) == 1234u, "");
        check("24-in-24 is unchanged", placeInContainer(0x7FFFFF, 24, 24) == 0x7FFFFFu, "");

        // 24-in-32: the whole point. Full scale becomes 0x7FFFFF00.
        check("24-in-32 full scale is left-justified",
              placeInContainer(0x7FFFFF, 32, 24) == 0x7FFFFF00u, "");
        check("24-in-32 silence is still silence",
              placeInContainer(0, 32, 24) == 0u, "");
        check("24-in-32 preserves the sign bit",
              placeInContainer(-1, 32, 24) == 0xFFFFFF00u, "");
        check("24-in-32 moves exactly 8 bits",
              placeInContainer(0x010000, 32, 24) == (0x010000u << 8), "");

        // 16-in-32, which a 16-bit device with a 32-bit container would present.
        check("16-in-32 full scale is left-justified",
              placeInContainer(0x7FFF, 32, 16) == 0x7FFF0000u, "");

        // 32-in-32 is the control: a full-width container must not be shifted,
        // because shifting it would push every sample out of the word entirely.
        check("32-in-32 is not shifted",
              placeInContainer(0x7FFFFFFF, 32, 32) == 0x7FFFFFFFu, "");
        check("32-in-32 preserves negative full scale",
              placeInContainer(static_cast<int32_t>(0x80000000u), 32, 32) == 0x80000000u, "");

        // A zero valid-bits field must not produce a shift by 32, which is
        // undefined. `readNativeFormat` can legitimately report 0.
        check("a zero valid-bits field is refused, not shifted",
              placeInContainer(1234, 32, 0) == 1234u, "");
    }

    std::printf("\n== 2. explicit dither modes ==\n");
    {
        neiro::Dither off(16);
        off.set_mode(neiro::Dither::Mode::Off);
        neiro::Dither tpdf(16);
        tpdf.set_mode(neiro::Dither::Mode::TPDF);
        neiro::Dither rect(16);
        rect.set_mode(neiro::Dither::Mode::Rectangular);
        neiro::Dither shaped(16);
        shaped.set_mode(neiro::Dither::Mode::NoiseShaped);
        std::set<int32_t> offValues, tpdfValues, rectValues, shapedValues;
        for (size_t i = 0; i < 4096; ++i) {
            offValues.insert(off.quantize(kTinySignal));
            tpdfValues.insert(tpdf.quantize(kTinySignal));
            rectValues.insert(rect.quantize(kTinySignal));
            shapedValues.insert(shaped.quantize(kTinySignal));
        }
        check("off rounds a half-LSB signal to one value",
              offValues.size() == 1, fmt("%zu values", offValues.size()));
        check("TPDF produces several values", tpdfValues.size() > 1,
              fmt("%zu values", tpdfValues.size()));
        check("rectangular produces several values", rectValues.size() > 1,
              fmt("%zu values", rectValues.size()));
        check("noise-shaped produces several values", shapedValues.size() > 1,
              fmt("%zu values", shapedValues.size()));
        check("off is not active", !off.active(), "");
        check("TPDF is active at 16 bit", tpdf.active(), "");
        check("rectangular is active at 16 bit", rect.active(), "");
        check("noise-shaped is active at 16 bit", shaped.active(), "");
    }

    std::printf("\n%s\n", g_failures == 0 ? "All dither checks passed."
                                          : "Some dither checks FAILED.");
    return g_failures == 0 ? 0 : 1;
}
