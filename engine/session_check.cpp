// Drives the session API, not the render primitive.
//
// The point is the state the listener gets, not that a function returned zero:
// position must advance while playing, must not move while paused, must land
// near a seek target, and must stop when stopped.

#include "nae.h"
#include "utf8_args.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

bool getState(NAE_Session* s, NAE_SessionState* out) {
    out->structSize = sizeof(*out);
    return NAE_SessionGetState(s, out) == NAE_OK;
}

bool waitState(NAE_Session* s, int32_t want, int timeoutMs, NAE_SessionState* out) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (getState(s, out) && out->state == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitPosition(NAE_Session* s, double atLeast, int timeoutMs, NAE_SessionState* out) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (getState(s, out) && out->position >= atLeast) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
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

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: session_check <file> [deviceId]\n");
        return 1;
    }

    NAE_SessionConfig config{};
    config.structSize = sizeof(config);
    config.deviceId = args.size() > 2 ? args[2].c_str() : nullptr;
    config.path = args[1].c_str();
    config.outputMode = NAE_OUTPUT_NATIVE;
    config.fixedRate = 0;
    config.quality = NAE_QUALITY_HIGH;
    config.shareMode = NAE_SHARE_EXCLUSIVE;
    config.sliceMs = 16;

    NAE_Session* session = nullptr;
    const int32_t openRc = NAE_SessionOpen(&config, &session);
    if (openRc != NAE_OK) {
        char err[512] = {};
        if (session) {
            NAE_SessionLastError(session, err, sizeof(err));
            NAE_SessionClose(session);
        }
        std::printf("open failed: rc=%d %s\n", openRc, err);
        return 1;
    }

    NAE_SessionState st{};
    st.structSize = sizeof(st);

    bool ok = waitState(session, NAE_SESSION_READY, 1000, &st);
    check(ok, "opens into READY", stateName(st.state));
    const int32_t openPerfect = st.bitPerfect;
    check(st.duration > 0.0, "reports a duration", std::to_string(st.duration) + " s");
    // The position wait below asks for 0.20 s of playback. A file shorter than
    // that can never reach it and the failure would read as "position does not
    // advance", which is about the fixture.
    if (st.duration > 0.0 && st.duration < 0.25) {
        std::fprintf(stderr,
                     "fixture too short: %.3f s; this check waits for the position to "
                     "reach 0.20 s, so it needs at least 0.25 s.\n",
                     st.duration);
        return 2;
    }
    check(st.sampleRate > 0 && st.channels > 0, "reports the device format",
          std::to_string(st.sampleRate) + " Hz / " + std::to_string(st.channels) + " ch");

    NAE_ResolvedSignalPath rp{};
    rp.structSize = sizeof(rp);
    check(NAE_SessionGetResolvedSignalPath(session, &rp) == NAE_OK,
          "resolved signal path is available");
    check(rp.sampleRate > 0 && rp.channels > 0, "resolved path has the output format",
          std::to_string(rp.sampleRate) + " Hz / " + std::to_string(rp.channels) + " ch");
    check(rp.diagnosticCount > 0, "resolved path carries diagnostics",
          std::to_string(rp.diagnosticCount) + " entries");
    char* resolvedJson = NAE_SessionGetResolvedSignalPathJson(session);
    check(resolvedJson && std::strstr(resolvedJson, "\"diagnostics\"") != nullptr,
          "resolved JSON carries diagnostics");
    if (resolvedJson) NAE_FreeString(resolvedJson);

    check(NAE_SessionPlay(session) == NAE_OK, "play is accepted");
    ok = waitState(session, NAE_SESSION_PLAYING, 1000, &st);
    check(ok, "state becomes PLAYING", stateName(st.state));
    ok = waitPosition(session, 0.20, 3000, &st);
    check(ok, "position advances while playing", std::to_string(st.position) + " s");

    check(NAE_SessionPause(session) == NAE_OK, "pause is accepted");
    ok = waitState(session, NAE_SESSION_PAUSED, 1000, &st);
    check(ok, "state becomes PAUSED", stateName(st.state));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    getState(session, &st);
    const double pausedAt = st.position;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    getState(session, &st);
    check(std::fabs(st.position - pausedAt) < 0.02, "position does not move while paused",
          std::to_string(pausedAt) + " -> " + std::to_string(st.position));

    const double target = st.duration > 4.0 ? 2.0 : st.duration * 0.5;
    check(NAE_SessionSeek(session, target) == NAE_OK, "seek is accepted");
    bool landed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        getState(session, &st);
        if (std::fabs(st.position - target) < 0.75) {
            landed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(landed, "position lands near the seek target",
          "target " + std::to_string(target) + " -> " + std::to_string(st.position));

    check(NAE_SessionResume(session) == NAE_OK, "resume is accepted");
    ok = waitPosition(session, target + 0.10, 3000, &st);
    check(ok, "position advances after resume", std::to_string(st.position) + " s");

    check(NAE_SessionSetVolume(session, 0.5) == NAE_OK, "volume 0.5 is accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    getState(session, &st);
    check(std::fabs(st.volume - 0.5) < 1e-9, "volume is reported as 0.5",
          std::to_string(st.volume));
    if (openPerfect) {
        check(st.bitPerfect == 0 && st.reason == NAE_REASON_VOLUME_NOT_UNITY,
              "volume 0.5 reports the volume reason",
              "bitPerfect=" + std::to_string(st.bitPerfect) +
                  " reason=" + std::to_string(st.reason));
    }
    check(NAE_SessionSetVolume(session, 1.0) == NAE_OK, "volume 1.0 is accepted");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    getState(session, &st);
    if (openPerfect) {
        check(st.bitPerfect == 1 && st.reason == 0, "unity volume restores bit-perfect",
              "bitPerfect=" + std::to_string(st.bitPerfect) +
                  " reason=" + std::to_string(st.reason));
    }

    NAE_EqParams eq;
    NAE_EqDefaults(&eq);
    eq.enabled = 1;
    eq.bandCount = 1;
    eq.bands[0].type = NAE_EQ_PEAKING;
    eq.bands[0].freqHz = 1000.0;
    eq.bands[0].gainDb = -6.0;
    eq.bands[0].q = 1.0;
    eq.bands[0].enabled = 1;
    check(NAE_SessionSetEq(session, &eq) == NAE_OK, "EQ is accepted by the session");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    getState(session, &st);
    check(st.eqEnabled == 1, "EQ is reported as enabled", std::to_string(st.eqEnabled));
    if (openPerfect) {
        check(st.bitPerfect == 0 && st.reason == NAE_REASON_EQ,
              "EQ reports the EQ reason",
              "bitPerfect=" + std::to_string(st.bitPerfect) +
                  " reason=" + std::to_string(st.reason));
    }
    eq.enabled = 0;
    check(NAE_SessionSetEq(session, &eq) == NAE_OK, "EQ can be disabled by the session");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    getState(session, &st);
    if (openPerfect) {
        check(st.bitPerfect == 1 && st.reason == 0, "disabling EQ restores bit-perfect",
              "bitPerfect=" + std::to_string(st.bitPerfect) +
                  " reason=" + std::to_string(st.reason));
    }

    NAE_CrossfeedParams cf;
    NAE_CrossfeedDefaults(&cf);
    cf.enabled = 1;
    cf.amount = 0.5;
    check(NAE_SessionSetCrossfeed(session, &cf) == NAE_OK,
          "crossfeed is accepted by the session");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    getState(session, &st);
    check(st.crossfeedEnabled == 1, "crossfeed is reported as enabled",
          std::to_string(st.crossfeedEnabled));
    if (openPerfect) {
        check(st.bitPerfect == 0 && st.reason == NAE_REASON_CROSSFEED,
              "crossfeed reports the crossfeed reason",
              "bitPerfect=" + std::to_string(st.bitPerfect) +
                  " reason=" + std::to_string(st.reason));
    }
    cf.enabled = 0;
    check(NAE_SessionSetCrossfeed(session, &cf) == NAE_OK,
          "crossfeed can be disabled by the session");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    getState(session, &st);
    if (openPerfect) {
        check(st.bitPerfect == 1 && st.reason == 0,
              "disabling crossfeed restores bit-perfect",
              "bitPerfect=" + std::to_string(st.bitPerfect) +
                  " reason=" + std::to_string(st.reason));
    }

    getState(session, &st);
    if (st.rgTrackValid) {
        NAE_ReplayGainParams rg;
        NAE_ReplayGainDefaults(&rg);
        rg.mode = NAE_RG_TRACK;
        rg.preventClipping = 0;
        check(NAE_SessionSetReplayGain(session, &rg) == NAE_OK,
              "ReplayGain is accepted by the session");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        getState(session, &st);
        check(st.replayGainEnabled == 1, "ReplayGain is reported as enabled",
              std::to_string(st.replayGainEnabled));
        check(st.rgAppliedGain > 0.0 && std::fabs(st.rgAppliedGain - 1.0) > 0.001,
              "ReplayGain resolves to a non-unity gain",
              std::to_string(st.rgAppliedGain));
        if (openPerfect) {
            check(st.bitPerfect == 0 && st.reason == NAE_REASON_REPLAYGAIN,
                  "ReplayGain reports the ReplayGain reason",
                  "bitPerfect=" + std::to_string(st.bitPerfect) +
                      " reason=" + std::to_string(st.reason));
        }
        rg.mode = NAE_RG_OFF;
        check(NAE_SessionSetReplayGain(session, &rg) == NAE_OK,
              "ReplayGain can be disabled by the session");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        getState(session, &st);
        if (openPerfect) {
            check(st.bitPerfect == 1 && st.reason == 0,
                  "disabling ReplayGain restores bit-perfect",
                  "bitPerfect=" + std::to_string(st.bitPerfect) +
                      " reason=" + std::to_string(st.reason));
        }
    } else {
        std::printf("  [SKIP] the file has no ReplayGain tags\n");
    }

    check(NAE_SessionStop(session) == NAE_OK, "stop is accepted");
    ok = waitState(session, NAE_SESSION_IDLE, 1000, &st);
    check(ok, "state becomes IDLE", stateName(st.state));
    NAE_SessionClose(session);

    std::printf("\n%s\n", failures == 0 ? "All session checks passed."
                                        : "Some session checks FAILED.");
    return failures == 0 ? 0 : 1;
}
