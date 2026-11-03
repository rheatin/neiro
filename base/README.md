# neiro-base — the composition root

Base is the layer above the engine and the analyzer. It is not an audio engine
and it does not own a device, a clock, a sample or a measurement.

```
UI / remote client
    ↕  commands + events (serializable)
Base (Rust)
    ├─ transport: play/pause/seek/stop, queue, endpoint choice
    ├─ output policy: rate mode, resampler quality, share mode
    ├─ analysis scheduler + LRU cache
    ├─ render-path fact (`RenderPath`)
    └─ plugin boundary (planned)
         ├→ NAE_Session (C++): playback thread, stream, DSP, gapless
         └→ NAE_AnalyzeFile (C++): path → format/tags/DR/BPM/peaks
```

## Why the session, not `Stream`

`Stream` is the render primitive: `NAE_Play(ms)` blocks, so every caller becomes
an audio scheduler. `Session` already owns the playback thread, the stream, the
position, the queue and the DSP. Base uses `Session`; it must not build a second
render loop around `Stream`. The `Stream` wrapper remains in `neiro-audio` for
the harnesses and low-level probes.

## The render path is an engine fact

`NAE_SessionState` is the single source of truth for what is actually in the
path. Base copies it into `RenderPath`:

| Field | Meaning |
|---|---|
| `resampling` / `dithered` | what the converter and requantiser actually did |
| `bit_perfect` / `reason` | the engine's honest claim, including EQ/crossfeed/convolver/crossfade/ReplayGain/volume |
| `eq_enabled` | the applied cascade, not the requested toggle |
| `crossfeed_enabled` | applied only when the stream is stereo |
| `convolver_enabled` | the IR actually loaded |
| `crossfade_enabled` | active crossfade; zero duration is gapless |
| `replay_gain_enabled` / `rg_applied_gain` | policy and resolved linear gain |
| `volume` | current linear target |

The UI must display this. It must not display "EQ on" because the user set EQ;
a flat EQ, a failed convolver, a resampler with equal rates and a zero-duration
crossfade are all cases where the requested stage and the active stage differ.

## Commands and events

Both are `serde` types in `src/protocol.rs`; the in-process UI calls
`BaseHandle`, and a future out-of-process UI can carry the same values over a
socket.

Commands: `play`, `enqueue`, `pause`, `resume`, `stop`, `seek`, `next`,
`analyze`, `set_volume`, `set_eq`, `set_crossfeed`, `set_replay_gain`,
`set_convolver`, `set_crossfade`, `set_policy`, `shutdown`.

`snapshot().track_serial` increments inside one session each time a preloaded
track takes over. A UI needs it to tell a gapless or manual seam from a new
stream: a seam keeps the session and the counter moves, while a close and reopen
starts over at 0 -- and every other field looks the same.

`settings` in the snapshot is the applied configuration read back from the
session (`EqParams`, `CrossfeedParams`, `ConvolverParams`, `CrossfadeParams`,
`ReplayGainParams`, volume). `render_path` is what is actually active on the
stream. They are deliberately separate: a configured stage may be bypassed on
this source, and the UI must not merge the two.

Events: `state`, `track_changed`, `position`, `analysis_ready`,
`render_path_changed`, `settings_changed`, `policy_changed`,
`queue_changed`, `error`, `shutdown`.

`snapshot()` is the state a late subscriber paints from. Events are changes; the
snapshot is the whole. `Snapshot.resolved` is the engine's actual resolved path
and diagnostics JSON for the current track; it is refreshed on open and on a
gapless track change, not derived by Base.

## Output policy

`PlaybackPolicy` is the one description of how a stream is opened:

| Field | Values | Default |
|---|---|---|
| `rate` | `native`, `device`, `fixed { rate_hz }` | `device` |
| `quality` | `fast`, `high`, `very_high` | `high` |
| `share` | `exclusive`, `shared` | `exclusive` |

These are open-time settings in the engine, so `set_policy` on a live session
reopens it at the current position, and while paused it reopens it paused. A
policy change that only updated a field would leave the old stream resampling
while the UI reported "native", which is exactly the disagreement the render
path exists to prevent.

`set_policy` is a request, not a guarantee. The endpoint decides: `fixed` may
be refused, and the engine reports the rate it granted plus a coercion reason in
`render_path`. `policy` in the snapshot is what was asked for; `render_path` is
what happened. Both are published because they can differ.

`Shared` is never chosen automatically. It exists for an environment where
another process holds the endpoint, and `Base::start_with_share` / the
`NEIRO_BASE_SHARE` probe input are the only ways to ask for it.

## Analysis

`play` and `enqueue` request analysis for the current and next track. The
workers decode on their own threads, write an LRU cache keyed by path + size +
mtime + `analyzerVersion`, and emit `analysis_ready`. The UI never decodes a
file and never writes the cache.

More than one worker, because the engine's analyzer is per-call state:
`computePeaksFromFile` builds its own decoder, its error text is `thread_local`,
and nothing in that path touches an FFmpeg global initialiser. A single worker
made a skip wait behind the prefetch of the track it was skipping to. The count
is `min(cores, 4)` — each decode holds a decoder and its buffers, so this is not
one thread per request — and `NEIRO_BASE_ANALYSIS_WORKERS` overrides it.

Requests for a path already being decoded are dropped, not queued: the same path
is requested twice in normal use, once as the current track and once as the next
track's prefetch, and one decode answers both through the single
`analysis_ready` event.

A track change cancels what it supersedes, at both ends. The engine's decoder
polls a flag the caller owns (`NAE_AnalyzeFileCancelable`), so a decode that is
already running stops between blocks; a request still sitting in the channel is
dropped when a worker picks it up, because the wanted set moved on without it.
A cancelled decode publishes nothing and caches nothing -- its result is a
prefix, not a measurement. That is also how shutdown avoids waiting for a file
nobody will read.

`bpm_valid == false` means the onset envelope was too short or the
autocorrelation peak too weak. The UI must show nothing for BPM, not a plausible
number.

## Plugin boundary

No loader is implemented yet. The intended split is deliberate:

- **Analysis / metadata / source plugins** are offline and may live in Base or
  in a sandboxed worker; they enrich facts for a path.
- **DSP plugins** execute inside the engine/session audio path, not in Base.
  Base only registers, configures and enables them. A plugin on the render path
  obeys the real-time rules: no allocation, no locks, no blocking.
- **UI plugins** run in the UI, not in Base.

A plugin system before the command/event contract is stable would freeze the
internal implementation. The contract above is the prerequisite.

## Run the probe

```powershell
cd engine
cmake --build build
cd base
$env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
cargo run --bin base_probe
cargo run --bin base_capi_check
```

`NEIRO_BASE_DEVICE=<endpoint id>` selects an endpoint; `NEIRO_BASE_SHARE=shared`
chooses shared output for that probe. Both are explicit test inputs, not
production fallbacks.

It generates two 12 s, 120 BPM click WAVs, plays A, enqueues B, checks the
render path, waits for A's analysis (BPM 120.07, confidence 0.833), verifies B
is a cache hit, exercises volume/EQ/crossfeed/crossfade/ReplayGain, then pauses,
resumes, seeks, skips to B and stops. It needs a real output endpoint.

## C ABI for Swift

`include/nae_base.h` exposes the serialized protocol. The Rust side is in
`src/capi.rs`; `base_capi_check` exercises start, snapshot, JSON command,
callback, string ownership and shutdown.

A Swift 6 client should own one actor and never touch UI state from the callback
thread:

```swift
actor BaseClient {
    private let raw: OpaquePointer

    init?() {
        guard let raw = NAE_BaseStart() else { return nil }
        self.raw = raw
    }

    func send(_ json: String) {
        json.withCString { _ = NAE_BaseCommand(raw, $0) }
    }

    func snapshot() -> Snapshot? {
        guard let p = NAE_BaseSnapshot(raw) else { return nil }
        defer { NAE_BaseFreeString(p) }
        return try? JSONDecoder().decode(Snapshot.self, from: Data(bytes: p, count: strlen(p)))
    }
}
```

The C callback is a plain function pointer with a `void *user`; pass an
`Unmanaged<BaseClient>` and dispatch to the actor. Event JSON is the same
`Event` type the Rust UI would consume.

## iOS and Swift 6

Base is intended to stay in Rust on iOS too: it cross-compiles to a static
library (`libneiro_base.a`), which is allowed on the App Store, and the same
Rust core can serve the Windows and iOS builds. The Swift 6 UI talks to it
through the C ABI above; the UI owns `AVAudioSession`, interruptions, route
changes, background audio and Now Playing, because those are platform lifecycle,
not playback policy.

The engine is the part that is not yet cross-platform. `nae.dll` is a Windows
dynamic library and the device layer is WASAPI; an iOS build needs a static
CoreAudio/RemoteIO backend behind the same C ABI. Base's session wrapper does
not change for that port, because it only sees the C ABI.

The plugin model must be static on iOS. App Store rules do not allow downloading
or `dlopen`-ing executable code. Analysis plugins are compiled Rust crates and
DSP plugins are compiled into the engine; the desktop build can offer a dynamic
loader, but the iOS build must share the same static registry. Do not design a
runtime plugin ABI that only works on desktop.

## Limits

- Manual `next` crosses the same seam gapless uses, so the stream, the
  negotiated format and the resampler phase are kept. It falls back to closing
  and reopening when the engine refuses the seam -- an empty queue, or a next
  track whose rate or channel count differs from the current one, which is
  exactly the condition gapless requires.
- The cache is bounded LRU (256 entries); there is no pre-analysis of a whole
  library yet.
- Cancellation is between decoded blocks, so a decode stops within one packet
  rather than instantly. It is not offered to the C ABI yet: a Swift host gets
  the behaviour (Base cancels on a track change) but cannot cancel a decode it
  started through `analyze`.
- The analysis worker count is fixed at start; there is no way to resize the pool
  while Base is running.
- No plugin loader and no out-of-process UI transport yet; both boundaries are
  represented by serializable protocol types.
- `set_policy` reopens the stream, which costs one device close and open. There
  is no crossfade or click suppression across that boundary; a UI should
  debounce a slider rather than send every intermediate value.
- The C ABI has one subscription and no unsubscribe; shutdown is the lifetime
  boundary.
- iOS is not built or verified here. `nae.dll` is Windows-only; an iOS build
  needs the engine's CoreAudio backend and a static link first.
