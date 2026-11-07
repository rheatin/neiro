//! Play real files through the engine and report what it did.
//!
//! Every other probe in this crate uses generated signals, because a fixture
//! that has to be checked in is a fixture that can go missing. That leaves one
//! question unanswered: does the engine play the formats a listener actually
//! owns? A click track is a WAV; it says nothing about DSD, APE or TAK.
//!
//! So this takes files from the command line, plays each one, and prints what
//! the engine reported rather than what was asked for: the render path, the
//! granted rate, and whether it called the result bit-perfect. A format that
//! falls back to the generic decoder shows up in that report as a reason code,
//! which is the whole point of asking.
//!
//! It needs a real output endpoint, the same as `base_probe`.
//!
//! Usage: `play_real_probe <file> [<file> ...]`

use std::path::PathBuf;
use std::sync::mpsc::Receiver;
use std::time::{Duration, Instant};

use neiro_base::protocol::{Event, PlaybackState, RenderPath};
use neiro_base::Base;

fn wait_for_playing(
    rx: &Receiver<Event>,
    base: &neiro_base::BaseHandle,
    path: &std::path::Path,
    budget: Duration,
) -> Option<RenderPath> {
    let deadline = Instant::now() + budget;
    loop {
        let snapshot = base.snapshot();
        if let Some(render) = snapshot.render_path {
            let playing = matches!(snapshot.state, PlaybackState::Playing);
            if playing && snapshot.path.as_deref() == Some(path) && render.source_rate > 0 {
                return Some(render);
            }
        }
        if matches!(
            snapshot.state,
            PlaybackState::Error | PlaybackState::DeviceLost
        ) {
            return None;
        }
        let left = deadline.saturating_duration_since(Instant::now());
        if left.is_zero() {
            return None;
        }
        let _ = rx.recv_timeout(left.min(Duration::from_millis(50)));
    }
}

fn main() {
    let files: Vec<PathBuf> = std::env::args().skip(1).map(PathBuf::from).collect();
    if files.is_empty() {
        eprintln!("usage: play_real_probe <file> [<file> ...]");
        std::process::exit(2);
    }

    // `NEIRO_PROBE_NATIVE=1` asks the endpoint for the source rate, which is
    // the only policy under which bit-perfect is even possible. The default
    // follows the device, so everything resamples and the flag is always false
    // -- correct, and useless for checking the claim.
    let native = std::env::var("NEIRO_PROBE_NATIVE").as_deref() == Ok("1");
    let policy = neiro_base::protocol::PlaybackPolicy {
        rate: if native {
            neiro_base::protocol::OutputRatePolicy::Native
        } else {
            neiro_base::protocol::OutputRatePolicy::Device
        },
        ..Default::default()
    };
    println!(
        "policy: rate {}",
        if native { "native" } else { "device" }
    );
    let base = Base::start_with_policy(policy).expect("start Base");
    let handle = base.handle();
    let (_ticket, events) = handle.subscribe();

    let mut failures = 0;
    for path in &files {
        println!("\n=== {} ===", path.display());
        match handle.play(path, None) {
            Ok(()) => {}
            Err(e) => {
                println!("  could not ask the engine to play it: {e}");
                failures += 1;
                continue;
            }
        }
        match wait_for_playing(&events, &handle, path, Duration::from_secs(30)) {
            None => {
                let snapshot = handle.snapshot();
                println!(
                    "  DID NOT PLAY — state {:?}, path {:?}",
                    snapshot.state, snapshot.path
                );
                failures += 1;
            }
            Some(render) => {
                println!(
                    "  source {} Hz / {} ch -> device {} Hz / {} bit ({} valid{})",
                    render.source_rate,
                    render.file_channels,
                    render.sample_rate,
                    render.bits_per_sample,
                    render.valid_bits,
                    if render.is_float { ", float" } else { "" }
                );
                println!(
                    "  resampling {} · dithered {} · bit-perfect {}",
                    render.resampling, render.dithered, render.bit_perfect
                );
                if !render.bit_perfect {
                    println!("  reason code {}", render.reason);
                }
                // A short listen, then stop: the point is that it opened and
                // advanced, not that it played to the end.
                let start = handle.snapshot().position;
                std::thread::sleep(Duration::from_millis(400));
                let moved = handle.snapshot().position - start;
                if moved <= 0.0 {
                    println!("  position did not advance ({start:.3}s)");
                    failures += 1;
                }
            }
        }
        handle.stop().ok();
        // Let the device close before the next open; exclusive mode will not
        // hand the same endpoint to two streams at once.
        std::thread::sleep(Duration::from_millis(250));
    }

    handle.shutdown().ok();
    drop(base);

    if failures > 0 {
        println!("\n{failures} file(s) did not play");
        std::process::exit(1);
    }
    println!("\nPLAY REAL PROBE PASSED ({} file(s))", files.len());
}
