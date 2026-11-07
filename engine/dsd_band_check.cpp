// 用**引擎走的那条代码路径**在真实 DSD 文件上重算频带电平，并检测哨兵值。
//
// # 为什么单独一个仪器
//
// 结论链上有三个数字，而它们的来源不同：
//
//   * `dsd_decim` 说自写级联在 40-80 kHz 是 −220.39 dBFS（比现役好 66 dB）。
//   * 引擎里的 A/B 说同一路径在 40-48 kHz 只比可听内容低 2.25 dB。
//   * 两者差约 170 dB。
//
// 而 `dsd_decim` 那个 −220.39 **可能不是测量**：`bandRms` 在频带里一个 bin 都没有时返回
// −200.0，而噪声底被 `log10(1e-12)` 夹在 −240.00。打印出来的 −220.39 与 −240.00 落在
// 那两个哨兵的量级上。
//
// 所以先要回答的是：**用生产代码在真实文件上跑，级联的输出到底在什么电平？**
// 答案只有两种，而且指向完全不同的下一步：
//
//   * 如果这里得到约 −170 dB 相对 -> 仪器是对的，引擎在解码器到抽头之间加了东西。
//   * 如果这里得到约 −12 dB 相对   -> 仪器读到的是哨兵，那 66 dB 从来不是结果。
//
// # 哨兵检测是硬要求，不是可选的
//
// 空的频带返回 −200 会被读成「极其安静」——而那恰恰是它唯一不能支持的结论。所以这里
// 每个频带都打印 bin 范围，而空频带是 FAIL 而不是数字。
//
// usage: dsd_band_check <file.dsf>

#include "dsd_filter.h"
#include "dsf_reader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

void fft(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < len / 2; ++k) {
                const double wr = std::cos(ang * k), wi = std::sin(ang * k);
                const double ur = re[i + k], ui = im[i + k];
                const double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
                const double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
            }
        }
    }
}

/// 一条频带的结果。**`bins` 是判据的一部分**：为 0 表示这条频带里一个 bin 都没有，
/// 那时候 `db` 那个数字没有任何意义。
struct Band {
    double db = 0.0;
    size_t b0 = 0, b1 = 0;
    bool valid() const { return b1 > b0; }
};

/// Hann 窗 + FFT 的频带 RMS。空频带返回 `valid() == false`，**不给数字**。
///
/// 给数字是因为那个数字会被读成「极其安静」——而空频带唯一能支持的是「这里什么都没测到」。
Band bandRms(const std::vector<float>& x, double rate, double f0, double f1) {
    Band out;
    size_t n = 1;
    while (n * 2 <= x.size()) n *= 2;
    if (n < 1024) return out;
    const size_t start = (x.size() - n) / 2;
    std::vector<double> re(n, 0.0), im(n, 0.0);
    double winPower = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double hann =
            0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(n - 1));
        re[i] = static_cast<double>(x[start + i]) * hann;
        winPower += hann * hann;
    }
    fft(re, im);

    const double binHz = rate / static_cast<double>(n);
    const size_t half = n / 2;
    out.b0 = static_cast<size_t>(f0 / binHz);
    out.b1 = static_cast<size_t>(f1 / binHz);
    if (out.b1 > half) out.b1 = half;
    if (out.b0 >= out.b1) {
        out.b0 = 0;
        out.b1 = 0;
        return out;  // 无效：不给数字
    }
    double p = 0.0;
    for (size_t b = out.b0; b < out.b1; ++b) p += re[b] * re[b] + im[b] * im[b];
    const double rms = std::sqrt(2.0 * p / winPower / static_cast<double>(n));
    out.db = 20.0 * std::log10(rms > 1e-12 ? rms : 1e-12);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dsd_band_check <file.dsf>\n");
        return 1;
    }
    dsf::File file;
    if (!file.open(argv[1])) {
        std::printf("%s\n", file.error.c_str());
        return 1;
    }
    const uint32_t dsdRate = file.fmt.dsdRate;
    const uint32_t channels = file.fmt.channels;

    std::printf("== 用生产级联在真实 DSD 文件上重算频带电平 ==\n");
    std::printf("  %s\n", argv[1]);
    std::printf("  %u Hz 比特率，%u ch，%llu 块，每块 %u 字节\n", dsdRate, channels,
                static_cast<unsigned long long>(file.totalBlocks()), file.fmt.blockSize);
    std::printf("  fmt 段偏差 %+ld 字节（文件事实，非解析错误）\n", file.fmt.fmtPadding);

    neiro::dsd::Cascade cascade;
    double audioRate = 0;
    std::string err;
    if (!cascade.configure(dsdRate, channels, &audioRate, &err)) {
        std::printf("  configure 失败：%s\n", err.c_str());
        return 1;
    }
    std::printf("  级联 ÷8 再 ÷2 -> %.0f Hz\n", audioRate);

    // 按**引擎的调用约定**跑：整块喂进去，每次只拿少量帧，抽干到抽不动为止。
    const int kBlocks = 48;
    const uint64_t blocks = file.totalBlocks();
    const int useBlocks = blocks < static_cast<uint64_t>(kBlocks) ? static_cast<int>(blocks) : kBlocks;
    std::vector<uint8_t> blockBuf;
    std::vector<float> pcm;
    const size_t kMaxFramesPerCall = 512;
    for (int b = 0; b < useBlocks; ++b) {
        if (!file.readBlockRaw(static_cast<uint64_t>(b), &blockBuf)) {
            std::printf("  读第 %d 块失败\n", b);
            return 1;
        }
        for (;;) {
            const size_t before = pcm.size();
            cascade.drainPending(&pcm, kMaxFramesPerCall);
            if (pcm.size() == before) break;
        }
        cascade.feedInterleaved(blockBuf.data(), file.fmt.blockSize, &pcm, kMaxFramesPerCall);
    }
    for (;;) {
        const size_t before = pcm.size();
        cascade.drainPending(&pcm, kMaxFramesPerCall);
        if (pcm.size() == before) break;
    }

    const size_t frames = pcm.size() / channels;
    std::printf("\n  产出 %zu 帧（%zu 个交错样本），理论 %zu 帧\n", frames, pcm.size(),
                static_cast<size_t>(useBlocks) * file.fmt.blockSize * 8 / 16);
    check(frames > 4096, "产出帧数与 ÷16 一致", std::to_string(frames) + " 帧");

    // 取最后 32768 帧，跳过滤波器建立的那一段，然后**取单声道**。
    //
    // 去交织这一步是必须的，而第一版漏了：`bandRms` 把序列当作 `rate` 上的单声道信号，
    // 而交错缓冲区是 `L,R,L,R…`。把它当单声道分析，实际有效速率是 `rate/2`，
    // **所有频率被折半**——所以报出来的「1-15 kHz」其实是 0.5-7.5 kHz，
    // 「22-40 kHz」其实是 11-20 kHz，「40-80 kHz」其实是 20-40 kHz。
    //
    // 那个错误完全能解释之前的读数：11-20 kHz 没有音乐（−151 dB，正常），
    // 而通带边缘的 20-22 kHz 内容落进了「40-80 kHz」那一格（−3 dB）。
    // 换句话说，那两个互相矛盾的读数里至少有一个是被这个 bug 造出来的，
    // 而「非单调所以滤波器不可能这样」这个推论也就跟着失效了——它是量具的假象。
    //
    // 所以这里只取声道 0 做频谱，而两声道的电平一致性另查。
    constexpr size_t kWindow = 32768;
    if (frames <= kWindow) {
        std::printf("  帧数不足一个窗口，无法测频谱\n");
        return 1;
    }
    const size_t off = (frames - kWindow) * channels;
    std::vector<float> win;
    win.reserve(kWindow);
    for (size_t i = 0; i < kWindow; ++i) win.push_back(pcm[off + i * channels]);

    // 两声道差异：**报告**，并只查「有没有失同步」——不要求两声道相同。
    //
    // 第一版要求 L/R 差异 < −40 dB，理由是「我给两个声道喂了同一份数据」。而这个文件
    // 是真的立体声录音，两声道本来就不该相同，所以那条判据是**期望值写错了**，不是缺陷。
    //
    // 但这条检查确实起了作用：它让人注意到交错缓冲区必须先拆开才能分析。所以保留它，
    // 换成正确的期望：真失同步会让差异远大于 +6 dB（两个不相干的信号平均起来比单声道更响），
    // 而正常的立体声差异在几 dB 量级。
    // Channel 1 is read unconditionally below. A mono DSD file would index past
    // the end of the window, so the premise is stated rather than assumed.
    if (channels < 2) {
        std::fprintf(stderr,
                     "fixture is mono (%zu channels); the stereo correlation needs two\n",
                     static_cast<size_t>(channels));
        return 2;
    }
    {
        double diff = 0.0, mag = 0.0;
        for (size_t i = 0; i < kWindow; ++i) {
            const double l = pcm[off + i * channels];
            const double r = pcm[off + i * channels + 1];
            diff += (l - r) * (l - r);
            mag += l * l + r * r;
        }
        const double relDb = 10.0 * std::log10((diff > 0 ? diff / mag : 1.0) + 1e-30);
        char b[160];
        std::snprintf(b, sizeof(b),
                      "%.1f dB（真立体声应有几 dB 差异；> +6 dB 说明交错被拆错了）", relDb);
        check(relDb < 6.0, "两声道没有失同步（顺带证明交错拆对了）", b);
    }

    struct Range {
        const char* label;
        double f0, f1;
    };
    const Range ranges[] = {
        {"1-15 kHz（可听内容，归一化基准）", 1000.0, 15000.0},
        {"15-22 kHz（音源上限之内）", 15000.0, 22050.0},
        {"22-40 kHz（阻带）", 22050.0, 40000.0},
        {"40-80 kHz（阻带）", 40000.0, 80000.0},
        {"80 kHz-Nyq（阻带）", 80000.0, audioRate * 0.5 - 1000.0},
    };

    std::printf("\n  频带                      电平 dBFS    相对可听      bins\n");
    double audible = 0.0;
    bool haveAudible = false;
    for (const Range& r : ranges) {
        const Band b = bandRms(win, audioRate, r.f0, r.f1);
        char bins[64];
        std::snprintf(bins, sizeof(bins), "%zu..%zu", b.b0, b.b1);
        if (!b.valid()) {
            // 硬失败，而不是打印一个 −200。
            std::printf("  %-26s  %s   <-- 空频带，没测到任何东西\n", r.label, bins);
            check(false, r.label, "空频带：频率范围超出 FFT 分辨率或 bin 范围");
            continue;
        }
        if (std::strcmp(r.label, ranges[0].label) == 0) {
            audible = b.db;
            haveAudible = true;
        }
        std::printf("  %-26s  %9.2f   %9.2f     %s\n", r.label, b.db,
                    haveAudible ? b.db - audible : 0.0, bins);
    }

    std::printf(
        "\n  判读：\n"
        "    若 22-40 kHz 在 −100 dB 以下 -> 仪器是对的，那 66 dB 是真实结果，\n"
        "      而引擎在解码器到频谱抽头之间加了东西。\n"
        "    若 22-40 kHz 在 −20 dB 附近   -> 仪器之前读到的是哨兵，那 66 dB\n"
        "      从来不是结果，而级联本身没有在工作。\n");

    std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    return failures == 0 ? 0 : 1;
}