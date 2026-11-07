//! Folder scanning and live watching, driven through a real Base.
//!
//! The scanner's own tests cover the walk; this covers the part they cannot:
//! that a file created on disk after `watch_folder` reaches a subscriber. A
//! watcher probe that only checked "the call returned" would pass with the
//! backend wired to nothing at all, so every assertion here waits for the state
//! it is measuring, with a bounded budget.
//!
//! It leaves the tree and the process as it found them: the directory is
//! removed and Base is shut down before the probe exits.

use std::path::PathBuf;
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

use neiro_base::protocol::Event;
use neiro_base::scan::Change;
use neiro_base::Base;

fn fail(why: &str) -> ! {
    eprintln!("FAIL: {why}");
    std::process::exit(1);
}

fn ok(what: &str) {
    println!("  ok   {what}");
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

fn wait_event(
    rx: &Receiver<Event>,
    ms: u64,
    label: &str,
    mut want: impl FnMut(&Event) -> bool,
) -> Event {
    let deadline = Instant::now() + Duration::from_millis(ms);
    let mut last_error: Option<String> = None;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            match last_error {
                Some(error) => fail(&format!("timeout waiting for {label}; last error: {error}")),
                None => fail(&format!("timeout waiting for {label}")),
            }
        }
        match rx.recv_timeout(left) {
            Ok(event) => {
                if want(&event) {
                    return event;
                }
                if let Event::Error { op, message } = &event {
                    last_error = Some(format!("{op}: {message}"));
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                fail("the event channel closed")
            }
        }
    }
}

/// Assert that nothing matching arrives within the budget.
///
/// The mirror of `wait_event`: some of these checks are about a change that
/// must NOT be reported, and a waiter that failed on timeout would turn the
/// expected silence into an error.
fn expect_quiet(
    rx: &Receiver<Event>,
    ms: u64,
    mut want: impl FnMut(&Event) -> bool,
) -> Result<(), String> {
    let deadline = Instant::now() + Duration::from_millis(ms);
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return Ok(());
        }
        match rx.recv_timeout(left) {
            Ok(event) => {
                if want(&event) {
                    return Err(format!("{event:?}"));
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => return Ok(()),
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                return Err("the event channel closed".into())
            }
        }
    }
}

/// A real, tiny WAV. The tree's other files are bytes with an audio extension,
/// which is enough for scanning but not for a tag write: the backend parses
/// what it is given, and a fake file fails to parse for a reason that has
/// nothing to do with the check.
fn write_wav(path: &PathBuf) {
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
    std::fs::write(path, w).expect("write a wav");
}

fn touch(path: &PathBuf) {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).expect("create the parent");
    }
    std::fs::write(path, b"not really audio, but the extension is what decides").expect("write");
}

fn main() {
    let root = std::env::temp_dir().join(format!("neiro-folders-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&root);
    std::fs::create_dir_all(&root).expect("create the tree");
    touch(&root.join("a.flac"));
    touch(&root.join("album/b.wav"));
    touch(&root.join("cover.jpg"));
    write_wav(&root.join("c.wav"));

    println!("=== {} ===", root.display());

    // A library is what makes reconciliation possible: the index lives in it,
    // and without one every scan reports the whole tree as new.
    let db = root.join("library.db");
    let library = neiro_base::library::Library::open(&db).expect("open a library");
    let base = Base::start_with_policy_and_library(
        neiro_base::protocol::PlaybackPolicy::default(),
        library,
    )
    .expect("start Base");
    let handle = base.handle();
    let (_ticket, events) = handle.subscribe();

    // ---- first scan: everything is new -----------------------------------
    handle.scan_folder(&root).expect("scan_folder");
    let (added, modified, removed, moved) = match wait_event(&events, 20_000, "the scan", |e| {
        matches!(e, Event::ScanDone { .. })
    }) {
        Event::ScanDone {
            added,
            modified,
            removed,
            moved,
            ..
        } => (added, modified, removed, moved),
        _ => unreachable!(),
    };
    check(
        added.len() == 3,
        "the first scan reports the audio files and only those",
        format!("{} added", added.len()),
    );
    check(
        removed.is_empty() && moved.is_empty() && modified.is_empty(),
        "and nothing else, because there was no index to compare against",
        String::new(),
    );

    // ---- a second scan of an unchanged tree changes nothing --------------
    handle.scan_folder(&root).expect("scan again");
    let quiet = match wait_event(&events, 20_000, "the second scan", |e| {
        matches!(e, Event::ScanDone { .. })
    }) {
        Event::ScanDone {
            added,
            modified,
            removed,
            moved,
            ..
        } => (added, modified, removed, moved),
        _ => unreachable!(),
    };
    check(
        quiet.0.is_empty() && quiet.1.is_empty() && quiet.2.is_empty() && quiet.3.is_empty(),
        "a rescan of an unchanged tree is an empty diff, not a re-import",
        format!("{quiet:?}"),
    );

    // ---- a file that moved keeps its identity ---------------------------
    handle.scan_folder(&root).expect("scan");
    let _ = wait_event(&events, 20_000, "the scan before the move", |e| {
        matches!(e, Event::ScanDone { .. })
    });
    let moved_from = root.join("album/b.wav");
    let moved_to = root.join("album/b-renamed.wav");
    std::fs::rename(&moved_from, &moved_to).expect("rename");
    handle.scan_folder(&root).expect("scan after the move");
    let after_move = match wait_event(&events, 20_000, "the diff", |e| {
        matches!(e, Event::ScanDone { .. })
    }) {
        Event::ScanDone {
            added,
            removed,
            moved,
            ..
        } => (added, removed, moved),
        _ => unreachable!(),
    };
    check(
        after_move.0.is_empty() && after_move.1.is_empty(),
        "a rename is not a removal and an addition",
        format!("{:?}", after_move),
    );
    check(
        after_move.2.len() == 1
            && after_move.2[0].from == moved_from
            && after_move.2[0].to == moved_to,
        "it is reported as one move",
        format!("{:?}", after_move.2),
    );


    // A rename that changes both the name and the directory is confirmed by
    // nothing: it is indistinguishable from a copy and a delete inside one
    // window. Reported as a removal and an addition on purpose -- a missed move
    // leaves a star where it was, a wrong one moves it onto another file.
    let unconfirmed_to = root.join("elsewhere.flac");
    std::fs::rename(&moved_to, &unconfirmed_to).expect("rename across");
    handle.scan_folder(&root).expect("scan after the second move");
    let unconfirmed = match wait_event(&events, 20_000, "the second diff", |e| {
        matches!(e, Event::ScanDone { .. })
    }) {
        Event::ScanDone { added, removed, moved, .. } => (added, removed, moved),
        _ => unreachable!(),
    };
    check(
        unconfirmed.2.is_empty(),
        "a rename that changes name and directory is NOT paired",
        format!("{:?}", unconfirmed.2),
    );

    // ---- watch -----------------------------------------------------------
    handle.watch_folder(&root).expect("watch_folder");
    let roots = match wait_event(&events, 5_000, "the watch", |e| {
        matches!(e, Event::FolderWatched { .. })
    }) {
        Event::FolderWatched { roots, .. } => roots,
        _ => unreachable!(),
    };
    check(
        roots.contains(&root),
        "the watcher reports the root it took",
        format!("{roots:?}"),
    );

    // ---- a file appears --------------------------------------------------
    //
    // The budget is generous because the backend batches: a save produces
    // several events and the worker coalesces them on a 150 ms tick.
    let added_path = root.join("added.flac");
    touch(&added_path);
    let changes = match wait_event(&events, 15_000, "the added file", |e| {
        matches!(e, Event::FilesChanged { changes } if changes.iter().any(|c| !c.added.is_empty()))
    }) {
        Event::FilesChanged { changes } => changes,
        _ => unreachable!(),
    };
    let added: Vec<PathBuf> = changes.iter().flat_map(|c| c.added.clone()).collect();
    check(
        added.contains(&added_path),
        "a file created after the watch is reported",
        format!("{added:?}"),
    );
    check(
        changes.iter().all(|c| c.root == root),
        "the change is attributed to the watched root",
        format!("{:?}", changes.iter().map(|c| c.root.clone()).collect::<Vec<_>>()),
    );

    // ---- a non-audio file is not a library change ------------------------
    let jpg = root.join("art.jpg");
    touch(&jpg);
    match expect_quiet(&events, 1_500, |e| {
        matches!(e, Event::FilesChanged { changes } if changes.iter().any(|c| c.added.contains(&jpg)))
    }) {
        Ok(()) => ok("a cover image is not a library change"),
        Err(event) => fail(&format!("a .jpg was reported as a library change: {event}")),
    }

    // ---- a file goes away ------------------------------------------------
    std::fs::remove_file(&added_path).expect("remove the file");
    let removed_changes = match wait_event(&events, 15_000, "the removed file", |e| {
        matches!(e, Event::FilesChanged { changes } if changes.iter().any(|c| !c.removed.is_empty()))
    }) {
        Event::FilesChanged { changes } => changes,
        _ => unreachable!(),
    };
    let removed: Vec<PathBuf> = removed_changes.iter().flat_map(|c| c.removed.clone()).collect();
    check(
        removed.contains(&added_path),
        "a file deleted after the watch is reported",
        format!("{removed:?}"),
    );

    // ---- a moved file is not decoded twice -------------------------------
    //
    // The analysis key is path + size + mtime + analyzer version, and a rename
    // changes only the first. If reconciliation does not carry the entry over,
    // moving an album costs one decode per file for no reason. `cached` on the
    // second analysis is the measurement.
    let before_move = root.join("c.wav");
    handle.analyze(&before_move).expect("analyze");
    match wait_event(&events, 30_000, "the first analysis", |e| {
        matches!(e, Event::AnalysisReady { path, .. } if path == &before_move)
    }) {
        Event::AnalysisReady { cached, .. } => check(
            !cached,
            "a file analysed for the first time is a miss",
            format!("cached={cached}"),
        ),
        _ => unreachable!(),
    }

    let after_move = root.join("c-moved.wav");
    std::fs::rename(&before_move, &after_move).expect("rename");
    handle.scan_folder(&root).expect("scan after the move");
    let moved_here = match wait_event(&events, 20_000, "the diff", |e| {
        matches!(e, Event::ScanDone { .. })
    }) {
        Event::ScanDone { moved, .. } => moved,
        _ => unreachable!(),
    };
    check(
        moved_here.len() == 1 && moved_here[0].to == after_move,
        "the move is detected before the cache is asked",
        format!("{moved_here:?}"),
    );

    handle.analyze(&after_move).expect("analyze the moved file");
    match wait_event(&events, 30_000, "the second analysis", |e| {
        matches!(e, Event::AnalysisReady { path, .. } if path == &after_move)
    }) {
        Event::AnalysisReady { cached, .. } => check(
            cached,
            "a moved file is a cache hit, not a second decode",
            format!("cached={cached}"),
        ),
        _ => unreachable!(),
    }

    // ---- a write from this process is not an import ----------------------
    //
    // The tag writer replaces the file by renaming over it, which the watcher
    // sees as a file appearing. That is the defect this suppression exists for;
    // the quiet window is longer than the grace period, so an event that
    // arrives late still fails the check.
    let tagged = root.join("c-moved.wav");
    let mut policy = neiro_base::tag::TagPolicy::default();
    policy.write = [neiro_base::tag::TagFamily::RiffInfo]
        .into_iter()
        .collect();
    handle
        .request_tag_write(
            &tagged,
            vec![neiro_base::tag::TagFieldValue {
                family: neiro_base::tag::TagFamily::RiffInfo,
                key: "TrackTitle".into(),
                name: "INAM".into(),
                value: Some("Written By Us".into()),
            }],
            policy,
        )
        .expect("write a tag");
    match wait_event(&events, 10_000, "the tag write", |e| {
        matches!(e, Event::TagsChanged { .. } | Event::Error { .. })
    }) {
        Event::TagsChanged { .. } => ok("the tag write lands and is reported as a tag change"),
        Event::Error { op, message } => fail(&format!("tag write failed: {op}: {message}")),
        _ => unreachable!(),
    }
    match expect_quiet(&events, 3_500, |e| {
        matches!(e, Event::FilesChanged { changes }
            if changes.iter().any(|c| c.added.contains(&tagged) || c.removed.contains(&tagged)))
    }) {
        Ok(()) => ok("a write from this process is not reported as a file change"),
        Err(event) => fail(&format!("a tag write was reported as a library change: {event}")),
    }

    // ---- a directory going away asks for a rescan ------------------------
    std::fs::remove_dir_all(root.join("album")).expect("remove the directory");
    let rescan = match wait_event(&events, 15_000, "a rescan", |e| {
        matches!(e, Event::FilesChanged { changes } if changes.iter().any(|c| c.rescan))
    }) {
        Event::FilesChanged { changes } => changes,
        _ => unreachable!(),
    };
    check(
        rescan.iter().any(|c: &Change| c.rescan && c.root == root),
        "a removed directory asks for a rescan instead of naming files it never saw",
        String::new(),
    );

    // ---- unwatch ---------------------------------------------------------
    handle.unwatch_folder(&root).expect("unwatch_folder");
    match wait_event(&events, 5_000, "the unwatch", |e| {
        matches!(e, Event::FolderUnwatched { .. })
    }) {
        Event::FolderUnwatched { roots, .. } => check(
            roots.is_empty(),
            "the root is gone from the watched set",
            format!("{roots:?}"),
        ),
        _ => unreachable!(),
    }

    // Nothing is reported after that: a root nobody watches cannot produce a
    // change, and a stale watcher would keep a deleted tree alive.
    let after = root.join("after.flac");
    touch(&after);
    match expect_quiet(&events, 1_500, |e| matches!(e, Event::FilesChanged { .. })) {
        Ok(()) => ok("no change is reported after unwatching"),
        Err(event) => fail(&format!("a change was reported after unwatching: {event}")),
    }

    handle.shutdown().expect("shutdown");
    drop(base);
    let _ = std::fs::remove_dir_all(&root);
    println!("\nFOLDER PROBE PASSED");
}
