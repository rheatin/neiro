// DSD bitstream downrate. See dsd_downrate.h.

#include "dsd_downrate.h"

#include "dsd_filter.h"

#include <algorithm>
#include <cmath>

namespace neiro {
namespace dsd {
namespace {

constexpr double kPassbandHz = 20000.0;
constexpr double kAttenuationDb = 90.0;

uint8_t bitAt(uint8_t byte, uint32_t index, bool msbFirst) {
    const uint32_t pos = msbFirst ? 7u - index : index;
    return static_cast<uint8_t>((byte >> pos) & 1u);
}

uint8_t putBit(uint8_t byte, uint32_t index, bool msbFirst, bool one) {
    if (!one) return byte;
    const uint32_t pos = msbFirst ? 7u - index : index;
    return static_cast<uint8_t>(byte | (1u << pos));
}

}  // namespace

bool Downrate::configure(uint32_t inRate, uint32_t outRate, uint32_t channels, bool msbFirst,
                         std::string* error) {
    configured_ = false;
    if (inRate == 0 || outRate == 0 || channels == 0) {
        if (error) *error = "downrate: rate or channel count is zero";
        return false;
    }
    if (inRate < outRate || inRate % outRate != 0) {
        if (error) *error = "downrate: the rates must divide evenly";
        return false;
    }
    const uint32_t factor = inRate / outRate;
    if (factor != 1 && factor != 2 && factor != 4 && factor != 8) {
        if (error) *error = "downrate: only 1, 2, 4 or 8 is supported";
        return false;
    }

    factor_ = factor;
    channels_ = channels;
    msbFirst_ = msbFirst;
    taps_.clear();
    if (factor > 1) {
        // Stopband at the new Nyquist: everything above it would fold into the
        // band that survives the decimation.
        const double stopband = static_cast<double>(outRate) * 0.5;
        taps_ = designDecimationFilter(static_cast<double>(inRate), kPassbandHz, stopband,
                                       kAttenuationDb);
        if (taps_.empty()) {
            if (error) *error = "downrate: filter design failed";
            return false;
        }
    }
    reset();
    configured_ = true;
    return true;
}

void Downrate::reset() {
    channelsData_.assign(channels_, Channel{});
    for (auto& c : channelsData_) {
        c.history.assign(std::max<size_t>(1, taps_.size()), 0.0);
    }
}

double Downrate::filterOutput(const Channel& c) const {
    const size_t n = c.history.size();
    double acc = 0.0;
    size_t idx = c.pos == 0 ? n - 1 : c.pos - 1;
    for (size_t k = 0; k < n; ++k) {
        acc += taps_[k] * c.history[idx];
        idx = idx == 0 ? n - 1 : idx - 1;
    }
    return acc;
}

int Downrate::pushBit(uint32_t channel, double sample) {
    Channel& c = channelsData_[channel];
    if (factor_ == 1) {
        return sample > 0.0 ? 1 : 0;
    }
    c.history[c.pos] = sample;
    c.pos = (c.pos + 1) % c.history.size();
    if (++c.phase < factor_) return -1;
    c.phase = 0;

    double x = filterOutput(c);
    x = std::max(-1.5, std::min(1.5, x));
    c.i1 += x - c.previous;
    c.i1 = std::max(-4.0, std::min(4.0, c.i1));
    c.i2 += c.i1 - c.previous;
    c.i2 = std::max(-8.0, std::min(8.0, c.i2));
    const double y = c.i2 >= 0.0 ? 1.0 : -1.0;
    c.previous = y;
    return y > 0.0 ? 1 : 0;
}

void Downrate::feed(const uint8_t* bytes, size_t byteCount, std::vector<uint8_t>* out) {
    if (!configured_ || bytes == nullptr || out == nullptr) return;
    if (channels_ == 0) return;
    if (factor_ == 1) {
        out->insert(out->end(), bytes, bytes + byteCount);
        return;
    }
    for (size_t i = 0; i + channels_ <= byteCount; i += channels_) {
        for (uint32_t c = 0; c < channels_; ++c) {
            const uint8_t byte = bytes[i + c];
            Channel& state = channelsData_[c];
            for (uint32_t bit = 0; bit < 8; ++bit) {
                const double sample = bitAt(byte, bit, msbFirst_) ? 1.0 : -1.0;
                const int bitValue = pushBit(c, sample);
                if (bitValue < 0) continue;
                state.outByte = putBit(state.outByte, state.outBits, msbFirst_, bitValue != 0);
                if (++state.outBits == 8) {
                    state.pendingByte = state.outByte;
                    state.outByte = 0;
                    state.outBits = 0;
                    state.hasByte = true;
                }
            }
        }
        bool allReady = true;
        for (const auto& c : channelsData_) allReady = allReady && c.hasByte;
        if (!allReady) continue;
        for (auto& c : channelsData_) {
            out->push_back(c.pendingByte);
            c.hasByte = false;
        }
    }
}

}  // namespace dsd
}  // namespace neiro
