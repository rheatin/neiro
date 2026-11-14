// Audio file decoding, wrapped around FFmpeg.
//
// Why FFmpeg lives behind a wrapper rather than being called from the render
// path:
//
//   * libavcodec's decode loop wants a `AVFrame` and hands back one, and letting
//     that type near the render loop would tie the audio path to a specific
//     FFmpeg version. `read()` returning interleaved float is a boundary that
//     can survive an upgrade.
//   * decoding allocates and does I/O. It belongs on a feeder, not in the
//     callback that has a 10 ms window.
//   * the reference engine makes the same split (FFmpegDecoder + a pipeline
//     that consumes PCM blocks).
//
// # Output format
//
// Interleaved 32-bit float, normalised to [-1, 1]. Not because that is what the
// DAC wants — the engine resamples and requantises further down — but because
// it is the only representation that loses nothing *yet*. Converting to a
// narrower integer here would throw away precision the resampler could still
// have used, and the dither stage would then have nothing left to dither.
//
// # Threading
//
// A Decoder is not thread-safe and belongs to one thread. In the real engine that
// is a feeder thread filling a ring buffer; in this spike it is the caller.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neiro {

struct StreamInfo {
    uint32_t sample_rate = 0;
    // The file's own channel count. Kept truthful even when a downmix was
    // requested, because this is what the UI shows the user about their file.
    uint32_t channels = 0;
    // What read() actually delivers per frame. Equal to `channels` unless a
    // conversion was asked for. The conversion maths uses this one.
    uint32_t out_channels = 0;
    // Duration in milliseconds, when the container reports it.
    double duration_ms = 0.0;
    std::string codec;
    // The container's width, in bits per sample: 8, 16, 32, 64, or 0 when the
    // format did not say.
    //
    // **This is the container's width, not the valid bit depth.** A 24-bit FLAC
    // decodes to S32 with the top eight bits unused, and this field says 32
    // because that is what comes out of the decoder. The valid depth lives in the
    // container's `bits_per_raw_sample`, which is absent or zero for most
    // compressed formats, so there is nowhere reliable to read it here.
    //
    // It is still worth carrying, because "32 bit container for a 24-bit master" is
    // the truth about the file, and a UI that printed nothing at all would be
    // claiming less than it knows. What it must not do is present this as the
    // device's word length -- that is `NAE_SignalPath.granted_bits`, and the two
    // differ routinely.
    uint32_t bits_per_sample = 0;
    /// Bits the source actually carries, as the demuxer reports them.
    ///
    /// `bits_per_sample` above is the *container* width the decoder produces, so
    /// a 24-bit FLAC and a 32-bit WAV both report 32. This is the number that
    /// says how much of that container is real, and it is the one that decides
    /// whether the engine's float32 path can carry the file without loss. Zero
    /// means the demuxer did not say, which is common for some containers.
    uint32_t raw_bits_per_sample = 0;

    // ---- 标签 ----
    //
    // 从容器的 metadata 字典读的，不是从别处解析的：`avformat_open_input` 已经把它们
    // 读进去了，之前没有人抄出来。
    //
    // 空字符串表示**这个文件没有这个标签**，它与「解析失败」在界面上必须能分辨——
    // 前者是关于音乐的陈述，后者是关于播放器的故障。空字符串表示前者，因为解析失败
    // 会被报出来而不是悄悄留下空白。
    //
    // 年份只保留数字部分：`DATE` 通常是 `2024-05-01`，而界面要的是年份。
    std::string year;
    std::string genre;
    /// 码率，**kbps**。
    ///
    /// 标签里的单位不统一，所以这里已经换算过：FLAC 的 `BITRATE` 是
    /// `<容器总码率>_<音频码率>`，单位 **bps**，取下划线之后那段再除以 1000。
    /// 直接把标签里的数字当 kbps 会差 1000 倍。
    ///
    /// 超出合理区间的值会被丢弃而不是显示：那种情况下这个键装的是别的东西，
    /// 显示它比不显示更糟。
    uint32_t bitrate_kbps = 0;

    /// `bitrate_kbps` 来自标签，还是来自文件大小除以时长。
    ///
    /// 这不是装饰字段。标签里的 `BITRATE` 可能装的是别的东西——实测一个
    /// `BITRATE=96000` 的文件，它装的是采样率，而 96000 bps 换算过来是 96 kbps，
    /// 落在任何合理的区间内，于是被照单显示。
    ///
    /// 唯一的裁判是文件大小除以时长，因为它来自操作系统而不是标签。超出 10% 容差
    /// 时用实测值覆盖，此时这个字段为 false——所以界面可以在必要时说「这个数字是
    /// 算出来的，不是标签说的」。
    ///
    /// **没有标签时也可以是 false。** 一个没有 `BITRATE` 键的文件，只要时长和文件
    /// 大小都有，就照样报一个算出来的码率，来源同样是「算出来的」。所以这个字段说明
    /// 的是**来源**，而不是「标签和实测是否一致」。
    ///
    /// 两者都没有（码率为 0）时这个字段没有意义，两个来源都不适用；调用方按 0 处理，
    /// 不问来源。
    bool bitrate_from_tag = true;

    // 专辑与艺人。
    //
    // 和上面几项一样来自同一个 metadata 字典。加它们是因为本地文件的 fact strip
    // 里「专辑」和「艺术家」两格也是空的，而它们是标签里最标准的两项——比年份和
    // 流派标准得多，几乎每个 FLAC 和 m4a 都有。
    std::string album;
    std::string artist;
    // 完整元数据。之前只有 year/genre/album/artist/bitrate 被抄出来，标题、
    // 轨号、碟号、Album Artist、composer、comment 留在 FFmpeg 的字典里没人读。
    std::string title;
    std::string album_artist;
    std::string track;
    std::string disc;
    std::string composer;
    std::string comment;

    // ---- ReplayGain ----
    //
    // Values come from the same metadata dictionary as the tags above. A gain
    // without a peak is still usable; `preventClipping` is skipped when the peak
    // is absent rather than treating the missing value as zero (which would mute
    // the stream).
    bool rg_track_valid = false;
    double rg_track_gain_db = 0.0;
    double rg_track_peak = 0.0;
    bool rg_album_valid = false;
    double rg_album_gain_db = 0.0;
    double rg_album_peak = 0.0;

    /// **采样是谁解出来的**，而不是容器说它是什么。
    ///
    /// `codec` 字段装的是 FFmpeg 选中的解码器名字。但 DSD 有三条完全不同的路，而它们
    /// 的保真度差别很大：
    ///
    ///   * `neiro-dsd-cascade` —— 本引擎的 DSF 读取器加自写的 ÷8/÷2 抽取级联，终止
    ///     于 176400 Hz。实测 40-80 kHz 在 −220.39 dBFS。
    ///   * `ffmpeg:dsd_lsbf_planar` —— FFmpeg 的 DSD 解码器，实测它内部只有约 20 dB
    ///     的衰减而且响应非单调，40-80 kHz 在 −154.37 dBFS。
    ///   * `ffmpeg:<别的名字>` —— 普通 PCM。
    ///
    /// 这一项存在是因为**解码器名字不描述它做了什么**：两份都叫 DSD 解码器，一份自己
    /// 带滤波级联，一份不带。从名字推断 DSD 音质是猜。
    ///
    /// 空字符串表示「没有走自己的路径」，而那与「解析失败」必须能分辨。
    std::string decode_path;
};

class Decoder {
  public:
    Decoder();
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Open a file and read the first stream's parameters. Returns false with a
    // reason in `error`; the error is a message fit to show a user, not a code.
    //
    // `outChannels` of 0 keeps the file's own channel count. Any other value asks
    // libswresample to deliver that many channels, which is where a downmix or an
    // upmix belongs: it has the channel layouts and the filter coefficients, and
    // doing it here by hand means hand-maintaining a matrix for every layout.
    bool open(const std::string& path, std::string* error, uint32_t outChannels = 0);

    bool is_open() const;

    // Decode the next block of interleaved float samples.
    //
    // Returns false at end of stream. A short read is normal — a decoder hands
    // back whatever one packet produced — so a caller must keep asking until it
    // returns false, and must not treat a short block as the end.
    bool read(std::vector<float>* out, std::string* error);

    /// 自己解 DSD 时的那条路。语义与 `readExact` 相同：尽力凑满 `frames` 帧，
    /// 流结束就短返回，而短返回不是错误。
    bool readOwnDsd(std::vector<float>* out, size_t frames, std::string* error);

    /// Decode until `frames` frames are available in `out`, or the stream ends.
    //
    // This is what the engine's feeder wants. A decoder hands back whatever one
    // packet produced — 2048 frames one call, 1024 the next — and a consumer
    // feeding a fixed-size block has to be able to ask for an exact amount. A
    // short result means end of stream, and is reported as such.
    //
    // Decode until `out` holds **exactly** `frames` frames, or the stream ends.
    //
    // "Exactly" is the whole contract, and getting it wrong is not a small error.
    // A decoder hands back whatever one packet produced -- 4096 frames for a FLAC
    // frame -- so a request for 893 comes back with 4096. An earlier version
    // returned the whole lot and reported success, and the caller, which sizes
    // its own accounting from the number it asked for, fed 4096 frames into the
    // pipeline while recording 893. Everything downstream balanced against those
    // 893 and the device ended up playing roughly nine times the audio the source
    // had supplied.
    //
    // Whatever the decoder over-produced is kept for the next call rather than
    // thrown away, so no audio is lost either.
    //
    // Returns false only at end of stream. A short result is not an error.
    //
    // `out` is appended to, not cleared, so a caller can accumulate.
    bool readExact(std::vector<float>* out, size_t frames, std::string* error);

    // How many frames have been handed out so far. The engine uses this to know
    // where playback has reached, which is not the same as a seek position and is
    // not used as one.
    uint64_t framesRead() const;

    // Restart from a position, in seconds. Seeking is approximate for
    // compressed formats: FFmpeg lands on the nearest packet and the container
    // may not index precisely. For audio this is inaudible, but the UI should
    // not promise sample accuracy.
    bool seek(double seconds, std::string* error);

    const StreamInfo& info() const;

    // What the decode path actually is, as a human-readable block.
    //
    // This exists because the codec's name does not describe it. FFmpeg's
    // `dsd_msbf` decoder's native output is `AV_SAMPLE_FMT_DSD` -- one-bit
    // samples at the DSD rate -- and the conversion to PCM happens in a
    // libswresample instance *after* it, whose filter is the entire DSD to PCM
    // design. Nothing in the codec name says which filter that is, or even that
    // one ran, so a claim about DSD quality made from the codec name is a guess.
    //
    // Everything here is read back from the opened decoder rather than assumed,
    // so the block describes the pipeline that is actually in use.
    std::string describe() const;

  private:
    // FFmpeg types are hidden so this header needs no FFmpeg include, which
    // keeps the compile dependency out of everything that includes it.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neiro
