//! The analysis worker: path in, facts out.
//!
//! It owns the cache. It never touches a stream or a device, and a slow or
//! failed analysis leaves playback alone. The current track is written into the
//! shared snapshot only when its path still matches, so a late result for the
//! previous track cannot overwrite the one now playing.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::mpsc::Receiver;
use std::sync::{Arc, Mutex};

use crate::cache::{Cache, CacheKey};
use crate::protocol::{Event, Snapshot};
use crate::EventBus;

const BUCKETS: u32 = 1024;

pub(crate) fn run(
    requests: Receiver<PathBuf>,
    cache: Arc<Mutex<Cache>>,
    state: Arc<Mutex<Snapshot>>,
    events: EventBus,
    version: u32,
) {
    let mut in_flight: HashSet<CacheKey> = HashSet::new();

    while let Ok(path) = requests.recv() {
        let key = CacheKey::for_path(&path, version);

        if let Some(cached) = cache.lock().unwrap_or_else(|e| e.into_inner()).get(&key) {
            publish(&path, cached, true, &state, &events);
            continue;
        }

        // The same path can be requested by the current track and the
        // pre-analysis of the next one. One decode is enough; its result is
        // published to whoever asked by the cache and the event.
        if !in_flight.insert(key.clone()) {
            continue;
        }

        let analysis = neiro_audio::analyze_file(&path, BUCKETS);
        in_flight.remove(&key);
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
