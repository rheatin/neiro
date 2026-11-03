// Output rate policy.
//
// The three modes exist because they answer genuinely different questions, and
// collapsing them to one is how a player ends up either resampling needlessly or
// refusing to play a 44.1 kHz file on a 192 kHz DAC.
//
// What the reference engine does, and why this mirrors it: its output stage
// carries a `targetSampleRate` that is either a number or the string "device",
// with "device" as the default. So does this.
//
// # The modes
//
//   Native     The source already runs at the device rate. Nothing is
//              resampled, nothing is requantised, nothing is dithered. This is
//              the only mode that is bit-perfect, and it is the one the UI
//              should claim bit-perfect *only* for.
//
//   Device     Resample to the device's rate with soxr, then dither to the
//              device's depth. The default, and the right answer for most
//              files: a 44.1 kHz CD on a 192 kHz interface.
//
//   Fixed      Resample to a chosen rate, usually so the signal path reads the
//              same regardless of which device is selected. Costs a conversion
//              that may be pointless, and the UI should say so.
//
// # Why not integer multiples
//
// A strict integer-ratio resampler is exact but only works when the rates
// divide, and 44.1 -> 192 does not (it is 480:1 in frames but 44.1 kHz is not a
// divisor of 192 kHz in any useful sense). Forcing integer-only would make the
// most common case impossible. soxr handles arbitrary ratios properly, so
// there is no reason to restrict it.
//
// An integer *upsample* is lossless in the strict sense — zero-stuffing plus a
// filter adds no information and loses none — but it is also pointless: the
// extra sample rate carries nothing, and it burns CPU and shifts the DSD
// multiplier. So it is not offered as a mode, rather than offered and
// discouraged.

#pragma once

#include <cmath>
#include <cstdint>

namespace neiro {

enum class OutputMode : int32_t {
    // Source rate already matches the device: pass through untouched.
    Native = 0,
    // Convert to the device's rate. The default.
    Device = 1,
    // Convert to an explicit rate.
    Fixed = 2,
};

// Why the path is or is not bit-perfect.
//
// A single boolean cannot be acted on. "Not bit-perfect" is true for a file at
// the wrong rate, for a device that cannot carry the format, and for a dither
// applied on purpose -- and in only one of those is the listener's setting the
// problem. So the verdict carries a name, and the name is what the UI turns into
// "what is happening" and "what do I change".
enum class PerfectReason : int32_t {
    BitPerfect = 0,
    ResampledToDeviceRate = 1,
    ResampledToFixedRate = 2,
    Dithered = 3,
    ChannelConverted = 4,
    DeviceCannotCarryFormat = 5,
    // The rate the file runs at is one the endpoint will not open at, so the
    // closest rate it *will* open at was chosen instead.
    //
    // Its own code because it is the one case where the listener is not wrong
    // and nothing they can change helps: a DSD64 file decodes to 352.8 kHz, and
    // an endpoint whose exclusive rates stop at 192 kHz simply cannot be opened
    // at the file's rate. Reporting that as ResampledToDeviceRate would send
    // them to the output-mode control, where every setting produces the same
    // result. The device's own rate is a better answer than an error, and the
    // interface has to be able to say *why* it is a conversion.
    ResampledToSupportedRate = 6,
    // The stream was opened in shared mode, so the Windows audio engine mixes it
    // with everything else on the endpoint and may convert it to the mix format.
    //
    // Its own code because **it is the user's choice, not a fault**, and the
    // interface has to be able to say that. Every other code here points at
    // something that went wrong or at a control that would fix it; this one is a
    // route the listener picked on purpose, in exchange for letting other programs
    // make sound at the same time.
    //
    // So the UI must not present it as a problem to be corrected, and must not
    // print any bit-perfect wording beside it. What it can honestly say is *why*
    // the route exists: sharing is what lets another program have the device.
    //
    // The product decision this encodes (2026-10-01): shared mode does not pursue
    // bit-perfectness. It is not known here whether Windows would in fact preserve
    // the samples -- that is untested, because for a long time there was no way to
    // ask. What is not untested is that we cannot *claim* it either way, so the
    // flag is false and this reason says which fact is the reason.
    SharedMode = 7,
};

struct OutputTarget {
    uint32_t rate = 0;
    bool resampling = false;
    // True only when the source samples reach the device unmodified. This is the
    // single value the UI must consult before saying "bit-perfect", because
    // dither and resampling both make the claim false.
    bool bitPerfect = false;
    // The name of the verdict. The boolean decides what the UI may *claim*; this
    // decides what it may *say*, and the two are different questions.
    PerfectReason reason = PerfectReason::DeviceCannotCarryFormat;
    // True when `rate` is not what the mode asked for, because the endpoint
    // refused it. The interface needs this to avoid telling the listener to
    // change a setting that was not the thing that changed.
    bool coercedToSupportedRate = false;
};

// The rates an endpoint will actually open in exclusive mode.
//
// Empty means "not probed", which is treated as "anything goes" rather than as
// "nothing works" -- an unprobed device must behave exactly as it did before
// this existed, and refusing everything would be a far worse failure than the
// one being fixed.
struct SupportedRates {
    const uint32_t* rates = nullptr;
    uint32_t count = 0;

    bool empty() const { return rates == nullptr || count == 0; }
    bool has(uint32_t rate) const {
        for (uint32_t i = 0; i < count; ++i) {
            if (rates[i] == rate) return true;
        }
        return false;
    }
};

// The closest rate in `supported` to `want`, in log-distance.
//
// Log rather than linear, because the rates being chosen between are ratios of
// each other: 352.8 kHz to 192 kHz is not "160.8 kHz away" in any useful sense,
// it is a factor of 1.84, and the factor is what an ear hears as the
// conversion. A linear comparison ranks 192 kHz closer to 44100 (147.9 kHz away)
// than to 96000 (96 kHz), which is not the answer anyone wants.
inline uint32_t nearestSupportedRate(const SupportedRates& supported, uint32_t want) {
    uint32_t best = 0;
    double bestDistance = 0.0;
    for (uint32_t i = 0; i < supported.count; ++i) {
        const uint32_t candidate = supported.rates[i];
        if (candidate == 0) continue;
        const double distance = std::log(static_cast<double>(want) /
                                         static_cast<double>(candidate));
        const double magnitude = distance < 0.0 ? -distance : distance;
        if (best == 0 || magnitude < bestDistance) {
            best = candidate;
            bestDistance = magnitude;
        }
    }
    return best;
}

// Decide the rate to open the device at.
//
// `sourceRate` is the file's own rate. `deviceRate` is the endpoint's native
// rate. `mode` and `fixedRate` come from the user's setting; `fixedRate` is
// ignored unless mode is Fixed. `supported` is what the endpoint was measured to
// accept; pass an empty one to skip the check.
//
// Returns false when the request cannot be satisfied — currently only a Fixed
// rate of zero.
inline bool resolveOutputTarget(OutputMode mode, uint32_t sourceRate, uint32_t deviceRate,
                                uint32_t fixedRate, const SupportedRates& supported,
                                OutputTarget* out) {
    if (!out) return false;
    out->coercedToSupportedRate = false;

    switch (mode) {
        case OutputMode::Native:
            // The device is reformatted to the source rate, so our path does no
            // conversion at all and the file's samples reach the DAC unchanged.
            //
            // Both flags are constant, and that is the point. An earlier version
            // computed `resampling` by comparing the source against the
            // endpoint's *native* rate, which made Native mode claim both
            // "resampling" and "bit-perfect" simultaneously — two statements that
            // cannot both be true, and the UI is entitled to act on either.
            //
            // The endpoint's native rate is irrelevant here: we are asking the
            // device to become the source, not the other way round.
            //
            // `bitPerfect` is a bad name for what this computes and the reason code
            // carries the honest label; see the NAE_OpenExclusive comment in nae.h.
            // It is true here and it means one thing only: no rate conversion was
            // needed. It says nothing about depth, channel count or sample format,
            // and the decoder's float32 intermediate means the samples are re-packed
            // even when this is true. `PerfectReason::BitPerfect` is likewise read as
            // "rate unchanged" by src/scripts/audio/reasonCodes.js, which is the
            // interface deliberately describing it in the only terms it can verify.
            out->rate = sourceRate;
            out->resampling = false;
            out->bitPerfect = true;
            out->reason = PerfectReason::BitPerfect;
            break;

        case OutputMode::Device:
            // The device stays at its own rate and we convert to meet it. This is
            // the mode where the two can genuinely differ.
            out->rate = deviceRate;
            out->resampling = (sourceRate != deviceRate);
            out->bitPerfect = (sourceRate == deviceRate);
            out->reason = out->bitPerfect
                              ? PerfectReason::BitPerfect
                              : PerfectReason::ResampledToDeviceRate;
            break;

        case OutputMode::Fixed:
            if (fixedRate == 0) return false;
            out->rate = fixedRate;
            out->resampling = (sourceRate != fixedRate);
            out->bitPerfect = (sourceRate == fixedRate);
            // A distinct code from the Device case, and not by accident: the fix
            // for this one is the fixed-rate setting, and a listener sent to the
            // output-mode control for it would be sent to the right place by
            // accident and to the wrong place by the description.
            out->reason = out->bitPerfect
                              ? PerfectReason::BitPerfect
                              : PerfectReason::ResampledToFixedRate;
            break;

        default:
            return false;
    }

    // The check that makes DSD playable, and the reason it lives here rather
    // than in the open path: this is where the rate is *decided*, so this is
    // where "the device will not do that" has to be answered. Handling it after
    // the fact means an Initialize that fails, and the only recovery is an error
    // telling the listener their file is unsupported -- which is a lie, because
    // the file is fine and the driver is the limit. A DSD64 file decodes
    // perfectly to 352.8 kHz and is refused by an endpoint whose exclusive rates
    // stop at 192 kHz, and the listener is told nothing they can act on.
    if (!supported.empty() && !supported.has(out->rate)) {
        const uint32_t replacement = nearestSupportedRate(supported, out->rate);
        if (replacement != 0 && replacement != out->rate) {
            out->rate = replacement;
            out->coercedToSupportedRate = true;
            // Bit-perfect is now false whatever the mode decided, and the mode's
            // own reason is no longer the true one: the listener did not choose
            // this rate, the driver did.
            out->resampling = true;
            out->bitPerfect = false;
            out->reason = PerfectReason::ResampledToSupportedRate;
        }
    }

    return true;
}

}  // namespace neiro
