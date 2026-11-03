# AGENTS.md

Rules for changing this repository. The UI has been removed; this is the audio
engine. `README.md` says what is here, `spike/audio-engine/README.md` says how
the engine behaves, and `spike/audio-engine/LESSONS.md` records what went wrong
on the way there. Read those before touching the render loop.

The rules below are the ones that have already been broken at least once, with
the measurement that showed it.

---

## 1. A value has exactly one owner

**Never re-derive a value from a second source to decide what to report.**

- The device is the authority on its own format. The engine negotiates it and
  reports the result; a host must not cache a format and present it as current.
- `bit_perfect` is computed once, in the engine, and is deliberately
  conservative: a channel conversion counts as processing. The host displays the
  flag and the reason code; it does not recompute either.
- Position comes from device frames written, not from wall-clock time or from
  the host's own counter. Two consumers of a position must call the same
  function.
- A rate the engine asked for and a rate the device granted are two fields for a
  reason. Collapsing them hides a coercion.

Ask of every new field: *if two places displayed this, would they agree?* If the
answer depends on which place, it belongs in one function.

## 2. A probe that cannot fail is worse than no probe

Every check must have been seen **failing against the real bug**, and must fail
fast.

- **Measure a consequence, not a callback having fired.** "The playhead is at
  the line's timestamp", not "the handler ran".
- **Wait for the state you are measuring**, with a bounded budget — not a fixed
  sleep. A seek is asynchronous; reading `position()` right after it returns the
  pre-seek value.
- **Report a timeout as a failure**, with the raw payload.
- **If a key can be `undefined`/null, assert it exists first.** `null > -80` is
  `true` in JS; the equivalent C++ mistake is a zeroed field that reads as a
  valid measurement.
- **A test signal has to be shown to reproduce the fault.** A -20 dBFS tone
  cannot answer "is there a click"; a 0 dBFS sine answers a different question.
- **Leave the device and the process as you found them.** Close every handle,
  join every thread, release the COM apartment.

## 3. Two consumers of one measurement go through one function

`localDynamicRangeDb` filtered on frames the tap had produced; `currentLevelDb`
did not. Same envelope, same moment, two answers. When a value is computed in
more than one place, extract it and let both call it. The duplicated version is
always the one that gets forgotten.

## 4. When the source cannot answer, say nothing

- A meter that cannot be measured draws nothing — not `--`, not a plausible
  number. A held reading expires when its basis moves (track change, seek,
  device change).
- A short render is a failure, not a success with less audio.
- A device that is gone is `NAE_ERR_DEVICE_LOST`, not a generic open error.
  Playback pauses; the handle, the decoder and the position stay valid.
- **No device is chosen on the caller's behalf.** A play carrying no device id
  can only land on the endpoint used last; if it is gone, that is device-lost,
  never a different endpoint.

## 5. Constants and layouts are exported, not restated

- `output_mode.h`, `nae.h` and the Rust `ffi.rs` describe the same structures.
  Every mirrored struct must pin its neighbours with `static_assert`; a
  total-size check cannot catch two adjacent doubles swapped.
- Layout constants live in one header and are read by both the producer and the
  consumer. A probe that writes the constant into itself measures itself.

## 6. The C ABI contract

From `nae.h`, and the only things to get right:

- **No exceptions cross the boundary.** Every export is `noexcept`; an escaping
  C++ exception into Rust is undefined behaviour. Allocation failure is
  `NAE_ERR_NO_MEMORY`.
- **Memory belongs to the side that allocated it.** Strings from the engine go
  back through `NAE_FreeString`; handles go back through `NAE_Close`, exactly
  once.
- **The export list is a link input.** `nae.map` / `nae.def` / the Rust `ffi.rs`
  are three copies of one list. A missing entry is not a link error inside the
  DLL — it links, and simply is not in the export table, so the host fails at
  load. Ninja does not track `nae.map` as a link input; a forced relink is
  needed after editing it.

## 7. Verify on the running engine

`cmake --build` passing is not verification. Run the checks against the real
endpoint:

```powershell
cd spike/audio-engine/build
.\nae_test.exe
.\resample_check.exe
.\dither_check.exe
.\mode_check.exe
.\reopen_check.exe
```

The Rust binding is the cross-language ABI probe:

```powershell
cd spike/audio-engine/rust
$env:RUSTUP_TOOLCHAIN = "stable-x86_64-pc-windows-gnu"
cargo run --bin probe -- <file.flac>
```

If `nae.dll` is held by a running process, the link fails. Stop it first.

### Toolchain

There is no MSVC `link.exe` here. Rust uses
`stable-x86_64-pc-windows-gnu`; the C++ side uses the scoop MinGW. The current
MinGW-Builds `16.2.0-rt_v14` is a UCRT toolchain, so `CMakeLists.txt` selects
`-mcrtdll=ucrt` when `libucrt.a` exists. Without it, folding `libwinpthread`
into the DLL fails on `__intrinsic_setjmpex`.

A function is only reachable if it is in all three export lists. The DLL is
loaded by name; an ordinal-only DLL cannot be used by the Rust binding.

## 8. PowerShell 5 will silently rewrite your UTF-8 files

`Get-Content` without `-Encoding` reads a BOM-less UTF-8 file as the system ANSI
code page; `Set-Content -Encoding UTF8` writes it back with a BOM. Every CJK
character and every em dash changes. Nothing errors, the build still passes, and
the diff looks mangled on both sides because the console decodes it the same way.

Run `scripts/check-encoding.ps1` before committing anything a PowerShell script
touched. The safe pattern is to decode and encode explicitly:

```powershell
$utf8 = New-Object System.Text.UTF8Encoding($false)
$t = $utf8.GetString([System.IO.File]::ReadAllBytes($p))
# ... change ...
[System.IO.File]::WriteAllBytes($p, $utf8.GetBytes($t))
```

Or use the editor, which is byte-safe. **Write ASCII only in `.ps1` files** —
`powershell -File` reads a BOM-less script as ANSI too, and a backtick in a
double-quoted string is an escape, not text.

## 9. Licensing

The current FFmpeg shared build is **GPL**. Linking it makes `nae.dll` GPL: fine
for development, not fine to distribute. The player needs none of the GPL
components; switching to an LGPL build is a link-time change, and nothing above
it moves. `soxr` is vendored under LGPL 2.1.

## 10. No UI in this branch

There is no web view, no Tauri shell, no GPUI. If a change needs pixels, it goes
in a separate host through the same C ABI. Do not add a UI framework to this
build graph.
