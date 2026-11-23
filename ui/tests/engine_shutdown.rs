// Dropping the engine while a remote listing is in flight must not wait for
// the network: Base's shutdown joins a worker that may sit in the request,
// and at quit that froze the window (the "quit while Jellyfin loads" hang).
use std::io::Read;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use std::time::{Duration, Instant};

use neiro_ui::engine::Engine;

#[test]
fn drop_does_not_wait_for_a_stuck_remote_request() {
    let listener = std::net::TcpListener::bind("127.0.0.1:0").expect("bind");
    let port = listener.local_addr().unwrap().port();
    // The server reads each request and never answers: the listing stays in
    // flight for as long as the worker lives.
    let accepted = Arc::new(AtomicUsize::new(0));
    let seen = accepted.clone();
    std::thread::spawn(move || {
        for stream in listener.incoming() {
            let Ok(mut s) = stream else { continue };
            seen.fetch_add(1, Ordering::SeqCst);
            let mut buf = [0u8; 4096];
            let _ = s.read(&mut buf);
            std::thread::sleep(Duration::from_secs(120));
        }
    });

    let dir = std::env::temp_dir().join(format!("neiro-shutdown-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let db = dir.join("library.db");
    let (engine, _rx) = match Engine::open(&db) {
        Ok(opened) => opened,
        Err(error) => {
            // No engine SDK beside this checkout: nothing to test here.
            eprintln!("skip: engine not openable: {error}");
            return;
        }
    };

    let set = serde_json::json!({
        "command": "set_remote",
        "cache_dir": dir.join("remote").to_string_lossy(),
        "server": {
            "kind": "jellyfin",
            "base_url": format!("http://127.0.0.1:{port}"),
            "auth": { "kind": "token", "token": "test" },
            "params": {},
        },
    });
    engine.command(&set.to_string()).expect("set_remote");
    std::thread::sleep(Duration::from_millis(300));
    engine
        .command(r#"{"command":"list_remote","start":0,"limit":200}"#)
        .expect("list_remote");
    // Wait for the request to reach the server, so the worker is inside it.
    let deadline = Instant::now() + Duration::from_secs(5);
    while accepted.load(Ordering::SeqCst) == 0 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_eq!(accepted.load(Ordering::SeqCst), 1, "the listing never reached the server");

    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || {
        let started = Instant::now();
        drop(engine);
        let _ = tx.send(started.elapsed());
    });
    let elapsed = rx.recv_timeout(Duration::from_secs(10)).expect("drop never returned");
    assert!(
        elapsed >= Duration::from_millis(400),
        "drop returned in {elapsed:?} — it should have waited for the 500ms shutdown timeout rather than skipping shutdown"
    );
    assert!(
        elapsed < Duration::from_secs(5),
        "dropping the engine waited {elapsed:?} for a stuck remote request"
    );
}

#[test]
fn a_quiet_engine_shuts_down_within_the_wait() {
    let dir = std::env::temp_dir().join(format!("neiro-quiet-stop-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let (engine, rx) = match Engine::open(&dir.join("library.db")) {
        Ok(opened) => opened,
        Err(error) => {
            eprintln!("skip: engine not openable: {error}");
            return;
        }
    };
    let started = Instant::now();
    drop(engine);
    let elapsed = started.elapsed();
    eprintln!("quiet shutdown took {elapsed:?}");
    assert!(
        elapsed < Duration::from_millis(450),
        "a quiet shutdown took {elapsed:?}: it should finish inside the 500ms wait"
    );
    assert!(
        matches!(
            rx.recv_timeout(Duration::from_millis(200)),
            Err(mpsc::RecvTimeoutError::Disconnected)
        ),
        "dropping the engine must unsubscribe and drop the event sender"
    );
}
