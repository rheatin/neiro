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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum PlaybackState {
    #[default]
    Stopped,
    Ready,
    Playing,
    Paused,
    DeviceLost,
    Ended,
    Error,
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

/// A track and how many times it has been played.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlayedTrack {
    pub path: PathBuf,
    pub play_count: i64,
}

/// One output endpoint, as the engine enumerated it.
///
/// Copied from the engine verbatim. Base does not cache a format and present it
/// as current: `sample_rate` and the bit fields are what the endpoint reported
/// when the list was taken, and a host that opened the device knows better than
/// to treat them as the live stream's format.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeviceInfo {
    pub id: String,
    pub name: String,
    /// The endpoint's native rate. 0 when it did not report one.
    pub sample_rate: u32,
    pub bits_per_sample: u32,
    pub valid_bits: u32,
    pub channels: u32,
    pub exclusive: bool,
    pub shared: bool,
}

/// How the engine should choose the output rate.
///
/// These are the engine's output modes, not a second vocabulary: `Native` asks
/// for the source rate, `Device` takes whatever the endpoint negotiates, and
/// `Fixed` asks for one rate and reports a coercion when the driver refuses it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case", tag = "mode")]
pub enum OutputRatePolicy {
    /// Open at the source rate. A track that the endpoint refuses is coerced,
    /// and the render path says so.
    Native,
    /// Open at the rate the endpoint grants for the source format.
    #[default]
    Device,
    /// Ask for this rate. Not a promise: the endpoint may refuse it.
    Fixed { rate_hz: u32 },
}


/// Resampler quality, mirrored from the engine's tiers.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum ResamplerQuality {
    Fast,
    #[default]
    High,
    VeryHigh,
}


/// WASAPI share mode. `Shared` is a deliberate choice for an environment where
/// another process holds the endpoint; it is never a fallback.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum ShareMode {
    #[default]
    Exclusive,
    Shared,
}


/// Everything that has to be decided before a stream can be opened.
///
/// These are open-time settings in the engine, so changing one while a track is
/// playing reopens the session at the current position. The resulting facts are
/// in `RenderPath`; this type is only the request.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct PlaybackPolicy {
    pub rate: OutputRatePolicy,
    pub quality: ResamplerQuality,
    pub share: ShareMode,
}

impl PlaybackPolicy {
    pub fn shared() -> Self {
        Self {
            share: ShareMode::Shared,
            ..Self::default()
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
    /// The endpoint the next open will use. `None` before one has been chosen.
    pub device_id: Option<String>,
    /// The endpoints the engine reported the last time they were listed.
    ///
    /// Empty means "not listed yet", not "this machine has none" -- the same
    /// rule the analysis plane follows for a missing measurement.
    pub devices: Vec<DeviceInfo>,
    pub position: f64,
    pub duration: f64,
    /// Increments inside one session each time a preloaded track takes over.
    ///
    /// This is how a UI tells a gapless or manual seam from a new stream: a seam
    /// keeps the same session and the counter moves, while a close and reopen
    /// starts a new session back at 0. The two look identical in every other
    /// field, which is why this one is carried.
    pub track_serial: u64,
    /// The render path the engine actually reports: resampling, dithering,
    /// every DSP stage that is active, and the bit-perfect reason. Never
    /// reconstructed from the settings the UI asked for.
    pub render_path: Option<RenderPath>,
    pub queue: Vec<PathBuf>,
    pub queue_index: Option<usize>,
    /// The policy the next open will use, and the one the current session was
    /// opened with. `render_path` says what the engine actually did with it;
    /// this says what was asked for.
    pub policy: PlaybackPolicy,
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
    /// Take one entry out of the queue by position.
    ///
    /// The entry that is currently playing cannot be removed this way: doing so
    /// would leave the stream playing a track the queue no longer names, and
    /// every path-derived fact would describe a file that is not there. `Stop`
    /// followed by `RemoveFromQueue` is the way to drop the current track.
    ///
    /// Removing an entry *before* the current index shifts both the index and
    /// the playing track's place in the queue; the engine's queue is rebuilt so
    /// the two stay in step.
    RemoveFromQueue {
        index: usize,
    },
    /// Move one queue entry to a different position.
    ///
    /// The entry that is playing can be moved: it keeps playing, and the index
    /// follows it. Moving an entry across the playing one shifts the playing
    /// entry's position, which is why the engine's queue is rebuilt afterwards.
    MoveInQueue {
        from: usize,
        to: usize,
    },
    /// Drop every queued entry except the one playing. The current track keeps
    /// playing and its index becomes 0.
    ClearQueue,
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
    /// Change how the output is opened. Takes effect immediately: an open
    /// session is reopened at the current position, because output mode and
    /// resampler quality are open-time settings in the engine.
    SetPolicy {
        policy: PlaybackPolicy,
    },
    /// Ask Base to list the endpoints. The answer arrives as a
    /// `devices_changed` event and is also stored in the snapshot.
    ListDevices,
    /// Choose the endpoint for the next open, and for the current one if a
    /// track is playing. This is separate from `Play` so a UI can offer a
    /// device menu without stopping playback or discarding the queue.
    ///
    /// `None` means "the endpoint last used"; if there is none, the system
    /// default is chosen once. It never silently moves to another endpoint
    /// after a device-lost error.
    SetDevice {
        device_id: Option<String>,
    },
    // ---- library --------------------------------------------------------
    //
    // Favourites, playlists and play counts. These are answered by the library
    // file rather than by the session, so they work with nothing playing -- a
    // listener can star a track before ever pressing play.
    SetFavourite {
        path: PathBuf,
        on: bool,
    },
    /// Ask for the favourite list. Answered as a `favourites_changed` event.
    ListFavourites,
    CreatePlaylist {
        name: String,
    },
    RenamePlaylist {
        id: i64,
        name: String,
    },
    DeletePlaylist {
        id: i64,
    },
    /// Ask for the playlist list. Answered as a `playlists_changed` event.
    ListPlaylists,
    AddToPlaylist {
        id: i64,
        path: PathBuf,
    },
    RemoveFromPlaylist {
        id: i64,
        path: PathBuf,
    },
    /// Ask for the entries of one playlist, in order.
    ListPlaylistEntries {
        id: i64,
    },
    /// Ask which playlists hold a track. Answered as
    /// `playlists_containing_changed`.
    ListPlaylistsContaining {
        path: PathBuf,
    },
    /// Record that a track was played. `at_ms` is the host's clock: Base does
    /// not own one and does not invent one.
    RecordPlay {
        path: PathBuf,
        at_ms: i64,
    },
    /// Ask for the most-played tracks, most first.
    ListMostPlayed {
        limit: usize,
    },
    /// Analyze a batch of paths ahead of playback.
    ///
    /// Base does not scan directories: the host supplies the paths, which keeps
    /// this layer from growing into a media library. Each path is decoded once
    /// and cached; results arrive as the usual `analysis_ready` events, and
    /// `preanalysis_progress` reports how far the batch has got.
    ///
    /// The batch is also what sizes the cache: a 5000-track scan against a
    /// 256-entry cache would evict its own early results.
    Preanalyze {
        paths: Vec<PathBuf>,
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
    ///
    /// Boxed: this variant is three times the size of the next largest, and
    /// every `Event` would otherwise be allocated at that size.
    State(Box<Snapshot>),
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
    /// The output policy changed. `render_path` in the next `State` says what
    /// the engine actually did with it.
    PolicyChanged {
        policy: PlaybackPolicy,
    },
    /// The endpoint list was refreshed. An empty list is a measurement -- the
    /// engine found none -- and is not the same as "not listed yet", which is
    /// the absence of this event.
    DevicesChanged {
        devices: Vec<DeviceInfo>,
    },
    /// The applied DSP parameters were changed or read after a setter.
    SettingsChanged {
        settings: SessionSettings,
    },
    /// The engine re-resolved its path for a new track.
    ResolvedPathChanged {
        resolved: serde_json::Value,
    },
    /// The favourite list, after a change or on request.
    FavouritesChanged {
        paths: Vec<PathBuf>,
    },
    /// The playlist list, after a change or on request. `track_count` is the
    /// size of each playlist, not the number loaded.
    PlaylistsChanged {
        playlists: Vec<crate::library::Playlist>,
    },
    /// One playlist's entries, in order.
    PlaylistEntriesChanged {
        id: i64,
        entries: Vec<crate::library::PlaylistEntry>,
    },
    /// The playlists that hold a path.
    PlaylistsContainingChanged {
        path: PathBuf,
        playlists: Vec<crate::library::Playlist>,
    },
    /// The most-played tracks, most first. Ties sort by path, so the order is
    /// stable across launches.
    MostPlayedChanged {
        tracks: Vec<PlayedTrack>,
    },
    /// How far a `preanalyze` batch has got.
    ///
    /// Reported rather than inferred from `analysis_ready` counts, because a
    /// path can fail and a path can already be cached -- neither of which
    /// produces a decode. `done` counts every path the batch has finished with,
    /// whatever the outcome.
    PreanalysisProgress {
        done: usize,
        total: usize,
        /// Set on the last event, so a UI knows the batch ended rather than
        /// paused.
        complete: bool,
    },
    /// A path's analysis is available. `cached` is true when this came from the
    /// Base cache rather than a fresh decode.
    AnalysisReady {
        path: PathBuf,
        /// Boxed: `Analysis` is the largest payload in the protocol, and every
        /// `Event` is allocated at the size of its largest variant.
        analysis: Box<Analysis>,
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
