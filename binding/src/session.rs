//! Safe wrapper for the engine's session API.
//!
//! The session owns the playback thread, the stream and the queue. A host sends
//! play/pause/seek/stop and reads a lock-free snapshot; it does not drive
//! `NAE_Play` slices itself. That is the difference between this module and
//! [`crate::Stream`], and the reason Base uses this one.
//!
//! `SessionState` is also the render-path fact: it already combines the open
//! signal path with the enabled EQ, crossfeed, convolver, crossfade and
//! ReplayGain stages, and the `bitPerfect` reason. A UI displays this; it does
//! not infer a stage from the command that configured it.

use serde::{Deserialize, Serialize};
use std::ffi::CString;
use std::path::{Path, PathBuf};
use std::ptr;

use crate::error::{EngineError, Result};
use crate::ffi;
use crate::output::{OutputMode, Quality};

/// One reading of a running session.
#[derive(Debug, Clone, Default)]
pub struct SessionState {
    pub state: i32,
    pub position: f64,
    pub duration: f64,
    pub sample_rate: u32,
    pub source_rate: u32,
    pub channels: u32,
    pub bits_per_sample: u32,
    pub valid_bits: u32,
    pub is_float: bool,
    pub bit_perfect: bool,
    /// A `NAE_REASON_*` value; 0 when `bit_perfect`.
    pub reason: i32,
    pub resampling: bool,
    pub dithered: bool,
    pub file_channels: u32,
    pub latency_ms: f64,
    pub volume: f64,
    pub eq_enabled: bool,
    pub crossfeed_enabled: bool,
    pub replay_gain_enabled: bool,
    pub convolver_enabled: bool,
    pub crossfade_enabled: bool,
    pub rg_track_valid: bool,
    pub rg_album_valid: bool,
    pub rg_track_gain_db: f64,
    pub rg_album_gain_db: f64,
    pub rg_applied_gain: f64,
    /// Increments when a preloaded track takes over.
    pub track_serial: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionPhase {
    Idle,
    Ready,
    Playing,
    Paused,
    DeviceLost,
    Ended,
    Error,
}

impl SessionState {
    pub fn phase(&self) -> SessionPhase {
        match self.state {
            ffi::NAE_SESSION_IDLE => SessionPhase::Idle,
            ffi::NAE_SESSION_READY => SessionPhase::Ready,
            ffi::NAE_SESSION_PLAYING => SessionPhase::Playing,
            ffi::NAE_SESSION_PAUSED => SessionPhase::Paused,
            ffi::NAE_SESSION_DEVICE_LOST => SessionPhase::DeviceLost,
            ffi::NAE_SESSION_ENDED => SessionPhase::Ended,
            ffi::NAE_SESSION_ERROR => SessionPhase::Error,
            _ => SessionPhase::Error,
        }
    }
}

/// An open session. Owns the C handle and closes it exactly once, in `Drop`.
pub struct Session {
    raw: *mut ffi::NAE_Session,
    path: PathBuf,
    device_id: Option<String>,
    share: ShareKind,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShareKind {
    Exclusive,
    Shared,
}

impl Session {
    /// Open `path` and `device_id` (or the endpoint last used / system default
    /// when `None`), then start the session thread.
    ///
    /// The session is left in `READY`; call [`play`](Self::play) to start it.
    /// On error the engine may still have produced a session in `ERROR`; this
    /// method reads its last error and closes it before returning.
    pub fn open(
        path: &Path,
        device_id: Option<&str>,
        mode: OutputMode,
        quality: Quality,
        share: ShareKind,
    ) -> Result<Self> {
        let path_str = path
            .to_str()
            .ok_or_else(|| EngineError::BadPayload("path is not valid UTF-8".into()))?;
        let path_c = CString::new(path_str)
            .map_err(|_| EngineError::BadPayload("path contains an interior NUL".into()))?;
        let device_c = match device_id {
            Some(id) => Some(CString::new(id).map_err(|_| {
                EngineError::BadPayload("device id contains an interior NUL".into())
            })?),
            None => None,
        };

        let (mode_raw, fixed_rate) = mode.as_raw();
        let config = ffi::NAE_SessionConfig {
            struct_size: std::mem::size_of::<ffi::NAE_SessionConfig>() as u32,
            device_id: device_c.as_ref().map_or(ptr::null(), |c| c.as_ptr()),
            path: path_c.as_ptr(),
            output_mode: mode_raw,
            fixed_rate,
            quality: quality.as_raw(),
            share_mode: match share {
                ShareKind::Exclusive => ffi::NAE_SHARE_EXCLUSIVE,
                ShareKind::Shared => ffi::NAE_SHARE_SHARED,
            },
            slice_ms: 0,
        };

        let mut raw: *mut ffi::NAE_Session = ptr::null_mut();
        // SAFETY: `config` points at live locals, and its two string pointers
        // outlive the call. `raw` is a live out-parameter.
        let rc = unsafe { ffi::NAE_SessionOpen(&config, &mut raw) };
        if rc != ffi::NAE_OK {
            let error = if raw.is_null() {
                EngineError::from_code(rc)
            } else {
                let error = session_error(raw, rc);
                // The contract says a session in ERROR is still valid and must
                // be released; do that before returning.
                unsafe { ffi::NAE_SessionClose(raw) };
                error
            };
            return Err(error);
        }
        if raw.is_null() {
            return Err(EngineError::BadPayload(
                "session reported success with a null handle".into(),
            ));
        }

        Ok(Self {
            raw,
            path: path.to_path_buf(),
            device_id: device_id.map(str::to_owned),
            share,
        })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn device_id(&self) -> Option<&str> {
        self.device_id.as_deref()
    }

    pub fn share(&self) -> ShareKind {
        self.share
    }

    pub fn play(&mut self) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionPlay(self.raw) })
    }

    pub fn pause(&mut self) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionPause(self.raw) })
    }

    pub fn resume(&mut self) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionResume(self.raw) })
    }

    pub fn seek(&mut self, seconds: f64) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionSeek(self.raw, seconds) })
    }

    pub fn stop(&mut self) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionStop(self.raw) })
    }

    pub fn state(&self) -> Result<SessionState> {
        let mut raw = ffi::NAE_SessionState {
            struct_size: std::mem::size_of::<ffi::NAE_SessionState>() as u32,
            ..Default::default()
        };
        // SAFETY: `raw` is a live, correctly-sized, zeroed struct; the session
        // writes at most sizeof(NAE_SessionState) into it.
        let rc = unsafe { ffi::NAE_SessionGetState(self.raw, &mut raw) };
        if rc != ffi::NAE_OK {
            return Err(session_error(self.raw, rc));
        }
        Ok(SessionState {
            state: raw.state,
            position: raw.position,
            duration: raw.duration,
            sample_rate: raw.sample_rate.max(0) as u32,
            source_rate: raw.source_rate.max(0) as u32,
            channels: raw.channels.max(0) as u32,
            bits_per_sample: raw.bits_per_sample.max(0) as u32,
            valid_bits: raw.valid_bits.max(0) as u32,
            is_float: raw.is_float != 0,
            bit_perfect: raw.bit_perfect != 0,
            reason: raw.reason,
            resampling: raw.resampling != 0,
            dithered: raw.dithered != 0,
            file_channels: raw.file_channels.max(0) as u32,
            latency_ms: raw.latency_ms,
            volume: raw.volume,
            eq_enabled: raw.eq_enabled != 0,
            crossfeed_enabled: raw.crossfeed_enabled != 0,
            replay_gain_enabled: raw.replay_gain_enabled != 0,
            convolver_enabled: raw.convolver_enabled != 0,
            crossfade_enabled: raw.crossfade_enabled != 0,
            rg_track_valid: raw.rg_track_valid != 0,
            rg_album_valid: raw.rg_album_valid != 0,
            rg_track_gain_db: raw.rg_track_gain_db,
            rg_album_gain_db: raw.rg_album_gain_db,
            rg_applied_gain: raw.rg_applied_gain,
            track_serial: raw.track_serial,
        })
    }

    pub fn queue_add(&mut self, path: &Path) -> Result<()> {
        let text = path
            .to_str()
            .ok_or_else(|| EngineError::BadPayload("path is not valid UTF-8".into()))?;
        let c = CString::new(text)
            .map_err(|_| EngineError::BadPayload("path contains an interior NUL".into()))?;
        self.check(unsafe { ffi::NAE_SessionQueueAdd(self.raw, c.as_ptr()) })
    }

    /// Switch to the next queued track without closing the stream.
    ///
    /// This is the manual skip. The session thread asks the handle to switch
    /// between render slices, so the device stays open and the negotiated format
    /// is kept -- the same seam gapless uses. Returns an error when nothing is
    /// preloaded, which is what a queue that has run out looks like.
    pub fn skip_to_next(&mut self) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionSkipToNext(self.raw) })
    }

    pub fn queue_clear(&mut self) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionQueueClear(self.raw) })
    }

    pub fn queue_count(&self) -> Result<i32> {
        let rc = unsafe { ffi::NAE_SessionQueueCount(self.raw) };
        if rc < 0 {
            return Err(session_error(self.raw, rc));
        }
        Ok(rc)
    }

    /// The output spectrum tap, as the session thread last published it.
    ///
    /// The handle-level `Stream::spectrum` cannot be used here: the handle
    /// belongs to the session thread. This reads the copy that thread publishes
    /// after every slice, which is why it is on `Session` rather than on
    /// `Stream`.
    pub fn spectrum(&self) -> Result<crate::engine::Spectrum> {
        let mut info = ffi::NAE_SpectrumInfo {
            struct_size: std::mem::size_of::<ffi::NAE_SpectrumInfo>() as u32,
            ..Default::default()
        };
        let mut bins = vec![0.0f32; 4096];
        // SAFETY: `info` is a live, correctly-sized struct; `bins` has exactly
        // the capacity passed, and the engine writes no more than that.
        let rc = unsafe {
            ffi::NAE_SessionQuerySpectrum(
                self.raw,
                &mut info,
                bins.as_mut_ptr(),
                bins.len() as i32,
            )
        };
        if rc != ffi::NAE_OK {
            return Err(session_error(self.raw, rc));
        }
        bins.truncate(info.bin_count as usize);
        Ok(crate::engine::Spectrum {
            bins,
            fft_size: info.fft_size,
            sample_rate: info.sample_rate,
            valid_bits: info.valid_bits,
            peak: info.peak,
            rms: info.rms,
            frames_fed: info.frames_fed,
            complete: true,
        })
    }

    /// The time-domain envelope, as the session thread last published it.
    pub fn envelope(&self) -> Result<crate::engine::Envelope> {
        let mut info = ffi::NAE_EnvelopeInfo {
            struct_size: std::mem::size_of::<ffi::NAE_EnvelopeInfo>() as u32,
            ..Default::default()
        };
        let mut frames = vec![0.0f32; 4096];
        // SAFETY: as above.
        let rc = unsafe {
            ffi::NAE_SessionQueryEnvelope(
                self.raw,
                &mut info,
                frames.as_mut_ptr(),
                frames.len() as i32,
            )
        };
        if rc != ffi::NAE_OK {
            return Err(session_error(self.raw, rc));
        }
        frames.truncate(info.frame_valid as usize);
        Ok(crate::engine::Envelope {
            frames,
            frame_hop_ms: info.frame_hop_ms,
            sample_rate: info.sample_rate,
            frames_fed: info.frames_fed,
            window_ms: f64::from(info.frame_count) * info.frame_hop_ms,
            complete: true,
        })
    }

    /// The engine's actual resolved path for the current track, as JSON.
    /// `None` before the session is ready or if the engine did not produce it.
    pub fn resolved_json(&self) -> Option<serde_json::Value> {
        // SAFETY: the session owns the handle; the returned string is engine
        // memory and is released with NAE_FreeString, never with Rust's allocator.
        let ptr = unsafe { ffi::NAE_SessionGetResolvedSignalPathJson(self.raw) };
        if ptr.is_null() {
            return None;
        }
        let text = unsafe { std::ffi::CStr::from_ptr(ptr) }
            .to_string_lossy()
            .into_owned();
        unsafe { ffi::NAE_FreeString(ptr) };
        serde_json::from_str(&text).ok()
    }

    pub fn last_error(&self) -> String {
        let mut buf = [0i8; 512];
        // SAFETY: `buf` is caller-owned and the session writes at most
        // `buf.len() - 1` bytes plus a NUL.
        let written =
            unsafe { ffi::NAE_SessionLastError(self.raw, buf.as_mut_ptr(), buf.len() as i32) };
        if written <= 0 {
            return String::new();
        }
        let bytes =
            unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, written as usize) };
        String::from_utf8_lossy(bytes).into_owned()
    }

    fn check(&self, code: i32) -> Result<()> {
        if code == ffi::NAE_OK {
            Ok(())
        } else {
            Err(session_error(self.raw, code))
        }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // SAFETY: `raw` came from a successful `NAE_SessionOpen`; the session
        // stops and joins its thread, then releases the handle. Called once.
        unsafe { ffi::NAE_SessionClose(self.raw) };
    }
}

fn session_error(raw: *mut ffi::NAE_Session, code: i32) -> EngineError {
    let message = {
        let mut buf = [0i8; 512];
        let written = unsafe { ffi::NAE_SessionLastError(raw, buf.as_mut_ptr(), buf.len() as i32) };
        if written <= 0 {
            String::new()
        } else {
            let bytes =
                unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, written as usize) };
            String::from_utf8_lossy(bytes).into_owned()
        }
    };
    EngineError::Code {
        code,
        hresult: 0,
        message,
    }
}

// ---- session DSP settings ---------------------------------------------------

pub const EQ_MAX_BANDS: usize = ffi::NAE_EQ_MAX_BANDS;
pub const EQ_PEAKING: i32 = ffi::NAE_EQ_PEAKING;
pub const EQ_LOW_SHELF: i32 = ffi::NAE_EQ_LOW_SHELF;
pub const EQ_HIGH_SHELF: i32 = ffi::NAE_EQ_HIGH_SHELF;
pub const EQ_HIGH_PASS: i32 = ffi::NAE_EQ_HIGH_PASS;
pub const EQ_LOW_PASS: i32 = ffi::NAE_EQ_LOW_PASS;
pub const EQ_NOTCH: i32 = ffi::NAE_EQ_NOTCH;
pub const EQ_ALL_PASS: i32 = ffi::NAE_EQ_ALL_PASS;
pub const EQ_PHASE_MINIMUM: i32 = ffi::NAE_EQ_PHASE_MINIMUM;
pub const EQ_PHASE_LINEAR: i32 = ffi::NAE_EQ_PHASE_LINEAR;
pub const RG_OFF: i32 = ffi::NAE_RG_OFF;
pub const RG_TRACK: i32 = ffi::NAE_RG_TRACK;
pub const RG_ALBUM: i32 = ffi::NAE_RG_ALBUM;
pub const CROSSFADE_LINEAR: i32 = ffi::NAE_CROSSFADE_LINEAR;
pub const CROSSFADE_EQUAL_POWER: i32 = ffi::NAE_CROSSFADE_EQUAL_POWER;

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EqBand {
    /// One of the `EQ_*` constants.
    pub kind: i32,
    pub freq_hz: f64,
    pub gain_db: f64,
    pub q: f64,
    pub enabled: bool,
    /// Bit mask of channels this band applies to; 0 means every channel.
    #[serde(default)]
    pub channel_mask: u32,
}

impl EqBand {
    fn to_ffi(&self) -> ffi::NAE_EqBand {
        ffi::NAE_EqBand {
            kind: self.kind,
            freq_hz: self.freq_hz,
            gain_db: self.gain_db,
            q: self.q,
            enabled: i32::from(self.enabled),
            channel_mask: self.channel_mask,
        }
    }

    fn from_ffi(raw: ffi::NAE_EqBand) -> Self {
        Self {
            kind: raw.kind,
            freq_hz: raw.freq_hz,
            gain_db: raw.gain_db,
            q: raw.q,
            enabled: raw.enabled != 0,
            channel_mask: raw.channel_mask,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EqParams {
    pub enabled: bool,
    pub preamp_db: f64,
    pub bands: Vec<EqBand>,
    /// `EQ_PHASE_MINIMUM` (default) or `EQ_PHASE_LINEAR`.
    #[serde(default)]
    pub phase: i32,
}

impl Default for EqParams {
    fn default() -> Self {
        let mut raw = ffi::NAE_EqParams::default();
        // SAFETY: `raw` is a live, correctly-sized struct of the declared type.
        unsafe { ffi::NAE_EqDefaults(&mut raw) };
        Self::from_ffi(&raw)
    }
}

impl EqParams {
    fn to_ffi(&self) -> std::result::Result<ffi::NAE_EqParams, EngineError> {
        if self.bands.len() > EQ_MAX_BANDS {
            return Err(EngineError::BadPayload(format!(
                "at most {EQ_MAX_BANDS} EQ bands"
            )));
        }
        let mut raw = ffi::NAE_EqParams {
            struct_size: std::mem::size_of::<ffi::NAE_EqParams>() as u32,
            ..Default::default()
        };
        raw.enabled = i32::from(self.enabled);
        raw.preamp_db = self.preamp_db;
        raw.band_count = self.bands.len() as i32;
        raw.phase = self.phase;
        for (slot, band) in raw.bands.iter_mut().zip(self.bands.iter()) {
            *slot = band.to_ffi();
        }
        Ok(raw)
    }

    fn from_ffi(raw: &ffi::NAE_EqParams) -> Self {
        let count = (raw.band_count.max(0) as usize).min(EQ_MAX_BANDS);
        Self {
            enabled: raw.enabled != 0,
            preamp_db: raw.preamp_db,
            phase: raw.phase,
            bands: raw.bands[..count]
                .iter()
                .copied()
                .map(EqBand::from_ffi)
                .collect(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CrossfeedParams {
    pub enabled: bool,
    pub amount: f64,
    pub delay_ms: f64,
    pub cutoff_hz: f64,
}

impl Default for CrossfeedParams {
    fn default() -> Self {
        let mut raw = ffi::NAE_CrossfeedParams::default();
        unsafe { ffi::NAE_CrossfeedDefaults(&mut raw) };
        Self::from_ffi(&raw)
    }
}

impl CrossfeedParams {
    fn to_ffi(&self) -> ffi::NAE_CrossfeedParams {
        ffi::NAE_CrossfeedParams {
            struct_size: std::mem::size_of::<ffi::NAE_CrossfeedParams>() as u32,
            enabled: i32::from(self.enabled),
            amount: self.amount,
            delay_ms: self.delay_ms,
            cutoff_hz: self.cutoff_hz,
        }
    }

    fn from_ffi(raw: &ffi::NAE_CrossfeedParams) -> Self {
        Self {
            enabled: raw.enabled != 0,
            amount: raw.amount,
            delay_ms: raw.delay_ms,
            cutoff_hz: raw.cutoff_hz,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ReplayGainParams {
    /// `RG_OFF`, `RG_TRACK` or `RG_ALBUM`.
    pub mode: i32,
    pub preamp_db: f64,
    pub prevent_clipping: bool,
    pub fallback_db: f64,
}

impl Default for ReplayGainParams {
    fn default() -> Self {
        let mut raw = ffi::NAE_ReplayGainParams::default();
        unsafe { ffi::NAE_ReplayGainDefaults(&mut raw) };
        Self::from_ffi(&raw)
    }
}

impl ReplayGainParams {
    fn to_ffi(&self) -> ffi::NAE_ReplayGainParams {
        ffi::NAE_ReplayGainParams {
            struct_size: std::mem::size_of::<ffi::NAE_ReplayGainParams>() as u32,
            mode: self.mode,
            preamp_db: self.preamp_db,
            prevent_clipping: i32::from(self.prevent_clipping),
            fallback_db: self.fallback_db,
        }
    }

    fn from_ffi(raw: &ffi::NAE_ReplayGainParams) -> Self {
        Self {
            mode: raw.mode,
            preamp_db: raw.preamp_db,
            prevent_clipping: raw.prevent_clipping != 0,
            fallback_db: raw.fallback_db,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ConvolverParams {
    pub enabled: bool,
    pub ir_path: String,
    pub wet: f64,
    pub dry: f64,
    pub gain_db: f64,
    pub normalize: bool,
}

impl Default for ConvolverParams {
    fn default() -> Self {
        let mut raw = ffi::NAE_ConvolverParams::default();
        unsafe { ffi::NAE_ConvolverDefaults(&mut raw) };
        Self::from_ffi(&raw)
    }
}

impl ConvolverParams {
    fn to_ffi(&self) -> std::result::Result<ffi::NAE_ConvolverParams, EngineError> {
        let path = CString::new(self.ir_path.as_str())
            .map_err(|_| EngineError::BadPayload("IR path contains an interior NUL".into()))?;
        if path.as_bytes().len() >= 512 {
            return Err(EngineError::BadPayload(
                "IR path is longer than the 511-byte field".into(),
            ));
        }
        let mut raw = ffi::NAE_ConvolverParams {
            struct_size: std::mem::size_of::<ffi::NAE_ConvolverParams>() as u32,
            enabled: i32::from(self.enabled),
            wet: self.wet,
            dry: self.dry,
            gain_db: self.gain_db,
            normalize: i32::from(self.normalize),
            ..ffi::NAE_ConvolverParams::default()
        };
        for (dst, src) in raw.ir_path.iter_mut().zip(path.as_bytes().iter()) {
            *dst = *src as std::os::raw::c_char;
        }
        Ok(raw)
    }

    fn from_ffi(raw: &ffi::NAE_ConvolverParams) -> Self {
        Self {
            enabled: raw.enabled != 0,
            ir_path: fixed_string(&raw.ir_path),
            wet: raw.wet,
            dry: raw.dry,
            gain_db: raw.gain_db,
            normalize: raw.normalize != 0,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CrossfadeParams {
    pub enabled: bool,
    pub duration_ms: f64,
    /// `CROSSFADE_LINEAR` or `CROSSFADE_EQUAL_POWER`.
    pub curve: i32,
}

impl Default for CrossfadeParams {
    fn default() -> Self {
        let mut raw = ffi::NAE_CrossfadeParams::default();
        unsafe { ffi::NAE_CrossfadeDefaults(&mut raw) };
        Self::from_ffi(&raw)
    }
}

impl CrossfadeParams {
    fn to_ffi(&self) -> ffi::NAE_CrossfadeParams {
        ffi::NAE_CrossfadeParams {
            struct_size: std::mem::size_of::<ffi::NAE_CrossfadeParams>() as u32,
            enabled: i32::from(self.enabled),
            duration_ms: self.duration_ms,
            curve: self.curve,
        }
    }

    fn from_ffi(raw: &ffi::NAE_CrossfadeParams) -> Self {
        Self {
            enabled: raw.enabled != 0,
            duration_ms: raw.duration_ms,
            curve: raw.curve,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct SessionSettings {
    pub volume: f64,
    pub eq: EqParams,
    pub crossfeed: CrossfeedParams,
    pub replay_gain: ReplayGainParams,
    pub convolver: ConvolverParams,
    pub crossfade: CrossfadeParams,
}

impl Session {
    pub fn set_volume(&mut self, gain: f64) -> Result<()> {
        self.check(unsafe { ffi::NAE_SessionSetVolume(self.raw, gain) })
    }

    pub fn volume(&self) -> f64 {
        unsafe { ffi::NAE_SessionGetVolume(self.raw) }
    }

    pub fn set_eq(&mut self, params: &EqParams) -> Result<()> {
        let raw = params.to_ffi()?;
        self.check(unsafe { ffi::NAE_SessionSetEq(self.raw, &raw) })
    }

    pub fn eq(&self) -> Result<EqParams> {
        let mut raw = ffi::NAE_EqParams {
            struct_size: std::mem::size_of::<ffi::NAE_EqParams>() as u32,
            ..Default::default()
        };
        self.check(unsafe { ffi::NAE_SessionGetEq(self.raw, &mut raw) })?;
        Ok(EqParams::from_ffi(&raw))
    }

    pub fn set_crossfeed(&mut self, params: &CrossfeedParams) -> Result<()> {
        let raw = params.to_ffi();
        self.check(unsafe { ffi::NAE_SessionSetCrossfeed(self.raw, &raw) })
    }

    pub fn crossfeed(&self) -> Result<CrossfeedParams> {
        let mut raw = ffi::NAE_CrossfeedParams {
            struct_size: std::mem::size_of::<ffi::NAE_CrossfeedParams>() as u32,
            ..Default::default()
        };
        self.check(unsafe { ffi::NAE_SessionGetCrossfeed(self.raw, &mut raw) })?;
        Ok(CrossfeedParams::from_ffi(&raw))
    }

    pub fn set_replay_gain(&mut self, params: &ReplayGainParams) -> Result<()> {
        let raw = params.to_ffi();
        self.check(unsafe { ffi::NAE_SessionSetReplayGain(self.raw, &raw) })
    }

    pub fn replay_gain(&self) -> Result<ReplayGainParams> {
        let mut raw = ffi::NAE_ReplayGainParams {
            struct_size: std::mem::size_of::<ffi::NAE_ReplayGainParams>() as u32,
            ..Default::default()
        };
        self.check(unsafe { ffi::NAE_SessionGetReplayGain(self.raw, &mut raw) })?;
        Ok(ReplayGainParams::from_ffi(&raw))
    }

    pub fn set_convolver(&mut self, params: &ConvolverParams) -> Result<()> {
        let raw = params.to_ffi()?;
        self.check(unsafe { ffi::NAE_SessionSetConvolver(self.raw, &raw) })
    }

    pub fn convolver(&self) -> Result<ConvolverParams> {
        let mut raw = ffi::NAE_ConvolverParams {
            struct_size: std::mem::size_of::<ffi::NAE_ConvolverParams>() as u32,
            ..Default::default()
        };
        self.check(unsafe { ffi::NAE_SessionGetConvolver(self.raw, &mut raw) })?;
        Ok(ConvolverParams::from_ffi(&raw))
    }

    pub fn set_crossfade(&mut self, params: &CrossfadeParams) -> Result<()> {
        let raw = params.to_ffi();
        self.check(unsafe { ffi::NAE_SessionSetCrossfade(self.raw, &raw) })
    }

    pub fn crossfade(&self) -> Result<CrossfadeParams> {
        let mut raw = ffi::NAE_CrossfadeParams {
            struct_size: std::mem::size_of::<ffi::NAE_CrossfadeParams>() as u32,
            ..Default::default()
        };
        self.check(unsafe { ffi::NAE_SessionGetCrossfade(self.raw, &mut raw) })?;
        Ok(CrossfadeParams::from_ffi(&raw))
    }

    pub fn settings(&self) -> Result<SessionSettings> {
        Ok(SessionSettings {
            volume: self.volume(),
            eq: self.eq()?,
            crossfeed: self.crossfeed()?,
            replay_gain: self.replay_gain()?,
            convolver: self.convolver()?,
            crossfade: self.crossfade()?,
        })
    }
}

fn fixed_string(raw: &[std::os::raw::c_char]) -> String {
    let bytes: &[u8] = {
        let ptr = raw.as_ptr();
        let len = raw.len();
        // SAFETY: `ptr` points at this struct's own initialised char array.
        unsafe { std::slice::from_raw_parts(ptr.cast::<u8>(), len) }
    };
    let end = bytes.iter().position(|b| *b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}
