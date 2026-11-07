//! Navidrome (Subsonic) client against a stub server.
//!
//! The part that cannot be unit-tested is the signature: the token is
//! `md5(password + salt)` and the salt changes per request, so a stub that
//! recomputes it is the only way to know the client signs correctly rather than
//! merely producing something that looks like a hash.
//!
//! The stub also enforces `format=raw` on the stream call. That parameter is
//! not decoration: without it Subsonic servers may transcode to whatever the
//! client advertises, and every bit-perfect claim about the track becomes false
//! while the download still succeeds.

use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::mpsc::{self, Receiver};
use std::thread;

use neiro_base::remote::{self, RemoteError, Server};

const USER: &str = "joe";
const PASSWORD: &str = "sesame";

fn fail(why: &str) -> ! {
    eprintln!("FAIL: {why}");
    std::process::exit(1);
}

fn check(pass: bool, label: &str, detail: String) {
    println!(
        "  [{}] {:<54} {}",
        if pass { "PASS" } else { "FAIL" },
        label,
        detail
    );
    if !pass {
        std::process::exit(1);
    }
}

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

struct Seen {
    path: String,
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
    loop {
        let mut header = String::new();
        if reader.read_line(&mut header)? == 0 {
            break;
        }
        if header.trim_end().is_empty() {
            break;
        }
    }
    let _ = seen.send(Seen { path: path.clone() });

    let query: Vec<(String, String)> = path
        .split_once('?')
        .map(|(_, q)| {
            q.split('&')
                .filter_map(|pair| pair.split_once('='))
                .map(|(k, v)| (k.to_owned(), v.to_owned()))
                .collect()
        })
        .unwrap_or_default();
    let param = |name: &str| {
        query
            .iter()
            .find(|(k, _)| k == name)
            .map(|(_, v)| v.clone())
    };

    // The signature has to be right, or this is not a Subsonic client.
    let (Some(user), Some(salt), Some(token)) = (param("u"), param("s"), param("t")) else {
        return json(&mut stream, "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":10,\"message\":\"missing auth\"}}}");
    };
    if user != USER || token != remote::subsonic_token(PASSWORD, &salt) {
        return json(&mut stream, "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":40,\"message\":\"Wrong username or password\"}}}");
    }

    if path.starts_with("/rest/search3.view") {
        let body = serde_json::json!({
            "subsonic-response": {
                "status": "ok",
                "version": "1.16.1",
                "searchResult3": {
                    "song": [
                        { "id": "tr-1", "title": "Salt", "artist": "Elin Vahl",
                          "album": "Northbound", "suffix": "wav", "duration": 0,
                          "size": wav_bytes().len(), "coverArt": "tr-1" },
                        { "id": "tr-2", "title": "Harbour Lights", "artist": "Elin Vahl",
                          "album": "Northbound", "suffix": "flac", "duration": 56 }
                    ]
                }
            }
        });
        let text = body.to_string();
        return json(&mut stream, &text);
    }

    if path.starts_with("/rest/stream.view") {
        // Transcoding is a silent audio change, so the stub refuses a request
        // that did not ask for the file as stored.
        if param("format").as_deref() != Some("raw") {
            return json(&mut stream, "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":10,\"message\":\"format must be raw\"}}}");
        }
        let id = param("id").unwrap_or_default();
        if id != "tr-1" {
            return json(&mut stream, "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":70,\"message\":\"not found\"}}}");
        }
        let body = wav_bytes();
        return audio(&mut stream, &body);
    }

    if path.starts_with("/rest/getCoverArt.view") {
        if param("id").as_deref() != Some("tr-1") {
            return json(&mut stream, "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":70,\"message\":\"no art\"}}}");
        }
        return respond(&mut stream, "image/png", b"\x89PNG\r\n\x1a\n fake cover");
    }

    if path.starts_with("/rest/getPlaylists.view") {
        let body = serde_json::json!({
            "subsonic-response": {
                "status": "ok", "version": "1.16.1",
                "playlists": { "playlist": [
                    { "id": "pl-1", "name": "Evening", "songCount": 2, "duration": 300 }
                ]}
            }
        });
        let text = body.to_string();
        return json(&mut stream, &text);
    }

    if path.starts_with("/rest/getPlaylist.view") {
        let body = serde_json::json!({
            "subsonic-response": {
                "status": "ok", "version": "1.16.1",
                "playlist": { "id": "pl-1", "name": "Evening", "songCount": 2,
                    "entry": [
                        { "id": "tr-1", "title": "Salt", "artist": "Elin Vahl",
                          "album": "Northbound", "suffix": "wav", "duration": 0, "coverArt": "tr-1" },
                        { "id": "tr-2", "title": "Harbour Lights", "artist": "Elin Vahl",
                          "album": "Northbound", "suffix": "flac", "duration": 56, "coverArt": "tr-2" }
                    ]}
            }
        });
        let text = body.to_string();
        return json(&mut stream, &text);
    }

    json(&mut stream, "{\"subsonic-response\":{\"status\":\"failed\",\"error\":{\"code\":0,\"message\":\"unknown endpoint\"}}}")
}

fn respond(stream: &mut TcpStream, kind: &str, body: &[u8]) -> std::io::Result<()> {
    let head = format!(
        "HTTP/1.1 200 OK\r\nContent-Type: {kind}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    stream.write_all(head.as_bytes())?;
    stream.write_all(body)?;
    stream.flush()
}

fn json(stream: &mut TcpStream, body: &str) -> std::io::Result<()> {
    respond(stream, "application/json", body.as_bytes())
}

fn audio(stream: &mut TcpStream, body: &[u8]) -> std::io::Result<()> {
    respond(stream, "audio/wav", body)
}

fn main() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
    let addr = listener.local_addr().expect("addr");
    let (tx, seen): (mpsc::Sender<Seen>, Receiver<Seen>) = mpsc::channel();
    thread::spawn(move || serve(listener, tx));

    let server = Server::Navidrome {
        base_url: format!("http://{addr}"),
        user: USER.into(),
        password: PASSWORD.into(),
    };
    let dest = std::env::temp_dir().join(format!("neiro-navidrome-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dest);

    println!("=== stub at {addr} ===");

    // ---- listing ---------------------------------------------------------
    let tracks = remote::list_audio(&server, 0, 50).expect("list");
    let first = seen.recv().expect("a request");
    check(
        first.path.contains("/rest/search3.view"),
        "the listing goes to the Subsonic endpoint",
        first.path.split('?').next().unwrap_or("").to_string(),
    );
    check(
        first.path.contains("f=json") && first.path.contains("c=neiro"),
        "it asks for JSON and identifies the client",
        String::new(),
    );
    check(
        tracks.len() == 2,
        "two songs parsed",
        format!("{}", tracks.len()),
    );
    let salt = tracks.iter().find(|t| t.id == "tr-1").expect("the wav song");
    check(salt.container == "wav", "the suffix names the file", salt.container.clone());
    check(
        tracks.iter().find(|t| t.id == "tr-2").map(|t| t.duration_ms) == Some(56_000),
        "seconds became milliseconds",
        String::new(),
    );

    // ---- a wrong password is refused by the signature, not by a flag -----
    let wrong = Server::Navidrome {
        base_url: format!("http://{addr}"),
        user: USER.into(),
        password: "wrong".into(),
    };
    match remote::list_audio(&wrong, 0, 10) {
        Err(RemoteError::Status { code, .. }) => {
            check(code == 40, "a wrong password is code 40", format!("{code}"))
        }
        other => fail(&format!("a wrong password returned {other:?}")),
    }
    // The refused call also reached the stub; take it so the next assertion
    // looks at the request it means to.
    let _ = seen.recv();

    // ---- download asks for the file, not a transcode ---------------------
    let want = wav_bytes();
    let path = remote::download(&server, salt, &dest).expect("download");
    let request = seen.recv().expect("a request");
    check(
        request.path.contains("format=raw"),
        "the download asks for the file as stored",
        String::new(),
    );
    check(
        request.path.contains("maxBitRate=0"),
        "and never asks for a bitrate limit",
        String::new(),
    );
    check(
        std::fs::read(&path).expect("read") == want,
        "the bytes are the bytes the server sent",
        format!("{} bytes", want.len()),
    );

    // ---- a song that is not there ---------------------------------------
    let missing = remote::RemoteTrack {
        id: "tr-2".into(),
        title: "x".into(),
        artist: "x".into(),
        album: "x".into(),
        container: "flac".into(),
        duration_ms: 0,
        size: None,
        cover_art: None,
    };
    match remote::download(&server, &missing, &dest) {
        Err(RemoteError::Status { code, body }) => {
            check(code == 70, "a missing song is code 70", body)
        }
        other => fail(&format!("a missing song returned {other:?}")),
    }
    check(
        !remote::cache_path(&dest, &missing).exists(),
        "and leaves no file behind",
        String::new(),
    );

    // ---- cover art -------------------------------------------------------
    let art = remote::fetch_artwork(&server, salt)
        .expect("artwork")
        .expect("this track has a cover");
    let _ = seen.recv();
    check(art.mime.starts_with("image/"), "the cover comes back as an image", art.mime.clone());
    check(!art.data.is_empty(), "and carries bytes", format!("{}", art.data.len()));

    // ---- playlists -------------------------------------------------------
    let lists = remote::list_playlists(&server).expect("playlists");
    check(lists.len() == 1, "one playlist", format!("{}", lists.len()));
    check(
        lists[0].track_count == 2 && lists[0].duration_ms == 300_000,
        "its count and duration came from the server",
        format!("{} / {} ms", lists[0].track_count, lists[0].duration_ms),
    );
    let entries = remote::playlist_tracks(&server, &lists[0].id, 0, 10).expect("entries");
    check(
        entries.len() == 2 && entries[0].id == "tr-1",
        "the entries keep the server's order",
        format!("{:?}", entries.iter().map(|t| t.id.clone()).collect::<Vec<_>>()),
    );

    // ---- a cover survives the process ------------------------------------
    //
    // Self-contained: one session warms the directory, a second one -- what a
    // restart gets, with an empty memory layer -- must answer without the
    // server. Measurement is the request queue, not a return value.
    let warm = |session: &mut remote::Session, dest: &std::path::Path| {
        session.run(remote::Job::Configure {
            config: Some(server.clone()),
            cache_dir: Some(dest.to_path_buf()),
        });
        session.run(remote::Job::List { start: 0, limit: 10 });
    };

    let mut first_run = remote::Session::default();
    warm(&mut first_run, &dest);
    let _ = first_run.run(remote::Job::FetchArtwork { id: "tr-1".into() });
    std::thread::sleep(std::time::Duration::from_millis(200));
    while seen.try_recv().is_ok() {}

    let mut after_restart = remote::Session::default();
    warm(&mut after_restart, &dest);
    std::thread::sleep(std::time::Duration::from_millis(200));
    while seen.try_recv().is_ok() {};

    let from_disk = after_restart.run(remote::Job::FetchArtwork { id: "tr-1".into() });
    match &from_disk {
        neiro_base::protocol::Event::RemoteArtwork { data: Some(data), .. } => check(
            !data.is_empty(),
            "a restart still answers with the cover",
            format!("{} bytes", data.len()),
        ),
        other => fail(&format!("a restart lost the cover: {other:?}")),
    }
    match seen.recv_timeout(std::time::Duration::from_millis(400)) {
        Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
            println!("  ok   and it did not reach the server")
        }
        Ok(request) => fail(&format!("a restart re-downloaded: {}", request.path)),
        Err(other) => fail(&format!("unexpected channel state: {other:?}")),
    }

    let _ = std::fs::remove_dir_all(&dest);
    println!("\nNAVIDROME PROBE PASSED");

    // ---- a cover is fetched once -----------------------------------------
    //
    // The second call must not reach the server at all. Measured on the stub's
    // request queue: anything arriving here means the cache did nothing.
    // Drain whatever the earlier checks left in the queue, so this measurement
    // sees only its own traffic.
    while seen.try_recv().is_ok() {}

    let mut session = remote::Session::default();
    session.run(remote::Job::Configure {
        config: Some(server.clone()),
        cache_dir: Some(dest.clone()),
    });
    session.run(remote::Job::List { start: 0, limit: 10 });
    let first = session.run(remote::Job::FetchArtwork { id: "tr-1".into() });
    // Let the first call's traffic land, then clear the queue completely: the
    // earlier checks in this probe also fetched covers, and one `recv` leaves
    // their requests behind to be mistaken for the second call's.
    std::thread::sleep(std::time::Duration::from_millis(150));
    while seen.try_recv().is_ok() {}
    let second = session.run(remote::Job::FetchArtwork { id: "tr-1".into() });
    match (&first, &second) {
        (neiro_base::protocol::Event::RemoteArtwork { data: a, .. },
         neiro_base::protocol::Event::RemoteArtwork { data: b, .. }) => {
            check(a.is_some() && a == b, "the same cover comes back twice", String::new());
        }
        other => fail(&format!("expected two artwork answers, got {other:?}")),
    }
    match seen.recv_timeout(std::time::Duration::from_millis(400)) {
        Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
            println!("  ok   the second cover did not reach the server")
        }
        Ok(request) => fail(&format!(
            "the second cover was fetched again: {}",
            request.path
        )),
        Err(other) => fail(&format!("unexpected channel state: {other:?}")),
    }

    // ---- what is actually on disk ----
    {
        use std::path::Path;
        let root = Path::new(&dest).join("artwork");
        eprintln!("DEBUG artwork root {:?} exists={}", root, root.exists());
        if let Ok(dirs) = std::fs::read_dir(&root) {
            for d in dirs.flatten() {
                eprintln!("  server dir {:?}", d.path());
                if let Ok(files) = std::fs::read_dir(d.path()) {
                    for f in files.flatten() { eprintln!("    file {:?}", f.file_name()); }
                }
            }
        }
    }

    // ---- a cover survives the process ------------------------------------
    //
    // A fresh Session with a fresh in-memory layer is what a restart gets. The
    // only thing that can answer is the directory.
    while seen.try_recv().is_ok() {}
    std::thread::sleep(std::time::Duration::from_millis(150));
    while seen.try_recv().is_ok() {}
    let mut restarted = remote::Session::default();
    restarted.run(remote::Job::Configure {
        config: Some(server.clone()),
        cache_dir: Some(dest.clone()),
    });
    restarted.run(remote::Job::List { start: 0, limit: 10 });
    // The listing is a request of its own; take it before measuring the cover.
    std::thread::sleep(std::time::Duration::from_millis(150));
    while seen.try_recv().is_ok() {}
    let from_disk = restarted.run(remote::Job::FetchArtwork {
        id: "tr-1".into(),
    });
    match &from_disk {
        neiro_base::protocol::Event::RemoteArtwork { data: Some(data), .. } => {
            check(!data.is_empty(), "a restart still answers with the cover", format!("{} bytes", data.len()))
        }
        other => fail(&format!("a restart lost the cover: {other:?}")),
    }
    match seen.recv_timeout(std::time::Duration::from_millis(400)) {
        Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
            println!("  ok   and it did not reach the server")
        }
        Ok(request) => fail(&format!("a restart re-downloaded the cover: {}", request.path)),
        Err(other) => fail(&format!("unexpected channel state: {other:?}")),
    }

    let _ = std::fs::remove_dir_all(&dest);
    println!("\nNAVIDROME PROBE PASSED");
}
