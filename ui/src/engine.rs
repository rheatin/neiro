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
type RemoteProvidersFn = unsafe extern "C" fn() -> *mut c_char;
type ListDirectoryFn = unsafe extern "C" fn(base: *mut c_void, path: *const c_char) -> *mut c_char;
type ListPluginsFn = unsafe extern "C" fn() -> *mut c_char;
type SetPluginEnabledFn = unsafe extern "C" fn(base: *mut c_void, plugin_id: *const c_char, enabled: std::os::raw::c_int) -> std::os::raw::c_int;
type ReloadPluginsFn = unsafe extern "C" fn(base: *mut c_void) -> *mut c_char;
type PlayContextFn = unsafe extern "C" fn(base: *mut c_void, items_json: *const c_char, start_index: usize) -> i32;
type PlayNextFn = unsafe extern "C" fn(base: *mut c_void, items_json: *const c_char) -> i32;
type PlayLaterFn = unsafe extern "C" fn(base: *mut c_void, items_json: *const c_char) -> i32;
type SetRepeatModeFn = unsafe extern "C" fn(base: *mut c_void, mode: i32) -> i32;
type SetShuffleModeFn = unsafe extern "C" fn(base: *mut c_void, enabled: bool) -> i32;
type PreviousFn = unsafe extern "C" fn(base: *mut c_void) -> i32;
type RemoveQueueItemFn = unsafe extern "C" fn(base: *mut c_void, id_json: *const c_char) -> i32;
type ClearUserQueueFn = unsafe extern "C" fn(base: *mut c_void) -> i32;
type MoveUserQueueItemFn = unsafe extern "C" fn(base: *mut c_void, from_idx: usize, to_idx: usize) -> i32;

/// A running Base. Dropping it unsubscribes (so no callback ever runs on
/// freed memory), shuts Base down and releases the DLL.
pub struct Engine {
    base: *mut c_void,
    command: CommandFn,
    unsubscribe: UnsubscribeFn,
    shutdown: ShutdownFn,
    snapshot: SnapshotFn,
    free_string: FreeStringFn,
    remote_providers: Option<RemoteProvidersFn>,
    list_directory: Option<ListDirectoryFn>,
    list_plugins: Option<ListPluginsFn>,
    set_plugin_enabled: Option<SetPluginEnabledFn>,
    reload_plugins: Option<ReloadPluginsFn>,
    play_context: Option<PlayContextFn>,
    play_next: Option<PlayNextFn>,
    play_later: Option<PlayLaterFn>,
    set_repeat_mode: Option<SetRepeatModeFn>,
    set_shuffle_mode: Option<SetShuffleModeFn>,
    previous: Option<PreviousFn>,
    remove_queue_item: Option<RemoveQueueItemFn>,
    clear_user_queue: Option<ClearUserQueueFn>,
    move_user_queue_item: Option<MoveUserQueueItemFn>,
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
        #[cfg(target_os = "macos")]
        paths.push(dir.join("../Frameworks").join(DLL));
    }
    for sdk in ["../.cache/neiro-core/current", ".cache/neiro-core/current"] {
        #[cfg(windows)]
        paths.push(PathBuf::from(sdk).join("bin").join(DLL));
        #[cfg(not(windows))]
        paths.push(PathBuf::from(sdk).join("lib").join(DLL));
    }
    for root in [
        "../neiro-core/base",
        "../../neiro-core/base",
        "../Neiro/base",
        "../base",
        "../../base",
    ] {
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
    remote_providers: Option<RemoteProvidersFn>,
    list_directory: Option<ListDirectoryFn>,
    list_plugins: Option<ListPluginsFn>,
    set_plugin_enabled: Option<SetPluginEnabledFn>,
    reload_plugins: Option<ReloadPluginsFn>,
    play_context: Option<PlayContextFn>,
    play_next: Option<PlayNextFn>,
    play_later: Option<PlayLaterFn>,
    set_repeat_mode: Option<SetRepeatModeFn>,
    set_shuffle_mode: Option<SetShuffleModeFn>,
    previous: Option<PreviousFn>,
    remove_queue_item: Option<RemoveQueueItemFn>,
    clear_user_queue: Option<ClearUserQueueFn>,
    move_user_queue_item: Option<MoveUserQueueItemFn>,
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
    remote_providers: Option<RemoteProvidersFn>,
    list_directory: Option<ListDirectoryFn>,
    list_plugins: Option<ListPluginsFn>,
    set_plugin_enabled: Option<SetPluginEnabledFn>,
    reload_plugins: Option<ReloadPluginsFn>,
    play_context: Option<PlayContextFn>,
    play_next: Option<PlayNextFn>,
    play_later: Option<PlayLaterFn>,
    set_repeat_mode: Option<SetRepeatModeFn>,
    set_shuffle_mode: Option<SetShuffleModeFn>,
    previous: Option<PreviousFn>,
    remove_queue_item: Option<RemoveQueueItemFn>,
    clear_user_queue: Option<ClearUserQueueFn>,
    move_user_queue_item: Option<MoveUserQueueItemFn>,
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
        let all_candidates = dll_candidates(exe_dir.as_deref());
        // Prefer a candidate that exports the full plugin C ABI (`NAE_BaseListPlugins`)
        // when a local `../neiro-core` build is newer than a cached v0.2.0 prebuilt.
        let mut ordered = Vec::with_capacity(all_candidates.len());
        let mut legacy = Vec::new();
        for c in all_candidates {
            if c.is_file() && Self::exports_plugin_api(&c) {
                ordered.push(c);
            } else {
                legacy.push(c);
            }
        }
        ordered.extend(legacy);

        for candidate in ordered {
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

    fn exports_plugin_api(path: &Path) -> bool {
        #[cfg(windows)]
        let lib = unsafe {
            libloading::os::windows::Library::load_with_flags(
                path,
                libloading::os::windows::LOAD_WITH_ALTERED_SEARCH_PATH,
            )
        }
        .ok()
        .map(libloading::Library::from);
        #[cfg(not(windows))]
        let lib = unsafe { libloading::Library::new(path) }.ok();
        let Some(lib) = lib else { return false };
        symbol_optional::<ListPluginsFn>(&lib, b"NAE_BaseListPlugins\0").is_some()
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
            remote_providers: exports.remote_providers,
            list_directory: exports.list_directory,
            list_plugins: exports.list_plugins,
            set_plugin_enabled: exports.set_plugin_enabled,
            reload_plugins: exports.reload_plugins,
            play_context: exports.play_context,
            play_next: exports.play_next,
            play_later: exports.play_later,
            set_repeat_mode: exports.set_repeat_mode,
            set_shuffle_mode: exports.set_shuffle_mode,
            previous: exports.previous,
            remove_queue_item: exports.remove_queue_item,
            clear_user_queue: exports.clear_user_queue,
            move_user_queue_item: exports.move_user_queue_item,
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

    pub fn remote_providers(&self) -> String {
        if let Some(func) = self.remote_providers {
            unsafe {
                let ptr = func();
                if !ptr.is_null() {
                    let text = CStr::from_ptr(ptr).to_string_lossy().into_owned();
                    (self.free_string)(ptr);
                    return text;
                }
            }
        }
        fallback_remote_providers_json()
    }

    pub fn list_directory(&self, path: &str) -> Result<String, String> {
        let Some(func) = self.list_directory else {
            return Ok(String::from("{\"path\":\"\",\"folders\":[],\"tracks\":[]}"));
        };
        let path_c = CString::new(path).map_err(|e| format!("路径包含 NUL: {e}"))?;
        unsafe {
            let ptr = func(self.base, path_c.as_ptr());
            if ptr.is_null() {
                return Ok(String::from("{\"path\":\"\",\"folders\":[],\"tracks\":[]}"));
            }
            let text = CStr::from_ptr(ptr).to_string_lossy().into_owned();
            (self.free_string)(ptr);
            Ok(text)
        }
    }

    pub fn list_plugins(&self) -> String {
        if let Some(func) = self.list_plugins {
            unsafe {
                let ptr = func();
                if !ptr.is_null() {
                    let text = CStr::from_ptr(ptr).to_string_lossy().into_owned();
                    (self.free_string)(ptr);
                    return text;
                }
            }
        }
        fallback_list_plugins_json()
    }

    pub fn set_plugin_enabled(&self, id: &str, enabled: bool) -> Result<(), String> {
        if let Some(func) = self.set_plugin_enabled {
            let c_id = CString::new(id).map_err(|e| e.to_string())?;
            let res = unsafe { func(self.base, c_id.as_ptr(), if enabled { 1 } else { 0 }) };
            if res == 1 {
                return Ok(());
            } else {
                return Err("E_ENGINE: set_plugin_enabled failed".to_string());
            }
        }
        if fallback_set_plugin_enabled(id, enabled) {
            Ok(())
        } else {
            Err("E_ENGINE: set_plugin_enabled failed".to_string())
        }
    }

    pub fn reload_plugins(&self) -> String {
        if let Some(func) = self.reload_plugins {
            unsafe {
                let ptr = func(self.base);
                if !ptr.is_null() {
                    let text = CStr::from_ptr(ptr).to_string_lossy().into_owned();
                    (self.free_string)(ptr);
                    return text;
                }
            }
        }
        fallback_list_plugins_json()
    }

    pub fn play_context(&self, items_json: &str, start_index: usize) -> Result<(), String> {
        let (norm_json, paths) = normalize_queue_items_json(items_json)?;
        if paths.is_empty() {
            return Ok(());
        }
        let tried_res = if let Some(func) = self.play_context {
            let c_json = CString::new(norm_json.as_str()).map_err(|e| e.to_string())?;
            let code = unsafe { func(self.base, c_json.as_ptr(), start_index) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: play_context failed ({code})"));
            }
        } else {
            let items: serde_json::Value = serde_json::from_str(&norm_json).map_err(|e| e.to_string())?;
            let cmd = serde_json::json!({
                "command": "play_context",
                "items": items,
                "start_index": start_index
            });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.play_context.is_none() && tried_res.is_err()) {
            let first_idx = if start_index < paths.len() { start_index } else { 0 };
            let first_path = &paths[first_idx];
            self.command(&serde_json::json!({ "command": "play", "path": first_path }).to_string())?;
            if first_idx + 1 < paths.len() {
                let rest = &paths[first_idx + 1..];
                if !rest.is_empty() {
                    self.command(&serde_json::json!({ "command": "enqueue", "paths": rest }).to_string())?;
                }
            }
            Ok(())
        } else {
            tried_res
        }
    }

    pub fn play_next(&self, items_json: &str) -> Result<(), String> {
        let (norm_json, paths) = normalize_queue_items_json(items_json)?;
        if paths.is_empty() {
            return Ok(());
        }
        let tried_res = if let Some(func) = self.play_next {
            let c_json = CString::new(norm_json.as_str()).map_err(|e| e.to_string())?;
            let code = unsafe { func(self.base, c_json.as_ptr()) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: play_next failed ({code})"));
            }
        } else {
            let items: serde_json::Value = serde_json::from_str(&norm_json).map_err(|e| e.to_string())?;
            let cmd = serde_json::json!({
                "command": "play_next",
                "items": items,
            });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.play_next.is_none() && tried_res.is_err()) {
            self.command(&serde_json::json!({ "command": "enqueue", "paths": paths }).to_string())
        } else {
            tried_res
        }
    }

    pub fn play_later(&self, items_json: &str) -> Result<(), String> {
        let (norm_json, paths) = normalize_queue_items_json(items_json)?;
        if paths.is_empty() {
            return Ok(());
        }
        let tried_res = if let Some(func) = self.play_later {
            let c_json = CString::new(norm_json.as_str()).map_err(|e| e.to_string())?;
            let code = unsafe { func(self.base, c_json.as_ptr()) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: play_later failed ({code})"));
            }
        } else {
            let items: serde_json::Value = serde_json::from_str(&norm_json).map_err(|e| e.to_string())?;
            let cmd = serde_json::json!({
                "command": "play_later",
                "items": items,
            });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.play_later.is_none() && tried_res.is_err()) {
            self.command(&serde_json::json!({ "command": "enqueue", "paths": paths }).to_string())
        } else {
            tried_res
        }
    }

    pub fn set_repeat_mode(&self, mode: i32) -> Result<(), String> {
        let tried_res = if let Some(func) = self.set_repeat_mode {
            let code = unsafe { func(self.base, mode) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: set_repeat_mode failed ({code})"));
            }
        } else {
            let mode_str = match mode {
                1 => "All",
                2 => "One",
                _ => "Off",
            };
            let cmd = serde_json::json!({
                "command": "set_repeat_mode",
                "mode": mode_str,
            });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.set_repeat_mode.is_none() && tried_res.is_err()) {
            Ok(())
        } else {
            tried_res
        }
    }

    pub fn set_shuffle_mode(&self, enabled: bool) -> Result<(), String> {
        let tried_res = if let Some(func) = self.set_shuffle_mode {
            let code = unsafe { func(self.base, enabled) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: set_shuffle_mode failed ({code})"));
            }
        } else {
            let cmd = serde_json::json!({
                "command": "set_shuffle_mode",
                "enabled": enabled,
            });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.set_shuffle_mode.is_none() && tried_res.is_err()) {
            Ok(())
        } else {
            tried_res
        }
    }

    pub fn previous(&self) -> Result<(), String> {
        let tried_res = if let Some(func) = self.previous {
            let code = unsafe { func(self.base) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: previous failed ({code})"));
            }
        } else {
            let cmd = serde_json::json!({ "command": "previous" });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.previous.is_none() && tried_res.is_err()) {
            if let Ok(snap) = self.snapshot() {
                if snap.position > 3.0 {
                    return self.command(&serde_json::json!({ "command": "seek", "seconds": 0.0 }).to_string());
                }
                if let Some(idx) = snap.queue_index {
                    if idx > 0 && idx - 1 < snap.queue.len() {
                        let prev_path = &snap.queue[idx - 1];
                        return self.command(&serde_json::json!({ "command": "play", "path": prev_path }).to_string());
                    }
                }
            }
            self.command(&serde_json::json!({ "command": "seek", "seconds": 0.0 }).to_string())
        } else {
            tried_res
        }
    }

    pub fn remove_queue_item(&self, id: &str) -> Result<(), String> {
        let tried_res = if let Some(func) = self.remove_queue_item {
            let c_id = CString::new(id).map_err(|e| e.to_string())?;
            let code = unsafe { func(self.base, c_id.as_ptr()) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: remove_queue_item failed ({code})"));
            }
        } else {
            let cmd = serde_json::json!({ "command": "remove_queue_item", "id": id });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.remove_queue_item.is_none() && tried_res.is_err()) {
            if let Ok(idx) = id.parse::<u32>() {
                let _ = self.command(&serde_json::json!({ "command": "remove_from_queue", "index": idx }).to_string());
            }
            Ok(())
        } else {
            tried_res
        }
    }

    pub fn clear_user_queue(&self) -> Result<(), String> {
        let tried_res = if let Some(func) = self.clear_user_queue {
            let code = unsafe { func(self.base) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: clear_user_queue failed ({code})"));
            }
        } else {
            let cmd = serde_json::json!({ "command": "clear_user_queue" });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.clear_user_queue.is_none() && tried_res.is_err()) {
            self.command(&serde_json::json!({ "command": "clear_queue" }).to_string())
        } else {
            tried_res
        }
    }

    pub fn move_user_queue_item(&self, from: usize, to: usize) -> Result<(), String> {
        let tried_res = if let Some(func) = self.move_user_queue_item {
            let code = unsafe { func(self.base, from, to) };
            if code == 0 {
                return Ok(());
            } else if code == -2 {
                Err("E_ENGINE: command 被拒绝（-2）".to_string())
            } else {
                return Err(format!("E_ENGINE: move_user_queue_item failed ({code})"));
            }
        } else {
            let cmd = serde_json::json!({ "command": "move_user_queue_item", "from": from, "to": to });
            self.command(&cmd.to_string())
        };

        if is_rejected_2(&tried_res) || (self.move_user_queue_item.is_none() && tried_res.is_err()) {
            let _ = self.command(&serde_json::json!({ "command": "move_in_queue", "from": from, "to": to }).to_string());
            Ok(())
        } else {
            tried_res
        }
    }
}

pub fn normalize_queue_items_json(items_json: &str) -> Result<(String, Vec<String>), String> {
    let val: serde_json::Value =
        serde_json::from_str(items_json).map_err(|e| format!("E_PARSE: {e}"))?;
    let items = match val {
        serde_json::Value::Array(arr) => arr,
        other => vec![other],
    };
    let mut normalized = Vec::with_capacity(items.len());
    let mut paths = Vec::with_capacity(items.len());

    for item in items {
        let obj = match item {
            serde_json::Value::Object(map) => map,
            _ => continue,
        };
        let path = obj
            .get("path")
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string();
        let id = obj
            .get("id")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string())
            .unwrap_or_else(|| path.clone());
        let title = obj
            .get("title")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string());
        let artist = obj
            .get("artist")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string());

        let duration_ms = if let Some(v) = obj.get("duration_ms").and_then(|v| v.as_u64()) {
            v
        } else if let Some(v) = obj.get("durationMs").and_then(|v| v.as_f64()) {
            v.round() as u64
        } else if let Some(v) = obj.get("duration_ms").and_then(|v| v.as_f64()) {
            v.round() as u64
        } else if let Some(v) = obj.get("durationMs").and_then(|v| v.as_u64()) {
            v
        } else {
            0u64
        };

        paths.push(path.clone());
        let mut norm_obj = serde_json::Map::new();
        norm_obj.insert("id".to_string(), serde_json::Value::String(id));
        norm_obj.insert("path".to_string(), serde_json::Value::String(path));
        if let Some(t) = title {
            norm_obj.insert("title".to_string(), serde_json::Value::String(t));
        } else {
            norm_obj.insert("title".to_string(), serde_json::Value::Null);
        }
        if let Some(a) = artist {
            norm_obj.insert("artist".to_string(), serde_json::Value::String(a));
        } else {
            norm_obj.insert("artist".to_string(), serde_json::Value::Null);
        }
        norm_obj.insert(
            "duration_ms".to_string(),
            serde_json::Value::Number(duration_ms.into()),
        );
        normalized.push(serde_json::Value::Object(norm_obj));
    }
    let norm_json = serde_json::to_string(&normalized).map_err(|e| format!("E_SERIALIZE: {e}"))?;
    Ok((norm_json, paths))
}

fn is_rejected_2(res: &Result<(), String>) -> bool {
    if let Err(msg) = res {
        msg.contains("-2") || msg.contains("被拒绝（-2）")
    } else {
        false
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
                remote_providers: self.remote_providers,
                list_directory: self.list_directory,
                list_plugins: self.list_plugins,
                set_plugin_enabled: self.set_plugin_enabled,
                reload_plugins: self.reload_plugins,
                play_context: self.play_context,
                play_next: self.play_next,
                play_later: self.play_later,
                set_repeat_mode: self.set_repeat_mode,
                set_shuffle_mode: self.set_shuffle_mode,
                previous: self.previous,
                remove_queue_item: self.remove_queue_item,
                clear_user_queue: self.clear_user_queue,
                move_user_queue_item: self.move_user_queue_item,
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
            remote_providers: symbol_optional::<RemoteProvidersFn>(
                library,
                b"NAE_BaseRemoteProviders\0",
            ),
            list_directory: symbol_optional::<ListDirectoryFn>(
                library,
                b"NAE_BaseListDirectory\0",
            ),
            list_plugins: symbol_optional::<ListPluginsFn>(
                library,
                b"NAE_BaseListPlugins\0",
            ),
            set_plugin_enabled: symbol_optional::<SetPluginEnabledFn>(
                library,
                b"NAE_BaseSetPluginEnabled\0",
            ),
            reload_plugins: symbol_optional::<ReloadPluginsFn>(
                library,
                b"NAE_BaseReloadPlugins\0",
            ),
            play_context: symbol_optional::<PlayContextFn>(
                library,
                b"NAE_BasePlayContext\0",
            ),
            play_next: symbol_optional::<PlayNextFn>(
                library,
                b"NAE_BasePlayNext\0",
            ),
            play_later: symbol_optional::<PlayLaterFn>(
                library,
                b"NAE_BasePlayLater\0",
            ),
            set_repeat_mode: symbol_optional::<SetRepeatModeFn>(
                library,
                b"NAE_BaseSetRepeatMode\0",
            ),
            set_shuffle_mode: symbol_optional::<SetShuffleModeFn>(
                library,
                b"NAE_BaseSetShuffleMode\0",
            ),
            previous: symbol_optional::<PreviousFn>(
                library,
                b"NAE_BasePrevious\0",
            ),
            remove_queue_item: symbol_optional::<RemoveQueueItemFn>(
                library,
                b"NAE_BaseRemoveQueueItem\0",
            ),
            clear_user_queue: symbol_optional::<ClearUserQueueFn>(
                library,
                b"NAE_BaseClearUserQueue\0",
            ),
            move_user_queue_item: symbol_optional::<MoveUserQueueItemFn>(
                library,
                b"NAE_BaseMoveUserQueueItem\0",
            ),
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

fn symbol_optional<T: Copy>(library: &Library, name: &[u8]) -> Option<T> {
    unsafe { library.get(name).ok().map(|s: Symbol<'_, T>| *s) }
}

fn fallback_disabled_set() -> &'static std::sync::Mutex<std::collections::HashSet<String>> {
    static DISABLED: std::sync::OnceLock<std::sync::Mutex<std::collections::HashSet<String>>> =
        std::sync::OnceLock::new();
    DISABLED.get_or_init(|| std::sync::Mutex::new(std::collections::HashSet::new()))
}

fn is_fallback_enabled(id: &str) -> bool {
    // Engine built-in decoders, expanders and DSP are permanent builtins in v0.2.0
    // and cannot be disabled without C ABI symbol support.
    let permanent_builtins = [
        "builtin.expander.cue",
        "builtin.expander.sacd",
        "builtin.decoder.dsd",
        "builtin.dsp.soxr",
    ];
    if permanent_builtins.contains(&id) {
        return true;
    }

    !fallback_disabled_set()
        .lock()
        .map(|s| s.contains(id))
        .unwrap_or(false)
}

fn fallback_set_plugin_enabled(id: &str, enabled: bool) -> bool {
    let permanent_builtins = [
        "builtin.expander.cue",
        "builtin.expander.sacd",
        "builtin.decoder.dsd",
        "builtin.dsp.soxr",
    ];
    if permanent_builtins.contains(&id) {
        // Refuse to fake dynamic disabling of built-in engine plugins on v0.2.0
        return enabled;
    }

    let toggleable = [
        "builtin.shell.industrial-rack",
        "builtin.shell.studio-minimal",
        "builtin.visualizer.signal-chain",
        "jellyfin",
        "jellyfin_token",
        "navidrome",
        "webdav",
    ];
    if !toggleable.contains(&id) {
        return false;
    }
    if let Ok(mut set) = fallback_disabled_set().lock() {
        if enabled {
            set.remove(id);
            if id == "jellyfin" {
                set.remove("jellyfin_token");
            }
        } else {
            set.insert(id.to_string());
            if id == "jellyfin" {
                set.insert("jellyfin_token".to_string());
            }
        }
        true
    } else {
        false
    }
}

fn fallback_list_plugins_json() -> String {
    let items = serde_json::json!([
        {
            "id": "builtin.shell.industrial-rack",
            "name": "Industrial Rack (Default)",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "Neiro 默认工业机架高保真主视图，具备完整的左右分栏与状态遥测",
            "tier": "ui",
            "category": "ui.shell",
            "enabled": is_fallback_enabled("builtin.shell.industrial-rack"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["playback.control", "library.read", "library.manage", "tags.write", "remote.manage", "dsp.read"]
        },
        {
            "id": "builtin.shell.studio-minimal",
            "name": "Studio Minimal Deck",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "极简母带工作室控制台视图，居中大黑胶封套与通栏信号链与曲目",
            "tier": "ui",
            "category": "ui.shell",
            "enabled": is_fallback_enabled("builtin.shell.studio-minimal"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["playback.control", "library.read", "remote.manage", "dsp.read"]
        },
        {
            "id": "builtin.visualizer.signal-chain",
            "name": "HiFi Signal Path & Spectrum Inspector",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "实时 60Hz FFT 音频频谱与信号链路可视化插槽",
            "tier": "ui",
            "category": "ui.visualizer",
            "enabled": is_fallback_enabled("builtin.visualizer.signal-chain"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["dsp.read", "playback.control"]
        },
        {
            "id": "jellyfin",
            "name": "Jellyfin 媒体服务器",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "支持账号密码与 API Token 双模式连接 Jellyfin 远程无损音乐库",
            "tier": "base",
            "category": "source.connector",
            "enabled": is_fallback_enabled("jellyfin"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["remote.read", "remote.manage"]
        },
        {
            "id": "navidrome",
            "name": "Navidrome / Subsonic",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "基于 Subsonic / OpenSubsonic 协议连接 Navidrome 个人音乐服务器",
            "tier": "base",
            "category": "source.connector",
            "enabled": is_fallback_enabled("navidrome"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["remote.read", "remote.manage"]
        },
        {
            "id": "webdav",
            "name": "WebDAV / NAS",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "通过 WebDAV 挂载 NAS 或网盘目录并按需流式读取音轨",
            "tier": "base",
            "category": "source.connector",
            "enabled": is_fallback_enabled("webdav"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["remote.read"]
        },
        {
            "id": "builtin.expander.cue",
            "name": "CUE Sheet 分轨解析器",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "CUE 索引切片与中文 GB18030/GBK/Big5 编码自动探测分轨",
            "tier": "base",
            "category": "source.expander",
            "enabled": is_fallback_enabled("builtin.expander.cue"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["library.read"]
        },
        {
            "id": "builtin.expander.sacd",
            "name": "Scarlet Book SACD ISO 光盘解析器",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "直接读取 SACD .iso 扇区 510 结构、Area TOC 分轨与 DSD 实时切片物化",
            "tier": "base",
            "category": "source.expander",
            "enabled": is_fallback_enabled("builtin.expander.sacd"),
            "builtin": true,
            "requiresRestart": false,
            "permissions": ["library.read"]
        },
        {
            "id": "builtin.decoder.dsd",
            "name": "DSD64/128/256 & DoP 帧解码器",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "原生 1-bit DSD 比特流解码与 DoP 0x05/0xFA 帧打包器",
            "tier": "engine",
            "category": "playback.decoder",
            "enabled": is_fallback_enabled("builtin.decoder.dsd"),
            "builtin": true,
            "requiresRestart": true,
            "permissions": []
        },
        {
            "id": "builtin.dsp.soxr",
            "name": "SoXR 母带级重采样与 TPDF 抖动",
            "version": "1.0.0",
            "author": "Char ONE",
            "description": "64-bit 浮点重采样卷积与三角概率密度函数随机抖动",
            "tier": "engine",
            "category": "playback.dsp",
            "enabled": is_fallback_enabled("builtin.dsp.soxr"),
            "builtin": true,
            "requiresRestart": false,
            "breaksBitPerfect": true,
            "permissions": ["dsp.control"]
        }
    ]);
    serde_json::to_string(&items).unwrap_or_else(|_| "[]".into())
}

fn fallback_remote_providers_json() -> String {
    let mut list = Vec::new();
    if is_fallback_enabled("jellyfin") {
        list.push(serde_json::json!({
            "id": "jellyfin",
            "name": "Jellyfin 媒体服务器",
            "fields": [
                { "key": "base_url", "label": "服务器地址", "kind": "url", "placeholder": "http://192.168.1.2:8096", "required": true, "secret": false },
                { "key": "user", "label": "用户名", "kind": "text", "placeholder": "admin", "required": false, "secret": false },
                { "key": "password", "label": "密码", "kind": "password", "placeholder": "", "required": false, "secret": true },
                { "key": "token", "label": "API Key / Token", "kind": "password", "placeholder": "可选 API Token", "required": false, "secret": true }
            ]
        }));
    }
    if is_fallback_enabled("navidrome") {
        list.push(serde_json::json!({
            "id": "navidrome",
            "name": "Navidrome / Subsonic",
            "fields": [
                { "key": "base_url", "label": "服务器地址", "kind": "url", "placeholder": "http://192.168.1.2:4533", "required": true, "secret": false },
                { "key": "user", "label": "用户名", "kind": "text", "placeholder": "admin", "required": true, "secret": false },
                { "key": "password", "label": "密码", "kind": "password", "placeholder": "", "required": true, "secret": true }
            ]
        }));
    }
    if is_fallback_enabled("webdav") {
        list.push(serde_json::json!({
            "id": "webdav",
            "name": "WebDAV / NAS",
            "fields": [
                { "key": "base_url", "label": "WebDAV 地址", "kind": "url", "placeholder": "http://192.168.1.2:5005/Music", "required": true, "secret": false },
                { "key": "user", "label": "用户名", "kind": "text", "placeholder": "可选", "required": false, "secret": false },
                { "key": "password", "label": "密码", "kind": "password", "placeholder": "可选", "required": false, "secret": true }
            ]
        }));
    }
    serde_json::to_string(&list).unwrap_or_else(|_| "[]".into())
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
    #[serde(default)]
    pub queue_snapshot: Option<serde_json::Value>,
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
    QueueDirectorChanged {
        snapshot: serde_json::Value,
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

    #[test]
    fn test_normalize_queue_items_json_camel_case_duration() {
        let input = r#"[{"id":"t1","path":"/path/1.flac","title":"Title 1","artist":"Artist 1","durationMs":180234.6}]"#;
        let (normalized, paths) = normalize_queue_items_json(input).expect("normalization succeeds");
        assert_eq!(paths, vec!["/path/1.flac"]);
        assert!(normalized.contains("\"duration_ms\":180235"));
        assert!(normalized.contains("\"title\":\"Title 1\""));
    }

    #[test]
    fn test_normalize_queue_items_json_snake_case_duration() {
        let input = r#"[{"id":"t2","path":"/path/2.flac","title":"Title 2","artist":"Artist 2","duration_ms":240000}]"#;
        let (normalized, paths) = normalize_queue_items_json(input).expect("normalization succeeds");
        assert_eq!(paths, vec!["/path/2.flac"]);
        assert!(normalized.contains("\"duration_ms\":240000"));
    }

    #[test]
    fn test_normalize_queue_items_json_missing_duration_defaults_to_zero() {
        let input = r#"[{"id":"t3","path":"/path/3.flac","title":"Title 3","artist":"Artist 3"}]"#;
        let (normalized, paths) = normalize_queue_items_json(input).expect("normalization succeeds");
        assert_eq!(paths, vec!["/path/3.flac"]);
        assert!(normalized.contains("\"duration_ms\":0"));
    }

    #[test]
    fn test_is_rejected_2_detection() {
        assert!(is_rejected_2(&Err("NAE_BASE_ERR_PARSE = -2".to_string())));
        assert!(is_rejected_2(&Err("播放失败：command 被拒绝（-2）".to_string())));
        assert!(is_rejected_2(&Err("rejected with -2".to_string())));
        assert!(!is_rejected_2(&Err("different error".to_string())));
        assert!(!is_rejected_2(&Ok(())));
    }
}
