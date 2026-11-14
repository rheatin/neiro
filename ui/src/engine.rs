//! Base, reached through its C ABI (`../Neiro/base/include/nae_base.h`).
//!
//! The library is opened at run time, never linked: `neiro_base.dll` beside
//! this binary on Windows, `libneiro_base.dylib` on macOS, or the same pair
//! under `../Neiro/base/target/release/` when this UI runs out of the engine
//! checkout. It imports the engine's own audio library (`nae.dll`,
//! `libnae.dylib`), which the platform resolves from the loaded library's own
//! directory — the Windows flag below makes that explicit, macOS reads it
//! from the dylib's `@loader_path` rpath — so neither library is copied or
//! put on a PATH. A machine without the engine still builds and runs this
//! UI; [`Engine::open`] reports `E_ENGINE` and the interface says the engine
//! is not connected instead of drawing plausible values.
//!
//! Commands and events are the serialized types of `base/src/protocol.rs`:
//! commands tagged `"command"`, events tagged `"event"`, both snake_case.
//! Commands answer nothing on the wire (`NAE_BaseCommand` only queues —
//! `0` for queued, never a result), so every data request is
//! fire-and-forget: the answer is an event, the store paints from events.

use std::ffi::{c_char, c_void, CStr, CString};
use std::path::{Path, PathBuf};

use libloading::{Library, Symbol};
use serde::Deserialize;

/// The engine's Base library beside the binary, named the way the platform
/// loads it.
#[cfg(windows)]
const DLL: &str = "neiro_base.dll";
#[cfg(target_os = "macos")]
const DLL: &str = "libneiro_base.dylib";
#[cfg(all(unix, not(target_os = "macos")))]
const DLL: &str = "libneiro_base.so";

type StartWithLibraryFn = unsafe extern "C" fn(*const c_char) -> *mut c_void;
type CommandFn = unsafe extern "C" fn(*mut c_void, *const c_char) -> i32;
type EventFn = extern "C" fn(*const c_char, *mut c_void);
type SubscribeFn = unsafe extern "C" fn(*mut c_void, Option<EventFn>, *mut c_void) -> i32;
type SnapshotFn = unsafe extern "C" fn(*mut c_void) -> *mut c_char;
type FreeStringFn = unsafe extern "C" fn(*mut c_char);
type UnsubscribeFn = unsafe extern "C" fn(*mut c_void) -> i32;
type ShutdownFn = unsafe extern "C" fn(*mut c_void);

/// A running Base. Dropping it unsubscribes (so no callback ever runs on
/// freed memory), shuts Base down and releases the DLL.
pub struct Engine {
    base: *mut c_void,
    command: CommandFn,
    unsubscribe: UnsubscribeFn,
    shutdown: ShutdownFn,
    snapshot: SnapshotFn,
    free_string: FreeStringFn,
    /// The subscription's context; freed in `Drop`, after `unsubscribe`.
    ctx: *mut Ctx,
    _library: Library,
}

// The pointer is process-owned state; nothing inside it is thread-local, and
// one instance is the whole process's engine.
unsafe impl Send for Engine {}

/// What the event callback carries; freed only after the unsubscribe that
/// guarantees no further callback.
struct Ctx {
    events: std::sync::mpsc::Sender<String>,
}

/// Base's event callback runs on Base's own thread and only forwards the raw
/// JSON line; parsing and state ownership happen on the pump's side.
extern "C" fn forward(json: *const c_char, user: *mut c_void) {
    let ctx = unsafe { &*(user as *const Ctx) };
    let text = unsafe { CStr::from_ptr(json) }.to_string_lossy().into_owned();
    let _ = ctx.events.send(text);
}

/// Where `neiro_base.dll` is looked for, in order: the environment's pointer
/// to one copy, then beside this binary, then in the engine checkout's target
/// directory (which also carries `nae.dll`, the loaded DLL's own import).
fn dll_candidates(exe_dir: Option<&Path>) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    if let Some(path) = std::env::var_os("NEIRO_BASE_DLL") {
        paths.push(PathBuf::from(path));
    }
    if let Some(dir) = exe_dir {
        paths.push(dir.join(DLL));
    }
    // Two layouts, because this UI lives in two places: beside the engine's
    // own checkout (`../Neiro/base`) in its own repository, and one directory
    // under the monorepo root (`../base` or `../../base`) when it ships inside
    // the engine's tree.
    for root in ["../Neiro/base", "../base", "../../base"] {
        paths.push(PathBuf::from(root).join("target/release").join(DLL));
    }
    paths
}

/// Everything between "the DLL is loaded" and "the events are subscribed":
/// the started Base and the exports around it. Dropping it prematurely is
/// not a path — [`Prepared::finish`] either succeeds fully or tears Base
/// down itself.
struct Prepared {
    base: *mut c_void,
    command: CommandFn,
    subscribe: SubscribeFn,
    unsubscribe: UnsubscribeFn,
    snapshot: SnapshotFn,
    free_string: FreeStringFn,
    shutdown: ShutdownFn,
    library: Library,
}

/// The exports this UI calls, resolved once by [`Engine::resolve_all`].
///
/// [`Engine::resolve_all`]: Engine::load
struct Exports {
    start_with_library: StartWithLibraryFn,
    command: CommandFn,
    subscribe: SubscribeFn,
    unsubscribe: UnsubscribeFn,
    snapshot: SnapshotFn,
    free_string: FreeStringFn,
    shutdown: ShutdownFn,
}

impl Engine {
    /// Opens the DLL, starts Base with its SQLite library at `library`, and
    /// subscribes. Every event arrives afterwards as its raw JSON line on
    /// the returned receiver; the channel stays open until the engine is
    /// dropped.
    pub fn open(
        library: &Path,
    ) -> Result<(Engine, std::sync::mpsc::Receiver<String>), String> {
        let exe_dir = std::env::current_exe()
            .ok()
            .and_then(|path| path.parent().map(Path::to_path_buf));
        let mut tried = Vec::new();
        for candidate in dll_candidates(exe_dir.as_deref()) {
            if !candidate.is_file() {
                tried.push(format!("{}（没有文件）", candidate.display()));
                continue;
            }
            let reason = match Self::load(&candidate, library) {
                Ok(prepared) => match prepared.finish() {
                    Ok((engine, receiver)) => return Ok((engine, receiver)),
                    Err(reason) => reason,
                },
                Err(reason) => reason,
            };
            tried.push(format!("{}：{reason}", candidate.display()));
        }
        Err(format!(
            "E_ENGINE: 无法打开 {DLL}。试过的位置：{}。需要先构建 ../Neiro/base。",
            tried.join("；")
        ))
    }

    /// Loads the exports and starts Base. Nothing subscribes yet: without an
    /// event channel a queued command loses its answer.
    fn load(path: &Path, library: &Path) -> Result<Prepared, String> {
        // Windows: `LOAD_WITH_ALTERED_SEARCH_PATH` makes the loaded module's
        // own directory the search root for its dependencies — `nae.dll`
        // sits beside `neiro_base.dll`, and the FFmpeg DLLs beside `nae.dll`.
        // The plain flags would look in the host's directory instead and fail
        // a DLL chain that provably works there (`base_capi_check.exe`).
        //
        // Elsewhere `dlopen` resolves the same way: macOS through the
        // dylib's `@loader_path` rpath, Linux through `$ORIGIN`.
        #[cfg(windows)]
        let library_file = {
            let loaded = unsafe {
                libloading::os::windows::Library::load_with_flags(
                    path,
                    libloading::os::windows::LOAD_WITH_ALTERED_SEARCH_PATH,
                )
            }
            .map_err(|e| format!("加载失败：{e}"))?;
            libloading::Library::from(loaded)
        };
        #[cfg(not(windows))]
        let library_file =
            unsafe { libloading::Library::new(path) }.map_err(|e| format!("加载失败：{e}"))?;
        let exports = Exports::resolve(&library_file)?;
        let library_c =
            CString::new(library.to_string_lossy().as_ref()).expect("路径里没有 NUL");
        let base =
            unsafe { (exports.start_with_library)(library_c.as_ptr() as *const c_char) };
        if base.is_null() {
            // Base was never started, so there is nothing to shut down; the
            // DLL unloads when this error returns.
            return Err("Base 无法启动（worker 线程或 library 打不开）".into());
        }
        Ok(Prepared {
            base,
            command: exports.command,
            subscribe: exports.subscribe,
            unsubscribe: exports.unsubscribe,
            snapshot: exports.snapshot,
            free_string: exports.free_string,
            shutdown: exports.shutdown,
            library: library_file,
        })
    }

    /// Sends one serialized command. `0` means queued; the state change
    /// arrives later as an event, so callers never assert on fresh state.
    pub fn command(&self, json: &str) -> Result<(), String> {
        let text = CString::new(json).map_err(|e| format!("E_ENGINE: {e}"))?;
        let code = unsafe { (self.command)(self.base, text.as_ptr() as *const c_char) };
        if code == 0 {
            Ok(())
        } else {
            Err(format!("E_ENGINE: command 被拒绝（{code}）"))
        }
    }

    /// One read of the whole point-in-time state. The events remain the
    /// authority while the app runs; this exists for a late subscriber and
    /// for checks.
    pub fn snapshot(&self) -> Result<Snapshot, String> {
        let raw = unsafe { (self.snapshot)(self.base) };
        if raw.is_null() {
            return Err("E_ENGINE: snapshot 为空".into());
        }
        let text = unsafe { CStr::from_ptr(raw) }.to_string_lossy().into_owned();
        unsafe { (self.free_string)(raw) };
        serde_json::from_str(&text).map_err(|e| format!("E_ENGINE: snapshot 无法解析：{e}"))
    }
}

impl Prepared {
    /// Subscribes the started Base to its events and yields the engine. On
    /// error the context is freed and Base shut down: a Base without events
    /// is useless here, and a half-subscribed one would queue commands
    /// nobody answers.
    fn finish(self) -> Result<(Engine, std::sync::mpsc::Receiver<String>), String> {
        let (sender, receiver) = std::sync::mpsc::channel::<String>();
        let ctx = Box::into_raw(Box::new(Ctx { events: sender }));
        let code =
            unsafe { (self.subscribe)(self.base, Some(forward), ctx as *mut c_void) };
        if code != 0 {
            unsafe {
                drop(Box::from_raw(ctx));
                (self.shutdown)(self.base);
            }
            return Err(format!("E_ENGINE: 订阅 Base 的事件失败（{code}）"));
        }
        Ok((
            Engine {
                base: self.base,
                command: self.command,
                unsubscribe: self.unsubscribe,
                shutdown: self.shutdown,
                snapshot: self.snapshot,
                free_string: self.free_string,
                ctx,
                _library: self.library,
            },
            receiver,
        ))
    }
}

impl Exports {
    /// Resolves the exports in one place so a missing symbol names itself.
    fn resolve(library: &Library) -> Result<Exports, String> {
        let read = |name: &[u8], printed: &str| -> Result<String, String> {
            let _ = (name, printed);
            Ok(String::new())
        };
        let _ = read;
        Ok(Exports {
            start_with_library: symbol::<StartWithLibraryFn>(
                library,
                b"NAE_BaseStartWithLibrary\0",
                "NAE_BaseStartWithLibrary",
            )?,
            command: symbol::<CommandFn>(library, b"NAE_BaseCommand\0", "NAE_BaseCommand")?,
            subscribe: symbol::<SubscribeFn>(
                library,
                b"NAE_BaseSubscribe\0",
                "NAE_BaseSubscribe",
            )?,
            unsubscribe: symbol::<UnsubscribeFn>(
                library,
                b"NAE_BaseUnsubscribe\0",
                "NAE_BaseUnsubscribe",
            )?,
            snapshot: symbol::<SnapshotFn>(library, b"NAE_BaseSnapshot\0", "NAE_BaseSnapshot")?,
            free_string: symbol::<FreeStringFn>(
                library,
                b"NAE_BaseFreeString\0",
                "NAE_BaseFreeString",
            )?,
            shutdown: symbol::<ShutdownFn>(library, b"NAE_BaseShutdown\0", "NAE_BaseShutdown")?,
        })
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        unsafe {
            // The unsubscribe guarantees the callback never runs again, so
            // freeing the context afterwards is safe; `-4` only says there
            // was nothing to drop.
            (self.unsubscribe)(self.base);
            (self.shutdown)(self.base);
            if !self.ctx.is_null() {
                drop(Box::from_raw(self.ctx));
            }
        }
    }
}

/// One export by its C name; the error carries the name, not a raw OS code.
fn symbol<T: Copy>(library: &Library, name: &[u8], printed: &str) -> Result<T, String> {
    let resolved: Symbol<'_, T> =
        unsafe { library.get(name) }.map_err(|e| format!("缺少符号 {printed}：{e}"))?;
    Ok(*resolved)
}

// ------------------------------------------------------------------ the wire

/// One row of the browsing index (`TrackRow` in `protocol.rs`). The fields
/// are the file's own tags plus what the engine's decode knows; the index is
/// a cache only, and a misreported file is a fact about the file.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct TrackRow {
    pub path: String,
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

/// One album, as the index groups them (an album is what the tags put
/// together; Base does not invent a main-artist rule).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct AlbumSummary {
    pub album: String,
    pub album_artist: String,
    pub year: i64,
    pub track_count: i64,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct ArtistSummary {
    pub artist: String,
    pub track_count: i64,
    pub album_count: i64,
}

/// One embedded picture's description, without bytes.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct ArtworkInfo {
    pub role: String,
    pub mime: Option<String>,
    pub size: i64,
}

/// One output endpoint, as the engine enumerated it.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Device {
    pub id: String,
    pub name: String,
    pub sample_rate: u32,
    pub bits_per_sample: u32,
    pub valid_bits: u32,
    pub channels: u32,
    pub exclusive: bool,
    pub shared: bool,
    /// The rates this endpoint opens, ascending, as Base measured by opening
    /// it. Empty is "not probed", never "supports nothing".
    #[serde(default)]
    pub supported_sample_rates: Vec<u32>,
}

/// What the engine actually did with this stream — never what was requested.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
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
    /// The engine's `NAE_REASON_*`: 0 when bit-perfect, otherwise why not.
    pub reason: i32,
    pub latency_ms: f64,
    pub eq_enabled: bool,
    pub crossfeed_enabled: bool,
    pub replay_gain_enabled: bool,
    pub convolver_enabled: bool,
    pub crossfade_enabled: bool,
    pub volume: f64,
}

/// The applied DSP parameters, read back from the session.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Settings {
    pub volume: f64,
    pub eq: Eq,
    pub crossfeed: Crossfeed,
    pub replay_gain: ReplayGain,
    pub convolver: Convolver,
    pub crossfade: Crossfade,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Eq {
    pub enabled: bool,
    pub preamp_db: f64,
    pub bands: Vec<EqBand>,
}

/// One EQ band, exactly as the wire spells it (`freq_hz`, not `freq`).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct EqBand {
    pub kind: i32,
    pub freq_hz: f64,
    pub gain_db: f64,
    pub q: f64,
    pub enabled: bool,
    pub channel_mask: u32,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Crossfeed {
    pub enabled: bool,
    pub amount: f64,
    pub delay_ms: f64,
    pub cutoff_hz: f64,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct ReplayGain {
    /// `0` off, `1` track, `2` album — the engine's own `RG_*` values.
    pub mode: i32,
    pub preamp_db: f64,
    pub prevent_clipping: bool,
    pub fallback_db: f64,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Convolver {
    pub enabled: bool,
    pub ir_path: String,
    pub wet: f64,
    pub dry: f64,
    pub gain_db: f64,
    pub normalize: bool,
}

#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Crossfade {
    pub enabled: bool,
    pub duration_ms: f64,
    /// The engine's `CROSSFADE_LINEAR` (0) / `CROSSFADE_EQUAL_POWER` (1).
    pub curve: i32,
}

/// `analysis_ready`'s facts: the engine's own decode. Tags among them are
/// where title, artist and album come from; how high the number `bpm` or
/// `loudness` may be shown depends on `bpm_valid` and `lufs_valid`, never on
/// plausibility.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Analysis {
    pub analyzer_version: u32,
    pub complete: bool,
    pub channels: u32,
    pub sample_rate: u32,
    pub bits_per_sample: u32,
    pub duration_ms: u32,
    pub codec: String,
    pub codec_profile: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub album_artist: String,
    pub track: String,
    pub disc: String,
    pub year: String,
    pub genre: String,
    pub composer: String,
    pub bitrate_kbps: u32,
    pub bitrate_from_tag: bool,
    pub rg_track_valid: bool,
    pub rg_track_gain_db: f64,
    pub rg_album_valid: bool,
    pub rg_album_gain_db: f64,
    pub loudness_db: f64,
    pub dynamic_range_db: f64,
    pub bpm: f64,
    pub bpm_valid: bool,
    pub lufs_integrated: f64,
    pub lufs_valid: bool,
    pub lufs_range: f64,
    pub peaks: Vec<f32>,
    pub error: i32,
}

/// A complete, point-in-time view for a late subscriber.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Snapshot {
    pub state: String,
    pub path: Option<String>,
    pub device_id: Option<String>,
    pub devices: Vec<Device>,
    pub position: f64,
    pub duration: f64,
    pub track_serial: u64,
    pub render_path: Option<RenderPath>,
    pub queue: Vec<String>,
    pub queue_index: Option<usize>,
    pub analysis: Option<Analysis>,
    pub settings: Option<Settings>,
    /// The engine's resolved path for this track, as a diagnostics JSON of
    /// its own contract (`NAE_ResolvedPath`), refreshed on open and on a
    /// gapless track change — carried through, never parsed twice.
    pub resolved: Option<serde_json::Value>,
}

/// One server playlist (`RemotePlaylist` in `remote.rs`).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct RemotePlaylistRow {
    pub id: String,
    pub name: String,
    pub track_count: u64,
    pub duration_ms: u64,
}

/// One item in a server's audio listing (`RemoteTrack` in `remote.rs`).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct RemoteTrackRow {
    pub id: String,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub container: String,
    pub duration_ms: u64,
    pub size: Option<u64>,
    pub cover_art: Option<String>,
}

/// One playlist, from the library file (`Playlist` in `library.rs`).
/// `position` is sidebar order assigned at creation and never reused.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct PlaylistRow {
    pub id: i64,
    pub name: String,
    pub track_count: i64,
}

/// One entry in a playlist: a path and its dense 0-based position.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct PlaylistEntryRow {
    pub path: String,
    pub position: i64,
}

/// One track and how often it was played (`PlayedTrack`).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct PlayedRow {
    pub path: String,
    pub play_count: i64,
}

/// One visualiser frame, at the interval the host asked for (`SetVisualiser`).
/// Only emitted while playing and only while a host subscribed; the bins are
/// linear magnitudes (a full-scale sine reads about 1.0 in its own bin).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct VisualFrame {
    pub bins: Vec<f32>,
    pub fft_size: u32,
    /// The device rate the window was captured at.
    pub sample_rate: u32,
    pub peak: f64,
    /// Per-channel peaks. `None` from a Base that does not report them yet,
    /// which the panel draws as one mono ladder rather than two invented ones.
    #[serde(default)]
    pub peak_l: Option<f64>,
    #[serde(default)]
    pub peak_r: Option<f64>,
    pub rms: f64,
    /// Total frames the tap has been fed, so a UI can tell new frames from
    /// repeats by comparing.
    pub frames_fed: u64,
    /// The time-domain envelope, oldest first; its length is what was written,
    /// not the ring's capacity.
    pub envelope: Vec<f32>,
    pub envelope_hop_ms: f64,
}

/// What Base observed and this UI consumes. Variants are named exactly as
/// Base serializes them (`tag = "event"`, `rename_all = "snake_case"`). A
/// variant Base adds later makes that one line fail to parse; the pump
/// counts the unparsed line and drops it — never guesses.
///
/// Some variants are parsed and then explicitly ignored (`_ => {}` in the
/// pump): their facts have no listener in this UI yet, and drawing a value
/// nobody reported would be a plausible default, the thing this crate never
/// does.
#[allow(dead_code)]
#[derive(Debug, Clone, Deserialize)]
#[serde(tag = "event", rename_all = "snake_case")]
pub enum WireEvent {
    /// Carries the whole snapshot, so a late subscriber paints it whole.
    State(Snapshot),
    TrackChanged {
        path: String,
        render_path: RenderPath,
    },
    Position {
        path: String,
        position: f64,
        duration: f64,
    },
    RenderPathChanged {
        path: String,
        render_path: RenderPath,
    },
    PolicyChanged {
        policy: serde_json::Value,
    },
    DevicesChanged {
        devices: Vec<Device>,
    },
    SettingsChanged {
        settings: Settings,
    },
    ResolvedPathChanged {
        resolved: serde_json::Value,
    },
    AnalysisReady {
        path: String,
        analysis: Analysis,
        cached: bool,
    },
    /// One visualiser frame, at the rate the host asked for.
    VisualFrame {
        frame: VisualFrame,
    },
    QueueChanged {
        queue: Vec<String>,
        index: Option<usize>,
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
    /// A picture listing, or one picture's bytes when `index` named it.
    ArtworkRead {
        path: String,
        pictures: Vec<ArtworkInfo>,
        index: Option<usize>,
        data: Option<String>,
    },
    ArtworkChanged {
        path: String,
        pictures: Vec<ArtworkInfo>,
    },
    FavouritesChanged {
        paths: Vec<String>,
    },
    PlaylistsChanged {
        playlists: Vec<PlaylistRow>,
    },
    PlaylistEntriesChanged {
        id: i64,
        entries: Vec<PlaylistEntryRow>,
    },
    MostPlayedChanged {
        tracks: Vec<PlayedRow>,
    },
    /// The remote session, after `set_remote`. Never carries the token.
    RemoteChanged {
        configured: bool,
        kind: Option<String>,
        base_url: Option<String>,
    },
    /// The server's audio items, after `list_remote`.
    RemoteListed {
        tracks: Vec<RemoteTrackRow>,
    },
    /// The outcome of `probe_remote`.
    RemoteProbed {
        #[serde(default)]
        ok: bool,
        #[serde(default)]
        message: String,
        #[serde(default)]
        count: usize,
    },
    /// One item is now a local file; `path` goes to `play`.
    RemoteFetched {
        track: RemoteTrackRow,
        path: String,
    },
    /// A track's cover, base64. Absent `data` means the server has no
    /// picture, which is a normal answer and not a failure.
    RemoteArtwork {
        id: String,
        mime: Option<String>,
        data: Option<String>,
    },
    RemotePlaylists {
        playlists: Vec<RemotePlaylistRow>,
    },
    RemotePlaylistItems {
        id: String,
        tracks: Vec<RemoteTrackRow>,
    },
    ScanStarted {
        root: String,
    },
    ScanDone {
        root: String,
        added: Vec<String>,
        modified: Vec<String>,
        removed: Vec<String>,
        moved: Vec<serde_json::Value>,
    },
    FolderWatched {
        root: String,
        roots: Vec<String>,
    },
    FilesChanged {
        #[serde(default)]
        changes: Vec<serde_json::Value>,
    },
    TagsIndexed {
        done: usize,
        total: usize,
        complete: bool,
    },
    /// A file's tag fields, after `read_tags`. Each field carries the family
    /// it came from: two families can disagree about one key, and a reader
    /// that needs the lyric wants to know which one said it.
    TagsRead {
        path: String,
        fields: Vec<TagField>,
    },
    Error {
        op: String,
        message: String,
    },
    Shutdown,
}

/// One tag field, as Base reports it.
#[derive(Debug, Clone, Default, PartialEq, Deserialize)]
#[serde(default)]
pub struct TagField {
    pub family: String,
    /// The `ItemKey` name: `TrackTitle`, `Lyrics`, `Bpm`.
    pub key: String,
    /// The name the family gives this field, when it has one.
    pub name: String,
    /// `None` for a non-text item (an embedded picture); text fields carry
    /// their string here.
    pub value: Option<String>,
}

/// Parses one event line. `Err` for a line the UI does not know yet (Base
/// may add events; the pump counts them and drops them).
pub fn parse(line: &str) -> Result<WireEvent, String> {
    serde_json::from_str::<WireEvent>(line).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The two events the player bar lives on, parsed exactly as Base
    /// serializes them (`base/src/protocol.rs`).
    #[test]
    fn parses_position_and_tracks_listed() {
        let event = parse(
            r#"{"event":"position","path":"D:/Music/a.flac","position":1.5,"duration":180.0}"#,
        )
        .expect("position parses");
        match event {
            WireEvent::Position { path, position, duration } => {
                assert_eq!(path, "D:/Music/a.flac");
                assert!((position - 1.5).abs() < f64::EPSILON);
                assert!((duration - 180.0).abs() < f64::EPSILON);
            }
            other => panic!("wrong variant: {other:?}"),
        }

        let event = parse(
            r#"{"event":"tracks_listed","tracks":[{"path":"p","title":"A","artist":"B",
                "album":"C","album_artist":"B","disc_no":1,"track_no":1,"year":1999,
                "genre":"","duration_ms":1000,"sample_rate":44100,"bits":16,"channels":2}],
                "total":1,"offset":0}"#,
        )
        .expect("tracks_listed parses");
        match event {
            WireEvent::TracksListed { tracks, total, offset } => {
                assert_eq!(tracks.len(), 1);
                assert_eq!((total, offset), (1, 0));
                assert_eq!(tracks[0].title, "A");
            }
            other => panic!("wrong variant: {other:?}"),
        }
    }

    /// A snapshot arrives whole: an absent `render_path` stays absent, and
    /// a state name is the string Base wrote.
    #[test]
    fn parses_state_snapshot() {
        // The state event is the tag plus the snapshot's fields inline
        // (`nae_base.h`'s example: {"event":"state","state":"playing",…}).
        let event = parse(
            r#"{"event":"state","state":"paused","path":null,"device_id":null,
                "devices":[],"position":0.0,"duration":0.0,"track_serial":0,
                "render_path":null,"queue":[],"queue_index":null,
                "analysis":null,"settings":null}"#,
        )
        .expect("state parses");
        match event {
            WireEvent::State(snapshot) => {
                assert_eq!(snapshot.state, "paused");
                assert!(snapshot.path.is_none());
                assert!(snapshot.render_path.is_none());
            }
            other => panic!("wrong variant: {other:?}"),
        }
    }

    /// A variant this UI does not compile for is dropped, and the error is
    /// the reason, not a panic.
    #[test]
    fn unknown_event_fails_to_parse() {
        // `visual_frame` 现在已经是这个 UI 编译的 variant；`telepathy_frame`
        // 永远不是。
        assert!(parse(r#"{"event":"telepathy_frame"}"#)
            .expect_err("unknown variants must not parse")
            .contains("unknown variant"));
    }
}
