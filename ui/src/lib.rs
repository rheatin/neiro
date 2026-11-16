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

use std::sync::{Arc, Mutex};

use nsh_core::element::CallbackArgs;
use nsh_core::prelude::*;
use serde::Deserialize;

/// [`Engine`]: ../src/engine.rs.html
pub mod engine;

use crate::engine::{Engine, RenderPath, Snapshot, WireEvent};

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

/// LRCLIB's search: the provider that needs no key and returns both timed and
/// untimed words for the same song.
///
/// A miss is an empty list. The interface asks this once per press, so a
/// provider that is down costs a delay and no error state.
fn fetch_lyrics(
    artist: String,
    title: String,
    album: Option<String>,
    duration_s: Option<f64>,
) -> Vec<LyricsCandidate> {
    let mut url = String::from("https://lrclib.net/api/search?track_name=");
    url.push_str(&percent(&title));
    if !artist.trim().is_empty() {
        url.push_str("&artist_name=");
        url.push_str(&percent(&artist));
    }
    if let Some(album) = album.as_deref().filter(|album| !album.trim().is_empty()) {
        url.push_str("&album_name=");
        url.push_str(&percent(album));
    }
    let response = ureq::get(&url)
        .header("User-Agent", "Neiro/0.1 (https://github.com/rheatin/neiro)")
        .header("Accept", "application/json")
        .call();
    let Ok(mut response) = response else {
        return Vec::new();
    };
    let Ok(body) = response.body_mut().read_to_string() else {
        return Vec::new();
    };
    let Ok(rows) = serde_json::from_str::<Vec<serde_json::Value>>(&body) else {
        return Vec::new();
    };
    rows.iter()
        .filter_map(|row| {
            let text = |key: &str| row.get(key).and_then(|v| v.as_str()).map(str::to_string);
            let duration = row.get("duration").and_then(|v| v.as_f64()).unwrap_or(0.0);
            // The caller's length is a hint the row shows, not a filter: a
            // live version or a remaster differs by minutes and still carries
            // the words. `duration_s` stays in the signature for that display.
            let _ = duration_s;
            let synced = text("syncedLyrics").filter(|t| !t.trim().is_empty());
            let plain = text("plainLyrics").filter(|t| !t.trim().is_empty());
            if synced.is_none() && plain.is_none() {
                return None;
            }
            Some(LyricsCandidate {
                id: row.get("id").and_then(|v| v.as_f64()).unwrap_or(0.0),
                track: text("trackName").unwrap_or_default(),
                artist: text("artistName").unwrap_or_default(),
                album: text("albumName").unwrap_or_default(),
                duration_s: duration,
                synced,
                plain,
            })
        })
        .take(10)
        .collect()
}

/// Percent-encoding for a query value. Only the unreserved set survives: a
/// Japanese title is UTF-8 bytes, and an unencoded `&` would split the query.
fn percent(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for byte in text.as_bytes() {
        match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => {
                out.push(*byte as char)
            }
            _ => out.push_str(&format!("%{byte:02X}")),
        }
    }
    out
}

/// The state the listeners last heard. Every field has one owner, here.
#[derive(Default)]
struct Inner {
    player: Option<PlayerState>,
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
                if let Some(player) = self.player.as_mut() {
                    // A new stream: the previous track's tags are wrong now.
                    if player.path.as_deref() != Some(path.as_str()) {
                        player.title = String::new();
                        player.artist = String::new();
                        player.album = String::new();
                        player.bitrate_kbps = None;
                        player.codec = String::new();
                    player.codec_profile = String::new();
                    }
                    player.path = Some(path);
                    player.track_serial += 1.0;
                }
                self.set_render_path(render_path);
            }
            WireEvent::Position { path, position, duration } => {
                let player = self.player.get_or_insert_with(PlayerState::default);
                if player.path.as_deref() != Some(path.as_str()) {
                    player.path = Some(path);
                }
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
            WireEvent::QueueChanged { queue, index } => {
                let info = QueueInfo {
                    queue,
                    index: index.map(|i| i as f64),
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
                let player = self.player.get_or_insert_with(PlayerState::default);
                if player.path.as_deref() == Some(path.as_str()) {
                    player.title = analysis.title.clone();
                    player.artist = analysis.artist.clone();
                    player.album = analysis.album.clone();
                    player.bitrate_kbps = Some(analysis.bitrate_kbps as f64);
                    player.codec = analysis.codec.clone();
                    player.codec_profile = analysis.codec_profile.clone();
                    self.on_player.post(player);
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
                self.on_remote_fetched
                    .post(&RemoteFetched { track: remote_track_of(track), path });
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
        let resolved_json = snapshot
            .resolved
            .as_ref()
            .map(|resolved| serde_json::to_string(resolved).expect("serializable JSON"));
        let player = PlayerState {
            state: snapshot.state.clone(),
            path: snapshot.path.clone(),
            title: analysis.title.clone(),
            artist: analysis.artist.clone(),
            album: analysis.album.clone(),
            position_ms: snapshot.position * 1000.0,
            duration_ms: snapshot.duration * 1000.0,
            at: now_ms(),
            volume: snapshot.render_path.as_ref().map(|path| path.volume),
            bitrate_kbps: snapshot.analysis.as_ref().map(|a| a.bitrate_kbps as f64),
            codec: analysis.codec.clone(),
            codec_profile: analysis.codec_profile.clone(),
            track_serial: snapshot.track_serial as f64,
            resolved_json: resolved_json.clone(),
        };
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
        if !snapshot.queue.is_empty() {
            let info = QueueInfo {
                queue: snapshot.queue.clone(),
                index: snapshot.queue_index.map(|index| index as f64),
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
                if let Ok(mut guard) = inner.lock() {
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
async fn pump(engine: Shared, receiver: std::sync::mpsc::Receiver<String>, inner: Arc<Mutex<Inner>>) {
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
        if engine.lock().map(|guard| guard.is_none()).unwrap_or(true) {
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
        let (engine, receiver) = Engine::open(&library)?;
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
            pump(pump_engine, receiver, pump_inner).await;
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
        self.command(&serde_json::json!({
            "command": "set_remote",
            "cache_dir": remote_cache_dir(),
            "server": {
                "kind": kind,
                "params": params,
            },
        }))
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
    /// Ask the online lyrics provider for this track's words.
    ///
    /// The request runs on a thread of its own and answers through the
    /// channel `lyrics_pump` drains: it blocks for as long as the provider
    /// takes, and the interface must keep painting while it does.
    pub fn search_lyrics(
        &self,
        artist: String,
        title: String,
        album: Option<String>,
        duration_s: Option<f64>,
    ) -> Result<(), String> {
        if title.trim().is_empty() {
            return Err("E_LYRICS: 这首歌没有标题，无法搜索。".into());
        }
        let sender = LYRICS_RESULTS
            .get_or_init(|| std::sync::Mutex::new(None))
            .lock()
            .ok()
            .and_then(|guard| guard.clone());
        std::thread::spawn(move || {
            let candidates = fetch_lyrics(artist, title, album, duration_s);
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

    // ---- helpers -----------------------------------------------------------

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
        let line =
            serde_json::to_string(json).map_err(|e| format!("E_ENGINE: command 无法序列化：{e}"))?;
        let engine = self.engine.lock().map_err(|_| "E_ENGINE: 引擎已释放".to_owned())?;
        match engine.as_ref() {
            Some(engine) => engine.command(&line),
            None => Err("E_ENGINE: 引擎已释放".to_owned()),
        }
    }
}

/// Locks the inner state, mapping a poisoned lock to an empty view rather
/// than a panic in a listener's path.
fn lock(inner: &Arc<Mutex<Inner>>) -> std::sync::MutexGuard<'_, Inner> {
    inner.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}
