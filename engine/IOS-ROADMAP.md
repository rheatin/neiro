# iOS roadmap (engine side)

Written 2026-11-07, while the UI side checks its NSH spike. This is what *this*
repository has to do for an iOS host; the UI side's list is separate.

The short version: the render path is nearly portable already, and the device
discovery layer is not. That split decides the order.

## What already transfers

Measured, not assumed:

- `output_stream_coreaudio.cpp` makes **zero** HAL calls (`AudioObject*`,
  `kAudioDeviceProperty*`, `AudioDeviceID`). It is an AudioUnit callback feeding a
  ring, and it presents the same "room, window, commit" interface WASAPI does.
- The DSP above the stream (resample, dither, EQ, crossfeed, ReplayGain,
  convolver, clip guard, loudness, position accounting) has no platform code.
- The C ABI. Every export is `noexcept` and owns its own memory, which is what a
  static link needs.
- Base: `rusqlite` (bundled), `lofty`, `ureq` + rustls, `md-5` all build for
  `aarch64-apple-ios`. Its own C ABI has no dynamic-loading dependency.

## What does not

- `backend_coreaudio.cpp` has **115** HAL references: enumeration, caps, hog,
  default-device, rate negotiation. None of it exists on iOS. iOS has one output
  route at a time and no exclusive mode.
- The stream asks for `kAudioUnitSubType_HALOutput`
  (`backend_coreaudio.cpp:937`). iOS needs `kAudioUnitSubType_RemoteIO`.
- `notify` watches folders. On iOS the app sees only what the document picker
  hands it, and a folder that disappears when the app is backgrounded is not
  something to watch.
- FFmpeg is linked as shared libraries. iOS needs static ones, and no `dlopen`.

## Phases

### P0 — prove sound on a device (the only phase that matters for the spike)

1. `output_stream_coreaudio.cpp`: take the AudioUnit subtype from the caller
   (`HALOutput` on macOS, `RemoteIO` on iOS). The ring and the callback are
   already platform-neutral; this is a parameter, not a fork.
2. `backend_ios.cpp` (new): the minimum `AudioBackend`.
   - `enumerate`: one device, the current route, id `"ios-default"`, name from
     `AVAudioSession.currentRoute`.
   - `queryCaps`: rate and channel count from the session, formats from the
     stream's physical format (float32 in practice).
   - `open`: configure the session's `preferredSampleRate` and
     `preferredIOBufferDuration`, then build the stream.
   - No hog. `exclusiveSupported = 0`, and say why in `reason`.
3. A check on hardware that does what `macos_play_check` does: open, play one
   second, read the position back. The simulator is not enough to time it.

Verification: `position 0.000 -> 1.000 s`, and a file at 44.1 kHz on a session at
48 kHz comes back with `resampling = true` and takes one second of wall time.

### P1 — package it

- Two static libraries: `libneiro_core.a` (policy, DSP, decode) and `libnae.a`
  (engine + ABI). One XCFramework for `arm64-apple-ios` and the simulator.
- FFmpeg static, LGPL: `scripts/build-ffmpeg-lgpl-ios.sh` mirroring the macOS
  one, with `--disable-programs` where possible, `--enable-cross-compile`,
  `--target-os=darwin`, `-arch arm64`. The licence gate stays: an unknown
  licence is not a licence.
- Export control: iOS has neither a `.def` nor a version script. A
  `-fvisibility=hidden` build with `__attribute__((visibility("default")))` on
  the `NAE_*` entry points is the equivalent, and `scripts/gen-exports.py` can
  check the list against `nm -gU` as it does on macOS.
- No `dlopen`, no downloaded code, no JIT. That is already true of the engine.

### P2 — the session lifecycle (where iOS differs most)

The host owns `AVAudioSession` (this matches ROADMAP E). The engine needs three
things it cannot ask for itself:

1. **Interruption** (a call, Siri, another app taking audio): the host gets
   `interruptionBegan`/`Ended`. The engine must be told, and it must tell the
   host whether it was playing so the host can decide to resume. Today there is
   no such call in the ABI. Proposal: `NAE_SessionNotifyInterruption(int32_t
   began)` — one export, no struct.
2. **Route change** (headphones out): the device is gone while the handle stays
   valid — the same state as `NAE_ERR_DEVICE_LOST` on Windows. The host should
   pause rather than the engine guess.
3. **Background audio**: `UIBackgroundModes: audio`, and the engine must not
   assume it keeps running for ever; a short render is still a failure, but the
   process may be suspended between slices.

### P3 — Base on the device

- `notify` behind `#[cfg(not(target_os = "ios"))]`. The folder watcher is
  meaningless there; scanning becomes an explicit, chunked operation the host
  drives.
- Library location: `Application Support`, not the sandbox root. Files arrive as
  security-scoped URLs from the picker; Base stores the path, and the *host*
  holds the scope for as long as it uses it.
- A scan of ten thousand tracks on a phone is a minutes-long operation. It needs
  to be resumable and it must report progress (it already reports start/done,
  not percentages — that stays).

### P4 — what iOS cannot do (state it, do not fake it)

- **Bit-perfect is impossible**: RemoteIO is float32. `bit_perfect = 0` with a
  reason, the same answer macOS gives today.
- **No exclusive mode**: `exclusiveSupported = 0`, and no hog.
- **DoP**: needs 24 valid bits and no resampling. RemoteIO does not offer an
  integer format, so DoP is not available; DSD falls back to PCM. Report it.
- **Rate changes are requests**: `preferredSampleRate` is advice. Read back what
  the route granted, and resample. Never report the request as the result.

## An external DAC on iOS: what can be known, what can be done

This is the question that decides how honest the UI can be about a USB DAC, so it
is written down separately from the phases.

### Detection: yes, at the level of "a route changed"

- `AVAudioSession.currentRoute.outputs` gives one entry per active output: its
  `portType`, `portName` and `uid`. A USB DAC (including the Lightning-to-USB
  adapters and most dongles) arrives as `.usbAudio`. Bluetooth is
  `.bluetoothA2DP` / `.bluetoothLE` / `.bluetoothHFP`; the jack is
  `.headphones`; AirPlay is `.airPlay`.
- `AVAudioSession.routeChangeNotification` reports `.newDeviceAvailable`,
  `.oldDeviceUnavailable`, `.categoryChange`, `.override`. That is how a DAC being
  plugged or pulled reaches the app.
- There is **no** device list and **no** format list. iOS will not tell you which
  rates or bit depths the DAC supports, nor let you enumerate several outputs.
  `enumerate()` on iOS is one device, forever.

### Control: the sample rate, as a request

- `setPreferredSampleRate(_:)` asks. Read `AVAudioSession.sampleRate` back to learn
  what the route granted; it can differ, and asking can itself reconfigure the
  route (which arrives as a route change).
- `setPreferredIOBufferDuration(_:)` is likewise a request.
- `setCategory(.playback)` and `setActive(true)` decide whether the app may play at
  all, and which routes it is allowed to use.

### What is refused

- **Bit depth.** The app side of the RemoteIO output is float32. Handing the unit
  integer samples makes the unit convert them; it does not set the DAC's bit
  depth and nothing confirms what the DAC received.
- **Exclusive access.** The system mixer is always in the playback path. There is
  no `kAudioDevicePropertyHogMode` equivalent. Another app playing will mix.
- **DoP.** It needs 24 valid bits with no resampling. There is no integer
  hardware format to ask for, and no way to prove the path stayed untouched.
- **Hardware volume.** `outputVolume` is read-only; the volume buttons belong to
  the system.
- **Vendor commands.** No USB host API for audio class devices. Anything a DAC
  offers beyond playback (filters, gain modes) is not reachable.

### What the engine therefore reports on iOS

```
exclusiveSupported = 0
bitPerfect         = 0, reason = SharedMode
dopSupported       = 0
nativeRate         = session.sampleRate   (what the route granted, not what we asked)
nativeFormat       = f32
```

And the UI may honestly say: "USB DAC · 48 kHz · f32", because that is what the
session reports. It may not say "bit-perfect", and it may not say which rate the
DAC is capable of beyond the one the session is on.

## What the UI side needs to decide, and tell this side

1. Who calls `AVAudioSession.setCategory` / `setActive`, and when. Proposed: the
   Swift host, always, and it never changes the category mid-playback.
2. The interruption contract: does the host want the engine to auto-resume, or
   is "resume" a host decision? Proposed: host decides; the engine only reports.
3. Whether the UI links Base statically (single process, no transport) or keeps
   the unix socket. Proposed: static. The socket exists so an out-of-process host
   can talk to Base; an app does not need that, and iOS makes the IPC pointless.
4. The minimum iOS version. `AVAudioSession` APIs used here are stable since 13;
   anything older changes the route-change notifications.

## Verification on CI

- `xcodebuild` for the framework and `cargo check --target aarch64-apple-ios`
  for Base can run on a macOS runner.
- Playback timing cannot: the simulator's audio clock is not the device's. The
  P0 check has to run on hardware, the way `macos_play_check` does.
- What CI *can* assert is that the build produces the framework, that the export
  list matches `nae.h`, and that the licence gate passes for the iOS FFmpeg.

## Order, and why

P0 then P1, because a spike that proves sound but cannot be shipped is worth
less than one that also packages. P2 before P3: a player that does not pause for
a phone call is broken on iOS in a way it never is on a desktop, and the fix is
one ABI call rather than an architecture.
