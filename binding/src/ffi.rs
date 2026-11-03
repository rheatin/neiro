// Raw FFI declarations for the audio engine's C ABI.
//
// These mirror `nae.h` exactly. Nothing here is safe to call directly -- the
// pointers, the caller-owned strings and the error codes are all handled by the
// safe wrappers in [`crate::engine`]. This module exists so that the mapping
// from C to Rust is written down in one place and can be checked by eye
// against the header.
//
// # Hand-written rather than bindgen
//
// bindgen would need libclang (a large extra install) and a build-time step.
// For a dozen functions whose signatures are stable and already declared
// `noexcept` in the header, declaring them by hand is auditable in a way a
// generated file is not: if the header changes, this file stops compiling
// rather than silently regenerating. Revisit once the ABI grows past roughly a
// dozen functions or starts returning structs.

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_void};

// Error codes, matching the `NAE_ERR_*` defines in `nae.h`.
pub const NAE_OK: i32 = 0;
pub const NAE_ERR_COM: i32 = 1;
pub const NAE_ERR_NO_DEVICE: i32 = 2;
pub const NAE_ERR_NO_NATIVE_FORMAT: i32 = 3;
pub const NAE_ERR_ACTIVATE: i32 = 4;
pub const NAE_ERR_INITIALIZE: i32 = 5;
pub const NAE_ERR_NO_MEMORY: i32 = 6;
pub const NAE_ERR_NOT_OPEN: i32 = 7;
// NAE_ERR_DEVICE_LOST = 10.
//
// Hand-maintained alongside the header rather than generated from it, so the
// number is duplicated in two places by necessity. They have to stay equal: the
// engine compares against this value to decide that a lost device is not a
// generic failure, and a mismatch would turn a recoverable pause into a stream
// that gets thrown away. The value is fixed in nae.h and asserted by
// `codes_match_the_header` in the tests, because "both sides were edited and one
// was forgotten" is the failure mode a duplicated constant actually has.
pub const NAE_ERR_DEVICE_LOST: i32 = 10;
pub const NAE_ERR_INVALID_ARGUMENT: i32 = 8;
pub const NAE_ERR_CANNOT_OPEN: i32 = 9;
/// The caller's cancellation flag was raised. Not a failure: the result is
/// incomplete and must be discarded, not cached or shown.
pub const NAE_ERR_CANCELLED: i32 = 13;

// Output rate modes, matching `NAE_OUTPUT_*` in `nae.h`.
pub const NAE_OUTPUT_NATIVE: i32 = 0;
pub const NAE_OUTPUT_DEVICE: i32 = 1;
pub const NAE_OUTPUT_FIXED: i32 = 2;

// Resampler quality tiers, matching `NAE_QUALITY_*` in `nae.h`.
pub const NAE_QUALITY_FAST: i32 = 0;
pub const NAE_QUALITY_HIGH: i32 = 1;
pub const NAE_QUALITY_VERY_HIGH: i32 = 2;

// Opaque engine handle. Never dereferenced from Rust -- the engine owns it and
// [`NAE_Close`] destroys it.
#[repr(C)]
pub struct NAE_Handle {
    _private: [u8; 0],
}

// Engine-wide capabilities, as a static JSON string. Never NULL, never freed.
extern "C" {
    pub fn NAE_GetEngineCapabilities() -> *const c_char;
}

// The last HRESULT seen on this thread, formatted. Never NULL. Points at a
// static buffer inside the DLL, so it must be copied before the next call.
extern "C" {
    pub fn NAE_GetLastError() -> *const c_char;
}

// The raw HRESULT behind the last failure. `0x8889000F` is
// `AUDCLNT_E_ENDPOINT_CREATE_FAILED`, `0x88890008` is
// `AUDCLNT_E_UNSUPPORTED_FORMAT`.
extern "C" {
    pub fn NAE_GetLastHresult() -> i32;
}

// Active render endpoints as a JSON array. Returns a buffer the **caller owns**;
// it must be released with [`NAE_FreeString`], never with Rust's deallocator.
// Returns null on failure (including OOM).
extern "C" {
    pub fn NAE_EnumerateDevices() -> *mut c_char;
}

// Release a string returned by this engine. Must be called on the same side
// that produced it, which is why the wrappers always funnel through here.
extern "C" {
    pub fn NAE_FreeString(s: *mut c_char);
}

// The endpoint Windows currently uses for console audio, as a raw id string.
// Caller owns the buffer; release with [`NAE_FreeString`]. Null on failure.
extern "C" {
    pub fn NAE_GetDefaultDeviceId() -> *mut c_char;
}

// Open `device_id` in exclusive mode, converting from `source_rate` to a rate
// chosen by `output_mode`.
//
// Every out-parameter is optional (pass null to skip) and is zeroed before any
// work starts, so a failed call still leaves the caller with defined values.
// On success `*out` owns a handle that must be released with [`NAE_Close`].
//
// `bit_perfect` out-param is the one the UI must consult before claiming
// bit-perfect output: resampling or dithering both make the claim false, and a
// player that says otherwise is lying.
#[allow(clippy::too_many_arguments)]
extern "C" {
    pub fn NAE_OpenExclusive(
        device_id: *const c_char,
        source_rate: i32,
        output_mode: i32,
        fixed_rate: i32,
        quality: i32,
        out: *mut *mut NAE_Handle,
        sample_rate: *mut i32,
        source_rate_out: *mut i32,
        channels: *mut i32,
        bits_per_sample: *mut i32,
        valid_bits: *mut i32,
        is_float: *mut i32,
        resampling: *mut i32,
        dithered: *mut i32,
        bit_perfect: *mut i32,
        latency_ms: *mut f64,
    ) -> i32;
}

// Open `device_id` in exclusive mode at the device's own native rate.
//
// The earlier single-purpose entry point, kept for the harnesses. Equivalent to
// [`NAE_OpenExclusive`] with `source_rate = 0` and `NAE_OUTPUT_DEVICE`.
extern "C" {
    pub fn NAE_OpenExclusiveSine(
        device_id: *const c_char,
        out: *mut *mut NAE_Handle,
        sample_rate: *mut i32,
        channels: *mut i32,
        bits_per_sample: *mut i32,
        valid_bits: *mut i32,
        is_float: *mut i32,
        latency_ms: *mut f64,
    ) -> i32;
}

// Render `milliseconds` of a sine at the handle's source rate, converting and
// dithering as configured at open time. Blocks.
extern "C" {
    pub fn NAE_PlaySine(handle: *mut NAE_Handle, milliseconds: i32) -> i32;
}

// Stop and release a handle. Safe to call with null.
extern "C" {
    pub fn NAE_Close(handle: *mut NAE_Handle);
}

// One reading of the output chain, its latency, and its fault counters.
//
// `struct_size` is first and is filled in by the engine, so this binding can tell
// which fields the DLL it loaded actually wrote. Without it, a host built against
// a newer header reads past the end of what was written and reports garbage as
// telemetry -- which is worse than reporting nothing, because it looks real.
//
// Mirrors `NAE_SignalPath` in nae.h field for field and in order. The layout is
// ABI: `#[repr(C)]`, and the field order is not free to change.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_SignalPath {
    pub struct_size: u32,
    pub source_rate: i32,
    pub source_channels: i32,
    pub sample_rate: i32,
    pub channels: i32,
    pub bits_per_sample: i32,
    pub valid_bits: i32,
    pub is_float: i32,
    pub resampling: i32,
    pub dithered: i32,
    pub bit_perfect: i32,
    /// A `NAE_REASON_*` value, or 0 when bit-perfect.
    pub reason: i32,
    pub exclusive: i32,
    pub buffer_latency_ms: f64,
    pub driver_latency_ms: f64,
    pub total_latency_ms: f64,
    pub buffer_frames: i32,
    pub callback_count: u64,
    pub deadline_miss_count: u64,
    pub peak_callback_ms: f64,
    pub underrun_count: u64,
    pub drop_count: u64,
    pub device_lost_count: u64,
    pub recovery_count: u64,
    pub coerced_to_supported_rate: i32,
    /// Largest absolute sample taken out of the render fifo, and largest absolute
    /// value written into the device buffer as a fraction of that container's full
    /// scale. Measured in the render loop because "the sound is too quiet" cannot
    /// be located by reading the code -- every layer looks right.
    pub fifo_peak: f32,
    pub written_peak: f32,
    /// Peak read back from the device buffer and decoded as the device decodes it.
    pub device_peak: f32,
    /// `wBitsPerSample` asked for, and as granted by `Initialize`.
    pub requested_bits: i32,
    pub granted_bits: i32,
    /// Device frames invented because the render fifo ran dry mid-buffer. Non-zero
    /// means the decoder is not keeping up in real time.
    pub pad_frame_count: u64,
    /// Worst wall-clock gap between consecutive device-buffer writes.
    pub max_buffer_gap_ms: f64,
    /// The buffer's own duration. Gap exceeding duration is a missed deadline.
    ///
    /// Order note: this sits between max_buffer_gap_ms and max_decode_stall_ms
    /// because that is where it sits in nae.h. This struct is read field-by-field
    /// by position, so moving a field here without moving it there silently
    /// renames every figure after it. It already happened once.
    pub buffer_duration_ms: f64,
    /// Worst time the render thread spent inside topUp(), in milliseconds.
    pub max_decode_stall_ms: f64,
    /// Worst time from reading the padding to handing the buffer over.
    pub max_fill_ms: f64,
    /// Worst buffer's lateness against its runway, and how many were late.
    pub max_late_ms: f64,
    pub late_count: i64,
    /// Worst step across a device-buffer boundary, over the signal's peak.
    pub worst_seam_ratio: f64,
    /// The signal's own largest adjacent-sample step, for comparison.
    pub seam_slew_ref: f64,
    /// Device-buffer seams examined.
    pub seam_count: u64,
    /// Device frames delivered per second of wall clock.
    pub device_rate_actual: f64,
    /// Device frames handed over as silence because the stream was paused.
    ///
    /// Deliberately not folded into `device_rate_actual`: a pause is elapsed time
    /// for the device but it is not playback, and merging them reports a long pause
    /// as a long stretch of correctly-timed music.
    pub silence_frames_written: u64,
    // ---- the seam ratio's distribution ----
    //
    // Appended at the end, not placed next to `worst_seam_ratio`, because this
    // struct is read by position and inserting in the middle silently renames every
    // field after the insertion point. The offset assertions below are what insisted
    // on that ordering when nae.h grew these fields; a total-size check would not
    // have, because the mistake is about order and not about size.
    /// Mean seam ratio over every seam seen, not just the retained ones.
    pub seam_mean: f64,
    /// Eight quantiles of the retained seam sample, minimum to maximum.
    ///
    /// The last is the maximum *of the retained sample*, so comparing it against
    /// `worst_seam_ratio` says whether the cap truncated anything. A quantile over a
    /// capped prefix is not the same number as one over everything, and
    /// `seam_sample_count` is reported so the reader can tell which it has.
    pub seam_quantiles: [f64; 8],
    /// How many seams the quantiles were computed from.
    pub seam_sample_count: u32,
    /// Explicit padding to the 8-byte boundary `seam_histogram` needs.
    ///
    /// `seam_quantiles` is 64 bytes of `f64`, so `seam_sample_count` lands at offset
    /// 64 and the `[u64; 41]` after it must start at 72. The four bytes of padding
    /// exist whether or not they are named; naming them is what makes this
    /// declaration and nae.h state the same layout.
    pub seam_histogram_pad: u32,
    /// Log-spaced counts, -2 to +2 decades in 0.1-decade buckets.
    ///
    /// Logarithmic because the ratio spans two orders of magnitude between a clean
    /// stream and a broken one, and a linear histogram over that range is one tall
    /// bar and sixty empty ones.
    pub seam_histogram: [u64; 41],
    /// Number of valid buckets in `seam_histogram`, so a reader never hard-codes 41.
    pub seam_histogram_max: u32,
    /// The ring size the render loop applies per channel, in frames. One more
    /// than the largest delay, so a delay equal to it still delays.
    pub channel_delay_max_frames: i32,
    /// Trailing padding to the struct's 8-byte alignment, named so it is stated.
    pub tail_pad: u32,
    pub tail_pad2: u32,
}

impl Default for NAE_SignalPath {
    /// A fully zeroed struct.
    ///
    /// Hand-written rather than derived because `[u64; 41]` is past the array
    /// lengths for which this toolchain implements `Default`, and hand-listing
    /// forty-seven fields would be a worse answer than this: a field added to the
    /// struct without being added here would then read as whatever the compiler
    /// happened to leave, which is the exact failure the `struct_size` contract
    /// exists to prevent.
    ///
    /// Zero is also the correct default specifically, not just conveniently
    /// derivable: the engine fills the struct and `struct_size` says which fields it
    /// wrote, so a field it did not write has to read as zero rather than as
    /// anything that looks like a measurement.
    fn default() -> Self {
        // SAFETY: every field is an integer or a float, so all-zero is a valid value
        // for all of them. There is no pointer, no `String` and no `Vec` in this
        // struct, so there is nothing here that owns memory or has a niche.
        unsafe { std::mem::zeroed() }
    }
}

// This struct is filled by position, not by name, so a field reordered here and
// not in nae.h does not fail to build -- it renames every figure after it. It
// happened: max_buffer_gap_ms was read while the C side had bufferDurationMs
// next, which reported a 53 ms decode stall that was really a duration, and a
// buffer duration of 0 that was really a decode stall. Two of the three new
// numbers were wrong and neither looked wrong.
//
// A total-size check would not have caught that, because swapping two f64s leaves
// the size identical. These assertions pin the neighbours instead.
const _: () = {
    use std::mem::offset_of;
    const G: usize = std::mem::size_of::<f64>();
    const N: usize = std::mem::size_of::<i64>();
    const U: usize = std::mem::size_of::<u32>();
    assert!(
        offset_of!(NAE_SignalPath, max_buffer_gap_ms) + G
            == offset_of!(NAE_SignalPath, buffer_duration_ms)
    );
    assert!(
        offset_of!(NAE_SignalPath, buffer_duration_ms) + G
            == offset_of!(NAE_SignalPath, max_decode_stall_ms)
    );
    assert!(
        offset_of!(NAE_SignalPath, max_decode_stall_ms) + G
            == offset_of!(NAE_SignalPath, max_fill_ms)
    );
    assert!(offset_of!(NAE_SignalPath, max_fill_ms) + G == offset_of!(NAE_SignalPath, max_late_ms));
    assert!(offset_of!(NAE_SignalPath, max_late_ms) + G == offset_of!(NAE_SignalPath, late_count));
    assert!(
        offset_of!(NAE_SignalPath, late_count) + N == offset_of!(NAE_SignalPath, worst_seam_ratio)
    );
    assert!(
        offset_of!(NAE_SignalPath, worst_seam_ratio) + G
            == offset_of!(NAE_SignalPath, seam_slew_ref)
    );
    assert!(
        offset_of!(NAE_SignalPath, seam_slew_ref) + G == offset_of!(NAE_SignalPath, seam_count)
    );
    assert!(
        offset_of!(NAE_SignalPath, seam_count) + N
            == offset_of!(NAE_SignalPath, device_rate_actual)
    );
    assert!(
        offset_of!(NAE_SignalPath, device_rate_actual) + G
            == offset_of!(NAE_SignalPath, silence_frames_written)
    );
    // The seam-distribution block must be last, and these are the assertions that
    // insisted on it.
    const Q: usize = 8 * std::mem::size_of::<f64>();
    const HB: usize = 41 * std::mem::size_of::<u64>();
    assert!(
        offset_of!(NAE_SignalPath, silence_frames_written) + N
            == offset_of!(NAE_SignalPath, seam_mean)
    );
    assert!(
        offset_of!(NAE_SignalPath, seam_mean) + G == offset_of!(NAE_SignalPath, seam_quantiles)
    );
    assert!(
        offset_of!(NAE_SignalPath, seam_quantiles) + Q
            == offset_of!(NAE_SignalPath, seam_sample_count)
    );
    assert!(
        offset_of!(NAE_SignalPath, seam_sample_count) + U
            == offset_of!(NAE_SignalPath, seam_histogram_pad)
    );
    assert!(
        offset_of!(NAE_SignalPath, seam_histogram_pad) + U
            == offset_of!(NAE_SignalPath, seam_histogram)
    );
    assert!(
        offset_of!(NAE_SignalPath, seam_histogram) + HB
            == offset_of!(NAE_SignalPath, seam_histogram_max)
    );
    // The tail: the ring size and the padding that keeps the struct's size a
    // multiple of 8. Each gap is accounted for by a named field, so the chain
    // checks a stated layout rather than a fudge factor.
    assert!(
        offset_of!(NAE_SignalPath, seam_histogram_max) + U
            == offset_of!(NAE_SignalPath, channel_delay_max_frames)
    );
    assert!(
        offset_of!(NAE_SignalPath, channel_delay_max_frames) + U
            == offset_of!(NAE_SignalPath, tail_pad)
    );
    assert!(
        offset_of!(NAE_SignalPath, tail_pad) + U == offset_of!(NAE_SignalPath, tail_pad2)
    );
    assert!(
        offset_of!(NAE_SignalPath, tail_pad2) + U == std::mem::size_of::<NAE_SignalPath>()
    );
};

extern "C" {
    pub fn NAE_QuerySignalPath(handle: *mut NAE_Handle, out: *mut NAE_SignalPath) -> i32;
}

/// Mirrors `NAE_SpectrumInfo` in nae.h.
///
/// `#[repr(C)]`, and the field order is not free to change: it is read by
/// position. The layout assertions below pin each field against the next, for the
/// same reason `NAE_SignalPath`'s do -- reordering two same-typed fields keeps the
/// struct's size identical and renames every figure after the swap without a
/// single compile error.
///
/// `peak` and `rms` are separated by the `u32` block and two `f64`s follow, so the
/// offsets are not a simple stride; they are stated rather than derived.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct NAE_SpectrumInfo {
    /// Set to the size of this struct by the caller; the engine fills in what it
    /// wrote. Same contract as `NAE_SignalPath.struct_size`, for the same reason.
    pub struct_size: u32,
    pub fft_size: u32,
    pub bin_count: u32,
    /// The rate the window was captured at, which is the *device's* rate.
    pub sample_rate: u32,
    pub valid_bits: u32,
    /// Explicit padding, present in nae.h for the same reason: five `u32` fields
    /// leave this struct at offset 20 and `peak` needs 8-alignment, so there are
    /// four bytes of padding whether or not they are named. Naming them is what
    /// makes the two declarations state the same layout rather than one of them
    /// happening to be right.
    pub reserved: u32,
    pub peak: f64,
    pub rms: f64,
    /// Device frames the tap has seen. Changes while audio flows; a reading whose
    /// `frames_fed` has not moved is a leftover, not a live measurement, and this
    /// is the only field that can tell the two apart.
    pub frames_fed: u64,
}

const _: () = {
    use std::mem::offset_of;
    const G: usize = std::mem::size_of::<f64>();
    const N: usize = std::mem::size_of::<u64>();
    const U: usize = std::mem::size_of::<u32>();
    assert!(
        offset_of!(NAE_SpectrumInfo, struct_size) + U == offset_of!(NAE_SpectrumInfo, fft_size)
    );
    assert!(offset_of!(NAE_SpectrumInfo, fft_size) + U == offset_of!(NAE_SpectrumInfo, bin_count));
    assert!(
        offset_of!(NAE_SpectrumInfo, bin_count) + U == offset_of!(NAE_SpectrumInfo, sample_rate)
    );
    assert!(
        offset_of!(NAE_SpectrumInfo, sample_rate) + U == offset_of!(NAE_SpectrumInfo, valid_bits)
    );
    assert!(offset_of!(NAE_SpectrumInfo, valid_bits) + U == offset_of!(NAE_SpectrumInfo, reserved));
    assert!(offset_of!(NAE_SpectrumInfo, reserved) + U == offset_of!(NAE_SpectrumInfo, peak));
    assert!(offset_of!(NAE_SpectrumInfo, peak) + G == offset_of!(NAE_SpectrumInfo, rms));
    assert!(offset_of!(NAE_SpectrumInfo, rms) + G == offset_of!(NAE_SpectrumInfo, frames_fed));
    assert!(
        offset_of!(NAE_SpectrumInfo, frames_fed) + N == std::mem::size_of::<NAE_SpectrumInfo>()
    );
};

extern "C" {
    /// `bins` receives `bin_count` linear magnitudes. `capacity` is in floats; too
    /// small truncates and still reports the true `bin_count`.
    pub fn NAE_QuerySpectrum(
        handle: *mut NAE_Handle,
        info: *mut NAE_SpectrumInfo,
        bins: *mut f32,
        capacity: i32,
    ) -> i32;
}

/// Mirrors `NAE_EnvelopeInfo` in nae.h.
///
/// `frame_hop_ms` is a `double` between two `uint32_t`s, so it needs 8-byte
/// alignment and there is 4 bytes of padding after `sample_rate`. The
/// `frames_fed_pad` field names that padding on both sides rather than leaving each
/// declaration to happen to agree with the other; the tail 4 bytes (`frame_valid`)
/// are a real field.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct NAE_EnvelopeInfo {
    pub struct_size: u32,
    /// **The ring's capacity**, not the number of frames written — that is
    /// `frame_valid`. The doc here used to say "frames written to `envelope`", which
    /// is what the implementation did *not* report, and the two differ until the
    /// ring has wrapped.
    pub frame_count: u32,
    /// Milliseconds between adjacent frames. A caller derives a beat period from
    /// frame counts using this, and must not have to guess the sample rate.
    ///
    /// **它在 `frame_count` 正后面（偏移 8）。** 这一份原来把它排在 `frames_fed`
    /// 之后——两个结构体的字段**顺序不同而 sizeof 相同**（都是 48），于是没有一条断言
    /// 看得出来，而运行时的读数是垃圾：`sample_rate` 读到了 `frame_hop_ms` 的低 4 字节
    /// （0），`frame_hop_ms` 读到了 `(44100, 0)` 的位模式（**2.12e-318**）。
    ///
    /// 前端那个 BPM 估计器拿到那样的跳长就永远返回 `null`——**界面上「BPM —」的原因
    /// 就是这个**，而它离显示它的那一格隔了三个文件（C 结构 → Rust 镜像 → 事件）。
    pub frame_hop_ms: f64,
    /// 设备的采样率。
    pub sample_rate: u32,
    /// Explicit padding to the 8-byte alignment `frames_fed` needs.
    ///
    /// `sample_rate` 落在偏移 16、`frames_fed` 是 `u64`，所以这四个字节的填充必然存在；
    /// 命名它只是让两份声明**写出同一个布局**，而不是各自碰巧同意。
    pub frames_fed_pad: u32,
    /// Device frames the tap has seen. Deliberately the same counter the spectrum
    /// reports: both describe the same ring, so two counters would invite the
    /// question "why did one go stale and not the other", which can only have one
    /// answer and it would be a bug in one of them.
    pub frames_fed: u64,
    /// Real time the window covers, from the frame count and the hop.
    pub window_ms: f64,
    /// How many of `envelope`'s entries are real, `0..frame_count`. They are the
    /// **oldest** ones, in order: `envelope[0]` is the oldest frame on offer and
    /// `envelope[frame_valid - 1]` the newest. `[frame_valid, frame_count)` is not
    /// written and must not be read.
    ///
    /// Renamed from `reserved`, which was tail padding: the four bytes were already
    /// there and the layout does not move. Every offset assertion on both sides is a
    /// literal number, so the two declarations have to change together or neither
    /// compiles.
    pub frame_valid: u32,
}

const _: () = {
    use std::mem::offset_of;
    // **写成绝对的字面偏移，不是「相邻」。**
    //
    // 上一版这里写的是 `offset_of!(a) + U == offset_of!(b)` 那种相邻断言——而那一种
    // **对字段重排是免疫的**：把 `sample_rate` 和 `frame_hop_ms` 对调，每一条相邻断言
    // 仍然成立，`sizeof` 也仍然是 48。所以它抓不住它存在就是为了抓的那个错。
    //
    // 这一组数字和 `nae.cpp` 里那组 `static_assert` 是**同一组字面数字**（那边的
    // `NAE_EnvelopeInfo` 断言是这一次补上的——它原来一条都没有，而这一侧的断言又对重排
    // 免疫，于是两份声明可以任意不一致而两边都编译通过）。
    assert!(offset_of!(NAE_EnvelopeInfo, struct_size) == 0);
    assert!(offset_of!(NAE_EnvelopeInfo, frame_count) == 4);
    assert!(offset_of!(NAE_EnvelopeInfo, frame_hop_ms) == 8);
    assert!(offset_of!(NAE_EnvelopeInfo, sample_rate) == 16);
    assert!(offset_of!(NAE_EnvelopeInfo, frames_fed_pad) == 20);
    assert!(offset_of!(NAE_EnvelopeInfo, frames_fed) == 24);
    assert!(offset_of!(NAE_EnvelopeInfo, window_ms) == 32);
    assert!(offset_of!(NAE_EnvelopeInfo, frame_valid) == 40);
    assert!(std::mem::size_of::<NAE_EnvelopeInfo>() == 48);
};

/// Mirrors `NAE_PeaksInfo` in nae.h.
///
/// 布局是 `u32 *4, f64, u32 *3` —— 四个 u32 之后 `duration_ms` 落在偏移 16（8 的
/// 倍数，所以没有内部 padding），三个 u32 之后是 4 字节**尾部** padding（结构体对齐
/// 8）。尾部 padding 用 `tail_pad` 显式命名，和 `NAE_EnvelopeInfo` 那边的
/// `frames_fed_pad` 同一个做法：让两份声明**写出同一个布局**，而不是各自碰巧同意。
///
/// 下面那组 `offset_of!` 断言和 `peaks_check.cpp` 里的 `static_assert` 断言的是
/// **同一组字面数字**。这就是跨语言那一层检查 —— 两边各写各的期望值，其中一边改了
/// 而另一边没改，编译就会失败。
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct NAE_PeaksInfo {
    /// 由调用方设为 `size_of::<NAE_PeaksInfo>()`。
    pub struct_size: u32,
    /// 填了多少桶。0 表示没有可用的 peaks（没有文件 / 算失败）。
    pub bucket_count: u32,
    /// 1 = 覆盖了整个文件；0 = 没有（或解码中途结束）。
    pub complete: u32,
    /// 保留。**原来是 `pending`**（引擎侧缓存里「别人正在算」）。
    ///
    /// 缓存挪到调用方之后它就没有含义了：没有 handle，引擎侧无处可放缓存，也就没有
    /// 「别人正在算」这回事。留着这 4 字节是因为它是 `duration_ms` 落在偏移 16（8 的
    /// 倍数）所必需的填充 —— 换个名字而不是删掉，那组布局断言就一个字不用改。
    pub reserved0: u32,
    /// 文件的时长，毫秒。0 表示容器没有报。
    pub duration_ms: f64,
    /// 文件的采样率。
    pub sample_rate: u32,
    /// 文件的声道数。
    pub channels: u32,
    /// 短窗的长度，毫秒。**0 = 下面那两个响度指标没有算。**
    ///
    /// 窗口的参数和数一起走，不是让调用方自己记一个常量：读出来的那两个数只有在
    /// 「多长的窗」说清楚之后才是数。
    pub metrics_window_ms: u32,
    /// 窗之间的步长，毫秒。
    pub metrics_hop_ms: u32,
    /// 所有短窗 RMS 电平的**中位数**，dBFS。口径见 `nae.cpp` 的
    /// `computePeaksFromFile`（`spectrum.js` 有一份逐字对齐的镜像）。
    pub loudness_db: f64,
    /// 短窗 RMS 电平的 p95 − p10，dB。
    pub dynamic_range_db: f64,
    /// ITU-R BS.1770-4 / EBU R128 integrated loudness, LUFS.
    pub lufs_integrated: f64,
    /// Loudest 400 ms block, LUFS, ungated.
    pub lufs_momentary_max: f64,
    /// Loudest 3 s window, LUFS, ungated.
    pub lufs_short_term_max: f64,
    /// EBU Tech 3342 loudness range, LU.
    pub lufs_range: f64,
    /// 0 = the four LUFS values are not measured.
    pub lufs_valid: i32,
    /// 0 = no 3 s window existed; the short-term max and the range are 0.
    pub lufs_short_term_valid: i32,
}

const _: () = {
    use std::mem::offset_of;
    const U: usize = std::mem::size_of::<u32>();
    // 逐个字段的偏移，**写成字面数字**，这样两边改一处就会有一边编译不过。
    //
    // 这一版加了 `loudness_db` / `dynamic_range_db` 之后 sizeof 从 40 变成 56——所以
    // 下面这些数字**全都改过一遍**，而它们是和 `nae.cpp` 那组 `static_assert` 一起改的。
    assert!(offset_of!(NAE_PeaksInfo, struct_size) == 0);
    assert!(offset_of!(NAE_PeaksInfo, bucket_count) == 4);
    assert!(offset_of!(NAE_PeaksInfo, complete) == 8);
    assert!(offset_of!(NAE_PeaksInfo, reserved0) == 12);
    assert!(offset_of!(NAE_PeaksInfo, duration_ms) == 16);
    assert!(offset_of!(NAE_PeaksInfo, sample_rate) == 24);
    assert!(offset_of!(NAE_PeaksInfo, channels) == 28);
    assert!(offset_of!(NAE_PeaksInfo, metrics_window_ms) == 32);
    assert!(offset_of!(NAE_PeaksInfo, metrics_hop_ms) == 36);
    assert!(offset_of!(NAE_PeaksInfo, loudness_db) == 40);
    assert!(offset_of!(NAE_PeaksInfo, dynamic_range_db) == 48);
    assert!(offset_of!(NAE_PeaksInfo, lufs_integrated) == 56);
    assert!(offset_of!(NAE_PeaksInfo, lufs_momentary_max) == 64);
    assert!(offset_of!(NAE_PeaksInfo, lufs_short_term_max) == 72);
    assert!(offset_of!(NAE_PeaksInfo, lufs_range) == 80);
    assert!(offset_of!(NAE_PeaksInfo, lufs_valid) == 88);
    assert!(offset_of!(NAE_PeaksInfo, lufs_short_term_valid) == 92);
    assert!(std::mem::size_of::<NAE_PeaksInfo>() == 96);
    // 顺带把「相邻」也钉住，这样加字段的人必须同时改上面那组数字。
    assert!(offset_of!(NAE_PeaksInfo, struct_size) + U == offset_of!(NAE_PeaksInfo, bucket_count));
    assert!(
        offset_of!(NAE_PeaksInfo, metrics_window_ms) + U
            == offset_of!(NAE_PeaksInfo, metrics_hop_ms)
    );
    assert!(
        offset_of!(NAE_PeaksInfo, metrics_hop_ms) + U == offset_of!(NAE_PeaksInfo, loudness_db)
    );
};

/// Mirrors `NAE_AnalysisInfo` in nae.h.
///
/// This is the one struct that carries full tags, ReplayGain, loudness,
/// dynamic range and BPM. The `offset_of!` block below pins the same literal
/// numbers as the `static_assert` block in `nae.cpp`: a field added on one side
/// without the other fails the build instead of reading a tag as a gain.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_AnalysisInfo {
    pub struct_size: u32,
    pub analyzer_version: u32,
    pub complete: u32,
    pub bucket_count: u32,
    pub channels: i32,
    pub sample_rate: i32,
    pub bits_per_sample: i32,
    pub duration_ms: i32,
    pub codec: [c_char; 32],
    pub title: [c_char; 128],
    pub artist: [c_char; 128],
    pub album: [c_char; 128],
    pub album_artist: [c_char; 128],
    pub track: [c_char; 16],
    pub disc: [c_char; 16],
    pub year: [c_char; 16],
    pub genre: [c_char; 64],
    pub composer: [c_char; 128],
    pub comment: [c_char; 256],
    pub bitrate_kbps: u32,
    pub bitrate_from_tag: i32,
    pub rg_track_valid: i32,
    pub rg_track_gain_db: f64,
    pub rg_track_peak: f64,
    pub rg_album_valid: i32,
    pub rg_album_gain_db: f64,
    pub rg_album_peak: f64,
    pub loudness_db: f64,
    pub dynamic_range_db: f64,
    pub metrics_window_ms: u32,
    pub metrics_hop_ms: u32,
    pub bpm: f64,
    pub bpm_confidence: f64,
    pub bpm_valid: i32,
    /// ITU-R BS.1770-4 integrated loudness, LUFS.
    pub lufs_integrated: f64,
    pub lufs_momentary_max: f64,
    pub lufs_short_term_max: f64,
    /// EBU Tech 3342 loudness range, LU.
    pub lufs_range: f64,
    pub lufs_valid: i32,
    /// 0 = no 3 s window existed.
    pub lufs_short_term_valid: i32,
}

impl Default for NAE_AnalysisInfo {
    fn default() -> Self {
        // SAFETY: every field is an integer, float or `c_char` array. All-zero
        // is valid for all of them; there is no pointer or owned value.
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    const C: usize = std::mem::size_of::<c_char>();
    assert!(offset_of!(NAE_AnalysisInfo, struct_size) == 0);
    assert!(offset_of!(NAE_AnalysisInfo, analyzer_version) == 4);
    assert!(offset_of!(NAE_AnalysisInfo, complete) == 8);
    assert!(offset_of!(NAE_AnalysisInfo, bucket_count) == 12);
    assert!(offset_of!(NAE_AnalysisInfo, channels) == 16);
    assert!(offset_of!(NAE_AnalysisInfo, sample_rate) == 20);
    assert!(offset_of!(NAE_AnalysisInfo, bits_per_sample) == 24);
    assert!(offset_of!(NAE_AnalysisInfo, duration_ms) == 28);
    assert!(offset_of!(NAE_AnalysisInfo, codec) == 32);
    assert!(offset_of!(NAE_AnalysisInfo, title) == 64);
    assert!(offset_of!(NAE_AnalysisInfo, artist) == 192);
    assert!(offset_of!(NAE_AnalysisInfo, album) == 320);
    assert!(offset_of!(NAE_AnalysisInfo, album_artist) == 448);
    assert!(offset_of!(NAE_AnalysisInfo, track) == 576);
    assert!(offset_of!(NAE_AnalysisInfo, disc) == 592);
    assert!(offset_of!(NAE_AnalysisInfo, year) == 608);
    assert!(offset_of!(NAE_AnalysisInfo, genre) == 624);
    assert!(offset_of!(NAE_AnalysisInfo, composer) == 688);
    assert!(offset_of!(NAE_AnalysisInfo, comment) == 816);
    assert!(offset_of!(NAE_AnalysisInfo, bitrate_kbps) == 1072);
    assert!(offset_of!(NAE_AnalysisInfo, bitrate_from_tag) == 1076);
    assert!(offset_of!(NAE_AnalysisInfo, rg_track_valid) == 1080);
    assert!(offset_of!(NAE_AnalysisInfo, rg_track_gain_db) == 1088);
    assert!(offset_of!(NAE_AnalysisInfo, rg_track_peak) == 1096);
    assert!(offset_of!(NAE_AnalysisInfo, rg_album_valid) == 1104);
    assert!(offset_of!(NAE_AnalysisInfo, rg_album_gain_db) == 1112);
    assert!(offset_of!(NAE_AnalysisInfo, rg_album_peak) == 1120);
    assert!(offset_of!(NAE_AnalysisInfo, loudness_db) == 1128);
    assert!(offset_of!(NAE_AnalysisInfo, dynamic_range_db) == 1136);
    assert!(offset_of!(NAE_AnalysisInfo, metrics_window_ms) == 1144);
    assert!(offset_of!(NAE_AnalysisInfo, metrics_hop_ms) == 1148);
    assert!(offset_of!(NAE_AnalysisInfo, bpm) == 1152);
    assert!(offset_of!(NAE_AnalysisInfo, bpm_confidence) == 1160);
    assert!(offset_of!(NAE_AnalysisInfo, bpm_valid) == 1168);
    assert!(offset_of!(NAE_AnalysisInfo, lufs_integrated) == 1176);
    assert!(offset_of!(NAE_AnalysisInfo, lufs_momentary_max) == 1184);
    assert!(offset_of!(NAE_AnalysisInfo, lufs_short_term_max) == 1192);
    assert!(offset_of!(NAE_AnalysisInfo, lufs_range) == 1200);
    assert!(offset_of!(NAE_AnalysisInfo, lufs_valid) == 1208);
    assert!(offset_of!(NAE_AnalysisInfo, lufs_short_term_valid) == 1212);
    assert!(std::mem::size_of::<NAE_AnalysisInfo>() == 1216);
    // Adjacency pins: adding a field must also update the numbers above.
    assert!(
        offset_of!(NAE_AnalysisInfo, struct_size) + 4
            == offset_of!(NAE_AnalysisInfo, analyzer_version)
    );
    assert!(offset_of!(NAE_AnalysisInfo, codec) + 32 * C == offset_of!(NAE_AnalysisInfo, title));
    assert!(offset_of!(NAE_AnalysisInfo, title) + 128 * C == offset_of!(NAE_AnalysisInfo, artist));
    assert!(
        offset_of!(NAE_AnalysisInfo, comment) + 256 * C
            == offset_of!(NAE_AnalysisInfo, bitrate_kbps)
    );
    assert!(offset_of!(NAE_AnalysisInfo, metrics_hop_ms) + 4 == offset_of!(NAE_AnalysisInfo, bpm));
    assert!(
        offset_of!(NAE_AnalysisInfo, bpm_confidence) + 8 == offset_of!(NAE_AnalysisInfo, bpm_valid)
    );
};

extern "C" {
    /// Decode `path` once and fill `info` plus `buckets * 2` interleaved peaks.
    /// Blocking; no device is opened. A short buffer is rejected, not truncated.
    pub fn NAE_AnalyzeFile(
        path: *const c_char,
        buckets: u32,
        info: *mut NAE_AnalysisInfo,
        peaks: *mut f32,
        capacity: i32,
        peaks_written: *mut i32,
    ) -> i32;

    pub fn NAE_AnalyzeFileCancelable(
        path: *const c_char,
        buckets: u32,
        info: *mut NAE_AnalysisInfo,
        peaks: *mut f32,
        capacity: i32,
        peaks_written: *mut i32,
        cancel: *const i32,
    ) -> i32;

    /// The analysis algorithm version; cache keys use it.
    pub fn NAE_GetAnalyzerVersion() -> u32;
}

extern "C" {
    /// 算出整首波形的 peaks。`out` 收到 `bucket_count * 2` 个交错的 min/max。
    ///
    /// **收的是路径，不是 handle。** 一开始写的是 handle 版，而
    /// `binding/src/engine.rs` 写着那条更严的约定：stream 是 `!Send`、
    /// 「commands are messages, not pointers, so nothing audio-related is ever handed
    /// between threads」。从另一个线程拿 handle 正是那条约定不允许的事，而且 handle
    /// 的生命周期归音频线程。路径本来就是消息。
    ///
    /// **阻塞**：要再解一遍文件（实测典型曲目约 0.5 秒，245 MB / 192 kHz 约 1.6 秒）。
    /// 所以这个调用**绝不能在 UI 线程上**。
    ///
    /// `buckets` 为 0、`path` 为空、`out` 为空、或 `capacity < buckets * 2` 时返回
    /// `NAE_ERR_INVALID_ARGUMENT`（**不截断**）；打不开路径时返回
    /// `NAE_ERR_CANNOT_OPEN`。
    pub fn NAE_ComputePeaks(
        path: *const c_char,
        buckets: u32,
        info: *mut NAE_PeaksInfo,
        out: *mut f32,
        capacity: i32,
    ) -> i32;
}

/// Mirrors `NAE_TrackInfo` in nae.h.
///
/// `codec` is a fixed 32-byte array rather than a pointer, so there is nothing to
/// free and nothing for the caller and the engine to disagree about who owns. That
/// is deliberate: the open out-params already have `char*` fields that the engine
/// allocates and the caller frees, and having a second convention next to it in the
/// same header is how one of them ends up freeing the other's memory.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_TrackInfo {
    pub struct_size: u32,
    pub channels: i32,
    pub sample_rate: i32,
    pub bits_per_sample: i32,
    pub duration_ms: i32,
    pub codec: [c_char; 32],
}

impl Default for NAE_TrackInfo {
    fn default() -> Self {
        // SAFETY: every field is an integer or a `c_char` array, so all-zero is a
        // valid value for all of them. No pointer, no `String`, nothing that owns
        // memory or has a niche.
        unsafe { std::mem::zeroed() }
    }
}

impl NAE_TrackInfo {
    /// The codec name, up to the first NUL.
    ///
    /// Read out of a C array rather than exposed as one: a `[c_char; 32]` handed to
    /// a caller invites them to treat it as a string, and the trailing garbage after
    /// the NUL is exactly what would show up in the UI.
    pub fn codec_str(&self) -> &str {
        // Searched by byte rather than collected-then-truncated, so this borrows
        // from `self` instead of allocating on every call. The engine calls this on
        // every track change and the result is a handful of bytes.
        let bytes: &[u8] = {
            let raw = self.codec.as_ptr();
            let len = self.codec.len();
            // SAFETY: `raw` points at `len` initialised `c_char` values -- this
            // struct's own field -- and the loop only reads within that range.
            unsafe { std::slice::from_raw_parts(raw.cast::<u8>(), len) }
        };
        let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
        std::str::from_utf8(&bytes[..end]).unwrap_or("")
    }
}

const _: () = {
    use std::mem::offset_of;
    const U: usize = std::mem::size_of::<u32>();
    const I: usize = std::mem::size_of::<i32>();
    const C: usize = 32 * std::mem::size_of::<c_char>();
    assert!(offset_of!(NAE_TrackInfo, struct_size) + U == offset_of!(NAE_TrackInfo, channels));
    assert!(offset_of!(NAE_TrackInfo, channels) + I == offset_of!(NAE_TrackInfo, sample_rate));
    assert!(
        offset_of!(NAE_TrackInfo, sample_rate) + I == offset_of!(NAE_TrackInfo, bits_per_sample)
    );
    assert!(
        offset_of!(NAE_TrackInfo, bits_per_sample) + I == offset_of!(NAE_TrackInfo, duration_ms)
    );
    assert!(offset_of!(NAE_TrackInfo, duration_ms) + I == offset_of!(NAE_TrackInfo, codec));
    assert!(offset_of!(NAE_TrackInfo, codec) + C == std::mem::size_of::<NAE_TrackInfo>());
};

extern "C" {
    pub fn NAE_QueryTrackInfo(handle: *mut NAE_Handle, out: *mut NAE_TrackInfo) -> i32;
}

/// Mirrors `NAE_Tags` in nae.h.
///
/// Fixed-size character arrays, so there is nothing to free and nothing for the
/// caller and the engine to disagree about. Deliberately a second convention
/// alongside the open out-params' `char*`, which the engine allocates and the
/// caller releases: two ownership conventions in one header is how one of them
/// starts freeing the other's memory.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_Tags {
    pub struct_size: u32,
    pub year: [c_char; 16],
    pub genre: [c_char; 64],
    pub album: [c_char; 128],
    pub artist: [c_char; 128],
    pub bitrate_kbps: u32,
    pub reserved: u32,
}

impl Default for NAE_Tags {
    fn default() -> Self {
        // SAFETY: every field is an integer or a `c_char` array, so all-zero is
        // valid for all of them. No pointer, no `String`, no niche.
        unsafe { std::mem::zeroed() }
    }
}

impl NAE_Tags {
    /// One fixed-size field as a `&str`, up to the first NUL.
    ///
    /// Truncated at the NUL rather than at the array length, because the engine
    /// writes NUL-terminated strings into a zeroed array -- and anything past the
    /// terminator is uninitialised in general, so reading the whole array would be
    /// reading garbage.
    pub(crate) fn field(&self, raw: &[c_char]) -> &str {
        let bytes: &[u8] = {
            let ptr = raw.as_ptr();
            let len = raw.len();
            // SAFETY: `ptr` points at `len` initialised `c_char` values -- this
            // struct's own field -- and the search only reads within that range.
            unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), len) }
        };
        let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
        std::str::from_utf8(&bytes[..end]).unwrap_or("")
    }
}

const _: () = {
    use std::mem::offset_of;
    const U: usize = std::mem::size_of::<u32>();
    const C: usize = std::mem::size_of::<c_char>();
    assert!(offset_of!(NAE_Tags, struct_size) + U == offset_of!(NAE_Tags, year));
    assert!(offset_of!(NAE_Tags, year) + 16 * C == offset_of!(NAE_Tags, genre));
    assert!(offset_of!(NAE_Tags, genre) + 64 * C == offset_of!(NAE_Tags, album));
    assert!(offset_of!(NAE_Tags, album) + 128 * C == offset_of!(NAE_Tags, artist));
    assert!(offset_of!(NAE_Tags, artist) + 128 * C == offset_of!(NAE_Tags, bitrate_kbps));
    assert!(offset_of!(NAE_Tags, bitrate_kbps) + U == offset_of!(NAE_Tags, reserved));
    assert!(offset_of!(NAE_Tags, reserved) + U == std::mem::size_of::<NAE_Tags>());
};

extern "C" {
    pub fn NAE_QueryTags(handle: *mut NAE_Handle, out: *mut NAE_Tags) -> i32;

    /// 采样**实际**是怎么被解出来的，写进 `buf`（最多 `NAE_DECODE_PATH_MAX` 字节，含结尾
    /// NUL）。
    ///
    /// 定长而不是 `&str`/`String`，因为引擎不分配——只有一种所有权约定。返回值是引擎的
    /// 错误码，**空字符串不是错误**：那表示「这个信号没有自己的路径」，与「读不出来」
    /// 必须能分辨。
    pub fn NAE_QueryDecodePath(
        handle: *mut NAE_Handle,
        buf: *mut std::os::raw::c_char,
        bufLen: i32,
    ) -> i32;
}

/// `NAE_DECODE_PATH_MAX`。C 头里的宏，Rust 侧必须与它一致——它决定缓冲区大小，写小了
/// 会被引擎截断而**不报错**，所以它是这里最容易悄悄出错的一个数。
pub const NAE_DECODE_PATH_MAX: usize = 256;

extern "C" {
    /// `envelope[0..frame_valid)` receives the mean-absolute values, **oldest first**;
    /// `[frame_valid, frame_count)` is not written.
    ///
    /// `capacity` is in floats. Too small truncates and still reports the true
    /// `frame_valid`, so a caller can grow its buffer and retry without the count
    /// lying about what was available.
    pub fn NAE_QueryEnvelope(
        handle: *mut NAE_Handle,
        info: *mut NAE_EnvelopeInfo,
        envelope: *mut f32,
        capacity: i32,
    ) -> i32;
}

// The rates the current device will open in exclusive mode, ascending.
//
// Measured by the engine rather than assumed, because WASAPI has no enumeration
// API and a list invented here would be a list of hopes. A count of zero means
// "not probed" and is not a claim that nothing works.
extern "C" {
    pub fn NAE_QuerySupportedRates(
        handle: *mut NAE_Handle,
        rates: *mut u32,
        capacity: u32,
        out_count: *mut u32,
    ) -> i32;
}

// ---------------------------------------------------------------------------
// File playback
// ---------------------------------------------------------------------------

// Open an audio file and the exclusive device together.
//
// Separate from [`NAE_OpenExclusive`] because the device must be configured for
// the *file's* rate, and the file's rate is only known once the file has been
// read. A caller that passed the rate in from outside could get it wrong, and
// the `bit_perfect` flag would then describe a stream that was never what was on
// disk.
//
// `device_id` of null means "whatever Windows is using right now", resolved here
// at open time rather than from a value the UI captured earlier — a user can
// plug in headphones between choosing and pressing play.
//
// `file_channels` is the file's own count and differs from `channels` (the
// device's) whenever a downmix or upmix happened. The UI shows the file's.
// A channel conversion makes `bit_perfect` false, because it is processing.
//
// `duration_ms` is 0 for a stream whose length the container does not report.
#[allow(clippy::too_many_arguments)]
extern "C" {
    pub fn NAE_OpenFile(
        device_id: *const c_char,
        path: *const c_char,
        output_mode: i32,
        fixed_rate: i32,
        quality: i32,
        out: *mut *mut NAE_Handle,
        sample_rate: *mut i32,
        source_rate_out: *mut i32,
        channels: *mut i32,
        bits_per_sample: *mut i32,
        valid_bits: *mut i32,
        is_float: *mut i32,
        resampling: *mut i32,
        dithered: *mut i32,
        bit_perfect: *mut i32,
        file_channels: *mut i32,
        duration_ms: *mut f64,
        latency_ms: *mut f64,
    ) -> i32;
}

// Open a file in **shared** mode. Identical to [`NAE_OpenFile`] in every argument
// and in what it does, except that the Windows mixer sits in the path so other
// programs can make sound at the same time.
//
// A separate entry point rather than an extra flag, because the capability string
// advertises a list of modes and a host reading that list has to be able to ask
// for exactly the ones it sees. `NAE_GetEngineCapabilities` names this symbol as
// the entry point for `shared`.
//
// `bit_perfect` is always 0 and the reason is always `NAE_REASON_SHARED_MODE` (7).
// Whether Windows would in fact preserve the samples is **untested**; the product
// decision (2026-10-01) is that shared mode does not pursue bit-perfectness, so
// the claim is made in neither direction.
#[allow(clippy::too_many_arguments)]
extern "C" {
    pub fn NAE_OpenFileShared(
        device_id: *const c_char,
        path: *const c_char,
        output_mode: i32,
        fixed_rate: i32,
        quality: i32,
        out: *mut *mut NAE_Handle,
        sample_rate: *mut i32,
        source_rate_out: *mut i32,
        channels: *mut i32,
        bits_per_sample: *mut i32,
        valid_bits: *mut i32,
        is_float: *mut i32,
        resampling: *mut i32,
        dithered: *mut i32,
        bit_perfect: *mut i32,
        file_channels: *mut i32,
        duration_ms: *mut f64,
        latency_ms: *mut f64,
    ) -> i32;
}

// Render `milliseconds` of the handle's source, converting and dithering as
// configured at open time. Blocks.
//
// `milliseconds` bounds how long the call *takes*, not how much source is
// consumed. Call repeatedly to play; one call does not play a whole track unless
// the whole track fits the time given. The source position carries across calls,
// so slicing is how the host is expected to drive this.
extern "C" {
    pub fn NAE_Play(handle: *mut NAE_Handle, milliseconds: i32) -> i32;
    /// Stop the device stream without discarding the handle. See nae.h.
    pub fn NAE_Stop(handle: *mut NAE_Handle) -> i32;
    /// Keep the stream running and feed it silence. See nae.h.
    ///
    /// This is what a pause should call. `NAE_Stop` is audible on this endpoint --
    /// measured, 100 stop/play cycles gave one artefact per cycle and the same
    /// cycles without the stop were clean -- so pausing by stopping costs a
    /// discontinuity that the samples themselves do not contain.
    ///
    /// Accounting does not move while paused: `NAE_Position` holds still and no
    /// source is consumed, so the resume lands on the same sample.
    pub fn NAE_Pause(handle: *mut NAE_Handle) -> i32;
    /// Leave the paused state and resume from the same sample. See nae.h.
    pub fn NAE_Resume(handle: *mut NAE_Handle) -> i32;
}

// Restart playback from `seconds` into the source.
//
// Approximate for compressed formats: the container lands on the nearest packet
// it can, so the position is near the target rather than exact. The UI should not
// promise sample accuracy.
extern "C" {
    pub fn NAE_Seek(handle: *mut NAE_Handle, seconds: f64) -> i32;
}

// Seconds into the source the listener has reached. Counts from the start of the
// file, not from the last seek, so this is what a progress bar should show.
extern "C" {
    pub fn NAE_Position(handle: *mut NAE_Handle) -> f64;
}

// Seconds the source lasts, or 0 when unknown.
extern "C" {
    pub fn NAE_Duration(handle: *mut NAE_Handle) -> f64;
}

// Copy the calling thread's last error message into `buf`, NUL-terminated and
// truncated to fit. Returns the number of bytes written, excluding the
// terminator.
//
// A code says something failed; it cannot say "this file is not audio", which is
// the thing a listener needs to know.
extern "C" {
    pub fn NAE_GetLastErrorText(buf: *mut c_char, buf_len: i32) -> i32;
}

// Release this thread's COM apartment.
//
// Only correct once every handle created on the thread has been closed. In the
// real app the audio thread does this as it exits; the UI thread never needs it.
// Calling it while a handle is still live is the bug the spike caught.
extern "C" {
    pub fn NAE_Shutdown();
}

// Silences an "unused import" warning while keeping the C types referenced, so
// the mapping above stays visibly tied to the header's real types.
#[allow(dead_code)]
type _AssertCTypesMatch = (c_void, c_int);

// ---- session -----------------------------------------------------------------
//
// The session is the engine's own playback clock: it owns the stream, the
// session thread and the queue. Base uses it instead of re-implementing a
// render loop around `NAE_Play`.

pub const NAE_SHARE_EXCLUSIVE: i32 = 0;
pub const NAE_SHARE_SHARED: i32 = 1;

pub const NAE_SESSION_IDLE: i32 = 0;
pub const NAE_SESSION_READY: i32 = 1;
pub const NAE_SESSION_PLAYING: i32 = 2;
pub const NAE_SESSION_PAUSED: i32 = 3;
pub const NAE_SESSION_DEVICE_LOST: i32 = 4;
pub const NAE_SESSION_ENDED: i32 = 5;
pub const NAE_SESSION_ERROR: i32 = 6;

#[repr(C)]
pub struct NAE_Session {
    _private: [u8; 0],
}

#[repr(C)]
pub struct NAE_SessionConfig {
    pub struct_size: u32,
    pub device_id: *const c_char,
    pub path: *const c_char,
    pub output_mode: i32,
    pub fixed_rate: i32,
    pub quality: i32,
    pub share_mode: i32,
    pub slice_ms: i32,
}

impl Default for NAE_SessionConfig {
    fn default() -> Self {
        // SAFETY: pointers and integers; all-zero is a valid "no device / no
        // path" config. The caller must set `path` before opening.
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    assert!(offset_of!(NAE_SessionConfig, struct_size) == 0);
    assert!(offset_of!(NAE_SessionConfig, device_id) == 8);
    assert!(offset_of!(NAE_SessionConfig, path) == 16);
    assert!(offset_of!(NAE_SessionConfig, output_mode) == 24);
    assert!(offset_of!(NAE_SessionConfig, fixed_rate) == 28);
    assert!(offset_of!(NAE_SessionConfig, quality) == 32);
    assert!(offset_of!(NAE_SessionConfig, share_mode) == 36);
    assert!(offset_of!(NAE_SessionConfig, slice_ms) == 40);
    assert!(std::mem::size_of::<NAE_SessionConfig>() == 48);
};

/// Mirrors `NAE_SessionState` in nae.h. This is the render-path fact: the
/// session already combines the open signal path with the enabled DSP stages
/// and the `bitPerfect` reason.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_SessionState {
    pub struct_size: u32,
    pub state: i32,
    pub position: f64,
    pub duration: f64,
    pub sample_rate: i32,
    pub source_rate: i32,
    pub channels: i32,
    pub bits_per_sample: i32,
    pub valid_bits: i32,
    pub is_float: i32,
    pub bit_perfect: i32,
    pub reason: i32,
    pub resampling: i32,
    pub dithered: i32,
    pub file_channels: i32,
    pub latency_ms: f64,
    pub volume: f64,
    pub eq_enabled: i32,
    pub crossfeed_enabled: i32,
    pub replay_gain_enabled: i32,
    pub convolver_enabled: i32,
    pub crossfade_enabled: i32,
    pub rg_track_valid: i32,
    pub rg_album_valid: i32,
    pub rg_track_gain_db: f64,
    pub rg_album_gain_db: f64,
    pub rg_applied_gain: f64,
    pub track_serial: u64,
}

impl Default for NAE_SessionState {
    fn default() -> Self {
        // SAFETY: integers, floats and no pointers; all-zero is valid.
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    assert!(offset_of!(NAE_SessionState, struct_size) == 0);
    assert!(offset_of!(NAE_SessionState, state) == 4);
    assert!(offset_of!(NAE_SessionState, position) == 8);
    assert!(offset_of!(NAE_SessionState, duration) == 16);
    assert!(offset_of!(NAE_SessionState, sample_rate) == 24);
    assert!(offset_of!(NAE_SessionState, source_rate) == 28);
    assert!(offset_of!(NAE_SessionState, channels) == 32);
    assert!(offset_of!(NAE_SessionState, bits_per_sample) == 36);
    assert!(offset_of!(NAE_SessionState, valid_bits) == 40);
    assert!(offset_of!(NAE_SessionState, is_float) == 44);
    assert!(offset_of!(NAE_SessionState, bit_perfect) == 48);
    assert!(offset_of!(NAE_SessionState, reason) == 52);
    assert!(offset_of!(NAE_SessionState, resampling) == 56);
    assert!(offset_of!(NAE_SessionState, dithered) == 60);
    assert!(offset_of!(NAE_SessionState, file_channels) == 64);
    assert!(offset_of!(NAE_SessionState, latency_ms) == 72);
    assert!(offset_of!(NAE_SessionState, volume) == 80);
    assert!(offset_of!(NAE_SessionState, eq_enabled) == 88);
    assert!(offset_of!(NAE_SessionState, crossfeed_enabled) == 92);
    assert!(offset_of!(NAE_SessionState, replay_gain_enabled) == 96);
    assert!(offset_of!(NAE_SessionState, convolver_enabled) == 100);
    assert!(offset_of!(NAE_SessionState, crossfade_enabled) == 104);
    assert!(offset_of!(NAE_SessionState, rg_track_valid) == 108);
    assert!(offset_of!(NAE_SessionState, rg_album_valid) == 112);
    assert!(offset_of!(NAE_SessionState, rg_track_gain_db) == 120);
    assert!(offset_of!(NAE_SessionState, rg_album_gain_db) == 128);
    assert!(offset_of!(NAE_SessionState, rg_applied_gain) == 136);
    assert!(offset_of!(NAE_SessionState, track_serial) == 144);
    assert!(std::mem::size_of::<NAE_SessionState>() == 152);
};

extern "C" {
    pub fn NAE_SessionOpen(config: *const NAE_SessionConfig, out: *mut *mut NAE_Session) -> i32;
    pub fn NAE_SessionPlay(session: *mut NAE_Session) -> i32;
    pub fn NAE_SessionPause(session: *mut NAE_Session) -> i32;
    pub fn NAE_SessionResume(session: *mut NAE_Session) -> i32;
    pub fn NAE_SessionSeek(session: *mut NAE_Session, seconds: f64) -> i32;
    pub fn NAE_SessionStop(session: *mut NAE_Session) -> i32;
    pub fn NAE_SessionGetState(session: *mut NAE_Session, out: *mut NAE_SessionState) -> i32;
    pub fn NAE_SessionSkipToNext(session: *mut NAE_Session) -> i32;
    pub fn NAE_SessionQueueAdd(session: *mut NAE_Session, path: *const c_char) -> i32;
    pub fn NAE_SessionQueueClear(session: *mut NAE_Session) -> i32;
    pub fn NAE_SessionQueueCount(session: *mut NAE_Session) -> i32;
    pub fn NAE_SessionLastError(session: *mut NAE_Session, buf: *mut c_char, buf_len: i32) -> i32;
    pub fn NAE_SessionGetResolvedSignalPathJson(session: *mut NAE_Session) -> *mut c_char;
    pub fn NAE_SessionClose(session: *mut NAE_Session);
}


// ---- session DSP parameters ------------------------------------------------
//
// Mirrored from nae.h. Base exposes these as serializable settings; the C
// structs stay at the ABI edge. Every field is pinned below.

pub const NAE_EQ_MAX_BANDS: usize = 32;
pub const NAE_EQ_PEAKING: i32 = 0;
pub const NAE_EQ_LOW_SHELF: i32 = 1;
pub const NAE_EQ_HIGH_SHELF: i32 = 2;
pub const NAE_EQ_HIGH_PASS: i32 = 3;
pub const NAE_EQ_LOW_PASS: i32 = 4;
pub const NAE_EQ_NOTCH: i32 = 5;
pub const NAE_EQ_ALL_PASS: i32 = 6;

pub const NAE_EQ_PHASE_MINIMUM: i32 = 0;
pub const NAE_EQ_PHASE_LINEAR: i32 = 1;

pub const NAE_RG_OFF: i32 = 0;
pub const NAE_RG_TRACK: i32 = 1;
pub const NAE_RG_ALBUM: i32 = 2;

pub const NAE_CROSSFADE_LINEAR: i32 = 0;
pub const NAE_CROSSFADE_EQUAL_POWER: i32 = 1;

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_EqBand {
    pub kind: i32,
    pub freq_hz: f64,
    pub gain_db: f64,
    pub q: f64,
    pub enabled: i32,
    /// Bit mask of channels the band applies to; 0 means every channel.
    pub channel_mask: u32,
}

impl Default for NAE_EqBand {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    assert!(offset_of!(NAE_EqBand, kind) == 0);
    assert!(offset_of!(NAE_EqBand, freq_hz) == 8);
    assert!(offset_of!(NAE_EqBand, gain_db) == 16);
    assert!(offset_of!(NAE_EqBand, q) == 24);
    assert!(offset_of!(NAE_EqBand, enabled) == 32);
    assert!(offset_of!(NAE_EqBand, channel_mask) == 36);
    assert!(std::mem::size_of::<NAE_EqBand>() == 40);
};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_EqParams {
    pub struct_size: u32,
    pub enabled: i32,
    pub preamp_db: f64,
    pub band_count: i32,
    pub bands: [NAE_EqBand; NAE_EQ_MAX_BANDS],
    /// `NAE_EQ_PHASE_MINIMUM` or `NAE_EQ_PHASE_LINEAR`.
    pub phase: i32,
    pub reserved0: u32,
}

impl Default for NAE_EqParams {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    assert!(offset_of!(NAE_EqParams, struct_size) == 0);
    assert!(offset_of!(NAE_EqParams, enabled) == 4);
    assert!(offset_of!(NAE_EqParams, preamp_db) == 8);
    assert!(offset_of!(NAE_EqParams, band_count) == 16);
    assert!(offset_of!(NAE_EqParams, bands) == 24);
    assert!(offset_of!(NAE_EqParams, phase) == 1304);
    assert!(offset_of!(NAE_EqParams, reserved0) == 1308);
    assert!(std::mem::size_of::<NAE_EqParams>() == 1312);
};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_CrossfeedParams {
    pub struct_size: u32,
    pub enabled: i32,
    pub amount: f64,
    pub delay_ms: f64,
    pub cutoff_hz: f64,
}

impl Default for NAE_CrossfeedParams {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    assert!(offset_of!(NAE_CrossfeedParams, struct_size) == 0);
    assert!(offset_of!(NAE_CrossfeedParams, enabled) == 4);
    assert!(offset_of!(NAE_CrossfeedParams, amount) == 8);
    assert!(offset_of!(NAE_CrossfeedParams, delay_ms) == 16);
    assert!(offset_of!(NAE_CrossfeedParams, cutoff_hz) == 24);
    assert!(std::mem::size_of::<NAE_CrossfeedParams>() == 32);
};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_ReplayGainParams {
    pub struct_size: u32,
    pub mode: i32,
    pub preamp_db: f64,
    pub prevent_clipping: i32,
    pub fallback_db: f64,
}

impl Default for NAE_ReplayGainParams {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    assert!(offset_of!(NAE_ReplayGainParams, struct_size) == 0);
    assert!(offset_of!(NAE_ReplayGainParams, mode) == 4);
    assert!(offset_of!(NAE_ReplayGainParams, preamp_db) == 8);
    assert!(offset_of!(NAE_ReplayGainParams, prevent_clipping) == 16);
    assert!(offset_of!(NAE_ReplayGainParams, fallback_db) == 24);
    assert!(std::mem::size_of::<NAE_ReplayGainParams>() == 32);
};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_ConvolverParams {
    pub struct_size: u32,
    pub enabled: i32,
    pub ir_path: [c_char; 512],
    pub wet: f64,
    pub dry: f64,
    pub gain_db: f64,
    pub normalize: i32,
}

impl Default for NAE_ConvolverParams {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    const C: usize = std::mem::size_of::<c_char>();
    assert!(offset_of!(NAE_ConvolverParams, struct_size) == 0);
    assert!(offset_of!(NAE_ConvolverParams, enabled) == 4);
    assert!(offset_of!(NAE_ConvolverParams, ir_path) == 8);
    assert!(offset_of!(NAE_ConvolverParams, wet) == 520);
    assert!(offset_of!(NAE_ConvolverParams, dry) == 528);
    assert!(offset_of!(NAE_ConvolverParams, gain_db) == 536);
    assert!(offset_of!(NAE_ConvolverParams, normalize) == 544);
    assert!(std::mem::size_of::<NAE_ConvolverParams>() == 552);
    assert!(512 * C == 512);
};

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NAE_CrossfadeParams {
    pub struct_size: u32,
    pub enabled: i32,
    pub duration_ms: f64,
    pub curve: i32,
}

impl Default for NAE_CrossfadeParams {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

const _: () = {
    use std::mem::offset_of;
    assert!(offset_of!(NAE_CrossfadeParams, struct_size) == 0);
    assert!(offset_of!(NAE_CrossfadeParams, enabled) == 4);
    assert!(offset_of!(NAE_CrossfadeParams, duration_ms) == 8);
    assert!(offset_of!(NAE_CrossfadeParams, curve) == 16);
    assert!(std::mem::size_of::<NAE_CrossfadeParams>() == 24);
};

extern "C" {
    pub fn NAE_EqDefaults(out: *mut NAE_EqParams);
    pub fn NAE_CrossfeedDefaults(out: *mut NAE_CrossfeedParams);
    pub fn NAE_ReplayGainDefaults(out: *mut NAE_ReplayGainParams);
    pub fn NAE_ConvolverDefaults(out: *mut NAE_ConvolverParams);
    pub fn NAE_CrossfadeDefaults(out: *mut NAE_CrossfadeParams);

    pub fn NAE_SessionSetVolume(session: *mut NAE_Session, gain: f64) -> i32;
    pub fn NAE_SessionGetVolume(session: *mut NAE_Session) -> f64;
    pub fn NAE_SessionSetEq(session: *mut NAE_Session, params: *const NAE_EqParams) -> i32;
    pub fn NAE_SessionGetEq(session: *mut NAE_Session, out: *mut NAE_EqParams) -> i32;
    pub fn NAE_SessionSetCrossfeed(
        session: *mut NAE_Session,
        params: *const NAE_CrossfeedParams,
    ) -> i32;
    pub fn NAE_SessionGetCrossfeed(
        session: *mut NAE_Session,
        out: *mut NAE_CrossfeedParams,
    ) -> i32;
    pub fn NAE_SessionSetReplayGain(
        session: *mut NAE_Session,
        params: *const NAE_ReplayGainParams,
    ) -> i32;
    pub fn NAE_SessionGetReplayGain(
        session: *mut NAE_Session,
        out: *mut NAE_ReplayGainParams,
    ) -> i32;
    pub fn NAE_SessionSetConvolver(
        session: *mut NAE_Session,
        params: *const NAE_ConvolverParams,
    ) -> i32;
    pub fn NAE_SessionGetConvolver(
        session: *mut NAE_Session,
        out: *mut NAE_ConvolverParams,
    ) -> i32;
    pub fn NAE_SessionSetCrossfade(
        session: *mut NAE_Session,
        params: *const NAE_CrossfadeParams,
    ) -> i32;
    pub fn NAE_SessionGetCrossfade(
        session: *mut NAE_Session,
        out: *mut NAE_CrossfadeParams,
    ) -> i32;
}
