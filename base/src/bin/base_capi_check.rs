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

/// A long WAV, so a decode is still running when the cancel arrives. A short
/// file would finish first and the check would be measuring nothing.
fn write_long_wav(path: &std::path::Path, seconds: u32) {
    const RATE: u32 = 44_100;
    let frames = RATE * seconds;
    let mut wav = Vec::with_capacity(44 + (frames as usize) * 4);
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36 + frames * 4).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16u32.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&2u16.to_le_bytes());
    wav.extend_from_slice(&RATE.to_le_bytes());
    wav.extend_from_slice(&(RATE * 4).to_le_bytes());
    wav.extend_from_slice(&4u16.to_le_bytes());
    wav.extend_from_slice(&16u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&(frames * 4).to_le_bytes());
    for i in 0..frames {
        let t = i as f64 / RATE as f64;
        let s = ((2.0 * std::f64::consts::PI * 440.0 * t).sin() * 8000.0) as i16;
        wav.extend_from_slice(&s.to_le_bytes());
        wav.extend_from_slice(&s.to_le_bytes());
    }
    std::fs::write(path, wav).expect("write long wav");
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
    // A play must not publish a list nobody asked for: that is an event with no
    // subscriber, and the work behind it is wasted. Recording one play before
    // any list has been requested, and asserting that nothing arrives, is the
    // only way to see that rule.
    //
    // The log is cleared first: earlier sections have already produced events,
    // and reading an accumulated log would find one of those and report the
    // opposite of what this check is for.
    {
        let events = LIBRARY_EVENTS.get_or_init(|| Mutex::new(Vec::new()));
        events.lock().unwrap().clear();
    }
    assert_eq!(
        send(
            base,
            r#"{"command":"record_play","path":"/music/c.flac","at_ms":1700000000050}"#
        ),
        0
    );
    // The command is a message, so the control thread has to have handled it
    // before its silence means anything. The wait is bounded and short: it is
    // the time a `record_play` takes, not a guess at when the event would
    // arrive if one were coming.
    let settle = Instant::now() + Duration::from_millis(300);
    let mut quiet = false;
    while Instant::now() < settle {
        std::thread::sleep(Duration::from_millis(10));
        quiet = LIBRARY_EVENTS
            .get()
            .map(|events| {
                let guard = events.lock().unwrap();
                guard.iter().any(|v| v["event"] == "most_played_changed")
            })
            .unwrap_or(false);
        if quiet {
            break;
        }
    }
    assert!(
        !quiet,
        "a play with no watched list published one anyway"
    );

    // Ask for the list first: a play must not publish a list nobody asked for,
    // so the refresh only happens once a list is being watched. Requesting it
    // after the plays would instead mask a play that stored nothing.
    assert_eq!(send(base, r#"{"command":"list_most_played","limit":10}"#), 0);
    let _ = wait_for_event("most_played_changed");

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
    // The condition is the final count, not the length: the first event carrying
    // two entries is a state where only one of the two plays has landed.
    let played = wait_for_event_where("most_played_changed", |v| {
        v["tracks"].as_array().is_some_and(|a| {
            a.len() == 3 && a.iter().any(|t| t["play_count"] == 2)
        })
    });
    assert_eq!(played["tracks"][0]["path"], "/music/a.flac", "{played}");
    assert_eq!(played["tracks"][0]["play_count"], 2, "{played}");
    // Two tracks have one play each, and ties sort by path, so b comes before c.
    assert_eq!(played["tracks"][1]["path"], "/music/b.flac", "{played}");
    assert_eq!(played["tracks"][2]["path"], "/music/c.flac", "{played}");

    // ---- cancelling an analysis by id -------------------------------------
    //
    // A Swift host that starts a decode and then decides it does not want it
    // must be able to stop it. The id is how it says which one it means; the
    // decode stops between blocks, so the check has to start one on a file
    // large enough that it is still running when the cancel arrives.
    {
        let big = dir.join("big.wav");
        write_long_wav(&big, 300);
        let big_text = big.to_str().expect("UTF-8");
        assert_eq!(
            send(
                base,
                &format!(
                    r#"{{"command":"analyze","path":{big_text:?},"cancel_id":7}}"#
                )
            ),
            0
        );
        // Give the worker time to start, then cancel. A cancel that arrives
        // before the decode began is answered by the wanted-set check instead,
        // which is a different path.
        std::thread::sleep(Duration::from_millis(80));
        assert_eq!(
            send(base, r#"{"command":"cancel_analysis","cancel_id":7}"#),
            0
        );
        // A cancelled decode publishes nothing: its result is a prefix of a
        // measurement, not a measurement.
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut published = false;
        while Instant::now() < deadline {
            if let Some(events) = LIBRARY_EVENTS.get() {
                let guard = events.lock().unwrap();
                if guard
                    .iter()
                    .any(|v| v["event"] == "analysis_ready" && v["path"].as_str() == Some(big_text))
                {
                    published = true;
                    break;
                }
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        assert!(!published, "a cancelled decode published an analysis");
        let _ = std::fs::remove_file(&big);
    }

    // ---- resizing the analysis pool ---------------------------------------
    //
    // The pool is fixed at start today; a host that wants to trade decode
    // parallelism for CPU while a track plays has no way to say so. The event
    // reports the target, not the threads running, because shrinking is
    // gradual.
    assert_eq!(
        send(base, r#"{"command":"set_analysis_workers","count":1}"#),
        0
    );
    let shrunk = wait_for_event("analysis_workers_changed");
    assert_eq!(shrunk["count"], 1, "{shrunk}");
    assert_eq!(
        send(base, r#"{"command":"set_analysis_workers","count":3}"#),
        0
    );
    let grown = wait_for_event("analysis_workers_changed");
    assert_eq!(grown["count"], 3, "{grown}");
    // Out of range is clamped rather than refused: a host that asks for 0 gets
    // one worker, because a pool with no workers never finishes a request.
    assert_eq!(
        send(base, r#"{"command":"set_analysis_workers","count":0}"#),
        0
    );
    let clamped = wait_for_event("analysis_workers_changed");
    assert_eq!(clamped["count"], 1, "{clamped}");

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
