// The engine's per-stream state, in one place.
//
// This was the top of `nae.cpp`, which meant every platform file that wanted to
// touch a stream had to be that file. It is a header now so the platform split
// can be a file boundary rather than an `#ifdef` boundary: a Windows edit lands
// in a file that does not compile on macOS at all, and the reverse.
#ifndef NEIRO_NAE_INTERNAL_H
#define NEIRO_NAE_INTERNAL_H

#include "nae.h"

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <mmsystem.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "wasapi_compat.hpp"
using Microsoft::WRL::ComPtr;
#endif
#include "audio_backend.h"
#include "clip_guard.h"
#include "decoder.h"
#include "dff_reader.h"
#include "dop_pack.h"
#include "dsd_downrate.h"
#include "dsf_reader.h"
#include "dither.h"
#include "eq_coeffs.h"
#include "linear_phase_eq.h"
#include "loudness.h"
#include "output_mode.h"
#include "output_stream.h"
#include "partitioned_convolver.h"
#include "resampler.h"
#include "routing.h"
#include "utf8_file.h"

constexpr size_t kSeamQuantiles = 8;
/// Log-spaced buckets from 0.01 to 100, i.e. -2..+2 decades.
constexpr size_t kSeamBuckets = 41;
/// Seams retained for the quantiles. 4096 is about 35 minutes at this buffer size:
/// long enough to catch a periodic artefact, short enough not to be a leak.
constexpr size_t kSeamSamples = 4096;


// The format the device agreed to, without a platform type.
//
// `WAVEFORMATEXTENSIBLE` is what WASAPI negotiates in, but the render loop only
// ever reads five numbers from it: rate, channels, container bits, valid bits
// and block align, plus whether the samples are float. Keeping the whole
// Windows struct on the handle made every one of the 48 read sites a Windows
// dependency, which is what stood between this file and a second backend.
//
// The WASAPI open path converts once, at the two places a handle is created.
// Everything downstream -- EQ, dither, resampler, routing, position accounting
// -- reads this and cannot tell which OS produced it.
struct StreamFormat {
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint32_t containerBits = 0;
    uint32_t validBits = 0;
    uint32_t blockAlign = 0;
    bool isFloat = false;
};

/// The five numbers plus the sample type, read once from what the device granted.
///
/// Filled from the `OpenedFormat` the backend returns, which is measured after
/// `Initialize`, so it is the agreed format rather than the requested one; the
/// difference between the two is what the render path reports, so taking it from
/// the device's answer is the only correct source.

// Handle definition lives here rather than in the header: the C ABI must not

/// The message behind the last non-OK return. One string per process, because
/// the C ABI reports it through `NAE_GetLastError` and a caller can only have
/// one call in flight that it is asking about.
extern thread_local std::string g_lastErrorText;

#ifndef _WIN32
// The share-mode names the call sites use, which come from audioclient.h on
// Windows. Kept here rather than in nae.cpp because the open implementation that
// uses them no longer lives in nae.cpp.
using AUDCLNT_SHAREMODE = int;
constexpr int AUDCLNT_SHAREMODE_SHARED = 0;
constexpr int AUDCLNT_SHAREMODE_EXCLUSIVE = 1;
#endif

/// Open a stream on the platform's device layer.
///
/// Declared here because the shared open path calls it, and defined once per
/// platform: in `nae.cpp` for Windows, in `open_coreaudio.cpp` for macOS. The
/// signature is the contract the shared code was written against, so a platform
/// that cannot fill it fails to link rather than failing at run time.
extern "C" int32_t openExclusiveImpl(const char* deviceId, int32_t sourceRate, int32_t outputMode,
                          int32_t fixedRate, int32_t quality, AUDCLNT_SHAREMODE shareMode,
                          NAE_Handle** out, int32_t* outSampleRate, int32_t* outSourceRate,
                          int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                          int32_t* isFloat, int32_t* resampling, int32_t* dithered,
                          int32_t* bitPerfect, double* latencyMs, int32_t bufferMode,
                          uint32_t bufferMs);

struct NAE_Handle {
    StreamFormat fmt{};
    /// The device, in the platform-neutral shape the loop drives.
    ///
    /// The stream owns the COM clients, the render event and the format it was
    /// opened with; the handle holds only the numbers derived from them. That is
    /// what makes one render loop serve both platforms.
    std::shared_ptr<neiro::OutputStream> stream;
    uint32_t bufferFrames = 0;
    double latencyMs = 0.0;

    // ---- conversion state ----
    //
    // These live on the handle rather than being rebuilt per block. soxr carries
    // filter history across calls, so recreating it would put an audible seam in
    // the output every block; the dither generator carries a random state for
    // the same reason.

    // The rate the source material is at, before conversion.
    uint32_t sourceRate = 0;

    // The file's own channel count, which differs from the device's whenever a

    // conversion happened. NAE_QuerySignalPath reports the file's, because the
    // listener's question is "what is in the file" and the device's number
    // answers a different one.
    int32_t fileChannels = 0;

    // Present only when a converter is actually in the path. When rates match,
    // `resampler.ready()` is false and samples pass through untouched — which is
    // what makes the bit-perfect claim true rather than merely intended.
    neiro::Resampler resampler;

    // Dither is sized to the device's *valid* bits, not the container size. A
    // 24-in-32 device must not dither as though it were 24-bit.
    neiro::Dither dither{32};
    bool dithered = false;

    // Reported back to the UI. The one value that decides whether "bit-perfect"
    // may be said out loud, and beside it the name of the verdict, which decides
    // what the UI may say about it.
    bool bitPerfect = false;
    bool resampling = false;
    neiro::PerfectReason reason = neiro::PerfectReason::DeviceCannotCarryFormat;

    // Which share mode this stream was actually opened in.
    //
    // Stored rather than assumed, because `NAE_QuerySignalPath` used to answer
    // `exclusive = 1` unconditionally -- a hardcoded claim that happened to be
    // true while exclusive was the only mode there was. It became false the moment
    // shared existed, and nothing caught it except an instrument that opened a
    // shared stream and read the field back.
    bool exclusive = true;

    // 采样实际是怎么被解出来的。见 `NAE_QueryDecodePath`。
    //
    // 存在句柄上而不是每次去问解码器，是因为解码器在打开之后就可能被换掉（换曲、seek），
    // 而界面要显示的是**当前**这条路径。打开时存一份，之后只读。
    std::string decodePath;
    // The listener's setting did not choose this rate; the driver did. Kept apart
    // from `reason` because the interface has to say both things and they are
    // different: the reason says what happened to the samples, this says whose
    // decision it was.
    bool coercedToSupportedRate = false;

    // ---- telemetry ----
    //
    // Read by NAE_QuerySignalPath. Counted on the audio thread and only ever
    // incremented, so no lock is needed and the read is a snapshot that may be a
    // few callbacks stale -- which for "did this happen during the track" is the
    // only question being asked.
    uint64_t callbackCount = 0;
    uint64_t deadlineMissCount = 0;
    double peakCallbackMs = 0.0;
    uint64_t underrunCount = 0;
    uint64_t dropCount = 0;
    // Device frames the render loop had to invent because the fifo ran dry
    // mid-buffer. Non-zero means the decoder is not keeping up in real time, and
    // it is the number that turns "the output burbles" into a measured statement.
    uint64_t padFrameCount = 0;
    // Worst wall-clock gap between two consecutive device-buffer writes, and the
    // device buffer's own duration in ms.
    //
    // This is the measurement that can see a missed deadline. Every other counter
    // here reports what the engine *did*, and a buffer written perfectly but late
    // is indistinguishable from one written on time. A gap larger than
    // `bufferDurationMs` means the device ran dry waiting, no matter how correct
    // the samples were -- which is the whole of "steady clicking at the buffer
    // rate", a fault that was invisible until this existed.
    double maxBufferGapMs = 0.0;

    /// Milliseconds the render thread spent inside topUp() in its worst period.
    ///
    /// Our render loop decodes from inside the device period; Twilight Echo's runs
    /// the decoder on its own thread. Whatever this costs is spent out of the
    /// runway the device gave us, so this is the number that has to be small.
    double maxDecodeStallMs = 0.0;

    /// Milliseconds from reading the padding to handing the buffer over, worst case.
    double maxFillMs = 0.0;

    /// Milliseconds by which the worst buffer missed its runway, and how often.
    ///
    /// Zero means every buffer reached the device before the device ran out of
    /// audio to play. A positive value is the device having played stale audio,
    /// which is what a bubble is.
    double maxLateMs = 0.0;
    int64_t lateCount = 0;
    double bufferDurationMs = 0.0;

    // Device frames actually delivered, per second of wall clock.
    //
    // Everything else here counts frames the engine *wrote*. This counts frames
    // the device was actually handed per second of real time, and it is the only
    // figure that can distinguish "playing at the right speed" from "playing
    // correctly and slowly". The fifo accounting balances either way.
    double deviceRateActual = 0.0;
    double rateWindowStart = 0.0;
    int64_t rateWindowFrames = 0;

    /// Nothing to hold here any more.
    ///
    /// This slot held the "discard the rate window on pause" flag, which was the
    /// second of two attempts to keep `deviceRateActual` from reporting a pause as
    /// playback at half rate. Both are gone; the reason is LESSONS.md 58 and the
    /// working definition is in the field's own documentation. Nothing is kept here
    /// because a field whose only content is "this used to be wrong" is a field
    /// nobody will read twice.
    /// Set once the first 3 s of the stream have elapsed and been discarded.
    bool rateWarmupDone = false;

    /// Device frames still to be faded in, after a start.
    ///
    /// A resume begins from silence -- the prefill fills the device buffer with
    /// zeros -- so a stream that starts at a non-zero phase steps out of silence and
    /// clicks. The ramp is applied to the first few milliseconds of real audio.
    int64_t fadeInRemaining = 0;
    int64_t fadeInTotal = 0;
    int64_t fadeInFrame = 0;

    /// Channel trims (dB), balance (-1..1), polarity mask, and per-channel delay
    /// in device frames. Applied after EQ/crossfeed, before volume.
    double channelTrimDb[NAE_MAX_CHANNELS]{};
    double balance = 0.0;
    uint32_t polarityMask = 0;
    std::vector<int32_t> channelDelay;  // device frames per channel
    std::vector<size_t> channelDelayPos;
    std::vector<float> channelDelayBuf;
    uint32_t channelDelayMax = 0;
    /// Clip guard. Configured by the session policy; inactive by default.
    neiro::ClipGuard clipGuard;
    /// Digital volume, linear 0..1, ramped per device frame.
    ///
    /// 1.0 is exact bypass: the render loop does not multiply at all, so a
    /// bit-perfect stream stays bit-perfect. Any other value is reported as
    /// NAE_REASON_VOLUME_NOT_UNITY.
    double volume = 1.0;
    double volumeTarget = 1.0;
    double volumeRampDelta = 0.0;
    int64_t volumeRampRemaining = 0;

    /// Equaliser cascade. Coefficients are recomputed in NAE_SetEq; the render
    /// loop only runs them. `eqZ*` are transposed direct-form-II state, one pair
    /// per band per channel.
    bool eqEnabled = false;
    double eqPreamp = 1.0;
    int32_t eqBandCount = 0;
    bool eqBandEnabled[NAE_EQ_MAX_BANDS]{};
    uint32_t eqBandChannelMask[NAE_EQ_MAX_BANDS]{};
    bool eqLinear = false;
    neiro::LinearPhaseEq linearEq;
    neiro::EqBiquad eqCoeffs[NAE_EQ_MAX_BANDS]{};
    std::vector<double> eqZ1;
    std::vector<double> eqZ2;
    NAE_EqParams eqParams{};

    /// Stereo crossfeed: a circular delay line of two channels, a one-pole
    /// low-pass per side, and the mix amount. Only used when the stream is 2ch.
    bool crossfeedEnabled = false;
    double crossfeedAmount = 0.3;
    double crossfeedDelayMs = 0.3;
    double crossfeedCutoffHz = 700.0;
    std::vector<double> crossfeedDelay;
    size_t crossfeedDelayFrames = 0;
    size_t crossfeedPos = 0;
    double crossfeedLpL = 0.0;
    double crossfeedLpR = 0.0;
    double crossfeedAlpha = 0.0;
    NAE_CrossfeedParams crossfeedParams{};

    /// FIR convolver. The IR is applied in the source domain, before the
    /// resampler, so it must be at the source rate.
    NAE_ConvolverParams convParams{};
    bool convEnabled = false;
    size_t convTaps = 0;
    size_t convChannels = 0;
    neiro::PartitionedConvolver conv;
    std::vector<float> convDryL;
    std::vector<float> convDryR;
    /// Error text for the render loop, reused so no string is constructed per
    /// device buffer.
    std::string renderErr;

    /// Render-loop scratch, sized once per channel count.
    std::vector<double> frameHeld;
    std::vector<double> frameOut;
    std::vector<float> frameFloat;

    std::vector<float> convInBuf;
    std::vector<float> convDryOut;
    /// The dry-path delay ring: kConvLatency frames x ch, one block.
    std::vector<float> convDryBuf;
    size_t convDryPos = 0;
    double convWet = 1.0;
    double convDry = 0.0;
    double convGain = 1.0;

    /// Crossfade: the next file's first frames are mixed into the current end.
    NAE_CrossfadeParams crossfadeParams{};
    bool crossfadeEnabled = false;
    double crossfadeDurationMs = 0.0;
    int32_t crossfadeCurve = NAE_CROSSFADE_EQUAL_POWER;
    int64_t crossfadeFrames = 0;
    bool crossfadeActive = false;
    std::vector<float> crossfadeBuf;
    size_t crossfadeBufPos = 0;
    size_t crossfadeBufFrames = 0;
    int64_t crossfadeConsumed = 0;

    /// ReplayGain policy and the gain it resolves to. `rgEnabled` is the mode;
    /// the multiply is skipped when `rgGain` is exactly 1.0.
    NAE_ReplayGainInfo rgInfo{};
    NAE_ReplayGainParams rgParams{};
    bool rgEnabled = false;
    double rgGain = 1.0;
    double rgTarget = 1.0;
    double rgDelta = 0.0;
    int64_t rgRamp = 0;

    /// Whether this handle has ever started a stream.
    ///
    /// Distinguishes "the beginning of a track", where silence before the first
    /// sample is right, from "a resume", where it is a 26.67 ms hole in the middle
    /// of the music. Both go through the same Start call.
    bool streamEverStarted = false;

    /// Whether the render loop should hand the device silence instead of audio.
    ///
    /// This is what makes a pause possible at all. Stopping the client and starting
    /// it again is audible on this endpoint -- measured, see NAE_Pause in nae.h --
    /// so a pause keeps the stream running and feeds it zeros. There is then no
    /// boundary for the device to react to, which is the entire point.
    bool silencing = false;

    /// Device frames handed over as silence, reported as `silenceFramesWritten`.
    int64_t silenceFramesWritten = 0;

    // ---- output spectrum tap ----
    //
    // A ring of the most recent `spectrumRing.size()` channel-0 device frames,
    // filled from inside the render loop and read by NAE_QuerySpectrum.
    //
    // `spectrumRingRight` is the same window on channel 1, written in lockstep
    // and only read to take its peak. The FFT and `rms` stay on channel 0, so
    // this is a second ring rather than an interleaved one: interleaving would
    // mean the transform has to deinterleave 4096 values it does not otherwise
    // need, on the thread reading the tap. A mono stream feeds it zeros, which is
    // what its right channel actually carries, so `peakR` reads 0 there.
    //
    // The render loop's only cost here is copying one double per frame; the FFT
    // happens on whichever thread calls the query. That split matters, because the
    // render loop's entire budget is the runway the device gave it and a 4096-point
    // transform inside a 5.7 ms period is a deadline miss waiting to happen.
    //
    // `spectrumLock` is the one lock in the handle. Everything else on this struct
    // is either written-only by the render thread or atomically readable, which is
    // possible because those are all monotonic counters. A ring buffer is neither:
    // the reader has to see a consistent window, so it needs the writer held off.
    std::mutex spectrumLock;
    std::vector<double> spectrumRing;
    std::vector<double> spectrumRingRight;
    size_t spectrumWrite = 0;
    size_t spectrumFilled = 0;
    uint64_t spectrumFramesFed = 0;
    /// Rate the ring was filled at. Recorded rather than read from the signal path
    /// because the tap is filled before any conversion to the device's own format,
    /// and after a rate change a window holding samples from two rates would be a
    /// measurement of nothing.
    uint32_t spectrumRate = 0;
    /// Channel count the ring was filled at. Same reason as `spectrumRate`: a
    /// stereo window followed by a mono stream would otherwise leave a right
    /// channel in the ring that nothing is writing to any more, and `peakR`
    /// would keep reporting a track that has stopped.
    uint32_t spectrumChannels = 0;

    // ---- 时域包络（节拍检测用）----
    //
    // 与频谱共用同一个 `spectrumRing`。环形缓冲里本来就是时域样本，频谱是读它时
    // 做的变换，所以这里**不复制一份**——第二份环形缓冲意味着两处状态要保持同步，
    // 而它们唯一的区别只是聚合方式。
    //
    // 包络本身是一串聚合后的帧，环形缓冲满了就被覆盖，所以它记录的是环形缓冲的
    // 历史，而不是独立的长期存储。窗口长度因此是固定的（见 kEnvelopeFrames），
    // 这是有意的：BPM 检测需要的是最近几秒，不是整首歌。
    std::vector<float> envelope;
    size_t envelopeFill = 0;
    uint32_t envelopeRate = 0;
    // 环形缓冲被消费了多少次，用来判断包络是否已经稳定。
    uint64_t envelopeEpoch = 0;
    // 跨调用的余数：**攒够 `hop` 个样本才写一帧**。
    //
    // 原来是 `for (offset = 0; offset < frames; offset += hop)`——每一帧的边界都对齐到
    // **本次调用**的 0，而且末尾不足一帧的那一段**照样写成一帧**。调用越碎，写出来的帧
    // 就越短，而 `frame_hop_ms` 一直报 5 ms。实测（`probe-tempo-window`，三次独立运行）：
    // 包络的真实帧率是**每秒 358 ~ 421 帧**，而 5 ms 的意思是 200 帧。
    //
    // 节拍估计器的时间基就是这个 5 ms，所以它算出来的每个 BPM 都带着一个 1.8 ~ 2.1 倍的、
    // 而且还在飘的偏差——界面上「同一段音频、同一个窗给出 103 ~ 197」就是这么来的。
    //
    // 改成跨调用累加之后，一帧的定义回到它该有的样子：**连续的 `hop` 个样本**。
    double envelopeAccum = 0.0;
    size_t envelopeAccumN = 0;

    uint64_t deviceLostCount = 0;
    uint64_t recoveryCount = 0;

    // Measured levels, for "the sound is too quiet, where did it go". See the
    // matching block in nae.h for why these are two numbers and not one.
    std::atomic<float> fifoPeak{0.0f};
    std::atomic<float> writtenPeak{0.0f};
    // Peak read back out of the device's own buffer, interpreted as the device
    // will interpret it. writtenPeak is computed before packing and cannot see
    // a wrong shift or a wrong offset; this can.
    std::atomic<float> devicePeak{0.0f};

    // What was asked for, and what the device handed back. `Initialize` writes
    // into the WAVEFORMATEX it was given, so a difference here is the device
    // changing the depth under us -- and the one case where writing the requested
    // depth would be writing the wrong number of bytes per sample.
    uint16_t requestedBits = 0;
    uint16_t grantedBits = 0;

    // Per-call pipeline accounting, for the last `NAE_Play`.
    //
    // This is the same data `NAE_TRACE` prints, exposed so a benchmark can read
    // it instead of scraping a log. The numbers exist because the alternative is
    // reasoning about a playback-rate bug, and reasoning is what produced four
    // wrong conclusions in a row before the counters were added.
    //
    // The invariant worth checking first: `sourceAdvance * deviceRate /
    // sourceRate` must equal `deviceWritten`. When it does not, the device is
    // being fed frames the source never paid for.
    double lastSourceAdvance = 0.0;
    double lastDeviceWritten = 0.0;
    double lastFifoFrames = 0.0;
    double lastProduced = 0.0;
    double lastConsumed = 0.0;
    double lastShortRender = 0.0;

    /// Whether IAudioClient is currently started.
    ///
    /// The bubbling, and the reason this flag exists.
    ///
    /// NAE_Play is a *slice* call: the host calls it every 200 ms
    /// (the Tauri host's SLICE_MS, since removed) and each call was doing
    /// prefill-with-zeros, Start, render, Stop. The zero prefill is a full-scale
    /// discontinuity in a continuous waveform, and it happened five times a
    /// second -- which is exactly the periodic click that was reported as
    /// "bubbling" and could not be found for a whole day.
    ///
    /// Nothing inside a single call could see it. Every counter read healthy:
    /// worstLate 0, padFrameCount 0, worstFill 0.4 ms against a 53 ms runway, and
    /// splice_check found no discontinuity across all 14M decoded frames. Each
    /// call was individually perfect; the fault was in the seam between calls,
    /// and every instrument in this engine measures within one call.
    ///
    /// It was also invisible to the obvious experiment. Deepening the fifo
    /// (NEIRO_FIFO_OVERFEED=4) changed nothing and left deviceRateActual at the
    /// same 98.5%, because what was lost was not audio we failed to supply --
    /// it was audio the device was told to discard.
    bool clientRunning = false;

    /// How many times the client has been stopped and restarted.
    ///
    /// This is the number that would have found it. It is exposed rather than
    /// kept private because a counter that cannot be read cannot accuse anything,
    /// and the whole reason this took a day is that the restart count was
    /// structurally invisible.
    uint64_t clientRestartCount = 0;

    /// The last sample of the previous written buffer, for the seam measurement.
    double seamTail = 0.0;
    bool haveSeamTail = false;

    /// Largest step the signal takes between adjacent samples, normalised.
    ///
    /// The reference the seam is judged against. A waveform that simply continued
    /// has a seam step the same order as this; a break is several times larger.
    double slewRef = 0.0;

    /// Worst seam step over the peak, and the slew it should be compared with.
    double worstSeamRatio = 0.0;
    // The distribution of that ratio, not just its maximum.
    //
    // A maximum with no distribution cannot be read. The recorded values across
    // sessions were 0.95, 1.04, 2.63, 3.46 and 11.68, and nothing said whether a
    // worst-case of 11.68 is a real boundary artefact or the largest legitimate
    // step in a particular passage -- and the difference decides whether the seam
    // needs fixing at all. A maximum answers "has this ever happened"; a
    // distribution answers "how often, and is this seam worse than the others".
    //
    // Percentiles rather than a histogram because the question is about ordering,
    // not about shape, and a fixed set of quantiles is comparable between runs
    // without a bin-width argument. The buckets below are logarithmic because the
    // ratio spans two orders of magnitude between a clean stream and a broken one.
    //
    // `seamCount` is the sample count for these quantiles, and it is already
    // reported -- so a percentile can always be checked against how many seams it
    // was computed from.
    double seamQuantiles[kSeamQuantiles] = {};
    uint64_t seamHistogram[kSeamBuckets] = {};
    uint64_t seamRatioTotal = 0.0;  // fixed point, thousandths, to keep it integral
    // Every seam's ratio, kept only if the count is small enough to be useful. The
    // point is to have the raw numbers when something looks wrong, and a cap keeps
    // that bounded: 4096 seams is 35 minutes at this buffer size, which is long
    // enough to catch a periodic artefact and short enough not to be a leak.
    float seamSample[kSeamSamples];
    size_t seamSampleCount = 0;
    double worstSeamSlew = 0.0;
    uint64_t seamCount = 0;

    // ---- stream state ----
    //
    // These live on the handle, not in the play function, and that placement is
    // load-bearing. The host renders in ~200 ms slices, each a separate
    // NAE_PlaySine call on the same handle. When this state was local, every
    // slice began with an empty fifo, sourcePos at 0 and the tone's phase at 0 —
    // so a 1 kHz test tone restarted every 200 ms, which puts a phase
    // discontinuity (a click) at 5 Hz, and real audio would have replayed from
    // the top of the file five times a second.
    //
    // A handle is one playback session. "Play for 200 ms more" is the correct
    // meaning of the call, and these are what make it mean that.
    std::vector<float> fifo;
    int64_t sourcePos = 0;
    int64_t phase = 0;

    // Where `sourcePos` was zeroed, in source frames.
    //
    // A seek resets `sourcePos` and then the decoder picks up from the new place,
    // so `sourcePos` counts frames played *since the seek*, not from the start of
    // the file. Reporting that directly would put a progress bar back at zero
    // after a jump to 3:20 and let it crawl forward from there while the music is
    // already three minutes in. Adding the base makes position mean "where in the
    // track the listener is", which is the only thing the UI wants to show.
    int64_t seekBaseFrames = 0;

    // ---- file source ----
    //
    // Present when playback was opened on a file rather than a tone. Null means
    // the test tone is the source, and only then is the synthesis path taken.
    //
    // The decoder is owned here so its FFmpeg types never reach the C ABI, for
    // the same reason the resampler is: a caller that had to know about an
    // AVFrame could not survive an FFmpeg upgrade.
#ifdef NEIRO_HAVE_FFMPEG
    std::unique_ptr<neiro::Decoder> decoder;

    // Decode-ahead. The producer owns the decoder and fills `decodeRing`; the
    // render thread only consumes source frames. The producer never touches the
    // device, and the render thread never calls readExact while it is running.
    std::mutex decodeMutex;
    std::condition_variable decodeCv;
    std::deque<float> decodeRing;
    bool decodeEof = false;
    bool decodeQuit = false;
    bool decodeRunning = false;
    std::thread decodeThread;

    // The file's channel count before the engine's downmix, kept so the UI can
    // report what the user's file actually is.
    uint32_t sourceChannels = 0;
    // Optional source->device routing. When active, the decoder keeps the file's
    // channel count and topUp routes before the resampler/fifo.
    neiro::RoutingProcessor routing;
    bool routingActive = false;
    std::vector<float> routingIn;
    std::vector<float> routingOut;
    // How far to keep feeding the device: the file's length plus half a second.
    //
    // Not the file's true length, despite what this comment used to say. The
    // slack exists so that a container whose duration estimate is slightly short
    // cannot cut a phrase off mid-word; the cost of being slightly long is a
    // moment of silence at the end. Playback pads out to here.
    int64_t totalSourceFrames = 0;
    // The file's true length in frames. 0 for a tone, where there is no end.
    //
    // Two values rather than one because they answer two questions: "how far may
    // playback run" and "how long is the source". `sourcePos` advances through
    // the padded tail, so anything that reports a position has to divide by this
    // one -- see NAE_Position.
    int64_t sourceFramesReal = 0;

    // 文件自己的信息，播放期间不变。
    //
    // 之前只有 `sourceChannels` 和时长被 open 的 out-param 带出去，其余全部在
    // open 时丢掉——于是 `track` 事件只带 channels 和 durationMs，本地文件的
    // fact strip 少了九个值。
    //
    // 而这些字段本来就在手上：`decoder->info()` 有采样率、位深、编码、声道。所以
    // 这不是「数据不存在」，是**没有人把它带上来**。存在而没上报，和不存在一样让
    // 界面只能说不知道——而后者会让人以为播放器没读那个字段。
    int32_t fileSampleRate = 0;
    /// 文件的位深，位/采样。0 表示容器没有报。
    ///
    /// 这是**容器声称的**，不是引擎实际用的：解码器总是产出 float32（见 nae.h
    /// 里 `bitPerfect` 那一段的说明）。所以它描述用户的音乐，不描述信号通路——
    /// 两者必须分开报。混在一起就会把「文件是 24 bit」说成「设备收到 24 bit」，
    /// 而这个引擎对那个说法有专门的、更严格的定义。
    int32_t fileBitsPerSample = 0;
    /// Bits the source actually carries (`StreamInfo::raw_bits_per_sample`).
    ///
    /// Separate from `fileBitsPerSample` above because that one is the container
    /// the decoder produces: a 24-bit FLAC and a 32-bit WAV both report 32 there.
    /// This is the number that decides whether the float32 path can carry the
    /// file without dropping bits. 0 means the demuxer did not say.
    int32_t fileRawBits = 0;
    /// The open-time verdict, before the source's depth is considered.
    ///
    /// `applySourcePrecision` only clears, so a gapless swap has to start from
    /// this rather than from the previous track's verdict.
    bool sourceBitPerfect = false;
    neiro::PerfectReason sourceReason = neiro::PerfectReason::DeviceCannotCarryFormat;
    /// 文件时长，毫秒。0 表示容器没有报。
    ///
    /// 之前时长只经由 open 的 out-param 传出去，而那个值只被**打开时**调用 open
    /// 的那一方读到——界面上过了几秒再问就问不到了。句柄上留一份是为了让
    /// `NAE_QueryTrackInfo` 在播放期间任何时刻都能回答。
    int32_t fileDurationMs = 0;
    /// 容器或编码名，如 "flac" / "aac"。空表示没有。
    ///
    /// 固定长度内嵌数组而不是 std::string：这个值要经过 C ABI，而 std::string 的
    /// 布局不是 ABI。它也不需要释放——与 open 的 out-param 里那个 `char*` 刻意
    /// 不同，那个由引擎分配、调用方 `NAE_FreeString` 释放。两处都像对方管内存是
    /// 一种很难查的错，所以做法必须明显不同。
    char fileCodec[32] = {};

    // The next file, opened ahead of time. Same source rate and channel count
    // only: the device stream and the resampler are not rebuilt for it.
    std::unique_ptr<neiro::Decoder> nextDecoder;
    int64_t nextTotalSourceFrames = 0;
    int64_t nextSourceFramesReal = 0;
    int32_t nextFileChannels = 0;
    int32_t nextFileSampleRate = 0;
    int32_t nextFileBitsPerSample = 0;
    int32_t nextFileRawBits = 0;
    int32_t nextFileDurationMs = 0;
    char nextFileCodec[32] = {};
    std::string nextDecodePath;
    NAE_ReplayGainInfo nextRgInfo{};
    std::vector<float> nextPrefetched;
    size_t nextPrefetchedFrames = 0;
    std::vector<float> prefetched;
    size_t prefetchedPos = 0;
    size_t prefetchedFrames = 0;

    // Set when the decoder ran out, so a short final block plays as silence
    // rather than as a truncated or repeated tail.
    bool sourceExhausted = false;
    // Set once the fifo has hit its ceiling and production has been stopped.
    bool fifoCapped = false;
#endif

    /// Increments when a preloaded file takes over. A tone stays at 0.
    uint64_t trackSerial = 0;
    /// Set by NAE_SkipToNext, consumed by the next render slice.
    ///
    /// The switch has to happen where the fifo and the resampler are owned -- on
    /// the render thread, between slices -- so the request is a flag rather than
    /// a swap performed by the caller.
    std::atomic<bool> skipRequested{false};

    // ---- DoP ----
    //
    // DoP is a transport, not a decoder: raw DSD bits are packed into 24-bit
    // carrier words and written straight to the device. NAE_Play and the whole
    // float DSP chain are bypassed; volume/EQ would corrupt the marker.
    bool dopMode = false;
    std::unique_ptr<dsf::File> dopDsf;
    std::unique_ptr<dff::File> dopDff;
    uint64_t dopBlock = 0;
    uint32_t dopBlockFrames = 0;
    uint32_t dopFrameInBlock = 0;
    std::vector<uint8_t> dopRaw;
    size_t dopRawPos = 0;
    size_t dopRawBytes = 0;
    uint64_t dopDataOffset = 0;
    uint32_t dopCarrierRate = 0;
    int32_t dopStreamRate = 0;
    uint8_t dopMarker = 0x05;
    int64_t dopFramesPlayed = 0;
    int64_t dopFramesTotal = 0;
    /// Host-owned native DSD sink. `nativeWrite` is called from
    /// NAE_PlayNativeDsd with interleaved 1-bit bytes.
    bool nativeSinkMode = false;
    NAE_DsdWriteFn nativeWrite = nullptr;
    void* nativeContext = nullptr;
    bool nativePaused = false;
    bool nativeEnded = false;
    uint32_t nativeRate = 0;
    uint32_t nativeChannels = 0;
    int32_t nativeBitOrderMsbFirst = 0;
    double nativeDurationMs = 0.0;
    std::unique_ptr<neiro::dsd::Downrate> nativeDownrate;
    std::vector<uint8_t> nativeIn;
    std::vector<uint8_t> nativeOut;

    /// Bitstream downrate (null when the source rate is played as is).
    std::unique_ptr<neiro::dsd::Downrate> dopDownrate;
    std::vector<uint8_t> dopDownOut;
    std::vector<uint8_t> dopDownIn;
    /// Read index into dopDownOut. Erasing from the front once per DoP frame
    /// was O(n) per frame and measured 15 ms of memmove per second of audio;
    /// the buffer is compacted only when the read index passes half of it.
    size_t dopDownRead = 0;
    uint32_t dopPairChannel = 0;
};
#endif  // NEIRO_NAE_INTERNAL_H
