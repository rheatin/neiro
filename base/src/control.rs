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
use crate::protocol::{
    Event, OutputRatePolicy, PlaybackPolicy, PlaybackState, RenderPath, ResamplerQuality,
    ShareMode, Snapshot,
};
use crate::EventBus;

const POLL: Duration = Duration::from_millis(30);
const POSITION_EMIT: Duration = Duration::from_millis(100);

pub(crate) fn run(
    commands: Receiver<crate::protocol::Command>,
    analysis: Sender<crate::analysis::Request>,
    events: EventBus,
    state: Arc<Mutex<Snapshot>>,
    version: u32,
    policy: PlaybackPolicy,
    registry: CancelRegistry,
) {
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
                self.close_session();
                self.queue = vec![path];
                self.index = Some(0);
                self.open_at(0, false);
            }
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

        self.open_at(index, true);
        let mut errors = Vec::new();
        if let Some(session) = self.session.as_mut() {
            if position > 0.0 {
                if let Err(e) = session.seek(position) {
                    errors.push(e.to_string());
                }
            }
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
        self.open_at(index + 1, true);
    }

    fn open_at(&mut self, index: usize, add_remaining: bool) {
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
        guard.position = state.position;
        guard.duration = state.duration;
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
