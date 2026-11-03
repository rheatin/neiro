// ReplayGain changes the level by a tagged amount, so the check measures the
// level and reads the tag back.
//
// The fixture is a 1 kHz FLAC with REPLAYGAIN_TRACK_GAIN=-6 dB and
// REPLAYGAIN_ALBUM_GAIN=-3 dB. The spectrum tap measures the tone's bin:
// -6 dB is 0.5012 of unity, -3 dB is 0.7079. Clipping prevention is disabled for
// the exact ratio, then the policy is turned off and the level must return.

#include "nae.h"
#include "utf8_args.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

bool measure(NAE_Handle* handle, double* peak, int32_t* bitPerfect, int32_t* reason) {
    for (int i = 0; i < 25; ++i) NAE_Play(handle, 20);
    NAE_SpectrumInfo info{};
    info.struct_size = sizeof(info);
    std::vector<float> bins(4096, 0.0f);
    if (NAE_QuerySpectrum(handle, &info, bins.data(),
                          static_cast<int32_t>(bins.size())) != NAE_OK) {
        return false;
    }
    *peak = info.peak;
    NAE_SignalPath path{};
    path.structSize = sizeof(path);
    const int32_t rc = NAE_QuerySignalPath(handle, &path);
    *bitPerfect = rc == NAE_OK ? path.bitPerfect : 0;
    *reason = rc == NAE_OK ? path.reason : -1;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: replaygain_check <file-with-replaygain-tags.flac>\n");
        return 1;
    }

    NAE_Handle* handle = nullptr;
    int32_t rate = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, args[1].c_str(), NAE_OUTPUT_NATIVE, 0,
                                    NAE_QUALITY_HIGH, &handle, &rate, &sourceRate, &channels,
                                    &bits, &valid, &isFloat, &resampling, &dithered,
                                    &bitPerfect, &fileChannels, &durationMs, &latencyMs);
    if (rc != NAE_OK || !handle) {
        std::printf("open failed: rc=%d\n", rc);
        return 1;
    }

    NAE_ReplayGainInfo info{};
    info.structSize = sizeof(info);
    check(NAE_QueryReplayGain(handle, &info) == NAE_OK, "ReplayGain metadata is readable");
    check(info.trackValid && std::fabs(info.trackGainDb + 6.0) < 0.01,
          "track gain is -6 dB", std::to_string(info.trackGainDb));
    check(info.albumValid && std::fabs(info.albumGainDb + 3.0) < 0.01,
          "album gain is -3 dB", std::to_string(info.albumGainDb));

    double unityPeak = 0.0, trackPeak = 0.0, albumPeak = 0.0, offPeak = 0.0;
    int32_t bpUnity = 0, reasonUnity = -1, bpTrack = 0, reasonTrack = -1, bpAlbum = 0,
            reasonAlbum = -1, bpOff = 0, reasonOff = -1;
    check(measure(handle, &unityPeak, &bpUnity, &reasonUnity), "unity measurement runs");
    check(unityPeak > 0.001, "unity tone has signal", std::to_string(unityPeak));

    NAE_ReplayGainParams params;
    NAE_ReplayGainDefaults(&params);
    params.mode = NAE_RG_TRACK;
    params.preventClipping = 0;
    check(NAE_SetReplayGain(handle, &params) == NAE_OK, "track policy is accepted");
    check(std::fabs(NAE_GetReplayGainGain(handle) - 0.501187) < 0.001,
          "track policy resolves to -6 dB",
          std::to_string(NAE_GetReplayGainGain(handle)));
    check(measure(handle, &trackPeak, &bpTrack, &reasonTrack), "track measurement runs");
    check(std::fabs(trackPeak / unityPeak - 0.501187) < 0.02,
          "track gain changes the level by -6 dB",
          "ratio " + std::to_string(trackPeak / unityPeak));
    if (bpUnity) {
        check(bpTrack == 0 && reasonTrack == NAE_REASON_REPLAYGAIN,
              "track policy reports not bit-perfect",
              "bitPerfect=" + std::to_string(bpTrack) + " reason=" +
                  std::to_string(reasonTrack));
    }

    params.mode = NAE_RG_ALBUM;
    check(NAE_SetReplayGain(handle, &params) == NAE_OK, "album policy is accepted");
    check(measure(handle, &albumPeak, &bpAlbum, &reasonAlbum), "album measurement runs");
    check(std::fabs(albumPeak / unityPeak - 0.707946) < 0.02,
          "album gain changes the level by -3 dB",
          "ratio " + std::to_string(albumPeak / unityPeak));

    params.mode = NAE_RG_OFF;
    check(NAE_SetReplayGain(handle, &params) == NAE_OK, "off policy is accepted");
    check(measure(handle, &offPeak, &bpOff, &reasonOff), "off measurement runs");
    check(std::fabs(offPeak / unityPeak - 1.0) < 0.05, "off restores the level",
          "ratio " + std::to_string(offPeak / unityPeak));
    if (bpUnity) {
        check(bpOff == 1 && reasonOff == 0, "off restores bit-perfect",
              "bitPerfect=" + std::to_string(bpOff) + " reason=" +
                  std::to_string(reasonOff));
    }

    params.mode = 99;
    check(NAE_SetReplayGain(handle, &params) == NAE_ERR_INVALID_ARGUMENT,
          "an unknown mode is rejected");
    params.mode = NAE_RG_TRACK;
    params.preampDb = 30.0;
    check(NAE_SetReplayGain(handle, &params) == NAE_ERR_INVALID_ARGUMENT,
          "preamp above +20 dB is rejected");

    NAE_Close(handle);
    NAE_Shutdown();

    std::printf("\n%s\n", failures == 0 ? "All ReplayGain checks passed."
                                        : "Some ReplayGain checks FAILED.");
    return failures == 0 ? 0 : 1;
}
