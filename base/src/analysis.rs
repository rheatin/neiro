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

use std::collections::HashSet;
use std::path::{Path, PathBuf};
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

pub(crate) fn run(
    requests: Arc<Mutex<Receiver<PathBuf>>>,
    cache: Arc<Mutex<Cache>>,
    state: Arc<Mutex<Snapshot>>,
    events: EventBus,
    version: u32,
    in_flight: InFlight,
) {
    loop {
        // The lock is held only while waiting for the next path, never while
        // decoding: a worker that held it through a decode would serialise every
        // other worker.
        let path = {
            let receiver = requests.lock().unwrap_or_else(|e| e.into_inner());
            match receiver.recv() {
                Ok(path) => path,
                // Every sender is gone: Base is shutting down.
                Err(_) => return,
            }
        };

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

        let analysis = neiro_audio::analyze_file(&path, BUCKETS);
        in_flight
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&key);
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
