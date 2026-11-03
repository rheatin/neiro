//! Checks the C ABI Swift will call: start, JSON command, snapshot, callback,
//! string ownership and shutdown. No device is opened.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

use neiro_base::capi::{
    NAE_BaseCommand, NAE_BaseFreeString, NAE_BaseShutdown, NAE_BaseSnapshot, NAE_BaseStart,
    NAE_BaseSubscribe,
};

static EVENTS: AtomicUsize = AtomicUsize::new(0);

extern "C" fn on_event(json: *const c_char, _user: *mut c_void) {
    if json.is_null() {
        return;
    }
    let text = unsafe { CStr::from_ptr(json) }.to_string_lossy();
    assert!(text.starts_with('{'), "event is JSON: {text}");
    EVENTS.fetch_add(1, Ordering::SeqCst);
}

fn main() {
    let base = NAE_BaseStart();
    assert!(!base.is_null(), "Base starts");

    let ptr = NAE_BaseSnapshot(base);
    assert!(!ptr.is_null(), "snapshot is available");
    let json = unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned();
    NAE_BaseFreeString(ptr);
    let snapshot: serde_json::Value = serde_json::from_str(&json).expect("snapshot JSON");
    assert_eq!(snapshot["state"], "stopped");
    assert!(snapshot["queue"].is_array());

    assert_eq!(NAE_BaseSubscribe(base, Some(on_event), std::ptr::null_mut()), 0);
    assert_eq!(NAE_BaseSubscribe(base, Some(on_event), std::ptr::null_mut()), -4);

    let bad = CString::new("not json").unwrap();
    assert_eq!(NAE_BaseCommand(base, bad.as_ptr()), -2);

    let stop = CString::new(r#"{"command":"stop"}"#).unwrap();
    assert_eq!(NAE_BaseCommand(base, stop.as_ptr()), 0);

    // The policy types are part of the serialized contract, so a Swift client
    // sets the output mode through the same JSON command, with no second entry
    // point to keep in sync.
    let policy = CString::new(
        r#"{"command":"set_policy","policy":{"rate":{"mode":"fixed","rate_hz":96000},"quality":"very_high","share":"shared"}}"#,
    )
    .unwrap();
    assert_eq!(NAE_BaseCommand(base, policy.as_ptr()), 0);

    // A command is a message to the control thread, not a synchronous call.
    // Reading the snapshot once right after it returns the state before the
    // message was handled, so wait for the state being asserted.
    let deadline = Instant::now() + Duration::from_secs(3);
    let mut json = String::new();
    let mut snapshot = serde_json::Value::Null;
    while Instant::now() < deadline {
        let ptr = NAE_BaseSnapshot(base);
        assert!(!ptr.is_null(), "snapshot after set_policy");
        json = unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned();
        NAE_BaseFreeString(ptr);
        snapshot = serde_json::from_str(&json).expect("snapshot JSON");
        if snapshot["policy"]["rate"]["mode"] == "fixed" {
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    assert_eq!(snapshot["policy"]["rate"]["mode"], "fixed", "{json}");
    assert_eq!(snapshot["policy"]["rate"]["rate_hz"], 96000, "{json}");
    assert_eq!(snapshot["policy"]["quality"], "very_high", "{json}");
    assert_eq!(snapshot["policy"]["share"], "shared", "{json}");

    let deadline = Instant::now() + Duration::from_secs(2);
    while EVENTS.load(Ordering::SeqCst) == 0 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(EVENTS.load(Ordering::SeqCst) > 0, "state event reached C callback");

    NAE_BaseShutdown(base);
    println!("All Base C ABI checks passed.");
}
