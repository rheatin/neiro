//! Jellyfin client against a stub server.
//!
//! There is no Jellyfin instance to point at here, and a probe that needed one
//! would only run on the machine that has it. So this starts a server on the
//! loopback that speaks the three endpoints the client uses, and checks the
//! part the unit tests cannot: that the request carries the token, that the
//! answer is read the way the API documents it, and that a download lands as a
//! whole file or not at all.
//!
//! What it does not prove: that Jellyfin's real answers match this stub. That
//! needs a server, and is the one thing a fixture cannot stand in for.

use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{self, Receiver};
use std::thread;

use neiro_base::remote::{self, RemoteError, RemoteTrack, Server};

const TOKEN: &str = "probe-token";

fn fail(why: &str) -> ! {
    eprintln!("FAIL: {why}");
    std::process::exit(1);
}

fn ok(what: &str) {
    println!("  ok   {what}");
}

fn check(pass: bool, label: &str, detail: String) {
    println!(
        "  [{}] {:<52} {}",
        if pass { "PASS" } else { "FAIL" },
        label,
        detail
    );
    if !pass {
        std::process::exit(1);
    }
}

/// A 0.1 s WAV, so a downloaded file is a real container and not random bytes.
fn wav_bytes() -> Vec<u8> {
    let frames: u32 = 4410;
    let mut w = Vec::with_capacity(44 + (frames as usize) * 2);
    w.extend_from_slice(b"RIFF");
    w.extend_from_slice(&(36 + frames * 2).to_le_bytes());
    w.extend_from_slice(b"WAVEfmt ");
    w.extend_from_slice(&16u32.to_le_bytes());
    w.extend_from_slice(&1u16.to_le_bytes());
    w.extend_from_slice(&1u16.to_le_bytes());
    w.extend_from_slice(&44_100u32.to_le_bytes());
    w.extend_from_slice(&(44_100u32 * 2).to_le_bytes());
    w.extend_from_slice(&2u16.to_le_bytes());
    w.extend_from_slice(&16u16.to_le_bytes());
    w.extend_from_slice(b"data");
    w.extend_from_slice(&(frames * 2).to_le_bytes());
    for i in 0..frames {
        let v = ((i as f32 * 0.02).sin() * 6000.0) as i16;
        w.extend_from_slice(&v.to_le_bytes());
    }
    w
}

/// One request the stub received, so the probe can assert what was sent.
struct Seen {
    path: String,
    headers: Vec<String>,
}

fn serve(listener: TcpListener, seen: mpsc::Sender<Seen>) {
    for stream in listener.incoming() {
        let Ok(stream) = stream else { return };
        if handle(stream, &seen).is_err() {
            return;
        }
    }
}

fn handle(mut stream: TcpStream, seen: &mpsc::Sender<Seen>) -> std::io::Result<()> {
    let mut reader = BufReader::new(stream.try_clone()?);
    let mut line = String::new();
    if reader.read_line(&mut line)? == 0 {
        return Ok(());
    }
    let path = line.split_whitespace().nth(1).unwrap_or("").to_owned();

    let mut headers = Vec::new();
    loop {
        let mut header = String::new();
        if reader.read_line(&mut header)? == 0 {
            break;
        }
        let trimmed = header.trim_end().to_owned();
        if trimmed.is_empty() {
            break;
        }
        headers.push(trimmed);
    }
    let _ = seen.send(Seen {
        path: path.clone(),
        headers: headers.clone(),
    });

    let authorised = headers.iter().any(|h| {
        let lower = h.to_ascii_lowercase();
        lower.starts_with("x-emby-token:") && h.contains(TOKEN)
            || lower.starts_with("authorization:") && h.contains(TOKEN)
    });
    if !authorised {
        return respond(&mut stream, 401, "application/json", b"{\"error\":\"unauthorised\"}");
    }

    // Ordered narrowly: the playlist entry query also asks for Audio items, so
    // this branch has to exclude it or it answers with the whole library.
    if path.starts_with("/Items?ParentId=pl-1") {
        let body = serde_json::json!({
            "Items": [{
                "Id": "aaa111", "Name": "Salt", "AlbumArtist": "Elin Vahl",
                "Album": "Northbound", "RunTimeTicks": 23_600_000_000u64,
                "MediaSources": [{ "Container": "wav", "Size": wav_bytes().len() }]
            }],
            "TotalRecordCount": 1
        });
        let text = body.to_string();
        return respond(&mut stream, 200, "application/json", text.as_bytes());
    }
    if path.starts_with("/Items?")
        && path.contains("IncludeItemTypes=Audio")
        && !path.contains("ParentId=")
    {
        let body = serde_json::json!({
            "Items": [
                {
                    "Id": "aaa111",
                    "Name": "Salt",
                    "AlbumArtist": "Elin Vahl",
                    "Album": "Northbound",
                    "RunTimeTicks": 23_600_000_000u64,
                    "MediaSources": [{ "Container": "WAV", "Size": wav_bytes().len() }]
                },
                {
                    "Id": "bbb222",
                    "Name": "Harbour Lights",
                    "Artists": ["Elin Vahl"],
                    "Album": "Northbound",
                    "RunTimeTicks": 56_200_000_000u64,
                    "MediaSources": [{ "Container": "flac" }]
                }
            ],
            "TotalRecordCount": 2
        });
        let text = body.to_string();
        return respond(&mut stream, 200, "application/json", text.as_bytes());
    }

    if path.starts_with("/Items/aaa111/Images/Primary") {
        return respond(&mut stream, 200, "image/jpeg", b"\xff\xd8\xff\xe0 fake jpeg");
    }
    if path.starts_with("/Items?IncludeItemTypes=Playlist") {
        if !path.contains("IncludeItemTypes=Playlist") {
            return respond(&mut stream, 200, "application/json", b"{}");
        }
        let body = serde_json::json!({
            "Items": [{
                "Id": "pl-1", "Name": "Evening", "ChildCount": 2,
                "RunTimeTicks": 3_000_000_000u64
            }],
            "TotalRecordCount": 1
        });
        let text = body.to_string();
        return respond(&mut stream, 200, "application/json", text.as_bytes());
    }
    if path == "/Items/aaa111/Download" {
        return respond(&mut stream, 200, "audio/wav", &wav_bytes());
    }
    if path == "/Items/missing/Download" {
        return respond(&mut stream, 404, "application/json", b"{\"error\":\"gone\"}");
    }

    respond(&mut stream, 404, "application/json", b"{}")
}

fn respond(
    stream: &mut TcpStream,
    code: u16,
    content_type: &str,
    body: &[u8],
) -> std::io::Result<()> {
    let head = format!(
        "HTTP/1.1 {code} X\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    stream.write_all(head.as_bytes())?;
    stream.write_all(body)?;
    stream.flush()
}

/// Wait for one event matching the predicate, with a bounded budget.
fn wait_event(
    rx: &std::sync::mpsc::Receiver<neiro_base::protocol::Event>,
    ms: u64,
    mut want: impl FnMut(&neiro_base::protocol::Event) -> bool,
) -> neiro_base::protocol::Event {
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(ms);
    loop {
        let left = deadline.saturating_duration_since(std::time::Instant::now());
        if left.is_zero() {
            fail("no matching event arrived");
        }
        match rx.recv_timeout(left) {
            Ok(event) => {
                if want(&event) {
                    return event;
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                fail("the event channel closed")
            }
        }
    }
}

fn main() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind a loopback port");
    let addr = listener.local_addr().expect("local addr");
    let (tx, seen): (mpsc::Sender<Seen>, Receiver<Seen>) = mpsc::channel();
    thread::spawn(move || serve(listener, tx));

    let config = Server::Jellyfin {
        base_url: format!("http://{addr}"),
        auth: neiro_base::remote::JellyfinAuth::Token {
            token: TOKEN.into(),
        },
    };

    let dest = std::env::temp_dir().join(format!("neiro-jellyfin-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dest);

    println!("=== stub at {addr} ===");

    // ---- the token reaches the server ------------------------------------
    let tracks = remote::list_audio(&config, 0, 50).expect("list");
    let first = seen.recv().expect("a request was seen");
    check(
        first
            .headers
            .iter()
            .any(|h| h.to_ascii_lowercase().starts_with("x-emby-token:") && h.contains(TOKEN)),
        "the request carries the token",
        first.path.clone(),
    );
    check(
        first.path.contains("IncludeItemTypes=Audio") && first.path.contains("Recursive=true"),
        "the listing asks for audio items recursively",
        first.path.clone(),
    );

    // ---- the answer is read as documented --------------------------------
    check(tracks.len() == 2, "two items parsed", format!("{}", tracks.len()));
    let salt = tracks.iter().find(|t| t.id == "aaa111").expect("the wav item");
    check(salt.container == "wav", "the container is lower-cased", salt.container.clone());
    check(salt.duration_ms == 2_360_000, "ticks became milliseconds", format!("{}", salt.duration_ms));
    check(salt.artist == "Elin Vahl", "AlbumArtist is used when present", salt.artist.clone());
    let harbour = tracks.iter().find(|t| t.id == "bbb222").expect("the flac item");
    check(
        harbour.artist == "Elin Vahl",
        "Artists[0] is used when AlbumArtist is absent",
        harbour.artist.clone(),
    );

    // ---- a download lands whole ------------------------------------------
    let want = wav_bytes();
    let path = remote::download(&config, salt, &dest).expect("download");
    let _ = seen.recv();
    check(path.exists(), "the file exists", path.display().to_string());
    check(
        path.file_name().and_then(|n| n.to_str()) == Some("aaa111.wav"),
        "it is named after the item",
        path.display().to_string(),
    );
    let got = std::fs::read(&path).expect("read it back");
    check(
        got == want,
        "the bytes are the bytes the server sent",
        format!("{} vs {}", got.len(), want.len()),
    );
    check(
        !std::fs::read_dir(&dest)
            .unwrap()
            .filter_map(|e| e.ok())
            .any(|e| e.file_name().to_string_lossy().contains(".part")),
        "no partial file is left behind",
        String::new(),
    );

    // ---- the same item lands on the same path ----------------------------
    let again = remote::download(&config, salt, &dest).expect("download again");
    let _ = seen.recv();
    check(
        again == path,
        "a second download reuses the path, so favourites keep resolving",
        again.display().to_string(),
    );

    // ---- failures are reported, not played -------------------------------
    let missing = RemoteTrack {
        id: "missing".into(),
        title: "x".into(),
        artist: "x".into(),
        album: "x".into(),
        container: "wav".into(),
        duration_ms: 0,
        size: None,
        cover_art: None,
    };
    match remote::download(&config, &missing, &dest) {
        Err(RemoteError::Status { code, .. }) => {
            ok(&format!("a missing item is an error, code {code}"))
        }
        other => fail(&format!("a missing item returned {other:?}")),
    }
    check(
        !remote::cache_path(&dest, &missing).exists(),
        "and leaves no file behind",
        remote::cache_path(&dest, &missing).display().to_string(),
    );

    // A catalogue that disagrees with the file is an error: one of the two is
    // wrong, and the listener would hear whichever arrived.
    let mut lying = salt.clone();
    lying.size = Some(want.len() as u64 + 1);
    match remote::download(&config, &lying, &dest) {
        Err(RemoteError::Shape(message)) => ok(&format!("a size mismatch is an error: {message}")),
        other => fail(&format!("a size mismatch returned {other:?}")),
    }

    // A bad token is refused by the server, and the client reports what the
    // server said rather than retrying or inventing a library.
    let wrong = Server::Jellyfin {
        base_url: config.base_url().to_string(),
        auth: neiro_base::remote::JellyfinAuth::Token {
            token: "not-the-token".into(),
        },
    };
    match remote::list_audio(&wrong, 0, 10) {
        Err(RemoteError::Status { code, .. }) => {
            check(code == 401, "a bad token is reported as 401", format!("{code}"))
        }
        other => fail(&format!("a bad token returned {other:?}")),
    }

    // ---- the cache may be created, but never escapes ----------------------
    let nested = dest.join("deeper");
    let path = remote::download(&config, salt, &nested).expect("download into a new dir");
    let _ = seen.recv();
    check(
        path.parent() == Some(nested.as_path()) && nested.exists(),
        "a missing cache directory is created",
        path.display().to_string(),
    );

    // ---- the session, which is what the protocol drives ------------------
    //
    // The raw functions above are the client; this is the state machine a host
    // actually talks to: configure, list, then fetch by id -- an id that can
    // only name something the server reported.
    let mut session = remote::Session::default();
    match session.run(remote::Job::Fetch { id: "aaa111".into() }) {
        neiro_base::protocol::Event::Error { op, .. } => {
            check(op == "remote", "a fetch before a listing is an error", op)
        }
        other => fail(&format!("a fetch before a listing returned {other:?}")),
    }

    match session.run(remote::Job::Configure {
        config: Some(config.clone()),
        cache_dir: Some(dest.clone()),
    }) {
        neiro_base::protocol::Event::RemoteChanged {
            configured,
            base_url,
            ..
        } => {
            check(configured, "configured after SetRemote", String::new());
            check(
                base_url.as_deref() == Some(config.base_url()),
                "the event names the server",
                format!("{base_url:?}"),
            );
        }
        other => fail(&format!("Configure returned {other:?}")),
    }

    match session.run(remote::Job::List { start: 0, limit: 10 }) {
        neiro_base::protocol::Event::RemoteListed { tracks } => {
            check(tracks.len() == 2, "the session lists what the server sent", format!("{}", tracks.len()))
        }
        other => fail(&format!("List returned {other:?}")),
    }

    match session.run(remote::Job::Fetch { id: "aaa111".into() }) {
        neiro_base::protocol::Event::RemoteFetched { track, path } => {
            check(track.id == "aaa111", "the fetch names what it fetched", track.id.clone());
            check(path.exists(), "and the path exists", path.display().to_string());
        }
        other => fail(&format!("Fetch returned {other:?}")),
    }

    match session.run(remote::Job::Fetch { id: "not-listed".into() }) {
        neiro_base::protocol::Event::Error { message, .. } => check(
            message.contains("not-listed"),
            "an id the listing never held is refused",
            message,
        ),
        other => fail(&format!("an unlisted id returned {other:?}")),
    }

    // Disconnecting is an empty token, and the listing it held is gone with it:
    // fetching afterwards must not use a server the host just disconnected.
    match session.run(remote::Job::Configure {
        config: None,
        cache_dir: Some(dest.clone()),
    }) {
        neiro_base::protocol::Event::RemoteChanged { configured, .. } => {
            check(!configured, "an empty config disconnects", String::new())
        }
        other => fail(&format!("disconnecting returned {other:?}")),
    }
    match session.run(remote::Job::Fetch { id: "aaa111".into() }) {
        neiro_base::protocol::Event::Error { .. } => ok("a fetch after disconnecting is an error"),
        other => fail(&format!("a fetch after disconnecting returned {other:?}")),
    }

    // ---- through a live Base, which is what a host drives ----------------
    //
    // The session above is the state machine; this proves the commands reach it
    // over the same channel a UI uses, and that the answer arrives as an event.
    let base = neiro_base::Base::start().expect("start Base");
    let handle = base.handle();
    let (_, events) = handle.subscribe();
    handle
        .set_remote(Some(config.clone()), &dest)
        .expect("set_remote");
    handle.list_remote(0, 10).expect("list_remote");

    let listing = wait_event(&events, 5000, |e| {
        matches!(e, neiro_base::protocol::Event::RemoteListed { .. })
    });
    match listing {
        neiro_base::protocol::Event::RemoteListed { tracks } => {
            check(tracks.len() == 2, "the commands reach the worker", format!("{}", tracks.len()))
        }
        other => fail(&format!("listing returned {other:?}")),
    }

    handle.fetch_remote("aaa111").expect("fetch_remote");
    let fetched = wait_event(&events, 10000, |e| {
        matches!(e, neiro_base::protocol::Event::RemoteFetched { .. })
    });
    let path = match fetched {
        neiro_base::protocol::Event::RemoteFetched { path, .. } => path,
        other => fail(&format!("fetch returned {other:?}")),
    };
    check(path.exists(), "the file is on disk", path.display().to_string());

    // The path is an ordinary path, so everything downstream works unchanged.
    // Asserted on the bytes rather than on a call returning: the file has to be
    // the one the server sent, not merely openable.
    let head = std::fs::read(&path).unwrap_or_default();
    check(
        head.len() > 12 && &head[..4] == b"RIFF" && &head[8..12] == b"WAVE",
        "the download is the container the server sent",
        format!("{} bytes", head.len()),
    );
    // And it opens through the same tag reader the rest of Base uses.
    let _ = neiro_base::tag::read_tags(&path).expect("the download opens as a container");
    handle.shutdown().expect("shutdown");
    drop(base);

    // ---- cover art -------------------------------------------------------
    let art = remote::fetch_artwork(&config, salt)
        .expect("artwork")
        .expect("this track has a cover");
    check(
        art.mime.starts_with("image/"),
        "the cover comes back as an image",
        art.mime.clone(),
    );

    // ---- playlists -------------------------------------------------------
    let lists = remote::list_playlists(&config).expect("playlists");
    check(lists.len() == 1, "one playlist", format!("{}", lists.len()));
    check(
        lists[0].track_count == 2,
        "its count comes from the server",
        format!("{}", lists[0].track_count),
    );
    let entries = remote::playlist_tracks(&config, &lists[0].id, 0, 10).expect("entries");
    check(
        entries.len() == 1 && entries[0].id == "aaa111",
        "the entries come back as tracks",
        format!("{:?}", entries.iter().map(|t| t.id.clone()).collect::<Vec<_>>()),
    );

    // ---- a cover is fetched once -----------------------------------------
    while seen.try_recv().is_ok() {}
    let mut session = remote::Session::default();
    session.run(remote::Job::Configure {
        config: Some(config.clone()),
        cache_dir: Some(dest.clone()),
    });
    session.run(remote::Job::List { start: 0, limit: 10 });
    let _first = session.run(remote::Job::FetchArtwork {
        id: "aaa111".into(),
    });
    std::thread::sleep(std::time::Duration::from_millis(150));
    while seen.try_recv().is_ok() {}
    let _second = session.run(remote::Job::FetchArtwork {
        id: "aaa111".into(),
    });
    match seen.recv_timeout(std::time::Duration::from_millis(400)) {
        Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
            println!("  ok   the second cover did not reach the server")
        }
        Ok(request) => fail(&format!("the second cover was fetched again: {}", request.path)),
        Err(other) => fail(&format!("unexpected channel state: {other:?}")),
    }

    let _ = std::fs::remove_dir_all(&dest);
    println!("\nJELLYFIN PROBE PASSED");
}
