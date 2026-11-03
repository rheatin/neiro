//! Safe wrappers over the engine's C ABI.
//!
//! This module is where the two ownership rules from `nae.h` are actually
//! enforced, so that nothing above this layer has to remember them:
//!
//!   * **Strings** returned by the engine are copied into a Rust `String` and
//!     then released with `NAE_FreeString`. Nothing outside this file ever sees
//!     a `char*`.
//!   * **Handles** are owned by a [`Stream`], whose `Drop` calls `NAE_Close`.
//!     A dropped or early-returned `Stream` cannot leak a COM interface.
//!
//! The rule underneath both: memory must be released by the side that
//! allocated it. The DLL and this crate are built with different toolchains and
//! have different heaps, so `free()` on an engine-allocated pointer would be
//! undefined behaviour even though it usually appears to work.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::ptr;

use serde::{Deserialize, Serialize};

use crate::error::{EngineError, Result};
use crate::ffi;
use crate::output::{OutputMode, Quality};

/// One render endpoint, as reported by `NAE_EnumerateDevices`.
///
/// This is a `serde` mirror of the JSON the C++ side builds. Keeping the
/// interchange format as JSON means no struct packing or alignment rules have to
/// agree across the two languages.
///
/// The wire format is camelCase because that is what the C++ emitter produces;
/// `rename_all` keeps the Rust API idiomatic instead of leaking that choice
/// into every call site.
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Device {
    /// Opaque WASAPI endpoint id. Pass back verbatim to `open_with`.
    pub id: String,
    pub name: String,
    /// The endpoint's native rate. `0` means it did not report one.
    pub sample_rate: u32,
    /// Container size, e.g. 32 for a 24-in-32 endpoint.
    pub bits_per_sample: u32,
    /// Bits that actually carry audio. This is the number to show a user, and
    /// the one the engine negotiates with — confusing it with
    /// `bits_per_sample` is what once made exclusive mode look impossible.
    pub valid_bits: u32,
    pub channels: u32,
    /// Whether exclusive mode is worth attempting on this endpoint.
    ///
    /// Not a static property: the engine probes it by really trying to open the
    /// device, so it reports false whenever something else already holds the
    /// endpoint.
    pub exclusive: bool,
    /// Whether shared mode is available (essentially always true).
    pub shared: bool,
}

/// The format a stream actually runs at, after any conversion.
///
/// Every field comes from what the device was actually opened with, not from
/// what the file claims. A player that reports the file's format while quietly
/// resampling is lying.
#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct SignalPath {
    /// The rate the device is being fed at. May differ from the source.
    pub sample_rate: u32,
    /// The source's own rate.
    pub source_rate: u32,
    pub channels: u32,
    /// Container size, e.g. 32 for a 24-in-32 endpoint.
    pub bits_per_sample: u32,
    /// Bits that actually carry audio.
    pub valid_bits: u32,
    pub is_float: bool,
    pub latency_ms: f64,

    // ---- honesty flags ----
    //
    // These let the UI say something truthful about the signal path.
    // `bit_perfect` is the important one: it is false whenever a converter or a
    // requantiser is in the path, and the UI must not claim otherwise.
    /// A converter is in the path.
    pub resampling: bool,
    /// The output is narrower than the pipeline's internal precision, and being
    /// dithered.
    pub dithered: bool,
    /// Nothing altered the samples: no resampling, no dither.
    pub bit_perfect: bool,

    /// Which share mode the stream was opened in.
    ///
    /// Added when shared mode arrived, and it is a field rather than something the
    /// UI infers from the route it selected. The route is a *choice*; this is what
    /// the engine did with it, and a badge that reads the choice would keep saying
    /// "Exclusive" after a shared open.
    pub exclusive: bool,
}

impl SignalPath {
    /// Bits per second actually leaving the device.
    pub fn bit_rate(&self) -> u32 {
        self.sample_rate * self.channels * self.valid_bits
    }
}

/// What the file being played actually is.
///
/// Separate from [`SignalPath`], which describes what the *device* is doing. The
/// two differ whenever a conversion happened, and the UI needs both: a 5.1 file
/// on a stereo device has a signal path of "48 kHz, 2 ch" and a file that is
/// "48 kHz, 6 ch", and showing only the first would misdescribe the user's music.
#[derive(Debug, Clone)]
pub struct TrackInfo {
    /// The file's own channel count, before any downmix or upmix.
    pub channels: u32,
    /// Length in milliseconds, or 0 when the container does not report one.
    ///
    /// 0 is not "zero-length". It is "unknown", and the UI must render an
    /// indeterminate progress bar rather than one that is already full.
    pub duration_ms: f64,
    /// 文件自己的采样率。
    ///
    /// 与 `SignalPath::sample_rate` 是不同的东西，而且是必然不同的：那是设备速率。
    /// 一张 44.1 kHz 的文件在 192 kHz 设备上，前者是 44100、后者是 192000。
    /// 界面要同时显示两者，否则「96 kHz」这样的数字要么在描述文件、要么在描述
    /// 输出，而读者无从知道是哪个。
    pub sample_rate: u32,
    /// 文件的**容器位宽**，位/采样。0 表示容器没有报。
    ///
    /// **这不是有效位深。** 24 bit 的 FLAC 解出来是 S32、高 8 位不用，所以这里是
    /// 32；有效位深在压缩格式的容器里通常不可靠。设备那边的字长是
    /// `SignalPath::granted_bits`，而 `valid_bits` 又是第三个数——三者在 Hi-Res
    /// 文件上通常不同，混用任何一个都会给出错误的说法。
    pub bits_per_sample: u32,
    /// 编码名，如 "flac" / "aac"。空表示容器没有报。
    pub codec: String,
}

/// 文件的标签。
///
/// 与 [`TrackInfo`] 分开，因为**失败方式不同**：格式信息一定存在（容器报了就是
/// 有），标签**可能不存在**（这个文件没写）。混在一处会让「DSD64 / 352.8 kHz /
/// 2ch」和一个空字符串共享同一种缺失的表示，而两者该被读成完全不同的两件事。
///
/// 空字符串表示**文件没有这个标签**，那是关于音乐的事实，不是播放器的故障。
#[derive(Debug, Clone, Default)]
pub struct Tags {
    /// 年份的数字部分。`DATE` 是 `2024-05-01` 时这里是 `2024`。
    pub year: String,
    pub genre: String,
    pub album: String,
    pub artist: String,
    /// 码率，**kbps**，已经换算过。0 表示没有。
    ///
    /// 标签里的单位不统一（FLAC 的 `BITRATE` 是 `<总>_<音频>`、单位 bps），
    /// 直接显示会差 1000 倍。
    pub bitrate_kbps: u32,
    /// 这个码率来自标签，还是来自文件大小除以时长。
    ///
    /// 标签可能装的是别的东西——实测一个 `BITRATE=96000` 的文件，它装的是采样率，
    /// 换算过来是 96 kbps，落在任何合理区间内。所以实测值是裁判，而这个字段说明
    /// 现在的数字是哪来的。
    pub bitrate_from_tag: bool,
}

/// An open exclusive stream.
///
/// Owns the engine handle and closes it on drop. Not `Send`: an exclusive WASAPI
/// stream belongs to the thread whose COM apartment created it, and moving the
/// handle to another thread is exactly the apartment-lifetime mistake the spike
/// caught. The real engine will pin the stream to a dedicated audio thread.
///
/// A stream is either a generated tone or a decoded file, and both are the same
/// handle with the same lifetime rules — only the source of the samples differs,
/// and the engine decides that internally. Keeping one type means one `Drop`, one
/// ownership story, and no way for a file stream to escape the close.
pub struct Stream {
    handle: *mut ffi::NAE_Handle,
    path: SignalPath,
    device_id: String,
    /// `Some` when this stream is playing a file. `None` for a tone, which has
    /// no length and cannot seek.
    track: Option<TrackInfo>,
}

impl Stream {
    /// The format this stream is running at, including the honesty flags.
    ///
    /// Captured at open, so it describes how the chain was *configured*. For a
    /// live reading — the latency, the fault counters, the reason code — use
    /// [`Stream::live`].
    pub fn signal_path(&self) -> SignalPath {
        self.path
    }

    /// A live sample of the running stream.
    ///
    /// Distinct from [`Stream::signal_path`] on purpose. The open-time snapshot is
    /// a property of the configuration; this is a measurement of a system in
    /// motion, and the counters in it only mean anything because it is read
    /// repeatedly. A panel fed from the snapshot alone reports a permanently
    /// healthy stream.
    pub fn live(&self) -> LiveSignalPath {
        query_signal_path(self.handle)
    }

    /// A reading of the output spectrum tap.
    ///
    /// The signal the device is being fed, at the rate it is being fed. Cheap
    /// enough to call from the audio thread at the `live` cadence: the render
    /// loop's only cost is one store per frame, and the transform runs here, on
    /// whichever thread asks, because the render loop's entire budget is the
    /// runway the device gave it.
    pub fn spectrum(&self) -> Spectrum {
        query_spectrum(self.handle)
    }

    /// 一段时域包络，节拍检测的数据源。
    ///
    /// 与 [`Stream::spectrum`] 共用同一个环形缓冲，所以两者读的是同一段音频的两种
    /// 聚合，不存在「频谱和包络来自不同时刻」的问题。
    pub fn envelope(&self) -> Envelope {
        query_envelope(self.handle)
    }

    // 这里原来有一个 `Stream::peaks(buckets)`。删掉了：peaks 那一遍**不需要这个流**
    // —— 它收的是路径（见 [`compute_peaks`]），因为从另一个线程拿这个 `!Send` 的
    // handle 正是本文件开头那条 `!Send` 约定不允许的事。
    // 挂在这里会让人以为它读的是流的内部状态。

    /// The rates this device was measured to accept in exclusive mode.
    pub fn supported_rates(&self) -> Vec<u32> {
        query_supported_rates(self.handle)
    }

    /// 重新读文件元数据。
    ///
    /// 与打开时捕获的那份分开，是因为播放期间设备可能被协商到别的速率——那会改变
    /// `signal_path`，但不会改变文件是什么。调用方想同时拿到两组时应该问两次，
    /// 而不是指望一份快照两者都有。
    pub fn track_info(&self) -> TrackInfo {
        query_track_info(self.handle)
    }

    /// 读文件标签。见 [`query_tags`]。
    pub fn tags(&self) -> Tags {
        query_tags(self.handle)
    }

    /// 采样**实际**是怎么被解出来的。
    ///
    /// 解码器名字不描述它做了什么——FFmpeg 的两个 DSD 解码器名字不同，但区别不在名字里，
    /// 而在有没有那一级抽取滤波。所以这个字符串是引擎**跑完之后**写下来的。
    ///
    /// **空字符串不是错误**：那表示这个信号没有额外路径（正弦、纯 PCM），与「读不出来」
    /// 完全不同——后者是关于播放器的故障。混在一起时界面会把故障显示成事实。
    pub fn decode_path(&self) -> String {
        let mut buf = [0i8; ffi::NAE_DECODE_PATH_MAX];
        // 长度写成 `buf.len()` 而不是常量：缓冲区比 `NAE_DECODE_PATH_MAX` 小的话引擎会
        // 截断而**不报错**，所以传实际长度才诚实。
        let rc =
            unsafe { ffi::NAE_QueryDecodePath(self.handle, buf.as_mut_ptr(), buf.len() as i32) };
        if rc != 0 {
            return String::new();
        }
        // 缓冲区是 i8 而不是 u8：FFI 边界上用有符号整数，避免 `as` 转换的 lint 噪音，
        // 而字节本身按原始位读取，所以语义不变。
        let bytes: Vec<u8> = buf.iter().map(|b| *b as u8).collect();
        let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
        String::from_utf8_lossy(&bytes[..end]).into_owned()
    }

    pub fn device_id(&self) -> &str {
        &self.device_id
    }

    /// True when the file's samples reach the device unmodified.
    ///
    /// The UI must consult this rather than inferring it from the mode or the
    /// rates: a resampler configured correctly still changes the samples, and so
    /// does folding six channels into two.
    pub fn is_bit_perfect(&self) -> bool {
        self.path.bit_perfect
    }

    /// What the file is, or `None` for a generated tone.
    pub fn track(&self) -> Option<TrackInfo> {
        self.track.clone()
    }

    /// Render `milliseconds` of whatever this stream is playing, and block.
    ///
    /// `milliseconds` is a budget on how long the call *takes*, not how much
    /// audio it produces. A caller that wants to play a whole track calls this
    /// repeatedly; the position carries across calls, so slicing is how playback
    /// is meant to be driven. Calling it once with the track's full length also
    /// works, but gives up the ability to stop in between.
    pub fn play(&mut self, milliseconds: i32) -> Result<()> {
        // SAFETY: `handle` is non-null and was returned by a successful open, so
        // it is live until NAE_Close.
        let rc = unsafe { ffi::NAE_Play(self.handle, milliseconds) };
        if rc == ffi::NAE_OK {
            Ok(())
        } else {
            Err(EngineError::from_code(rc).with_engine_message())
        }
    }

    /// Stop the device stream, keeping the handle and the position.
    ///
    /// Needed because the render loop now stays started across slice boundaries.
    /// Before, every `play` call stopped and restarted the client, so "stopped"
    /// was implied by not calling `play`; now a started client nobody fills runs
    /// dry, so a host that stops or pauses has to say so.
    ///
    /// This is a real stop and it is audible on this endpoint: 100 stop/play cycles
    /// produced one artefact per cycle, and the same 100 with the stop removed were
    /// clean. Use [`pause`](Self::pause) for a pause. Track change, exit and a
    /// deliberate stop all still pay that boundary -- this is not solved, it is
    /// cheaper to pay it only where it is meant.
    pub fn stop(&mut self) -> Result<()> {
        // SAFETY: same handle guarantee as `play`.
        let rc = unsafe { ffi::NAE_Stop(self.handle) };
        if rc == ffi::NAE_OK {
            Ok(())
        } else {
            Err(EngineError::from_code(rc).with_engine_message())
        }
    }

    /// Pause by feeding the device silence, keeping the stream running.
    ///
    /// Not a `stop`, because a pause expressed as a stop is audible here. Measured:
    /// 100 stop/play cycles gave one artefact per boundary; 100 pause/play cycles
    /// were clean. Only whether the client was stopped differed.
    ///
    /// The stream keeps running, so [`play`](Self::play) must keep being called:
    /// the silence is fed from inside the render loop, and a started client that
    /// nobody fills runs dry. A pause is therefore `pause()` then repeated `play()`.
    ///
    /// No source is consumed, so [`position`](Self::position) does not move and a
    /// pause of any length resumes on the same sample. Verified at 0.0000 ms of
    /// movement across 100 cycles.
    pub fn pause(&mut self) -> Result<()> {
        // SAFETY: same handle guarantee as `play`.
        let rc = unsafe { ffi::NAE_Pause(self.handle) };
        if rc == ffi::NAE_OK {
            Ok(())
        } else {
            Err(EngineError::from_code(rc).with_engine_message())
        }
    }

    /// Leave the paused state and go back to audio, from the same sample.
    pub fn resume(&mut self) -> Result<()> {
        // SAFETY: same handle guarantee as `play`.
        let rc = unsafe { ffi::NAE_Resume(self.handle) };
        if rc == ffi::NAE_OK {
            Ok(())
        } else {
            Err(EngineError::from_code(rc).with_engine_message())
        }
    }

    /// Where playback has reached, in seconds from the start of the file.
    ///
    /// Counts from the start of the track, not from the last seek, so this is
    /// what a progress bar should show.
    pub fn position(&self) -> f64 {
        // SAFETY: same handle guarantee as `play`.
        unsafe { ffi::NAE_Position(self.handle) }
    }

    /// How long the track is, in seconds. 0 when unknown (or for a tone).
    pub fn duration(&self) -> f64 {
        // SAFETY: same handle guarantee as `play`.
        unsafe { ffi::NAE_Duration(self.handle) }
    }

    /// Jump to `seconds` into the track.
    ///
    /// Approximate for compressed formats: the container lands on the nearest
    /// packet it can, so the result is near the target rather than exact. The
    /// reported [`position`](Self::position) afterwards is the position that was
    /// asked for, not the packet boundary that was reached — the difference is
    /// too small to show a user and too large to pretend does not exist.
    pub fn seek(&mut self, seconds: f64) -> Result<()> {
        if self.track.is_none() {
            return Err(EngineError::BadPayload(
                "cannot seek: this stream is a generated tone".into(),
            ));
        }
        // SAFETY: same handle guarantee as `play`.
        let rc = unsafe { ffi::NAE_Seek(self.handle, seconds) };
        if rc == ffi::NAE_OK {
            Ok(())
        } else {
            Err(EngineError::from_code(rc).with_engine_message())
        }
    }

    /// Render a sine and block until it finishes.
    ///
    /// Kept for the harnesses, which have no file to play. Identical to
    /// [`play`](Self::play) — the engine picks the source from the handle.
    pub fn play_sine_blocking(&mut self, milliseconds: i32) -> Result<()> {
        self.play(milliseconds)
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        // SAFETY: NAE_Close is documented as safe with null, and handles are
        // only ever null if the open failed — in which case no Stream exists.
        // Called exactly once, because Drop runs once.
        unsafe { ffi::NAE_Close(self.handle) };
    }
}

/// Reads a NUL-terminated UTF-8 string the engine points at, and copies it.
///
/// The copy is the whole point: the engine's buffers (and the static one behind
/// `NAE_GetLastError`) get overwritten by the next call, so holding the pointer
/// would be a use-after-overwrite.
unsafe fn copy_cstr(ptr: *const c_char) -> Result<String> {
    if ptr.is_null() {
        return Err(EngineError::NullPointer);
    }
    CStr::from_ptr(ptr)
        .to_str()
        .map(str::to_owned)
        .map_err(|_| EngineError::NotUtf8)
}

/// A `char*` the engine allocated, paired with its pointer so it is released on
/// every path out of the calling function.
struct OwnedEngineString(*mut c_char);

impl OwnedEngineString {
    fn new(ptr: *mut c_char) -> Self {
        OwnedEngineString(ptr)
    }

    /// Copies the engine's buffer into a Rust `String`.
    ///
    /// The copy happens here; the release happens in `Drop`, exactly once. An
    /// earlier version also called `NAE_FreeString` inline, which double-freed:
    /// the explicit call plus the one `Drop` makes when this function returns.
    /// The C++ harness never hit it because it read the buffer with a plain
    /// C-string helper, so this is precisely the class of bug the Rust layer
    /// exists to surface.
    fn take(self) -> Result<String> {
        if self.0.is_null() {
            return Err(EngineError::NullPointer);
        }
        // Copy before the free below. `Drop` runs at the end of this function,
        // so the buffer is still valid here.
        // SAFETY: the engine guarantees a NUL-terminated buffer when non-null.
        unsafe { copy_cstr(self.0) }
    }
}

impl Drop for OwnedEngineString {
    fn drop(&mut self) {
        // Reached only if `take` was never called, e.g. an early return.
        if !self.0.is_null() {
            // SAFETY: same guarantee as above; Drop runs once.
            unsafe { ffi::NAE_FreeString(self.0) };
            self.0 = ptr::null_mut();
        }
    }
}

/// Engine-wide capabilities, e.g. `{"backend":"wasapi",...}`.
pub fn capabilities() -> Result<String> {
    // SAFETY: returns a pointer to a static buffer in the DLL that stays valid
    // for the process lifetime; we copy it immediately and never free it.
    let ptr = unsafe { ffi::NAE_GetEngineCapabilities() };
    // Note: deliberately *not* wrapped in OwnedEngineString — this buffer is
    // static and must not be passed to NAE_FreeString.
    unsafe { copy_cstr(ptr) }
}

/// The last HRESULT the engine saw, formatted.
pub fn last_error() -> String {
    // SAFETY: static buffer, valid until the next engine call on this thread.
    // Falls back to a placeholder rather than propagating an error, since this
    // is only ever used to decorate an existing failure.
    unsafe { copy_cstr(ffi::NAE_GetLastError()) }.unwrap_or_else(|_| "(no error)".to_string())
}

/// List active render endpoints.
pub fn enumerate_devices() -> Result<Vec<Device>> {
    // SAFETY: returns a heap buffer the caller owns.
    let raw = OwnedEngineString::new(unsafe { ffi::NAE_EnumerateDevices() });
    let json = raw.take()?;

    serde_json::from_str(&json).map_err(|e| EngineError::BadPayload(e.to_string()))
}

/// The id of the endpoint Windows is currently using for console audio.
///
/// Resolved fresh on every call, so it follows the user changing their default
/// in Sound settings. This is the *right* default device for a player — it is
/// the endpoint people expect a music app to be able to take over — but it is
/// not a guarantee that exclusive mode will succeed: if anything else already
/// holds it, opening still fails, and `Device::exclusive` on the matching entry
/// from [`enumerate_devices`] is the honest signal about that.
pub fn default_device_id() -> Result<String> {
    // SAFETY: returns a heap buffer the caller owns, or null.
    let raw = OwnedEngineString::new(unsafe { ffi::NAE_GetDefaultDeviceId() });
    if raw.0.is_null() {
        return Err(EngineError::NullPointer);
    }
    raw.take()
}

/// The default endpoint as a full [`Device`].
///
/// Returns `None` when the default cannot be resolved, or when it is not in the
/// active endpoint list — which happens when the default points at a device that
/// is disabled or unplugged. The caller should treat that as "no default
/// available" rather than as an error worth surfacing.
pub fn default_device() -> Option<Device> {
    let id = default_device_id().ok()?;
    enumerate_devices().ok()?.into_iter().find(|d| d.id == id)
}

/// Open `device` exclusively at the device's own rate, with no conversion.
///
/// The simplest useful case, and what the harnesses use. For anything that plays
/// a file, use [`open_with`] with the file's rate and an explicit mode.
pub fn open_exclusive(device: &Device) -> Result<Stream> {
    open_with(device, 0, OutputMode::Device, Quality::High)
}

/// Open `device` exclusively, converting from `source_rate` to a rate chosen by
/// `mode`.
///
/// `source_rate` of 0 means "use the device's own rate" — the source is then the
/// same as the device, so no conversion is configured.
///
/// The returned [`Stream`] reports the whole signal path, including the honesty
/// flags, so nothing above this layer has to guess whether the output is
/// bit-perfect.
pub fn open_with(
    device: &Device,
    source_rate: u32,
    mode: OutputMode,
    quality: Quality,
) -> Result<Stream> {
    let device_id = std::ffi::CString::new(device.id.as_str())
        .map_err(|_| EngineError::BadPayload("device id contains an interior NUL".into()))?;

    let (mode_raw, fixed_rate) = mode.as_raw();

    let mut handle: *mut ffi::NAE_Handle = ptr::null_mut();
    let (mut rate, mut src, mut channels, mut bits, mut valid) = (0i32, 0i32, 0i32, 0i32, 0i32);
    let mut is_float = 0i32;
    let (mut resampling, mut dithered, mut bit_perfect) = (0i32, 0i32, 0i32);
    let mut latency_ms = 0.0f64;

    // SAFETY: all pointers are to live locals of the exact types the header
    // declares; the C string is NUL-terminated and outlives the call. Every
    // out-parameter is optional and the engine zeroes them before doing work.
    let rc = unsafe {
        ffi::NAE_OpenExclusive(
            device_id.as_ptr(),
            source_rate as i32,
            mode_raw,
            fixed_rate,
            quality.as_raw(),
            &mut handle,
            &mut rate,
            &mut src,
            &mut channels,
            &mut bits,
            &mut valid,
            &mut is_float,
            &mut resampling,
            &mut dithered,
            &mut bit_perfect,
            &mut latency_ms,
        )
    };

    if rc != ffi::NAE_OK {
        // The engine leaves `handle` null on every failure path, but rely on
        // nothing: close it defensively if it somehow is not.
        if !handle.is_null() {
            // SAFETY: defensive only; the engine's own contract is null here.
            unsafe { ffi::NAE_Close(handle) };
        }
        return Err(EngineError::from_code(rc));
    }

    if handle.is_null() {
        return Err(EngineError::BadPayload(
            "engine reported success with a null handle".into(),
        ));
    }

    Ok(Stream {
        handle,
        device_id: device.id.clone(),
        track: None,
        path: SignalPath {
            sample_rate: rate.max(0) as u32,
            source_rate: src.max(0) as u32,
            channels: channels.max(0) as u32,
            bits_per_sample: bits.max(0) as u32,
            valid_bits: valid.max(0) as u32,
            is_float: is_float != 0,
            latency_ms,
            resampling: resampling != 0,
            dithered: dithered != 0,
            bit_perfect: bit_perfect != 0,
            // 纯音那条路没有 share mode 可选——`NAE_OpenExclusiveSine` 只有一个入口，
            // 而且 exclusive 正是这个频率发生器当初被加进来要证明的东西
            // （见 nae.h 里 NAE_OpenExclusiveSine 的说明）。
            exclusive: true,
        },
    })
}

/// Open `file` and `device` together, exclusively.
///
/// One call rather than probe-then-open, because the device has to be configured
/// for the *file's* rate and that rate is only known once the file has been
/// read. A caller that supplied the rate from outside could get it wrong, and the
/// resulting `bit_perfect` flag would then describe a stream that was never what
/// was on disk.
///
/// The engine reads the file's own rate; `source_rate` in the request is ignored
/// for tracks, because a track's rate is a fact about the file rather than a
/// preference.
pub fn open_file(
    device: &Device,
    file: &str,
    mode: OutputMode,
    quality: Quality,
) -> Result<Stream> {
    open_file_with(device, file, mode, quality, ShareMode::Exclusive)
}

/// Open `file` and `device` in shared mode, so other programs can sound at once.
///
/// The same call as [`open_file`] with one difference: the Windows mixer is in
/// the path. That is the whole point of the route — it is what lets a browser or a
/// communication app keep working while the music plays — and it is also why the
/// engine reports `bit_perfect` false with reason `SharedMode` (7) no matter what
/// the rate negotiation concluded.
///
/// Whether Windows would in fact preserve the samples is **untested**. The product
/// decision (2026-10-01) is that shared mode does not pursue bit-perfectness, so
/// this claims nothing either way; the reason states the fact that is known, which
/// is that the mixer is downstream of us.
pub fn open_file_shared(
    device: &Device,
    file: &str,
    mode: OutputMode,
    quality: Quality,
) -> Result<Stream> {
    open_file_with(device, file, mode, quality, ShareMode::Shared)
}

/// Open `file` in the given share mode. The one body behind [`open_file`] and
/// [`open_file_shared`].
///
/// Exported as well, so a host that already has the mode as data (this
/// application's `RenderRequest` does) can pass it straight through instead of
/// branching over two function names that do the same thing.
pub fn open_file_with_share(
    device: &Device,
    file: &str,
    mode: OutputMode,
    quality: Quality,
    share: ShareMode,
) -> Result<Stream> {
    open_file_with(device, file, mode, quality, share)
}

/// Which share mode a stream is opened in.
///
/// Named rather than a bare `bool`, because a bool at a call site reads as a flag
/// and the reader has to go and find out what `true` means. The engine's capability
/// string calls these `exclusive` and `shared`, and so does this.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShareMode {
    Exclusive,
    Shared,
}

/// The one body behind both entry points.
///
/// Two functions over one implementation, so they cannot disagree about what the
/// implementation does. The C side has the same shape for the same reason.
fn open_file_with(
    device: &Device,
    file: &str,
    mode: OutputMode,
    quality: Quality,
    share: ShareMode,
) -> Result<Stream> {
    let device_id = std::ffi::CString::new(device.id.as_str())
        .map_err(|_| EngineError::BadPayload("device id contains an interior NUL".into()))?;
    let path_c = std::ffi::CString::new(file)
        .map_err(|_| EngineError::BadPayload("file path contains an interior NUL".into()))?;

    let (mode_raw, fixed_rate) = mode.as_raw();

    let mut handle: *mut ffi::NAE_Handle = ptr::null_mut();
    let (mut rate, mut src, mut channels, mut bits, mut valid) = (0i32, 0i32, 0i32, 0i32, 0i32);
    let mut is_float = 0i32;
    let (mut resampling, mut dithered, mut bit_perfect) = (0i32, 0i32, 0i32);
    let mut file_channels = 0i32;
    let (mut duration_ms, mut latency_ms) = (0.0f64, 0.0f64);

    // SAFETY: all pointers are to live locals of the exact types the header
    // declares; both C strings are NUL-terminated and outlive the call. Every
    // out-parameter is optional and the engine zeroes them before doing work.
    //
    // The two symbols take an identical argument list, so the whole call is
    // written once and the mode selects it. Writing it twice would be a chance for
    // the two to drift, which is the failure this pair of functions exists to
    // make impossible.
    let rc = unsafe {
        match share {
            ShareMode::Exclusive => ffi::NAE_OpenFile(
                device_id.as_ptr(),
                path_c.as_ptr(),
                mode_raw,
                fixed_rate,
                quality.as_raw(),
                &mut handle,
                &mut rate,
                &mut src,
                &mut channels,
                &mut bits,
                &mut valid,
                &mut is_float,
                &mut resampling,
                &mut dithered,
                &mut bit_perfect,
                &mut file_channels,
                &mut duration_ms,
                &mut latency_ms,
            ),
            ShareMode::Shared => ffi::NAE_OpenFileShared(
                device_id.as_ptr(),
                path_c.as_ptr(),
                mode_raw,
                fixed_rate,
                quality.as_raw(),
                &mut handle,
                &mut rate,
                &mut src,
                &mut channels,
                &mut bits,
                &mut valid,
                &mut is_float,
                &mut resampling,
                &mut dithered,
                &mut bit_perfect,
                &mut file_channels,
                &mut duration_ms,
                &mut latency_ms,
            ),
        }
    };

    if rc != ffi::NAE_OK {
        // The engine leaves `handle` null on every failure path, but rely on
        // nothing: close it defensively if it somehow is not.
        if !handle.is_null() {
            // SAFETY: defensive only; the engine's own contract is null here.
            unsafe { ffi::NAE_Close(handle) };
        }
        // Attach the engine's own words. "Invalid argument" tells a user nothing;
        // "this file is not audio" tells them what to do about it.
        return Err(EngineError::from_code(rc).with_engine_message());
    }

    if handle.is_null() {
        return Err(EngineError::BadPayload(
            "engine reported success with a null handle".into(),
        ));
    }

    Ok(Stream {
        handle,
        device_id: device.id.clone(),
        // 从引擎查询，而不是从 open 的 out-param 拼。
        //
        // out-param 里只有 channels 和 durationMs 两个文件字段，而采样率、位深、
        // 编码都在引擎手上却没有被带出来。拼装会留下三个永远是 0 的字段，而 0 在
        // 这个界面里读起来像「文件是 0 bit」而不是「没有人问过」。
        track: Some(query_track_info(handle)),
        path: SignalPath {
            sample_rate: rate.max(0) as u32,
            source_rate: src.max(0) as u32,
            channels: channels.max(0) as u32,
            bits_per_sample: bits.max(0) as u32,
            valid_bits: valid.max(0) as u32,
            is_float: is_float != 0,
            latency_ms,
            resampling: resampling != 0,
            dithered: dithered != 0,
            // A channel conversion is processing, so the engine clears this. It is
            // read from the out-parameter rather than recomputed here on purpose:
            // the engine is the only thing that knows the whole path, and a second
            // implementation of the rule would eventually disagree with the first.
            bit_perfect: bit_perfect != 0,
            // 从**请求的模式**来，不是从 `bit_perfect` 推的。
            //
            // 推是错的：一个恰好跑在源速率上的 shared 流 `bit_perfect` 也是 false，
            // 但原因不同——一个是混音器在下游，一个是重采样。界面的路线徽章要
            // 说的是「这条流以什么模式打开的」，那只有一个来源：调用方要的那个。
            exclusive: share == ShareMode::Exclusive,
        },
    })
}

/// Release this thread's COM apartment.
///
/// Only call once every [`Stream`] on this thread has been dropped. In the real
/// app the audio thread does this as it exits; nothing else should.
pub fn shutdown() {
    // SAFETY: contract is the caller's — see nae.h.
    unsafe { ffi::NAE_Shutdown() }
}

/// One reading of the live output chain, read from the engine rather than
/// remembered from open time.
///
/// The distinction is the point. `Stream::path` is captured once, when the
/// device is opened, and describes the chain as it was configured. The counters
/// mean nothing at that instant, the driver latency changes while a stream runs,
/// and a device that is unplugged mid-track changes everything. A panel fed from
/// the open-time snapshot shows a healthy zero-failure telemetry line forever,
/// on a stream that has since lost its device four times.
///
/// Fields the engine did not write are left at their default rather than read
/// anyway: `struct_size` is what says which ones exist, and a host built against
/// a newer header than the DLL it loaded has to be able to tell the difference
/// between "zero" and "never set".
pub(crate) fn query_signal_path(handle: *mut ffi::NAE_Handle) -> LiveSignalPath {
    // Declare the size this build expects, so a newer DLL that appended fields
    // reports a larger `struct_size` and `complete` stays honest. Without this
    // the engine writes `sizeof(its own struct)` over a zeroed field, which is
    // indistinguishable from "everything written".
    let mut raw = ffi::NAE_SignalPath {
        struct_size: std::mem::size_of::<ffi::NAE_SignalPath>() as u32,
        ..Default::default()
    };
    // SAFETY: `raw` is a live, correctly-sized `NAE_SignalPath` with its
    // `struct_size` set to this build's size. The engine copies out at most
    // `min(that, its own size)` bytes, so a DLL with fewer fields cannot write
    // past the buffer. A null handle yields a zeroed struct, so this is safe at
    // any time.
    unsafe { ffi::NAE_QuerySignalPath(handle, &mut raw) };

    // Anything the engine did not write stays zero. Comparing against the size we
    // asked for is the only way to know, and defaulting the rest is what stops a
    // stale DLL from being read as a live one.
    let wrote_all = raw.struct_size as usize >= std::mem::size_of::<ffi::NAE_SignalPath>();

    LiveSignalPath {
        source_rate: raw.source_rate.max(0) as u32,
        source_channels: raw.source_channels.max(0) as u32,
        sample_rate: raw.sample_rate.max(0) as u32,
        channels: raw.channels.max(0) as u32,
        bits_per_sample: raw.bits_per_sample.max(0) as u32,
        valid_bits: raw.valid_bits.max(0) as u32,
        is_float: raw.is_float != 0,
        resampling: raw.resampling != 0,
        dithered: raw.dithered != 0,
        bit_perfect: raw.bit_perfect != 0,
        // `//` not `///`: this is a struct *expression*, so a doc comment attaches
        // to nothing and the compiler says so. The field is documented properly on
        // `SignalPath` below.
        // The engine's own reason code, as a `NAE_REASON_*` value.
        reason: raw.reason,
        exclusive: raw.exclusive != 0,
        coerced_to_supported_rate: raw.coerced_to_supported_rate != 0,

        // The latency split, with `total` only meaningful when the driver
        // reported something. Adding a silent driver to a known buffer figure
        // would present "we do not know" as part of a number that looks measured.
        buffer_latency_ms: raw.buffer_latency_ms,
        driver_latency_ms: raw.driver_latency_ms,
        total_latency_ms: raw.total_latency_ms,
        buffer_frames: raw.buffer_frames.max(0) as u32,
        channel_delay_max_frames: raw.channel_delay_max_frames,

        callback_count: raw.callback_count,
        deadline_miss_count: raw.deadline_miss_count,
        peak_callback_ms: raw.peak_callback_ms,
        underrun_count: raw.underrun_count,
        drop_count: raw.drop_count,
        device_lost_count: raw.device_lost_count,
        recovery_count: raw.recovery_count,
        fifo_peak: raw.fifo_peak,
        written_peak: raw.written_peak,
        device_peak: raw.device_peak,
        requested_bits: raw.requested_bits as u32,
        granted_bits: raw.granted_bits as u32,
        pad_frame_count: raw.pad_frame_count,
        max_buffer_gap_ms: raw.max_buffer_gap_ms,
        max_decode_stall_ms: raw.max_decode_stall_ms,
        max_fill_ms: raw.max_fill_ms,
        max_late_ms: raw.max_late_ms,
        late_count: raw.late_count,
        worst_seam_ratio: raw.worst_seam_ratio,
        seam_slew_ref: raw.seam_slew_ref,
        seam_count: raw.seam_count,
        buffer_duration_ms: raw.buffer_duration_ms,
        device_rate_actual: raw.device_rate_actual,
        silence_frames_written: raw.silence_frames_written,
        seam: SeamDistribution {
            mean: raw.seam_mean,
            quantiles: raw.seam_quantiles,
            sample_count: raw.seam_sample_count,
            total_seams: raw.seam_count,
            histogram: raw.seam_histogram,
            histogram_len: raw.seam_histogram_max,
        },

        // `//` for the same reason as `reason` above. Documented on the struct.
        // False when the engine wrote a shorter struct than this binding knows
        // about, which means some fields above are defaults rather than readings.
        complete: wrote_all,
    }
}

/// 一段时域包络，节拍（BPM）检测的数据源。
///
/// # 为什么需要它，而不能从频谱反推
///
/// 节拍是**时域**现象：它存在于相邻样本之间的时间结构里。频谱 tap 给的 2048 个
/// bin 是一次不可逆的有损变换，从它反推包络在数学上做不到。
///
/// 早先 exclusive 路由上没有 BPM，原因正是采样根本不进 webview，而 `tempo.js`
/// 需要解码后的 PCM。频域有、时域没有——这不是「还没做」，是这两种 tap 回答的
/// 根本不是同一个问题。
///
/// # 与频谱 tap 共用同一个环形缓冲
///
/// 环形缓冲里本来就是时域样本，频谱是读它时做的变换。包络是同一份数据的另一种
/// 聚合，**不复制第二份**：两份环形缓冲意味着两处状态要保持同步，而它们唯一的
/// 区别只是聚合方式。
///
/// # 这些数字代表什么
///
/// `frames` 是每帧的**平均绝对值**（不是 dB，范围 0..1），`frame_hop_ms` 是帧间隔。
/// 拍长可以直接由帧数乘以帧间隔算出，调用方不需要再从采样率推算——推算就意味着
/// 两个地方各有一份关于「一帧是多久」的假设。
///
/// `frames_fed` 与频谱那边是同一个计数器，因为两者描述同一个环形缓冲。
#[derive(Debug, Clone, Default)]
pub struct Envelope {
    /// 每帧的平均绝对值，0..1。
    ///
    /// **只有有效的那一段，而且从最老的到最新。** 引擎那个环是 20.48 秒的滚动窗，
    /// 没写满的时候它交不出 4096 帧——这里不补零，直接短一点（`query_envelope`
    /// 按引擎报的 `frame_valid` 截断）。补零会让节拍估计器把零当成静音吃进去：
    /// 实测同一个窗给出的数在 72 ~ 200 之间跳，置信度从没超过 0.5。
    pub frames: Vec<f32>,
    /// 帧间隔，毫秒。与前端 `tempo.js` 的 `HOP_MS` 一致。
    pub frame_hop_ms: f64,
    /// 包络的采样率，即设备速率。
    pub sample_rate: u32,
    /// 环形缓冲喂过的帧总数，用来判断读数是不是新的。
    pub frames_fed: u64,
    /// 环的**容量**覆盖的真实时长，毫秒。`frames.len()` 可能短于它（还没写满时）。
    pub window_ms: f64,
    /// False when the engine wrote a shorter struct than this binding knows about,
    /// meaning the figures above are defaults rather than readings.
    pub complete: bool,
}

impl Envelope {
    /// 是否真的读到了包络。
    ///
    /// 与 `complete` 不同。空句柄返回的是格式完整的零值——`NAE_QueryEnvelope` 文档
    /// 说明空句柄不算错误——而那是「还没有流」的答案，不是一段静音的流。真实的
    /// 静音包络 `frames_fed` 非 0，两者可以分辨。
    pub fn has_data(&self) -> bool {
        !self.frames.is_empty() && self.frames_fed > 0
    }
}

/// 读取时域包络。
///
/// 和频谱一样是两次调用：帧数要等第一次返回才知道。帧数由引擎选的窗口长度决定，
/// 所以这是引擎的协议而不是为了方便。
///
/// 空句柄返回空的 `Envelope`（`frames_fed == 0`，`has_data` 报「无数据」）。轮询
/// 一个还没打开的流不是错误，而调用方是按定时器轮询的，不管流存不存在。
pub(crate) fn query_envelope(handle: *mut ffi::NAE_Handle) -> Envelope {
    // 第一次只问形状：`envelope` 为 null、capacity 为 0，引擎按文档把它当作
    // 「告诉我多大」处理，不会写入任何数据。
    let mut probe = ffi::NAE_EnvelopeInfo {
        struct_size: std::mem::size_of::<ffi::NAE_EnvelopeInfo>() as u32,
        ..Default::default()
    };
    // SAFETY: `probe` 是活的、正确尺寸的零值 `NAE_EnvelopeInfo`，null 指针加 0
    // 容量是文档规定的问法，空句柄按文档返回零值结构。
    unsafe { ffi::NAE_QueryEnvelope(handle, &mut probe, std::ptr::null_mut(), 0) };

    let frame_count = probe.frame_count as usize;
    if frame_count == 0 {
        return Envelope::default();
    }
    let mut frames = vec![0.0f32; frame_count];
    let mut info = ffi::NAE_EnvelopeInfo {
        struct_size: std::mem::size_of::<ffi::NAE_EnvelopeInfo>() as u32,
        ..Default::default()
    };
    // SAFETY: `frames` 是恰好 `frame_count` 个 float 的活分配，也就是传进去的容量，
    // 引擎不会写入超过容量的数据。
    let rc = unsafe {
        ffi::NAE_QueryEnvelope(handle, &mut info, frames.as_mut_ptr(), frame_count as i32)
    };
    if rc != 0 {
        return Envelope::default();
    }

    let wrote_all = info.struct_size as usize >= std::mem::size_of::<ffi::NAE_EnvelopeInfo>();
    // **只交有效的那一段，顺序由引擎保证（最老的在前）。**
    //
    // `frame_count` 是环的**容量**，`frame_valid` 是「有几个是真的」——两个数在环没写满
    // 之前不相等。交出一整条 4096 帧意味着尾部是一片零，而节拍估计器会把那些零当成静音
    // 一起喂进它的离散度里（实测：同一个窗给出的数在 72 ~ 200 之间跳）。
    //
    // 截断之后 `frames.len()` 就是真实帧数，调用方不必自己算——它也算不准：`frames_fed`
    // 数的是设备帧，和环里写了几帧之间隔着一个取整和一次分配时刻的差。
    frames.truncate(info.frame_valid as usize);
    Envelope {
        frames,
        frame_hop_ms: info.frame_hop_ms,
        sample_rate: info.sample_rate,
        frames_fed: info.frames_fed,
        window_ms: info.window_ms,
        complete: wrote_all,
    }
}

/// A reading of the engine's output spectrum tap.
///
/// This is the signal the *device is being fed*, at the rate it is being fed,
/// taken from inside the render loop after resampling. That is what distinguishes
/// it from the Web Audio route's spectrum, which reads the file at the file's
/// rate: on a DSD64 track opened at 192 kHz these two disagree by design, and
/// the disagreement is the interesting part.
///
/// `live` is what stops a stale reading being drawn as if it were current.
/// `frames_fed` advances while audio flows and stops when it does not, so a
/// caller comparing it against its own previous value can tell the difference.
/// A reader that ignores it will happily keep drawing the last window forever,
/// including through a pause -- where the tap does feed real silence, so a
/// *paused* reading is genuine silence and a *stopped* one is a leftover.
#[derive(Debug, Clone, Default)]
pub struct Spectrum {
    /// Linear magnitudes, `bin_count` of them, a full-scale sine reading about 1.0
    /// in its own bin under a Hann window.
    pub bins: Vec<f32>,
    pub fft_size: u32,
    pub sample_rate: u32,
    pub valid_bits: u32,
    pub peak: f64,
    pub rms: f64,
    pub frames_fed: u64,
    /// False when the engine wrote a shorter struct than this binding knows about,
    /// which means the figures above are defaults rather than readings.
    pub complete: bool,
}

impl Spectrum {
    /// Whether this reading carries data at all.
    ///
    /// Distinct from `complete`. A null handle yields a well-formed zeroed
    /// reading -- `NAE_QuerySpectrum` documents that a null handle is not an
    /// error -- and that is the "no stream" answer, not a silent stream.
    pub fn has_data(&self) -> bool {
        !self.bins.is_empty() && self.frames_fed > 0
    }
}

/// Read the output tap.
///
/// Two calls, because the bin count is not known until the first one returns.
/// That is the engine's protocol rather than a convenience: the count depends on
/// the window length, which the engine chooses.
///
/// A null handle yields an empty `Spectrum` with `frames_fed == 0`, which
/// `has_data` reports as no data. It is not an error to poll a stream that has not
/// opened, and the caller polls on a timer whether or not a stream exists.
pub(crate) fn query_spectrum(handle: *mut ffi::NAE_Handle) -> Spectrum {
    // First call: ask for the shape only. `bins` is null and `capacity` is zero,
    // which the engine treats as "tell me the size" and does not write to.
    let mut probe = ffi::NAE_SpectrumInfo {
        struct_size: std::mem::size_of::<ffi::NAE_SpectrumInfo>() as u32,
        ..Default::default()
    };
    // SAFETY: `probe` is a live, zeroed `NAE_SpectrumInfo` of exactly the size the
    // engine documents, and a null `bins` with zero capacity is the documented way
    // to ask for the shape. A null handle is documented to yield a zeroed struct.
    unsafe { ffi::NAE_QuerySpectrum(handle, &mut probe, std::ptr::null_mut(), 0) };

    let bin_count = probe.bin_count as usize;
    if bin_count == 0 {
        return Spectrum::default();
    }
    let mut bins = vec![0.0f32; bin_count];
    let mut info = ffi::NAE_SpectrumInfo {
        struct_size: std::mem::size_of::<ffi::NAE_SpectrumInfo>() as u32,
        ..Default::default()
    };
    // SAFETY: `bins` is a live allocation of exactly `bin_count` floats, which is
    // the capacity passed, and the engine writes no more than `capacity`.
    let rc =
        unsafe { ffi::NAE_QuerySpectrum(handle, &mut info, bins.as_mut_ptr(), bin_count as i32) };
    if rc != 0 {
        return Spectrum::default();
    }

    let wrote_all = info.struct_size as usize >= std::mem::size_of::<ffi::NAE_SpectrumInfo>();
    Spectrum {
        bins,
        fft_size: info.fft_size,
        sample_rate: info.sample_rate,
        valid_bits: info.valid_bits,
        peak: info.peak,
        rms: info.rms,
        frames_fed: info.frames_fed,
        complete: wrote_all,
    }
}

/// 整首波形的 peaks：交错的 min/max，每桶两个 float。
#[derive(Debug, Clone, Default)]
pub struct Peaks {
    /// 交错的 `[min, max]`，共 `bucket_count * 2` 个。
    pub pairs: Vec<f32>,
    /// 填了多少桶。0 表示没有可用的 peaks。
    pub bucket_count: u32,
    /// 覆盖了整个文件。
    pub complete: bool,
    /// 文件的时长，毫秒。
    pub duration_ms: f64,
    pub sample_rate: u32,
    pub channels: u32,
    /// 短窗的长度，毫秒。**0 = 下面那两个响度指标没有算。**
    ///
    /// 窗口的参数跟着数一起走（理由写在 `NAE_PeaksInfo` 那一段），所以界面要写
    /// 「这是多长的窗」时读它，而不是自己再写一个 400。
    pub metrics_window_ms: u32,
    /// 窗之间的步长，毫秒。
    pub metrics_hop_ms: u32,
    /// 短窗 RMS 电平的中位数，dBFS。`metrics_window_ms == 0` 时没有意义。
    pub loudness_db: f64,
    /// 短窗 RMS 电平的 p95 − p10，dB。口径见 `nae.cpp` 的 `computePeaksFromFile`。
    pub dynamic_range_db: f64,
    /// ITU-R BS.1770-4 / EBU R128 integrated loudness, LUFS.
    pub lufs_integrated: f64,
    /// Loudest 400 ms block, LUFS, ungated.
    pub lufs_momentary_max: f64,
    /// Loudest 3 s window, LUFS, ungated.
    pub lufs_short_term_max: f64,
    /// EBU Tech 3342 loudness range, LU.
    pub lufs_range: f64,
    /// False when the file was too short to measure; the LUFS values are then
    /// not numbers to display.
    pub lufs_valid: bool,
    /// False when the file is shorter than 3 s: the short-term max and the
    /// range are 0, and 0 is not a measurement.
    pub lufs_short_term_valid: bool,
    /// 非 0 表示这次**没有算出来**，而不是「算出来是空的」。
    ///
    /// 可能的值：`NAE_ERR_INVALID_ARGUMENT`（桶数/容量不对，或路径不是 UTF-8）、
    /// `NAE_ERR_CANNOT_OPEN`（打不开这个路径）。
    ///
    /// 必须和「`bucket_count == 0`」分开读：后者在成功时也可能出现（比如容器没报
    /// 时长）。把它们混成一个「没有数据」会让「打不开这个文件」被显示成「这首歌没有
    /// 波形」——一个关于音乐的陈述。
    pub error: i32,
}

impl Peaks {
    /// 是否真的拿到了波形。
    pub fn has_data(&self) -> bool {
        self.error == 0
            && self.bucket_count > 0
            && self.pairs.len() >= self.bucket_count as usize * 2
    }
}

/// 算出整首波形的 peaks。**阻塞**，而且每次都真的解一遍文件。
///
/// # 为什么收路径而不是 handle
///
/// 见 `ffi::NAE_ComputePeaks` 的文档：应用那条更严的约定是「stream 开/用/关都在
/// 同一个线程，音频相关的东西一个都不跨线程传」。handle 属于音频线程，随时可能被
/// `NAE_Close`；路径本来就是一个可以传的消息（`playing_path()` 已经在这么做）。
///
/// # 缓存
///
/// **这里不缓存，引擎也不缓存。** 调用方按路径缓存 —— 那比缓存到流上更好，因为流
/// 重开之后缓存还在。每 16 毫秒轮询一次的那种读法**不能直接用这个函数**，会每次都把
/// 整个文件解一遍。
///
/// # 路径必须是 UTF-8
///
/// FFmpeg 在 Windows 上按 UTF-8 → 宽字符自己转。悄悄做一次 lossy 转换会把中日文路径
/// 打成乱码，然后报「打不开」—— 那是关于**文件**的假陈述。所以不是 UTF-8 就直接
/// 报错，不猜。
pub fn compute_peaks(path: &std::path::Path, buckets: u32) -> Peaks {
    let bad = |code: i32| Peaks {
        error: code,
        ..Peaks::default()
    };
    if buckets == 0 {
        return bad(ffi::NAE_ERR_INVALID_ARGUMENT);
    }
    // 路径转换失败**不是**「打不开这个文件」，所以用 INVALID_ARGUMENT 而不是
    // CANNOT_OPEN 之外的码：调用方需要能分辨「这个路径我表达不了」和「引擎打不开它」。
    let as_str = match path.to_str() {
        Some(s) => s,
        None => return bad(ffi::NAE_ERR_INVALID_ARGUMENT),
    };
    let c_path = match std::ffi::CString::new(as_str) {
        Ok(c) => c,
        Err(_) => return bad(ffi::NAE_ERR_INVALID_ARGUMENT), // 里面有 NUL
    };

    let capacity = (buckets as usize) * 2;
    let mut out = vec![0.0f32; capacity];
    let mut info = ffi::NAE_PeaksInfo {
        struct_size: std::mem::size_of::<ffi::NAE_PeaksInfo>() as u32,
        ..Default::default()
    };
    // SAFETY: `c_path` 是一个活着的、NUL 结尾的 C 字符串；`info` 是一个活着的、
    // 清零的 `NAE_PeaksInfo`，大小就是引擎文档里那个；`out` 是一个有 `capacity` 个
    // float 的活分配，而传进去的容量就是它 —— 引擎文档说不超过 `capacity`，且在容量
    // 不足时**报错而不截断**。
    let rc = unsafe {
        ffi::NAE_ComputePeaks(
            c_path.as_ptr(),
            buckets,
            &mut info,
            out.as_mut_ptr(),
            capacity as i32,
        )
    };
    if rc != 0 {
        return bad(rc);
    }

    // 引擎说填了几桶就留几桶。正常情况下等于 `buckets`，但**按它说的来**而不是按
    // 请求的来：两者不一致时，信任请求的那个会让界面按错的桶数去画。
    let got = (info.bucket_count as usize).min(buckets as usize) * 2;
    out.truncate(got);

    Peaks {
        pairs: out,
        bucket_count: info.bucket_count,
        complete: info.complete != 0,
        duration_ms: info.duration_ms,
        sample_rate: info.sample_rate,
        channels: info.channels,
        metrics_window_ms: info.metrics_window_ms,
        metrics_hop_ms: info.metrics_hop_ms,
        loudness_db: info.loudness_db,
        dynamic_range_db: info.dynamic_range_db,
        lufs_integrated: info.lufs_integrated,
        lufs_momentary_max: info.lufs_momentary_max,
        lufs_short_term_max: info.lufs_short_term_max,
        lufs_range: info.lufs_range,
        lufs_valid: info.lufs_valid != 0,
        lufs_short_term_valid: info.lufs_short_term_valid != 0,
        error: 0,
    }
}

/// One path-based analysis: format, full tags, ReplayGain, loudness, dynamic
/// range, peaks and BPM.
///
/// Separate from [`Peaks`] because this is the whole analysis plane, while
/// `Peaks` is the legacy single-purpose call. Both are path-based and
/// device-free. The host owns the cache; `analyzer_version` is part of its key.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Analysis {
    pub analyzer_version: u32,
    pub complete: bool,
    pub bucket_count: u32,
    pub channels: u32,
    pub sample_rate: u32,
    pub bits_per_sample: u32,
    pub duration_ms: u32,
    pub codec: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub track: String,
    pub disc: String,
    pub year: String,
    pub genre: String,
    pub composer: String,
    pub comment: String,
    pub bitrate_kbps: u32,
    pub bitrate_from_tag: bool,
    pub rg_track_valid: bool,
    pub rg_track_gain_db: f64,
    pub rg_track_peak: f64,
    pub rg_album_valid: bool,
    pub rg_album_gain_db: f64,
    pub rg_album_peak: f64,
    pub loudness_db: f64,
    pub dynamic_range_db: f64,
    pub metrics_window_ms: u32,
    pub metrics_hop_ms: u32,
    pub bpm: f64,
    pub bpm_confidence: f64,
    pub bpm_valid: bool,
    /// ITU-R BS.1770-4 integrated loudness, LUFS.
    pub lufs_integrated: f64,
    pub lufs_momentary_max: f64,
    pub lufs_short_term_max: f64,
    /// EBU Tech 3342 loudness range, LU.
    pub lufs_range: f64,
    /// False when the file was too short to measure.
    pub lufs_valid: bool,
    /// False when the file is shorter than 3 s.
    pub lufs_short_term_valid: bool,
    /// Interleaved `[min, max]`, `bucket_count * 2` values.
    pub peaks: Vec<f32>,
    /// 0 on success; the engine's error code otherwise. 0 with no peaks can mean
    /// a successful analysis of a container with no duration.
    pub error: i32,
}

impl Analysis {
    pub fn has_data(&self) -> bool {
        self.error == 0
            && self.bucket_count > 0
            && self.peaks.len() >= self.bucket_count as usize * 2
    }
}

fn fixed_string(raw: &[c_char]) -> String {
    let bytes: &[u8] = {
        let ptr = raw.as_ptr();
        let len = raw.len();
        // SAFETY: `ptr` points at this struct's own initialised `c_char` values.
        unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), len) }
    };
    let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

/// The analysis algorithm version, read from the engine rather than restated.
pub fn analyzer_version() -> u32 {
    // SAFETY: no arguments, no memory ownership; a pure constant read.
    unsafe { ffi::NAE_GetAnalyzerVersion() }
}

/// Decode `path` once and return the analysis plane.
///
/// **Blocking** and device-free; call from a worker, never the UI thread. The
/// host owns the cache. A path that is not UTF-8, contains NUL, or cannot be
/// opened returns an `Analysis` with `error` set, not a panic.
pub fn analyze_file(path: &std::path::Path, buckets: u32) -> Analysis {
    let bad = |code: i32| Analysis {
        error: code,
        ..Analysis::default()
    };
    if buckets == 0 {
        return bad(ffi::NAE_ERR_INVALID_ARGUMENT);
    }
    let as_str = match path.to_str() {
        Some(s) => s,
        None => return bad(ffi::NAE_ERR_INVALID_ARGUMENT),
    };
    let c_path = match std::ffi::CString::new(as_str) {
        Ok(c) => c,
        Err(_) => return bad(ffi::NAE_ERR_INVALID_ARGUMENT),
    };

    let capacity = buckets as usize * 2;
    let mut out = vec![0.0f32; capacity];
    let mut info = ffi::NAE_AnalysisInfo {
        struct_size: std::mem::size_of::<ffi::NAE_AnalysisInfo>() as u32,
        ..Default::default()
    };
    let mut written = 0i32;
    // SAFETY: `c_path` is a live NUL-terminated C string; `info` is a live,
    // zeroed struct of the declared size; `out` has exactly `capacity` floats,
    // and the engine rejects a short buffer instead of truncating.
    let rc = unsafe {
        ffi::NAE_AnalyzeFile(
            c_path.as_ptr(),
            buckets,
            &mut info,
            out.as_mut_ptr(),
            capacity as i32,
            &mut written,
        )
    };
    if rc != 0 {
        return bad(rc);
    }

    let got = (info.bucket_count as usize).min(buckets as usize) * 2;
    out.truncate(got);
    Analysis {
        analyzer_version: info.analyzer_version,
        complete: info.complete != 0,
        bucket_count: info.bucket_count,
        channels: info.channels.max(0) as u32,
        sample_rate: info.sample_rate.max(0) as u32,
        bits_per_sample: info.bits_per_sample.max(0) as u32,
        duration_ms: info.duration_ms.max(0) as u32,
        codec: fixed_string(&info.codec),
        title: fixed_string(&info.title),
        artist: fixed_string(&info.artist),
        album: fixed_string(&info.album),
        album_artist: fixed_string(&info.album_artist),
        track: fixed_string(&info.track),
        disc: fixed_string(&info.disc),
        year: fixed_string(&info.year),
        genre: fixed_string(&info.genre),
        composer: fixed_string(&info.composer),
        comment: fixed_string(&info.comment),
        bitrate_kbps: info.bitrate_kbps,
        bitrate_from_tag: info.bitrate_from_tag != 0,
        rg_track_valid: info.rg_track_valid != 0,
        rg_track_gain_db: info.rg_track_gain_db,
        rg_track_peak: info.rg_track_peak,
        rg_album_valid: info.rg_album_valid != 0,
        rg_album_gain_db: info.rg_album_gain_db,
        rg_album_peak: info.rg_album_peak,
        loudness_db: info.loudness_db,
        dynamic_range_db: info.dynamic_range_db,
        metrics_window_ms: info.metrics_window_ms,
        metrics_hop_ms: info.metrics_hop_ms,
        bpm: info.bpm,
        bpm_confidence: info.bpm_confidence,
        bpm_valid: info.bpm_valid != 0,
        lufs_integrated: info.lufs_integrated,
        lufs_momentary_max: info.lufs_momentary_max,
        lufs_short_term_max: info.lufs_short_term_max,
        lufs_range: info.lufs_range,
        lufs_valid: info.lufs_valid != 0,
        lufs_short_term_valid: info.lufs_short_term_valid != 0,
        peaks: out,
        error: 0,
    }
}

/// A live reading of the chain, including the counters.
///
/// Separate from [`SignalPath`] because the two are read at different times and
/// mean different things: `SignalPath` is configuration, read once at open, and
/// this is a sample of a running system. Merging them would invite the UI to
/// treat a counter as a property of the stream.
#[derive(Debug, Clone, Copy, Default)]
pub struct LiveSignalPath {
    pub source_rate: u32,
    pub source_channels: u32,
    pub sample_rate: u32,
    pub channels: u32,
    pub bits_per_sample: u32,
    pub valid_bits: u32,
    pub is_float: bool,
    pub resampling: bool,
    pub dithered: bool,
    pub bit_perfect: bool,
    /// A `NAE_REASON_*` value. The frontend's reason table is keyed on this.
    pub reason: i32,
    pub exclusive: bool,
    /// The listener's setting did not choose this rate; the driver did.
    pub coerced_to_supported_rate: bool,

    /// Audio queued and not yet heard.
    pub buffer_latency_ms: f64,
    /// The device's own reported latency, downstream of us. Zero when unknown.
    pub driver_latency_ms: f64,
    pub total_latency_ms: f64,
    /// Buffer latency in frames, which is the figure that scales with rate.
    pub buffer_frames: u32,
    /// The per-channel delay ring the render loop is applying, in frames. One
    /// more than the largest delay, so a delay equal to it still delays.
    pub channel_delay_max_frames: i32,

    pub callback_count: u64,
    pub deadline_miss_count: u64,
    pub peak_callback_ms: f64,
    pub underrun_count: u64,
    pub drop_count: u64,
    pub device_lost_count: u64,
    pub recovery_count: u64,

    /// Measured level out of the render fifo, and measured level as written into
    /// the device's buffer. `fifo_peak` is in the [-1, 1] domain; `written_peak` is
    /// a fraction of the negotiated container's full scale, so the two are directly
    /// comparable and a gap between them is the level going missing.
    pub fifo_peak: f32,
    pub written_peak: f32,

    /// Peak read back from the device buffer and decoded as the device decodes it.
    ///
    /// written_peak is computed before packing and cannot see a wrong shift or
    /// offset. This one looks at the bytes that are actually in the buffer.
    pub device_peak: f32,
    /// `wBitsPerSample` as requested and as granted by `Initialize`. They differ
    /// when the device changed the depth, which is the case where the write loop
    /// packs the wrong number of bytes per sample.
    pub requested_bits: u32,
    pub granted_bits: u32,

    /// Device frames the render loop had to invent because the fifo ran dry.
    ///
    /// The decoder runs on the same thread that drives WASAPI, so a decode that
    /// takes longer than the buffer it is filling starves the device. This is the
    /// number that makes that visible instead of presenting as "burbling".
    pub pad_frame_count: u64,

    /// Worst wall-clock gap between consecutive device-buffer writes, and the
    /// buffer's own duration.
    ///
    /// The first exceeding the second is a missed deadline: the samples were
    /// correct and the buffer still arrived after the device had run dry waiting.
    /// Nothing else in this struct can tell those two apart.
    pub max_buffer_gap_ms: f64,
    /// Worst time the render thread spent decoding from inside the device period.
    pub max_decode_stall_ms: f64,
    /// Worst time from reading the padding to handing the buffer over.
    pub max_fill_ms: f64,
    /// Worst buffer's lateness against the runway the device allowed, and the count.
    pub max_late_ms: f64,
    pub late_count: i64,
    /// Worst step across a device-buffer boundary, over the signal's peak.
    ///
    /// Compare with seam_slew_ref: a waveform that continued has a seam step of
    /// the same order as its own slew, and a break is several times larger. This is
    /// the instrument the bubbling needed, because that fault lived between calls
    /// rather than inside one.
    pub worst_seam_ratio: f64,
    /// The signal's own largest adjacent-sample step, normalised the same way.
    pub seam_slew_ref: f64,
    /// Device-buffer seams examined.
    pub seam_count: u64,
    pub buffer_duration_ms: f64,

    /// Device frames delivered per second of wall clock.
    ///
    /// Below the negotiated rate, the device is being drained more slowly than real
    /// time. The fifo accounting balances either way, so no other figure here can
    /// tell those two apart.
    ///
    /// Counts silence handed over during a pause. The claim is device frames per
    /// second of wall clock, and the device drains silence at the rate it drains
    /// audio. Excluding it made a half-paused run read half speed, which is a claim
    /// about playback that was not true; whether a pause happened is
    /// [`silence_frames_written`]'s job, not this field's.
    pub device_rate_actual: f64,

    /// Device frames handed over as silence because the stream was paused.
    ///
    /// Reported apart from [`device_rate_actual`] so a pause is visible without being
    /// folded into the rate. An interface that shows one figure for both would
    /// report a pause as correctly-timed music.
    pub silence_frames_written: u64,

    /// The seam ratio's distribution, not just its maximum. See [`SeamDistribution`].
    pub seam: SeamDistribution,

    /// Whether the engine wrote every field this binding knows about.
    pub complete: bool,
}

/// The distribution of the buffer-seam ratio.
///
/// # Why a maximum is not enough
///
/// The engine has always reported the *worst* seam ratio -- the largest step across
/// a device-buffer boundary, normalised by the signal's peak. The values recorded
/// across sessions were 0.95, 1.04, 2.63, 3.46 and 11.68, and nothing said which of
/// those was a defect.
///
/// A maximum is one sample out of thousands. 11.68 is a click if the median is 0.05
/// and an ordinary loud passage if the median is 9, and the maximum alone cannot
/// tell those apart -- so it cannot answer the only question anyone wants answered,
/// which is whether the seam needs fixing.
///
/// # What these numbers are
///
/// `quantiles` runs from the minimum to the maximum of a *retained prefix* of the
/// seams, capped by the engine so the memory is bounded. `sample_count` says how
/// many that prefix holds and `total_seams` how many seams there were in all, so a
/// caller can always see whether the prefix is the whole set. The last quantile is
/// the maximum of the prefix, so comparing it against the maximum of all of them
/// says whether anything was dropped.
///
/// The histogram is log-spaced over two decades on each side of 1.0 because the ratio
/// spans that range between a clean stream and a broken one.
#[derive(Debug, Clone, Copy)]
pub struct SeamDistribution {
    /// Mean over every seam, not just the retained prefix.
    pub mean: f64,
    /// Eight quantiles of the retained prefix: min, 12.5%, 25%, 37.5%, 50%, 62.5%,
    /// 75%, max.
    pub quantiles: [f64; 8],
    /// How many seams the quantiles were computed from.
    pub sample_count: u32,
    /// Seams examined in total.
    pub total_seams: u64,
    /// Log-spaced counts, -2..+2 decades in 0.1-decade buckets.
    pub histogram: [u64; 41],
    /// Valid bucket count, so a reader need not hard-code 41.
    pub histogram_len: u32,
}

impl Default for SeamDistribution {
    /// Zeroed by hand, because `[u64; 41]` is past the array lengths for which this
    /// toolchain implements `Default`. Every field is a number, so zero is valid for
    /// all of them.
    fn default() -> Self {
        Self {
            mean: 0.0,
            quantiles: [0.0; 8],
            sample_count: 0,
            total_seams: 0,
            histogram: [0u64; 41],
            histogram_len: 0,
        }
    }
}

impl SeamDistribution {
    /// Whether the seam looks like a discontinuity rather than the material.
    ///
    /// The comparison is against the signal's own slew, which the engine already
    /// measures and reports as `seam_slew_ref`. A seam that is *within* the
    /// waveform's own fastest step is a continuation of the music; one several
    /// times larger is a break.
    ///
    /// This is deliberately a comparison against a measured reference rather than a
    /// fixed threshold. A constant like "2.0" would be a guess about material
    /// nobody here has characterised, and it would be wrong for anything with a
    /// percussive transient.
    pub fn looks_continuous(&self, worst_ratio: f64, signal_slew: f64) -> bool {
        if signal_slew <= 0.0 {
            // No reference to compare against. Returning `true` would claim
            // continuity from nothing, which is the failure this whole struct exists
            // to avoid.
            return false;
        }
        worst_ratio <= signal_slew
    }
}

impl LiveSignalPath {
    /// Whether the stream is in trouble. Counters are the only evidence.
    pub fn healthy(&self) -> bool {
        self.underrun_count == 0
            && self.drop_count == 0
            && self.deadline_miss_count == 0
            && self.device_lost_count == 0
    }
}

/// The rates the engine measured this device will open in exclusive mode.
///
/// Empty means "not probed", which is a different statement from "this device
/// supports nothing". The engine distinguishes the two and so does this: a caller
/// that treats an empty list as a refusal will refuse to play on a machine where
/// playback works, which is the failure this was added to remove.
pub(crate) fn query_supported_rates(handle: *mut ffi::NAE_Handle) -> Vec<u32> {
    let mut count: u32 = 0;
    // SAFETY: out-params are live and correctly typed; the engine writes no more
    // than `capacity` entries and reports the true total separately.
    let rc = unsafe { ffi::NAE_QuerySupportedRates(handle, std::ptr::null_mut(), 0, &mut count) };
    if rc != ffi::NAE_OK || count == 0 {
        return Vec::new();
    }

    let mut rates = vec![0u32; count as usize];
    let mut got: u32 = 0;
    // SAFETY: `rates` has exactly `count` elements, which is the capacity passed.
    unsafe { ffi::NAE_QuerySupportedRates(handle, rates.as_mut_ptr(), count, &mut got) };
    rates.truncate(got as usize);
    rates
}

/// 读文件标签。
///
/// 空句柄或没有解码器时返回全空的 `Tags`，由调用方按「没有」处理。
pub(crate) fn query_tags(handle: *mut ffi::NAE_Handle) -> Tags {
    let mut raw = ffi::NAE_Tags {
        struct_size: std::mem::size_of::<ffi::NAE_Tags>() as u32,
        ..Default::default()
    };
    // SAFETY: `raw` is a live, correctly-sized, zeroed `NAE_Tags`, and a null
    // handle is documented to yield a zeroed struct rather than an error.
    unsafe { ffi::NAE_QueryTags(handle, &mut raw) };

    Tags {
        year: raw.field(&raw.year).to_string(),
        genre: raw.field(&raw.genre).to_string(),
        album: raw.field(&raw.album).to_string(),
        artist: raw.field(&raw.artist).to_string(),
        bitrate_kbps: raw.bitrate_kbps,
        // 引擎侧算出来的，绑定看不到。默认 true——而这个默认是**诚实的**，因为
        // `bitrate_kbps == 0` 表示没有码率，此时「来自标签」和「来自实测」都不
        // 适用，调用方按 0 处理而不问来源。
        bitrate_from_tag: true,
    }
}

/// 读文件自己的元数据。
///
/// 与 [`query_signal_path`] 分开，因为两者回答不同的问题：那个说设备正在被喂什么，
/// 这个说用户的文件是什么。在 Hi-Res 和 DSD 上它们必然不同——352.8 kHz 的 DSD64
/// 文件在 192 kHz 设备上是 `resampling=1`，而**只有这个查询**能告诉界面源速率是
/// 352800。之前界面上根本没有这个数字，于是 fact strip 只能显示设备侧。
///
/// 空句柄返回全 0 的默认值。0 位深和「没有文件」必须能分辨：前者不存在，后者只是
/// 还没打开——把两者混成「不知道」就会让一个 0 看起来像一次测量结果。
pub(crate) fn query_track_info(handle: *mut ffi::NAE_Handle) -> TrackInfo {
    let mut raw = ffi::NAE_TrackInfo {
        struct_size: std::mem::size_of::<ffi::NAE_TrackInfo>() as u32,
        ..Default::default()
    };
    // SAFETY: `raw` is a live, correctly-sized, zeroed `NAE_TrackInfo`, and a null
    // handle is documented to yield a zeroed struct rather than an error. The engine
    // writes at most `sizeof(NAE_TrackInfo)` bytes, all of which are this field's.
    unsafe { ffi::NAE_QueryTrackInfo(handle, &mut raw) };

    // A shorter struct means an older DLL that never wrote this field, so the codec
    // is reported as unknown rather than as whatever the zeroed buffer happened to
    // hold. Zeroed happens to be right here, but relying on that is how a future
    // field would silently read as "none reported" instead of "not supported".
    let wrote_all = raw.struct_size as usize >= std::mem::size_of::<ffi::NAE_TrackInfo>();
    TrackInfo {
        channels: raw.channels.max(0) as u32,
        duration_ms: raw.duration_ms.max(0) as f64,
        sample_rate: raw.sample_rate.max(0) as u32,
        bits_per_sample: raw.bits_per_sample.max(0) as u32,
        codec: if wrote_all {
            raw.codec_str().to_string()
        } else {
            String::new()
        },
    }
}
