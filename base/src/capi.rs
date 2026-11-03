//! C ABI for Swift 6 (and any other non-Rust UI).
//!
//! The Rust API is the implementation; this file is only a JSON transport over
//! the already-serializable protocol. Swift owns the UI and the iOS lifecycle;
//! Base owns playback policy, analysis and the cache.
//!
//! Every function is `extern "C"` and must not unwind. Strings returned here
//! belong to Base and are released with `NAE_BaseFreeString`.
//!
//! Threading: `NAE_BaseCommand` and `NAE_BaseSnapshot` may be called from any
//! thread, but one control thread is the intended use. Event callbacks run on a
//! dedicated Rust thread; a Swift 6 client must hop to its actor before
//! touching UI state.

#![allow(non_camel_case_types)]

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};

use crate::protocol::{Command, Event};
use crate::Base;

pub type NAE_BaseEventCallback = extern "C" fn(*const c_char, *mut c_void);

pub struct NAE_Base {
    base: Option<Base>,
    forward: Option<JoinHandle<()>>,
    forward_stop: Option<Arc<AtomicBool>>,
}

const NAE_BASE_OK: i32 = 0;
const NAE_BASE_ERR_ARGUMENT: i32 = -1;
const NAE_BASE_ERR_PARSE: i32 = -2;
const NAE_BASE_ERR_CLOSED: i32 = -3;
const NAE_BASE_ERR_ALREADY: i32 = -4;

/// Start Base with exclusive output. Returns null when the workers could not
/// be spawned.
#[no_mangle]
pub extern "C" fn NAE_BaseStart() -> *mut NAE_Base {
    start(neiro_audio::ShareKind::Exclusive)
}

/// Start Base with an explicit share mode: 0 = exclusive, 1 = shared.
/// Shared is a choice for platforms where another process owns the endpoint;
/// it is not an automatic fallback.
#[no_mangle]
pub extern "C" fn NAE_BaseStartWithShare(share: i32) -> *mut NAE_Base {
    match share {
        0 => start(neiro_audio::ShareKind::Exclusive),
        1 => start(neiro_audio::ShareKind::Shared),
        _ => std::ptr::null_mut(),
    }
}

fn start(share: neiro_audio::ShareKind) -> *mut NAE_Base {
    match catch_unwind(|| Base::start_with_share(share)) {
        Ok(Ok(base)) => Box::into_raw(Box::new(NAE_Base {
            base: Some(base),
            forward: None,
            forward_stop: None,
        })),
        _ => std::ptr::null_mut(),
    }
}

/// Send one serialized `Command`. Returns 0 on success.
///
/// The command is the same JSON `Command` the Rust API uses, e.g.
/// `{"command":"play","path":"...","device_id":null}`.
#[no_mangle]
pub extern "C" fn NAE_BaseCommand(base: *mut NAE_Base, json: *const c_char) -> i32 {
    if base.is_null() || json.is_null() {
        return NAE_BASE_ERR_ARGUMENT;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Some(base) = (unsafe { base.as_ref() }) else {
            return NAE_BASE_ERR_ARGUMENT;
        };
        let Some(base) = base.base.as_ref() else {
            return NAE_BASE_ERR_CLOSED;
        };
        let text = match unsafe { CStr::from_ptr(json) }.to_str() {
            Ok(text) => text,
            Err(_) => return NAE_BASE_ERR_ARGUMENT,
        };
        let command: Command = match serde_json::from_str(text) {
            Ok(command) => command,
            Err(_) => return NAE_BASE_ERR_PARSE,
        };
        match base.handle().dispatch(command) {
            Ok(()) => NAE_BASE_OK,
            Err(_) => NAE_BASE_ERR_CLOSED,
        }
    }))
    .unwrap_or(NAE_BASE_ERR_ARGUMENT)
}

/// Subscribe once. Events are JSON `Event` values on a dedicated thread.
///
/// The callback and `user` must stay valid until `NAE_BaseShutdown`; Base does
/// not copy or retain the UI object behind `user`.
#[no_mangle]
pub extern "C" fn NAE_BaseSubscribe(
    base: *mut NAE_Base,
    callback: Option<NAE_BaseEventCallback>,
    user: *mut c_void,
) -> i32 {
    if base.is_null() || callback.is_none() {
        return NAE_BASE_ERR_ARGUMENT;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Some(base) = (unsafe { base.as_mut() }) else {
            return NAE_BASE_ERR_ARGUMENT;
        };
        if base.forward.is_some() {
            return NAE_BASE_ERR_ALREADY;
        }
        let Some(inner) = base.base.as_ref() else {
            return NAE_BASE_ERR_CLOSED;
        };
        let receiver = inner.handle().subscribe();
        let callback = callback.expect("checked above");
        let user = user as usize;
        let stop = Arc::new(AtomicBool::new(false));
        let forward_stop = stop.clone();
        let handle = match thread::Builder::new()
            .name("neiro-base-capi-events".into())
            .spawn(move || {
                while !forward_stop.load(Ordering::Acquire) {
                    let event = match receiver.recv() {
                        Ok(event) => event,
                        Err(_) => break,
                    };
                    let shutdown = matches!(event, Event::Shutdown);
                    if let Ok(json) = serde_json::to_string(&event) {
                        if let Ok(text) = CString::new(json) {
                            callback(text.as_ptr(), user as *mut c_void);
                        }
                    }
                    if shutdown {
                        break;
                    }
                }
            }) {
            Ok(handle) => handle,
            Err(_) => return NAE_BASE_ERR_ARGUMENT,
        };
        base.forward = Some(handle);
        base.forward_stop = Some(stop);
        NAE_BASE_OK
    }))
    .unwrap_or(NAE_BASE_ERR_ARGUMENT)
}

/// The current snapshot as JSON. Caller owns the string; free with
/// `NAE_BaseFreeString`. Returns null on failure.
#[no_mangle]
pub extern "C" fn NAE_BaseSnapshot(base: *mut NAE_Base) -> *mut c_char {
    if base.is_null() {
        return std::ptr::null_mut();
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Some(base) = (unsafe { base.as_ref() }) else {
            return std::ptr::null_mut();
        };
        let Some(inner) = base.base.as_ref() else {
            return std::ptr::null_mut();
        };
        match serde_json::to_string(&inner.handle().snapshot()) {
            Ok(json) => match CString::new(json) {
                Ok(text) => text.into_raw(),
                Err(_) => std::ptr::null_mut(),
            },
            Err(_) => std::ptr::null_mut(),
        }
    }))
    .unwrap_or(std::ptr::null_mut())
}

/// Release a string returned by `NAE_BaseSnapshot`.
#[no_mangle]
pub extern "C" fn NAE_BaseFreeString(text: *mut c_char) {
    if text.is_null() {
        return;
    }
    unsafe {
        drop(CString::from_raw(text));
    }
}

/// Stop playback, join the workers and release Base. Safe on null.
#[no_mangle]
pub extern "C" fn NAE_BaseShutdown(base: *mut NAE_Base) {
    if base.is_null() {
        return;
    }
    let mut wrapper = unsafe { Box::from_raw(base) };
    if let Some(stop) = wrapper.forward_stop.take() {
        stop.store(true, Ordering::Release);
    }
    // Dropping Base sends Shutdown and joins the control/analysis workers. The
    // event thread then sees `Event::Shutdown` and exits.
    if let Some(inner) = wrapper.base.take() {
        drop(inner);
    }
    if let Some(thread) = wrapper.forward.take() {
        let _ = thread.join();
    }
}
