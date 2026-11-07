//! The browsing index, driven the way a UI drives it.
//!
//! A folder is written, tagged, scanned, and then read back through the three
//! queries a library view makes: a track list, an album list and a search. The
//! point is that none of them opens a file -- the rows come from SQLite, which
//! is what makes a ten-thousand-track library open in one query instead of ten
//! thousand decodes.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use lofty::config::WriteOptions;
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::tag::{ItemKey, Tag, TagType};

use neiro_base::protocol::{Command, Event};
use neiro_base::tag::{TagFamily, TagFieldValue, TagPolicy};
use neiro_base::Base;

fn check(ok: bool, label: &str, detail: String) {
    println!(
        "  [{}] {:<52} {}",
        if ok { "PASS" } else { "FAIL" },
        label,
        detail
    );
    if !ok {
        std::process::exit(1);
    }
}

/// A short 16-bit PCM WAV. Nothing decodes it here; it only has to be a file
/// that the tag writer and the walk both accept.
fn write_wav(path: &PathBuf, ms: u32) {
    let rate = 44_100u32;
    let frames = rate * ms / 1000;
    let data_len = frames * 2;
    let mut wav = Vec::with_capacity((44 + data_len) as usize);
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36 + data_len).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16u32.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&rate.to_le_bytes());
    wav.extend_from_slice(&(rate * 2).to_le_bytes());
    wav.extend_from_slice(&2u16.to_le_bytes());
    wav.extend_from_slice(&16u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&data_len.to_le_bytes());
    for i in 0..frames {
        let v = ((i as f32 * 0.05).sin() * 8000.0) as i16;
        wav.extend_from_slice(&v.to_le_bytes());
    }
    std::fs::write(path, wav).expect("write fixture");
}

/// Tag the file with lofty directly, not through Base.
///
/// That is the case that matters: a listener points Base at a folder that was
/// tagged years ago by something else. Going through `request_tag_write` would
/// make Base's own write path do the indexing, and the scan's -- the one a real
/// import uses -- would never run.
fn tag_with_lofty(path: &PathBuf, title: &str, artist: &str, album: &str, track: u32) {
    let mut file = lofty::read_from_path(path).expect("read fixture for tagging");
    if file.tag(TagType::RiffInfo).is_none() {
        file.insert_tag(Tag::new(TagType::RiffInfo));
    }
    let tag = file.tag_mut(TagType::RiffInfo).expect("a tag to write");
    tag.insert_text(ItemKey::TrackTitle, title.to_string());
    tag.insert_text(ItemKey::TrackArtist, artist.to_string());
    tag.insert_text(ItemKey::AlbumTitle, album.to_string());
    tag.insert_text(ItemKey::TrackNumber, track.to_string());
    file.save_to_path(path, WriteOptions::default())
        .expect("save the tags");
}

fn wait_for(
    rx: &std::sync::mpsc::Receiver<Event>,
    timeout: Duration,
    mut want: impl FnMut(&Event) -> bool,
    label: &str,
) -> Event {
    let deadline = Instant::now() + timeout;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            panic!("timeout waiting for {label}");
        }
        match rx.recv_timeout(left.min(Duration::from_millis(100))) {
            Ok(event) => {
                if let Event::Error { op, message } = &event {
                    panic!("Base error during {label}: {op}: {message}");
                }
                if want(&event) {
                    return event;
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                panic!("the event channel closed while waiting for {label}")
            }
        }
    }
}

fn main() {
    let dir = std::env::current_dir().expect("cwd").join("index_probe");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("create fixture dir");

    let db = std::env::temp_dir().join("neiro-index-probe.db");
    let _ = std::fs::remove_file(&db);
    let library = neiro_base::library::Library::open(&db).expect("open library");
    let base = Base::start_with_policy_and_library(
        neiro_base::protocol::PlaybackPolicy::default(),
        library,
    )
    .expect("start Base");
    let handle = base.handle();
    let (_ticket, events) = handle.subscribe();

    let albums = [
        ("one.wav", "First Light", "The Signals", "Night Drive", 1),
        ("two.wav", "Second Wind", "The Signals", "Night Drive", 2),
        ("three.wav", "100% Pure", "Someone Else", "Day Room", 1),
    ];
    let mut paths = Vec::new();
    for (file, title, artist, album, track) in albums {
        let path = dir.join(file);
        write_wav(&path, 120);
        tag_with_lofty(&path, title, artist, album, track);
        paths.push(path);
    }

    handle
        .dispatch(Command::ScanFolder { root: dir.clone() })
        .expect("scan");
    let done = wait_for(
        &events,
        Duration::from_secs(30),
        |e| matches!(e, Event::ScanDone { .. }),
        "the scan to finish",
    );
    if let Event::ScanDone { added, .. } = &done {
        check(added.len() == 3, "the scan found three files", format!("{}", added.len()));
    }

    // The files were tagged by lofty above, not by Base, so the only thing that
    // can have filled the index is the scan. That is the path a real import
    // takes; a probe that tagged through Base would exercise Base's own write
    // handler and leave the scan's indexing untested.
    //
    // Counted across completions rather than read from one event: a batch is
    // whatever was queued when the worker last came back. The property that
    // matters is that three rows arrived, not how they were grouped.
    let mut indexed = 0usize;
    let deadline = Instant::now() + Duration::from_secs(30);
    while indexed < 3 {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            panic!("timeout waiting for the scan to fill the index");
        }
        match events.recv_timeout(left.min(Duration::from_millis(100))) {
            Ok(Event::TagsIndexed {
                done,
                complete: true,
                ..
            }) => indexed += done,
            Ok(Event::Error { op, message }) => {
                panic!("Base error while indexing: {op}: {message}")
            }
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                panic!("the event channel closed while indexing")
            }
        }
    }
    check(indexed == 3, "every tagged file was indexed", format!("{indexed}"));

    handle
        .dispatch(Command::ListTracks {
            offset: 0,
            limit: 100,
        })
        .expect("list tracks");
    let listed = wait_for(
        &events,
        Duration::from_secs(10),
        |e| matches!(e, Event::TracksListed { .. }),
        "the track list",
    );
    if let Event::TracksListed { tracks, total, .. } = &listed {
        check(*total == 3, "the index holds three tracks", format!("{total}"));
        let titles: Vec<&str> = tracks.iter().map(|t| t.title.as_str()).collect();
        check(
            titles.contains(&"First Light") && titles.contains(&"Second Wind"),
            "titles come from the tags",
            format!("{titles:?}"),
        );
        let first = tracks.iter().find(|t| t.title == "First Light");
        check(
            first.map(|t| t.duration_ms > 0).unwrap_or(false),
            "duration came from the file, not the tags",
            format!("{:?}", first.map(|t| t.duration_ms)),
        );
    }

    handle.dispatch(Command::ListAlbums).expect("list albums");
    let listed = wait_for(
        &events,
        Duration::from_secs(10),
        |e| matches!(e, Event::AlbumsListed { .. }),
        "the album list",
    );
    if let Event::AlbumsListed { albums } = &listed {
        check(albums.len() == 2, "two albums", format!("{}", albums.len()));
        let night = albums.iter().find(|a| a.album == "Night Drive");
        check(
            night.map(|a| a.track_count == 2).unwrap_or(false),
            "Night Drive has both its tracks",
            format!("{:?}", night.map(|a| a.track_count)),
        );
    }

    handle.dispatch(Command::ListArtists).expect("list artists");
    let listed = wait_for(
        &events,
        Duration::from_secs(10),
        |e| matches!(e, Event::ArtistsListed { .. }),
        "the artist list",
    );
    if let Event::ArtistsListed { artists } = &listed {
        check(artists.len() == 2, "two artists", format!("{}", artists.len()));
        let signals = artists.iter().find(|a| a.artist == "The Signals");
        check(
            signals.map(|a| a.album_count == 1).unwrap_or(false),
            "one artist can span one album here",
            format!("{:?}", signals.map(|a| a.album_count)),
        );
    }

    handle
        .dispatch(Command::Search {
            query: "Second".into(),
            limit: 10,
        })
        .expect("search");
    let found = wait_for(
        &events,
        Duration::from_secs(10),
        |e| matches!(e, Event::SearchResults { .. }),
        "the search",
    );
    if let Event::SearchResults { tracks, .. } = &found {
        check(
            tracks.len() == 1 && tracks[0].title == "Second Wind",
            "search finds one title",
            format!("{:?}", tracks.iter().map(|t| t.title.as_str()).collect::<Vec<_>>()),
        );
    }

    // A percent sign in the query is a character, not a wildcard. Without the
    // ESCAPE clause every title matches "%" and the search box lies.
    handle
        .dispatch(Command::Search {
            query: "%".into(),
            limit: 10,
        })
        .expect("search for a percent");
    let found = wait_for(
        &events,
        Duration::from_secs(10),
        |e| matches!(e, Event::SearchResults { .. }),
        "the percent search",
    );
    if let Event::SearchResults { tracks, .. } = &found {
        check(
            tracks.len() == 1 && tracks[0].title == "100% Pure",
            "a percent in the query matches only that title",
            format!("{} hit(s)", tracks.len()),
        );
    }

    // The write path: a retag through Base must refresh the row it already has,
    // which is the opposite case from the scan above.
    let mut policy = TagPolicy::default();
    policy.write = [TagFamily::RiffInfo].into_iter().collect();
    handle
        .request_tag_write(
            &paths[0],
            vec![TagFieldValue {
                family: TagFamily::RiffInfo,
                key: "TrackTitle".into(),
                name: "INAM".into(),
                value: Some("Renamed Light".into()),
            }],
            policy,
        )
        .expect("retag");
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        if Instant::now() >= deadline {
            panic!("timeout waiting for the retag to reach the index");
        }
        match events.recv_timeout(Duration::from_millis(100)) {
            Ok(Event::TagsIndexed { complete: true, .. }) => break,
            Ok(Event::Error { op, message }) => {
                panic!("Base error while re-indexing: {op}: {message}")
            }
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                panic!("the event channel closed while re-indexing")
            }
        }
    }
    handle
        .dispatch(Command::ListTracks {
            offset: 0,
            limit: 100,
        })
        .expect("list tracks after the retag");
    let listed = wait_for(
        &events,
        Duration::from_secs(10),
        |e| matches!(e, Event::TracksListed { .. }),
        "the track list after the retag",
    );
    if let Event::TracksListed { tracks, .. } = &listed {
        check(
            tracks.iter().any(|t| t.title == "Renamed Light"),
            "a retag replaces the row instead of adding one",
            format!("{} track(s)", tracks.len()),
        );
        check(
            tracks.len() == 3,
            "the retag did not duplicate the row",
            format!("{} track(s)", tracks.len()),
        );
    }

    let _ = std::fs::remove_dir_all(&dir);
    base.shutdown();
    println!("\nAll index checks passed.");
}
