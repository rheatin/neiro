// `bit_perfect` must account for the source, not only the device.
//
// Every stage between the decoder and the device is float32, which carries 24
// significant bits. A 24-bit source survives that exactly; a 32-bit integer
// source loses its low eight bits on the way in. The flag used to look only at
// the device side -- resampling, dithering, channel conversion, coercion -- so a
// 32-bit file on a 32-bit endpoint was reported as bit-perfect after the engine
// had already truncated it.
//
// The three files are generated here at the same rate, so the only difference
// between them is depth. They are opened in Native mode so the device is asked
// for the file's own rate: a resampling verdict would mask the one being
// measured.
//
// # What this can and cannot assert on a foreign machine
//
// Whether a 16-bit file is bit-perfect depends on the endpoint accepting its
// rate, which is not this check's subject. So the portable assertion is the one
// that must hold everywhere: a 32-bit source is never bit-perfect. When the
// 16-bit file *is* clean -- which proves this device accepted the rate and that
// no other stage intervened -- the 32-bit verdict must name the source and not
// something else, and that is asserted too.

#include "nae.h"
#include "utf8_args.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// One second of a 1 kHz sine at `bits` per sample, stereo, 44.1 kHz.
bool writeWav(const char* path, int bits) {
    constexpr std::uint32_t rate = 44100;
    constexpr std::uint32_t frames = rate;
    constexpr std::uint16_t channels = 2;
    const int bytesPerSample = bits / 8;
    const std::uint32_t dataBytes = frames * channels * bytesPerSample;

    std::vector<std::uint8_t> wav;
    wav.reserve(44 + dataBytes);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(wav, 36 + dataBytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(wav, 16);
    put16(wav, 1);  // PCM
    put16(wav, channels);
    put32(wav, rate);
    put32(wav, rate * channels * bytesPerSample);
    put16(wav, static_cast<std::uint16_t>(channels * bytesPerSample));
    put16(wav, static_cast<std::uint16_t>(bits));
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(wav, dataBytes);

    for (std::uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const double s = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t);
        // The low bits are what the float32 path would drop, so fill them with
        // something: a sample that is only a power-of-two of the top bits would
        // survive even a lossy round trip and make the check pass for the wrong
        // reason.
        for (int c = 0; c < channels; ++c) {
            if (bits == 16) {
                const std::int16_t v = static_cast<std::int16_t>(s * 32767.0);
                put16(wav, static_cast<std::uint16_t>(v));
            } else if (bits == 24) {
                const std::int32_t v = static_cast<std::int32_t>(s * 8388607.0);
                const std::uint32_t u = static_cast<std::uint32_t>(v) & 0xFFFFFFu;
                wav.push_back(static_cast<std::uint8_t>(u));
                wav.push_back(static_cast<std::uint8_t>(u >> 8));
                wav.push_back(static_cast<std::uint8_t>(u >> 16));
            } else {
                // 32-bit: use the full word, including bits a float cannot hold.
                const std::int32_t v =
                    static_cast<std::int32_t>(s * 2147483000.0) + 0x00000FFF;
                put32(wav, static_cast<std::uint32_t>(v));
            }
        }
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(wav.data(), 1, wav.size(), f) == wav.size();
    std::fclose(f);
    return ok;
}

struct Verdict {
    bool bitPerfect = false;
    int32_t reason = 0;
    bool opened = false;
};

Verdict openNative(const char* path) {
    Verdict v;
    NAE_Handle* handle = nullptr;
    int32_t rate = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, NAE_QUALITY_HIGH,
                                    &handle, &rate, &sourceRate, &channels, &bits, &valid,
                                    &isFloat, &resampling, &dithered, &bitPerfect,
                                    &fileChannels, &durationMs, &latencyMs);
    if (rc != NAE_OK || !handle) {
        char buf[256] = {0};
        NAE_GetLastErrorText(buf, static_cast<int32_t>(sizeof(buf)));
        std::printf("  open failed rc=%d %s\n", rc, buf);
        return v;
    }
    NAE_SignalPath signal{};
    signal.structSize = sizeof(signal);
    if (NAE_QuerySignalPath(handle, &signal) == NAE_OK) {
        v.bitPerfect = signal.bitPerfect != 0;
        v.reason = signal.reason;
        v.opened = true;
    }
    NAE_Close(handle);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    // The UTF-8 helper is not needed for the paths below, but keeping the shape
    // of every other harness here means this one is run the same way.
    (void)utf8Args(argc, argv);

    const char* p16 = "width_16.wav";
    const char* p24 = "width_24.wav";
    const char* p32 = "width_32.wav";
    check(writeWav(p16, 16), "16-bit fixture");
    check(writeWav(p24, 24), "24-bit fixture");
    check(writeWav(p32, 32), "32-bit fixture");

    const Verdict v16 = openNative(p16);
    const Verdict v24 = openNative(p24);
    const Verdict v32 = openNative(p32);
    check(v16.opened && v24.opened && v32.opened, "all three open in native mode");

    std::printf("  16-bit: bitPerfect=%d reason=%d\n", v16.bitPerfect ? 1 : 0, v16.reason);
    std::printf("  24-bit: bitPerfect=%d reason=%d\n", v24.bitPerfect ? 1 : 0, v24.reason);
    std::printf("  32-bit: bitPerfect=%d reason=%d\n", v32.bitPerfect ? 1 : 0, v32.reason);

    // Portable: true whatever endpoint this runs on. A 32-bit integer source
    // cannot survive a float32 path, so no device can make this true.
    check(!v32.bitPerfect, "a 32-bit source is never bit-perfect");

    // When the 16-bit file is clean, this device accepted the rate and nothing
    // else interfered -- so the 32-bit verdict can be attributed exactly.
    if (v16.bitPerfect) {
        check(v24.bitPerfect, "24 bits still fit the 24-bit float path");
        check(v32.reason == NAE_REASON_SOURCE_PRECISION,
              "the 32-bit verdict names the source, not something else",
              std::to_string(v32.reason));
    } else {
        std::printf(
            "  [INFO] this endpoint is not bit-perfect for a 16-bit file either "
            "(reason %d);\n         the source-precision attribution is not asserted\n",
            v16.reason);
    }

    std::remove(p16);
    std::remove(p24);
    std::remove(p32);

    std::printf("\n%s\n", failures == 0 ? "All source-width checks passed."
                                        : "Some source-width checks FAILED.");
    return failures == 0 ? 0 : 1;
}
