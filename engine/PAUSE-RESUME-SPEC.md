# Pause, resume, seek and stop: what has to hold, and how it is checked

Written before the work, so the acceptance criteria are not written after the fact
to match whatever the code happens to do.

**Correction, made before anything was built.** The first draft of this assumed a
pause that keeps the stream running. There is not one. `src/scripts/app.js:222`
says it outright: "This engine has no pause, so `stop` is how a pause is
expressed." So the boundary that actually exists is **stop, then play**, and the
client is stopped -- which flushes the device buffer -- rather than left running
with nobody filling it. The risk the bubbling fix introduced does not reach the
application, and pretending otherwise would have meant building instruments for a
path nothing takes.

**Correction to that correction, after the measurements.** The paragraph above is
wrong in its conclusion and it was wrong in an expensive way: it assumed the
stop→play boundary was harmless. It is not. `NAE_Pause` and `NAE_Resume` now
exist, and the whole reason they exist is that a pause expressed as a stop is
audible on this endpoint. The premise was not "the wrong path is being taken", it
was "the path being taken is fine", and that was never tested.

So the two corrections are not equal. The first was a reading of the code, and it
was right about what the code said. The second is a measurement, and it should
have come before the argument rather than after it.

## What the boundary actually is

Measured on the exclusive route, 100 cycles each, 300 ms per segment, same file:

| what the cycle does | artefacts |
| --- | --- |
| `NAE_Play`, then `NAE_Stop` | one per boundary, audible from the first |
| `NAE_Play`, `NAE_Stop` never called | none |
| `NAE_Play`, then `NAE_Pause` (silence fed, stream left running) | none |

Only the middle and bottom rows differ by whether the client is stopped. The
samples the engine writes are continuous across that boundary -- the seam ratio
read 0.88 against a reference derived from the signal's own slew, and 0.73 with
the stop removed -- so the discontinuity is produced by the device rather than
handed to it.

**A pause must not be a stop on this endpoint.** That is the finding, and it is a
property of the device rather than a preference.

## The prediction that came out wrong

Written before the instrument existed, so that it could come out wrong:

> `NAE_Play` prefills the device buffer with silence and starts. The audio is
> discarded by the stop, so the stream resumes **at whatever phase the decoder
> had reached**. If that phase is non-zero, the first sample out of silence is a
> step, and there is a click at every stop and resume.

It came out wrong. An 8 ms fade-in on resume, built for exactly this, changed
nothing, and so did gating the silent prefill to the first start of a stream. The
seam instrument could not see the prefill seam at all -- the prefill goes out
through a `ReleaseBuffer` that never touches the fifo -- and a fade aimed at a
step that is not being heard is a no-op.

Both changes were kept, because both are correct on their own terms: silence
before the first sample of a track is right, and silence in the middle of one is
not. They are not the fix, and shipping them on the strength of a hypothesis that
came out wrong would have been the same mistake in a new place.

## How a pause is expressed

`NAE_Pause` keeps the client running and has the render loop hand the device
`AUDCLNT_BUFFERFLAGS_SILENT` every period. `NAE_Resume` goes back to audio and
arms the 8 ms ramp -- which now has a real job, because the device has just been
handed nothing but silence and audio resumes out of actual zero.

The host must keep calling `NAE_Play` while paused. `silencing` only has somewhere
to take effect inside the render loop, and a started client that nobody fills runs
dry. A pause is therefore `NAE_Play(gapMs)` with the handle paused, not `Sleep`.

The silence branch sits **before** `topUp` on purpose. `topUp` is the only thing
in the loop that consumes source, so a pause that decoded would move the position
-- a 30 s pause would drop 30 s of the track and resume somewhere the listener
never asked for. That fault has no artefact to announce it.

## The invariants

**Accounting does not move across a pause.** Checked, not assumed: across 100
pause/play cycles, `NAE_Position` advanced by **0.0000 ms** while paused, and
`silenceFramesWritten` reached 1480399 frames, about 30.8 s, matching the 30 s of
pause that was asked for. Those two figures together are the evidence: the pause
fed silence rather than doing nothing, and it consumed no source.

Silence frames are counted apart from `deviceRateActual` on purpose. The first
attempt folded them out of the numerator only, and the rate duly read 24024
against a negotiated 48000 -- exactly half, the arithmetic of half the run having
been paused. A pause reported as playback at half speed is a wrong number in the
same way the merged version would have been: it claims something about playback
that did not happen. Folding silence into the numerator instead would hold the
figure at 48000 through a pause, which is worse, because it hides the pause. So
the silenced *time* is excluded from the rate window as well, and the rate means
"frames of real audio per second of time actually delivering real audio".

**No click at the pause boundary.** Covered by the table above: three variables,
one difference, the answer was clean.

**No stale audio after resume.** *Not yet checked.* There is no longer a device
buffer to leak, because the client is never stopped, so the original version of
this invariant no longer describes the risk. The live version is: does audio
resume promptly after a long pause, and is the first buffer the ramp rather than
the fifo's contents. The fifo is deliberately left untouched during a pause, so
whatever was decoded before it is what comes out.

**Seek while paused, and track change while paused.** *Checked now — see below.*
These were new risks that did not exist when this was written: the fifo holds audio
decoded before the pause, so a seek issued while paused has to replace it, and a
track change while paused has to replace it. A stale-buffer leak would be inaudible
in a level meter and obvious in the ear, so the check compares **position against
content** rather than either alone — a playhead that says it is at the new place
while the audio coming out is the old place is exactly the failure, and position
alone cannot see it.

Measured by `pause_edge_check`, on `1-0009767734.flac` (295.8 s) and
`07. Kikoemasuka.flac` (220.3 s):

*Seek while paused*

    暂停期间喂了静音            0 -> 19798 帧
    暂停期间位置不动            3133.2 -> 3133.2 ms
    暂停中 seek 被接受
    seek 之后位置落在目标附近   目标 177497 ms，实测 177497.4 ms
    恢复后从 seek 处前进        177497.4 -> 179568.8（前进 2071.4 ms）

**The fifo is replaced.** The 2071 ms of advance is measured from the seek target,
not from the 3133 ms the pause sat at — and 2 s of requested playback came out as
2071 ms of advance.

*Track change while paused*

    A 播到 2064.6 ms 后暂停
    B 打开时位置                0.0 ms
    B 播出了音频                2000 ms
    B 的位置只由自己决定        B 播到 2080.6 ms（A 停在 2064.6）

The two numbers being nearly equal is the point: B's playhead is unaffected by
where A stopped. Had the paused handle's fifo leaked into the new stream, B would
have started at A's position.

Run serially, not with both files open at once: two simultaneous exclusive clients
give `0x8889000A`, which is a property of this endpoint rather than of the engine.
The application closes before it opens too.

**Silence onset after stop.** Still true and still unmeasured: `NAE_Stop` remains
the way a stream ends, it still flushes the device buffer, and it is still audible
on this endpoint. Pausing no longer costs a boundary; stopping, changing track and
exiting all still do. That is a real remaining cost and not a solved problem.

## The stress loop

- 100 stop and play cycles back to back -- done, one artefact per cycle
- 100 cycles with the stop removed -- done, clean, this is the control
- 100 pause and play cycles -- done, clean, position unmoved
- seek immediately after a stop -- **not done**
- a track change immediately after a stop -- **not done**
- seek immediately after a pause -- **done**, see the invariants section
- track change immediately after a pause -- **done**, see the invariants section
- play immediately after stop with no gap at all -- done as the control above

## Priority, and why

**The spectrum comes before the truncated tail.** The tail costs 8-59 ms at the
very end of a track and is close to inaudible. The spectrum on the exclusive route
has no analyser behind it at all, so the interface may be drawing something it
does not have -- that is a honesty failure, and this project's rule is not to
claim a capability that was not measured. The cheapest correct step is for the
interface to say "unavailable" and draw nothing when there is no analyser. Whether
to wire a real one is a separate decision after that.

**The truncated tail is verified through accounting, not by ear.** At the end of a
track, total frames produced should equal `ceil(total source frames * ratio) +/- 1`.
Run it on three files with three different rate ratios. The loss varies with the
ratio -- 8 to 59 ms across the chunk sizes already measured -- which is what a
swallowed resampler latency looks like, and it also says the fix cannot be "flush
at the end of `readExact`" without breaking the count.
