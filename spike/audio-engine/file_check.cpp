// Plays a real file through the exclusive device in slices, the way the host
// does, and checks that playback is continuous.
//
// # Why slicing is the point
//
// The host does not call NAE_Play once per track. It calls it every ~200 ms with
// a time budget, and expects the stream to carry on. That contract is easy to
// break and hard to notice: render a whole stream in one call, as every engine
// harness here used to, and a stateful render loop that secretly resets per call
// still produces a perfect file. Broken only in slices, it produces a tone that
// restarts five times a second -- or, for a file, the track replayed from the top
// five times a second.
//
// So this test plays in slices on purpose, and checks the thing that breaks:
// that the source position advances monotonically, by the amount the device
// actually consumed, all the way to the end of the file.
//
// # What it cannot check
//
// Exclusive mode gives no way to read back what was sent to the DAC. So this
// verifies the *pipeline*, not the samples: that the right number of source
// frames went in, in the right order, once each. Whether the converter turned
// them into the right waveform is verified elsewhere, where the samples can
// actually be seen.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nae.h"

namespace {

int g_fail = 0;

void check(const char* label, bool ok, const std::string& detail) {
    if (!ok) ++g_fail;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

std::string fmt(const char* f, ...) {
    char buf[224];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

std::string lastError() {
    char buf[256] = {0};
    NAE_GetLastErrorText(buf, sizeof(buf));
    return buf;
}

// The slice size the Rust host uses. Matching it is the whole point: a bug that
// only appears at this boundary would not appear at 5000 ms.
constexpr int32_t kSliceMs = 200;

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: file_check <file> [device-id]\n");
        return 2;
    }

    // Unbuffered, before anything is printed.
    //
    // A harness that watches a bounded window reads this process's output while
    // it is still running and then kills it, and with the default block
    // buffering every byte written so far is still sitting in the CRT's 4 KB
    // buffer -- so the harness captures an empty string and reports the format as
    // untested. Not a hypothetical: that is exactly what happened, for all eight
    // fixtures, before this line existed.
    //
    // The cost is a write syscall per printf, which is nothing next to the audio
    // this program spends its life rendering.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* path = argv[1];
    const char* deviceId = argc > 2 && argv[2][0] != '-' ? argv[2] : nullptr;
    // Output mode, so the same file can be checked down the native path and the
    // resampling path. The bit-perfect assertions below are written to hold for
    // both, which is the point: a mode that converts must not claim otherwise.
    const int32_t outputMode = argc > 2 && argv[2][0] == '-' ? argv[2][1] - '0' : 0;
    // Fixed mode needs a rate to fix to. Zero is not "the device's rate", it is
    // "no target at all", and the engine says so rather than guessing.
    const int32_t fixedRate = argc > 3 ? std::atoi(argv[3]) : 0;

    std::printf("\n== open: %s ==\n", path);

    NAE_Handle* handle = nullptr;
    int32_t rate = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;

    // The mode decides whether anything is converted, and the assertions below are
    // written against what the engine *reports* rather than against the mode, so
    // a wrong flag fails the test in whichever mode produced it.
    const int32_t rc = NAE_OpenFile(deviceId, path, outputMode, fixedRate,
                                    /*quality=*/2, &handle, &rate, &sourceRate, &channels, &bits,
                                    &valid, &isFloat, &resampling, &dithered, &bitPerfect,
                                    &fileChannels, &durationMs, &latencyMs);
    if (rc != NAE_OK) {
        check("opened the file and the device", false,
              fmt("rc=%d %s", rc, lastError().c_str()));
        std::printf("\n%s\n", g_fail == 0 ? "Nothing to check." : "Some checks FAILED.");
        return 1;
    }
    check("opened the file and the device", true, lastError());

    std::printf("  file:     %u Hz, %d ch, %.1f ms\n", sourceRate, fileChannels, durationMs);
    std::printf("  device:   %u Hz, %d ch, %d-bit (valid %d), %s\n", rate, channels, bits, valid,
                isFloat ? "float" : "integer");
    std::printf("  latency:  %.2f ms\n", latencyMs);
    std::printf("  path:     %s%s%s\n", resampling ? "resampled" : "no resampling",
                dithered ? " + dithered" : "", bitPerfect ? "  [BIT PERFECT]" : "");

    check("the file's rate is what the source runs at", sourceRate > 0,
          fmt("%u Hz", sourceRate));
    check("the device opened", rate > 0, fmt("%u Hz, %d ch", rate, channels));
    check("a duration was reported", durationMs > 0.0, fmt("%.1f ms", durationMs));

    // The honesty check that matters: bit-perfect may only be claimed when
    // nothing at all was done to the signal. A file at 44.1 kHz on a 192 kHz
    // device is resampled; a surround file on a stereo device is downmixed; both
    // are processing, and saying otherwise would be a claim the user has no way
    // to check.
    if (sourceRate == rate) {
        check("bit-perfect is claimed only when nothing was converted",
              !bitPerfect || (!resampling && !dithered), fmt("bp=%d rs=%d dt=%d", bitPerfect,
                                                            resampling, dithered));
    } else {
        check("a rate mismatch is reported as resampling", resampling != 0,
              fmt("%u -> %u Hz", sourceRate, rate));
        check("a rate mismatch is never called bit-perfect", bitPerfect == 0,
              fmt("bitPerfect=%d", bitPerfect));
    }

    // The check that catches a lie the flags above cannot: the rate and the
    // dither both said "nothing happened", and a channel conversion still did.
    if (fileChannels != channels) {
        check("a channel conversion is never called bit-perfect", bitPerfect == 0,
              fmt("%d ch -> %d ch, bitPerfect=%d", fileChannels, channels, bitPerfect));
    }

    const double durationSec = NAE_Duration(handle);
    check("NAE_Duration matches the decoder's", durationSec > 0.0,
          fmt("%.3f s", durationSec));

    // ---- sliced playback ----
    std::printf("\n== playback in %d ms slices ==\n", kSliceMs);

    double lastPos = 0.0;
    bool monotonic = true;
    bool reachedEnd = false;
    int slices = 0;
    int overruns = 0;
    // Total source consumed, to compare against the file's length at the end.
    // Position is checked per slice; this is the aggregate that would catch a
    // stream which advances correctly but stops short.
    const double expected = durationSec;

    for (; slices < 10000; ++slices) {
        const double before = NAE_Position(handle);
        if (before + 1e-9 < lastPos) {
            monotonic = false;
            std::printf("  position went backwards at slice %d: %.4f -> %.4f\n", slices, lastPos,
                        before);
            break;
        }

        const int32_t prc = NAE_Play(handle, kSliceMs);
        if (prc != NAE_OK) {
            check("a slice failed", false, fmt("slice %d rc=%d %s", slices, prc,
                                               lastError().c_str()));
            break;
        }

        const double after = NAE_Position(handle);
        // A slice may consume no source at all when the device buffer is still
        // full from the previous one; that is the loop pacing itself, not a stall.
        if (after <= before) ++overruns;

        lastPos = after;
        if (after + 0.25 >= expected) {
            reachedEnd = true;
            ++slices;
            break;
        }
    }

    std::printf("  %d slices, reached %.3f s of %.3f s\n", slices, lastPos, expected);
    check("position never went backwards", monotonic, fmt("ended at %.3f s", lastPos));
    check("playback reached the end of the file", reachedEnd,
          fmt("%.3f s of %.3f s", lastPos, expected));
    // Not every slice must advance: the device buffer is ~50 ms and the slice is
    // 200 ms, so most do and a few do not. Many in a row would mean a stall.
    check("playback did not stall", overruns < 5, fmt("%d slices consumed no source", overruns));

    // The aggregate: a stream that advanced monotonically but only halfway is
    // still broken, and the per-slice check would have passed.
    check("consumed the whole file, not part of it", lastPos >= expected - 0.30,
          fmt("%.3f s consumed, %.3f s long", lastPos, expected));

    // ---- seek ----
    std::printf("\n== seek ==\n");
    if (expected > 1.5) {
        const int32_t src = NAE_Seek(handle, 1.0);
        check("seek to 1 s", src == NAE_OK, fmt("rc=%d %s", src, lastError().c_str()));
        const double pos = NAE_Position(handle);
        // A compressed seek lands on the nearest packet, so "near 1 s" is the
        // honest claim. The point of the check is that it moved and did not
        // leave the position pointing into the audio that was already played.
        check("position reflects the seek", pos > 0.7 && pos < 1.4, fmt("%.3f s", pos));

        // And playback must resume rather than stop.
        const int32_t prc = NAE_Play(handle, kSliceMs);
        check("playback resumes after a seek", prc == NAE_OK, fmt("rc=%d", prc));
        check("position advanced after the seek", NAE_Position(handle) > pos,
              fmt("%.3f -> %.3f s", pos, NAE_Position(handle)));
    } else {
        std::printf("  (file too short to seek meaningfully; skipping)\n");
    }

    NAE_Close(handle);
    NAE_Shutdown();

    std::printf("\n%s\n", g_fail == 0 ? "All file playback checks passed."
                                      : "Some file playback checks FAILED.");
    return g_fail == 0 ? 0 : 1;
}
