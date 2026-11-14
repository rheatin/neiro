//! The pause/resume/buffering probe: measure what a listener hears as
//! "刺啦" with the engine's own counters instead of an ear, over time and
//! across output policies.
//!
//! Plays one file through Base — the same open the interface does — and
//! prints, every second (state 变化时也打印), the diagnostics fields the
//! engine carries in `resolved`: `padFrameCount`（垫出去的帧，非零即解码没
//! 跟上实时）, `maxBufferGapMs`（两次写设备之间的最大墙钟间隔）, underrun/
//! drop counts, `devicePeak`（设备缓冲里的实读峰值，playing 时应为 ~0.9）.
//! Then runs the same stretch again under each output policy
//! (`native`/`device`/`fixed 96000`), so 换一种开流方式后计数的变化可见.
//!
//! ```text
//! cargo run --bin buffer_probe -- <file> [secs_per_phase]
//! ```

use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use neiro_ui::engine::{Engine, RenderPath, WireEvent};

fn main() {
    let path = std::env::args()
        .nth(1)
        .expect("用法: buffer_probe <audio file> [每段秒数]");
    let section = std::env::args()
        .nth(2)
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(10);

    let (engine, receiver) = neiro_ui::engine::Engine::open(&library_dir().join("library.db"))
        .expect("E_ENGINE: Base 无法启动");
    engine
        .command(r#"{"command":"list_devices"}"#)
        .expect("list_devices");

    // 只留最新的驾驶舱：resolved 的 JSON、render_path、报错。
    let latest: Arc<Mutex<Latest>> = Arc::new(Mutex::new(Latest::default()));
    let reader = {
        let latest = Arc::clone(&latest);
        std::thread::spawn(move || {
            while let Ok(line) = receiver.recv() {
                match neiro_ui::engine::parse(&line) {
                    Ok(WireEvent::Error { op, message }) => {
                        eprintln!("[base 报错] {op}: {message}");
                    }
                    Ok(WireEvent::DevicesChanged { devices }) => {
                        latest.lock().expect("latest").devices =
                            Some(devices);
                    }
                    Ok(WireEvent::RenderPathChanged { render_path, .. }) => {
                        latest.lock().expect("latest").render_path = Some(render_path);
                    }
                    Ok(WireEvent::PolicyChanged { .. }) => {
                        // 策略变化以 render_path 与 snapshot 为准，这里只做 flag。
                        latest.lock().expect("latest").policy_seen = true;
                    }
                    _ => {}
                }
            }
        })
    };

    list_devices(&engine, &latest);

    // 三种 rate 策略跑同一文件段；暂停/恢复插入在默认策略那一轮。
    for mode in ["fixed", "device", "native"] {
        println!("\n######## 策略: {mode} ########");
        match mode {
            "fixed" => set_policy(&engine, fixed(96000)),
            "device" => set_policy(&engine, device()),
            _ => set_policy(&engine, native()),
        }
        send(
            &engine,
            &serde_json::json!({ "command": "play", "path": path }),
            "play",
        );
        run_stage(&engine, &latest, "起播与稳态", section);
        if mode != "native" {
            send(&engine, &serde_json::json!({ "command": "stop" }), "stop");
            continue;
        }
        // 默认策略轮才有暂停/恢复段：一次暂停 5s、恢复继续。
        send(&engine, &serde_json::json!({ "command": "pause" }), "pause");
        run_stage(&engine, &latest, "暂停中", 5);
        send(&engine, &serde_json::json!({ "command": "resume" }), "resume");
        run_stage(&engine, &latest, "恢复后", section);
        send(&engine, &serde_json::json!({ "command": "stop" }), "stop");
    }
    let _ = reader;
}

#[derive(Default)]
struct Latest {
    render_path: Option<RenderPath>,
    devices: Option<Vec<neiro_ui::engine::Device>>,
    policy_seen: bool,
}

/// 打 devices 一次（题头：端点能够的原生格式，说明 fixed 96k 是否成立）.
fn list_devices(_engine: &Engine, latest: &Arc<Mutex<Latest>>) {
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        if let Some(devices) = latest.lock().expect("latest").devices.clone() {
            println!("-- 设备：");
            for device in devices.iter().take(3) {
                println!(
                    "    {}: {} kHz | {} bit (valid {}) | {} ch | id={}",
                    device.name,
                    device.sample_rate as f64 / 1000.0,
                    device.bits_per_sample,
                    device.valid_bits,
                    device.channels,
                    device.id,
                );
            }
            return;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    println!("-- 设备：10s 内没有 devices_changed");
}

/// Runs one stage: prints the diagnostics a second, pausing whenever the
/// state changes, for `secs` seconds.
fn run_stage(engine: &Engine, latest: &Arc<Mutex<Latest>>, label: &str, secs: u64) {
    let stop = Instant::now() + Duration::from_secs(secs + 1);
    let mut last_state = String::from("?");
    let mut last_position = -1.0;
    let mut seen = None;
    println!("== {label} ==");
    while Instant::now() < stop {
        if let Ok(snapshot) = engine.snapshot() {
            if snapshot.state != last_state
                || (snapshot.position - last_position).abs() > 0.5
            {
                last_state = snapshot.state.clone();
                last_position = snapshot.position;
                let resolved = snapshot.resolved.clone().unwrap_or(serde_json::Value::Null);
                let (pad, gap, underrun, dropped, peak) = diagnostics(&resolved);
                println!(
                    "  t={:.1}  {} pos={:>6.1}s  pad={} gap={:.2}ms underrun={} drop={} devPeak={:.3}  rp:{refined}",
                    Instant::now().elapsed().as_secs_f64() % 1000.0,
                    snapshot.state,
                    snapshot.position,
                    pad,
                    gap,
                    underrun,
                    dropped,
                    peak,
                    refined = match &latest.lock().expect("latest").render_path {
                        Some(rp) => format!(
                            "bit_perfect={} resample={} dithered {}→{}",
                            rp.bit_perfect, rp.resampling, rp.source_rate, rp.sample_rate
                        ),
                        None => "—".to_string(),
                    }
                );
            }
            if seen.is_none() && !snapshot.resolved.is_none() {
                println!("  resolved 原文: {}", serde_json::to_string(&snapshot.resolved.clone().expect("checked")).expect("string"));
            }
            if snapshot.state == "playing" {
                seen = Some(snapshot.resolved.clone());
            }
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    if seen.is_none() {
        println!("  （这一段没有进入 playing——设备被占或策略被拒）");
    }
}

/// 摸几个字段，缺哪个都当 0/空——每代引擎可能改名。
fn diagnostics(value: &serde_json::Value) -> (u64, f64, u64, u64, f64) {
    let field = |key: &str| value.get(key).cloned().unwrap_or(serde_json::Value::Null);
    let as_u64 = |v: &serde_json::Value| v.as_u64().or_else(|| v.as_f64().map(|f| f as u64)).unwrap_or(0);
    (
        as_u64(&field("padFrameCount")),
        v_f64(&value),
        as_u64(&field("underrunCount")),
        as_u64(&field("dropCount")),
        field("devicePeak").as_f64().unwrap_or(0.0),
    )
}

fn v_f64(value: &serde_json::Value) -> f64 {
    value
        .get("maxBufferGapMs")
        .and_then(|v| v.as_f64())
        .unwrap_or(0.0)
}

fn set_policy(engine: &Engine, body: serde_json::Value) {
    engine
        .command(
            &serde_json::json!({ "command": "set_policy", "policy": body })
                .to_string(),
        )
        .expect("set_policy");
}

fn fixed(rate: u32) -> serde_json::Value {
    serde_json::json!({ "rate": { "mode": "fixed", "rate_hz": rate }, "quality": "very_high", "share": "exclusive" })
}

fn device() -> serde_json::Value {
    serde_json::json!({ "rate": { "mode": "device" }, "quality": "very_high", "share": "exclusive" })
}

fn native() -> serde_json::Value {
    serde_json::json!({ "rate": { "mode": "native" }, "quality": "very_high", "share": "exclusive" })
}

fn send(engine: &Engine, body: &serde_json::Value, op: &'static str) {
    let line = serde_json::to_string(body).expect("valid json");
    engine
        .command(&line)
        .unwrap_or_else(|error| panic!("[FAIL] {op} 被拒绝：{error}"));
}

/// The probe keeps its library beside the app's, so nothing invents a
/// temporary one that would orphan a favourite between runs.
fn library_dir() -> std::path::PathBuf {
    std::env::var_os("LOCALAPPDATA")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| std::path::PathBuf::from("."))
        .join("Neiro")
}
