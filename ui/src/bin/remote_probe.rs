//! The remote layer's probe, in the spirit of the engine's `base_probe`:
//! point Base at a server (credentials from the environment, never the source
//! tree), list one page, fetch one item, read its cover, and report each
//! step. A path our interface repeats every session; measuring it here means
//! the first remote bug is one layer deep, not two UIs deep.
//!
//! ```text
//! cargo run --bin remote_probe
//! NEIRO_REMOTE_URL=http://host:8096     # Jellyfin scheme + authority, no slash
//! NEIRO_REMOTE_USER=char                # Jellyfin user (token form: NEIRO_REMOTE_TOKEN)
//! NEIRO_REMOTE_PASS=…                   # its password
//! NEIRO_REMOTE_KIND=jellyfin|navidrome  # default jellyfin
//! NEIRO_REMOTE_LIMIT=5                  # listing page size for the probe
//! ```

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use neiro_ui::engine::{Engine, WireEvent};

/// What the event reader accumulates for the driver's polls.
#[derive(Default)]
struct Observed {
    status: Option<String>,
    error: Option<String>,
    listed: Vec<(String, String, String)>,
    fetched: Option<(String, String)>,
    artwork: Option<(String, usize)>,
}

fn main() {
    let url = std::env::var("NEIRO_REMOTE_URL")
        .expect("NEIRO_REMOTE_URL（scheme + authority，无尾斜杠）");
    let kind = std::env::var("NEIRO_REMOTE_KIND").unwrap_or_else(|_| "jellyfin".into());
    let limit: u32 = std::env::var("NEIRO_REMOTE_LIMIT")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(5);

    let (engine, receiver) = Engine::open(&library_dir().join("library.db"))
        .expect("E_ENGINE: Base 无法启动");

    // The reader accumulates what each step needs; Base's errors surface
    // through the wait's failure path, not through this thread.
    let events: Arc<Mutex<Observed>> = Arc::new(Mutex::new(Observed::default()));
    let reader = {
        let events = Arc::clone(&events);
        std::thread::spawn(move || {
            while let Ok(line) = receiver.recv() {
                match neiro_ui::engine::parse(&line) {
                    Ok(WireEvent::RemoteChanged { .. }) => {
                        events.lock().expect("observed").status = Some(line);
                    }
                    Ok(WireEvent::Error { op, message }) => {
                        let error = format!("{op}: {message}");
                        eprintln!("[base 报错] {error}");
                        events.lock().expect("observed").error = Some(error);
                    }
                    Ok(WireEvent::RemoteListed { tracks }) => {
                        let mut seen = events.lock().expect("observed");
                        for track in tracks {
                            seen.listed.push((track.id, track.title, track.artist));
                        }
                    }
                    Ok(WireEvent::RemoteFetched { track, path }) => {
                        events.lock().expect("observed").fetched = Some((track.id, path));
                    }
                    Ok(WireEvent::RemoteArtwork { id, data, .. }) => {
                        let size = data.as_ref().map(|bytes| bytes.len()).unwrap_or(0);
                        events.lock().expect("observed").artwork = Some((id, size));
                    }
                    _ => {}
                }
            }
        })
    };

    send(
        &engine,
        &serde_json::json!({
            "command": "set_remote",
            "cache_dir": library_dir().join("remote"),
            "server": server(&kind, &url),
        }),
        "set_remote",
    );
    wait(
        &events,
        Duration::from_secs(15),
        |seen| seen.status.is_some() || seen.error.is_some(),
        "15s 内没有 remote_changed",
    );
    println!(
        "  [PASS] remote_changed 到了：{}",
        events.lock().expect("observed").status.as_deref().unwrap_or("")
    );

    send(
        &engine,
        &serde_json::json!({ "command": "list_remote", "start": 0, "limit": limit as u64 }),
        "list_remote",
    );
    wait(
        &events,
        Duration::from_secs(30),
        |seen| !seen.listed.is_empty(),
        "30s 内没有 remote_listed——列表为空或协议没跑通",
    );
    let seen = events.lock().expect("observed");
    let count = seen.listed.len();
    let first = seen.listed.first().expect("checked above").clone();
    drop(seen);
    println!("  [PASS] remote_listed {count} 条，第一项：{}｜{}", first.1, first.2);

    send(
        &engine,
        &serde_json::json!({ "command": "fetch_remote", "id": first.0 }),
        "fetch_remote",
    );
    wait(
        &events,
        Duration::from_secs(300),
        |seen| matches!(&seen.fetched, Some((id, _)) if *id == first.0),
        "5 分钟内没有 remote_fetched（下载超时或失败）",
    );
    let path = events
        .lock()
        .expect("observed")
        .fetched
        .as_ref()
        .expect("checked above")
        .1
        .clone();
    println!("  [PASS] remote_fetched：{path}");

    send(
        &engine,
        &serde_json::json!({ "command": "fetch_remote_artwork", "id": first.0 }),
        "fetch_remote_artwork",
    );
    wait(
        &events,
        Duration::from_secs(30),
        |seen| matches!(&seen.artwork, Some((id, _)) if *id == first.0),
        "30s 内没有 remote_artwork",
    );
    let artwork = events
        .lock()
        .expect("observed")
        .artwork
        .clone()
        .expect("checked above");
    println!(
        "  [PASS] remote_artwork：id {}，{} B（0 B 是服务器的正常「没图」回答）",
        artwork.0, artwork.1,
    );

    println!("\nAll remote checks passed.");
    drop(reader);
}

/// The value of a required credential variable; the message names the
/// variable, never a value.
fn required(name: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| panic!("{name} 未设置"))
}

/// The server object the command carries, one JSON exactly like the wire.
fn server(kind: &str, url: &str) -> serde_json::Value {
    match kind {
        "navidrome" => serde_json::json!({
            "kind": "navidrome",
            "base_url": url,
            "user": required("NEIRO_REMOTE_USER"),
            "password": required("NEIRO_REMOTE_PASS"),
        }),
        "jellyfin_token" => serde_json::json!({
            "kind": "jellyfin",
            "base_url": url,
            "auth": { "kind": "token", "token": required("NEIRO_REMOTE_TOKEN") },
        }),
        _ => serde_json::json!({
            "kind": "jellyfin",
            "base_url": url,
            "auth": {
                "kind": "password",
                "user": required("NEIRO_REMOTE_USER"),
                "password": required("NEIRO_REMOTE_PASS"),
            },
        }),
    }
}

/// One serialized command; a refusal aborts the probe loudly.
fn send(engine: &Engine, body: &serde_json::Value, op: &'static str) {
    let line = serde_json::to_string(body).expect("valid json");
    engine
        .command(&line)
        .unwrap_or_else(|error| panic!("[FAIL] {op} 被拒绝：{error}"));
}

/// Polls the observed state until `done` or the deadline, aborting on an
/// error Base reported.
fn wait(events: &Arc<Mutex<Observed>>, limit: Duration, done: impl Fn(&Observed) -> bool, message: &str) {
    let start = Instant::now();
    loop {
        let seen = events.lock().expect("observed");
        if seen.error.is_some() {
            let reason = seen.error.clone().expect("checked");
            eprintln!("[FAIL] {message}（Base 报错：{reason}）");
            std::process::exit(1);
        }
        if done(&seen) {
            return;
        }
        drop(seen);
        if start.elapsed() > limit {
            eprintln!("[FAIL] {message}");
            std::process::exit(1);
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

/// Where the probe keeps its library — beside the app's, so a cached
/// favourite never belongs to a temp directory that moved.
fn library_dir() -> std::path::PathBuf {
    std::env::var_os("LOCALAPPDATA")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::PathBuf::from("."))
        .join("Neiro")
}
