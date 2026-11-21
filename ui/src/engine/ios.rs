//! iOS loader: the core is linked at build time, so every symbol below is a
//! link-time reference rather than a name lookup. There is no optional
//! probing and no `-2` fallback — a missing symbol fails the link, and a
//! mismatched ABI fails `Engine::open` before Base starts.
//!
//! The symbol set and the ownership rules are normative in
//! `docs/ios/core-contract.md`. Adding a symbol here without updating that
//! file is a contract drift.

use std::ffi::{c_char, c_void, CStr, CString};
use std::path::Path;

use super::{Exports, Prepared};

/// The ABI this host builds against. Checked before anything else.
pub(super) const REQUIRED_ABI: u32 = 2;

unsafe extern "C" {
    fn NAE_BaseStartWithLibrary(library: *const c_char) -> *mut c_void;
    fn NAE_BaseCommand(base: *mut c_void, json: *const c_char) -> i32;
    fn NAE_BaseSubscribe(
        base: *mut c_void,
        callback: Option<extern "C" fn(*const c_char, *mut c_void)>,
        user: *mut c_void,
    ) -> i32;
    fn NAE_BaseUnsubscribe(base: *mut c_void) -> i32;
    fn NAE_BaseSnapshot(base: *mut c_void) -> *mut c_char;
    fn NAE_BaseFreeString(text: *mut c_char);
    fn NAE_BaseShutdown(base: *mut c_void);

    fn NAE_BaseRemoteProviders() -> *mut c_char;
    fn NAE_BaseListDirectory(base: *mut c_void, path: *const c_char) -> *mut c_char;

    fn NAE_BasePlayContext(base: *mut c_void, items_json: *const c_char, start_index: usize) -> i32;
    fn NAE_BasePlayNext(base: *mut c_void, items_json: *const c_char) -> i32;
    fn NAE_BasePlayLater(base: *mut c_void, items_json: *const c_char) -> i32;
    fn NAE_BaseSetRepeatMode(base: *mut c_void, mode: i32) -> i32;
    fn NAE_BaseSetShuffleMode(base: *mut c_void, enabled: bool) -> i32;
    fn NAE_BasePrevious(base: *mut c_void) -> i32;
    fn NAE_BaseRemoveQueueItem(base: *mut c_void, id_json: *const c_char) -> i32;
    fn NAE_BaseClearUserQueue(base: *mut c_void) -> i32;
    fn NAE_BaseMoveUserQueueItem(base: *mut c_void, from_index: usize, to_index: usize) -> i32;

    fn NAE_BaseAbiVersion() -> u32;
    fn NAE_BaseAudioCapabilities() -> *mut c_char;
    fn NAE_BaseReinitializeAudio(sample_rate: u32, channels: u16) -> i32;
}

/// Starts the linked core with its library path. The path is the SQLite
/// database; it is not a library search path on iOS.
pub(super) fn start(library: &Path) -> Result<Prepared, String> {
    let abi = unsafe { NAE_BaseAbiVersion() };
    if abi != REQUIRED_ABI {
        return Err(format!(
            "E_ENGINE: 核心 ABI 版本 {abi}，本机要求 {REQUIRED_ABI}"
        ));
    }
    let exports = Exports {
        start_with_library: NAE_BaseStartWithLibrary,
        command: NAE_BaseCommand,
        subscribe: NAE_BaseSubscribe,
        unsubscribe: NAE_BaseUnsubscribe,
        snapshot: NAE_BaseSnapshot,
        free_string: NAE_BaseFreeString,
        shutdown: NAE_BaseShutdown,
        remote_providers: Some(NAE_BaseRemoteProviders),
        list_directory: Some(NAE_BaseListDirectory),
        // The plugin group is absent from the iOS build on purpose: the
        // plugin manager is compiled out and no symbol may be reported.
        list_plugins: None,
        set_plugin_enabled: None,
        reload_plugins: None,
        play_context: Some(NAE_BasePlayContext),
        play_next: Some(NAE_BasePlayNext),
        play_later: Some(NAE_BasePlayLater),
        set_repeat_mode: Some(NAE_BaseSetRepeatMode),
        set_shuffle_mode: Some(NAE_BaseSetShuffleMode),
        previous: Some(NAE_BasePrevious),
        remove_queue_item: Some(NAE_BaseRemoveQueueItem),
        clear_user_queue: Some(NAE_BaseClearUserQueue),
        move_user_queue_item: Some(NAE_BaseMoveUserQueueItem),
    };
    let library_c =
        CString::new(library.to_string_lossy().as_ref()).expect("路径里没有 NUL");
    let base = unsafe { (exports.start_with_library)(library_c.as_ptr() as *const c_char) };
    if base.is_null() {
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
        library: std::marker::PhantomData::<()>,
    })
}

/// The linked core's ABI version. Stage 0 displays it in the spike shell.
#[allow(dead_code)]
pub(crate) fn abi_version() -> u32 {
    unsafe { NAE_BaseAbiVersion() }
}

/// The capability object as JSON. Stage 1 consumes it for the route panel.
#[allow(dead_code)]
pub(crate) fn capabilities_json() -> Option<String> {
    let raw = unsafe { NAE_BaseAudioCapabilities() };
    if raw.is_null() {
        return None;
    }
    let text = unsafe { CStr::from_ptr(raw) }.to_string_lossy().into_owned();
    unsafe { NAE_BaseFreeString(raw) };
    Some(text)
}

/// Rebuilds the output unit after a route change or media-services reset.
/// The session ordering that precedes it is in `docs/ios/core-contract.md`.
#[allow(dead_code)]
pub(crate) fn reinitialize_audio(sample_rate: u32, channels: u16) -> Result<(), String> {
    let code = unsafe { NAE_BaseReinitializeAudio(sample_rate, channels) };
    if code == 0 {
        Ok(())
    } else {
        Err(format!("E_ENGINE: 重初始化音频失败（{code}）"))
    }
}

