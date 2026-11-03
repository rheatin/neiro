// Characterises a decode path in the frequency domain.
//
// # Why
//
// The time-domain checks in `decode_check` cannot see the failure that matters for
// DSD. Aliasing from a decimation filter is invisible in a peak measurement and
// in a duration measurement: the output is the right length, the right level, and
// wrong. A bandwidth of ultrasonic noise folded down into the audible band is
// exactly the defect a HiFi player exists not to have, and it is precisely what a
// peak check reports as "fine".
//
// This also prints what the decode path *is*, because the codec's name does not
// describe it.
//
// # 这段注释原来写错了（已按实测更正）
//
// 原文是：「FFmpeg 的 `dsd_msbf` 产生 `AV_SAMPLE_FMT_DSD`——比特率上的1-bit 采样，
// PCM 转换发生在之后的 libswresample 实例里。那个滤波器就是 DSD 到 PCM 的全部设计。」
//
// 两处都不成立，都由 `dsd_probe` 实测推翻：
//
//   1. **这个 FFmpeg 构建里没有 `AV_SAMPLE_FMT_DSD` 这个符号。** 编译器直接报
//      "AV_SAMPLE_FMT_DSD was not declared"。所以这里不存在「打包的比特」这个中间
//      格式——名字里带 `dsd` 不代表它输出 DSD。
//   2. **解码器内部已经做了抽取滤波**，不是「输出比特、由 libswresample 转换」。
//      `dsd_probe` 解出同一段数据的浮点输出是 `min -0.069 max 0.055`，相邻样本平均
//      差 **0.0189**。原始 1-bit 展开必然出现 ±1.0，相邻样本平均差约 2.0；实测两个
//      量都差得极远，所以它是 2822400 bps 抽取到 352800 Hz（÷8）之后的平滑输出。
//      libswresample 那一步 `in_rate == out_rate == 352800`、flags 为 0，只做
//      fltp -> FLT 的格式转换，一点滤波都没做。
//
// 所以「DSD 到 PCM 的滤波器在解码器里，不在 swresample 里」。而它不可配置——
// 无法换成一个更好的滤波器，这正是自己写 DSD 解码器时唯一能改的地方。
//
// # What it measures
//
// Two modes.
//
// **Band survey** (default): RMS in dBFS across the audible band and the
// ultrasonic remainder. The 150 kHz-to-Nyquist band decides whether a 176.4 kHz
// intermediate rate is even meaningful -- if the decimation filter has already put
// its stopband down there, resampling to 176.4 first is free, and if it has not,
// the proposal is worse rather than better.
//
// **Tone probe** (`--tone hz`): the peak within a few bins of `hz`, the local noise
// floor around it, and both relative to a reference (`--ref-db`). This is the mode
// that can distinguish a folded input from the decoder's own noise, which a band
// average cannot: a spur sitting in a quiet band is averaged away, and a band that
// contains both a spur and noise reports a number belonging to neither.
//
// The peak and the floor are reported separately and deliberately. A single
// magnitude at the predicted alias frequency cannot distinguish "the filter folded
// the input" from "the decoder is noisy there" from "the window leaked", and those
// have opposite implications for whether a 176.4 kHz intermediate is viable.
//
// # Usage
//
//   dsd_spectrum <file> [--seconds N] [--tone hz] [--ref-db X] [--out raw-f32]

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "decoder.h"

namespace {

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

// In-place iterative radix-2 FFT. Length must be a power of two.
void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// A window is described by its coefficients but judged by what it actually
// computes. sum(w)/N is the coherent gain, which sets the amplitude scale, and
// sum(w^2)/N is the noise power gain, which sets how far a tone leaks into a
// quiet band. For a correct four-term Blackman-Harris they are 0.35875 and
// 0.2580; for Hann, 0.5 and 0.375.
//
// These exist because the window was wrong and nothing said so. The same fixture
// read -59.71 dBFS under Hann and -72.07 under Blackman-Harris -- 12.4 dB, where
// the coherent-gain difference accounts for 2.9. An unexplained 9.5 dB is a window
// that is not the one its name claims, and coefficients read off a table are not
// evidence that the code applies them.
void printWindowMoments(size_t n);

// Blackman-Harris, four-term.
//
//
// A window is described by its coefficients but judged by what it actually
// computes. sum(w)/N is the coherent gain, which sets the amplitude scale, and
// sum(w^2)/N is the noise power gain, which sets how far a tone leaks into a
// quiet band. For a correct four-term Blackman-Harris they are 0.35875 and
// 0.2580; for Hann, 0.5 and 0.375.
//
// These exist because the window was wrong and nothing said so. The same fixture
// read -59.71 dBFS under Hann and -72.07 under Blackman-Harris -- 12.4 dB, where
// the coherent-gain difference accounts for 2.9. An unexplained 9.5 dB is a window
// that is not the one its name claims, and coefficients read off a table are not
// evidence that the code applies them.
void printWindowMoments(size_t n);

// The floor at which float32 stops being able to represent anything.
//
// FFmpeg hands this decoder's output over as fltp, so every sample is a 32-bit
// float and the smallest non-zero magnitude available is about -144 dBFS. A
// measurement that lands near here has not measured a small signal, it has
// measured the absence of a representable one, and reporting it as a real
// rejection would be claiming precision the format cannot carry. The figure is
// printed alongside the results for exactly that reason.
constexpr double kFloat32FloorDb = -144.0;

// Blackman-Harris, four-term.
//
// Chosen over Hann for one specific reason: this measurement is asked to
// distinguish a rejection around -55 dBc from a floor near -140 dBFS, and that is
// an 85 dB question. A Hann window's sidelobes sit only 31 dB below its main lobe,
// so a strong tone 20 kHz away leaks into a quiet band and raises the apparent
// floor by tens of decibels -- which would make every rejection look worse than
// it is and the low ones look like noise.
//
// Blackman-Harris suppresses sidelobes near 92 dB, comfortably below the dynamic
// range being asked about, and the main lobe is 8 bins wide rather than 4. The
// wider lobe is the cost, and it is why the floor measurement excludes a window
// around the peak rather than merely stepping over it.
void applyWindow(std::vector<double>& x) {
    const size_t n = x.size();
    if (n < 4) return;
    const double k = 2.0 * 3.14159265358979323846 / static_cast<double>(n - 1);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i);
        const double w = 0.35875 - 0.48829 * std::cos(k * t) + 0.14128 * std::cos(2.0 * k * t) -
                         0.01168 * std::cos(3.0 * k * t);
        x[i] *= w;
    }
}

// The window's own first two moments, printed under NAE_TRACE.
//
// A window is described by its coefficients but judged by what it actually
// computes. sum(w)/N is the coherent gain, which sets the amplitude scale, and
// sum(w^2)/N is the noise power gain, which sets how far a tone leaks into a
// quiet band. For a correct four-term Blackman-Harris they are 0.35875 and
// 0.2580; for Hann, 0.5 and 0.375.
//
// These exist because the window was wrong and nothing said so. The same fixture
// read -59.71 dBFS under Hann and -72.07 under Blackman-Harris -- 12.4 dB, where
// the coherent-gain difference accounts for 2.9. An unexplained 9.5 dB means the
// window is not the one its name claims, and coefficients copied off a table are
// not evidence that the code applies them.
void printWindowMoments(size_t n) {
    std::vector<double> w(n, 1.0);
    applyWindow(w);
    double s1 = 0.0, s2 = 0.0;
    for (double v : w) { s1 += v; s2 += v * v; }
    std::fprintf(stderr,
                 "[dsd] window n=%zu  CG=%.6f (want 0.358750)  NPG=%.6f (want 0.258000)\n", n,
                 s1 / static_cast<double>(n), s2 / static_cast<double>(n));
}

double db(double linear) { return 20.0 * std::log10(linear > 1e-20 ? linear : 1e-20); }

// The peak within `halfWidth` bins of `hz`, and where it actually was.
//
// The frequency is returned as well as the magnitude, and the difference from
// what was asked for is printed. A tone that lands somewhere other than its
// predicted fold is either a bin-quantisation artefact or a real component that
// the prediction missed, and the two look identical in a magnitude alone -- which
// is how a measurement of the wrong thing can pass every threshold it was given.
struct Peak {
    double hz = 0.0;
    double amp = 0.0;
    double deltaBins = 0.0;
};

Peak findPeak(const std::vector<std::complex<double>>& spec, double hz, double binHz,
              int halfWidth, double scale) {
    const double centre = hz / binHz;
    const int lo = std::max(1, static_cast<int>(std::lround(centre)) - halfWidth);
    const int hi = std::min(static_cast<int>(spec.size() / 2) - 1,
                            static_cast<int>(std::lround(centre)) + halfWidth);
    Peak p;
    for (int k = lo; k <= hi; ++k) {
        const double m = std::abs(spec[k]) * scale;
        if (m > p.amp) { p.amp = m; p.hz = static_cast<double>(k) * binHz; }
    }
    p.deltaBins = (p.hz - hz) / binHz;
    return p;
}

// Median magnitude in a band, with the peak's own neighbourhood removed.
//
// Two things have to be excluded, and the second is the one that is easy to miss:
//
//   * the peak itself, which would otherwise define the floor;
//   * the window either side of it, because a Blackman-Harris main lobe is 8 bins
//     wide and its first sidelobe is only 92 dB down. Measuring the floor with the
//     lobe still in it reports the tone's own leakage, and "peak above floor" then
//     comes out smaller than the truth -- quietly, in the direction that makes a
//     rejection look better than it is.
//
// Median rather than mean: one spur in the band would move a mean, and a spur is
// exactly what a floor is supposed to be ignoring.
double bandMedian(const std::vector<std::complex<double>>& spec, double loHz, double hiHz,
                  double binHz, double scale, double excludeLoHz, double excludeHiHz) {
    std::vector<double> mags;
    for (size_t k = 1; k < spec.size() / 2; ++k) {
        const double f = static_cast<double>(k) * binHz;
        if (f < loHz || f >= hiHz) continue;
        if (f >= excludeLoHz && f <= excludeHiHz) continue;
        mags.push_back(std::abs(spec[k]) * scale);
    }
    if (mags.empty()) return 0.0;
    std::sort(mags.begin(), mags.end());
    return mags[mags.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dsd_spectrum <file> [--seconds N] [--out raw-f32]\n");
        return 2;
    }
    const std::string path = argv[1];
    double seconds = 2.0;
    const char* outPath = nullptr;
    double toneHz = 0.0;
    double refDb = 0.0;
    int fftSize = 0;
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0) seconds = std::atof(argv[i + 1]);
        if (std::strcmp(argv[i], "--out") == 0) outPath = argv[i + 1];
        if (std::strcmp(argv[i], "--tone") == 0) toneHz = std::atof(argv[i + 1]);
        if (std::strcmp(argv[i], "--ref-db") == 0) refDb = std::atof(argv[i + 1]);
        if (std::strcmp(argv[i], "--fft") == 0) fftSize = std::atoi(argv[i + 1]);
    }

    neiro::Decoder dec;
    std::string err;
    if (!dec.open(path, &err)) {
        std::printf("open failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("\n== decode path ==\n%s\n", dec.describe().c_str());

    const auto& info = dec.info();
    const double rate = info.sample_rate;
    const double nyquist = rate / 2.0;
    std::printf("  output rate       %.0f Hz, Nyquist %.1f kHz\n", rate, nyquist / 1000.0);
    std::printf("  duration          %.2f s\n\n", info.duration_ms / 1000.0);

    // One FFT-sized window, taken from the middle of the file. The start of a
    // track is often silence and the end may be a fade; the middle is the one
    // place a whole window of programme material is likely to be.
    //
    // 2^19 by default when probing a tone, because the whole point is to resolve a
    // peak from its neighbours, and a 16 k window at 352.8 kHz has 22 Hz bins --
    // wide enough that a 20 kHz-spaced test set would be measured inside its own
    // main lobe.
    const size_t N = fftSize > 0 ? static_cast<size_t>(fftSize) : (toneHz > 0 ? 524288u : 16384u);
    const size_t ch = info.out_channels;
    const size_t want = static_cast<size_t>(seconds * rate);
    std::vector<float> all;
    std::vector<float> block;
    while (all.size() < want * ch) {
        block.clear();
        if (!dec.read(&block, &err)) break;
        all.insert(all.end(), block.begin(), block.end());
        if (all.size() > (want + N * ch) * 2) break;  // enough for a mid-file window
    }
    if (all.size() < N * ch) {
        std::printf("not enough audio for a %zu-frame window (have %zu)\n", N, all.size() / ch);
        return 1;
    }

    if (outPath) {
        FILE* f = std::fopen(outPath, "wb");
        if (f) {
            std::fwrite(all.data(), sizeof(float), all.size(), f);
            std::fclose(f);
            std::printf("wrote %s (%zu samples)\n\n", outPath, all.size());
        }
    }

    // Take the window from the middle.
    const size_t mid = (all.size() / ch - N) / 2;

    // Refuse to characterise silence -- unless silence is what was asked for.
    //
    // The first run of this printed "0-20 kHz: -149 dBFS" and read as a dramatic
    // finding about the DSD filter. It was a fade-in: the window landed on digital
    // silence, and a spectrum of silence is a spectrum of nothing. A measurement
    // tool has to establish that its input is the thing it was asked to measure.
    //
    // The exception is a deliberately silent fixture, where a low reading is the
    // measurement rather than a mistake: it is the baseline every other number is
    // compared against. The caller says so by passing --allow-silent.
    double windowPeak = 0.0;
    size_t nanCount = 0, infCount = 0;
    double lo = 1e300, hi = -1e300;
    for (size_t i = 0; i < N; ++i) {
        const double v = static_cast<double>(all[(mid + i) * ch]);
        // Counted explicitly rather than inferred. `std::max` silently ignores a
        // NaN -- `a < NaN` is false, so it returns `a` -- which means a peak check
        // over data containing NaN reports a perfectly healthy number while the
        // FFT turns the whole array into NaN and every band reads as the floor.
        // That is exactly what happened, and it cost a long time to find because
        // the time-domain check said everything was fine.
        if (std::isnan(v)) { ++nanCount; continue; }
        if (std::isinf(v)) { ++infCount; continue; }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
        windowPeak = std::max(windowPeak, std::fabs(v));
    }
    std::printf("  samples: min %.6g  max %.6g  NaN %zu  Inf %zu  (of %zu)\n", lo, hi, nanCount,
                infCount, N);
    const bool allowSilent = outPath != nullptr && std::getenv("NEIRO_ALLOW_SILENT") != nullptr;
    std::printf("  window at %.2f s, peak %.1f dBFS%s\n", static_cast<double>(mid) / rate,
                db(windowPeak),
                (windowPeak < 0.01 && !allowSilent) ? "   <-- SILENT, not a valid window" : "");
    if (windowPeak < 0.01 && !allowSilent) {
        std::printf("The window is silent. Re-run with --seconds large enough to reach "
                    "programme material.\n");
        return 1;
    }

    std::vector<std::complex<double>> spec(N);
    for (size_t i = 0; i < N; ++i) spec[i] = std::complex<double>(all[(mid + i) * ch], 0.0);

    // Remove DC before windowing, or a large offset in the signal leaks across the
    // whole spectrum and flattens every band being measured.
    double mean = 0.0;
    for (size_t i = 0; i < N; ++i) mean += spec[i].real();
    mean /= static_cast<double>(N);
    for (size_t i = 0; i < N; ++i) spec[i] -= mean;

    std::vector<double> re(N), im(N);
    for (size_t i = 0; i < N; ++i) {
        re[i] = spec[i].real();
        im[i] = spec[i].imag();
    }
    applyWindow(re);
    applyWindow(im);
    if (getenv("NAE_TRACE")) printWindowMoments(N);
    for (size_t i = 0; i < N; ++i) spec[i] = std::complex<double>(re[i], im[i]);

    fft(spec);
    // Scale so a full-scale sine reads 0 dBFS. The Hann coherent gain is 0.5 and
    // the single-sided spectrum needs a factor of 2, hence 4/N.
    const double scale = 4.0 / static_cast<double>(N);

    const double binHz = rate / static_cast<double>(N);

    if (toneHz > 0.0) {
        // The alias measurement.
        //
        // What the fixture's own collision check does and does not cover, because
        // it decides how this number may be read: the check enumerates the *discrete*
        // harmonics of the test tone and proves none of them folds onto the target.
        // A sigma-delta modulator's quantisation noise is not discrete, it is a
        // continuous shaped plume, and decimation folds that too. For the 220 kHz
        // fixture the plume peaks near 1180 kHz, which folds to 121.6 kHz -- 11 kHz
        // from the 132.8 kHz target, harmless, but the same mechanism would not be
        // harmless at 1 kHz. So "no collision" means no known line problem, and the
        // part of the floor attributable to folded noise is what the local
        // measurement below is for.
        constexpr int kHalfWidth = 8;      // covers a Blackman-Harris main lobe
        constexpr int kExclusionBins = 40; // lobe plus first sidelobes
        const double exclusionHz = kExclusionBins * binHz;

        const Peak peak = findPeak(spec, toneHz, binHz, kHalfWidth, scale);
        const double floor = bandMedian(spec, toneHz - 1000.0, toneHz + 1000.0, binHz, scale,
                                        toneHz - exclusionHz, toneHz + exclusionHz);

        std::printf("== tone probe ==\n");
        std::printf("  expected          %.2f Hz\n", toneHz);
        std::printf("  measured          %.2f Hz   (%+.2f bins, bin = %.3f Hz)\n", peak.hz,
                    peak.deltaBins, binHz);
        std::printf("  peak              %8.2f dBFS\n", db(peak.amp));
        std::printf("  local floor       %8.2f dBFS   (median of %.0f-%.0f Hz, +/-%d bins around "
                    "the peak excluded)\n",
                    db(floor), toneHz - 1000.0, toneHz + 1000.0, kExclusionBins);
        std::printf("  peak above floor  %8.2f dB\n", db(peak.amp) - db(floor));
        std::printf("  float32 floor     %8.2f dBFS   (a result at this level is below what the "
                    "format can carry)\n", kFloat32FloorDb);
        if (refDb != 0.0) {
            std::printf("  dBc vs reference  %8.2f dB   (reference %+.2f dBFS)\n",
                        db(peak.amp) - refDb, refDb);
        }

        // A peak that is not where it was predicted is not a smaller version of the
        // expected result, it is a different measurement. Failing here rather than
        // reporting the magnitude is the point: a confident number for a peak that
        // moved 40 bins is how a wrong thing gets recorded as a right one.
        if (std::fabs(peak.deltaBins) > 2.0) {
            std::printf(
                "\n  FAILED: the peak is %.2f bins from the predicted fold, which is more than\n"
                "  bin quantisation can explain. Either the fold prediction is wrong for this\n"
                "  case, or the strongest component here is not the fold of the input at all\n"
                "  -- an unrelated spur would do it. Not reporting a dBc for this.\n",
                peak.deltaBins);
            return 2;
        }
        return 0;
    }

    std::printf("== band RMS (window %zu, %zu frames = %.1f ms) ==\n", N, N, N * 1000.0 / rate);

    struct Band { const char* label; double lo; double hi; };
    const std::vector<Band> bands = {
        {"0-20 kHz     (audible)", 0.0, 20000.0},
        {"20-80 kHz               ", 20000.0, 80000.0},
        {"80-150 kHz              ", 80000.0, 150000.0},
        {"150 kHz-Nyquist         ", 150000.0, nyquist},
    };
    for (const auto& b : bands) {
        double sum = 0.0;
        size_t count = 0;
        for (size_t k = 1; k < N / 2; ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f < b.lo || f >= std::min(b.hi, nyquist)) continue;
            const double m = std::abs(spec[k]) * scale;
            sum += m * m;
            ++count;
        }
        const double rms = count ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
        std::printf("  %s  %7.2f dBFS   (%zu bins)\n", b.label, db(rms), count);
    }

    // Coarse spectrum, because a band average will hide a narrow spur.
    std::printf("\n== coarse spectrum ==\n");
    const int bars = 48;
    double peakDb = -200.0;
    std::vector<double> barDb(bars, -200.0);
    for (int b = 0; b < bars; ++b) {
        const double f0 = nyquist * b / bars, f1 = nyquist * (b + 1) / bars;
        double sum = 0.0;
        size_t count = 0;
        for (size_t k = 1; k < N / 2; ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f < f0 || f >= f1) continue;
            const double m = std::abs(spec[k]) * scale;
            sum += m * m;
            ++count;
        }
        barDb[b] = count ? db(std::sqrt(sum / static_cast<double>(count))) : -200.0;
        peakDb = std::max(peakDb, barDb[b]);
    }
    for (int b = 0; b < bars; ++b) {
        const double f0 = nyquist * b / bars, f1 = nyquist * (b + 1) / bars;
        const int width = static_cast<int>(std::lround((barDb[b] - peakDb) / 2.0));
        std::printf("  %6.1f-%6.1f kHz %7.2f dB |%s\n", f0 / 1000.0, f1 / 1000.0, barDb[b],
                    std::string(width > 0 ? width : 0, '#').c_str());
    }
    std::printf("\n");
    return 0;
}
