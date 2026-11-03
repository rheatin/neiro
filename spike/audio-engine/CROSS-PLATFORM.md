# Cross-platform core and macOS

Status: architecture, not a shipped macOS build. Windows/WASAPI is the only
backend implemented and verified today. Platform coverage is the top priority;
see [`ROADMAP.md`](ROADMAP.md) for the current sequence and what is parked.

## The split

```
platform-neutral core
  policy.cpp          OutputPolicy + DeviceCaps + ResolvedSignalPath + diagnostics
  hilbert.cpp/.h      90-degree FIR Hilbert for Lt/Rt
  routing matrix      complex gains, presets, LTRT, bass management
  dither / resampler  already platform-neutral
  analysis            already path-based
        |
        v
backend interface
  enumerate / capabilities / open / start / stop / close / device-lost
        |
        +-- WASAPI (implemented, nae.cpp)
        +-- CoreAudio (planned)
        +-- AAudio/OpenSL ES (planned)
```

Base stays Rust and calls the C ABI. A Swift 6 UI calls Base through the C ABI
already in `base/include/nae_base.h`; it does not link the audio backend.

## Current Windows coupling

`nae.cpp` is WASAPI/COM-specific: `IMMDevice`, `IAudioClient`, `WAVEFORMATEXTENSIBLE`,
`CoInitializeEx`, device events, exclusive-mode probing. It cannot be compiled on
macOS as-is.

`policy.cpp` and `hilbert.cpp` deliberately include no Windows header and compile
as the `policy_check` / `hilbert_check` targets. The resolver can be reused on
macOS before CoreAudio exists.

## macOS: what you configure, and what I build

The engine cannot be compiled on macOS today because `nae.cpp` and `session.cpp`
are WASAPI/COM. The platform-neutral core (`neiro_core`) already has no Windows
header and can be configured on macOS. The steps below separate what you set up
once from what still has to be written.

### What you need on the Mac

1. **Xcode Command Line Tools** (`xcode-select --install`). Gives clang, the
   macOS SDK and `codesign`.
2. **CMake** (3.20+) and **Ninja**:
   `brew install cmake ninja`.
3. **An FFmpeg build for macOS.** Two options:
   - `brew install ffmpeg` (Homebrew, shared libs under
     `/opt/homebrew/lib` or `/usr/local/lib`);
   - or an LGPL static/shared build you keep for release. The Windows scoop
     build is GPL; macOS distribution needs LGPL too.
4. **Rust** (for Base) if you want the Rust composition root on the Mac:
   `rustup target add aarch64-apple-darwin x86_64-apple-darwin`.
5. **soxr**: already vendored in `third_party/soxr`; nothing to install.
6. **A real output device.** Built-in speakers are enough for a smoke test;
   a USB DAC is needed for DoP/rate checks.

Configure the core only (it stops before the Windows backend):

```bash
cd spike/audio-engine
cmake --preset macos
cmake --build --preset macos
./build-macos/policy_check
./build-macos/hilbert_check
./build-macos/routing_check
```

`CMakePresets.json` also has `windows` for the full Windows build (UCRT flags included).
The preset finds Homebrew (`brew --prefix`) for FFmpeg; override with
`-DNEIRO_FFMPEG_ROOT=<prefix>` if it is elsewhere.

`NEIRO_FFMPEG_ROOT` must be a directory containing `include/` and `lib/`. If
FFmpeg is not found, the build still produces the tone path and the core checks;
decoding is disabled with a warning.

### What is implemented

1. **CoreAudio backend** (`backend_coreaudio.cpp`): CoreAudio device enumeration,
   default device, caps (rate/channels/layout/format), HAL AudioUnit output at
   the requested rate, pull-based render callback. DoP/native DSD are not claimed
   because this HAL path does not advertise them.
2. **AAudio backend** (`backend_aaudio.cpp`): default-device discovery, caps
   from an opened AAudio stream, float32 pull callback, low-latency mode.
3. **WASAPI adapter** (`backend_wasapi.cpp`): enumeration, caps, and a stream
   that drives the existing `NAE_Play` in slices.
4. **CMake backend selection**: `NEIRO_BACKEND=WASAPI|COREAUDIO|AAUDIO`.
   WASAPI builds `nae.dll`; CoreAudio/AAudio build `neiro_backend` plus the core
   checks, and do not duplicate DSP.

## What still has to be written

1. **Route/interruption lifecycle** on Apple: CoreAudio device notifications,
   AVAudioSession interruptions and route changes are host concerns; the backend
   currently reports `deviceLost()` only when a call fails.
2. **iOS XCFramework**: Rust Base plus `neiro_core` and the CoreAudio backend
   need a static build and a Swift package.
3. **AAudio device enumeration**: AAudio has no public enumeration API; the
   backend reports the default device only.
4. **DoP/native DSD** on CoreAudio/AAudio: DoP needs a device that accepts the
   carrier rate; native DSD is driver-specific and must be advertised.
3. **File helper portability**: `utf8_file.h` already has a non-Windows branch;
   `decoder.cpp` uses `std::filesystem`, which is portable. No change expected
   beyond a compile pass.
4. **Base on macOS**: the Rust crate already has a C ABI; it needs a static
   library target and an XCFramework for the Swift UI. Base itself does not
   change.
5. **Apple lifecycle**: `AVAudioSession` (iOS), interruptions, route changes,
   background audio and Now Playing belong in the Swift shell, not the engine.
6. **DoP/native DSD on macOS**: DoP needs an endpoint that accepts the carrier
   rate at 24 valid bits; native DSD is driver/vendor-specific and must be
   advertised by caps, not assumed.

### What is not required

- No Windows toolchain, no MinGW, no `nae.def`/`nae.map` on macOS.
- No second copy of DSP, routing, policy, dither, resampler or diagnostics.
- No UI framework in the engine build graph.

## What macOS needs

1. **Extract the backend interface** from `nae.cpp`: device enumeration,
   capabilities, open/close, stream start/stop, event-driven buffer timing,
   device-lost detection, shutdown.
2. **Keep one DSP/render core.** Channel routing, EQ, crossfeed, convolver,
   ReplayGain, volume, clip guard, dither, resampler and the buffer ring must not
   be duplicated per OS. The backend supplies frames; the core processes them.
3. **CoreAudio backend**:
   - device enumeration and default-device notifications;
   - AudioUnit/RemoteIO or AudioDevice HAL with the requested sample rate,
     channel layout and format;
   - buffer frame size and latency reporting;
   - DoP carrier support; native DSD is normally vendor-driver/SDK-specific and
     must be advertised by `NAE_DeviceCaps`, not assumed;
   - route changes and interruptions invalidate `capsGeneration`.
4. **CMake split**:
   - `NEIRO_BACKEND_WASAPI`, `NEIRO_BACKEND_COREAUDIO`, `NEIRO_BACKEND_AAUDIO`;
   - platform-neutral core as a static library;
   - `policy_check` / `hilbert_check` build on all platforms.
5. **App lifecycle**:
   - macOS does not have `AVAudioSession`; use CoreAudio + `NSWorkspace`/default
     device notifications. iOS uses `AVAudioSession` in the Swift shell.
   - Background audio, interruptions, route changes and Now Playing stay in the
     Swift/iOS shell.
6. **Distribution**:
   - static link Rust Base + C++ engine; no `dlopen` plugins on iOS;
   - App Store: no downloaded executable code, sandboxed file access, hardened
     runtime/signing;
   - LGPL FFmpeg build is required for distribution (the current shared build is
     GPL).

## Backend interface

[`audio_backend.h`](audio_backend.h) is the extraction target: enumerate,
queryCaps, open, stream start/stop/pause/resume, device-lost and latency. The
current WASAPI implementation is still inside `nae.cpp`; the interface is the
only contract the core may eventually depend on.

## What is done now

- `policy.cpp`: `NAE_OutputPolicyDefaults`, matrix builder/registry, pure
  resolver, per-source diagnostics.
- WASAPI `NAE_QueryDeviceCaps`: native format, exclusive rates, layouts, DoP
  carriers; buffer/shared-rate groups remain unprobed rather than guessed.
- `hilbert.cpp`: 255-tap Hilbert FIR, 127-frame latency, tested at 1 kHz.
- `routing.cpp`: complex matrix processor with per-source quadrature and
  direct-path latency alignment; tested standalone and through
  `NAE_OpenFileWithRouting` in the actual render path, including a
  policy-driven Session path (6->2 LtRt), including registered custom
  matrices through `NAE_GetRoutingMatrix`.
- `neiro_core` is a static, platform-neutral target; `policy_check` and
  `hilbert_check` link it. CMake stops after these checks on non-Windows until a
  backend exists.
- `policy_check` / `hilbert_check` are platform-neutral executables.
- Resolver/matrix symbols are exported from `nae.dll`.

## What is not done

- `NAE_QueryDeviceCaps` on macOS/Android; the Windows adapter exists but leaves
  buffer and shared-rate groups unprobed.
- Session policy application is partial: rate/resampler/dither are applied;
  routing/DSD/buffer/channel/clip are reported as WARNING diagnostics and not
  silently applied. The session exposes both requested policy and actual path.
- CoreAudio backend, AVAudioSession lifecycle on iOS, and macOS build verification.
- IIR all-pass Hilbert coefficient generator (FIR is the current working filter).

The next implementation step is the backend interface + WASAPI caps adapter, so
the same resolver can be called by `NAE_Session` on every platform.
