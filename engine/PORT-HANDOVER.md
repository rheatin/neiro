# Handover — macOS port, 2026-11-07

Read this before touching `engine/nae.cpp`, `engine/backend_coreaudio.cpp`,
`engine/output_stream_coreaudio.cpp`, or `base/`.

## Where things stand

```
Windows   engine:   nae_test all checks pass
          base:     base_probe "All Base checks passed."
          binding:  probe "All Rust binding checks passed."
macOS     engine:   builds, libnae.dylib, 109 exports, playback works
          base:     base_probe "All Base checks passed."
          transport: transport_check "All transport checks passed."
          binding:  probe "All Rust binding checks passed."
```

Both platforms are green on the same source, and `base_probe` was swept rather
than run once: 10 consecutive runs on macOS and 8 on Windows with no failure,
after the three ordering faults below were fixed. It used to fail roughly one
run in eight with two different symptoms.

The one remaining parity item is bit depth, which needs a DAC to verify.

## The defect that was fixed (keep the fix)

`base_probe` failed with `render_path.sample_rate` back at the device rate while
`policy.rate` still read `Fixed { rate_hz: 96000 }`. The policy field was a
symptom, not the fault: the wait predicate matched a snapshot published before
the reset had reopened, because the reset's open read a device that had not yet
been restored.

The root cause was the destructor requesting `setDeviceSampleRate(device,
restoreRate)` and returning. The HAL applies a nominal-rate change late, so the
next open read the rate the closing stream had set. `macos_play_check` at 44100
on a device at 48000 left the device at 44100; that is measurable with
`backend_check` and was the confirming observation.

The destructor now polls until the device reports the target rate (100 x 5 ms)
before returning. A close means the machine is back as it was found, not that a
request was sent. Do not turn that wait back into fire-and-forget.

### Retracted: "`~CoreAudioStream` never runs"

That conclusion was wrong, and it was wrong for a boring reason: the target was
not rebuilt, so the diagnostic was not in the binary that ran. With
`handle->stream.use_count()` printed it reads `1` and the destructor prints
`device 71 restoreRate 96000 hogged 0`. The lesson is the same one `nae_test`
already taught: confirm the binary is new before believing a negative result.

### Fixed: the restored rate now has to arrive

The destructor requested `setDeviceSampleRate(device, restoreRate)` and returned
immediately. The HAL applies a nominal-rate change late, so the next open read
the rate the closing stream had set -- which is how "switch back to Device"
resolved to the fixed rate. The destructor now polls until the device reports the
target (100 x 5 ms) before returning. Measured: after a play at 44100 on a device
at 48000, `backend_check` reports `native: 48000 Hz`.

`hogged 0` in that print is expected for the shared path; only an exclusive open
takes the device.

## What was fixed today and must not be undone

- **Rate settle before creating the AudioUnit.** Opening at a rate other than the
  device's raced the first IO start; the first run after a build failed, later
  runs passed. Waiting until the device reports the target rate twice fixed it
  (4/4 runs).
- **`hogSupported` is capability only** (`hogOwner` readable), not occupancy. A
  device held by another process is busy, not unsupported.
- **hog is gated on exclusive.** `shareMode` is plumbed through
  `StreamFormatRequest`; shared leaves the device alone so other programs keep
  playing. Verified: `hog_mode_check` reads `-1` from a separate process during
  shared playback.
- **Exclusive is `exclusiveSupported = 1`** on macOS: hog *is* this platform's
  exclusive. It does not claim bit-exactness; `bitPerfect` stays 0 with reason 7.
- **RAII guard around hog in `open()`** so every early return gives the device
  back.
- `.cargo/config.toml` keeps `CC`/`CXX` global because cargo has no per-target
  `env` table; other platforms override from the environment:
  `CC=/usr/bin/cc CXX=/usr/bin/c++ cargo check`.

## Tools that exist now

| Binary | What it answers |
|---|---|
| `hog_watch` | who holds the device, from a separate process (the only honest read) |
| `hog_probe` | can hog be taken at all, with the owner before/after |
| `hog_mode_check <wav> <hog_watch>` | shared is free, exclusive is not, released after close |
| `rate_set <hz>` | put the device back to a known rate (polls until it settles) |
| `macos_smoke` | the C ABI loads and answers |
| `macos_play_check <wav>` | open, play, position advances |
| `output_stream_check [s]` | frames consumed per wall-clock second |
| `backend_check` | device capabilities from the device |

## macOS build invocation

```sh
export PATH=$HOME/.rustup/toolchains/stable-aarch64-apple-darwin/bin:/opt/homebrew/bin:$PATH
export CC=/usr/bin/cc CXX=/usr/bin/c++
cd engine && cmake --preset macos -DNEIRO_WIP_ENGINE_MACOS=ON && cmake --build --preset macos
```

The engine target is opt-in so a work-in-progress failure never reddens the
default build.

## Platform facts that cost time to learn

- The HAL applies a nominal-rate change **late**; every read-back needs to poll
  until two consecutive reads agree. This bit three separate times (first open
  after a build, backend_check's per-rate loop, reading caps after a close).
- `kAudioDevicePropertyHogMode` returns the owner pid or -1; when *we* hold it the
  property is unreadable, and a forked child cannot read it at all. Only a
  separate `exec`'d binary gives a trustworthy answer.
- Code under `#[cfg(windows)]` in `base/src/lib.rs:42` means the Windows transport
  is not compiled on macOS at all. `src/transport/unix.rs` is the Unix
  counterpart, wired with `#[path]`.
- The recorder `base_probe` writes its own fixtures (12 s); a run that takes
  longer than that ends the track and every later wait times out.

## Resampling on macOS was never wired

The macOS open path computed `resampling` from the two rates and reported it, but
never called `resampler.prepare`. `handle->resampler.ready()` stayed false, so
samples went to the device untouched while the signal path claimed they had been
converted. A 500 ms tone at a 96 kHz device finished in 265 ms, which is how it
was found; the NAE_TRACE line said `resample=0` on an open that had asked for
44.1k into 96k.

Both the file and the tone path go through the fifo, so the single prepare at
open fixes both. Measured after: `resample=1`, 46403 device frames for a budget
of 48000, 511 ms of wall time.

The `didResample` out-parameter now reports `handle->resampling` rather than
recomputing it from the rates. A value that is reported must be the value that
was configured.

## Licensing: the default link is LGPL

`engine/CMakeLists.txt` now prefers `vendor/ffmpeg-lgpl` over the system package
and refuses to configure against a tree that enables GPL or nonfree
(`NEIRO_REQUIRE_LGPL`, on by default). Both system packages are GPL builds, so
without the vendored tree configure fails with the command that fetches one.

- Windows: `scripts/build-lgpl.ps1` downloads the BtbN LGPL shared build, verifies
  its configuration string, installs it in `vendor/ffmpeg-lgpl`, builds and reads
  the licence back. Measured: `"ffmpegLicense":"LGPL version 3 or later"`.
- macOS: `scripts/build-ffmpeg-lgpl-macos.sh` builds FFmpeg from source with
  `--disable-gpl --disable-nonfree` into the same directory (111 s on this
  machine). Measured: `"ffmpegLicense":"LGPL version 2.1 or later"`.

`--disable-programs` must not be passed: the configure step reads the licence out
of `${prefix}/bin/ffmpeg`, and a tree with no executable cannot be checked.

A build directory configured before the vendored tree existed keeps its cached
`NEIRO_FFMPEG_ROOT`; the verification scripts pass the vendored path explicitly
for that reason. A stale cache now fails loudly instead of shipping a GPL dll.

### The Windows backend is real now

`backend_wasapi.cpp` is a backend: `WasapiOutputStream` owns `IAudioClient`,
`IAudioRenderClient`, the render event and the granted format, and nothing in it
reads `NAE_Handle`. `openWasapiStream(want, &opened, &error)` mirrors
`openCoreAudioStream`, and both `openExclusiveImpl` branches are now the same
shape: build a `StreamFormatRequest`, call the platform factory, fill the handle
from what came back.

`NAE_Handle` lost its `ComPtr` members and the `#ifdef _WIN32` around `stream`.
The adapter structs that chose between `handle->client` and `handle->stream` are
gone; the render loop has one device path on both platforms.

The Windows helpers that both the engine and the backend need -- `ensureCom`,
`deviceIsGone`, `remember`, the native-format read -- moved to
`engine/wasapi_device.h`, so there is one COM apartment flag and one definition of
"the device is gone" rather than two copies that can drift.

Two things the port had to get right, both found by running the checks:

- `SetEventHandle` must be called **after** `Initialize`, not before. The first
  version had it before, WASAPI refused it, and the stream silently fell back to
  timer pacing -- playback worked but was slower than realtime.
- On the endpoint this machine has, an event-driven *exclusive* `Initialize` is
  itself refused (`AUDCLNT_E_BUFFER_SIZE_ERROR`). The client is rebuilt without
  the flag and the stream runs the timer path; that fallback is explicit and
  documented rather than an accident.

The pacing a stream got is now reported rather than assumed: `OpenedFormat`
carries `eventDriven` and `pacingNote`, and an open under `NAE_TRACE` prints
`pacing=event` or `pacing=timer (reason)`. On this machine it prints
`pacing=timer (the endpoint refused an event-driven stream)`, which is the
answer to a question that used to have none: before this, a silent fallback to
timer pacing looked exactly like a healthy run.

### What the Windows backend slice actually requires

Not a file move. On Windows `NAE_Play(handle, ms)` calls
`IAudioRenderClient::GetBuffer`, fills it from the fifo, and releases it -- the
engine owns the pacing. On macOS the device pulls: `AudioUnit` calls the render
callback and the engine fills what it is handed. The CoreAudio backend can
therefore own the AudioUnit and hand the core a `BackendStream`, and the core's
`NAE_Play` becomes "ask the stream for a slice".

To make Windows look the same:

1. Move the WASAPI open (the 400-line branch of `openExclusiveImpl`) into a real
   `WasapiBackend::open`, returning a `BackendStream` that owns
   `IAudioClient`/`IAudioRenderClient`, the event handle and the format.
2. Give that stream a `write()` that does what `NAE_Play` does today: wait for
   the period, get a buffer, fill from the fifo, release.
3. Change `NAE_Play`'s Windows path to call `handle->stream->write()`, which is
   what the macOS path already does through the adapter struct in `nae.cpp`.
4. Delete the Windows branch of `openExclusiveImpl` and the `ComPtr` members of
   `NAE_Handle` (they are in the header under `#ifdef _WIN32`).

Step 3 is the one that can regress audio silently: the loop's timing, the
device-lost path and the underrun accounting all live there, and `nae_test` plus
`mode_check` are the instruments that would notice. Do it with `nae_test`
running after every step, not once at the end.

## Check coverage is now the same on both platforms

`dither_check` was the only one of the arithmetic checks that needed Windows (it
includes `wasapi_compat.hpp` for `placeInContainer`). The rest -- `resample_check`,
`loudness_check`, `linear_phase_eq_check`, `partitioned_convolver_check`,
`dsd_downrate_check` -- were defined inside the Windows-only section, so a macOS
build compiled the DSP and ran nothing against it. They are now in the
cross-platform section and all five pass on macOS.

## Verification and CI

`scripts/verify.ps1` and `scripts/verify.sh` run the same steps on both
platforms: configure, build, the device-free checks, Base's `transport_check` and
`index_probe`, and the binding build. `-WithDevices` / `--with-devices` adds the
checks that need an endpoint. `.github/workflows/windows.yml` and `macos.yml` call
the scripts; a runner has no audio device, so CI runs the device-free set only.

The compilers are passed explicitly by the scripts. A scoop MinGW has no
`gcc.exe` -- its driver is `cc.exe` -- and CMake's `CC` environment handling
reports the missing path as "the C compiler is not a full path", which reads as a
toolchain problem rather than a name problem.

## The platform split, first slice

`NAE_Handle` was 627 lines at the top of `nae.cpp`; it is now
`engine/nae_internal.h`, together with the seam constants and `StreamFormat` that
sat beside it and the `Thread_local` error string.

`engine/open_coreaudio.cpp` holds the macOS device open and
`NAE_GetDefaultDeviceId`. It is compiled into the macOS target only, so a change
there cannot reach the Windows build, and the compiler says so rather than a
reviewer.

What remains in `nae.cpp` under `#ifndef _WIN32` is small and is left for a
reason: the JSON half of `enumerateDevicesImpl` (it shares the allocation tail
with the Windows half), and the adapter struct that turns `neiro::OutputStream`
into the status the shared render loop reads. Both are in-function branches, not
backends.

The Windows "backend" is gone. `backend_wasapi.cpp` implemented `AudioBackend` by
calling this DLL's own exports, and its stream was a thread pumping `NAE_Play`; it
was linked into `nae` although the engine never asked for it, and `backend_check`
was its only consumer. The adapter now lives in `backend_check.cpp` under
`#ifdef _WIN32`, and `nae` links `nae.cpp` + `session.cpp` again. What that
removes is an object that made the interface look implemented on Windows; what it
does not do is move the WASAPI device code, which is still in `nae.cpp`.

Found while doing it: `NAE_GetEngineCapabilities` still filtered the share-mode
list down to `shared` on macOS, with a comment claiming `exclusiveSupported` was
0 there. That stopped being true when hog mode became this platform's exclusive,
and the effect was that the engine advertised the one mode a host could not be
told to use while the device list said the device had it. Both modes are listed
now.

## Review round (blue team), and what was fixed

Two scoped reviews were run: the macOS backend plus the licence gate, and the
Base browsing index. Every finding below was checked against the code before
anything was changed; the ones that did not hold up are listed too.

Fixed in `engine/backend_coreaudio.cpp`:
- `resume()` did not set `running`, so a later `stop()` skipped
  `AudioOutputUnitStop` and the destructor disposed a unit the HAL was still
  calling. It now restores the flag when the start succeeds.
- `supportedShareModes` was overwritten with shared-only a few lines after both
  modes were set, so a host reading the list could never ask for exclusive while
  `exclusiveSupported` said 1.
- `std::stoul` on a device id threw out of a C ABI boundary; it is now parsed
  with `strtoull` and a full-parse check, and a bad id is an error string.
- `deviceAlive` conflated "the device is gone" with "the property could not be
  read", so one transient failure could latch `lost` for the life of the stream.
  It now reports whether it could answer.
- A listener that could not be installed was silent; it now says so under
  `NAE_TRACE`.
- `queryCaps`'s settle loop ended on `0 == 0` (a device mid-change reports 0) and
  always slept 5 ms even when nothing moved; it now needs two equal non-zero
  readings.
- DoP carriers were filled from the rate list and then the capability was
  switched off, leaving a host reading carriers with the flag cleared. Both are
  now cleared, because this route is float32 only.
- The destructor restored the sample rate while the AudioUnit still held the
  device. It now disposes the unit first -- the reverse of the order `open`
  builds it.

Fixed in `engine/CMakeLists.txt`:
- An unreadable `ffmpeg` (empty output, non-zero exit) was treated as "not GPL".
  It is now fatal: an unknown licence is not a licence.
- The gate read a cached `find_program` result, so a build directory configured
  against the system package kept reading that binary after the root moved. The
  evidence path is resolved fresh, without caching.
- The CLI's `libavcodec` major is now compared with the library file that would
  be linked, so a binary copied into another tree cannot vouch for it.
- The status line names the root actually used, and says so when a vendored tree
  exists but a cached root is in force.

Fixed in Base:
- A renamed file kept its `tags` row under the old path; `rewrite_path` now
  moves it.
- A deleted file kept its row forever; `apply_scan` now deletes it.
- The four browse queries returned an empty list when Base had no library, which
  the file's own contract forbids; they now report an error.
- `list_artists` counted albums with binary collation while `list_albums`
  grouped case-insensitively.
- Search folded only ASCII; a `search_text` column folded in Rust is what a query
  is matched against now.
- `index_tags` could never refresh a path it already had; it forgets first.
- Rows in flight at shutdown were dropped; there is now a bounded final drain.
- The probe tagged through Base, so the scan's indexing path was never
  exercised. It now tags with lofty directly, scans, and only then retests the
  write path.

Not fixed, and why: the gate still authenticates a tree by its CLI. Comparing
the library's major version closes the obvious mismatch, but a tree assembled by
hand from mismatched parts cannot be detected from a version number. That is a
process problem, and the build script is the process.

## Base: the browsing index

The library now holds what a list needs: title, artist, album, album artist,
disc/track number, year, genre, duration, sample rate, bit depth and channels.
Before this, an album view had to open every file on every launch -- ten thousand
tracks meant ten thousand opens before the first row.

- `list_tracks`, `list_albums`, `list_artists`, `search`, `index_tags` commands;
  `tracks_listed`, `albums_listed`, `artists_listed`, `search_results`,
  `tags_indexed` events.
- Filled by the scan (added and modified files) and by a tag write, which drops
  its row and asks for it again. One worker thread: a header read, and two of
  them would make a spinning disk seek between them.
- Search is `LIKE`, not FTS5: the bundled SQLite may not carry FTS5, and a search
  that silently returns nothing is worse than a scan.
- Measured on both platforms: `index_probe` writes three tagged WAVs, scans,
  and gets three tracks, two albums, two artists and one search hit, with
  durations read from the file rather than the tags.
- Two defects found while checking the first version: a file the scan reported
  as *modified* was skipped because it already had a row (so an external retag
  never showed up), and `%` in a search was passed straight to `LIKE` as a
  wildcard. The scan now forgets the row of a modified file before indexing it,
  and the query escapes `\`, `%` and `_` with an `ESCAPE` clause. The probe
  covers the second: a `%` query returns exactly the one title that contains it.

## Base: a queue edit could strand the session

`open_at` closed the session first and only then looked at `queue[index]`. When
the index was past the end -- `skip` at the last entry does `open_at(index + 1)`
-- it set `index = None` and returned. Base was then left with no session and
nothing published, and `poll` returns immediately without one, so the snapshot
froze at the last position and every later command that needed a session did
nothing. The probe sat on a frozen `position 5.208458049886621` and timed out on
an enqueue that had been accepted and could never run.

The lookup now happens before the close, and the empty case publishes `Stopped`
rather than leaving the host looking at a dead snapshot.

## Base: progress could go backwards

`Command::Enqueue` pushed the batch, sent every path to the analysis pool, and
*then* emitted `PreanalysisProgress { done: 0 }`. The workers publish from their
own threads, so an entry that was already cached could emit `done: 1` before the
control thread got to its own `done: 0`. The comment claimed "they are ordered by
the bus"; the bus is a channel, and two threads writing to one channel have no
order. `base_probe`'s monotonicity assertion caught it once in about ten runs on
Windows with `progress went backwards: 0 after 1`.

The zero is now emitted before any work is queued, which is the only order that
cannot invert.

The same class of fault had a second site: `analysis::finish` computed `done`
under the batch lock and emitted the event **after dropping the guard**, so two
workers could compute 2 and 3 in that order and publish 3 then 2. The event now
leaves while the lock is held; `emit` only queues, so holding the lock across it
costs a channel send.

## Binding probe

`binding` links against `libnae.dylib` on macOS (see its `build.rs`) and
`cargo run --bin probe -- <file>` passes there. Three of its checks encoded
Windows-only expectations and were not defects in the engine:

- `bit_perfect` for Native mode: CoreAudio here has one route, float32, so an
  integer file is converted and bit-perfect is impossible at any rate. That is
  the bit-depth gap; the probe now asserts `false` on macOS and `true` on
  Windows.
- The capabilities check looked for the string `wasapi`. It now looks for the
  backend of the platform it is running on.
- The probe's tag check needs a file that has tags; `build/g1.wav` does not.

## Base transport

The Windows transport is `transport/mod.rs` (gated `#![cfg(windows)]`), the Unix
one is `transport/unix.rs`; `lib.rs` selects between them with `#[path]`. The
serve loops are duplicated on purpose: the Windows one is built around a
non-blocking `PeekNamedPipe` pump and the Unix one blocks on the socket read with
a timeout, so sharing them would mean threading a trait through working code to
save about sixty lines.

`transport_check` passes on both. Two defects it found on Unix, both real:

- The check hardcoded `\\.\pipe\neiro-base-transport-check`. On Unix that is a
  file whose name contains backslashes, so the socket landed in the working
  directory. The name is now derived from `default_pipe_name()`, which is a pipe
  path on Windows and a socket path on Unix.
- `Transport::stop` unlinked the socket unconditionally, and `Drop` calls `stop`.
  `server = Transport::start(...)` binds the new socket *before* the old value is
  dropped, so the old `stop` deleted the socket the new server was listening on.
  Two changes fix it: stop is idempotent (the `thread.take()` that returns `None`
  means it already ran), and a per-path count keeps the file until the last
  server for that path goes away.

## Still not done

- `NAE_QueryPeaks` does not exist any more. It appears only in comments that
  explain why it was replaced by a path-based call; there is no declaration, no
  definition and no export entry. An earlier note claiming it was declared but
  undefined was wrong.
- **Bit depth on macOS, the last parity item.** The route is float32 only, so an
  integer file is converted and `bit_perfect` cannot be true at any rate. The
  fix is to open the device with an integer stream format when the source has
  one; the built-in output advertises exactly one physical format (f32/32), so
  the change cannot be verified here. `backend_check` prints what the endpoint
  advertises and is the instrument for the day a DAC is available.
- `Stop` deliberately keeps hog: only a close gives the device back.
