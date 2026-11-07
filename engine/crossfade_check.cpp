// Crossfade is a mix, so the check has to see both tracks at once and then the
// next track's position after the seam.
//
// Two 1 s files of different pitch are used. At 0.75 s, with a 0.5 s fade, both
// bins must be present. After the swap the serial is 1, the position starts at
// about the fade length rather than zero, and the underrun/pad counters must not
// move. The session path repeats the position/serial check through the queue.
//
// The two pitches are **measured from the files**, not named here. The comment
// used to say 300 Hz and 600 Hz while the fixtures beside it are 440 Hz and
// 880 Hz, so the check searched empty bins and reported 0.000003 -- a failure
// about its own constants. Whatever two tones are supplied, it finds them.

#include "nae.h"
#include "utf8_args.h"

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

void getState(NAE_Session* session, NAE_SessionState* out) {
    out->structSize = sizeof(*out);
    NAE_SessionGetState(session, out);
}

/// Measure which bin each fixture actually occupies, instead of asserting one.
///
/// This check used to look for 300 Hz and 600 Hz, the tones its own comment
/// described. The fixtures it is run with are 440 Hz and 880 Hz, so the search
/// landed on empty bins and reported 0.000003 -- a failure about two constants
/// in the probe, not about the crossfade. A constant that describes the test
/// signal has to come from the signal, or the probe measures itself.
///
/// Plays the file alone for long enough to fill the spectrum tap, takes the
/// loudest bin, and returns it. -1 when the file cannot be opened.
int dominantBin(const std::string& path, double* hz, double* rate, double* durMs) {
    NAE_Handle* h = nullptr;
    int32_t r = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    if (NAE_OpenFile(nullptr, path.c_str(), NAE_OUTPUT_NATIVE, 0, NAE_QUALITY_HIGH, &h, &r,
                     &sourceRate, &channels, &bits, &valid, &isFloat, &resampling,
                     &dithered, &bitPerfect, &fileChannels, &durationMs, &latencyMs) !=
            NAE_OK ||
        !h) {
        return -1;
    }
    for (int i = 0; i < 25; ++i) NAE_Play(h, 20);  // half a second, no fade involved

    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(info);
    std::vector<float> bins(4096, 0.0f);
    int peak = -1;
    double best = 0.0;
    if (NAE_QuerySpectrum(h, &info, bins.data(), static_cast<int32_t>(bins.size())) == NAE_OK) {
        const uint32_t fft = info.fftSize ? info.fftSize : 4096;
        for (uint32_t i = 1; i < fft && i < bins.size(); ++i) {
            if (bins[i] > best) {
                best = bins[i];
                peak = static_cast<int>(i);
            }
        }
        if (peak >= 0) {
            *hz = static_cast<double>(peak) * static_cast<double>(sourceRate) / fft;
        }
    }
    *rate = static_cast<double>(sourceRate);
    *durMs = durationMs;
    NAE_Close(h);
    return peak;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 3) {
        std::printf("usage: crossfade_check <trackA> <trackB>\n");
        return 1;
    }

    // What each fixture sounds like, measured from the fixture.
    double hzA = 0.0, hzB = 0.0, rateA = 0.0, rateB = 0.0, durA = 0.0, durB = 0.0;
    const int binA = dominantBin(args[1], &hzA, &rateA, &durA);
    const int binB = dominantBin(args[2], &hzB, &rateB, &durB);
    if (binA < 0 || binB < 0) {
        std::printf("cannot open a fixture\n");
        return 1;
    }
    std::printf("  fixtures: A %.0f Hz %.0f ms, B %.0f Hz %.0f ms (%.0f Hz device rate)\n",
                hzA, durA, hzB, durB, rateA);
    // The fade is 500 ms, so track A must contain one. Everything downstream is
    // expressed relative to the fixtures' own lengths -- the stop point is 300 ms
    // before A ends, and the session deadline allows A's length plus the fade.
    // A 1 s file used to be encoded in a literal 0.70 and in `duration > 0.9
    // && < 1.1`; any other fixture then reported engine failures that were only
    // the check assuming its own fixture.
    // `> 0` on both: a zero length means the engine did not answer, and refusing
    // there would file an engine fault under "wrong fixture".
    if (durA > 0.0 && durB > 0.0 && (durA < 800.0 || durB < 400.0)) {
        std::fprintf(stderr,
                     "fixture too short: A %.0f ms, B %.0f ms; this check needs a "
                     "500 ms fade inside track A (at least 800 ms) and track B to "
                     "have a position after it (at least 400 ms).\n",
                     durA, durB);
        return 2;
    }
    const double fadeSec = 0.5;

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

    NAE_CrossfadeParams xf;
    NAE_CrossfadeDefaults(&xf);
    xf.enabled = 1;
    xf.durationMs = 500.0;
    xf.curve = NAE_CROSSFADE_EQUAL_POWER;
    check(NAE_SetCrossfade(handle, &xf) == NAE_OK, "crossfade is accepted");
    check(NAE_PreloadNext(handle, args[2].c_str()) == NAE_OK, "next track is preloaded");

    NAE_SignalPath before{};
    before.structSize = sizeof(before);
    check(NAE_QuerySignalPath(handle, &before) == NAE_OK, "counters before the mix");

    // Stop at a known source position, not a fixed slice count: the device
    // budget can overshoot and the fade would already be over.
    const double stopAt = durA / 1000.0 - 0.30;  // 300 ms before A ends: mid-fade
    for (int i = 0; i < 4000 && NAE_Position(handle) < stopAt; ++i) NAE_Play(handle, 5);

    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(info);
    std::vector<float> bins(4096, 0.0f);
    check(NAE_QuerySpectrum(handle, &info, bins.data(),
                            static_cast<int32_t>(bins.size())) == NAE_OK,
          "spectrum at the midpoint");
    const bool binOk = binA >= 0 && binB >= 0 &&
                       binA < static_cast<int>(bins.size()) &&
                       binB < static_cast<int>(bins.size());
    const double a = binOk ? bins[binA] : 0.0;
    const double b = binOk ? bins[binB] : 0.0;
    check(binOk && a > 0.005 && b > 0.005, "both tracks are present mid-fade",
          "A " + std::to_string(hzA) + " Hz bin " + std::to_string(a) + ", B " +
              std::to_string(hzB) + " Hz bin " + std::to_string(b) + " at pos " +
              std::to_string(NAE_Position(handle)));

    bool crossed = false;
    for (int i = 0; i < 40; ++i) {
        NAE_Play(handle, 20);
        if (NAE_GetTrackSerial(handle) == 1) {
            crossed = true;
            break;
        }
    }
    check(crossed, "crossfade crosses to track B", "serial 1");
    NAE_SignalPath after{};
    after.structSize = sizeof(after);
    check(NAE_QuerySignalPath(handle, &after) == NAE_OK, "counters after the mix");
    check(after.underrunCount == before.underrunCount, "no underruns at the crossfade",
          std::to_string(before.underrunCount) + " -> " +
              std::to_string(after.underrunCount));
    check(after.padFrameCount == before.padFrameCount, "no invented frames at the crossfade",
          std::to_string(before.padFrameCount) + " -> " +
              std::to_string(after.padFrameCount));
    check(NAE_Position(handle) > 0.4 && NAE_Position(handle) < 0.8,
          "next track starts after the fade, not at zero",
          std::to_string(NAE_Position(handle)));
    check(NAE_GetCrossfade(handle, &xf) == NAE_OK && xf.durationMs == 500.0,
          "crossfade readback matches");
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
    NAE_CrossfadeParams sxf;
    NAE_CrossfadeDefaults(&sxf);
    sxf.enabled = 1;
    sxf.durationMs = 500.0;
    sxf.curve = NAE_CROSSFADE_EQUAL_POWER;
    check(NAE_SessionSetCrossfade(session, &sxf) == NAE_OK, "session accepts crossfade");
    check(NAE_SessionPlay(session) == NAE_OK, "session plays");

    NAE_SessionState st{};
    bool advanced = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<int>(durA) + 3000);
    while (std::chrono::steady_clock::now() < deadline) {
        getState(session, &st);
        if (st.trackSerial == 1) {
            advanced = true;
            break;
        }
        if (st.state == NAE_SESSION_ERROR || st.state == NAE_SESSION_ENDED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(advanced, "session serial advances through the crossfade",
          "serial " + std::to_string(st.trackSerial));
    if (advanced) {
        check(st.state == NAE_SESSION_PLAYING, "session stays PLAYING through the mix");
        check(st.position > 0.4 && st.position < 0.7,
              "session position includes the faded part of track B",
              std::to_string(st.position));
        check(std::fabs(st.duration - durB / 1000.0) < 0.11, "session duration is track B",
              std::to_string(st.duration) + " vs B's " + std::to_string(durB / 1000.0));
    }
    check(NAE_SessionStop(session) == NAE_OK, "session stops");
    NAE_SessionClose(session);

    std::printf("\n%s\n", failures == 0 ? "All crossfade checks passed."
                                        : "Some crossfade checks FAILED.");
    return failures == 0 ? 0 : 1;
}
