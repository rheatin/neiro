# AGENTS.md — Neiro Standalone UI Host Architecture & Invariants

This repository (`neiro`) is the standalone desktop client and host for the Neiro High-Fidelity Audio System, built on [Native Shell](https://nsh.dev) (NSH) TSX and a Rust host integration crate (`neiro-ui`).

It communicates with the audio engine exclusively across the `neiro-core` C ABI (`neiro_base.dll` / `libneiro_base.dylib`). Prebuilt core SDK binaries are fetched via `scripts/fetch-core.sh` (or `scripts/fetch-core.ps1`). **Do NOT assume local compilation of C++ engine or local submodules in this repository.**

---

## 1. Single Owner State Invariants

Every piece of state has exactly ONE authoritative owner. Never re-derive or maintain competing copies.

- **Audio Stream Bit-Perfect Status**:
  The only authoritative predicate is `isStreamBitPerfect()` in `ui/src/state/resolved.ts`.
  It evaluates both the engine's reported `renderPath().bitPerfect` and host-side DSP layers (OPRA headphone calibration, crossfade, host EQ). All UI surfaces (`surface/source.tsx`, `surface/master-bus.tsx`, `surface/telemetry.tsx`, `shells/studio-minimal.tsx`, `format-badges.tsx`, `output-interface.tsx`, `pages/settings.tsx`) MUST call `isStreamBitPerfect()`.
- **Sample Rate Formatting**:
  The canonical formatter is `formatHz` in `ui/src/state/resolved.ts`. `formatRate` in `formats.ts` re-exports `formatHz`.
- **Playback Order (Shuffle / Repeat)**:
  `shuffleMode` and `repeatMode` in `ui/src/state/player.ts` are the single source of truth for the host. When the engine emits `queueSnapshot`, host state syncs immediately; user toggles persist to `Storage` and send commands to the engine. `state/shuffle.ts` re-exports directly from `player.ts`.
- **Output Policy (Rate, Share Mode, Dither, DSD)**:
  `applyPolicy(...)` in `ui/src/state/resolved.ts` is the single point for changing and persisting output policies. On startup when engine health becomes ready, `restorePolicyAtStartup()` reapplies the user's saved policy over the engine's default.

---

## 2. C ABI & Backward Compatibility Invariants

The UI host must remain backward and forward compatible across `neiro-core` SDK versions (specifically `v0.2.0` and `>= v0.2.1` / `QueueDirector`).

- **Queue Item JSON Normalization**:
  Any `QueueItem` serialized across the boundary MUST supply snake_case `duration_ms: u64` (even if TypeScript has camelCase `durationMs`). In `ui/src/engine.rs`, `normalize_queue_items_json` cleans and normalizes all incoming queue items.
- **Graceful v0.2.0 `-2` Command Fallback**:
  If the loaded DLL lacks newer symbols (`NAE_BasePlayContext`, `NAE_BasePlayNext`, `NAE_BasePlayLater`, `NAE_BaseSetRepeatMode`, `NAE_BaseSetShuffleMode`, `NAE_BasePrevious`, etc.) or returns `NAE_BASE_ERR_PARSE = -2`, `ui/src/engine.rs` MUST automatically degrade to `v0.2.0` primitive commands (`play`, `enqueue`, `clear_queue`, `seek 0.0`) without raising fatal errors to the user.
- **Honest Plugin Reporting**:
  In fallback mode without dynamic plugin control ABI symbols, the host must not falsely report built-in decoders (`cue`, `sacd`, `dsd`, `soxr`) as togglable. Built-in engine stages remain marked as permanently active.

---

## 3. Remote Streaming & Virtual Path Invariants

- **`remote://` Virtual Paths**:
  Tracks originating from remote sources (Jellyfin, Navidrome, WebDAV) carry virtual paths in the format `remote://{id}.{ext}`.
  The host player MUST intercept `path.startsWith("remote://")` before dispatching to the native playback engine. The first track must resolve to a valid local cache path via `withRemotePath` / `playRemoteByPath`; subsequent items in context queues must resolve to cached local files or be deferred until cached, preventing C++ decoders from opening raw virtual URIs.
- **Callback Array & Epoch Guards**:
  In `ui/src/state/remote.ts`, `fetchedActions` must hold an array of callbacks (`Array<(path: string) => void>`) so concurrent requests for the same track do not drop handlers. All pagination must verify `listEpoch` to discard outdated page responses.

---

## 4. UI Performance & Resource Lifecycle

- **List Virtualization**:
  Large collections (all tracks, scoped folder workspaces, remote catalog) must use virtualized `<List>` with item recycling rather than unbounded `<Each>` inside `<Scroll>`, avoiding UI freezing.
- **On-Demand 60Hz Visualizer**:
  `visualiserWanted` in `ui/src/state/visualiser.ts` must only request 60Hz FFT frames when music is actively playing AND an actual visual consumer is visible (Now Playing sheet open, Output console open, Studio Minimal shell active, or non-off toolbar effect). When hidden, it calls `engine.setVisualiser(null)`.
- **LRU Cache & Watchdog Cleanups**:
  When evicting items from `artwork.ts` cache, always delete from the `asked` set to allow re-fetching on scroll-back. Background catalog pagination must employ watchdog timeouts (15s) to recover from lost IPC events. Subscriptions (e.g. lyrics search) must provide explicit cancellation on sheet closure.

---

## 5. Verification Commands

Run all checks from the repository root:

```bash
# Set up Rust environment if needed
export RUSTUP_HOME="$HOME/Library/Caches/puccinialin/rustup"
export CARGO_HOME="$HOME/Library/Caches/puccinialin/cargo"
export PATH="$HOME/.local/bin:$RUSTUP_HOME/toolchains/stable-aarch64-apple-darwin/bin:$CARGO_HOME/bin:/opt/homebrew/bin:$PATH"

# Typecheck and build Rust crate & TSX
(cd ui && nsh check)

# Run Rust unit tests
cargo test --manifest-path ui/Cargo.toml --lib

# Run all headless UI integration tests
(cd ui && nsh test)
```
