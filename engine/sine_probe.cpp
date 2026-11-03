// Our own engine's pure-tone path, on its own, with nothing else running.
//
// This exists to split the bubbling in half. NAE_OpenExclusiveSine bypasses the
// decoder, the resampler and the dither, and writes a synthesised sine straight
// into the device buffer. So:
//
//   bubbles here  -> the fault is in the write path: packing, container
//                    justification, channel mask, or the loop's pacing
//   clean here    -> the fault is in what feeds the write path, and decode,
//                    resample and dither can be separated one at a time
//
// It also has to be run again rather than trusted from earlier today. The
// 24-in-32 fix raised every sample by 48 dB, so anything that was below the
// noise floor of the comparison became audible afterwards. A measurement taken
// before the fix does not carry over.

#include "nae.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <windows.h>

// usage:
//   sine_probe <seconds> [deviceId]                 synthesised tone, no decoder
//   sine_probe <seconds> <file> [deviceId]          a real file through the file path
//
// The second form exists because the synthesised tone turned out not to be a usable
// control: it runs at the device's own rate rather than the file path's, it renders
// in 200 ms slices, and at 0.9 amplitude it emitted one transient and then silence.
// Three runs of it produced rateActual figures of 189840, 190260 and 186108, and
// none of them can be believed. A control that does not emit a steady tone cannot
// tell you anything about a periodic artefact in one.

int main(int argc, char** argv) {
  const int seconds = (argc > 1) ? std::atoi(argv[1]) : 10;
  const char* maybeFile = (argc > 2) ? argv[2] : nullptr;
  const bool fileMode = maybeFile && (std::strchr(maybeFile, '.') != nullptr);
  const char* deviceId = fileMode ? ((argc > 3) ? argv[3] : nullptr) : maybeFile;

  if (fileMode) {
    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileCh = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(deviceId, maybeFile, NAE_OUTPUT_NATIVE, 0, 0, &h, &rate,
                                    &srcRate, &ch, &bits, &valid, &isFloat, &resampling, &dithered,
                                    &bitPerfect, &fileCh, &durationMs, &latencyMs);
    if (rc != 0) {
      std::fprintf(stderr, "NAE_OpenFile(%s) -> %d\n", maybeFile, rc);
      return 1;
    }
    std::printf("file    %s\n", maybeFile);
    std::printf("open: out %d Hz  src %d Hz  %d ch  %d bit (valid %d)  resampling=%d dithered=%d\n",
                rate, srcRate, ch, bits, valid, resampling, dithered);
    std::printf("duration %.1f s  latency %.2f ms\n", durationMs / 1000.0, latencyMs);
    std::printf("playing %d s", seconds);
    std::fflush(stdout);

    // The slice is the whole experiment.
    //
    // The application called NAE_Play(200) in a loop -- the Tauri host's
    // SLICE_MS was 200 -- so it cut the stream
    // five times a second. sine_probe used to make one call for the whole run.
    // Same file, same engine, same device, same format: the only difference
    // between a clean run and a bubbling one was how often this function was
    // called. If a boundary introduces a discontinuity, the artefact's period is
    // the call rate, and changing the slice changes the period. That is a
    // prediction, which makes it worth testing rather than assuming.
    const char* sliceEnv = std::getenv("NAE_SLICE_MS");
    const int sliceMs = sliceEnv ? std::atoi(sliceEnv) : 0;

    // The stop/play stress loop.
    //
    // There is no pause on this route -- src/scripts/app.js:222 says so -- so the
    // boundary that exists is stop followed by play, and that is the one the
    // bubbling fix could have broken. A prediction worth testing rather than
    // assuming: NAE_Play prefills the device buffer with silence and starts, so a
    // resumed stream begins at whatever phase the decoder had reached, and if that
    // phase is non-zero the first sample out of silence is a step.
    //
    // Cycles back to back with no gap, because a gap is the easy case and the
    // faults live at the boundary rather than in the steady state.
    const char* cyclesEnv = std::getenv("NAE_STOP_PLAY_CYCLES");
    if (cyclesEnv && std::atoi(cyclesEnv) > 0) {
        const int cycles = std::atoi(cyclesEnv);
        const char* slicePlayEnv = std::getenv("NAE_CYCLE_PLAY_MS");
        const char* gapEnv = std::getenv("NAE_CYCLE_GAP_MS");
        const int playMs = slicePlayEnv ? std::atoi(slicePlayEnv) : 300;
        const int gapMs = gapEnv ? std::atoi(gapEnv) : 0;
        // The control. Two hypotheses about the stop/play boundary have already been
        // falsified by ear -- a fade-in on resume, and the silent prefill -- and both
        // were reasoned from an instrument that turned out to be blind here. So the
        // next run isolates the variable instead: the same cycles with NAE_Stop not
        // called at all.
        //
        // With the stop removed, clientRunning stays true for the whole run, so the
        // loop performs exactly one Start. That is the case already measured clean,
        // which makes this a control rather than another attempt. If bubbles persist
        // here, the boundary is not the stop and the reasoning has been wrong about
        // something more basic.
        const bool noStop = std::getenv("NAE_CYCLE_NO_STOP") != nullptr;

        // Pause instead of stop. This is the candidate replacement for pause, and it
        // is the first run that can check the accounting invariant rather than only
        // asking an ear.
        //
        // The host must keep calling NAE_Play while paused: `silencing` only has
        // somewhere to take effect inside the render loop, and a started client that
        // nobody fills runs dry. So a pause is NAE_Play(gapMs) with the handle
        // paused, not Sleep.
        //
        // The check that matters is NAE_Position not moving across exactly those
        // calls. If it moves, the pause consumed source and the resume will not
        // land where the listener left off -- an invisible bug that no artefact
        // would ever report.
        const bool usePause = std::getenv("NAE_CYCLE_PAUSE") != nullptr;

        std::printf(" in %d %s cycles: %d ms play, %d ms gap%s\n", cycles,
                    usePause ? "pause/play" : "stop/play", playMs, gapMs,
                    noStop ? ", NAE_Stop NOT called (control)" : "");
        double worstPauseAdvance = 0.0;
        for (int i = 0; i < cycles; ++i) {
            if (NAE_Play(h, playMs) != 0) { std::printf("  play failed at cycle %d\n", i); break; }
            if (usePause) {
                const double before = NAE_Position(h);
                NAE_Pause(h);
                const double afterPause = NAE_Position(h);
                NAE_Play(h, gapMs);
                const double afterGap = NAE_Position(h);
                NAE_Resume(h);
                const double advanced = (afterGap - afterPause) * 1000.0;
                if (advanced > worstPauseAdvance) worstPauseAdvance = advanced;
                if (before != afterPause) {
                    std::printf("  NAE_Pause itself moved the position by %.3f ms\n",
                                (afterPause - before) * 1000.0);
                }
            } else if (!noStop) {
                NAE_Stop(h);
            }
            if (gapMs > 0 && !usePause) Sleep(static_cast<DWORD>(gapMs));
        }
        if (usePause) {
            std::printf("  pause consumed at most %.4f ms of source (must be 0)\n",
                        worstPauseAdvance);
        }
    } else if (sliceMs > 0) {
        std::printf(" in %d ms slices (%d calls)\n", sliceMs, (seconds * 1000) / sliceMs);
        int elapsed = 0;
        while (elapsed < seconds * 1000) {
            if (NAE_Play(h, sliceMs) != 0) break;
            elapsed += sliceMs;
        }
    } else {
        std::printf(" in one call\n");
        NAE_Play(h, seconds * 1000);
    }

    NAE_SignalPath sp{};
    if (NAE_QuerySignalPath(h, &sp) == 0) {
      std::printf("\nfifoPeak %.4f  writtenPeak %.4f  devicePeak %.6f\n", sp.fifoPeak,
                  sp.writtenPeak, sp.devicePeak);
      std::printf("requested %d bit  granted %d bit   padFrames %llu\n", sp.requestedBits,
                  sp.grantedBits, (unsigned long long)sp.padFrameCount);
      std::printf("buffer %.2f ms  rateActual %.0f  worstGap %.1f ms\n", sp.bufferDurationMs,
                  sp.deviceRateActual, sp.maxBufferGapMs);
      std::printf("worstFill %.2f  worstDecodeStall %.2f  worstLate %.2f  lateBuffers %lld\n",
                  sp.maxFillMs, sp.maxDecodeStallMs, sp.maxLateMs, (long long)sp.lateCount);
      // The seam figures, and the one number that decides the boundary question.
      //
      // worstStep/peak is the largest jump between one device buffer and the next,
      // relative to the signal. signalSlew/peak is the largest jump the signal makes
      // between adjacent samples, relative to the same peak. If the stream simply
      // continued, the two are the same order. A ratio of several times means the
      // waveform broke at a seam, and the ratio is what says by how much.
      const double slew = sp.seamSlewRef;
      std::printf("seams %llu  worstStep/peak %.4f  signalSlew/peak %.6f  ratio %.2fx\n",
                  (unsigned long long)sp.seamCount, sp.worstSeamRatio, slew,
                  slew > 0.0 ? sp.worstSeamRatio / slew : 0.0);
      // Reported apart from deviceRateActual on purpose: these frames are elapsed
      // time for the device but they are not playback, and folding them into the
      // rate would let a pause look like correctly-timed music.
      std::printf("silenceFrames %llu\n", (unsigned long long)sp.silenceFramesWritten);
    }
    NAE_Close(h);
    return 0;
  }

  NAE_Handle* handle = nullptr;
  int32_t rate = 0, channels = 0, bits = 0, validBits = 0, isFloat = 0;
  double latencyMs = 0.0;

  const int32_t rc = NAE_OpenExclusiveSine(deviceId, &handle, &rate, &channels, &bits, &validBits,
                                           &isFloat, &latencyMs);
  if (rc != 0) {
    std::fprintf(stderr, "NAE_OpenExclusiveSine -> %d\n", rc);
    return 1;
  }
  std::printf("exclusive open: %d Hz  %d ch  %d bit (valid %d)  float=%d  latency %.2f ms\n", rate,
              channels, bits, validBits, isFloat, latencyMs);
  std::printf("playing %d s of sine, nothing else in the path\n", seconds);
  std::fflush(stdout);

  const int32_t prc = NAE_PlaySine(handle, seconds * 1000);
  std::printf("NAE_PlaySine -> %d\n", prc);

  NAE_SignalPath sp{};
  if (NAE_QuerySignalPath(handle, &sp) == 0) {
    std::printf("\nfifoPeak %.4f  writtenPeak %.4f  devicePeak %.6f\n", sp.fifoPeak, sp.writtenPeak,
                sp.devicePeak);
    std::printf("requested %d bit  granted %d bit   padFrames %llu\n", sp.requestedBits,
                sp.grantedBits, (unsigned long long)sp.padFrameCount);
    std::printf("buffer %.2f ms  rateActual %.0f  worstGap %.1f ms\n", sp.bufferDurationMs,
                sp.deviceRateActual, sp.maxBufferGapMs);
    std::printf("worstFill %.2f ms  worstDecodeStall %.2f ms  worstLate %.2f ms  lateBuffers %lld\n",
                sp.maxFillMs, sp.maxDecodeStallMs, sp.maxLateMs, (long long)sp.lateCount);
  }

  NAE_Close(handle);
  return 0;
}
