// 造一个 DSD 容器（.dsf）用于测试后置抽取在不同 DSD 等级下是否触发。
//
// # 为什么需要它
//
// 后置抽取原来写死 `sample_rate == 352800`，那是 **DSD64 44.1 kHz** 的解码率。DSD128 解到
// 705600，于是那一级**静默不生效**。而「静默不生效」是这个项目最不能接受的一种失效方式。
//
// 修法是从流的实际速率推出音频速率与阻带，所以这里必须**真的造出 DSD128** 来验证——
// 而「代码看起来对」不算验证。
//
// 用法: make_dsd128 <out.dsf> [seconds] [dsd64|dsd128]

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void put32(std::vector<uint8_t>* v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v->push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void put64(std::vector<uint8_t>* v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v->push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}
void tag(std::vector<uint8_t>* v, const char* t) {
    for (int i = 0; i < 4; ++i) v->push_back(static_cast<uint8_t>(t[i]));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: make_dsd128 <out.dsf>\n");
        return 1;
    }

    // DSD128 44.1 kHz：比特率 5644800，解码率 705600，音频速率仍是 44100。
    //
    // 所以**音频速率不变**，而解码率翻倍——这正是原来那个写死的判据会漏掉的情况。
    const bool dsd64 = argc > 3 && std::strcmp(argv[3], "dsd64") == 0;
    const uint32_t kDsdRate = dsd64 ? 2822400u : 5644800u;  // 比特率
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kBlockSize = 4096;    // 每声道每块字节
    const double seconds = argc > 2 ? std::atof(argv[2]) : 0.37;
    const uint32_t kBlocks = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::ceil(seconds * kDsdRate / (kBlockSize * 8.0))));
    // DSF's sampleCount counts bytes per channel on the files this engine's
    // reader was validated against; dsd_probe cross-checks it against the data
    // chunk size. Writing bits here made the probe require eight times the data.
    const uint64_t sampleCount = static_cast<uint64_t>(kBlockSize) * kBlocks;

    const uint64_t dataBytes = static_cast<uint64_t>(kBlockSize) * kChannels * kBlocks;
    const uint64_t totalSize = 28 + 12 + 52 + 12 + dataBytes;

    std::vector<uint8_t> f;
    tag(&f, "DSD ");
    put64(&f, 28);
    put64(&f, totalSize);
    put64(&f, 0);
    tag(&f, "fmt ");
    put64(&f, 52);
    put32(&f, 1);            // format version
    put32(&f, 0);            // format id: DSD raw
    put32(&f, 2);            // channel type: stereo
    put32(&f, kChannels);
    put32(&f, kDsdRate);
    put32(&f, 1);            // bits per sample
    put64(&f, sampleCount);
    put32(&f, kBlockSize);
    put32(&f, 0);
    tag(&f, "data");
    put64(&f, dataBytes);

    // 数据：1 kHz 正弦的一阶 sigma-delta。没有噪声整形，但足够判断 DoP
    // 的字节顺序：错误的顺序会把音调变成噪声，而不会仍然像音乐。
    double err[kChannels] = {0.0, 0.0};
    uint64_t bitIndex[kChannels] = {0, 0};
    for (uint32_t block = 0; block < kBlocks; ++block) {
        for (uint32_t c = 0; c < kChannels; ++c) {
            for (uint32_t i = 0; i < kBlockSize; ++i) {
                uint8_t byte = 0;
                for (int bit = 0; bit < 8; ++bit) {
                    const double t = static_cast<double>(bitIndex[c]) / kDsdRate;
                    const double x = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t);
                    err[c] += x;
                    if (err[c] >= 0.0) {
                        byte |= static_cast<uint8_t>(1u << bit);
                        err[c] -= 1.0;
                    } else {
                        err[c] += 1.0;
                    }
                    ++bitIndex[c];
                }
                f.push_back(byte);
            }
        }
    }

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::printf("写不了 %s\n", argv[1]);
        return 1;
    }
    std::fwrite(f.data(), 1, f.size(), out);
    std::fclose(out);

    std::printf("写了 %s\n", argv[1]);
    std::printf("  DSD%u，%u Hz 比特率 -> 期望解码率 %u Hz，音频速率 %u Hz\n",
                dsd64 ? 64 : 128, kDsdRate, kDsdRate / 8, kDsdRate / 64);
    std::printf("  %u 块 × %u 字节 × %u ch = %llu 字节\n", kBlocks, kBlockSize, kChannels,
                static_cast<unsigned long long>(dataBytes));
    std::printf("  1 kHz 正弦，%.2f s；用于 DoP 字节顺序的耳朵测试。\n", seconds);
    return 0;
}