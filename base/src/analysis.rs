//! The analysis workers: path in, facts out.
//!
//! They own the cache. They never touch a stream or a device, and a slow or
//! failed analysis leaves playback alone. The current track is written into the
//! shared snapshot only when its path still matches, so a late result for the
//! previous track cannot overwrite the one now playing.
//!
//! # Why more than one worker
//!
//! The engine's analyzer is per-call state: `computePeaksFromFile` builds its own
//! `neiro::Decoder`, its error text is `thread_local`, and nothing in the path
//! calls an FFmpeg global initialiser. A decode is therefore independent of
//! every other decode, and a single worker made one file wait for another for no
//! reason -- a skip had to queue behind the prefetch of the track it was
//! skipping to.
//!
//! The bound matters more than the count: each decode holds a decoder and its
//! buffers, so this is not "one thread per request".

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicI32, AtomicUsize, Ordering};
use std::sync::mpsc::Receiver;
use std::sync::{Arc, Mutex};

use crate::cache::{Cache, CacheKey};
use crate::protocol::{Event, Snapshot};
use crate::EventBus;

const BUCKETS: u32 = 1024;

/// Requests in flight, shared by every worker.
///
/// The same path is requested twice in normal use: once as the current track and
/// once as the next track's prefetch. Two workers that both picked up the same
/// path would decode it twice and race to publish it. With one worker this set
/// could never hit -- the entry was inserted and removed inside the same
/// iteration -- which is why it looked like dead code.
pub(crate) type InFlight = Arc<Mutex<HashSet<CacheKey>>>;

pub(crate) fn new_in_flight() -> InFlight {
    Arc::new(Mutex::new(HashSet::new()))
}

/// What the control thread still wants analyzed, and every decode that is
/// currently running with the flag that stops it.
///
/// The control thread owns the question "is this still wanted"; a worker owns the
/// decode. This is where they meet, and both halves are needed:
///
/// * `running` stops a decode that has already started. Without it a skip waits
///   for a file nobody is going to look at.
/// * `keep` stops one that has not started yet. A request sitting in the channel
///   when the track changes would otherwise begin decoding after the cancel was
///   issued, and raise no flag because there was nothing to raise it on.
pub(crate) struct Registry {
    keep: HashSet<PathBuf>,
    running: HashMap<PathBuf, Arc<AtomicI32>>,
    /// Host-issued ids for decodes in flight. Separate from `running` because a
    /// cancel names a request, and two requests for the same path are the same
    /// decode -- cancelling one has to stop the decode only if nothing else
    /// wants it, which is what `keep` already answers.
    by_id: HashMap<u64, Arc<AtomicI32>>,
}

pub(crate) type CancelRegistry = Arc<Mutex<Registry>>;

/// How far a pre-analysis batch has got.
///
/// Separate from the request stream because a batch is a set of paths with an
/// outcome each, and the outcomes are not all decodes: a path already in the
/// cache finishes without one, a path another worker is already decoding
/// finishes when that worker publishes, and a path that cannot be opened
/// finishes with an error. Counting `analysis_ready` events would undercount
/// every one of those.
#[derive(Default)]
pub(crate) struct Batch {
    /// Paths still to be accounted for.
    pending: HashSet<PathBuf>,
    total: usize,
}

impl Batch {
    /// A batch of `paths`. Duplicates collapse, because the same file requested
    /// twice is one decode and one outcome -- counting it twice would leave the
    /// progress bar short of its own total.
    pub fn new(paths: Vec<PathBuf>) -> Self {
        let pending: HashSet<PathBuf> = paths.into_iter().collect();
        Self {
            total: pending.len(),
            pending,
        }
    }
}

pub(crate) type Batches = Arc<Mutex<Vec<Batch>>>;

pub(crate) fn new_batches() -> Batches {
    Arc::new(Mutex::new(Vec::new()))
}

/// Account for one path finishing. Emits progress when it belongs to a batch.
fn finish(batches: &Batches, path: &Path, events: &EventBus) {
    let mut guard = batches.lock().unwrap_or_else(|e| e.into_inner());
    let Some(batch) = guard.iter_mut().find(|b| b.pending.contains(path)) else {
        return;
    };
    batch.pending.remove(path);
    let done = batch.total - batch.pending.len();
    let total = batch.total;
    let complete = batch.pending.is_empty();
    // The batch is dropped outside the loop below, so the borrow ends first.
    let finished = complete;
    if finished {
        guard.retain(|b| !b.pending.is_empty());
    }
    drop(guard);
    events.emit(Event::PreanalysisProgress {
        done,
        total,
        complete,
    });
}

pub(crate) fn new_registry() -> CancelRegistry {
    Arc::new(Mutex::new(Registry {
        keep: HashSet::new(),
        running: HashMap::new(),
        by_id: HashMap::new(),
    }))
}

/// Replace the wanted set and stop every decode that is no longer in it.
///
/// A track change is the reason this exists: the decode that was prefetching the
/// track the listener just skipped is no longer worth the CPU, and the decode of
/// the new current track should not have to wait for it.
pub(crate) fn wanted(registry: &CancelRegistry, keep: Vec<PathBuf>) {
    let mut inner = registry.lock().unwrap_or_else(|e| e.into_inner());
    let keep: HashSet<PathBuf> = keep.into_iter().collect();
    for (path, token) in inner.running.iter() {
        if !keep.contains(path) {
            token.store(1, Ordering::Release);
        }
    }
    inner.keep = keep;
}

/// Stop the decode a host named. Returns whether that id was running.
pub(crate) fn cancel_id(registry: &CancelRegistry, id: u64) -> bool {
    let mut inner = registry.lock().unwrap_or_else(|e| e.into_inner());
    match inner.by_id.remove(&id) {
        Some(token) => {
            token.store(1, Ordering::Release);
            true
        }
        None => false,
    }
}

/// Stop everything. Used on shutdown so the workers join promptly instead of
/// finishing a decode nobody will read.
pub(crate) fn cancel_all(registry: &CancelRegistry) {
    let mut inner = registry.lock().unwrap_or_else(|e| e.into_inner());
    for token in inner.running.values() {
        token.store(1, Ordering::Release);
    }
    inner.by_id.clear();
    inner.keep.clear();
}

/// One request from the control thread.
#[derive(Debug, Clone)]
pub(crate) struct Request {
    pub path: PathBuf,
    /// An explicit `analyze` or `enqueue`. It is answered even when the path is
    /// not in the wanted set, because the caller asked for it by name.
    pub explicit: bool,
    /// The host's ticket for this request, if it gave one. A `CancelAnalysis`
    /// for this id stops the decode.
    pub cancel_id: Option<u64>,
}

/// How many workers should be running.
///
/// A worker reads this after every request and exits when its own index is past
/// the target. That is the only safe way to shrink a pool: a thread cannot be
/// killed, and one that is mid-decode has to finish it before it can notice
/// anything. Growing is handled by the control thread spawning new ones.
#[derive(Clone)]
pub(crate) struct Target {
    desired: Arc<AtomicUsize>,
}

impl Target {
    pub fn new(n: usize) -> Self {
        Self {
            desired: Arc::new(AtomicUsize::new(n)),
        }
    }
    pub fn get(&self) -> usize {
        self.desired.load(Ordering::Acquire)
    }
    pub fn set(&self, n: usize) {
        self.desired.store(n, Ordering::Release);
    }
    /// Whether a worker at `index` should keep running.
    pub fn wants(&self, index: usize) -> bool {
        index < self.get()
    }
}

/// Starts a worker and returns its handle.
///
/// The handle matters: `Base::stop_workers` joins every analysis thread, and a
/// spawner that dropped them would leave shutdown returning while a decode was
/// still running -- the process could exit mid-decode with the decoder's buffers
/// unreleased.
pub(crate) type Spawner =
    Arc<dyn Fn(usize) -> Option<std::thread::JoinHandle<()>> + Send + Sync>;

/// Everything an analysis worker is handed at start.
pub(crate) struct Worker {
    pub requests: Arc<Mutex<Receiver<Request>>>,
    /// This worker's slot. A worker whose slot is past the target exits after
    /// the request it is on.
    pub index: usize,
    pub target: Target,
    pub cache: Arc<Mutex<Cache>>,
    pub state: Arc<Mutex<Snapshot>>,
    pub events: EventBus,
    pub version: u32,
    pub in_flight: InFlight,
    pub registry: CancelRegistry,
    pub batches: Batches,
}

pub(crate) fn run(worker: Worker) {
    let Worker {
        requests,
        index,
        target,
        cache,
        state,
        events,
        version,
        in_flight,
        registry,
        batches,
    } = worker;
    loop {
        // A worker only exits while it is *idle*. Checking after taking a
        // request is what broke this: the worker had already removed it from
        // the channel, so returning lost it -- a batch sat at `done = total - 1`
        // forever, and an `analyze` was answered by nobody. The first version
        // did exactly that, and eight paths on a one-worker pool never finished.
        //
        // The check therefore comes before the wait, and the wait is bounded so
        // a worker parked on the channel notices a shrink within a poll.
        if !target.wants(index) {
            return;
        }
        let request = {
            let receiver = requests.lock().unwrap_or_else(|e| e.into_inner());
            match receiver.recv_timeout(std::time::Duration::from_millis(200)) {
                Ok(request) => request,
                // Nothing to do: re-check the target and wait again.
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => continue,
                // Every sender is gone: Base is shutting down.
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return,
            }
        };
        let path = request.path;

        // The track changed while this sat in the channel. Nothing will read the
        // result, so do not spend a decode on it.
        if !request.explicit {
            let wanted = registry
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .keep
                .contains(&path);
            if !wanted {
                finish(&batches, &path, &events);
                continue;
            }
        }

        let key = CacheKey::for_path(&path, version);

        if let Some(cached) = cache.lock().unwrap_or_else(|e| e.into_inner()).get(&key) {
            publish(&path, cached, true, &state, &events);
            finish(&batches, &path, &events);
            continue;
        }

        // Another worker is already decoding this path. It publishes; this
        // request is satisfied by that event.
        if !in_flight
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(key.clone())
        {
            // Another worker owns this path and will finish it; accounting for
            // it here too would count it twice.
            continue;
        }

        let token = Arc::new(AtomicI32::new(0));
        {
            let mut inner = registry.lock().unwrap_or_else(|e| e.into_inner());
            // The cancel may have been raised between the check above and here;
            // publishing the token after it would start a decode nothing stops.
            if !request.explicit && !inner.keep.contains(&path) {
                finish(&batches, &path, &events);
                continue;
            }
            inner.running.insert(path.clone(), token.clone());
            if let Some(id) = request.cancel_id {
                inner.by_id.insert(id, token.clone());
            }
        }

        let analysis = neiro_audio::analyze_file_cancellable(&path, BUCKETS, Some(&token));

        {
            let mut inner = registry.lock().unwrap_or_else(|e| e.into_inner());
            inner.running.remove(&path);
            if let Some(id) = request.cancel_id {
                inner.by_id.remove(&id);
            }
        }
        in_flight
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&key);

        // Cancelled: the result is a prefix of a decode, so it is neither cached
        // nor published. The request that cancelled it has already asked for
        // something else, or Base is shutting down.
        if analysis.error == neiro_audio::ffi::NAE_ERR_CANCELLED {
            // Cancelled paths are still finished as far as the batch is
            // concerned: the batch asked for them and will not ask again, so
            // leaving them pending would hold the progress bar at 99% forever.
            finish(&batches, &path, &events);
            continue;
        }
        if analysis.error == 0 {
            cache
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .put(key, analysis.clone());
        }
        publish(&path, analysis, false, &state, &events);
        finish(&batches, &path, &events);
    }
}

fn publish(
    path: &Path,
    analysis: neiro_audio::Analysis,
    cached: bool,
    state: &Arc<Mutex<Snapshot>>,
    events: &EventBus,
) {
    let is_current = state
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .path
        .as_deref()
        == Some(path);
    if is_current && analysis.error == 0 {
        state.lock().unwrap_or_else(|e| e.into_inner()).analysis = Some(analysis.clone());
    }
    if analysis.error != 0 {
        events.emit(Event::Error {
            op: "analyze".into(),
            message: format!("{}: engine error {}", path.display(), analysis.error),
        });
    }
    events.emit(Event::AnalysisReady {
        path: path.to_path_buf(),
        analysis: Box::new(analysis),
        cached,
    });
}
