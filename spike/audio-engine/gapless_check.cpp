// Gapless is a property of the boundary, so the check has to cross one.
//
// Two same-format files are used. The engine path preloads the second and plays
// through the seam; the underrun and pad counters must not move, because a gap is
// exactly what those counters would show. The session path exercises the queue:
// the serial must advance while the state stays PLAYING, the position must reset,
// and the state must reach ENDED only after the second file.

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
        std::printf("usage: gapless_check <trackA> <trackB>\n");
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
    check(NAE_Duration(handle) > 0.9 && NAE_Duration(handle) < 1.1,
          "duration is track B", std::to_string(NAE_Duration(handle)));
    check(NAE_ClearNext(handle) == NAE_OK, "clear-next is safe after a swap");
    NAE_Close(handle);
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
        check(st.duration > 0.9 && st.duration < 1.1, "session duration is track B",
              std::to_string(st.duration));
    }

    bool ended = false;
    const auto endDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
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
