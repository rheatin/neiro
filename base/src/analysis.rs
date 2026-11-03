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
use std::sync::atomic::{AtomicI32, Ordering};
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
}

pub(crate) type CancelRegistry = Arc<Mutex<Registry>>;

pub(crate) fn new_registry() -> CancelRegistry {
    Arc::new(Mutex::new(Registry {
        keep: HashSet::new(),
        running: HashMap::new(),
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

/// Stop everything. Used on shutdown so the workers join promptly instead of
/// finishing a decode nobody will read.
pub(crate) fn cancel_all(registry: &CancelRegistry) {
    let mut inner = registry.lock().unwrap_or_else(|e| e.into_inner());
    for token in inner.running.values() {
        token.store(1, Ordering::Release);
    }
    inner.keep.clear();
}

/// One request from the control thread.
#[derive(Debug, Clone)]
pub(crate) struct Request {
    pub path: PathBuf,
    /// An explicit `analyze` or `enqueue`. It is answered even when the path is
    /// not in the wanted set, because the caller asked for it by name.
    pub explicit: bool,
}

pub(crate) fn run(
    requests: Arc<Mutex<Receiver<Request>>>,
    cache: Arc<Mutex<Cache>>,
    state: Arc<Mutex<Snapshot>>,
    events: EventBus,
    version: u32,
    in_flight: InFlight,
    registry: CancelRegistry,
) {
    loop {
        // The lock is held only while waiting for the next path, never while
        // decoding: a worker that held it through a decode would serialise every
        // other worker.
        let request = {
            let receiver = requests.lock().unwrap_or_else(|e| e.into_inner());
            match receiver.recv() {
                Ok(request) => request,
                // Every sender is gone: Base is shutting down.
                Err(_) => return,
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
                continue;
            }
        }

        let key = CacheKey::for_path(&path, version);

        if let Some(cached) = cache.lock().unwrap_or_else(|e| e.into_inner()).get(&key) {
            publish(&path, cached, true, &state, &events);
            continue;
        }

        // Another worker is already decoding this path. It publishes; this
        // request is satisfied by that event.
        if !in_flight
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(key.clone())
        {
            continue;
        }

        let token = Arc::new(AtomicI32::new(0));
        {
            let mut inner = registry.lock().unwrap_or_else(|e| e.into_inner());
            // The cancel may have been raised between the check above and here;
            // publishing the token after it would start a decode nothing stops.
            if !request.explicit && !inner.keep.contains(&path) {
                continue;
            }
            inner.running.insert(path.clone(), token.clone());
        }

        let analysis = neiro_audio::analyze_file_cancellable(&path, BUCKETS, Some(&token));

        registry
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .running
            .remove(&path);
        in_flight
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&key);

        // Cancelled: the result is a prefix of a decode, so it is neither cached
        // nor published. The request that cancelled it has already asked for
        // something else, or Base is shutting down.
        if analysis.error == neiro_audio::ffi::NAE_ERR_CANCELLED {
            continue;
        }
        if analysis.error == 0 {
            cache
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .put(key, analysis.clone());
        }
        publish(&path, analysis, false, &state, &events);
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
