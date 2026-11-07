# Neiro HiFi Guide

**English** · [中文](HIFI-GUIDE.zh.md)

Each capability below answers three questions: **what it changes**, **how to prove
it is working**, and **what a listener hears**.

This is not the implementation description. Resolution rules, stage ordering and
the ABI (application binary interface) contract live in
`engine/OUTPUT-POLICY.md`; this document refers to them
and does not restate them.

---

## 0. One thing to know before reading

**`bitPerfect` is a wider name than the thing it proves.**

`engine/output_mode.h` says it plainly:

> `bitPerfect` is a bad name for what this computes and the reason code carries
> the honest label... It is true here and it means one thing only: no rate
> conversion was needed. It says nothing about depth, channel count or sample
> format, and the decoder's float32 intermediate means the samples are re-packed
> even when this is true.

So wherever this document says "bit-perfect", it means the whole set of
conditions in `OUTPUT-POLICY.md §6` holds — not that the bytes in the file reach
the DAC untouched. The decoder works in float32, so the samples are always
re-packed once.

There is a second layer for lossy sources: **`bitPerfect = true` means the
decoded samples were not modified further**, not that the source was lossless. A
lossy file can take a zero-processing path, and those two statements do not
conflict. In the 21 real files measured for this guide, MP3 under the native
policy is `true`, and that is correct.

Anyone claiming a "bit-perfect player" is worth asking: **which of these does it
check?**

---

## 1. Exclusive vs shared mode

**What it changes.** Exclusive opens the WASAPI (Windows Audio Session API) endpoint
directly, bypassing the Windows mixer. Shared leaves the mixer in the path.

**How to tell.** `render_path.share`, and reason code `7 = SHARED_MODE`.

Shared has its own code not because it is worse but because **it is the
listener's choice**. `nae.h`: every other reason points at something that went
wrong or a control that would change the outcome; this one is a route chosen on
purpose, in exchange for other programs being able to make sound. So
`bitPerfect = 0`, and whether Windows would in fact preserve the samples is
**untested** — the claim is made in neither direction.

**What the listener hears.** Other programs go silent while exclusive playback
runs, and switching tracks may include a device re-lock (a relay click, a short
gap). That is the cost.

---

## 2. Rate policy: native / device / fixed

**What it changes.** Who decides the sample rate.

| Policy | Whose rate wins | Conversion |
|---|---|---|
| Native | the file's; the device is reformatted to it | none |
| Device | the endpoint's; the file is converted to it | yes, when they differ |
| Fixed | a value the listener picked | yes, when the file differs |

**How to tell.** `policy` is the **request**; `render_path` is **what the device
granted**. They are separate fields on purpose: `fixed` may be refused, and the
engine reports the rate it actually granted plus a coercion reason.

In native mode `bitPerfect` and `resampling` are constants, and that is correct.
An earlier version compared the source rate against the endpoint's *native* rate,
which made native mode claim both "resampling" and "bit-perfect" at once — two
statements that cannot both be true.

**What the listener hears.** Under native, tracks at different rates cause a
device re-lock; under device there is no re-lock, but the signal has certainly
been resampled.

---

## 3. Resampling (soxr, three quality tiers)

**What it changes.** Rate conversion, only when needed, by soxr.

**How to tell.** `resampling = true`, and the reason code says **which kind**:

- `1 = RESAMPLED_TO_DEVICE_RATE` — to match the endpoint
- `2 = RESAMPLED_TO_FIXED_RATE` — to match a value you chose
- `6 = RESAMPLED_TO_SUPPORTED_RATE` — the endpoint refused the rate and a supported one was used

Three codes because **the fix differs**. 1 and 2 are answered by the output-mode
control; 6 is a device capability, and sending the listener to the mode control
for it wastes their time.

**What the listener hears.** 44.1 → 192 interpolation softens transient-dense
passages (brushed drums, piano attacks); quality tiers differ mostly at the
frequency extremes and in transients.

---

## 4. Dither

**What it changes.** Added only when the output depth is narrower than the
internal precision. Internally float32; 16/24-bit integer output needs it, 32-bit
integer or float output does not.

**How to tell.** `dithered = true`, reason code `3 = DITHERED`.

**What the listener hears.** At 16-bit output, very quiet passages (room tone, a
fading tail) have a granular quality without dither; dither trades quantisation
distortion for low-level noise, which reads as "cleaner" rather than "quieter".

---

## 5. Bit depth, container bits and valid bits

**What it changes.** It distinguishes how many bits the container carries from how
many are actually significant.

**How to tell.** `bits_per_sample` (container) versus `valid_bits` (significant).

**What the listener hears.** Nothing — but this is the root cause behind many
"why is it not clean" reports. **24-in-32** is common on HDMI and some USB paths:
the device claims 32 bits while 24 are significant. Comparing against "32-bit"
gives the wrong answer.

The default endpoint measured for this guide reports `192000 Hz / 32 bit (24
valid)`.

---

## 6. Channel conversion and routing

**What it changes.** Down/up-mixing when source and device channel counts differ,
including the LtRt matrix.

**How to tell.** Reason code `4 = CHANNEL_CONVERTED`; routing entries appear in
`active_stages`.

**What the listener hears.** Stereo on a mono device loses the image; 5.1 folded
to stereo loses the surround information; LtRt is deliberate encoding, not a
degradation.

---

## 7. The DSP chain: EQ / crossfeed / convolver / ReplayGain / crossfade / volume

**What it changes.** Each one modifies the samples, and each has its own reason
code.

| Capability | Changes | Code | What the listener hears |
|---|---|---|---|
| EQ (equaliser) | frequency response | `9` | timbre — the most direct |
| Crossfeed | a delayed, low-passed copy of one channel mixed into the other | `10` | less in-head effect on headphones, image pushed forward |
| Convolver | replaces the signal with a FIR (finite impulse response) | `12` | the whole response is replaced; the most structural |
| ReplayGain | a gain | `11` | loudness matched between tracks |
| Crossfade | the end of one file mixed with the start of the next | `13` | no hard cut between tracks |
| Volume | digital gain | `8` | level — but note that it happens **in the digital domain** |

**One rule that is easy to get wrong:** ReplayGain resolved to 0 dB **still
counts as bit-perfect**, because no multiply happens.

**Priority.** `OUTPUT-POLICY.md §6` defines it as **device/shared/format →
routing → DSP → volume**, which is not the numeric order of the codes. The reason
reported is always the **most fundamental** one.

**How to tell.** `reason` and `active_stages`; `bypassed_stages` is the other
half of the same structure.

---

## 8. DSD

**Three paths, three different costs.**

| Path | Carrier | Lossless | How to tell |
|---|---|---|---|
| Native | the device driver accepts DSD directly | yes | caps report `nativeDsdSupported` |
| DoP (DSD over PCM) | DSD packed inside PCM (pulse-code modulation) frames | yes (the carrier is PCM) | `NAE_OpenDop` |
| Downrated | every D-th bit retained | **lossy** | `NAE_OpenDopDownrated` |

Downrating is not simply taking one bit in D — out-of-band noise would come with
it. The implementation decimates first, requantises, and packs DoP at the lower
carrier.

**DoP does not lower the requirement — it wraps DSD in a PCM carrier.** DSD64
needs an endpoint that accepts 176.4 kHz at 24 valid bits. If the endpoint will
not, DoP does not help. `engine/README.md` records that DoP has been verified on
one endpoint (`扬声器 (WALKMAN)`); that is a **capability boundary, not a general
guarantee**.

**Measured for this guide:** a DSD64 source reports `176400 Hz` and, on an
endpoint that only accepts 192 kHz, ends with `reason 6`. On a DAC that accepts
176.4 it does not happen.

**What the listener hears.** Native/DoP versus downrating differs at the very top
end and in spatial impression; the downrating loss is **measurable**, not only
audible.

---

## 9. Gapless

**What it changes.** The next track is decoded and ready before the previous one
ends, so the seam neither closes the stream nor reopens the device.

**How to tell.** **Two fields, both required:**

- `track_serial` advances while `state` stays `PLAYING` — this is a seam, not a new stream
- `position` resets to the new track — this is a track change, not the old one continuing

Either one alone is misleading: a rising serial with a position that did not reset
is a misalignment; a reset position with a state that dropped is a reopened
stream.

**What the listener hears.** Live albums and classical movements should have no
silent gap and no click.

**This check itself was broken.** It hard-coded "track B must be exactly one
second" (`> 0.9 && < 1.1`), so any other fixture was reported as an engine
failure, and the usage text never said so. It now **measures B's real duration**
before comparing, and the usage states that A has to end within about 2.5 s.
Verified with B at 3 s and at 1 s: both pass.

---

## 10. Buffering, latency and realtime behaviour

**What it changes.** Device buffer size, per-period fill, and whether decoding
stalls the render thread.

**How to tell.** Three numbers:

- `bufferDurationMs` / `latencyMs` — what the device gave
- `maxBufferGapMs` — the longest gap between writes
- `maxDecodeStallMs` — the longest the render thread spent **decoding**

The last one is structural: this engine decodes inside the render loop, while the
reference implementation runs its decoder on its own thread. Every millisecond
spent decoding is taken out of the budget the device granted.

**What the listener hears.** A large buffer resists jitter, raises latency and
makes track changes feel sluggish; a small buffer feels immediate but turns any
stall into a click. A listener with a large `maxDecodeStallMs` hears occasional
artefacts in quiet passages while every other counter looks normal — because each
individual write was complete and correct and simply **arrived late**.

**Latency numbers cannot be compared across platforms.** On macOS the figure is
FIFO (first in, first out) depth plus device buffer, which is not the same sum as on Windows.

---

## 11. Analysis: dynamic range, BPM, peaks, spectrum

**What it changes.** A separate decode on its own thread; results go into an LRU (least recently used)
cache keyed by `path + size + mtime + analyzerVersion`.

**How to tell.** `analysis_ready { cached }`. `cached = true` means a reuse, not
another decode.

**One hard rule:** when `bpm_valid = false` the interface **must show nothing** —
not a plausible number. The analyser says the signal was too short or the
autocorrelation peak too weak, and "unavailable" is the answer.

**What the listener hears.** DR (dynamic range) shows how compressed a master is; BPM (beats per minute) helps
pick tracks; the spectrum is decoration, but it has to be **real** — a fake spectrum
and no spectrum sound identical, and differ completely in trust.

---

## 12. Failure and device loss

**Layered, not merged.**

| Situation | Reported | Afterwards |
|---|---|---|
| Device disappeared | `DEVICE_LOST` | playback pauses; **handle, decoder and position stay valid** |
| File will not open | a specific error | no pretence of success |
| Short render | failure | not "success with slightly less audio" |
| No selectable device | error | **never chooses an endpoint on the caller's behalf** |

The last one is a product decision: a play carrying no device id can only land on
the endpoint used last; if it is gone, that is device-lost, not "use the laptop
speakers then".

**What the listener hears.** Pulling the DAC (digital-to-analogue converter) stops the music and waits for it to
come back, instead of quietly continuing on the internal speakers.

---

## 13. How a listener or tester verifies this themselves

### 13.1 By ear, with known reference material

1. **Native vs device.** One 44.1 kHz track under both policies; under device the
   endpoint no longer re-locks if it is running at 192 kHz.
2. **Dither.** 16-bit output, a very quiet fade; compare with dither on and off.
3. **Gapless.** A live album; there should be no gap or click at the seam.
4. **24-in-32.** Do not listen — read `valid_bits`.

### 13.2 By number, reproducibly

Measured on real content for this guide:

```
21 real files (flac / wav / m4a / ape / mp3 / tak), native policy:
  FLAC · WAV · ALAC · APE · MP3 · TAK   all bitPerfect = true, resampling = false
  DSF (176400 Hz) on a 192 kHz endpoint  reason = 6, bitPerfect = false
```

The engine's own checks (under `engine/build/`; all need a real endpoint):

| Check | Expected |
|---|---|
| `nae_test` | All engine checks passed |
| `mode_check` | All output mode checks passed |
| `reopen_check` | no crash across rapid reopen cycles |
| `decode_thread_check <file>` | the render thread still gets audio |
| `pause_edge_check <A> <B>` (both files >= 5 s) | where A paused does not move B |
| `gapless_check <A> <B>` | A must end within about 2.5 s; nothing is assumed about B |

---

## 14. What is measured, and what is only heard

| Measurable only | Audible only |
|---|---|
| sample rate, bit depth, valid bits | whether the EQ sounds right |
| whether resampling or dithering happened | dither in quiet passages |
| bit-perfect and its reason code | resampler quality tiers |
| whether a seam occurred (serial + position) | whether the seam clicks |
| buffer / latency / decode stall, in ms | whether the system feels responsive |
| DR, BPM, peaks, spectrum | whether the display is believable |

Every item on the left has a field behind it; every item on the right has no
substitute.

**One principle:** what can be measured should not be claimed by ear. "It sounds
bit-perfect" is meaningless — `reason` is either 0 or it is not.
