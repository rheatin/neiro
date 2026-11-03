# Neiro

A WASAPI playback engine in C++, behind a C ABI.

The project is the engine. Everything that draws pixels has been removed; the
old UI is preserved in git on `rewrite/gpui` and in the history before
`core/engine`.

## What is here

| Path | What it is |
|---|---|
| `spike/audio-engine/` | The engine: decode, resample, dither, exclusive/shared WASAPI output, DSD, transport, telemetry. CMake + MinGW. |
| `spike/audio-engine/rust/` | An optional Rust binding. It proves the C ABI from a non-C++ caller; it is not on the audio path. |
| `spike/audio-engine/base/` | Base: the composition root. Session commands, queue/device policy, analysis scheduling and cache, render-path facts and UI-facing events. It is not an audio engine. |
| `spike/wasapi-probe/` | Hardware measurement of the endpoint itself. |
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
cd spike/audio-engine
cmake -B build -G Ninja
cmake --build build
```

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
- **DSP**: digital volume with a 20 ms ramp, 16-band biquad EQ, stereo crossfeed,
  ReplayGain (track/album), a direct-FIR convolver, and read-only spectrum,
  envelope and peaks taps.
- **DoP**: stereo DSF and uncompressed DFF, at the carrier rate the DAC accepts.
- **API**: a C ABI and a name/value parameter registry; every setting has a
  default and a range. `neiro_cli` is the reference host.

## Limitations

- Windows only for now. The ABI and CLI are platform-neutral, but the device
  layer is WASAPI; CoreAudio and ALSA are not implemented. `Base` is Rust and is
  intended to cross-compile to iOS; the C++ engine still needs a CoreAudio
  backend and a static (not `nae.dll`) link before an iOS app can ship.
- DoP needs an endpoint that opens 176400 Hz (DSD64) or 352800 Hz (DSD128) at
  24 valid bits **without resampling**. An endpoint that coerces the rate is
  refused; it cannot carry DoP.
- DoP is stereo and uncompressed only. Volume, EQ, crossfeed, ReplayGain,
  convolver, crossfade and seek are bypassed on a DoP handle.
- The convolver is a direct FIR limited to 4096 taps. Longer room IRs need
  partitioned FFT convolution.
- The internal path is float32, so a true 32-bit integer source on a 32-bit
  endpoint is not bit-perfect even though the current flag does not compare the
  source width.
- The FFmpeg build is GPL. That is fine for development and not fine to
  distribute; an LGPL build is a link-time change.

## Verification

Build, checks, hardware DoP results and measurements are in the developer report:
[`spike/audio-engine/VERIFICATION.md`](spike/audio-engine/VERIFICATION.md).

## License

The engine links FFmpeg (currently a GPL build) and vendored soxr (LGPL 2.1).
See `spike/audio-engine/README.md` before distributing.
