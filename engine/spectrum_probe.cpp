// Reads the engine's output spectrum tap, and checks it against a known signal.
//
// # Why this is an instrument and not a screenshot
//
// The tap was added because the player had no data source for its visualiser on
// the exclusive route: the samples never enter the webview, so there was nothing
// to draw. That is a reason it is *needed*, and no evidence that it is *right*.
//
// A spectrum display that is fed the wrong window, the wrong channel, the wrong
// scaling or the wrong rate will look entirely plausible and be measuring
// something else. So this asserts against a signal whose frequency and amplitude
// are both known before the engine is involved:
//
//   * `NAE_OpenExclusiveSine` emits a 1 kHz sine at -20 dBFS, so 0.1 full scale.
//     The peak bin must land on the bin 1000 Hz falls in, and its magnitude must
//     be 0.1 times the Hann window's response at that bin's fractional offset.
//     Both numbers are computed here rather than eyeballed, so a scaling mistake
//     of 3 dB or a rate mistake shows up as a ratio and not as a plausible curve.
//
//   * The fractional offset matters and is not a detail. 4096 bins at 192 kHz is
//     46.875 Hz per bin, so 1000 Hz sits between bins 21 and 22 and a rectangular
//     window would report roughly 0.85 of the true amplitude from bin leakage
//     alone. The expected value is the windowed one, which is why the Hann
//     response is computed instead of assuming 0.1.
//
// # The file mode exists for the DSD question
//
// Playing a real file and reading the tap measures what the engine *produced*
// after resampling, at the rate it produced it. That is the number the
// `dsd_spectrum` decoder-level instrument cannot give, and it is the one the
// player will actually draw on screen -- so if the DSD track's ultrasonic content
// is audible or not, this is where it shows up.
//
// usage:
//   spectrum_probe sine [seconds]              verify against 1 kHz at -20 dBFS
//   spectrum_probe file <path> [seconds]       band survey of a real file

#include "nae.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

/// The tone NAE_OpenExclusiveSine emits.
///
/// The frequency is fixed in the engine. The amplitude is not: nae.cpp reads
/// `NAE_SINE_AMPLITUDE` and falls back to 0.5, having raised it from 0.1 because
/// "-20 dBFS was too quiet to judge" -- at 0.1 the answer to "is there a periodic
/// click" was not "no artefact" but "I could not hear it either way", which is
/// the same as having no measurement.
///
/// So this probe sets the variable itself rather than reading it back, and checks
/// against what it asked for. A probe that trusted the engine's default would
/// have been asserting an amplitude the engine had already changed once, and it
/// would have failed by 14 dB while looking like a broken tap.
constexpr double kToneHz = 1000.0;
constexpr double kToneAmplitude = 0.25;  // -12 dBFS, set below via the env var

/// Magnitude response of a symmetric Hann window at a fractional bin offset, per bin.
///
/// The tap's bins are `2/N` times the DFT magnitude of a Hann-windowed frame, so a
/// tone at exactly bin k reads `A`, and one at fractional offset `d` from a bin
/// reads `A * |W(d)|`. For Hann that is `sinc(d) / (1 - d^2)`.
///
/// This is the *closed form* and it is what the probe used to assert against. It
/// was wrong by 6.02 dB, and the error was reproducible to four decimals across
/// runs, which is what made it obviously a formula error rather than a signal
/// fault. The closed form describes the window's continuous-frequency response;
/// what the DFT actually produces at a fractional offset is the same thing
/// sampled, and the two differ by exactly the factor that was missing.
///
/// So the expected value is now computed by summing the same window against the
/// same tone, rather than by evaluating a formula about the window. That makes
/// the check independent of the tap: it cannot be satisfied by a shared mistake,
/// because the only thing they share is the definition of the tone.
double expectedBinMagnitude(double amplitude, double hz, uint32_t rate, uint32_t fftSize,
                            uint32_t bin) {
    double re = 0.0;
    double im = 0.0;
    for (uint32_t n = 0; n < fftSize; ++n) {
        // The same symmetric Hann the engine applies.
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(n) /
                                              static_cast<double>(fftSize - 1)));
        const double x = amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(n) /
                                              static_cast<double>(rate));
        const double a = 2.0 * kPi * static_cast<double>(bin) * static_cast<double>(n) /
                         static_cast<double>(fftSize);
        re += x * w * std::cos(a);
        im -= x * w * std::sin(a);
    }
    return std::sqrt(re * re + im * im) * 2.0 / static_cast<double>(fftSize);
}

struct Reading {
    NAE_SpectrumInfo info{};
    std::vector<float> bins;
    bool ok = false;
};

Reading readSpectrum(NAE_Handle* h) {
    Reading r;
    NAE_SpectrumInfo probe{};
    probe.struct_size = sizeof(NAE_SpectrumInfo);
    if (NAE_QuerySpectrum(h, &probe, nullptr, 0) != NAE_OK) return r;
    r.bins.assign(probe.binCount ? probe.binCount : 1, 0.0f);
    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(NAE_SpectrumInfo);
    if (NAE_QuerySpectrum(h, &info, r.bins.data(), static_cast<int32_t>(r.bins.size())) != NAE_OK) {
        return r;
    }
    r.info = info;
    r.ok = true;
    return r;
}

/// Peak bin and the level around it, printed as facts rather than a curve.
void reportPeaks(const Reading& r, int topN) {
    const uint32_t n = static_cast<uint32_t>(r.bins.size());
    struct P {
        uint32_t bin;
        float mag;
    };
    std::vector<P> all;
    all.reserve(n);
    for (uint32_t k = 1; k < n; ++k) all.push_back({k, r.bins[k]});
    std::partial_sort(all.begin(), all.begin() + std::min<size_t>(topN, all.size()), all.end(),
                      [](const P& a, const P& b) { return a.mag > b.mag; });
    std::printf("  top bins (bin width %.2f Hz):\n", r.info.sampleRate / static_cast<double>(r.info.fftSize));
    for (int i = 0; i < topN && i < static_cast<int>(all.size()); ++i) {
        const double hz = all[static_cast<size_t>(i)].bin * r.info.sampleRate /
                          static_cast<double>(r.info.fftSize);
        std::printf("    %6u  %9.2f Hz  %.6f  %7.2f dBFS\n", all[static_cast<size_t>(i)].bin, hz,
                    all[static_cast<size_t>(i)].mag,
                    20.0 * std::log10(std::max(1e-9f, all[static_cast<size_t>(i)].mag)));
    }
}

/// Band survey: level in dB relative to the window's own peak, per band.
///
/// Relative rather than absolute, because a spectrum display's floor is
/// meaningful only against the loudest thing in the same window. An absolute
/// dBFS figure for a band of a quiet passage says more about the passage than
/// about the path.
void reportBands(const Reading& r) {
    const double rate = static_cast<double>(r.info.sampleRate);
    const double nyquist = rate * 0.5;
    struct Band {
        const char* name;
        double lo;
        double hi;
    };
    const Band bands[] = {
        {"20-200 Hz   ", 20, 200},      {"200-2k Hz   ", 200, 2000},
        {"2k-8k Hz    ", 2000, 8000},   {"8k-20k Hz   ", 8000, 20000},
        {"20k-40k Hz  ", 20000, 40000}, {"40k-80k Hz  ", 40000, 80000},
        {"80k-Nyquist ", 80000, 1e9},
    };
    std::printf("  band survey, relative to this window's peak (%.2f):\n", r.info.peak);
    for (const Band& b : bands) {
        const double hi = std::min(b.hi, nyquist);
        if (b.lo >= hi) {
            std::printf("    %s  above Nyquist (%.0f Hz) -- not measured\n", b.name, nyquist);
            continue;
        }
        double sum = 0.0;
        uint32_t count = 0;
        for (uint32_t k = 1; k < r.info.binCount; ++k) {
            const double hz = k * rate / static_cast<double>(r.info.fftSize);
            if (hz < b.lo || hz >= hi) continue;
            const double m = r.bins[k];
            sum += m * m;
            ++count;
        }
        const double rms = count ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
        const double db = 20.0 * std::log10(std::max(1e-9, rms / std::max(1e-9, r.info.peak)));
        std::printf("    %s  %+7.2f dB   (%u bins, %s)\n", b.name, db, count,
                    b.hi > nyquist ? "clipped at Nyquist" : "whole band");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "sine";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 6;

    if (std::strcmp(mode, "file") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: spectrum_probe file <path> [seconds]\n");
            return 2;
        }
        const char* path = argv[2];
        const int secs = argc > 3 ? std::atoi(argv[3]) : seconds;
        NAE_Handle* h = nullptr;
        int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
                dithered = 0, bitPerfect = 0, fileCh = 0;
        double durationMs = 0.0, latencyMs = 0.0;
        const int32_t rc = NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &h, &rate, &srcRate,
                                        &ch, &bits, &valid, &isFloat, &resampling, &dithered,
                                        &bitPerfect, &fileCh, &durationMs, &latencyMs);
        if (rc != 0) {
            std::fprintf(stderr, "NAE_OpenFile(%s) -> %d\n", path, rc);
            return 1;
        }
        std::printf("file    %s\n", path);
        std::printf("open: src %d Hz -> out %d Hz  %d ch  valid %d bit  resampling=%d dithered=%d bitPerfect=%d\n",
                    srcRate, rate, ch, valid, resampling, dithered, bitPerfect);
        std::printf("playing %d s\n", secs);
        std::fflush(stdout);

        // Sliced exactly as the application does it: 200 ms per NAE_Play. A probe
        // that made one call for the whole run would be measuring a path the
        // application never takes.
        const int sliceMs = 200;
        int played = 0;
        Reading last;
        while (played < secs * 1000) {
            const int32_t want = std::min(sliceMs, secs * 1000 - played);
            const int32_t prc = NAE_Play(h, want);
            if (prc != 0) {
                std::printf("NAE_Play -> %d after %d ms\n", prc, played);
                break;
            }
            played += want;
            last = readSpectrum(h);
            if (played % 1000 == 0) {
                std::printf("  %5d ms  peak %.6f  rms %.6f  framesFed %llu\n", played,
                            last.info.peak, last.info.rms,
                            static_cast<unsigned long long>(last.info.framesFed));
                std::fflush(stdout);
            }
        }
        if (!last.ok) {
            std::fprintf(stderr, "NAE_QuerySpectrum produced nothing\n");
            NAE_Close(h);
            return 1;
        }
        std::printf("final window: fft %u  bins %u  rate %u Hz  valid %u bit  peak %.6f  rms %.6f\n",
                    last.info.fftSize, last.info.binCount, last.info.sampleRate, last.info.validBits,
                    last.info.peak, last.info.rms);
        reportBands(last);
        reportPeaks(last, 6);
        NAE_Close(h);
        return 0;
    }

    // ---- the seam ratio's distribution ----
//
// Uses the file path, because a synthesised tone has no buffer boundaries of its
// own worth describing and the question is about real material.
if (std::strcmp(mode, "seam") == 0) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: spectrum_probe seam <path> [seconds]\n");
        return 2;
    }
    const char* path = argv[2];
    const int secs = argc > 3 ? std::atoi(argv[3]) : 120;
    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileCh = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &h, &rate, &srcRate,
                                    &ch, &bits, &valid, &isFloat, &resampling, &dithered,
                                    &bitPerfect, &fileCh, &durationMs, &latencyMs);
    if (rc != 0) {
        std::fprintf(stderr, "NAE_OpenFile(%s) -> %d\n", path, rc);
        return 1;
    }
    std::printf("file  %s\n", path);
    std::printf("open: src %d Hz -> out %d Hz  %d ch  valid %d bit  resampling=%d bitPerfect=%d\n",
                srcRate, rate, ch, valid, resampling, bitPerfect);
    std::printf("playing %d s to accumulate seams\n\n", secs);
    std::fflush(stdout);

    const int sliceMs = 200;
    int played = 0;
    while (played < secs * 1000) {
        const int32_t want = std::min(sliceMs, secs * 1000 - played);
        if (NAE_Play(h, want) != 0) break;
        played += want;
    }

    NAE_SignalPath sp{};
    sp.structSize = sizeof(sp);
    NAE_QuerySignalPath(h, &sp);

    std::printf("seams examined   %llu\n", (unsigned long long)sp.seamCount);
    std::printf("worst ratio      %.6f\n", sp.worstSeamRatio);
    std::printf("signal slew ref  %.6f   (the waveform's own fastest step, normalised)\n",
                sp.seamSlewRef);
    std::printf("mean ratio       %.6f\n", sp.seamMean);
    std::printf("quantiles from   %u retained seams%s\n", sp.seamSampleCount,
                sp.seamSampleCount < sp.seamCount ? "  (CAPPED -- not the whole set)" : "");
    std::printf("  min %.4f  12.5%% %.4f  25%% %.4f  37.5%% %.4f  "
                "50%% %.4f  62.5%% %.4f  75%% %.4f  max %.4f\n",
                sp.seamQuantiles[0], sp.seamQuantiles[1], sp.seamQuantiles[2],
                sp.seamQuantiles[3], sp.seamQuantiles[4], sp.seamQuantiles[5],
                sp.seamQuantiles[6], sp.seamQuantiles[7]);

    std::printf("\nhistogram (log-spaced, -2..+2 decades, 0.1 per bucket)\n");
    uint64_t histTotal = 0;
    for (uint32_t b = 0; b < sp.seamHistogramMax; ++b) histTotal += sp.seamHistogram[b];
    for (uint32_t b = 0; b < sp.seamHistogramMax; ++b) {
        if (sp.seamHistogram[b] == 0) continue;
        const double decade = -2.0 + static_cast<double>(b) * 0.1;
        const double lo = std::pow(10.0, decade);
        const double hi = std::pow(10.0, decade + 0.1);
        const double pct = histTotal ? 100.0 * static_cast<double>(sp.seamHistogram[b]) /
                                          static_cast<double>(histTotal)
                                    : 0.0;
        std::printf("  %7.3f-%7.3f  %8llu  %5.1f%%  ", lo, hi,
                    (unsigned long long)sp.seamHistogram[b], pct);
        const int bars = static_cast<int>(pct / 2.0);
        for (int i = 0; i < std::min(bars, 50); ++i) std::printf("#");
        std::printf("\n");
    }
    std::printf("\n  total in histogram %llu of %llu seams%s\n", (unsigned long long)histTotal,
                (unsigned long long)sp.seamCount,
                histTotal < sp.seamCount
                    ? "  (ratios outside -2..+2 decades are not binned)"
                    : "");

    const bool capped = sp.seamSampleCount < sp.seamCount;
    std::printf("\nverdict\n");
    if (sp.seamSlewRef > 0.0) {
        const bool continuous = sp.worstSeamRatio <= sp.seamSlewRef;
        std::printf("  worst seam %.4f vs the signal's own slew %.4f  ->  %s\n",
                    sp.worstSeamRatio, sp.seamSlewRef,
                    continuous ? "continuous: no seam exceeds the material's own step"
                               : "DISCONTINUOUS: some seam is larger than the material's step");
        std::printf("  ratio of worst to slew: %.2fx\n", sp.worstSeamRatio / sp.seamSlewRef);
    } else {
        std::printf("  no slew reference measured, so no verdict is available\n");
    }
    if (capped) {
        std::printf("  NOTE: the quantile set covers the first %u of %llu seams. The maximum\n"
                    "        above is over all of them, so the two do not describe the same set.\n",
                    sp.seamSampleCount, (unsigned long long)sp.seamCount);
    }
    NAE_Close(h);
    return 0;
}

// ---- 文件元数据 ----
if (std::strcmp(mode, "track") == 0) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: spectrum_probe track <path>\n");
        return 2;
    }
    const char* path = argv[2];
    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileCh = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &h, &rate, &srcRate,
                                    &ch, &bits, &valid, &isFloat, &resampling, &dithered,
                                    &bitPerfect, &fileCh, &durationMs, &latencyMs);
    if (rc != 0) {
        std::fprintf(stderr, "NAE_OpenFile(%s) -> %d  %s\n", path, rc, NAE_GetLastError());
        return 1;
    }
    NAE_TrackInfo ti{};
    ti.structSize = sizeof(ti);
    NAE_QueryTrackInfo(h, &ti);

    std::printf("file      %s\n", path);
    std::printf("\nNAE_QueryTrackInfo -- 用户文件是什么\n");
    std::printf("  codec            %s\n", ti.codec[0] ? ti.codec : "(none reported)");
    std::printf("  sampleRate       %d Hz\n", ti.sampleRate);
    std::printf("  channels         %d\n", ti.channels);
    std::printf("  bitsPerSample    %d   (container width, NOT valid depth)\n", ti.bitsPerSample);
    std::printf("  durationMs       %d  (%.2f s)\n", ti.durationMs, ti.durationMs / 1000.0);

    std::printf("\nNAE_QueryTags -- 标签（可能不存在）\n");
    NAE_Tags tags{};
    tags.structSize = sizeof(tags);
    NAE_QueryTags(h, &tags);
    std::printf("  year             %s\n", tags.year[0] ? tags.year : "(none reported)");
    std::printf("  genre            %s\n", tags.genre[0] ? tags.genre : "(none reported)");
    std::printf("  album            %s\n", tags.album[0] ? tags.album : "(none reported)");
    std::printf("  artist           %s\n", tags.artist[0] ? tags.artist : "(none reported)");
    std::printf("  bitrate          %u kbps%s\n", tags.bitrateKbps,
                tags.bitrateKbps == 0 ? "  (none reported)" : "");

    std::printf("\nNAE_SignalPath -- 设备正在被喂什么\n");
    NAE_SignalPath sp{};
    sp.structSize = sizeof(sp);
    NAE_QuerySignalPath(h, &sp);
    std::printf("  sampleRate       %d Hz\n", sp.sampleRate);
    std::printf("  channels         %d\n", sp.channels);
    std::printf("  requestedBits    %d\n", sp.requestedBits);
    std::printf("  grantedBits      %d\n", sp.grantedBits);
    std::printf("  validBits        %d\n", sp.validBits);
    std::printf("  resampling       %d\n", sp.resampling);
    std::printf("  bitPerfect       %d\n", sp.bitPerfect);

    std::printf("\n两者的差异（界面需要同时显示这两组）\n");
    const bool rateDiffers = ti.sampleRate != sp.sampleRate;
    std::printf("  采样率           文件 %d / 输出 %d  %s\n", ti.sampleRate, sp.sampleRate,
                rateDiffers ? "不同 -> 文件被重采样了" : "相同 -> 原生速率");
    const bool widthDiffers = ti.bitsPerSample != sp.grantedBits;
    std::printf("  位宽             容器 %d / 授予 %d  %s\n", ti.bitsPerSample, sp.grantedBits,
                widthDiffers ? "不同 -> 容器位宽不等于设备字长" : "相同");
    if (ti.channels != sp.channels) {
        std::printf("  声道             文件 %d / 输出 %d  -> 下混/上混了\n", ti.channels,
                    sp.channels);
    }
    NAE_Close(h);
    return 0;
}

// ---- 时域包络（节拍检测的数据源）----
if (std::strcmp(mode, "envelope") == 0) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: spectrum_probe envelope <path> [seconds]\n");
        return 2;
    }
    const char* path = argv[2];
    const int secs = argc > 3 ? std::atoi(argv[3]) : 30;
    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileCh = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &h, &rate, &srcRate,
                                    &ch, &bits, &valid, &isFloat, &resampling, &dithered,
                                    &bitPerfect, &fileCh, &durationMs, &latencyMs);
    if (rc != 0) {
        std::fprintf(stderr, "NAE_OpenFile(%s) -> %d\n", path, rc);
        return 1;
    }
    std::printf("file  %s\n", path);
    std::printf("open: src %d Hz -> out %d Hz  %d ch  valid %d bit  resampling=%d bitPerfect=%d\n",
                srcRate, rate, ch, valid, resampling, bitPerfect);

    const int sliceMs = 200;
    int played = 0;
    while (played < secs * 1000) {
        const int32_t want = std::min(sliceMs, secs * 1000 - played);
        if (NAE_Play(h, want) != 0) break;
        played += want;
    }

    NAE_EnvelopeInfo probe{};
    probe.struct_size = sizeof(probe);
    NAE_QueryEnvelope(h, &probe, nullptr, 0);
    if (probe.frame_count == 0) {
        std::fprintf(stderr, "the envelope is empty\n");
        NAE_Close(h);
        return 1;
    }
    std::vector<float> env(probe.frame_count);
    NAE_EnvelopeInfo info{};
    info.struct_size = sizeof(info);
    NAE_QueryEnvelope(h, &info, env.data(), static_cast<int32_t>(env.size()));

    // **只有前 `frame_valid` 帧是真的**（`frame_count` 是环的容量），而且顺序是最老的
    // 在前。以前这里把整条 `env` 都当成读数，于是尾部那一片零被算进了 min/max/mean——
    // 和界面上的节拍估计器原本犯的是同一个错。
    //
    // 名字叫 `framesValid` 不叫 `valid`：这个函数上面已经有一个 `valid`（解码器报的有效
    // 位深），重名会让编译停下——这是好事，但改名字比改那个更便宜。
    const size_t framesValid = std::min<size_t>(info.frame_valid, env.size());
    if (framesValid == 0) {
        std::fprintf(stderr, "the envelope has no filled frames yet\n");
        NAE_Close(h);
        return 1;
    }

    std::printf("\nenvelope: %llu real of %u capacity, hop %.1f ms, rate %u Hz, window %.0f ms, "
                "framesFed %llu\n",
                static_cast<unsigned long long>(framesValid), info.frame_count, info.frame_hop_ms,
                info.sample_rate, info.window_ms,
                static_cast<unsigned long long>(info.frames_fed));

    // 每 40 帧（200 ms）打一个点，和播放的分片节奏一致，这样能直接看出周期性。
    const size_t stride = static_cast<size_t>(info.frame_hop_ms > 0 ? 200.0 / info.frame_hop_ms : 40);
    std::printf("\nenvelope over time, one sample per %.0f ms:\n",
                static_cast<double>(stride) * info.frame_hop_ms);
    double lo = 1e30;
    double hi = -1e30;
    double sum = 0.0;
    for (size_t i = 0; i < framesValid; ++i) {
        lo = std::min(lo, static_cast<double>(env[i]));
        hi = std::max(hi, static_cast<double>(env[i]));
        sum += static_cast<double>(env[i]);
    }
    for (size_t i = 0; i < framesValid; i += stride) {
        std::printf("%7.1f ms  %.6f  ", static_cast<double>(i) * info.frame_hop_ms,
                    static_cast<double>(env[i]));
        const int bars = static_cast<int>(static_cast<double>(env[i]) / std::max(1e-9, hi) * 50.0);
        for (int b = 0; b < std::min(bars, 50); ++b) std::printf("#");
        std::printf("\n");
    }
    std::printf("\n  min %.6f  max %.6f  mean %.6f\n", lo, hi,
                sum / static_cast<double>(framesValid));
    std::printf("  all finite: %s\n",
                std::all_of(env.begin(), env.begin() + static_cast<ptrdiff_t>(framesValid),
                            [](float v) { return std::isfinite(v); })
                    ? "yes"
                    : "NO -- a non-finite frame would poison the tempo detector");
    NAE_Close(h);
    return 0;
}

// ---- verification against a known tone ----
    //
    // The engine reads NAE_SINE_AMPLITUDE when the stream opens, so it has to be
    // in the environment before NAE_OpenExclusiveSine and not after.
    _putenv_s("NAE_SINE_AMPLITUDE", "0.25");
    std::printf("verifying the tap against NAE_OpenExclusiveSine's own tone\n");
    std::printf("  expected: %.0f Hz at %.4f full scale (%.1f dBFS)\n", kToneHz, kToneAmplitude,
                20.0 * std::log10(kToneAmplitude));
    NAE_Handle* h = nullptr;
    int32_t rate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0;
    double latencyMs = 0.0;
    const int32_t rc = NAE_OpenExclusiveSine(nullptr, &h, &rate, &ch, &bits, &valid, &isFloat, &latencyMs);
    if (rc != 0) {
        std::fprintf(stderr, "NAE_OpenExclusiveSine -> %d\n", rc);
        return 1;
    }
    std::printf("opened: %d Hz  %d ch  %d bit (valid %d)  latency %.2f ms\n", rate, ch, bits, valid,
                latencyMs);
    std::fflush(stdout);

    int played = 0;
    Reading last;
    while (played < seconds * 1000) {
        const int32_t want = std::min(200, seconds * 1000 - played);
        if (NAE_PlaySine(h, want) != 0) break;
        played += want;
        last = readSpectrum(h);
    }
    if (!last.ok || last.info.framesFed == 0) {
        std::fprintf(stderr, "the tap was never fed: framesFed=%llu\n",
                     static_cast<unsigned long long>(last.info.framesFed));
        NAE_Close(h);
        return 1;
    }

    const double binHz = last.info.sampleRate / static_cast<double>(last.info.fftSize);
    const double exact = kToneHz / binHz;
    const uint32_t nearest = static_cast<uint32_t>(std::llround(exact));
    const double offset = exact - static_cast<double>(nearest);
    const double expected = expectedBinMagnitude(kToneAmplitude, kToneHz, last.info.sampleRate,
                                                 last.info.fftSize, nearest);
    const float got = last.bins[nearest < last.bins.size() ? nearest : 0];
    const double gotDb = 20.0 * std::log10(std::max(1e-9f, got));

    std::printf("window: fft %u  bins %u  rate %u Hz  bin width %.4f Hz  framesFed %llu\n",
                last.info.fftSize, last.info.binCount, last.info.sampleRate, binHz,
                static_cast<unsigned long long>(last.info.framesFed));
    std::printf("peak %.6f  rms %.6f  valid %u bit\n", last.info.peak, last.info.rms,
                last.info.validBits);
    std::printf("\ncheck\n");
    std::printf("  %0.f Hz is bin %.4f, nearest bin %u (offset %+.4f)\n", kToneHz, exact, nearest,
                offset);
    std::printf("  expected magnitude there     %.6f  (%+.2f dBFS)\n", expected,
                20.0 * std::log10(std::max(1e-9, expected)));
    std::printf("  measured                     %.6f  (%+.2f dBFS)\n", got, gotDb);
    const double ratio = got / std::max(1e-9, expected);
    std::printf("  ratio measured/expected      %.4f  (%+.2f dB)\n", ratio, 20.0 * std::log10(ratio));

    // The peak bin alone is a weak check when the tone falls between bins, so the
    // window's total spectral energy is checked as well. That total is fixed by
    // the amplitude and the window regardless of where the tone sits, which makes
    // it insensitive to bin alignment -- so passing both means the scaling and the
    // alignment are each right, and passing only one says which is broken.
    //
    // The scale is `2/sqrt(N)`, not the `2/N` the bins use. Those are different
    // conventions serving different purposes and conflating them was worth a
    // -66 dB "failure" that was entirely arithmetic: the bins' `2/N` normalises
    // so a full-scale sine reads 1.0 in its own bin, which is the right target for
    // a dBFS display, while a sum over bins is an energy and needs the sqrt
    // normalisation. An earlier version of this check divided the bins by nothing
    // at all and reported 0.0005, a factor of exactly 2000 out -- 2*sqrt(4096),
    // which is what a missing `sqrt` looks like.
    double energy = 0.0;
    for (float b : last.bins) energy += static_cast<double>(b) * static_cast<double>(b);
    double w2 = 0.0;
    for (uint32_t n = 0; n < last.info.fftSize; ++n) {
        const double w =
            0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(n) /
                                 static_cast<double>(last.info.fftSize - 1)));
        w2 += w * w;
    }
    // The factor is `2/N`, read straight out of a scratch program that sums the same
    // window against the same tone: it reported `sum(bin^2) / E = 0.000488281` for
    // `N = 4096`, which is `2/N` to nine decimals.
    //
    // Three factors were tried before that -- none, `2/N^2` and `4/N^2` -- and
    // each read a round 0.5, 4096 or 2000. The lesson is the one worth keeping:
    // a ratio that comes out an exact power of two is arithmetic, not signal, and
    // three wrong guesses in a row was the cost of deriving it by reasoning
    // instead of measuring it once.
    const double n = static_cast<double>(last.info.fftSize);
    const double expectedEnergy =
        (kToneAmplitude * kToneAmplitude / 2.0 * w2) * (2.0 / n);
    const double energyRatio = energy / std::max(1e-12, expectedEnergy);
    std::printf("\n  window energy (sum of bins squared)\n");
    std::printf("    expected %.6f   measured %.6f   ratio %.4f (%+.2f dB)\n", expectedEnergy,
                energy, energyRatio, 20.0 * std::log10(std::max(1e-12, energyRatio)));

    // 0.5 dB is generous on purpose: the window straddles a buffer boundary and
    // the amplitude estimate carries the residual of that. A 3 dB scaling error or
    // a wrong rate would miss this by a wide margin, and those are the mistakes
    // that make a spectrum look plausible and measure something else.
    const bool pass = ratio > 0.944 && ratio < 1.059 && energyRatio > 0.944 &&
                      energyRatio < 1.059;
    std::printf("\n  %s\n", pass ? "PASS within 0.5 dB" : "FAIL outside 0.5 dB");
    reportPeaks(last, 3);
    NAE_Close(h);
    return pass ? 0 : 1;
}
