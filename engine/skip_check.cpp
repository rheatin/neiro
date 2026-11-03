// A manual skip must keep the stream open.
//
// The session's `next` used to close and reopen the handle, which costs a
// device close/open and a renegotiation -- and produces a gap. The engine can
// switch at the source boundary instead, which is the same seam gapless uses.
//
// What this measures is the consequence, not that a function returned OK:
//
//   * the track serial advances while the state stays PLAYING,
//   * the position restarts near zero rather than continuing,
//   * the negotiated format is unchanged, which is the thing a close/reopen
//     could quietly alter,
//   * no underruns and no invented frames were counted across the switch. Those
//     two counters are what a gap and a padded seam look like from inside the
//     engine, and a renegotiation would move at least one of them.

#include "nae.h"
#include "utf8_args.h"

#include <chrono>
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

void getState(NAE_Session* session, NAE_SessionState* out) {
    out->structSize = sizeof(*out);
    NAE_SessionGetState(session, out);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 3) {
        std::printf("usage: skip_check <trackA> <trackB>\n");
        return 1;
    }

    NAE_SessionConfig config{};
    config.structSize = sizeof(config);
    config.path = args[1].c_str();
    config.outputMode = NAE_OUTPUT_NATIVE;
    config.quality = NAE_QUALITY_HIGH;
    config.shareMode = NAE_SHARE_EXCLUSIVE;
    config.sliceMs = 16;
    NAE_Session* session = nullptr;
    if (NAE_SessionOpen(&config, &session) != NAE_OK || !session) {
        char buf[256] = {0};
        NAE_GetLastErrorText(buf, static_cast<int32_t>(sizeof(buf)));
        std::printf("session open failed: %s\n", buf);
        return 1;
    }

    check(NAE_SessionQueueAdd(session, args[2].c_str()) == NAE_OK, "track B queued");
    check(NAE_SessionPlay(session) == NAE_OK, "playback started");

    // Skip before anything is preloaded: this must be refused, not silently
    // accepted and then discovered by the render loop.
    check(NAE_SessionSkipToNext(session) == NAE_ERR_NOT_OPEN,
          "a skip with nothing preloaded is refused");

    NAE_SessionState st{};
    // Wait for the preload, then for the stream to be genuinely playing. The
    // skip is only meaningful once there is audio in flight.
    const auto preloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool playing = false;
    while (std::chrono::steady_clock::now() < preloadDeadline) {
        getState(session, &st);
        if (st.state == NAE_SESSION_PLAYING && st.position > 0.05) {
            playing = true;
            break;
        }
        if (st.state == NAE_SESSION_ERROR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(playing, "track A is playing", std::to_string(st.position));

    // The session does not expose its handle, and the underrun/pad counters live
    // on a handle. They are checked on the handle path below, where the same
    // switch runs; here the format and the position are the session-level facts.
    const int32_t rateBefore = st.sampleRate;
    const int32_t channelsBefore = st.channels;
    const double positionBefore = st.position;

    const int32_t skipRc = NAE_SessionSkipToNext(session);
    check(skipRc == NAE_OK, "skip is accepted once B is preloaded",
          std::to_string(skipRc));

    bool advanced = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        getState(session, &st);
        if (st.trackSerial == 1) {
            advanced = true;
            break;
        }
        if (st.state == NAE_SESSION_ERROR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(advanced, "the serial advances to track B", std::to_string(st.trackSerial));

    check(st.state == NAE_SESSION_PLAYING, "the state stays PLAYING across the skip",
          std::to_string(st.state));
    check(st.sampleRate == rateBefore, "the negotiated rate is unchanged",
          std::to_string(rateBefore) + " -> " + std::to_string(st.sampleRate));
    check(st.channels == channelsBefore, "the channel count is unchanged",
          std::to_string(channelsBefore) + " -> " + std::to_string(st.channels));

    // The position restarts. Without the skip it would still be growing from
    // wherever A had got to.
    check(st.position < positionBefore, "the position restarts at the new track",
          std::to_string(positionBefore) + " -> " + std::to_string(st.position));


    // The second skip has nothing behind it: B was the last queued track, so the
    // request must be refused rather than ending the stream.
    check(NAE_SessionSkipToNext(session) == NAE_ERR_NOT_OPEN,
          "a skip past the end of the queue is refused");

    check(NAE_SessionStop(session) == NAE_OK, "session stops");
    NAE_SessionClose(session);

    // ---- the handle path, where the counters are visible -------------------
    //
    // Same switch, one level down. A gap at the seam and a padded one show up in
    // these two counters and nowhere else the session exposes.
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
    for (int i = 0; i < 10; ++i) NAE_Play(handle, 20);
    check(NAE_SkipToNext(handle) == NAE_ERR_NOT_OPEN,
          "the engine refuses a skip with nothing preloaded");
    check(NAE_PreloadNext(handle, args[2].c_str()) == NAE_OK, "engine preloads track B");

    NAE_SignalPath before{};
    before.structSize = sizeof(before);
    check(NAE_QuerySignalPath(handle, &before) == NAE_OK, "counters before the switch");
    const uint64_t serialBefore = NAE_GetTrackSerial(handle);

    check(NAE_SkipToNext(handle) == NAE_OK, "the engine accepts the switch");
    NAE_Play(handle, 200);

    check(NAE_GetTrackSerial(handle) == serialBefore + 1,
          "the engine serial advances",
          std::to_string(serialBefore) + " -> " +
              std::to_string(NAE_GetTrackSerial(handle)));

    NAE_SignalPath after{};
    after.structSize = sizeof(after);
    check(NAE_QuerySignalPath(handle, &after) == NAE_OK, "counters after the switch");
    check(after.underrunCount == before.underrunCount, "no underruns across the switch",
          std::to_string(before.underrunCount) + " -> " +
              std::to_string(after.underrunCount));
    check(after.padFrameCount == before.padFrameCount, "no invented frames at the switch",
          std::to_string(before.padFrameCount) + " -> " +
              std::to_string(after.padFrameCount));
    NAE_Close(handle);
    NAE_Shutdown();

    std::printf("\n%s\n", failures == 0 ? "All skip checks passed."
                                        : "Some skip checks FAILED.");
    return failures == 0 ? 0 : 1;
}
