// Uniform-partitioned FFT convolution for the convolver stage.
//
// Platform-neutral. configure() with the IR, then process() an interleaved
// buffer in place.
//
// Block size is fixed at 256 samples; the FFT is 512 points. The IR is split
// into 256-sample partitions. Output has one block of latency, which the caller
// must account for.
//
// One state set per channel: a 6-channel stream needs six IRs and six histories.
// A channel whose IR is empty gets a centred delta, so it passes through at the
// same latency as the others instead of being silently bypassed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace neiro {

class PartitionedConvolver {
  public:
    PartitionedConvolver();
    ~PartitionedConvolver();

    // Two-channel form. `right` empty means "one IR for both channels", which is
    // what a mono IR means. Both channels always get built; an empty `right`
    // used to leave channel 1 silent.
    bool configure(const std::vector<float>& left, const std::vector<float>& right);

    // Per-channel form: one IR per channel, in stream channel order. An empty
    // entry for a channel is an identity (centred delta) at the same latency.
    // Returns false for zero channels, all-empty IRs, or too many partitions.
    bool configure(const std::vector<std::vector<float>>& perChannel);

    // In-place, interleaved. `channels` must equal the configured count; extra
    // channels are left untouched and the count is reported by channels().
    void process(float* interleaved, size_t frames, size_t channels);
    void reset();

    size_t channels() const;

    static constexpr size_t blockSize() { return 256; }
    static constexpr size_t fftSize() { return 512; }
    static constexpr size_t maxPartitions() { return 32; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neiro
