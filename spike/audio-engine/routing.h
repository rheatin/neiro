// Complex routing matrix + Hilbert phase stage.
//
// Platform-neutral DSP shared by every backend. The render loop must call
// configure() before process(); process() performs no allocation.

#pragma once

#include "hilbert.h"
#include "nae_policy.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neiro {

class RoutingProcessor {
  public:
    bool configure(const NAE_RoutingMatrix& matrix, size_t maxFrames, std::string* error);
    bool ready() const { return ready_; }
    void reset();

    size_t source_channels() const { return sourceChannels_; }
    size_t target_channels() const { return targetChannels_; }
    size_t latency_frames() const { return latencyFrames_; }

    // Input is sourceChannels interleaved; output is targetChannels interleaved.
    // `frames` must not exceed the maxFrames passed to configure().
    void process(const float* in, size_t frames, float* out);

  private:
    bool ready_ = false;
    size_t sourceChannels_ = 0;
    size_t targetChannels_ = 0;
    size_t maxFrames_ = 0;
    size_t latencyFrames_ = 0;
    bool align_ = false;
    bool phase_ = false;
    bool iir_ = false;

    std::vector<float> real_;   // target * source
    std::vector<float> quad_;   // target * source
    std::vector<Quadrature> hilbert_;
    std::vector<std::vector<float>> hilbertOut_;
    std::vector<IirQuadrature> iirFilters_;
    std::vector<std::vector<float>> iirReal_;
    std::vector<std::vector<float>> iirImag_;
    std::vector<float> scratch_;
    std::vector<float> directFrame_;
    std::vector<std::vector<float>> delay_;
    std::vector<size_t> delayPos_;
    std::vector<uint32_t> sourceBits_;
    std::vector<uint32_t> targetBits_;

    // Bass management: one low-pass for the LFE row, fed from the routed sum.
    bool bass_ = false;
    size_t lfeIndex_ = 0;
    double lfeGain_ = 1.0;
    double lpAlpha_ = 0.0;
    double lpState_ = 0.0;
};

}  // namespace neiro

