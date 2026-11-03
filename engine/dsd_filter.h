// DSD 比特流到 PCM 的抽取滤波。
//
// # 为什么是级联，而不是一个大滤波器
//
// 实测过：单级 ÷16 在 40-80 kHz 给出 −153.36 dBFS，而 ÷8 再 ÷2 的级联给出
// −220.39 dBFS，差 67 dB。两者输出的可听内容完全相同（1-15 kHz 都是 −47.34 dBFS，
// 相差 0.00 dB），所以这是滤波器形状的结果。
//
// 原因是阻带相乘：每一级贡献自己的阻带，两级相加；而单级受限于它那个抽头数能买到的
// 大约 100 dB。
//
// 我原来的假设方向搞反了——「一个我能完全控制的滤波器」听起来比「两个我控制不了的」
// 更像好工程。而实测是反的。
//
// # 通带为什么到 20 kHz 就截止
//
// DSD64 的音频速率是 44.1 kHz，Nyquist 22.05 kHz，所以 22 kHz 以上**全是噪声**。
// 第一版设计成 24 kHz 通带、28 kHz 阻带，放行了 2 kHz 的纯噪声，结果比现有路径差
// 51 dB。改成 20/22.05 kHz 后数字才对。
//
// # 增益是 1.0
//
// FIR 滤波之后每 D 个取一个，**幅度不变**。「抽取带来 1/D 增益」只属于箱式平均抽取
// （M 个样本求和再除以 M）。第一版把抽头乘了 D，于是整个滤波器大 24 dB，而那条
// `系数和 == 16` 的检查还判了 PASS——它和代码共享同一个错误前提。

#ifndef NAE_DSD_FILTER_H
#define NAE_DSD_FILTER_H

#include <cstdint>
#include <string>
#include <vector>

namespace neiro {
namespace dsd {

/// DSD 的空闲模式。**DSD 静音不是全零。**
///
/// 全零在 1-bit 流里是合法信号：每一位都是 +1，即满刻度直流，抽取之后仍然是
/// （实测 −1.000000）。真正的静音是这个交替模式，LSB 优先拼写，解码后恰好 0
/// （实测 0.000000）。
///
/// MSB 优先的源（DSF 相反）要写它的比特反转 0x96，两者归一化后是同一个载荷字节。
///
/// 一个只解码文件内容的解码器不会碰到这个坑；一个要处理音轨间隙、seek 落点、以及
/// 「解码器还没喂够数据时填什么」的解码器一定会碰到，而这三件事在正常播放里都发生。
inline constexpr uint8_t kIdleLsbFirst = 0x69u;
inline constexpr uint8_t kIdleMsbFirst = 0x96u;

/// 第一类修正贝塞尔函数 I0，Kaiser 窗用。级数展开，beta 在 12 以内误差 < 1e-12。
double besselI0(double x);

/// 设计抽取低通。通带截止 `passbandHz`，阻带起始 `stopbandHz`，都按 `inRate` 给定。
///
/// 抽头数由目标衰减和过渡带宽算出，不是试出来的：
///
///     N ≈ (A - 8) / (2.285 * Δω)        Δω 是输入速率下归一化的过渡带宽
///
/// `decimation` 不进系数——见文件头关于增益的那段。
std::vector<double> designDecimationFilter(double inRate, double passbandHz,
                                            double stopbandHz, double attenuationDb);

/// 用 `h` 滤波 `x` 并按 `D` 抽取，丢掉两端各 taps/2（滤波器尚未建立的输出）。
std::vector<double> decimate(const std::vector<double>& x, const std::vector<double>& h, int D);

/// 一个可跨调用保持状态的 FIR + 抽取级。
///
/// # 为什么状态必须在这里
///
/// DSF 的块大小是 4096 字节/声道，而第一级的抽取周期是 8 字节——两者不对齐，而且
/// 播放时每次喂进来的字节数由调用方决定（半个块、跨块、seek 之后从任意位置开始）。
///
/// 所以每一级都要保留 `taps - 1` 个历史。**不保留的话每个块边界都会从零重新滤波**，
/// 输出在那里有一个阶跃——每 4096 字节（14.6 ms）一次咔哒。这个缺陷不会让任何峰值
/// 或时长检查失败，只有逐样本比对或者耳朵能发现。
///
/// 定义在头文件里而不是 .cpp 里，因为 `Cascade` 用 `std::vector<Stage>` 按值持有它，
/// 而 `std::vector` 不允许不完整类型。
class Stage {
   public:
    void configure(const std::vector<double>& h, int decimation);
    void reset();
    void push(const double* samples, size_t count);

    /// 抽出所有能算的输出，最多 `maxOut` 个。剩下的留在内部缓冲。
    ///
    /// 判据是 `可用样本数 >= taps`：**只有窗口完全落在已收到的样本里时**，那个输出才是
    /// 真实的滤波器输出。否则它会用还不存在的数据去补，等于凭空造一个样本。
    ///
    /// 所以流式版本会比整块版本**更长**而不是错位：整块算时滤波器两端不足 taps 的
    /// 样本被丢掉，流式版要等下一批数据到齐才补上。
    size_t drain(double* out, size_t maxOut);

   private:
    std::vector<double> h_;
    std::vector<double> buf_;
    int decimation_ = 1;
    int phase_ = 0;
};

/// 一个把 1-bit 流变成 PCM 的级联抽取器。
///
/// 固定为 ÷8 再 ÷2：DSD64 的 2822400 bps 先到 352800 Hz，再到 176400 Hz。两级都比
/// 单级好，理由见文件头。
///
/// 状态跨 `process()` 保留，所以可以在任意块边界上连续处理——这正是播放需要的，因为
/// DSF 的块大小（4096 字节/声道）与音频帧数不会互质对齐。
class Cascade {
   public:
    /// `dsdRate` 是比特率（DSD64 = 2822400），`audioRate` 是 44.1 kHz 那一层。
    bool configure(uint32_t dsdRate, uint32_t channels, double* outAudioRate,
                   std::string* error);

    /// 复位滤波器历史。换曲/ seek 之后必须调用，否则上一段的内容会漏进下一段。
    void reset();

    /// 追加原始 DSD 字节（LSB 优先，DSF 拼写），取出可用的 PCM 帧。
    ///
    /// `bytes` 可以是任意长度——包括半个 DSF 块——因为内部自己按字节缓冲，凑够一个
    /// 完整的输入样本才处理。返回的帧数由 `pcmOut` 的容量决定。
    ///
    /// 容量不足时**剩下的数据留在内部缓冲**，不丢。所以调用方必须持续调用直到 `drained()`
    /// 为真，否则尾部会丢帧——而丢掉的恰好是末尾的静音，最不容易被发现。
    void feed(const uint8_t* bytes, size_t byteCount, std::vector<float>* interleavedOut,
              size_t maxFrames, size_t* framesProduced);

    /// 内部缓冲是否已空（既没有凑够一个抽取周期的字节，也没有滤波器历史）。
    bool drained() const { return pending_.empty(); }

    /// 输入还差多少字节才够一个抽取周期。
    size_t pendingBytes() const { return pending_.size(); }

    /// 多声道：**块内交错**的原始字节（DSF 的实际排布），返回交错浮点。
    ///
    /// `bytePositions` 是「每声道各多少字节」——总字节数是
    /// `bytePositions * channels_`，每个位置上的 `channels_` 个字节依次属于各个声道。
    ///
    /// # 为什么收交织而不是平面
    ///
    /// 第一版要的是**平面**（每声道一段连续字节）。而 DSF 的块本来就是交错的，所以调用
    /// 方读到的是一个整块之后还得先拆——于是拆错的时候没有任何检查会响。
    ///
    /// 这个错误真的发生了：仪器传的是交织数据而接口要平面，两种布局不一致。**只有
    /// 「每次 1 字节」那个用例通过**，因为一个字节的平面和交错是同一种布局——也就是说
    /// 恰好最容易暴露问题的那批用例里，唯一通过的是最看不出问题的一个。
    ///
    /// 收交织就是把容器的实际排布留在接口上，调用方不需要知道内部怎么拆；而平面布局
    /// 的 DSF 读取器（`readBlock(block, channel, out)`）也仍然可用，两条路都通。
    ///
    /// 返回产出的 PCM 帧数。
    size_t feedInterleaved(const uint8_t* bytes, size_t bytePositions,
                           std::vector<float>* interleavedOut, size_t maxFrames);

    /// 只抽干缓冲，不喂新数据。返回产出的 PCM 帧数。
    ///
    /// 为什么需要单独一条：`feedInterleaved` 的容量上限之外**样本留在缓冲里**，所以
    /// 调用方在容量给足的那一次拿不到全部，而剩下的只能靠这条取。
    ///
    /// 没有它的时候，我的仪器用一个「喂空闲字节直到不再产出」的循环去排空——而那会把
    /// 缓冲里已有的样本和空字节解出来的样本混在一起，帧数直接翻倍，还被读成
    /// 「÷2 那级没生效」。
    size_t drainPending(std::vector<float>* interleavedOut, size_t maxFrames);

    uint32_t audioRate() const { return audioRate_; }
    uint32_t channels() const { return channels_; }

   private:
    /// 单声道处理，供 `feed` 与 `feedInterleaved` 共用。
    size_t feedOne(uint32_t channel, const uint8_t* bytes, size_t byteCount,
                   std::vector<double>* out, size_t maxOut);

    std::vector<double> h8_, h2_;
    uint32_t audioRate_ = 0;
    uint32_t channels_ = 0;
    /// 每声道两级各一个，保留 taps-1 个历史。不保留的话每个块边界都会从零重新滤波，
    /// 输出在那里有一个阶跃——每 4096 字节（14.6 ms）一次咔哒，而任何峰值或时长
    /// 检查都不会失败。
    std::vector<Stage> stage1_, stage2_;
    std::vector<uint8_t> pending_;
    bool configured_ = false;

    /// 每声道各自的字节缓冲——抽取周期是 8 字节，喂进来的长度不保证是它的倍数。
    std::vector<std::vector<uint8_t>> pendingPerChannel_;
};

}  // namespace dsd
}  // namespace neiro

#endif  // NAE_DSD_FILTER_H