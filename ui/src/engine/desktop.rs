//! Desktop loader: Base is opened at run time with `libloading`, never
//! linked, so the UI builds and runs on a machine where the engine was never
//! built. Optional-symbol probing and the `-2` fallback live here and only
//! here; iOS links the same symbols at build time (see `engine/ios.rs`).

use std::ffi::{c_char, CString};
use std::path::{Path, PathBuf};

use libloading::{Library, Symbol};

use super::{
    ClearUserQueueFn, CommandFn, Engine, Exports, FreeStringFn, ListDirectoryFn, ListPluginsFn,
    MoveUserQueueItemFn, PlayContextFn, PlayLaterFn, PlayNextFn, Prepared, PreviousFn,
    ReloadPluginsFn, RemoteProvidersFn, RemoveQueueItemFn, SetPluginEnabledFn, SetRepeatModeFn,
    SetShuffleModeFn, ShutdownFn, SnapshotFn, StartWithLibraryFn, SubscribeFn, UnsubscribeFn,
};

/// The engine's Base library beside the binary, named the way the platform
/// loads it.
#[cfg(windows)]
const DLL: &str = "neiro_base.dll";
#[cfg(target_os = "macos")]
const DLL: &str = "libneiro_base.dylib";
#[cfg(all(unix, not(target_os = "macos")))]
const DLL: &str = "libneiro_base.so";

/// Opens Base from the first candidate that loads and subscribes. A machine
/// without the engine gets `E_ENGINE`, never a plausible default.
pub(super) fn open(library: &Path) -> Result<(Engine, std::sync::mpsc::Receiver<String>), String> {
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
        if c.is_file() && exports_plugin_api(&c) {
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
        let reason = match load(&candidate, library) {
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
    let exports = resolve_exports(&library_file)?;
    let library_c =
        CString::new(library.to_string_lossy().as_ref()).expect("路径里没有 NUL");
    let base = unsafe { (exports.start_with_library)(library_c.as_ptr() as *const c_char) };
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

/// Resolves the exports in one place so a missing symbol names itself.
fn resolve_exports(library: &Library) -> Result<Exports, String> {
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
        list_directory: symbol_optional::<ListDirectoryFn>(library, b"NAE_BaseListDirectory\0"),
        list_plugins: symbol_optional::<ListPluginsFn>(library, b"NAE_BaseListPlugins\0"),
        set_plugin_enabled: symbol_optional::<SetPluginEnabledFn>(
            library,
            b"NAE_BaseSetPluginEnabled\0",
        ),
        reload_plugins: symbol_optional::<ReloadPluginsFn>(library, b"NAE_BaseReloadPlugins\0"),
        play_context: symbol_optional::<PlayContextFn>(library, b"NAE_BasePlayContext\0"),
        play_next: symbol_optional::<PlayNextFn>(library, b"NAE_BasePlayNext\0"),
        play_later: symbol_optional::<PlayLaterFn>(library, b"NAE_BasePlayLater\0"),
        set_repeat_mode: symbol_optional::<SetRepeatModeFn>(library, b"NAE_BaseSetRepeatMode\0"),
        set_shuffle_mode: symbol_optional::<SetShuffleModeFn>(library, b"NAE_BaseSetShuffleMode\0"),
        previous: symbol_optional::<PreviousFn>(library, b"NAE_BasePrevious\0"),
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

/// One export by its C name; the error carries the name, not a raw OS code.
fn symbol<T: Copy>(library: &Library, name: &[u8], printed: &str) -> Result<T, String> {
    let resolved: Symbol<'_, T> =
        unsafe { library.get(name) }.map_err(|e| format!("缺少符号 {printed}：{e}"))?;
    Ok(*resolved)
}

fn symbol_optional<T: Copy>(library: &Library, name: &[u8]) -> Option<T> {
    unsafe { library.get(name).ok().map(|s: Symbol<'_, T>| *s) }
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
