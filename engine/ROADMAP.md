# Roadmap

Status: **platform coverage is the top priority.** Feature work that is not
needed for a platform-complete core is parked below, not pursued in parallel.

## The build premise

- **One C++ source tree, one CMake project, one DSP/policy core.**
- Per-OS binaries are still necessary: a Windows DLL, a macOS framework/static
  library, an iOS static library, an Android `.so`. That is not "writing the
  business again"; only the backend and the UI shell differ.
- No platform gets a second copy of EQ, routing, ReplayGain, dither, resampler,
  queue policy or diagnostics.
- CI builds the same core on every platform. The backend is selected at configure
  time (`NEIRO_BACKEND=WASAPI|COREAUDIO|AAUDIO`) and the rest of the graph is
  unchanged.
- Base stays Rust. Swift 6 and Android UI call the same Base C ABI. The UI never
  links a backend.

## P0 — platform-complete core (current)

### A. Backend interface

Extract one interface from the WASAPI code and make it the only OS boundary:

```
enumerateDevices()
queryCaps(deviceId, shareMode)
open(request) -> Stream
Stream.start / stop / pause / render
Stream.deviceLost / close
```

The interface returns `NAE_DeviceCaps`; it does not know about routing, DSD
policy, EQ, diagnostics or Base.

**Progress (2026-11-07):** `NAE_Handle` is in `engine/nae_internal.h`, and the
macOS device open plus the default-device lookup are in
`engine/open_coreaudio.cpp`, a translation unit Windows does not compile. What is
left of the platform code in `nae.cpp` is the Windows backend (which still talks
to WASAPI directly), forty lines of macOS JSON assembly, and the adapter struct
that turns `neiro::OutputStream` into the status the render loop reads.

`backend_wasapi.cpp` is gone: it was not a backend. Every method called back into
the DLL's own exports, its "stream" pumped `NAE_Play` on a thread, and nothing in
the engine used it -- it was linked into `nae` for no reason and used only by
`backend_check`. The adapter now lives inside that check, where its only caller
is, and the engine's source list is back to `nae.cpp` + `session.cpp`.

The real Windows backend exists now: `backend_wasapi.cpp` owns the
`IAudioClient` and presents the same "room, window, commit" stream the CoreAudio
one does, and both open paths are the same function shape. `NAE_Play` has one
device path on both platforms. See `PORT-HANDOVER.md` for what the port had to
get right (the `SetEventHandle` order, and the event-driven exclusive fallback on
this endpoint).

### B. Platform-neutral core target

- `neiro_core` is now a static target with `policy.cpp`, `hilbert.cpp`,
  `dither.cpp`, `resampler.cpp` and, when FFmpeg is present, `decoder.cpp`.
- It has no Windows header. `policy_check` and `hilbert_check` link it.
- `nae` links it on Windows. The CMake graph stops after the core checks on a
  non-Windows system until a backend exists.
- The render/DSP loop still has to move behind `audio_backend.h`; sample
  processing stays shared.

### C. WASAPI backend

- Move the existing `IMMDevice`/`IAudioClient` code behind the interface.
- `NAE_QueryDeviceCaps` for WASAPI is now implemented and exported (native
  format, exclusive rates, layouts, DoP carriers; buffer and shared-rate probing
  remain unprobed and are reported as unknown).
- Keep the measured behaviour: exclusive/shared, event-driven buffer, 50 ms
  default, device-lost semantics.
- Session and resolver consume caps; no UI-side format guesses.

### D. CoreAudio backend (macOS)

- Implemented: enumeration, default device, caps, HAL AudioUnit output, pull
  render callback, backend selection in CMake, exclusive access through
  `kAudioDevicePropertyHogMode` (shared never takes the device), the resampler
  wired at open, and the device's sample rate restored on close.
- Implemented since: device loss. A listener on
  `kAudioDevicePropertyDeviceIsAlive` sets the stream's lost flag, and a failed
  `AudioOutputUnitStart` on a device the HAL no longer knows does the same, so
  the engine's `NAE_ERR_DEVICE_LOST` path is reachable here instead of being dead
  code. Verified only for the absence of false positives: the firing path needs
  a device to be unplugged mid-stream.
- Still open: interruption/route lifecycle (there is no AVAudioSession on macOS;
  a default-device change is answered by the next query, because the C ABI is
  pull-based), DoP carrier probing, native DSD, bit depth (the route is float32
  only, so `bit_perfect` cannot be true).

### E. iOS shell

- Static `libneiro_base.a` + `libnae.a`; no dynamic loading.
- Swift 6 owns `AVAudioSession`, interruptions, route changes, background audio
  and Now Playing; Base owns queue/analysis/cache.
- App Store: no downloaded code, no `dlopen` plugins, sandboxed file access.
- One XCFramework build, not per-screen builds.

### F. Android backend

- Implemented: AAudio default-device caps and float32 pull stream, backend
  selection in CMake.
- Still open: USB host, audio focus, foreground service, route changes, DoP/native
  DSD, NDK build packaging.

## P1 — platform parity

These are not new product features; they are the engine reaching the same
behaviour everywhere.

1. **Session ↔ resolver ↔ diagnostics wiring** — **actual path done**;
   policy/Auto preview still open.
   - `NAE_QueryDeviceCaps` fills real caps.
   - `NAE_Session` stores the actual resolved path and diagnostics per track.
   - `NAE_SessionGetResolvedSignalPath` / JSON C ABI.
   - Base snapshot/event exposes `resolved`; the engine refreshes it on open and
     gapless track change.
   - Policy-driven open now applies rate/resampler/dither where supported;
     routing/DSD/buffer/channel/clip policy fields emit WARNING diagnostics and
     use the decoder/backend defaults.
   - Still open: implementing those stages so the warnings disappear.
2. **Routing in the render path**
   - The matrix builder, complex gains and `RoutingProcessor` exist and are
     tested on 5.1 LtRt (quadrature, direct gain, 127-frame latency).
   - `NAE_OpenFileWithRouting` keeps the file's source channels, routes in
     `topUp` before the resampler, and passes real routed playback.
   - Session policy builds/applies preset routing automatically and can consume
     a registered custom matrix (`NAE_GetRoutingMatrix`) when
     `routingMode=MATRIX`. Direct routed open and `session_routing_check` pass.
   - Channel trim/balance/polarity/delay and the clip guard limiter are applied
     from policy; DSD routing remains open.
   - Buffer policy is applied: Auto/Stable default, LowLatency 10 ms, Fixed
     duration; a refused request falls back and reports.
   - Bass management derives the LFE from a low-passed routed sum; session policy
     carries the crossover/gain into the matrix. CoreAudio/AAudio still use
     backend buffer defaults.
3. **DSD policy parity**
   - Session now attempts DoP, falls back to PCM with diagnostics, and rejects
     native DSD when no backend supports it.
   - Native DSD API and Session path are scaffolded
     (`NAE_OpenNativeDsd`/`NAE_PlayNativeDsd`). The capability probe now marks
     `NAE_CAPS_PROBED_NATIVE_DSD` and reports 0 on WASAPI, which defines no
     1-bit stream format; `NAE_OpenNativeDsd` answers from that probe instead of
     unconditionally. A backend that advertises 1-bit formats still has no
     stream path compiled in.
   - DSD `EXTRACT` runs through the PCM path and is reported as extracted;
     `DOWNRATE` decimates the bitstream (1/2/4/8) and is carried over DoP.
     Still open: an ASIO/USB/vendor native backend, native DSD on
     macOS/Android, and DoP verification on an endpoint that accepts a carrier.
4. **IIR all-pass Hilbert**
   - FIR 255 is the working LtRt filter; add the frequency-warped IIR for
     lower latency/flat magnitude, selectable, with a diagnostic.
5. **LGPL FFmpeg**
   - The current shared build is GPL; distribution (especially App Store)
     requires the LGPL link.
6. **Platform CI**
   - Windows, macOS, iOS simulator/device, Android; the same `policy_check`
     and `hilbert_check` run everywhere.

## P2 — feature work (parked)

Do not start these before P0/P1 is green. They are listed so they do not get
lost and do not get mixed into the platform refactor.

- PCM→DSD modulator, native DSD output, DSD domain volume/gain.
- Full routing DSP: arbitrary matrices in the render loop, all immersive presets,
  speaker distance/bass management execution.
- EQ: 32 bands, minimum or linear phase, per-channel masks, no channel cap
  (the engine's 32-channel limit applies).
- Convolver: one state set per channel; a 5.1 IR is applied to all six.
- Advanced DSP: IIR all-pass Hilbert is implemented and is the LtRt default;
  the FIR remains selectable. Nothing outstanding at this level. (Clip guard with a 4x
  polyphase true-peak detector, a uniform-partitioned FFT convolver, BS.1770-4 /
  EBU R128 loudness with gating, and minimum/linear-phase per-channel EQ are
  implemented.)
- Analysis extras: cover art, lyrics, key, beat grid, true peak, official DR.
- Plugin ABI: static analysis/DSP registry first; dynamic loader only on
  platforms that permit it.
- Parallel analysis workers, cancellation, library-wide pre-analysis.
- UIs: Swift 6 macOS/iOS, Android, desktop host.

## Parked defects / debt

These are known and not part of the current platform push:

- `NAE_Session` path/queue index observability: the queue count and the
  preloaded flag are exposed, but not the queue's contents.
- Platform-specific DSD native paths (vendor SDK/driver constraints).

Resolved since this list was written: IIR Hilbert is implemented and is the
LtRt default; `NAE_QueryDeviceCaps` is exported and probed; Base carries the
resolved path and diagnostics; the session applies policy; and the FFmpeg
licence is reported by the engine and `scripts/build-lgpl.ps1` builds and
verifies the LGPL variant.

## Definition of done for P0

- The same core library builds and its checks pass on Windows, macOS and Android
  (iOS via XCFramework).
- No platform header appears in the core target.
- A device on each backend produces the same `NAE_DeviceCaps` shape and the same
  resolver results for the same source/policy.
- Session, Base and UI consume one resolved path/diagnostic stream.
- Windows behaviour is not regressed by the extraction; existing checks and
  measured latencies remain.

## Next commit sequence

1. Backend interface header + no-op/stub backend for compile guards.
2. Extract core from `nae.cpp`; keep the Windows backend behaviour identical.
3. WASAPI caps adapter; `policy_check` with real caps.
4. Session/Base resolved-path wiring.
5. CoreAudio backend; macOS CI.
6. iOS/Android shells.
