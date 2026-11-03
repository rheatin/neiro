# Resampling — result

**Status: working.** soxr converts cleanly at every ratio tested, with zero
frame-count drift.

```
All resampling checks passed.
exit=0
```

| Conversion | Frames | Drift |
|---|---|---|
| 48000 → 48000 (identity) | 48000 | 0 |
| 44100 → 48000 (CD → device) | 48000 | 0 |
| 48000 → 96000 | 96000 | 0 |
| 48000 → 192000 | 192000 | 0 |
| 192000 → 44100 | 44100 | 0 |
| 44100 → 192000 (the 480x case) | 192000 | 0 |
| 96000 → 44100 | 44100 | 0 |

Each case also checks that the 1 kHz test tone survives at its input level
(measured with a Goertzel filter, not judged by ear) and that the output stays
inside full scale.

## Why soxr, vendored

It is what the reference engine uses, via `libswresample`'s
`resampler=soxr` with precision 20 and 28 — the `SoxrHq` / `SoxrVhq` tiers. It
is LGPL, so vendoring keeps the project permissive. See `third_party/soxr`.

Vendored rather than fetched so the build needs no network and CI is
reproducible. Three things had to be worked around, all recorded in
`CMakeLists.txt` and `cmake/SetSystemProcessor.cmake`:

- `cmake_minimum_required(VERSION 3.1)`, which CMake 4 rejects — handled with
  `CMAKE_POLICY_VERSION_MINIMUM`, not by editing vendored source
- `include(SetSystemProcessor)`, a module CMake 4 removed — handled with a shim
  in our own tree, so a future soxr upgrade arrives as a clean upgrade
- `BUILD_TESTS` / `BUILD_EXAMPLES` / `BUILD_LSR_TESTS` are the option names it
  actually tests, not `SOXR_`-prefixed ones

**OpenMP is off.** soxr defaults it on, which is wrong here twice over: it drags
in a `-fopenmp` runtime dependency, and a resampler that picks its own thread
count can oversubscribe the machine mid-render.

## Three bugs the resampler test caught

This component produced the most instructive failures in the project, and all
three were invisible to listening.

### 1. Discarding 92% of the audio

The first `process()` loop exited early when `produced` came back below the
scratch capacity, on the reasoning that a short output meant soxr had run dry.

It had not. A resampler with a filter produces fewer frames than requested until
its delay line is full — that is the normal steady state. A 44100 → 48000
conversion returned **3566 of 48000 frames**.

The reason this is dangerous: the tone still measured correct, because what
survived was still a clean 1 kHz sine. Only the length was wrong. A player
would have played 7% of each track at the right pitch and nobody would have
known why.

### 2. Then losing the tail

Removing the early exit got the count to 47300 of 48000. The remainder is the
filter's held-back tail, which needs end-of-input to be released.

### 3. Feeding zeros instead of signalling end-of-input

The obvious fix — loop feeding zeros until the drain finishes — is wrong, and
the failure is worth recording: it produced **40 million frames of silence**
before the iteration guard caught it. Feeding zeros is supplying *more real
audio*, so soxr has no reason to stop asking for more.

soxr's header says it plainly:

> End-of-input (i.e. no data is available nor shall be available) may be
> indicated by setting `in` to NULL.

Two iterations, one correct. `process()` now runs the real input, then a flush
pass with a null pointer, and stops when the flush produces nothing.

## The rule these three share

All three were found by **measuring the output**, not by reasoning about it:

- a Goertzel filter for frequency and level
- an exact frame count for length

A resampler is the easiest component in this project to break in a way that
still looks correct. Every intermediate version above passed a "does it produce
sound at the right pitch" check. Only the length check separated them.

## Not done yet

- **Dither.** soxr works in floating point; converting to the device's integer
  depth with TPDF dither is a separate stage and does not exist yet. Decided:
  dither follows the output depth automatically rather than being a user toggle,
  because a user who turns it off hears quantisation noise without being told
  why.
- **The three output modes** (native / device-aligned / fixed rate) are not
  wired into the engine yet. `Resampler::prepare()` takes explicit in/out rates,
  so the policy layer above it is still to come.
- **Streaming.** `process()` currently buffers a whole call's output. The render
  path needs the incremental `soxr_delay` accounting so a ring buffer can be
  refilled without a per-block allocation.
