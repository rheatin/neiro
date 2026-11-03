# Lessons

What went wrong in this engine, and what it cost. Not a design document and not a
changelog — a record of the failures that a future change could plausibly
introduce again, with the measurement that would have caught each one earlier.

The README documents behaviour. This documents the paths that led there.

Related: `RESAMPLING.md` (three early resampler bugs), and
`../src-tauri/CRASH_POSTMORTEM.md` (the null-pointer crash in the render loop).

---

## The shape of most of these

Three of the worst bugs shared one structure, and it is worth naming because it is
the shape to look for:

> **Correct for the only caller that existed, and wrong for the caller that
> arrived later.**

The 1 kHz test tone was the first caller, and it accidentally satisfied every
precondition each of these bugs relied on. Its length was computed *from*
`milliseconds`, it fit in a single call, and it had no channel conversion to
report. File playback is the caller that broke all three at once, and every
engine harness passed throughout, because they too rendered a whole stream in one
call.

A green test suite is evidence about the tests that ran. It is not evidence about
the next caller.

---

## 1. A parameter that was never read

**`milliseconds` was not enforced anywhere.** The render loop's only exit was the
source running out, which for a tone coincided with the time budget — a tone's
source limit was computed *from* `milliseconds`. For a file the source limit is
the file, so one 200 ms call consumed a whole five-second track.

*Fixed* by measuring the budget in device frames, because the device is what keeps
real time. See §6 for why the device frames.

**How it hid:** the parameter was *derived from* another value inside the code, so
reading the code showed it being used. A parameter that one caller happens to
compute from something else looks used and is not.

## 2. State that did not outlive the call

**Stream state was function-local.** `fifo`, `sourcePos` and the tone's phase
lived in the play function. The host renders in 200 ms slices, so the waveform
restarted five times a second — 200 ms is not a whole number of 1 kHz cycles, so
that is a click, not a harmless restart — and a file replayed from the top five
times a second.

*Fixed* by moving all three onto the handle, which is one playback session.

**How it hid:** every harness renders a whole stream in one call, so "one call"
and "many calls" were never distinguished until the app existed.

## 3. An honesty flag that was only mostly honest

**Bit-perfect ignored channel conversion.** The flag was computed from the sample
rate and the dither, so a 6-channel file at 48 kHz on a 48 kHz stereo device
claimed to be bit-perfect while libswresample folded six channels into two. Both
flags were telling the truth; they were just not the whole truth.

*Fixed* in `NAE_OpenFile`. The UI badge is driven solely by that flag, so a wrong
value is not a rounding error — it is the engine making a claim it cannot support.

## 4. A function that broke its own contract

**`NAE_GetLastErrorText` returned `NAE_OK` where the header promised a byte
count.** `NAE_OK` is 0, so "the engine had nothing to say" and "here is the
message" were indistinguishable, and every error reached the UI as a bare code.

This one is worth dwelling on: the function existed *specifically* to stop that,
and reintroduced it. The header was the correct document and the implementation
was wrong — reading the declaration rather than the body would have caught it.

**How it hid:** a caller written against the header is correct and a caller
written against the body is not. Only one of them gets tested.

## 5. A quantity that meant something other than its name

**`Decoder::readExact` returned "at least" the requested frames, and the caller
sized its accounting from the request.** A FLAC frame is 4096 frames; asking for
893 got 4096, all of which went into the pipeline, while `sourcePos` — the number
everything downstream balances against — advanced by 893.

This was the root cause of §6, and the reason the numbers looked impossible. The
FIFO was fed roughly nine times the audio the source account said it had
supplied.

*Fixed* by making it return exactly, keeping the remainder for the next call.

**How it hid:** the caller and the callee were each internally consistent, and
nothing checks that they are consistent *with each other*.

## 6. A request that could not be satisfied, reported as satisfied

**`topUp` could consume source, produce nothing, and return true.** The resampler
emits only once it holds a whole input block (682 frames for 44.1 → 192 kHz).
`topUp` asked for `needDevice × ratio + 64` source frames, which for a device
wanting 160 frames is 100 — less than one block. The resampler absorbed them and
returned nothing, and `topUp` returned `true` regardless.

The `+ 64` was meant as slack for filter latency. It has to be at least one whole
block, because that is the smallest amount that can produce any output.

## 7. Production with no consumer

**The render loop pulled source when the device had no room for it.** On
`capacity == 0` it called `topUp(bufferFrames)` "so the converter keeps a steady
input rate". That is backwards: a full device buffer means the consumer has
stopped, so every frame pulled there was paid for by nobody. Since `topUp` pulls
in whole resampler blocks, each pull was far larger than the demand that would
follow it, and the FIFO grew without bound — a DSD stream ran the playhead at
**122% of real time**, consuming source the device had not asked for and never
would.

## 8. Success reported over a render that did not happen

**`NAE_PlaySine` returned `NAE_OK` no matter how the render loop exited.** Every
`break` on a WASAPI failure left the loop early, `stopClient` then succeeded, and
the caller was told everything was fine. The host looped, each call returned
immediately, and playback ran at a fraction of real time with **no error
anywhere**: the position still advanced and the signal path still read correctly.

*Fixed*, with one carve-out: a track ending mid-slice cannot fill its budget, and
the first version of the check flagged the last slice of every file as an error.
`short = 0` is a real signal; `short = 1` at the end of a track is not.

## 9. A reset that existed and was never called

**A seek did not reset the resampler.** `Resampler::reset()` was present and its
comment said "Call on seek"; `NAE_Seek` never called it. soxr is a polyphase
filter holding the last frames of every phase, so the first few milliseconds
after a jump were the tail of the *old* position mixed into the new one — an
audible smear exactly where the listener jumped on purpose.

*Fixed*, and deliberately with `reset()` rather than `flush()`: flushing is
permanent, and calling it mid-stream is what once wrote through a null pointer and
took the process down.

---

## Measuring: three confident wrong numbers

Worth more than any single bug above, because all three survived scrutiny and were
acted on. Each was a number that looked precise and meant nothing.

### 10. "66.5% of real time" — from a stream running at 100%

Position updates arrive once per second of audio, so a window holding N updates
spans N−1 seconds of audio. Dividing an audio span by an external wall-clock
window reports exactly (N−1)/N. A correct engine was reported as two-thirds speed.

*Fixed* by timestamping events on arrival and comparing like with like.

### 11. "57 ms of latency" — from one line of trace

The steady-state FIFO was quoted from a single trace line, caught while draining
a backlog that a *different* bug had built up. The median was 8.8 ms. The work
planned against the 57 ms figure was seven times the size of the real problem.

*Fixed* by reporting a median with p5/p95, and by having a tool that measures
steady state over a window rather than a sample.

### 12. "100% of a core" — from an instrument that does not measure CPU

`std::clock()` on Windows returns elapsed wall-clock time, so `clock_delta / wall`
is 1.000 for an idle process and a saturated one alike. The loop was reported at
100% CPU for several rounds, a plan was made to fix it, and the plan targeted a
problem that did not exist. The real figure, via `GetThreadTimes`, is **1–2%**.

What gave it away was not the value but its *insensitivity*: PCM WAV, where
decoding is a `memcpy`, reported the same 100% as FLAC. **A cost that does not
move when the work changes by an order of magnitude is not a cost.**

### 13. "No accumulation" — from a tool that collected no samples

`sustain_check` reported four `[PASS]` lines and exit 0 for a run that measured
nothing. A 20-second file had been asked to play for 150 seconds, hit end-of-file
at 19.8, and every slope it printed was a least-squares fit over zero points. A
fit over no data is not a flat line; it is the absence of one, and reporting it
as evidence of absence is the whole failure this file keeps recording.

A second instance of the same fault was hiding behind it: the sampling branch had
never executed at all, because `sampleStart` was initialised to the absolute
`steady_clock` reading while the comparison used a relative one. Every
comparison was off by a constant near 85000 seconds. Zero samples, twice over,
for two different reasons, and the tool's own output said "PASS" both times.

*Fixed* by hard-failing below three samples, and by fixing the units. Neither
would have been found without a negative control.

### 14. A negative control that measured the clamp instead of the detector

`NEIRO_FIFO_OVERFEED` exists to prove the growth detector can see growth. After the
fifo ceiling was added, the control settled at 22 000–26 000 frames against a cap
of roughly 30 000 — pressed against it — and reported **no trend**, correctly,
because the canary had done its job and turned an unbounded fifo into a constant.

So the control had stopped testing what it was for. Two defensive changes were in
direct conflict and neither alone would have shown it: the canary that stops a
leak, and the detector that measures one.

*Fixed* by lifting the ceiling in the control. With it up, the overfeed grows at
+71.9 frames/s, predicting +3257 frames over the window against a scatter of 494,
halves agreeing — 6.6× the series' own noise.

> Two changes can each be correct and jointly make a measurement meaningless. The
> only way to see that is to have a control that is expected to fail.

### 15. A verdict printed with the wrong evidence beside it

The sustained-playback pass/fail lines read the half-window slopes from captured
variables, which held the **fifo's** halves. The RSS line therefore printed
"halves disagree in sign (+0.008 / −0.040)" directly above a slope of
+0.0006 MB/s — evidence for a verdict, from a different series.

The RSS conclusion happened to survive, because RSS's own halves agreed. But a
verdict with unverifiable evidence next to it is not a verdict, and in this case
the check that actually decided RSS was one that had never been made.

*Fixed* by passing each series' own halves as parameters. A small fix, recorded
because the habit it represents — verifying that a printed number came from the
computation next to it — is the same habit that catches the four wrong measurements
above.

### 16. RSS: an unexplained order of magnitude between two formats

Not a wrong number — a real trend, and an unexplained one. FLAC 44.1→192 over ten
minutes grew 0.9 MB from a base of ~21 MB, with a second-half slope of
+0.0006…+0.0013 MB/s. DSD256 1.411M→192 over four minutes grew 1.3 MB from a base
of 22.6 MB, at +0.0040/+0.0099 MB/s — and both halves positive, so unlike the fifo
slopes this one is not noise.

That is roughly an order of magnitude, from two runs of the *same* render loop,
differing in source format and rate ratio. The obvious candidate is `soxr`
allocating its polyphase filter tables lazily and much larger at 7.35:1 than at
4.35:1, which would be a one-off step rather than a leak. **That is a guess and
was not tested.**

What makes it worth writing down rather than leaving in a table: extrapolated, it
is 60–110 MB over 24 hours for FLAC and **340–860 MB for DSD256**, and the second
of those is not a number a player can wave away. The first is fine. Same code, same
session, conclusion would have flipped with the format.

Both are extrapolations from a 100–300 second window, with no long run behind
them, so both are upper bounds rather than typical rates — and neither is shown to
stop. The honest summary is "slow growth, bounded by nothing we have measured,
one order of magnitude worse on DSD than on FLAC, cause unknown."

### The pattern

Three of these, plus a fourth where a spectrum tool printed the frequency
response of digital silence because its analysis window had landed in a fade-in.

> A number that never varies is as suspicious as one that varies wildly.

Which is the argument for the instrumentation that exists now: `NAE_TRACE` prints
`sourceAdvance`, `deviceWritten`, `fifo`, `produced` and `consumed` per call, and
`NAE_PipelineStats` returns the same as JSON so a test can assert on it. The
invariant is

```
sourceAdvance × deviceRate / sourceRate  ==  deviceWritten
```

When it holds, nothing was invented. When it broke, the row said which side was
lying:

```
src +8868   devWritten 38760   fifo 12114->10890   produced 37536   consumed 38760
```

0.201 s of source, 0.202 s of device audio. Before the fixes the same line read
`+893` against `8878` — the whole bug in one row.

Two more from the same period, both about the instruments rather than the engine:
a tool that printed four PASS lines having collected zero samples (#13), and a
negative control that had quietly stopped testing its detector because a canary
clamped the very growth it was meant to prove measurable (#14).

---

## Rejected on measurement: the two-stage DSD path

Worth recording because the reasoning was sound, the implementation was not
written, and the measurement killed it.

The proposal was to resample DSD through a 176.4 kHz intermediate — 352.8 → 176.4
as a clean 2:1 decimation, then 176.4 → 192 as a near-unity ratio — on the theory
that an integer decimation is cheaper and gives the resampler an easier job. It
is the mirror of what soxr already does internally (`cr.c:318`: it turns 147:80
into a 2× halfband interpolate plus 160:147).

Measured, on a real DSD64 recording via `dsd_spectrum`:

| band | RMS |
|---|---:|
| 0–20 kHz (audible) | −59.6 dBFS |
| 20–80 kHz | −66.3 |
| 80–150 kHz | −64.1 |
| 150 kHz–Nyquist | **−69.2** |

The ultrasonic band is flat to within 2 dB right up to the 176.4 kHz Nyquist,
with no decimation stopband visible. That is what DSD is supposed to look like
*before* it is filtered — but it means a 2:1 decimation first would fold
150–176.4 kHz down to |150 − 176.4| = **26.4 kHz**, straight into the audible
band, at roughly 15 dB below the music. The second stage cannot undo aliasing.

The single-stage 352.8 → 192 path is correct: soxr lowpasses at 96 kHz, so that
band is attenuated before folding rather than after.

**Nothing was changed.** The proposal was not "unproven", it was worse, and the
only reason that is known is that somebody measured the band first.

---

## Two things that are not bugs but are easy to get wrong

**A file is not a DSF because its extension says so.** FFmpeg has the DSD
*decoders* (`dsd_lsbf`, `dsd_msbf`) and the `dsf` demuxer but no DSF or DFF
*muxer* in this build, so FFmpeg cannot write a format it can read. Grepping for
`dsdiff` finds nothing, because the demuxer is called **`iff`** — which is what
made DSD look undecodable until a real file was available. `Decoder::describe()`
prints the actual decode path for exactly this reason: the codec name does not
describe it, and `dsd_msbf` in particular reports its *post-decimation* rate
(352 800 Hz) rather than the bitstream rate (2 822 400 Hz).

**A Chinese filename reaches the engine as mojibake** when passed through
PowerShell to a native binary, because the argument is encoded with the console
code page. The file is fine; copy it to an ASCII path to test it.

---

## Appendix: earlier spikes

## Bugs found and fixed during the spike

These are the point of the exercise. All three would have been much harder to
diagnose once wrapped in Tauri's event loop.

### 1. COM apartment torn down while handles were still live

`NAE_OpenExclusiveSine` called `CoUninitialize()` before returning its handle.
The handle holds `IAudioClient` / `IAudioRenderClient` interfaces created in
*that* apartment, so the apartment was being destroyed out from under objects
that were still going to be used. The symptom was an access violation inside
`IAudioClient::Start()` — hundreds of lines away from the actual mistake.

Fixed by making apartment lifetime explicit: `ensureCom()` initialises
per-thread and never tears down, and the host releases it via `NAE_Shutdown()`
only once every handle is closed.

**Rule for the engine:** the COM apartment must outlive every object created in
it. Never `CoUninitialize` from a function that hands back live interfaces.

### 2. MinGW's `IAudioRenderClient::GetBuffer` has the wrong arity

The real SDK declares `GetBuffer(UINT32, BYTE**, UINT32*)` and reports how many
frames you may write. MinGW declares it with **two** parameters and reports
nothing — the caller just gets back whatever it asked for.

`wasapi_compat.hpp` normalises this (along with `Start()` taking no
`REFERENCE_TIME`, and `GetService` being non-templated) behind
`getRenderBuffer()` / `startClient()` / `stopClient()`. The engine source is
now correct against either toolchain. **This shim is load-bearing** — deleting
it silently reintroduces a buffer-overrun on MinGW.

### 3. Concurrent exclusive opens are not reliably refused

The original test asserted that a second exclusive open on a held device must
fail. It succeeded. This is the driver, not the engine: exclusive mode is
*nominally* single-occupant but enforcement varies by driver.

The test now asserts the engine's actual obligation — that it reports the
outcome faithfully and never returns a half-built handle — and records which
behaviour this driver has. The UI still has to handle both.

## Hardening added after the first green run

These are not test failures; they are defects found by inspection once the
checks passed, each of which would have been expensive to diagnose later.

### 4. The DLL exported 8598 symbols

`nae.def` was being applied with `-Wl,--output-def`, which does **not** restrict
exports — it left the entire C++ implementation visible: mangled `std::*`
helpers, `openExclusiveSineImpl`, the COM plumbing. All of it was public ABI that
bindgen could bind to and none of it was meant to be stable.

The `libnae.dll.a` it produced was 2.9 MB of *real code* rather than import
stubs, so linking against it would have silently statically embedded the engine
into the host instead of calling the DLL — quietly destroying the boundary the
whole design rests on.

Fixed with a linker version script (`nae.map`, `local: *;`). Exports are now
exactly the nine `NAE_*` functions and the import library is 6.6 KB.

**Do not "simplify" this back to `--output-def`.**

### 5. `nae.dll` dragged in the whole MinGW runtime

It imported `libstdc++-6.dll` and `libgcc_s_seh-1.dll`, so the host app would
have had to ship and locate two extra DLLs — an easy way to ship a broken app,
especially inside a Tauri bundle where the DLL lives in a resource directory.

Now linked with `-static-libgcc -static-libstdc++` and the pinned
`--whole-archive -lstdc++ -lwinpthread` pair. `nae.dll` depends on `KERNEL32`
and `ole32` only, plus the system UCRT. This is only safe because the ABI
forbids exceptions crossing the boundary; if that rule is ever relaxed, these
flags have to go.

### 6. Exceptions could have unwound into the host

`NAE_EnumerateDevices` builds a `std::string` and could throw `bad_alloc` out of
an `extern "C"` function — straight into the caller's frames. `new
(std::nothrow)` does not help: it covers the allocation, not the constructor.

All nine exports are now `noexcept` on **both** the declaration and the
definition (bindgen reads the header, so the declaration is what matters), and
the two allocating exports wrap their bodies in `try`/`catch` and report
`NAE_ERR_NO_MEMORY`.

### 7. Valid bits were being invented, and it looked exactly like contention

`wrap()` set `wValidBitsPerSample` from `wBitsPerSample`, because
`readNativeFormat()` returns a `WAVEFORMATEX` — and that struct has no room for
a valid-bits field.

The development machine's default endpoint reports **32-bit container, 24 valid
bits**. Asking it for 32 bits of real data is something the driver cannot
supply, and it answers `AUDCLNT_E_ENDPOINT_CREATE_FAILED (0x8889000F)` — **the
same error Windows gives when another application is holding the endpoint.**

That is a nasty coincidence, and it cost real time. Exclusive mode was declared
impossible on a machine where it works fine; the UI grew a "this device is in
use by another application" message; the test suite learned to accept that
failure as expected. All of it was our own bug wearing a convincing disguise.

`format_matrix.cpp` settled it by brute force, and the result was unambiguous:

```
wrap() as implemented [excl]       0x8889000F  AUDCLNT_E_ENDPOINT_CREATE_FAILED
native blob verbatim  [excl]        0x00000000  S_OK        <- same device, same moment
48000 Hz / 16-bit / int [excl]     0x00000000  S_OK
96000 Hz / 16-bit / int [excl]     0x00000000  S_OK
192000 Hz / 16-bit / int [excl]    0x00000000  S_OK
```

If a single format succeeds exclusively, the endpoint is not busy. No amount of
hypothesising substitutes for running the matrix.

**The lesson worth keeping: when two very different failures share an HRESULT,
enumerate the possibilities instead of picking the plausible one.** The fix is
`readNativeFormat()` now also returns `wValidBitsPerSample` from the EXTENSIBLE
tail, and `wrap()` takes it as a parameter. The default endpoint went from
`exclusive: false` to **192000 Hz / 24-bit valid / 50.7 ms**, exclusive.

## Carried forward from the earlier WASAPI probe

Still true, still important:

1. **Never guess the format.** Read `PKEY_AudioEngine_DeviceFormat` and re-wrap
   it as `WAVEFORMATEXTENSIBLE`. Passing it through verbatim gives
   `E_INVALIDARG` for plain PCM; a hardcoded 48k/24 gives
   `AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008)`.
2. **`AUDCLNT_STREAMFLAGS_RAW` is rejected** with
   `AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021)`. Opt-in and probed only; the
   default path is polling mode (flags = 0).
3. **`GetMixFormat()` is meaningless in exclusive mode.** Report the format
   actually requested, not the one WASAPI suggests.
4. **Valid bits are not container bits.** The `WAVEFORMATEX` a naive read gives
   you cannot express "24 bits in a 32-bit container", and guessing costs you
   exclusive mode entirely. See finding 7.

