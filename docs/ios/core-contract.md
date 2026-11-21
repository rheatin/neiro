# iOS core contract (signable handoff)

**Target:** `neiro-core` → `neiro_base.xcframework` for `aarch64-apple-ios` and `aarch64-apple-ios-sim`.
**Consumer:** `neiro-ui` (`ui/src/engine.rs`, `ui/build.rs`).
**Status:** draft for signature — Stage 0 gate of `docs/designs/neiro-ios.md`.
**Owner:** `neiro-core` maintainer signs the artifact sections; the author signs the consumption sections.

This is the normative copy of `docs/designs/neiro-ios.md` §3 (engine seam and core contract) and §4 (audio session ownership). The design document and this file must not drift: if either changes, the other is updated in the same commit.

## 1. Artifact

`neiro-core` publishes `neiro_base.xcframework` with exactly two slices:

| Slice | Contents |
| --- | --- |
| `ios-arm64` | `libneiro_base.a` (engine, static) and `FFmpeg.framework` (dynamic, `@rpath` install names) |
| `ios-arm64-simulator` | the same |

The build selects a slice from the Rust target (`CARGO_CFG_TARGET_ARCH` plus `CARGO_CFG_TARGET_ABI`), never from a user path.

**Why FFmpeg is a dynamic framework.** The current build of FFmpeg is shared-only (`scripts/build-ffmpeg-lgpl-macos.sh`), LGPL requires a replaceable library, and the App Store prohibits loading executable code at run time — a framework embedded in the app and linked at build time satisfies both. If the core cannot ship `FFmpeg.framework`, the fallback is a static FFmpeg plus published relinkable object files; that fallback needs written legal sign-off and is not the default.

**Version pin.** The version comes from `NEIRO_CORE_VERSION` at the repository root, the same file `scripts/fetch-core.sh` reads. There is no second pin.

## 2. Symbols

### 2.1 Existing, mandatory

| Symbol | Signature (as used today) |
| --- | --- |
| `NAE_BaseStartWithLibrary` | `unsafe extern "C" fn(*const c_char) -> *mut c_void` |
| `NAE_BaseCommand` | `unsafe extern "C" fn(*mut c_void, *const c_char) -> i32` |
| `NAE_BaseSubscribe` | `unsafe extern "C" fn(*mut c_void, Option<extern "C" fn(*const c_char, *mut c_void)>, *mut c_void) -> i32` |
| `NAE_BaseUnsubscribe` | `unsafe extern "C" fn(*mut c_void) -> i32` |
| `NAE_BaseSnapshot` | `unsafe extern "C" fn(*mut c_void) -> *mut c_char` |
| `NAE_BaseFreeString` | `unsafe extern "C" fn(*mut c_char)` |
| `NAE_BaseShutdown` | `unsafe extern "C" fn(*mut c_void)` |

### 2.2 Queue group, mandatory on iOS

`NAE_BasePlayContext`, `NAE_BasePlayNext`, `NAE_BasePlayLater`, `NAE_BaseSetRepeatMode`, `NAE_BaseSetShuffleMode`, `NAE_BasePrevious`, `NAE_BaseRemoveQueueItem`, `NAE_BaseClearUserQueue`, `NAE_BaseMoveUserQueueItem`.

Also required on iOS: `NAE_BaseRemoteProviders` and `NAE_BaseListDirectory`.

On desktop these are optional and the host falls back to `play` / `enqueue` / `seek 0.0` when a loaded DLL lacks them. On iOS they are linked at build time, so a missing symbol is a link failure, not a silent fallback.

### 2.3 Plugin group, absent on iOS

`NAE_BaseListPlugins`, `NAE_BaseSetPluginEnabled`, `NAE_BaseReloadPlugins` are not declared and not linked on iOS; the host's plugin manager is compiled out.

### 2.4 New symbols required for iOS

| Symbol | Signature | Purpose |
| --- | --- | --- |
| `NAE_BaseAbiVersion` | `() -> u32` | returns the ABI version; the host requires **2** |
| `NAE_BaseAudioCapabilities` | `() -> *mut c_char` | JSON capability object, freed by `NAE_BaseFreeString` |
| `NAE_BaseReinitializeAudio` | `(sample_rate: u32, channels: u16) -> i32` | rebuild the output unit after a route change or media-services reset |

**Capability object** (`NAE_BaseAudioCapabilities`):

```json
{
  "exclusive": false,
  "device_selection": false,
  "dither": false,
  "dsd": false,
  "system_mixer": true
}
```

`NAE_BaseAbiVersion` is checked before anything else. A mismatch or a missing symbol marks the engine `failed` with a message naming both versions. There is no silent degradation on iOS, and the desktop `-2` fallback does not apply.

## 3. Events

New events ride the existing subscription channel and the existing JSON shape, one line per event: `{"event": "<name>", ...}`.

| Event | Fields | Emitted when |
| --- | --- | --- |
| `audio_route_changed` | `route`: `speaker` / `headphones` / `bluetooth` / `usb` / `airplay` / `carplay` / `unknown`; `name`: string; `sample_rate`: number; `channels`: number | the output route changes |
| `audio_format_changed` | `sample_rate`: number; `channels`: number; `sample_format`: `f32` / `s16` / `s24` / `s32`; `resampling`: bool | the render format changes |
| `audio_interruption` | `phase`: `began` / `ended`; `should_resume`: bool | the system interrupts or resumes |
| `audio_capabilities` | `exclusive`: bool; `device_selection`: bool; `dither`: bool; `dsd`: bool; `system_mixer`: bool | after open and after every reinitialize |

Events are additive. An older core simply never emits them; parsing lives in the shared `WireEvent` in `ui/src/engine.rs` so one parser serves both loaders.

## 4. Audio ownership and command order

**The core owns the audio I/O unit and the render callback.** On iOS that is RemoteIO. The core reports the actual render format through `audio_format_changed`.

**The host owns `AVAudioSession`**, its category and activation, and owns lock-screen metadata and remote commands. The host never touches the render callback.

| Moment | Order |
| --- | --- |
| first play | `setCategory(.playback)` → `setActive(true)` → `NAE_BaseReinitializeAudio(rate, channels)` → `play` |
| interruption began | host pauses playback; core stops the renderer |
| interruption ended, `should_resume` | `setActive(true)` → `NAE_BaseReinitializeAudio` → resume |
| route change | read new route → `NAE_BaseReinitializeAudio` → core emits `audio_format_changed` → UI repaints |
| media-services reset | `setActive(false)` → rebuild the session object → `setActive(true)` → `NAE_BaseReinitializeAudio`. The engine handle and the queue are not torn down. |
| background | session stays active; background audio is declared in `Info.plist` |
| shutdown | `setActive(false)` after the engine stops |

**Threading.** Commands and `NAE_BaseReinitializeAudio` are called on the host's control thread. The render callback runs on the audio thread and must not allocate, lock a JS-visible mutex, or call back into the host. Events are delivered on the existing subscription thread.

**No claim of bit-perfect.** The host's `isStreamBitPerfect()` is untouched and not called for iOS badges. iOS shows the route, render format, channels and system volume, with the neutral label "system mixer in path".

## 5. Consumption

`neiro-ui` binds the symbols at link time on iOS through `extern "C"` declarations in `ui/src/engine/ios.rs`; nothing is looked up by name, and no `dlopen`/`dlsym` is used. The desktop keeps its `libloading` path with optional probing and the `-2` fallback, in `ui/src/engine/desktop.rs`.

`ui/build.rs` emits the link search path and `cargo:rustc-link-lib` for the selected slice. If the slice is missing, the build script fails with a message naming the missing archive: `nsh` swallows build-script warnings, and a link failure would report unresolved symbols instead of the cause.

## 6. Signature

| Party | Role | Decision |
| --- | --- | --- |
| `neiro-core` maintainer | Artifact, symbols, events | ☐ Accepted / ☐ Changes required (describe below) |
| Author | Consumption, ownership, ordering | ☐ Accepted / ☐ Changes required (describe below) |

Decision date: ____________
