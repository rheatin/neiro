//! The Base contract: commands in, facts and events out.
//!
//! These types are serializable on purpose. The in-process UI can use the Rust
//! API directly, and a future out-of-process UI can carry exactly these values
//! over a local socket without inventing a second contract.
//!
//! The Base owns the queue, the scheduler and the cache. The engine owns the
//! device, the render clock and the samples; the analyzer owns a path-to-facts
//! computation. Nothing here re-derives a device format or a position.

use std::path::PathBuf;

use neiro_audio::{
    Analysis, CrossfadeParams, CrossfeedParams, ConvolverParams, EqParams, ReplayGainParams,
    SessionPhase, SessionSettings, SessionState,
};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PlaybackState {
    Stopped,
    Ready,
    Playing,
    Paused,
    DeviceLost,
    Ended,
    Error,
}

impl Default for PlaybackState {
    fn default() -> Self {
        Self::Stopped
    }
}

impl From<SessionPhase> for PlaybackState {
    fn from(phase: SessionPhase) -> Self {
        match phase {
            SessionPhase::Idle => Self::Stopped,
            SessionPhase::Ready => Self::Ready,
            SessionPhase::Playing => Self::Playing,
            SessionPhase::Paused => Self::Paused,
            SessionPhase::DeviceLost => Self::DeviceLost,
            SessionPhase::Ended => Self::Ended,
            SessionPhase::Error => Self::Error,
        }
    }
}

/// A complete, point-in-time view for a UI that just connected.
///
/// Events describe changes; this is the state those changes landed in. A UI
/// must be able to paint from this alone, because events can be missed while a
/// client is starting.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Snapshot {
    pub state: PlaybackState,
    pub path: Option<PathBuf>,
    pub device_id: Option<String>,
    pub position: f64,
    pub duration: f64,
    /// The render path the engine actually reports: resampling, dithering,
    /// every DSP stage that is active, and the bit-perfect reason. Never
    /// reconstructed from the settings the UI asked for.
    pub render_path: Option<RenderPath>,
    pub queue: Vec<PathBuf>,
    pub queue_index: Option<usize>,
    /// The analysis for the current track, if it has arrived. `None` is
    /// "not measured yet", never "this track has no facts".
    pub analysis: Option<Analysis>,
    /// The engine's actual resolved path for this track, including diagnostics.
    /// The fields are the engine JSON contract, not a second derivation.
    pub resolved: Option<serde_json::Value>,
    /// The applied DSP parameters, read back from the session. Separate from
    /// `render_path`: this says what was configured, `render_path` says what is
    /// actually active on this stream.
    pub settings: Option<SessionSettings>,
    /// Included so a cache or a UI never has to hard-code the algorithm version.
    pub analyzer_version: u32,
}

/// The engine's current render chain, as one fact.
///
/// This is deliberately not "what the user enabled". A requested stage can be
/// bypassed (flat EQ, equal source/device rates), a convolver can fail to load,
/// and zero-length crossfade is gapless rather than a crossfade. `bit_perfect`
/// and `reason` are the engine's call.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct RenderPath {
    pub source_rate: u32,
    pub sample_rate: u32,
    pub channels: u32,
    pub bits_per_sample: u32,
    pub valid_bits: u32,
    pub is_float: bool,
    pub resampling: bool,
    pub dithered: bool,
    pub bit_perfect: bool,
    pub reason: i32,
    pub file_channels: u32,
    pub latency_ms: f64,
    pub eq_enabled: bool,
    pub crossfeed_enabled: bool,
    pub replay_gain_enabled: bool,
    pub convolver_enabled: bool,
    pub crossfade_enabled: bool,
    pub volume: f64,
    pub rg_track_valid: bool,
    pub rg_album_valid: bool,
    pub rg_track_gain_db: f64,
    pub rg_album_gain_db: f64,
    pub rg_applied_gain: f64,
}

impl From<SessionState> for RenderPath {
    fn from(state: SessionState) -> Self {
        Self {
            source_rate: state.source_rate,
            sample_rate: state.sample_rate,
            channels: state.channels,
            bits_per_sample: state.bits_per_sample,
            valid_bits: state.valid_bits,
            is_float: state.is_float,
            resampling: state.resampling,
            dithered: state.dithered,
            bit_perfect: state.bit_perfect,
            reason: state.reason,
            file_channels: state.file_channels,
            latency_ms: state.latency_ms,
            eq_enabled: state.eq_enabled,
            crossfeed_enabled: state.crossfeed_enabled,
            replay_gain_enabled: state.replay_gain_enabled,
            convolver_enabled: state.convolver_enabled,
            crossfade_enabled: state.crossfade_enabled,
            volume: state.volume,
            rg_track_valid: state.rg_track_valid,
            rg_album_valid: state.rg_album_valid,
            rg_track_gain_db: state.rg_track_gain_db,
            rg_album_gain_db: state.rg_album_gain_db,
            rg_applied_gain: state.rg_applied_gain,
        }
    }
}

/// What a client asks Base to do.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "command", rename_all = "snake_case")]
pub enum Command {
    /// Replace the queue with this one track and start it.
    Play {
        path: PathBuf,
        /// `None` means "the endpoint last used"; if there is none yet, the
        /// current default is selected once. It never silently moves to another
        /// endpoint after a device-lost error.
        device_id: Option<String>,
    },
    /// Append tracks to the existing queue. Each one is pre-analyzed.
    Enqueue {
        paths: Vec<PathBuf>,
    },
    Pause,
    Resume,
    Stop,
    Seek {
        seconds: f64,
    },
    /// Stop the current track and open the next queue entry.
    Next,
    /// Analyze a path now. The result is also cached.
    Analyze {
        path: PathBuf,
    },
    SetVolume {
        gain: f64,
    },
    SetEq {
        params: EqParams,
    },
    SetCrossfeed {
        params: CrossfeedParams,
    },
    SetReplayGain {
        params: ReplayGainParams,
    },
    SetConvolver {
        params: ConvolverParams,
    },
    SetCrossfade {
        params: CrossfadeParams,
    },
    /// Stop playback, close the stream and end both workers.
    Shutdown,
}

/// What Base observed.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "event", rename_all = "snake_case")]
pub enum Event {
    /// A state transition or a track/queue change. Carries the whole snapshot
    /// so a late subscriber does not have to stitch several events together.
    State(Snapshot),
    TrackChanged {
        path: PathBuf,
        device_id: String,
        render_path: RenderPath,
    },
    Position {
        path: PathBuf,
        position: f64,
        duration: f64,
    },
    /// A DSP stage or the resampler was enabled/disabled, or a track change
    /// changed the path. This is copied from `NAE_SessionState`.
    RenderPathChanged {
        path: PathBuf,
        render_path: RenderPath,
    },
    /// The applied DSP parameters were changed or read after a setter.
    SettingsChanged {
        settings: SessionSettings,
    },
    /// The engine re-resolved its path for a new track.
    ResolvedPathChanged {
        resolved: serde_json::Value,
    },
    /// A path's analysis is available. `cached` is true when this came from the
    /// Base cache rather than a fresh decode.
    AnalysisReady {
        path: PathBuf,
        analysis: Analysis,
        cached: bool,
    },
    QueueChanged {
        queue: Vec<PathBuf>,
        index: Option<usize>,
    },
    Error {
        op: String,
        message: String,
    },
    Shutdown,
}
