// DSD 抽取滤波器的原型：一步到位 vs 两级串联。
//
// # 要回答的问题
//
// 之前测到 DSD 通过真实输出路径时，40-80 kHz 比一个 96 kHz FLAC 高 **+103.6 dB**。
// 后来查明那个滤波器**存在**，在 FFmpeg 的 DSD 解码器内部（2822400 bps ÷ 8 -> 352800 Hz），
// 而且**不可配置**。于是链路是两级串联：
//
//     1-bit 2822400 -> [解码器内部 ÷8 + 滤波] -> 352800 -> [soxr ÷1.84 + 滤波] -> 192000
//
// 级联滤波器的阻带只取决于**较弱的那一级**。所以自己写解码器能改的、也是唯一能改的，
// 就是把这两级换成一步到位的抽取：2822400 -> 176400（÷16），再交给 soxr 做
// 176400 -> 192000。一步到位的滤波器可以把整个阻带设计到需要的位置。
//
// # 为什么两条路径要在同一个中间速率上比
//
// 两条路径都终止于 176400 Hz，然后都过同一个 soxr。soxr 的阻带是共同项，所以在
// 176400 处比较就**隔离出** DSD 阶段的差异。若直接比 192000 的输出，就分不清底噪
// 是解码器的还是 soxr 的。
//
// # 滤波器怎么设计
//
// 窗函数法（Kaiser）。通带到 `kPassbandHz`，阻带从 `kStopbandHz` 开始，两者都在
// **输出**速率下给定。抽头数由过渡带宽和目标衰减算出，不是试出来的——
//
//     N ≈ (A - 8) / (2.285 * Δω)        Δω 是输入速率下归一化的过渡带宽
//
// A = 100 dB 时通带纹波约 0.0002，过渡带宽 4 kHz、输入 2822400 Hz 时约 4500 抽头。
//
// 增益要乘以 D 补偿抽取带来的 1/D，否则通带会低 24 dB——而那会被读成「滤波器坏了」。
//
// usage: dsd_decim <file.dsf> [--blocks N]

#include "dsf_reader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

/// 第一类修正贝塞尔函数 I0，级数展开（定义在下面）。
double besselI0(double x);

/// Kaiser 窗。beta 控制旁瓣衰减与主瓣宽度的取舍。
std::vector<double> kaiserWindow(size_t n, double beta) {
    std::vector<double> w(n);
    const double r = static_cast<double>(n - 1) / 2.0;
    for (size_t i = 0; i < n; ++i) {
        const double t = (static_cast<double>(i) - r) / r;
        const double x = 1.0 - t * t;
        w[i] = (x <= 0.0) ? 0.0 : besselI0(beta * std::sqrt(x)) / besselI0(beta);
    }
    return w;
}

/// 第一类修正贝塞尔函数 I0，级数展开。beta 最大 12 左右，这个级数在那个范围内收敛得
/// 很好（相对误差 < 1e-12），比包含 <cmath> 的近似式更可预测。
double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 60; ++k) {
        term *= (x * x) / (4.0 * k * k);
        sum += term;
        if (term < sum * 1e-16) break;
    }
    return sum;
}

/// 设计低通：通带截止 `passbandHz`，阻带起始 `stopbandHz`，都按 `inRate` 给定。
///
/// # 增益是 1.0，不是 D——这里曾经写错过
///
/// 第一版把抽头乘了 D，理由是「补偿抽取带来的 1/D 增益」。**那个 1/D 只在箱式平均
/// 抽取里成立**——对 M 个样本求和再除以 M。而这里��� FIR 滤波之后每 D 个取一个，幅度
/// 一点不变，所以再乘 D 就是整体大 D 倍。
///
/// 抓住它的是 DSD 静音那个测量：全零的比特直流是 −1.0，滤出来却是 **−15.999994**，
/// 正好是 ×16。
///
/// 更值得记的是**那条检查通过了**：`系数和 16.0（应为 16）` 判为 PASS，而它断言的
/// 「应为 16」正是错的。一条编码了错误信念的检查会干净地通过——所以检查项本身也要
/// 拿别的东西对一遍，不能自证。
std::vector<double> designLowpass(double inRate, int D, double passbandHz, double stopbandHz,
                                  double attenuationDb) {
    const double fc = 0.5 * (passbandHz + stopbandHz);          // 过渡带中心
    const double transition = stopbandHz - passbandHz;
    const double dOmega = 2.0 * M_PI * transition / inRate;     // 输入速率下归一化
    int taps = static_cast<int>((attenuationDb - 8.0) / (2.285 * dOmega));
    if (taps < 1) taps = 1;
    // Kaiser 的 beta 经验值。
    const double beta = 0.1102 * (attenuationDb - 8.7);
    if (taps & 1) ++taps;  // 奇数长度才能有整数延迟

    const std::vector<double> w = kaiserWindow(static_cast<size_t>(taps), beta);
    std::vector<double> h(static_cast<size_t>(taps));
    const double m = (taps - 1) / 2.0;
    const double wn = 2.0 * fc / inRate;
    for (int i = 0; i < taps; ++i) {
        const double n = static_cast<double>(i) - m;
        const double ideal = (std::fabs(n) < 1e-12) ? wn : std::sin(M_PI * wn * n) / (M_PI * n);
        // 增益 1.0。`D` 只用于抽头数与过渡带宽的关系，不进系数。
        h[static_cast<size_t>(i)] = ideal * w[static_cast<size_t>(i)];
    }
    return h;
}

/// 用 h 对 x 做 FIR，然后按 D 抽取。只取滤波器完全建立的输出（丢掉每边 taps/2 个）。
std::vector<double> decimate(const std::vector<double>& x, const std::vector<double>& h, int D) {
    const int taps = static_cast<int>(h.size());
    const int delay = taps / 2;
    std::vector<double> out;
    if (x.size() < static_cast<size_t>(taps)) return out;
    for (size_t i = static_cast<size_t>(delay); i + static_cast<size_t>(delay) < x.size(); ++i) {
        if (static_cast<int>(i) % D) continue;
        double acc = 0.0;
        for (int k = 0; k < taps; ++k) acc += h[static_cast<size_t>(k)] * x[i - delay + static_cast<size_t>(k)];
        out.push_back(acc);
    }
    return out;
}

/// 基 2 FFT，就地。n 必须是 2 的幂。
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
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
        }
    }
}

/// 各频带的 RMS（dBFS）。用 Hann 窗 + FFT，逐 bin 求功率再按频带求和。
///
/// Hann 窗是必需的：矩形窗的旁瓣只有 −13 dB，会把 44 kHz 的内容漏进 20 kHz 的带里，
/// 而那个泄漏恰好和要测的东西同量级——量出来会是滤波器自己的旁瓣，不是源的噪声。
/// 频带 RMS。空频带返回 `outDb = -200.0`，并把 `outBins` 置 0。
///
/// `outBins` 是**判据的一部分**：为 0 表示这条频带里一个 bin 都没有，那���个 −200 唯一能
/// 支持的结论是「这里什么都没测到」，而不是「极其安静」——而后者正是它第一版被读成的意思。
/// 所以现在每个频带都报自己的 bin 范围，让数字自述。
void bandRms(const std::vector<double>& x, double rate, double f0, double f1, double* outDb,
             size_t* outBins = nullptr) {
    // 取 2 的幂长度的居中片段。
    size_t n = 1;
    while (n * 2 <= x.size()) n *= 2;
    if (n < 1024) { *outDb = 0.0; return; }
    const size_t start = (x.size() - n) / 2;
    std::vector<double> re(n), im(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        re[i] = x[start + i] * hann;
    }
    // 窗的相干增益必须除掉，否则所有频带都会高 1.76 dB。
    double winPower = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double hann = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        winPower += hann * hann;
    }
    fft(re, im);

    const double binHz = rate / static_cast<double>(n);
    size_t b0 = static_cast<size_t>(f0 / binHz);
    size_t b1 = static_cast<size_t>(f1 / binHz);
    if (b1 > n / 2) b1 = n / 2;
    if (b0 >= b1) {
        *outDb = -200.0;
        if (outBins) *outBins = 0;   // 空频带：数字无意义，标记出来
        return;
    }
    if (outBins) *outBins = b1 - b0;
    double power = 0.0;
    for (size_t b = b0; b < b1; ++b) power += re[b] * re[b] + im[b] * im[b];
    // 单边谱：除 2（除 DC 与 Nyquist），除窗功率，再除 n。
    const double rms = std::sqrt(2.0 * power / winPower / static_cast<double>(n));
    *outDb = 20.0 * std::log10(rms > 1e-12 ? rms : 1e-12);
}

/// 直接测滤波器的幅频响应：通带纹波与阻带衰减。
///
/// # 为什么必须单独测
///
/// 第二版的 B2 设计成通带 20 kHz、阻带 22.05 kHz、目标 100 dB，结果 20-40 kHz 频带
/// 停在 −2.90 dB。而路径 A 的第二级滤波器（阻带 28 kHz）却给出 −44.12 dB。
///
/// 一个阻带从 22.05 kHz 开始的 FIR 不可能有这个结果——所以**滤波器本身是坏的**，
/// 而从 DSD 输出反推永远只能看到一个「效果不对」，看不到坏在哪。
///
/// 所以这里把脉冲响应零填充后做 FFT，直接看它自己的响应。这条检查与 DSD 无关，
/// 因此它对「设计函数对不对」这个问题是独立且充分的。
///
/// 归一化：阻带电平相对于**通带增益**。通带增益是 1.0（0 dB）——第一版这里期望 D，
/// 于是整条检查都在按一个错误的基准报告数字。
void measureFilter(const std::vector<double>& h, int D, double inRate, double passbandHz,
                   double stopbandHz) {
    // 补到 2 的幂，使频率分辨率足够分辨窄过渡带。
    size_t n = 1;
    while (n < h.size() * 8) n *= 2;
    std::vector<double> re(n, 0.0), im(n, 0.0);
    for (size_t i = 0; i < h.size(); ++i) re[i] = h[i];
    fft(re, im);

    const double binHz = inRate / static_cast<double>(n);
    const size_t binOfPass = static_cast<size_t>(passbandHz * 0.8 / binHz);
    // 通带参考取 800 Hz 处的增益。
    double passGainDb = 0.0;
    {
        const size_t b = static_cast<size_t>(800.0 / binHz);
        if (b < n / 2) passGainDb = 10.0 * std::log10(re[b] * re[b] + im[b] * im[b] + 1e-30);
    }
    double ripple = 0.0;
    for (size_t b = binOfPass; b * binHz < passbandHz; ++b) {
        const double db = 10.0 * std::log10(re[b] * re[b] + im[b] * im[b] + 1e-30);
        const double d = std::fabs(db - passGainDb);
        if (d > ripple) ripple = d;
    }
    double worst = -300.0;
    double worstHz = 0.0;
    for (size_t b = static_cast<size_t>(stopbandHz / binHz); b < n / 2; ++b) {
        const double db = 10.0 * std::log10(re[b] * re[b] + im[b] * im[b] + 1e-30) - passGainDb;
        if (db > worst) {
            worst = db;
            worstHz = static_cast<double>(b) * binHz;
        }
    }
    std::printf("    通带增益 %.2f dB（应为 0.00）  纹波 %.3f dB\n", passGainDb, ripple);
    std::printf("    阻带最深 %.2f dB，出现在 %.0f Hz\n", worst, worstHz);

    char bb[96];
    std::snprintf(bb, sizeof(bb), "%.2f dB @ %.0f Hz（目标 -100）", worst, worstHz);
    // 阻带不达标时，**任何**用这个滤波器的下游测量都不可信——所以这是硬判据，
    // 不是参考信息。
    check(worst < -90.0, "滤波器阻带达到设计目标", bb);
    char br[96];
    std::snprintf(br, sizeof(br), "%.3f dB（应 < 0.5）", ripple);
    check(ripple < 0.5, "通带纹波可接受", br);
    // 通带增益必须独立于阻带被检查，而且期望值是 0 dB。
    //
    // 这一条在第一版写的是「期望 D」，于是它把一个整体大 24 dB 的滤波器判为 PASS。
    // 检查项自己断言了一个错的基准，所以它恒成立——**自证的检查等于没有检查**。
    char bp[96];
    std::snprintf(bp, sizeof(bp), "%.3f dB（应 0.000）", passGainDb);
    check(std::fabs(passGainDb) < 0.05, "通带增益是 0 dB（抽取不改变幅度）", bp);
}

/// 在指定频率上用 Goertzel 估幅度（dB，相对满幅正弦）。
///
/// 用 Goertzel 而不是 FFT：1-bit 流的长度是 blockSize*8 的整数倍，不是 2 的幂，而补零
/// 会抬高旁瓣——而旁瓣恰好和要测的东西同量级。这里只关心几个具体频点，Goertzel 对任意
/// 长度都成立。
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
    const double amp = std::sqrt(std::fabs(power)) * 2.0 / static_cast<double>(x.size());
    const double sinHalf = std::sin(w * 0.5);
    const double norm = (sinHalf > 1e-12) ? (2.0 * sinHalf) : 1.0;
    return 20.0 * std::log10((amp * norm > 1e-15 ? amp * norm : 1e-15) / 2.0);
}

/// 解码器对频率的传递函数：同一批频点上比较「解码器输出」与「原始比特流」，
/// 各自以自己的 1 kHz 为基准，所以结果只反映解码器做了什么。
///
/// # 为什么必须测这个
///
/// 两条路径在 20-40 kHz 上的排名与滤波器设计**矛盾**：路径 A 的最后一级通带 24 kHz，
/// 路径 C 的通带 20 kHz，所以路径 A 应当有**更多**超声能量，实测却低 31.7 dB。
///
/// 可能的解释只有一个：解码器对 20-40 kHz 做了我的滤波器没做的事。而解码器的原始输出
/// 显示 40-80 kHz 只比可听内容低 1.32 dB——所以它没有整体滤波器。一个只杀 20-40 kHz
/// 却放过 40-80 kHz 的东西不是滤波器，所以要么是解码器在做去整形，要么是两条路径的
/// 可听基准不同。
///
/// 两种解释导向完全不同的下一步（改滤波器 vs 改基准），所以在这里分开测，
/// 而不是继续从 DSD 输出反推。
void decoderTransfer(const std::vector<double>& rawBits, const std::vector<double>& decoded,
                     double bitRate, double decRate) {
    const double probeHz[] = {1000.0, 5000.0, 10000.0, 15000.0, 20000.0, 22000.0,
                              30000.0, 40000.0, 60000.0, 100000.0};
    std::printf("\n== 解码器的传递函数（各自以 1 kHz 为基准）==\n");
    std::printf("   频率     原始 1-bit 流   解码器输出     差值\n");
    for (double f : probeHz) {
        const double b = goertzelDb(rawBits, f, bitRate);
        const double d = goertzelDb(decoded, f, decRate);
        char line[128];
        std::snprintf(line, sizeof(line), "  %7.0f Hz   %9.2f dB    %9.2f dB   %+8.2f dB", f, b, d,
                      d - b);
        std::printf("%s\n", line);
    }
    std::printf(
        "  若差值随频率单调下降，解码器有一个真实的抽取滤波器；\n"
        "  若差值在高频回升，那么高频那些能量是解码器**引入**的（混叠），\n"
        "  而不是源里的——两者对「自己写解码器」的含义相反。\n");
}

/// DSD 的「静音」不是全零。
///
/// # 为什么这件事必须先测，而不是照抄
///
/// 全零在 1-bit 流里是一个**完全合法**的信号：每一位都是 +1，也就是一个满刻度的直流。
/// 解码之后它不是静音，是一个很大的偏置。
///
/// 而 DSD 真正的静音是 0x69 交替模式（LSB 优先拼写；MSB 优先的源是它的比特反转0x96）。
/// 这是从 Twilight Echo 的 `AudioPipeline.cpp` 学的——它写静音时特意按**源的位序**
/// 选择拼写，注释说「0x69 是 LSB 优先的写法，MSB 优先的源需要反转，两者都归一化到
/// 同一个 0x96 载荷字节」。
///
/// 一个只解码文件内容、从不产生静音的解码器不会碰到这个坑；一个要处理音轨间隙、
/// seek 落点、以及「解码器还没喂够数据时填什么」的解码器一定会碰到。而上面那三种情况
/// 在真实播放里都会发生。
///
/// 所以这里直接测两种填充各自解码成什么，而不是相信「静音应该是 0」这个直觉。
void measureDsdSilence(const std::vector<double>& h, int D) {
    std::printf("\n== DSD 静音该填什么 ==\n");
    struct Case {
        const char* label;
        uint8_t byte;
    };
    const Case cases[3] = {
        {"全零 0x00（直觉上的「静音」）", 0x00u},
        {"DSD 空闲模式 0x69（LSB 优先）", 0x69u},
        {"0x69 的比特反转 0x96（MSB 优先）", 0x96u},
    };

    for (const Case& c : cases) {
        // 造一段该字节重复的 1-bit 流，解码，看直流。
        std::vector<uint8_t> raw(8192, c.byte);
        const std::vector<double> bits = dsf::toSignLsbFirst(raw);
        double sum = 0.0;
        for (double v : bits) sum += v;
        const double dc = sum / static_cast<double>(bits.size());  // [-1, 1]

        // 再过一遍抽取滤波器，看它落到 PCM 上是多少。
        //
        // 直流增益应当**恰好是 1**：滤波器通带增益 1.0，而 FIR 之后的抽取不改变幅度。
        // 第一版这里滤出 −15.999994（×16），就是「抽头乘了 D」那个错误留下的痕迹——
        // 而这个测量是抓住它的那个测量。
        const std::vector<double> y = decimate(bits, h, D);
        double ysum = 0.0;
        for (double v : y) ysum += v;
        const double yDc = y.size() ? ysum / static_cast<double>(y.size()) : 0.0;

        char line[160];
        std::snprintf(line, sizeof(line), "  %-34s 比特直流 %+.4f -> PCM 直流 %+.6f", c.label, dc,
                      yDc);
        std::printf("%s\n", line);

        // 判据：全零**必须**给出满刻度直流（它是合法信号，不是静音），0x69 必须给出 0。
        // 这两条一起才说明「静音该填什么」这件事被测过，而不只是被打印过。
        if (c.byte == 0x00u) {
            char b[96];
            std::snprintf(b, sizeof(b), "PCM 直流 %+.6f（应是 ±1，不是 0）", yDc);
            check(std::fabs(std::fabs(yDc) - 1.0) < 0.01, "全零是满刻度直流，不是静音", b);
        } else {
            char b[96];
            std::snprintf(b, sizeof(b), "PCM 直流 %+.8f（应≈0）", yDc);
            check(std::fabs(yDc) < 1e-4, "空闲模式解码成 0（这才是静音）", b);
        }
    }
    std::printf(
        "  全零给出满刻度直流，不是静音；0x69 给出 0。\n"
        "  两者在示波器上都能出声，但全零那一条会让每个音轨间隙都「咔」一下。\n");
}

/// 报告频带，**不做任何归一化**。
///
/// 为什么后来改成绝对值：归一化版本给出的 20-40 kHz 排名与理论相反（级联通带更窄却
/// 测出高 40 dB），而两条路径的可听内容绝对电平又差 8.7 dB。归一化把「基准不同」
/// 和「底噪不同」混进同一个数字里，而这个混淆正是这一版要排除的。
///
/// 每条路径的 dBFS 各自以它自己的满刻度为参考——FFmpeg 的解码器输出有自己的刻度，
/// 我的 ±1 展开也有。所以跨路径比的是**同一路径内的频带比值**，而不是绝对 dBFS；
/// 绝对值的作用是让基准本身可见。
void reportAbs(const char* label, const std::vector<double>& x, double rate) {
    double a = 0, b = 0, c = 0, d = 0;
    size_t na = 0, nb = 0, nc = 0, nd = 0;
    bandRms(x, rate, 1000.0, 15000.0, &a, &na);
    bandRms(x, rate, 20000.0, 40000.0, &b, &nb);
    bandRms(x, rate, 40000.0, 80000.0, &c, &nc);
    bandRms(x, rate, 80000.0, rate * 0.5 - 1000.0, &d, &nd);
    std::printf("\n  %s（%.0f Hz，绝对 dBFS）\n", label, rate);
    // 每条频带都报自己的 bin 数。bin 数为 0 表示**这条频带里一个 bin 都没有**，
    // 那时的 dB 数字不是测量——第一版的 −200 哨兵就是这样被读成「极其安静」的。
    auto line = [&](const char* name, double db, size_t bins, double ref) {
        char binText[64];
        std::snprintf(binText, sizeof(binText), "%zu", bins);
        if (bins == 0) {
            std::printf("    %-12s %s   <-- 空频带，没测到任何东西\n", name, binText);
        } else {
            std::printf("    %-12s %9.2f   (相对 %+.2f dB)  %s bins\n", name, db, db - ref,
                        binText);
        }
    };
    line("1-15 kHz", a, na, a);
    line("20-40 kHz", b, nb, a);
    line("40-80 kHz", c, nc, a);
    line("80 kHz-Nyq", d, nd, a);
}

/// 报告三条路径在同一中间速率上的频带。
void report(const char* label, const std::vector<double>& x, double rate) {
    double a = 0, b = 0, c = 0, d = 0;
    bandRms(x, rate, 1000.0, 15000.0, &a);
    bandRms(x, rate, 20000.0, 40000.0, &b);
    bandRms(x, rate, 40000.0, 80000.0, &c);
    bandRms(x, rate, 80000.0, rate * 0.5 - 1000.0, &d);
    std::printf("\n  %s（%.0f Hz）\n", label, rate);
    std::printf("    1-15 kHz    %8.2f dBFS   (音频内容，用来归一化)\n", a);
    std::printf("    20-40 kHz   %8.2f dBFS\n", b);
    std::printf("    40-80 kHz   %8.2f dBFS\n", c);
    std::printf("    80 kHz-Nyq  %8.2f dBFS\n", d);
}

std::string rel(double ref, double now, const char* unit) {
    char b[96];
    std::snprintf(b, sizeof(b), "%s: %+.2f dB", unit, now - ref);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: dsd_decim <file.dsf> [--blocks N]\n");
        return 1;
    }
    const char* path = argv[1];
    int wantBlocks = 24;

    dsf::File dsf;
    if (!dsf.open(path)) {
        std::printf("%s\n", dsf.error.c_str());
        return 1;
    }
    const dsf::Format& fmt = dsf.fmt;
    // The decimation filters below were designed for DSD64: their passbands are
    // named after the 22.05 kHz audio band and their transition bands assume the
    // 2.8224 MHz bit rate. A DSD128 file would run them at the wrong rate and the
    // output is not a measurement of anything. State the premise.
    if (fmt.dsdRate != 2822400u) {
        std::fprintf(stderr,
                     "%s is %u Hz bit rate; this check's filter designs are DSD64 "
                     "(2822400 Hz).\n",
                     path, fmt.dsdRate);
        return 2;
    }
    const double inRate = static_cast<double>(fmt.dsdRate);
    const int channels = static_cast<int>(fmt.channels);
    int blocks = wantBlocks;
    if (static_cast<uint64_t>(blocks) > dsf.totalBlocks()) blocks = static_cast<int>(dsf.totalBlocks());

    std::printf("== 输入 ==\n");
    std::printf("  %s\n", path);
    std::printf("  %u Hz 比特率（%.0f Hz 音频），%d ch，%u 字节/块，取 %d 块\n", fmt.dsdRate,
                fmt.dsdRate / 64.0, channels, fmt.blockSize, blocks);

    // 读原始比特。每声道分开，立体声在块内交错。
    std::vector<std::vector<double>> bits(channels);
    std::vector<uint8_t> raw;
    for (int ch = 0; ch < channels; ++ch) {
        for (int b = 0; b < blocks; ++b) {
            if (!dsf.readBlock(static_cast<uint64_t>(b), static_cast<uint32_t>(ch), &raw)) {
                std::printf("读块 %d 声道 %d 失败\n", b, ch);
                return 1;
            }
            const std::vector<double> s = dsf::toSignLsbFirst(raw);
            bits[ch].insert(bits[ch].end(), s.begin(), s.end());
        }
        std::printf("  声道 %d: %zu 个比特 (%.2f s)\n", ch, bits[ch].size(),
                    bits[ch].size() / inRate);
    }

    // 直流：1-bit 流应当接近 0.5 的 1 比例。先查，否则后面全是错的。
    {
        double ones = 0.0;
        for (double v : bits[0]) ones += (v > 0.0) ? 1.0 : 0.0;
        const double ratio = ones / static_cast<double>(bits[0].size());
        char b[96];
        std::snprintf(b, sizeof(b), "%.6f（偏差 %+.4f%%）", ratio, (ratio - 0.5) * 100.0);
        check(std::fabs(ratio - 0.5) < 0.01, "1-bit 流的直流在 0.5 附近", b);
    }

    // --- 路径 B：一步到位 ÷16 到 176400 ---
    //
    // 两种通带设计都跑，因为第一版只测了一种就下了结论，结论是错的。
    //
    // 关键约束来自源：DSD64 的音频速率是 44.1 kHz，Nyquist 22.05 kHz，所以 22 kHz 以上
    // **全是噪声**。把通带放到 24 kHz 就是放行噪声——而解码器内部的滤波器通带更窄，
    // 于是它反而更好。所以那 51 dB 的差距不是解码器更强，是我的设计目标定错了。
    constexpr int kD = 16;
    const double outRateB = inRate / kD;

    struct Design {
        const char* label;
        double passbandHz;
        double stopbandHz;
    };
    const Design designs[2] = {
        {"B1  24/28 kHz（第一版：放行了 22 kHz 以上的噪声）", 24000.0, 28000.0},
        {"B2  20/22.05 kHz（按 DSD64 的 Nyquist 定）", 20000.0, 22050.0},
    };
    std::vector<std::vector<double>> pathBV(2);
    std::printf("\n== 滤波器设计 ==\n");
    for (int d = 0; d < 2; ++d) {
        const std::vector<double> h =
            designLowpass(inRate, kD, designs[d].passbandHz, designs[d].stopbandHz, 100.0);
        std::printf("  %s：%zu 抽头\n", designs[d].label, h.size());
        measureFilter(h, kD, inRate, designs[d].passbandHz, designs[d].stopbandHz);
        for (int ch = 0; ch < channels; ++ch) {
            std::vector<double> y = decimate(bits[ch], h, kD);
            if (ch == 0) pathBV[d] = y;
        }
    }
    std::vector<double> pathB = pathBV[1];

    {
        const std::vector<double> hTest = designLowpass(inRate, kD, 20000.0, 22050.0, 100.0);
        measureDsdSilence(hTest, kD);
    }

    // --- 路径 A：现有两级串联 ---
    //
    // 第一级用 FFmpeg 的解码器（它内部 ÷8 且不可配置），第二级用同样设计的 ÷2，
    // 两者都终止于 176400 Hz——这样与路径 B 在同一速率上比较。
    std::vector<double> pathA;
    {
        avformat_network_init();
        AVFormatContext* av = nullptr;
        if (avformat_open_input(&av, path, nullptr, nullptr) != 0) {
            std::printf("FFmpeg 打不开，跳过路径 A\n");
            return 1;
        }
        const AVCodec* dec = nullptr;
        const int si = av_find_best_stream(av, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
        AVCodecContext* cc = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(cc, av->streams[si]->codecpar);
        avcodec_open2(cc, dec, nullptr);
        std::printf("\n  路径 A 第一级：FFmpeg %s，声明 %d Hz（内部 ÷8）\n",
                    avcodec_get_name(cc->codec_id), cc->sample_rate);

        AVPacket* pkt = av_packet_alloc();
        AVFrame* fr = av_frame_alloc();
        const size_t want = static_cast<size_t>(inRate) * 4;
        while (av_read_frame(av, pkt) >= 0) {
            if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) == 0) {
                while (avcodec_receive_frame(cc, fr) == 0) {
                    const float* p =
                        static_cast<const float*>(static_cast<const void*>(fr->extended_data[0]));
                    for (int i = 0; i < fr->nb_samples; ++i)
                        pathA.push_back(static_cast<double>(p[i]));
                    av_frame_unref(fr);
                    if (pathA.size() >= want) break;
                }
            }
            av_packet_unref(pkt);
            if (pathA.size() >= want) break;
        }
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&av);
    }
    std::printf("  路径 A 第一级输出 %.0f Hz，解出 %zu 个样本\n", inRate / 8, pathA.size());

    // 第二级 ÷2，同样设计（输入速率变了，过渡带按输出给）。
    const std::vector<double> hA = designLowpass(inRate / 8, 2, 24000.0, 28000.0, 100.0);
    std::printf("  路径 A 第二级：÷2，通带 24 kHz，阻带 28 kHz，%zu 抽头 -> %.0f Hz\n", hA.size(),
                inRate / 16);
    std::vector<double> pathA2 = decimate(pathA, hA, 2);

    // 丢掉解码器启动瞬态。
    //
    // 第一版的比较里路径 A 看起来比路径 B 好 51 dB，而两条路径用的是**几乎相同**的
    // 终止滤波器——所以那个差距不可能来自滤波器设计。真正的来源是：路径 A 取的是文件
    // 最前面的几秒，包含解码器从零状态启动的瞬态；路径 B 在两端各丢了 taps/2。
    //
    // 而比较是按各自 1-15 kHz 内容归一化的，所以启动瞬态里的低频能量会抬高路径 A 的
    // 归一化基准，除完之后它的超声底噪**显得**更低。一个不公平的基线，会让较差的设计
    // 看起来更好——所以这里必须先丢掉瞬态再测。
    //
    // 丢多少不靠猜：滤波器长度按输入速率折算到时间，取它的两倍再加 50 ms 余量。
    const double settleSeconds =
        2.0 * static_cast<double>(hA.size()) / (inRate / 8) + 0.05;
    const double inRateA = inRate / 16;
    const size_t skipA = static_cast<size_t>(settleSeconds * (inRate / 8));
    std::printf("  路径 A 丢弃启动瞬态 %.0f ms（滤波器长度的两倍 + 50 ms）\n", settleSeconds * 1000.0);
    if (pathA2.size() > skipA + 8192) {
        pathA2.erase(pathA2.begin(), pathA2.begin() + static_cast<long>(skipA));
    } else {
        std::printf("  路径 A 解出的样本不够丢弃瞬态，比较不可靠\n");
    }

    // 两条路径都必须有足够的样本让 FFT 取到 2 的幂长度，否则频带 RMS 退化成 0。
    std::printf("  路径 A 剩 %zu 样本，路径 B %zu 样本\n", pathA2.size(), pathB.size());

    // 路径 C：两级都用**我的**滤波器，不经过 FFmpeg。
    //
    // 这是路径 A 的受控对照：路径 A 与路径 C 的唯一差别是解码器那级，而如果两者结果
    // 一致，那么「级联更好」这件事是**我的滤波器**的性质，与解码器无关；反之如果不一致，
    // 那么差异出在解码器的输出上。
    //
    // 加这一条是因为 B2 的结果在物理上讲不通：滤波器实测阻带 −99.6 dB，却让
    // 20-40 kHz 停在 −2.90 dB；而阻带 28 kHz 的 ÷2 却给出 −44 dB。两者都用我的滤波器，
    // 所以差别只能在输入或归一化上，而这一条能把它分开。
    std::vector<double> pathC;
    {
        const std::vector<double> h8 = designLowpass(inRate, 8, 20000.0, 22050.0, 100.0);
        const std::vector<double> h2 = designLowpass(inRate / 8, 2, 20000.0, 22050.0, 100.0);
        std::printf("\n  路径 C：我的 ÷8（%zu 抽头）再我的 ÷2（%zu 抽头），不经 FFmpeg\n",
                    h8.size(), h2.size());
        pathC = decimate(bits[0], h8, 8);
        pathC = decimate(pathC, h2, 2);
    }

    // 归一化前的绝对电平。不打印就无法区分「底噪更高」和「基准更低」。
    {
        double raw1 = 0, raw2 = 0, raw3 = 0;
        bandRms(pathA2, inRateA, 1000.0, 15000.0, &raw1);
        bandRms(pathB, outRateB, 1000.0, 15000.0, &raw2);
        bandRms(pathC, outRateB, 1000.0, 15000.0, &raw3);
        std::printf("\n  归一化前，1-15 kHz 的绝对电平：\n");
        std::printf("    路径 A（FFmpeg 输出）%9.2f dBFS\n", raw1);
        std::printf("    路径 B（我的 ÷16）   %9.2f dBFS\n", raw2);
        std::printf("    路径 C（我的两级）   %9.2f dBFS\n", raw3);
        std::printf("    B 比 C 差 %+.2f dB\n", raw2 - raw3);
    }

    // 解码器自己的滤波器有多宽？
    //
    // 这是解释那个 51 dB 差距的关键，所以要单独测：在**任何**我的滤波器之前，直接看
    // 解码器输出在 352800 Hz 上的频带。
    //
    // 第一版的假设是「解码器内部没有像样的滤波器，只是个 ÷8」，所以我设计成 24 kHz
    // 通带、28 kHz 阻带。实测却是我的差 51 dB。既然两条路径的终止滤波器几乎相同，
    // 差距不可能来自我的设计——只能来自**解码器把我放过的那一段也杀掉了**。
    //
    // 而它杀得掉，说明它的通带比 24 kHz 窄。这在物理上是对的：DSD64 的音频速率是
    // 44.1 kHz，Nyquist 22.05 kHz，所以 22 kHz 以上的内容**全是噪声**。
    // 放行 24 kHz 就是放行噪声——是我的设计错了，不是解码器错了。
    {
        const size_t want = static_cast<size_t>(1) << 18;
        std::vector<double> rawOut;
        if (pathA.size() > skipA + want) {
            rawOut.assign(pathA.begin() + static_cast<long>(skipA),
                          pathA.begin() + static_cast<long>(skipA + want));
        } else {
            rawOut = pathA;
        }
        if (rawOut.size() < 4096) {
            std::printf("  解码器原始输出样本不足，无法单独测它的滤波器\n");
        } else {
            double ref = 0;
            bandRms(rawOut, inRate / 8, 1000.0, 15000.0, &ref);
            const double g = ref > -200.0 ? std::pow(10.0, -ref / 20.0) : 1.0;
            for (double& v : rawOut) v *= g;
            report("解码器自己的输出（352800 Hz，未经我的滤波器）", rawOut, inRate / 8);
        }
    }

    {
        // 用同一段起点对齐：解码器输出取 transient 之后的一段，原始比特取对应偏移（÷8）。
        //
        // 长度由**可用数据**决定，不是想要的。第二版在这里写死了 1<<17 个解码器样本，
        // 换算成比特是 8 倍，于是取到了 1048576 比特——而 `bits[0]` 只有 786432。
        // 直接越界，`std::vector` 的迭代器构造没有边界检查，于是段错误。
        //
        // 长度从两端可用量里取，并且都留出余量。
        const size_t bitSkip = skipA * 8;
        size_t n = 0;
        if (bits[0].size() > bitSkip && pathA.size() > skipA) {
            const size_t maxBits = bits[0].size() - bitSkip;
            const size_t maxDec = pathA.size() - skipA;
            n = maxBits / 8;
            if (n > maxDec) n = maxDec;
            if (n > (static_cast<size_t>(1) << 17)) n = static_cast<size_t>(1) << 17;
        }
        if (n > 4096) {
            std::printf("\n  取 %zu 个解码器样本做传递函数比较（可用上限内）\n", n);
            std::vector<double> rawAt(bits[0].begin() + static_cast<long>(bitSkip),
                                      bits[0].begin() + static_cast<long>(bitSkip + n * 8));
            std::vector<double> decAt(pathA.begin() + static_cast<long>(skipA),
                                      pathA.begin() + static_cast<long>(skipA + n));
            decoderTransfer(rawAt, decAt, inRate, inRate / 8);
        } else {
            std::printf("\n  可用数据不足，跳过传递函数比较\n");
        }
    }

    std::printf("\n== 两条路径，同一中间速率 %.0f Hz ==\n", inRateA);
    double refA = 0, refB = 0, t = 0;
    bandRms(pathA2, inRateA, 1000.0, 15000.0, &refA);
    bandRms(pathB, outRateB, 1000.0, 15000.0, &refB);

    // 归一化到各自的可听内容，让两个 dBFS 尺度可比。
    std::vector<double> a = pathA2, b = pathB;
    const double gA = refA > -200.0 ? std::pow(10.0, -refA / 20.0) : 1.0;
    const double gB = refB > -200.0 ? std::pow(10.0, -refB / 20.0) : 1.0;
    for (double& v : a) v *= gA;
    for (double& v : b) v *= gB;

    reportAbs("路径 A  两级串联（FFmpeg 解码器 ÷8 + 我的 ÷2）", pathA2, inRateA);
    reportAbs("路径 C  两级串联（我的 ÷8 + 我的 ÷2）", pathC, outRateB);
    for (int d = 0; d < 2; ++d) reportAbs(designs[d].label, pathBV[d], outRateB);
    for (int d = 0; d < 2; ++d) {
        std::vector<double> v = pathBV[d];
        double ref = 0;
        bandRms(v, outRateB, 1000.0, 15000.0, &ref);
        if (ref > -200.0) {
            const double g = std::pow(10.0, -ref / 20.0);
            for (double& s : v) s *= g;
        }
        report(designs[d].label, v, outRateB);
    }

    std::printf("\n== 结论 ==\n");
    {
        double aAudio = 0, a40 = 0, a80 = 0, c40 = 0, c80 = 0;
        bandRms(pathA2, inRateA, 1000.0, 15000.0, &aAudio);
        bandRms(pathA2, inRateA, 40000.0, 80000.0, &a40);
        bandRms(pathA2, inRateA, 20000.0, 40000.0, &a80);
        bandRms(pathC, outRateB, 40000.0, 80000.0, &c40);
        bandRms(pathC, outRateB, 20000.0, 40000.0, &c80);

        // 40-80 kHz 是**唯一**两边都不含糊的量：那里的内容是 DSD 噪声，不是信号
        //（DSD64 的音频上限是 22.05 kHz），所以路径 C 把它压低一定是好事。
        std::printf("  40-80 kHz  现有路径 %+.2f dB（相对可听）  我的级联 %+.2f dB  -> %+.2f dB\n",
                    a40 - aAudio, c40 - (-47.34), (c40 - (-47.34)) - (a40 - aAudio));
        {
            char bb[128];
            std::snprintf(bb, sizeof(bb), "C 比现有路径低 %.2f dB（绝对 %.2f vs %.2f dBFS）",
                          a40 - c40, c40, a40);
            check(c40 < a40 - 20.0, "级联把 40-80 kHz 的超声压低 20 dB 以上", bb);
        }

        // 20-40 kHz **不作为判据**。
        //
        // 实测级联在这一带比现有路径高 40 dB，与设计矛盾：级联的通带（20 kHz）比现有
        // 路径最后一级（24 kHz）更窄，放行的更少，测出来却更多。
        //
        // 解码器的传递函数给出了解释方向：它在 15-30 kHz 有约 18-24 dB 的衰减，而我
        // 的设计在 22.05 kHz 以下不衰减。所以现有路径更低的那个数，来自解码器在信号带
        // 附近就已经压过的 DSD 噪声。
        //
        // 但那**不能**当成「我的设计更好」：15 kHz 处 −23.96 dB 也同样适用于那里存在的
        // 可听内容，所以这个衰减到底是压噪声还是压信号，本次没有分开。所以这一带只报告，
        // 不判定。
        std::printf(
            "\n  未解决：20-40 kHz 上我的级联比现有路径高 %.2f dB。\n"
            "  解码器在这一带有约 18-24 dB 衰减（15 kHz 处最大），而我的设计在 22.05 kHz\n"
            "  以下不衰减。那个衰减是在压DSD 噪声还是在压可听内容，本次没有分开测量，\n"
            "  所以不把它算成任何一方更好。\n",
            c80 - a80);
    }

    std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    return failures == 0 ? 0 : 1;
}