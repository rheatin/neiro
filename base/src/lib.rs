//! Base: the composition root above the engine and the analyzer.
//!
//! Base owns orchestration: the queue, the device choice (passed through, never
//! re-derived), playback commands, the analysis scheduler, the cache and the
//! event stream. It does not own a device format, a position or a measurement;
//! those come from `neiro-audio` and are copied into [`Snapshot`] unchanged.
//!
//! The UI talks to [`BaseHandle`]. It never receives a `Stream`, never links the
//! C ABI and never decodes a file. This is also the boundary a future
//! out-of-process UI can use: [`protocol::Command`] and [`protocol::Event`] are
//! both serializable.
//!
//! # Threading
//!
//! The engine's `Stream` is `!Send` because an exclusive WASAPI stream belongs
//! to the COM apartment of the thread that created it. Base therefore creates,
//! drives and drops the stream on one dedicated audio thread. Commands cross a
//! channel; position and state cross back as events. Analysis runs on a second
//! thread because it decodes a different file and must never block the render
//! loop.

use std::fmt;
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, Sender, SyncSender};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};

use neiro_audio::{
    analyzer_version, CrossfadeParams, CrossfeedParams, ConvolverParams, EqParams,
    ReplayGainParams,
};

mod analysis;
mod cache;
pub mod library;
pub mod capi;
mod control;
pub mod protocol;
#[cfg(windows)]
pub mod transport;

use cache::Cache;
use protocol::{Command, Event, PlaybackPolicy, Snapshot};

#[derive(Debug)]
pub enum BaseError {
    Engine(neiro_audio::EngineError),
    /// The audio thread is gone; no later command can be delivered.
    Closed,
    /// A worker thread could not be started.
    Spawn(String),
}

impl fmt::Display for BaseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BaseError::Engine(e) => write!(f, "{e}"),
            BaseError::Closed => write!(f, "Base audio thread is not running"),
            BaseError::Spawn(why) => write!(f, "could not start Base worker: {why}"),
        }
    }
}

impl std::error::Error for BaseError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            BaseError::Engine(e) => Some(e),
            _ => None,
        }
    }
}

impl From<neiro_audio::EngineError> for BaseError {
    fn from(value: neiro_audio::EngineError) -> Self {
        Self::Engine(value)
    }
}

/// Fan-out to every subscribed UI or logger.
///
/// The channel is bounded and never blocks the control thread. Position is
/// emitted ten times a second, so a full channel drops the newest position and
/// keeps the subscriber; a control event (state, track, settings, analysis,
/// shutdown) that cannot be queued means the subscriber is more than
/// `SUBSCRIBER_DEPTH` events behind, which is a subscriber that has stopped
/// working. That subscriber is dropped rather than allowed to grow without a
/// bound or to stall the thread that owns the session.
///
/// Unbounded channels were the earlier shape: a subscriber that stopped
/// draining accumulated every position event for as long as it lived, on the
/// thread that also drives playback.
/// A subscriber's ticket. Handed out by `subscribe` and given back by
/// `unsubscribe`; a ticket that was already used is not an error.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, serde::Serialize, serde::Deserialize)]
pub struct SubscriptionId(pub u64);

#[derive(Clone, Default)]
pub(crate) struct EventBus {
    inner: Arc<Mutex<Subscribers>>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unsubscribe_frees_the_slot_and_stops_events() {
        let bus = EventBus::default();
        let (first, first_rx) = bus.subscribe();
        let (_second, second_rx) = bus.subscribe();
        assert_eq!(bus.len(), 2);

        assert!(bus.unsubscribe(first));
        assert_eq!(bus.len(), 1, "the slot came back");

        // The dropped subscriber sees the channel close rather than another
        // event: a UI that closed its window must not keep receiving.
        bus.emit(Event::Shutdown);
        assert!(
            first_rx.recv().is_err(),
            "a cancelled subscription receives nothing"
        );
        assert!(matches!(second_rx.recv(), Ok(Event::Shutdown)));

        // A ticket that was already used is not an error the caller has to
        // special-case; it just reports false.
        assert!(!bus.unsubscribe(first));
    }
}

#[derive(Default)]
struct Subscribers {
    next_id: u64,
    list: Vec<(SubscriptionId, SyncSender<Event>)>,
}

/// Deep enough for a burst of control events, shallow enough to be a bound.
const SUBSCRIBER_DEPTH: usize = 64;

impl EventBus {
    /// A new event stream, and the ticket that cancels it.
    ///
    /// The ticket exists because a receiver that is simply dropped looks the
    /// same as one that is slow: the bus only notices when a send fails. A UI
    /// that closes a window wants its slot back now, not on the next event.
    pub fn subscribe(&self) -> (SubscriptionId, Receiver<Event>) {
        let (tx, rx) = mpsc::sync_channel(SUBSCRIBER_DEPTH);
        let mut subs = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let id = SubscriptionId(subs.next_id);
        subs.next_id += 1;
        subs.list.push((id, tx));
        (id, rx)
    }

    /// Drop a subscriber. Returns whether the ticket was still live.
    pub fn unsubscribe(&self, id: SubscriptionId) -> bool {
        let mut subs = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let before = subs.list.len();
        subs.list.retain(|(ticket, _)| *ticket != id);
        subs.list.len() != before
    }

    pub fn emit(&self, event: Event) {
        let mut subs = self.inner.lock().unwrap_or_else(|e| e.into_inner());
        let is_position = matches!(event, Event::Position { .. });
        subs.list.retain(|(_, tx)| match tx.try_send(event.clone()) {
            Ok(()) => true,
            // A full queue drops the newest position; the next one is 100 ms
            // away and carries a later reading.
            Err(std::sync::mpsc::TrySendError::Full(_)) => is_position,
            Err(std::sync::mpsc::TrySendError::Disconnected(_)) => false,
        });
    }

    /// Live subscribers, for a check that needs to see a slot come back.
    #[cfg(test)]
    pub fn len(&self) -> usize {
        self.inner.lock().unwrap_or_else(|e| e.into_inner()).list.len()
    }
}

/// The client-facing handle. Clone it into a UI thread; it carries no audio
/// object and therefore cannot move a stream across an apartment.
#[derive(Clone)]
pub struct BaseHandle {
    commands: Sender<Command>,
    events: EventBus,
    state: Arc<Mutex<Snapshot>>,
}

impl BaseHandle {
    /// Send a command without interpreting it. This is the entry point a remote
    /// UI would use after deserializing a [`Command`].
    pub fn dispatch(&self, command: Command) -> Result<(), BaseError> {
        self.commands.send(command).map_err(|_| BaseError::Closed)
    }

    pub fn play(&self, path: impl Into<PathBuf>, device_id: Option<&str>) -> Result<(), BaseError> {
        self.dispatch(Command::Play {
            path: path.into(),
            device_id: device_id.map(str::to_owned),
        })
    }

    pub fn enqueue(&self, paths: Vec<PathBuf>) -> Result<(), BaseError> {
        self.dispatch(Command::Enqueue { paths })
    }

    /// Take one entry out of the queue by position. The entry that is playing
    /// cannot be removed; the engine would keep playing a track the queue no
    /// longer names.
    pub fn remove_from_queue(&self, index: usize) -> Result<(), BaseError> {
        self.dispatch(Command::RemoveFromQueue { index })
    }

    /// Drop every queued entry except the one playing.
    pub fn clear_queue(&self) -> Result<(), BaseError> {
        self.dispatch(Command::ClearQueue)
    }

    /// Move one queue entry to a different position. The playing entry keeps
    /// playing and its index follows it.
    pub fn move_in_queue(&self, from: usize, to: usize) -> Result<(), BaseError> {
        self.dispatch(Command::MoveInQueue { from, to })
    }

    pub fn pause(&self) -> Result<(), BaseError> {
        self.dispatch(Command::Pause)
    }

    pub fn resume(&self) -> Result<(), BaseError> {
        self.dispatch(Command::Resume)
    }

    pub fn stop(&self) -> Result<(), BaseError> {
        self.dispatch(Command::Stop)
    }

    pub fn seek(&self, seconds: f64) -> Result<(), BaseError> {
        self.dispatch(Command::Seek { seconds })
    }

    pub fn next(&self) -> Result<(), BaseError> {
        self.dispatch(Command::Next)
    }

    pub fn analyze(&self, path: impl Into<PathBuf>) -> Result<(), BaseError> {
        self.dispatch(Command::Analyze {
            path: path.into(),
            cancel_id: None,
        })
    }

    /// Analyze a path under an id a later `cancel_analysis` can name.
    pub fn analyze_with_id(
        &self,
        path: impl Into<PathBuf>,
        cancel_id: u64,
    ) -> Result<(), BaseError> {
        self.dispatch(Command::Analyze {
            path: path.into(),
            cancel_id: Some(cancel_id),
        })
    }

    /// Stop an analysis started with [`analyze_with_id`](Self::analyze_with_id).
    pub fn cancel_analysis(&self, cancel_id: u64) -> Result<(), BaseError> {
        self.dispatch(Command::CancelAnalysis { cancel_id })
    }

    /// Resize the analysis pool. Growing starts threads immediately; shrinking
    /// takes effect as each worker finishes the decode it is on.
    pub fn set_analysis_workers(&self, count: usize) -> Result<(), BaseError> {
        self.dispatch(Command::SetAnalysisWorkers { count })
    }
    pub fn set_volume(&self, gain: f64) -> Result<(), BaseError> {
        self.dispatch(Command::SetVolume { gain })
    }

    pub fn set_eq(&self, params: EqParams) -> Result<(), BaseError> {
        self.dispatch(Command::SetEq { params })
    }

    pub fn set_crossfeed(&self, params: CrossfeedParams) -> Result<(), BaseError> {
        self.dispatch(Command::SetCrossfeed { params })
    }

    pub fn set_replay_gain(&self, params: ReplayGainParams) -> Result<(), BaseError> {
        self.dispatch(Command::SetReplayGain { params })
    }

    pub fn set_convolver(&self, params: ConvolverParams) -> Result<(), BaseError> {
        self.dispatch(Command::SetConvolver { params })
    }

    pub fn set_crossfade(&self, params: CrossfadeParams) -> Result<(), BaseError> {
        self.dispatch(Command::SetCrossfade { params })
    }

    /// Change output mode, resampler quality or share mode. A live stream is
    /// reopened at the current position, because the engine decides these when
    /// it opens the device.
    pub fn set_policy(&self, policy: PlaybackPolicy) -> Result<(), BaseError> {
        self.dispatch(Command::SetPolicy { policy })
    }

    /// Ask Base to list the endpoints. The answer arrives as a
    /// `DevicesChanged` event and is stored in the snapshot.
    pub fn list_devices(&self) -> Result<(), BaseError> {
        self.dispatch(Command::ListDevices)
    }

    /// Choose the endpoint, without touching the queue. A live stream reopens
    /// at the current position on the new endpoint. `None` means "the endpoint
    /// last used"; if there is none, the system default is chosen once.
    pub fn set_device(&self, device_id: Option<&str>) -> Result<(), BaseError> {
        self.dispatch(Command::SetDevice {
            device_id: device_id.map(str::to_owned),
        })
    }

    // ---- library ---------------------------------------------------------

    /// Star or unstar a track. Works with nothing playing.
    pub fn set_favourite(&self, path: impl Into<PathBuf>, on: bool) -> Result<(), BaseError> {
        self.dispatch(Command::SetFavourite {
            path: path.into(),
            on,
        })
    }

    pub fn list_favourites(&self) -> Result<(), BaseError> {
        self.dispatch(Command::ListFavourites)
    }

    pub fn create_playlist(&self, name: impl Into<String>) -> Result<(), BaseError> {
        self.dispatch(Command::CreatePlaylist { name: name.into() })
    }

    pub fn rename_playlist(&self, id: i64, name: impl Into<String>) -> Result<(), BaseError> {
        self.dispatch(Command::RenamePlaylist {
            id,
            name: name.into(),
        })
    }

    pub fn delete_playlist(&self, id: i64) -> Result<(), BaseError> {
        self.dispatch(Command::DeletePlaylist { id })
    }

    pub fn list_playlists(&self) -> Result<(), BaseError> {
        self.dispatch(Command::ListPlaylists)
    }

    pub fn add_to_playlist(&self, id: i64, path: impl Into<PathBuf>) -> Result<(), BaseError> {
        self.dispatch(Command::AddToPlaylist {
            id,
            path: path.into(),
        })
    }

    pub fn remove_from_playlist(
        &self,
        id: i64,
        path: impl Into<PathBuf>,
    ) -> Result<(), BaseError> {
        self.dispatch(Command::RemoveFromPlaylist {
            id,
            path: path.into(),
        })
    }

    pub fn list_playlist_entries(&self, id: i64) -> Result<(), BaseError> {
        self.dispatch(Command::ListPlaylistEntries { id })
    }

    /// Ask which playlists hold a track.
    pub fn list_playlists_containing(&self, path: impl Into<PathBuf>) -> Result<(), BaseError> {
        self.dispatch(Command::ListPlaylistsContaining { path: path.into() })
    }

    /// Record that a track was played. `at_ms` is the host's clock.
    pub fn record_play(&self, path: impl Into<PathBuf>, at_ms: i64) -> Result<(), BaseError> {
        self.dispatch(Command::RecordPlay {
            path: path.into(),
            at_ms,
        })
    }

    /// Ask for the most-played tracks, most first.
    pub fn list_most_played(&self, limit: usize) -> Result<(), BaseError> {
        self.dispatch(Command::ListMostPlayed { limit })
    }

    /// Start or stop forwarding visualiser frames. `None` stops.
    ///
    /// The interval is what the host can draw: 16 ms for a 60 Hz view, 33 for
    /// 30 Hz. Frames are only produced while playing and only while this is set.
    pub fn set_visualiser(&self, interval_ms: Option<u32>) -> Result<(), BaseError> {
        self.dispatch(Command::SetVisualiser { interval_ms })
    }

    /// Analyze a batch of paths ahead of playback.
    ///
    /// Base does not scan directories: the host supplies the paths. Results
    /// arrive as the usual `AnalysisReady` events and
    /// `PreanalysisProgress` reports how far the batch has got.
    pub fn preanalyze(&self, paths: Vec<PathBuf>) -> Result<(), BaseError> {
        self.dispatch(Command::Preanalyze { paths })
    }

    /// Ask the workers to stop. `Base`'s `Drop` also does this and joins.
    pub fn shutdown(&self) -> Result<(), BaseError> {
        self.dispatch(Command::Shutdown)
    }

    /// A new event stream, with the ticket that cancels it. Events emitted
    /// before subscription are not replayed; call [`snapshot`](Self::snapshot)
    /// first for the current state.
    pub fn subscribe(&self) -> (SubscriptionId, Receiver<Event>) {
        self.events.subscribe()
    }

    /// Drop a subscriber. Safe to call twice and safe after shutdown; returns
    /// whether the ticket was still live.
    pub fn unsubscribe(&self, id: SubscriptionId) -> bool {
        self.events.unsubscribe(id)
    }

    /// The latest state. This is a copy owned by Base, not a second source of
    /// truth: the fields in it were written by the audio thread from the engine.
    pub fn snapshot(&self) -> Snapshot {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).clone()
    }
}

/// How many analysis workers to start.
///
/// Each decode holds a decoder and its buffers, so this is deliberately small:
/// the point is that a skip does not queue behind a prefetch, not that every
/// core is busy. `NEIRO_BASE_ANALYSIS_WORKERS` overrides it, which is how the
/// concurrency check compares one worker against several.
fn analysis_workers() -> usize {
    if let Ok(text) = std::env::var("NEIRO_BASE_ANALYSIS_WORKERS") {
        if let Ok(n) = text.trim().parse::<usize>() {
            return n.clamp(1, MAX_ANALYSIS_WORKERS);
        }
    }
    thread::available_parallelism()
        .map(|n| n.get().min(MAX_ANALYSIS_WORKERS))
        .unwrap_or(1)
        .max(1)
}

/// Upper bound on decode threads, whatever the machine reports.
const MAX_ANALYSIS_WORKERS: usize = 4;

/// Owns the workers. Dropping it stops playback and joins every thread.
pub struct Base {
    handle: BaseHandle,
    control_thread: Option<JoinHandle<()>>,
    analysis_threads: analysis::Threads,
    registry: analysis::CancelRegistry,
}

impl Base {
    /// Start Base with exclusive output. No device is opened here; the first
    /// `Play` chooses one.
    pub fn start() -> Result<Self, BaseError> {
        Self::start_with_share(neiro_audio::ShareKind::Exclusive)
    }

    /// Start Base with an explicit share mode. `Shared` is for environments
    /// where another process holds the endpoint exclusively. It is a choice, not
    /// a fallback: the player still never moves to shared on its own.
    pub fn start_with_share(share: neiro_audio::ShareKind) -> Result<Self, BaseError> {
        let policy = match share {
            neiro_audio::ShareKind::Shared => PlaybackPolicy::shared(),
            neiro_audio::ShareKind::Exclusive => PlaybackPolicy::default(),
        };
        Self::start_with_policy(policy)
    }

    /// Start Base with an explicit output policy: rate mode, resampler quality
    /// and share mode. No device is opened here; the first `Play` chooses one.
    ///
    /// No library is opened either: favourites and playlists report "no library"
    /// until [`start_with_policy_and_library`] is used. That is deliberate rather
    /// than an empty library, because "you have no playlists" is a statement
    /// about the listener's music and not about this process.
    pub fn start_with_policy(policy: PlaybackPolicy) -> Result<Self, BaseError> {
        Self::start_inner(policy, None)
    }

    /// Start Base with a library file for favourites and playlists.
    pub fn start_with_policy_and_library(
        policy: PlaybackPolicy,
        library: library::Library,
    ) -> Result<Self, BaseError> {
        Self::start_inner(policy, Some(library))
    }

    fn start_inner(policy: PlaybackPolicy, library: Option<library::Library>) -> Result<Self, BaseError> {
        let version = analyzer_version();
        let state = Arc::new(Mutex::new(Snapshot {
            analyzer_version: version,
            policy,
            ..Snapshot::default()
        }));
        let events = EventBus::default();
        let cache = Arc::new(Mutex::new(Cache::default()));

        let (command_tx, command_rx) = mpsc::channel();
        let (analysis_tx, analysis_rx) = mpsc::channel();
        let registry = analysis::new_registry();
        let batches = analysis::new_batches();

        // The pool's wiring has to exist before the control thread, because the
        // control thread owns the spawner a resize uses.
        let analysis_rx = Arc::new(Mutex::new(analysis_rx));
        let in_flight = analysis::new_in_flight();
        let workers = analysis_workers();
        let target = analysis::Target::new(workers);
        let analysis_threads = analysis::new_threads();
        let live = Arc::new(Mutex::new(
            (0..workers).collect::<std::collections::HashSet<usize>>(),
        ));

        // One spawn closure, used both at start and by a later resize. A resize
        // that spawned its own threads would be a second copy of this wiring,
        // and the copies would drift.
        let spawner: analysis::Spawner = {
            let analysis_rx = analysis_rx.clone();
            let cache = cache.clone();
            let events = events.clone();
            let state = state.clone();
            let in_flight = in_flight.clone();
            let registry = registry.clone();
            let batches = batches.clone();
            let target = target.clone();
            let threads = analysis_threads.clone();
            let live = live.clone();
            Arc::new(move |index: usize| {
                let rx = analysis_rx.clone();
                let cache = cache.clone();
                let events = events.clone();
                let state = state.clone();
                let in_flight = in_flight.clone();
                let registry = registry.clone();
                let batches = batches.clone();
                let target = target.clone();
                let worker_live = live.clone();
                let thread = thread::Builder::new()
                    .name(format!("neiro-base-analysis-{index}"))
                    .spawn(move || {
                        analysis::run(analysis::Worker {
                            requests: rx,
                            live: worker_live,
                            index,
                            target,
                            cache,
                            state,
                            events,
                            version,
                            in_flight,
                            registry,
                            batches,
                        });
                    });
                // Recorded under the shared list, so a thread started by a later
                // resize is joined by the same shutdown path as the ones started
                // here.
                if let Ok(thread) = thread {
                    threads.lock().unwrap_or_else(|e| e.into_inner()).push(thread);
                }
            })
        };

        let audio_events = events.clone();
        let audio_state = state.clone();
        let registry_for_control = registry.clone();
        let batches_for_control = batches.clone();
        let cache_for_control = cache.clone();
        let target_for_control = target.clone();
        let spawn_for_control = spawner.clone();
        let live_for_control = live.clone();
        let control_thread = thread::Builder::new()
            .name("neiro-base-control".into())
            .spawn(move || {
                control::run(control::Start {
                    commands: command_rx,
                    analysis: analysis_tx,
                    events: audio_events,
                    state: audio_state,
                    version,
                    policy,
                    registry: registry_for_control,
                    batches: batches_for_control,
                    cache: cache_for_control,
                    library,
                    target: target_for_control,
                    spawn: spawn_for_control,
                    live: live_for_control,
                });
            })
            .map_err(|e| BaseError::Spawn(e.to_string()))?;

        for index in 0..workers {
            // A worker that failed to start is not fatal: the pool is smaller
            // than intended, and the first request simply waits for a free slot.
            spawner(index);
        }

        Ok(Self {
            handle: BaseHandle {
                commands: command_tx,
                events,
                state,
            },
            control_thread: Some(control_thread),
            analysis_threads,
            registry,
        })
    }

    pub fn handle(&self) -> BaseHandle {
        self.handle.clone()
    }

    pub fn shutdown(mut self) {
        self.stop_workers();
    }

    fn stop_workers(&mut self) {
        // Raise the flags before asking the workers to stop, so a decode that is
        // running ends at the next block instead of after the whole file.
        analysis::cancel_all(&self.registry);
        let _ = self.handle.commands.send(Command::Shutdown);
        if let Some(thread) = self.control_thread.take() {
            let _ = thread.join();
        }
        let threads: Vec<_> = self
            .analysis_threads
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .drain(..)
            .collect();
        for thread in threads {
            let _ = thread.join();
        }
    }
}

impl Drop for Base {
    fn drop(&mut self) {
        self.stop_workers();
    }
}
