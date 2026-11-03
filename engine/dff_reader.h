// Minimal DSDIFF (DFF) reader: header only, raw bytes on demand.
//
// DFF stores DSD bytes MSB-first and interleaved by channel; DSF stores them
// LSB-first in per-channel blocks. DoP has to know which one it is holding,
// because one needs a bit reversal and the other does not.
//
// Only the uncompressed `DSD ` form is accepted. `DST ` is a lossless
// compression with a different decoder, and pretending it is raw would produce
// noise that still looks like a valid stream.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "utf8_file.h"

namespace dff {

struct Format {
    uint32_t sampleRate = 0;  // DSD bit rate, e.g. 2822400
    uint32_t channels = 0;
    uint64_t dataBytes = 0;
    std::string compression;
};

struct File {
    std::FILE* f = nullptr;
    Format fmt;
    uint64_t dataOffset = 0;
    std::string error;

    ~File() {
        if (f) std::fclose(f);
    }

    bool open(const char* path) {
        f = neiro::openUtf8(path, "rb");
        if (!f) {
            error = std::string("cannot open ") + path;
            return false;
        }
        char id[5] = {0};
        uint64_t size = 0;
        if (!readTag(id) || !readU64(&size)) {
            error = "FRM8 header failed";
            return false;
        }
        if (std::strcmp(id, "FRM8") != 0) {
            error = std::string("not DFF: first chunk is '") + id + "'";
            return false;
        }
        char form[5] = {0};
        if (std::fread(form, 1, 4, f) != 4 || std::strcmp(form, "DSD ") != 0) {
            error = "FRM8 form is not DSD";
            return false;
        }

        bool haveRate = false;
        bool haveChannels = false;
        bool haveData = false;
        bool haveCompression = false;

        while (true) {
            if (!readTag(id)) break;
            if (!readU64(&size)) break;
            const long next = std::ftell(f) + static_cast<long>(size) + (size & 1);
            if (std::strcmp(id, "PROP") == 0) {
                char propForm[5] = {0};
                if (std::fread(propForm, 1, 4, f) != 4) break;
                if (std::strcmp(propForm, "SND ") == 0) {
                    const long propEnd = static_cast<long>(std::ftell(f)) +
                                         static_cast<long>(size) - 4;
                    while (std::ftell(f) < propEnd) {
                        char sub[5] = {0};
                        uint64_t subSize = 0;
                        if (!readTag(sub) || !readU64(&subSize)) break;
                        const long subNext = static_cast<long>(std::ftell(f)) +
                                             static_cast<long>(subSize) + (subSize & 1);
                        if (std::strcmp(sub, "FS  ") == 0 && subSize >= 4) {
                            uint32_t rate = 0;
                            if (readU32(&rate)) {
                                fmt.sampleRate = rate;
                                haveRate = true;
                            }
                        } else if (std::strcmp(sub, "CHNL") == 0 && subSize >= 2) {
                            uint16_t channels = 0;
                            if (readU16(&channels)) {
                                fmt.channels = channels;
                                haveChannels = channels > 0;
                            }
                        } else if (std::strcmp(sub, "CMPR") == 0 && subSize >= 4) {
                            char comp[5] = {0};
                            if (std::fread(comp, 1, 4, f) == 4) {
                                fmt.compression = comp;
                                haveCompression = true;
                            }
                        }
                        if (std::fseek(f, subNext, SEEK_SET) != 0) break;
                    }
                }
            } else if (std::strcmp(id, "DSD ") == 0) {
                dataOffset = static_cast<uint64_t>(std::ftell(f));
                fmt.dataBytes = size;
                haveData = true;
            }
            if (std::fseek(f, next, SEEK_SET) != 0) break;
        }

        if (!haveRate || !haveChannels || !haveData) {
            error = "DFF is missing FS, CHNL or DSD data";
            return false;
        }
        if (!haveCompression) {
            error = "DFF has no CMPR chunk";
            return false;
        }
        if (fmt.compression != "DSD ") {
            error = "DFF compression is '" + fmt.compression + "', only DSD is supported";
            return false;
        }
        return true;
    }

    /// Read `bytes` from the data chunk at `offset` within it.
    bool readData(uint64_t offset, uint8_t* out, size_t bytes) {
        if (offset + bytes > fmt.dataBytes) return false;
        if (std::fseek(f, static_cast<long>(dataOffset + offset), SEEK_SET) != 0) return false;
        return std::fread(out, 1, bytes, f) == bytes;
    }

    uint64_t totalDopFrames() const {
        if (!fmt.channels) return 0;
        return fmt.dataBytes / (static_cast<uint64_t>(fmt.channels) * 2);
    }

  private:
    bool readTag(char out[5]) {
        out[0] = out[1] = out[2] = out[3] = out[4] = 0;
        return std::fread(out, 1, 4, f) == 4;
    }
    bool readU16(uint16_t* v) {
        uint8_t b[2];
        if (std::fread(b, 1, 2, f) != 2) return false;
        *v = static_cast<uint16_t>((b[0] << 8) | b[1]);
        return true;
    }
    bool readU32(uint32_t* v) {
        uint8_t b[4];
        if (std::fread(b, 1, 4, f) != 4) return false;
        *v = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
             (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
        return true;
    }
    bool readU64(uint64_t* v) {
        uint8_t b[8];
        if (std::fread(b, 1, 8, f) != 8) return false;
        uint64_t r = 0;
        for (int i = 0; i < 8; ++i) r = (r << 8) | b[i];
        *v = r;
        return true;
    }
};

}  // namespace dff
