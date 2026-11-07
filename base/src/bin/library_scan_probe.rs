//! Read-only facts about a music folder.
//!
//! Nothing here writes: no index, no cache, no temp file. It answers what a
//! library scan will cost on a given tree and whether the watcher can be
//! registered on it, which are the two things that differ between a local disk
//! and a network share.
//!
//! Usage: `library_scan_probe <root> [max_entries]`
//!
//! The walk is the real one (`scan::walk_indexed`), so the numbers are what a
//! `scan_folder` would produce rather than an estimate. `max_entries` only
//! limits how many sample paths are printed.

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::time::Instant;

fn main() {
    let mut args = std::env::args().skip(1);
    let Some(root) = args.next() else {
        eprintln!("usage: library_scan_probe <root> [samples]");
        std::process::exit(2);
    };
    let samples: usize = args.next().and_then(|n| n.parse().ok()).unwrap_or(10);
    let root = PathBuf::from(root);
    // `--analyze N` opens the first N files through the engine's own decoder.
    // Read-only on the tree; it is the only way to prove a network path is
    // playable without touching it.
    let analyze_first: usize = std::env::var("NEIRO_PROBE_ANALYZE")
        .ok()
        .and_then(|n| n.parse().ok())
        .unwrap_or(0);

    println!("root: {}", root.display());
    match std::fs::metadata(&root) {
        Ok(meta) => println!(
            "  a {} ({} bytes reported for the directory itself)",
            if meta.is_dir() { "directory" } else { "file" },
            meta.len()
        ),
        Err(e) => {
            eprintln!("  cannot stat it: {e}");
            std::process::exit(1);
        }
    }

    // ---- the walk --------------------------------------------------------
    let cancel = AtomicBool::new(false);
    let started = Instant::now();
    let walk = match neiro_base::scan::walk_indexed(&root, &cancel) {
        Ok(walk) => walk,
        Err(e) => {
            eprintln!("  the walk failed: {e}");
            std::process::exit(1);
        }
    };
    let elapsed = started.elapsed();
    let files = walk.files;
    if walk.unreadable > 0 {
        println!("  {} file(s) could NOT be stat'ed", walk.unreadable);
    }

    let total_bytes: u64 = files.iter().map(|f| f.size.max(0) as u64).sum();
    println!(
        "  {} audio file(s), {:.1} MiB, walk took {:.2?} ({:.0} files/s)",
        files.len(),
        total_bytes as f64 / (1024.0 * 1024.0),
        elapsed,
        files.len() as f64 / elapsed.as_secs_f64().max(0.001)
    );

    // ---- what the extensions are ----------------------------------------
    let mut by_ext: std::collections::BTreeMap<String, usize> = Default::default();
    for file in &files {
        let ext = file
            .path
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("(none)")
            .to_ascii_lowercase();
        *by_ext.entry(ext).or_default() += 1;
    }
    let mut ext: Vec<(String, usize)> = by_ext.into_iter().collect();
    ext.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    println!("  by extension: {:?}", ext);

    // ---- how readable the metadata is ------------------------------------
    //
    // A network share can return a zero timestamp or a stale one. A file whose
    // mtime cannot be read is a file that looks changed on every scan, which
    // would make every launch report the whole tree as modified.
    let zero_mtime = files.iter().filter(|f| f.mtime_ms == 0).count();
    let zero_size = files.iter().filter(|f| f.size == 0).count();
    println!(
        "  {} file(s) with no readable mtime, {} with size 0",
        zero_mtime, zero_size
    );

    // ---- can a watcher even be registered? -------------------------------
    //
    // Read-only: registering a watch changes nothing on the tree. Whether
    // events actually arrive cannot be tested without writing, and this probe
    // does not write.
    match (neiro_base::scan::FolderWatcher::new(), true) {
        (Ok(mut watcher), _) => {
            let started = Instant::now();
            match watcher.watch(&root) {
                Ok(()) => println!(
                    "  a watcher registered in {:.2?} — events still unproven without a write",
                    started.elapsed()
                ),
                Err(e) => println!("  a watcher could NOT be registered: {e}"),
            }
        }
        (Err(e), _) => println!("  no watcher is available on this system: {e}"),
    }

    // ---- a sample, so the shape of the paths is visible ------------------
    for file in files.iter().take(samples) {
        println!(
            "  sample: {} ({} bytes, mtime {})",
            file.path.display(),
            file.size,
            if file.mtime_ms == 0 {
                "unreadable".to_string()
            } else {
                file.mtime_ms.to_string()
            }
        );
    }
    if files.len() > samples {
        println!("  … and {} more", files.len() - samples);
    }

    // ---- long paths, measured the way Windows counts them ----------------
    //
    // MAX_PATH is 260 UTF-16 units, not 260 bytes: a CJK path is three bytes
    // per character in Rust's OsStr and one unit here. Getting this wrong makes
    // a library look safe when it is not, and a path over the limit fails at
    // CreateFileW unless long paths are enabled for the machine.
    let utf16_len = |p: &std::path::Path| p.to_string_lossy().encode_utf16().count();
    let over = files.iter().filter(|f| utf16_len(&f.path) > 260).count();
    if let Some(deepest) = files.iter().max_by_key(|f| utf16_len(&f.path)) {
        println!(
            "  longest path: {} UTF-16 units (MAX_PATH is 260)",
            utf16_len(&deepest.path)
        );
        println!("  {} file(s) over 260 units", over);
        if over > 0 {
            println!("  longest: {}", deepest.path.display());
        }
    }

    // Whether the machine will even open those.
    #[cfg(windows)]
    {
        use std::process::Command;
        let out = Command::new("reg")
            .args([
                "query",
                r"HKLM\SYSTEM\CurrentControlSet\Control\FileSystem",
                "/v",
                "LongPathsEnabled",
            ])
            .output();
        let text = out
            .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
            .unwrap_or_default();
        let enabled = text.split_whitespace().last() == Some("0x1");
        println!(
            "  LongPathsEnabled: {}",
            if enabled { "yes" } else { "no (paths over 260 will fail to open)" }
        );
    }

    // ---- can the engine open these? --------------------------------------
    //
    // The analyser decodes with the same code the player does, on its own
    // thread and with its own file handle. A file that analyses is a file that
    // plays; this proves the network path is usable without writing anything.
    if analyze_first > 0 {
        use std::sync::mpsc::RecvTimeoutError;
        use std::time::Duration;
        println!("  -- opening {analyze_first} file(s) through the engine --");
        let base = match neiro_base::Base::start() {
            Ok(base) => base,
            Err(e) => {
                eprintln!("  Base did not start: {e}");
                std::process::exit(1);
            }
        };
        let handle = base.handle();
        let (_ticket, events) = handle.subscribe();
        for file in files.iter().take(analyze_first) {
            if handle.analyze(&file.path).is_err() {
                println!("  {}: could not ask", file.path.display());
                continue;
            }
            let deadline = Instant::now() + Duration::from_secs(60);
            loop {
                let left = deadline.saturating_duration_since(Instant::now());
                if left.is_zero() {
                    println!("  {}: timed out", file.path.display());
                    break;
                }
                match events.recv_timeout(left.min(Duration::from_millis(200))) {
                    Ok(neiro_base::protocol::Event::AnalysisReady { path, analysis, .. })
                        if path == file.path =>
                    {
                        println!(
                            "  OK {} — {} ms, {} Hz, {} ch, {} bit, {}",
                            path.display(),
                            analysis.duration_ms,
                            analysis.sample_rate,
                            analysis.channels,
                            analysis.bits_per_sample,
                            analysis.codec
                        );
                        break;
                    }
                    Ok(neiro_base::protocol::Event::Error { op, message }) => {
                        println!("  {}: error from {op}: {message}", file.path.display());
                        break;
                    }
                    Ok(_) => {}
                    Err(RecvTimeoutError::Timeout) => {}
                    Err(RecvTimeoutError::Disconnected) => {
                        println!("  the event channel closed");
                        break;
                    }
                }
            }
        }
        handle.shutdown().ok();
        drop(base);
    }
}
