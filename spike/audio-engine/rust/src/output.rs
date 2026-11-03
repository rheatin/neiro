//! Output rate policy, mirrored from the engine's `output_mode.h`.
//!
//! Kept in the wrapper rather than only in C++ so the frontend can be offered
//! the same three choices the engine implements, and so a mode is named the same
//! way at every layer. The values match the `NAE_OUTPUT_*` constants.

use crate::ffi;

/// How the output sample rate is chosen.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutputMode {
    /// The source rate as-is. The device is reformatted to match, so no
    /// conversion happens in our path and the file's samples reach the DAC
    /// unchanged. The only mode that is unconditionally bit-perfect.
    Native,
    /// Convert to the device's own rate with soxr. The default, and the right
    /// answer for most files: a 44.1 kHz CD on a 192 kHz interface.
    Device,
    /// Convert to an explicit rate, usually so the signal path reads the same
    /// regardless of device. Costs a conversion that may be pointless, and the UI
    /// should say so.
    Fixed(u32),
}

impl OutputMode {
    /// The `NAE_OUTPUT_*` value plus, for `Fixed`, the rate to use.
    pub(crate) fn as_raw(self) -> (i32, i32) {
        match self {
            OutputMode::Native => (ffi::NAE_OUTPUT_NATIVE, 0),
            OutputMode::Device => (ffi::NAE_OUTPUT_DEVICE, 0),
            OutputMode::Fixed(rate) => (ffi::NAE_OUTPUT_FIXED, rate as i32),
        }
    }
}

/// Resampler quality, mirrored from the engine's `ResamplerQuality`.
///
/// The trade is CPU against the top octave. `High` is the default; `VeryHigh`
/// is worth it when the rates genuinely differ, which with a 192 kHz interface is
/// most of the time.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Quality {
    /// soxr "quick". Roughly an order of magnitude cheaper, audibly duller above
    /// about 8 kHz. For scrub previews.
    Fast,
    /// soxr "high", 20-bit precision. The default.
    High,
    /// soxr "very high", 28-bit precision.
    VeryHigh,
}

impl Quality {
    pub(crate) fn as_raw(self) -> i32 {
        match self {
            Quality::Fast => ffi::NAE_QUALITY_FAST,
            Quality::High => ffi::NAE_QUALITY_HIGH,
            Quality::VeryHigh => ffi::NAE_QUALITY_VERY_HIGH,
        }
    }
}
