// Sample rate conversion, wrapped around soxr.
//
// Why a wrapper rather than calling soxr directly from the render path:
//
//   * soxr is a C library with output/input callback variants. The render path
//     wants a simple "give me these frames, here are your output frames", and
//     keeping that translation in one place means the streaming and one-shot
//     cases cannot drift apart.
//   * soxr needs a scratch buffer. Allocating inside the render callback is not
//     acceptable, so the buffer is reserved in prepare() and reused.
//   * Quality tiers map to soxr quality/reciprocal-precision pairs. Those
//     numbers are soxr's business, not the caller's.
//
// Bit depth is deliberately *not* handled here. soxr works in floating point,
// and converting to the target integer depth is a separate concern with its own
// dither rules — see dither.h. Mixing the two would make "resample" and
// "requantise" impossible to reason about separately, which is exactly the kind
// of coupling that produces audible mistakes.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace neiro {

// Conversion quality. Mirrors the tiers the reference engine exposes, and maps
// onto soxr's quality + precision settings.
enum class ResamplerQuality {
    // soxr's "quick" recipe. For previews and scrubbing, where latency matters
    // more than the last few dB.
    Fast,
    // soxr's "medium"/"high" recipes. The default.
    High,
    // soxr's "very high" with maximum precision. Slowest, and what you want
    // when the file rate and the device rate differ.
    VeryHigh,
};

class Resampler {
  public:
    Resampler() = default;

    ~Resampler();
    Resampler(const Resampler&) = delete;
    Resampler& operator=(const Resampler&) = delete;

    // Configure for interleaved float samples. Must be called before process().
    // `outRate` is the device rate; `inRate` is the source rate.
    bool prepare(uint32_t channels, uint32_t inRate, uint32_t outRate,
                 ResamplerQuality quality, std::string* error);

    bool ready() const { return ready_; }

    // Convert `in` (interleaved, at the configured input rate) and append the
    // result to `out`. Returns false only on a soxr error.
    //
    // Streaming-safe: soxr's filter delay carries across calls, and any output it
    // produced beyond what the previous caller consumed is held here and
    // returned on the next call. The caller does not have to track how much input
    // is owed.
    //
    // This does NOT flush. Flushing is `flush()`, and it must only be called when
    // the source has genuinely ended. Calling it per block resets the filter
    // delay every time, which silently reduces the resampler to a 1:1 copier —
    // a bug that is completely invisible in one-shot use and ruins streaming.
    bool process(const std::vector<float>& in, std::vector<float>* out, std::string* error);

    // Drain the filter's held-back tail. Call once, when the source ends.
    //
    // Without this a conversion is short by its filter latency, which is how a
    // track ends up with a truncated last fraction of a second.
    bool flush(std::vector<float>* out, std::string* error);

    // Output frames ready to be taken but not yet consumed.
    size_t pending_frames() const { return pending_.size() / (channels_ ? channels_ : 1); }

    // The fixed number of *input* frames soxr is fed, and therefore the smallest
    // amount of input that can produce any output at all.
    //
    // This has to be public because the caller sizes its requests against it. A
    // request smaller than one block is silently absorbed into the input FIFO and
    // returns nothing, so a caller that asks for less and assumes it got output
    // will consume source, produce silence, and report success.
    size_t input_block_frames() const { return inputBlockFrames_; }

    // Discard buffered samples and history. Call on seek.
    void reset();

    uint32_t in_rate() const { return inRate_; }
    uint32_t out_rate() const { return outRate_; }
    uint32_t channels() const { return channels_; }

  private:
    bool fail(std::string* error, const char* what);

    // Copy `frames` frames out of the scratch buffer into the caller's output.
    void appendScratch(std::vector<float>* out, size_t frames) const;

    // soxr reports failures as a string rather than a status code.
    static bool processError(std::string* error, const char* what);

    void* soxr_ = nullptr;   // soxr_t, kept opaque so this header needs no C header
    uint32_t channels_ = 0;
    uint32_t inRate_ = 0;
    uint32_t outRate_ = 0;
    ResamplerQuality quality_ = ResamplerQuality::High;
    bool ready_ = false;

    // soxr's output buffer. Reserved in prepare() so process() never allocates:
    // the render thread cannot afford a heap hit, and a failed allocation there
    // would be an underrun.
    std::vector<float> scratch_;

    // Source samples waiting to be handed to soxr as whole blocks. This is what
    // decouples the caller's block size from the converter's, which is required
    // rather than merely tidy: irregular input blocks drove soxr into an
    // out-of-bounds write inside its own deinterleave, and because the engine is
    // a DLL in a host process, that killed the app with no message at all.
    std::vector<float> inQueue_;

    // Converted frames not yet handed to the caller, so a short block does not
    // lose samples soxr already produced.
    std::vector<float> pending_;

    // The one and only input block size soxr is ever given. Fixed at prepare
    // time so the output it can produce always fits scratch_.
    size_t inputBlockFrames_ = 0;
};

}  // namespace neiro
