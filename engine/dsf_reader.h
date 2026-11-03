// DSF 容器的最小读取器。只读头和原始字节，不解码。
//
// # 为什么单独成文件
//
// 这个解析器在第一次写成仪器时被改了 5 次才通过：漏了 `channelType` 字段、按声明长度
// 跳过了 `data` 标记、变量名三次搞错。在写第二个仪器时复制一份，等于把那 5 个错误
// 复制一份——而其中两个（漏字段、跳错标记）不会报错，只会让后面的值看起来合理。
//
// # 三条必须保留的规则，都是被这份文件本身教会的
//
// 1. **每个字段单独断言。** 漏掉 `channelType` 之后，后面每个字段都晚一格：读出来的
//    channelNum 是 2822400、bitsPerSample 是 1、sampleCount 高 4 字节是 615508992。
//    全部是整数、全部在 uint32 范围内、没有任何一个会触发越界检查。「解析成功」在这里
//    完全没有信息量。
//
// 2. **不按 `fmt` 段声明的 chunkSize 跳。** 这个文件声明 52 字节而只写了 44，
//    `data` 标记在 0x54，按声明值算出来是 0x5c。差 8 字节——按声明值跳会跳过标记
//    本身。偏差必须报出来而不是悄悄处理掉，因为它是文件的事实。
//
// 3. **交错的块要按 `channelType` 排布读，不能假设。** DSF 的 `channelType` 决定
//    声道在块内如何排列（1=mono 2=stereo 3=3ch 4=quad 5=4ch 6=5ch 7=5.1），立体声
//    是块内交错。下面的读取按 `channels` 均分，这是 DSF 对 channelType 2..7 的做法；
//    mono 是特例，因为只有 1 个声道时交错与顺序无法区分。
//
// 不支持 DFF（DSDIFF）——那个容器的头完全不同，而且本机没有 DFF 文件，所以按
// 「没有样本就没有基线」处理，不假装支持。

#ifndef NAE_DSF_READER_H
#define NAE_DSF_READER_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "utf8_file.h"

namespace dsf {

struct Format {
    uint32_t formatVersion = 0;
    uint32_t formatId = 0;      // 0 = DSD raw
    uint32_t channelType = 0;   // 1=mono 2=stereo 3=3ch 4=quad 5=4ch 6=5ch 7=5.1
    uint32_t channels = 0;
    uint32_t dsdRate = 0;       // 比特率（DSD64 = 2822400），不是音频采样率
    uint32_t bitsPerSample = 0; // 1
    uint64_t sampleCount = 0;   // 每声道的比特数
    uint32_t blockSize = 0;     // 每声道每块的字节数

    // 容器事实，不是解析器的意见。
    uint64_t fmtChunkSizeDeclared = 0;
    long fmtPadding = 0;      // `data` 标记实际位置 − 按声明长度算出的位置
    long fmtDataMarkAt = 0;
};

/// 打开并解析头。不读音频数据。
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
            error = std::string("打不开 ") + path;
            return false;
        }
        // 第一个 chunk：'DSD ' + 8 字节长度 + 8 字节总文件大小 + 8 字节元数据指针。
        char id[5] = {0};
        uint64_t size = 0, total = 0, meta = 0;
        if (fread(id, 1, 4, f) != 4 || !u64(&size) || !u64(&total) || !u64(&meta)) {
            error = "DSD 段头读取失败";
            return false;
        }
        if (std::strcmp(id, "DSD ") != 0) {
            error = std::string("不是 DSF：首段是 '") + id + "'";
            return false;
        }

        // 第二个 chunk：'fmt '。
        const long fmtStart = ftell(f);
        uint64_t fmtSize = 0;
        if (fread(id, 1, 4, f) != 4 || !u64(&fmtSize)) {
            error = "fmt 段头读取失败";
            return false;
        }
        if (std::strcmp(id, "fmt ") != 0) {
            error = std::string("第二段是 '") + id + "'，不是 'fmt '";
            return false;
        }
        if (!u32(&fmt.formatVersion) || !u32(&fmt.formatId) || !u32(&fmt.channelType) ||
            !u32(&fmt.channels) || !u32(&fmt.dsdRate) || !u32(&fmt.bitsPerSample) ||
            !u64(&fmt.sampleCount) || !u32(&fmt.blockSize)) {
            error = "fmt 段字段不足";
            return false;
        }
        fmt.fmtChunkSizeDeclared = fmtSize;

        // 找 `data` 标记，并记录它与声明长度的偏差。见文件头规则 2。
        const long afterFields = ftell(f);
        const long declaredEnd = fmtStart + 12 + static_cast<long>(fmtSize);
        long found = -1;
        for (long p = afterFields; p < afterFields + 64; ++p) {
            char probe[5] = {0};
            if (fseek(f, static_cast<long>(p), SEEK_SET) != 0) break;
            if (fread(probe, 1, 4, f) != 4) break;
            if (std::strcmp(probe, "data") == 0) {
                found = p;
                break;
            }
        }
        if (found < 0) {
            error = "fmt 字段之后 64 字节内没有 'data' 标记";
            return false;
        }
        fmt.fmtPadding = found - declaredEnd;
        fmt.fmtDataMarkAt = found;

        // `found` points at the four-byte `data` tag. The size follows it, so
        // reading from `found` puts four bytes of the tag into the offset and
        // starts the payload four bytes early. Measured on hardware as one
        // channel buzzing: the left channel began with the size field.
        if (fseek(f, found + 4, SEEK_SET) != 0 || !u64(&size)) {
            error = "data 段头读取失败";
            return false;
        }
        dataOffset = static_cast<uint64_t>(ftell(f));
        return true;
    }

    /// 读第 `block` 个块的第 `channel` 个声道。
    ///
    /// DSF 在块内交错排列：每声道 `blockSize` 字节，依次排布。立体声（channelType 2）
    /// 正是这样，所以这里不能整块读再假设前后各一半。
    bool readBlock(uint64_t block, uint32_t channel, std::vector<uint8_t>* out) {
        const uint64_t offset =
            dataOffset + (block * fmt.channels + channel) * fmt.blockSize;
        out->resize(fmt.blockSize);
        if (fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return false;
        return fread(out->data(), 1, fmt.blockSize, f) == fmt.blockSize;
    }

    /// 读第 `block` 个块的**全部**声道，保持 DSF 的块内交错排布。
    ///
    /// 长度是 `blockSize * channels`。交错就是容器实际的排布，而
    /// `dsd::Cascade::feedInterleaved` 收的正是交错数据——所以这一条路径上不需要任何
    /// 一方知道「平面」是什么。
    ///
    /// 第一版这里只提供按声道读的 `readBlock`，于是调用方得自己拆成交错顺序，而拆错的
    /// 时候没有任何检查会响：帧数、峰值、长度全都一致，只有逐样本比对才看得出两个声道
    /// 读到了不同的字节位置。
    bool readBlockRaw(uint64_t block, std::vector<uint8_t>* out) {
        const size_t need = static_cast<size_t>(fmt.blockSize) * fmt.channels;
        out->resize(need);
        const uint64_t offset = dataOffset + block * need;
        if (fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return false;
        return fread(out->data(), 1, need, f) == need;
    }

    uint64_t totalBlocks() const { return fmt.blockSize ? fmt.sampleCount / fmt.blockSize : 0; }

    uint32_t audioRate() const { return fmt.dsdRate / 64; }

   private:
    bool u32(uint32_t* v) { return fread(v, 4, 1, f) == 1; }
    bool u64(uint64_t* v) {
        uint8_t b[8];
        if (fread(b, 1, 8, f) != 8) return false;
        // DSF 的多字节整数全部小端，显式拼出来而不依赖主机字节序。
        uint64_t r = 0;
        for (int i = 7; i >= 0; --i) r = (r << 8) | b[i];
        *v = r;
        return true;
    }
};

/// DSF 字节 -> 时间上最早的比特在 LSB（DSF 规范）。DFF 相反，本读取器不支持。
inline uint8_t reverseBits(uint8_t v) {
    v = static_cast<uint8_t>(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = static_cast<uint8_t>(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    return static_cast<uint8_t>(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
}

/// 原始字节 -> ±1.0 的时间序列（LSB 优先）。
inline std::vector<double> toSignLsbFirst(const std::vector<uint8_t>& raw) {
    std::vector<double> out;
    out.reserve(raw.size() * 8);
    for (uint8_t byte : raw) {
        const uint8_t v = reverseBits(byte);
        for (int b = 0; b < 8; ++b) out.push_back((v >> b) & 1 ? 1.0 : -1.0);
    }
    return out;
}

}  // namespace dsf

#endif  // NAE_DSF_READER_H