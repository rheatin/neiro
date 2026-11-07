// Gapless is a property of the boundary, so the check has to cross one.
//
// Two same-format files are used. The engine path preloads the second and plays
// through the seam; the underrun and pad counters must not move, because a gap is
// exactly what those counters would show. The session path exercises the queue:
// the serial must advance while the state stays PLAYING, the position must reset,
// and the state must reach ENDED only after the second file.

#include "nae.h"
#include "utf8_args.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

const char* stateName(int32_t state) {
    switch (state) {
        case NAE_SESSION_IDLE: return "IDLE";
        case NAE_SESSION_READY: return "READY";
        case NAE_SESSION_PLAYING: return "PLAYING";
        case NAE_SESSION_PAUSED: return "PAUSED";
        case NAE_SESSION_DEVICE_LOST: return "DEVICE_LOST";
        case NAE_SESSION_ENDED: return "ENDED";
        case NAE_SESSION_ERROR: return "ERROR";
        default: return "?";
    }
}

bool getState(NAE_Session* session, NAE_SessionState* out) {
    out->structSize = sizeof(*out);
    return NAE_SessionGetState(session, out) == NAE_OK;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 3) {
        std::printf(
            "usage: gapless_check <trackA> <trackB>\n"
            "  Both must be the same format. trackA has to end within about\n"
            "  2.5 s of the start, because that is how far this check plays\n"
            "  before it gives up waiting for the seam; trackB's length is\n"
            "  measured, not assumed.\n");
        return 1;
    }

    // Track B's duration, measured before the seam test rather than assumed.
    //
    // This used to be `duration > 0.9 && duration < 1.1`, which is a check that
    // silently encodes "your fixture is exactly one second" and reports any
    // other fixture as an engine failure. A probe that fails for a reason that
    // is not the property under test teaches the reader to ignore it.
    double trackBSeconds = 0.0;
    {
        NAE_Handle* probe = nullptr;
        int32_t r = 0, sr = 0, ch = 0, b = 0, v = 0, f = 0, res = 0, dit = 0, bp = 0, fc = 0;
        double dur = 0.0, lat = 0.0;
        if (NAE_OpenFile(nullptr, args[2].c_str(), NAE_OUTPUT_NATIVE, 0, NAE_QUALITY_HIGH, &probe,
                         &r, &sr, &ch, &b, &v, &f, &res, &dit, &bp, &fc, &dur, &lat) != NAE_OK ||
            !probe) {
            std::printf("could not read track B's duration: open failed\n");
            return 1;
        }
        // NAE_OpenFile reports milliseconds; everything else here is seconds.
        trackBSeconds = dur / 1000.0;
        NAE_Close(probe);
    }
    if (trackBSeconds <= 0.0) {
        std::printf("track B reports no duration: %.3f s\n", trackBSeconds);
        return 1;
    }

    // ---- engine path ----
    NAE_Handle* handle = nullptr;
    int32_t rate = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    if (NAE_OpenFile(nullptr, args[1].c_str(), NAE_OUTPUT_NATIVE, 0, NAE_QUALITY_HIGH, &handle,
                     &rate, &sourceRate, &channels, &bits, &valid, &isFloat, &resampling,
                     &dithered, &bitPerfect, &fileChannels, &durationMs, &latencyMs) !=
            NAE_OK ||
        !handle) {
        std::printf("engine open failed\n");
        return 1;
    }
    // The seam budget below is 500 ms played here plus 100 x 20 ms waiting, so
    // track A has to end inside that window. The usage text said so and the code
    // did not: on a longer file the wait ran out and the check reported that the
    // engine never crossed the seam.
    if (durationMs > 2500.0) {
        std::fprintf(stderr,
                     "track A too long: %.0f ms. This check plays 500 ms and then "
                     "waits 2000 ms for the seam, so track A must end within 2500 ms.\n",
                     durationMs);
        return 2;
    }
    for (int i = 0; i < 25; ++i) NAE_Play(handle, 20);  // 500 ms into track A
    check(NAE_PreloadNext(handle, args[2].c_str()) == NAE_OK, "engine preloads track B");

    NAE_SignalPath before{};
    before.structSize = sizeof(before);
    check(NAE_QuerySignalPath(handle, &before) == NAE_OK, "counters before the seam");

    bool crossed = false;
    for (int i = 0; i < 100; ++i) {
        NAE_Play(handle, 20);
        if (NAE_GetTrackSerial(handle) == 1) {
            crossed = true;
            break;
        }
    }
    check(crossed, "engine crosses the seam", "serial 1");

    NAE_SignalPath after{};
    after.structSize = sizeof(after);
    check(NAE_QuerySignalPath(handle, &after) == NAE_OK, "counters after the seam");
    check(after.underrunCount == before.underrunCount, "no underruns at the seam",
          std::to_string(before.underrunCount) + " -> " +
              std::to_string(after.underrunCount));
    check(after.padFrameCount == before.padFrameCount, "no invented frames at the seam",
          std::to_string(before.padFrameCount) + " -> " +
              std::to_string(after.padFrameCount));
    check(NAE_Position(handle) < 0.5, "position reset to track B",
          std::to_string(NAE_Position(handle)));
    check(std::abs(NAE_Duration(handle) - trackBSeconds) < 0.05 + trackBSeconds * 0.02,
          "duration is track B",
          std::to_string(NAE_Duration(handle)) + " vs " + std::to_string(trackBSeconds));
    check(NAE_ClearNext(handle) == NAE_OK, "clear-next is safe after a swap");
    NAE_Close(handle);

    // ---- a seek before the seam must not follow the listener into track B ----
    //
    // `seekBaseFrames` carries how far into the track a seek moved, and is added
    // to `sourcePos` when reporting a position. It used to be reset only by
    // `NAE_Seek`, so a gapless transition *after* a seek handed A's offset to B:
    // measured 3.327 s of position for a track B that had just started, on a
    // track that was never seeked. Nothing else here seeks, which is why it went
    // unnoticed -- the plain seam resets `sourcePos` to zero and the two agree.
    {
        NAE_Handle* h2 = nullptr;
        int32_t r = 0, sr = 0, c = 0, b = 0, v = 0, fl = 0, rs = 0, dt = 0, bp = 0, fc = 0;
        double dm = 0.0, lm = 0.0;
        if (NAE_OpenFile(nullptr, args[1].c_str(), NAE_OUTPUT_NATIVE, 0, NAE_QUALITY_HIGH, &h2,
                         &r, &sr, &c, &b, &v, &fl, &rs, &dt, &bp, &fc, &dm, &lm) == NAE_OK &&
            h2) {
            check(NAE_PreloadNext(h2, args[2].c_str()) == NAE_OK,
                  "preload after the seek test");
            const double seekTo = dm / 1000.0 * 0.5;
            check(NAE_Seek(h2, seekTo) == NAE_OK, "seek inside track A",
                  std::to_string(seekTo) + " s");
            bool crossed2 = false;
            for (int i = 0; i < 2000; ++i) {
                NAE_Play(h2, 20);
                if (NAE_GetTrackSerial(h2) == 1) {
                    crossed2 = true;
                    break;
                }
            }
            check(crossed2, "crosses to track B after a seek");
            if (crossed2) {
                for (int i = 0; i < 10; ++i) NAE_Play(h2, 20);
                const double bPos = NAE_Position(h2);
                // Measured against the seek point, not a constant. A fixed 1.0 s
                // threshold let the defect through on a 1 s fixture: the seek is
                // 0.5 s there and B reported 0.850 s, which is wrong and passed.
                // B has just started, so anything approaching A's offset is the
                // offset having followed it across the seam.
                // B has just started, so its own progress is a fraction of a
                // second; the defect put it at A's offset plus that progress.
                // Halfway between the two, expressed relative to the seek so the
                // threshold means the same thing on any fixture length.
                // The floor is the 200 ms this check plays into B after the seam:
                // healthy, bPos is that much plus a little. On a short track A the
                // seek point can be smaller than that budget, and a bare
                // `bPos < seekTo` would then fail on a correct engine.
                const double limit = std::max(seekTo, 0.30);
                check(bPos < limit, "track B's position does not inherit A's seek offset",
                      std::to_string(bPos) + " s, threshold " + std::to_string(limit) +
                          " s (A was seeked to " + std::to_string(seekTo) + " s)");
            }
            NAE_Close(h2);
        }
    }
    NAE_Shutdown();

    // ---- session path ----
    NAE_SessionConfig config{};
    config.structSize = sizeof(config);
    config.path = args[1].c_str();
    config.outputMode = NAE_OUTPUT_NATIVE;
    config.quality = NAE_QUALITY_HIGH;
    config.shareMode = NAE_SHARE_EXCLUSIVE;
    config.sliceMs = 16;
    NAE_Session* session = nullptr;
    check(NAE_SessionOpen(&config, &session) == NAE_OK, "session opens track A");
    check(NAE_SessionQueueAdd(session, args[2].c_str()) == NAE_OK, "session queues track B");
    check(NAE_SessionPlay(session) == NAE_OK, "session plays");

    NAE_SessionState st{};
    bool advanced = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        getState(session, &st);
        if (st.trackSerial == 1) {
            advanced = true;
            break;
        }
        if (st.state == NAE_SESSION_ERROR || st.state == NAE_SESSION_ENDED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(advanced, "session serial advances to track B",
          "serial " + std::to_string(st.trackSerial));
    if (advanced) {
        check(st.state == NAE_SESSION_PLAYING, "the state stayed PLAYING at the seam",
              stateName(st.state));
        check(st.position < 0.5, "session position reset", std::to_string(st.position));
        check(std::abs(st.duration - trackBSeconds) < 0.05 + trackBSeconds * 0.02,
              "session duration is track B",
              std::to_string(st.duration) + " vs " + std::to_string(trackBSeconds));
    }

    bool ended = false;
    const auto endDeadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(
                                 static_cast<int>(trackBSeconds * 1000.0) + 2000);
    while (std::chrono::steady_clock::now() < endDeadline) {
        getState(session, &st);
        if (st.state == NAE_SESSION_ENDED) {
            ended = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(ended, "session ends after track B", stateName(st.state));
    check(NAE_SessionStop(session) == NAE_OK, "session stops");
    NAE_SessionClose(session);

    std::printf("\n%s\n", failures == 0 ? "All gapless checks passed."
                                        : "Some gapless checks FAILED.");
    return failures == 0 ? 0 : 1;
}
