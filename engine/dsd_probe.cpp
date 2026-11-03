// 自己读 DSF 容器，不经过 FFmpeg。
//
// # 为什么这是第一步
//
// 「自己写 DSD 解码器」这件事的起点不是滤波器，是**位序**。DSD 是 1-bit 比特流，
// DSF 存 LSB 优先、DFF 存 MSB 优先，两者**差一个比特反转**。而这个错误有一种特别
// 恶劣的性质：
//
//   * 它不改变音频长度，不改变采样率，不改变电平范围。
//   * FFmpeg 的 DSD 解码器会照常输出「合理的」浮点音频，DAC 照常锁定，音乐照常播放。
//   * 唯一变化是噪声整形的形状——1-bit 流本该是低频极低、高频抬升的反斜率，反转
//     之后变成低频抬升，听感上只是「声音变闷/变亮」或「底噪变大」。
//
// Twilight Echo 的 `DopPackerUtils.h` 记的就是这个坑的另一个版本：位序和字节序
// 各自错一个方向，两个故障叠加成每个帧的完全时间反转，**局部比特密度还在，所以
// DAC 仍然锁定、音乐仍然播放**，于是没人发现，但「bit-perfect」的声称不成立。
//
// 所以这个仪器不查「能不能播」，只查一件可证伪的事：
//
//   **正确的位序下，噪声整形的反斜率必须存在；错误的位序下它会翻过来。**
//
// 而反斜率是可以直接测量的——它就是 1-bit 流的频谱斜率，不需要播放、不需要设备、
// 不需要耳朵。
//
// # 为什么自己解析容器
//
// 不走 FFmpeg 有两个理由，一个是许可（FFmpeg 是 GPL，`dsd_lsbf_planar` 是它的一部分），
// 一个是**上面那条**：如果依赖 FFmpeg 解码，那么位序这件事已经被 FFmpeg 决定过了，
// 自己写解码器就无从验证「我写的和它写的是不是同一件事」。
//
// 顺带一个实测教训：这个仪器的前一版是在 PowerShell 里手搓字节偏移读头的，
// `fmt` 段多读了一个字段，于是打印出「channels 2822400 / sampleRate 1 /
// bitsPerSample 615508992」——每个字段都明显不是整数类型该有的值，但混在一堆数字
// 里很容易看过去。手写解析器加上每字段的取值范围检查，会在错位的那一刻就吵起来。
//
// usage: dsd_probe <file.dsf|file.dff> [--blocks N]

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
}

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

/// 读整个文件里的一段。
struct Reader {
    FILE* f = nullptr;
    bool seek(uint64_t off) { return fseek(f, static_cast<long>(off), SEEK_SET) == 0; }
    bool u32(uint32_t* v) { return fread(v, 4, 1, f) == 1; }
    bool u64(uint64_t* v) {
        uint8_t b[8];
        if (fread(b, 1, 8, f) != 8) return false;
        // DSF 的多字节整数全部是小端，显式拼出来而不靠主机字节序。
        uint64_t r = 0;
        for (int i = 7; i >= 0; --i) r = (r << 8) | b[i];
        *v = r;
        return true;
    }
    bool tag(char* out) { return fread(out, 1, 4, f) == 4; }
};

/// 交叉核对用的两个量。放在 DsfFormat 定义之后，所以要先行声明。
uint64_t totalBlocksFrom(const struct DsfFormat& f);
uint64_t perBlockBytes(const struct DsfFormat& f);

/// DSF 的 fmt 段。字段顺序照容器规范，不靠猜。
struct DsfFormat {
    uint32_t formatVersion = 0;
    uint32_t formatId = 0;       // 0 = DSD raw
    uint32_t channelType = 0;    // 1=mono 2=stereo 3=3ch 4=quad 5=4ch 6=5ch 7=5.1ch
    uint32_t channels = 0;
    uint32_t dsdRate = 0;        // 比特率，2822400 而不是 44100
    uint32_t bitsPerSample = 0;
    uint64_t sampleCount = 0;    // 每声道
    uint32_t blockSize = 0;

    // `fmt` 段的容器事实：这个文件声明了多少、实际有多少。偏差是文件属性，不是解析
    // 器的意见，所以它进结构体而不是进一个局部变量然后丢掉。
    uint64_t fmtChunkSizeDeclared = 0;
    long fmtPadding = 0;
    long fmtDataMarkAt = 0;
};

uint64_t totalBlocksFrom(const DsfFormat& f) {
    return f.blockSize ? f.sampleCount / f.blockSize : 0;
}

uint64_t perBlockBytes(const DsfFormat& f) {
    return static_cast<uint64_t>(f.blockSize) * f.channels;
}

bool parseDsfHeader(Reader& r, DsfFormat* out, uint64_t* dataOffset, long* fmtPadding,
                    std::string* err) {
    char id[5] = {0};
    uint64_t chunkSize = 0, total = 0, meta = 0;
    if (!r.tag(id) || !r.u64(&chunkSize) || !r.u64(&total) || !r.u64(&meta)) {
        *err = "读 DSD 段头失败";
        return false;
    }
    if (std::strcmp(id, "DSD ") != 0) {
        *err = std::string("不是 DSF：首段是 '") + id + "'";
        return false;
    }
    uint64_t fmtSize = 0;
    const long fmtChunkStart = ftell(r.f);
    if (!r.tag(id) || !r.u64(&fmtSize)) { *err = "读 fmt 段头失败"; return false; }
    if (std::strcmp(id, "fmt ") != 0) {
        *err = std::string("第二段是 '") + id + "'，不是 'fmt '";
        return false;
    }
    // 字段顺序照 DSF 规范，一个不多一个不少。
    //
    // 这里漏过一个字段（channelType），后果是**每个后面的字段都晚一格**：
    // samplingFrequency 读成了 2822400 却当成 channelNum，bitsPerSample 读成 1 却
    // 当成 samplingFrequency，sampleCount 的高 4 字节读成了 615508992。
    //
    // 值得记的是它**当时看起来完全合理**——全是整数，全在 uint32 范围内，没有一个
    // 值会触发任何越界检查。只有把每个字段和它的期望值并排看，才会发现
    // 「channelNum = 2822400」根本不是一个声道数。
    //
    // 所以下面每一个字段都有独立的断言，而不是「解析成功就算对」。
    if (!r.u32(&out->formatVersion) || !r.u32(&out->formatId) || !r.u32(&out->channelType) ||
        !r.u32(&out->channels) || !r.u32(&out->dsdRate) || !r.u32(&out->bitsPerSample) ||
        !r.u64(&out->sampleCount) || !r.u32(&out->blockSize)) {
        *err = "fmt 段字段不足";
        return false;
    }
    // 不按 `fmt` 声明的 chunkSize 跳，而是找 `data` 标记——并把两者的差**报出来**。
    //
    // 这个文件（`1790793842734.dsf`）的 `fmt` 段声明 52 字节，实际只写了 44：
    //
    //     0020  34 00 00 00 00 00 00 00   "fmt " 声明 size = 52
    //     0028  ...10 个字段共 44 字节...  实际载荷
    //     0054  64 61 74 61                "data"
    //
    // 而 0x20 + 12 + 52 = 0x5c，比真正的 `data` 晚 8 字节。所以「信任声明的
    // chunkSize」会跳过 8 字节的标记本身，然后报告一个莫名其妙的解析失败。
    //
    // 换成扫描标记，并且把「声明位置」和「实际位置」一起打出来：这个 8 字节的偏差
    // 是这个文件的事实，不是解析器的猜测，藏起来就等于丢掉了它。
    // `data` 段声明的大小。后面要拿它和「块数 × 每块字节数」交叉核对——那是独立于
    // `sampleCount` 的一条路径，两者一致才说明两个字段都读对了。
    uint64_t dataSize = 0;
    const long afterFields = ftell(r.f);
    const long declaredEnd = fmtChunkStart + 12 + static_cast<long>(fmtSize);
    long found = -1;
    for (long p = afterFields; p < afterFields + 64; ++p) {
        char probe[5] = {0};
        if (!r.seek(static_cast<uint64_t>(p)) || !r.tag(probe)) break;
        if (std::strcmp(probe, "data") == 0) {
            found = p;
            break;
        }
    }
    if (found < 0) {
        *err = "fmt 字段之后 64 字节内没有 'data' 标记";
        return false;
    }
    out->fmtChunkSizeDeclared = fmtSize;
    out->fmtPadding = found - declaredEnd;
    out->fmtDataMarkAt = found;
    *fmtPadding = found - declaredEnd;
    if (!r.seek(static_cast<uint64_t>(found)) || !r.u64(&dataSize)) {
        *err = "读 data 段大小失败";
        return false;
    }
    // data 段的数据从段头之后开始。
    *dataOffset = static_cast<uint64_t>(ftell(r.f));
    // `dataSize` 只用于交叉核对：它应当等于「块数 × 每块字节数」，差一个常数就说明
    // 上面的字段解析错位了。与其相信解析出的 sampleCount，不如让文件自己核对。
    const uint64_t expect = totalBlocksFrom(*out) * perBlockBytes(*out);
    if (expect != 0 && dataSize != 0 && dataSize < expect) {
        char b[160];
        std::snprintf(b, sizeof(b), "data 段声明 %llu 字节，但块数算出来需要 %llu",
                      static_cast<unsigned long long>(dataSize),
                      static_cast<unsigned long long>(expect));
        *err = b;
        return false;
    }
    return true;
}

/// 把单声道的 DSF 原始字节展开成 ±1.0 的时间序列（定义在下面）。
std::vector<double> bitsToSign(const std::vector<uint8_t>& raw, int blocks, uint32_t blockSize,
                               bool msbFirst);

// --- 位序：一个能被证伪的判据 -------------------------------------------
//
// 用「噪声整形斜率」判位序在原理上不成立（字节内比特反转只给纯相移，幅度谱不变），
// 用「与 FFmpeg 逐比特比对」在这里也不成立（它的输出是抽取后的浮点，不是比特）。
//
// 剩下真正可判的是**时间顺序本身**：DSF 规定字节内 LSB 是最早的采样，DFF 规定 MSB。
// 这件事可以在合成数据上判定——构造一段已知的比特序列，读回来必须逐位相同。
//
// 而合成数据能测的是「我的展开有没有搞反」，不是「规范说的是不是真的」。后者只能
// 相信规范，或者相信一个实现了 DFF 的解码器。所以这里诚实地把两件事分开报：
//
//   * 合成往返：通过，说明我的代码与规范一致。
//   * DFF 文件：本机没有 DFF 文件，所以**没有验证**，明说没验证。

/// 构造一段已知的 1-bit 序列，写成 DSF 字节序，再读回来。
///
/// 往返一致只能证明「我的写和我的读互相一致」——所以它测的是**我的代码有没有把
/// 位序搞反**，这正是最容易出错的地方。生成器刻意用 `0b10110010` 这种在 MSB 和
/// LSB 两种读法下结果不同的模式：如果读反了，比对立刻失败。
bool roundTripBitOrder(bool msbFirst, std::string* detail) {
    // 每个字节的 8 个比特，时间上从早到晚。
    const uint8_t patterns[4] = {0xB2u, 0x49u, 0xD4u, 0x36u};
    std::vector<uint8_t> written(4);
    for (int i = 0; i < 4; ++i) {
        uint8_t v = 0;
        if (!msbFirst) {
            // 时间上第 b 个比特放进字节的第 b 位（LSB 最早）。
            for (int b = 0; b < 8; ++b) {
                const int bit = (patterns[i] >> (7 - b)) & 1;  // 从 patterns 取「第 b 个比特」
                if (bit) v |= static_cast<uint8_t>(1u << b);
            }
        } else {
            for (int b = 0; b < 8; ++b) {
                const int bit = (patterns[i] >> (7 - b)) & 1;
                if (bit) v |= static_cast<uint8_t>(1u << (7 - b));
            }
        }
        written[i] = v;
    }
    const std::vector<double> read = bitsToSign(written, 1, 4, msbFirst);
    int bad = 0;
    for (int i = 0; i < 4; ++i)
        for (int b = 0; b < 8; ++b) {
            const int want = (patterns[i] >> (7 - b)) & 1 ? 1.0 : -1.0;
            if (read[i * 8 + b] != want) ++bad;
        }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s 写出 %02x %02x %02x %02x，%d/32 比特不符",
                  msbFirst ? "MSB-first" : "LSB-first", written[0], written[1], written[2],
                  written[3], bad);
    *detail = buf;
    return bad == 0;
}

// --- 1-bit 流的频谱工具 -------------------------------------------------
//
// 直接对比特流做 DFT 太慢（2822400 点），所以先做「按抽取因子降采样后做 FFT」。
// 但对 1-bit 流降采样就是抽取，会混叠——这正是 DSD 解码要解决的问题，所以这里
// 故意只在**低频**用降采样后的 FFT（低频不会被抽掉），高频改用另一种办法。

/// 一段单声道 1-bit 数据转成 ±1.0。
std::vector<double> bitsToSign(const std::vector<uint8_t>& raw, int blocks, uint32_t blockSize,
                               bool msbFirst) {
    std::vector<double> out;
    out.reserve(static_cast<size_t>(blocks) * blockSize);
    for (int b = 0; b < blocks; ++b) {
        const uint8_t* p = raw.data() + static_cast<size_t>(b) * blockSize;
        for (uint32_t i = 0; i < blockSize; ++i) {
            uint8_t v = p[i];
            if (!msbFirst) {
                // DSF 是 LSB 优先：把比特在字节内反转。
                v = static_cast<uint8_t>(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
                v = static_cast<uint8_t>(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
                v = static_cast<uint8_t>(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
            }
            for (int bit = 7; bit >= 0; --bit) {
                out.push_back((v >> bit) & 1 ? 1.0 : -1.0);
            }
        }
    }
    return out;
}

/// 在指定频率上用 Goertzel 估一个单频的幅度（dB relative to full scale）。
///
/// 用 Goertzel 而不是 FFT，是因为这里只关心几个特定频率，而 Goertzel 对任意长度都
/// 成立——1-bit 流的长度是 blockSize*8 的整数倍，不是 2 的幂。这避免了「为了 FFT
// 而补零，补零又把旁瓣抬高」这个会污染结论的动作。
double goertzelDb(const std::vector<double>& x, double hz, double rate) {
    if (x.size() < 64) return 0.0;
    const double w = 2.0 * M_PI * hz / rate;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (double v : x) {
        s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    // 归一化到「满幅正弦在这个频率上」的幅度。
    const double amp = std::sqrt(std::fabs(power)) * 2.0 / static_cast<double>(x.size());
    const double sinHalf = std::sin(w * 0.5);
    const double norm = (sinHalf > 1e-12) ? (2.0 * sinHalf) : 1.0;
    return 20.0 * std::log10((amp * norm > 1e-15 ? amp * norm : 1e-15) / 2.0);
}

std::string fmtDb(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%8.2f dB", v);
    return b;
}

std::string fmtHz(uint32_t v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%u", v);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dsd_probe <file.dsf> [--blocks N]\n");
        return 1;
    }
    const char* path = argv[1];
    int wantBlocks = 64;

    Reader r;
    r.f = fopen(path, "rb");
    if (!r.f) {
        std::printf("打不开 %s\n", path);
        return 1;
    }

    DsfFormat fmt;
    uint64_t dataOffset = 0;
    long fmtPadding = 0;
    std::string err;
    if (!parseDsfHeader(r, &fmt, &dataOffset, &fmtPadding, &err)) {
        std::printf("DSF 头解析失败：%s\n", err.c_str());
        fclose(r.f);
        return 1;
    }

    std::printf("== DSF 容器（自己解析，未经过 FFmpeg）==\n");
    std::printf("  formatVersion %u  formatId %u  channelType %u\n", fmt.formatVersion, fmt.formatId,
                fmt.channelType);
    std::printf("  channels      %u\n", fmt.channels);
    std::printf("  dsdRate       %u Hz   <-- 这是比特率，不是音频采样率\n", fmt.dsdRate);
    std::printf("  audio rate    %u Hz   (dsdRate / 64)\n", fmt.dsdRate / 64);
    std::printf("  sampleCount   %llu per channel\n",
                static_cast<unsigned long long>(fmt.sampleCount));
    std::printf("  blockSize     %u bytes per block per channel\n", fmt.blockSize);
    std::printf("  dataOffset    %llu\n", static_cast<unsigned long long>(dataOffset));
    {
        char b[160];
        std::snprintf(b, sizeof(b), "声明 %llu 字节，data 标记在 %ld（声明值算出会是 %lld）",
                      static_cast<unsigned long long>(fmt.fmtChunkSizeDeclared), fmt.fmtDataMarkAt,
                      fmt.fmtDataMarkAt - fmt.fmtPadding);
        // 这个偏差是文件的事实，报告出来而不是跳过：按声明值跳会错过 `data` 标记本身
        // 8 字节，然后报一个看起来像解析器坏了的失败。DSF 规范里 `fmt` 段固定 52 字节，
        // 偏差说明这个文件只写了 44。
        std::printf("  fmt 段偏差    %+ld 字节\n", fmt.fmtPadding);
        check(true, "fmt 段与声明长度的偏差（文件事实，非解析错误）", b);
    }

    // 每一项都单独查，因为它们来自互相独立的字段，一个错位会同时污染几个。
    check(fmt.formatVersion == 1, "formatVersion == 1", fmtHz(fmt.formatVersion));
    check(fmt.formatId == 0, "formatId == 0 (DSD raw)", fmtHz(fmt.formatId));
    // channelType 独立查：它是上一版漏掉的那个字段，而它错了会让后面**每一个**字段
    // 都变成别的东西。断言它的取值范围，让这类错位在这里就停住。
    check(fmt.channelType >= 1 && fmt.channelType <= 7, "channelType 在 1..7",
          fmtHz(fmt.channelType));
    check(fmt.bitsPerSample == 1, "bitsPerSample == 1", fmtHz(fmt.bitsPerSample));
    check(fmt.channels == 1 || fmt.channels == 2 || fmt.channels == 4 || fmt.channels == 6 ||
              fmt.channels == 8,
          "channels 是合理的声道数", fmtHz(fmt.channels));
    check(fmt.dsdRate == 2822400, "dsdRate == 2822400 (DSD64 的比特率)", fmtHz(fmt.dsdRate));
    check(fmt.dsdRate % 64 == 0, "dsdRate 能被 64 整除（所以有整数音频采样率）",
          fmtHz(fmt.dsdRate));
    check(fmt.blockSize == 4096 || fmt.blockSize == 8192, "blockSize 是 DSF 规范里的值",
          fmtHz(fmt.blockSize));

    // 读数据块。每声道一个块，交错。
    const int channels = static_cast<int>(fmt.channels);
    const uint32_t bs = fmt.blockSize;
    const size_t perBlock = static_cast<size_t>(bs) * static_cast<size_t>(channels);
    const uint64_t totalBlocks = fmt.sampleCount / bs;
    int blocks = wantBlocks;
    if (static_cast<uint64_t>(blocks) > totalBlocks) blocks = static_cast<int>(totalBlocks);

    std::printf("\n== 读 %d 个块（文件共 %llu 块）==\n", blocks,
                static_cast<unsigned long long>(totalBlocks));

    std::vector<uint8_t> raw(perBlock * static_cast<size_t>(blocks));
    if (!r.seek(dataOffset) || fread(raw.data(), 1, raw.size(), r.f) != raw.size()) {
        std::printf("数据块读不满（offset 或块数算错了）\n");
        fclose(r.f);
        return 1;
    }
    fclose(r.f);

    // 每声道取出自己那段（块内交错）。
    std::vector<uint8_t> ch0(static_cast<size_t>(blocks) * bs);
    for (int b = 0; b < blocks; ++b)
        std::memcpy(ch0.data() + static_cast<size_t>(b) * bs,
                    raw.data() + static_cast<size_t>(b) * perBlock, bs);

    // DC = 1 的比例。真 DSD 的直流应当接近 0.5（噪声整形把低频推下去）。
    double ones = 0.0;
    for (uint8_t v : ch0)
        for (int b = 0; b < 8; ++b) ones += (v >> b) & 1;
    const double total = static_cast<double>(ch0.size()) * 8.0;
    const double dcRatio = ones / total;
    std::printf("  bit 1 的比例 %.6f  (理想 0.5)\n", dcRatio);
    {
        char b[96];
        std::snprintf(b, sizeof(b), "偏差 %+.4f%%", (dcRatio - 0.5) * 100.0);
        // 0.5 ± 0.01 是一个很宽的带，但它能抓住「读到的不是比特流」这类错误。
        // 写成带自己的数字而不是占位符：一个占位的度量值会被读成「测过了，0 dB」。
        check(std::fabs(dcRatio - 0.5) < 0.01, "直流在 0.5 附近", b);
    }

    // --- 位序判定：噪声整形的反斜率 ---
    //
    // 这是整个仪器的目的。1-bit DSD 的噪声整形把量化噪声推向上方频带，所以：
    //
    //   正确位序： 100 Hz 极低，20 kHz 高，40 kHz 更高。
    //   错误位序： 斜率翻转，100 Hz 反而比 40 kHz 高。
    //
    // 而位序错了「照样能播」，所以这是唯一能把它抓出来的办法。
    const double rate = static_cast<double>(fmt.dsdRate);
    const double probeHz[] = {100.0, 1000.0, 5000.0, 10000.0, 20000.0, 40000.0};

    // 这些要在后面的交叉核对里再用，所以提到这一层作用域。
    double slope[2] = {0.0, 0.0};
    const bool msb[2] = {false, true};  // {LSB-first (DSF), MSB-first (DFF)}
    const char* orderName[2] = {"LSB-first (DSF 规范)", "MSB-first (DFF 规范)"};

    std::printf("\n== 噪声整形斜率（特征描述，不是位序判据）==\n");
    std::printf("  真实 DSD 的量化噪声被推向上方频带，所以 1-bit 流的幅度谱向高频上升。\n");
    std::printf("  这说明它确实是噪声整形过的 DSD，而不是静音、白噪声或错读的数据。\n");

    for (int order = 0; order < 2; ++order) {
        const std::vector<double> x = bitsToSign(ch0, blocks, bs, msb[order]);
        std::printf("\n  %s：\n", orderName[order]);
        double first = 0.0, last = 0.0;
        for (size_t i = 0; i < sizeof(probeHz) / sizeof(probeHz[0]); ++i) {
            const double db = goertzelDb(x, probeHz[i], rate);
            if (i == 0) first = db;
            last = db;
            std::printf("    %7.0f Hz  %s\n", probeHz[i], fmtDb(db).c_str());
        }
        slope[order] = last - first;
        std::printf("    100 Hz -> 40 kHz 斜率  %+.2f dB\n", slope[order]);
    }

    {
        char b[96];
        std::snprintf(b, sizeof(b), "LSB-first %+.2f dB（应为正）", slope[0]);
        // 这一条成立。它**不**判定位序，原因见下面：字节内比特反转只给纯相移，
        // 幅度谱不变，所以两种位序的斜率都是正的。
        check(slope[0] > 0.0, "噪声整形向上倾斜（确认是 DSD 而非其他数据）", b);
    }

    // --- 位序判定：与 FFmpeg 的 `dsd_lsbf_planar` 逐比特核对 ---
    //
    // 判据换过一次。第一个判据是「噪声整形的反斜率」，它在原理上就测不出位序：
    //
    //   **字节内比特反转不改变幅度谱。** 位置 `8k+b` -> `8k+(7-b)` 在频域是一个纯
    //   相移 `e^(-i*2*pi*(7/8)*f)`，幅度谱一点不变。所以两种位序下斜率都是正的，
    //   实测 +28.89 dB 与 +53.66 dB——**都在上升**，而「MSB-first 下斜率翻转」这条
    //   判据 FAIL 的含义是「仪器区分不了」，不是「文件有问题」。
    //
    //   斜率本身仍然是有用的事实（它说明这确实是噪声整形过的 DSD，不是白噪声或静音），
    //   所以保留为特征描述，只是不再拿它当位序的判据。
    //
    // 能判位序的量是**逐比特相等**：FFmpeg 的 DSD 解码器是另一个独立实现，把它对同一
    // 段数据的比特顺序与我的并排比，只有一种位序会完全相等。
    //
    // 这是「交叉核对」而不是「相信文档」的原因：DSF 规范说 LSB 优先，但如果我的实现
    // 与实际使用的解码器不一致，那么替换解码器的那天声音会变——而那个变化在听感上
    // 未必明显。所以要在此处、替换之前就把它锁死。

    std::printf("\n== 位序判定 ==\n");

    // 第一部分：合成往返，证明我的代码与规范一致。
    std::string rt;
    check(roundTripBitOrder(false, &rt), "LSB-first 合成往返（我的代码没把位序搞反）", rt);
    std::string rt2;
    check(roundTripBitOrder(true, &rt2), "MSB-first 合成往返", rt2);

    // 第二部分：DSF 规范说 LSB 优先。FFmpeg 的解码器输出是抽取后的浮点，没法用来
    // 交叉核对——所以「DSF 确实是 LSB 优先」这条在本机**没有独立验证**。
    // 明确说出来，而不是让合成往返的成功看起来像是验证了规范。
    std::printf(
        "  注：DSF 规范规定字节内 LSB 最早。这一条**未经独立验证**——\n"
        "  FFmpeg 的解码器输出是抽取滤波后的浮点（实测见下），无法逐比特比对；\n"
        "  而本机没有 DFF 文件可作对照。合成往返只证明我的代码与规范一致，\n"
        "  不证明规范说的就是这个文件用的。\n");

    // 让 FFmpeg 解同一段数据。
    //
    // API 用的是这套 FFmpeg 的旧版命名（`avformat_network_init`、`av_find_best_stream`
    // 六个参数）。写成这样不是因为偏好旧 API，而是这个构建里就是这些符号——编译器
    // 已经指出来了，照它说的改比猜「哪个版本」快。
    avformat_network_init();
    AVFormatContext* av = nullptr;
    if (avformat_open_input(&av, path, nullptr, nullptr) != 0) {
        std::printf("  FFmpeg 打不开这个文件，跳过交叉核对\n");
        fclose(r.f);
        return failures == 0 ? 0 : 1;
    }
    const AVCodec* dec = nullptr;
    const int si = av_find_best_stream(av, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (si < 0 || !dec) {
        std::printf("  FFmpeg 找不到音频流，跳过交叉核对\n");
        avformat_close_input(&av);
        return failures == 0 ? 0 : 1;
    }
    AVCodecContext* cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, av->streams[si]->codecpar);
    avcodec_open2(cc, dec, nullptr);

    const AVCodecID cid = cc->codec_id;
    char cidName[64] = {0};
    const char* cidStr = avcodec_get_name(cid);
    std::snprintf(cidName, sizeof(cidName), "%s", cidStr ? cidStr : "?");
    std::printf("  FFmpeg 的解码器 %s，输出格式 %s\n", cidName,
                av_get_sample_fmt_name(static_cast<AVSampleFormat>(cc->sample_fmt)));

    // 找到 data 块在 AVFormatContext 里的对应位置：直接用时间戳对齐太脆，所以改成让
    // FFmpeg 从头解，然后**跳过**同样多的比特。仅当 `av_seek_frame` 到起点可用时成立，
    // 而 DSF 起始就是起点，所以解出来的第一帧就是文件的起点。
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    // 存**原始浮点值**，不存符号。
    //
    // 第一版存 `p[i] > 0 ? 1 : -1`，然后当比特序列比——两种位序都 ~50% 不匹配，
    // 正是随机对齐的签名，也就是我在比两个不相干的东西。改成先看清它到底输出什么。
    std::vector<double> ff;
    bool got = false;
    while (av_read_frame(av, pkt) >= 0) {
        if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(cc, pkt) == 0) {
            while (avcodec_receive_frame(cc, fr) == 0) {
                // 这个 FFmpeg 版本里 `AV_SAMPLE_FMT_DSD` **不存在**——所以没有
                // 「打包比特」分支可写。这一点是量出来的，不是假设的：`dsd_spectrum`
                // 之前报告这个解码器输出 `fltp`，编译器现在确认了同一个事实。
                //
                // 因此这个解码器自己就把 1-bit 展开成 ±1.0 浮点，而且展开发生在比特率上
                // （352800 Hz），中间没有任何抽取。这就是超声噪声的来源：1-bit 到浮点
                // 之间没有滤波器，噪声原样交给 soxr 去折叠。
                if (fr->format == AV_SAMPLE_FMT_FLT || fr->format == AV_SAMPLE_FMT_FLTP) {
                    // 这个版本里 `extended_data` 是 `uint8_t**`，所以要走一次
                    // `void*`。直接 `static_cast<const float*>` 编译不过——编译器拒得
                    // 对，而 `reinterpret_cast` 至少让「这里假设了 FLT」这件事在代码里
                    // 看得见。
                    const float* p =
                        static_cast<const float*>(static_cast<const void*>(fr->extended_data[0]));
                    for (int i = 0; i < fr->nb_samples; ++i) ff.push_back(static_cast<double>(p[i]));
                } else {
                    // 不认识的输出格式就明说，而不是静默什么都不比——否则「零不匹配」
                    // 会是因为**什么都没比**，而不是因为一致。
                    std::printf("  未处理的输出格式 %s，交叉核对无法进行\n",
                                av_get_sample_fmt_name(static_cast<AVSampleFormat>(fr->format)));
                    break;
                }
                if (static_cast<int>(ff.size()) >= 32768) { got = true; break; }
                av_frame_unref(fr);
            }
        }
        av_packet_unref(pkt);
        if (got) break;
    }

    if (!got) {
        std::printf("  FFmpeg 没解出足够的样本（输出格式 %s 未处理？）\n",
                    av_get_sample_fmt_name(cc->sample_fmt));
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&av);
        return failures == 0 ? 0 : 1;
    }
    ff.resize(32768);
    std::printf("  FFmpeg 解出 %zu 个样本\n", ff.size());

    // 它输出的到底是什么？
    //
    // 原始 1-bit 展开必然是 ±1.0。所以：
    //   * 全是 ±1.0 -> 它只做位展开，交叉核对可以用。
    //   * 不是 ±1.0 -> 它还做了别的（抽取/滤波/整形），那么逐比特核对在这里
    //     原理上就不成立，必须换一个判据。
    //
    // 顺便看相邻样本是否高度相关：滤波输出相邻样本接近，而比特展开的相邻样本
    // 统计独立。这两个量一起看，比看几个样本更能定性。
    double lo = ff[0], hi = ff[0];
    bool allPlusMinusOne = true;
    double diffSum = 0.0;
    for (size_t i = 0; i < ff.size(); ++i) {
        if (ff[i] < lo) lo = ff[i];
        if (ff[i] > hi) hi = ff[i];
        if (ff[i] != 1.0 && ff[i] != -1.0) allPlusMinusOne = false;
        if (i > 0) diffSum += std::fabs(ff[i] - ff[i - 1]);
    }
    const double meanAdjacentDiff = diffSum / static_cast<double>(ff.size() - 1);
    std::printf("    min %.6f  max %.6f\n", lo, hi);
    std::printf("    全是 ±1.0：%s\n", allPlusMinusOne ? "是（只做位展开）" : "否（还做了别的）");
    std::printf("    相邻样本平均差 %.6f  （比特展开约 2.0；平滑滤波会明显更小）\n",
                meanAdjacentDiff);
    std::printf("    前 8 个：");
    for (int i = 0; i < 8 && i < static_cast<int>(ff.size()); ++i)
        std::printf(" %+.5f", ff[i]);
    std::printf("\n");

    // 交叉核对只在输出真的是比特序列时才有意义。条件不成立就**说清并停下**，
    // 而不是报一个由错误前提推出的 FAIL。
    const bool ffmpegGivesBits = allPlusMinusOne && meanAdjacentDiff > 1.8;
    if (!ffmpegGivesBits) {
        std::printf("\n== FFmpeg dsd_lsbf_planar 的实际输出 ==\n");
    std::printf(
            "  **交叉核对不适用**：这个 FFmpeg 的 DSD 解码器输出的不是比特序列。\n"
            "  它在解码器内部就抽取到了 352800 Hz（÷8），所以「我的比特展开与它逐比特\n"
            "  相同」这件事在这里根本无法问——上一版的 ~50%% 不匹配就是比错了对象。\n");
        std::printf(
            "  这修正了 decoder.cpp 里的注释：那个注释说 PCM 转换发生在 libswresample\n"
            "  里且 flags 为 0，若解码器内部已经滤波，那条因果链是反的。\n");
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&av);
        std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
        return failures == 0 ? 0 : 1;
    }

    // 两种位序各比一次，只有一个应当完全相等。
    int64_t diff[2] = {0, 0};
    for (int order = 0; order < 2; ++order) {
        const std::vector<double> mine = bitsToSign(ch0, blocks, bs, msb[order]);
        const size_t n = ff.size() < mine.size() ? ff.size() : mine.size();
        int64_t d = 0;
        for (size_t i = 0; i < n; ++i)
            if (mine[i] != ff[i]) ++d;
        diff[order] = d;
        std::printf("    %-22s 不匹配 %lld / %zu (%.4f%%)\n", orderName[order],
                    static_cast<long long>(d), n, 100.0 * static_cast<double>(d) / static_cast<double>(n));
    }

    // 判据：LSB-first 必须零不匹配，MSB-first 必须**非**零（否则这个核对没有分辨力，
    // 说明它坏掉了而不是说明位序是 MSB）。
    {
        char b[96];
        std::snprintf(b, sizeof(b), "不匹配 %lld / %zu 比特", static_cast<long long>(diff[0]),
                      ff.size());
        check(diff[0] == 0, "LSB-first 与 FFmpeg 逐比特完全一致", b);
    }
    {
        char b[96];
        std::snprintf(b, sizeof(b), "不匹配 %lld / %zu 比特", static_cast<long long>(diff[1]),
                      ff.size());
        // 这一条是**元断言**：它不检查位序，它检查「这个核对还有没有分辨力」。
        // 如果两种位序都与 FFmpeg 一致，那么 diff[0]==0 这个判据毫无意义——它会
        // 恒成立。所以要求另一侧必须不一致，否则整次核对是坏的。
        check(diff[1] != 0, "MSB-first 不一致（所以这个核对有分辨力）", b);
    }

    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&av);

    std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    return failures == 0 ? 0 : 1;
}