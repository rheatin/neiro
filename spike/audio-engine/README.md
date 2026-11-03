# nae — Neiro audio engine

A WASAPI exclusive-mode audio engine behind a C ABI. Decoding, rate conversion,
dithering, requantisation and device output all happen here, so audio samples
never cross a language boundary.

C++ (MinGW) → `nae.dll`. Rust bindings in [`rust/`](rust/). vended `soxr` in
[`third_party/soxr`](third_party/soxr).

> **Why failures are written down separately.** `LESSONS.md` records the bugs
> found while building this and the measurements that caught them. `RESAMPLING.md`
> covers three earlier resampler bugs. The README describes the engine as it is;
> those describe how it got that way.

## What it does

Exclusive WASAPI output — the endpoint is opened in exclusive share mode and
reformatted to whatever the source needs, rather than asking Windows to
resample. Verified working on a 192 kHz / 24-valid-in-32 endpoint.

| | |
|---|---|
| Decode | FLAC, WAV, ALAC, AIFF, MP3, AAC, M4A, Ogg/Opus, WMA, APE, WavPack, **DSD64 (DSF, DFF)** |
| Rate | any rational source → device, via `soxr` at three quality tiers |
| Depth | float internally; 16/24/32-bit integer or float out, dithered only when the output is narrower than the internal precision |
| Channels | libswresample handles mono, stereo, 5.1 → stereo |
| Output | three modes: follow the file, match the device, or a fixed rate |

A 5.1 file at 48 kHz on a 48 kHz stereo endpoint plays without resampling and
**without claiming to be bit-perfect**, because a channel conversion is processing
too. The `bit_perfect` flag is the single source for the UI's badge, so it is
deliberately conservative.

## Build

Needs MinGW-w64 and CMake ≥ 4, plus an FFmpeg shared build.

```powershell
cmake -B build -G Ninja
cmake --build build
```

FFmpeg is found at `$USERPROFILE/scoop/apps/ffmpeg-shared/current` by default;
override with `-DNEIRO_FFMPEG_ROOT=...`. The engine still builds without it —
decoding is compiled out and the test tone still works.

### Licensing — read this before shipping

The current FFmpeg build (`ffmpeg-shared`, `full_build`) is **GPL**. Linking it
makes `nae.dll` GPL. That is fine for development and **not fine to distribute**.

An LGPL build differs only in which optional components are compiled in, and this
player needs none of the GPL ones — no x264, no xvid. The swap is a link-time
change and nothing above it moves. `soxr` is vended and LGPL 2.1.

## Exports

The authoritative list is `nae.map` (GNU ld) and `nae.def` (MSVC), with
`local: *` so nothing else is public. It covers device enumeration and probing,
file open, the render primitives, the query/telemetry surface, transport
(`NAE_Stop/Pause/Resume`), digital volume (`NAE_SetVolume/GetVolume`), the EQ
(`NAE_EqDefaults/SetEq/GetEq`), crossfeed
(`NAE_CrossfeedDefaults/SetCrossfeed/GetCrossfeed`), ReplayGain
(`NAE_ReplayGainDefaults/QueryReplayGain/SetReplayGain/GetReplayGain`),
gapless (`NAE_PreloadNext/ClearNext/GetTrackSerial`), the convolver
(`NAE_ConvolverDefaults/SetConvolver/GetConvolver`), crossfade
(`NAE_CrossfadeDefaults/SetCrossfade/GetCrossfade`), DoP
(`NAE_OpenDop/OpenDopDownrated/PlayDop`), native DSD through a host-owned
device (`NAE_OpenNativeDsdSink/PlayNativeDsd`), the parameter registry
(`NAE_ParamCount/ParamAt/SessionParamGet/Set`) and the session API
(`NAE_SessionOpen/Play/Pause/Resume/Seek/Stop/GetState/SetVolume/GetVolume/LastError/Close`).

**A function missing from that list is not a link error** — it compiles, links,
and simply is not in the export table, so the host fails to resolve it at load.
A new entry point that "is not there" means the map first.

CMake declares both files as `LINK_DEPENDS`, so editing them relinks the DLL. If
`nae.dll` is held by a running process the link fails; stop it first.

### Ownership rules

From `nae.h`, and the only two things to get right:

* **Strings** the engine returns are `malloc`'d by the engine and must be released
  with `NAE_FreeString` — never with the host's deallocator.
* **Handles** are owned by the caller and released with `NAE_Close`, exactly once.

## Behaviour worth knowing before reading the code

**A null device id means the system default**, resolved at open time rather than
from a value the UI captured earlier — a user can plug in headphones between
choosing a device and pressing play.

**Errors carry the engine's own message**, not only a code, via
`NAE_GetLastErrorText`. A missing file and an unreadable one produce different
text, because a listener needs to know which.

**Slicing is the contract.** The host calls `NAE_Play` repeatedly with a time
budget; the source position carries across calls. One call does not play a whole
track.

**A short render is a failure** — `NAE_Play` errors rather than succeeding over a
render that stopped early. The exception is a track ending mid-slice, where the
remaining budget cannot be filled by anything.

**DoP can downrate the bitstream.** `NAE_OpenDopDownrated` decimates DSD256/
DSD128 to a lower rate with a filtered 1-bit path and an error-feedback
requantiser, then packs DoP at the lower carrier. Taking every D-th bit without
the filter would fold the shaped noise into the audio band; `dsd_downrate_check`
measures the tone and the in-band floor against a native encoding.

**The equaliser has two modes.** `phase = NAE_EQ_PHASE_MINIMUM` runs the RBJ
biquad cascade per sample, as before. `NAE_EQ_PHASE_LINEAR` builds one symmetric
FIR per channel from that channel's bands and convolves with the partitioned
FFT: constant group delay, at the cost of `taps/2 + 256` frames of latency
(1280 at 48 kHz, 2304 at 192 kHz; the FIR length follows the rate to keep roughly
50 Hz of bin spacing). A band carries a `channelMask`, so a per-channel curve is
built from bands that name their channels; zero means every channel. Both modes
read the same mask field.

## Decode thread

Decoding runs on a producer thread that fills a one-second source ring. The
render thread consumes blocks, and still owns resampling, the gapless/crossfade
source switching, the DSP and the device; the producer never touches the device.
If the producer cannot start, the render thread falls back to calling
`readExact` inline. `NAE_PipelineStats` reports `decodeThread`.

## Analysis plane

`NAE_AnalyzeFile(path, buckets, ...)` decodes the file once and returns format,
full tags (title, artist, album, album artist, track, disc, year, genre,
composer, comment), ReplayGain, loudness, dynamic range, peaks and BPM with a
confidence. It opens no device and touches no playback handle; the host runs it
on a worker and owns the cache. `analyzerVersion` is part of the cache key.

`lufsIntegrated` is ITU-R BS.1770-4 / EBU R128: K-weighting designed for the
file's own rate, 400 ms blocks at 100 ms hop, absolute gate at -70 LUFS and
relative gate at -10 LU. `lufsMomentaryMax` / `lufsShortTermMax` are ungated
maxima; `lufsRange` is EBU Tech 3342. Cross-checked against FFmpeg's `ebur128`
on a real FLAC: -10.22 vs -10.2 LUFS, LRA 4.45 vs 4.6 LU. `lufsValid == 0`
means the file was too short (under 400 ms), not that it is silent.

`loudness_db` remains the median short-window RMS and `dynamic_range_db` the
p95−p10 spread; they are a different, cheaper estimate and are not LUFS.
`bpmValid` is 0 when the onset envelope is too short or the autocorrelation peak
is too weak; the host should then show nothing.

## Output policy (proposed)

[`OUTPUT-POLICY.md`](OUTPUT-POLICY.md) and [`nae_policy.h`](nae_policy.h) define
the requested `NAE_OutputPolicy`, measured `NAE_DeviceCaps`, and the
`NAE_ResolvedSignalPath` with per-source diagnostics. `policy.cpp` now implements
defaults, matrix build/registration, the pure resolver and Auto/fallback/failure
diagnostics; `hilbert.cpp` implements the 255-tap LtRt Hilbert FIR. The resolver
is not wired into the session yet and `NAE_QueryDeviceCaps` is still pending.
The contract covers Auto rate/resampler/dither/DSD/buffer/routing, channel
trim/phase, clip guard, bit-perfect reasons and no-silent-fallback rules.
Platform coverage is the current priority. The build sequence, accepted scope and
parked features are in [`ROADMAP.md`](ROADMAP.md); backend architecture is in
[`CROSS-PLATFORM.md`](CROSS-PLATFORM.md).

## Known gaps

- **DoP is verified on one endpoint.** `扬声器 (WALKMAN)` accepts 24-valid-bit
  carriers at 176400 and 352800: DFF DSD64, DSF DSD64 and DSF DSD128 play on
  both channels; the DAC displays DSD 2.8 MHz and 5.6 MHz. Other endpoints need
  `dop_probe`; a coerced carrier is refused. Stereo and uncompressed DSD only.
- **The convolver is a 4096-tap uniform-partitioned FFT convolution**, with one
  block of latency the dry path is delayed to match, and one state set per
  channel (a 5.1 stream convolves all six; a mono IR applies to every channel,
  any other IR channel count is rejected). Longer IRs need more partitions (the
  core supports up to 32) and a larger `NAE_CONVOLVER_MAX_TAPS`.
- **The internal format is float32.** 16/24-bit integer sources round-trip
  exactly; a true 32-bit integer source does not, and the bit-perfect flag does
  not compare the source width.
- **Windows only for now.** The device layer is WASAPI; the C ABI and CLI are
  platform-neutral. CoreAudio and ALSA are not implemented.

The detailed feature notes (volume/EQ/crossfeed/ReplayGain/gapless/crossfade
semantics) are in the root README and in `VERIFICATION.md`.

## When the output device disappears

An endpoint that is unplugged, disabled, or taken by another application in
exclusive mode makes the render loop fail with `AUDCLNT_E_DEVICE_INVALIDATED`
(0x88890008) or one of its two siblings. Those return **`NAE_ERR_DEVICE_LOST`**
(10) / `EngineError::DeviceLost`, which is deliberately not the generic
`NAE_ERR_INITIALIZE`:

- The stream **pauses**, it does not fail. The file, the decoder and the position
  are all still valid.
- The host sends the exact position once before dropping the stream, so a retry
  has a resume point better than the last periodic progress tick.
- Any work already queued is discarded, except shutdown — see below.

**No device is chosen on the caller's behalf**, and this is the property that
took three attempts to get right. It is stated as an invariant because the first
version of it was simply false:

> After any successful open, a play request carrying no device id can only ever
> land on the endpoint used last. If that endpoint is gone the result is
> `NAE_ERR_DEVICE_LOST` — never a different device.

`None` used to mean "the Windows default", which is not a neutral choice. When a
hardware endpoint is disabled Windows reassigns the default, so a queued play
carrying `None` opened whatever became the fallback. On this machine that was a
virtual audio device at 48 kHz, 16-bit, dithered, and not bit-perfect: the
listener was moved off their DAC onto a worse path, with nothing said. Draining
the queue does not close that hole, because a play can be queued *after* the
drain and still carry `None`. Only redefining `None` closes it regardless of
timing. A first launch has nothing remembered and legitimately uses the default.

The queue drain is still there, and it is selective: play requests and a pending
seek are dropped, **shutdown is not**. Shutdown has to survive — the audio thread
only exits when it receives one, so discarding it leaves the thread blocked
forever and the application hangs on quit.

### Every HRESULT in the render loop, and what happens when the device is gone

Written by enumerating the call sites rather than from memory, because the
`GetCurrentPadding` omission was found by running the test after three passes of
reasoning had listed the two calls that *write* audio and skipped the one that
*measures* the buffer.

| Call | When | Fails on device loss | Handling |
|---|---|---|---|
| `GetCurrentPadding` | top of every slice | **yes — first to fail** | `deviceIsGone` → `NAE_ERR_DEVICE_LOST` |
| `IAudioRenderClient::GetBuffer` | every slice | yes | `deviceIsGone` → same |
| `IAudioRenderClient::ReleaseBuffer` | every slice | yes | `deviceIsGone` → same |
| `IAudioClient::Start` | once, at open | yes, in a narrow race | `deviceIsGone` → same |
| `IAudioClient::Stop` | end of every `NAE_Play` | yes | result ignored; the device-lost message is deliberately **not** overwritten by it |
| `IAudioClient::Initialize` | open | no | open error |
| `IAudioClient::GetService` | open | no | open error |
| `IAudioClient::GetBufferSize` | open | no | open error, with fallback |
| `IMMDeviceEnumerator::GetDefaultAudioEndpoint` | enumerate | n/a | open error |
| `IMMDevice::Activate` / `OpenPropertyStore` / `CoInitializeEx` | open, probe | no | open error / treated as absent |

The three in the "yes" rows at the top of the loop are the ones that can fire
during playback, and all three are handled identically. Anything that can only
fail while opening a stream cannot be a mid-playback device loss, so it keeps the
generic open error.

That position is the engine's `sourcePos` at the start of the last successful
slice. It leads the FIFO and the device buffer (10–60 ms combined), so relative
to what the listener actually last heard it may be **up to one slice (200 ms)
early**, or a few tens of ms late, depending on when in the slice the device went.
Early repeats a short passage; late drops one. Repeating is the less noticeable
of the two. This is not corrected — asking a dead endpoint how much is queued in
it is not possible, so the only way to be exact would be a worse approximation.

Recovery reopens the stream on the new device and **seeks to that position**,
reusing the tested seek path rather than adding a second one. The cost is that
the decoder is rebuilt: retrying a 682 MB DSD256 file reopens the file, which is
a visible delay. The alternative is rebinding the endpoint inside a live handle,
which is new COM code in the one place a mistake shows up as a crash.

`NAE_ERR_DEVICE_LOST` is also what the host switches on, by numeric code, so the
frontend never has to match on the message text.

## Verification

A successful build is not verification. The check list, hardware DoP results,
latency and sustained-playback measurements, and the defects the hardware found
are in [VERIFICATION.md](VERIFICATION.md). That file is the developer record;
this README describes the engine.

## Note on `/DEF:` in CMake

A `LINK_FLAGS "/DEF:path"` gets mangled into a filesystem path by CMake on
Windows and the link fails with `ld: cannot find /DEF:...`. Use
`target_link_options(nae PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/nae.def")`.
