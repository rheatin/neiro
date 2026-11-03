// DoP v1.1 packing.
//
// One 24-bit carrier sample per channel:
//   bits 23..16 marker (0x05 / 0xFA, alternating every carrier frame)
//   bits 15..8  earlier DSD byte
//   bits 7..0   later DSD byte
//
// DSF stores DSD LSB-first; DFF stores it MSB-first. The payload is sent
// MSB-first, so a DSF byte is reversed and a DFF byte is not. Getting that wrong
// still produces a locked stream and audible noise, which is why this is a
// separate, testable function.

#pragma once

#include <cstdint>

namespace dop {

inline uint8_t reverseBits(uint8_t v) {
    v = static_cast<uint8_t>(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = static_cast<uint8_t>(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    return static_cast<uint8_t>(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
}

inline uint32_t word(uint8_t marker, uint8_t earlier, uint8_t later) {
    return (static_cast<uint32_t>(marker) << 16) | (static_cast<uint32_t>(earlier) << 8) |
           static_cast<uint32_t>(later);
}

/// The next marker and the toggle for the following frame.
inline uint8_t nextMarker(uint8_t* toggle) {
    const uint8_t marker = *toggle;
    *toggle = (marker == 0x05) ? 0xFA : 0x05;
    return marker;
}

/// DSF: two LSB-first bytes -> the DoP payload bytes, MSB-first.
inline uint32_t dsfWord(uint8_t marker, uint8_t first, uint8_t second) {
    return word(marker, reverseBits(first), reverseBits(second));
}

/// DFF: two MSB-first bytes -> the DoP payload bytes, MSB-first.
inline uint32_t dffWord(uint8_t marker, uint8_t first, uint8_t second) {
    return word(marker, first, second);
}

}  // namespace dop
