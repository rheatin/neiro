// 同一个引擎里做 A/B：FFmpeg 的 DSD 解码器 vs 自写的抽取级联。
//
// # 为什么必须是 A/B 而不是拿仪器比
//
// `dsd_decim` 已经测出两者的频带差异（−220.39 vs −154.37 dBFS），但那是**仪器**里的
// 数字，中间没有引擎、没有 soxr、没有 WASAPI。而那 66 dB 会不会真的出现在输出端，取决于
// 后面几级：
//
//   * 引擎的 soxr（176400 -> 192000）会不会自己引入折叠；
//   * 引擎报告的采样率是不是真的变了（如果忘了改，重采样器会按 352800 工作，
//     那是一个「能播但速率错了两倍」的缺陷）；
//   * 抽取级联之后引擎的比特计数、时长、seek 是否还对得上。
//
// 这些只有在引擎里才存在。所以这里跑同一个文件两次，只切环境变量，然后把两次的频谱
// 放在一起比。
//
// # 用法
//
//   dsd_path_ab <file.dsf>
//   （环境变量 NEIRO_DSD_DECODER 由调用方设置，本仪器自己会跑两次）

#include "nae.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-50s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

struct BandRms {
    double audible = -300.0;  // 1-15 kHz
    double b20_40 = -300.0;
    double b40_80 = -300.0;
    double b80_nyq = -300.0;
    // 粗频谱：形状比单个频带的数字更能说明问题。
    //
    // 20-40 kHz 只比可听内容低 1.3 dB——那个形状是**噪声整形过的 DSD 噪声**（噪声被
    // 推向高频），而一个通带到 20 kHz 的抽取滤波器之后不该还有这种形状。所以要看的不是
    // 「差多少」，而是「长什么形状」：未经滤波的噪声向高频爬升，过滤后的信号在阻带里
    // 应该塌下去。
    double coarse[8] = {-300.0, -300.0, -300.0, -300.0, -300.0, -300.0, -300.0, -300.0};
    // 0-48 kHz、每 2 kHz 一格的形状。
    //
    // 这是分辨「音乐被上移了」与「那里真的有超声内容」的唯一办法：级联已经测到阻带在
    // −156 dB，而引擎里 24-48 kHz 却只低 2.25 dB。若音乐整体上移一个八度，可听段的
    // 形状会被拉宽一倍——在 2 kHz 分辨率下看得出来。
    double shape[24] = {0};
    int deviceRate = 0;      // 设备速率
    int sourceRate = 0;      // 引擎拿到的源速率
    int32_t bitPerfect = 0;
    int32_t resampling = 0;
    // 源位置对墙钟前进的速率。**这是不依赖频谱的那一半证据。**
    //
    // 阻带断崖出现在 2 x 22.05 kHz 说明信号被以两倍速率消费；而位置以两倍墙钟速度前进是
    // 同一件事的算术表达，且不需要 FFT。两条独立证据互相印证，才排得掉「频谱测量出错」
    // 这个解释。
    double positionAdvanceRatio = 0.0;
    std::string note;
};

/// 从引擎的频谱抽头算各频带 RMS。
///
/// 用引擎自己的 `NAE_QuerySpectrum`，而不是自己解码一遍——因为要测的正是**引擎送出去
/// 的东西**，在解码器输出上测就答错了问题。
BandRms measure(const std::string& path, int32_t forceRate) {
    BandRms out;
    std::string decodeNote;
    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    // `forceRate` > 0 时用 `NAE_OUTPUT_FIXED` 强制输出速率。
    //
    // 需要它是为了测「引擎在**重采样路径**上的底噪」：这台设备的原生混合格式里就有 44100，
    // 所以 `OUTPUT_NATIVE` 会让 44.1 kHz 的文件走 44100 -> 44100，**根本不重采样**。
    // 44.1 kHz 的 PCM 在 22.05 kHz 以上物理为空，所以强制上采样之后，22 kHz 以上读到的
    // 就是引擎在重采样路径上自己产生的东西——那才是 DSD 路径该被比对的底噪。
    const int32_t mode = forceRate > 0 ? NAE_OUTPUT_FIXED : NAE_OUTPUT_NATIVE;
    const int32_t rc =
        NAE_OpenFile(nullptr, path.c_str(), mode, forceRate > 0 ? forceRate : 0, 0, &h, &rate,
                     &srcRate, &ch, &bits, &valid, &isFloat, &resampling, &dithered, &bitPerfect,
                     &fileChannels, &durationMs, &latencyMs);
    if (rc != 0 || !h) {
        char buf[256];
        NAE_GetLastErrorText(buf, static_cast<int32_t>(sizeof(buf)));
        if (true)
            decodeNote = std::string("open failed rc=") + std::to_string(rc) + " " + buf;
        return out;
    }
    out.deviceRate = rate;
    out.bitPerfect = bitPerfect;
    out.resampling = resampling; out.sourceRate = srcRate;

    {
        char b[256];
        NAE_TrackInfo ti{};
        ti.structSize = sizeof(ti);
        int dsdRate = 0;
        if (NAE_QueryTrackInfo(h, &ti) == 0) {
            // `NAE_TrackInfo` 报的是**文件**的格式，与引擎开在哪个速率无关——这正是
            // 它存在的理由：文件是 DSD64 / 352.8 kHz，而设备在 192 kHz。
            std::snprintf(b, sizeof(b), "文件 %d Hz 比特率 / %d ch；引擎源 %d Hz -> 设备 %d Hz",
                          ti.sampleRate ? static_cast<int>(ti.sampleRate) : 0,
                          ti.channels ? static_cast<int>(ti.channels) : 0, srcRate, rate);
        } else {
            std::snprintf(b, sizeof(b), "NAE_QueryTrackInfo 失败；引擎源 %d Hz -> 设备 %d Hz",
                          srcRate, rate);
        }
        (void)dsdRate;
        if (true) decodeNote = b;
    }

    // 往前跑一段，跳过起始瞬态（滤波器建立、淡入）。
    NAE_Play(h, 2500);

    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(info);
    // 引擎的 FFT 是 4096，所以 bin 数是 2048；容量给足以免被截断。
    constexpr int32_t kBins = 4096;
    std::vector<float> spectrum(kBins, 0.0f);

    // 累加多个窗口，降低单个窗口的方差。频带 RMS 对单个窗口的随机性很敏感。
    constexpr int kWindows = 24;
    std::vector<double> acc(kBins, 0.0);
    uint32_t bins = 0, tapRate = 0;
    int used = 0;
    for (int i = 0; i < kWindows; ++i) {
        NAE_Play(h, 40);
        if (NAE_QuerySpectrum(h, &info, spectrum.data(), kBins) != 0) break;
        if (used == 0) {
            bins = info.binCount;
            tapRate = info.sampleRate;
        }
        if (info.binCount != bins || info.sampleRate != tapRate) break;
        for (uint32_t b = 0; b < bins && b < kBins; ++b) acc[b] += spectrum[b];
        ++used;
    }

    // 位置速率：**必须在 NAE_Close 之前**测。
    //
    // 第一版把它放在频带计算那一段，而那一段在 `NAE_Close(h)` 之后——于是 `NAE_Position(h)`
    // 拿到的是已释放的句柄，直接 0xC0000005。
    //
    // 而频带计算**可以**放在关闭之后，因为那些数据已经在 `acc` 里了。这里不一样：
    // `NAE_Position` 要问活着的引擎。所以顺序是「问完引擎，再关引擎，最后算频带」。
    {
        // `NAE_Position` is in **seconds** and `NAE_Play` takes **milliseconds**.
        // Dividing the first by the second gave a ratio whose ceiling was 0.001,
        // and the caller asserts 1.0 within 0.02 -- so this check could never
        // pass and the tool always exited 1, hiding the A/B result it exists to
        // report.
        //
        // The slice is also clamped to what is left of the file: a fixed 4 s
        // request against a 5 s file, after the spectrum pass above has already
        // consumed 3.5 s, leaves 1.5 s and a ratio of 0.375 that says nothing
        // about the engine's rate.
        const double posBefore = NAE_Position(h);
        const double duration = NAE_Duration(h);
        const double remainingMs = std::max(0.0, (duration - posBefore) * 1000.0);
        const double playMs = std::min(4000.0, remainingMs);
        if (playMs >= 500.0) {
            NAE_Play(h, static_cast<int32_t>(playMs));
            const double posAfter = NAE_Position(h);
            out.positionAdvanceRatio = (posAfter - posBefore) / (playMs / 1000.0);
        }
    }

    NAE_Close(h);

    if (used == 0 || bins == 0) {
        if (decodeNote.empty()) decodeNote = "频谱抽头没有返回任何窗口";
        return out;
    }

    const double binHz = static_cast<double>(tapRate) / static_cast<double>(bins);
    for (uint32_t b = 0; b < bins; ++b) acc[b] /= used;
    auto bandDb = [&](double f0, double f1) {
        size_t b0 = static_cast<size_t>(f0 / binHz);
        size_t b1 = static_cast<size_t>(f1 / binHz);
        if (b1 > bins) b1 = bins;
        if (b0 >= b1) return -300.0;
        double p = 0.0;
        for (size_t b = b0; b < b1; ++b) p += acc[b] * acc[b];
        const double rms = std::sqrt(p);
        return 20.0 * std::log10(rms > 1e-12 ? rms : 1e-12);
    };
    out.audible = bandDb(1000.0, 15000.0);
    out.b20_40 = bandDb(20000.0, 40000.0);
    out.b40_80 = bandDb(40000.0, 80000.0);
    out.b80_nyq = bandDb(80000.0, static_cast<double>(tapRate) * 0.5 - 1000.0);
    for (int i = 0; i < 8; ++i) {
        const double f0 = i * static_cast<double>(tapRate) / 8.0;
        const double f1 = (i + 1) * static_cast<double>(tapRate) / 8.0;
        out.coarse[i] = bandDb(f0, f1);
    }
    for (int i = 0; i < 24; ++i) {
        const double f0 = i * 2000.0;
        out.shape[i] = bandDb(f0, f0 + 2000.0);
    }

    out.note = decodeNote;
    return out;
}

/// 报告，并**同时给出绝对值与相对值**。
///
/// 只报绝对值是不够的，而第一版就只报了绝对值：两条路径的可听内容差 6 dB
/// （FFmpeg 的解码器输出被缩放到很小，实测 min/max ±0.069；而自写路径是 ±1 的比特
/// 展开），所以 40-80 kHz 上那 5.71 dB 的差距里绝大部分只是基准不同。
///
/// 相对值（相对各自 1-15 kHz）才是「超声底噪相对于音乐有多高」，也就是听感上关心的量。
void report(const char* label, const BandRms& m) {
    std::printf("\n  %s\n", label);
    std::printf("    %s\n", m.note.c_str());
    std::printf("    引擎源 %d Hz -> 设备 %d Hz  bitPerfect=%d  resampling=%d\n", m.sourceRate,
                m.deviceRate, m.bitPerfect, m.resampling);
    std::printf("    频带          绝对 dB    相对可听\n");
    std::printf("    1-15 kHz    %8.2f      %8.2f\n", m.audible, 0.0);
    std::printf("    20-40 kHz   %8.2f      %8.2f\n", m.b20_40, m.b20_40 - m.audible);
    std::printf("    40-80 kHz   %8.2f      %8.2f\n", m.b40_80, m.b40_80 - m.audible);
    std::printf("    80 kHz-Nyq  %8.2f      %8.2f\n", m.b80_nyq, m.b80_nyq - m.audible);
    std::printf("    粗频谱（每 %4.1f kHz，相对可听）：\n", m.deviceRate / 2000.0 / 8.0);
    for (int i = 0; i < 8; ++i) {
        char bar[64] = {0};
        const double rel = m.coarse[i] - m.audible;
        int bars = static_cast<int>((rel + 80.0) / 2.0);
        if (bars < 0) bars = 0;
        if (bars > 30) bars = 30;
        for (int k = 0; k < bars; ++k) bar[k] = '#';
        std::printf("      %5.1f-%5.1f kHz  %7.2f dB  %s\n", i * m.deviceRate / 8000.0,
                    (i + 1) * m.deviceRate / 8000.0, rel, bar);
    }
    // 0-48 kHz 每 2 kHz 一格。用来分辨「音乐被上移了」与「那里真的有超声内容」——
    // 前者会把可听段的形状拉宽一倍。
    std::printf("    可听段形状（每 2 kHz，相对 1-15 kHz）：\n      ");
    for (int i = 0; i < 24; ++i) {
        const double rel = m.shape[i] - m.audible;
        std::printf("%+6.1f", rel);
        if (i % 12 == 11) std::printf("\n      ");
    }
    std::printf("\n      （每行 12 格，从 0 kHz 起）\n");
    // 位置速率与频带无关，所以单独列出来——它是同一结论的独立证据。
    std::printf("    源位置前进 %.4f × 墙钟（1.0 = 正常速度；2.0 = 被以两倍速率消费）\n",
                m.positionAdvanceRatio);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dsd_path_ab <file.dsf>\n");
        return 1;
    }
    const std::string path = argv[1];
    // 可选的第二个参数：强制输出速率，用来测引擎在**重采样路径**上的底噪。
    const int32_t forceRate = argc >= 3 ? static_cast<int32_t>(std::atoi(argv[2])) : 0;
    if (forceRate > 0) std::printf("  （强制输出速率 %d Hz）\n", forceRate);

    std::printf("== DSD 解码路径 A/B（同一引擎、同一设备、同一个文件）==\n");
    std::printf("  文件 %s\n", path.c_str());

    // 第一次：不设环境变量 -> FFmpeg 的解码器。
    // 第一次：清空环境变量 -> FFmpeg 的解码器。引擎只判断这个变量是否存在，
    // 所以「存在但为空」就等于「关」。
    _putenv_s("NEIRO_DSD_DECODER", "");
    const BandRms a = measure(path, forceRate);
    report("A  FFmpeg dsd_lsbf_planar + 引擎 soxr", a);

    // 第二次：自写级联。
    _putenv_s("NEIRO_DSD_DECODER", "1");
    const BandRms b = measure(path, forceRate);
    report("B  自写 DSF 读取器 + ÷8/÷2 级联 + 引擎 soxr", b);

    std::printf("\n== 结论 ==\n");
    if (a.b40_80 < -290.0 || b.b40_80 < -290.0) {
        std::printf("  有一条路径没有测到频谱，无法比较。\n");
        return 1;
    }

    // 比**引擎看到的源速率**，不是设备速率。
    //
    // 第一版比的是 `NAE_OpenFile` 那个 `sampleRate` 出参，而那是**设备**速率——两边
    // 都是 192000，于是判据恒失败。而真正要看的是 `sourceRateOut`：自写路径必须报
    // 176400（它送出的就是那个），FFmpeg 路径报 352800（解码器声明的）。
    {
        char d[160];
        std::snprintf(d, sizeof(d), "A 源 %d Hz，B 源 %d Hz", a.sourceRate, b.sourceRate);
        check(b.sourceRate == 176400, "自写路径让引擎看到的源速率是 176400 Hz", d);
    }

    // 相对各自可听内容的差。
    //
    // 只比绝对值会得出错误的结论：两条路径的可听内容差 6 dB（FFmpeg 的解码器输出被缩放
    // 到很小，实测 min/max ±0.069；自写路径是 ±1 的比特展开），所以第一版那「+5.71 dB
    // 更差」里绝大部分只是基准不同。
    const double d20 = (b.b20_40 - b.audible) - (a.b20_40 - a.audible);
    const double d40 = (b.b40_80 - b.audible) - (a.b40_80 - a.audible);
    const double d80 = (b.b80_nyq - b.audible) - (a.b80_nyq - a.audible);
    char d[192];
    std::snprintf(d, sizeof(d), "20-40 %+.2f / 40-80 %+.2f / 80-Nyq %+.2f dB", d20, d40, d80);
    std::printf("  相对各自可听内容，B 比 A：\n");
    std::printf("    20-40 kHz     %+.2f dB\n", d20);
    std::printf("    40-80 kHz     %+.2f dB\n", d40);
    std::printf("    80 kHz-Nyq    %+.2f dB\n", d80);
    check(d80 < 0.0, "自写级联在 80 kHz 以上明显更低", d);

    // 位置速率：这一条完全不含频谱，所以它是「被以两倍速率消费」的独立证据。
    {
        char r[192];
        std::snprintf(r, sizeof(r), "A %.4f ×，B %.4f ×（正常应为 1.0）",
                      a.positionAdvanceRatio, b.positionAdvanceRatio);
        std::printf("  源位置前进：A %.4f ×，B %.4f ×（1.0 = 正常）\n", a.positionAdvanceRatio,
                    b.positionAdvanceRatio);
        // 先确认 A 是正常的，否则 B 的读数没有参照。
        check(std::fabs(a.positionAdvanceRatio - 1.0) < 0.02,
              "FFmpeg 路径的位置以正常速度前进（这是 B 的参照）", r);
        // 然后把 B 的读数报出来——**不判定**，因为还不知道该判成什么，而一个我不理解
        // 的读数不该被我写成 FAIL 或 PASS。
        std::printf("  B 的 %.4f 倍**只报告不判定**：需要先知道它偏离 1.0 的原因，\n"
                    "  才谈得上它是不是缺陷。\n",
                    b.positionAdvanceRatio);
    }

    std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    return failures == 0 ? 0 : 1;
}
