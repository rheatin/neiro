# Neiro Three-Tier Plugin Architecture

* **Author:** Char ONE
* **Date:** 2026-11-16

## Overview

Neiro will introduce a three-tier plugin architecture across its C++ audio engine (`neiro-core/engine`), its Rust data and orchestration layer (`neiro-core/base`), and its TypeScript/NSH frontend (`neiro/ui`). The goal is to let first-party authors, community contributors, and local users in developer mode extend or replace audio processing, music sources, container expanders, and the user interface itself without modifying core orchestration code.

Under the proposed concept:
1. **Engine Tier (`neiro-core/engine`)**: Loads C ABI dynamic libraries (`.dylib` / `.dll`) for `playback.decoder`, `playback.dsp`, and `playback.output` at startup or on explicit engine restart. Any active DSP plugin marks `bit_perfect = false` with an explicit reason code.
2. **Base Tier (`neiro-core/base`)**: Manages `source.connector` (remote servers and streaming services), `source.storage` (byte-range virtual filesystems for local disks, WebDAV, NAS, and cloud drives), `source.expander` (multi-track containers such as CUE sheets and SACD `.iso` images), and `source.enricher` (lyrics and artwork). Built-in plugins run in-process via `PluginRegistry`; external plugins run out-of-process.
3. **UI Tier (`neiro/ui`)**: Supports hot-swappable full-window shells (`ui.shell`) and modular slot/visualizer components (`ui.slot`, `ui.visualizer`). The host core retains only the Base SDK bridge, permission checks for destructive file operations, and the Settings / Plugin Manager overlay, while the current Industrial Rack interface runs as the default compiled-in `ui.shell`.

## Analysis

Six independent, zero-context Goldfish reviewers (`egm-design`, `egm-zoom-out`, `egm-codebase-details`, `egm-comprehension`, `egm-implementation-readiness`, and `egm-structural`) evaluated this concept. Their deduplicated findings are grouped below by severity and architectural theme.

### 1. Bricked UI Risk and Shell Lifecycle Leaks (`ui.shell` Hot-Swapping) — Blocker
* **Lenses:** `egm-comprehension`, `egm-implementation-readiness`, `egm-zoom-out`, `egm-structural`, `egm-design`
* **Problem:** If the entire application layout is delegated to a swappable `ui.shell` plugin and the active third-party shell crashes on boot, enters an infinite render loop, or omits the button that opens the Settings / Plugin Manager overlay, the user is locked out of the application with no way to switch back. In addition, swapping shells or visualizers at runtime without automatic teardown will leak Base event subscriptions (`on_player`, `on_visualiser`) and starve the UI thread with duplicate 60 Hz FFT callbacks.
* **Required Safeguards:**
  1. **Compiled-In Fallback Shell & Hardware Safe Mode:** The default Industrial Rack shell must remain compiled into the host binary (not loaded from an external file that can be deleted or corrupted). If an external `ui.shell` throws during mount or fails to signal readiness within a 2-second boot watchdog, the host automatically reverts to the compiled-in default shell.
  2. **Un-interceptable Host Shortcut & CLI Flag:** The host window reserves a global keyboard shortcut (`Cmd/Ctrl + Shift + ,` or `Shift + Escape`) and a `--safe-mode` CLI flag that open the Host Plugin/Settings Overlay regardless of what the active `ui.shell` renders.
  3. **Scoped Disposer per Plugin Instance:** Every `ui.shell` or `ui.slot` receives a scoped `NeiroPluginContext` whose event listeners and timers are automatically unsubscribed by the host the moment the plugin unmounts.

### 2. Out-of-Process IPC Bottlenecks for High-Bitrate Audio (`source.storage` & `source.connector`) — Blocker
* **Lenses:** `egm-design`, `egm-codebase-details`, `egm-comprehension`, `egm-structural`, `egm-zoom-out`
* **Problem:** Using `stdio` JSON-RPC for metadata while opening a second ad-hoc socket or pipe to stream raw audio bytes for `source.storage::read_range` forces plugin authors to coordinate two transports, handle seek aborts, and manage backpressure. At high bitrates (such as DSD256/DSD512 at 22.5 to 45.2 Mbps) or during SACD `.iso` sector table parsing over network shares, naive per-range IPC roundtrips cause context-switch overhead, file descriptor exhaustion, and audio buffer underruns.
* **Required Safeguards:**
  1. **Local File / HTTP Range Staging instead of Dual-Channel Custom Sockets:** External `source.connector` plugins should resolve tracks either to a local cached file path or to a standard HTTP/Unix-socket URL that supports standard `Range` headers, rather than inventing a custom binary framing protocol alongside `stdio` JSON-RPC.
  2. **Block Cache & Handle Ceiling in Base VFS:** `neiro-core/base` retains sole ownership of byte-range caching (e.g., 256 KB to 1 MB read-ahead blocks with bounded LRU file/connection handles) so container expanders (`CueExpander`, `SacdIsoExpander`) never issue thousands of tiny IPC or network calls.
  3. **Request Correlation & Crash Capture:** Every `stdio` JSON-RPC call carries a `request_id` and timeout; `base` captures child PID, exit status, and a bounded `stderr` ring buffer so timeouts or crashes surface in `Event::Error` with the offending `plugin_id`.

### 3. In-Process C ABI Safety, Version Drift, and `bit_perfect` Attribution in `Engine` — Blocker
* **Lenses:** `egm-codebase-details`, `egm-implementation-readiness`, `egm-structural`, `egm-comprehension`
* **Problem:** Loading third-party `.dylib` / `.dll` files into the C++ real-time audio thread means any struct layout mismatch, heap allocation, mutex lock, or C++ exception inside a plugin will cause audio dropouts or crash the process. Furthermore, if a chain of DSP plugins sets `bit_perfect = false`, a single integer reason code does not identify which plugin broke bit-perfect playback.
* **Required Safeguards:**
  1. **Frozen ABI Handshake:** Every Engine plugin exports `nae_plugin_abi_version()` and populates `struct_size` on every C struct. If the version or size does not match `NEIRO_ENGINE_ABI_VERSION`, `engine` refuses to load the library and reports a clear error without crashing.
  2. **Explicit Restart State Machine:** Enabling, disabling, or reordering Engine `.dylib` plugins while a stream is open either reopens the session at the current playback position (when only session-level DSP nodes change) or returns an explicit `REQUIRES_RESTART` state in the Plugin Manager, never calling `dlclose` while audio threads are active.
  3. **Plugin Attribution in Resolved Diagnostics:** When an Engine DSP plugin modifies samples, `resolvedJson` records both `NAE_BP_REASON_DSP_PLUGIN` and the `activeDspPlugins: [plugin_id, ...]` list, plus per-plugin deadline-miss counters via a lock-free atomic counter.

### 4. Destructive API Isolation & Database URI Compatibility (`Rule 7`) — Major
* **Lenses:** `egm-implementation-readiness`, `egm-structural`, `egm-codebase-details`, `egm-zoom-out`, `egm-comprehension`
* **Problem:** Allowing UI or enricher plugins to invoke `write_tags`, `write_lyrics`, `set_artwork`, or `forget_folder` introduces two hazards: (1) a buggy plugin loop can corrupt user audio files if manifest permissions alone are trusted without write coordination, and (2) storing virtual plugin URIs (`#cue=...`, `#sacd=...`) in SQLite leaves orphaned rows if a user disables the corresponding expander plugin.
* **Required Safeguards:**
  1. **Exclusive Write Coordinator & Interactive Confirmation:** Plugins never write to audio files directly. All tag/artwork writes go through `neiro-core/base`'s atomic temp-file-and-rename pipeline (`Rule 7`), require `"permissions": ["tags.write"]` in `manifest.json`, log an audit entry with `calling_plugin_id`, and return a typed `PermissionDeniedError` when unauthorized.
  2. **Non-Destructive Database Tombstones:** Disabling a `source.expander` or `source.connector` plugin hides its virtual tracks from active browse queries without deleting user playlists, favourites, or play counts from SQLite.

## Goals vs. Non-Goals

* **In-Scope Goals:**
  * Define a unified plugin directory (`~/.config/neiro/plugins/<plugin-id>/manifest.json`) and lifecycle state (`enabled` / `disabled` / `requires_restart`) owned by `neiro-core/base`.
  * Support runtime enable/disable of built-in and external plugins via both CLI (`neiro_plugin`) and the Host Plugin Manager UI.
  * Support hot-swappable `ui.shell` and `ui.slot` plugins in `neiro/ui` with a compiled-in fallback shell, a reserved host recovery shortcut, and automatic subscription disposal on unmount.
  * Enforce manifest-declared permissions (`tags.write`, `library.manage`, `remote.connect`) in the UI Host SDK bridge.
  * Support out-of-process `stdio` JSON-RPC plugins in `neiro-core/base` for `source.connector` and `source.enricher`, with local file/HTTP staging for audio streams.
  * Define a versioned C ABI plugin struct (`NEIRO_ENGINE_ABI_VERSION`) for `neiro-core/engine` DSP/decoder plugins.
* **Out-of-Scope Non-Goals:**
  * Calling `dlclose` on C++ `.dylib`/`.dll` plugins while an audio render thread or analysis worker is actively executing code from that library.
  * Streaming raw high-bitrate PCM/DSD frames as base64-encoded JSON strings over `stdio` pipes.
  * Allowing external plugins to bypass `neiro-core/base` and mutate audio files on disk directly.
  * Building a cloud-hosted third-party plugin marketplace server in this phase (local directory installation, CLI install, and developer watch mode come first).

## Alternatives Weighed

* **Rejected by author: Restricting UI plugins to declarative JSON schemas only.** The author requires unlimited visual and layout freedom so developers and community members can build entirely different interfaces, layouts, and visual styles on top of `Base`'s APIs, including replacing the main workspace view.
* **Rejected by author: Supporting live `dlclose` hot-unloading of C++ Engine plugins during playback.** The author confirmed that requiring an engine/session restart for `Engine` or `Base` native plugins is acceptable if live unloading risks instability, keeping live hot-plugging focused on `UI` plugins and out-of-process `Base` connectors.
* **Rejected during Goldfish review: Dual-channel `stdio` JSON-RPC + custom raw binary socket protocol for every `source.storage` plugin.** Rejected because custom binary socket framing and backpressure handling impose excessive complexity on plugin authors and risk buffer underruns; external connectors will instead resolve streams to local cached files or standard HTTP `Range` endpoints managed by `Base`'s block cache.
* **Rejected during Goldfish review: Moving the default Industrial Rack UI out of the compiled binary into an external disk-only plugin.** Rejected because a missing or broken external file would brick the app on startup; the default Industrial Rack shell remains compiled in as the immutable fallback `ui.shell`.

## Key Decisions & Staging for Next Steps

1. **Phase 1 — Unified Plugin Manifest, Registry State & CLI (`neiro-core/base`)**:
   * Extend `PluginRegistry` in `neiro-core/base/src/plugin.rs` to manage plugin descriptors across all tiers, persist `enabled: bool` state in `plugins.json`, scan `~/.config/neiro/plugins/*/manifest.json`, and expose CLI commands (`list`, `enable`, `disable`, `install`, `probe`).
2. **Phase 2 — Out-of-Process `stdio` JSON-RPC Bridge (`neiro-core/base`)**:
   * Implement `ExternalProcessPlugin` in `neiro-core/base` with request timeouts, `plugin_id` error attribution, and `stderr` capture so external scripts/binaries can act as `source.connector` or `source.enricher` plugins without recompiling Rust.
3. **Phase 3 — Host Core Shell, Safe-Mode Watchdog & Permission Gate (`neiro/ui`)**:
   * Refactor `neiro/ui` root so the host core owns the `NeiroPluginSDK` (with scoped subscription cleanup and manifest permission checks for `tags.write` / `library.manage`), a global recovery shortcut (`Cmd/Ctrl + Shift + ,`), the Plugin Manager overlay, and a `ui.shell` mount point backed by the compiled-in Industrial Rack default shell and Developer Mode file watcher.
4. **Phase 4 — Versioned C ABI DSP / Decoder Plugin Loader (`neiro-core/engine`)**:
   * Add `NEIRO_ENGINE_ABI_VERSION` and `NAE_DspPluginV1` header contract in `neiro-core/engine`, loading enabled `.dylib`/`.dll` DSP plugins on session open and recording `activeDspPlugins` in `resolvedJson` whenever `bit_perfect` is invalidated.
