# test/

Audio fixtures for the engine's checks. Not in the repository — see below.

## What lives here

Files dropped in for manual testing. Ignored by `.gitignore`; nothing in the
build depends on this directory.

## The DSD sample

`01.你的背包.dff` — DSD64, stereo, 4:00.60. Needed by:

```powershell
.\spike\audio-engine\build\decode_check.exe  test\01.你的背包.dff
.\spike\audio-engine\build\dsd_spectrum.exe  test\01.你的背包.dff --seconds 20
```

It is 162 MB of one-bit audio, which is why it is ignored rather than committed.
A file that size makes every clone slow and every history operation expensive,
and it is a recording rather than something this project produces.

**Copy it to an ASCII path before passing it to a build tool.** A Chinese
filename reaches the engine as mojibake when PowerShell hands an argument to a
native binary, because the argument is encoded with the console code page. The
file is fine; the path is not:

```powershell
Copy-Item -LiteralPath "test\01.你的背包.dff" "$env:TEMP\opencode\d64.dff"
```

## Generating DSD instead

`scripts/make-dsd-fixture.mjs` builds a DSD64 file from a sine, so a DSD test does
not strictly need a recording:

```powershell
node scripts\make-dsd-fixture.mjs out.dsf 2 2822400
```

It currently **fails on purpose**. The generator probes its own output with
`ffprobe` and exits non-zero if the file cannot be read, because a generator that
writes something nobody can decode is worse than none — it looks like coverage
and the failure surfaces somewhere unrelated later. This FFmpeg build has the DSD
*decoders* and the `dsf` *demuxer* but no DSF or DFF *muxer*, and the `dsf`
demuxer rejects a hand-built file with `block_align invalid is not implemented`.

So until that is resolved, a real recording is the only way to exercise DSD, and
that is also the more honest test: a synthetic sine proves the plumbing, a
recording proves the decoder.
