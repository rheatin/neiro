# neiro-core

Everything under the UI: the C++ audio engine, its Rust binding and Base.

`neiro` is the app. This repository is the layer beneath it — the only code that
owns a device, a clock, a sample or a measurement. There is no UI here, and none
belongs here; a host draws pixels in its own repository through this C ABI.

## What is here

| Path | What it is |
|---|---|
| `engine/` | The C++ engine: decode, resample, dither, DSP, exclusive/shared output, DSD, transport, telemetry. CMake + MinGW. |
| `binding/` | `neiro-audio`, the Rust binding over the C ABI. It proves the ABI from a non-C++ caller and is how Base reaches the engine. |
| `base/` | `neiro-base`, the composition root: session commands, queue/device policy, analysis scheduling and cache, render-path facts and UI-facing events. It is not an audio engine. |
| `tools/wasapi-probe/` | Hardware measurement of the endpoint itself, independent of the engine. |
| `test/README.md` | What the audio fixtures are and where they go. |

`nae.dll` exports a C ABI; only the `NAE_*` names leave the DLL. Audio samples
never cross the boundary. The host may be C++, Rust or a CLI.

## The session owns the clock

`NAE_Play(milliseconds)` is still the render primitive, but a host no longer has
to drive it. `NAE_SessionOpen` starts a thread that owns the handle, the stream
and the position; `NAE_SessionPlay/Pause/Resume/Seek/Stop` are messages to it,
and `NAE_SessionGetState` is a lock-free snapshot. That removes the "every host
is an audio scheduler" problem.

Decode runs ahead on its own producer thread and fills a one-second source ring.
The render thread owns resampling, gapless/crossfade source switching, the DSP
and the device; the producer never touches the device.

`Base` sits above the session and is what a UI talks to. It sends session
commands, schedules path-based analysis on a worker, owns the analysis cache and
turns `NAE_SessionState` into a `RenderPath` event. It does not create a stream,
drive `NAE_Play`, or infer a DSP stage from the setting the user requested.

## Build

```powershell
cd engine
cmake --preset windows
cmake --build build
```

The preset brings the UCRT toolchain file with it. A bare `cmake -B build -G Ninja`
fails on the UCRT-only MinGW builds, because `cmake_minimum_required()` implicitly
runs `project()`, which compiles the first test program before any flag in
`CMakeLists.txt` can apply — `ld: cannot find -lmsvcrt`. If you must configure by
hand, pass `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-ucrt.cmake`.

Needs MinGW-w64, CMake >= 3.20, and an FFmpeg shared build (found under
`scoop/apps/ffmpeg-shared/current`, override with `-DNEIRO_FFMPEG_ROOT=...`).
soxr is vendored.

The build selects UCRT when the toolchain ships `libucrt.a`; see the comment at
the top of `CMakeLists.txt`. This is what makes the current MinGW-Builds
toolchain link at all.

## CLI

`neiro_cli` links the same DLL. It is a test/diagnostic host, not a second
engine:

```powershell
.\build\neiro_cli.exe devices
.\build\neiro_cli.exe caps
.\build\neiro_cli.exe params
.\build\neiro_cli.exe probe "C:\Music\track.flac" --json
.\build\neiro_cli.exe play  "C:\Music\track.flac" --duration 10
.\build\neiro_cli.exe play  "C:\Music\track.flac" --volume 0.5
.\build\neiro_cli.exe play  "C:\Music\track.flac" --eq 1000:-6:1
.\build\neiro_cli.exe play  "C:\Music\track.flac" --crossfeed --crossfeed-amount 0.4
.\build\neiro_cli.exe play  "C:\Music\track.flac" --replaygain album
.\build\neiro_cli.exe play  "C:\Music\track1.flac" --queue "C:\Music\track2.flac"
.\build\neiro_cli.exe play  "C:\Music\track.flac" --convolver "C:\Music\ir\room.wav"
.\build\neiro_cli.exe play  "C:\Music\track1.flac" --queue "C:\Music\track2.flac" --crossfade 4000
.\build\neiro_cli.exe dop   "C:\Music\track.dff" --device "<id>" --seconds 10
.\build\neiro_cli.exe params --json
# in --commands: 0.5:set volume.gain=0.5; 0.8:get volume.gain
.\build\neiro_cli.exe play  "C:\Music\track.flac" --json `
    --commands "0.5:pause;1.0:status;1.4:seek 30;1.8:resume;3.0:stop"
```

Every setting is also in a name/value registry: `NAE_ParamCount/ParamAt` for
descriptions (type, range, default, flags) and `NAE_SessionParamGet/Set` plus
the string form for `convolver.ir`. `params --json` is the same table. The typed
structures remain the contract; the registry is the discoverable view.

Open-time settings are CLI flags and `NAE_SessionConfig` fields: output mode,
fixed rate, resampler quality, share mode, slice length. Runtime parameters are
`volume.gain` (0..1, unity bypass), the equaliser cascade (`NAE_SessionSetEq`),
stereo crossfeed (`NAE_SessionSetCrossfeed`) and ReplayGain
(`NAE_SessionSetReplayGain`, tags read by the engine). The other DSP blocks do
not exist in this build, and `params` says so rather than listing them.

Paths are converted from the wide Windows command line to UTF-8 before they
reach the engine (`utf8_args.h`). MinGW's narrow `argv` is ANSI, and a CJK path
mangled at that layer looks like an unreadable file.

## Features

- **Output**: WASAPI exclusive and shared; native, match-device and fixed-rate
  modes; three resampler quality tiers; TPDF dither only when narrowing.
- **Formats**: FLAC, WAV, ALAC, AIFF, MP3, AAC, M4A, Ogg/Opus, WMA, APE,
  WavPack, DSD64/128 (DSF and uncompressed DFF).
- **Transport**: play, pause, seek, stop; gapless; crossfade; a queue.
- **Decode**: a producer thread fills a source ring; resampling and DSP stay on
  the render thread.
- **Analysis**: `NAE_AnalyzeFile` is a path-based, device-free plane that returns
  format, full tags, ReplayGain, loudness, dynamic range, waveform peaks and BPM
  with a confidence; the host schedules it and owns the cache.
- **DSP**: digital volume with a 20 ms ramp, up to 32 biquad or linear-phase EQ
  bands, stereo crossfeed, ReplayGain (track/album), a uniform-partitioned FFT
  convolver, a clip guard with a polyphase true-peak detector, BS.1770-4 loudness
  with gating, and read-only spectrum, envelope and peaks taps.
- **DoP**: stereo DSF and uncompressed DFF, at the carrier rate the DAC accepts.
- **API**: a C ABI and a name/value parameter registry; every setting has a
  default and a range. `neiro_cli` is the reference host.

## Limitations

- Windows only for now. `nae.cpp` opens devices through its own WASAPI path and
  includes `windows.h`; the platform-neutral `AudioBackend` interface in
  `engine/audio_backend.h` is implemented by the CoreAudio and AAudio adapters,
  but the engine itself does not route through it yet. Until it does, macOS and
  Android build only the core and its checks, and an iOS host would also need a
  static link instead of `nae.dll`.
- DoP needs an endpoint that opens 176400 Hz (DSD64) or 352800 Hz (DSD128) at
  24 valid bits **without resampling**. An endpoint that coerces the rate is
  refused; it cannot carry DoP.
- DoP is stereo and uncompressed only. Volume, EQ, crossfeed, ReplayGain,
  convolver, crossfade and seek are bypassed on a DoP handle.
- The internal path is float32, which carries 24 significant bits. A source
  deeper than that -- a 32-bit integer WAV, say -- is reported as *not*
  bit-perfect with reason `NAE_REASON_SOURCE_PRECISION`, because the low bits are
  gone before the device sees them, whatever the endpoint accepts.
- The FFmpeg build is GPLv3, which makes `nae.dll` GPL and not distributable.
  The engine **reports** this: `NAE_GetEngineCapabilities` carries
  `ffmpegLicense`, read from `avutil_license()` on the library actually linked,
  so a host can ask the loaded DLL instead of trusting a note.
  `scripts/build-lgpl.ps1` builds and verifies the LGPL variant end to end; on
  this machine it reports `LGPL version 3 or later` and all checks pass.

## Verification

Build, checks, hardware DoP results and measurements are in the developer report:
[`engine/VERIFICATION.md`](engine/VERIFICATION.md).

## License

The engine links FFmpeg (currently a GPL build) and vendored soxr (LGPL 2.1).
See `engine/README.md` before distributing.
