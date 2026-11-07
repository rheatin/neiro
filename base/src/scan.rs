//! Finding audio files under a folder.
//!
//! The engine decodes from paths and nothing else, so the one thing a library
//! needs from the filesystem is the set of paths worth handing it. This module
//! is that set: a walk, a filter on what the engine can actually decode, and a
//! stable order.
//!
//! # Why the extension list lives here
//!
//! A file is worth listing if the engine can open it. The engine's own README
//! lists the containers it decodes, and this is that list as extensions. It is
//! a restatement, which is normally a smell -- but the alternative is walking
//! every file and asking the engine, which decodes each one to find out. The
//! list is codec knowledge, not a measurement, and it is the only place in this
//! crate that knows it.
//!
//! # Why symlinks are not followed
//!
//! A directory junction on Windows and a symlink elsewhere can point back at an
//! ancestor. A walk that follows them does not terminate; it fills the list
//! until memory runs out. The walk records a directory only once and follows no
//! link, which is what a music library wants anyway: a link is not a file.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

// `notify`'s `watch`/`unwatch` live on this trait; the struct below is named
// `FolderWatcher` so the two do not shadow each other.
use notify::Watcher as _;

/// What the engine can open, lower-case, without the dot.
///
/// This is what the bundled FFmpeg decodes, not what the engine's README
/// advertises. Those are different lists, and the difference was measured on a
/// real library: 13 `.tak` files sat in a 3,572-file collection while the README
/// mentioned no TAK at all, and the decoder opened every one of them at 96 kHz /
/// 32-bit. A whitelist built from the feature list would have hidden them
/// without a word.
///
/// An unknown extension is still skipped rather than probed: opening every
/// `.jpg` in a music folder to see whether it decodes costs more than it can
/// ever find. The cost of the choice is that a format nobody listed is
/// invisible, which is why `library_scan_probe` prints every extension in the
/// tree -- so the next one of these is found by looking, not by guessing.
pub const AUDIO_EXTENSIONS: &[&str] = &[
    // Lossless, common in a collection.
    "flac", "wav", "wave", "aiff", "aif", "aifc", "alac", "ape", "wv", "tak", "tta", "mpc", "shn",
    // Lossy.
    "mp3", "aac", "m4a", "m4b", "mp4", "ogg", "oga", "opus", "wma", "mka",
    // DSD.
    "dsf", "dff",
];

/// Whether an entry is hidden by its name.
///
/// A leading dot is how this system hides a file, and it is also how two things
/// a music library must never list are named: the copy `tag.rs` writes beside a
/// file before renaming it, and the `._name.flac` resource fork macOS leaves on
/// non-HFS volumes. The second has an audio extension and is not audio at all --
/// it would be listed, decode to nothing, and be reported as a broken file.
///
/// Applies to the entries a walk or a watcher sees, not to a root a caller names
/// explicitly: a folder called `.music` is a choice, not noise.
pub fn is_hidden(path: &Path) -> bool {
    path.file_name()
        .and_then(|n| n.to_str())
        .map(|n| n.starts_with('.'))
        .unwrap_or(false)
}

/// Whether the engine is expected to open this path.
pub fn is_audio(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| {
            let lower = e.to_ascii_lowercase();
            AUDIO_EXTENSIONS.contains(&lower.as_str())
        })
        .unwrap_or(false)
}

/// Directories a music library never contains music in.
///
/// Skipped because walking them is slow and their contents are never wanted.
/// A folder the listener names is still walked if it *is* one of these -- the
/// rule applies to children, not to the root the caller asked for.
const SKIP_DIRS: &[&str] = &[
    "$RECYCLE.BIN",
    "System Volume Information",
    ".git",
    "node_modules",
    ".Trash",
];

#[derive(Debug)]
pub enum ScanError {
    Io { path: PathBuf, source: std::io::Error },
    /// The walk was cancelled before it finished.
    Cancelled,
}

impl std::fmt::Display for ScanError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io { path, source } => write!(f, "{}: {source}", path.display()),
            Self::Cancelled => write!(f, "the scan was cancelled"),
        }
    }
}

impl std::error::Error for ScanError {}

/// How far into a tree the walk goes.
///
/// A bound rather than none: a folder pointed at a drive root with a junction
/// loop in it would otherwise run until it ran out of memory. 32 is deeper than
/// any music collection and shallow enough to terminate.
const MAX_DEPTH: usize = 32;

/// Every audio file under `root`, sorted.
///
/// Sorted so a scan and a rescan of the same tree produce the same list in the
/// same order: an order that depended on directory enumeration would make every
/// comparison between two scans a diff of noise.
///
/// `cancel` is checked between directories, so a walk of a large drive stops
/// within one directory rather than after the whole tree.
pub fn scan(root: &Path, cancel: &AtomicBool) -> Result<Vec<PathBuf>, ScanError> {
    let mut out = Vec::new();
    let mut seen: BTreeSet<PathBuf> = BTreeSet::new();
    let mut stack: Vec<(PathBuf, usize)> = vec![(root.to_path_buf(), 0)];

    while let Some((dir, depth)) = stack.pop() {
        if cancel.load(Ordering::Relaxed) {
            return Err(ScanError::Cancelled);
        }
        // A directory reached twice is walked once. Junctions and symlinks are
        // not followed, but two spellings of one path are still possible.
        let key = fs::canonicalize(&dir).unwrap_or_else(|_| dir.clone());
        if !seen.insert(key) {
            continue;
        }

        let entries = match fs::read_dir(&dir) {
            Ok(entries) => entries,
            // An unreadable directory is skipped, not fatal: one denied folder
            // in a tree should not cost the listener the other thousand files.
            Err(_) => continue,
        };

        for entry in entries.flatten() {
            let path = entry.path();
            // `file_type` does not follow links, which is what decides whether
            // a symlinked directory is walked. A linked file is still a file
            // the caller may want, so only directories are excluded.
            let Some(file_type) = entry.file_type().ok() else {
                continue;
            };
            if is_hidden(&path) {
                continue;
            }
            if file_type.is_dir() {
                if depth + 1 > MAX_DEPTH {
                    continue;
                }
                let name = entry.file_name();
                let name = name.to_string_lossy();
                if SKIP_DIRS.iter().any(|skip| skip.eq_ignore_ascii_case(&name)) {
                    continue;
                }
                if file_type.is_symlink() {
                    continue;
                }
                stack.push((path, depth + 1));
            } else if is_audio(&path) {
                out.push(path);
            }
        }
    }

    out.sort();
    Ok(out)
}


/// What changed under one root.
///
/// `rescan` is set when the watcher cannot say which files left -- a directory
/// was removed -- because the file names inside it were never enumerated. A
/// host that gets `rescan` re-scans the root; a watcher that invented a list
/// would be reporting files it never saw.
#[derive(Debug, Clone, Default, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Change {
    pub root: PathBuf,
    pub added: Vec<PathBuf>,
    pub removed: Vec<PathBuf>,
    pub rescan: bool,
}

impl Change {
    pub fn is_empty(&self) -> bool {
        self.added.is_empty() && self.removed.is_empty() && !self.rescan
    }
}

/// Translate one backend event into what changed for audio files.
///
/// `exists` is passed in rather than taken from the filesystem so the rule can
/// be tested without one. A rename arrives twice -- once as the old name gone
/// and once as the new name present -- and both halves are real: the library
/// has to forget one and learn the other.
pub fn classify(
    event: &notify::Event,
    exists: impl Fn(&Path) -> bool,
) -> (Vec<PathBuf>, Vec<PathBuf>, Vec<PathBuf>) {
    use notify::EventKind;

    let mut added = Vec::new();
    let mut removed = Vec::new();
    let mut rescan = Vec::new();

    for path in &event.paths {
        // The copy a tag write stages beside its target is hidden and has an
        // audio extension; reporting it as a removal would name a file the
        // library never had.
        if is_hidden(path) {
            continue;
        }
        // A directory's contents were never enumerated, so its appearance or
        // disappearance can only be answered with "look again". Which root has
        // to be rescanned follows from where the directory is.
        if exists(path) && path.is_dir() {
            if matches!(event.kind, EventKind::Create(_)) {
                rescan.push(path.clone());
            }
            continue;
        }
        if is_audio(path) {
            if exists(path) {
                added.push(path.clone());
            } else {
                removed.push(path.clone());
            }
        } else if !exists(path) && matches!(event.kind, EventKind::Remove(_)) {
            // Something that is gone and is not a file we would have listed:
            // possibly a directory of music. The contents cannot be named.
            rescan.push(path.clone());
        }
    }

    for list in [&mut added, &mut removed, &mut rescan] {
        list.sort();
        list.dedup();
    }
    // A path cannot be both: the last state wins, and `exists` decides it.
    removed.retain(|path| !added.contains(path));
    (added, removed, rescan)
}

/// The root a path belongs to, longest match first.
///
/// Longest first because roots can nest -- a folder added inside another one --
/// and the change belongs to the more specific of the two.
pub fn root_of<'a>(path: &Path, roots: &'a [PathBuf]) -> Option<&'a PathBuf> {
    roots
        .iter()
        .filter(|root| path.starts_with(root))
        .max_by_key(|root| root.as_os_str().len())
}

/// A live watcher over the roots the host has added.
///
/// Named `FolderWatcher` rather than `Watcher` because notify's trait of that
/// name has to be in scope for `watch` and `unwatch` to resolve at all.
pub struct FolderWatcher {
    inner: notify::RecommendedWatcher,
    rx: std::sync::mpsc::Receiver<Result<notify::Event, notify::Error>>,
    roots: Vec<PathBuf>,
}

impl FolderWatcher {
    pub fn new() -> Result<Self, ScanError> {
        let (tx, rx) = std::sync::mpsc::channel();
        let inner = notify::recommended_watcher(move |event| {
            // A closed receiver means the worker is gone; dropping the event is
            // the only thing left to do and is not an error worth reporting.
            let _ = tx.send(event);
        })
        .map_err(|e| ScanError::Io {
            path: PathBuf::new(),
            source: std::io::Error::other(e.to_string()),
        })?;
        Ok(Self {
            inner,
            rx,
            roots: Vec::new(),
        })
    }

    /// Watch a root recursively. Watching one twice is not an error.
    pub fn watch(&mut self, root: &Path) -> Result<(), ScanError> {
        if self.roots.contains(&root.to_path_buf()) {
            return Ok(());
        }
        self.inner
            .watch(root, notify::RecursiveMode::Recursive)
            .map_err(|e| ScanError::Io {
                path: root.to_path_buf(),
                source: std::io::Error::other(e.to_string()),
            })?;
        self.roots.push(root.to_path_buf());
        Ok(())
    }

    pub fn unwatch(&mut self, root: &Path) {
        let _ = self.inner.unwatch(root);
        self.roots.retain(|r| r != root);
    }

    pub fn roots(&self) -> &[PathBuf] {
        &self.roots
    }

    /// The next event if one is already waiting, without blocking.
    ///
    /// `Err(())` is the backend saying it lost events -- a full buffer, a handle
    /// that stopped working. Silence would be indistinguishable from "nothing
    /// changed", which is the one thing a watcher must never imply when it does
    /// not know.
    pub fn try_next(&self) -> Result<Option<notify::Event>, ()> {
        match self.rx.try_recv() {
            Ok(Ok(event)) => Ok(Some(event)),
            Ok(Err(_)) => Err(()),
            Err(_) => Ok(None),
        }
    }

}


/// The paths this process is writing, for as long as the write is in flight.
///
/// `tag.rs` replaces a file by staging a copy beside it and renaming over it,
/// which the watcher sees as a file appearing. Reporting that to the host would
/// make every metadata edit look like an import. The writer is in this process,
/// so the window is exact rather than Jellyfin's 45-second guess: the path is
/// marked before the write and stays marked for a moment after it returns,
/// because the backend delivers the rename late.
#[derive(Debug, Default)]
pub struct Suppressed {
    inner: std::sync::Mutex<std::collections::HashMap<PathBuf, std::time::Instant>>,
}

/// How long a path stays suppressed after a write.
///
/// Long enough to cover the backend's delivery of the rename, short enough that
/// a listener who copies a file onto a path this process just wrote still sees
/// it. Two worker ticks (150 ms each) was not enough on this machine; measured
/// events arrive within about a second.
const SUPPRESS_GRACE: std::time::Duration = std::time::Duration::from_secs(2);

impl Suppressed {
    pub fn new() -> std::sync::Arc<Self> {
        std::sync::Arc::new(Self::default())
    }

    /// Mark a path as being written right now.
    pub fn mark(&self, path: &Path) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        inner.insert(path.to_path_buf(), std::time::Instant::now());
    }


    /// Forget a mark. Used when the write was refused before it touched the
    /// file: a path that was never written must not suppress a real event for
    /// the length of the grace period.
    pub fn clear(&self, path: &Path) {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        inner.remove(path);
    }

    /// Whether this path is inside a write window, pruning expired entries.
    pub fn contains(&self, path: &Path) -> bool {
        let mut inner = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        inner.retain(|_, at| at.elapsed() < SUPPRESS_GRACE);
        inner.contains_key(path)
    }
}

/// A finished walk, on its way back to the thread that owns the index.
#[derive(Debug)]
pub struct ScanResult {
    pub root: PathBuf,
    pub present: Vec<crate::library::IndexedFile>,
    /// Files the walk could not stat. Removals are not trustworthy when this is
    /// non-zero: a file that could not be read looks exactly like a file that
    /// was deleted.
    pub unreadable: usize,
    pub error: Option<String>,
}

/// What a host asks the folder worker to do.
#[derive(Debug)]
pub enum Job {
    /// Walk one folder and report every audio file under it.
    Scan { root: PathBuf },
    /// Start reporting changes under one folder.
    Watch { root: PathBuf },
    Unwatch { root: PathBuf },
    /// Reconcile every watched root this often. Zero turns it off.
    SetReconcile { seconds: u64 },
}

/// The worker's state: the roots being watched and the walk's cancel flag.
pub struct Session {
    watcher: Option<FolderWatcher>,
    cancel: AtomicBool,
    suppressed: std::sync::Arc<Suppressed>,
    /// How often to reconcile the watched roots on their own.
    reconcile: std::time::Duration,
    last_reconcile: std::time::Instant,
    /// The last walk, waiting to be collected by the thread that owns the
    /// index. One slot is enough because the loop runs one job and then
    /// collects: a result is taken before the next job can produce one. The
    /// slot exists so the walk and the comparison live on their own threads,
    /// not to queue anything.
    pending: Option<ScanResult>,
}

impl Default for Session {
    fn default() -> Self {
        Self {
            // A watcher that cannot be created is not fatal: scanning still
            // works, and `Watch` reports the failure rather than pretending.
            watcher: FolderWatcher::new().ok(),
            cancel: AtomicBool::new(false),
            suppressed: Suppressed::new(),
            reconcile: std::time::Duration::ZERO,
            last_reconcile: std::time::Instant::now(),
            pending: None,
        }
    }
}

impl Session {
    /// Run one job, returning the event to publish.
    /// How long between automatic reconciliations, zero for never.
    pub fn set_reconcile(&mut self, seconds: u64) {
        self.reconcile = std::time::Duration::from_secs(seconds);
        self.last_reconcile = std::time::Instant::now();
    }

    /// Queue one scan per watched root when the interval has elapsed.
    fn maybe_schedule_reconcile(&mut self, due: &mut std::collections::VecDeque<PathBuf>) {
        if self.reconcile.is_zero() || due.len() > 0 {
            return;
        }
        if self.last_reconcile.elapsed() < self.reconcile {
            return;
        }
        let roots = self
            .watcher
            .as_ref()
            .map(|w| w.roots().to_vec())
            .unwrap_or_default();
        if roots.is_empty() {
            return;
        }
        self.last_reconcile = std::time::Instant::now();
        due.extend(roots);
    }

    /// The suppression set this session consults.
    pub fn suppressed(&self) -> std::sync::Arc<Suppressed> {
        self.suppressed.clone()
    }

    /// Take the finished walk, if one is waiting.
    pub fn take_scan(&mut self) -> Option<ScanResult> {
        self.pending.take()
    }

    pub fn run(&mut self, job: Job) -> crate::protocol::Event {
        match job {
            Job::Scan { root } => {
                // The walk happens here; the comparison against the index
                // happens on the thread that owns it. Handing the result back
                // is what keeps one connection to the library file and one
                // owner of the index.
                self.cancel.store(false, Ordering::Relaxed);
                let result = match walk_indexed(&root, &self.cancel) {
                    Ok(walk) => ScanResult {
                        root: root.clone(),
                        present: walk.files,
                        unreadable: walk.unreadable,
                        error: None,
                    },
                    Err(e) => ScanResult {
                        root: root.clone(),
                        present: Vec::new(),
                        unreadable: 0,
                        error: Some(e.to_string()),
                    },
                };
                self.pending = Some(result);
                // The event the worker itself can answer.
                crate::protocol::Event::ScanStarted { root }
            }
            Job::Watch { root } => match self.watcher.as_mut() {
                None => self.error("no file watcher is available on this system"),
                Some(watcher) => match watcher.watch(&root) {
                    Ok(()) => crate::protocol::Event::FolderWatched {
                        root,
                        roots: watcher.roots().to_vec(),
                    },
                    Err(e) => self.error(&e.to_string()),
                },
            },
            Job::SetReconcile { seconds } => {
                self.set_reconcile(seconds);
                crate::protocol::Event::ReconcileChanged { seconds }
            }
            Job::Unwatch { root } => match self.watcher.as_mut() {
                None => self.error("no file watcher is available on this system"),
                Some(watcher) => {
                    watcher.unwatch(&root);
                    crate::protocol::Event::FolderUnwatched {
                        root,
                        roots: watcher.roots().to_vec(),
                    }
                }
            },
        }
    }

    /// Drain the backend and turn it into what changed for audio files.
    ///
    /// Coalesced into one event per call rather than one per backend event: a
    /// single save produces several, and a host that redrew the list for each
    /// would repaint the same change four times.
    pub fn drain(&mut self) -> Option<crate::protocol::Event> {
        let watcher = self.watcher.as_ref()?;
        let roots = watcher.roots().to_vec();
        if roots.is_empty() {
            return None;
        }

        let mut changes: std::collections::BTreeMap<PathBuf, Change> =
            std::collections::BTreeMap::new();
        let mut lost = false;
        let mark = |changes: &mut std::collections::BTreeMap<PathBuf, Change>,
                        path: &PathBuf,
                        apply: &dyn Fn(&mut Change, PathBuf)| {
            let Some(root) = root_of(path, &roots).cloned() else {
                return;
            };
            let entry = changes.entry(root.clone()).or_insert_with(|| Change {
                root,
                ..Change::default()
            });
            apply(entry, path.clone());
        };

        loop {
            let event = match watcher.try_next() {
                Ok(Some(event)) => event,
                Ok(None) => break,
                Err(()) => {
                    lost = true;
                    continue;
                }
            };
            let (added, removed, rescan) = classify(&event, |p| p.exists());
            for path in &added {
                if self.suppressed.contains(path) {
                    continue;
                }
                mark(&mut changes, path, &|c, p| c.added.push(p));
            }
            for path in &removed {
                if self.suppressed.contains(path) {
                    continue;
                }
                mark(&mut changes, path, &|c, p| c.removed.push(p));
            }
            for path in &rescan {
                mark(&mut changes, path, &|c, _| c.rescan = true);
            }
        }
        if lost {
            // One root is not attributable: the backend does not say which
            // watcher failed. Every root is marked, because the alternative is
            // a library that is silently stale.
            for root in &roots {
                changes
                    .entry(root.clone())
                    .or_insert_with(|| Change {
                        root: root.clone(),
                        ..Change::default()
                    })
                    .rescan = true;
            }
        }

        let mut changed: Vec<Change> = changes.into_values().filter(|c| !c.is_empty()).collect();
        if changed.is_empty() {
            return None;
        }
        for change in &mut changed {
            change.added.sort();
            change.added.dedup();
            change.removed.sort();
            change.removed.dedup();
            change.removed.retain(|p| !change.added.contains(p));
        }
        Some(crate::protocol::Event::FilesChanged { changes: changed })
    }

    fn error(&self, message: &str) -> crate::protocol::Event {
        crate::protocol::Event::Error {
            op: "folders".into(),
            message: message.into(),
        }
    }
}

/// Run jobs and the watcher on one thread.
///
/// The same thread does both because the watcher's channel and the job channel
/// cannot be selected on together without a third crate. A scan therefore delays
/// file events by however long it takes; the events are not lost, they queue in
/// the backend and arrive after it.
pub(crate) fn run(
    jobs: std::sync::mpsc::Receiver<Job>,
    scans: std::sync::mpsc::Sender<ScanResult>,
    events: crate::EventBus,
    suppressed: std::sync::Arc<Suppressed>,
) {
    let mut session = Session::default();
    session.suppressed = suppressed;
    let mut due: std::collections::VecDeque<PathBuf> = Default::default();
    loop {
        // A scheduled reconciliation is the same Job a host sends, so there is
        // one comparison path and one place a diff can come from.
        let job = if let Some(root) = due.pop_front() {
            Some(Job::Scan { root })
        } else {
            match jobs.recv_timeout(std::time::Duration::from_millis(150)) {
                Ok(job) => Some(job),
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => None,
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
            }
        };
        if let Some(job) = job {
            events.emit(session.run(job));
            if let Some(result) = session.take_scan() {
                let _ = scans.send(result);
            }
        }
        session.maybe_schedule_reconcile(&mut due);
        if let Some(event) = session.drain() {
            events.emit(event);
        }
    }
}

#[cfg(test)]
mod watch_tests {
    use super::*;
    use std::collections::HashSet;

    fn event(kind: notify::EventKind, paths: Vec<PathBuf>) -> notify::Event {
        let mut event = notify::Event::new(kind);
        for path in paths {
            event = event.add_path(path);
        }
        event
    }

    /// A file that appeared is added; one that vanished is removed. Both are
    /// decided by whether the path exists now, not by the event kind: a rename
    /// arrives as a create and a remove of two different paths.
    #[test]
    fn a_change_is_decided_by_what_exists_now() {
        let here = PathBuf::from("/music/a.flac");
        let gone = PathBuf::from("/music/b.flac");
        let present: HashSet<PathBuf> = [here.clone()].into_iter().collect();

        let (added, removed, rescan) = classify(
            &event(notify::EventKind::Any, vec![here.clone(), gone.clone()]),
            |p| present.contains(p),
        );
        assert_eq!(added, vec![here]);
        assert_eq!(removed, vec![gone]);
        assert!(rescan.is_empty());
    }

    /// A cover image appearing is not a library change.
    #[test]
    fn a_non_audio_file_is_ignored() {
        let cover = PathBuf::from("/music/cover.jpg");
        let (added, removed, rescan) =
            classify(&event(notify::EventKind::Any, vec![cover.clone()]), |p| p == cover);
        assert!(added.is_empty() && removed.is_empty() && rescan.is_empty());
    }

    /// A directory that went away cannot be enumerated, so the caller has to
    /// look again rather than be told of files nobody listed.
    #[test]
    fn a_removed_directory_asks_for_a_rescan() {
        let dir = PathBuf::from("/music/album");
        let (_, _, rescan) = classify(
            &event(
                notify::EventKind::Remove(notify::event::RemoveKind::Any),
                vec![dir.clone()],
            ),
            |_| false,
        );
        assert_eq!(rescan, vec![dir], "a directory removal has to ask for a rescan");
    }

    /// Nested roots resolve to the more specific one.
    #[test]
    fn the_longest_root_wins() {
        let roots = vec![PathBuf::from("/music"), PathBuf::from("/music/live")];
        let path = PathBuf::from("/music/live/a.flac");
        assert_eq!(root_of(&path, &roots), Some(&PathBuf::from("/music/live")));
        let other = PathBuf::from("/music/a.flac");
        assert_eq!(root_of(&other, &roots), Some(&PathBuf::from("/music")));
    }
}

/// What one reconciliation changed.
///
/// Four lists and no `rescan`: with an index, a directory that went away can be
/// answered with the files that were actually under it. `rescan` survives only
/// as the answer to "the watcher lost events", where the truth genuinely is not
/// known.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Diff {
    pub added: Vec<crate::library::IndexedFile>,
    /// Present before and present now, with a different size or timestamp.
    pub modified: Vec<crate::library::IndexedFile>,
    pub removed: Vec<PathBuf>,
    /// Same file, new path: `(from, to)`.
    pub moved: Vec<(PathBuf, PathBuf)>,
}

impl Diff {
    pub fn is_empty(&self) -> bool {
        self.added.is_empty()
            && self.modified.is_empty()
            && self.removed.is_empty()
            && self.moved.is_empty()
    }
}



/// Whether two paths are plausibly the same file under a new name.
///
/// Either the file name is unchanged and the directory moved, or the directory
/// is unchanged and the name moved. Both are ordinary renames; a change to both
/// at once is not confirmed by anything, and is left as a removal and an
/// addition.
fn same_place(from: &Path, to: &Path) -> bool {
    let same_name = from.file_name() == to.file_name();
    let same_parent = from.parent() == to.parent();
    same_name || same_parent
}

/// How two paths are compared to decide they name one file.
///
/// ASCII case folding, which is what SQLite's `NOCASE` does and what Windows
/// and SMB do for paths. The index uses the same rule on the same columns, so a
/// folder reached under a different spelling is one folder in both places. Two
/// rules here would mean the database merges two rows that the comparison
/// believes are two files -- which is exactly the state that made a case-varied
/// root report a library-wide move.
pub fn path_key(path: &Path) -> String {
    path.to_string_lossy().to_ascii_lowercase()
}

/// Compare what the index remembers with what a walk just found.
///
/// Pure, because this is the part with a rule worth testing: a move is only
/// reported when exactly one removed file and one added file share a size and a
/// timestamp. Anything ambiguous is left as a removal and an addition -- a wrong
/// pairing would hand one file another's play count, and a missed pairing only
/// costs what the old behaviour cost.
pub fn reconcile(indexed: &[crate::library::IndexedFile], present: &[crate::library::IndexedFile]) -> Diff {
    use std::collections::HashMap;

    // Keyed by `path_key`, not by the path itself: the same file reached under
    // a different spelling is one file, and both the index and this comparison
    // have to agree about that.
    let before: HashMap<String, &crate::library::IndexedFile> =
        indexed.iter().map(|f| (path_key(&f.path), f)).collect();
    let after: HashMap<String, &crate::library::IndexedFile> =
        present.iter().map(|f| (path_key(&f.path), f)).collect();

    let mut removed: Vec<PathBuf> = before
        .iter()
        .filter(|(key, _)| !after.contains_key(*key))
        .map(|(_, f)| f.path.clone())
        .collect();
    let mut added: Vec<&crate::library::IndexedFile> = after
        .iter()
        .filter(|(key, _)| !before.contains_key(*key))
        .map(|(_, f)| *f)
        .collect();

    let mut modified: Vec<crate::library::IndexedFile> = Vec::new();
    for (key, now) in &after {
        if let Some(was) = before.get(key) {
            if was.size != now.size || was.mtime_ms != now.mtime_ms {
                modified.push((*now).clone());
            }
        }
    }

    // Pair the gone against the new. `(size, mtime)` identifies a file well
    // enough to spot a rename and badly enough that a collision has to stop it.
    let mut removed_by_key: HashMap<(i64, i64), Vec<PathBuf>> = HashMap::new();
    for path in &removed {
        let was = before[&path_key(path)];
        removed_by_key
            .entry((was.size, was.mtime_ms))
            .or_default()
            .push(path.clone());
    }
    let mut added_by_key: HashMap<(i64, i64), Vec<&crate::library::IndexedFile>> = HashMap::new();
    for file in &added {
        added_by_key
            .entry((file.size, file.mtime_ms))
            .or_default()
            .push(file);
    }

    let mut moved: Vec<(PathBuf, PathBuf)> = Vec::new();
    for (key, from) in &removed_by_key {
        if let Some(to) = added_by_key.get(key) {
            // A unique fingerprint is not enough on its own: two rips of one
            // master can share a size and a timestamp, and a copy-then-delete
            // in the same window produces a pair that never was a rename. A
            // real rename keeps at least one of the name or the directory, so
            // that is the confirmation. It converts a possible wrong pairing
            // into a missed one, which is the direction the design prefers.
            if from.len() == 1 && to.len() == 1 && same_place(&from[0], &to[0].path) {
                moved.push((from[0].clone(), to[0].path.clone()));
            }
        }
    }

    let moved_from: std::collections::HashSet<&PathBuf> = moved.iter().map(|(f, _)| f).collect();
    let moved_to: std::collections::HashSet<&PathBuf> =
        moved.iter().map(|(_, t)| t).collect();
    removed.retain(|p| !moved_from.contains(p));
    added.retain(|f| !moved_to.contains(&f.path));

    let mut added: Vec<crate::library::IndexedFile> = added.into_iter().cloned().collect();
    added.sort_by(|a, b| a.path.cmp(&b.path));
    removed.sort();
    modified.sort_by(|a, b| a.path.cmp(&b.path));
    moved.sort();

    Diff {
        added,
        modified,
        removed,
        moved,
    }
}

/// Walk a root and read what the filesystem says about each file.
///
/// The same traversal as [`scan`], plus the two numbers a reconciliation needs.
/// A file whose metadata cannot be read is left out rather than guessed at: an
/// entry with a zero size would look like a change on every scan.
pub fn walk_indexed(
    root: &Path,
    cancel: &AtomicBool,
) -> Result<Walk, ScanError> {
    use std::time::UNIX_EPOCH;

    let paths = scan(root, cancel)?;
    let mut out = Vec::with_capacity(paths.len());
    let mut unreadable = 0usize;
    for path in paths {
        // A file whose metadata cannot be read is NOT a file that went away.
        // Dropping it here would make the reconciliation report it as removed,
        // which is the one answer the source did not give. Count it instead and
        // let the caller decide; an SMB hiccup must not delete a library.
        let Ok(meta) = fs::metadata(&path) else {
            unreadable += 1;
            continue;
        };
        let mtime_ms = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        out.push(crate::library::IndexedFile {
            path,
            size: meta.len() as i64,
            mtime_ms,
        });
    }
    Ok(Walk { files: out, unreadable })
}

/// What a walk found, and how much of it it could not see.
#[derive(Debug)]
pub struct Walk {
    pub files: Vec<crate::library::IndexedFile>,
    /// Files the walk found but could not stat. Non-zero means the tree was
    /// only partly visible, so absence in `files` proves nothing.
    pub unreadable: usize,
}

#[cfg(test)]
mod reconcile_tests {
    use super::*;
    use crate::library::IndexedFile;

    fn f(path: &str, size: i64, mtime_ms: i64) -> IndexedFile {
        IndexedFile {
            path: PathBuf::from(path),
            size,
            mtime_ms,
        }
    }

    #[test]
    fn an_unchanged_tree_is_an_empty_diff() {
        let files = vec![f("/m/a.flac", 10, 100), f("/m/b.flac", 20, 200)];
        assert!(reconcile(&files, &files).is_empty());
    }

    #[test]
    fn a_new_file_is_added_and_a_gone_one_removed() {
        let before = vec![f("/m/a.flac", 10, 100), f("/m/b.flac", 20, 200)];
        let after = vec![f("/m/a.flac", 10, 100), f("/m/c.flac", 30, 300)];
        let diff = reconcile(&before, &after);
        assert_eq!(diff.added, vec![f("/m/c.flac", 30, 300)]);
        assert_eq!(diff.removed, vec![PathBuf::from("/m/b.flac")]);
        assert!(diff.moved.is_empty());
    }

    #[test]
    fn a_changed_timestamp_is_modified_not_added() {
        let before = vec![f("/m/a.flac", 10, 100)];
        let after = vec![f("/m/a.flac", 10, 999)];
        let diff = reconcile(&before, &after);
        assert_eq!(diff.modified, vec![f("/m/a.flac", 10, 999)]);
        assert!(diff.added.is_empty() && diff.removed.is_empty());
    }

    /// The whole point: a rename keeps the file's identity.
    #[test]
    fn a_rename_is_reported_as_a_move() {
        let before = vec![f("/m/a/track.flac", 10, 100)];
        let after = vec![f("/m/b/track.flac", 10, 100)];
        let diff = reconcile(&before, &after);
        assert_eq!(
            diff.moved,
            vec![(PathBuf::from("/m/a/track.flac"), PathBuf::from("/m/b/track.flac"))]
        );
        assert!(diff.added.is_empty() && diff.removed.is_empty());
    }

    /// Two candidates on either side is ambiguous, and a wrong pairing would
    /// hand one file the other's history. Give up instead.
    #[test]
    fn an_ambiguous_pairing_is_not_a_move() {
        let before = vec![f("/m/a.flac", 10, 100), f("/m/b.flac", 10, 100)];
        let after = vec![f("/m/c.flac", 10, 100)];
        let diff = reconcile(&before, &after);
        assert!(diff.moved.is_empty(), "paired {:?}", diff.moved);
        assert_eq!(diff.added.len(), 1);
        assert_eq!(diff.removed.len(), 2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_root(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("neiro-scan-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("create the tree");
        dir
    }

    fn touch(path: &Path) {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).expect("create the parent");
        }
        fs::write(path, b"x").expect("write a file");
    }

    /// The filter is the engine's decode list. A text file and a cover in the
    /// same folder are not part of the library.
    #[test]
    fn only_audio_files_are_listed() {
        let root = temp_root("filter");
        touch(&root.join("a.flac"));
        touch(&root.join("b.MP3"));
        touch(&root.join("cover.jpg"));
        touch(&root.join("notes.txt"));
        let cancel = AtomicBool::new(false);
        let found = scan(&root, &cancel).expect("scan");
        let names: Vec<String> = found
            .iter()
            .map(|p| p.file_name().unwrap().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names, vec!["a.flac", "b.MP3"], "{}", root.display());
    }

    /// Subdirectories are walked, and the order does not depend on the
    /// filesystem's enumeration order.
    #[test]
    fn the_walk_is_recursive_and_sorted() {
        let root = temp_root("recursive");
        touch(&root.join("z/z.flac"));
        touch(&root.join("a/a.wav"));
        touch(&root.join("m.flac"));
        let cancel = AtomicBool::new(false);
        let found = scan(&root, &cancel).expect("scan");
        let mut sorted = found.clone();
        sorted.sort();
        assert_eq!(found, sorted, "the list is sorted");
        assert_eq!(found.len(), 3);
    }

    /// A missing root is an empty library, not a crash: a folder that was
    /// removed between a launch and the scan is a normal thing.
    #[test]
    fn a_missing_root_lists_nothing() {
        let cancel = AtomicBool::new(false);
        let found = scan(Path::new("/no/such/folder/anywhere"), &cancel).expect("scan");
        assert!(found.is_empty());
    }

    /// Hidden entries are not library content: the tag writer's staged copy
    /// and macOS resource forks both live under a leading dot.
    #[test]
    fn hidden_entries_are_skipped() {
        let root = temp_root("hidden");
        touch(&root.join("real.flac"));
        touch(&root.join(".hidden.flac"));
        touch(&root.join("._resource.flac"));
        touch(&root.join(".hidden-dir/inside.flac"));
        let cancel = AtomicBool::new(false);
        let found = scan(&root, &cancel).expect("scan");
        let names: Vec<String> = found
            .iter()
            .map(|p| p.file_name().unwrap().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names, vec!["real.flac"], "{}", root.display());
    }

    /// TAK decodes in this build, measured on a real library. It is the case
    /// that proves the list has to come from the decoder and not from a
    /// feature table.
    #[test]
    fn tak_is_audio() {
        assert!(is_audio(Path::new("/m/01 - Catch You Catch Me.tak")));
    }

}
