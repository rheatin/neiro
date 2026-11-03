// DSD bitstream downrate: 1-bit at one rate to 1-bit at a lower rate.
//
// Two stages per channel:
//   1. a low-pass FIR on the ±1 stream, evaluated once every `factor` input bits,
//      with the stopband at the new Nyquist so the noise that would fold back is
//      removed;
//   2. a second-order sigma-delta modulator that re-quantises the multi-bit
//      result to one bit, keeping the noise shaped above the audio band.
//
// Taking every D-th bit without the filter would alias the shaped noise
// straight into the audio band. Converting to PCM and back would discard the
// noise shaping and add a second resampler; this keeps the stream DSD.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neiro {
namespace dsd {

class Downrate {
  public:
    // `inRate`/`outRate` are bit rates (DSD128 = 5644800). `factor` must divide
    // evenly and be 1, 2, 4 or 8. `msbFirst` describes the byte bit order of
    // the caller's stream; the output uses the same order.
    bool configure(uint32_t inRate, uint32_t outRate, uint32_t channels, bool msbFirst,
                   std::string* error);

    // Interleaved input bytes (one byte = eight bits = eight samples per
    // channel). Appends interleaved output bytes. A byte is emitted only when
    // every channel has accumulated eight output bits, so the output length can
    // lag the input by less than one byte.
    void feed(const uint8_t* bytes, size_t byteCount, std::vector<uint8_t>* out);

    void reset();

    uint32_t factor() const { return factor_; }
    bool configured() const { return configured_; }
    uint32_t channels() const { return channels_; }

  private:
    struct Channel {
        std::vector<double> history;  // ring, newest at pos-1
        size_t pos = 0;
        uint32_t phase = 0;
        double i1 = 0.0;
        double i2 = 0.0;
        double previous = 1.0;
        uint8_t outByte = 0;
        uint32_t outBits = 0;
        uint8_t pendingByte = 0;
        bool hasByte = false;
    };

    /// Returns -1 when the decimator produced no new bit yet, else 0 or 1.
    int pushBit(uint32_t channel, double sample);
    double filterOutput(const Channel& c) const;

    std::vector<double> taps_;
    std::vector<Channel> channelsData_;
    uint32_t factor_ = 1;
    uint32_t channels_ = 0;
    bool msbFirst_ = false;
    bool configured_ = false;
};

}  // namespace dsd
}  // namespace neiro
