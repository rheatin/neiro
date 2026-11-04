//! The one thread that owns the session handle.
//!
//! The C++ session already owns the playback thread, the stream, the queue and
//! the render clock. Base does not duplicate any of that: it opens one session,
//! sends commands, polls `NAE_SessionState`, and copies the render-path facts
//! into events. The session thread is the only place a WASAPI stream exists.

use std::path::PathBuf;
use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use neiro_audio::{
    default_device_id, OutputMode, Quality, Session, SessionPhase, SessionSettings, ShareKind,
};

use crate::analysis::{self, CancelRegistry};
use crate::cache::Cache;
use crate::library::Library;
use crate::protocol::{
    DeviceInfo, Event, OutputRatePolicy, PlaybackPolicy, PlaybackState, RenderPath,
    ResamplerQuality, ShareMode, Snapshot,
};
use crate::EventBus;

const POLL: Duration = Duration::from_millis(30);
const POSITION_EMIT: Duration = Duration::from_millis(100);

/// Everything the control thread is handed at start.
///
/// A struct rather than eight parameters: the list had grown one field at a
/// time, and a call site that swaps two of them compiles.
pub(crate) struct Start {
    pub commands: Receiver<crate::protocol::Command>,
    pub analysis: Sender<crate::analysis::Request>,
    pub events: EventBus,
    pub state: Arc<Mutex<Snapshot>>,
    pub version: u32,
    pub policy: PlaybackPolicy,
    pub registry: CancelRegistry,
    pub batches: crate::analysis::Batches,
    pub cache: Arc<Mutex<Cache>>,
    pub library: Option<Library>,
    /// The pool's target size, and the means to grow it.
    pub target: crate::analysis::Target,
    pub spawn: crate::analysis::Spawner,
    pub live: std::sync::Arc<std::sync::Mutex<std::collections::HashSet<usize>>>,
}

pub(crate) fn run(start: Start) {
    let Start {
        commands,
        analysis,
        events,
        state,
        version,
        policy,
        registry,
        batches,
        cache,
        library,
        target,
        spawn,
        live,
    } = start;
    let mut control = Control {
        commands,
        analysis,
        events,
        state,
        version,
        session: None,
        queue: Vec::new(),
        index: None,
        device_id: None,
        policy,
        registry,
        batches,
        cache,
        target,
        spawn,
        live,
        devices: Vec::new(),
        library,
        most_played_watched: None,
        visual_interval: None,
        last_visual_emit: Instant::now(),
        settings: None,
        last_serial: None,
        last_phase: None,
        last_render: None,
        resolved: None,
        last_position_emit: Instant::now(),
    };
    control.loop_until_shutdown();
    control.close_session();
    // The control thread may have asked for the default endpoint id, which
    // initialises COM on this thread. Release it after the session is closed.
    neiro_audio::shutdown();
}

struct Control {
    commands: Receiver<crate::protocol::Command>,
    analysis: Sender<crate::analysis::Request>,
    events: EventBus,
    state: Arc<Mutex<Snapshot>>,
    version: u32,
    session: Option<Session>,
    queue: Vec<PathBuf>,
    index: Option<usize>,
    device_id: Option<String>,
    policy: PlaybackPolicy,
    registry: CancelRegistry,
    batches: crate::analysis::Batches,
    /// The analysis cache, so a batch can grow it before the workers start.
    cache: Arc<Mutex<Cache>>,
    target: analysis::Target,
    spawn: analysis::Spawner,
    /// Slots a thread currently occupies, shared with the workers. A slot is
    /// removed when its worker exits, so a shrink-then-grow cannot start two
    /// threads on one index.
    live: std::sync::Arc<std::sync::Mutex<std::collections::HashSet<usize>>>,
    devices: Vec<DeviceInfo>,
    /// The favourites, playlists and play counts. `None` when Base was started
    /// without a library location -- the rest of Base works, and every library
    /// command reports that instead of pretending the answer is empty.
    library: Option<Library>,
    /// How often to publish a visualiser frame, or `None` when nothing is
    /// watching. `None` is the default: forwarding a spectrum costs a copy per
    /// frame, and a UI that has no spectrum view should not pay for it.
    visual_interval: Option<Duration>,
    last_visual_emit: Instant,

    /// The limit of the list the UI is currently watching, or `None` when it
    /// has never asked for one.
    ///
    /// `None` is what stops a play from publishing a list nobody requested: an
    /// event with no subscriber is work the UI cannot use, and the default of
    /// 20 made every play emit one.
    most_played_watched: Option<usize>,
    settings: Option<SessionSettings>,
    last_serial: Option<u64>,
    last_phase: Option<SessionPhase>,
    last_render: Option<RenderPath>,
    resolved: Option<serde_json::Value>,
    last_position_emit: Instant,
}

impl Control {
    fn loop_until_shutdown(&mut self) {
        loop {
            match self.commands.recv_timeout(POLL) {
                Ok(command) => {
                    if !self.handle(command) {
                        return;
                    }
                }
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => return,
            }
            self.poll();
        }
    }

    fn handle(&mut self, command: crate::protocol::Command) -> bool {
        use crate::protocol::Command;
        match command {
            Command::Play { path, device_id } => {
                if let Some(id) = device_id {
                    self.device_id = Some(id);
                }
                // `play` replaces the queue by design: it is the "start this
                // track" command. Choosing an endpoint is `SetDevice`, which
                // deliberately does not touch the queue.
                self.close_session();
                self.queue = vec![path];
                self.index = Some(0);
                self.open_at(0, false, 0.0);
            }
            Command::ListDevices => self.list_devices(),
            Command::SetDevice { device_id } => self.set_device(device_id),
            Command::RemoveFromQueue { index } => self.remove_from_queue(index),
            Command::MoveInQueue { from, to } => self.move_in_queue(from, to),
            Command::ClearQueue => self.clear_queue(),
            Command::Enqueue { paths } => {
                if paths.is_empty() {
                    return true;
                }
                if self.session.is_none() {
                    self.emit_error("queue", "cannot enqueue without an open session");
                    return true;
                }
                let mut errors = Vec::new();
                {
                    let session = self.session.as_mut().expect("checked above");
                    for path in &paths {
                        match session.queue_add(path) {
                            Ok(()) => self.queue.push(path.clone()),
                            Err(e) => errors.push(e.to_string()),
                        }
                    }
                }
                for message in errors {
                    self.emit_error("queue", &message);
                }
                self.request_explicit(&paths);
                self.publish_queue();
            }
            Command::Pause => {
                if let Some(session) = self.session.as_mut() {
                    if let Err(e) = session.pause() {
                        self.emit_error("pause", &e.to_string());
                    }
                }
                self.poll();
            }
            Command::Resume => {
                if let Some(session) = self.session.as_mut() {
                    if let Err(e) = session.resume() {
                        self.emit_error("resume", &e.to_string());
                    }
                }
                self.poll();
            }
            Command::Stop => {
                if let Some(session) = self.session.as_mut() {
                    if let Err(e) = session.stop() {
                        self.emit_error("stop", &e.to_string());
                    }
                }
                self.publish_phase(PlaybackState::Stopped, self.render_path());
            }
            Command::Seek { seconds } => {
                if let Some(session) = self.session.as_mut() {
                    if let Err(e) = session.seek(seconds) {
                        self.emit_error("seek", &e.to_string());
                    }
                }
                self.poll();
            }
            Command::Next => self.skip(),
            Command::SetPolicy { policy } => self.set_policy(policy),
            Command::Analyze { path, cancel_id } => {
                let _ = self.analysis.send(analysis::Request {
                    path,
                    explicit: true,
                    cancel_id,
                });
            }
            Command::CancelAnalysis { cancel_id } => {
                let stopped = analysis::cancel_id(&self.registry, cancel_id);
                if !stopped {
                    // Not an error: the decode may already have finished, and a
                    // host that cancels late is doing the right thing rather
                    // than making a mistake.
                    self.events.emit(Event::Error {
                        op: "cancel".into(),
                        message: format!("no analysis with id {cancel_id} is running"),
                    });
                }
            }
            Command::SetVolume { gain } => self.apply("volume", |session| session.set_volume(gain)),
            Command::SetEq { params } => self.apply("eq", |session| session.set_eq(&params)),
            Command::SetCrossfeed { params } => {
                self.apply("crossfeed", |session| session.set_crossfeed(&params))
            }
            Command::SetReplayGain { params } => {
                self.apply("replaygain", |session| session.set_replay_gain(&params))
            }
            Command::SetConvolver { params } => {
                self.apply("convolver", |session| session.set_convolver(&params))
            }
            Command::SetCrossfade { params } => {
                self.apply("crossfade", |session| session.set_crossfade(&params))
            }
            Command::SetFavourite { path, on } => self.set_favourite(path, on),
            Command::ListFavourites => self.publish_favourites(),
            Command::CreatePlaylist { name } => self.create_playlist(name),
            Command::RenamePlaylist { id, name } => self.rename_playlist(id, name),
            Command::DeletePlaylist { id } => self.delete_playlist(id),
            Command::ListPlaylists => self.publish_playlists(),
            Command::AddToPlaylist { id, path } => self.add_to_playlist(id, path),
            Command::RemoveFromPlaylist { id, path } => self.remove_from_playlist(id, path),
            Command::ListPlaylistEntries { id } => self.publish_playlist_entries(id),
            Command::ListPlaylistsContaining { path } => self.publish_playlists_containing(path),
            Command::RecordPlay { path, at_ms } => self.record_play(path, at_ms),
            Command::Preanalyze { paths } => self.preanalyze(paths),
            Command::SetVisualiser { interval_ms } => self.set_visualiser(interval_ms),
            Command::SetAnalysisWorkers { count } => self.set_analysis_workers(count),
            Command::ListMostPlayed { limit } => self.publish_most_played(limit),
            Command::Shutdown => {
                analysis::cancel_all(&self.registry);
                self.close_session();
                self.events.emit(Event::Shutdown);
                return false;
            }
        }
        true
    }

    fn apply(
        &mut self,
        op: &str,
        setter: impl FnOnce(&mut Session) -> neiro_audio::Result<()>,
    ) {
        let Some(session) = self.session.as_mut() else {
            self.emit_error(op, "no open session");
            return;
        };
        match setter(session) {
            Ok(()) => {
                self.refresh_settings();
                self.poll();
                self.refresh_resolved();
            }
            Err(e) => self.emit_error(op, &e.to_string()),
        }
    }

    fn refresh_settings(&mut self) {
        let settings = match self.session.as_ref() {
            Some(session) => match session.settings() {
                Ok(settings) => settings,
                Err(e) => {
                    self.emit_error("settings", &e.to_string());
                    return;
                }
            },
            None => return,
        };
        self.settings = Some(settings.clone());
        self.events.emit(Event::SettingsChanged { settings });
    }

    fn refresh_resolved(&mut self) {
        let resolved = match self.session.as_ref() {
            Some(session) => session.resolved_json(),
            None => None,
        };
        if let Some(value) = resolved {
            self.resolved = Some(value.clone());
            self.events.emit(Event::ResolvedPathChanged { resolved: value });
        }
    }

    /// Ask the engine for the endpoint list.
    ///
    /// The engine probes each endpoint by really opening it, so this can take a
    /// moment and can report a device as not exclusive-capable simply because
    /// another program holds it. That is the engine's answer, not a cached one,
    /// and Base passes it through unchanged.
    fn list_devices(&mut self) {
        match neiro_audio::enumerate_devices() {
            Ok(devices) => {
                self.devices = devices
                    .into_iter()
                    .map(|d| DeviceInfo {
                        id: d.id,
                        name: d.name,
                        sample_rate: d.sample_rate,
                        bits_per_sample: d.bits_per_sample,
                        valid_bits: d.valid_bits,
                        channels: d.channels,
                        exclusive: d.exclusive,
                        shared: d.shared,
                    })
                    .collect();
                let devices = self.devices.clone();
                self.events.emit(Event::DevicesChanged {
                    devices: devices.clone(),
                });
                self.publish_phase(
                    PlaybackState::from(self.last_phase.unwrap_or(SessionPhase::Idle)),
                    self.render_path(),
                );
            }
            Err(e) => self.emit_error("devices", &e.to_string()),
        }
    }

    /// Choose the endpoint without disturbing the queue.
    ///
    /// A live stream is reopened at the current position on the new endpoint,
    /// because the endpoint is an open-time choice in the engine. That is the
    /// same reason `set_policy` reopens; a UI that showed the new device while
    /// the old one kept playing would be reporting a change that did not happen.
    ///
    /// `None` clears the choice, so the next open asks the engine for the
    /// system default once. It is not a request to move to "some other device"
    /// after a device-lost error: that stays an error.
    fn set_device(&mut self, device_id: Option<String>) {
        if self.device_id == device_id {
            return;
        }
        self.device_id = device_id;
        self.publish_phase(
            PlaybackState::from(self.last_phase.unwrap_or(SessionPhase::Idle)),
            self.render_path(),
        );

        let Some(index) = self.index else {
            return;
        };
        let live = matches!(
            self.last_phase,
            Some(SessionPhase::Playing | SessionPhase::Paused)
        );
        if !live {
            return;
        }
        let paused = matches!(self.last_phase, Some(SessionPhase::Paused));
        let position = self
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .position;

        self.open_at(index, true, position);
        // `open_at` reports its own failure and leaves `self.session` as None.
        // Without this the snapshot kept the previous stream's resolved report,
        // naming an endpoint the engine is no longer on -- a UI would show the
        // old device while nothing was playing.
        if self.session.is_none() {
            self.resolved = None;
            self.events.emit(Event::ResolvedPathChanged {
                resolved: serde_json::Value::Null,
            });
        }
        let mut errors = Vec::new();
        if let Some(session) = self.session.as_mut() {
            if paused {
                if let Err(e) = session.pause() {
                    errors.push(e.to_string());
                }
            }
        }
        for message in errors {
            self.emit_error("device", &message);
        }
        self.poll();
    }

    /// Output mode and resampler quality are open-time settings in the engine,
    /// so a change is only real once the stream is reopened. Reopen at the
    /// current position rather than deferring it to the next track: a UI that
    /// reports "native now" while the old stream keeps resampling is reporting
    /// a change that did not happen.
    ///
    /// With no live stream there is nothing to reopen; the next `open_at` uses
    /// the new policy.
    fn set_policy(&mut self, policy: PlaybackPolicy) {
        if self.policy == policy {
            return;
        }
        self.policy = policy;
        self.events.emit(Event::PolicyChanged { policy });
        self.publish_phase(
            PlaybackState::from(self.last_phase.unwrap_or(SessionPhase::Idle)),
            self.render_path(),
        );

        let Some(index) = self.index else {
            return;
        };
        let paused = matches!(self.last_phase, Some(SessionPhase::Paused));
        let live = matches!(
            self.last_phase,
            Some(SessionPhase::Playing | SessionPhase::Paused)
        );
        if !live {
            return;
        }
        let position = self
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .position;

        self.open_at(index, true, position);
        let mut errors = Vec::new();
        if let Some(session) = self.session.as_mut() {
            if paused {
                if let Err(e) = session.pause() {
                    errors.push(e.to_string());
                }
            }
        }
        for message in errors {
            self.emit_error("policy", &message);
        }
        self.poll();
    }

    /// The library, or an error that says why there is none.
    ///
    /// A missing library is not an empty library: reporting "no playlists" when
    /// the file was never opened would look like the listener has none, which is
    /// a statement about their music rather than about this process.
    fn library(&self, op: &str) -> Option<&Library> {
        if self.library.is_none() {
            self.emit_error(op, "no library is open");
        }
        self.library.as_ref()
    }

    fn set_favourite(&mut self, path: PathBuf, on: bool) {
        let changed = match self.library("favourite") {
            Some(library) => match library.set_favourite(&path, on) {
                Ok(changed) => changed,
                Err(e) => return self.emit_error("favourite", &e.to_string()),
            },
            None => return,
        };
        // A UI that already shows the right star does not need an event, and one
        // that does not will get the list below.
        if changed {
            self.publish_favourites();
        }
    }

    fn publish_favourites(&mut self) {
        let paths = match self.library("favourites") {
            Some(library) => match library.favourites() {
                Ok(paths) => paths,
                Err(e) => return self.emit_error("favourites", &e.to_string()),
            },
            None => return,
        };
        self.events.emit(Event::FavouritesChanged { paths });
    }

    fn create_playlist(&mut self, name: String) {
        let id = match self.library("playlist") {
            Some(library) => match library.create_playlist(&name) {
                Ok(playlist) => playlist.id,
                Err(e) => return self.emit_error("playlist", &e.to_string()),
            },
            None => return,
        };
        self.publish_playlists();
        self.publish_playlist_entries(id);
    }

    fn rename_playlist(&mut self, id: i64, name: String) {
        match self.library("playlist") {
            Some(library) => {
                if let Err(e) = library.rename_playlist(id, &name) {
                    return self.emit_error("playlist", &e.to_string());
                }
            }
            None => return,
        }
        self.publish_playlists();
    }

    fn delete_playlist(&mut self, id: i64) {
        match self.library("playlist") {
            Some(library) => {
                if let Err(e) = library.delete_playlist(id) {
                    return self.emit_error("playlist", &e.to_string());
                }
            }
            None => return,
        }
        self.publish_playlists();
        // The entries of a deleted playlist are gone, and a UI that still shows
        // them is showing a playlist that no longer exists.
        self.events.emit(Event::PlaylistEntriesChanged {
            id,
            entries: Vec::new(),
        });
    }

    fn publish_playlists(&mut self) {
        let playlists = match self.library("playlists") {
            Some(library) => match library.playlists() {
                Ok(list) => list,
                Err(e) => return self.emit_error("playlists", &e.to_string()),
            },
            None => return,
        };
        self.events.emit(Event::PlaylistsChanged { playlists });
    }

    fn add_to_playlist(&mut self, id: i64, path: PathBuf) {
        let changed = match self.library("playlist") {
            Some(library) => match library.add_to_playlist(id, &path) {
                Ok(changed) => changed,
                Err(e) => return self.emit_error("playlist", &e.to_string()),
            },
            None => return,
        };
        if changed {
            self.publish_playlists();
            self.publish_playlist_entries(id);
        }
    }

    fn remove_from_playlist(&mut self, id: i64, path: PathBuf) {
        let changed = match self.library("playlist") {
            Some(library) => match library.remove_from_playlist(id, &path) {
                Ok(changed) => changed,
                Err(e) => return self.emit_error("playlist", &e.to_string()),
            },
            None => return,
        };
        if changed {
            self.publish_playlists();
            self.publish_playlist_entries(id);
        }
    }

    fn publish_playlist_entries(&mut self, id: i64) {
        let entries = match self.library("playlist") {
            Some(library) => match library.playlist_entries(id) {
                Ok(entries) => entries,
                Err(e) => return self.emit_error("playlist", &e.to_string()),
            },
            None => return,
        };
        self.events.emit(Event::PlaylistEntriesChanged { id, entries });
    }

    fn publish_playlists_containing(&mut self, path: PathBuf) {
        let playlists = match self.library("playlist") {
            Some(library) => match library.playlists_containing(&path) {
                Ok(list) => list,
                Err(e) => return self.emit_error("playlist", &e.to_string()),
            },
            None => return,
        };
        self.events.emit(Event::PlaylistsContainingChanged { path, playlists });
    }

    /// Start analyzing a batch the host supplied.
    ///
    /// The cache is grown to hold the batch before anything is queued: a scan
    /// against a 256-entry cache would evict its own early results as it
    /// produced them, and the ten minutes it cost would leave a fortieth of it
    /// behind.
    ///
    /// Every path is requested as explicit, so the wanted-set rule that cancels
    /// a stale prefetch does not drop half a library scan when the track changes.
    fn preanalyze(&mut self, paths: Vec<PathBuf>) {
        if paths.is_empty() {
            return;
        }
        let total = paths.len();
        {
            let mut cache = self.cache.lock().unwrap_or_else(|e| e.into_inner());
            cache.reserve(total);
        }
        {
            let mut batches = self.batches.lock().unwrap_or_else(|e| e.into_inner());
            batches.push(crate::analysis::Batch::new(paths.clone()));
        }
        for path in &paths {
            let _ = self.analysis.send(crate::analysis::Request {
                path: path.clone(),
                explicit: true,
                cancel_id: None,
            });
        }
        // A batch of one that is already cached finishes synchronously inside
        // the worker before this event is built, so the first progress event
        // can arrive before this one -- which is fine, they are ordered by the
        // bus and a UI that paints from either lands on the same number.
        self.events.emit(Event::PreanalysisProgress {
            done: 0,
            total,
            complete: false,
        });
    }

    /// Resize the analysis pool.
    ///
    /// Growing spawns immediately. Shrinking only moves the target: the workers
    /// past it exit after the decode they are on, because a thread cannot be
    /// killed and abandoning a decode halfway would leave the batch accounting
    /// short of its own total.
    ///
    /// The count is clamped to the same bound the startup default uses, so a
    /// host cannot ask for more decode threads than the machine should run.
    fn set_analysis_workers(&mut self, count: usize) {
        let count = count.clamp(1, crate::MAX_ANALYSIS_WORKERS);
        let before = self.target.get();
        if count == before {
            return;
        }
        self.target.set(count);
        if count > before {
            for index in before..count {
                // A worker whose slot was cut has not necessarily noticed yet:
                // it checks the target at the top of its loop, up to one poll
                // (200 ms) or one decode later. Shrinking and growing within
                // that window would start a second thread on the same slot,
                // and both would see `wants(index)` true and neither would
                // exit -- the pool runs at `count + duplicates` with a decoder
                // each, which is what the bound exists to prevent.
                //
                // `live` is the set of slots a thread currently occupies; it is
                // cleared when a worker notices it is past the target.
                let mut live = self.live.lock().unwrap_or_else(|e| e.into_inner());
                if live.insert(index) {
                    (self.spawn)(index);
                }
            }
        }
        self.events.emit(Event::AnalysisWorkersChanged {
            count: self.target.get(),
        });
    }

    /// Start or stop the visualiser stream.
    ///
    /// The interval is what the host can draw, not what the engine produces: the
    /// session publishes a tap snapshot every slice, and this decides how often
    /// one is forwarded. A UI drawing at 60 Hz asks for 16 ms; one drawing at
    /// 30 Hz asks for 33 and pays half the copies.
    fn set_visualiser(&mut self, interval_ms: Option<u32>) {
        self.visual_interval = interval_ms.map(|ms| Duration::from_millis(u64::from(ms.max(1))));
        // A new interval starts a fresh window, so a UI that just opened its
        // spectrum view does not wait out the previous interval.
        self.last_visual_emit = Instant::now();
        if self.visual_interval.is_some() {
            self.poll();
        }
    }

    /// Forward one frame if the interval has elapsed and something is watching.
    ///
    /// Reading the taps is a copy of two vectors; it happens here rather than in
    /// `poll`'s hot path so a host with no spectrum view pays nothing.
    fn maybe_emit_visual(&mut self) {
        let Some(interval) = self.visual_interval else {
            return;
        };
        if self.last_visual_emit.elapsed() < interval {
            return;
        }
        let Some(session) = self.session.as_ref() else {
            return;
        };
        let (spectrum, envelope) = match (session.spectrum(), session.envelope()) {
            (Ok(s), Ok(e)) => (s, e),
            // Nothing published yet, or the session is between tracks. Saying
            // nothing is right: a zeroed frame would draw a flat line, which
            // reads as silence rather than as "no reading".
            _ => return,
        };
        self.last_visual_emit = Instant::now();
        self.events.emit(Event::VisualFrame {
            frame: crate::protocol::VisualFrame {
                bins: spectrum.bins,
                fft_size: spectrum.fft_size,
                sample_rate: spectrum.sample_rate,
                peak: spectrum.peak,
                rms: spectrum.rms,
                frames_fed: spectrum.frames_fed,
                envelope: envelope.frames,
                envelope_hop_ms: envelope.frame_hop_ms,
            },
        });
    }

    fn record_play(&mut self, path: PathBuf, at_ms: i64) {
        match self.library("play") {
            Some(library) => {
                if let Err(e) = library.record_play(&path, at_ms) {
                    return self.emit_error("play", &e.to_string());
                }
            }
            None => return,
        }
        // The count changed, so a list that is showing it is now stale -- but
        // only if one is showing it. Refreshing a list nobody asked for costs a
        // query and an event that no subscriber can use.
        if let Some(limit) = self.most_played_watched {
            self.publish_most_played(limit);
        }
    }

    fn publish_most_played(&mut self, limit: usize) {
        self.most_played_watched = Some(limit);
        let tracks = match self.library("most played") {
            Some(library) => match library.most_played(limit) {
                Ok(list) => list,
                Err(e) => return self.emit_error("most played", &e.to_string()),
            },
            None => return,
        };
        self.events.emit(Event::MostPlayedChanged {
            tracks: tracks
                .into_iter()
                .map(|(path, play_count)| crate::protocol::PlayedTrack { path, play_count })
                .collect(),
        });
    }

    /// Rebuild the engine's queue from Base's, after a local edit.
    ///
    /// The engine's queue is what it will actually seam to, so an edit that
    /// only changed Base's copy would be a queue the UI shows and the engine
    /// does not have. `queue_clear` also drops whatever was preloaded, which is
    /// why the rebuild re-adds from the current index forward.
    fn rebuild_engine_queue(&mut self) {
        let Some(index) = self.index else {
            return;
        };
        let Some(session) = self.session.as_mut() else {
            return;
        };
        let mut errors = Vec::new();
        if let Err(e) = session.queue_clear() {
            errors.push(e.to_string());
        }
        for more in self.queue.iter().skip(index + 1) {
            if let Err(e) = session.queue_add(more) {
                errors.push(e.to_string());
            }
        }
        for message in errors {
            self.emit_error("queue", &message);
        }
        self.request_current_and_next();
    }

    fn remove_from_queue(&mut self, index: usize) {
        if index >= self.queue.len() {
            self.emit_error("queue", "no such queue entry");
            return;
        }
        if self.index == Some(index) {
            // The stream is playing this entry. Removing it would leave Base
            // naming a track the queue does not contain, so this is refused
            // rather than guessed at.
            self.emit_error("queue", "cannot remove the entry that is playing");
            return;
        }
        self.queue.remove(index);
        if let Some(current) = self.index {
            if index < current {
                // Everything after the removed slot moved down by one, and so
                // did the playing entry's own position.
                self.index = Some(current - 1);
            }
        }
        self.rebuild_engine_queue();
        self.publish_queue();
    }

    /// Move a queue entry, keeping the playing one playing.
    ///
    /// The index has to be recomputed rather than incremented: moving an entry
    /// across the playing one changes its position, and a UI that then read
    /// `queue_index` would point at a different file than the one playing.
    fn move_in_queue(&mut self, from: usize, to: usize) {
        if from >= self.queue.len() {
            self.emit_error("queue", "no such queue entry");
            return;
        }
        if to >= self.queue.len() {
            self.emit_error("queue", "no such queue position");
            return;
        }
        if from == to {
            return;
        }
        let Some(current) = self.index else {
            self.emit_error("queue", "nothing is playing");
            return;
        };

        let entry = self.queue.remove(from);
        self.queue.insert(to, entry);

        // Where the playing entry ended up. It is identified by the position it
        // held, not by the path: two queue entries can name the same file, and
        // following a path would jump to the wrong one.
        self.index = Some(reindex(current, from, to));

        self.rebuild_engine_queue();
        self.publish_queue();
    }

    fn clear_queue(&mut self) {
        let Some(index) = self.index else {
            self.queue.clear();
            self.publish_queue();
            return;
        };
        // Only the entry that is playing survives, and it becomes the head.
        // Truncating to `index + 1` would keep every already-played entry
        // ahead of it -- a queue that grows the longer a playlist is played
        // through, and whose "next" is still the old one.
        let playing = self.queue.remove(index);
        self.queue.clear();
        self.queue.push(playing);
        self.index = Some(0);
        self.rebuild_engine_queue();
        self.publish_queue();
    }

    fn skip(&mut self) {
        let Some(index) = self.index else {
            return;
        };
        if index + 1 >= self.queue.len() {
            self.close_session();
            self.index = None;
            self.publish_phase(PlaybackState::Stopped, self.render_path());
            return;
        }

        // The engine can switch at the source boundary, which keeps the device
        // stream, the negotiated format and the resampler phase. Closing and
        // reopening cost all three and left an audible gap, which is why this
        // is the first path tried rather than the fallback.
        //
        // The session owns the queue it was seeded with and pops as it preloads,
        // so after this switch its next preload is already the track after the
        // one being opened. Base only has to move its own index.
        let skipped = match self.session.as_mut() {
            Some(session) => match session.skip_to_next() {
                Ok(()) => true,
                // Nothing preloaded: an empty queue, or a format the engine
                // cannot seam across (gapless requires the same rate and
                // channel count). Reopening is the honest answer for those.
                Err(_) => false,
            },
            None => false,
        };

        if skipped {
            // The index is not advanced here. `poll` moves it when the engine's
            // serial changes, which is the moment the audio actually switched;
            // advancing now would report track B while track A is still what the
            // device is being fed, and every path-derived fact would be about a
            // file that is not playing.
            self.last_position_emit = Instant::now();
            self.poll();
            self.request_current_and_next();
            return;
        }
        self.open_at(index + 1, true, 0.0);
    }

    /// Open the queue entry at `index`.
    ///
    /// `resume_at` is where the new stream should start, or 0.0 for the
    /// beginning. It is applied **before** `play`, which is the whole reason it
    /// is a parameter: the first version seeked after `play` and polled before
    /// returning, so by the time a caller could read the snapshot the new
    /// session had already advanced and no check could tell a resumed stream
    /// from one that had restarted. Seeking before the first slice removes that
    /// window entirely -- there is no audio in flight to race against.
    fn open_at(&mut self, index: usize, add_remaining: bool, resume_at: f64) {
        self.close_session();
        let Some(path) = self.queue.get(index).cloned() else {
            self.index = None;
            return;
        };
        self.index = Some(index);

        let device_id = match self.device_id.clone() {
            Some(id) => Some(id),
            None => match default_device_id() {
                Ok(id) => {
                    self.device_id = Some(id.clone());
                    Some(id)
                }
                Err(e) => {
                    self.emit_error("open", &e.to_string());
                    self.publish_phase(PlaybackState::Error, self.render_path());
                    return;
                }
            },
        };

        let (mode, quality, share) = to_engine(self.policy);
        match Session::open(&path, device_id.as_deref(), mode, quality, share) {
            Ok(mut session) => {
                if resume_at > 0.0 {
                    if let Err(e) = session.seek(resume_at) {
                        self.emit_error("open", &e.to_string());
                    }
                    // A seek is a message to the session thread, so the position
                    // is not `resume_at` the moment the call returns. Publishing
                    // straight away reported 0.000 and looked like a reopen that
                    // had restarted -- the fact and the request are two things,
                    // and the fact is what gets published.
                    //
                    // Bounded, and bounded short: the seek is applied on the
                    // session thread within a slice or two. A timeout is not an
                    // error here -- the reopen still happened, and the position
                    // it reached is reported as whatever the session says.
                    let deadline = Instant::now() + Duration::from_millis(500);
                    while Instant::now() < deadline {
                        match session.state() {
                            Ok(state) if (state.position - resume_at).abs() < 0.10 => break,
                            Ok(_) => std::thread::sleep(Duration::from_millis(2)),
                            Err(_) => break,
                        }
                    }
                }
                let serial = session.state().ok().map(|s| s.track_serial);
                self.last_serial = serial;
                self.session = Some(session);

                // Publish before `play`, not after.
                //
                // `play` starts the render thread, so the moment it returns the
                // position has already moved past whatever it was asked to
                // resume at. A snapshot taken after it therefore describes a
                // stream that has been playing, and a caller could never tell a
                // reopen that resumed at the right place from one that restarted
                // at zero -- four different checks were written against that
                // window and all of them passed with the seek removed.
                //
                // Publishing the ready state here makes the reopen observable:
                // the position in this snapshot is exactly `resume_at`.
                self.publish_phase(PlaybackState::from(SessionPhase::Ready), self.render_path());
                self.refresh_settings();
                self.refresh_resolved();

                if let Some(session) = self.session.as_mut() {
                    if let Err(e) = session.play() {
                        self.emit_error("play", &e.to_string());
                    }
                }
                if add_remaining {
                    let mut errors = Vec::new();
                    if let Some(session) = self.session.as_mut() {
                        for more in self.queue.iter().skip(index + 1) {
                            if let Err(e) = session.queue_add(more) {
                                errors.push(e.to_string());
                            }
                        }
                    }
                    for message in errors {
                        self.emit_error("queue", &message);
                    }
                }
                self.last_position_emit = Instant::now();
                self.request_current_and_next();
                self.poll();
            }
            Err(e) => {
                self.emit_error("open", &e.to_string());
                self.session = None;
                self.publish_phase(PlaybackState::Error, self.render_path());
            }
        }
    }

    fn poll(&mut self) {
        let Some(session) = self.session.as_ref() else {
            return;
        };
        let state = match session.state() {
            Ok(state) => state,
            Err(e) => {
                self.emit_error("state", &e.to_string());
                return;
            }
        };

        let render = RenderPath::from(state.clone());
        let phase = state.phase();
        let current_path = self.current_path();
        let mut track_changed = false;

        if let Some(serial) = self.last_serial {
            if state.track_serial != serial {
                if let Some(index) = self.index {
                    if index + 1 < self.queue.len() {
                        self.index = Some(index + 1);
                    }
                }
                self.last_serial = Some(state.track_serial);
                self.request_current_and_next();
                track_changed = true;
            }
        } else {
            self.last_serial = Some(state.track_serial);
        }

        let phase_changed = self.last_phase != Some(phase);
        if phase_changed && matches!(phase, SessionPhase::DeviceLost | SessionPhase::Error) {
            let message = session.last_error();
            self.emit_error(
                "session",
                if message.is_empty() {
                    "session failed"
                } else {
                    &message
                },
            );
        }
        // Read while `session` is still borrowed immutably. `refresh_resolved`
        // below takes `&mut self`, which ends this borrow -- so the count has to
        // be taken before it, not after.
        let engine_queue = session.queue_count().ok().map(|n| n.max(0) as usize);

        self.last_phase = Some(phase);
        if track_changed {
            self.refresh_resolved();
        }

        if let Some(settings) = self.settings.as_mut() {
            // The getter is a copy of the target; the state snapshot is what the
            // session thread has actually applied. Use the state for the live
            // volume so a set followed by an immediate snapshot cannot show the
            // old target.
            settings.volume = state.volume;
        }

        let mut guard = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let path_changed = guard.path != current_path;
        guard.state = PlaybackState::from(phase);
        guard.path = current_path.clone();
        guard.device_id = self.device_id.clone();
        guard.devices = self.devices.clone();
        guard.position = state.position;
        guard.duration = state.duration;
        guard.track_serial = state.track_serial;
        guard.render_path = Some(render.clone());
        guard.settings = self.settings.clone();
        guard.resolved = self.resolved.clone();
        guard.queue = self.queue.clone();
        guard.queue_index = self.index;
        guard.engine_queue_count = engine_queue;
        guard.policy = self.policy;
        guard.analyzer_version = self.version;
        if path_changed {
            guard.analysis = None;
        }
        let snapshot = guard.clone();
        drop(guard);

        if path_changed || track_changed || phase_changed {
            self.events.emit(Event::State(Box::new(snapshot.clone())));
            if let Some(path) = current_path.clone() {
                self.events.emit(Event::TrackChanged {
                    path,
                    device_id: self.device_id.clone().unwrap_or_default(),
                    render_path: render.clone(),
                });
            }
        }
        if self.last_render.as_ref() != Some(&render) {
            self.last_render = Some(render.clone());
            if let Some(path) = current_path {
                self.events.emit(Event::RenderPathChanged {
                    path,
                    render_path: render,
                });
            }
        }
        if self.last_position_emit.elapsed() >= POSITION_EMIT {
            self.emit_position();
            self.last_position_emit = Instant::now();
        }
        // After the state, so a UI that redraws on `State` already has the new
        // position when the first frame arrives.
        self.maybe_emit_visual();
    }

    fn close_session(&mut self) {
        if let Some(mut session) = self.session.take() {
            let _ = session.stop();
            // Dropping the session closes it and joins its thread.
        }
        self.last_serial = None;
        self.last_phase = None;
    }

    fn current_path(&self) -> Option<PathBuf> {
        self.index.and_then(|index| self.queue.get(index)).cloned()
    }

    fn request_current_and_next(&self) {
        let mut paths = Vec::new();
        if let Some(index) = self.index {
            if let Some(path) = self.queue.get(index) {
                paths.push(path.clone());
            }
            if let Some(path) = self.queue.get(index + 1) {
                paths.push(path.clone());
            }
        }
        // Anything else being decoded belongs to a track the listener has moved
        // past. Publishing the wanted set stops both halves of the problem: a
        // decode already running is flagged, and a request still in the channel
        // finds itself unwanted when a worker picks it up.
        analysis::wanted(&self.registry, paths.clone());
        for path in paths {
            let _ = self.analysis.send(analysis::Request {
                path,
                explicit: false,
                cancel_id: None,
            });
        }
    }

    /// Requests the caller asked for by name: an `analyze` command or a newly
    /// enqueued track. They are not governed by the wanted set.
    fn request_explicit(&self, paths: &[PathBuf]) {
        for path in paths {
            let _ = self.analysis.send(analysis::Request {
                path: path.clone(),
                explicit: true,
                cancel_id: None,
            });
        }
    }

    fn emit_position(&self) {
        let Some(path) = self.current_path() else {
            return;
        };
        let state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        self.events.emit(Event::Position {
            path,
            position: state.position,
            duration: state.duration,
        });
    }

    fn emit_error(&self, op: &str, message: &str) {
        self.events.emit(Event::Error {
            op: op.into(),
            message: message.into(),
        });
    }

    fn render_path(&self) -> Option<RenderPath> {
        self.session
            .as_ref()
            .and_then(|session| session.state().ok())
            .map(RenderPath::from)
    }

    fn publish_queue(&mut self) {
        self.publish_phase(
            PlaybackState::from(self.last_phase.unwrap_or(SessionPhase::Idle)),
            self.render_path(),
        );
        self.events.emit(Event::QueueChanged {
            queue: self.queue.clone(),
            index: self.index,
        });
    }

    fn publish_phase(&mut self, state: PlaybackState, render: Option<RenderPath>) -> Snapshot {
        let snapshot = {
            let mut guard = self.state.lock().unwrap_or_else(|e| e.into_inner());
            guard.state = state;
            guard.path = self.current_path();
            guard.device_id = self.device_id.clone();
            guard.devices = self.devices.clone();
            // The position and the duration come from the live session, not from
            // whatever the guard held. `publish_phase` used to leave both alone,
            // so a snapshot published right after a reopen carried the *previous*
            // stream's position -- a state that described a stream that no longer
            // existed, and which hid a reopen that had resumed at the wrong place.
            if let Some(session) = self.session.as_ref() {
                if let Ok(live) = session.state() {
                    guard.position = live.position;
                    guard.duration = live.duration;
                    guard.track_serial = live.track_serial;
                }
                guard.engine_queue_count =
                    session.queue_count().ok().map(|n| n.max(0) as usize);
            }
            guard.render_path = render;
            guard.settings = self.settings.clone();
            guard.resolved = self.resolved.clone();
            guard.queue = self.queue.clone();
            guard.queue_index = self.index;
            guard.policy = self.policy;
            guard.analyzer_version = self.version;
            guard.clone()
        };
        self.events.emit(Event::State(Box::new(snapshot.clone())));
        snapshot
    }
}

/// One translation, in one place: the policy is the only description of an
/// open request, and the engine's types are derived from it here.
fn to_engine(policy: PlaybackPolicy) -> (OutputMode, Quality, ShareKind) {
    let mode = match policy.rate {
        OutputRatePolicy::Native => OutputMode::Native,
        OutputRatePolicy::Device => OutputMode::Device,
        OutputRatePolicy::Fixed { rate_hz } => OutputMode::Fixed(rate_hz),
    };
    let quality = match policy.quality {
        ResamplerQuality::Fast => Quality::Fast,
        ResamplerQuality::High => Quality::High,
        ResamplerQuality::VeryHigh => Quality::VeryHigh,
    };
    let share = match policy.share {
        ShareMode::Exclusive => ShareKind::Exclusive,
        ShareMode::Shared => ShareKind::Shared,
    };
    (mode, quality, share)
}

/// Where a position ends up after one entry is moved from `from` to `to`.
///
/// A move is a remove followed by an insert, and both shift positions:
///
/// * the moved entry itself lands on `to`;
/// * an entry strictly between `from` and `to` shifts one step towards `from`;
/// * everything else keeps its position.
fn reindex(position: usize, from: usize, to: usize) -> usize {
    if position == from {
        return to;
    }
    if from < to && position > from && position <= to {
        return position - 1;
    }
    if to < from && position >= to && position < from {
        return position + 1;
    }
    position
}

#[cfg(test)]
mod tests {
    use super::reindex;

    /// A move is a remove followed by an insert, and both shift positions. The
    /// four cases are covered because they are the four ways a position can
    /// relate to a move, and three of them are easy to get wrong in the same
    /// direction.
    #[test]
    fn moving_an_entry_reindexes_every_position() {
        // Moving 0 to 3: [a b c d] -> [b c d a]
        //   0 -> 3 (the moved one), 1..=3 shift down, 4 stays.
        assert_eq!(reindex(0, 0, 3), 3);
        assert_eq!(reindex(1, 0, 3), 0);
        assert_eq!(reindex(2, 0, 3), 1);
        assert_eq!(reindex(3, 0, 3), 2);
        assert_eq!(reindex(4, 0, 3), 4);

        // Moving 3 to 0: [a b c d] -> [d a b c]
        //   3 -> 0 (the moved one), 0..3 shift up, 4 stays.
        assert_eq!(reindex(3, 3, 0), 0);
        assert_eq!(reindex(0, 3, 0), 1);
        assert_eq!(reindex(1, 3, 0), 2);
        assert_eq!(reindex(2, 3, 0), 3);
        assert_eq!(reindex(4, 3, 0), 4);
    }

    /// A position outside the span is untouched, which is the case a naive
    /// `if from < to { position - 1 }` gets wrong for everything after `to`.
    #[test]
    fn positions_outside_the_move_do_not_change() {
        assert_eq!(reindex(0, 2, 4), 0);
        assert_eq!(reindex(1, 2, 4), 1);
        assert_eq!(reindex(5, 2, 4), 5);
        assert_eq!(reindex(0, 4, 2), 0);
        assert_eq!(reindex(5, 4, 2), 5);
    }
}
