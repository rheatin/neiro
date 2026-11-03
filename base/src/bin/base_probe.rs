//! End-to-end probe for Base: queue, playback, pause/resume/seek, pre-analysis,
//! cache and the render-path snapshot.
//!
//! It generates its own click tracks, so it needs no fixture. It does need a
//! real output endpoint: the session opens one just like the player does.

use std::path::Path;
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

use neiro_audio::{
    CrossfadeParams, CrossfeedParams, EqBand, EqParams, ReplayGainParams,
    CROSSFADE_EQUAL_POWER, EQ_PEAKING, EQ_PHASE_MINIMUM, RG_ALBUM,
};
use neiro_base::protocol::{Event, OutputRatePolicy, PlaybackPolicy, PlaybackState};
use neiro_audio::ShareKind;
use neiro_base::Base;

const RATE: u32 = 44_100;
const SECONDS: u32 = 12;

fn put16(out: &mut Vec<u8>, v: u16) {
    out.extend_from_slice(&v.to_le_bytes());
}
fn put32(out: &mut Vec<u8>, v: u32) {
    out.extend_from_slice(&v.to_le_bytes());
}

fn write_click(path: &Path, gain: f64) {
    let frames = RATE * SECONDS;
    let mut wav = Vec::with_capacity(44 + (frames as usize) * 4);
    wav.extend_from_slice(b"RIFF");
    put32(&mut wav, 36 + frames * 4);
    wav.extend_from_slice(b"WAVEfmt ");
    put32(&mut wav, 16);
    put16(&mut wav, 1);
    put16(&mut wav, 2);
    put32(&mut wav, RATE);
    put32(&mut wav, RATE * 4);
    put16(&mut wav, 4);
    put16(&mut wav, 16);
    wav.extend_from_slice(b"data");
    put32(&mut wav, frames * 4);
    for i in 0..frames {
        let t = i as f64 / RATE as f64;
        let phase = t - (t / 0.5).floor() * 0.5;
        let s = if phase < 0.002 {
            gain * (2.0 * std::f64::consts::PI * 1000.0 * phase).sin()
        } else {
            0.0
        };
        let v = (s * 32767.0) as i16;
        wav.extend_from_slice(&v.to_le_bytes());
        wav.extend_from_slice(&v.to_le_bytes());
    }
    std::fs::write(path, wav).expect("write click fixture");
}

fn check(ok: bool, label: &str, detail: String) {
    println!(
        "  [{}] {:<42} {}",
        if ok { "PASS" } else { "FAIL" },
        label,
        detail
    );
    if !ok {
        std::process::exit(1);
    }
}

fn wait_until(
    rx: &Receiver<Event>,
    snapshot: impl Fn() -> neiro_base::protocol::Snapshot,
    timeout: Duration,
    mut done: impl FnMut(&neiro_base::protocol::Snapshot) -> bool,
    label: &str,
) -> neiro_base::protocol::Snapshot {
    let deadline = Instant::now() + timeout;
    loop {
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            panic!(
                "timeout waiting for {label}; last snapshot: {:?}",
                snapshot()
            );
        }
        match rx.recv_timeout(left.min(Duration::from_millis(50))) {
            Ok(Event::Error { op, message }) => {
                panic!("Base error during {label}: {op}: {message}")
            }
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                panic!("Base event channel closed while waiting for {label}")
            }
        }
        let s = snapshot();
        if done(&s) {
            return s;
        }
    }
}

fn main() {
    let dir = std::env::current_dir().expect("cwd");
    let a = dir.join("base_probe_a.wav");
    let b = dir.join("base_probe_b.wav");
    write_click(&a, 0.10);
    write_click(&b, 0.05);

    let share = match std::env::var("NEIRO_BASE_SHARE").as_deref() {
        Ok("shared") => ShareKind::Shared,
        _ => ShareKind::Exclusive,
    };
    let base = Base::start_with_share(share).expect("start Base");
    let handle = base.handle();
    let events = handle.subscribe();

    let device = std::env::var("NEIRO_BASE_DEVICE").ok();
    handle
        .play(&a, device.as_deref())
        .expect("send play");
    handle.enqueue(vec![b.clone()]).expect("enqueue B");

    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| s.state == PlaybackState::Playing && s.path.as_deref() == Some(a.as_path()),
        "A playing",
    );
    check(
        snap.duration > 11.0,
        "A duration",
        format!("{:.3}s", snap.duration),
    );
    let render = snap.render_path.expect("render path while playing");
    check(
        render.sample_rate > 0,
        "render path sample rate",
        format!("{} Hz", render.sample_rate),
    );
    let resolved = snap.resolved.as_ref().expect("engine resolved path");
    check(
        resolved["sampleRate"].as_u64().unwrap_or(0) > 0,
        "resolved path rate",
        resolved["sampleRate"].to_string(),
    );
    check(
        resolved["diagnostics"]
            .as_array()
            .is_some_and(|v| !v.is_empty()),
        "resolved diagnostics",
        resolved["diagnostics"].to_string(),
    );

    check(
        snap.policy == PlaybackPolicy::default(),
        "policy is reported",
        format!("{:?}", snap.policy),
    );

    // Output mode and quality are open-time settings. Changing the policy has
    // to reopen the stream, and the render path is the proof it did: a policy
    // change that only updated a field would leave sample_rate alone.
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| s.position > 2.0,
        "A plays past 2 s",
    );
    let before_policy = snap.position;
    let device_rate = render.sample_rate;

    handle
        .set_policy(PlaybackPolicy {
            rate: OutputRatePolicy::Fixed { rate_hz: 96_000 },
            ..snap.policy
        })
        .expect("send policy");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| {
            s.render_path
                .as_ref()
                .is_some_and(|r| r.sample_rate == 96_000)
        },
        "policy reopens at 96 kHz",
    );
    check(
        snap.render_path.as_ref().map(|r| r.sample_rate) == Some(96_000),
        "fixed rate reaches the engine",
        format!("{} Hz", snap.render_path.map(|r| r.sample_rate).unwrap_or(0)),
    );
    check(
        snap.state == PlaybackState::Playing,
        "policy change keeps playing",
        format!("{:?}", snap.state),
    );
    // A seek is asynchronous, so the position cannot be read once and compared:
    // right after the reopen it is whatever the new session had before the seek
    // landed.
    //
    // The budget is bounded on purpose. A wait that is merely "long enough" is
    // satisfied by plain playback when the seek is missing entirely -- a fresh
    // session at 0 s reaches the old position on its own. The window here is
    // shorter than the 2 s of playback that was already behind us, so a stream
    // that restarted at zero cannot reach the target in time.
    let deadline = Instant::now() + Duration::from_millis(1_200);
    let mut restored = handle.snapshot().position;
    while Instant::now() < deadline {
        restored = handle.snapshot().position;
        if (restored - before_policy).abs() < 0.30 {
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    check(
        (restored - before_policy).abs() < 0.30,
        "policy change keeps position",
        format!("{:.3}s -> {:.3}s", before_policy, restored),
    );

    // Back to the default, so the rest of the probe runs on the same path it
    // was written against.
    handle
        .set_policy(PlaybackPolicy::default())
        .expect("send policy");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| {
            s.render_path
                .as_ref()
                .is_some_and(|r| r.sample_rate == device_rate)
        },
        "policy returns to the device rate",
    );
    check(
        snap.policy.rate == OutputRatePolicy::Device,
        "policy reset is reported",
        format!("{:?}", snap.policy.rate),
    );

    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| {
            s.analysis
                .as_ref()
                .is_some_and(|a| a.bpm_valid && a.bpm > 100.0)
        },
        "A analysis",
    );
    let analysis = snap.analysis.expect("analysis");
    check(
        (analysis.bpm - 120.0).abs() < 3.0,
        "A BPM",
        format!("{:.2} ({:.3})", analysis.bpm, analysis.bpm_confidence),
    );

    // The enqueue requested B while A was playing. Once A's analysis is done,
    // asking again must be a cache hit, not a second decode.
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut b_cached = false;
    while Instant::now() < deadline {
        handle.analyze(&b).expect("request B analysis");
        match events.recv_timeout(Duration::from_millis(50)) {
            Ok(Event::AnalysisReady { path, cached, .. }) if path == b => {
                if cached {
                    b_cached = true;
                    break;
                }
            }
            Ok(Event::Error { op, message }) => {
                panic!("Base error during B analysis: {op}: {message}")
            }
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    check(b_cached, "B analysis cached", format!("{}", b.display()));

    handle.set_volume(0.5).expect("set volume");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| s.settings.as_ref().is_some_and(|v| (v.volume - 0.5).abs() < 1e-6)
            && s.render_path.as_ref().is_some_and(|p| (p.volume - 0.5).abs() < 1e-6),
        "volume 0.5",
    );
    check(true, "volume 0.5", format!("{:.3}", snap.render_path.unwrap().volume));

    handle
        .set_eq(EqParams {
            enabled: true,
            preamp_db: -3.0,
            bands: vec![EqBand {
                kind: EQ_PEAKING,
                freq_hz: 1000.0,
                gain_db: 6.0,
                q: 1.0,
                enabled: true,
                channel_mask: 0,
            }],
            phase: EQ_PHASE_MINIMUM,
        })
        .expect("set eq");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| {
            s.render_path.as_ref().is_some_and(|p| p.eq_enabled)
                && s.settings
                    .as_ref()
                    .is_some_and(|v| v.eq.bands.first().is_some_and(|b| (b.gain_db - 6.0).abs() < 1e-6))
        },
        "EQ enabled",
    );
    check(true, "EQ enabled", format!("{:?}", snap.settings.unwrap().eq));

    handle
        .set_crossfeed(CrossfeedParams {
            enabled: true,
            amount: 0.4,
            delay_ms: 0.3,
            cutoff_hz: 700.0,
        })
        .expect("set crossfeed");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| s.render_path.as_ref().is_some_and(|p| p.crossfeed_enabled),
        "crossfeed enabled",
    );
    check(true, "crossfeed enabled", format!("{:?}", snap.settings.unwrap().crossfeed));

    handle
        .set_crossfade(CrossfadeParams {
            enabled: true,
            duration_ms: 0.0,
            curve: CROSSFADE_EQUAL_POWER,
        })
        .expect("set crossfade");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| {
            s.render_path.as_ref().is_some_and(|p| p.crossfade_enabled)
                && s.settings.as_ref().is_some_and(|v| v.crossfade.duration_ms == 0.0)
        },
        "crossfade/gapless mode",
    );
    check(true, "crossfade mode", format!("{:?}", snap.settings.unwrap().crossfade));

    handle
        .set_replay_gain(ReplayGainParams {
            mode: RG_ALBUM,
            preamp_db: 0.0,
            prevent_clipping: true,
            fallback_db: 0.0,
        })
        .expect("set replaygain");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| s.render_path.as_ref().is_some_and(|p| p.replay_gain_enabled),
        "ReplayGain enabled",
    );
    check(true, "ReplayGain enabled", format!("{:?}", snap.settings.unwrap().replay_gain));

    handle.pause().expect("pause");
    wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| s.state == PlaybackState::Paused,
        "paused",
    );

    handle.resume().expect("resume");
    wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| s.state == PlaybackState::Playing,
        "resumed",
    );

    handle.seek(2.0).expect("seek");
    wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| s.position >= 1.8,
        "seek to 2s",
    );

    // A manual skip now crosses the seam the way gapless does. What that has to
    // look like from outside is the same format on both sides: a close and
    // reopen could renegotiate the endpoint, and this probe runs on a device
    // that reports 192 kHz, so a renegotiation would be visible in the rate.
    let before_next = handle.snapshot();
    handle.next().expect("next");
    // The path flips to B before the position necessarily reflects it, and the
    // new track starts advancing immediately. Waiting for the path alone and
    // then sampling the position afterwards compares two different moments --
    // which read 2.0 s against 3.6 s once, and looked like a skip that had not
    // happened. The condition therefore includes the position.
    let after_next = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(10),
        |s| {
            s.path.as_deref() == Some(b.as_path())
                && s.state == PlaybackState::Playing
                && s.position < before_next.position
        },
        "B playing from its own start",
    );
    check(
        after_next.render_path.as_ref().map(|r| r.sample_rate)
            == before_next.render_path.as_ref().map(|r| r.sample_rate),
        "the skip keeps the negotiated rate",
        format!(
            "{:?} -> {:?}",
            before_next.render_path.as_ref().map(|r| r.sample_rate),
            after_next.render_path.as_ref().map(|r| r.sample_rate)
        ),
    );
    // The rate and the position alone cannot tell a seam from a reopened
    // stream: a close and reopen also keeps the rate and also starts at zero.
    // The serial is the field that separates them -- it moves inside one session
    // and starts over in a new one.
    check(
        after_next.track_serial > before_next.track_serial,
        "the skip crosses a seam instead of reopening the session",
        format!(
            "serial {} -> {}",
            before_next.track_serial, after_next.track_serial
        ),
    );
    check(
        after_next.position < before_next.position,
        "the skip restarts the position",
        format!("{:.3}s -> {:.3}s", before_next.position, after_next.position),
    );

    handle.stop().expect("stop");
    wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(5),
        |s| s.state == PlaybackState::Stopped,
        "stopped",
    );

    // One decode per path, however many requests ask for it.
    //
    // `play` asks for the current track and the prefetch asks for the next one,
    // so the same path is requested twice in normal use. Before the workers
    // shared an in-flight set each request decoded again; with it, the duplicates
    // are dropped and exactly one `analysis_ready` is published for the path.
    let c = dir.join("base_probe_c.wav");
    write_click(&c, 0.07);

    // The engine has to poll the flag for a cancel to mean anything. A flag
    // raised before the call is the cheapest way to see that it does: without
    // the poll the call decodes the file and returns success, and the caller
    // would cache a result it was told to abandon.
    {
        let flag = std::sync::atomic::AtomicI32::new(1);
        let cancelled = neiro_audio::analyze_file_cancellable(&c, 1024, Some(&flag));
        check(
            cancelled.error == neiro_audio::ffi::NAE_ERR_CANCELLED,
            "a raised flag cancels the decode",
            format!("error {}", cancelled.error),
        );
        flag.store(0, std::sync::atomic::Ordering::Release);
        let decoded = neiro_audio::analyze_file_cancellable(&c, 1024, Some(&flag));
        check(
            decoded.error == 0,
            "a clear flag decodes normally",
            format!("error {}", decoded.error),
        );
    }

    for _ in 0..3 {
        handle.analyze(c.clone()).expect("analyze C");
    }
    let mut first = None;
    let deadline = Instant::now() + Duration::from_secs(20);
    while Instant::now() < deadline {
        match events.recv_timeout(Duration::from_millis(50)) {
            Ok(Event::AnalysisReady { path, cached, .. }) if path == c => {
                first = Some(cached);
                break;
            }
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    check(
        first == Some(false),
        "three requests decode C once",
        format!("first event cached={first:?}"),
    );
    // A second event for the same path would mean the duplicates were decoded
    // rather than deduplicated. The window is short because the answer is
    // already here: nothing is still decoding C.
    let mut duplicates = 0;
    let settle = Instant::now() + Duration::from_millis(400);
    while Instant::now() < settle {
        match events.recv_timeout(Duration::from_millis(20)) {
            Ok(Event::AnalysisReady { path, .. }) if path == c => duplicates += 1,
            Ok(_) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    check(
        duplicates == 0,
        "no duplicate analysis event for C",
        format!("{duplicates} extra"),
    );
    let _ = std::fs::remove_file(&c);

    // ---- device list and selection -----------------------------------------
    //
    // This runs last, on its own track, because a device change reopens the
    // stream and therefore advances the current one by the length of the reopen.
    // Sharing a fixture with the skip and policy checks made every one of them
    // race the end of a 12 s file.
    //
    // The engine probes each endpoint by opening it, so the list is a
    // measurement rather than a cache. Two things are checked: that Base
    // forwards it, and that choosing an endpoint is a different command from
    // `play` -- `play` replaces the queue by design, and a device menu that did
    // the same would discard what the listener queued.
    let d = dir.join("base_probe_d.wav");
    write_click(&d, 0.06);
    handle.play(&d, None).expect("play D");
    handle.enqueue(vec![b.clone()]).expect("enqueue B again");
    wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| s.state == PlaybackState::Playing && s.path.as_deref() == Some(d.as_path()),
        "D playing",
    );

    handle.list_devices().expect("list devices");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| !s.devices.is_empty(),
        "device list",
    );
    let devices = snap.devices;
    check(
        devices.iter().all(|d| !d.id.is_empty() && !d.name.is_empty()),
        "every device has an id and a name",
        format!("{} devices", devices.len()),
    );
    let queue_before = handle.snapshot().queue;
    let index_before = handle.snapshot().queue_index;
    // A device that is NOT the one already open: choosing the current one
    // returns early without reopening, so a check built on it could not fail.
    // The device that is actually open. `play` with no id leaves Base's field
    // unset until `open_at` fills it in, so the snapshot is the authority --
    // reading `None` here would pick devices[0] even when that is the endpoint
    // already playing, and `set_device` would then return early.
    let current = handle.snapshot().device_id.clone();
    let chosen = match devices.iter().find(|dev| Some(&dev.id) != current.as_ref()) {
        Some(dev) => dev.id.clone(),
        None => {
            println!("  [INFO] only one endpoint here; the reopen path is not exercised");
            devices[0].id.clone()
        }
    };
    let different = Some(&chosen) != current.as_ref();
    handle.set_device(Some(&chosen)).expect("set device");
    let snap = wait_until(
        &events,
        || handle.snapshot(),
        Duration::from_secs(20),
        |s| s.device_id.as_deref() == Some(chosen.as_str()),
        "device chosen",
    );
    check(
        snap.queue == queue_before && snap.queue_index == index_before,
        "choosing a device leaves the queue alone",
        format!("{} entries, index {:?}", snap.queue.len(), snap.queue_index),
    );
    if different {
        // What can be asserted without racing the reopen: the state still
        // reports playback and the track is unchanged. The engine's own
        // resolved report is NOT asserted here -- `open_at` reopens and the
        // resolved event lands on its own schedule, and a check that waits for
        // it either races or cannot fail. It is listed as unverified in
        // README.md rather than covered by a check that means nothing.
        check(
            snap.state == PlaybackState::Playing,
            "changing the endpoint keeps playing",
            format!("{:?}", snap.state),
        );
        check(
            snap.path.as_deref()
                == queue_before.get(index_before.unwrap_or(0)).map(|p| p.as_path()),
            "changing the endpoint keeps the track",
            format!("{:?}", snap.path),
        );
    } else {
        check(
            snap.state == PlaybackState::Playing,
            "choosing the current device does not stop playback",
            format!("{:?}", snap.state),
        );
    }
    let _ = std::fs::remove_file(&d);

    base.shutdown();
    let _ = std::fs::remove_file(&a);
    let _ = std::fs::remove_file(&b);
    println!("\nAll Base checks passed.");
}
