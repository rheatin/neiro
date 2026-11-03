// Crossfade is a mix, so the check has to see both tracks at once and then the
// next track's position after the seam.
//
// Two 1 s files (300 Hz and 600 Hz) are used. At 0.75 s, with a 0.5 s fade, both
// bins must be present. After the swap the serial is 1, the position starts at
// about the fade length rather than zero, and the underrun/pad counters must not
// move. The session path repeats the position/serial check through the queue.

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

double binNear(const std::vector<float>& bins, double hz, double rate, uint32_t fft) {
    const double perBin = rate / static_cast<double>(fft);
    const int index = static_cast<int>(std::lround(hz / perBin));
    double peak = 0.0;
    for (int i = std::max(0, index - 1); i <= index + 1 && i < static_cast<int>(bins.size());
         ++i) {
        peak = std::max(peak, static_cast<double>(bins[i]));
    }
    return peak;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 3) {
        std::printf("usage: crossfade_check <trackA> <trackB>\n");
        return 1;
    }

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
    for (int i = 0; i < 400 && NAE_Position(handle) < 0.70; ++i) NAE_Play(handle, 5);

    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(info);
    std::vector<float> bins(4096, 0.0f);
    check(NAE_QuerySpectrum(handle, &info, bins.data(),
                            static_cast<int32_t>(bins.size())) == NAE_OK,
          "spectrum at the midpoint");
    const double low = binNear(bins, 300.0, 44100.0, info.fftSize ? info.fftSize : 4096);
    const double high = binNear(bins, 600.0, 44100.0, info.fftSize ? info.fftSize : 4096);
    check(low > 0.005 && high > 0.005, "both tracks are present mid-fade",
          "pos " + std::to_string(NAE_Position(handle)) + " bins " +
              std::to_string(low) + " / " + std::to_string(high));

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
    check(advanced, "session serial advances through the crossfade",
          "serial " + std::to_string(st.trackSerial));
    if (advanced) {
        check(st.state == NAE_SESSION_PLAYING, "session stays PLAYING through the mix");
        check(st.position > 0.4 && st.position < 0.7,
              "session position includes the faded part of track B",
              std::to_string(st.position));
        check(st.duration > 0.9 && st.duration < 1.1, "session duration is track B",
              std::to_string(st.duration));
    }
    check(NAE_SessionStop(session) == NAE_OK, "session stops");
    NAE_SessionClose(session);

    std::printf("\n%s\n", failures == 0 ? "All crossfade checks passed."
                                        : "Some crossfade checks FAILED.");
    return failures == 0 ? 0 : 1;
}
