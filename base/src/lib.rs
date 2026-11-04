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
pub mod capi;
mod control;
pub mod protocol;

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
#[derive(Clone, Default)]
pub(crate) struct EventBus {
    subscribers: Arc<Mutex<Vec<SyncSender<Event>>>>,
}

/// Deep enough for a burst of control events, shallow enough to be a bound.
const SUBSCRIBER_DEPTH: usize = 64;

impl EventBus {
    pub fn subscribe(&self) -> Receiver<Event> {
        let (tx, rx) = mpsc::sync_channel(SUBSCRIBER_DEPTH);
        let mut subs = self.subscribers.lock().unwrap_or_else(|e| e.into_inner());
        subs.push(tx);
        rx
    }

    pub fn emit(&self, event: Event) {
        let mut subs = self.subscribers.lock().unwrap_or_else(|e| e.into_inner());
        let is_position = matches!(event, Event::Position { .. });
        subs.retain(|tx| match tx.try_send(event.clone()) {
            Ok(()) => true,
            // A full queue drops the newest position; the next one is 100 ms
            // away and carries a later reading.
            Err(std::sync::mpsc::TrySendError::Full(_)) => is_position,
            Err(std::sync::mpsc::TrySendError::Disconnected(_)) => false,
        });
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
        self.dispatch(Command::Analyze { path: path.into() })
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

    /// Ask the workers to stop. `Base`'s `Drop` also does this and joins.
    pub fn shutdown(&self) -> Result<(), BaseError> {
        self.dispatch(Command::Shutdown)
    }

    /// A new event stream. Events emitted before subscription are not replayed;
    /// call [`snapshot`](Self::snapshot) first for the current state.
    pub fn subscribe(&self) -> Receiver<Event> {
        self.events.subscribe()
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
    analysis_threads: Vec<JoinHandle<()>>,
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
    pub fn start_with_policy(policy: PlaybackPolicy) -> Result<Self, BaseError> {
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

        let audio_events = events.clone();
        let audio_state = state.clone();
        let registry_for_control = registry.clone();
        let control_thread = thread::Builder::new()
            .name("neiro-base-control".into())
            .spawn(move || {
                control::run(
                    command_rx,
                    analysis_tx,
                    audio_events,
                    audio_state,
                    version,
                    policy,
                    registry_for_control,
                );
            })
            .map_err(|e| BaseError::Spawn(e.to_string()))?;

        let analysis_rx = Arc::new(Mutex::new(analysis_rx));
        let in_flight = analysis::new_in_flight();
        let workers = analysis_workers();
        let mut analysis_threads = Vec::with_capacity(workers);
        for index in 0..workers {
            let rx = analysis_rx.clone();
            let cache = cache.clone();
            let events = events.clone();
            let state = state.clone();
            let in_flight = in_flight.clone();
            let registry = registry.clone();
            match thread::Builder::new()
                .name(format!("neiro-base-analysis-{index}"))
                .spawn(move || {
                    analysis::run(rx, cache, state, events, version, in_flight, registry);
                }) {
                Ok(thread) => analysis_threads.push(thread),
                Err(e) => {
                    // The control worker is already alive and would wait forever.
                    let _ = command_tx.send(Command::Shutdown);
                    let _ = control_thread.join();
                    for thread in analysis_threads {
                        let _ = thread.join();
                    }
                    return Err(BaseError::Spawn(e.to_string()));
                }
            }
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
        for thread in self.analysis_threads.drain(..) {
            let _ = thread.join();
        }
    }
}

impl Drop for Base {
    fn drop(&mut self) {
        self.stop_workers();
    }
}
