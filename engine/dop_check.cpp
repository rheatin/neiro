// DoP without a device: parse a DSF/DFF header and check the packer against the
// bytes the container actually stores.
//
// DFF is MSB-first and byte-interleaved; DSF is LSB-first in per-channel blocks.
// A packer that mixes those up still produces 24-bit words and a locked-looking
// stream, so the payload is compared to the source bytes here rather than left
// to an ear tomorrow.

#include "dop_pack.h"
#include "dsd_downrate.h"
#include "nae.h"
#include "dff_reader.h"
#include "dsf_reader.h"
#include "utf8_file.h"
#include "utf8_args.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-48s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

bool startsWith(const char* path, const char* magic) {
    std::FILE* f = neiro::openUtf8(path, "rb");
    if (!f) return false;
    char b[5] = {};
    const bool ok = std::fread(b, 1, 4, f) == 4 && std::strcmp(b, magic) == 0;
    std::fclose(f);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: dop_check <file.dff|file.dsf>\n");
        return 1;
    }
    const char* path = args[1].c_str();

    // The marker is a fixed protocol detail, not a user setting.
    uint8_t toggle = 0x05;
    check(dop::nextMarker(&toggle) == 0x05 && toggle == 0xFA, "marker starts 0x05 then 0xFA");
    check(dop::nextMarker(&toggle) == 0xFA && toggle == 0x05, "marker alternates back");
    check(dop::reverseBits(0xB2) == 0x4D, "bit reversal is checked",
          std::to_string(dop::reverseBits(0xB2)));

    // Bitstream downrate, device-free: the decimated bytes must be exactly what
    // the packer then carries, and invalid targets must be refused before any
    // device is opened.
    if (startsWith(path, "DSD ")) {
        dsf::File dsf;
        if (dsf.open(path) && dsf.fmt.dsdRate > 2822400u) {
            std::vector<std::uint8_t> block;
            if (dsf.readBlockRaw(0, &block)) {
                std::vector<std::uint8_t> interleaved(block.size());
                const std::size_t perChannel = dsf.fmt.blockSize;
                for (std::size_t i = 0; i < perChannel; ++i) {
                    interleaved[i * 2] = block[i];
                    interleaved[i * 2 + 1] = block[perChannel + i];
                }
                neiro::dsd::Downrate down;
                std::string err;
                check(down.configure(dsf.fmt.dsdRate, dsf.fmt.dsdRate / 2, 2, false, &err),
                      "downrate configures for the container's rate");
                std::vector<std::uint8_t> out;
                down.feed(interleaved.data(), interleaved.size(), &out);
                check(out.size() + 2 >= interleaved.size() / 2 &&
                          out.size() <= interleaved.size() / 2,
                      "the downrated stream is half the bytes",
                      std::to_string(out.size()));
                // What PlayDop packs: word(marker, earlier byte, later byte).
                const std::uint32_t word = dop::word(0x05, out[0], out[2]);
                check(((word >> 16) & 0xFF) == 0x05 && ((word >> 8) & 0xFF) == out[0] &&
                          (word & 0xFF) == out[2],
                      "the packed word carries the decimated payload");
            }
        }
        // Rejections happen before the device is touched, so they are testable
        // without a DoP-capable endpoint.
        NAE_Handle* handle = nullptr;
        int32_t carrier = 0, chn = 0, bits = 0;
        double lat = 0.0;
        dsf::File probeDsf;
        if (probeDsf.open(path)) {
            const int32_t source = static_cast<int32_t>(probeDsf.fmt.dsdRate);
            const int32_t tooHigh = source * 2;
            check(NAE_OpenDopDownrated(nullptr, path, tooHigh, &handle, &carrier, &chn, &bits,
                                       &lat) == NAE_ERR_INVALID_ARGUMENT,
                  "a target above the source rate is rejected");
            check(NAE_OpenDopDownrated(nullptr, path, source / 3, &handle, &carrier, &chn,
                                       &bits, &lat) == NAE_ERR_INVALID_ARGUMENT,
                  "a non-dividing target is rejected");
            check(handle == nullptr, "a rejected downrate opens no handle");
        }
    }

    if (startsWith(path, "FRM8")) {
        dff::File dff;
        if (!dff.open(path)) {
            std::printf("DFF open failed: %s\n", dff.error.c_str());
            return 1;
        }
        check(dff.fmt.sampleRate > 0, "DFF sample rate", std::to_string(dff.fmt.sampleRate));
        check(dff.fmt.channels == 2, "DFF is stereo", std::to_string(dff.fmt.channels));
        check(dff.fmt.dataBytes >= 4, "DFF has data", std::to_string(dff.fmt.dataBytes));
        check(dff.totalDopFrames() == dff.fmt.dataBytes / (dff.fmt.channels * 2),
              "DFF frame count", std::to_string(dff.totalDopFrames()));

        std::uint8_t bytes[4] = {};
        check(dff.readData(0, bytes, 4), "DFF first frame is readable");
        // Stereo byte order: L0 R0 L1 R1. Payload: earlier=L0, later=L1.
        const std::uint32_t left = dop::dffWord(0x05, bytes[0], bytes[2]);
        const std::uint32_t right = dop::dffWord(0x05, bytes[1], bytes[3]);
        check(((left >> 16) & 0xFF) == 0x05 && ((left >> 8) & 0xFF) == bytes[0] &&
                  (left & 0xFF) == bytes[2],
              "DFF left word keeps MSB-first bytes");
        check(((right >> 16) & 0xFF) == 0x05 && ((right >> 8) & 0xFF) == bytes[1] &&
                  (right & 0xFF) == bytes[3],
              "DFF right word keeps MSB-first bytes");
        check((dff.fmt.sampleRate / 16) > 0, "DFF DoP carrier",
              std::to_string(dff.fmt.sampleRate / 16));
    } else if (startsWith(path, "DSD ")) {
        dsf::File dsf;
        if (!dsf.open(path)) {
            std::printf("DSF open failed: %s\n", dsf.error.c_str());
            return 1;
        }
        check(dsf.fmt.dsdRate > 0, "DSF DSD rate", std::to_string(dsf.fmt.dsdRate));
        check(dsf.fmt.channels == 2, "DSF is stereo", std::to_string(dsf.fmt.channels));
        check(dsf.fmt.blockSize > 0, "DSF block size", std::to_string(dsf.fmt.blockSize));
        std::vector<std::uint8_t> raw;
        check(dsf.readBlockRaw(0, &raw), "DSF first block is readable");
        if (raw.size() >= 4) {
            const std::size_t bs = dsf.fmt.blockSize;
            const std::uint32_t left = dop::dsfWord(0xFA, raw[0], raw[1]);
            const std::uint32_t right = dop::dsfWord(0xFA, raw[bs], raw[bs + 1]);
            check(((left >> 8) & 0xFF) == dop::reverseBits(raw[0]) &&
                      (left & 0xFF) == dop::reverseBits(raw[1]),
                  "DSF left payload is bit-reversed");
            check(((right >> 8) & 0xFF) == dop::reverseBits(raw[bs]) &&
                      (right & 0xFF) == dop::reverseBits(raw[bs + 1]),
                  "DSF right payload is bit-reversed");
        }
    } else {
        std::printf("not DFF or DSF: %s\n", path);
        return 1;
    }

    std::printf("\n%s\n", failures == 0 ? "All DoP packing checks passed."
                                        : "Some DoP packing checks FAILED.");
    return failures == 0 ? 0 : 1;
}
