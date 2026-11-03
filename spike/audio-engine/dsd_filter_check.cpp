// 生产版抽取滤波器的仪器：流式必须与整块逐样本相同。
//
// # 为什么这是唯一的判据
//
// `dsd_filter.cpp` 里的 `Stage`跨调用保留 `taps - 1` 个历史。如果那部分写错了——
// 比如漏了历史、或者每块重新 `reset()`——输出会是：
//
//   * 长度对（抽取因子决定长度）
//   * 电平对（通带增益 1.0）
//   * 频谱大体对（滤波器形状还在）
//   * **在每个块边界上有一个阶跃**
//
// DSF 的块是 4096 字节/声道，第一级的抽取周期是 8 字节。所以那个阶跃每 14.6 ms 一次。
// 峰值检查通过，时长检查通过，频带 RMS 也只差一点点。只有把同一段数据
//
//   * 整块算一遍（`decimate`，一次调用）
//   * 按任意不对齐的长度喂进去（`Cascade::feed`/`feedChannels` 多次调用）
//
// 然后**逐样本比**，才看得出差别。
//
// 而「听」是最后一道，不是第一道——14.6 ms 一次的阶跃在音乐里很容易被盖过去。
//
// # 喂进去的长度刻意不对齐
//
// 用 8 的倍数去喂会让一个「按整块处理」的错误实现看起来完全正确。所以这里用
// 1、3、7、13、4095、4096、4097 这些长度——没有一个是 8 的倍数，而且包含一个恰好
// 等于 DSF 块大小的（4096）以及比它小 1 和大 1 的（4095/4097）。如果实现只在整块
// 边界上正确，这三种里至少有一种会失败。
//
// usage: dsd_filter_check


#include "dsd_filter.h"


#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>


namespace {


int failures = 0;

/// 在指定频率上用 Goertzel 估幅度（dB，相对满幅正弦）。
///
/// 绕开 FFT 的理由和别处一样：1-bit 流的长度不是 2 的幂，而补零会抬高旁瓣，旁瓣恰好
/// 和要测的东西同量级。这里只关心几个具体频点，Goertzel 对任意长度都成立。
double goertzelDb(const std::vector<double>& x, double hz, double rate) {
    if (x.size() < 256) return 0.0;
    const double w = 2.0 * M_PI * hz / rate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (double v : x) {
        const double s0 = v + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    // Standard Goertzel amplitude: a full-scale sine reads 1.0. The previous
    // version also multiplied by 2*sin(pi*f/fs), which is not part of the
    // algorithm and cost -41 dB at 1 kHz and -17.5 dB at 15 kHz, so the
    // passband probes failed while the filter was fine.
    const double amp = 2.0 * std::sqrt(std::fabs(power)) / static_cast<double>(x.size());
    const double level = amp > 1e-15 ? amp : 1e-15;
    return 20.0 * std::log10(level);
}


void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-50s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}


/// 一段确定性的伪随机比特流。用固定的 xorshift 而不是 `rand()`，因为结果必须能重复——
/// 一个不可重复的失败没法在第二次运行时确认。
std::vector<uint8_t> makeBits(size_t byteCount, uint32_t seed) {
    std::vector<uint8_t> out(byteCount);
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < byteCount; ++i) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        out[i] = static_cast<uint8_t>(s >> 11);
    }
    return out;
}


uint8_t reverseBits(uint8_t v) {
    v = static_cast<uint8_t>(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = static_cast<uint8_t>(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    return static_cast<uint8_t>(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
}


std::vector<double> bitsFromBytes(const std::vector<uint8_t>& raw) {
    std::vector<double> v;
    v.reserve(raw.size() * 8);
    for (uint8_t b : raw) {
        const uint8_t r = reverseBits(b);
        for (int i = 0; i < 8; ++i) v.push_back((r >> i) & 1 ? 1.0 : -1.0);
    }
    return v;
}


// 流式跑一遍，返回交错浮点。
//
// `maxFramesPerCall` 是每次 `feedInterleaved` 的容量上限——**这一项是引擎真正的用法**。
// 第一版所有档位都给了「容量给足」，而引擎每次只给「还缺多少帧」。所以「小容量下
// 分批抽干」这条路径从未被测过，而它正是引擎唯一走的那条。
//
// 而引擎里那条路的实测结果是：24-48 kHz 只比可听内容低 2.25 dB，而整块算出来那里
// 应该是 −173 dB 相对。差 170 dB，所以这条路径要么有 bug，要么与整块不等价——
// 两者都要在这里分开。
std::vector<float> runStreaming(const std::vector<uint8_t>& raw, uint32_t dsdRate,
                                uint32_t channels, const std::vector<size_t>& chunkPattern,
                                size_t maxFramesPerCall) {
    neiro::dsd::Cascade cascade;
    double audioRate = 0;
    std::string err;
    if (!cascade.configure(dsdRate, channels, &audioRate, &err)) {
        std::printf("  configure 失败：%s\n", err.c_str());
        failures++;
        return {};
    }

    std::vector<uint8_t> chunk;
    std::vector<float> out;
    size_t offset = 0;
    size_t p = 0;
    while (offset < raw.size()) {
        size_t want = chunkPattern[p % chunkPattern.size()];
        const size_t n = want > raw.size() - offset ? raw.size() - offset : want;
        p++;
        // 同一批字节喂给所有声道，所以交织之后每两个字节相同——「成对重复」于是本身
        // 就成一个可检查的性质。
        chunk.assign(n * channels, 0);
        for (size_t i = 0; i < n; ++i)
            for (uint32_t ch = 0; ch < channels; ++ch) chunk[i * channels + ch] = raw[offset + i];

        // 容量不足时剩下的样本留在缓冲里，所以必须反复抽干——这正是引擎的循环做的事。
        for (;;) {
            const size_t before = out.size();
            cascade.drainPending(&out, maxFramesPerCall);
            if (out.size() == before) break;
        }
        cascade.feedInterleaved(chunk.data(), n, &out, maxFramesPerCall);
        offset += n;
    }
    // 末尾把滤波器历史抽干。**只抽干，不再喂新数据**——第一版在这里喂空闲字节，于是
    // 缓冲里已有的样本与空字节解出来的样本混在一起，帧数直接翻倍，还被读成「÷2 那级
    // 没生效」。
    for (;;) {
        const size_t before = out.size();
        cascade.drainPending(&out, maxFramesPerCall);
        if (out.size() == before) break;
    }
    return out;
}


/// 整块跑一遍（`decimate` 两次），返回交错浮点。
std::vector<float> runWholeBuffer(const std::vector<uint8_t>& raw, uint32_t dsdRate,
                                  uint32_t channels) {
    const double inRate = static_cast<double>(dsdRate);
    const std::vector<double> h8 = neiro::dsd::designDecimationFilter(inRate, 20000.0, 22050.0,
                                                                       100.0);
    const std::vector<double> h2 = neiro::dsd::designDecimationFilter(inRate / 8, 20000.0,
                                                                      22050.0, 100.0);
    std::vector<float> out;
    std::vector<std::vector<double>> perChannelBits(channels);
    for (uint8_t b : raw) {
        for (uint32_t ch = 0; ch < channels; ++ch)
            perChannelBits[ch].push_back((reverseBits(b) >> 0) & 1 ? 1.0 : -1.0);
    }
    (void)bitsFromBytes;
    // 每个声道用相同的比特（伪随机的字节），只是为了测长度与相位，不测立体声差异。
    const std::vector<double> bits = bitsFromBytes(raw);
    const std::vector<double> s1 = neiro::dsd::decimate(bits, h8, 8);
    const std::vector<double> s2 = neiro::dsd::decimate(s1, h2, 2);
    for (double v : s2)
        for (uint32_t ch = 0; ch < channels; ++ch) out.push_back(static_cast<float>(v));
    return out;
}


/// 逐样本比较，返回第一个不同的样本的序号（没有不同则返回 -1）。
/// 只比前 `limit` 个样本。
long long firstDifference(const std::vector<float>& a, const std::vector<float>& b,
                          size_t limit, double* worstAbs) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    if (limit && n > limit) n = limit;
    *worstAbs = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        if (d > *worstAbs) *worstAbs = d;
        if (d > 1e-6) return static_cast<long long>(i);
    }
    return -1;
}


}  // namespace


int main() {
    constexpr uint32_t kDsdRate = 2822400;  // DSD64
    constexpr uint32_t kChannels = 2;

    // --- 隔离测试：浮点正弦直接进 decimate ---
    //
    // 真实文件上测到 40-80 kHz 只剩 −3.00 dB（相对可听内容），而这个滤波器的脉冲响应实测
    // 阻带是 −99.6 dB。两者不可能同时为真，所以要么设计错、要么应用错。
    //
    // 脉冲响应那条已经排除设计，所以是应用。而**隔离**的办法是绕开整条 DSD 路径：直接给
    // `decimate` 一个 40 kHz 的浮点正弦。输出（40/8 = 5 kHz 处）必须是零。
    //
    // 绕开比特流是关键：1-bit 流的位序反了只改相位不改幅度，所以如果测的是比特流，
    // 「位序错了」和「滤波器没生效」会给同一个频谱。
    {
        std::printf("== 隔离测试：浮点正弦经过 decimate ==\n");
        const double inRate = static_cast<double>(kDsdRate);
        const std::vector<double> h = neiro::dsd::designDecimationFilter(inRate, 20000.0,
                                                                          22050.0, 100.0);
        const size_t n = 1u << 18;
        struct Probe {
            const char* label;
            double hz;
            bool inBand;
        };
        const Probe probes[] = {
            {"1000 Hz（在通带内）", 1000.0, true},
            {"15000 Hz（在通带内）", 15000.0, true},
            {"40000 Hz（阻带）", 40000.0, false},
            {"60000 Hz（阻带）", 60000.0, false},
        };
        for (const Probe& p : probes) {
            std::vector<double> x(n);
            for (size_t i = 0; i < n; ++i)
                x[i] = std::sin(2.0 * M_PI * p.hz * static_cast<double>(i) / inRate);
            const std::vector<double> y = neiro::dsd::decimate(x, h, 8);
            // 抽取不改变频率：输出速率变成 inRate/8，而一个 1 kHz 正弦仍然是 1 kHz
            // （每周期 352.8 个采样点）。早先这里按 hz/8 去测，于是通带测试在错误的
            // 频点上读 −107 dB——量的是它自己的错，而不是滤波器。
            const double db = goertzelDb(y, p.hz, inRate / 8.0);
            char b[160];
            std::snprintf(b, sizeof(b), "输出 %zu 样本，%.0f Hz 处 %.2f dB（满幅正弦 = 0 dB）",
                          y.size(), p.hz, db);
            // 通带内必须接近 0 dB（增益 1.0），阻带必须远低于 -80 dB。
            if (p.inBand) {
                check(db > -1.0 && db < 1.0, p.label, b);
            } else {
                check(db < -80.0, p.label, b);
            }
        }
    }


    // --- 先把各级到底产出了多少看清楚，而不是从最终帧数反推 ---
    //
    // 一次性喂入整段数据，流式给出 31667 帧而整块给出 15283 帧，正好差一倍多。而
    // 31667 恰好是 ÷8 之后的数量（31665 加尾部 2）——所以看起来 ÷2 那级没生效。
    // 但代码读下来是对的，所以先量各级实际产出，不猜。
    {
        std::printf("== 各级实际产出（聚焦测量）==\n");
        const std::vector<uint8_t> probe = makeBits(32768, 0x9E3779B9u);
        const double inRate = static_cast<double>(kDsdRate);
        const std::vector<double> h8 =
            neiro::dsd::designDecimationFilter(inRate, 20000.0, 22050.0, 100.0);
        const std::vector<double> h2 =
            neiro::dsd::designDecimationFilter(inRate / 8, 20000.0, 22050.0, 100.0);
        const std::vector<double> bits = bitsFromBytes(probe);
        const std::vector<double> s1 = neiro::dsd::decimate(bits, h8, 8);
        const std::vector<double> s2 = neiro::dsd::decimate(s1, h2, 2);
        std::printf("  输入比特        %zu\n", bits.size());
        std::printf("  h8 抽头%zu  h2 抽头 %zu\n", h8.size(), h2.size());
        std::printf("  第一级 ÷8 输出  %zu  (理论 %zu)\n", s1.size(), bits.size() / 8);
        std::printf("  第二级 ÷2 输出  %zu  (理论 %zu)\n", s2.size(), s1.size() / 2);


        // 流式：Cascade 一次喂入，然后用足够大的容量把缓冲排空。
        neiro::dsd::Cascade cascade;
        double ar = 0;
        std::string err;
        cascade.configure(kDsdRate, kChannels, &ar, &err);
        std::vector<uint8_t> chunk(probe.size() * kChannels);
        for (size_t i = 0; i < probe.size(); ++i)
            for (uint32_t ch = 0; ch < kChannels; ++ch) chunk[i * kChannels + ch] = probe[i];
        std::vector<float> out;
        const size_t got = cascade.feedInterleaved(chunk.data(), probe.size(), &out, 1u << 20);
        std::printf("  Cascade 一次喂入  %zu 帧（容量给足，不截断）\n", got);
        std::printf("  理论应为 %zu\n", bits.size() / 16);
        check(got * 16 < bits.size() + 64 * 16,
              "Cascade 的输出帧数与 ÷16 一致（不是 ÷8）",
              "得到 " + std::to_string(got) + "，理论 " + std::to_string(bits.size() / 16));
    }


    std::printf("\n== 抽取滤波器的流式 / 整块一致性 ==\n");
    std::printf("  DSD64 %u Hz 比特率，2 ch，级联 ÷8 再 ÷2 -> %u Hz\n", kDsdRate, kDsdRate / 16);


    // 32768 字节 = 262144 比特 -> 整块约 16384 个 176400 Hz 样本。足够长到滤波器
    // 两端的历史远小于主体，也足够短到跑得快。
    const std::vector<uint8_t> raw = makeBits(32768, 0x9E3779B9u);


    double audioRate = 0;
    {
        neiro::dsd::Cascade probe;
        std::string err;
        const bool ok = probe.configure(kDsdRate, kChannels, &audioRate, &err);
        char b[128];
        std::snprintf(b, sizeof(b), "%.0f Hz（应为 %u）", audioRate, kDsdRate / 16);
        check(ok && audioRate == static_cast<double>(kDsdRate / 16), "配置的输出速率", b);
    }


    const std::vector<float> whole = runWholeBuffer(raw, kDsdRate, kChannels);
    std::printf("  整块结果：%zu 帧\n", whole.size() / kChannels);


    struct Pattern {
        const char* label;
        std::vector<size_t> sizes;
        size_t maxFramesPerCall;
    };
    // 刻意没有一个是 8 的倍数；含 4095/4096/4097（DSD 块大小 ±1 与正好）。
    //
    // 容量这一维是**引擎真正的用法**：引擎每次只给「还缺多少帧」，而第一版所有档位都给了
    // 「容量给足」。引擎里 24-48 kHz 只比可听内容低 2.25 dB，而整块算出来那里是 −173 dB
    // 相对——差 170 dB，所以「小容量 + 分批抽干」这条路径必须单独测。
    const Pattern patterns[] = {
        {"一次性整块喂入（32768 字节），容量给足", {32768}, static_cast<size_t>(1) << 20},
        {"每次 1 字节（最坏情况），容量给足", {1}, static_cast<size_t>(1) << 20},
        {"每次 3 字节，容量给足", {3}, static_cast<size_t>(1) << 20},
        {"每次 7 字节（8 的倍数 −1），容量给足", {7}, static_cast<size_t>(1) << 20},
        {"每次 13 字节，容量给足", {13}, static_cast<size_t>(1) << 20},
        {"4095/4096/4097 交替（DSD 块边界），容量给足", {4095, 4096, 4097},
         static_cast<size_t>(1) << 20},
        {"随机 1..40 混合，容量给足", {1, 17, 2, 40, 5, 33, 8, 12, 3, 25},
         static_cast<size_t>(1) << 20},
        // 下面三档是引擎的实际用法：整块 4096 字节喂进去，每次只拿少量帧。
        {"引擎用法：4096 字节整块，每次 512 帧", {4096}, 512},
        {"引擎用法：4096 字节整块，每次 64 帧", {4096}, 64},
        {"引擎用法：4096 字节整块，每次 1 帧", {4096}, 1},
    };

    for (const Pattern& p : patterns) {
        const std::vector<float> streamed =
            runStreaming(raw, kDsdRate, kChannels, p.sizes, p.maxFramesPerCall);
        double worst = 0.0;
        // 只比前 8192 帧：流式版开头会比整块版**晚 taps/16 个样本**才开始产出（它在等
        // 历史到齐），所以直接从头对齐比较会报出一个假的不同。比较前先各自跳过同样多
        // 的样本——但跳过多少不该靠猜，所以这里先量出偏移量再比。
        char detail[256];
        std::snprintf(detail, sizeof(detail), "整块 %zu 帧，流式 %zu 帧", whole.size() / kChannels,
                      streamed.size() / kChannels);
        std::printf("\n  %s\n    %s\n", p.label, detail);


        // 帧数已经一致，所以直接逐样本比，不需要任何对齐搜索。
        //
        // 第一版加了「找对齐点」的启发式，理由是「流式会晚 taps/16 个样本才开始产出」。
        // 那个理由在**长度上**是对的（滤波器要等历史到齐），但既然两边长度都等于 15283，
        // 就说明流式并没有更短——它只是**不产出**那几个样本，而不是晚产出同样多的。
        // 于是长度自动对齐了，搜索纯属多余，而且它把「开头就相等」（偏移 0）判成失败。
        const long long bad = firstDifference(whole, streamed, 0, &worst);
        std::snprintf(detail, sizeof(detail), "最大差 %.3e", worst);
        check(bad < 0, "逐样本与整块相同（历史没有丢）", detail);
        if (bad >= 0) {
            const size_t k = static_cast<size_t>(bad);
            std::printf("      第一个不同的样本在 %zu（%.2f ms）\n", k,
                        static_cast<double>(k) * 1000.0 / 176400.0);
            std::printf("        整块 %+.9f   流式 %+.9f   差 %+.9f\n", whole[k], streamed[k],
                        whole[k] - streamed[k]);
            std::printf("        整块前 4 个：");
            for (size_t i = 0; i < 4 && i < whole.size(); ++i) std::printf(" %+.6f", whole[i]);
            std::printf("\n        流式前 4 个：");
            for (size_t i = 0; i < 4 && i < streamed.size(); ++i) std::printf(" %+.6f", streamed[i]);
            std::printf("\n");
        }
    }


    std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    return failures == 0 ? 0 : 1;
}
