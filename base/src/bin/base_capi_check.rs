//! Checks the C ABI Swift will call: start, JSON command, snapshot, callback,
//! string ownership and shutdown. No device is opened.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use neiro_base::capi::{
    NAE_Base, NAE_BaseCommand, NAE_BaseFreeString, NAE_BaseShutdown, NAE_BaseSnapshot,
    NAE_BaseStart, NAE_BaseStartWithLibrary, NAE_BaseSubscribe, NAE_BaseUnsubscribe,
};

static EVENTS: AtomicUsize = AtomicUsize::new(0);

/// Every event the library section sees, so a check can wait for the one it
/// needs. A global because the C callback has no user-data path that carries
/// Rust state here; the section is single-threaded.
static LIBRARY_EVENTS: std::sync::OnceLock<Mutex<Vec<serde_json::Value>>> = std::sync::OnceLock::new();

extern "C" fn on_library_event(json: *const c_char, _user: *mut c_void) {
    if json.is_null() {
        return;
    }
    let text = unsafe { CStr::from_ptr(json) }.to_string_lossy().into_owned();
    if let Ok(value) = serde_json::from_str::<serde_json::Value>(&text) {
        if let Some(events) = LIBRARY_EVENTS.get() {
            events.lock().unwrap().push(value);
        }
    }
}

/// Wait for one event of the given kind and return it.
///
/// The tag is `event` in the serialized form, so the check asserts the wire
/// format the host will actually see rather than the Rust variant name.
fn wait_for_event(kind: &str) -> serde_json::Value {
    wait_for_event_where(kind, |_| true)
}

/// The same, for an event that has to carry something.
///
/// Creating a playlist emits its (empty) entry list, and adding to it emits the
/// same event with contents; a check that only matched the tag would read the
/// empty one and conclude the add did nothing.
fn wait_for_event_where(
    kind: &str,
    accept: impl Fn(&serde_json::Value) -> bool,
) -> serde_json::Value {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        if let Some(events) = LIBRARY_EVENTS.get() {
            let mut guard = events.lock().unwrap();
            if let Some(pos) = guard
                .iter()
                .position(|v| v["event"] == kind && accept(v))
            {
                return guard.remove(pos);
            }
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    panic!("no {kind} event arrived");
}

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

    // ---- library through the C ABI ---------------------------------------
    //
    // The whole point of this layer is that a Swift or Android host can reach
    // it, and the only way to know that is to drive it the way a host does:
    // JSON in, JSON out, no Rust types on the wire.
    //
    // The answers arrive as events, so the check reads the callback stream
    // rather than the snapshot: favourites and playlists are not part of the
    // snapshot, which is about the stream.
    let dir = std::env::temp_dir().join("neiro-capi-library");
    let _ = std::fs::remove_dir_all(&dir);
    let db = dir.join("library.db");
    let db_text = CString::new(db.to_str().expect("temp path is UTF-8")).unwrap();
    let base = NAE_BaseStartWithLibrary(db_text.as_ptr());
    assert!(!base.is_null(), "Base starts with a library");

    // Every event is recorded, so a check can look for the one it needs without
    // draining the others.
    {
        let events = LIBRARY_EVENTS.get_or_init(|| Mutex::new(Vec::new()));
        events.lock().unwrap().clear();
    }
    assert_eq!(NAE_BaseSubscribe(base, Some(on_library_event), std::ptr::null_mut()), 0);

    // Takes the handle as a parameter rather than capturing it: the restart
    // below replaces `base`, and a closure that captured the old pointer would
    // keep sending to a shutdown Base.
    let send = |base: *mut NAE_Base, json: &str| {
        let text = CString::new(json).unwrap();
        NAE_BaseCommand(base, text.as_ptr())
    };

    // `set_favourite` publishes the list itself, so no explicit list command is
    // needed -- and issuing one would mask a `set` that published nothing, which
    // is the failure this check is for.
    assert_eq!(send(base, r#"{"command":"set_favourite","path":"/music/a.flac","on":true}"#), 0);
    let favourites = wait_for_event("favourites_changed");
    assert_eq!(
        favourites["paths"][0], "/music/a.flac",
        "the favourite round-trips through the ABI: {favourites}"
    );

    assert_eq!(send(base, r#"{"command":"create_playlist","name":"Evening"}"#), 0);
    let playlists = wait_for_event("playlists_changed");
    assert_eq!(playlists["playlists"][0]["name"], "Evening", "{playlists}");
    let id = playlists["playlists"][0]["id"].as_i64().expect("playlist id");

    assert_eq!(
        send(base, &format!(
            r#"{{"command":"add_to_playlist","id":{id},"path":"/music/a.flac"}}"#
        )),
        0
    );
    let entries = wait_for_event_where("playlist_entries_changed", |v| {
        v["entries"].as_array().is_some_and(|a| !a.is_empty())
    });
    assert_eq!(entries["entries"][0]["path"], "/music/a.flac", "{entries}");

    // A duplicate name is refused by the schema's unique index, and the host
    // learns it from an error event rather than from a silent success.
    assert_eq!(send(base, r#"{"command":"create_playlist","name":"evening"}"#), 0);
    let error = wait_for_event("error");
    assert!(
        error["message"].as_str().unwrap_or("").contains("evening"),
        "a duplicate name is reported: {error}"
    );

    // The library survives a restart, which is the difference between a
    // favourite and a favourite-for-this-run.
    NAE_BaseShutdown(base);
    let base = NAE_BaseStartWithLibrary(db_text.as_ptr());
    assert!(!base.is_null(), "Base reopens the same library");
    {
        let events = LIBRARY_EVENTS.get_or_init(|| Mutex::new(Vec::new()));
        events.lock().unwrap().clear();
    }
    assert_eq!(NAE_BaseSubscribe(base, Some(on_library_event), std::ptr::null_mut()), 0);
    assert_eq!(send(base, r#"{"command":"list_favourites"}"#), 0);
    let favourites = wait_for_event("favourites_changed");
    assert_eq!(
        favourites["paths"][0], "/music/a.flac",
        "the favourite survived the restart: {favourites}"
    );

    // ---- play counts ------------------------------------------------------
    //
    // The count is the host's clock and Base's storage. Recording twice must
    // show up as two, and the list must be ordered rather than merely non-empty.
    assert_eq!(
        send(
            base,
            r#"{"command":"record_play","path":"/music/a.flac","at_ms":1700000000000}"#
        ),
        0
    );
    assert_eq!(
        send(
            base,
            r#"{"command":"record_play","path":"/music/b.flac","at_ms":1700000000100}"#
        ),
        0
    );
    assert_eq!(
        send(
            base,
            r#"{"command":"record_play","path":"/music/a.flac","at_ms":1700000000200}"#
        ),
        0
    );
    // No explicit `list_most_played` here: `record_play` republishes the list,
    // and issuing the list command afterwards would mask a play that stored
    // nothing -- the check would read the list the command produced and pass.
    // That is the same mistake the favourite check had, caught the same way.
    //
    // The condition is the final count, not the length: the first event carrying
    // two entries is a state where only one of the two plays has landed.
    let played = wait_for_event_where("most_played_changed", |v| {
        v["tracks"]
            .as_array()
            .is_some_and(|a| a.iter().any(|t| t["play_count"] == 2))
    });
    assert_eq!(played["tracks"][0]["path"], "/music/a.flac", "{played}");
    assert_eq!(played["tracks"][0]["play_count"], 2, "{played}");
    assert_eq!(played["tracks"][1]["play_count"], 1, "{played}");

    // ---- unsubscribe ------------------------------------------------------
    //
    // A host with two windows closes one without shutting Base down. The
    // callback must stop, and the same handle must be subscribable again.
    assert_eq!(NAE_BaseUnsubscribe(base), 0, "unsubscribe succeeds");
    assert_eq!(
        NAE_BaseUnsubscribe(base),
        -4,
        "a second unsubscribe reports there was nothing to drop"
    );
    {
        let events = LIBRARY_EVENTS.get_or_init(|| Mutex::new(Vec::new()));
        events.lock().unwrap().clear();
    }
    assert_eq!(NAE_BaseSubscribe(base, Some(on_library_event), std::ptr::null_mut()), 0);
    assert_eq!(send(base, r#"{"command":"list_favourites"}"#), 0);
    let favourites = wait_for_event("favourites_changed");
    assert_eq!(favourites["paths"][0], "/music/a.flac", "{favourites}");

    NAE_BaseShutdown(base);
    let _ = std::fs::remove_dir_all(&dir);
    println!("All Base C ABI checks passed.");
}
