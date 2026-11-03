// Minimal WASAPI output engine behind a C ABI.
//
// Deliberately not a Node addon yet: this file is the part that has to be
// right on its own, so it can be tested from a plain executable before any JS
// runtime is involved. The N-API layer in napi_addon.cpp is a thin shell over
// exactly this interface.
//
// ---- contract for the calling side -----------------------------------------
//
// Three rules, all of which the Rust host depends on:
//
//  1. **No exceptions cross this boundary.** Every export is noexcept. Letting
//     a C++ exception unwind into a Rust frame is undefined behaviour, and in
//     practice means "the process dies somewhere unrelated". If an export does
//     allocate and the allocation fails, the export reports NAE_ERR_NO_MEMORY.
//     An escaping exception is caught and reported like any other error.
//
//  2. **Memory belongs to the side that allocated it.** Strings returned here
//     must be released with NAE_FreeString, never with free()/Rust's dealloc.
//     This matters because the DLL and the host may be built with different
//     toolchains and therefore different heaps. Handles follow the same rule:
//     NAE_Close them, do not drop them.
//
//  3. **Never guess the format.** Read the endpoint's native format and wrap
//     it as WAVEFORMATEXTENSIBLE. See the note in nae.cpp on why a hardcoded
//     48k/24 fails with AUDCLNT_E_UNSUPPORTED_FORMAT.

#pragma once

#include <cstddef>
#include <cstdint>

#include "nae_policy.h"

#ifdef __cplusplus
#define NAE_NOEXCEPT noexcept
extern "C" {
#else
#define NAE_NOEXCEPT
#endif

#define NAE_OK 0
#define NAE_ERR_COM 1
#define NAE_ERR_NO_DEVICE 2
#define NAE_ERR_NO_NATIVE_FORMAT 3
#define NAE_ERR_ACTIVATE 4
#define NAE_ERR_INITIALIZE 5
#define NAE_ERR_NO_MEMORY 6
#define NAE_ERR_NOT_OPEN 7
// A caller passed something impossible: no file, a negative duration, a seek on
// a stream that cannot seek. Distinct from NAE_ERR_CANNOT_OPEN because "you asked
// for something that cannot exist" and "the thing you asked for is broken" call
// for different messages to the user.
#define NAE_ERR_INVALID_ARGUMENT 8
// A file could not be read, or could not be decoded. NAE_GetLastErrorText says
// which, in words.
#define NAE_ERR_CANNOT_OPEN 9
// The output device went away mid-playback: it was unplugged, disabled, or
// preempted by another application in exclusive mode.
//
// Its own code, and not NAE_ERR_INITIALIZE with a message, because the two call
// for opposite behaviour. Every other failure here means "this stream could not
// be delivered" and the caller should surface an error and stop. This one means
// "the device is temporarily gone and the stream is still good" -- playback
// paused, position intact, and the same handle resumes when the caller has a
// device to give it again.
//
// Before this existed the render loop reported a lost device as
// NAE_ERR_INITIALIZE plus a text containing AUDCLNT_E_DEVICE_INVALIDATED, which
// made the difference a fact only available to a host that already knew the
// Windows error code by heart. A host that did not treat it as an error at all
// had no way to tell.
#define NAE_ERR_DEVICE_LOST 10
/** The parameter exists but is fixed for the open stream; reopen to change it. */
#define NAE_ERR_REQUIRES_REOPEN 11
/** The endpoint cannot carry the format at all (e.g. the DoP carrier rate). */
#define NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT 12

typedef struct NAE_Handle NAE_Handle;

/** Engine-wide capabilities, as a static JSON string. Never NULL. */
const char* NAE_GetEngineCapabilities(void) NAE_NOEXCEPT;

/** Human-readable name for a NAE_* error code. Never NULL. */
const char* NAE_GetLastError(void) NAE_NOEXCEPT;

/** Last HRESULT seen by any call on this thread. */
int32_t NAE_GetLastHresult(void) NAE_NOEXCEPT;

// ---- device enumeration ----------------------------------------------------

/**
 * Enumerate active render endpoints as JSON:
 *   [{"id":"...","name":"...","sampleRate":48000,"bitsPerSample":16,
 *     "channels":2,"exclusive":true}, ...]
 * Caller owns the returned buffer; free it with NAE_FreeString.
 */
char* NAE_EnumerateDevices(void) NAE_NOEXCEPT;
void NAE_FreeString(char* s) NAE_NOEXCEPT;

// ---- playback --------------------------------------------------------------

/**
 * Open `deviceId` in exclusive mode at the device's native format and start a
 * 1 kHz sine at -20 dBFS.
 *
 * Fills the out-params with the format actually in effect so the caller can
 * report an honest signal path. On failure returns a NAE_* code and leaves the
 * out-params zeroed.
 */
int32_t NAE_OpenExclusiveSine(const char* deviceId, NAE_Handle** out, int32_t* sampleRate,
                             int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                             int32_t* isFloat, double* latencyMs) NAE_NOEXCEPT;

/** Begin a single playback pass of `milliseconds`. Blocks. */
int32_t NAE_PlaySine(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT;

/** Stop and release. Safe to call with NULL. */
void NAE_Close(NAE_Handle* handle) NAE_NOEXCEPT;

// ---- output rate modes ------------------------------------------------------

/** How the output rate is chosen. Mirrors OutputMode in output_mode.h. */
#define NAE_OUTPUT_NATIVE 0 /**< Source rate as-is. Bit-perfect when it matches. */
#define NAE_OUTPUT_DEVICE 1 /**< Resample to the device rate. The default. */
#define NAE_OUTPUT_FIXED 2  /**< Resample to an explicit rate. */

/** Resampler quality tiers. */
#define NAE_QUALITY_FAST 0     /**< soxr "quick". For scrub previews. */
#define NAE_QUALITY_HIGH 1     /**< soxr "high", 20-bit. The default. */
#define NAE_QUALITY_VERY_HIGH 2 /**< soxr "very high", 28-bit. */

/**
 * Why the path is not bit-perfect.
 *
 * These are the ABI form of `neiro::PerfectReason`, and the identifiers the
 * frontend's reason table is keyed on. `scripts/verify-reasons.mjs` greps the
 * engine sources for every `NAE_REASON_` that appears and fails if the frontend
 * table does not carry it, so a new reason cannot reach the interface as a bare
 * identifier with nothing to show for it.
 */
#define NAE_REASON_BIT_PERFECT 0
#define NAE_REASON_RESAMPLED_TO_DEVICE_RATE 1
#define NAE_REASON_RESAMPLED_TO_FIXED_RATE 2
#define NAE_REASON_DITHERED 3
#define NAE_REASON_CHANNEL_CONVERTED 4
#define NAE_REASON_DEVICE_CANNOT_CARRY_FORMAT 5
#define NAE_REASON_RESAMPLED_TO_SUPPORTED_RATE 6
/**
 * The stream was opened in shared mode, so the Windows mixer is in the path.
 *
 * Its own code because **it is the listener's choice, not a fault**. Every other
 * reason points at something that went wrong or at a control that would change the
 * outcome; this one is a route they picked on purpose, in exchange for letting
 * other programs make sound at the same time. The interface has to be able to say
 * that, which it cannot if the code says "resampled to device rate".
 *
 * The product decision (2026-10-01) is that shared mode does not pursue
 * bit-perfectness. Whether Windows would in fact preserve the samples is
 * **untested** -- for a long time there was no way to ask, because no shared open
 * path existed. So the claim is made in neither direction: `bitPerfect` is 0, and
 * the reason states the fact that is known, which is that the mixer is downstream.
 */
#define NAE_REASON_SHARED_MODE 7
/**
 * The output is not unity gain. A digital volume other than 1.0 changes the
 * samples, and a ramp that has not settled is still changing them.
 *
 * Kept last in the priority order: a stream that is also shared or resampled
 * reports the more fundamental reason first. This code exists for the case where
 * volume is the only thing in the way.
 */
#define NAE_REASON_VOLUME_NOT_UNITY 8
/**
 * The equaliser is active. It changes the frequency response, so the samples
 * are not the source's.
 *
 * Ranked above volume: an EQ that is on is the more structural of the two, and
 * naming it sends the listener to the right control.
 */
#define NAE_REASON_EQ 9
/**
 * Crossfeed is active. It mixes a delayed, low-passed copy of each channel into
 * the other, which is a deliberate change to the stereo image.
 *
 * Ranked after EQ and before volume: it is processing of the signal, and the
 * listener should be sent to the crossfeed control rather than the volume one.
 */
#define NAE_REASON_CROSSFEED 10
/**
 * ReplayGain is applying a non-unity gain. It is a gain, not dynamics, but it
 * still changes the samples; a mode that resolves to 0 dB is reported as
 * bit-perfect because no multiply happens.
 */
#define NAE_REASON_REPLAYGAIN 11
/**
 * The convolver is active. It replaces the signal with a finite impulse
 * response, which is the most structural processing in the chain.
 */
#define NAE_REASON_CONVOLVER 12
/** Crossfade is active: the end of one file is mixed with the start of the next. */
#define NAE_REASON_CROSSFADE 13

/**
 * One reading of the output chain.
 *
 * `structSize` comes first and is filled in by the engine, so a host built
 * against a newer header than the DLL it loaded can tell which fields exist
 * instead of reading past the end of what was actually written. Every host
 * zeroes the structure before the call, and reads a field only when
 * `structSize` covers it.
 */
typedef struct NAE_SignalPath {
    /**
     * In: the size of the struct this caller was compiled against. Out: the
     * size the engine actually wrote, which may be larger than the input if
     * this header is newer than the DLL.
     *
     * The engine copies out only `min(in, its own size)` bytes, so a host built
     * against an older, smaller header cannot have its stack overrun by fields
     * it does not know about. Leave it zeroed to mean "this build's size".
     */
    uint32_t structSize;

    /* ---- what the file is ---- */
    int32_t sourceRate;
    int32_t sourceChannels;

    /* ---- what the device is doing ---- */
    int32_t sampleRate;
    int32_t channels;
    int32_t bitsPerSample;
    int32_t validBits;
    int32_t isFloat;
    int32_t resampling;
    int32_t dithered;
    int32_t bitPerfect;
    /** A NAE_REASON_* value. 0 when `bitPerfect`. */
    int32_t reason;
    int32_t exclusive;

    /**
     * Latency, split.
     *
     * `bufferLatencyMs` is the audio this application has queued and not yet
     * heard: WASAPI's buffer, divided by the rate it is running at.
     * `driverLatencyMs` is the device's own reported stream latency, which is
     * downstream of us and which we do not control. One number for the two is
     * worse than either, because it is not clear which of them is large.
     */
    double bufferLatencyMs;
    double driverLatencyMs;
    double totalLatencyMs;
    /** The same buffer latency in frames, which is the number that scales. */
    int32_t bufferFrames;

    /* ---- render-thread load ----
     *
     * Counted with lock-free counters on the audio thread. `deadlineMissCount` is
     * how many callbacks ran past the time WASAPI allowed them, which is the
     * event a listener hears as a click.
     */
    uint64_t callbackCount;
    uint64_t deadlineMissCount;
    double peakCallbackMs;

    /* ---- faults ----
     *
     * All zero on a healthy stream. `deviceLostCount` and `recoveryCount` are the
     * two that matter when a device is disabled mid-track: the first says it
     * happened, the second says the application came back by itself.
     */
    uint64_t underrunCount;
    uint64_t dropCount;
    uint64_t deviceLostCount;
    uint64_t recoveryCount;

    /* ---- the rate the driver forced ----
     *
     * `coercedToSupportedRate` is set when the mode asked for a rate the endpoint
     * refused and the closest one it would accept was used instead. It is
     * separate from `reason` because the interface has to say two different
     * things: the reason code says what happened to the samples, and this says
     * that the listener's setting was not what chose it. Without it, a DSD64 file
     * on a 192 kHz endpoint reads as "you chose the wrong output mode".
     */
    int32_t coercedToSupportedRate;

    /* ---- what actually reached the device, measured ----
     *
     * `fifoPeak` is the largest absolute sample taken out of the render fifo, in
     * the [-1, 1] domain the pipeline carries. `writtenPeak` is the largest
     * absolute value written into the device's own buffer, in that container's
     * units.
     *
     * Two numbers rather than one because the question they answer is "where does
     * the level go", and the two halves have opposite fixes. A `fifoPeak` near 1.0
     * with a `writtenPeak` near 1/256 of full scale means the loss is in the
     * quantise-and-pack step -- a depth mismatch, most likely the engine writing
     * 4 bytes per sample into a buffer the device opened for 2. Both low means the
     * decoder produced quiet audio and the output path is innocent.
     *
     * `requestedBits` is `wBitsPerSample` as the engine asked for it;
     * `grantedBits` is what came back from `Initialize`. A difference means the
     * device changed the format under us, which is exactly the case where writing
     * the requested depth would be writing the wrong bytes.
     */
    float fifoPeak;
    float writtenPeak;

    /// Peak read back out of the device's own buffer, interpreted the way the
    /// device interprets it.
    ///
    /// `writtenPeak` is computed before packing, so a wrong shift or a wrong byte
    /// offset still reports a perfect number -- which is how a 256x level error
    /// stayed invisible here for a day. This figure looks at the bytes that are
    /// actually in the buffer.
    float devicePeak;
    int32_t requestedBits;
    int32_t grantedBits;

    /// Device frames the render loop had to invent because the fifo ran dry
    /// part-way through a buffer.
    ///
    /// Zero means the decoder kept up. Anything else is the decoder failing to
    /// keep up in real time, and it is the measurement behind "the output
    /// burbles": the bubbled frames were previously released short, and WASAPI
    /// plays back whatever the last fill left in the unwritten remainder -- so
    /// the fault presented as a periodic stale-audio artefact while
    /// `underrunCount` and `dropCount` both stayed at zero, because releasing a
    /// short buffer was never counted as an event.
    uint64_t padFrameCount;

    /// Worst wall-clock gap between two consecutive device-buffer writes, and the
    /// device buffer's own duration in milliseconds.
    ///
    /// `maxBufferGapMs > bufferDurationMs` means a buffer was handed over late and
    /// the device ran dry waiting for it. This is the only figure that can see that:
    /// every other counter describes what the engine wrote, and a buffer written
    /// correctly but late is identical to one written on time.
    double maxBufferGapMs;
    double bufferDurationMs;

    /// Worst time the render thread spent inside `topUp()` -- which decodes -- during
    /// a single device period.
    ///
    /// This engine decodes from inside the render loop. Twilight Echo's engine runs
    /// the decoder on its own thread (`AudioPipeline.cpp:1020`) and is clean on the
    /// same device, format and file. Whatever a decode costs here is spent out of
    /// the runway the device granted, so this is the number that has to be small and
    /// currently has no reason to be.
    double maxDecodeStallMs;

    /// Worst time from reading `GetCurrentPadding` to handing the buffer over.
    double maxFillMs;

    /// Worst buffer's lateness against its runway, and how many were late.
    ///
    /// The runway is `padding / rate`: what the device still has queued, expressed
    /// as the time it can keep playing without more. A positive `maxLateMs` is the
    /// device having run out of audio while this loop was still filling, so the
    /// listener heard the previous buffer instead -- which is what a bubble is, and
    /// is invisible to every other figure here because the fifo is never empty, the
    /// wait never times out, and each write is individually correct.
    double maxLateMs;
    int64_t lateCount;

    /// Worst step across a device-buffer boundary, over the signal's peak.
    ///
    /// The signal's own largest adjacent-sample step, normalised, is reported
    /// alongside it as seamSlewRef. A waveform that simply continued has a seam
    /// step of the same order as its slew; a break is several times larger. That
    /// makes the ratio self-calibrating, so no threshold has to be chosen for one
    /// piece of music.
    ///
    /// This is the instrument the bubbling needed and did not have. That fault lived
    /// in the seam between two calls, and every other figure here is measured inside
    /// one call.
    double worstSeamRatio;

    /// The slew a seam step should be compared against, normalised the same way.
    double seamSlewRef;

    /// Device-buffer seams examined.
    uint64_t seamCount;

    /// Device frames actually delivered, per second of wall clock.
    ///
    /// Compare it against the negotiated rate. Below it, the device is being drained
    /// more slowly than real time, and the fifo accounting balances either way, so
    /// nothing else in this struct can see the difference.
    ///
    /// Counts silence handed over during a pause, deliberately. The claim is device
    /// frames per second of wall clock, and the device drains silence at the rate it
    /// drains audio -- a slow device clock is as visible through a pause as through
    /// music. Excluding it made a run of pause/play cycles read half speed, which is
    /// a claim about playback that was not true. Whether a pause happened is
    /// `silenceFramesWritten`'s job; this field does not carry both.
    ///
    /// Two earlier definitions were wrong, and both announced it in the number rather
    /// than in a test. Subtracting the paused time from the elapsed denominator
    /// excluded almost nothing, because the loop waits for the device *before* it
    /// reaches the paused branch. Discarding the window on pause reported 0 whenever
    /// playback was interrupted more often than every 500 ms -- and 0 is a claim, not
    /// an absence of measurement.
    double deviceRateActual;

    /// Device frames handed to the device as silence because the stream was paused.
    ///
    /// Counted separately rather than folded into `deviceRateActual` on purpose.
    /// The device really is consuming them, so they are part of how much wall clock
    /// elapsed -- but they are not playback, and folding them in would make a long
    /// pause look like a long stretch of correctly-timed music. A UI that reports
    /// "playing at 48 kHz" while this climbs is reporting a pause as playback.
    ///
    /// Non-zero across a NAE_Pause/NAE_Resume pair is the evidence that the pause
    /// actually fed silence rather than doing nothing. `NAE_Position` not moving
    /// across the same pair is the evidence that the source did not.
    uint64_t silenceFramesWritten;

    // ---- the seam ratio's distribution, not just its maximum ----
    //
    // Appended at the end, not next to `worstSeamRatio`, because this struct is
    // read field-by-field by position and inserting in the middle silently renames
    // every figure after the insertion point. The `static_assert` chain below caught
    // exactly that on the first build of this change, which is what those assertions
    // are for -- a total-size check would not have, since the new fields are all
    // appended and the mistake was purely about order.
    //
    // A maximum with no distribution cannot be read. The worst seam over a session
    // is one sample out of thousands, and whether it is a defect depends on where it
    // sits in the set: 11.68 is a click if the median is 0.05 and a loud passage if
    // the median is 9. The maximum alone cannot tell those apart.
    //
    // `seamCount` above is how many seams these figures describe, so every one can
    // be checked against its sample count. `seamSampleCount` is how many the
    // quantiles were computed from -- lower, because that set is capped.
    /** Mean ratio over every seam, not just the retained ones. */
    double seamMean;
    /**
     * Eight quantiles of the retained sample, minimum to maximum.
     *
     * The last is the maximum *of the retained sample*, so comparing it against
     * `worstSeamRatio` says whether the cap truncated anything. A quantile over a
     * capped prefix is not the same number as one over everything.
     */
    double seamQuantiles[8];
    /** How many seams the quantiles were computed from. */
    uint32_t seamSampleCount;
    /**
     * Explicit padding to the 8-byte boundary `seamHistogram` needs.
     *
     * `seamQuantiles` is 64 bytes of `double`, so `seamSampleCount` lands at offset
     * 64 and the `uint64_t` array after it must start at 72. There are four bytes of
     * padding whether or not anybody names them, and naming them is what lets the C
     * declaration and the Rust binding state the same layout instead of one of them
     * happening to be right.
     */
    uint32_t seamHistogramPad;
    /**
     * Log-spaced counts, -2 to +2 decades in 0.1-decade buckets.
     *
     * Logarithmic because the ratio spans two orders of magnitude between a clean
     * stream and a broken one, and a linear histogram over that range is one tall
     * bar and sixty empty ones. `seamHistogramMax` is the bucket count, so a reader
     * never has to hard-code 41.
     */
    uint64_t seamHistogram[41];
    /** Number of valid buckets in `seamHistogram`. */
    uint32_t seamHistogramMax;
    /**
     * The per-channel delay the render loop is actually applying, in frames.
     * `channelDelayMaxFrames` is the ring size, which is one more than the
     * largest delay so that a delay equal to it still delays.
     *
     * Read by channel_check: a probe that re-implements the ring arithmetic
     * cannot fail against the bug it exists for.
     */
    int32_t channelDelayMaxFrames;
    /** Trailing padding to the struct's 8-byte alignment, named so it is stated. */
    uint32_t tailPad;
    uint32_t tailPad2;
} NAE_SignalPath;

// The Rust side of this struct is filled by position. These pin the neighbours so
// that adding a field here and forgetting to add it there is a build error rather
// than a set of plausible wrong numbers. A size check would not do: two adjacent
// doubles swapped leave the size identical. See the matching assertions in
// rust/src/ffi.rs, which is where the mistake was actually made.
static_assert(offsetof(NAE_SignalPath, bufferDurationMs) ==
                  offsetof(NAE_SignalPath, maxBufferGapMs) + sizeof(double),
              "bufferDurationMs must follow maxBufferGapMs; rust/src/ffi.rs pins the same order");
static_assert(offsetof(NAE_SignalPath, maxDecodeStallMs) ==
                  offsetof(NAE_SignalPath, bufferDurationMs) + sizeof(double),
              "maxDecodeStallMs must follow bufferDurationMs");
static_assert(offsetof(NAE_SignalPath, maxFillMs) ==
                  offsetof(NAE_SignalPath, maxDecodeStallMs) + sizeof(double),
              "maxFillMs must follow maxDecodeStallMs");
static_assert(offsetof(NAE_SignalPath, maxLateMs) ==
                  offsetof(NAE_SignalPath, maxFillMs) + sizeof(double),
              "maxLateMs must follow maxFillMs");
static_assert(offsetof(NAE_SignalPath, lateCount) ==
                  offsetof(NAE_SignalPath, maxLateMs) + sizeof(double),
              "lateCount must follow maxLateMs");
static_assert(offsetof(NAE_SignalPath, worstSeamRatio) ==
                  offsetof(NAE_SignalPath, lateCount) + sizeof(int64_t),
              "worstSeamRatio must follow lateCount");
static_assert(offsetof(NAE_SignalPath, seamSlewRef) ==
                  offsetof(NAE_SignalPath, worstSeamRatio) + sizeof(double),
              "seamSlewRef must follow worstSeamRatio");
static_assert(offsetof(NAE_SignalPath, seamCount) ==
                  offsetof(NAE_SignalPath, seamSlewRef) + sizeof(uint64_t),
              "seamCount must follow seamSlewRef");

// These two were previously unpinned. They happened to sit at the right offsets
// because a double and a uint64_t are both 8 bytes on this target, so the
// preceding assertion passed by arithmetic coincidence rather than by anything
// checking the order. Stated as the types they are, so that a target where they
// differ would have caught it.
static_assert(offsetof(NAE_SignalPath, deviceRateActual) ==
                  offsetof(NAE_SignalPath, seamCount) + sizeof(uint64_t),
              "deviceRateActual must follow seamCount");
static_assert(offsetof(NAE_SignalPath, silenceFramesWritten) ==
                  offsetof(NAE_SignalPath, deviceRateActual) + sizeof(double),
              "silenceFramesWritten must follow deviceRateActual");

/**
 * The rates an endpoint will open in exclusive mode, measured rather than assumed.
 *
 * WASAPI has no enumeration API for this, so it is found the only way it can be:
 * by trying each candidate rate on a throwaway client and keeping the ones the
 * driver accepts. That is slow enough to be worth caching and worth telling the
 * interface about -- a UI that offers a fixed-rate picker has to know which
 * entries will actually work, or it sends the listener to a setting that cannot
 * be honoured.
 *
 * `rates` is filled with up to `capacity` rates, ascending. `outCount` receives
 * the total number found, which may exceed `capacity`; when it does, the list is
 * truncated but the count is honest, so a caller can grow the buffer and retry.
 *
 * A null handle, or a device that could not be opened, yields NAE_OK with
 * `*outCount` set to 0. Zero supported rates means "not probed", never "none
 * work": the engine then behaves exactly as it did before this query existed.
 */
int32_t NAE_QuerySupportedRates(NAE_Handle* handle, uint32_t* rates, uint32_t capacity,
                                uint32_t* outCount) NAE_NOEXCEPT;

/**
 * Read the current signal path, latency and counters from an open handle.
 *
 * Separate from the open call rather than returned by it because the counters
 * only mean anything over time, and a host that polls this between callbacks gets
 * a rate; a host that reads it once at open gets a zero.
 *
 * Returns NAE_OK and writes a zeroed structure for a null handle, so a caller
 * polling a stream that has not opened yet gets a well-formed "nothing yet"
 * rather than an error it has to special-case.
 */
int32_t NAE_QuerySignalPath(NAE_Handle* handle, NAE_SignalPath* out) NAE_NOEXCEPT;

/**
 * What the output actually contains, read from a tap inside the render loop.
 *
 * # Why this exists
 *
 * The player draws a spectrum. On the Web Audio route it reads an AudioBuffer,
 * which is the *file* at the *file's* rate. On the exclusive route the samples
 * never enter the webview at all, so there was nothing to read and the visualiser
 * was simply blank -- and the reason was not obvious, because the engine's own
 * header had listed "the spectrum tap" among the things it did. There was no tap.
 *
 * So this is the tap, and it is deliberately placed *after* resampling and inside
 * the engine. That placement is the whole point: the bins describe the signal the
 * device is being fed, at the rate it is being fed, not the file. A DSD64 track
 * opened on a 192 kHz endpoint reports its ultrasonic content here, which is
 * precisely the thing a player that calls itself HiFi exists to show.
 *
 * # What the numbers are
 *
 * `bins` are linear magnitudes of a Hann-windowed FFT, so a full-scale sine
 * reads about 1.0 in its bin and the reference for a dB display is 1.0. A Hann
 * window is applied rather than a rectangular one because a rectangular window
 * leaks a full-scale tone across the whole spectrum by up to 13 dB, which on a
 * dB-scaled display is a floor that is not there.
 *
 * The window is taken on channel 0 only. That is a choice, not an oversight: the
 * Web Audio route reads `getChannelData(0)` as well, so both routes describe the
 * same channel and the two pictures are comparable. A stereo image's difference
 * between the channels is not shown.
 *
 * # Staleness
 *
 * `framesFed` counts every device frame the tap has seen and is the only way to
 * tell a live reading from a leftover one. It advances during a pause, because the
 * pause branch feeds the tap actual silence rather than leaving the previous
 * window in place: a paused player showing a frozen spectrum that looks alive is
 * claiming to be playing when it is not.
 */
typedef struct NAE_SpectrumInfo {
    /** Set to `sizeof(NAE_SpectrumInfo)` by the caller. See NAE_SignalPath. */
    uint32_t struct_size;
    /** Window length the FFT used. Always a power of two. */
    uint32_t fftSize;
    /** Magnitudes written to `bins`: `fftSize / 2`. */
    uint32_t binCount;
    /** The rate the window was captured at, which is the device's rate. */
    uint32_t sampleRate;
    /** Bits the device receives, so a dBFS display can be scaled to what is
     *  actually delivered rather than to the container.
     *
     *  The explicit `uint32_t` is not decoration. `peak` follows it and is a
     *  `double`, and on the `x86_64` MSVC ABI a `double` is already 8-aligned --
     *  but the five `uint32_t` fields before it leave the struct at offset 20, so
     *  `peak` would be padded to 24. The padding is real whether or not anyone
     *  writes it down, and a host binding that assumed the fields were packed
     *  would read `peak` as `validBits` and everything after it as garbage. The
     *  Rust binding asserts these offsets for the same reason `NAE_SignalPath`'s
     *  do, and that assertion is what caught the mismatch: it is the only thing
     *  here that could. */
    uint32_t validBits;
    /** Explicit padding to the 8-byte boundary `peak` needs. Written as a field
     *  rather than left implicit so that the C and Rust declarations state the
     *  same layout instead of one of them happening to be right. */
    uint32_t reserved;
    /** Peak magnitude in the window, 0..1. */
    double peak;
    /** Broadband RMS of the window before windowing, 0..1. */
    double rms;
    /** Total device frames the tap has seen. Changes between calls while audio
     *  is flowing; stops changing if the stream is not. */
    uint64_t framesFed;
} NAE_SpectrumInfo;

/**
 * Read the output tap. `bins` receives `binCount` linear magnitudes.
 *
 * `capacity` is in floats. A capacity below `binCount` truncates and still
 * reports the true `binCount`, so a caller can grow its buffer and retry without
 * the count lying about what was available.
 *
 * Returns NAE_OK and a zeroed structure for a null handle, like
 * NAE_QuerySignalPath: polling a stream that has not opened yields a
 * well-formed "nothing yet" rather than an error to special-case. `framesFed` of
 * zero is that answer, and it is distinguishable from a real reading of silence
 * because a real one has a non-zero `framesFed`.
 */
int32_t NAE_QuerySpectrum(NAE_Handle* handle, NAE_SpectrumInfo* info, float* bins,
                          int32_t capacity) NAE_NOEXCEPT;

/**
 * 一段时域包络，供节拍（BPM）检测使用。
 *
 * # 为什么需要它，而不能复用频谱
 *
 * 节拍是时域现象：它存在于相邻样本之间的**时间**结构里，频谱的 2048 个 bin 里没有
 * 这个信息。你不能从频谱反推出包络——那是一次不可逆的有损变换，正是这个项目
 * 反复记录的那类错误（「从未测量的东西不能声称」）。
 *
 * 早先 exclusive 路由上没有 BPM，原因就是采样根本不进 webview，`tempo.js` 需要
 * 解码后的 PCM，而频谱 tap 给的是频域。现在时域和频域两个 tap 共用同一个环形
 * 缓冲：**不额外复制一份样本**，环形缓冲里本来就是时域数据，频谱是读它时做的
 * 变换。
 *
 * # 包络是怎么算的
 *
 * 5 ms 一帧（与 `src/scripts/audio/tempo.js` 的 `HOP_MS` 一致，否则两条路由会给出
 * 不同的 BPM），每帧存该帧的**平均绝对值**。用平均绝对值而不是 RMS 是因为便宜
 * 三分之一，而起始点检测要的是能量，不是相位——现有实现也是这么选的，保持一致
 * 才能让两条路由的答案可比。
 *
 * 只取通道 0，和频谱 tap 同样的理由：`tempo.js` 也只读第一个通道。
 *
 * # 读出来的数字代表什么
 *
 * `envelope[i]` 是第 `i` 帧的平均绝对值，范围 0..1（不是 dB）。`bpmFrameHopMs`
 * 告诉调用方相邻两帧隔多少毫秒，节拍周期可以直接由帧数乘出来，不需要调用方再
 * 猜采样率。
 *
 * `framesFed` 与频谱那边语义相同：它在推进说明是实时读数，不推进说明是残留。
 * 暂停时喂真静音，所以暂停时的包络是真实的静音包络，而不是上一段的残留。
 */
typedef struct NAE_EnvelopeInfo {
    /** 由调用方设为 `sizeof(NAE_EnvelopeInfo)`。见 NAE_SignalPath。 */
    uint32_t struct_size;
    /**
     * **环的容量**，帧。不是写进去的帧数——那个是 `frame_valid`。
     *
     * 这条注释原来写的是「写入 envelope 的帧数」，而实现里它一直报的是容量：环没写满
     * 的时候两者不相等，调用方按它去切片就会切到还没写过的零。
     */
    uint32_t frame_count;
    /** 包络的帧间隔，毫秒。相邻两帧之间的时间就是它。 */
    double frame_hop_ms;
    /** 包络的采样率，即设备速率。 */
    uint32_t sample_rate;
    /**
     * 显式填充。`sample_rate` 落在偏移 8，而 `frames_fed` 是 `uint64_t`，需要 8
     * 字节对齐，所以中间必然有 4 个字节的填充——写不写都在。
     *
     * 命名它的理由和频谱那边一样：让 C 的声明和 Rust 的绑定**陈述同一个布局**，
     * 而不是其中一边碰巧是对的。Rust 侧的偏移断言就是靠这条抓到了不一致。
     */
    uint32_t framesFedPad;
    /** 环形缓冲里一共喂过多少帧，用来判断读数是不是新的。 */
    uint64_t frames_fed;
    /** 包络窗口覆盖的真实时长，毫秒。由**容量**和间隔算出。 */
    double window_ms;
    /**
     * `envelope` 里**有效的**帧数，0..frame_count。
     *
     * 有效的那一段在**最前面**，而且**从最老的到最新**：`envelope[0]` 是能给出的最老
     * 的一帧，`envelope[frame_valid - 1]` 是最新的一帧。`[frame_valid, frame_count)`
     * 那一段**不写**，调用方不该读。
     *
     * 这个字段原来叫 `reserved`（尾部填充，把 sizeof 顶到 8 的整数倍）。用它是因为那
     * 4 个字节本来就在、布局不动；而两边的偏移断言都是**字面数字**，所以这一侧和
     * Rust 那一侧必须一起改，否则编译不过。
     */
    uint32_t frame_valid;
} NAE_EnvelopeInfo;

/**
 * 读取时域包络。
 *
 * `envelope[0..frame_valid)` 收到平均绝对值，**从最老的到最新**；`[frame_valid,
 * frame_count)` 不写。`capacity` 是调用方给的缓冲长度（float 计），小于 `frame_valid`
 * 时截断，但 `frame_valid` 仍然报真实的可用帧数——调用方据此扩大缓冲重试。
 *
 * **顺序是有保证的，而这不是细节。** 这一版之前它是「从写指针开始拷 filled 个」：
 * 写满之后那正好是最老的一帧，**没写满的时候那是还没写过的区域**——于是读者拿到一个
 * 「旋转 + 补零」的数组，而它没办法从 4096 个 float 里看出旋转了多少。实测
 * （`scripts/probe-tempo-window.mjs`）：非零段的起点在 1983 / 1243 / 420 / 0 之间动，
 * 而节拍估计器在同一段音频上给出的数在 72 ~ 200 之间跳。
 *
 * 句柄为空时返回 NAE_OK 和清零的结构，和 NAE_QuerySpectrum 一致：轮询一个还没
 * 打开的流应该得到一个格式完整的「还没有」，而不是一个需要调用方特判的错误。
 * `frames_fed` 为 0 就是那个答案，而且和「真实的静音包络」可以区分——后者
 * `frames_fed` 非 0。
 */
int32_t NAE_QueryEnvelope(NAE_Handle* handle, NAE_EnvelopeInfo* info, float* envelope,
                          int32_t capacity) NAE_NOEXCEPT;

/**
 * 整首波形的 peaks，交错存放（每桶两个 float：min, max），范围 -1..1。
 *
 * `bucket_count` 是**真正填了几桶**。`out` 收到 `bucket_count * 2` 个 float，
 * 所以 `capacity` 至少要 `buckets * 2`。桶数不够时返回
 * `NAE_ERR_INVALID_ARGUMENT` 而不是截断 —— 截断会让调用方按错的桶数去画。
 */
typedef struct NAE_PeaksInfo {
    /** 调用方填 `sizeof`。和其余几个查询结构同一个约定。 */
    uint32_t struct_size;
    /** 填了多少桶。0 表示没有可用的 peaks（没有文件 / 算失败）。 */
    uint32_t bucket_count;
    /** 1 = 覆盖了整个文件；0 = 没有（或解码中途结束）。 */
    uint32_t complete;
    /**
     * 保留。**原来是 `pending`**（引擎侧缓存里「别人正在算」）。
     *
     * 缓存挪到调用方之后它就没有含义了：没有 handle，引擎侧无处可放缓存，也就没有
     * 「别人正在算」这回事。留着这个 4 字节是因为它是 `duration_ms` 落在偏移 16
     * （8 的倍数）所必需的填充 —— 换个名字而不是删掉，那组布局断言就一个字不用改。
     */
    uint32_t reserved0;
    /** 文件的时长，毫秒。0 表示容器没有报。 */
    double duration_ms;
    /** 文件的采样率。 */
    uint32_t sample_rate;
    /** 文件的声道数（解码器交付的那个，可能已经过下混）。 */
    uint32_t channels;
    /**
     * 短窗的长度，毫秒。0 = **这两个响度指标没有算**（没有文件 / 解不开），
     * 这时下面两个数没有意义。
     *
     * **窗口的参数和数一起走**，不是让调用方自己记一个常量：读出来的那两个数只有在
     * 「多长的窗」说清楚之后才是数。写死 400 在界面那一侧就是同一个量、两处代码，
     * 而 `spectrum.js` 那边还有一份 JS 的镜像实现——三处各写一个 400，改一处就错。
     */
    uint32_t metrics_window_ms;
    /** 窗之间的步长，毫秒。和上面同一个理由。 */
    uint32_t metrics_hop_ms;
    /**
     * **响度**：所有短窗 RMS 电平的**中位数**，dBFS（0 = 满刻度）。
     *
     * 这是一个「这首歌有多响」的估计，不是 EBU R128 的 LUFS —— 见 `nae.cpp` 里
     * `computePeaksFromFile` 那一段（它写了门限和取分位数的口径，`spectrum.js` 的
     * 镜像实现必须逐字对齐）。
     */
    double loudness_db;
    /** **动态范围**：短窗 RMS 电平的 p95 − p10，dB。口径同上。 */
    double dynamic_range_db;
    /**
     * ITU-R BS.1770-4 / EBU R128。**和上面两个数不是一个量**：上面是中位数 RMS
     * 的估计，这里是带 K 加权、400 ms 块和双门限的标准值。`lufs_valid` 为 0 时
     * 四个数都没有意义（文件太短或解不开），界面应当什么都不画。
     *
     * 立体声两声道同时是 -23 dBFS 的 997 Hz 正弦读 -23 LUFS —— 标准把各声道相加，
     * 所以单声道满刻度正弦是 -3.01 LUFS，双声道是 0.0 LUFS。
     */
    double lufs_integrated;
    /** 最响的 400 ms 块，LUFS，不经过门限。 */
    double lufs_momentary_max;
    /**
     * Loudest 3 s window, LUFS, ungated.
     *
     * When `lufs_short_term_valid` is 0 this is 0, and 0 is not a measurement:
     * a file shorter than 3 s has no such window. Read the flag first; do not
     * paint 0 as "0 LUFS".
     */
    double lufs_short_term_max;
    /** EBU Tech 3342 loudness range, LU. Meaningless when short-term is invalid. */
    double lufs_range;
    int32_t lufs_valid;
    /** 1 = a 3 s window existed, so the two fields above are measurements. */
    int32_t lufs_short_term_valid;
} NAE_PeaksInfo;

/**
 * 算出整首波形的 peaks，`out` 收到 `buckets * 2` 个交错的 min/max（范围 -1..1）。
 *
 * # 为什么是**路径**而不是 handle
 *
 * 一开始写的是 `NAE_QueryPeaks(handle, ...)`，让调用方从**另一个线程**拿着 handle 调。
 * 而 `src-tauri/src/audio/service.rs:5-17` 写着应用那条更严的约定：
 *
 *     neiro_audio::Stream is `!Send` on purpose: an exclusive WASAPI stream
 *     belongs to the COM apartment of the thread that created it.
 *       * the stream is opened, used and closed on exactly one thread
 *       * commands are messages, not pointers, so nothing audio-related is ever
 *         handed between threads
 *
 * 从另一个线程用那个 handle 正是那条约定不允许的事，而且 handle 的生命周期归音频
 * 线程 —— 它随时可能 `NAE_Close`，于是另一个线程手里就是一个 use-after-free 窗口。
 *
 * 引擎这一侧其实为并发设计好了（`computePeaksFromFile` 不碰 `handle->decoder`，只
 * 开一个自己的 `neiro::Decoder`），但**应用那一侧更严，而应用才是用它的地方**。所以
 * 这个入口不碰 handle：它自己打开路径、自己解、自己关。
 *
 * 路径本来就是**消息**：`playing_path()` 已经在用同一件事
 * （`SLOT: OnceLock<Arc<Mutex<Option<PathBuf>>>>`）。
 *
 * # 缓存因此搬到调用方
 *
 * 这里**不缓存**：没有 handle 就没有地方放，而加一个全局缓存就是给这份文件引入一处
 * 共享可变状态（现在一处都没有）。调用方按路径缓存，那反而更好 —— 流重开之后缓存还在。
 *
 * # 代价
 *
 * 每次都**再解一遍文件**（引擎不持有整首解码后的 PCM，它流式解码）。实测
 * （`scripts/probe-timeline-decode-cost.mjs`）：典型曲目约 0.5 秒，245 MB / 192 kHz
 * 约 1.6 秒。所以**它必须在调用方的后台线程上跑**，绝不能在 UI 线程上。
 *
 * # 语义与前端那支 `computePeaks` 逐字对齐
 *
 * 见 `computePeaksFromFile` 的注释（只取第 0 声道、每桶 lo/hi 从 0 起、按帧数均分）。
 * 两条路给出不同形状的波形时，**看起来不一样的那个会被当成坏了一个**。
 *
 * # 参数
 *
 * `buckets` 为 0、`path` 为空、`out` 为空、或 `capacity < buckets * 2` 时返回
 * `NAE_ERR_INVALID_ARGUMENT`（**不截断**：截断会让调用方按错的桶数去画，而那看起来
 * 和「这首歌就是这样」没有区别）。打不开路径时返回 `NAE_ERR_CANNOT_OPEN`。
 */
int32_t NAE_ComputePeaks(const char* path, uint32_t buckets, NAE_PeaksInfo* info,
                         float* out, int32_t capacity) NAE_NOEXCEPT;


// ---- analysis plane ---------------------------------------------------------
//
// A pure, path-based description of a file: format, full tags, ReplayGain,
// loudness, dynamic range, waveform peaks and BPM. It never opens a device and
// never touches a playback handle; the host calls it on its own worker and owns
// the cache. `analyzerVersion` belongs in the cache key.

#define NAE_ANALYZER_VERSION 2

typedef struct NAE_AnalysisInfo {
    uint32_t structSize;
    /** Algorithm version. Changing a measurement bumps this; cache keys use it. */
    uint32_t analyzerVersion;
    /** 1 when the whole file was decoded. */
    uint32_t complete;
    /** Number of peak buckets written. */
    uint32_t bucketCount;

    int32_t channels;
    int32_t sampleRate;
    int32_t bitsPerSample;
    int32_t durationMs;
    char codec[32];

    char title[128];
    char artist[128];
    char album[128];
    char albumArtist[128];
    char track[16];
    char disc[16];
    char year[16];
    char genre[64];
    char composer[128];
    char comment[256];
    uint32_t bitrateKbps;
    int32_t bitrateFromTag;

    int32_t rgTrackValid;
    double rgTrackGainDb;
    double rgTrackPeak;
    int32_t rgAlbumValid;
    double rgAlbumGainDb;
    double rgAlbumPeak;

    /** Median short-window RMS, dBFS. Not EBU R128 LUFS. */
    double loudnessDb;
    /** p95 - p10 of short-window RMS, dB. Not the official DR value. */
    double dynamicRangeDb;
    uint32_t metricsWindowMs;
    uint32_t metricsHopMs;

    /**
     * Tempo estimate from an offline onset envelope. `bpmValid` is 0 when the
     * envelope was too short or the autocorrelation score was too low; the host
     * must then show nothing rather than a plausible number. `bpmConfidence` is
     * 0..1, the normalised autocorrelation peak.
     */
    double bpm;
    double bpmConfidence;
    int32_t bpmValid;
    /** ITU-R BS.1770-4 integrated loudness, LUFS. See NAE_PeaksInfo. */
    double lufsIntegrated;
    double lufsMomentaryMax;
    double lufsShortTermMax;
    /** EBU Tech 3342 loudness range, LU. */
    double lufsRange;
    int32_t lufsValid;
    /** 1 = a 3 s window existed; 0 makes the two fields above meaningless. */
    int32_t lufsShortTermValid;
} NAE_AnalysisInfo;

/**
 * Decode `path` once and fill `info` plus `buckets * 2` interleaved min/max
 * peaks. Blocking; call from a host worker. No device is opened. `capacity` is
 * in floats and must be at least `buckets * 2`; a short buffer is rejected, not
 * truncated. Returns NAE_ERR_CANNOT_OPEN if the file cannot be decoded.
 */
int32_t NAE_AnalyzeFile(const char* path, uint32_t buckets, NAE_AnalysisInfo* info,
                        float* peaks, int32_t capacity,
                        int32_t* peaksWritten) NAE_NOEXCEPT;

/** The analysis algorithm version. Cache keys use this; it is not a build id. */
uint32_t NAE_GetAnalyzerVersion(void) NAE_NOEXCEPT;

/**
 * Open `deviceId` exclusively, converting from `sourceRate` to a rate chosen by
 * `outputMode`.
 *
 * This is the call the player uses. NAE_OpenExclusiveSine is the earlier
 * single-purpose entry point and is kept for the harnesses; it is equivalent to
 * this with mode = NAE_OUTPUT_NATIVE and sourceRate = the device's own rate.
 *
 * `sourceRate` is the file's rate. `fixedRate` is only read when
 * `outputMode` is NAE_OUTPUT_FIXED. `quality` is a NAE_QUALITY_* value.
 *
 * Fills the out-params with what is actually in effect, including:
 *   - the rate the device was opened at, which may differ from the source
 *   - `resampling` (nonzero when a converter is in the path)
 *   - `dithered`     (nonzero when requantisation is dithered)
 *   - `bitPerfect`   (see below -- it does NOT mean what its name says)
 *
 * `bitPerfect` is a misnomer and this paragraph is the correction, because the
 * previous one said "nonzero ONLY when nothing altered the samples" and that was
 * false in a way a caller would act on.
 *
 * What it actually measures: the device was opened at the file's own sample rate.
 * That is the whole of it. What it does not measure, and cannot today:
 *
 *   - the bit depth, the channel count and the sample format are the endpoint's,
 *     not the file's, and they are not compared against anything;
 *   - the decoder always produces float32
 *     (`swr_alloc_set_opts2(..., AV_SAMPLE_FMT_FLT, ...)`), so a 24-bit integer
 *     source is unpacked to float and packed again on the way out. The integers
 *     round-trip exactly, and they did not arrive untouched.
 *
 * So the correct name is `sourceRateExact`, and the interface already treats it
 * that way: `src/scripts/audio/reasonCodes.js` labels reason 0 "Rate unchanged"
 * and explains that it is a statement about the rate rather than the samples. The
 * field kept its old name so the ABI does not move for a rename that buys nothing
 * on its own.
 *
 * A caller must not render this as "bit-perfect". The honest version of that
 * claim needs the determination to be derived from the format that actually
 * opened and the path the samples actually took, and nothing here measures that.
 * Until then the strongest true statement is the one the interface already makes.
 */
int32_t NAE_OpenExclusive(const char* deviceId, int32_t sourceRate, int32_t outputMode,
                          int32_t fixedRate, int32_t quality, NAE_Handle** out,
                          int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                          int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                          int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                          double* latencyMs) NAE_NOEXCEPT;

/**
 * Render `milliseconds` of the handle's source, converting and dithering as
 * configured at open time.
 *
 * `milliseconds` bounds how long the call *takes*, not how much source is
 * consumed. The device buffer paces the loop, and a host that renders in slices
 * gets `milliseconds` of wall time per call while the source advances by
 * however much the device actually consumed. Call this repeatedly to play; one
 * call does not play a whole track unless the whole track fits the time given.
 */
int32_t NAE_Play(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT;

/**
 * 文件自己的信息——采样率、位深、声道、时长、编码。
 *
 * # 为什么单独一个结构体，而不是再给 open 加 out-param
 *
 * open 已经有 16 个 out-param 了，而文件的元数据有 5 个字段。再加 5 个参数会让
 * 调用点没法读，而且每加一个字段就是一次 ABI 变更。一个结构体是一次。
 *
 * # 这些数字和 `NAE_SignalPath` 里的不是一回事
 *
 * `NAE_SignalPath` 描述**设备正在被喂什么**：输出速率、容器位宽、有效位深、是否
 * 重采样。这个结构描述**用户的文件是什么**。
 *
 * 两者在两种情况下必然不同，而这正是界面需要它们都在的原因：
 *
 *   * 一张 44.1 kHz 的 FLAC 放在 192 kHz 的设备上：文件是 44100，输出是 192000，
 *     而 `resampling` 为真。
 *   * 一张 24 bit 的 FLAC：文件容器是 32 bit（解出来的格式），有效位深在容器里
 *     但压缩格式通常不可靠，而设备那边是 24-in-32。
 *
 * 把它们混成一处，就会出现「文件 24 bit」被说成「设备收到 24 bit」——而这个引擎
 * 对后一个说法有专门的、更严格的定义（见 `bitPerfect` 那一段），它并不是同一个
 * 主张。
 *
 * # 无效句柄
 *
 * 返回 NAE_OK 和清零的结构，和 NAE_QuerySignalPath 一样。0 表示「没有」而不是
 * 「零」：一个 0 位的文件不存在，而一个没有打开的流确实什么都报不出来。UI 必须
 * 能分辨这两种。
 */
typedef struct NAE_TrackInfo {
    /** 由调用方设为 `sizeof(NAE_TrackInfo)`。见 NAE_SignalPath。 */
    uint32_t structSize;
    /** 文件自己的声道数，下混之前。 */
    int32_t channels;
    /** 文件自己的采样率。 */
    int32_t sampleRate;
    /**
     * 文件的容器位宽，位/采样。0 表示容器没有报。
     *
     * **这是容器位宽，不是有效位深。** 24 bit 的 FLAC 解出来是 S32、高 8 位不用，
     * 所以这里是 32。有效位深在压缩格式的容器里通常不可靠，没有可靠的地方可读。
     *
     * 它仍然值得带上来，因为「24 bit 母带装在 32 bit 容器里」对文件来说是事实，
     * 而什么都不显示等于声称的比已知的更少。**不能**拿它当设备的字长说——那是
     * `NAE_SignalPath.granted_bits`，两者经常不同。
     */
    int32_t bitsPerSample;
    /** 时长，毫秒。0 表示容器没有报。 */
    int32_t durationMs;
    /** 编码名，如 "flac" / "aac"，NUL 结尾。无编码信息时首字节为 0。 */
    char codec[32];
} NAE_TrackInfo;

/**
 * 读文件信息。
 *
 * 与频谱、信号路径一样：空句柄返回 NAE_OK 和清零结构。`capacity` 不适用，因为
 * 这个结构是定长的——定长是刻意的，见上面关于 `codec` 用固定数组而非 `char*` 的
 * 说明：固定长度意味着调用方不需要 `NAE_FreeString`，也没有人需要猜谁管内存。
 */
int32_t NAE_QueryTrackInfo(NAE_Handle* handle, NAE_TrackInfo* out) NAE_NOEXCEPT;

/**
 * 文件的标签——年份、流派、专辑、艺人、码率。
 *
 * 与 `NAE_TrackInfo` 分开，因为它们回答不同的问题，而且**失败方式不同**：格式信息
 * 一定存在（容器报了就是有），标签**可能不存在**（这个文件没写）。混在一处会让
 * 「DSD64 / 352.8 kHz / 2ch」和一个空字符串共享同一种缺失的表示，而两者该被读成
 * 完全不同的两件事。
 *
 * 全部是定长字符数组。标签可以任意长，所以这里必须截断——而截断在哪里发生是写出来
 * 的：保留 NUL、丢掉超出部分，不静默地溢出。定长还意味着没有所有权问题：这个头文件
 * 里已经有 `char*` 形式的 out-param 由引擎分配、调用方 `NAE_FreeString` 释放，再放
 * 第二种约定就是某处开始释放对方内存的起点。
 *
 * 空字符串表示**文件没有这个标签**，而那是关于音乐的事实，不是播放器的故障。解析
 * 失败会被报出来，不会悄悄留下空白。
 *
 * 码率已经换算成 **kbps**。标签里的单位不统一——FLAC 的 `BITRATE` 是
 * `<容器总码率>_<音频码率>`，单位 bps——而直接显示会差 1000 倍。
 */
typedef struct NAE_Tags {
    /** 由调用方设为 `sizeof(NAE_Tags)`。见 NAE_SignalPath。 */
    uint32_t structSize;
    /** 年份的数字部分。`DATE` 是 `2024-05-01` 时这里是 `2024`。 */
    char year[16];
    char genre[64];
    char album[128];
    char artist[128];
    /** 码率，**kbps**，已经换算过。0 表示没有这个标签。 */
    uint32_t bitrateKbps;
    /** 显式填充：`bitrateKbps` 是 u32，后面若加 8 字节字段需要它。 */
    uint32_t reserved;
} NAE_Tags;

/** 读标签。空句柄或没有解码器时返回 NAE_OK 和清零结构。 */
int32_t NAE_QueryTags(NAE_Handle* handle, NAE_Tags* out) NAE_NOEXCEPT;

/**
 * 把 `NAE_DECODE_PATH_MAX` 个字节的解码路径写进 `buf`，包括结尾的 NUL。
 *
 * # 为什么单独一个函数，而不是在 `NAE_SignalPath` 里加字段
 *
 * 信号路径那个结构体是**已经**在用的 ABI，Rust 侧对它有一串 `offset_of!` 断言。加一个字段
 * 就得同时改断言、而且老宿主读到的 `structSize` 会截断——那是对的但更啰嗦。
 *
 * 而解码路径是一个**只在界面需要显示时**才读的字符串，不参与任何计算。所以一个固定长度的
 * 输出缓冲区是最省事的形式：**固定长度 `char[]` 而不是 `char*`**，因为引擎不分配、调用方
 * 提供缓冲区，就只有一种所有权约定——而两种约定并存的接口迟早出现跨边界的 free。
 *
 * # 它装的是什么
 *
 * 采样**实际**是怎么被解出来的，而不是容器声称它是什么。DSD 尤其如此，因为三条路线的
 * 保真度差别很大：
 *
 *   * `ffmpeg:dsd_msbf+neiro-dsd-decimate-2`
 *   * `ffmpeg:dsd_lsbf_planar`（没有后置抽取）
 *   * `neiro-dsd-cascade`（自写的 DSF 读取器 + ÷8/÷2 级联）
 *
 * 解码器名字不描述它做了什么——两份都叫 DSD 解码器，一份带抽取滤波一级，一份不带。
 * 所以从名字推断 DSD 音质是猜，而这个字符串是那条路径**实际**跑了之后写下来的。
 *
 * 空字符串表示「没有自己的路径」，而那与「读不出来」必须能分辨：前者是关于音乐的陈述，
 * 后者是关于播放器的故障。
 */
#define NAE_DECODE_PATH_MAX 256
int32_t NAE_QueryDecodePath(NAE_Handle* handle, char* buf, int32_t bufLen) NAE_NOEXCEPT;

/**
 * Open an audio file and the exclusive device together.
 *
 * Separate from NAE_OpenExclusive because the device must be configured for the
 * *file's* rate, and the file's rate is only known once the file is read. Passing
 * the rate in from outside would let the two disagree, and the bit-perfect flag
 * would then describe a stream that was never what was on disk.
 *
 * `fileChannels` is the file's own channel count, which differs from `channels`
 * (the device's) whenever a downmix or upmix happened. The UI shows the file's.
 * `durationMs` is 0 for a stream whose length the container does not report.
 *
 * Returns NAE_ERR_CANNOT_OPEN for a file the decoder cannot read; call
 * NAE_GetLastErrorText for a message fit to show a user.
 */
/**
 * Open with an explicit buffer policy. `bufferMode` is NAE_BUFFER_*; `bufferMs`
 * is used only for FIXED. Auto/Stable use the backend default, LowLatency asks
 * for the smallest period, Fixed asks for the exact duration. A refused explicit
 * request falls back to the backend default and reports it.
 */
int32_t NAE_OpenFileWithBuffer(const char* deviceId, const char* path,
                               int32_t outputMode, int32_t fixedRate, int32_t quality,
                               int32_t bufferMode, uint32_t bufferMs, NAE_Handle** out,
                               int32_t* sampleRate, int32_t* sourceRateOut,
                               int32_t* channels, int32_t* bitsPerSample,
                               int32_t* validBits, int32_t* isFloat,
                               int32_t* resampling, int32_t* dithered,
                               int32_t* bitPerfect, int32_t* fileChannels,
                               double* durationMs, double* latencyMs) NAE_NOEXCEPT;

int32_t NAE_OpenFile(const char* deviceId, const char* path, int32_t outputMode,
                     int32_t fixedRate, int32_t quality, NAE_Handle** out,
                     int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                     int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                     int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                     int32_t* fileChannels, double* durationMs, double* latencyMs) NAE_NOEXCEPT;

/**
 * Open an audio file and the device in **shared** mode.
 *
 * Identical to NAE_OpenFile in every argument and in everything it does, except
 * that the stream is opened with AUDCLNT_SHAREMODE_SHARED so Windows' mixer sits
 * in the path and other programs can make sound at the same time.
 *
 * It is a separate entry point rather than an extra argument because the
 * capability string advertises a list of modes, and a host reading that list has
 * to be able to ask for exactly the ones it sees. A mode listed but not openable
 * was the bug this replaces.
 *
 * **`bitPerfect` is always 0 and the reason is always `SharedMode` (7)**, whatever
 * the rate negotiation concluded. A shared stream that happens to run at the
 * source rate is still not bit-perfect, because the mixer is downstream of us.
 *
 * Whether Windows would in fact preserve the samples is **untested** -- for a long
 * time there was no way to ask, since no shared open path existed. The product
 * decision (2026-10-01) is that shared mode does not pursue bit-perfectness, so
 * the claim is not made in either direction. What is stated is the reason, and the
 * reason is a fact: the mixer is in the path.
 */
/**
 * Open a file with a source->device routing matrix. The decoder keeps the
 * file's channel count; the render path applies the matrix before resampling.
 * The matrix must match the file's source channels and the device's target
 * channels, otherwise this fails rather than silently falling back. The decoder
 * keeps the source channels, topUp applies the matrix before the resampler, and
 * crossfade/preload are rejected while routing is active.
 */
int32_t NAE_OpenFileWithRouting(const char* deviceId, const char* path,
                                int32_t outputMode, int32_t fixedRate, int32_t quality,
                                const NAE_RoutingMatrix* routing, NAE_Handle** out,
                                int32_t* sampleRate, int32_t* sourceRateOut,
                                int32_t* channels, int32_t* bitsPerSample,
                                int32_t* validBits, int32_t* isFloat,
                                int32_t* resampling, int32_t* dithered,
                                int32_t* bitPerfect, int32_t* fileChannels,
                                double* durationMs, double* latencyMs) NAE_NOEXCEPT;

int32_t NAE_OpenFileShared(const char* deviceId, const char* path, int32_t outputMode,
                           int32_t fixedRate, int32_t quality, NAE_Handle** out,
                           int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                           int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                           int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                           int32_t* fileChannels, double* durationMs,
                           double* latencyMs) NAE_NOEXCEPT;

/**
 * Per-call pipeline accounting for the last NAE_Play, as JSON. Caller owns the
 * result; free it with NAE_FreeString. Null for a null handle.
 *
 * This exists so a benchmark can read the counters instead of scraping the
 * NAE_TRACE log. The invariant worth checking is:
 *
 *     sourceAdvance * deviceRate / sourceRate  ==  deviceWritten
 *
 * When it does not hold, the device is being fed frames the source never paid
 * for. That is a playback-rate bug, and no format flag will admit to it — the
 * signal path still reads correctly and the playhead still advances.
 */
char* NAE_PipelineStats(NAE_Handle* handle) NAE_NOEXCEPT;

/**
 * Restart playback from `seconds` into the source.
 *
 * Approximate for compressed formats: the container lands on the nearest packet
 * it can and the decoder discards what it has buffered, so the position is
 * accurate to a frame or two of the seek target rather than exact. Do not
 * promise sample accuracy in the UI.
 */
int32_t NAE_Seek(NAE_Handle* handle, double seconds) NAE_NOEXCEPT;

/** Seconds of source consumed so far. 0 for a generated tone. */
double NAE_Position(NAE_Handle* handle) NAE_NOEXCEPT;

/**
 * Open the next file on the same device stream.
 *
 * Gapless requires the same source rate and channel count as the current file:
 * the resampler and the device format stay untouched. A mismatch is rejected
 * with NAE_ERR_INVALID_ARGUMENT rather than silently resampled, because a
 * mismatch that is resampled is no longer the same signal path.
 *
 * Call on the thread that owns the handle, between render calls. The current
 * file continues to play; the swap happens when its source is exhausted, before
 * the fifo runs dry.
 */
int32_t NAE_PreloadNext(NAE_Handle* handle, const char* path) NAE_NOEXCEPT;

/** Drop the preloaded next file. The current file keeps playing. */
int32_t NAE_ClearNext(NAE_Handle* handle) NAE_NOEXCEPT;

/** Increments each time a preloaded file takes over. 0 for a tone. */
uint64_t NAE_GetTrackSerial(NAE_Handle* handle) NAE_NOEXCEPT;

/** Seconds the source lasts, or 0 when unknown. */
double NAE_Duration(NAE_Handle* handle) NAE_NOEXCEPT;

// ---- DoP --------------------------------------------------------------------
//
// DoP is a transport, not a decoder: raw DSD bits from a DSF or DFF file are
// packed into 24-bit carrier words and written straight to the device. It
// bypasses NAE_Play, the fifo, the resampler and every float DSP block, because
// volume or EQ would corrupt the marker bytes.
//
// The device must accept the carrier rate (DSD64 = 176400 Hz) at 24-bit without
// resampling. Coercion is a hard failure: a resampled carrier is not DoP.

/**
 * Open `path` (DSF or DFF, uncompressed DSD) and the device at the carrier rate.
 * Returns NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT if the endpoint cannot open that
 * rate at 24 bits without resampling.
 */
/**
 * Open and decimate the DSD bitstream to `targetDsdRate` (a lower rate that
 * divides the source rate by 1, 2, 4 or 8) before packing DoP. The device is
 * opened at `targetDsdRate / 16`. `carrierRate` reports the carrier actually
 * opened; the caller can compare it with the file's rate to know whether a
 * downrate happened.
 */
int32_t NAE_OpenDopDownrated(const char* deviceId, const char* path, int32_t targetDsdRate,
                             NAE_Handle** out, int32_t* carrierRate, int32_t* channels,
                             int32_t* bits, double* latencyMs) NAE_NOEXCEPT;

int32_t NAE_OpenDop(const char* deviceId, const char* path, NAE_Handle** out,
                    int32_t* carrierRate, int32_t* channels, int32_t* bits,
                    double* latencyMs) NAE_NOEXCEPT;

/**
 * Native DSD: the raw DSD bitstream is handed to a backend that advertises
 * native DSD. WASAPI has no standard native-DSD path, so this backend currently
 * returns NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT; the API and Session path exist for
 * ASIO/USB/CoreAudio implementations. The Session treats native as a request:
 * with PCM fallback it reports the failure and continues as PCM; without
 * fallback the open fails.
 */
/**
 * The host's native DSD device, behind one callback.
 *
 * WASAPI has no 1-bit stream format, so the engine cannot open a native DSD
 * endpoint itself. A host that owns such a device (ASIO, a vendor SDK, a USB
 * driver) opens the file with this call instead: the engine reads the DSF/DFF,
 * applies the idle pattern and an optional downrate, and hands interleaved
 * bytes to `write` from inside NAE_PlayNativeDsd.
 *
 * The callback owns pacing and the device. It must not call back into the
 * engine. `bytesPerChannel` is the number of bytes for each channel in a
 * frame of `channels` adjacent bytes; one byte is eight 1-bit samples.
 * Return 0 to continue; any other value stops playback and is returned by
 * NAE_PlayNativeDsd, so the host's own error code reaches the caller.
 *
 * `bitOrderMsbFirst` describes the bytes: 1 for DFF-style MSB-first, 0 for
 * DSF-style LSB-first. It is a property of the file, and the sink must not
 * reverse it silently.
 */
typedef int32_t (*NAE_DsdWriteFn)(void* context, const uint8_t* interleaved,
                                  int32_t bytesPerChannel, int32_t channels);

int32_t NAE_OpenNativeDsdSink(const char* path, int32_t targetDsdRate,
                              NAE_DsdWriteFn write, void* context, NAE_Handle** out,
                              int32_t* dsdRate, int32_t* channels,
                              int32_t* bitOrderMsbFirst,
                              double* durationMs) NAE_NOEXCEPT;

int32_t NAE_OpenNativeDsd(const char* deviceId, const char* path, NAE_Handle** out,
                          int32_t* dsdRate, int32_t* channels, int32_t* bits,
                          double* latencyMs) NAE_NOEXCEPT;

/** Render `milliseconds` of native DSD. Blocks. */
int32_t NAE_PlayNativeDsd(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT;

/** Render `milliseconds` of packed DoP. Blocks. */
int32_t NAE_PlayDop(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT;

/**
 * Copy the calling thread's last error message into `buf`, NUL-terminated and
 * truncated to fit. Returns the number of bytes written, excluding the
 * terminator — so 0 means the engine had nothing to say, and a non-zero return is
 * a length rather than a status code.
 *
 * A code tells a caller that something failed; it cannot say "this file is not
 * audio", which is the thing a listener needs to know.
 */
int32_t NAE_GetLastErrorText(char* buf, int32_t bufLen) NAE_NOEXCEPT;

/**
 * Render `milliseconds` of a sine at the handle's source rate.
 *
 * Retained under its original name; identical to NAE_Play. New code should call
 * NAE_Play, because the name is no longer what it does.
 */
int32_t NAE_PlaySine(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT;

/**
 * Stop the device stream without discarding the handle.
 *
 * This exists because NAE_Play is a slice call. The host invokes it every
 * SLICE_MS, and the loop now deliberately stays started across those boundaries --
 * stopping and restarting put a buffer of zeroes into a continuous waveform five
 * times a second, which was the bubbling.
 *
 * The consequence is that "stopped" is no longer implied by simply not calling
 * NAE_Play: a started client that nobody fills runs dry. A host that pauses or
 * stops has to say so here.
 *
 * Safe on a null handle, and safe to call when the stream is already stopped.
 */
int32_t NAE_Stop(NAE_Handle* handle) NAE_NOEXCEPT;

/**
 * Keep the stream running but hand the device silence, and resume.
 *
 * This exists because of a measured property of this endpoint, not out of taste.
 * Stopping and restarting the client is audible here: 100 stop/play cycles
 * produced one artefact per cycle, and the same 100 cycles with the stop removed
 * were clean. That is the only difference between the two runs. The samples the
 * engine writes are continuous across the boundary -- the seam ratio read 0.88,
 * against a self-calibrating reference derived from the signal's own slew -- so the
 * discontinuity is produced by the device rather than handed to it. A fade-in on
 * resume and gating the silent prefill were both tried and both changed nothing.
 *
 * So a pause must not be a stop. Silence is fed instead, which means there is no
 * boundary for the device to react to at all.
 *
 * The accounting must not move while paused. `NAE_Position` does not advance, the
 * fifo is not drained, and no source is consumed, so a pause of any length resumes
 * at the same sample with the same position. Silence frames are counted in
 * `silenceFramesWritten` rather than folded into playback, so the delivered-rate
 * figure keeps meaning what it says.
 *
 * A pause is not a stop: NAE_Stop is still the way to end a stream, and it is
 * still audible on this endpoint. Pausing costs no boundary; stopping costs one.
 *
 * Safe on a null handle. Pausing an already-paused handle, or resuming one that is
 * not paused, both succeed and do nothing.
 */
int32_t NAE_Pause(NAE_Handle* handle) NAE_NOEXCEPT;
int32_t NAE_Resume(NAE_Handle* handle) NAE_NOEXCEPT;

/**
 * Set digital volume, linear 0.0..1.0, with a 20 ms ramp.
 *
 * Call this on the thread that owns the handle, between render calls. 1.0 is
 * exact bypass: no multiply happens, and the bit-perfect flag is not changed.
 * Any other value makes the stream processed and is reported as
 * NAE_REASON_VOLUME_NOT_UNITY.
 *
 * The multiply is applied in float before quantisation, so it costs resolution
 * only once. For bit-perfect playback prefer the DAC's analog volume.
 */
int32_t NAE_SetVolume(NAE_Handle* handle, double gain) NAE_NOEXCEPT;

/** Dither selection. OFF leaves rounding undithered; NOISE_SHAPED uses
 * first-order error feedback and is never chosen by Auto. */
#define NAE_DITHER_OFF 0
#define NAE_DITHER_TPDF 1
#define NAE_DITHER_RECTANGULAR 2
#define NAE_DITHER_NOISE_SHAPED 3

/** Per-channel trim in dB, applied after EQ/crossfeed. count <= NAE_MAX_CHANNELS. */
int32_t NAE_SetChannelTrim(NAE_Handle* handle, const double* trimDb,
                           uint32_t count) NAE_NOEXCEPT;
/** L/R balance, -1.0 left .. +1.0 right. */
int32_t NAE_SetBalance(NAE_Handle* handle, double balance) NAE_NOEXCEPT;
/** Bit N inverts channel N. */
int32_t NAE_SetPolarityMask(NAE_Handle* handle, uint32_t mask) NAE_NOEXCEPT;
/** Per-channel delay in device frames. */
int32_t NAE_SetChannelDelay(NAE_Handle* handle, const int32_t* frames,
                            uint32_t count) NAE_NOEXCEPT;

/**
 * Enable the clip guard with a ceiling in dBTP and lookahead/release in ms.
 * `enabled` 0 disables it. This is a sample-peak feed-forward limiter, not an
 * inter-sample true-peak detector.
 */
int32_t NAE_SetClipGuard(NAE_Handle* handle, int32_t enabled, double ceilingDbTp,
                         double lookaheadMs, double releaseMs) NAE_NOEXCEPT;
/** Current gain reduction in dB; 0 when no reduction. */
double NAE_GetClipGuardReductionDb(NAE_Handle* handle) NAE_NOEXCEPT;

int32_t NAE_SetDitherMode(NAE_Handle* handle, int32_t mode) NAE_NOEXCEPT;
int32_t NAE_GetDitherMode(NAE_Handle* handle) NAE_NOEXCEPT;

/** The volume target, or 1.0 for a null handle. */
double NAE_GetVolume(NAE_Handle* handle) NAE_NOEXCEPT;

/**
 * Release the calling thread's COM apartment.
 *
 * Only valid once every handle created on this thread has been closed. In the
 * real app the audio thread does this as it exits, after its handle is gone;
 * the UI thread never needs it.
 */
void NAE_Shutdown(void) NAE_NOEXCEPT;

// ---- system default ---------------------------------------------------------

/**
 * The endpoint Windows currently uses for console audio, as a raw id string.
 * Caller owns the result; free it with NAE_FreeString. Returns null on failure.
 *
 * Resolved fresh on every call, so it follows the user changing the default in
 * Sound settings. The app treats this as a first-class device choice rather than
 * a fallback: "whatever Windows is set to" is the right default for a player,
 * and it is the endpoint a user expects to be able to take over exclusively.
 *
 * Note that resolving the id is not the same as being able to open it. If
 * another application already holds the default endpoint, opening will still
 * fail — see NAE_EnumerateDevices, whose `exclusive` flag is a real probe.
 */
char* NAE_GetDefaultDeviceId(void) NAE_NOEXCEPT;

// ---- equaliser -------------------------------------------------------------

/**
 * Bands in one cascade. The linear-phase path builds a single FIR per channel
 * from the summed bands, so its cost does not grow with this number; the limit
 * is the size of this struct. Raising it is a layout change on both sides of
 * the ABI.
 */
#define NAE_EQ_MAX_BANDS 32

#define NAE_EQ_PEAKING 0
#define NAE_EQ_LOW_SHELF 1
#define NAE_EQ_HIGH_SHELF 2
#define NAE_EQ_HIGH_PASS 3
#define NAE_EQ_LOW_PASS 4
#define NAE_EQ_NOTCH 5
#define NAE_EQ_ALL_PASS 6

#define NAE_EQ_PHASE_MINIMUM 0
#define NAE_EQ_PHASE_LINEAR 1

/**
 * One biquad. `freqHz` and `q` are validated against the stream's rate; a band
 * that cannot be realised is rejected rather than silently moved.
 */
typedef struct NAE_EqBand {
    int32_t type;  // NAE_EQ_*
    double freqHz;
    double gainDb;  // peaking and shelves; ignored by the others
    double q;
    int32_t enabled;
    /**
     * Bit mask of channels the band applies to; bit 0 is the first channel.
     * 0 means every channel, so a struct zeroed by the caller keeps the old
     * behaviour. A band aimed at one channel is how a per-channel curve is
     * built: the cascade is the sum of the bands that carry that bit set.
     */
    uint32_t channelMask;
} NAE_EqBand;

/**
 * The whole cascade. `structSize` is set to `sizeof(NAE_EqParams)`.
 *
 * Bands are applied in order. `preampDb` is applied before them, which is the
 * headroom control: a boost needs an equal cut here or the output clips.
 *
 * Setting this resets the filter state. That is honest for a one-shot
 * configuration and audible during live editing; a host that changes bands
 * while playing should crossfade the new cascade in.
 */
typedef struct NAE_EqParams {
    uint32_t structSize;
    int32_t enabled;
    double preampDb;
    int32_t bandCount;  // 0..NAE_EQ_MAX_BANDS
    NAE_EqBand bands[NAE_EQ_MAX_BANDS];
    /**
     * 0 = minimum phase (the cascade runs as biquads per sample).
     * 1 = linear phase (one symmetric FIR per channel, constant group delay).
     *
     * Linear phase costs `taps/2 + 256` frames of latency at the device rate
     * (1280 at 48 kHz) and is built once when the cascade is set, not in the
     * render loop.
     */
    int32_t phase;  // NAE_EQ_PHASE_*
    uint32_t reserved0;
} NAE_EqParams;

/** Defaults: disabled, 0 dB preamp, no bands. */
void NAE_EqDefaults(NAE_EqParams* out) NAE_NOEXCEPT;

/** Replace the cascade. Call on the thread that owns the handle. */
int32_t NAE_SetEq(NAE_Handle* handle, const NAE_EqParams* params) NAE_NOEXCEPT;

/** Read the last applied cascade. */
int32_t NAE_GetEq(NAE_Handle* handle, NAE_EqParams* out) NAE_NOEXCEPT;

// ---- crossfeed -------------------------------------------------------------

/**
 * Headphone crossfeed: a delayed, low-passed copy of each channel is added to
 * the other. Only applied to stereo streams; a multichannel stream is left alone.
 */
typedef struct NAE_CrossfeedParams {
    uint32_t structSize;
    int32_t enabled;
    /** Mix of the crossfed copy, linear 0..1. */
    double amount;
    /** Delay of the crossfed copy, milliseconds. */
    double delayMs;
    /** First-order low-pass cutoff, Hz. */
    double cutoffHz;
} NAE_CrossfeedParams;

/** Disabled, 0.3 amount, 0.3 ms delay, 700 Hz cutoff. */
void NAE_CrossfeedDefaults(NAE_CrossfeedParams* out) NAE_NOEXCEPT;

/** Replace the crossfeed settings. Call on the thread that owns the handle. */
int32_t NAE_SetCrossfeed(NAE_Handle* handle, const NAE_CrossfeedParams* params) NAE_NOEXCEPT;

/** Read the last applied crossfeed settings. */
int32_t NAE_GetCrossfeed(NAE_Handle* handle, NAE_CrossfeedParams* out) NAE_NOEXCEPT;

// ---- ReplayGain -------------------------------------------------------------

#define NAE_RG_OFF 0
#define NAE_RG_TRACK 1
#define NAE_RG_ALBUM 2

/** What the file's metadata said. A missing peak is not a zero peak. */
typedef struct NAE_ReplayGainInfo {
    uint32_t structSize;
    int32_t trackValid;
    double trackGainDb;
    double trackPeak;
    int32_t albumValid;
    double albumGainDb;
    double albumPeak;
} NAE_ReplayGainInfo;

/**
 * How the gain is chosen. `fallbackDb` is used when the chosen tag is absent.
 * `preventClipping` lowers the gain when gain + peak would exceed full scale.
 */
typedef struct NAE_ReplayGainParams {
    uint32_t structSize;
    int32_t mode;  // NAE_RG_*
    double preampDb;
    int32_t preventClipping;
    double fallbackDb;
} NAE_ReplayGainParams;

void NAE_ReplayGainDefaults(NAE_ReplayGainParams* out) NAE_NOEXCEPT;

/** Read the file's ReplayGain metadata. Safe on a null handle; fills zeros. */
int32_t NAE_QueryReplayGain(NAE_Handle* handle, NAE_ReplayGainInfo* out) NAE_NOEXCEPT;

/** Replace the gain policy. Call on the thread that owns the handle. */
int32_t NAE_SetReplayGain(NAE_Handle* handle,
                          const NAE_ReplayGainParams* params) NAE_NOEXCEPT;

/** Read the last applied policy. */
int32_t NAE_GetReplayGain(NAE_Handle* handle, NAE_ReplayGainParams* out) NAE_NOEXCEPT;

/** The linear gain the current policy resolved to. 1.0 for a null handle. */
double NAE_GetReplayGainGain(NAE_Handle* handle) NAE_NOEXCEPT;

// ---- convolver --------------------------------------------------------------

#define NAE_CONVOLVER_MAX_TAPS 4096

/**
 * A finite impulse response at the source rate. The IR is loaded from an audio
 * file; a stereo IR is applied per channel, a mono IR to both. Longer than
 * `NAE_CONVOLVER_MAX_TAPS` is rejected rather than truncated.
 *
 * The engine convolves with a uniform-partitioned FFT (256-sample blocks, up to
 * 32 partitions), so this cap is the array limit, not a direct-FIR cost.
 */
typedef struct NAE_ConvolverParams {
    uint32_t structSize;
    int32_t enabled;
    char irPath[512];
    double wet;
    double dry;
    double gainDb;
    int32_t normalize;
} NAE_ConvolverParams;

void NAE_ConvolverDefaults(NAE_ConvolverParams* out) NAE_NOEXCEPT;

/** Load the IR (when the path is non-empty) and apply the settings. */
int32_t NAE_SetConvolver(NAE_Handle* handle,
                         const NAE_ConvolverParams* params) NAE_NOEXCEPT;

int32_t NAE_GetConvolver(NAE_Handle* handle, NAE_ConvolverParams* out) NAE_NOEXCEPT;

// ---- crossfade --------------------------------------------------------------

#define NAE_CROSSFADE_LINEAR 0
#define NAE_CROSSFADE_EQUAL_POWER 1

/**
 * Mix the end of the current file with the start of the preloaded next one.
 * The fade consumes the next file's first `durationMs`; the position after the
 * swap starts at that point, not at zero. A duration of 0 is gapless.
 */
typedef struct NAE_CrossfadeParams {
    uint32_t structSize;
    int32_t enabled;
    double durationMs;
    int32_t curve;  // NAE_CROSSFADE_*
} NAE_CrossfadeParams;

void NAE_CrossfadeDefaults(NAE_CrossfadeParams* out) NAE_NOEXCEPT;
int32_t NAE_SetCrossfade(NAE_Handle* handle,
                         const NAE_CrossfadeParams* params) NAE_NOEXCEPT;
int32_t NAE_GetCrossfade(NAE_Handle* handle, NAE_CrossfadeParams* out) NAE_NOEXCEPT;

// ---- session: the engine owns the playback clock ---------------------------
//
// Everything above is a render primitive: NAE_Play(milliseconds) blocks, and the
// caller decides when to call it again. That makes every host an audio scheduler,
// and the host's loop becomes the clock. A session moves that loop into the
// engine: one thread owns the handle, the stream and the position; the caller
// sends play/pause/seek/stop and reads a snapshot.
//
// The session thread still renders through NAE_Play in slices, so decode still
// shares that thread with the device. Splitting decode onto its own thread is the
// next step; it is named here rather than pretended away.
//
// One session per process for now: the COM apartment is per-thread and the engine
// has one device stream at a time.

#define NAE_SESSION_IDLE 0
#define NAE_SESSION_READY 1
#define NAE_SESSION_PLAYING 2
#define NAE_SESSION_PAUSED 3
#define NAE_SESSION_DEVICE_LOST 4
#define NAE_SESSION_ENDED 5
#define NAE_SESSION_ERROR 6

#define NAE_SHARE_EXCLUSIVE 0
#define NAE_SHARE_SHARED 1

typedef struct NAE_Session NAE_Session;

/**
 * Everything a session needs at open time. `structSize` makes the struct
 * extensible; set it to `sizeof(NAE_SessionConfig)`.
 *
 * `sliceMs` is the render slice the session thread uses. 0 means 16 ms. It bounds
 * how long a pause/seek/stop can wait, and it is the freshness limit for the
 * position snapshot.
 */
typedef struct NAE_SessionConfig {
    uint32_t structSize;
    const char* deviceId;  // null or "" = the endpoint used last / system default
    const char* path;
    int32_t outputMode;  // NAE_OUTPUT_*
    int32_t fixedRate;   // only for NAE_OUTPUT_FIXED
    int32_t quality;     // NAE_QUALITY_*
    int32_t shareMode;   // NAE_SHARE_*
    int32_t sliceMs;     // 0 = 16
} NAE_SessionConfig;

/** One reading of a session. Caller sets `structSize` to `sizeof`. */
typedef struct NAE_SessionState {
    uint32_t structSize;
    int32_t state;  // NAE_SESSION_*
    /** Seconds of source consumed, from device frames written. */
    double position;
    double duration;
    int32_t sampleRate;
    int32_t sourceRate;
    int32_t channels;
    int32_t bitsPerSample;
    int32_t validBits;
    int32_t isFloat;
    int32_t bitPerfect;
    int32_t reason;  // NAE_REASON_*
    int32_t resampling;
    int32_t dithered;
    int32_t fileChannels;
    double latencyMs;
    /** Current volume target, linear 0..1. */
    double volume;
    /** Non-zero when the equaliser is active. */
    int32_t eqEnabled;
    /** Non-zero when crossfeed is active. */
    int32_t crossfeedEnabled;
    /** Non-zero when ReplayGain is applying a policy (it may resolve to 0 dB). */
    int32_t replayGainEnabled;
    /** Non-zero when the convolver is active. */
    int32_t convolverEnabled;
    /** Non-zero when crossfade is active. */
    int32_t crossfadeEnabled;
    int32_t rgTrackValid;
    int32_t rgAlbumValid;
    double rgTrackGainDb;
    double rgAlbumGainDb;
    /** Linear gain the current policy resolved to; 1.0 when nothing is applied. */
    double rgAppliedGain;
    /** Increments when a preloaded track takes over. */
    uint64_t trackSerial;
} NAE_SessionState;

/**
 * Open the file and the device, start the session thread, and wait for the
 * stream to be ready.
 *
 * On a non-zero return `*out` is still a valid session in `NAE_SESSION_ERROR`;
 * read the engine's message with NAE_SessionLastError and release it with
 * NAE_SessionClose. That keeps the file/device failure text, which a bare code
 * cannot carry.
 */
int32_t NAE_SessionOpen(const NAE_SessionConfig* config, NAE_Session** out) NAE_NOEXCEPT;

/**
 * Open with an explicit output policy. Supported today: rate selection
 * (Native/Device/Fixed/Auto), resampler quality, and dither Off/TPDF/
 * Rectangular. Routing/DSD/buffer/clip policy fields are reported as
 * unimplemented warnings rather than silently applied.
 */
int32_t NAE_SessionOpenWithPolicy(const NAE_SessionConfig* config,
                                  const NAE_OutputPolicy* policy,
                                  NAE_Session** out) NAE_NOEXCEPT;

/** The requested policy (not the effective path; use GetResolvedSignalPath). */
int32_t NAE_SessionGetOutputPolicy(NAE_Session* session,
                                   NAE_OutputPolicy* out) NAE_NOEXCEPT;

/** Ready or paused -> playing. Ended -> seek 0 and play. No-op while playing. */
int32_t NAE_SessionPlay(NAE_Session* session) NAE_NOEXCEPT;

/** Playing -> paused by feeding silence. Never stops the stream. */
int32_t NAE_SessionPause(NAE_Session* session) NAE_NOEXCEPT;
int32_t NAE_SessionResume(NAE_Session* session) NAE_NOEXCEPT;

/**
 * Seek. Asynchronous: the position snapshot moves when the session thread has
 * performed the seek. Compressed formats land near the target, not exactly on it.
 */
int32_t NAE_SessionSeek(NAE_Session* session, double seconds) NAE_NOEXCEPT;

/** Stop the thread, close the stream and leave the session in IDLE. */
int32_t NAE_SessionStop(NAE_Session* session) NAE_NOEXCEPT;

/** Lock-free snapshot. Safe from any thread, including while the session runs. */
int32_t NAE_SessionGetState(NAE_Session* session, NAE_SessionState* out) NAE_NOEXCEPT;

/** Set digital volume for the session. Applied on the session thread. */
int32_t NAE_SessionSetVolume(NAE_Session* session, double gain) NAE_NOEXCEPT;

/** Current volume target. 1.0 for a null session. */
double NAE_SessionGetVolume(NAE_Session* session) NAE_NOEXCEPT;

/** Replace the equaliser. Copied and applied on the session thread. */
int32_t NAE_SessionSetEq(NAE_Session* session, const NAE_EqParams* params) NAE_NOEXCEPT;

/** Read the session's equaliser settings. */
int32_t NAE_SessionGetEq(NAE_Session* session, NAE_EqParams* out) NAE_NOEXCEPT;

/** Replace crossfeed. Copied and applied on the session thread. */
int32_t NAE_SessionSetCrossfeed(NAE_Session* session,
                                const NAE_CrossfeedParams* params) NAE_NOEXCEPT;

/** Read the session's crossfeed settings. */
int32_t NAE_SessionGetCrossfeed(NAE_Session* session, NAE_CrossfeedParams* out) NAE_NOEXCEPT;

/** Replace ReplayGain. Copied and applied on the session thread. */
int32_t NAE_SessionSetReplayGain(NAE_Session* session,
                                 const NAE_ReplayGainParams* params) NAE_NOEXCEPT;

/** Read the session's ReplayGain policy. */
int32_t NAE_SessionGetReplayGain(NAE_Session* session, NAE_ReplayGainParams* out) NAE_NOEXCEPT;

/** Replace the convolver. The IR is loaded and applied on the session thread. */
int32_t NAE_SessionSetConvolver(NAE_Session* session,
                                const NAE_ConvolverParams* params) NAE_NOEXCEPT;
int32_t NAE_SessionGetConvolver(NAE_Session* session, NAE_ConvolverParams* out) NAE_NOEXCEPT;

int32_t NAE_SessionSetCrossfade(NAE_Session* session,
                                const NAE_CrossfadeParams* params) NAE_NOEXCEPT;
int32_t NAE_SessionGetCrossfade(NAE_Session* session, NAE_CrossfadeParams* out) NAE_NOEXCEPT;

/**
 * Append a track to the gapless queue. The session preloads it; if the source
 * rate or channel count differs, the track stays queued and the last error says
 * why it could not be preloaded.
 */
int32_t NAE_SessionQueueAdd(NAE_Session* session, const char* path) NAE_NOEXCEPT;
int32_t NAE_SessionQueueClear(NAE_Session* session) NAE_NOEXCEPT;
int32_t NAE_SessionQueueCount(NAE_Session* session) NAE_NOEXCEPT;

/** Copy the last session error. Returns bytes written, excluding the NUL. */
int32_t NAE_SessionLastError(NAE_Session* session, char* buf, int32_t bufLen) NAE_NOEXCEPT;

/** The actual path for the current track; per-track, not a settings dump. */
int32_t NAE_SessionGetResolvedSignalPath(NAE_Session* session,
                                          NAE_ResolvedSignalPath* out) NAE_NOEXCEPT;

/**
 * The same path as JSON, including diagnostics. Caller owns the string and
 * releases it with NAE_FreeString. Null until the session is ready.
 */
char* NAE_SessionGetResolvedSignalPathJson(NAE_Session* session) NAE_NOEXCEPT;

/** Stop if needed, join the thread, release the session. Safe on NULL. */
void NAE_SessionClose(NAE_Session* session) NAE_NOEXCEPT;

// ---- parameter registry ----------------------------------------------------
//
// The typed structures above are the contract. This is the same set exposed as
// name/value pairs for a CLI or a settings UI that does not know the structs.
// Every entry has a default and a range; nothing here is required to play.

#define NAE_PARAM_BOOL 0
#define NAE_PARAM_INT 1
#define NAE_PARAM_DOUBLE 2
#define NAE_PARAM_ENUM 3
#define NAE_PARAM_STRING 4

#define NAE_PARAM_RUNTIME 1u
#define NAE_PARAM_OPEN 2u
#define NAE_PARAM_STRUCT 4u
#define NAE_PARAM_BREAKS_BP 8u
#define NAE_PARAM_READONLY 16u

typedef struct NAE_ParamDesc {
    uint32_t structSize;
    const char* name;
    const char* unit;
    int32_t type;
    double minValue;
    double maxValue;
    double defaultValue;
    uint32_t flags;
    /** Comma-separated enum labels, or NULL. */
    const char* values;
} NAE_ParamDesc;

int32_t NAE_ParamCount(void) NAE_NOEXCEPT;
int32_t NAE_ParamAt(int32_t index, NAE_ParamDesc* out) NAE_NOEXCEPT;

int32_t NAE_SessionParamGet(NAE_Session* session, const char* name, double* out) NAE_NOEXCEPT;
int32_t NAE_SessionParamSet(NAE_Session* session, const char* name, double value) NAE_NOEXCEPT;
int32_t NAE_SessionParamGetString(NAE_Session* session, const char* name, char* buf,
                                  int32_t bufLen) NAE_NOEXCEPT;
int32_t NAE_SessionParamSetString(NAE_Session* session, const char* name,
                                  const char* value) NAE_NOEXCEPT;

#ifdef __cplusplus
}
#endif
