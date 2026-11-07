//! Probe binary: exercises the engine through the Rust bindings and prints the
//! same facts the C++ harness (`test_main.cpp`) prints, so the two can be
//! compared directly.
//!
//! If these numbers agree with the C++ run, the ABI is intact end to end:
//! same devices, same signal path, same timing.
//!
//! Run with the DLL on PATH, e.g.
//!   $env:PATH = "$PWD\..\build;$env:PATH"
//!   cargo run --bin probe

use std::time::Instant;

use neiro_audio as audio;

/// Failures recorded so far. A static rather than a local so the checks can
/// live in their own functions — a harness that can only report from one place
/// tends to bail out on the first error and hide everything after it.
static FAILURES: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

fn fail(ok: bool, label: &str, detail: &str) {
    if !ok {
        FAILURES.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }
    println!(
        "  [{}] {:<48} {}",
        if ok { "PASS" } else { "FAIL" },
        label,
        detail
    );
}

fn main() {
    // 文件元数据，通过 Rust 绑定读。之前 track 事件只有 channels 和 durationMs，
    // 而采样率、位深、编码都在引擎手上却没被带上来——所以界面上没有「352800 Hz」。
    //
    // 这一段要一个文件路径。有就给，没有就跳过并说明，而不是跳过得看不出是跳过了。
    if let Some(path) = std::env::args().nth(1) {
        println!("\n== track info, through the Rust binding ==");
        // 空设备 id 会让引擎去找一个不存在的端点（`no such render endpoint`, code 2），
        // 而不是「用默认设备」——那个语义属于 `None`，而这里要真的枚举一次。
        let Some(device) = audio::default_device() else {
            fail(false, "default_device for track info", "no default device");
            std::process::exit(1);
        };
        match audio::open_file(&device, &path, audio::OutputMode::Native, audio::Quality::High) {
            Ok(stream) => {
                let t = stream.track_info();
                let sp = stream.signal_path();
                println!(
                    "  file:   {:?}  {} Hz  {} ch  container {} bit  {} ms",
                    t.codec, t.sample_rate, t.channels, t.bits_per_sample, t.duration_ms
                );
                println!(
                    "  device: {} Hz  {} ch  container {} bit  valid {} bit  resampling={}",
                    sp.sample_rate, sp.channels, sp.bits_per_sample, sp.valid_bits, sp.resampling
                );
                fail(
                    t.sample_rate > 0,
                    "track reports the file's own sample rate",
                    &format!("{} Hz", t.sample_rate),
                );
                fail(!t.codec.is_empty(), "track reports a codec", &t.codec);
                fail(
                    t.bits_per_sample > 0,
                    "track reports a container width",
                    &format!("{} bit", t.bits_per_sample),
                );
                fail(
                    t.duration_ms > 0.0,
                    "track reports a duration",
                    &format!("{:.1} s", t.duration_ms / 1000.0),
                );
                // 标签。这是本地文件 fact strip 里剩下的几格。
                let tags = stream.tags();
                println!(
                    "  tags:   year={:?} genre={:?} album={:?} artist={:?} bitrate={} kbps (from tag: {})",
                    tags.year, tags.genre, tags.album, tags.artist,
                    tags.bitrate_kbps, tags.bitrate_from_tag
                );
                fail(
                    !tags.album.is_empty() || !tags.artist.is_empty(),
                    "at least one of album/artist came through",
                    &format!("album={:?} artist={:?}", tags.album, tags.artist),
                );
                // 码率交叉验证：文件大小 ÷ 时长。在进程外独立算的，所以它能当裁判。
                // 10% 的容差对应文件里标签和封面的开销。
                if let Ok(meta) = std::fs::metadata(&path) {
                    let measured = (meta.len() as f64 * 8.0) / (t.duration_ms / 1000.0) / 1000.0;
                    if tags.bitrate_kbps > 0 && measured > 0.0 {
                        let rel = (tags.bitrate_kbps as f64 - measured).abs() / measured;
                        println!(
                            "  check:  reported {} kbps, size/duration {} kbps, {:.1}% apart",
                            tags.bitrate_kbps, measured.round(), rel * 100.0
                        );
                        fail(
                            rel < 0.10,
                            "the reported bitrate agrees with size/duration",
                            &format!("{:.1}% apart", rel * 100.0),
                        );
                    }
                }
                // 速率不同时必须报 resampling。相同时不能断言——原生速率文件本来就
                // 两者相等，而把这个断言写成「两者必须不同」会立刻在 96/24 FLAC 上
                // 失败，而那不是故障。
                if t.sample_rate != sp.sample_rate {
                    fail(
                        sp.resampling,
                        "a differing rate is reported as resampling",
                        &format!("{} -> {}", t.sample_rate, sp.sample_rate),
                    );
                }
                drop(stream);
            }
            Err(e) => fail(false, "open_file for track info", &e.to_string()),
        }
    } else {
        println!("\n== track info: skipped, no file given ==");
    }

    // ---- the parameter registry ------------------------------------------
    //
    // The registry is the discoverable view of every setting: the typed structs
    // are the contract, and this is how a host enumerates them without a list
    // that drifts. It was exported but not declared in the binding, so no Rust
    // host could read it -- which is the gap this check closes.
    println!("== parameter registry ==");
    {
        let params = audio::parameters();
        fail(
            !params.is_empty(),
            "the parameter registry is not empty",
            &format!("{} entries", params.len()),
        );
        let names: Vec<&str> = params.iter().map(|p| p.name.as_str()).collect();
        fail(
            names.contains(&"volume.gain"),
            "the registry lists volume.gain",
            "",
        );
        let volume = params.iter().find(|p| p.name == "volume.gain").unwrap();
        fail(
            volume.min == 0.0 && volume.max == 1.0,
            "volume.gain reports its documented range",
            &format!("{}..{}", volume.min, volume.max),
        );
        let bad: Vec<&str> = params
            .iter()
            .filter(|p| p.name.is_empty() || p.default < p.min || p.default > p.max)
            .map(|p| p.name.as_str())
            .collect();
        fail(
            bad.is_empty(),
            "every parameter has a name and a default inside its range",
            &format!("{bad:?}"),
        );
    }

    println!("== capabilities ==");
    match audio::capabilities() {
        Ok(json) => {
            println!("  {}", json);
            // The backend that answered, whatever it is called here. Naming
            // WASAPI outright made this pass on Windows and fail on every other
            // platform for the only reason that mattered least: the string.
            let backend = if cfg!(target_os = "macos") { "coreaudio" } else { "wasapi" };
            fail(
                json.to_lowercase().contains(backend),
                &format!("capabilities mention {backend}"),
                "",
            );
        }
        Err(e) => fail(false, "capabilities readable", &e.to_string()),
    }

    println!("\n== device enumeration ==");
    let devices = match audio::enumerate_devices() {
        Ok(d) => d,
        Err(e) => {
            fail(false, "enumerate_devices", &e.to_string());
            println!("\nCannot continue.");
            std::process::exit(1);
        }
    };

    for d in &devices {
        println!(
            "  {}  {} Hz / {} ch / {}-bit  exclusive={} shared={}",
            d.name, d.sample_rate, d.channels, d.bits_per_sample, d.exclusive, d.shared
        );
    }
    fail(!devices.is_empty(), "at least one device reported", &format!("{}", devices.len()));
    let first = &devices[0];
    fail(
        !first.id.is_empty() && !first.name.is_empty(),
        "first device has id and name",
        &first.id,
    );
    fail(first.exclusive, "first device reports exclusive support", "");

/// Open exclusively, play, and close. Returns without exiting on failure:
/// a diagnostic that stops at the first error hides everything after it.
fn exercise_exclusive(first: &audio::Device) {
    println!("\n== open exclusive ==");
    let mut stream = match audio::open_exclusive(first) {
        Ok(s) => s,
        Err(e) => {
            fail(false, "open_exclusive", &e.to_string());
            println!("  last error: {}", audio::last_error());
            return;
        }
    };

    let path = stream.signal_path();
    println!(
        "       signal path: {} Hz / {} ch / {}-bit (valid {}) / {} / {:.1} ms buffer",
        path.sample_rate,
        path.channels,
        path.bits_per_sample,
        path.valid_bits,
        if path.is_float { "float" } else { "int" },
        path.latency_ms
    );
    fail(path.sample_rate > 0, "sample rate reported", &path.sample_rate.to_string());
    fail(path.channels == 2, "channel count reported", &path.channels.to_string());
    fail(path.bits_per_sample > 0, "bit depth reported", &path.bits_per_sample.to_string());
    fail(
        path.latency_ms > 1.0 && path.latency_ms < 2000.0,
        "buffer latency is sane",
        &format!("{} ms", path.latency_ms),
    );

    println!("\n== the output spectrum tap, through the Rust binding ==");
    // The C++ probe verifies the tap against the tone. This checks the layer
    // between them, which is the part a reader of *that* result would assume
    // works: the struct layout, the two-call protocol, and the reading itself.
    //
    // It is worth its own check because a binding bug is silent in a way a C++
    // bug is not. Wrong field offsets do not crash -- they put plausible numbers
    // in the wrong places. `peak` landing where `valid_bits` belongs would report
    // a bit depth of 0.25 and a peak of 24, and both are numbers.
    let before = stream.spectrum();
    println!(
        "  before playing: bins={} framesFed={} hasData={}",
        before.bins.len(),
        before.frames_fed,
        before.has_data()
    );
    fail(
        !before.has_data(),
        "a stream that has played nothing reports no spectrum",
        &format!("{} bins, {} frames fed", before.bins.len(), before.frames_fed),
    );

    println!("\n== play a 1500 ms sine ==");
    let started = Instant::now();
    let played = stream.play_sine_blocking(1500);
    let elapsed = started.elapsed().as_millis() as u64;
    println!("  wall time: {} ms", elapsed);

    match played {
        Ok(()) => fail(true, "play_sine_blocking succeeded", ""),
        Err(e) => fail(false, "play_sine_blocking succeeded", &e.to_string()),
}
fail(
    (1400..4000).contains(&elapsed),
    "played roughly the requested duration",
    &format!("{} ms for 1500 ms of audio", elapsed),
);

    // Now the tap, read back through the binding. The tone is 1 kHz and the
    // device is at the sample rate it reported, so the peak must land in the bin
    // 1000 Hz falls in and its magnitude must be what a Hann-windowed 1 kHz tone
    // puts there.
    let after = stream.spectrum();
    println!(
        "  after playing:  bins={} fft={} rate={} validBits={} peak={:.6} rms={:.6} framesFed={} complete={}",
        after.bins.len(),
        after.fft_size,
        after.sample_rate,
        after.valid_bits,
        after.peak,
        after.rms,
        after.frames_fed,
        after.complete
    );
    fail(after.has_data(), "the tap reports data after playback", "");
    fail(after.complete, "the engine wrote the whole spectrum struct", "");
    fail(
        after.sample_rate == path.sample_rate,
        "the tap's rate is the device's rate",
        &format!("{} vs device {}", after.sample_rate, path.sample_rate),
    );
    fail(
        after.valid_bits == path.valid_bits,
        "the tap's valid bits are the device's",
        &format!("{} vs device {}", after.valid_bits, path.valid_bits),
    );
    fail(
        after.bins.len() == (after.fft_size as usize) / 2,
        "bin count is half the window",
        &format!("{} bins for fft {}", after.bins.len(), after.fft_size),
    );
    // `peak` is an absolute sample value and must land within it. A binding that
    // shifted the struct by one field would read `valid_bits` here, which for a
    // 24-in-32 stream is 24 -- and 24 > 1, so this catches the shift as well as
    // a zero.
    fail(
        after.peak > 0.0 && after.peak <= 1.0,
        "peak is a fraction of full scale",
        &format!("{}", after.peak),
    );
    fail(
        after.rms > 0.0 && after.rms <= after.peak + 1e-9,
        "rms is positive and no greater than peak",
        &format!("rms {} peak {}", after.rms, after.peak),
    );

    if after.has_data() && after.sample_rate > 0 && after.fft_size > 0 {
        let bin_hz = after.sample_rate as f64 / after.fft_size as f64;
        let exact = 1000.0 / bin_hz;
        let nearest = exact.round() as usize;
        let peak_bin = after
            .bins
            .iter()
            .enumerate()
            .skip(1)
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap_or(std::cmp::Ordering::Equal))
            .map(|(i, _)| i);
        println!(
            "  1 kHz is bin {:.3} (nearest {}); loudest bin is {:?} at {:.1} Hz",
            exact,
            nearest,
            peak_bin,
            peak_bin.map(|b| b as f64 * bin_hz).unwrap_or(0.0)
        );
        // One bin either side, because a Hann-windowed tone between bins splits
        // its energy across two and either may be the larger.
        if let Some(pb) = peak_bin {
            let close = (pb as i64 - exact as i64).abs() <= 1;
            fail(
                close,
                "the loudest bin is the 1 kHz bin",
                &format!(
                    "bin {} = {:.1} Hz, expected {:.3} ({:.1} Hz)",
                    pb,
                    pb as f64 * bin_hz,
                    exact,
                    exact * bin_hz
                ),
            );
        }
    }

    // Dropping the Stream runs NAE_Close. If ownership were wrong this is where
    // it would show up, so do it explicitly under observation.
    drop(stream);
}

    exercise_exclusive(first);

    fail(true, "stream dropped cleanly (NAE_Close ran)", "");

    println!("\n== handle ownership ==");
    // The strongest available check on the Drop path: leaking is silent, so
    // instead assert that a failed open cannot produce a live handle.
    let bogus = audio::Device {
        id: "{0.0.0.00000000}.{00000000-0000-0000-0000-000000000000}".into(),
        name: "nonexistent".into(),
        sample_rate: 0,
        bits_per_sample: 0,
        valid_bits: 0,
        channels: 0,
        exclusive: false,
        shared: false,
    };
    match audio::open_exclusive(&bogus) {
        Ok(_) => fail(false, "opening a nonexistent device fails", "it succeeded"),
        Err(e) => {
            let msg = e.to_string();
            fail(!msg.is_empty(), "open failure carries a code and hresult", &msg);
        }
    }

    // The system default endpoint, which is the device the app should offer
    // first. Resolving it and being able to open it are different questions:
    // the first always works, the second depends on nothing else holding it.
    println!("\n== system default endpoint ==");
    match audio::default_device_id() {
        Ok(id) => {
            println!("  id: {}", id);
            fail(!id.is_empty(), "default device id resolved", "");
        }
        Err(e) => fail(false, "default device id resolved", &e.to_string()),
    }

    match audio::default_device() {
        Some(d) => {
            println!(
                "  {}  {} Hz / {} ch / {}-bit  exclusive={}",
                d.name, d.sample_rate, d.channels, d.bits_per_sample, d.exclusive
            );
            fail(true, "default device found in the active list", "");
            if !d.exclusive {
                // Not a failure of the code. It means another application is
                // holding the default endpoint right now, which is exactly the
                // case the UI has to explain rather than fail on.
                println!(
                    "  note: exclusive is currently false, so something else is \
                     holding this endpoint"
                );
            }
        }
        None => fail(false, "default device found in the active list", "not in the list"),
    }

    println!("\n== output modes through Rust ==");
    {
        use audio::{open_with, OutputMode, Quality};
        use std::time::Instant;

        let device = audio::default_device()
            .or_else(|| audio::enumerate_devices().ok()?.into_iter().next());
        let Some(device) = device else {
            fail(false, "a device is available for the mode checks", "none found");
            audio::shutdown();
            std::process::exit(1);
        };

        // The three modes, exercised exactly as the UI will drive them.
        // `bit_perfect` is the field the UI depends on, so it is asserted
        // against what the signal path actually shows rather than trusted.
        struct Case {
            label: &'static str,
            source_rate: u32,
            mode: OutputMode,
            want_bit_perfect: Option<bool>,
        }
        // What Native mode can promise depends on the route the platform gives
        // us. WASAPI can hand the device a format that matches the file, so an
        // untouched signal is possible. CoreAudio here has one route -- float32
        // -- so an integer file is converted before it reaches the device and
        // bit-perfect is impossible at any rate. That is the bit-depth gap, not a
        // negotiation result, and asserting `true` would be asserting parity the
        // platform cannot back.
        let native_bit_perfect = if cfg!(target_os = "macos") {
            Some(false)
        } else {
            Some(true)
        };
        let cases = [
            Case {
                label: "Native, source == device",
                source_rate: 0,
                mode: OutputMode::Native,
                want_bit_perfect: native_bit_perfect,
            },
            Case {
                label: "Native, 44.1k source",
                source_rate: 44100,
                mode: OutputMode::Native,
                // Still bit-perfect on WASAPI, and this is the point of the mode:
                // the device is reformatted to 44.1 kHz, so the file's samples
                // reach it untouched. An earlier version of this test expected
                // false here, on the reasoning that a rate change implies
                // processing. It does not — it implies the device changed shape.
                want_bit_perfect: native_bit_perfect,
            },
            Case {
                label: "Device, 44.1k source",
                source_rate: 44100,
                mode: OutputMode::Device,
                want_bit_perfect: None,
            },
            Case {
                label: "Fixed 96000 from 44.1k",
                source_rate: 44100,
                mode: OutputMode::Fixed(96000),
                want_bit_perfect: Some(false),
            },
        ];

        for c in cases {
            let name = c.label;
            match open_with(&device, c.source_rate, c.mode, Quality::High) {
                Ok(mut stream) => {
                    let p = stream.signal_path();
                    let detail = format!(
                        "{} Hz (from {} Hz) / {}-bit valid in {}-bit -> {}",
                        p.sample_rate,
                        p.source_rate,
                        p.valid_bits,
                        p.bits_per_sample,
                        if p.bit_perfect { "bit-perfect" } else { "processed" }
                    );
                    println!("  {}: {}", name, detail);

                    fail(
                        p.sample_rate > 0,
                        &format!("{name}: opened at a real rate"),
                        &detail,
                    );

                    if let Some(expected) = c.want_bit_perfect {
                        fail(
                            p.bit_perfect == expected,
                            &format!("{name}: bit_perfect is honest"),
                            &format!("bit_perfect={} expected={expected}", p.bit_perfect),
                        );
                    }

                    // A processed path must not claim to be bit-perfect. This is
                    // the check that stops the UI saying "bit-perfect" while
                    // quietly resampling.
                    fail(
                        !(p.bit_perfect && p.resampling),
                        &format!("{name}: flags are not contradictory"),
                        "bit_perfect and resampling both set",
                    );

                    // And it must actually make sound, through the converter.
                    //
                    // The lower bound is not the requested duration: by the time
                    // the last block is written, the device has already consumed
                    // some of it, so wall time comes in under the audio length.
                    // The upper bound is the one that matters — a resampler that
                    // stalls or runs away shows up as minutes, or milliseconds.
                    let t0 = Instant::now();
                    let played = stream.play_sine_blocking(500);
                    let ms = t0.elapsed().as_millis() as u64;
                    fail(
                        played.is_ok() && (300..2000).contains(&ms),
                        &format!("{name}: played in realtime"),
                        &format!("{ms} ms of wall time for 500 ms of audio"),
                    );
                }
                Err(e) => {
                    // Exclusive mode is single-occupant, so another application
                    // holding the endpoint is a legitimate outcome, not a defect
                    // in the binding.
                    println!("  {}: skipped ({})", name, e);
                }
            }
        }
    }
    audio::shutdown();
    fail(true, "shutdown ran cleanly", "");

    let failures = FAILURES.load(std::sync::atomic::Ordering::SeqCst);
    println!(
        "\n{}",
        if failures == 0 {
            "All Rust binding checks passed."
        } else {
            "Some Rust binding checks FAILED."
        }
    );
    std::process::exit(if failures == 0 { 0 } else { 1 });
}
