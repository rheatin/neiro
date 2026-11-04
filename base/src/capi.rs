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
// `not_unsafe_ptr_arg_deref`: these exports take raw pointers by necessity -- a
// C or Swift caller has nothing else to pass -- but they are deliberately safe
// to call from Rust. Every one of them null-checks its arguments before
// dereferencing, and every body runs under `catch_unwind`, so the worst a bad
// argument produces is an error code. Marking them `unsafe` would only move the
// burden to the Rust harnesses while changing nothing for the C side.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

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
    /// The ticket for the current subscription, so it can be given back.
    ticket: Option<crate::SubscriptionId>,
}

const NAE_BASE_OK: i32 = 0;
const NAE_BASE_ERR_ARGUMENT: i32 = -1;
const NAE_BASE_ERR_PARSE: i32 = -2;
const NAE_BASE_ERR_CLOSED: i32 = -3;
const NAE_BASE_ERR_ALREADY: i32 = -4;

/// Start Base with exclusive output and no library. Returns null when the
/// workers could not be spawned.
///
/// Library commands then report "no library is open" rather than an empty
/// library: "you have no playlists" is a statement about the listener's music,
/// not about this process.
#[no_mangle]
pub extern "C" fn NAE_BaseStart() -> *mut NAE_Base {
    start(neiro_audio::ShareKind::Exclusive, None)
}

/// Start Base with a library file for favourites, playlists and play counts.
///
/// The file is created, with its schema, when it does not exist; parent
/// directories are created too. Returns null when the library could not be
/// opened or the workers could not be spawned -- the two are not distinguished
/// here, because a host that cannot get either has nothing to do with the
/// result.
#[no_mangle]
pub extern "C" fn NAE_BaseStartWithLibrary(path: *const c_char) -> *mut NAE_Base {
    if path.is_null() {
        return std::ptr::null_mut();
    }
    let library = catch_unwind(AssertUnwindSafe(|| {
        let text = unsafe { CStr::from_ptr(path) }.to_str().ok()?;
        crate::library::Library::open(std::path::Path::new(text)).ok()
    }))
    .ok()
    .flatten();
    let Some(library) = library else {
        return std::ptr::null_mut();
    };
    start(neiro_audio::ShareKind::Exclusive, Some(library))
}

/// Start Base with an explicit share mode: 0 = exclusive, 1 = shared.
/// Shared is a choice for platforms where another process owns the endpoint;
/// it is not an automatic fallback.
#[no_mangle]
pub extern "C" fn NAE_BaseStartWithShare(share: i32) -> *mut NAE_Base {
    match share {
        0 => start(neiro_audio::ShareKind::Exclusive, None),
        1 => start(neiro_audio::ShareKind::Shared, None),
        _ => std::ptr::null_mut(),
    }
}

fn start(
    share: neiro_audio::ShareKind,
    library: Option<crate::library::Library>,
) -> *mut NAE_Base {
    let policy = match share {
        neiro_audio::ShareKind::Shared => crate::protocol::PlaybackPolicy::shared(),
        neiro_audio::ShareKind::Exclusive => crate::protocol::PlaybackPolicy::default(),
    };
    // The start call is inside the closure, not before it: a Base that panics
    // while spawning a worker must come back as null rather than unwind into the
    // C caller, and `catch_unwind` only catches what runs inside it.
    match catch_unwind(AssertUnwindSafe(|| match library {
        Some(library) => Base::start_with_policy_and_library(policy, library),
        None => Base::start_with_policy(policy),
    })) {
        Ok(Ok(base)) => Box::into_raw(Box::new(NAE_Base {
            base: Some(base),
            forward: None,
            forward_stop: None,
            ticket: None,
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
        let (ticket, receiver) = inner.handle().subscribe();
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
        base.ticket = Some(ticket);
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

/// Drop the current subscription and let its thread exit.
///
/// Without this a host that closes a window has to shut Base down entirely, and
/// a host with two windows cannot close one. Returns 0 on success and -4 when
/// there is no subscription to drop.
///
/// The callback is not invoked again once this returns; the forward thread
/// stops on the next event or immediately, whichever comes first.
#[no_mangle]
pub extern "C" fn NAE_BaseUnsubscribe(base: *mut NAE_Base) -> i32 {
    if base.is_null() {
        return NAE_BASE_ERR_ARGUMENT;
    }
    catch_unwind(AssertUnwindSafe(|| {
        let Some(base) = (unsafe { base.as_mut() }) else {
            return NAE_BASE_ERR_ARGUMENT;
        };
        let Some(ticket) = base.ticket.take() else {
            return NAE_BASE_ERR_ALREADY;
        };
        if let Some(inner) = base.base.as_ref() {
            inner.handle().unsubscribe(ticket);
        }
        if let Some(stop) = base.forward_stop.take() {
            stop.store(true, Ordering::Release);
        }
        if let Some(thread) = base.forward.take() {
            let _ = thread.join();
        }
        NAE_BASE_OK
    }))
    .unwrap_or(NAE_BASE_ERR_ARGUMENT)
}

/// Stop playback, join the workers and release Base. Safe on null.
#[no_mangle]
pub extern "C" fn NAE_BaseShutdown(base: *mut NAE_Base) {
    if base.is_null() {
        return;
    }
    let mut wrapper = unsafe { Box::from_raw(base) };
    if let Some(ticket) = wrapper.ticket.take() {
        if let Some(inner) = wrapper.base.as_ref() {
            inner.handle().unsubscribe(ticket);
        }
    }
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
