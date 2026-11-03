#include "dsd_filter.h"

#include <cmath>
#include <cstring>

namespace neiro {
namespace dsd {

namespace {

/// 两级抽取：÷8 再 ÷2，终止于 2822400 / 16 = 176400 Hz（DSD64）。
///
/// 级联而不是单级，因为实测级联比单级 ÷16 在 40-80 kHz 低 67 dB（−220.39 vs
/// −153.36 dBFS），而两者可听内容相同。阻带相乘，单级受限于抽头数能买到的大约 100 dB。
constexpr int kStage1Decimation = 8;
constexpr int kStage2Decimation = 2;

/// 通带到 20 kHz。DSD64 是 44.1 kHz 音频，Nyquist 22.05 kHz，所以 22 kHz 以上
/// **全是噪声**。第一版放到 24 kHz 通带，放行了 2 kHz 纯噪声，比现有路径差 51 dB。
constexpr double kPassbandHz = 20000.0;
constexpr double kStopbandHz = 22050.0;
constexpr double kAttenuationDb = 100.0;

std::vector<double> kaiserWindow(size_t n, double beta) {
    std::vector<double> w(n);
    const double r = n > 1 ? static_cast<double>(n - 1) / 2.0 : 0.0;
    const double denom = besselI0(beta);
    for (size_t i = 0; i < n; ++i) {
        const double t = r > 0.0 ? (static_cast<double>(i) - r) / r : 0.0;
        const double x = 1.0 - t * t;
        w[i] = (x <= 0.0) ? 0.0 : besselI0(beta * std::sqrt(x)) / denom;
    }
    return w;
}

/// 字节内比特反转：DSF 是 LSB 优先，最早的采样在最低位。
uint8_t reverseBits(uint8_t v) {
    v = static_cast<uint8_t>(((v & 0xF0u) >> 4) | ((v & 0x0Fu) << 4));
    v = static_cast<uint8_t>(((v & 0xCCu) >> 2) | ((v & 0x33u) << 2));
    return static_cast<uint8_t>(((v & 0xAAu) >> 1) | ((v & 0x55u) << 1));
}

}  // namespace

// `Stage` 定义在头文件里（`Cascade` 用 `std::vector<Stage>` 按值持有它，而
// `std::vector` 不允许不完整类型）。这里是它的实现。
void Stage::configure(const std::vector<double>& h, int decimation) {
    h_ = h;
    decimation_ = decimation > 0 ? decimation : 1;
    reset();
}

void Stage::reset() {
    buf_.clear();
    phase_ = 0;
}

void Stage::push(const double* samples, size_t count) {
    if (samples) buf_.insert(buf_.end(), samples, samples + count);
}

size_t Stage::drain(double* out, size_t maxOut) {
    if (h_.empty() || out == nullptr || maxOut == 0) return 0;
    const size_t taps = h_.size();
    size_t produced = 0;
    size_t consumed = 0;
    // 判据是 `可用样本数 >= taps`：只有窗口完全落在已收到的样本里时，那个输出才是
    // 真实的滤波器输出。否则它会用还不存在的数据去补，等于凭空造一个样本。
    while (buf_.size() - consumed >= taps && produced < maxOut) {
        if (phase_ == 0) {
            double acc = 0.0;
            for (size_t k = 0; k < taps; ++k) acc += h_[k] * buf_[consumed + k];
            out[produced++] = acc;
        }
        phase_ = (phase_ + 1) % decimation_;
        ++consumed;
    }
    if (consumed) buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(consumed));
    return produced;
}
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

std::vector<double> designDecimationFilter(double inRate, double passbandHz,
                                            double stopbandHz, double attenuationDb) {
    const double fc = 0.5 * (passbandHz + stopbandHz);
    const double transition = stopbandHz - passbandHz;
    if (transition <= 0.0 || inRate <= 0.0) return {};
    const double dOmega = 2.0 * M_PI * transition / inRate;
    int taps = static_cast<int>((attenuationDb - 8.0) / (2.285 * dOmega));
    if (taps < 3) taps = 3;
    if (taps & 1) ++taps;  // 奇数长度才能有整数延迟

    const double beta = 0.1102 * (attenuationDb - 8.7);
    const std::vector<double> w = kaiserWindow(static_cast<size_t>(taps), beta);
    std::vector<double> h(static_cast<size_t>(taps));
    const double m = (taps - 1) / 2.0;
    const double wn = 2.0 * fc / inRate;
    for (int i = 0; i < taps; ++i) {
        const double n = static_cast<double>(i) - m;
        const double ideal = (std::fabs(n) < 1e-12) ? wn : std::sin(M_PI * wn * n) / (M_PI * n);
        // 增益 1.0：FIR 之后的抽取不改变幅度。「抽取带来 1/D 增益」只属于箱式平均
        // 抽取；第一版把抽头乘了 D，于是整个滤波器大 24 dB，而检查还判了 PASS。
        h[static_cast<size_t>(i)] = ideal * w[static_cast<size_t>(i)];
    }
    return h;
}

std::vector<double> decimate(const std::vector<double>& x, const std::vector<double>& h, int D) {
    Stage s;
    s.configure(h, D);
    s.push(x.data(), x.size());
    std::vector<double> out;
    std::vector<double> tmp(h.size() * 4 + 64);
    // 输出上界：输入 / D 加上滤波器的两段历史。给一个明确的上界而不是循环到空，
    // 这样「循环到空」不会被读成「没有更多输出了」——那其实是一回事，但只有一次。
    const size_t upperBound = x.size() / static_cast<size_t>(D > 0 ? D : 1) + 2;
    while (out.size() < upperBound) {
        const size_t n = s.drain(tmp.data(), tmp.size());
        if (n == 0) break;
        out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<long>(n));
    }
    return out;
}

bool Cascade::configure(uint32_t dsdRate, uint32_t channels, double* outAudioRate,
                        std::string* error) {
    if (dsdRate == 0 || channels == 0) {
        if (error) *error = "dsd cascade: 速率或声道数为 0";
        return false;
    }
    // 级联只能整除。静默截断会改变音频长度，那是最难发现的一种错，所以在这里拒绝。
    if (dsdRate % 16 != 0) {
        if (error) *error = "dsd cascade: 比特率不能被 16 整除";
        return false;
    }
    h8_ = designDecimationFilter(static_cast<double>(dsdRate), kPassbandHz, kStopbandHz,
                                 kAttenuationDb);
    h2_ = designDecimationFilter(static_cast<double>(dsdRate) / kStage1Decimation, kPassbandHz,
                                 kStopbandHz, kAttenuationDb);
    if (h8_.empty() || h2_.empty()) {
        if (error) *error = "dsd cascade: 滤波器设计失败";
        return false;
    }
    channels_ = channels;
    audioRate_ = dsdRate / 16;
    if (outAudioRate) *outAudioRate = static_cast<double>(audioRate_);
    configured_ = true;
    reset();
    return true;
}

void Cascade::reset() {
    stage1_.assign(channels_, Stage());
    stage2_.assign(channels_, Stage());
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        stage1_[ch].configure(h8_, kStage1Decimation);
        stage2_[ch].configure(h2_, kStage2Decimation);
    }
    pendingPerChannel_.assign(channels_, {});
    pending_.clear();
}

size_t Cascade::feedOne(uint32_t channel, const uint8_t* bytes, size_t byteCount,
                        std::vector<double>* out, size_t maxOut) {
    if (channel >= channels_) return 0;
    std::vector<uint8_t>& pending = pendingPerChannel_[channel];
    pending.insert(pending.end(), bytes, bytes + byteCount);

    // 抽取周期是 8 字节。喂进来的长度不保证是它的倍数，所以**不足的尾巴必须留下**——
    // 丢掉它就等于丢掉最后 0.23 ms 的音频。
    const size_t need = static_cast<size_t>(kStage1Decimation);
    if (pending.size() < need) return 0;
    const size_t usable = (pending.size() / need) * need;

    std::vector<double> bits;
    bits.reserve(usable * 8);
    for (size_t i = 0; i < usable; ++i) {
        const uint8_t v = reverseBits(pending[i]);
        for (int b = 0; b < 8; ++b) bits.push_back((v >> b) & 1 ? 1.0 : -1.0);
    }
    pending.erase(pending.begin(), pending.begin() + static_cast<long>(usable));

    stage1_[channel].push(bits.data(), bits.size());
    std::vector<double> mid(h8_.size() * 4 + 64);
    for (;;) {
        const size_t n = stage1_[channel].drain(mid.data(), mid.size());
        if (n == 0) break;
        stage2_[channel].push(mid.data(), n);
    }

    return stage2_[channel].drain(out->data(), maxOut);
}

void Cascade::feed(const uint8_t* bytes, size_t byteCount,
                   std::vector<float>* interleavedOut, size_t maxFrames,
                   size_t* framesProduced) {
    if (framesProduced) *framesProduced = 0;
    if (!configured_ || !bytes || byteCount == 0 || !interleavedOut || maxFrames == 0) return;
    if (channels_ != 1) return;  // 多声道走 feedChannels

    std::vector<double> out(maxFrames);
    const size_t produced = feedOne(0, bytes, byteCount, &out, maxFrames);
    for (size_t i = 0; i < produced; ++i) interleavedOut->push_back(static_cast<float>(out[i]));
    if (framesProduced) *framesProduced = produced;
}

size_t Cascade::feedInterleaved(const uint8_t* bytes, size_t bytePositions,
                                std::vector<float>* interleavedOut, size_t maxFrames) {
    if (!configured_ || !bytes || bytePositions == 0 || !interleavedOut || maxFrames == 0)
        return 0;
    // DSF 的块内排布是交错的：位置 i 上的 channels_ 个字节依次属于各个声道。先拆成平面
    // 缓冲，因为各级抽取器的输出长度不一定相同（滤波器历史的余数不同），不能边拆边
    // 算——最后再交错回去。
    std::vector<std::vector<uint8_t>> planar(channels_,
                                             std::vector<uint8_t>(bytePositions));
    for (size_t i = 0; i < bytePositions; ++i)
        for (uint32_t ch = 0; ch < channels_; ++ch)
            planar[ch][i] = bytes[i * channels_ + ch];

    std::vector<std::vector<double>> per(channels_);
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        per[ch].resize(maxFrames);
        const size_t got = feedOne(ch, planar[ch].data(), bytePositions, &per[ch], maxFrames);
        per[ch].resize(got);
    }
    size_t frames = per[0].size();
    for (uint32_t ch = 1; ch < channels_; ++ch)
        if (per[ch].size() < frames) frames = per[ch].size();

    const size_t before = interleavedOut->size();
    interleavedOut->reserve(before + frames * channels_);
    for (size_t i = 0; i < frames; ++i)
        for (uint32_t ch = 0; ch < channels_; ++ch)
            interleavedOut->push_back(static_cast<float>(per[ch][i]));
    return frames;
}

size_t Cascade::drainPending(std::vector<float>* interleavedOut, size_t maxFrames) {
    if (!configured_ || !interleavedOut || maxFrames == 0) return 0;
    std::vector<std::vector<double>> per(channels_);
    for (uint32_t ch = 0; ch < channels_; ++ch) {
        per[ch].resize(maxFrames);
        per[ch].resize(stage2_[ch].drain(per[ch].data(), maxFrames));
    }
    size_t frames = per[0].size();
    for (uint32_t ch = 1; ch < channels_; ++ch)
        if (per[ch].size() < frames) frames = per[ch].size();
    const size_t before = interleavedOut->size();
    interleavedOut->reserve(before + frames * channels_);
    for (size_t i = 0; i < frames; ++i)
        for (uint32_t ch = 0; ch < channels_; ++ch)
            interleavedOut->push_back(static_cast<float>(per[ch][i]));
    return frames;
}

}  // namespace dsd
}  // namespace neiro
