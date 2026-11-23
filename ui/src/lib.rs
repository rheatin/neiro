//! The app's Rust code: what `App.tsx` imports from `neiro/native`.
//!
//! One class, [`Base`], is the whole door between the interface and the
//! engine: the TSX holds one handle, reads state through `onXxx` listeners
//! and asks for data and playback through methods. The listeners get the
//! current value when they subscribe and every change after it; a request's
//! answer is an event, because Base's C ABI only queues commands.
//!
//! One owner per piece of state (spotish's rule, and NSH's): the state lives
//! here, in the fields of [`Inner`]; the TSX mirrors it in signals and
//! changes it only through these methods.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use nsh_core::element::CallbackArgs;
use nsh_core::prelude::*;
use serde::Deserialize;

/// [`Engine`]: ../src/engine.rs.html
pub mod engine;
pub mod lyrics_providers;
pub mod audio_session;
#[cfg(target_os = "ios")]
pub mod ios;

/// A folder the user picked: the path to scan now, the bookmark to store,
/// and the folder's own name.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct PickedFolderInfo {
    pub path: String,
    pub bookmark: String,
    pub label: String,
}

/// A stored bookmark resolved for this launch: the path now, the bookmark
/// to keep (a fresh one when the stored one was stale), and whether it was.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct FolderAccessInfo {
    pub path: String,
    pub bookmark: String,
    pub stale: bool,
}

use crate::engine::{Analysis, Engine, RenderPath, Snapshot, WireEvent};

/// The crate's UI set (`nsh.json` `"rust"`): what `App.tsx` imports from
/// `neiro/native`.
#[native_module("neiro/native")]
fn native(registry: &mut Registry) {
    registry.class::<Base>();
}

// ---------------------------------------------------------------- the models

/// What plays, as one object per change. Fields Base has not reported stay
/// empty, zero or `null` — a plausible default would be a measurement the
/// engine never made.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct PlayerState {
    /// `"stopped" | "ready" | "playing" | "paused" | "device_lost" |
    /// "ended" | "error"`, as Base reports it.
    pub state: String,
    /// The file, when one is open.
    pub path: Option<String>,
    /// Tags of the playing track, empty until `analysis_ready`.
    pub title: String,
    pub artist: String,
    pub album: String,
    /// The position when it was sampled, in milliseconds.
    pub position_ms: f64,
    /// The track's length, in milliseconds; `0` when unknown.
    pub duration_ms: f64,
    /// When it was sampled (epoch ms): the UI moves the position on from
    /// here while it plays.
    pub at: f64,
    /// The linear gain the render path reports (0..1); `null` before one
    /// does. The volume slider maps onto it directly.
    pub volume: Option<f64>,
    /// The decode's bitrate, when the analysis reported one.
    pub bitrate_kbps: Option<f64>,
    /// The codec the engine decoded with (`flac`, `alac`, `aac`, `pcm_s24le`),
    /// as the analysis named it. The container in the file's extension is a
    /// guess — `.m4a` carries AAC and ALAC — so this is what the panel shows.
    pub codec: String,
    /// FFmpeg's codec profile; the fact that separates E-AC-3 JOC (Dolby
    /// Atmos) from plain DD+ at the same channel count.
    pub codec_profile: String,
    /// Increments inside one session each time a preloaded track takes
    /// over: how a gapless seam is told from a new stream.
    pub track_serial: f64,
    /// The engine's diagnostics for this stream (`NAE_ResolvedPath`), as a
    /// JSON string the interface parses when a view of it exists. The facts
    /// `padFrameCount` and `maxBufferGapMs` live here: "the output burbles"
    /// is only ever measured, never guessed.
    pub resolved_json: Option<String>,
}

/// One row of the browsing index.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct Track {
    pub path: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub disc_no: f64,
    pub track_no: f64,
    pub year: f64,
    pub genre: String,
    pub duration_ms: f64,
    pub sample_rate: f64,
    pub bits: f64,
    pub channels: f64,
}

/// One album, as the index groups them.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct Album {
    pub album: String,
    pub album_artist: String,
    pub year: f64,
    pub track_count: f64,
}

#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct Artist {
    pub artist: String,
    pub track_count: f64,
    pub album_count: f64,
}

/// What the engine actually did with this stream — never what was requested.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RenderPathInfo {
    pub source_rate: f64,
    pub sample_rate: f64,
    pub channels: f64,
    pub bits_per_sample: f64,
    pub valid_bits: f64,
    pub is_float: bool,
    pub resampling: bool,
    pub dithered: bool,
    pub bit_perfect: bool,
    /// The engine's `NAE_REASON_*`: 0 when bit-perfect, otherwise why not.
    pub reason: f64,
    pub latency_ms: f64,
    pub volume: f64,
    pub eq_enabled: bool,
    pub crossfeed_enabled: bool,
    pub replay_gain_enabled: bool,
    pub convolver_enabled: bool,
    pub crossfade_enabled: bool,
}

/// One output endpoint, as the engine listed it.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct DeviceInfo {
    pub id: String,
    pub name: String,
    pub sample_rate: f64,
    pub bits_per_sample: f64,
    pub valid_bits: f64,
    pub channels: f64,
    pub exclusive: bool,
    pub shared: bool,
    /// The rates the endpoint opens, as Base measured. Empty means "not
    /// probed".
    pub supported_sample_rates: Vec<f64>,
}

#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct QueueInfo {
    pub queue: Vec<String>,
    pub index: Option<f64>,
    pub snapshot_json: Option<String>,
}

/// The applied DSP parameters, read back from the session.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct SettingsInfo {
    pub volume: f64,
    pub eq_enabled: bool,
    pub eq_preamp_db: f64,
    pub eq_bands: Vec<EqBandInfo>,
    pub crossfeed_enabled: bool,
    pub crossfeed_amount: f64,
    pub crossfeed_delay_ms: f64,
    pub crossfeed_cutoff_hz: f64,
    /// ReplayGain mode: `0` off, `1` track, `2` album.
    pub replay_gain_mode: f64,
    pub replay_gain_preamp_db: f64,
    pub replay_gain_prevent_clipping: bool,
    pub replay_gain_fallback_db: f64,
    pub convolver_enabled: bool,
    pub convolver_ir_path: String,
    pub convolver_wet: f64,
    pub convolver_dry: f64,
    pub convolver_gain_db: f64,
    pub convolver_normalize: bool,
    pub crossfade_enabled: bool,
    pub crossfade_duration_ms: f64,
    /// `0` linear, `1` equal power.
    pub crossfade_curve: f64,
}

/// One EQ band, exactly as the wire spells it (`freq_hz`, not `freq`).
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct EqBandInfo {
    pub kind: f64,
    pub freq_hz: f64,
    pub gain_db: f64,
    pub q: f64,
    pub enabled: bool,
    pub channel_mask: f64,
}

/// One embedded picture's description, without bytes.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct ArtworkPicture {
    pub role: String,
    pub mime: Option<String>,
    pub size: f64,
}

/// The pictures a file embeds (the cheap question: no bytes).
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct ArtworkList {
    pub path: String,
    pub pictures: Vec<ArtworkPicture>,
}

/// One picture's bytes, base64 (the expensive question, asked by index).
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct ArtworkBytes {
    pub path: String,
    pub index: f64,
    pub mime: Option<String>,
    pub data: Option<String>,
}

/// A folder scan or watch, as one object: `kind` says which.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct ScanEvent {
    /// `"started" | "done" | "watched" | "changed"`.
    pub kind: String,
    /// The scan's or the watch's root; `""` for `"changed"`.
    pub root: String,
    /// Only `"done"` carries the diff: a difference against the index,
    /// never a tree.
    pub added: Vec<String>,
    pub modified: Vec<String>,
    pub removed: Vec<String>,
    /// `"watched"` and `"changed"` carry the whole set of roots.
    pub roots: Vec<String>,
    /// `"changed"` with `rescan` set: the watcher could not name what
    /// changed, so the honest answer is to scan again.
    pub rescan: bool,
}

/// One visualiser frame: the spectrum the engine already measured, for the
/// Now Playing's light. Only arrives while playing and while subscribed.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct VisualFrameJs {
    /// Linear magnitudes, `fft_size / 2` of them (a full-scale sine reads
    /// about 1.0 in its own bin).
    pub bins: Vec<f64>,
    pub fft_size: f64,
    pub sample_rate: f64,
    pub peak: f64,
    /// Per-channel peaks, `null` when the engine does not report them.
    /// `peak` stays the maximum of the two.
    pub peak_l: Option<f64>,
    pub peak_r: Option<f64>,
    pub rms: f64,
    /// Total frames the tap has been fed — compare with the previous frame
    /// to know whether this is new audio.
    pub frames_fed: f64,
    /// The time-domain envelope, oldest first.
    pub envelope: Vec<f64>,
    pub envelope_hop_ms: f64,
}

/// A failure Base reported (`op` names it was doing).
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct EngineError {
    pub op: String,
    pub message: String,
}

/// One item in a server's audio listing, until `fetch_remote` turns it
/// into a local file.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RemoteTrack {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    /// The container the server stores: `flac`, `mp3`, `dsf`.
    pub container: String,
    pub duration_ms: f64,
    pub size: Option<f64>,
    /// The server's id for this track's cover, when it is not the track id.
    pub cover_art: Option<String>,
}

/// What a 测试连接 asked the server and what it answered.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RemoteProbe {
    pub ok: bool,
    pub message: String,
    pub count: f64,
}

/// One set of words an online provider offers for a track.
///
/// `synced` is the provider's timestamped text (LRC), `plain` its untimed one.
/// A provider may have either, both, or neither — an instrumental row has a
/// name and no words, which is a result the interface shows rather than hides.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct LyricsCandidate {
    pub id: f64,
    pub track: String,
    pub artist: String,
    pub album: String,
    pub duration_s: f64,
    pub synced: Option<String>,
    pub plain: Option<String>,
    pub provider: String,
    pub provider_name: String,
}

/// A playlist on the server.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RemotePlaylist {
    pub id: String,
    pub name: String,
    pub track_count: f64,
    pub duration_ms: f64,
}

/// The remote session. Never carries the token or the password.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RemoteStatus {
    /// Whether a server is configured; `false` is "disconnected", not a
    /// failure (that is what `on_error` is for).
    pub configured: bool,
    /// `"jellyfin"` or `"navidrome"`, so the UI can label the connection.
    pub kind: Option<String>,
    pub base_url: Option<String>,
}

/// A fetched item: it is a local file now, at `path`.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RemoteFetched {
    pub track: RemoteTrack,
    pub path: String,
}

/// One playlist's entries, in the order the server reports.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RemoteItemsPage {
    pub id: String,
    pub tracks: Vec<RemoteTrack>,
}

/// A cover from the server, base64. `data: null` says the server has no
/// picture for this track — a normal answer, not a failure.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct RemoteArtwork {
    pub id: String,
    pub mime: Option<String>,
    pub data: Option<String>,
}

/// The listener's favourite paths (whole set; the UI owns the diff).
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct Favourites {
    pub paths: Vec<String>,
}

/// One playlist in the sidebar order.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct Playlist {
    pub id: f64,
    pub name: String,
    /// How many entries the playlist holds, not how many loaded.
    pub track_count: f64,
}

/// One playlist's entries, in order, each with its dense 0-based position.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct PlaylistEntriesPage {
    pub id: f64,
    pub entries: Vec<PlaylistEntry>,
}

#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct PlaylistEntry {
    pub path: String,
    pub position: f64,
}

/// One track and how often it was played, most first.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct MostPlayed {
    pub path: String,
    pub play_count: f64,
}

// ------------------------------------------------------------------ plumbing

/// A table of one event's listeners, each with its unsubscribe id.
struct Listeners<T> {
    next: u64,
    items: Vec<(u64, Callback<T>)>,
}

impl<T> Default for Listeners<T> {
    fn default() -> Self {
        Listeners { next: 0, items: Vec::new() }
    }
}

impl<T: CallbackArgs + Clone + 'static> Listeners<T> {
    fn add(&mut self, listener: Callback<T>) -> u64 {
        let id = self.next;
        self.next += 1;
        self.items.push((id, listener));
        id
    }

    fn remove(&mut self, id: u64) {
        self.items.retain(|(kept, _)| *kept != id);
    }

    fn post(&self, value: &T) {
        for (_, listener) in &self.items {
            listener.post(value.clone());
        }
    }
}



/// The state the listeners last heard. Every field has one owner, here.
#[derive(Default)]
struct Inner {
    player: Option<PlayerState>,
    /// Tags that arrived for a track before it became the current one: the
    /// engine analyses the next track while the previous still plays, so
    /// `analysis_ready` can precede the `track_changed` that names it.
    pending_analysis: Option<(String, Analysis)>,
    render_path: Option<RenderPathInfo>,
    devices: Option<Vec<DeviceInfo>>,
    queue: Option<QueueInfo>,
    settings: Option<SettingsInfo>,
    tracks: Option<TracksPage>,
    albums: Option<Vec<Album>>,
    artists: Option<Vec<Artist>>,
    search: Option<SearchPage>,
    artwork_list: Option<ArtworkList>,
    remote: Option<RemoteStatus>,
    remote_tracks: Option<Vec<RemoteTrack>>,
    remote_playlists: Option<Vec<RemotePlaylist>>,
    remote_items: Option<RemoteItemsPage>,
    favourites: Option<Favourites>,
    playlists: Option<Vec<Playlist>>,
    playlist_entries: Option<PlaylistEntriesPage>,
    most_played: Option<Vec<MostPlayed>>,
    on_visual_frame: Listeners<VisualFrameJs>,
    resolved: Option<String>,
    on_player: Listeners<PlayerState>,
    on_render_path: Listeners<RenderPathInfo>,
    on_devices: Listeners<Vec<DeviceInfo>>,
    on_queue: Listeners<QueueInfo>,
    on_settings: Listeners<SettingsInfo>,
    on_tracks: Listeners<TracksPage>,
    on_albums: Listeners<Vec<Album>>,
    on_artists: Listeners<Vec<Artist>>,
    on_search: Listeners<SearchPage>,
    on_lyrics_found: Listeners<Vec<LyricsCandidate>>,
    on_remote_probed: Listeners<RemoteProbe>,
    on_artwork_list: Listeners<ArtworkList>,
    on_artwork_data: Listeners<ArtworkBytes>,
    on_scan: Listeners<ScanEvent>,
    on_error: Listeners<EngineError>,
    on_remote: Listeners<RemoteStatus>,
    on_remote_tracks: Listeners<Vec<RemoteTrack>>,
    on_remote_playlists: Listeners<Vec<RemotePlaylist>>,
    on_remote_fetched: Listeners<RemoteFetched>,
    on_remote_artwork: Listeners<RemoteArtwork>,
    on_remote_items: Listeners<RemoteItemsPage>,
    on_favourites: Listeners<Favourites>,
    on_playlists: Listeners<Vec<Playlist>>,
    on_playlist_entries: Listeners<PlaylistEntriesPage>,
    on_most_played: Listeners<Vec<MostPlayed>>,
    on_resolved: Listeners<String>,
    tags: Option<TagFields>,
    on_tags: Listeners<TagFields>,
}

/// A file's tag fields, after `read_tags`. `path` is the one that was asked
/// for, so a late reply for a track that has since changed is recognisable.
#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct TagFields {
    pub path: String,
    pub fields: Vec<TagFieldJs>,
}

#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct TagFieldJs {
    pub family: String,
    pub key: String,
    pub name: String,
    /// Empty for a non-text item.
    pub value: String,
}

#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct TracksPage {
    pub tracks: Vec<Track>,
    pub total: f64,
    pub offset: f64,
}

#[derive(Debug, Clone, Default, PartialEq, JsObject)]
pub struct SearchPage {
    pub query: String,
    pub tracks: Vec<Track>,
}

/// The engine plus the tables, shared between the class and the pump task.
/// `None` once `dispose` ran; the pump ends when it sees that.
type Shared = Arc<Mutex<Option<Engine>>>;

/// Where this app keeps its own data, per platform.
///
/// It used to be `LOCALAPPDATA` with `"."` as the fallback, which is a Windows
/// variable: on macOS the root became the *current directory*, and a packaged
/// app launched from Finder runs with `/` as its cwd — so Base tried to open
/// `/Neiro/library.db`, could not, and the window said "引擎未连接" beside a
/// library that was perfectly built and loadable.
fn data_root() -> std::path::PathBuf {
    #[cfg(windows)]
    {
        std::env::var_os("LOCALAPPDATA")
            .map(std::path::PathBuf::from)
            .unwrap_or_else(|| std::path::PathBuf::from("."))
    }
    #[cfg(target_os = "macos")]
    {
        std::env::var_os("HOME")
            .map(std::path::PathBuf::from)
            .unwrap_or_else(|| std::path::PathBuf::from("."))
            .join("Library")
            .join("Application Support")
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        std::env::var_os("XDG_DATA_HOME")
            .map(std::path::PathBuf::from)
            .or_else(|| {
                std::env::var_os("HOME")
                    .map(std::path::PathBuf::from)
                    .map(|home| home.join(".local").join("share"))
            })
            .unwrap_or_else(|| std::path::PathBuf::from("."))
    }
}

/// The path of Base's SQLite library: favourites and playlists persist
/// across runs because this path never moves.
fn library_path() -> std::path::PathBuf {
    data_root().join("Neiro").join("library.db")
}

/// Where remote downloads and covers land — stable across runs, because a
/// cache directory that moved would orphan every favourite and playlist
/// entry that names one of its files.
fn remote_cache_dir() -> String {
    library_path()
        .parent()
        .expect("the library always has a directory")
        .join("remote")
        .to_string_lossy()
        .into_owned()
}

/// A cache root for tests and the private helpers that must not hard-code
/// the app data path. Production passes the real directories.
fn remote_cache_dir_in(root: &std::path::Path) -> std::path::PathBuf {
    root.join("Neiro").join("remote")
}

// ---- Remote identity -------------------------------------------------------
//
// Jellyfin groups sessions by the device that opened them. A name shared by
// every install ("neiro-base") makes two phones, or two windows of the same
// app, look like one device to the server. This installation therefore keeps
// its own opaque id, persisted beside the library so it survives a restart.
//
// The linked core (v0.3.1) still sends its fixed `DeviceId="neiro-base"`
// claim and does not read `device_id`; the host sends the id anyway, so the
// day the core accepts it nothing else changes. Until then the connection
// probe is the one request that already reports this install.

/// The per-install remote identity, created once and then read.
static INSTALLATION_ID: std::sync::OnceLock<String> = std::sync::OnceLock::new();

/// The file holding [`installation_id`], under an explicit root (tests).
fn installation_id_path_in(root: &std::path::Path) -> std::path::PathBuf {
    root.join("Neiro").join("device-id")
}

/// This install's id: 32 lowercase hex characters. A missing or empty file
/// is replaced; a write failure is said out loud once and the id stays
/// in-memory for this run.
fn installation_id_in(root: &std::path::Path) -> String {
    let path = installation_id_path_in(root);
    if let Ok(text) = std::fs::read_to_string(&path) {
        let text = text.trim();
        if !text.is_empty() {
            return text.to_owned();
        }
    }
    let id = new_installation_id(root);
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    if let Err(error) = std::fs::write(&path, &id) {
        eprintln!("neiro: 设备标识没有写入 {}：{error}", path.display());
    }
    id
}

/// Two FNV passes over the clock, the process and the data root. It is not
/// a cryptographic random value; it only has to differ between installs and
/// stay stable once written.
fn new_installation_id(root: &std::path::Path) -> String {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|elapsed| elapsed.as_nanos())
        .unwrap_or(0);
    let seed = format!("{nanos}:{}:{}", std::process::id(), root.display());
    let first = fnv1a64(seed.as_bytes());
    let second = fnv1a64(format!("{seed}:second").as_bytes());
    format!("{first:016x}{second:016x}")
}

fn installation_id() -> String {
    INSTALLATION_ID
        .get_or_init(|| installation_id_in(&data_root()))
        .clone()
}

// ---- Remote cache ----------------------------------------------------------

/// The ceiling for downloaded remote audio. Downloads are whole files, so a
/// handful of lossless albums is hundreds of megabytes; 2 GiB keeps a phone
/// bounded without making the cache useless. The oldest go first.
const REMOTE_AUDIO_CACHE_BUDGET: u64 = 2 * 1024 * 1024 * 1024;

/// Paths this run downloaded. They are the session's playback material: the
/// queue and the player already hold these paths, so eviction must not take
/// them out from under a row that is about to play. Only older files pay for
/// the ceiling.
static SESSION_FETCHED: Mutex<Vec<String>> = Mutex::new(Vec::new());

fn remember_session_fetch(path: &str) {
    if path.is_empty() {
        return;
    }
    let mut kept = SESSION_FETCHED
        .lock()
        .unwrap_or_else(|poison| poison.into_inner());
    if !kept.iter().any(|known| known == path) {
        kept.push(path.to_owned());
    }
}

/// Keeps downloaded audio under `budget`, oldest first, dropping 10% past
/// the ceiling so the next download does not walk the directory again.
/// Subdirectories (artwork, catalog) and half-written `.part` files are not
/// counted here. Returns how many files were removed.
fn prune_remote_audio_cache_in(root: &std::path::Path, budget: u64) -> usize {
    let dir = remote_cache_dir_in(root);
    let Ok(entries) = std::fs::read_dir(&dir) else {
        return 0;
    };
    let session: Vec<String> = SESSION_FETCHED
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .clone();
    let mut files: Vec<(std::path::PathBuf, u64, std::time::SystemTime)> = Vec::new();
    let mut total = 0u64;
    for entry in entries.flatten() {
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        if !metadata.is_file() {
            continue;
        }
        let path = entry.path();
        if path.extension().and_then(|ext| ext.to_str()) == Some("part") {
            continue;
        }
        if session.iter().any(|kept| std::path::Path::new(kept) == path) {
            continue;
        }
        total = total.saturating_add(metadata.len());
        files.push((
            path,
            metadata.len(),
            metadata.modified().unwrap_or(std::time::UNIX_EPOCH),
        ));
    }
    if total <= budget {
        return 0;
    }
    files.sort_by(|left, right| left.2.cmp(&right.2));
    let mut to_free = total - budget + budget / 10;
    let mut removed = 0;
    for (path, len, _) in files {
        if to_free == 0 {
            break;
        }
        if std::fs::remove_file(&path).is_ok() {
            to_free = to_free.saturating_sub(len);
            removed += 1;
        }
    }
    removed
}

/// Removes `<id>.<ext>.part` files — a download that never finished — from
/// the remote audio directory. Returns how many were removed.
#[cfg_attr(not(any(target_os = "ios", test)), allow(dead_code))]
fn remove_part_files_in(root: &std::path::Path) -> usize {
    let dir = remote_cache_dir_in(root);
    let mut removed = 0;
    if let Ok(entries) = std::fs::read_dir(&dir) {
        for entry in entries.flatten() {
            if entry.path().extension().and_then(|ext| ext.to_str()) != Some("part") {
                continue;
            }
            if entry.metadata().map(|meta| meta.is_file()).unwrap_or(false)
                && std::fs::remove_file(entry.path()).is_ok()
            {
                removed += 1;
            }
        }
    }
    removed
}

/// What a launch owes the cache: a process killed mid-download leaves
/// `<id>.<ext>.part`, which no later fetch can use, and the audio bucket may
/// have grown past its ceiling. Returns how many files were removed.
fn reconcile_remote_cache_in(root: &std::path::Path) -> usize {
    // The desktop's WebDAV fetch resumes the part an earlier run left behind,
    // so it is not junk there. iOS ships the whole-file fetch only: a part is
    // always the remains of an interrupted download.
    #[cfg(target_os = "ios")]
    let removed = remove_part_files_in(root);
    #[cfg(not(target_os = "ios"))]
    let removed = 0;
    removed + prune_remote_audio_cache_in(root, REMOTE_AUDIO_CACHE_BUDGET)
}

// ---- Local cover cache -----------------------------------------------------
//
// Embedded pictures are read out of the file (and decoded by Base) every time
// the interface shows them; over SMB that is a network read per cover per
// launch, and the artist and album cards show the same file's picture again.
// This keeps the `data:` URI the interface already paints on disk, keyed by
// the source file's identity, so a restart (or a scroll back) is a local read.
// The remote pipeline needs nothing here: Base already caches server covers
// under `remote/artwork/`.

/// One text file per cover, holding the `data:` URI (or `none`).
fn local_cover_cache_dir() -> std::path::PathBuf {
    library_path()
        .parent()
        .expect("the library always has a directory")
        .join("artwork-local")
}

/// The local cover cache under an explicit root (tests).
fn local_cover_cache_dir_in(root: &std::path::Path) -> std::path::PathBuf {
    root.join("Neiro").join("artwork-local")
}

/// One cache bucket's size and file count. Directories and links are not
/// files: they are never counted or followed.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
struct CacheBucket {
    bytes: u64,
    files: u64,
}

impl CacheBucket {
    fn walk(dir: &std::path::Path, recursive: bool) -> Self {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return Self::default();
        };
        let mut total = Self::default();
        for entry in entries.flatten() {
            let Ok(file_type) = entry.file_type() else { continue };
            if file_type.is_dir() {
                if recursive {
                    total = total + Self::walk(&entry.path(), true);
                }
                continue;
            }
            if !file_type.is_file() {
                continue;
            }
            let Ok(metadata) = entry.metadata() else { continue };
            total.bytes += metadata.len();
            total.files += 1;
        }
        total
    }

    /// Deletes the files of this bucket only, returning the failures left
    /// behind (a playing file may be open; the next clean can take it).
    fn clear(dir: &std::path::Path, recursive: bool) -> Self {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return Self::default();
        };
        for entry in entries.flatten() {
            let Ok(file_type) = entry.file_type() else { continue };
            if file_type.is_file() {
                let _ = std::fs::remove_file(entry.path());
            }
        }
        if recursive {
            let _ = Self::walk_subdirs(dir, recursive);
        }
        Self::walk(dir, recursive)
    }

    fn walk_subdirs(dir: &std::path::Path, recursive: bool) -> std::io::Result<()> {
        for entry in std::fs::read_dir(dir)?.flatten() {
            if entry.file_type().is_ok_and(|ft| ft.is_dir()) {
                Self::clear(&entry.path(), recursive);
            }
        }
        Ok(())
    }
}

impl std::ops::Add for CacheBucket {
    type Output = Self;
    fn add(self, rhs: Self) -> Self {
        Self {
            bytes: self.bytes + rhs.bytes,
            files: self.files + rhs.files,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
struct CacheStats {
    remote_audio_bytes: u64,
    remote_audio_files: u64,
    remote_artwork_bytes: u64,
    remote_artwork_files: u64,
    remote_catalog_bytes: u64,
    remote_catalog_files: u64,
    local_artwork_bytes: u64,
    local_artwork_files: u64,
}

/// The four buckets, under one app-data root. `audio` is Base's flat remote
/// directory; the other two are named subdirectories.
fn cache_stats_in(root: &std::path::Path) -> CacheStats {
    let remote = remote_cache_dir_in(root);
    let audio = CacheBucket::walk(&remote, false);
    let artwork = CacheBucket::walk(&remote.join("artwork"), true);
    let catalog = CacheBucket::walk(&remote.join("catalog"), true);
    let local = CacheBucket::walk(&local_cover_cache_dir_in(root), false);
    CacheStats {
        remote_audio_bytes: audio.bytes,
        remote_audio_files: audio.files,
        remote_artwork_bytes: artwork.bytes,
        remote_artwork_files: artwork.files,
        remote_catalog_bytes: catalog.bytes,
        remote_catalog_files: catalog.files,
        local_artwork_bytes: local.bytes,
        local_artwork_files: local.files,
    }
}

fn parse_cache_kind(kind: &str) -> Result<&'static str, String> {
    match kind {
        "remote-audio" => Ok("remote-audio"),
        "remote-artwork" => Ok("remote-artwork"),
        "remote-catalog" => Ok("remote-catalog"),
        "local-artwork" => Ok("local-artwork"),
        "all" => Ok("all"),
        _ => Err(format!("E_CACHE: 未知缓存类型 {kind}")),
    }
}

/// Clears one known bucket under `root`, keeping its directory alive. "All"
/// means all four buckets. The returned stats are the caller's truth.
fn clear_cache_in(root: &std::path::Path, kind: &str) -> Result<String, String> {
    parse_cache_kind(kind)?;
    let remote = remote_cache_dir_in(root);
    let artwork_dir = remote.join("artwork");
    let catalog_dir = remote.join("catalog");
    let local_dir = local_cover_cache_dir_in(root);
    match kind {
        "remote-audio" => { CacheBucket::clear(&remote, false); }
        "remote-artwork" => { CacheBucket::clear(&artwork_dir, true); }
        "remote-catalog" => { CacheBucket::clear(&catalog_dir, true); }
        "local-artwork" => { CacheBucket::clear(&local_dir, false); }
        _ => {
            let _ = CacheBucket::clear(&remote, false);
            let _ = CacheBucket::clear(&artwork_dir, true);
            let _ = CacheBucket::clear(&catalog_dir, true);
            let _ = CacheBucket::clear(&local_dir, false);
        }
    }
    for dir in [remote.join("artwork"), remote.join("catalog"), local_dir] {
        let shown = dir.display().to_string();
        if let Err(e) = std::fs::create_dir_all(&dir) {
            if !dir.is_dir() {
                return Err(format!("E_CACHE: 无法保留缓存目录 {shown}：{e}"));
            }
        }
    }
    let stats = cache_stats_in(root);
    serde_json::to_string(&stats).map_err(|e| format!("E_CACHE: 统计结果序列化失败：{e}"))
}

/// Writes an 8-second 44100 Hz stereo 16-bit PCM WAV containing a 440 Hz sine
/// at -12 dBFS. Platform-neutral so its byte layout can be tested anywhere,
/// but only iOS plays it, so elsewhere it exists for the test alone.
#[cfg(any(target_os = "ios", test))]
fn write_test_tone(path: &std::path::Path) -> Result<(), String> {
    const SAMPLE_RATE: u32 = 44100;
    const CHANNELS: u16 = 2;
    const SECONDS: u32 = 8;
    const SAMPLES: u32 = SAMPLE_RATE * SECONDS;
    const DATA_BYTES: u32 = SAMPLES * CHANNELS as u32 * 2;
    const AMPLITUDE: f64 = (0.25 * (i16::MAX as f64)) as f64;

    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| format!("E_TONE: 无法创建目录 {}：{e}", dir.display()))?;
    }
    let mut bytes = Vec::with_capacity(44 + DATA_BYTES as usize);
    bytes.extend_from_slice(b"RIFF");
    bytes.extend_from_slice(&(36 + DATA_BYTES).to_le_bytes());
    bytes.extend_from_slice(b"WAVE");
    bytes.extend_from_slice(b"fmt ");
    bytes.extend_from_slice(&16u32.to_le_bytes());
    bytes.extend_from_slice(&1u16.to_le_bytes());
    bytes.extend_from_slice(&CHANNELS.to_le_bytes());
    bytes.extend_from_slice(&SAMPLE_RATE.to_le_bytes());
    bytes.extend_from_slice(&(SAMPLE_RATE * CHANNELS as u32 * 2).to_le_bytes());
    bytes.extend_from_slice(&(CHANNELS * 2).to_le_bytes());
    bytes.extend_from_slice(&16u16.to_le_bytes());
    bytes.extend_from_slice(b"data");
    bytes.extend_from_slice(&DATA_BYTES.to_le_bytes());
    for sample_index in 0..SAMPLES {
        let sample =
            (std::f64::consts::TAU * 440.0 * sample_index as f64 / SAMPLE_RATE as f64).sin();
        let value = (sample * AMPLITUDE) as i16;
        bytes.extend_from_slice(&value.to_le_bytes());
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    std::fs::write(path, bytes).map_err(|e| format!("E_TONE: 无法写入 {}：{e}", path.display()))
}

/// The cache's ceiling. A 300 KB cover is ~1750 files at this budget; the
/// oldest go first when it is crossed.
const COVER_CACHE_BUDGET: u64 = 512 * 1024 * 1024;

/// Puts since the last trim: the first put of a session trims, then every
/// 128th, so the directory walk is not paid on every cover.
static COVER_PUTS: AtomicUsize = AtomicUsize::new(0);

fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for &byte in bytes {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

/// The cache key for `path`: a hash of the path, its size and its mtime, so
/// a replaced picture (new bytes, new mtime) is read again rather than served
/// stale. `None` when the file cannot be stat'ed: a key without a real
/// identity would go stale, so that answer is not cached.
fn cover_cache_key(path: &str) -> Option<String> {
    let metadata = std::fs::metadata(path).ok()?;
    let modified = metadata
        .modified()
        .ok()
        .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|since| since.as_nanos())
        .unwrap_or(0);
    let mut seed = String::with_capacity(path.len() + 40);
    seed.push_str(path);
    seed.push('\0');
    seed.push_str(&modified.to_string());
    seed.push('\0');
    seed.push_str(&metadata.len().to_string());
    Some(format!("{:016x}", fnv1a64(seed.as_bytes())))
}

/// Trims the cache to its budget, oldest first, dropping 10% past the ceiling
/// so the next put does not walk the directory again.
fn prune_local_cover_cache(dir: &std::path::Path) {
    prune_local_cover_cache_to(dir, COVER_CACHE_BUDGET);
}

fn prune_local_cover_cache_to(dir: &std::path::Path, budget: u64) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    let mut files: Vec<(std::path::PathBuf, u64, std::time::SystemTime)> = Vec::new();
    let mut total = 0u64;
    for entry in entries.flatten() {
        let Ok(metadata) = entry.metadata() else {
            continue;
        };
        if !metadata.is_file() {
            continue;
        }
        let modified = metadata.modified().unwrap_or(std::time::UNIX_EPOCH);
        total = total.saturating_add(metadata.len());
        files.push((entry.path(), metadata.len(), modified));
    }
    if total <= budget {
        return;
    }
    files.sort_by(|left, right| left.2.cmp(&right.2));
    let mut to_free = total - budget + budget / 10;
    for (path, len, _) in files {
        if to_free == 0 {
            break;
        }
        if std::fs::remove_file(&path).is_ok() {
            to_free = to_free.saturating_sub(len);
        }
    }
}

/// Reads `path`'s cached answer from `dir`: the `data:` URI, `"none"`, or
/// `""` when the file has no entry.
fn cover_cache_read(dir: &std::path::Path, path: &str) -> String {
    let Some(key) = cover_cache_key(path) else {
        return String::new();
    };
    std::fs::read_to_string(dir.join(format!("{key}.txt"))).unwrap_or_default()
}

/// Writes `path`'s answer into `dir`. Files that cannot be stat'ed are not
/// cached: a key without a real identity would serve stale bytes.
fn cover_cache_write(dir: &std::path::Path, path: &str, uri: &str) -> Result<(), String> {
    let Some(key) = cover_cache_key(path) else {
        return Ok(());
    };
    std::fs::create_dir_all(dir).map_err(|e| format!("E_CACHE: {e}"))?;
    // Written aside and renamed, so a crash mid-write never leaves a
    // truncated URI for a later run to paint.
    let file = dir.join(format!("{key}.txt"));
    let temp = dir.join(format!(".{key}.tmp"));
    std::fs::write(&temp, uri.as_bytes()).map_err(|e| format!("E_CACHE: {e}"))?;
    std::fs::rename(&temp, &file).map_err(|e| format!("E_CACHE: {e}"))?;
    if COVER_PUTS.fetch_add(1, Ordering::Relaxed) % 128 == 0 {
        prune_local_cover_cache(dir);
    }
    Ok(())
}

/// Empties the tags that describe the previous track: a new path must not
/// inherit them while its own analysis is on the way.
fn clear_track_tags(player: &mut PlayerState) {
    player.title.clear();
    player.artist.clear();
    player.album.clear();
    player.bitrate_kbps = None;
    player.codec.clear();
    player.codec_profile.clear();
}

/// Writes one `analysis_ready` into the player.
fn apply_analysis(player: &mut PlayerState, analysis: &Analysis) {
    player.title = analysis.title.clone();
    player.artist = analysis.artist.clone();
    player.album = analysis.album.clone();
    player.bitrate_kbps = Some(analysis.bitrate_kbps as f64);
    player.codec = analysis.codec.clone();
    player.codec_profile = analysis.codec_profile.clone();
}

impl Inner {
    fn deliver(&mut self, line: &str) {
        let event = match engine::parse(line) {
            Ok(event) => event,
            // An event this UI does not compile for yet: dropped, never
            // guessed at.
            Err(_) => return,
        };
        match event {
            WireEvent::State(snapshot) => self.absorb(snapshot),
            WireEvent::TrackChanged { path, render_path } => {
                let changed = self
                    .player
                    .as_ref()
                    .and_then(|player| player.path.as_deref())
                    != Some(path.as_str());
                let pending = if changed { self.pending_analysis.take() } else { None };
                let player = self.player.get_or_insert_with(PlayerState::default);
                if changed {
                    // A new stream: the previous track's tags are wrong now.
                    clear_track_tags(player);
                    if let Some((pending_path, analysis)) = pending {
                        if pending_path == path {
                            // Its analysis arrived while the previous track
                            // still played; use it instead of waiting for a
                            // second `analysis_ready` that may never come.
                            apply_analysis(player, &analysis);
                        }
                    }
                    player.track_serial += 1.0;
                }
                player.path = Some(path);
                self.set_render_path(render_path);
                // The interface hears the new path (and its cleared tags)
                // now: waiting for the next position left the previous
                // title on screen beside the new cover.
                if let Some(player) = self.player.as_ref() {
                    self.on_player.post(player);
                }
            }
            WireEvent::Position { path, position, duration } => {
                let changed = self
                    .player
                    .as_ref()
                    .and_then(|player| player.path.as_deref())
                    != Some(path.as_str());
                let pending = if changed { self.pending_analysis.take() } else { None };
                let player = self.player.get_or_insert_with(PlayerState::default);
                if changed {
                    // The first word of a new track can be its position, not
                    // its `track_changed`: the old tags must not survive it.
                    clear_track_tags(player);
                    if let Some((pending_path, analysis)) = pending {
                        if pending_path == path {
                            apply_analysis(player, &analysis);
                        }
                    }
                    player.track_serial += 1.0;
                }
                player.path = Some(path);
                player.position_ms = position * 1000.0;
                player.duration_ms = duration * 1000.0;
                player.at = now_ms();
                self.on_player.post(player);
            }
            WireEvent::RenderPathChanged { render_path, .. } => {
                self.set_render_path(render_path);
            }
            WireEvent::DevicesChanged { devices } => {
                let listed = devices.into_iter().map(device_info).collect::<Vec<_>>();
                self.on_devices.post(&listed.clone());
                self.devices = Some(listed);
            }
            WireEvent::SettingsChanged { settings } => {
                let info = settings_info(&settings);
                self.on_settings.post(&info.clone());
                self.settings = Some(info);
            }
            WireEvent::QueueDirectorChanged { snapshot } => {
                let snapshot_json = serde_json::to_string(&snapshot).ok();
                let info = QueueInfo {
                    queue: self.queue.as_ref().map(|q| q.queue.clone()).unwrap_or_default(),
                    index: self.queue.as_ref().and_then(|q| q.index),
                    snapshot_json,
                };
                self.on_queue.post(&info.clone());
                self.queue = Some(info);
            }
            WireEvent::QueueChanged { queue, index } => {
                let snapshot_json = self.queue.as_ref().and_then(|q| q.snapshot_json.clone());
                let info = QueueInfo {
                    queue,
                    index: index.map(|i| i as f64),
                    snapshot_json,
                };
                self.on_queue.post(&info.clone());
                self.queue = Some(info);
            }
            WireEvent::ResolvedPathChanged { resolved } => {
                let text = serde_json::to_string(&resolved).expect("serializable JSON");
                if let Some(player) = self.player.as_mut() {
                    player.resolved_json = Some(text.clone());
                }
                self.on_resolved.post(&text);
            }
            WireEvent::AnalysisReady { path, analysis, .. } => {
                let current = self
                    .player
                    .as_ref()
                    .and_then(|player| player.path.as_deref())
                    == Some(path.as_str());
                if current {
                    let player = self.player.get_or_insert_with(PlayerState::default);
                    apply_analysis(player, &analysis);
                    self.on_player.post(player);
                } else {
                    // A preloaded track is analysed before it takes over:
                    // keep the tags for the `track_changed` that names it,
                    // instead of dropping the only word of its title.
                    self.pending_analysis = Some((path, analysis));
                }
            }
            WireEvent::TagsRead { path, fields } => {
                let page = TagFields {
                    path,
                    fields: fields
                        .into_iter()
                        .map(|field| TagFieldJs {
                            family: field.family,
                            key: field.key,
                            name: field.name,
                            value: field.value.unwrap_or_default(),
                        })
                        .collect(),
                };
                self.tags = Some(page.clone());
                self.on_tags.post(&page);
            }
            WireEvent::TracksListed { tracks, total, offset } => {
                let page = TracksPage {
                    tracks: tracks.into_iter().map(track_of).collect(),
                    total: total as f64,
                    offset: offset as f64,
                };
                self.on_tracks.post(&page.clone());
                self.tracks = Some(page);
            }
            WireEvent::AlbumsListed { albums } => {
                let listed = albums
                    .into_iter()
                    .map(|album| Album {
                        album: album.album,
                        album_artist: album.album_artist,
                        year: album.year as f64,
                        track_count: album.track_count as f64,
                    })
                    .collect::<Vec<_>>();
                self.on_albums.post(&listed.clone());
                self.albums = Some(listed);
            }
            WireEvent::ArtistsListed { artists } => {
                let listed = artists
                    .into_iter()
                    .map(|artist| Artist {
                        artist: artist.artist,
                        track_count: artist.track_count as f64,
                        album_count: artist.album_count as f64,
                    })
                    .collect::<Vec<_>>();
                self.on_artists.post(&listed.clone());
                self.artists = Some(listed);
            }
            WireEvent::SearchResults { query, tracks } => {
                let page = SearchPage {
                    query,
                    tracks: tracks.into_iter().map(track_of).collect(),
                };
                self.on_search.post(&page.clone());
                self.search = Some(page);
            }
            WireEvent::ArtworkRead { path, pictures, index, data } => match index {
                Some(index) => {
                    let mime = pictures.get(index).and_then(|picture| picture.mime.clone());
                    let bytes = ArtworkBytes {
                        path,
                        index: index as f64,
                        mime,
                        data,
                    };
                    self.on_artwork_data.post(&bytes);
                }
                None => {
                    let list = ArtworkList {
                        path,
                        pictures: pictures
                            .into_iter()
                            .map(|picture| ArtworkPicture {
                                role: picture.role,
                                mime: picture.mime,
                                size: picture.size as f64,
                            })
                            .collect(),
                    };
                    self.on_artwork_list.post(&list.clone());
                    self.artwork_list = Some(list);
                }
            },
            WireEvent::ScanStarted { root } => {
                self.on_scan.post(&ScanEvent {
                    kind: "started".into(),
                    root,
                    ..ScanEvent::default()
                });
            }
            WireEvent::ScanDone { root, added, modified, removed, .. } => {
                self.on_scan.post(&ScanEvent {
                    kind: "done".into(),
                    root,
                    added,
                    modified,
                    removed,
                    ..ScanEvent::default()
                });
            }
            WireEvent::FolderWatched { roots, .. } => {
                self.on_scan.post(&ScanEvent {
                    kind: "watched".into(),
                    roots,
                    ..ScanEvent::default()
                });
            }
            WireEvent::FilesChanged { changes } => {
                // Coalesced per worker tick; `rescan` means the watcher could
                // not name what changed, so a root scan is the only answer.
                let mut roots = Vec::new();
                let mut rescan = false;
                for change in changes {
                    if let Ok(change) = serde_json::from_value::<Change>(change) {
                        if !roots.contains(&change.root) {
                            roots.push(change.root);
                        }
                        rescan = rescan || change.rescan;
                    }
                }
                self.on_scan.post(&ScanEvent {
                    kind: "changed".into(),
                    roots,
                    rescan,
                    ..ScanEvent::default()
                });
            }
            WireEvent::Error { op, message } => {
                self.on_error.post(&EngineError { op, message });
            }
            WireEvent::VisualFrame { frame } => {
                // 频谱/包络逐帧直达（无缓存——过期的帧没有听的价值）。
                self.on_visual_frame.post(&VisualFrameJs {
                    bins: frame.bins.iter().map(|bin| *bin as f64).collect(),
                    fft_size: frame.fft_size as f64,
                    sample_rate: frame.sample_rate as f64,
                    peak: frame.peak,
                    peak_l: frame.peak_l,
                    peak_r: frame.peak_r,
                    rms: frame.rms,
                    frames_fed: frame.frames_fed as f64,
                    envelope: frame.envelope.iter().map(|sample| *sample as f64).collect(),
                    envelope_hop_ms: frame.envelope_hop_ms,
                });
            }
            WireEvent::RemoteChanged { configured, kind, base_url } => {
                let status = RemoteStatus { configured, kind, base_url };
                self.on_remote.post(&status.clone());
                self.remote = Some(status);
            }
            WireEvent::RemoteListed { tracks } => {
                let listed = tracks.into_iter().map(remote_track_of).collect::<Vec<_>>();
                self.on_remote_tracks.post(&listed.clone());
                self.remote_tracks = Some(listed);
            }
            WireEvent::RemoteProbed { ok, message, count } => {
                let probe = RemoteProbe { ok, message, count: count as f64 };
                self.on_remote_probed.post(&probe.clone());
            }
            WireEvent::RemoteFetched { track, path } => {
                remember_session_fetch(&path);
                self.on_remote_fetched
                    .post(&RemoteFetched { track: remote_track_of(track), path });
                // A download just grew the bucket; keep it under its ceiling.
                prune_remote_audio_cache_in(&data_root(), REMOTE_AUDIO_CACHE_BUDGET);
            }
            WireEvent::RemoteArtwork { id, mime, data } => {
                self.on_remote_artwork
                    .post(&RemoteArtwork { id, mime, data });
            }
            WireEvent::RemotePlaylists { playlists } => {
                let listed = playlists
                    .into_iter()
                    .map(|playlist| RemotePlaylist {
                        id: playlist.id,
                        name: playlist.name,
                        track_count: playlist.track_count as f64,
                        duration_ms: playlist.duration_ms as f64,
                    })
                    .collect::<Vec<_>>();
                self.on_remote_playlists.post(&listed.clone());
                self.remote_playlists = Some(listed);
            }
            WireEvent::RemotePlaylistItems { id, tracks } => {
                let page = RemoteItemsPage {
                    id,
                    tracks: tracks.into_iter().map(remote_track_of).collect(),
                };
                self.on_remote_items.post(&page.clone());
                self.remote_items = Some(page);
            }
            WireEvent::FavouritesChanged { paths } => {
                let set = Favourites { paths };
                self.on_favourites.post(&set.clone());
                self.favourites = Some(set);
            }
            WireEvent::PlaylistsChanged { playlists } => {
                let listed = playlists
                    .into_iter()
                    .map(|playlist| Playlist {
                        id: playlist.id as f64,
                        name: playlist.name,
                        track_count: playlist.track_count as f64,
                    })
                    .collect::<Vec<_>>();
                self.on_playlists.post(&listed.clone());
                self.playlists = Some(listed);
            }
            WireEvent::PlaylistEntriesChanged { id, entries } => {
                let page = PlaylistEntriesPage {
                    id: id as f64,
                    entries: entries
                        .into_iter()
                        .map(|entry| PlaylistEntry {
                            path: entry.path,
                            position: entry.position as f64,
                        })
                        .collect(),
                };
                self.on_playlist_entries.post(&page.clone());
                self.playlist_entries = Some(page);
            }
            WireEvent::MostPlayedChanged { tracks } => {
                let listed: Vec<MostPlayed> = tracks
                    .into_iter()
                    .map(|track| MostPlayed {
                        path: track.path,
                        play_count: track.play_count as f64,
                    })
                    .collect();
                self.on_most_played.post(&listed.clone());
                self.most_played = Some(listed);
            }
            // Everything else Base reports has no listener in this UI yet.
            _ => {}
        }
    }

    /// Paints from one whole snapshot: the state a late subscriber needs.
    fn absorb(&mut self, snapshot: Snapshot) {
        let analysis = snapshot.analysis.clone().unwrap_or_default();
        // A snapshot names the stream it belongs to; its `analysis`, though,
        // can still be the previous stream's while the new one is being
        // analysed. When the path did not change and tags are already known,
        // keep them: the snapshot is not the tag-update channel
        // (`analysis_ready` is), and taking a stale analysis here put the
        // previous song's title on the new cover.
        let previous = self.player.clone();
        let same_path = previous
            .as_ref()
            .and_then(|player| player.path.as_deref())
            == snapshot.path.as_deref();
        let prior = previous
            .as_ref()
            .filter(|player| same_path && !player.title.is_empty());
        let title = prior
            .map(|player| player.title.clone())
            .unwrap_or_else(|| analysis.title.clone());
        let artist = prior
            .map(|player| player.artist.clone())
            .unwrap_or_else(|| analysis.artist.clone());
        let album = prior
            .map(|player| player.album.clone())
            .unwrap_or_else(|| analysis.album.clone());
        let bitrate = prior.map(|player| player.bitrate_kbps).unwrap_or_else(|| {
            snapshot
                .analysis
                .as_ref()
                .map(|a| a.bitrate_kbps as f64)
        });
        let codec = prior
            .map(|player| player.codec.clone())
            .unwrap_or_else(|| analysis.codec.clone());
        let codec_profile = prior
            .map(|player| player.codec_profile.clone())
            .unwrap_or_else(|| analysis.codec_profile.clone());
        let resolved_json = snapshot
            .resolved
            .as_ref()
            .map(|resolved| serde_json::to_string(resolved).expect("serializable JSON"));
        let mut player = PlayerState {
            state: snapshot.state.clone(),
            path: snapshot.path.clone(),
            title,
            artist,
            album,
            position_ms: snapshot.position * 1000.0,
            duration_ms: snapshot.duration * 1000.0,
            at: now_ms(),
            volume: snapshot.render_path.as_ref().map(|path| path.volume),
            bitrate_kbps: bitrate,
            codec,
            codec_profile,
            track_serial: snapshot.track_serial as f64,
            resolved_json: resolved_json.clone(),
        };
        // A preloaded analysis that arrived before this snapshot is newer
        // than the snapshot's own: keep its title instead of reverting it.
        if let Some((pending_path, pending)) = self.pending_analysis.take() {
            if snapshot.path.as_deref() == Some(pending_path.as_str()) {
                apply_analysis(&mut player, &pending);
            } else {
                self.pending_analysis = Some((pending_path, pending));
            }
        }
        self.on_player.post(&player.clone());
        self.player = Some(player);
        if let Some(text) = resolved_json.clone() {
            self.on_resolved.post(&text);
        }
        self.resolved = resolved_json;
        if let Some(render_path) = snapshot.render_path {
            self.set_render_path(render_path);
        }
        if !snapshot.devices.is_empty() {
            let listed: Vec<DeviceInfo> = snapshot
                .devices
                .iter()
                .map(|device| device_info(device.clone()))
                .collect();
            self.on_devices.post(&listed.clone());
            self.devices = Some(listed);
        }
        let snapshot_json = snapshot.queue_snapshot.as_ref().and_then(|s| serde_json::to_string(s).ok());
        if !snapshot.queue.is_empty() || snapshot_json.is_some() {
            let info = QueueInfo {
                queue: snapshot.queue.clone(),
                index: snapshot.queue_index.map(|index| index as f64),
                snapshot_json,
            };
            self.on_queue.post(&info.clone());
            self.queue = Some(info);
        }
        if let Some(settings) = snapshot.settings {
            let info = settings_info(&settings);
            self.on_settings.post(&info.clone());
            self.settings = Some(info);
        }
    }

    fn set_render_path(&mut self, render_path: RenderPath) {
        let info = RenderPathInfo {
            source_rate: render_path.source_rate as f64,
            sample_rate: render_path.sample_rate as f64,
            channels: render_path.channels as f64,
            bits_per_sample: render_path.bits_per_sample as f64,
            valid_bits: render_path.valid_bits as f64,
            is_float: render_path.is_float,
            resampling: render_path.resampling,
            dithered: render_path.dithered,
            bit_perfect: render_path.bit_perfect,
            reason: render_path.reason as f64,
            latency_ms: render_path.latency_ms,
            volume: render_path.volume,
            eq_enabled: render_path.eq_enabled,
            crossfeed_enabled: render_path.crossfeed_enabled,
            replay_gain_enabled: render_path.replay_gain_enabled,
            convolver_enabled: render_path.convolver_enabled,
            crossfade_enabled: render_path.crossfade_enabled,
        };
        self.on_render_path.post(&info.clone());
        if let Some(player) = self.player.as_mut() {
            player.volume = Some(info.volume);
        }
        self.render_path = Some(info);
    }
}

/// One `files_changed` entry (`Change` in `scan.rs`).
#[derive(Debug, Deserialize)]
struct Change {
    root: String,
    #[serde(default)]
    rescan: bool,
}

fn track_of(row: engine::TrackRow) -> Track {
    Track {
        path: row.path,
        title: row.title,
        artist: row.artist,
        album: row.album,
        album_artist: row.album_artist,
        disc_no: row.disc_no as f64,
        track_no: row.track_no as f64,
        year: row.year as f64,
        genre: row.genre,
        duration_ms: row.duration_ms as f64,
        sample_rate: row.sample_rate as f64,
        bits: row.bits as f64,
        channels: row.channels as f64,
    }
}

fn device_info(device: engine::Device) -> DeviceInfo {
    DeviceInfo {
        id: device.id,
        name: device.name,
        sample_rate: device.sample_rate as f64,
        bits_per_sample: device.bits_per_sample as f64,
        valid_bits: device.valid_bits as f64,
        channels: device.channels as f64,
        exclusive: device.exclusive,
        shared: device.shared,
        supported_sample_rates: device
            .supported_sample_rates
            .iter()
            .map(|rate| *rate as f64)
            .collect(),
    }
}

/// Maps one server listing row for the interface.
fn remote_track_of(row: engine::RemoteTrackRow) -> RemoteTrack {
    RemoteTrack {
        id: row.id,
        title: row.title,
        artist: row.artist,
        album: row.album,
        container: row.container,
        duration_ms: row.duration_ms as f64,
        size: row.size.map(|size| size as f64),
        cover_art: row.cover_art,
    }
}

fn settings_info(settings: &engine::Settings) -> SettingsInfo {
    SettingsInfo {
        volume: settings.volume,
        eq_enabled: settings.eq.enabled,
        eq_preamp_db: settings.eq.preamp_db,
        eq_bands: settings
            .eq
            .bands
            .iter()
            .map(|band| EqBandInfo {
                kind: band.kind as f64,
                freq_hz: band.freq_hz,
                gain_db: band.gain_db,
                q: band.q,
                enabled: band.enabled,
                channel_mask: band.channel_mask as f64,
            })
            .collect(),
        crossfeed_enabled: settings.crossfeed.enabled,
        crossfeed_amount: settings.crossfeed.amount,
        crossfeed_delay_ms: settings.crossfeed.delay_ms,
        crossfeed_cutoff_hz: settings.crossfeed.cutoff_hz,
        replay_gain_mode: settings.replay_gain.mode as f64,
        replay_gain_preamp_db: settings.replay_gain.preamp_db,
        replay_gain_prevent_clipping: settings.replay_gain.prevent_clipping,
        replay_gain_fallback_db: settings.replay_gain.fallback_db,
        convolver_enabled: settings.convolver.enabled,
        convolver_ir_path: settings.convolver.ir_path.clone(),
        convolver_wet: settings.convolver.wet,
        convolver_dry: settings.convolver.dry,
        convolver_gain_db: settings.convolver.gain_db,
        convolver_normalize: settings.convolver.normalize,
        crossfade_enabled: settings.crossfade.enabled,
        crossfade_duration_ms: settings.crossfade.duration_ms,
        crossfade_curve: settings.crossfade.curve as f64,
    }
}

fn now_ms() -> f64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|since| since.as_millis() as f64)
        .unwrap_or(0.0)
}

/// Where a finished lyrics search leaves its answer: the search runs on a
/// thread of its own, the listeners live on the core's, and this is the only
/// thing the two share.
static LYRICS_RESULTS: std::sync::OnceLock<
    std::sync::Mutex<Option<std::sync::mpsc::Sender<Vec<LyricsCandidate>>>>,
> = std::sync::OnceLock::new();

/// Drains a completed lyrics search into the listeners, waking every 30 ms —
/// the same shape as the engine's own channel, for the same reason.
async fn lyrics_pump(
    receiver: std::sync::mpsc::Receiver<Vec<LyricsCandidate>>,
    inner: Arc<Mutex<Inner>>,
) {
    loop {
        match receiver.try_recv() {
            Ok(candidates) => {
                if let Ok(guard) = inner.lock() {
                    guard.on_lyrics_found.post(&candidates);
                }
            }
            Err(std::sync::mpsc::TryRecvError::Empty) => {
                timer(std::time::Duration::from_millis(30)).await
            }
            Err(std::sync::mpsc::TryRecvError::Disconnected) => break,
        }
    }
}

/// Drains the engine's event channel into the listeners. Runs as a task of
/// the core, waking every 30 ms: the events are low-rate (state changes, one
/// position event per track tick), so a poll costs nothing next to a
/// cross-thread wakeup primitive this crate does not have.
async fn pump(
    engine: Shared,
    receiver: std::sync::mpsc::Receiver<String>,
    inner: Arc<Mutex<Inner>>,
    interruptions: Option<std::sync::mpsc::Receiver<audio_session::Interruption>>,
) {
    // Whether the track was playing when the interruption began: the ended
    // notification says whether playback may continue, not what it should
    // continue from.
    let mut was_playing = false;
    loop {
        match receiver.try_recv() {
            Ok(line) => {
                if let Ok(mut guard) = inner.lock() {
                    guard.deliver(&line);
                }
            }
            Err(std::sync::mpsc::TryRecvError::Empty) => timer(std::time::Duration::from_millis(30)).await,
            Err(std::sync::mpsc::TryRecvError::Disconnected) => break,
        }
        if let Some(interruptions) = &interruptions {
            while let Ok(event) = interruptions.try_recv() {
                answer_interruption(&engine, &inner, event, &mut was_playing);
            }
        }
        if engine.lock().map(|guard| guard.is_none()).unwrap_or(true) {
            break;
        }
    }
}

/// Carries out one interruption on the thread that owns [`Inner`], the one the
/// listeners may be called from.
fn answer_interruption(
    engine: &Shared,
    inner: &Arc<Mutex<Inner>>,
    event: audio_session::Interruption,
    was_playing: &mut bool,
) {
    let steps = match event {
        audio_session::Interruption::Began => {
            // Inner is what the UI is showing, and the pump has absorbed every
            // engine event before this one.
            let playing = lock(inner)
                .player
                .as_ref()
                .map(|player| player.state == "playing")
                .unwrap_or(false);
            *was_playing = playing;
            audio_session::begun_steps(playing)
        }
        audio_session::Interruption::Ended { should_resume } => {
            let was = std::mem::replace(was_playing, false);
            audio_session::ended_steps(should_resume, was)
        }
    };
    #[cfg(target_os = "ios")]
    let mut granted: Option<audio_session::Granted> = None;
    for step in steps {
        let result = match step {
            audio_session::Step::Pause => {
                send_command(engine, &serde_json::json!({ "command": "pause" }))
            }
            // There is no session to activate off iOS, and nothing sends
            // interruptions there; the arm exists so the decision table is
            // one piece of code on every build.
            #[cfg(target_os = "ios")]
            audio_session::Step::Activate => match audio_session::activate() {
                Ok(granted_format) => {
                    granted = Some(granted_format);
                    Ok(())
                }
                Err(e) => Err(e),
            },
            #[cfg(not(target_os = "ios"))]
            audio_session::Step::Activate => Ok(()),
            #[cfg(target_os = "ios")]
            audio_session::Step::Reinitialize => match granted {
                Some(granted_format) => crate::engine::ios::reinitialize_audio(
                    granted_format.sample_rate,
                    granted_format.channels,
                ),
                None => Ok(()),
            },
            #[cfg(not(target_os = "ios"))]
            audio_session::Step::Reinitialize => Ok(()),
            audio_session::Step::Resume => {
                send_command(engine, &serde_json::json!({ "command": "resume" }))
            }
        };
        // A notification has no call to return an error to; the page's usual
        // error surface is what tells the user the resume did not happen. The
        // remaining steps are dropped: resuming into a session that would not
        // activate is the failure this reports, not a recovery.
        if let Err(message) = result {
            lock(inner).on_error.post(&EngineError {
                op: "interruption".to_owned(),
                message,
            });
            break;
        }
    }
}

// ----------------------------------------------------------------- the class

/// The engine: Base, started in this process and driven over its C ABI.
/// Construct it as often as you like; every instance is a handle onto its own
/// Base, and the interface keeps exactly one.
#[class]
pub struct Base {
    engine: Shared,
    inner: Arc<Mutex<Inner>>,
}

#[class]
impl Base {
    /// Starts Base with its library at `%LOCALAPPDATA%/Neiro/library.db`.
    /// Throws `E_ENGINE: …` when the DLL is missing or Base cannot start;
    /// the interface then says the engine is not connected.
    pub fn new(cx: CallCx<'_>) -> Result<Self, String> {
        // Base opens a SQLite file; SQLite creates the file, not its
        // directory. A packaged app's data directory may not exist yet, so it
        // is made here, once, before anything asks for the file.
        let library = library_path();
        if let Some(dir) = library.parent() {
            std::fs::create_dir_all(dir)
                .map_err(|e| format!("E_ENGINE: 无法创建数据目录 {}：{e}", dir.display()))?;
        }
        // Nothing is downloading yet, so a `.part` file now is what a killed
        // process left behind, and the oldest downloads may be over budget.
        let cleaned = reconcile_remote_cache_in(&data_root());
        if cleaned > 0 {
            eprintln!("neiro: 启动时清理了 {cleaned} 个远程缓存文件");
        }
        #[cfg(target_os = "ios")]
        let granted = audio_session::activate()
            .map_err(|e| format!("E_ENGINE: 无法激活音频会话：{e}"))?;
        let (engine, receiver) = Engine::open(&library)?;
        // The contract's first-play order: activate, then tell the core the
        // format the route granted, then open. Nothing is stale this early, so
        // a refusal here is not fatal -- a core that predates this call (it
        // answers -5) still reads the route itself when it opens.
        #[cfg(target_os = "ios")]
        let _ = crate::engine::ios::reinitialize_audio(granted.sample_rate, granted.channels);
        // The snapshot is a synchronous read: the listeners about to register
        // get the whole current state before any event arrives, so the first
        // paint is the engine's, not an empty page.
        let inner = Arc::new(Mutex::new(Inner::default()));
        if let Ok(snapshot) = engine.snapshot() {
            if let Ok(mut guard) = inner.lock() {
                guard.absorb(snapshot);
            }
        }
        // 默认输出策略：device 速率、very_high 重采样、**shared** 共享模式
        // （用户要求 shared 为默认；exclusive 需要端点上没有别的进程，普通
        // 桌面使用 shared 才不会和浏览器/会议软件互斥）。界面里可改，改后
        // 由界面持久化并在连接后重发。
        let _ = engine.command(
            r#"{"command":"set_policy","policy":{"rate":{"mode":"device"},"quality":"very_high","share":"shared"}}"#,
        );
        let engine = Arc::new(Mutex::new(Some(engine)));
        // An interruption is a session event, so the host answers it: pause
        // what was playing, and after the system says playback may continue,
        // reactivate the deactivated session and resume. The notification
        // arrives on an AVFoundation thread, which must not touch `Inner`
        // (its listeners are not Send), so it only forwards the event and the
        // pump -- the thread that already owns `Inner` -- decides and acts.
        let interruptions = {
            let (send, receive) = std::sync::mpsc::channel::<audio_session::Interruption>();
            #[cfg(target_os = "ios")]
            {
                audio_session::install(move |event| {
                    let _ = send.send(event);
                })
                .map_err(|e| format!("E_ENGINE: 无法监听音频中断：{e}"))?;
                Some(receive)
            }
            #[cfg(not(target_os = "ios"))]
            {
                let _ = (send, receive);
                None
            }
        };

        let pump_engine = engine.clone();
        let pump_inner = inner.clone();
        cx.start(async move {
            let (lyrics_send, lyrics_receive) = std::sync::mpsc::channel();
            if let Ok(mut slot) = LYRICS_RESULTS
                .get_or_init(|| std::sync::Mutex::new(None))
                .lock()
            {
                *slot = Some(lyrics_send);
            }
            spawn(lyrics_pump(lyrics_receive, pump_inner.clone()));
            pump(pump_engine, receiver, pump_inner, interruptions).await;
        });
        Ok(Base { engine, inner })
    }

    /// Stops Base now (the pump ends when it sees the engine gone). Every
    /// Base also has `dispose()`, which frees the handle; call both only on
    /// teardown, not on hot updates.
    pub fn shutdown(&mut self) {
        if let Ok(mut guard) = self.engine.lock() {
            *guard = None;
        }
    }

    /// Calls `listener` with the player's state now, then after every change.
    pub fn on_player(&self, #[args(player)] listener: Callback<PlayerState>) -> Stop {
        self.subscribe(|inner| &mut inner.on_player, |inner| inner.player.clone(), listener)
    }

    /// Calls `listener` with the render path now, then after every change.
    pub fn on_render_path(&self, #[args(render_path)] listener: Callback<RenderPathInfo>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_render_path,
            |inner| inner.render_path.clone(),
            listener,
        )
    }

    /// Calls `listener` with the endpoints now (empty means the engine found
    /// none), then after every listing.
    pub fn on_devices(&self, #[args(devices)] listener: Callback<Vec<DeviceInfo>>) -> Stop {
        self.subscribe(|inner| &mut inner.on_devices, |inner| inner.devices.clone(), listener)
    }

    /// Calls `listener` with the queue now, then after every change.
    pub fn on_queue(&self, #[args(queue)] listener: Callback<QueueInfo>) -> Stop {
        self.subscribe(|inner| &mut inner.on_queue, |inner| inner.queue.clone(), listener)
    }

    /// Calls `listener` with the applied settings now, then after every
    /// change.
    pub fn on_settings(&self, #[args(settings)] listener: Callback<SettingsInfo>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_settings,
            |inner| inner.settings.clone(),
            listener,
        )
    }

    /// Calls `listener` with the last page of tracks, then after every
    /// `list_tracks` answer.
    pub fn on_tracks(&self, #[args(page)] listener: Callback<TracksPage>) -> Stop {
        self.subscribe(|inner| &mut inner.on_tracks, |inner| inner.tracks.clone(), listener)
    }

    /// Calls `listener` with the last album list, then after every
    /// `list_albums` answer.
    pub fn on_albums(&self, #[args(albums)] listener: Callback<Vec<Album>>) -> Stop {
        self.subscribe(|inner| &mut inner.on_albums, |inner| inner.albums.clone(), listener)
    }

    /// Calls `listener` with the last artist list, then after every
    /// `list_artists` answer.
    pub fn on_artists(&self, #[args(artists)] listener: Callback<Vec<Artist>>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_artists,
            |inner| inner.artists.clone(),
            listener,
        )
    }

    /// Calls `listener` with the last search results, then after every
    /// `search` answer.
    pub fn on_search(&self, #[args(page)] listener: Callback<SearchPage>) -> Stop {
        self.subscribe(|inner| &mut inner.on_search, |inner| inner.search.clone(), listener)
    }

    /// Calls `listener` with the pictures of the last asked path, then after
    /// every `read_artwork_list` answer.
    pub fn on_artwork_list(&self, #[args(list)] listener: Callback<ArtworkList>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_artwork_list,
            |inner| inner.artwork_list.clone(),
            listener,
        )
    }

    /// Calls `listener` after every `read_artwork_bytes` answer.
    pub fn on_artwork_data(&self, #[args(bytes)] listener: Callback<ArtworkBytes>) -> Stop {
        let inner = self.inner.clone();
        let id = {
            let mut guard = lock(&inner);
            guard.on_artwork_data.add(listener)
        };
        Stop::new(move || {
            if let Ok(mut guard) = inner.lock() {
                guard.on_artwork_data.remove(id);
            }
        })
    }

    /// Calls `listener` after every scan or watch event.
    pub fn on_scan(&self, #[args(event)] listener: Callback<ScanEvent>) -> Stop {
        let inner = self.inner.clone();
        let id = {
            let mut guard = lock(&inner);
            guard.on_scan.add(listener)
        };
        Stop::new(move || {
            if let Ok(mut guard) = inner.lock() {
                guard.on_scan.remove(id);
            }
        })
    }

    /// Calls `listener` after every error Base reports.
    pub fn on_error(&self, #[args(error)] listener: Callback<EngineError>) -> Stop {
        let inner = self.inner.clone();
        let id = {
            let mut guard = lock(&inner);
            guard.on_error.add(listener)
        };
        Stop::new(move || {
            if let Ok(mut guard) = inner.lock() {
                guard.on_error.remove(id);
            }
        })
    }

    // ---- the remote library ---------------------------------------------

    /// Calls `listener` with the remote session's status now, then after
    /// every change. Never carries the token or the password.
    pub fn on_remote(&self, #[args(status)] listener: Callback<RemoteStatus>) -> Stop {
        self.subscribe(|inner| &mut inner.on_remote, |inner| inner.remote.clone(), listener)
    }

    /// Calls `listener` with the last server listing, then after every
    /// `list_remote` answer.
    pub fn on_remote_tracks(&self, #[args(tracks)] listener: Callback<Vec<RemoteTrack>>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_remote_tracks,
            |inner| inner.remote_tracks.clone(),
            listener,
        )
    }

    /// Calls `listener` with the last server playlist list, then after every
    /// `list_remote_playlists` answer.
    pub fn on_remote_playlists(
        &self,
        #[args(playlists)] listener: Callback<Vec<RemotePlaylist>>,
    ) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_remote_playlists,
            |inner| inner.remote_playlists.clone(),
            listener,
        )
    }

    /// Calls `listener` after every `list_remote_playlist_items` answer.
    pub fn on_remote_items(&self, #[args(page)] listener: Callback<RemoteItemsPage>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_remote_items,
            |inner| inner.remote_items.clone(),
            listener,
        )
    }

    /// Calls `listener` after every `fetch_remote` answer: the item is a
    /// local file at `path` now.
    pub fn on_remote_fetched(&self, #[args(fetched)] listener: Callback<RemoteFetched>) -> Stop {
        let inner = self.inner.clone();
        let id = {
            let mut guard = lock(&inner);
            guard.on_remote_fetched.add(listener)
        };
        Stop::new(move || {
            if let Ok(mut guard) = inner.lock() {
                guard.on_remote_fetched.remove(id);
            }
        })
    }

    /// Calls `listener` after every `fetch_remote_artwork` answer.
    pub fn on_remote_artwork(&self, #[args(artwork)] listener: Callback<RemoteArtwork>) -> Stop {
        let inner = self.inner.clone();
        let id = {
            let mut guard = lock(&inner);
            guard.on_remote_artwork.add(listener)
        };
        Stop::new(move || {
            if let Ok(mut guard) = inner.lock() {
                guard.on_remote_artwork.remove(id);
            }
        })
    }

    /// Points the remote worker at a Jellyfin server that signs in by
    /// username and password; the exchange to a token happens once, at the
    /// server, and nothing below this point keeps the password. The answer
    /// is the `on_remote` and `on_error` events, not this call.
    pub fn set_remote_jellyfin_password(
        &self,
        base_url: String,
        user: String,
        password: String,
    ) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_remote",
            "cache_dir": remote_cache_dir(),
            "server": {
                "kind": "jellyfin",
                "base_url": base_url,
                "auth": { "kind": "password", "user": user, "password": password },
                "device_id": installation_id(),
            },
        }))
    }

    /// Points the remote worker at a Jellyfin server holding an API key or
    /// an earlier token.
    pub fn set_remote_jellyfin_token(
        &self,
        base_url: String,
        token: String,
    ) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_remote",
            "cache_dir": remote_cache_dir(),
            "server": {
                "kind": "jellyfin",
                "base_url": base_url,
                "auth": { "kind": "token", "token": token },
                "device_id": installation_id(),
            },
        }))
    }

    /// Points the remote worker at a Navidrome (Subsonic) server.
    pub fn set_remote_navidrome(
        &self,
        base_url: String,
        user: String,
        password: String,
    ) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_remote",
            "cache_dir": remote_cache_dir(),
            "server": { "kind": "navidrome", "base_url": base_url, "user": user, "password": password },
        }))
    }

    /// Points the remote worker at a WebDAV server.
    pub fn set_remote_webdav(
        &self,
        base_url: String,
        user: String,
        password: String,
    ) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_remote",
            "cache_dir": remote_cache_dir(),
            "server": {
                "kind": "webdav",
                "base_url": base_url,
                "user": if user.is_empty() { None } else { Some(user) },
                "password": if password.is_empty() { None } else { Some(password) },
            },
        }))
    }

    /// Points the remote worker at any registered `RemoteProviderPlugin` by kind and JSON params map.
    pub fn set_remote(&self, kind: String, params_json: String) -> Result<(), String> {
        let params: serde_json::Value = serde_json::from_str(&params_json)
            .unwrap_or_else(|_| serde_json::Value::Object(serde_json::Map::new()));
        let base_url = params
            .get("base_url")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let user = params
            .get("user")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let password = params
            .get("password")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();
        let token = params
            .get("token")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .to_string();

        let server = match kind.as_str() {
            "jellyfin" | "jellyfin_token" => {
                let auth = if kind == "jellyfin_token" || (!token.is_empty() && password.is_empty())
                {
                    serde_json::json!({ "kind": "token", "token": token })
                } else {
                    serde_json::json!({ "kind": "password", "user": user, "password": password })
                };
                serde_json::json!({
                    "kind": "jellyfin",
                    "base_url": base_url,
                    "auth": auth,
                    // The core's Server reader keeps every flat text field in
                    // its params map, so this survives a core that does not
                    // know the key yet.
                    "device_id": installation_id(),
                    "params": params,
                })
            }
            "navidrome" => serde_json::json!({
                "kind": "navidrome",
                "base_url": base_url,
                "user": user,
                "password": password,
                "params": params,
            }),
            "webdav" => serde_json::json!({
                "kind": "webdav",
                "base_url": base_url,
                "user": if user.is_empty() { None } else { Some(user) },
                "password": if password.is_empty() { None } else { Some(password) },
                "params": params,
            }),
            _ => serde_json::json!({
                "kind": kind,
                "base_url": base_url,
                "params": params,
            }),
        };

        self.command(&serde_json::json!({
            "command": "set_remote",
            "cache_dir": remote_cache_dir(),
            "server": server,
        }))
    }

    /// This installation's remote identity, the same value `set_remote`
    /// sends to the core. The connection probe reads it so its request names
    /// the same device the engine's downloads do.
    pub fn device_id(&self) -> String {
        installation_id()
    }

    /// Disconnects. The local cover cache survives: a server that is
    /// re-configured later (the common case) keeps its downloads.
    pub fn clear_remote(&self) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_remote",
            "cache_dir": remote_cache_dir(),
            "server": null,
        }))
    }

    /// Asks Base for one file's tag fields; answered on `on_tags`.
    pub fn read_tags(&self, path: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "read_tags", "path": path }))
    }

    /// One page of the server's audio items; answered on `on_remote_tracks`.
    /// The limit defaults to a page (200), not the library.
    pub fn list_remote(&self, start: u32, limit: Option<u32>) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "list_remote",
            "start": start as u64,
            "limit": limit.map(|limit| limit as u64),
        }))
    }

    /// Downloads one listed item into the cache; answered on
    /// `on_remote_fetched` with the local path to `play`. The id must come
    /// from the last listing: a guess would cache a file the caller never
    /// saw.
    pub fn fetch_remote(&self, id: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "fetch_remote", "id": id }))
    }

    /// One item's cover, base64; answered on `on_remote_artwork`. `data`
    /// `null` is "the server has no picture", not a failure.
    pub fn fetch_remote_artwork(&self, id: String) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "fetch_remote_artwork",
            "id": id,
        }))
    }

    /// The server's playlists; answered on `on_remote_playlists`.
    pub fn list_remote_playlists(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "list_remote_playlists" }))
    }

    /// One playlist's entries; answered on `on_remote_items`.
    pub fn list_remote_playlist_items(
        &self,
        id: String,
        start: u32,
        limit: Option<u32>,
    ) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "list_remote_playlist_items",
            "id": id,
            "start": start as u64,
            "limit": limit.map(|limit| limit as u64),
        }))
    }

    // ---- the library: favourites, playlists, play counts --------------------

    /// Calls `listener` with the favourites now, then after every change.
    /// The set is whole every time; the UI owns the diff and the name
    /// ("Liked Songs" is what spotish would call it — here it is "喜欢的
    /// 歌曲").
    pub fn on_favourites(&self, #[args(paths)] listener: Callback<Favourites>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_favourites,
            |inner| inner.favourites.clone(),
            listener,
        )
    }

    /// Calls `listener` with the playlists now, then after every change.
    /// The list is whole, in sidebar order.
    pub fn on_playlists(&self, #[args(playlists)] listener: Callback<Vec<Playlist>>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_playlists,
            |inner| inner.playlists.clone(),
            listener,
        )
    }

    /// Calls `listener` with the last asked playlist's entries, then after
    /// every change to that playlist.
    pub fn on_playlist_entries(
        &self,
        #[args(page)] listener: Callback<PlaylistEntriesPage>,
    ) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_playlist_entries,
            |inner| inner.playlist_entries.clone(),
            listener,
        )
    }

    /// Calls `listener` with the fields of the last asked file, then after
    /// every `read_tags` answer. `path` names the file that was read, so a
    /// reply that arrives after the track changed is recognisable.
    pub fn on_tags(&self, #[args(fields)] listener: Callback<TagFields>) -> Stop {
        self.subscribe(|inner| &mut inner.on_tags, |inner| inner.tags.clone(), listener)
    }

    /// Calls `listener` with the engine's diagnostics JSON for the stream
    /// now, then after every re-resolve (`render_path` 描述的是结果；
    /// `resolved` 里的 `padFrameCount`/`maxBufferGapMs` 是"为什么"）。
    /// `resolved_json` 是一个字符串，界面/诊断自己 `JSON.parse`。
    pub fn on_resolved(&self, #[args(resolved_json)] listener: Callback<String>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_resolved,
            |inner| inner.resolved.clone(),
            listener,
        )
    }

    /// Calls `listener` with the most-played list now, then whenever one
    /// republishes it (a play that happened after this UI asked).
    pub fn on_most_played(&self, #[args(tracks)] listener: Callback<Vec<MostPlayed>>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_most_played,
            |inner| inner.most_played.clone(),
            listener,
        )
    }

    /// Stars a track, or takes the star back. The answer's value ("whether
    /// it changed") is Base's event business, not this call's.
    pub fn set_favourite(&self, path: String, on: bool) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_favourite",
            "path": path,
            "on": on,
        }))
    }

    /// 把 EQ 参数整体发下去（enabled 开关、preamp、全部 bands 原样回传）。
    /// answered by `settings_changed`。
    pub fn set_eq(&self, enabled: bool, preamp_db: f64, bands: Vec<EqBandInfo>) -> Result<(), String> {
        let bands_json: Vec<serde_json::Value> = bands
            .iter()
            .map(|band| {
                serde_json::json!({
                    "kind": band.kind as u64,
                    "freq_hz": band.freq_hz,
                    "gain_db": band.gain_db,
                    "q": band.q,
                    "enabled": band.enabled,
                    "channel_mask": band.channel_mask as u64,
                })
            })
            .collect();
        self.command(&serde_json::json!({
            "command": "set_eq",
            "params": {
                "enabled": enabled,
                "preamp_db": preamp_db,
                "bands": bands_json,
                "phase": 0,
            },
        }))
    }

    /// Crossfeed 开关与它的三个参数（原样回传）。
    pub fn set_crossfeed(&self, enabled: bool, amount: f64, delay_ms: f64, cutoff_hz: f64) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_crossfeed",
            "params": { "enabled": enabled, "amount": amount, "delay_ms": delay_ms, "cutoff_hz": cutoff_hz },
        }))
    }

    /// ReplayGain 模式（0 off / 1 track / 2 album）与其余参数。
    pub fn set_replay_gain(&self, mode: u32, preamp_db: f64, prevent_clipping: bool, fallback_db: f64) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_replay_gain",
            "params": {
                "mode": mode as u64,
                "preamp_db": preamp_db,
                "prevent_clipping": prevent_clipping,
                "fallback_db": fallback_db,
            },
        }))
    }

    /// Convolver 开关（ir_path 等参数原样回传——Base 里 IR 由设置持有）。
    pub fn set_convolver(&self, enabled: bool, ir_path: String, wet: f64, dry: f64, gain_db: f64, normalize: bool) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_convolver",
            "params": {
                "enabled": enabled,
                "ir_path": ir_path,
                "wet": wet,
                "dry": dry,
                "gain_db": gain_db,
                "normalize": normalize,
            },
        }))
    }

    /// Crossfade 开关与时长（曲线 0 = linear，1 = equal power，原样回传）。
    pub fn set_crossfade(&self, enabled: bool, duration_ms: f64, curve: u32) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_crossfade",
            "params": { "enabled": enabled, "duration_ms": duration_ms, "curve": curve as u64 },
        }))
    }

    /// Starts or stops forwarding visualiser frames. `interval_ms` 是节拍
    /// （60 Hz 的视图给 16 ms）；`null` 停。帧只在播放中才有——answered on
    /// `on_visual_frame`。
    pub fn set_visualiser(&self, interval_ms: Option<u32>) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_visualiser",
            "interval_ms": interval_ms.map(|ms| ms as u64),
        }))
    }

    /// Calls `listener` with every visualiser frame (no cache — a stale
    /// frame is worthless). The frames stop when the stream stops.
    pub fn on_visual_frame(&self, #[args(frame)] listener: Callback<VisualFrameJs>) -> Stop {
        let inner = self.inner.clone();
        let id = {
            let mut guard = lock(&inner);
            guard.on_visual_frame.add(listener)
        };
        Stop::new(move || {
            if let Ok(mut guard) = inner.lock() {
                guard.on_visual_frame.remove(id);
            }
        })
    }

    /// Asks for the playlist list; answered on `on_playlists`.
    pub fn list_playlists(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "list_playlists" }))
    }

    /// Asks for the whole favourite list; answered on `on_favourites`.
    pub fn list_favourites(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "list_favourites" }))
    }

    /// Creates a playlist. Names are unique case-insensitively — a rejected
    /// duplicate arrives as an `on_error`.
    pub fn create_playlist(&self, name: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "create_playlist", "name": name }))
    }

    /// Renames a playlist; the answer re-publishes the whole list.
    pub fn rename_playlist(&self, id: f64, name: String) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "rename_playlist",
            "id": id as u64,
            "name": name,
        }))
    }

    /// Deletes a playlist and its entries (the schema's cascade).
    pub fn delete_playlist(&self, id: f64) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "delete_playlist",
            "id": id as u64,
        }))
    }

    /// Adds one track. Adding twice is a no-op, not an error.
    pub fn add_to_playlist(&self, id: f64, path: String) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "add_to_playlist",
            "id": id as u64,
            "path": path,
        }))
    }

    /// Removes one track; positions stay dense (the answer re-lists).
    pub fn remove_from_playlist(&self, id: f64, path: String) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "remove_from_playlist",
            "id": id as u64,
            "path": path,
        }))
    }

    /// One playlist's entries in order; answered on `on_playlist_entries`.
    pub fn list_playlist_entries(&self, id: f64) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "list_playlist_entries",
            "id": id as u64,
        }))
    }

    /// The most-played tracks, most first; answered on `on_most_played`.
    pub fn list_most_played(&self, limit: u32) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "list_most_played",
            "limit": limit as u64,
        }))
    }

    /// Records a play. `at_ms` is the host's clock (the interface's
    /// `Date.now()`); Base owns no clock and invents none.
    pub fn record_play(&self, path: String, at_ms: f64) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "record_play",
            "path": path,
            "at_ms": at_ms as u64,
        }))
    }

    /// One page of the browsing index, album/artist/disc/track order.
    /// A page is at most 500 rows; a library beyond that pages through.
    pub fn list_tracks(&self, offset: u32, limit: u32) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "list_tracks",
            "offset": offset as u64,
            "limit": limit as u64,
        }))
    }

    /// The whole album list, one row per album.
    pub fn list_albums(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "list_albums" }))
    }

    /// The whole artist list, with track and album counts.
    pub fn list_artists(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "list_artists" }))
    }

    /// Substring search over title, artist, album and album artist. Base's
    /// match is `LIKE`, so a search-as-you-type view debounces.
    pub fn search(&self, query: String, limit: u32) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "search",
            "query": query,
            "limit": limit as u64,
        }))
    }

    /// The endpoints the engine sees now; answered on `on_devices`.
    pub fn list_devices(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "list_devices" }))
    }

    /// Reads a file's embedded pictures (descriptions only, no bytes);
    /// answered on `on_artwork_list`.
    pub fn read_artwork_list(&self, path: String) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "read_artwork",
            "path": path,
        }))
    }

    /// Reads one picture's bytes, base64; answered on `on_artwork_data`.
    pub fn read_artwork_bytes(&self, path: String, index: u32) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "read_artwork",
            "path": path,
            "index": index as u64,
        }))
    }

    /// The cover a previous run cached for `path`: the `data:` URI the
    /// interface paints, or `"none"` when that run read the file and it
    /// carries no picture. `""` means this file has no entry (not cached
    /// yet, or its identity changed).
    pub fn cover_cache_get(&self, path: String) -> Result<String, String> {
        Ok(cover_cache_read(&local_cover_cache_dir(), &path))
    }

    /// Remembers the answer for `path`: the `data:` URI, or `"none"` when
    /// the file has no embedded picture. A file that cannot be stat'ed is
    /// not cached — a key without a real identity would serve stale bytes.
    pub fn cover_cache_put(&self, path: String, uri: String) -> Result<(), String> {
        cover_cache_write(&local_cover_cache_dir(), &path, &uri)
    }

    /// The cache buckets' size and count, as a JSON object the interface
    /// formats for people.
    pub fn cache_stats(&self) -> Result<String, String> {
        let root = data_root();
        let stats = cache_stats_in(&root);
        serde_json::to_string(&stats).map_err(|e| format!("E_CACHE: 统计结果序列化失败：{e}"))
    }

    /// The linked core's ABI version on iOS; `0` elsewhere, where the core is
    /// loaded at run time and has no link-time version. The Stage 0 spike
    /// displays it to prove the host → Rust → C link.
    pub fn base_abi_version(&self) -> u32 {
        #[cfg(target_os = "ios")]
        {
            crate::engine::ios::abi_version()
        }
        #[cfg(not(target_os = "ios"))]
        {
            0
        }
    }

    /// Deletes one cache bucket's files (`all` = every bucket), keeps the
    /// directories, and answers the stats as they are after the clean.
    pub fn clear_cache(&self, kind: String) -> Result<String, String> {
        clear_cache_in(&data_root(), &kind)
    }

    /// Writes the iOS test tone once, then plays it through the core.
    pub fn play_tone(&self) -> Result<(), String> {
        #[cfg(target_os = "ios")]
        {
            let path = data_root().join("Neiro").join("tone.wav");
            if !path.exists() {
                write_test_tone(&path)?;
            }
            self.play(path.to_string_lossy().into_owned())
        }
        #[cfg(not(target_os = "ios"))]
        {
            Err("E_PLATFORM: play_tone 只在 iOS 上可用".to_string())
        }
    }

    // ---- playback: each is a queued command; the state changes as an event -

    /// Replaces the queue with this one track and starts it.
    pub fn play(&self, path: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "play", "path": path }))
    }

    /// Appends tracks to the existing queue (each one pre-analyzed). 随机
    /// 播放就是 `play(第一首)` + `enqueue(其余随机序)`——Base 的队列是按序
    /// 播放的事实，shuffle 是构建队列时的排列，不改引擎语义。
    pub fn enqueue(&self, paths: Vec<String>) -> Result<(), String> {
        if paths.is_empty() {
            return Ok(());
        }
        self.command(&serde_json::json!({ "command": "enqueue", "paths": paths }))
    }

    /// Takes one entry out of the queue by position. The entry that is
    /// playing cannot be removed this way (the engine refuses): stopping it
    /// first is the way to drop the current track.
    pub fn remove_from_queue(&self, index: u32) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "remove_from_queue", "index": index }))
    }

    /// Moves one queue entry to another position; the playing entry keeps
    /// playing and the index follows it.
    pub fn move_in_queue(&self, from: u32, to: u32) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "move_in_queue", "from": from, "to": to }))
    }

    /// Drops every queued entry except the one playing.
    pub fn clear_queue(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "clear_queue" }))
    }

    /// Pauses the current track, keeping the position.
    pub fn pause(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "pause" }))
    }

    /// Resumes a paused track from its position.
    pub fn resume(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "resume" }))
    }

    /// Stops the current track; the queue stays as it is.
    pub fn stop(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "stop" }))
    }

    /// Stops the current track and opens the next queue entry.
    pub fn next(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "next" }))
    }

    /// Steps to previous track or seeks to 0.0 based on playback position (>3s seek 0; <=3s pop history).
    pub fn previous(&self) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.previous(),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Apple Music dual-tier queue: plays a full context list starting at start_index.
    pub fn play_context(&self, items_json: String, start_index: u32) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.play_context(&items_json, start_index as usize),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Apple Music dual-tier queue: inserts items to the front of the user queue (Play Next).
    pub fn play_next(&self, items_json: String) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.play_next(&items_json),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Apple Music dual-tier queue: appends items to the back of the user queue (Play Later).
    pub fn play_later(&self, items_json: String) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.play_later(&items_json),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Sets the repeat mode: 0 = Off, 1 = All, 2 = One.
    pub fn set_repeat_mode(&self, mode: i32) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.set_repeat_mode(mode),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Toggles shuffle mode.
    pub fn set_shuffle_mode(&self, enabled: bool) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.set_shuffle_mode(enabled),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Removes an item from the user queue by ID.
    pub fn remove_queue_item(&self, id: String) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.remove_queue_item(&id),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Clears the user-specified Up Next queue.
    pub fn clear_user_queue(&self) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.clear_user_queue(),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Reorders an item in the user queue from index to index.
    pub fn move_user_queue_item(&self, from: u32, to: u32) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.move_user_queue_item(from as usize, to as usize),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Seeks to `seconds` into the track.
    pub fn seek(&self, seconds: f64) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "seek", "seconds": seconds }))
    }

    /// Releases the output device/DAC immediately while preserving track position and queue.
    pub fn release_device(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "release_device" }))
    }

    /// Returns the registered remote providers JSON string from Base.
    pub fn remote_providers(&self) -> Result<String, String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => Ok(engine.remote_providers()),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Lists immediate child folders and audio tracks in `path` on demand.
    pub fn list_directory(&self, path: String) -> Result<String, String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.list_directory(&path),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Returns JSON array of all registered plugins across UI, Base, and Engine tiers.
    pub fn list_plugins(&self) -> Result<String, String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => Ok(engine.list_plugins()),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Toggles whether a plugin is enabled.
    pub fn set_plugin_enabled(&self, id: String, enabled: bool) -> Result<(), String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.set_plugin_enabled(&id, enabled),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Rescans external plugins and returns the updated JSON array of all plugins.
    pub fn reload_plugins(&self) -> Result<String, String> {
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => Ok(engine.reload_plugins()),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }

    /// Sets the linear gain (0..1). The engine's volume is linear
    /// (`OUTPUT-POLICY.md`: "−3 dB = 0.7071068"); the slider maps onto it
    /// directly, and the render path reports what is actually applied.
    pub fn set_volume(&self, gain: f64) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "set_volume", "gain": gain }))
    }

    /// Chooses the endpoint for the next open, and for the current one when
    /// a track is playing. `null` means "the endpoint last used".
    pub fn set_device(&self, device_id: Option<String>) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_device",
            "device_id": device_id,
        }))
    }

    /// Changes the output policy (`{"command":"set_policy","policy":…}`):
    /// rate 模式（native / device / fixed）、resampler 质量、共享模式。
    /// The interface builds the JSON in the engine's own field names, so what
    /// the engine grants stays what the engine grants — 正是这个请求不做任
    /// 何承诺，`render_path` 才如报告所见。一个 open-time 设置：正在播放的
    /// 会话马上在当前位置重开。
    pub fn set_policy(&self, policy_json: String) -> Result<(), String> {
        let policy = serde_json::from_str::<serde_json::Value>(&policy_json)
            .map_err(|e| format!("E_ENGINE: policy 无法解析：{e}"))?;
        self.command(&serde_json::json!({ "command": "set_policy", "policy": policy }))
    }

    /// Walks a folder and diffs it against the index; answered on `on_scan`
    /// as `"started"` then `"done"`.
    pub fn scan_folder(&self, root: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "scan_folder", "root": root }))
    }

    /// Reports changes under a folder from now on; answered on `on_scan`.
    pub fn watch_folder(&self, root: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "watch_folder", "root": root }))
    }

    /// Stops reporting changes under `root`; answered on `on_scan`.
    /// Write a lyrics text into the file's tag families.
    ///
    /// `families` are the tag families the container holds (`id3v2`,
    /// `vorbis_comments`, `mp4_ilst`, ...). The caller picks them from the
    /// file that is open; a family the container cannot hold is refused by Base
    /// before anything is written, so a wrong guess reports an error rather
    /// than silently doing nothing.
    /// Ask the online lyrics providers for this track's words.
    ///
    /// The request runs on a thread of its own and answers through the
    /// channel `lyrics_pump` drains: it blocks for as long as the providers
    /// take, and the interface keeps painting while it does.
    pub fn search_lyrics(
        &self,
        artist: String,
        title: String,
        album: Option<String>,
        duration_s: Option<f64>,
        providers_json: Option<String>,
    ) -> Result<(), String> {
        if title.trim().is_empty() {
            return Err("E_LYRICS: 这首歌没有标题，无法搜索。".into());
        }
        let enabled_providers: Option<Vec<String>> = providers_json
            .as_deref()
            .and_then(|s| serde_json::from_str::<Vec<String>>(s).ok());
        let sender = LYRICS_RESULTS
            .get_or_init(|| std::sync::Mutex::new(None))
            .lock()
            .ok()
            .and_then(|guard| guard.clone());
        std::thread::spawn(move || {
            let candidates = lyrics_providers::search_all_lyrics(
                &artist,
                &title,
                album.as_deref(),
                duration_s,
                enabled_providers.as_deref(),
            );
            if let Some(sender) = sender {
                let _ = sender.send(candidates);
            }
        });
        Ok(())
    }

    /// Asks the server one item and reports the outcome; answered on
    /// `onRemoteProbed`.
    pub fn probe_remote(&self) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "probe_remote" }))
    }

    /// Calls `listener` with each 测试连接 outcome.
    pub fn on_remote_probed(&self, #[args(probe)] listener: Callback<RemoteProbe>) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_remote_probed,
            |_inner| None,
            listener,
        )
    }

    /// Calls `listener` after every online lyrics search.
    pub fn on_lyrics_found(
        &self,
        #[args(candidates)] listener: Callback<Vec<LyricsCandidate>>,
    ) -> Stop {
        self.subscribe(
            |inner| &mut inner.on_lyrics_found,
            |_inner| None,
            listener,
        )
    }

    /// Writes lyrics text into the file's tag families.
    pub fn write_lyrics(&self, path: String, text: String) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "write_lyrics",
            "path": path,
            "text": text,
        }))
    }

    /// Stops reporting changes under `root` and deletes what was indexed under
    /// it; answered on `on_scan`.
    pub fn forget_folder(&self, root: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "forget_folder", "root": root }))
    }

    /// Stops watching a folder for changes; answered on `on_scan`.
    pub fn unwatch_folder(&self, root: String) -> Result<(), String> {
        self.command(&serde_json::json!({ "command": "unwatch_folder", "root": root }))
    }

    /// Reconciles every watched root this often; `0` turns it off. A
    /// watcher reports only the failures it is told about, and a network
    /// share loses events silently — the walk is the only mechanism that
    /// can see a silent loss.
    pub fn set_reconcile_interval(&self, seconds: u32) -> Result<(), String> {
        self.command(&serde_json::json!({
            "command": "set_reconcile_interval",
            "seconds": seconds as u64,
        }))
    }

    /// The system's folder dialog; `null` when the user cancelled. Runs as
    /// a task, so the interface awaits it.
    /// The system's file dialog for an impulse response; `null` when the user
    /// cancelled. Convolver IRs are audio files, so the picker offers the
    /// formats the engine can read.
    pub async fn pick_ir_file(this: Instance<Self>) -> Option<String> {
        let _ = &this;
        let picked = pick_paths(
            nsh_core::file_dialog::OpenDialog::files().button_label("选择脉冲响应"),
        );
        let picked = picked.await;
        let Ok(Some(paths)) = picked else {
            return None;
        };
        paths.first().map(|path| path.to_string_lossy().into_owned())
    }

    /// Opens the system folder picker dialog; `null` when cancelled.
    pub async fn pick_folder(this: Instance<Self>) -> Option<String> {
        let _ = &this;
        let picked = pick_paths(
            nsh_core::file_dialog::OpenDialog::folders().button_label("选择音乐文件夹"),
        );
        let picked = picked.await;
        let Ok(Some(paths)) = picked else {
            return None;
        };
        paths.first().map(|path| path.to_string_lossy().into_owned())
    }

    /// Shows the system's folder picker and returns the folder with a
    /// security-scoped bookmark of it. A picked folder's path is only valid
    /// until the app is relaunched, so iOS keeps the folder by bookmark and
    /// resolves it each launch (`resolve_folder_bookmark`); the desktop has
    /// no such rule, and `pick_folder` is its own door. `None` when the user
    /// cancelled.
    pub async fn pick_folder_bookmark(
        this: Instance<Self>,
    ) -> Result<Option<PickedFolderInfo>, String> {
        let _ = &this;
        #[cfg(target_os = "ios")]
        {
            let Some(url) = ios::picker::pick_folder()? else {
                return Ok(None);
            };
            let path = url
                .path()
                .map(|path| path.to_string())
                .ok_or_else(|| "E_BOOKMARK: 选中的文件夹没有路径".to_owned())?;
            let label = url
                .lastPathComponent()
                .map(|name| name.to_string())
                .unwrap_or_else(|| path.rsplit('/').next().unwrap_or_default().to_owned());
            let bookmark = ios::bookmarks::create(&url)?;
            Ok(Some(PickedFolderInfo {
                path,
                bookmark,
                label,
            }))
        }
        #[cfg(not(target_os = "ios"))]
        {
            Err("E_BOOKMARK: 当前平台不支持文件夹书签，请用 pickFolder".to_owned())
        }
    }

    /// Resolves a stored bookmark to the folder's path for this launch and
    /// opens the app's access to it. `None` when the bookmark no longer
    /// resolves: the user has to pick the folder again. The desktop stores
    /// paths, not bookmarks, so there the stored value is the path.
    pub fn resolve_folder_bookmark(&self, bookmark: String) -> Result<Option<FolderAccessInfo>, String> {
        #[cfg(target_os = "ios")]
        {
            match ios::bookmarks::resolve(&bookmark) {
                Ok(resolved) => Ok(Some(FolderAccessInfo {
                    path: resolved.path,
                    bookmark: resolved.bookmark,
                    stale: resolved.stale,
                })),
                Err(error) => {
                    // A stale folder is a state the user repairs, not a
                    // failure of the call: the page says 需要重新选择.
                    eprintln!("neiro: 书签解析失败：{error}");
                    Ok(None)
                }
            }
        }
        #[cfg(not(target_os = "ios"))]
        {
            Ok(Some(FolderAccessInfo {
                path: bookmark.clone(),
                bookmark,
                stale: false,
            }))
        }
    }

    /// Stops the app's access to a folder that left the library (iOS); the
    /// desktop keeps none.
    pub fn release_folder_bookmark(&self, bookmark: String) {
        #[cfg(target_os = "ios")]
        ios::bookmarks::release(&bookmark);
        #[cfg(not(target_os = "ios"))]
        let _ = bookmark;
    }

    /// Registers `listener` and posts the cached value, in one place: every
    /// listener with a current value behaves the same.
    fn subscribe<T, Table, Value>(&self, table: Table, value: Value, listener: Callback<T>) -> Stop
    where
        T: CallbackArgs + Clone + 'static,
        Table: for<'a> Fn(&'a mut Inner) -> &'a mut Listeners<T> + 'static,
        Value: Fn(&Inner) -> Option<T> + 'static,
    {
        let inner = self.inner.clone();
        let id = {
            let mut guard = lock(&inner);
            if let Some(current) = value(&guard) {
                listener.post(current);
            }
            table(&mut guard).add(listener)
        };
        Stop::new(move || {
            if let Ok(mut guard) = inner.lock() {
                table(&mut guard).remove(id);
            }
        })
    }

    /// Sends one command to Base; a refused command throws with `E_ENGINE`.
    fn command(&self, json: &serde_json::Value) -> Result<(), String> {
        send_command(&self.engine, json)
    }
}

/// Sends one command to Base through its shared handle. Split out of
/// [`Base::command`] because the interruption handler has no `Base`: it runs on
/// the thread that posted the notification and reaches the engine only through
/// the handle Base left behind.
fn send_command(engine: &Shared, json: &serde_json::Value) -> Result<(), String> {
    let line =
        serde_json::to_string(json).map_err(|e| format!("E_ENGINE: command 无法序列化：{e}"))?;
    let engine = engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
    match engine.as_ref() {
        Some(engine) => engine.command(&line),
        None => Err("E_ENGINE: 引擎已释放".to_owned()),
    }
}

/// Locks the inner state, mapping a poisoned lock to an empty view rather
/// than a panic in a listener's path.
fn lock(inner: &Arc<Mutex<Inner>>) -> std::sync::MutexGuard<'_, Inner> {
    inner.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[cfg(test)]
mod cache_bucket_tests {
    use super::*;

    static CACHE_TEST_SEQ: AtomicUsize = AtomicUsize::new(0);

    fn cache_tree() -> std::path::PathBuf {
        let root = std::env::temp_dir().join(format!(
            "neiro-cache-test-{}-{}-{}",
            std::process::id(),
            CACHE_TEST_SEQ.fetch_add(1, Ordering::SeqCst),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_nanos()
        ));
        let remote = remote_cache_dir_in(&root);
        std::fs::create_dir_all(remote.join("artwork")).unwrap();
        std::fs::create_dir_all(remote.join("catalog/nested")).unwrap();
        std::fs::create_dir_all(local_cover_cache_dir_in(&root)).unwrap();
        std::fs::write(remote.join("song.flac"), b"audio").unwrap();
        std::fs::write(remote.join("artwork/cover.jpg"), b"cover").unwrap();
        std::fs::write(remote.join("catalog/index.json"), b"catalog").unwrap();
        std::fs::write(remote.join("catalog/nested/page.json"), b"page").unwrap();
        std::fs::write(local_cover_cache_dir_in(&root).join("cover.txt"), b"local").unwrap();
        let unrelated = root.join("Neiro").join("unrelated");
        std::fs::create_dir_all(&unrelated).unwrap();
        std::fs::write(unrelated.join("keep.txt"), b"keep").unwrap();
        root
    }

    #[test]
    fn stats_walks_the_known_buckets_only() {
        let root = cache_tree();
        let stats = cache_stats_in(&root);
        assert_eq!(stats.remote_audio_files, 1);
        assert_eq!(stats.remote_audio_bytes, 5);
        assert_eq!(stats.remote_artwork_files, 1);
        assert_eq!(stats.remote_artwork_bytes, 5);
        assert_eq!(stats.remote_catalog_files, 2);
        assert_eq!(stats.remote_catalog_bytes, 11);
        assert_eq!(stats.local_artwork_files, 1);
        assert_eq!(stats.local_artwork_bytes, 5);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn clear_removes_files_and_keeps_directories() {
        let root = cache_tree();
        // The runtime may add its own local cover entries while the Rust
        // suite runs; this test only owns the remote catalog bucket.
        let json = clear_cache_in(&root, "remote-catalog").unwrap();
        assert_eq!(
            remote_cache_dir_in(&root).join("song.flac").exists(),
            true,
            "clearing catalog must keep remote audio"
        );
        assert_eq!(remote_cache_dir_in(&root).join("catalog").exists(), true);
        assert_eq!(remote_cache_dir_in(&root).join("catalog/nested").exists(), true);
        assert_eq!(remote_cache_dir_in(&root).join("catalog/index.json").exists(), false);
        assert_eq!(remote_cache_dir_in(&root).join("catalog/nested/page.json").exists(), false);
        let stats: CacheStats = serde_json::from_str(&json).unwrap();
        assert_eq!(stats.remote_catalog_files, 0);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn clear_all_does_not_touch_unrelated_files_or_links() {
        let root = cache_tree();
        let link = remote_cache_dir_in(&root).join("linked.flac");
        #[cfg(unix)]
        std::os::unix::fs::symlink(remote_cache_dir_in(&root).join("song.flac"), &link).unwrap();
        clear_cache_in(&root, "all").unwrap();
        assert_eq!(remote_cache_dir_in(&root).join("song.flac").exists(), false);
        assert_eq!(remote_cache_dir_in(&root).join("catalog").exists(), true);
        assert_eq!(remote_cache_dir_in(&root).join("artwork").exists(), true);
        assert_eq!(local_cover_cache_dir_in(&root).exists(), true);
        assert_eq!(root.join("Neiro/unrelated/keep.txt").exists(), true);
        #[cfg(unix)]
        assert_eq!(link.symlink_metadata().unwrap().file_type().is_symlink(), true);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn installation_id_is_persisted_and_opaque() {
        let root = cache_tree();
        let first = installation_id_in(&root);
        assert_eq!(first.len(), 32);
        assert!(first
            .chars()
            .all(|c| c.is_ascii_digit() || ('a'..='f').contains(&c)));
        assert_eq!(
            installation_id_in(&root),
            first,
            "a second launch reads the same identity"
        );
        let path = installation_id_path_in(&root);
        assert_eq!(std::fs::read_to_string(&path).unwrap().trim(), first);
        // An emptied file is replaced rather than sent empty.
        std::fs::write(&path, "").unwrap();
        let again = installation_id_in(&root);
        assert_eq!(again.len(), 32);
        assert_ne!(again, "");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn reconciliation_removes_partial_downloads_and_keeps_finished_ones() {
        let root = cache_tree();
        let part = remote_cache_dir_in(&root).join("album.flac.part");
        std::fs::write(&part, b"half").unwrap();
        assert_eq!(remove_part_files_in(&root), 1);
        assert!(!part.exists(), "an interrupted download is not kept");
        // The launch pass never touches a finished download.
        reconcile_remote_cache_in(&root);
        assert!(remote_cache_dir_in(&root).join("song.flac").exists());
        assert!(remote_cache_dir_in(&root).join("artwork/cover.jpg").exists());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn remote_prune_drops_the_oldest_past_the_budget() {
        let root = std::env::temp_dir().join(format!(
            "neiro-remote-prune-{}-{}",
            std::process::id(),
            CACHE_TEST_SEQ.fetch_add(1, Ordering::SeqCst)
        ));
        let dir = remote_cache_dir_in(&root);
        std::fs::create_dir_all(&dir).unwrap();
        for (name, age) in [("a.flac", 0u64), ("b.flac", 1), ("c.flac", 2)] {
            let path = dir.join(name);
            std::fs::write(&path, vec![0u8; 100]).unwrap();
            let handle = std::fs::File::options().write(true).open(&path).unwrap();
            handle
                .set_modified(
                    std::time::SystemTime::UNIX_EPOCH + std::time::Duration::from_secs(1_000 + age),
                )
                .unwrap();
        }
        // The oldest file is this session's own download: it stays. The next
        // oldest pays for the ceiling, so 300 bytes over 150 drops one file.
        remember_session_fetch(&dir.join("a.flac").to_string_lossy());
        assert_eq!(prune_remote_audio_cache_in(&root, 150), 1);
        assert!(dir.join("a.flac").exists(), "a session download is never evicted");
        assert!(!dir.join("b.flac").exists());
        assert!(dir.join("c.flac").exists());
        let _ = std::fs::remove_dir_all(&root);
    }
}

#[cfg(test)]
mod cover_cache_tests {
    use super::*;
    use std::time::{Duration, SystemTime};

    fn temp_dir(name: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("neiro-cover-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).expect("temp dir");
        dir
    }

    #[test]
    fn fnv1a64_matches_the_reference_vectors() {
        assert_eq!(fnv1a64(b""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(fnv1a64(b"a"), 0xaf63_dc4c_8601_ec8c);
    }

    #[test]
    fn key_changes_when_the_file_does() {
        let dir = temp_dir("key");
        let file = dir.join("song.flac");
        std::fs::write(&file, b"first").expect("write");
        let path = file.to_string_lossy().into_owned();
        let first = cover_cache_key(&path).expect("first key");
        std::fs::write(&file, b"second-and-longer").expect("rewrite");
        let second = cover_cache_key(&path).expect("second key");
        assert_ne!(first, second, "a replaced file must get a new key");
        let missing = dir.join("missing");
        assert!(cover_cache_key(missing.to_string_lossy().as_ref()).is_none());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn put_then_get_roundtrips_including_none() {
        let dir = temp_dir("roundtrip");
        let cache = dir.join("cache");
        let file = dir.join("song.flac");
        std::fs::write(&file, b"bytes").expect("write");
        let path = file.to_string_lossy().into_owned();
        assert_eq!(cover_cache_read(&cache, &path), "", "empty before any put");
        cover_cache_write(&cache, &path, "data:image/png;base64,AAAA").expect("put");
        assert_eq!(cover_cache_read(&cache, &path), "data:image/png;base64,AAAA");
        cover_cache_write(&cache, &path, "none").expect("put none");
        assert_eq!(cover_cache_read(&cache, &path), "none");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn prune_drops_the_oldest_first() {
        let dir = temp_dir("prune");
        for (name, age) in [("a", 0u64), ("b", 1), ("c", 2)] {
            let path = dir.join(name);
            std::fs::write(&path, vec![0u8; 100]).expect("write");
            let handle = std::fs::File::options()
                .write(true)
                .open(&path)
                .expect("open");
            handle
                .set_modified(SystemTime::UNIX_EPOCH + Duration::from_secs(1_000 + age))
                .expect("mtime");
        }
        // 300 bytes over a 150 budget: the two oldest go, the newest stays.
        prune_local_cover_cache_to(&dir, 150);
        assert_eq!(std::fs::read_dir(&dir).expect("read").count(), 1);
        assert!(dir.join("c").exists());
        let _ = std::fs::remove_dir_all(&dir);
    }
}

#[cfg(test)]
mod test_tone_tests {
    use super::*;

    #[test]
    fn test_tone_has_a_valid_riff_header_and_byte_count() {
        let file = std::env::temp_dir().join(format!("neiro-tone-{}.wav", std::process::id()));
        write_test_tone(&file).expect("write tone");
        let bytes = std::fs::read(&file).expect("read tone");
        assert_eq!(bytes.len(), 44 + 44100 * 8 * 2 * 2);
        assert_eq!(&bytes[0..4], b"RIFF");
        assert_eq!(&bytes[8..12], b"WAVE");
        assert_eq!(&bytes[12..16], b"fmt ");
        assert_eq!(&bytes[36..40], b"data");
        assert_eq!(u32::from_le_bytes(bytes[4..8].try_into().unwrap()), 44 + 44100 * 8 * 2 * 2 - 8);
        assert_eq!(u32::from_le_bytes(bytes[40..44].try_into().unwrap()), 44100 * 8 * 2 * 2);
        let _ = std::fs::remove_file(&file);
    }
}

#[cfg(test)]
mod event_order_tests {
    use super::*;

    fn inner_with(path: &str, title: &str) -> Inner {
        let mut inner = Inner::default();
        inner.player = Some(PlayerState {
            path: Some(path.into()),
            title: title.into(),
            artist: "old artist".into(),
            album: "old album".into(),
            ..Default::default()
        });
        inner
    }

    #[test]
    fn position_for_a_new_path_clears_the_previous_tags() {
        // `position` can be the first word of a new track; it must not leave
        // the previous title standing beside the new cover.
        let mut inner = inner_with("A", "A title");
        inner.deliver(r#"{"event":"position","path":"B","position":0.0,"duration":180.0}"#);
        let player = inner.player.as_ref().expect("player");
        assert_eq!(player.path.as_deref(), Some("B"));
        assert_eq!(player.title, "", "the previous title must not survive");
        assert_eq!(player.artist, "");
        assert_eq!(player.album, "");
        assert_eq!(player.track_serial, 1.0);
    }

    #[test]
    fn preloaded_analysis_applies_when_its_track_takes_over() {
        // The engine analyses the next track while the previous still plays:
        // that answer must be kept, not dropped, or the new title never comes.
        let mut inner = inner_with("A", "A title");
        inner.deliver(
            r#"{"event":"analysis_ready","path":"B","analysis":{"title":"B title","artist":"B artist","album":"B album"},"cached":false}"#,
        );
        assert_eq!(
            inner.player.as_ref().unwrap().title,
            "A title",
            "a preloaded track is not current yet"
        );
        assert!(inner.pending_analysis.is_some());

        inner.deliver(r#"{"event":"track_changed","path":"B","render_path":{}}"#);
        let player = inner.player.as_ref().unwrap();
        assert_eq!(player.path.as_deref(), Some("B"));
        assert_eq!(player.title, "B title");
        assert_eq!(player.artist, "B artist");
        assert_eq!(player.album, "B album");
        assert!(inner.pending_analysis.is_none(), "used once");
    }

    #[test]
    fn analysis_for_the_current_path_applies_at_once() {
        let mut inner = inner_with("B", "B title");
        inner.deliver(
            r#"{"event":"analysis_ready","path":"B","analysis":{"title":"B title 2","artist":"B artist","album":"B album"},"cached":false}"#,
        );
        let player = inner.player.as_ref().unwrap();
        assert_eq!(player.title, "B title 2");
        assert_eq!(player.artist, "B artist");
    }

    #[test]
    fn a_snapshot_for_the_same_track_does_not_revert_its_tags() {
        let mut inner = inner_with("B", "B title");
        inner.deliver(
            r#"{"event":"state","path":"B","analysis":{"title":"A title","artist":"A artist","album":"A album"}}"#,
        );
        let player = inner.player.as_ref().unwrap();
        assert_eq!(player.title, "B title");
        assert_eq!(player.artist, "old artist");
    }

    #[test]
    fn a_snapshot_for_a_new_path_takes_its_tags() {
        let mut inner = inner_with("A", "A title");
        inner.deliver(
            r#"{"event":"state","path":"B","analysis":{"title":"B title","artist":"B artist","album":"B album"}}"#,
        );
        let player = inner.player.as_ref().unwrap();
        assert_eq!(player.path.as_deref(), Some("B"));
        assert_eq!(player.title, "B title");
        assert_eq!(player.artist, "B artist");
    }

    #[test]
    fn a_stale_snapshot_does_not_put_the_previous_title_back() {
        // The reported case: the new track's position and analysis land, then
        // a snapshot still carrying the previous analysis arrives.
        let mut inner = inner_with("A", "A title");
        inner.deliver(r#"{"event":"position","path":"B","position":0.0,"duration":257.0}"#);
        inner.deliver(
            r#"{"event":"analysis_ready","path":"B","analysis":{"title":"Butterfly","artist":"木村カエラ","album":"5years"},"cached":false}"#,
        );
        inner.deliver(
            r#"{"event":"state","path":"B","analysis":{"title":"幸福了 然后呢","artist":"A-Lin","album":"幸福了 然后呢"}}"#,
        );
        let player = inner.player.as_ref().unwrap();
        assert_eq!(player.title, "Butterfly");
        assert_eq!(player.artist, "木村カエラ");
        assert_eq!(player.album, "5years");
    }
}
