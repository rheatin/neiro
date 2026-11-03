# Output policy, device caps and the resolved path

Status: **proposed contract, for review.** The declarations are in
[`nae_policy.h`](nae_policy.h), included by `nae.h`. The resolver is not wired
into the session yet; this document defines what it must do before code is
allowed to depend on it.

## 1. Ownership

```
host / Base / UI                 engine
-----------------                ------
NAE_OutputPolicy     ->          resolve with NAE_DeviceCaps + NAE_SourceInfo
                                 |
NAE_ResolvedSignalPath <-        the one answer the host displays
```

- The host sends a **requested policy**. It does not resolve it.
- The engine reads **measured caps** and produces a **resolved path**.
- The UI displays the resolved path. It never turns a requested value into an
  "active" badge.
- Auto is resolved by the engine. Every Auto choice has a reason in
  `autoDecisions`; it is not a hidden default.

This is the same rule as `NAE_SessionState`: if two places displayed the value,
they must call one function. There is one resolver.

## 2. Device caps

`NAE_QueryDeviceCaps(deviceId, shareMode, out)` returns caps **for one device in
one share mode**. Exclusive and shared can accept different rates, formats and
DSD carriers, so they are separate queries.

`NAE_DeviceCaps.probed` is a bitmask:

| Bit | Means |
|---|---|
| `NATIVE` | native format/rate/channels were read |
| `RATES` | `rates[]` was probed |
| `FORMATS` | `formats[]` was probed |
| `LAYOUTS` | `layouts[]` was probed |
| `BUFFER` | buffer/period ranges and event-driven flag were probed |
| `DOP` | DoP carriers were probed |
| `NATIVE_DSD` | native DSD rates were probed |
| `SHARE` | exclusive/shared support was probed |

A zero bit is **unknown**, not false. A resolver must not turn "not probed"
into "unsupported". If the policy needs a group that was not probed, the
resolution fails with `NAE_RESOLVE_UNPROBED`; the engine probes before resolving.
It does not guess and does not open the device to find out inside the pure
resolver.

`capsGeneration` increments when the device is re-probed or its capabilities
change. A resolved path stores the generation it used; a host can discard stale
facts without comparing every field.

Fields:

- device id/name, backend, share mode, default/hotplug flags.
- native format, valid/container bits, rate, channels, channel mask, max channels.
- `rates[]` ascending, `formats[]` with valid bits, `layouts[]` as channel masks.
- buffer/period ranges, event-driven support, reported latency.
- DoP carriers, native DSD rates.
- exclusive/shared support.

## 3. Requested policy

`NAE_OutputPolicy` defaults to everything Auto. Numeric `0` means "auto" only
where documented; enum fields use `NAE_AUTO (-1)`.

| Field | Values | Meaning |
|---|---|---|
| `ratePolicy` | Auto, Native, Device, Fixed | how the output rate is chosen |
| `fixedRate` | rate | only when Fixed |
| `resampleAlgorithm` | Auto, Soxr, Linear | Linear is preview-only |
| `resampleQuality` | Auto, Fast, High, VeryHigh | soxr quality when resampling |
| `ditherMode` | Auto, Off, TPDF, Rectangular, NoiseShaped | quantization noise |
| `dsdMode` | Auto, PCM, DSD, PCM→DSD | source-domain handling |
| `dsdTransport` | Auto, None, DoP, Native | how a DSD bitstream reaches the device |
| `dsdRatePolicy` | Auto, FollowSource, DSD64/128/256, PcmFallback, Downrate, Extract | DSD rate handling |
| `targetDsdRate` | 0 = auto, otherwise 2822400/5644800/11289600 | PCM→DSD or fixed DSD target |
| `allowPcmFallback` | bool | permit DSD→PCM when no DSD carrier/native path exists |
| `allowDownrate` | bool | permit DSD128→DSD64 etc. |
| `allowExtractRate` | bool | permit DSD source decimation to PCM |
| `bufferMode` | Auto, LowLatency, Stable, Fixed | device buffer policy |
| `bufferMs` / `periodFrames` | number, 0 = auto | only for Fixed |
| `routingMode` | Auto, PassThrough, Downmix, Upmix, Matrix | channel routing |
| `targetChannelMask` | mask, 0 = auto | requested output layout |
| `upmixPreset` / `downmixPreset` | preset | explicit conversion policy |
| `matrixId` | id, 0 = none | registered custom matrix |
| `bassManagement` | Auto, Off, On | LFE/bass management |
| `lfeMode` | Auto, Silent, LowPassSum, Matrix | where LFE comes from |
| `lfeCrossoverHz` / `lfeGainDb` / `centerGainDb` / `surroundGainDb` | values | bass-management trims |
| `channelMode` | Auto, Normal, Mono, Swap, MidSide | channel operations |
| `balance` | -1..+1 | L/R balance; 0 centre |
| `polarityMask` | bit N | invert channel N |
| `delayFrames[]` | frames | per-channel delay |
| `trimDb[]` | dB | per-channel gain |
| `clipGuardMode` | Auto, Off, Limiter | true-peak limiter |
| `clipCeilingDbTp` | dBTP | limiter ceiling |
| `clipLookaheadMs` / `clipReleaseMs` | ms | limiter timing |

Defaults proposed:

```text
rate Auto, resample Auto/Soxr, quality Auto/High
dither Auto; dsd Auto; dsdRate Auto
allowPcmFallback = 1, allowDownrate = 0, allowExtractRate = 1
buffer Auto; routing Auto; target mask 0; upmix None; downmix ITU_R_775
channel Normal, balance 0, polarity 0, delays/trims 0
clip guard Auto, ceiling -1.0 dBTP, lookahead 5 ms, release 100 ms
```

The two permissions (`allowPcmFallback`, `allowExtractRate`) are explicit
because they are quality degradations. A resolved path still reports which one
happened.

## 4. Resolution rules

### 4.1 Rate

For a PCM source:

1. `Fixed`: only the exact `fixedRate` is accepted. If it is not in
   `caps.rates`, fail with `UNSUPPORTED_RATE`. No silent coercion.
2. `Native`: source rate. If `caps.rates` is unprobed or does not contain it,
   fail (`UNPROBED` or `UNSUPPORTED_RATE`).
3. `Device`: device native rate. If `caps.native`/`caps.rates` is unprobed or
   does not contain it, fail.
4. `Auto`:
   - source rate if probed and present → `SOURCE_RATE_SUPPORTED`;
   - else device native rate if present → `DEVICE_NATIVE_RATE`;
   - else the log-nearest supported rate → `NEAREST_SUPPORTED_RATE`,
     `coerced = 1`;
   - else fail `UNSUPPORTED_RATE`.
5. The output rate must not be 0 on success.

For a DSD source, the DSD mode/rate is resolved first; the output rate is the
carrier/native DSD rate or the PCM extract rate. PCM rate policy does not
override it.

### 4.2 Resampler

- DSD DoP/native: no PCM resampler; DSD rate policy owns conversion.
- Same rate: `resampling = 0`, reason `NO_RESAMPLE`.
- Different rate: `resampling = 1`; `Auto` algorithm = Soxr, quality = High.
  Explicit Linear is allowed but marked preview-only.
- `resampleRatio = outputRate / sourceRate`.
- Dither happens after resampling, never inside it.

### 4.3 Dither

- Target float: `Off`, reason `NO_DITHER`.
- `validBits >= 24`: `Off` (the internal path is 24-bit; 24-in-32 needs no
  dither).
- `validBits < 24`: `Auto` = TPDF, reason `DITHER_REQUIRED`.
- `Off` requested: off even when the depth is narrower; the host is responsible
  for the audible consequence.
- `Rectangular` and `NoiseShaped` only when explicitly requested; Auto never
  selects them. Noise-shaped uses first-order error feedback.
- `ditherBits` is the target valid bits.

### 4.4 DSD: conversion vs transport

Two different things, and the API keeps them separate:

| | DoP | PCM→DSD |
|---|---|---|
| What it does | packs an existing DSD bitstream into 24-bit PCM carrier words with 0x05/0xFA markers | generates a new DSD bitstream from PCM with a sigma-delta modulator and noise shaping |
| Source | DSD (DSF/DFF) | PCM |
| Bit-exact? | yes, if packing is correct and no DSP | no; it is a conversion |
| Device rate | carrier = DSD rate / 16: 176400, 352800, 705600 | target DSD rate; then native or DoP transport |
| DSP | bypasses float DSP; markers would be destroyed | DSP can run before the modulator; volume changes the modulation |
| Failure mode | endpoint cannot accept carrier | no valid modulator/target rate |

`dsdMode` says what happens to the source domain: `PCM`, `DSD`, or `PCM→DSD`.
`dsdTransport` says how a DSD bitstream is carried: DoP, native, or none.
DoP is **not** a conversion, and PCM→DSD is **not** a transport.

For a PCM source:

- `dsdMode = Auto` → `PCM`, `dsdTransport = None`. Auto never invents a DSD
  modulator.
- `dsdMode = PCM_TO_DSD` explicit: modulate at `targetDsdRate` or the resolved
  DSD rate; `dsdModeResolved = PCM_TO_DSD`, `dsdTransport` follows the transport
  policy, `perfectReason = PCM_TO_DSD`.
- If `dsdTransport = DoP`, the target DSD rate must have a carrier in
  `caps.dopCarriers[]`; if native, the rate must be in `nativeDsdRates[]`.
  Otherwise fail; no silent fallback.

For a DSD source:

- `dsdMode = Auto`:
  1. `DSD` with native transport if backend and caps support the source rate;
  2. else `DSD` with DoP if the source rate's carrier is in `dopCarriers[]`;
  3. else `PCM` fallback if `allowPcmFallback` → `DSD_PCM_FALLBACK`;
  4. else fail `DSD_UNAVAILABLE`.
- `dsdMode = DSD`: keep DSD; `dsdTransport = Auto` resolves native → DoP →
  none. If none, fail (explicit DSD mode does not silently become PCM).
- `dsdMode = PCM`: decode/decimate to PCM; `dsdTransport = None`; rate follows
  `dsdRatePolicy`/device.
- `dsdTransport = DoP` explicit: require a carrier; fail if absent.
- `dsdTransport = Native` explicit: require the backend/native rate; fail if
  absent.
- DoP carrier is `sourceDsdRate / 16`. `dsdCarrierRate` carries that value;
  `dsdRateResolved` carries the DSD bitstream rate.

`dsdRatePolicy`:

- `FollowSource`: exact source rate.
- `DSD64/128/256` or `targetDsdRate`: exact target; if unavailable, fail
  unless the policy is Auto/Downrate and `allowDownrate`.
- `PcmFallback`: require `allowPcmFallback`; output PCM.
- `Downrate`: choose the nearest lower DSD rate in caps; require
  `allowDownrate`; report `DSD_DOWNRATED`.
- `Extract`: PCM decimation at a chosen rate; require `allowExtractRate`;
  report `DSD_EXTRACTED`.
- `dsdPcmFallback`, `dsdDownrated`, `dsdExtracted` are separate booleans; the
  UI must say which degradation happened.

DoP/native output is not passed through the PCM DSP chain. EQ, crossfeed,
convolver, volume, balance, polarity, delay and clip guard are bypassed;
`bypassedStages` says so. PCM→DSD runs the PCM DSP first, then the modulator;
the modulator itself is `NAE_STAGE_DSD_MODULATOR`, and the result is never
bit-perfect from a PCM source.

### 4.5 Buffer

- `Auto`: backend default/stable buffer, event-driven if caps support it.
- `LowLatency`: smallest supported buffer/period; if it cannot be met, fail
  rather than silently using a larger one.
- `Stable`: backend default; prefer event-driven.
- `Fixed`: `bufferMs`/`periodFrames` must fit caps; otherwise fail.
- On success fill `bufferFrames`, `periodFrames`, `eventDriven`,
  `bufferLatencyMs`, `driverLatencyMs`, `totalLatencyMs`.
- `bufferMs` is a requested duration; the actual frames are rate-dependent.

### 4.6 Routing and channels

- `targetChannelMask = 0` means "source layout if the device supports it", or
  the preset's implied target for an explicit upmix/downmix.
- The canonical complete route is `MATRIX`: `NAE_RoutingMatrix` carries
  `gain[target][source]` complex gains for up to 32 channels, source/target
  masks, normalization, phase mode/taps and direct-path latency alignment. Presets are conveniences over the same
  matrix; `NAE_RegisterRoutingMatrix` returns the `matrixId` a policy carries.
- Named layouts: mono, stereo, 2.1, 3.0, 4.0, 5.0, 5.1, 6.1, 7.1, 5.1.2,
  5.1.4, 7.1.2, 7.1.4 (the last four use the top channel bits).
- **Auto never upmixes.** Same layout → pass-through. Target with fewer
  channels → standard downmix. Target with more channels → fail; the caller
  must choose an upmix preset or a matrix.
- Explicit upmix presets (source → target):

  | Preset | Source | Target | Proposed mapping |
  |---|---|---|---|
  | `MONO_TO_STEREO` | mono | 2.0 | L = R = M |
  | `MONO_TO_CENTER` | mono | 5.1/7.1 | C = M; other channels silent |
  | `MONO_TO_ALL` | mono | any multi | equal-power copy to every non-LFE channel; LFE silent |
  | `STEREO_TO_3_0` | 2.0 | 3.0 | L/R direct; C from sum |
  | `STEREO_TO_4_0` | 2.0 | 4.0 | L/R direct; surrounds from L/R |
  | `STEREO_TO_5_0` | 2.0 | 5.0 | 3.0 + surrounds |
  | `STEREO_TO_5_1` | 2.0 | 5.1 | L/R direct; C from sum; surrounds from L/R; LFE silent |
  | `STEREO_TO_5_1_BASS` | 2.0 | 5.1 | same + 80 Hz low-pass LFE from the sum |
  | `STEREO_TO_6_1` | 2.0 | 6.1 | 5.1 + back center |
  | `STEREO_TO_7_1` | 2.0 | 7.1 | 5.1 mapping + side/back copies of surrounds |
  | `5_1_TO_6_1` | 5.1 | 6.1 | copy surrounds into back center |
  | `5_1_TO_7_1` | 5.1 | 7.1 | copy surrounds into side/back |
  | `STEREO_TO_5_1_2` | 2.0 | 5.1.2 | 5.1 + top front = 0.5 L/R |
  | `STEREO_TO_5_1_4` | 2.0 | 5.1.4 | 5.1 + top front/back = 0.5 L/R |
  | `STEREO_TO_7_1_2` | 2.0 | 7.1.2 | 7.1 + top front = 0.5 L/R |
  | `STEREO_TO_7_1_4` | 2.0 | 7.1.4 | 7.1 + top front/back = 0.5 L/R |
  | `5_1_TO_5_1_2` | 5.1 | 5.1.2 | top front = 0.5 L/R |
  | `5_1_TO_5_1_4` | 5.1 | 5.1.4 | top front/back = 0.5 L/R |
  | `7_1_TO_7_1_2` | 7.1 | 7.1.2 | top front = 0.5 L/R |
  | `7_1_TO_7_1_4` | 7.1 | 7.1.4 | top front/back = 0.5 L/R |

  These are explicit product presets; the exact coefficients and whether an LFE
  is invented are listed in section 10 as open decisions. They are never used
  by Auto.
- Explicit downmix presets: `5_1_TO_3_0`, `5_1_TO_2_1`, `5_1_TO_STEREO`,
  `7_1_TO_3_0`, `7_1_TO_5_1`, `7_1_TO_STEREO`, `5_1_2_TO_5_1`,
  `5_1_4_TO_5_1`, `7_1_2_TO_7_1`, `7_1_4_TO_7_1`, plus `STANDARD`,
  `ITU_R_775`, `LORO`, `LTRT`. The canonical fallback for missing cases is
  `ITU_R_775`; custom coefficients go through `MATRIX`.
- Bass management: `Auto` is on when the target has LFE and the source does not;
  `lfeMode=LowPassSum` derives LFE from the sum at `lfeCrossoverHz`, `Matrix`
  takes it from the matrix, `Silent` leaves it empty. `lfeGainDb`,
  `centerGainDb`, `surroundGainDb` are applied after routing.
- A preset's explicit LFE row wins over `bassManagement=Auto`: `STEREO_TO_5_1`
  leaves LFE silent, `STEREO_TO_5_1_BASS` derives it. Auto does not turn the
  former into the latter behind the host's back.
- `STANDARD` is the ITU_R_775 coefficient set for the matched 5.1/7.1 layout;
  `LORO` uses the same magnitudes without the surround phase shift; `LTRT` needs
  the future all-pass stage
### Standard coefficients

Presets are only defaults; `NAE_BuildRoutingMatrix()` returns the same values as
an editable `NAE_RoutingMatrix`. A host may change any gain, set
`routingMode = MATRIX`, register the matrix and use its `matrixId`.

Gain notation is linear; -3 dB = 0.7071068, -6 dB = 0.5.

| Preset | Standard coefficients |
|---|---|
| `MONO_TO_STEREO` | L = R = M |
| `MONO_TO_CENTER` | C = M; all others 0 |
| `MONO_TO_ALL` | every non-LFE channel = M / sqrt(N_non_lfe); LFE = 0 |
| `STEREO_TO_3_0` | L, R direct; C = 0.7071 (L + R) |
| `STEREO_TO_4_0` | L, R direct; BL = 0.7071 L; BR = 0.7071 R |
| `STEREO_TO_5_0` / `5_1` | L, R direct; C = 0.7071 (L + R); BL = 0.7071 L; BR = 0.7071 R |
| `STEREO_TO_5_1_BASS` | as 5.1; LFE = low-pass 80 Hz of 0.5 (L + R) |
| `STEREO_TO_6_1` | as 5.1; BC = 0.5 (L + R) |
| `STEREO_TO_7_1` | as 5.1; SL = 0.5 L; SR = 0.5 R |
| `5_1_TO_6_1` | BC = 0.7071 (BL + BR) |
| `5_1_TO_7_1` | SL = BL; SR = BR |
| `STEREO_TO_*_2/*_4` | base layout + top front/back = 0.5 L / 0.5 R |
| `5_1_TO_5_1_2/*_4` | top front/back = 0.5 L / 0.5 R |
| `7_1_TO_7_1_2/*_4` | top front/back = 0.5 L / 0.5 R |
| `ITU_R_775` (5.1→2.0) | L' = L + 0.7071 C + 0.7071 Ls; R' = R + 0.7071 C + 0.7071 Rs; LFE omitted |
| `5_1_TO_3_0` | L' = L + 0.7071 Ls; R' = R + 0.7071 Rs; C' = C; LFE omitted |
| `5_1_TO_2_1` | L/R as ITU; LFE' = LFE |
| `7_1_TO_5_1` | L, R, C, LFE direct; BL' = 0.7071 (BL + SL); BR' = 0.7071 (BR + SR) |
| `7_1_TO_3_0` | L' = L + 0.7071 C + 0.5 (BL + SL); R' = R + 0.7071 C + 0.5 (BR + SR); C' = C; LFE omitted |
| `7_1_TO_STEREO` | same as `7_1_TO_3_0` with C folded into L/R; LFE omitted |
| `5_1_2_TO_5_1` | L += 0.5 TFL; R += 0.5 TFR |
| `5_1_4_TO_5_1` | L += 0.5 (TFL + TBL); R += 0.5 (TFR + TBR) |
| `7_1_2_TO_7_1` | L += 0.5 TFL; R += 0.5 TFR |
| `7_1_4_TO_7_1` | L += 0.5 (TFL + TBL); R += 0.5 (TFR + TBR) |
| `LTRT` (5.1/7.1→2.0) | complex: see the LtRt section; quadrature 0.7071 (5.1) or 0.5 per surround/back (7.1) |

`LORO` uses the ITU magnitudes with no surround phase shift.

### LtRt is solvable now

A real gain matrix cannot express a 90-degree phase shift, so `NAE_RoutingMatrix`
has **complex gains**: `NAE_ComplexGain { real, quadrature }`. A source column
with a non-zero `quadrature` gain is passed through a 90-degree Hilbert FIR
(`phaseMode = HILBERT_FIR`). The resulting complex multiply is exact in phase for
an ideal Hilbert; the FIR is the realizable approximation.

Standard `LTRT` matrix, 5.1:

```text
Lt = L_d + 0.7071 C_d + 0.7071 H(Ls)
Rt = R_d + 0.7071 C_d - 0.7071 H(Rs)
LFE omitted
```

For 7.1, fold first with the same 0.7071 rule: `Ls = 0.7071 (BL + SL)`,
`Rs = 0.7071 (BR + SR)`. The quadrature gains are therefore 0.5 for each of
`BL`/`SL` and `-0.5` for each of `BR`/`SR`.

Implementation:

- First concrete implementation: `phaseMode = FIR_HILBERT`, `phaseTaps = 255`.
  The FIR has a constant group delay of 127 frames at every rate and is covered
  by `hilbert_check`. Its known trade-off is Hilbert low-frequency roll-off:
  20 Hz is attenuated; `phaseTaps` can be raised for more low-frequency accuracy
  at the cost of latency and CPU.
- The frequency-warped parallel all-pass mode (`IIR_ALLPASS_90`) remains
  selectable in the policy for a lower-latency, flat-magnitude implementation;
  its coefficient generator is not built yet. The resolver reports which mode
  was actually used.
- `alignPhaseLatency = 1` delays the direct L/R/C paths by the measured group
  delay so the stereo image does not shift. The resolved path reports
  `phaseLatencyFrames`; for IIR the value is the design's nominal delay.
- `phaseMode`, `phaseOrder`, `phaseTaps` and `alignPhaseLatency` are matrix
  fields. A host can change them, but must read back the resolved values rather
  than predicting latency.
- `LTRT` is a `DOWNMIX` preset with `downmixPreset = LTRT`, or the same matrix
  built by `NAE_BuildRoutingMatrix` and edited/registered.
- `phaseShiftActive`, `phaseFilterOrder`, `phaseFilterTaps` and
  `phaseLatencyFrames` are in `NAE_ResolvedSignalPath`; `activeStages` carries
  `NAE_STAGE_PHASE_SHIFT`.
- LTRT is never bit-perfect (`NAE_REASON_PHASE`/`ROUTING`) and is unavailable on
  DoP/native DSD; it requires a PCM path (PCM fallback or PCM source).
- The sign convention above is the Dolby Pro Logic II convention (Ls positive,
  Rs negative quadrature). A decoder expecting the opposite sign is a matrix
  edit, not a code change.

- `MATRIX` requires `matrixId` and a registered matrix; `targetChannelMask`
  must match the matrix's output count. The matrix coefficients already include
  the LFE row; `NAE_OutputPolicy.lfeGainDb` is the bass-management trim for
  presets, not a second matrix gain.
- Preset versus `targetChannelMask` must agree. A mismatch fails
  `ROUTING_UNAVAILABLE`; the resolver does not ignore one of them.
- `channelConversion = 1` whenever masks or counts differ; reason
  `CHANNEL_CONVERTED`/`ROUTING`.
- Balance, polarity, delay and trims are applied after routing and gains, before
  clip guard. They are per-output-channel arrays; the engine applies them
  through `NAE_SetBalance`, `NAE_SetPolarityMask`, `NAE_SetChannelDelay` and
  `NAE_SetChannelTrim`.
- Any of balance != 0, polarity != 0, delay != 0, trim != 0 marks the path
  non-bit-perfect with the matching reason.

### 4.7 Clip guard

- `Auto` = limiter **on** when the path is not bit-perfect and any gain/DSP/
  routing stage is active; off when the path is bit-perfect. Report
  `CLIP_GUARD_AUTO_ON/OFF`.
- Explicit `Limiter`: active; explicit `Off`: off.
- `clipGuardActive` is 1 when the limiter is in the chain, not when it is
  currently reducing gain. Runtime gain reduction is exposed by
  `NAE_GetClipGuardReductionDb`.
- The implementation is a feed-forward limiter with lookahead and a 4x
  polyphase-sinc true-peak detector. Cubic interpolation was tried first and
  rejected: at fs/4 it reconstructs the sample points, not the waveform.
- The limiter is after all gains and routing, before dither/DSD output.
- It is skipped on DoP/native DSD; those paths cannot carry a PCM limiter.

### 4.8 ReplayGain and loudness

ReplayGain policy is a session setting, not part of `NAE_OutputPolicy`.
`replayGainMode` and `replayGainAppliedDb` are copied into the resolved path
from the session's applied state. A non-unity applied gain marks
`NAE_REASON_REPLAYGAIN`. `NAE_REASON_LOUDNESS` is reserved for R128/loudness
normalization when that exists; it must not be used for ReplayGain.

## 5. Canonical stage order

```
decode (source layout)
  -> EQ (per source channel)
  -> crossfeed (stereo only)
  -> convolver (source rate)
  -> ReplayGain
  -> volume
  -> routing / channel conversion
  -> bass management (LFE crossover)
  -> balance / per-channel trim / polarity / delay
  -> clip guard
  -> dither / requantise
  -> DSD modulator (only PCM→DSD)
  -> DSD transport (DoP/native; DSD sources skip the PCM chain)
  -> device buffer
```

DoP/native DSD bypasses every float DSP stage; the resolved path lists them as
`bypassedStages`, not `activeStages`. Crossfade is a transition stage
(`NAE_STAGE_CROSSFADE`) and is reported with `crossfadeActive` and
`crossfadeDurationMs`; gapless is a queue property reported by `gapless`, not a
per-sample stage.

## 6. Bit-perfect

`bitPerfect = 1` only when all of these hold:

- no resampling;
- no dither;
- no routing/channel conversion;
- no EQ, crossfeed, convolver, crossfade;
- ReplayGain resolved to 0 dB / off;
- volume == 1.0;
- balance == 0, polarity == 0, delays == 0, trims == 0;
- clip guard off;
- not shared mode (existing product decision);
- DSD transport is bit-exact; see the DSD rule below.

Reason priority: device/shared/format first, then routing, then DSP, then
volume. A new `NAE_REASON_*` must be added to the existing reason table; no
silent reuse.

### DSD bit-perfect rule

A DSD source is `bitPerfect = 1` when:

- `dsdModeResolved = DSD`;
- `dsdTransportResolved = DoP` or `Native`;
- there is no PCM fallback or extract in this branch; DSD downrate is a
  decimation of the bitstream carried over DoP;
- no PCM DSP or crossfade is applied;
- no volume, balance, polarity, delay, trim or clip guard is applied;
- the transport does not resample or alter the bitstream.

A PCM-only stage that the user requested on a DSD path is bypassed, not applied.
That does not by itself break bit-perfectness, but `bypassedStages` must say so
and the UI must not show the setting as active.

DoP packing is exact; the marker bytes are transport, not processing. Native
DSD is exact when the backend passes the bitstream unchanged. PCM→DSD is always
`bitPerfect = 0` (`NAE_REASON_PCM_TO_DSD`). A DSD source decoded to PCM,
downrated or extracted is also 0.

`activeStages` still contains `NAE_STAGE_DSD_TRANSPORT` for a bit-perfect DoP or
native path: it is a transport, not a DSP stage. `NAE_REASON_DSD_TRANSPORT` is
reserved for a future transport that cannot claim bit-exactness.

## 6.1 Diagnostics

`NAE_ResolvedSignalPath` carries `diagnosticCount` and up to 32
`NAE_Diagnostic` entries. Every resolve fills them for the current source and
policy, so they are **per song**, not a static settings dump.

```c
typedef struct NAE_Diagnostic {
    uint32_t structSize;
    int32_t severity;   // INFO, AUTO, FALLBACK, WARNING, ERROR
    int32_t code;       // AUTO_CHOICE, RATE_COERCED, DSD_PCM_FALLBACK, ...
    uint32_t field;     // rate, resampler, dither, DSD, buffer, routing, ...
    int32_t requested;
    int32_t resolved;
    uint32_t value;     // rate/frames/bits, 0 when not numeric
    char message[192];  // English fallback; UI can localize by code
} NAE_Diagnostic;
```

- Every Auto field that was resolved emits an `AUTO` diagnostic with the chosen
  value and reason, e.g. "Auto selected 192000 Hz", "Auto kept PCM because the
  source is PCM", "Auto enables the clip guard for this processed path".
- Every fallback emits `FALLBACK`: rate coercion, DSD→PCM, DSD downrate, DSD
  extract, Auto downmix, upmix, phase/LtRt, bass management.
- Every failure emits `ERROR` with the unmet constraint; `resolved = 0` and
  `failure` is set. No path is invented.
- The UI should show `message`, or localize `code`/`field` and ignore the text.
- `NAE_ResolveSignalPath` is pure and must be called again when the track,
  device caps or policy changes. Do not cache a resolved path across songs.

`policy_check` covers native, coerced, DSD DoP, LtRt, failure and diagnostic
counts. `hilbert_check` covers the FIR quadrature, taps and LtRt matrix.

## 6.2 Session application

`NAE_SessionOpenWithPolicy` consumes the policy where the engine can honour it:

- rate: Native, Device, Fixed; Auto is resolved from the source rate and the
  measured device rates (source if accepted, otherwise Device)
- resampler quality
- dither: Off, TPDF, Rectangular
- `NAE_SessionGetOutputPolicy` returns the requested policy

The session still does not implement routing, DSD transport, buffer policy,
channel balance/polarity/delay/trim, bass management or clip guard. Those fields
are not silently accepted: the resolved path carries WARNING diagnostics naming
the field, while playback continues with the decoder/backend defaults.

`NAE_SessionGetResolvedSignalPath` remains the actual path; the requested policy
and the actual path are separate facts.

## 7. Failure

- `NAE_ResolveSignalPath` returns an `NAE_ERR_*` code and sets
  `out->status` / `out->failure`.
- `resolved = 0`; no fields are synthesized.
- Auto never falls back to another device, share mode, sample rate, channel
  count, dither mode, DSD mode or buffer size. Fallbacks exist only where the
  policy explicitly permits them (`allowPcmFallback`, `allowDownrate`,
  `allowExtractRate`), and they set the corresponding resolved flag/reason.
- A device-lost path is `NAE_RESOLVE_DEVICE_LOST`, not a generic format error.

## 8. Versioning and ABI

- `structSize` first; `version` second.
- Fields are appended only. A host reads a field only when `structSize` covers
  it; the engine zeroes the output before writing.
- `capsGeneration` is the cache key for device facts.
- Fixed arrays keep the struct copyable and C ABI-safe. No pointers, no
  allocation, no string ownership.
- Current sizes with MinGW x86_64 / default alignment (for review; append-only
  changes will grow them):

  | Struct | Size | Align |
  |---|---:|---:|
  | `NAE_DeviceFormat` | 12 | 4 |
  | `NAE_DeviceCaps` | 1352 | 8 |
  | `NAE_SourceInfo` | 36 | 4 |
  | `NAE_OutputPolicy` | 568 | 8 |
  | `NAE_RoutingMatrix` | 8236 | 8 |
  | `NAE_AutoDecision` | 16 | 8 |
  | `NAE_Diagnostic` | 220 | 4 |
  | `NAE_ResolvedSignalPath` | 9160 | 8 |

  These are not an ABI promise across architectures; the Rust/C++ mirror asserts
  will pin the actual target's numbers when the resolver is implemented.
- Base mirrors these structs with `serde` types and `offset_of!` asserts; C++
  pins the same literals with `static_assert`. A one-sided edit fails the build.
- The resolver is pure: no device open, no sample touch, safe to call from a UI
  preview or a test.

## 9. Platform notes

- **WASAPI**: caps per share mode; DoP carrier must open at 24-bit without
  resampling; native DSD is not a standard WASAPI path.
- **CoreAudio**: caps per AudioUnit/route; interruptions and route changes
  invalidate `capsGeneration`; AVAudioSession owns the app-level category.
- **AAudio/OpenSL ES**: caps per device/route; USB DAC capabilities are the
  source of truth; audio focus is a host concern.
- `shareMode` in caps is the mode being queried, not a mode the resolver may
  switch to.
- Buffer ranges are backend-specific. Do not normalize them into a fake
  "all devices support 10 ms".

## 10. Unprobed caps

`NAE_ResolveSignalPath` is pure and therefore cannot probe. If a decision
requires a group whose `probed` bit is clear, return `NAE_RESOLVE_UNPROBED` with
the field named in `failure`. The engine's `NAE_QueryDeviceCaps` performs the
probe (or refreshes `capsGeneration`) before calling the resolver.

Unknown is not a default. It is also not a license to open a device from the
resolver.

## 11. Open decisions for review

1. **Fixed rate unsupported**: fail (proposed) or coerce with a reason? The old
   engine coerced; the new policy is stricter.
2. **DoP/native DSD bit-perfect**: **resolved: yes** for exact transport with
   no DSP, fallback, downrate or extract. PCM→DSD remains non-bit-perfect.
3. **Auto PCM fallback default**: proposed `allowPcmFallback = 1` so a DSD file
   plays on a PCM-only device, with an explicit resolved flag. Purist default is
   0.
4. **Routing**: **resolved**. `MATRIX` is the adjustable canonical form;
   presets use the standard coefficients above. Auto still never upmixes.
5. **Auto clip guard**: proposed on for any non-bit-perfect path, off for
   bit-perfect. Does an EQ/volume path always get a limiter?
6. **Buffer Auto**: stable backend default, or low-latency when the route is
   interactive?
7. **Channel trim order**: proposed after routing; confirm per-channel trims
   refer to output channels, not source channels.
8. **Mono-to-multi**: `MONO_TO_ALL` equal-power copy vs center-only default; the
   preset list has both, but which one should a future UI offer first?
9. **LtRt filter**: the working default is the 255-tap FIR (exact latency,
   2.7 ms at 48 kHz, low-frequency roll-off). The IIR all-pass remains the
   lower-latency/flat-magnitude option once its coefficient generator lands.
   The phase is solvable now; this is a quality/latency tuning choice, not an
   open design decision.
10. **Immersive presets**: 5.1.2/5.1.4/7.1.2/7.1.4 now have standard house
    coefficients; confirm the top-layer 0.5 L/R gain and the 0.5 fold-down.

## 12. Implementation order (after review)

1. `NAE_OutputPolicyDefaults`, `NAE_QueryDeviceCaps` for WASAPI, and the pure
   resolver with unit tests for every Auto branch.
2. Wire the resolver into `NAE_SessionOpen`; no behavior change until the
   resolved path matches current behavior.
3. Add `NAE_GetResolvedSignalPath(session, out)` and the Base mirror; UI reads
   only that.
4. Add routing/balance/polarity/delay/clip guard stages behind the policy.
5. Add DSD modes/rate policy, then CoreAudio/AAudio caps.
