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

/// One frame of the visualiser: the spectrum, and the waveform strip.
///
/// The spectrum is the main display and the envelope is the time axis, so both
/// travel together: two events at different rates would let a UI draw a
/// spectrum against a waveform from a different moment.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct VisualFrame {
    /// Linear magnitudes, `fft_size / 2` of them. A full-scale sine reads about
    /// 1.0 in its own bin.
    pub bins: Vec<f32>,
    pub fft_size: u32,
    /// The device rate the window was captured at.
    pub sample_rate: u32,
    pub peak: f64,
    pub rms: f64,
    /// Total frames the tap has been fed. A UI compares this with the previous
    /// frame to know whether it is looking at new audio.
    pub frames_fed: u64,
    /// The time-domain envelope, oldest first. Its length is what has been
    /// written, not the ring's capacity.
    pub envelope: Vec<f32>,
    pub envelope_hop_ms: f64,
}

/// One row of the browsing index.
///
/// These are the file's own tags, cached so a list can be drawn without opening
/// every file. `duration_ms`, `sample_rate`, `bits` and `channels` come from the
/// decode, not from the tag frame -- a WAV carries no duration in its tags, and
/// the engine already knows it.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct TrackRow {
    pub path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub disc_no: i64,
    pub track_no: i64,
    pub year: i64,
    pub genre: String,
    pub duration_ms: i64,
    pub sample_rate: i64,
    pub bits: i64,
    pub channels: i64,
}

/// One album, as the index groups them.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct AlbumSummary {
    pub album: String,
    pub album_artist: String,
    pub year: i64,
    pub track_count: i64,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ArtistSummary {
    pub artist: String,
    pub track_count: i64,
    pub album_count: i64,
}

/// A track and how many times it has been played.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PlayedTrack {
    pub path: PathBuf,
    pub play_count: i64,
}


/// One file that was renamed rather than deleted and added.
///
/// Detected by size and timestamp within a single reconciliation, which is why
/// this crate needs no persistent id column: the pairing rule is the identity.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Move {
    pub from: PathBuf,
    pub to: PathBuf,
}

/// The field names one tag family accepts, in the family's own vocabulary.
///
/// The names are not shared between families: ID3v2 calls the title `TIT2`,
/// Vorbis comments call it `TITLE`, APE calls it `Title`. A control that
/// offers fields has to know which family it is offering them for.

/// How many items `ListRemote` asks for when the caller does not say.
///
/// A page, not the library: a host that wants everything pages through it, and
/// a default that fetched ten thousand items would be the slowest possible
/// first request.
fn default_remote_limit() -> usize {
    200
}

#[derive(Debug, Clone, serde::Serialize, serde::Deserialize)]
pub struct TagKeyList {
    pub family: crate::tag::TagFamily,
    pub keys: Vec<String>,
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
    /// How many entries the *engine* holds after the current one.
    ///
    /// Separate from `queue.len() - queue_index - 1` on purpose: Base's copy and
    /// the engine's queue are two things that have to agree, and a UI that only
    /// sees Base's copy cannot tell when they do not. This is the engine's own
    /// count, reported as it reports it.
    pub engine_queue_count: Option<usize>,
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
    ///
    /// `cancel_id`, when given, names this request so `CancelAnalysis` can stop
    /// it. Without one the decode runs to completion -- a host that wants to
    /// abandon it has to be able to say which one it means.
    Analyze {
        path: PathBuf,
        #[serde(default)]
        cancel_id: Option<u64>,
    },
    /// Stop an analysis started with `Analyze { cancel_id }`.
    ///
    /// Cancellation is between decoded blocks, so the decode stops within one
    /// packet rather than instantly. A cancelled decode publishes nothing: its
    /// result is a prefix, not a measurement.
    CancelAnalysis {
        cancel_id: u64,
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
    /// The browsing index: one page of tracks, album/artist order.
    ListTracks {
        offset: usize,
        limit: usize,
    },
    ListAlbums,
    ListArtists,
    /// Substring match over title, artist, album and album artist.
    Search {
        query: String,
        limit: usize,
    },
    /// Read these files' tags into the index.
    ///
    /// A scan fills the index on its own; this is for a host that already has
    /// paths from somewhere else, and for re-indexing after a tag write.
    IndexTags {
        paths: Vec<PathBuf>,
    },
    // ---- tags -----------------------------------------------------------
    //
    // A file's own text. Not library state: the file is the owner, and an
    // answer describes one file at the moment it was read. The write goes
    // through a copy and a rename (`tag.rs`), so it costs a file copy and
    // belongs on the control thread rather than on the caller's.

    /// Read one file's tags. Answered as a `tags_read` event.
    ReadTags {
        path: PathBuf,
    },
    /// Write fields into one file. Answered as `tags_changed` once the
    /// replacement is in place, or as an `error` with `op` `"tags"`.
    ///
    /// `policy` has no default on purpose. Which families to write is the
    /// caller's decision, and a default would make it here: an empty policy
    /// writes nothing, and a preset one would pick ID3v2 or Vorbis on the
    /// caller's behalf.
    WriteTags {
        path: PathBuf,
        fields: Vec<crate::tag::TagFieldValue>,
        policy: crate::tag::TagPolicy,
    },
    /// The field names a family accepts, for a control to offer.
    ///
    /// Answered as `tag_keys_changed`. An empty `families` asks for all of
    /// them: the answer depends on the family and on no file, so a host asks
    /// once instead of per selection.
    ListTagKeys {
        #[serde(default)]
        families: Vec<crate::tag::TagFamily>,
    },
    /// The pictures a file embeds. Answered as `artwork_read`.
    ///
    /// Two questions in one command, because they are asked at different
    /// moments and only one of them is expensive:
    ///
    /// * `index` absent lists every picture with its type and size, and carries
    ///   no bytes. A tag editor opens with this.
    /// * `index` present also carries that picture's bytes, base64, for a view
    ///   that has to draw it.
    ///
    /// Splitting them keeps a track change from moving a megabyte of cover art
    /// through the bridge when the library grid already has a thumbnail.
    ReadArtwork {
        path: PathBuf,
        #[serde(default)]
        index: Option<usize>,
    },
    /// Put a picture into one family. Answered as `artwork_changed`.
    ///
    /// The family is named because pictures live in one: a file can carry the
    /// same cover in ID3v2 and in APE, each with its own list, so an index
    /// without a family addresses nothing.
    ///
    /// `index` present replaces that picture in the family's list; absent
    /// appends. `data` is base64, the same form `artwork_read` answers with.
    SetArtwork {
        path: PathBuf,
        family: crate::tag::TagFamily,
        #[serde(default)]
        index: Option<usize>,
        data: String,
        #[serde(default)]
        mime: Option<String>,
        /// The `PictureType` name. Absent means `CoverFront`: a caller that has
        /// bytes and nothing else is asking for a cover.
        #[serde(default)]
        kind: Option<String>,
        #[serde(default)]
        description: Option<String>,
    },
    /// Remove one picture from one family. Answered as `artwork_changed`.
    RemoveArtwork {
        path: PathBuf,
        family: crate::tag::TagFamily,
        index: usize,
    },
    // ---- remote (Jellyfin) ----------------------------------------------
    //
    // A remote library is fetched into local files and then played as files.
    // The engine stays audio-only and path-based: streaming would make its DSD
    // path, its analysis cache and its bit-perfect accounting each answer a
    // question about a URL they cannot measure.

    /// Point the remote worker at a server, or at nothing when `token` is
    /// empty. Answered as `remote_changed`.
    ///
    /// `cache_dir` is where downloaded files land. It has to be stable across
    /// runs, because favourites and playlists are stored by path and a cache
    /// directory that moved would orphan them.
    SetRemote {
        /// `null` disconnects. The two backends are tagged: Jellyfin has a
        /// token header, Navidrome signs every request with a salt.
        server: Option<crate::remote::Server>,
        cache_dir: PathBuf,
    },
    /// List the server's audio items. Answered as `remote_listed`.
    ListRemote {
        #[serde(default)]
        start: usize,
        #[serde(default = "default_remote_limit")]
        limit: usize,
    },
    /// Download one listed item into the cache. Answered as `remote_fetched`.
    ///
    /// The id is resolved against the last listing, so a fetch can only name
    /// something the server actually reported. A guess would put a file in the
    /// cache under a name the caller never saw.
    FetchRemote {
        id: String,
    },
    /// The cover of one listed track. Answered as `remote_artwork`, or as
    /// `remote_artwork` with no data when the server says there is none.
    FetchRemoteArtwork {
        id: String,
    },
    /// The server's playlists. Answered as `remote_playlists`.
    ListRemotePlaylists,
    /// The entries of one server playlist. Answered as `remote_playlist_items`.
    ListRemotePlaylistItems {
        id: String,
        #[serde(default)]
        start: usize,
        #[serde(default = "default_remote_limit")]
        limit: usize,
    },
    // ---- local folders ---------------------------------------------------
    //
    // Scanning and watching are Base's job now. Earlier this layer deliberately
    // did not scan, on the grounds that a host that supplies paths keeps Base
    // from becoming a media library. A folder the listener added is a fact
    // about the music, not about the host, and two hosts would disagree about
    // it -- so it lives here, next to the favourites that name the same files.

    /// Walk a folder and answer every audio file under it.
    ScanFolder {
        root: PathBuf,
    },
    /// Report changes under a folder from now on. Answered as `folder_watched`.
    WatchFolder {
        root: PathBuf,
    },
    UnwatchFolder {
        root: PathBuf,
    },
    /// Reconcile every watched root this often, on the folder worker.
    /// Answered as `reconcile_changed`. Zero turns it off.
    ///
    /// The reason this exists: a watcher reports the failures it is told about,
    /// and a network share loses events silently. A periodic reconciliation is
    /// the only thing that can see a silent loss, and it is cheap -- a 3,585
    /// file library over SMB walks in about six seconds.
    SetReconcileInterval {
        seconds: u64,
    },
    /// Start or stop forwarding visualiser frames.
    ///
    /// `None` stops. The interval is what the host can draw: a 60 Hz view asks
    /// for 16 ms, and a phone showing the spectrum only in a full-screen mode
    /// asks for it while that mode is open.
    SetVisualiser {
        interval_ms: Option<u32>,
    },
    /// Analyze a batch of paths ahead of playback.
    ///
    /// The paths come from the host -- or from `ScanFolder`, which reports a
    /// tree without deciding anything about it. Each path is decoded once
    /// and cached; results arrive as the usual `analysis_ready` events, and
    /// `preanalysis_progress` reports how far the batch has got.
    ///
    /// The batch is also what sizes the cache: a 5000-track scan against a
    /// 256-entry cache would evict its own early results.
    Preanalyze {
        paths: Vec<PathBuf>,
    },
    /// Change how many analysis workers run.
    ///
    /// Growing starts new threads immediately. Shrinking lets the workers that
    /// are no longer needed finish the decode they are on and then exit -- a
    /// thread cannot be killed, and abandoning a decode halfway would leave the
    /// batch accounting short of its own total.
    SetAnalysisWorkers {
        count: usize,
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
    /// A tag write landed: the file now carries these families.
    ///
    /// Emitted after the replacement is in place, so a subscriber that reads
    /// the file sees the new tags. The fields themselves are not carried --
    /// asking the file is one call and cannot disagree with it.
    TagsChanged {
        path: PathBuf,
        families: Vec<crate::tag::TagFamily>,
    },
    /// One file's tags, after a `ReadTags`.
    ///
    /// `families` is what the file carries; `writable` is what the container
    /// can be written with. They are two questions: a FLAC carries Vorbis
    /// comments and can be written with them, while a WAV carrying an ID3v2
    /// chunk may still have no ID3v1 to write. A UI draws its write choices
    /// from `writable` and its family columns from `families`.
    TagsRead {
        path: PathBuf,
        families: Vec<crate::tag::TagFamily>,
        writable: Vec<crate::tag::TagFamily>,
        /// Every field, each with the family it came from. Two families can
        /// disagree about one key, which is why the family is carried rather
        /// than merged.
        fields: Vec<crate::tag::TagFieldValue>,
    },
    /// The field names each family accepts, after a `ListTagKeys`.
    TagKeysChanged {
        lists: Vec<TagKeyList>,
    },
    /// A file's embedded pictures, after a `ReadArtwork`.
    ///
    /// `pictures` always lists what is there. `data` is present only when the
    /// command named an index, and it is that picture's bytes, base64: the
    /// bridge is one JSON string and a picture is not text. `mime` comes from
    /// the tag itself and may be absent -- a file that does not say what its
    /// picture is has not told us, and guessing from the first bytes here would
    /// be a second opinion the file never gave.
    ArtworkRead {
        path: PathBuf,
        pictures: Vec<crate::tag::ArtworkInfo>,
        #[serde(skip_serializing_if = "Option::is_none")]
        index: Option<usize>,
        #[serde(skip_serializing_if = "Option::is_none")]
        data: Option<String>,
    },
    /// The remote session, after `SetRemote`.
    ///
    /// The token is not carried. An event is what a host logs and a UI shows,
    /// and a token in either is a token that leaks.
    RemoteChanged {
        configured: bool,
        /// Which protocol, so a host can label the connection.
        #[serde(skip_serializing_if = "Option::is_none")]
        kind: Option<String>,
        #[serde(skip_serializing_if = "Option::is_none")]
        base_url: Option<String>,
        #[serde(skip_serializing_if = "Option::is_none")]
        cache_dir: Option<PathBuf>,
    },
    /// The server's audio items, after `ListRemote`.
    RemoteListed {
        tracks: Vec<crate::remote::RemoteTrack>,
    },
    /// A track's cover, after `FetchRemoteArtwork`.
    ///
    /// `data` is base64 for the same reason the local artwork is: the bridge is
    /// one JSON string. Absent means the server has no picture for this track,
    /// which is a normal answer and not a failure.
    RemoteArtwork {
        id: String,
        #[serde(skip_serializing_if = "Option::is_none")]
        mime: Option<String>,
        #[serde(skip_serializing_if = "Option::is_none")]
        data: Option<String>,
    },
    /// The server's playlists, after `ListRemotePlaylists`.
    RemotePlaylists {
        playlists: Vec<crate::remote::RemotePlaylist>,
    },
    /// One server playlist's entries, after `ListRemotePlaylistItems`.
    RemotePlaylistItems {
        id: String,
        tracks: Vec<crate::remote::RemoteTrack>,
    },
    /// One item is now a local file, after `FetchRemote`.
    ///
    /// The path is what the host hands to `play`: nothing above this module
    /// knows the file arrived over the network.
    RemoteFetched {
        track: crate::remote::RemoteTrack,
        path: PathBuf,
    },
    /// A walk of `root` has started. The answer is `scan_done`.
    ///
    /// A pair of events rather than a percentage: a library big enough that the
    /// silence is a problem is big enough to warrant moving the walk off the
    /// worker, and a progress bar would hide that.
    ScanStarted {
        root: PathBuf,
    },
    /// What the walk found that the index did not know, after `ScanFolder`.
    ///
    /// A diff, not the whole tree. Reporting every file on every launch would
    /// make a host re-import the library each time, which is worse than not
    /// scanning at all.
    ScanDone {
        root: PathBuf,
        added: Vec<PathBuf>,
        /// Present before and present now, with a different size or timestamp.
        modified: Vec<PathBuf>,
        removed: Vec<PathBuf>,
        /// The same file under a new path. The library already moved its star,
        /// play count and playlist entries; the host only has to move whatever
        /// it caches by path.
        moved: Vec<Move>,
    },
    /// A folder is being watched. `roots` is the whole set, because a host that
    /// shows "watching 3 folders" must not have to add up deltas.
    FolderWatched {
        root: PathBuf,
        roots: Vec<PathBuf>,
    },
    FolderUnwatched {
        root: PathBuf,
        roots: Vec<PathBuf>,
    },
    /// The automatic reconciliation interval, after `SetReconcileInterval`.
    ReconcileChanged {
        seconds: u64,
    },
    /// What changed under the watched folders.
    ///
    /// Coalesced per call: one save produces several backend events, and a host
    /// redrawing for each would repaint the same change repeatedly. `rescan` is
    /// set when the watcher cannot name the files that left -- a directory was
    /// removed and its contents were never enumerated -- so the honest answer is
    /// "look again" rather than a list it would have had to invent.
    FilesChanged {
        changes: Vec<crate::scan::Change>,
    },
    /// The pictures a file carries after an artwork edit.
    ///
    /// The whole list rather than a delta: a host that kept the delta would
    /// have to apply it to its own copy, and two copies of one measurement
    /// drift (AGENTS.md rule 1). The listing is small; it carries no bytes.
    ArtworkChanged {
        path: PathBuf,
        pictures: Vec<crate::tag::ArtworkInfo>,
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
    TracksListed {
        tracks: Vec<TrackRow>,
        total: i64,
        offset: usize,
    },
    AlbumsListed {
        albums: Vec<AlbumSummary>,
    },
    ArtistsListed {
        artists: Vec<ArtistSummary>,
    },
    SearchResults {
        query: String,
        tracks: Vec<TrackRow>,
    },
    /// How far an indexing batch has got. `complete` is the last one.
    TagsIndexed {
        done: usize,
        total: usize,
        complete: bool,
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
    /// One visualiser frame, at the rate the host asked for.
    ///
    /// Only emitted while playing and only when a host has subscribed: the taps
    /// are read from the session thread every slice, and forwarding them costs a
    /// copy. A UI that closes the spectrum view stops asking, and the events
    /// stop.
    VisualFrame {
        frame: VisualFrame,
    },
    /// The analysis pool changed size. The count is the target, not the
    /// threads currently running: shrinking takes effect as each worker
    /// finishes the decode it is on.
    AnalysisWorkersChanged {
        count: usize,
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
