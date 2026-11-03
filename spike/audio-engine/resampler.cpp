#include "resampler.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <soxr.h>
}

namespace neiro {

namespace {

// How much output to ask soxr for per call.
//
// soxr will not produce more input-rate-dependent output than it can complete,
// so this is a bound, not a promise. Keeping it modest means the scratch buffer
// stays small while still amortising the per-call overhead.
constexpr size_t kScratchFrames = 4096;

// Ceiling on what one process() call will hand back. A caller that converts a
// whole track in one go would otherwise be able to force an allocation
// proportional to the file, on the audio thread.
constexpr size_t kMaxOutputSamples = 8 * 1024 * 1024;  // samples, ~32 MB of stereo float

// Map our tiers onto soxr's quality struct.
//
// soxr exposes its named recipes as macros (SOXR_QQ/LQ/MQ/HQ/VHQ) that expand to
// full struct initialisers, so there is no "recipe id" to pass — the struct is
// the API. The named constants are used where they exist because they encode
// passband and stopband choices that were tuned together; the numbers are only
// spelled out where no recipe matches.
//
// passband_end is the fraction of Nyquist kept flat. 0.913 is soxr's own
// default and is deliberately below 1.0: keeping the entire band to Nyquist
// would need a very long filter, and the top octave is not where music
// intelligibility lives.
soxr_quality_spec_t qualitySpec(ResamplerQuality q) {
    switch (q) {
        case ResamplerQuality::Fast:
            // 'Quick' — cubic interpolation, an order of magnitude cheaper than
            // the sinc recipes and audibly duller above about 8 kHz. For scrub
            // previews, where following the playhead matters more than the top
            // octave.
            return soxr_quality_spec(SOXR_QQ, 0);

        case ResamplerQuality::High:
            // 'High' — 20-bit precision, the sensible default.
            return soxr_quality_spec(SOXR_HQ, 0);

        case ResamplerQuality::VeryHigh:
            // 'Very high' — 28-bit precision. This is the tier worth using
            // whenever the file rate and the device rate actually differ, which
            // is most of the time with a 192 kHz DAC.
            return soxr_quality_spec(SOXR_VHQ, 0);
    }
    return soxr_quality_spec(SOXR_HQ, 0);
}

}  // namespace

Resampler::~Resampler() {
    if (soxr_) soxr_delete(static_cast<soxr_t>(soxr_));
}

bool Resampler::fail(std::string* error, const char* what) {
    if (error) {
        *error = std::string("resampler: ") + what;
        if (soxr_) {
            const char* detail = soxr_error(static_cast<soxr_t>(soxr_));
            if (detail && *detail) {
                *error += " (";
                *error += detail;
                *error += ")";
            }
        }
    }
    return false;
}

bool Resampler::prepare(uint32_t channels, uint32_t inRate, uint32_t outRate,
                        ResamplerQuality quality, std::string* error) {
    if (channels == 0 || inRate == 0 || outRate == 0) {
        return fail(error, "channels and rates must be non-zero");
    }

    if (soxr_) {
        soxr_delete(static_cast<soxr_t>(soxr_));
        soxr_ = nullptr;
        ready_ = false;
    }

    channels_ = channels;
    inRate_ = inRate;
    outRate_ = outRate;
    quality_ = quality;

    // Interleaved 32-bit float on both sides. soxr's SOXR_FLOAT32_I is
    // "interleaved float", as opposed to the _S variants which are split
    // (planar) — the pipeline here is interleaved throughout.
    //
    // soxr_io_spec takes only the two datatypes; the channel count goes to
    // soxr_create. Passing it here was a third argument that does not exist.
    const soxr_io_spec_t iospec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    const soxr_quality_spec_t qspec = qualitySpec(quality);

    // Reserving before create() keeps process() allocation-free. Over-reserving
    // is cheap: a few tens of kilobytes on a thread that already holds the
    // render buffer.
    scratch_.assign(kScratchFrames * channels, 0.0f);
    pending_.clear();
    inQueue_.clear();

    // The fixed input block soxr is fed, chosen so the output it can produce
    // for one block comfortably fits the scratch buffer. Sizing it by the
    // device-to-source ratio is what stops soxr owing more frames than we have
    // room for -- an earlier version computed this per call from whatever the
    // caller passed, which is how soxr ended up writing past its own buffer.
    const size_t ratio = outRate > inRate ? static_cast<size_t>((outRate + inRate - 1) / inRate) : 1;
    inputBlockFrames_ = std::max<size_t>(1, kScratchFrames / (ratio + 1));
    // Reserve the FIFO so feeding a block never reallocates it mid-render.
    inQueue_.reserve(inputBlockFrames_ * 4 * channels);

    // Argument order is (rate_in, rate_out, channels, &err, &io, &quality,
    // &runtime) — the error out-param comes *before* the specs, which is easy
    // to get wrong and produces a type error rather than a runtime one.
    soxr_error_t createError = nullptr;
    soxr_t handle = soxr_create(inRate, outRate, channels, &createError, &iospec, &qspec,
                                nullptr);
    if (!handle) {
        if (error) {
            *error = "resampler: soxr_create failed";
            if (createError) {
                *error += " (";
                *error += createError;
                *error += ")";
            }
        }
        return false;
    }

    soxr_ = handle;
    ready_ = true;
    return true;
}

void Resampler::reset() {
    if (soxr_) soxr_clear(static_cast<soxr_t>(soxr_));
    pending_.clear();
    inQueue_.clear();
}

bool Resampler::processError(std::string* error, const char* what) {
    if (error) {
        *error = std::string("resampler: soxr_process failed (") + what + ")";
    }
    return false;
}

void Resampler::appendScratch(std::vector<float>* out, size_t frames) const {
    out->insert(out->end(), scratch_.begin(),
                scratch_.begin() + static_cast<long>(frames * channels_));
}

bool Resampler::process(const std::vector<float>& in, std::vector<float>* out,
                        std::string* error) {
    if (!ready_) return fail(error, "prepare() has not been called");
    if (!out) return fail(error, "no output buffer");

    // Hand back anything already converted and waiting. This is what lets the
    // caller ask for a block's worth without tracking soxr's filter latency.
    if (!pending_.empty()) {
        out->insert(out->end(), pending_.begin(), pending_.end());
        pending_.clear();
    }

    if (in.empty()) return true;

    // Append to the input FIFO, then convert whole blocks out of it.
    //
    // soxr is fed a *constant* block size, never whatever the caller happened to
    // supply. Two reasons, and the second one is not a theory:
    //
    //   * soxr sizes internal ring buffers from the block sizes it is given.
    //     Handing it a stream of irregular sizes (472 one call, 819 the next)
    //     drove it into `_soxr_deinterleave_f` writing past its own buffer, and
    //     since the engine lives in a DLL inside a host process, that killed
    //     the whole app with no message. Reproduced under gdb, thread
    //     "neiro-audio", SIGSEGV in `_soxr_deinterleave_f`.
    //
    //   * a fixed block is also the only sane shape for a realtime path: the
    //     render callback then never has to reason about how much soxr will
    //     consume this call.
    //
    // The FIFO absorbs the difference, so callers can pass whatever is
    // convenient.
    inQueue_.insert(inQueue_.end(), in.begin(), in.end());

    const size_t blockSamples = inputBlockFrames_ * channels_;
    size_t fed = 0;
    while (inQueue_.size() - fed >= blockSamples) {
        size_t consumed = 0;
        size_t produced = 0;

        const soxr_error_t err =
            soxr_process(static_cast<soxr_t>(soxr_), inQueue_.data() + fed, inputBlockFrames_,
                         &consumed, scratch_.data(), kScratchFrames, &produced);
        if (err) return processError(error, err);

        if (produced) appendScratch(out, produced);

        if (consumed == 0) break;  // no forward progress; avoid spinning
        fed += consumed * channels_;
    }

    // Keep whatever soxr has not consumed yet for the next call.
    if (fed) inQueue_.erase(inQueue_.begin(), inQueue_.begin() + static_cast<long>(fed));
    return true;
}

bool Resampler::flush(std::vector<float>* out, std::string* error) {
    if (!ready_) return fail(error, "prepare() has not been called");
    if (!out) return fail(error, "no output buffer");

    if (!pending_.empty()) {
        out->insert(out->end(), pending_.begin(), pending_.end());
        pending_.clear();
    }

    // First, hand soxr the tail that never filled a whole block.
    //
    // process() only feeds complete blocks, so up to one block's worth can be
    // sitting in the FIFO when the source ends. Skipping this loses those
    // samples outright: a 48000-frame conversion returned 47104 until it was
    // added.
    if (!inQueue_.empty()) {
        const size_t remaining = inQueue_.size() / channels_;
        size_t consumed = 0;
        size_t produced = 0;
        const soxr_error_t err =
            soxr_process(static_cast<soxr_t>(soxr_), inQueue_.data(), remaining, &consumed,
                         scratch_.data(), kScratchFrames, &produced);
        if (err) return processError(error, err);
        if (produced) appendScratch(out, produced);
        // Whatever soxr still holds back is drained below.
        inQueue_.clear();
    }

    // Then drain the filter's tail, by passing a null input pointer, which
    // soxr's header defines as end-of-input: "no data is available nor shall be
    // available".
    //
    // Passing zeros instead is not equivalent — it is more real audio, so soxr
    // has no reason to stop asking and the loop runs away. One attempt produced
    // 40 million frames of silence before the iteration guard caught it.
    for (int guard = 0; guard < 64; ++guard) {
        size_t consumed = 0;
        size_t produced = 0;
        const soxr_error_t err = soxr_process(static_cast<soxr_t>(soxr_), nullptr, 0, &consumed,
                                              scratch_.data(), kScratchFrames, &produced);
        if (err) return processError(error, err);
        if (produced == 0) break;  // nothing left to drain
        appendScratch(out, produced);
    }
    return true;
}

}  // namespace neiro
