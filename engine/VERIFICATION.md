# Verification report

Developer document. `README.md` is for users; this file records what was
actually run, on which hardware, and what it proved. A result that was not
observed here is marked as not covered rather than assumed.

## Environment

| | |
|---|---|
| OS | Windows 10.0.26300 |
| Compiler | MinGW-Builds 16.2.0-rt_v14 (UCRT) |
| CMake / Ninja | 4.4.5 |
| Decoder | FFmpeg shared build (GPL) |
| Resampler | vendored soxr 0.1.3 (LGPL) |
| Endpoints used | `LG TV SSCR2` 192 kHz / 24-in-32; `扬声器 (WALKMAN)` 48 kHz native, DoP-capable |

The engine is built with `-mcrtdll=ucrt` when the toolchain ships
`libucrt.a`; without it, `libwinpthread` fails to link on
`__intrinsic_setjmpex`. This is a toolchain property, not an engine option.

## Build

```powershell
cd engine
cmake -B build -G Ninja
cmake --build build
```

Current state: all targets build. The Rust binding is the cross-language ABI
probe:

```powershell
cd rust
$env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
cargo run --bin probe -- <file.flac>
```

## Check matrix

All commands run from `build/`. Latest result: **all pass**.

| Check | What it measures | Result |
|---|---|---|
| `nae_test.exe` | device, native format, latency, capabilities, concurrent open, shutdown | PASS |
| `resample_check.exe` | rates, ratios, block boundaries, frame drift | PASS |
| `dither_check.exe` | TPDF/Rectangular/Off, quantisation levels, container placement | PASS |
| `mode_check.exe` | native / device / fixed output modes | PASS |
| `reopen_check.exe` | exclusive open/close cycles | PASS, 12/12 |
| `session_check.exe <file>` | play/pause/seek/stop through the session thread; volume, EQ, crossfeed, ReplayGain, state, resolved path + diagnostics | PASS |
| `decode_thread_check.exe <file>` | producer active, render thread still advances | PASS |
| `analysis_check.exe [tagged.flac]` | self-generated 120 BPM click, loudness/DR/peaks; optional full tags/ReplayGain | PASS |
| `policy_check.exe` | output policy: native/coerced rate, DSD DoP, LtRt matrix, registry, failure and diagnostics | PASS |
| `session_policy_check.exe <file>` | policy-driven session: requested/actual rate, dither, unsupported-field warnings | PASS |
| `channel_check.exe` | channel trim/balance/polarity/delay API validation | PASS |
| `buffer_policy_check.exe <file>` | Auto/LowLatency/Fixed buffer open and reported latency | PASS |
| `bass_check.exe` | LFE low-pass: 40 Hz passes, 5 kHz rejected | PASS |
| `partitioned_convolver_check.exe` | FFT convolution matches direct FIR; 1, 2 and 6 channels; identity channel; mono-IR form | PASS |
| `loudness_check.exe` | BS.1770 calibration, both gates, momentary/short-term/LRA, per-rate filters | PASS |
| `linear_phase_eq_check.exe` | symmetric impulse response, constant group delay, per-channel mask, identity delay | PASS |
| `hilbert_check.exe` | FIR and IIR quadrature, phase error over 20 Hz - 20 kHz, LtRt matrix fields | PASS |
| `caps_check.exe [device]` | native format/rates/DoP and the probed native-DSD answer (0 on WASAPI) | PASS |
| `native_dsd_sink_check.exe <file.dsf\|dff>` | host sink receives the file bit for bit; downrate and host errors | PASS |
| `dsd_downrate_check.exe` | DSD128 to DSD64 keeps the tone and the in-band noise floor | PASS |
| `clip_guard_check.exe` | limiter holds 2.0 signal under -1 dBFS; true-peak vs sample-peak control; rejects bad settings | PASS |
| `hilbert_check.exe` | 255-tap Hilbert amplitude/quadrature at 1 kHz; LtRt matrix fields | PASS |
| `routing_check.exe` | complex 5.1 LtRt processor: quadrature, direct gain, 127-frame latency | PASS |
| `routing_play_check.exe` | routed open/play: source stays 6ch, device 2ch, LtRt path, non-bit-perfect | PASS |
| `session_routing_check.exe` | policy-driven Session builds/applies LtRt; 6->2; no routing warning | PASS |
| `session_dsd_check.exe <dsd>` | DSD policy: DoP attempt, PCM fallback, extract, native API/path | PASS |
| `session_policy_check.exe <file>` | policy-driven session: rate/dither, unsupported warnings | PASS |
| `caps_check.exe` | WASAPI `NAE_QueryDeviceCaps`: native format, exclusive rates, layouts, export | PASS |
| `backend_check.exe` | backend interface: enumerate, caps, WASAPI adapter | PASS |
| `base_probe.exe` | Base snapshot carries engine resolved path/diagnostics; per-track refresh | PASS |
| `volume_check.exe` | unity vs 0.5 on a constant tone, reason 8 | PASS, ratio 0.500000 |
| `eq_check.exe` | −12 dB peaking band at the tone's bin, reason 9 | PASS, ratio 0.251189 |
| `crossfeed_check.exe` | right-only tone measured in the left, reason 10 | PASS, 0 → 0.229777 → 0 |
| `replaygain_check.exe <tagged.flac>` | track −6 dB / album −3 dB tags, reason 11 | PASS, 0.501187 / 0.707946 |
| `gapless_check.exe <a> <b>` | same-format seam, underrun and padded-frame counters | PASS, 0 → 0 |
| `convolver_check.exe` | 0.5 delta IR, reason 12 | PASS, ratio 0.499969 |
| `crossfade_check.exe <a> <b>` | both tones mid-fade, next position, counters | PASS |
| `param_check.exe <file>` | registry: 107 entries, typed round-trip, open-time refusal | PASS |
| `dop_check.exe <file.dff\|dsf>` | markers, payload bit order, carrier, downrate packing, no device | PASS |
| `dsd_filter_check.exe` | two-stage decimation against a whole-block reference; passband/stopband | PASS |
| `latency_bench.exe <file> --label NAME --seconds 10` | FIFO depth, latency contribution, CPU, drift | see Measurements |
| `sustain_check.exe` | long-run FIFO and drift, not a summary | see Measurements |

The Rust ABI probe (`cargo run --bin probe -- <file.flac>`) also passes: device
enumeration, native/device/fixed modes, bit-perfect flags, decode path, stream
ownership, shutdown.

## Output policy, diagnostics and LtRt

`policy_check` is pure: no device is opened. It uses synthetic caps and checks
the resolver and diagnostics:

| Check | Result |
|---|---|
| 44.1 kHz native | PASS, bit-perfect |
| 88.2 kHz Auto | PASS, coerced to the 192 kHz device native rate; resampling and coercion diagnostics |
| DSD64 + DoP | PASS, 176400 carrier, DSD stays DSD, bit-perfect |
| LtRt 5.1 -> 2.0 | PASS, phase stage active, matrix has 0.7071 quadrature, not bit-perfect |
| unsupported Fixed rate | PASS, unresolved with an ERROR diagnostic |
| per-source diagnostics | PASS, Auto/fallback entries emitted |
| DSD without carrier | PASS, PCM fallback marked and opted out only when denied |
| routing registry | PASS, register/readback/release, LtRt phase fields preserved |

`hilbert_check` measures the implemented FIR at 1 kHz: 255 taps, 127-frame
latency, amplitude 0.99986, phase -90.00 degrees against the delayed input.
`routing_check` applies the same matrix to a 5.1 signal: Lt/Rt carry the
0.7071 quadrature surround, the direct left path stays 0.5, and the phase
latency is 127 frames.

`session_dsd_check` opens a DFF through the policy session. On this endpoint
DoP is unavailable, so it records the PCM fallback and its diagnostic.
`dsdRatePolicy=EXTRACT` takes the PCM path and reports `dsdExtracted` with an
extract diagnostic; `DOWNRATE` is reported as not implemented rather than
silently ignored. The native path calls `NAE_OpenNativeDsd`; the WASAPI backend returns
`NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT`, and without fallback the Session fails
rather than silently playing PCM.

`session_routing_check` opens a 5.1 WAV through
`NAE_SessionOpenWithPolicy` with `routingMode=DOWNMIX`,
`downmixPreset=LTRT`; the session builds the matrix, opens with
`NAE_OpenFileWithRouting`, reports 6->2 in the resolved path, and does not emit
the routing warning. `routing_play_check` opens the same matrix directly. The decoder keeps the file's
6 channels, `NAE_Play` renders through `RoutingProcessor` before the resampler,
and the device receives 2 channels. The path reports non-bit-perfect.
Crossfade/preload are rejected while routing is active.
`NAE_SetDitherMode`/`NAE_GetDitherMode` expose Off, TPDF and Rectangular;
Noise-shaped is rejected by the C ABI as not implemented. `dither_check` measures
Off vs TPDF vs Rectangular on a half-LSB signal.

The FIR has the expected Hilbert low-frequency roll-off; a longer FIR or the
future IIR all-pass mode is a tuning choice, not an open design question.

The resolver, matrix and caps symbols are exported from `nae.dll`
(`NAE_OutputPolicyDefaults`, `NAE_ResolveSignalPath`, `NAE_BuildRoutingMatrix`,
`NAE_RegisterRoutingMatrix`, `NAE_ReleaseRoutingMatrix`, `NAE_QueryDeviceCaps`).
The Windows caps adapter reports native/rates/layouts/DoP; buffer and
shared-rate groups are explicitly unprobed. Session/Base wiring and CoreAudio
are the next implementation steps; see [`CROSS-PLATFORM.md`](CROSS-PLATFORM.md).

## Resolved path through Session and Base

`NAE_SessionGetResolvedSignalPath` and
`NAE_SessionGetResolvedSignalPathJson` expose the actual path for the current
track, refreshed after open and after a gapless track change. It includes the
actual rate/resampler/dither/channel conversion/DSP flags and diagnostics; it is
not a policy preview.

Base polls that JSON on open and track change, stores it in `Snapshot.resolved`,
and emits `ResolvedPathChanged`. `base_probe` verifies `sampleRate` and a
non-empty diagnostics array from the engine, not from Base inference.

## Bass management

`RoutingProcessor` derives the LFE from a low-passed sum of the routed channels
when `bassManagement=ON`. Measured: a 40 Hz tone reaches LFE at 0.431 peak; a
5 kHz tone is rejected (0.008). Session policy carries bass settings into the
routing matrix and the resolved path reports them.

## IIR Hilbert

`IirQuadrature` is a frequency-warped parallel all-pass pair: six first-order
sections per path, both paths all-pass (measured amplitude 1.0000 at 1 kHz), and
the phase difference is -89.63 degrees at 1 kHz and within 0.80 degrees over
20 Hz to 20 kHz at 48 kHz. The pole set was designed once against 20 Hz to
min(20 kHz, 0.42*fs) and re-derived per sample rate by the bilinear transform;
an offline sweep measured 0.586 degrees worst-case at 32, 44.1, 48, 88.2, 96,
176.4 and 192 kHz.

LtRt now defaults to this mode: the 255-tap FIR was measured 21 dB off in
amplitude at 20 Hz (its useful band starts near 190 Hz), so it is not a
defensible default for a downmix that must be correct at the bottom of the
band. The FIR remains selectable by setting `phaseMode/phaseTaps` on the
matrix; `routing_check` still verifies its 127-sample group delay.

The IIR pair filters both paths, so `alignPhaseLatency` is ignored in this mode:
delaying the direct path would break the quadrature relation.

## Audit fixes (red-team pass, blue-team verdicts)

Each finding below has a probe that fails without its fix:

- **Convolver ran two channels only.** `nch = min(channels, 2)` left 3..6 dry;
  the call site wrote only channels 0 and 1 and the IR decoder folded any
  multichannel IR to L/R. Now per-channel state, a per-channel IR list, and a
  reject for an IR whose channel count is neither 1 nor the stream.
  Measured: 6 channels at 0.25 with a 0.5-gain IR (was 0.5 on 3..5), and with
  distinct per-channel IRs each channel uses its own (0.5 / 0.25 / 0.5).
  An empty per-channel entry is an identity whose delta sits at tap 0, so its
  onset lands at the same sample as a filtered channel: onset 266 / 266 / 266.
  Moving the delta to `B/2` makes the probe fail (266 / 394 / 266), which is
  the check that the probe measures position and not just level.
- **A delay equal to the ring length applied no delay.** The ring is now
  `maxDelay + 1`; `channel_check` opens a sine stream, sets the maximum delay
  and reads `channelDelayMaxFrames` back from the engine (48001). Sizing the
  ring at `maxDelay` makes it fail with 48000.
- **Latency: one value in three fields, and a 1000x unit error.**
  `session.cpp` divided a millisecond value by 1000 and copied it into buffer,
  driver and total. Now the three come from `NAE_QuerySignalPath`, no division,
  and a refresh runs after every DSP apply. Measured at 192 kHz: open 68.050 ms
  (buffer 58.050 + driver 10.000), linear-phase EQ 97.075, EQ off 68.050.
- **Render loop allocated per device buffer.** `held`, `frameOut`,
  `frameFloat` and the error string are handle members now.
- **Base's event bus was unbounded.** `sync_channel(64)`: position uses
  `try_send` (newest dropped when behind), control events stay reliable.
- **The rate cache was mutated without a lock.** Mutex around all three probe
  sites (the open path, the caps query and the reader); the resolver reads a
  copy, never the shared vector.
- **A sink handle answered all zeros.** `NAE_QuerySignalPath` has a native-DSD
  branch reporting the stream rate, 2 channels, 1 valid bit.
- **Linear-phase EQ refused >8 channels with a sample-rate message.** The cap
  is gone (16 channels measured), and the message names all three causes.
- **EQ q was clamped**, the Blackman window was computed three times, and four
  dead members existed. Removed; the window is computed once.
- **Two 100 KB executables were tracked** while `.gitignore` already covered
  them. Untracked.
- **`NAE_QuerySignalPath` wrote its own full size into the caller's buffer.**
  A host built against an older, smaller header had its stack overrun by the
  fields appended since. The engine now reads the caller's `structSize` first
  and copies out only that many bytes. Measured: an old 676-byte buffer followed
  by canaries keeps them intact (`structSize` reports 688); a full memset of the
  same layout clobbers them, so the canary can fail. `channel_check` now runs
  that case: declaring 676 bytes leaves the canaries intact and reports 688,
  and restoring the unconditional memset fails it with "canaries CLOBBERED".

## Review fixes (blue-team pass)

The independent review found real defects; each now has a probe that fails
without its fix:

- Routing's FIR alignment ring was written inside the target loop, so only
  target 0 was delayed. `routing_check` feeds one source into two targets and
  requires both impulse peaks at 127.
- The clip guard released before the offending sample left the lookahead.
  It now holds through the lookahead; a 2.0 impulse with 50 ms lookahead and
  1 ms release measures 0.891251 at the output.
- `RoutingProcessor::reset` left the LFE one-pole state running; a reset then
  silence measures exactly 0. `ClipGuard::reset` now clears its interpolation
  history, so a fresh quiet stream is not attenuated by pre-reset peaks.
- An even FIR Hilbert length used to leave the quadrature path silent with a
  success return; `routing_check` requires the rejection.
- Loudness weights every surround channel at 1.41, not only indices 4 and 5;
  BC/SL/SR now read the same as BL.
- The routing matrix's sample rate was stamped from the endpoint's native rate;
  the engine now overrides it with the negotiated rate, so the IIR pole set and
  the LFE crossover use the rate that actually runs.
- EQ q is no longer clamped; an extreme but positive q is used as given and a
  non-positive one is rejected.
- `totalLatencyMs` now includes linear-phase EQ, convolver and clip-guard delay
  (12.0 ms measured for the linear EQ at 192 kHz).
- The FIR Hilbert check pins the sign to -90 degrees, not only the magnitude,
  so a polarity flip cannot pass.

## Native DSD sink

`NAE_OpenNativeDsdSink` hands interleaved 1-bit bytes to a host callback, with
the file's own bit order reported. `native_dsd_sink_check` captures the bytes
and compares them with the container: 1417216 of 1417216 bytes identical for a
DSD128 fixture, position in seconds of DSD, pause stops the flow, and a host
write returning 77 makes `NAE_PlayNativeDsd` return 77 unchanged. The same
fixture asked for DSD64 produces exactly the bytes the downrate module produces
offline (708608), so the sink is not a second implementation.

WASAPI itself still cannot carry native DSD (no 1-bit format); the capability
probe reports that, and the sink is the engine-side path a vendor backend
plugs into.

## DSD bitstream downrate

`dsd_downrate_check` encodes the same 1 kHz tone at DSD128 and DSD64, decimates
the DSD128 stream to DSD64 and decodes all three through the existing cascade:
tone amplitude 0.4995 (downrated) vs 0.4998 (native DSD64) vs 0.5001 (source),
and the in-band residual is within 0.42 dB of the native DSD64 encoding. Taking
every second bit without the filter would fold the shaped noise into the audio
band; factor 1 is byte-identical, and non-dividing/upsampling targets are
rejected.

The DoP path carries it: `NAE_OpenDopDownrated` opens the carrier at
target/16 and feeds the decimated bytes to the packer (`dop_check` verifies the
packed word payload is the decimated stream). Session chooses the highest lower
rate whose carrier the device lists and reports `dsdDownrated` plus
`DSD_DOWNRATED`; the "not implemented" warning is gone.

**Not verified on this machine:** neither endpoint accepts a DoP carrier
(`dop_probe`: 176400 opens with rc=2), so the end-to-end DoP read path could
not be played here. The DSP and the packer are verified device-free; session
falls back to PCM and reports it.

## Equaliser

The array is 32 bands; the linear-phase cost does not grow with the count, and
`eq_check` accepts all 32 and rejects 33.

`eq_check` still measures the minimum-phase cascade (a -12 dB band at the tone's
bin: ratio 0.251189) and now also the linear-phase path through the same C ABI
and the same band: RMS ratio 0.2608 against 0.2512 for the biquads, with the
difference being the FIR's window and the rate-dependent tap count. A band with
`channelMask = 1` moves the measured (left) channel on both paths.

`linear_phase_eq_check` proves the phase claim directly: the impulse response
peaks at the reported latency and is symmetric to within 1e-6, and the measured
group delay is the same at 500 Hz and 4 kHz (1280 samples). A channel with no
bands is a pure delay at the same latency, so per-channel settings cannot
misalign the image. A band above Nyquist is rejected.

The FIR length follows the sample rate because a fixed 2048 taps was measured
1.26 dB off at 192 kHz: 42.7 ms at 48 kHz but only 10.7 ms at 192 kHz.

## Loudness (BS.1770-4 / EBU R128)

`loudness_check` measures the standard's own calibration points: one full-scale
997 Hz channel reads -3.01 LUFS; two identical -23 dBFS channels read -23.0
LUFS; 44.1 kHz and 48 kHz agree within 0.01 LU because the filters are designed
for the actual rate. Gating is shown to be load-bearing: a tone followed by
digital silence reads -20.07 instead of the ungated -23.0, and a -25 LU section
is removed by the relative gate (2.93 LU above the ungated reading). A -6 LU
section is inside the gate and the measured mean matches the arithmetic within
0.001 LU. LRA of a 20 LU step reads 20.0 LU. Under 400 ms of input reports
`lufsValid = 0`, not 0 LUFS.

A file shorter than 3 s has no short-term window, so `lufsShortTermMax` and
`lufsRange` are 0 and `lufsShortTermValid` is 0. The value used to stay at its
`-200.0` init while `lufsValid` was 1, which reads as a measurement of -200
LUFS. Measured: a 1 s tone reports `shortTermValid = 0` and 0.000000; a 4 s
tone reports -19.999986 with the flag set.

Cross-implementation check on a real FLAC (297 s, loud pop) against FFmpeg's
`ebur128` filter: integrated -10.22 vs -10.2 LUFS, LRA 4.45 vs 4.6 LU. The same
file through `peaks_check` reports a stable value across two calls (bit-identical)
and the momentary maximum matches FFmpeg's M max at -6.40 LUFS.

## Partitioned convolver

The convolver stage is a uniform-partitioned FFT convolution: 256-sample
blocks, 512-point FFT, up to 16 partitions (4096 taps). Measured against a
direct FIR on random signals: identical output after the one-block latency
(difference 0.000000 for a 1-tap, a 1000-tap and a 700-tap stereo IR, and the
same after reset()). The
engine-level `convolver_check` still measures the 0.5 delta IR halving the tone
(ratio 0.499969), so the C ABI path uses the same implementation.

The wet path is delayed by one block; the dry path is delayed by the same
amount so a wet/dry mix does not comb-filter.

## Buffer policy

`NAE_OpenFileWithBuffer` maps Auto/Stable to the backend default, LowLatency to a
10 ms request, and Fixed to the requested duration. A refused explicit request
falls back to the backend default with a message. Measured on this endpoint:

| Policy | Reported latency |
|---|---|
| Auto | 50.667 ms |
| LowLatency | 10.667 ms |
| Fixed 50 ms | 50.667 ms |

The Session applies `effectivePolicy.bufferMode/bufferMs` and reports the mode
in the resolved path.

## Clip guard

`clip_guard_check` drives a 2.0-amplitude tone through the limiter: the output
peak stays at 0.891 (-1 dBFS ceiling) and gain reduction is reported.

True peak is measured on a 12 kHz sine sampled at 45 degrees, amplitude 1.10:
its samples sit at 0.778 (under the ceiling) while the waveform between them
reaches 1.10. A sample-peak limiter leaves 0.778; the 4x polyphase detector
reduces to 0.630. The control run with true-peak disabled is what proves the
measurement can tell the two apart: an earlier cubic-interpolation version
produced the same 0.778 as the control and was rejected.

Invalid settings (positive ceiling, zero channels, excess lookahead) are
rejected.

The Session applies `clipGuardMode=LIMITER` with the policy ceiling/lookahead/
release; the resolved path reports `clipGuardActive`, ceiling and reason.

## Channel controls

`NAE_SetChannelTrim`, `NAE_SetBalance`, `NAE_SetPolarityMask` and
`NAE_SetChannelDelay` apply per-output-channel gain, L/R balance, polarity
inversion and delay in the render loop after EQ/crossfeed. Session policy
applies them when a non-DSD path opens. `channel_check` pins the argument
contract; the audible effect is exercised through the existing render checks.

## Base vertical slice

`base/` wraps `NAE_Session`, not `Stream`: the session already owns the playback
thread, the queue, the DSP and the position. Base adds the analysis scheduler,
the cache, the render-path snapshot and the UI-facing command/event protocol.

```powershell
cd base
$env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
cargo run --bin base_probe
cargo run --bin base_capi_check
```

`base_probe` generates two 12 s, 120 BPM click WAVs. It runs on the default
endpoint; `NEIRO_BASE_DEVICE` selects another endpoint and
`NEIRO_BASE_SHARE=shared` chooses shared output explicitly (never a fallback).
The A/B/analysis/transport path was observed on the 192 kHz endpoint; the DSP
settings run below was on the 48 kHz virtual endpoint because the LG TV endpoint
was held exclusively by another process. The C ABI check opens no device: it starts Base, reads a snapshot,
subscribes, sends `{"command":"stop"}`, receives the state callback, frees the
snapshot string and shuts down.

| Check | Result |
|---|---|
| A playing, duration | PASS, 12.000 s |
| render path | PASS, 192000 Hz |
| A BPM | PASS, 120.07, confidence 0.833 |
| B pre-analysis/cache | PASS, second request was a cache hit |
| volume 0.5 | PASS, `RenderPath.volume` and applied settings agree |
| EQ enabled | PASS, one 1 kHz +6 dB band round-trips |
| crossfeed enabled | PASS, amount/delay/cutoff round-trip |
| crossfade duration 0 | PASS, enabled; zero duration is the gapless mode |
| ReplayGain album policy | PASS, `replayGainEnabled` |
| pause / resume | PASS |
| seek to 2 s | PASS, position reached |
| next -> B | PASS |
| stop | PASS |
| C ABI (`base_capi_check`) | PASS, start/snapshot/JSON command/callback/string free/shutdown |

The render path is copied from `NAE_SessionState`; the probe never infers
`bitPerfect`, resampling, EQ, crossfade or gapless from a request. Base does not
claim a DSP stage that the engine did not enable.

## DoP verification

DoP needs an endpoint that accepts the carrier rate (DSD64 = 176400 Hz) at
24 valid bits **without coercion**. A resampled carrier is not DoP.

### Carrier check

```powershell
build\dop_probe.exe [deviceId]
```

The passing line is `opened at 176400 Hz, 2 ch, 24 bit (valid 24),
resampling=0`. The LG TV endpoint coerces 176400 to 192000 and is refused; the
WALKMAN endpoint accepts both 176400 and 352800.

### Packing check (no device)

`dop_check.exe` reads a real DFF or DSF and compares the 24-bit words to the
source bytes:

- marker alternates 0x05 / 0xFA;
- DFF payload keeps the stored MSB-first bytes;
- DSF payload is bit-reversed from the stored LSB-first bytes;
- carrier is `dsdRate / 16`.

### Hardware result

Endpoint: `扬声器 (WALKMAN)`,
`{0.0.0.00000000}.{b183398e-d366-4058-a0f5-b3acf1938681}`.
Carrier opened with a 52 ms buffer (9216 frames at 176400).

| Fixture | Carrier | Result | DAC display |
|---|---:|---|---|
| DFF DSD64 music, 10 s | 176400 | normal music, both ears, no clicks | DSD 2.8 MHz |
| DSF DSD64 1 kHz tone, 10 s | 176400 | normal tone, both ears | DSD 2.8 MHz |
| DSF DSD128 1 kHz tone, 10 s | 352800 | normal tone, both ears | DSD 5.6 MHz |

Commands:

```powershell
build\make_dsd128.exe tone64.dsf  10 dsd64
build\make_dsd128.exe tone128.dsf 10 dsd128
build\neiro_cli.exe dop tone64.dsf  --device "<walkman id>" --seconds 10
build\neiro_cli.exe dop tone128.dsf --device "<walkman id>" --seconds 10
build\neiro_cli.exe dop "track.dff" --device "<walkman id>" --seconds 10
```

### Faults found by the hardware

| Symptom | Root cause | Fix |
|---|---|---|
| DFF distorted, clicks | 10 ms default buffer; the feed requested a whole buffer and spun on `AUDCLNT_E_BUFFER_TOO_LARGE` | DoP opens at 24-in-32 and asks for 50 ms; feed uses `GetCurrentPadding` capacity |
| DSF 10 s returned in 0.05 s | `dopBlockFrames` started at `blockSize/2`, so the first `nextPair` used an empty buffer | start at 0 and force the first block read |
| DSF one ear normal, one buzzing | `dsf::File` read the `data` size from the tag itself; payload started four bytes early | seek to `found + 4` before reading the size |
| DSD128 no sound | generator wrote `sampleCount` in bits and the sigma-delta never added 1 on a 0 | sampleCount in bytes; add 1 on a 0 |

Bring-up switches, only for identifying a wrong order:

| Variable | Effect |
|---|---|
| `NEIRO_DOP_DFF_REVERSE=1` | reverse DFF bytes |
| `NEIRO_DOP_SWAP_BYTES=1` | swap the two payload bytes |
| `NEIRO_DOP_SWAP_CHANNELS=1` | swap left/right |
| `NEIRO_TRACE_DOP=1` | per-buffer feed trace |

Failure codes: `rc=12` endpoint coerced/cannot carry; `rc=9` file unreadable;
`rc=10` device disappeared.

## Measurements

### Latency

| | engine | device |
|---|---:|---:|
| bit-perfect, no resampler | 1.3 ms | 53.3 ms |
| FLAC → 192 kHz | 8.8–11.9 ms | 50.7 ms |
| DSD64 → 192 kHz | 3.0 ms | 50.7 ms |

The device column is Windows and cannot be reduced from here.

### Sustained playback

| | FLAC 44.1→192, 10 min | DSD256 1.411M→192, 4 min, music |
|---|---:|---:|
| FIFO max, steady | 3229–3262 frames | 816–829 frames |
| FIFO max slope | +0.008 frames/s | +0.013 frames/s |
| Drift, total | 0.6 ms | 3.0 ms |
| Short renders | 0 | 0 |
| Thread CPU | 0.9% of one core | 3.3% of one core |

Neither FIFO accumulates; drift is a fixed offset rather than a ramp. RSS is
**not** shown to be zero: FLAC rose 0.9 MB over 10 min; DSD256 rose 1.3 MB over
4 min with both halves positive, extrapolating to 340–860 MB over 24 hours. That
is not explained and is an extrapolation, not a measured long run.

### DSD fold caveat

The DSD fold measurement is incomplete. The window is verified
(`CG=0.358749`, `NPG=0.257963`) and the fold position is confirmed
(200 kHz reads 152800.28 Hz against 152800.00), but the amplitude scale is not
calibrated: the same fixture reads −59.71 dBFS under Hann and −72.07 under
Blackman-Harris, 12.4 dB apart where coherent gain accounts for 2.9. No dBFS or
dBc figure from that path should be quoted until the remaining 9.5 dB is found.

## Analysis plane

`analysis_check` generates a 30 s click fixture in the build directory at
120 BPM (15 s loud, 15 s quiet) and a tagged FLAC. Result: BPM 120.064 with
confidence 0.827, loudness −47.96 dB, dynamic range 20.00 dB, 2000 peak floats;
title/artist/album/album artist/track/disc/year/genre/composer/comment and
ReplayGain all read back. Analyzer version 1.

The host owns the cache key (`path + mtime + size + analyzerVersion`) and the
scheduling; the engine call is blocking and device-free.

## Decode-ahead producer

`nae.cpp` starts one producer thread per file handle. It owns the decoder and
fills a source ring to one second, then waits on a condition variable. The render
thread consumes exact blocks through `readDecodedFrames`; if the producer cannot
start, it falls back to inline `readExact`. Seek and gapless/crossfade swap stop
the producer, reset the ring/resampler, and restart it with the new decoder.

Measured with `decode_thread_check` on the 44.1 kHz FLAC: `decodeThread:1` and
position 0.000000 → 0.803605 over 25 slices. `latency_bench` 5 s: `short=0`,
drift +0.36%. `sustain_check` 11 s: `short=0`, FIFO slope within its scatter;
CPU 31% at `NAE_QUALITY_VERY_HIGH`, which was not compared against the old
inline path. All existing checks still pass.

## Not covered

| Item | Why |
|---|---|
| Resampling/DSP off the render thread | Only decode moved; resampling, gapless/crossfade switching and DSP still run on the render thread. Moving those needs the same branch-and-gate treatment. |
| CoreAudio / ALSA | No macOS/Linux machine here to compile or verify; the device layer is still WASAPI. |
| DoP DSD256 | The WALKMAN endpoint was tested at 176400 and 352800 only. |
| DoP multichannel / DST | Rejected or not implemented; DoP is stereo, uncompressed only. |
| 32-bit integer bit-perfect | Internal path is float32; the flag does not compare source width. |
| Long-run RSS (24 h) | Only 4–10 minute windows were measured. |

## Reproduce

Run every non-hardware check:

```powershell
cd engine/build
.\nae_test.exe
.\resample_check.exe
.\dither_check.exe
.\mode_check.exe
.\reopen_check.exe
.\session_check.exe "C:\Music\track.flac"
.\volume_check.exe
.\eq_check.exe
.\crossfeed_check.exe
.\gapless_check.exe a.wav b.wav
.\convolver_check.exe
.\crossfade_check.exe a.wav b.wav
.\param_check.exe "C:\Music\track.flac"
.\dop_check.exe "C:\Music\track.dff"
```

`replaygain_check.exe` needs a file carrying `REPLAYGAIN_TRACK_GAIN` /
`REPLAYGAIN_ALBUM_GAIN`; `gapless_check` and `crossfade_check` need two files of
the same rate and channel count. The DoP hardware run additionally needs
`dop_probe.exe` to report the requested carrier unchanged.
