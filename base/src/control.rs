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
    /// The remote worker's inbox. One thread, because a download saturates the
    /// link and a listing is one request.
    pub remote: Sender<crate::remote::Job>,
    /// The folder worker's inbox: scans and the live watcher.
    pub folders: Sender<crate::scan::Job>,
    /// Finished walks, coming back so the index is compared on this thread.
    pub scans: Receiver<crate::scan::ScanResult>,
    /// Paths this process is writing, shared with the watcher.
    pub suppressed: std::sync::Arc<crate::scan::Suppressed>,
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
        remote,
        folders,
        scans,
        suppressed,
    } = start;
    let (tag_in, tag_jobs) = std::sync::mpsc::channel::<PathBuf>();
    let (tag_tx, tag_out) =
        std::sync::mpsc::channel::<(PathBuf, Option<crate::library::StoredTags>)>();
    // One thread: this is a stat and a read of a header, and two of them in
    // parallel would make a spinning disk seek between them for no gain.
    let _ = std::thread::Builder::new()
        .name("neiro-base-index".into())
        .spawn(move || {
            for path in tag_jobs {
                let row = crate::tag::read_index_row(&path).ok();
                if tag_tx.send((path, row)).is_err() {
                    return;
                }
            }
        });

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
        remote,
        folders,
        scans,
        suppressed,
        most_played_watched: None,
        visual_interval: None,
        last_visual_emit: Instant::now(),
        settings: None,
        last_serial: None,
        last_phase: None,
        last_render: None,
        resolved: None,
        last_position_emit: Instant::now(),
        tag_in,
        tag_out,
        tag_total: 0,
        tag_done: 0,
    };
    control.loop_until_shutdown();
    control.finish_indexing();
    control.close_session();
    // The control thread may have asked for the default endpoint id, which
    // initialises COM on this thread. Release it after the session is closed.
    neiro_audio::shutdown();
}

struct Control {
    /// The index worker: paths in, rows back.
    ///
    /// A row is written to the library on this thread, because the connection
    /// belongs to it. The worker only opens the file, which is the slow part.
    tag_in: Sender<PathBuf>,
    tag_out: Receiver<(PathBuf, Option<crate::library::StoredTags>)>,
    tag_total: usize,
    tag_done: usize,
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
    /// Where the remote worker is told what to do. The worker owns the server
    /// config and the listing; this end only forwards.
    remote: Sender<crate::remote::Job>,
    /// Where the folder worker is told what to do.
    folders: Sender<crate::scan::Job>,
    /// Walks that finished in the worker, waiting to be compared.
    scans: Receiver<crate::scan::ScanResult>,
    /// Paths this process is writing right now. The watcher consults the same
    /// set, so a tag write does not look like an import.
    suppressed: std::sync::Arc<crate::scan::Suppressed>,
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

/// Said by every browse query when Base was started without a library.
///
/// An empty list is not the same answer: a host cannot tell a misconfigured
/// start from a library with nothing in it, and paints a permanent empty state.
/// The largest page a browse query will return.
///
/// The answer is one JSON event on the control thread's channel; ten thousand
/// rows is megabytes of it, and the host that asked for them blocks the same
/// thread while it parses. A host that wants everything pages through.
const MAX_PAGE: usize = 500;

const NO_LIBRARY_LOCATION: &str = "no library location was given; browse queries need one";

fn to_track_row(tags: &crate::library::StoredTags) -> crate::protocol::TrackRow {
    crate::protocol::TrackRow {
        path: tags.path.clone(),
        title: tags.title.clone(),
        artist: tags.artist.clone(),
        album: tags.album.clone(),
        album_artist: tags.album_artist.clone(),
        disc_no: tags.disc_no,
        track_no: tags.track_no,
        year: tags.year,
        genre: tags.genre.clone(),
        duration_ms: tags.duration_ms,
        sample_rate: tags.sample_rate,
        bits: tags.bits,
        channels: tags.channels,
    }
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
            // A walk finishes on the worker and comes back here, because the
            // index is compared on the thread that owns it.
            while let Ok(result) = self.scans.try_recv() {
                self.finish_scan(result);
            }
            self.drain_index();
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
            Command::ReadTags { path } => self.read_tags(path),
            Command::WriteTags {
                path,
                fields,
                policy,
            } => self.write_tags(path, fields, policy),
            Command::ListTagKeys { families } => self.publish_tag_keys(families),
            Command::ListTracks { offset, limit } => self.list_tracks(offset, limit),
            Command::ListAlbums => self.list_albums(),
            Command::ListArtists => self.list_artists(),
            Command::Search { query, limit } => self.search(query, limit),
            Command::IndexTags { paths } => self.reindex_paths(paths),
            Command::ReadArtwork { path, index } => self.read_artwork(path, index),
            Command::SetRemote { server, cache_dir } => self.set_remote(server, cache_dir),
            Command::ListRemote { start, limit } => self.list_remote(start, limit),
            Command::FetchRemote { id } => self.fetch_remote(id),
            Command::FetchRemoteArtwork { id } => {
                self.send_remote(crate::remote::Job::FetchArtwork { id })
            }
            Command::ListRemotePlaylists => self.send_remote(crate::remote::Job::ListPlaylists),
            Command::ListRemotePlaylistItems { id, start, limit } => {
                self.send_remote(crate::remote::Job::ListPlaylistItems { id, start, limit })
            }
            Command::ScanFolder { root } => self.scan_folder(root),
            Command::WatchFolder { root } => self.watch_folder(root),
            Command::UnwatchFolder { root } => self.unwatch_folder(root),
            Command::SetReconcileInterval { seconds } => self.set_reconcile_interval(seconds),
            Command::SetArtwork {
                path,
                family,
                index,
                data,
                mime,
                kind,
                description,
            } => self.set_artwork(path, family, index, data, mime, kind, description),
            Command::RemoveArtwork {
                path,
                family,
                index,
            } => self.remove_artwork(path, family, index),
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


    /// Point the remote worker at a server, or at none.
    ///
    /// The worker owns the remote session, so this only forwards. An empty
    /// token is how a host disconnects: there is no separate verb for it, and a
    /// second verb would be a second way to reach the same state.
    fn set_remote(&mut self, server: Option<crate::remote::Server>, cache_dir: PathBuf) {
        let config = server.filter(|s| !s.base_url().is_empty());
        if let Err(e) = self.remote.send(crate::remote::Job::Configure {
            config,
            cache_dir: Some(cache_dir),
        }) {
            self.emit_error("remote", &e.to_string());
        }
    }

    fn list_remote(&mut self, start: usize, limit: usize) {
        if let Err(e) = self.remote.send(crate::remote::Job::List { start, limit }) {
            self.emit_error("remote", &e.to_string());
        }
    }

    /// Forward a job to the remote worker, reporting a closed channel the same
    /// way every other remote command does.
    fn send_remote(&mut self, job: crate::remote::Job) {
        if let Err(e) = self.remote.send(job) {
            self.emit_error("remote", &e.to_string());
        }
    }

    fn fetch_remote(&mut self, id: String) {
        if let Err(e) = self.remote.send(crate::remote::Job::Fetch { id }) {
            self.emit_error("remote", &e.to_string());
        }
    }


    /// Compare a finished walk against the index and publish the difference.
    ///
    /// The index is what makes this cheap and what makes a deleted directory
    /// answerable: it remembers which files were under the root, so "gone" is a
    /// list rather than "look again". A scan that cannot open the library still
    /// reports what it found -- it just cannot say what changed.
    fn finish_scan(&mut self, result: crate::scan::ScanResult) {
        let crate::scan::ScanResult {
            root,
            present,
            unreadable,
            error,
        } = result;
        if let Some(error) = error {
            return self.emit_error("folders", &error);
        }

        let indexed = match self.library.as_mut() {
            Some(library) => match library.files_under(&root) {
                Ok(indexed) => indexed,
                Err(e) => return self.emit_error("folders", &e.to_string()),
            },
            None => {
                // No index: everything the walk found is new, and nothing can
                // be said about what left. That is what a host with no library
                // location gets, and it is honest rather than wrong.
                let added = present.iter().map(|f| f.path.clone()).collect();
                return self.events.emit(Event::ScanDone {
                    root,
                    added,
                    modified: Vec::new(),
                    removed: Vec::new(),
                    moved: Vec::new(),
                });
            }
        };

        let mut diff = crate::scan::reconcile(&indexed, &present);
        if unreadable > 0 {
            // Part of the tree could not be stat'ed, so a file missing from the
            // walk proves nothing about whether it is still there. Report the
            // additions and changes, and say why the rest is not being claimed.
            diff.removed.clear();
            diff.moved.clear();
            self.emit_error(
                "folders",
                &format!(
                    "{unreadable} file(s) under {} could not be read; removals are not being reported this round",
                    root.display()
                ),
            );
        }
        // An unchanged tree has nothing to write. Upserting every row anyway
        // cost 80 ms of the control thread on a 3,585-file library -- measured
        // -- for a diff the host will see as empty either way.
        let skipped = if diff.is_empty() {
            Vec::new()
        } else {
            match self.library.as_mut() {
                Some(library) => {
                    match library.apply_scan(&root, &present, &diff.removed, &diff.moved) {
                        Ok(skipped) => skipped,
                        Err(e) => return self.emit_error("folders", &e.to_string()),
                    }
                }
                None => Vec::new(),
            }
        };
        // The analysis key is path + size + mtime + analyzer version, and a
        // rename changes only the first. Carrying the entry over is what stops
        // a library reorganisation from costing one decode per file.
        {
            let mut cache = self.cache.lock().unwrap_or_else(|e| e.into_inner());
            for (from, to) in &diff.moved {
                cache.rename_path(from, to);
            }
        }

        if !skipped.is_empty() {
            // A move the library refused because the destination was already
            // known. The index is right; what the listener said about the old
            // path stayed there. Worth saying out loud.
            self.emit_error(
                "folders",
                &format!(
                    "{} move(s) could not be carried over because the destination was already known: {:?}",
                    skipped.len(),
                    skipped
                ),
            );
        }

        let added: Vec<PathBuf> = diff.added.into_iter().map(|f| f.path).collect();
        let modified: Vec<PathBuf> = diff.modified.into_iter().map(|f| f.path).collect();
        // Index what the walk just found. A host that opens an album view after
        // a scan must not have to ask for a second pass to see anything in it.
        self.index_paths(added.iter().cloned().collect::<Vec<_>>());
        // Modified files already have a row, and `index_paths` skips those. A
        // file whose tags changed on disk is exactly the case the row cannot
        // answer, so drop it first -- otherwise the list keeps showing the old
        // title until something else removes the row.
        for path in &modified {
            if let Some(library) = self.library.as_ref() {
                let _ = library.forget_tags(path);
            }
        }
        self.index_paths(modified.iter().cloned().collect::<Vec<_>>());
        self.events.emit(Event::ScanDone {
            root,
            added,
            modified,
            removed: diff.removed,
            moved: diff
                .moved
                .into_iter()
                .map(|(from, to)| crate::protocol::Move { from, to })
                .collect(),
        });
    }

    /// A scan is a walk of a tree, so it goes to the folder worker rather than
    /// running here: the control thread is what keeps the position and the
    /// queue in step, and a walk of a large drive would freeze both.
    fn scan_folder(&mut self, root: PathBuf) {
        if let Err(e) = self.folders.send(crate::scan::Job::Scan { root }) {
            self.emit_error("folders", &e.to_string());
        }
    }

    fn watch_folder(&mut self, root: PathBuf) {
        if let Err(e) = self.folders.send(crate::scan::Job::Watch { root }) {
            self.emit_error("folders", &e.to_string());
        }
    }

    /// The worker owns the timer, because it owns the roots. A second copy of
    /// the root list here would be a second answer to "which folders".
    fn set_reconcile_interval(&mut self, seconds: u64) {
        if let Err(e) = self.folders.send(crate::scan::Job::SetReconcile { seconds }) {
            self.emit_error("folders", &e.to_string());
        }
    }

    fn unwatch_folder(&mut self, root: PathBuf) {
        if let Err(e) = self.folders.send(crate::scan::Job::Unwatch { root }) {
            self.emit_error("folders", &e.to_string());
        }
    }

    /// Read one file's tags and answer with everything a tag editor draws on.
    ///
    /// `families` and `writable` are two different questions. The first is what
    /// this file carries now; the second is what its container can be written
    /// with. A UI that offered the first as the write choice would offer
    /// ID3v1 to a FLAC, whose container has no room for it.
    ///
    /// This does not go through the library: a tag is a fact about a file, and
    /// reading one must work with no library file open.
    fn read_tags(&mut self, path: PathBuf) {
        let snapshot = match crate::tag::read_tags(&path) {
            Ok(snapshot) => snapshot,
            Err(e) => return self.emit_error("tags", &e.to_string()),
        };
        let writable = match crate::tag::writable_families(&path) {
            Ok(list) => list,
            Err(e) => return self.emit_error("tags", &e.to_string()),
        };
        // Built before `families` is moved out: `values()` borrows the snapshot.
        let fields = snapshot.values();
        self.events.emit(Event::TagsRead {
            path,
            families: snapshot.families,
            writable,
            fields,
        });
    }

    /// Write fields into one file, then report what the file carries afterwards.
    ///
    /// The policy says what was asked for; the file says what happened. Only
    /// the second is published, and it is read back rather than assumed, so a
    /// subscriber that hears about the write can re-read and agree with it.
    fn write_tags(
        &mut self,
        path: PathBuf,
        values: Vec<crate::tag::TagFieldValue>,
        policy: crate::tag::TagPolicy,
    ) {
        let fields = match crate::tag::fields_from_values(&values) {
            Ok(fields) => fields,
            Err(e) => return self.emit_error("tags", &e.to_string()),
        };
        // The write replaces the file, and the watcher would report that as a
        // new file. Mark the path for the duration and just past it.
        self.suppressed.mark(&path);
        let written = crate::tag::write_tags(&path, &fields, &policy);
        match &written {
            // The window starts from the end of the write, so the backend's
            // late rename is still covered.
            Ok(()) => self.suppressed.mark(&path),
            // Refused before anything was written: a mark here would swallow a
            // real event for the length of the grace period.
            Err(_) => self.suppressed.clear(&path),
        }
        if let Err(e) = written {
            return self.emit_error("tags", &e.to_string());
        }
        // The index is a cache of the file's tags, so a write makes its row
        // stale. Dropping the row and asking for it again is how the next list
        // shows the new title instead of the old one.
        if let Some(library) = self.library.as_ref() {
            let _ = library.forget_tags(&path);
        }
        self.index_paths(vec![path.clone()]);
        match crate::tag::read_tags(&path) {
            Ok(snapshot) => self.events.emit(Event::TagsChanged {
                path,
                families: snapshot.families,
            }),
            // The write landed; only the re-read failed. Reporting the write as
            // failed would be a lie, and inventing a family list would be one
            // too, so the caller gets the error and can ask again.
            Err(e) => self.emit_error("tags", &e.to_string()),
        }
    }

    /// Answer with the pictures a file embeds.
    ///
    /// The listing is always complete so a tag editor can show every picture
    /// and its size; the bytes come only for the one the caller named, because
    /// a cover is a hundred kilobytes and most calls are asking whether there
    /// is one at all.
    ///
    /// An index past the end is an error, not an empty answer: the caller asked
    /// for a picture that is not there, and silence would look like a file with
    /// a picture that failed to decode.
    fn read_artwork(&mut self, path: PathBuf, index: Option<usize>) {
        let artwork = match crate::tag::read_artwork(&path) {
            Ok(list) => list,
            Err(e) => return self.emit_error("artwork", &e.to_string()),
        };
        let pictures: Vec<crate::tag::ArtworkInfo> = artwork.iter().map(|a| a.info()).collect();
        let data = match index {
            None => None,
            Some(i) => match artwork.get(i) {
                Some(picture) => Some(picture.to_base64()),
                None => {
                    return self.emit_error(
                        "artwork",
                        &format!(
                            "{} has {} picture(s); there is no index {}",
                            path.display(),
                            pictures.len(),
                            i
                        ),
                    )
                }
            },
        };
        self.events.emit(Event::ArtworkRead {
            path,
            pictures,
            index,
            data,
        });
    }


    /// Put a picture into a family, then report what the file carries.
    ///
    /// The bytes arrive base64, the same form `artwork_read` answers with: one
    /// encoding for one bridge. A decode failure is an error rather than an
    /// empty picture, because a zero-byte image is not what the caller sent.
    #[allow(clippy::too_many_arguments)]
    fn set_artwork(
        &mut self,
        path: PathBuf,
        family: crate::tag::TagFamily,
        index: Option<usize>,
        data: String,
        mime: Option<String>,
        kind: Option<String>,
        description: Option<String>,
    ) {
        use base64::Engine as _;
        let bytes = match base64::engine::general_purpose::STANDARD.decode(data) {
            Ok(bytes) => bytes,
            Err(e) => return self.emit_error("artwork", &format!("the picture is not base64: {e}")),
        };
        if bytes.is_empty() {
            return self.emit_error("artwork", "the picture is empty");
        }
        let input = crate::tag::ArtworkInput {
            data: bytes,
            mime,
            kind,
            description,
        };
        self.suppressed.mark(&path);
        let written = crate::tag::set_artwork(&path, family, index, &input);
        match &written {
            Ok(()) => self.suppressed.mark(&path),
            Err(_) => self.suppressed.clear(&path),
        }
        if let Err(e) = written {
            return self.emit_error("artwork", &e.to_string());
        }
        self.publish_artwork(path);
    }

    /// Remove one picture and report what is left.
    fn remove_artwork(&mut self, path: PathBuf, family: crate::tag::TagFamily, index: usize) {
        self.suppressed.mark(&path);
        let written = crate::tag::remove_artwork(&path, family, index);
        match &written {
            Ok(()) => self.suppressed.mark(&path),
            Err(_) => self.suppressed.clear(&path),
        }
        if let Err(e) = written {
            return self.emit_error("artwork", &e.to_string());
        }
        self.publish_artwork(path);
    }

    /// Re-read the pictures and publish them.
    ///
    /// Read back rather than predicted: the edit says what was asked for, the
    /// file says what happened, and only the second is worth reporting.
    fn publish_artwork(&mut self, path: PathBuf) {
        let pictures = match crate::tag::read_artwork(&path) {
            Ok(list) => list.iter().map(|a| a.info()).collect(),
            Err(e) => return self.emit_error("artwork", &e.to_string()),
        };
        self.events.emit(Event::ArtworkChanged { path, pictures });
    }

    /// Answer with the field names each family accepts.
    ///
    /// The answer depends on the family and on no file, so a host asks once at
    /// startup instead of on every selection.
    fn publish_tag_keys(&mut self, families: Vec<crate::tag::TagFamily>) {
        let asked: Vec<crate::tag::TagFamily> = if families.is_empty() {
            crate::tag::all_families().to_vec()
        } else {
            families
        };
        let lists = asked
            .into_iter()
            .map(|family| crate::protocol::TagKeyList {
                family,
                keys: crate::tag::writable_keys(family)
                    .into_iter()
                    .map(|(name, _)| name.to_owned())
                    .collect(),
            })
            .collect();
        self.events.emit(Event::TagKeysChanged { lists });
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
        // The zero goes out before any work is queued, and that order is the
        // point.
        //
        // It used to be sent after the requests, on the reasoning that the bus
        // would put them in order. It does not: the workers publish from their
        // own threads, so an item that finished immediately could emit `done: 1`
        // before the control thread emitted its own `done: 0`. A progress bar
        // painting that stream goes 1, then 0 -- it moves backwards, and a host
        // that treats progress as monotonic reports a defect in the engine.
        self.events.emit(Event::PreanalysisProgress {
            done: 0,
            total,
            complete: false,
        });
        for path in &paths {
            let _ = self.analysis.send(crate::analysis::Request {
                path: path.clone(),
                explicit: true,
                cancel_id: None,
            });
        }
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

    // ---- the browsing index ---------------------------------------------

    /// Take back whatever the index worker finished.
    fn drain_index(&mut self) {
        let mut rows: Vec<crate::library::StoredTags> = Vec::new();
        let mut touched = 0usize;
        loop {
            let Ok((_path, row)) = self.tag_out.try_recv() else {
                break;
            };
            self.tag_done += 1;
            touched += 1;
            if let Some(row) = row {
                rows.push(row);
            }
            if self.tag_total > 0 && self.tag_done >= self.tag_total {
                break;
            }
        }
        if touched == 0 {
            return;
        }
        // One transaction per burst: a first scan of ten thousand files would
        // otherwise be ten thousand implicit transactions on the control thread.
        let failure = match self.library.as_ref() {
            Some(library) => library.put_tags_many(&rows).err(),
            None => None,
        };
        if let Some(error) = failure {
            self.emit_error("index", &error.to_string());
        }
        // One event per burst, not per row: a scan used to emit ten thousand
        // progress events through a channel the host is reading, which is more
        // work than the indexing it was reporting on.
        let complete = self.tag_total > 0 && self.tag_done >= self.tag_total;
        self.events.emit(Event::TagsIndexed {
            done: self.tag_done,
            total: self.tag_total,
            complete,
        });
        if complete {
            self.tag_total = 0;
            self.tag_done = 0;
        }
    }

    /// Ask the worker to index these paths.
    ///
    /// A path already in the index is skipped: re-reading every file after every
    /// scan would make a no-op scan cost the same as the first one.
    /// Forget the rows for these paths and read them again.
    ///
    /// `IndexTags` means "these files changed on disk, look again". The plain
    /// `index_paths` skips anything already indexed, which made the command a
    /// no-op that also emitted no event -- a host waiting for one waited forever.
    fn reindex_paths(&mut self, paths: Vec<PathBuf>) {
        for path in &paths {
            if let Some(library) = self.library.as_ref() {
                let _ = library.forget_tags(path);
            }
        }
        self.index_paths(paths);
    }

    /// Give the worker a bounded chance to finish before shutdown.
    ///
    /// A scan of a few thousand files outlives a host that quits immediately;
    /// rows still in flight used to be dropped, so the next launch showed a
    /// library with holes in it until something touched the files again.
    fn finish_indexing(&mut self) {
        let deadline = Instant::now() + std::time::Duration::from_secs(2);
        loop {
            self.drain_index();
            if self.tag_total == 0 || Instant::now() >= deadline {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
    }

    fn index_paths(&mut self, paths: impl IntoIterator<Item = PathBuf>) {
        for path in paths {
            let known = self
                .library
                .as_ref()
                .map(|library| library.has_tags(&path).unwrap_or(false))
                .unwrap_or(false);
            if known {
                continue;
            }
            self.tag_total += 1;
            if self.tag_in.send(path).is_err() {
                self.tag_total -= 1;
                self.emit_error("index", "the index worker is gone");
                return;
            }
        }
    }

    fn list_tracks(&mut self, offset: usize, limit: usize) {
        let limit = limit.clamp(1, MAX_PAGE);
        let result = match self.library.as_ref() {
            Some(library) => library
                .list_tracks(offset as i64, limit as i64)
                .map(|rows| (rows, library.track_count().unwrap_or(0))),
            None => return self.emit_error("library", NO_LIBRARY_LOCATION),
        };
        match result {
            Ok((rows, total)) => self.events.emit(Event::TracksListed {
                tracks: rows.iter().map(to_track_row).collect(),
                total,
                offset,
            }),
            Err(e) => self.emit_error("index", &e.to_string()),
        }
    }

    fn list_albums(&mut self) {
        let result = match self.library.as_ref() {
            Some(library) => library.list_albums(),
            None => return self.emit_error("library", NO_LIBRARY_LOCATION),
        };
        match result {
            Ok(rows) => self.events.emit(Event::AlbumsListed {
                albums: rows
                    .into_iter()
                    .map(|row| crate::protocol::AlbumSummary {
                        album: row.album,
                        album_artist: row.album_artist,
                        year: row.year,
                        track_count: row.track_count,
                    })
                    .collect(),
            }),
            Err(e) => self.emit_error("index", &e.to_string()),
        }
    }

    fn list_artists(&mut self) {
        let result = match self.library.as_ref() {
            Some(library) => library.list_artists(),
            None => return self.emit_error("library", NO_LIBRARY_LOCATION),
        };
        match result {
            Ok(rows) => self.events.emit(Event::ArtistsListed {
                artists: rows
                    .into_iter()
                    .map(|row| crate::protocol::ArtistSummary {
                        artist: row.artist,
                        track_count: row.track_count,
                        album_count: row.album_count,
                    })
                    .collect(),
            }),
            Err(e) => self.emit_error("index", &e.to_string()),
        }
    }

    fn search(&mut self, query: String, limit: usize) {
        let limit = limit.clamp(1, MAX_PAGE);
        let result = match self.library.as_ref() {
            Some(library) => library.search(&query, limit as i64),
            None => return self.emit_error("library", NO_LIBRARY_LOCATION),
        };
        match result {
            Ok(rows) => self.events.emit(Event::SearchResults {
                query,
                tracks: rows.iter().map(to_track_row).collect(),
            }),
            Err(e) => self.emit_error("index", &e.to_string()),
        }
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
        // Look before closing.
        //
        // Closing first and then discovering there is nothing at `index` left
        // Base with no session and nothing published: `poll` returns early when
        // there is no session, so the snapshot froze at the last position and
        // every later command that needed a session did nothing at all. A probe
        // saw a queue edit take the index past the end and then waited five
        // seconds for a command that could never run.
        let Some(path) = self.queue.get(index).cloned() else {
            self.close_session();
            self.index = None;
            self.publish_phase(PlaybackState::Stopped, None);
            return;
        };
        self.close_session();
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
