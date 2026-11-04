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
    pub library: Option<Library>,
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
        library,
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
        devices: Vec::new(),
        library,
        most_played_limit: 20,
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
    devices: Vec<DeviceInfo>,
    /// The favourites, playlists and play counts. `None` when Base was started
    /// without a library location -- the rest of Base works, and every library
    /// command reports that instead of pretending the answer is empty.
    library: Option<Library>,
    /// The limit the last most-played request used, so a play can refresh the
    /// list the UI is actually watching.
    most_played_limit: usize,
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
            Command::Analyze { path } => {
                let _ = self.analysis.send(analysis::Request {
                    path,
                    explicit: true,
                });
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

    fn record_play(&mut self, path: PathBuf, at_ms: i64) {
        match self.library("play") {
            Some(library) => {
                if let Err(e) = library.record_play(&path, at_ms) {
                    return self.emit_error("play", &e.to_string());
                }
            }
            None => return,
        }
        // The count changed, so a list that is showing it is now stale. The
        // limit is the one the last request used, so a UI watching the list sees
        // it move rather than silently disagreeing with the store.
        let limit = self.most_played_limit;
        self.publish_most_played(limit);
    }

    fn publish_most_played(&mut self, limit: usize) {
        self.most_played_limit = limit;
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
                }
                if let Err(e) = session.play() {
                    self.emit_error("play", &e.to_string());
                }
                let serial = session.state().ok().map(|s| s.track_serial);
                self.last_serial = serial;
                self.session = Some(session);
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
                self.refresh_settings();
                self.refresh_resolved();
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
