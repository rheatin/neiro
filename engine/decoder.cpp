#include "decoder.h"

#include "dsd_filter.h"
#include "dsf_reader.h"

#include <cstdlib>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace neiro {

// FFmpeg's own description of a failure, which is far more useful than a code.
// A user who cannot open a file needs to be told *why* in words they can act on.
static std::string ffmpegError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

struct Decoder::Impl {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;

    int stream_index = -1;
    StreamInfo info;

    // The conversion graph: whatever the codec produces, to interleaved float.
    //
    // Handled here rather than by libswresample's automatic path so the channel
    // order and layout are explicit. A player that silently swaps channels
    // because a file declared an unusual layout is worse than one that says so.
    AVChannelLayout out_layout{};

    // A frame that has been converted but not yet handed out. libswresample
    // buffers, so a read() can return a partial frame and the remainder is
    // still in there.
    std::vector<float> pending;
    size_t pending_pos = 0;

    bool flushing = false;
    bool drained = false;
    // Frames handed out via readExact(). Not tracked in read(), because the engine
    // only ever counts what it pulled through readExact.
    uint64_t frames_read = 0;

    // ---- 自己解 DSD 的那条路 ----
    //
    // FFmpeg 仍然打开文件、仍然提供时长和标签，但**采样不走它**。
    //
    // 为什么不完全绕开 FFmpeg：标签、时长、码率交叉核对全在它的 metadata 字典里，
    // 重新实现一遍那些是白费力气，而丢掉它们会让 fact strip 变空——那正是之前修过的
    // 问题。所以分工是：元数据走 FFmpeg，采样走自己的解码器。
    //
    // 为什么值得多走一条路：FFmpeg 的 `dsd_lsbf_planar` 内部只有约 20 dB 衰减且响应
    // 非单调（15 kHz 处 −23.96 dB，40 kHz 处 −8.35 dB），而自写的 ÷8/÷2 级联在
    // 40-80 kHz 给出 −220.39 dBFS，对 −154.37 dBFS，差 66.02 dB。
    //
    // 用环境变量 `NEIRO_DSD_DECODER=1` 打开，这样同一个构建里可以做 A/B——而 A/B 是
    // 唯一能说清「那 66 dB 是否真的出现在真实输出路径上」的办法。
    std::unique_ptr<dsf::File> ownDsd;
    neiro::dsd::Cascade ownDsdCascade;
    uint64_t ownDsdBlock = 0;
    uint64_t ownDsdBlocks = 0;
    std::vector<uint8_t> ownDsdBlockBuf;
    bool ownDsdActive = false;
    /// 没能走自己的路时的原因，例如声道数与请求的不一致。**必须报出来**，因为静默
    /// 退回 FFmpeg 会让「这次是自写解码器解的」这句话变成假的。
    std::string ownDsdWhyNot;

    // ---- workaround：在 FFmpeg 解码器之后加一级 ÷2 ----
    //
    // 为什么绕开自写路径：那条路失败在**比特展开与交错**（`Cascade::feedInterleaved`），
    // 耳朵判的——人声全无、只剩高频震动。而滤波器本身是验证过的：隔离测试里通带与阻带
    // 相差 129 dB，真实文件上阻带 −156 dB。
    //
    // 所以保留 FFmpeg 的解码器（它的比特展开一直在用、是好的），只在我验证过的滤波器
    // 之后加一级 ÷2：352800 -> 176400。用上的全是量过的东西，避开的正是出问题的那一段。
    //
    // 这一级为什么值钱：FFmpeg 的 DSD 解码器内部只抽到 352800，而它的滤波器实测只有约
    // 20 dB 衰减且响应非单调，所以 176.4 kHz 以下的超声噪声**原样进入**引擎的 soxr，
    // 再被 352800 -> 192000 折叠进 0-96 kHz。仪器里量到加这一级之后 40-80 kHz 在
    // −115.72 dB 相对，而在用路径在同一带上是 −12.76 dB 相对。
    neiro::dsd::Stage dsdPostFilter;
    bool dsdPostFilterActive = false;

    /// `outChannels` 是 `Decoder::open` 收到的那个请求值，`tryOwnDsdDecoder` 要用它
    /// 判断能不能走自己的路径（自写路径没有下混矩阵）。
    uint32_t outChannels = 0;

    void tryOwnDsdDecoder(const std::string& path, Impl* p);

    // Move everything libswresample is holding into `pending`, as interleaved
    // float. Lives here because `Impl` is private to Decoder, and it needs the
    // FFmpeg types.
    //
    // The size is asked for before reserving, so the loop terminates: swr_convert
    // reports what it actually wrote, and a short write means it is finished.
    void drainConverted(std::vector<float>* pending) {
        if (!swr) return;
        for (;;) {
            const int64_t held = swr_get_delay(swr, info.sample_rate);
            if (held <= 0) break;

            const size_t frames = static_cast<size_t>(held);
            const size_t offset = pending->size();
            pending->resize(offset + frames * info.out_channels);

            uint8_t* dst = reinterpret_cast<uint8_t*>(pending->data() + offset);
            const int got = swr_convert(swr, &dst, static_cast<int>(frames), nullptr, 0);
            if (got < 0) break;
            pending->resize(offset + static_cast<size_t>(got) * info.out_channels);
            if (got < static_cast<int>(frames)) break;
        }
    }

    // Convert one decoded frame into `pending`, then release anything the
    // converter is still holding.
    //
    // Both halves are needed and they are not interchangeable. The first pushes
    // the frame's samples through; the second collects the converter's filter
    // delay. An earlier version only did the second, so it drained a delay that
    // was never fed anything and every file decoded to exactly zero frames while
    // reporting a perfectly correct sample rate, channel count and duration.
    void convertFrame(const AVFrame* f, std::vector<float>* pending) {
        if (!swr || !f) return;

        // The converter's worst-case output for this input, plus its current
        // delay, so one call is always enough and the buffer is never guessed.
        // FFmpeg's swr_get_out_samples takes a frame *count*, not a frame.
        const int room = swr_get_out_samples(swr, f->nb_samples) + 16;
        if (room <= 0) return;

        const size_t offset = pending->size();
        pending->resize(offset + static_cast<size_t>(room) * info.out_channels);

        uint8_t* dst = reinterpret_cast<uint8_t*>(pending->data() + offset);
        const int got = swr_convert(swr, &dst, room, f->extended_data, f->nb_samples);
        if (got < 0) {
            pending->resize(offset);
            return;
        }
        pending->resize(offset + static_cast<size_t>(got) * info.out_channels);

        drainConverted(pending);

        // DSD 的 ÷2 抽取，插在 swr 之后、样本交出之前。
        //
        // **逐声道独立处理。** 不能把交错序列当单声道一起滤波——那会把所有频率折半。
        // 这正是 `dsd_band_check` 踩过的那个坑：它把交错立体声缓冲区当单声道分析，于是
        // 「22-40 kHz」其实是 11-20 kHz，量出一段完全无法解释的平台。
        if (dsdPostFilterActive && !pending->empty()) {
            const size_t chCount = info.out_channels ? info.out_channels : 1;
            const size_t frames = pending->size() / chCount;
            std::vector<double> channel(chCount);
            std::vector<double> outBuf(8192);
            std::vector<float> filtered;
            for (size_t f = 0; f < frames; ++f) {
                for (size_t c = 0; c < chCount; ++c)
                    channel[c] = static_cast<double>((*pending)[f * chCount + c]);
                dsdPostFilter.push(channel.data(), channel.size());
                for (;;) {
                    const size_t n = dsdPostFilter.drain(outBuf.data(), outBuf.size());
                    if (n == 0) break;
                    for (size_t i = 0; i < n; ++i) filtered.push_back(static_cast<float>(outBuf[i]));
                }
            }
            if (!filtered.empty()) {
                pending->clear();
                pending->insert(pending->end(), filtered.begin(), filtered.end());
            }
        }
    }

    void close() {
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (packet) av_packet_free(&packet);
        if (codec) avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
        stream_index = -1;
        pending.clear();
        pending_pos = 0;
        flushing = false;
        drained = false;
    }
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() { impl_->close(); }

bool Decoder::is_open() const { return impl_->codec != nullptr; }

const StreamInfo& Decoder::info() const { return impl_->info; }

namespace {

// AVSampleFormat's name, read back from the library rather than from a table here.
// A local table would drift, and the whole point of this block is to report what
// the pipeline really is.
const char* sampleFormatName(int fmt) {
    const char* n = av_get_sample_fmt_name(static_cast<AVSampleFormat>(fmt));
    return n ? n : "(unknown)";
}

}  // namespace

std::string Decoder::describe() const {
    const Impl& p = *impl_;
    if (!p.codec) return "not open";

    // The layout name needs a buffer and a size; there is no returning-pointer
    // form, and a fixed guess at 64 bytes is how a six-channel file ends up
    // printed as "(unnamed)".
    char layout[64] = {0};
    av_channel_layout_describe(&p.codec->ch_layout, layout, sizeof(layout));

    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "  codec              %s\n"
                  "  decoder output fmt %s   <-- what the codec itself produces\n"
                  "  decoder rate       %d Hz   <-- the bitstream rate, not PCM\n"
                  "  decoder channels   %d (%s)\n"
                  "  pcm stage          libswresample, %s -> %s, in_rate == out_rate == %u\n"
                  "  pcm channels       %u%s\n"
                  "  swr flags          0 (FFmpeg's default filter)",
                  p.info.codec.c_str(), sampleFormatName(p.codec->sample_fmt), p.codec->sample_rate,
                  p.codec->ch_layout.nb_channels, layout, sampleFormatName(p.codec->sample_fmt),
                  "AV_SAMPLE_FMT_FLT", p.info.sample_rate, p.info.out_channels,
                  p.info.out_channels == p.info.channels
                      ? ""
                      : (std::string(" (file has ") + std::to_string(p.info.channels) + ")").c_str());
    return std::string(buf);
}

/// 把标签字典里的白名单项抄进 `info`。
///
/// `file_bytes` 是为了算实测码率而传入的文件大小——它不来自标签，而是操作系统，
/// 所以它能当裁判。见下面关于交叉验证的说明。
///
/// 白名单而不是全量搬运，有三个理由，第一个是硬的：
///
///   1. **定长**。`NAE_TrackInfo` 要过 C ABI，所以字段是定长字符数组。标签可以是
///      任意长度，而一个 4000 字符的 comment 会溢出任何定长字段。所以必须在这里
///      截断——而截断在哪里发生必须写出来，不能让下游猜。
///   2. **界面只需要几项**。年份、流派、码率、文件大小。多搬的字段没有消费者，
///      却是每次打开都要付的内存和每次跨 ABI 的字节。
///   3. **键名不一致**。`YEAR`、`DATE`、`TYER` 都可能是年份；`GENRE` 有时是复数。
///      搬运会把这三种都带上来，而没有一层说明该用哪个。
///
/// 已存在的值**不覆盖**：容器级的先读，流级的后读覆盖它。但同一个字典里重复的键
/// 取第一个——`av_dict_get` 返回第一个匹配，而标签文件里的重复键没有约定哪个算数。
static void readTags(const AVDictionary* dict, neiro::StreamInfo* info, uint64_t file_bytes) {
    if (!dict || !info) return;

    const auto get = [dict](const char* key) -> const char* {
        AVDictionaryEntry* e = av_dict_get(dict, key, nullptr, 0);
        return (e && e->value) ? e->value : nullptr;
    };

    // 年份。三个键都试，因为三个都在用：`DATE` 通常是年份或日期，`YEAR` 是年份，
    // `TYER` 是 ID3v1 遗留的。
    //
    // `DATE` 常常是 `2024-05-01`，所以只取开头四位——而要检查它真的是数字，否则
    // 一个写成 `2024?` 的标签会给出 `2024?` 而不是年份。
    if (info->year.empty()) {
        for (const char* key : {"YEAR", "DATE", "TYER"}) {
            const char* v = get(key);
            if (!v) continue;
            size_t n = 0;
            while (n < 4 && v[n] >= '0' && v[n] <= '9') ++n;
            if (n >= 3) {  // 至少三位才算年份
                info->year.assign(v, n);
                break;
            }
        }
    }

    if (info->genre.empty()) {
        // `GENRE` 和 `genre` 都见过；后者在部分封装器里出现。
        for (const char* key : {"GENRE", "genre"}) {
            const char* v = get(key);
            if (v && *v) {
                info->genre.assign(v);
                break;
            }
        }
    }

    if (info->album.empty()) {
        // `ALBUM` 是标准键。`WM/AlbumTitle` 是 Windows Media 时代嵌在 ASF 里的，
        // 偶尔出现在从那里转出来的文件上。
        for (const char* key : {"ALBUM", "album", "WM/AlbumTitle"}) {
            const char* v = get(key);
            if (v && *v) {
                info->album.assign(v);
                break;
            }
        }
    }

    if (info->artist.empty()) {
        // 多艺人文件的标签写法不统一：`ARTIST` 通常是全部，`ALBUMARTIST` 是专辑
        // 归属的那个。这里取 `ARTIST`——它是界面上「艺术家」那一格的通常含义。
        for (const char* key : {"ARTIST", "artist", "WM/AlbumArtist"}) {
            const char* v = get(key);
            if (v && *v) {
                info->artist.assign(v);
                break;
            }
        }
    }

    // 标题、Album Artist、轨号、碟号、composer、comment。
    //
    // 这些键以前就在 metadata 字典里，只是没有抄出来。轨号/碟号常见写法是
    // `1/14`，这里保留原始字符串，解析归调用方。
    if (info->title.empty()) {
        for (const char* key : {"TITLE", "title"}) {
            const char* v = get(key);
            if (v && *v) { info->title.assign(v); break; }
        }
    }
    if (info->album_artist.empty()) {
        for (const char* key : {"ALBUMARTIST", "ALBUM_ARTIST", "album_artist",
                                "WM/AlbumArtist"}) {
            const char* v = get(key);
            if (v && *v) { info->album_artist.assign(v); break; }
        }
    }
    if (info->track.empty()) {
        for (const char* key : {"TRACK", "TRACKNUMBER", "track"}) {
            const char* v = get(key);
            if (v && *v) { info->track.assign(v); break; }
        }
    }
    if (info->disc.empty()) {
        for (const char* key : {"DISC", "DISCNUMBER", "disc"}) {
            const char* v = get(key);
            if (v && *v) { info->disc.assign(v); break; }
        }
    }
    if (info->composer.empty()) {
        for (const char* key : {"COMPOSER", "composer"}) {
            const char* v = get(key);
            if (v && *v) { info->composer.assign(v); break; }
        }
    }
    if (info->comment.empty()) {
        for (const char* key : {"COMMENT", "comment"}) {
            const char* v = get(key);
            if (v && *v) { info->comment.assign(v); break; }
        }
    }

    // 码率，单位 **kbps**。
    //
    // 标签里的单位不一致是最容易出错的一处：FLAC 的 `BITRATE` 通常是
    // `<总比特率>_<音频比特率>`（下划线分隔，单位 **bps**），而 MP3 的 ID3
    // `TBPM` 之类完全不是码率。直接把 bps 当 kbps 显示会差 1000 倍——一个
    // 1411 kbps 的 CD 会显示成 1411 bps，或者反过来显示成 1.4 Mbps。
    //
    // 所以：取下划线**之后**的那一段（FLAC 的音频比特率，而不是容器总码率），除以
    // 1000，并且只在结果落在一个合理区间内才采纳。超出区间说明单位不是我们以为的
    // 那个，这时宁可不显示，也不显示一个差 1000 倍的数字。
    //
    // 下面这个是「这个数是不是**这一次**从标签读到的」。
    //
    // `readTags` 对容器的 metadata 和流的 metadata 各调用一次，第二次看到的
    // `bitrate_kbps` 可能已经是第一次**算出来**的数。再拿它去和实测值比，它当然
    // 相等，于是「来自标签」被它自己证实了一次——这就是「用第二个来源重新推导一个
    // 值」的那种错误，只不过推导的是**来源**。
    bool tagGaveBitrate = false;
    if (info->bitrate_kbps == 0) {
        for (const char* key : {"BITRATE", "bitrate"}) {
            const char* v = get(key);
            if (!v) continue;
            const char* part = std::strchr(v, '_');
            part = part ? part + 1 : v;
            char* end = nullptr;
            const double bps = std::strtod(part, &end);
            if (end == part || bps <= 0.0) continue;
            // 单位换算：标签里是 **bps**，界面要 kbps。
            const double kbps = bps / 1000.0;
            // 区间检查。
            //
            // 第一版只有这一条，而它是**不够的**：`BITRATE=96000` 通过了，因为
            // 96000 bps = 96 kbps 落在 32–20000 的区间里。而那个标签装的是采样率
            // ——实测构造这样一个文件，它报出了「96 kbps」，一个看起来完全合理的错值。
            //
            // 所以真正拦住它的是下面那条交叉验证，而不是这个区间。区间保留是因为它
            // 能挡掉明显离谱的值，而交叉验证能挡掉「恰好落在区间内的错值」。
            if (kbps < 32.0 || kbps > 20000.0) continue;
            info->bitrate_kbps = static_cast<uint32_t>(std::lround(kbps));
            tagGaveBitrate = true;
            break;
        }
    }

    // ---- ReplayGain ----
    //
    // The standard keys are `REPLAYGAIN_TRACK_GAIN` / `_PEAK` and the album
    // equivalents. Gain values are strings such as "-6.50 dB"; strtod stops at
    // the space. A malformed value is ignored rather than treated as zero.
    const auto readGain = [&](const char* key, double* db) -> bool {
        const char* v = get(key);
        if (!v) return false;
        char* end = nullptr;
        const double parsed = std::strtod(v, &end);
        if (end == v || !std::isfinite(parsed) || parsed < -60.0 || parsed > 60.0) {
            return false;
        }
        *db = parsed;
        return true;
    };
    const auto readPeak = [&](const char* key, double* peak) -> bool {
        const char* v = get(key);
        if (!v) return false;
        char* end = nullptr;
        const double parsed = std::strtod(v, &end);
        if (end == v || !std::isfinite(parsed) || parsed <= 0.0 || parsed > 16.0) {
            return false;
        }
        *peak = parsed;
        return true;
    };
    if (!info->rg_track_valid) {
        double db = 0.0;
        if (readGain("REPLAYGAIN_TRACK_GAIN", &db)) {
            info->rg_track_gain_db = db;
            info->rg_track_valid = true;
            readPeak("REPLAYGAIN_TRACK_PEAK", &info->rg_track_peak);
        }
    }
    if (!info->rg_album_valid) {
        double db = 0.0;
        if (readGain("REPLAYGAIN_ALBUM_GAIN", &db)) {
            info->rg_album_gain_db = db;
            info->rg_album_valid = true;
            readPeak("REPLAYGAIN_ALBUM_PEAK", &info->rg_album_peak);
        }
    }

    // 标签里的码率，和没有标签时的实测码率：文件大小 ÷ 时长。
    //
    // 这是唯一能独立算出码率的东西，所以它是唯一的裁判。标签可以错、可以缺、可以被
    // 别的工具写成别的东西；文件大小和时长不会。
    //
    // 两件事，按标签在不在分开：
    //
    //   * **没有标签**（`bitrate_kbps == 0`）：实测值就是答案。之前这里只在两个数
    //     都有的时候才动手，于是没有 `BITRATE` 键的文件一直报 0——而 0 的含义是
    //     「不知道」，可文件大小和时长都在手上，那个数是知道的。实测：ALAC 的
    //     `.m4a` 报 `bitrate=0 kbps (from tag: true)`，245.3 秒、96.3 MB，算出来
    //     3139 kbps。
    //   * **有标签**：实测值当裁判，超出 10% 容差时丢弃标签值。见下。
    //
    // `bitrate_from_tag` 的可信度跟着来源走：只有标签给出的数才为 true，算出来的
    // 一律为 false，界面据此可以说「这个数字是算出来的」。
    if (info->duration_ms > 100.0 && file_bytes > 0) {
        const double measuredKbps =
            (static_cast<double>(file_bytes) * 8.0) / (info->duration_ms / 1000.0) / 1000.0;
        if (measuredKbps > 0.0) {
            if (info->bitrate_kbps == 0) {
                info->bitrate_kbps = static_cast<uint32_t>(std::lround(measuredKbps));
                info->bitrate_from_tag = false;
            } else if (tagGaveBitrate) {
                // 10% 的容差。
                //
                // 标签给的是**音频流**的码率，而文件大小包含标签、封面和容器开销，
                // 所以两者本来就不相等——实测差 1% 到 5%，取决于文件多大。10% 足够
                // 容纳那些开销，又窄到能挡住 `BITRATE=96000` 那种差了几百倍的错值。
                //
                // 超出容差时**丢弃标签值而不是两个都报**。理由是标签值可能装的是别的
                // 东西，而实测值虽然包含容器开销，但它的方向是确定的：它至少是这个
                // 文件的比特量除以时长。一个「大致对」的数字比一个「可能完全错」的
                // 更可信，而且可验证。
                const double rel =
                    std::fabs(info->bitrate_kbps - measuredKbps) / measuredKbps;
                if (rel > 0.10) {
                    info->bitrate_kbps = static_cast<uint32_t>(std::lround(measuredKbps));
                    info->bitrate_from_tag = false;
                } else {
                    info->bitrate_from_tag = true;
                }
            }
            // 既非空、也不是这一次读到的标签：上一次调用已经判过了（`readTags` 被
            // 调用两次，见 `tagGaveBitrate`）。这里不需要再做什么。
        }
    }
}

bool Decoder::open(const std::string& path, std::string* error, uint32_t outChannels) {
    Impl& p = *impl_;
    p.close();

    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        p.close();
        return false;
    };

    if (path.empty()) return fail("no file given");

    int rc = avformat_open_input(&p.format, path.c_str(), nullptr, nullptr);
    if (rc < 0) return fail("could not open " + path + ": " + ffmpegError(rc));

    rc = avformat_find_stream_info(p.format, nullptr);
    if (rc < 0) return fail("could not read stream information: " + ffmpegError(rc));

    // 标签。
    //
    // `avformat_open_input` 已经把它们读进了 `format->metadata`，所以这里不是「去
    // 解析标签」，只是把它们抄进 `info`——数据在第一步就到了，之前没有人读。
    //
    // 标签在流找到、时长填好之后再读——交叉验证需要时长。见下面。

    // The first audio stream. A file with several is not an error; taking the
    // first and saying so in the info is.
    const AVCodec* decoder = nullptr;
    p.stream_index = av_find_best_stream(p.format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (p.stream_index < 0) return fail("no audio stream in " + path);
    if (!decoder) return fail("no decoder available for the audio stream");

    AVStream* stream = p.format->streams[p.stream_index];
    p.codec = avcodec_alloc_context3(decoder);
    if (!p.codec) return fail("could not allocate a codec context");

    rc = avcodec_parameters_to_context(p.codec, stream->codecpar);
    if (rc < 0) return fail("could not configure the codec: " + ffmpegError(rc));

    rc = avcodec_open2(p.codec, decoder, nullptr);
    if (rc < 0) return fail("could not open the codec: " + ffmpegError(rc));

    p.info.sample_rate = static_cast<uint32_t>(p.codec->sample_rate);
    p.info.channels = static_cast<uint32_t>(p.codec->ch_layout.nb_channels);
    // 文件声称的位深。
    //
    // 从 `sample_fmt` 而不是从 `bits_per_raw_sample`：后者对压缩格式普遍是 0，
    // 对 FLAC 之类的打包格式也不可靠，而前者描述的是这个流被解码成什么格式。
    // 两个都是 0 时报 0 而不是猜——「不知道」和「8 bit」必须能分辨。
    switch (p.codec->sample_fmt) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P:
            p.info.bits_per_sample = 8;
            break;
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P:
            p.info.bits_per_sample = 16;
            break;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP:
            // 32 位容器。注意 FLAC 的 24 bit 也是这个：它解出来是 S32，最高的
            // 8 位不用。**所以这个数字是容器的位宽，不是有效位深**——有效位深要
            // 从容器的 bits_per_raw_sample 读，那个字段在压缩格式上常常不可靠。
            // 报 32 而不是报 24，因为那才是解码器真的产出的东西。
            p.info.bits_per_sample = 32;
            break;
        case AV_SAMPLE_FMT_S64:
        case AV_SAMPLE_FMT_S64P:
        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP:
            p.info.bits_per_sample = 64;
            break;
        default:
            p.info.bits_per_sample = 0;
            break;
    }
    // How much of that container is signal. FFmpeg fills this from the stream
    // for PCM and for FLAC/ALAC, and leaves it 0 when it does not know -- which
    // is why the caller treats 0 as "no claim" rather than as "8 bits".
    p.info.raw_bits_per_sample = static_cast<uint32_t>(std::max(0, p.codec->bits_per_raw_sample));
    if (stream->duration > 0 && stream->time_base.den > 0) {
        p.info.duration_ms =
            1000.0 * static_cast<double>(stream->duration) * av_q2d(stream->time_base);
    }
    // FFmpeg 9's avcodec_get_name takes a codec *id*, not a codec pointer; the
    // context carries it. Older FFmpeg took the pointer, which is the kind of
    // break a wrapper exists to absorb.
    const char* name = avcodec_get_name(p.codec->codec_id);
    p.info.codec = name ? name : "unknown";
    // The codec id alone is `eac3` for both plain DD+ and its Atmos (JOC)
    // variant; FFmpeg's profile is the field that tells them apart.
    const char* profile = avcodec_profile_name(p.codec->codec_id, p.codec->profile);
    p.info.codec_profile = profile ? profile : "";

    // 标签。
    //
    // `avformat_open_input` 已经把它们读进了 metadata 字典，所以这里不是「去解析
    // 标签」，只是把它们抄进 `info`——数据在第一步就到了，之前没有人读。
    //
    // **放在这里**而不是紧跟着 open：交叉验证码率需要 `duration_ms`，而那是上面
    // 几行才填好的。第一版把它放在 `av_find_best_stream` 之前，于是流索引还是 -1、
    // 时长还是 0，两个问题叠加——既读不到流级标签，码率验证也用不了。
    //
    // 容器级（`format->metadata`）先读，流级（`stream->metadata`）后读并覆盖。
    // 大多数格式把标签放在容器级，但有的放流级，只读一处会在那些文件上得到空的
    // ——而「空的」和「这个文件没有标签」看起来一模一样。
    //
    // 只取白名单里的键，不搬整个字典：标签可以有任何名字和任意长度，而跨过 ABI 的
    // 结构体需要定长字段。界面上真正要显示的只有那几项。
    {
        uint64_t fileBytes = 0;
        std::error_code ec;
        const auto sz = std::filesystem::file_size(path, ec);
        if (!ec) fileBytes = static_cast<uint64_t>(sz);
        readTags(p.format->metadata, &p.info, fileBytes);
        readTags(stream->metadata, &p.info, fileBytes);
    }

    // Output: interleaved float at the file's own rate, and at `out_channels`
    // channels when the caller asked for a conversion.
    //
    // The rate is deliberately the file's, not the device's. Resampling belongs
    // to the engine's single conversion stage, so having the decoder resample as
    // well would mean two converters in series and no single place to look when
    // something sounds wrong.
    p.info.out_channels = outChannels ? outChannels : p.info.channels;
    p.outChannels = outChannels;
    av_channel_layout_default(&p.out_layout, p.info.out_channels);

    if (swr_alloc_set_opts2(&p.swr, &p.out_layout, AV_SAMPLE_FMT_FLT, p.codec->sample_rate,
                           &p.codec->ch_layout, p.codec->sample_fmt, p.codec->sample_rate, 0,
                           nullptr) < 0 ||
        !p.swr) {
        return fail("could not configure sample conversion");
    }
    if (swr_init(p.swr) < 0) {
        swr_free(&p.swr);
        return fail("could not initialise sample conversion");
    }

    p.packet = av_packet_alloc();
    p.frame = av_frame_alloc();
    if (!p.packet || !p.frame) return fail("out of memory");

    p.info.decode_path = "ffmpeg:" + p.info.codec;

    // DSD 的 ÷2 后置抽取。用环境变量开关，这样同一个构建里能 A/B。
    // 值必须是**非空且不是 "0"** 才算「显式关闭」，而不仅仅是「变量存在」。
    //
    // 只判断存在是个陷阱：设成空字符串等于打开，而「设成空」恰恰是最自然的关闭方式。
    // 我自己的 A/B 脚本就是这么写的，于是第一轮「关闭」其实也是打开——量到的两条路径
    // 其实是同一条。切换任何功能开关都要能区分「未设置」「空」「0」「1」。
    //
    // **默认打开。** 依据是实测：在同一台设备、同一个文件上，40-80 kHz 从 +5.72 dB 相对
    // 降到 −12.21 dB 相对（17.9 dB），20-40 kHz 改善 17.4 dB，80 kHz 以上改善 17.1 dB，
    // 三条频带一致；而听者确认底噪明显变小。
    //
    // 仪器预测的是 103 dB，实测只有 17–18 dB。按实测报，不按预测报。
    //
    // 它只动 22 kHz 以上，不碰可听频带，所以它不是「换一条路径」，而是在既有路径后面
    // 加一级滤波。引擎仍然报 resampling=1 bitPerfect=0——那仍然是真的。
    const char* postFilterEnv = std::getenv("NEIRO_DSD_POSTFILTER");
    const bool postFilterOff = postFilterEnv && postFilterEnv[0] != '\0' &&
                                std::strcmp(postFilterEnv, "0") == 0;

    // 判断依据是「这是不是 DSD 流」，而不是「速率是不是 352800」。
    //
    // 352800 是 **DSD64 44.1 kHz** 的解码率（FFmpeg 的 DSD 解码器输出比特率 ÷ 8）。
    // 写死那个数字的后果是：DSD128 解到 705600，于是**这一级静默不生效**——而用户不会收到
    // 任何提示。那是这个项目最不能接受的一种失效方式。
    //
    // 通带与阻带也从流的实际速率推出来，而不是写死 20 kHz / 22.05 kHz：44.1 kHz 的源用
    // 22.05 kHz 阻带，而 48 kHz 的 DSD 变体该用 24 kHz。写死会在那种文件上放行噪声。
    const bool isDsdStream = p.info.codec.size() >= 3 &&
                             p.info.codec.compare(0, 3, "dsd") == 0;
    if (isDsdStream) {
        // FFmpeg 的 DSD 解码器输出 `dsdRate / 8`，而 DSD 的比特率是
        // `audioRate × 64 × 2^(等级-6)`。所以 `sample_rate / audioRate` 是 8、16、32、64…
        //
        // **不能只除以 8。** 我第一版就是这么写的，而它只对 DSD64 成立：DSD128 解到
        // 705600，除以 8 得 88200——那是真实音频速率的两倍，于是阻带会被放到 44100 Hz，
        // **放行 20 kHz 的噪声同时声称在滤波**。那是比不滤波更坏的一种错。
        //
        // 所以这里辨认音频速率：对 DSD 的每个标准音频速率，检查 `sample_rate / base` 是否是
        // 2 的幂且 ≥ 8。实际文件里只有一个会命中。
        static const uint32_t kDsdBases[] = {44100, 48000,  88200, 96000,
                                             176400, 192000, 352800, 384000};
        uint32_t audioRate = 0;
        for (uint32_t base : kDsdBases) {
            if (p.info.sample_rate < base * 8) continue;
            const uint32_t ratio = p.info.sample_rate / base;
            // 必须是 2 的幂，且 64 以上（超过 64 说明那不是 DSD 的倍数关系）。
            if (ratio < 8 || ratio > 64) continue;
            bool pow2 = (ratio & (ratio - 1)) == 0;
            if (pow2 && p.info.sample_rate % base == 0) {
                audioRate = base;
                break;
            }
        }

        if (audioRate == 0) {
            // **说出来**。「DSD 没有超声滤波」正是用户需要知道的事，而静默不做等于谎称
            // 界面上的路径就是实际路径。
            p.info.decode_path += "（DSD 后置抽取未生效：无法从解码率 " +
                                 std::to_string(p.info.sample_rate) +
                                 " Hz 反推 DSD 的音频速率，所以超声滤波无法定位）";
        } else {
            const double stopbandHz = static_cast<double>(audioRate) * 0.5;
            const double passbandHz = stopbandHz * 0.9;

            if (postFilterOff) {
                p.info.decode_path += "（DSD 后置抽取被 NEIRO_DSD_POSTFILTER=0 关掉）";
            } else {
                const std::vector<double> h = neiro::dsd::designDecimationFilter(
                    static_cast<double>(p.info.sample_rate), passbandHz, stopbandHz, 100.0);
                if (!h.empty()) {
                    // 抽到 4 × 音频速率。那仍然远高于音源的 Nyquist（2 ×），所以再抽就会
                    // 真的丢带宽。因子由两级速率算出，不是写死的 2——虽然对 DSD64/128/256
                    // 它恰好总是 2。
                    const uint32_t decimation =
                        p.info.sample_rate / (audioRate * 4) > 1
                            ? p.info.sample_rate / (audioRate * 4)
                            : 2;
                    p.dsdPostFilter.configure(h, static_cast<int>(decimation));
                    p.dsdPostFilterActive = true;
                    p.info.sample_rate = p.info.sample_rate / decimation;
                    p.info.decode_path += "+neiro-dsd-decimate-" + std::to_string(decimation);
                } else {
                    p.info.decode_path += "（DSD 后置抽取滤波器设计失败，未生效）";
                }
            }
        }
    }

    // ---- 自己解 DSD ----
    //
    // 放在最后：上面那些（时长、标签、码率交叉核对）都依赖 FFmpeg 已经打开的上下文，
    // 而下面要覆盖的是 `sample_rate` 与实际送出的采样。
    // 必须写成 `p.tryOwnDsdDecoder(...)`：嵌套类的成员函数**不在**外层类成员的作用域里，
    // 所以不加限定名找不到它。第一版写成裸调用，编译器直接指出来了。
    p.tryOwnDsdDecoder(path, &p);

    return true;
}

void Decoder::Impl::tryOwnDsdDecoder(const std::string& path, Impl* p) {
    // 只对 DSF 走这条。DFF 是另一种排布，而那个容器本机没有样本，所以按
    // 「没有样本就没有基线」处理，不假装支持。
    const bool wantOwn = std::getenv("NEIRO_DSD_DECODER") != nullptr;
    if (!wantOwn) return;

    ownDsd = std::make_unique<dsf::File>();
    if (!ownDsd->open(path.c_str())) {
        // 不是 DSF，或者头解析失败。**不是错误**——大多数文件都走不到这里。
        ownDsdWhyNot = ownDsd->error;
        ownDsd.reset();
        return;
    }

    const uint32_t fileChannels = ownDsd->fmt.channels;
    const uint32_t wantChannels = outChannels ? outChannels : fileChannels;
    if (wantChannels != fileChannels) {
        // 自写路径没有下混矩阵。FFmpeg 有（它带着声道布局和系数），所以这种情况退回
        // FFmpeg——但**说清楚为什么退**，否则「这条是自写解码器」就成了一句空话。
        ownDsdWhyNot = "requested " + std::to_string(wantChannels) + " channels, file has " +
                       std::to_string(fileChannels);
        ownDsd.reset();
        return;
    }

    double audioRate = 0;
    std::string err;
    if (!ownDsdCascade.configure(ownDsd->fmt.dsdRate, fileChannels, &audioRate, &err)) {
        ownDsdWhyNot = err;
        ownDsd.reset();
        return;
    }

    ownDsdBlocks = ownDsd->totalBlocks();
    ownDsdBlock = 0;
    ownDsdActive = true;
    ownDsdWhyNot.clear();

    // 这两个字段决定引擎后续怎么处理这段音频，所以**必须**改成我们自己送出的速率，
    // 而不是 FFmpeg 声明的那个（352800）。留着旧的会让引擎的重采样器按错误的输入速率
    // 工作——那是一个「能播但速率错了四倍」的缺陷。
    p->info.sample_rate = static_cast<uint32_t>(audioRate);
    p->info.channels = fileChannels;
    p->info.bits_per_sample = 32;
    p->info.codec = "DSD (1-bit, " + std::to_string(ownDsd->fmt.dsdRate) + " bps)";
    p->info.decode_path = "neiro-dsd-cascade";
}

bool Decoder::seek(double seconds, std::string* error) {
    Impl& p = *impl_;
    if (!p.format) {
        if (error) *error = "not open";
        return false;
    }
    if (seconds < 0) seconds = 0;

    // 自写 DSD 路径：seek 是**算出来的**，不是让容器去找的。
    //
    // 而且这里**必须**重置抽取滤波器的历史——上一次播放留下的 taps-1 个样本会漏进
    // 跳转后的第一段，听起来是开头一声「咚」。FFmpeg 那边靠 `avcodec_flush_buffers`
    // 做同样的事，所以两条路径的行为是对称的。
    if (p.ownDsdActive && p.ownDsd) {
        const double audioRate = static_cast<double>(p.info.sample_rate);
        const uint64_t frames = static_cast<uint64_t>(seconds * audioRate);
        // 落到块边界：块的粒度是 4096 字节 = 2048 帧 = 11.6 ms，这个误差远小于任何
        // 压缩格式的 seek 误差，而且让重启后的滤波器从零状态开始，是可预测的。
        const uint64_t framesPerBlock =
            static_cast<uint64_t>(p.ownDsd->fmt.blockSize) * 8 / 16;
        p.ownDsdBlock = framesPerBlock ? frames / framesPerBlock : 0;
        if (p.ownDsdBlock > p.ownDsdBlocks) p.ownDsdBlock = p.ownDsdBlocks;
        p.ownDsdCascade.reset();
        p.drained = false;
        p.pending.clear();
        p.pending_pos = 0;
        return true;
    }

    // Seek on the stream's own time base, then let the codec discard what it has
    // buffered. Both steps are required: the first moves the container, the
    // second the decoder.
    const int64_t target = static_cast<int64_t>(seconds / av_q2d(p.format->streams[p.stream_index]->time_base));
    int rc = av_seek_frame(p.format, p.stream_index, target, AVSEEK_FLAG_BACKWARD);
    if (rc < 0) {
        if (error) *error = "seek failed: " + ffmpegError(rc);
        return false;
    }
    avcodec_flush_buffers(p.codec);
    if (p.swr) swr_convert(p.swr, nullptr, 0, nullptr, 0);

    p.pending.clear();
    p.pending_pos = 0;
    p.flushing = false;
    p.drained = false;
    // Counts from the new position, so a caller using it to report progress
    // reports progress through the file the user actually asked for.
    p.frames_read = 0;
    return true;
}

uint64_t Decoder::framesRead() const { return impl_->frames_read; }

bool Decoder::readOwnDsd(std::vector<float>* out, size_t frames, std::string* error) {
    Impl& p = *impl_;
    if (!p.ownDsdActive || !p.ownDsd) {
        if (error) *error = "own DSD path is not active";
        return false;
    }
    if (!out) return false;

    const size_t ch = p.info.out_channels ? p.info.out_channels : 1;
    const size_t want = frames * ch;
    const size_t start = out->size();

    // 抽头 8822 + 1102 个历史，而一个 DSF 块是 4096 字节/声道 = 32768 比特 =
    // 2048 个 176400 Hz 帧。所以一个块不足以填满滤波器，至少要两个。
    //
    // 循环先抽干缓冲再读新块，顺序不能反：抽干是廉价的，而每读一个块都要做 I/O。
    while (out->size() - start < want) {
        p.ownDsdCascade.drainPending(out, want - (out->size() - start));
        if (out->size() - start >= want) break;
        if (p.ownDsdBlock >= p.ownDsdBlocks) break;  // 流结束
        if (!p.ownDsd->readBlockRaw(p.ownDsdBlock, &p.ownDsdBlockBuf)) break;
        ++p.ownDsdBlock;
        // 每块的字节位置数 = blockSize（每声道），总字节 = blockSize * channels，
        // 而 readBlockRaw 给的正是块内交错的整块。
        p.ownDsdCascade.feedInterleaved(p.ownDsdBlockBuf.data(), p.ownDsd->fmt.blockSize, out,
                                        want - (out->size() - start));
    }

    const size_t got = out->size() - start;
    p.frames_read += got / ch;
    if (got < want) {
        if (error) error->clear();
        return false;  // 流结束，短返回不是错误
    }
    return true;
}

bool Decoder::readExact(std::vector<float>* out, size_t frames, std::string* error) {
    Impl& p = *impl_;
    if (p.ownDsdActive) return readOwnDsd(out, frames, error);
    if (!p.codec) {
        if (error) *error = "not open";
        return false;
    }
    if (!out) return false;

    const size_t ch = p.info.out_channels ? p.info.out_channels : 1;
    const size_t want = frames * ch;
    const size_t start = out->size();

    // Each read() may add nothing (the converter swallowed a frame) or a lot, so
    // this keeps asking until it has enough or the stream genuinely ends. The
    // guard is not paranoia for its own sake: a decoder that returns true with
    // no samples would otherwise spin here forever.
    for (int guard = 0; out->size() - start < want && guard < 4096; ++guard) {
        const size_t before = out->size();
        if (!read(out, error)) {
            if (error) error->clear();
            break;  // end of stream; a short result is the answer, not a failure
        }
        if (out->size() == before) continue;
    }

    const size_t got = out->size() - start;
    // Counted *after* the excess is stashed below, and as what this call
    // delivered rather than as what the decoder produced.
    //
    // This line used to sit above the trim, adding got, which is the total
    // the decoder handed over. The difference between the two is exactly the
    // excess -- and the excess is not gone, it is in pending and this same
    // function hands it back on the next call, where it is counted again. So
    // every stashed sample was counted twice and the error grew with every call
    // rather than settling.
    //
    // Measured on a 295.8 s / 96 kHz FLAC, against the samples actually
    // delivered:
    //
    //   block size      framesRead        over-count
    //        4096         31,949,666      +3,550,080   (11%)
    //      100000         28,558,498        +158,912
    //           1      16,372,199,859   +16,343,800,273
    //
    // The audio was correct in every one of those runs -- tail_check confirms
    // zero samples missing at every block size -- so this was never audible.
    // It was visible as a playhead that outran the music, because this counter
    // is what the engine reports as the source position.
    //
    // The last row is the tell for the size dependence: at a block size of 1
    // the counter reads 575x the truth, because every call stashes nearly a
    // whole decoder frame and re-counts it. Small blocks are not a
    // pathological input either -- a slow device produces them for free, as
    // measured elsewhere in this project at 2 to 28 frames per pass.
    p.frames_read += (got < want ? got : want) / ch;

    // Hand back the excess rather than delivering more than was asked for.
    //
    // The caller sizes its own bookkeeping from `frames`, so returning a whole
    // decoder frame's worth for a smaller request makes the pipeline receive more
    // audio than the source account says it did -- and the device then plays
    // several times what the source supplied. Keeping the remainder in `pending_`
    // loses nothing: the next read hands it straight back.
    if (got > want) {
        p.pending.assign(out->begin() + static_cast<long>(start + want), out->end());
        p.pending_pos = 0;
        out->resize(start + want);
    }

    return got >= want;
}

bool Decoder::read(std::vector<float>* out, std::string* error) {
    Impl& p = *impl_;
    if (!p.codec) {
        if (error) *error = "not open";
        return false;
    }
    if (!out) return false;

    // Hand back whatever is left of an already-converted frame first.
    if (p.pending_pos < p.pending.size()) {
        out->insert(out->end(), p.pending.begin() + static_cast<long>(p.pending_pos),
                    p.pending.end());
        p.pending.clear();
        p.pending_pos = 0;
        return true;
    }
    if (p.drained) return false;

    for (;;) {
        int rc = avcodec_receive_frame(p.codec, p.frame);
        if (rc == 0) {
            // Convert this frame and hand it over.
            p.pending.clear();
            p.pending_pos = 0;
            p.convertFrame(p.frame, &p.pending);
            if (!p.pending.empty()) {
                out->insert(out->end(), p.pending.begin(), p.pending.end());
                p.pending.clear();
                return true;
            }
            av_frame_unref(p.frame);
            continue;
        }
        if (rc == AVERROR_EOF) {
            // End of stream: flush the converter so the last samples are not
            // stranded in its delay line, then report the end.
            p.flushing = true;
            p.drained = true;
            p.drainConverted(&p.pending);
            if (!p.pending.empty()) {
                out->insert(out->end(), p.pending.begin(), p.pending.end());
                p.pending.clear();
                return true;
            }
            return false;
        }
        if (rc != AVERROR(EAGAIN)) {
            if (error) *error = "decode failed: " + ffmpegError(rc);
            return false;
        }

        // Need more input.
        rc = av_read_frame(p.format, p.packet);
        if (rc == AVERROR_EOF) {
            // No more packets: tell the codec, then loop to collect the tail.
            avcodec_send_packet(p.codec, nullptr);
            continue;
        }
        if (rc < 0) {
            if (error) *error = "read failed: " + ffmpegError(rc);
            return false;
        }
        if (p.packet->stream_index == p.stream_index) {
            avcodec_send_packet(p.codec, p.packet);
        }
        av_packet_unref(p.packet);
    }
}

}  // namespace neiro
