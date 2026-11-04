# neiro-base — the composition root

Base is the layer above the engine and the analyzer. It is not an audio engine
and it does not own a device, a clock, a sample or a measurement.

```
UI / remote client
    ↕  commands + events (serializable)
Base (Rust)
    ├─ library: favourites, playlists, play counts (SQLite)
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
`set_convolver`, `set_crossfade`, `set_policy`, `list_devices`, `set_device`,
`remove_from_queue`, `clear_queue`, `shutdown`, and the library commands
`set_favourite`, `list_favourites`, `create_playlist`, `rename_playlist`,
`delete_playlist`, `list_playlists`, `add_to_playlist`, `remove_from_playlist`,
`list_playlist_entries`, `list_playlists_containing`, `record_play`,
`list_most_played`, `preanalyze`.

`play` replaces the queue by design: it is the "start this track" command.
Choosing an endpoint is `set_device`, which deliberately does not touch the
queue -- a device menu that discarded what the listener queued would be the
wrong shape.

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

## Library

Favourites, playlists and play counts live here rather than in a host. A host
that kept its own copy would eventually disagree with the queue -- a UI showing
"20 in this playlist" while the engine plays from a three-entry queue is two
answers to one question, which is what AGENTS.md rule 1 is about.

What this does not do: it does not scan directories, watch them for changes or
own a file. A path is a fact the host supplies; the library remembers what the
listener said about it.

Storage is SQLite, compiled into the crate (`bundled`). A playlist is a
many-to-many relation with an order, and "which playlists contain this track" is
asked on every selection -- that is a join, not a scan. A JSON file would rewrite
the whole library on every favourite toggle, which is fine at a hundred tracks
and not at ten thousand.

| Operation | Notes |
|---|---|
| `set_favourite` / `is_favourite` / `favourites` | returns whether it changed, so a UI can skip an event nobody needs |
| `record_play` / `play_count` / `most_played` | the clock is the host's; Base does not own one |
| `create_playlist` / `rename_playlist` / `delete_playlist` / `playlists` | names are unique case-insensitively, enforced by the schema's index rather than by a check in front of it |
| `add_to_playlist` / `remove_from_playlist` / `playlist_entries` | adding a track twice is a no-op; a removal closes the position gap |
| `move_in_queue` | a move is a remove plus an insert; the index follows the playing entry |
| `playlists_containing` | the reverse query a UI asks on selection |
| `record_play` / `list_most_played` | the clock is the host's; a play republishes the list the UI last asked for |

Every one of these is on the wire as a command, and answers as an event
(`favourites_changed`, `playlists_changed`, `playlist_entries_changed`,
`playlists_containing_changed`) rather than as a return value: the C ABI is
asynchronous, so a host that expected a synchronous answer would block a thread
waiting for something that already arrived.

A library is opened with `Base::start_with_policy_and_library` in Rust or
`NAE_BaseStartWithLibrary` in C. Without one, every library command reports
"no library is open" instead of returning an empty list -- "you have no playlists"
is a statement about the listener's music, not about this process.

Positions inside a playlist stay dense: the UI addresses entries by index, and a
hole would make `entry[3]` and `position 3` disagree. Deleting a playlist takes
its entries with it through the schema's cascade.

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
cargo run --bin library_check
```

The library layer pulls in `rusqlite` with the `bundled` feature, so cc-rs
compiles SQLite from source. cc-rs looks for `gcc` by name and this machine's
MinGW ships `cc`/`c++` only, so `base/.cargo/config.toml` names them -- that file
holds absolute paths and is therefore **not committed**. A machine with `gcc`
on PATH needs nothing; otherwise set `CC` and `CXX` in the environment. The CRT
matters: `nae.dll` is built with `-mcrtdll=ucrt`, and a bundled SQLite on a
different runtime would be two allocators in one process.

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
- The cache is a bounded LRU that starts at 256 entries and grows to hold a
  pre-analysis batch, never shrinking. A scan of 5000 tracks against a 256-entry
  cache would evict its own early results as it produced them.
- Pre-analysis takes an explicit path list: Base does not scan directories, so a
  host that wants to analyze a library supplies the paths (from its own index, or
  from favourites and playlists). Progress arrives as `preanalysis_progress`
  with `done`/`total`, and `complete` on the last event.
- Cancellation is between decoded blocks, so a decode stops within one packet
  rather than instantly. It is on the wire: `analyze` takes an optional
  `cancel_id`, and `cancel_analysis` stops the decode that carries it. A
  cancelled decode publishes nothing.
- The analysis pool can be resized at runtime with `set_analysis_workers`.
  Growing starts threads immediately; shrinking moves the target and each
  worker past it exits when it next becomes idle -- a worker never exits while
  holding a request, because the request is already off the channel and would
  be lost.
- The most-played list is refreshed after a play only if one is being watched,
  and at the limit of the last `list_most_played`. A host that never asked for
  one gets no event, which is the point: an event nobody subscribed to is work
  the UI cannot use.
- No plugin loader. The out-of-process transport exists:
  `neiro_base::transport` serves one client at a time over a named pipe, one
  JSON object per line, commands in and events out on the same connection.
- The transport is Windows-only and single-client by design: the control thread
  owns the session, and two clients issuing commands would be two owners of one
  queue.
- `set_policy` reopens the stream, which costs one device close and open. There
  is no crossfade or click suppression across that boundary; a UI should
  debounce a slider rather than send every intermediate value.
- `set_device` on a live stream reopens on the new endpoint at the current
  position. The reopen publishes `Ready` at the resumed position before it
  starts the new stream, which is what makes the resume assertable: the check
  compares against the position it left, within 0.10 s, and fails with
  `1.539s -> 0.000s` when the seek is removed.
- Queue editing is move, remove and clear. Removing an entry
  rebuilds the engine's queue from the current index forward, so the engine seams
  to what the UI shows. The entry that is playing cannot be removed -- `Stop` first.
- The queue rebuild is checked against the engine's own count, not Base's copy:
  `engine_queue_count` in the snapshot is what the session reports holding, and
  it is compared with the edited queue. Dropping the rebuild reports
  `engine Some(3), expected 1`.
- The C ABI has one subscription at a time per `NAE_Base`; `NAE_BaseUnsubscribe`
  drops it and lets the same handle subscribe again, which is what a host with
  several windows needs. Two simultaneous subscribers still require two handles.
- iOS is not built or verified here. `nae.dll` is Windows-only; an iOS build
  needs the engine's CoreAudio backend and a static link first.
