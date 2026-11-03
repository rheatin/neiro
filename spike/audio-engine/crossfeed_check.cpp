// Crossfeed is a stereo change, so the check needs a signal that exists in only
// one channel and a way to see it appear in the other.
//
// The test writes a 44.1 kHz stereo WAV with a 300 Hz tone in the right channel
// and silence in the left, opens it natively, and measures channel 0 through the
// spectrum tap. Crossfeed off: the left channel is silence. Crossfeed on: the
// delayed, low-passed right channel appears in the left. The filter is then
// disabled and the left channel must go quiet again.

#include "nae.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

void put16(std::vector<uint8_t>* out, uint16_t v) {
    out->push_back(static_cast<uint8_t>(v));
    out->push_back(static_cast<uint8_t>(v >> 8));
}
void put32(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

bool writeFixture(const char* path) {
    constexpr uint32_t rate = 44100;
    constexpr uint32_t seconds = 2;
    constexpr uint32_t frames = rate * seconds;
    constexpr uint16_t channels = 2;
    constexpr uint16_t bits = 16;

    std::vector<uint8_t> wav;
    wav.reserve(44 + frames * channels * 2);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(&wav, 36 + frames * channels * 2);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    put32(&wav, 16);
    put16(&wav, 1);  // PCM
    put16(&wav, channels);
    put32(&wav, rate);
    put32(&wav, rate * channels * (bits / 8));
    put16(&wav, channels * (bits / 8));
    put16(&wav, bits);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(&wav, frames * channels * (bits / 8));

    for (uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double right = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 300.0 * t);
        put16(&wav, 0);  // left
        put16(&wav, static_cast<uint16_t>(static_cast<int16_t>(right * 32767.0)));
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t written = std::fwrite(wav.data(), 1, wav.size(), f);
    std::fclose(f);
    return written == wav.size();
}

bool measureLeft(NAE_Handle* handle, double* peak, int32_t* bitPerfect, int32_t* reason) {
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

int main() {
    const char* fixture = "crossfeed_test.wav";
    if (!writeFixture(fixture)) {
        std::printf("could not write %s\n", fixture);
        return 1;
    }

    NAE_Handle* handle = nullptr;
    int32_t rate = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, fixture, NAE_OUTPUT_NATIVE, 0, NAE_QUALITY_HIGH,
                                    &handle, &rate, &sourceRate, &channels, &bits, &valid,
                                    &isFloat, &resampling, &dithered, &bitPerfect,
                                    &fileChannels, &durationMs, &latencyMs);
    if (rc != NAE_OK || !handle) {
        std::printf("open failed: rc=%d (native 44.1 kHz stereo)\n", rc);
        std::remove(fixture);
        return 1;
    }

    double offPeak = 0.0, onPeak = 0.0, restoredPeak = 0.0;
    int32_t bpOff = 0, reasonOff = -1, bpOn = 0, reasonOn = -1, bpRestored = 0,
            reasonRestored = -1;
    check(measureLeft(handle, &offPeak, &bpOff, &reasonOff), "crossfeed-off measurement runs");
    check(offPeak < 0.01, "left channel is silent with crossfeed off",
          std::to_string(offPeak));

    NAE_CrossfeedParams cf;
    NAE_CrossfeedDefaults(&cf);
    cf.enabled = 1;
    cf.amount = 0.5;
    cf.delayMs = 0.3;
    cf.cutoffHz = 700.0;
    check(NAE_SetCrossfeed(handle, &cf) == NAE_OK, "crossfeed is accepted");
    check(measureLeft(handle, &onPeak, &bpOn, &reasonOn), "crossfeed-on measurement runs");
    check(onPeak > 0.05, "crossfeed puts the right channel into the left",
          std::to_string(onPeak));
    if (bpOff) {
        check(bpOn == 0 && reasonOn == NAE_REASON_CROSSFEED,
              "crossfeed reports not bit-perfect",
              "bitPerfect=" + std::to_string(bpOn) + " reason=" +
                  std::to_string(reasonOn));
    }

    cf.enabled = 0;
    check(NAE_SetCrossfeed(handle, &cf) == NAE_OK, "crossfeed can be disabled");
    check(measureLeft(handle, &restoredPeak, &bpRestored, &reasonRestored),
          "restored measurement runs");
    check(restoredPeak < 0.01, "left channel is silent again",
          std::to_string(restoredPeak));
    if (bpOff) {
        check(bpRestored == 1 && reasonRestored == 0,
              "disabling crossfeed restores bit-perfect",
              "bitPerfect=" + std::to_string(bpRestored) + " reason=" +
                  std::to_string(reasonRestored));
    }

    cf.enabled = 1;
    cf.amount = 2.0;
    check(NAE_SetCrossfeed(handle, &cf) == NAE_ERR_INVALID_ARGUMENT,
          "amount above one is rejected");
    cf.amount = 0.3;
    cf.delayMs = 10.0;
    check(NAE_SetCrossfeed(handle, &cf) == NAE_ERR_INVALID_ARGUMENT,
          "delay above five milliseconds is rejected");
    cf.delayMs = 0.3;
    cf.cutoffHz = 30000.0;
    check(NAE_SetCrossfeed(handle, &cf) == NAE_ERR_INVALID_ARGUMENT,
          "cutoff above the stream's Nyquist is rejected");

    NAE_Close(handle);
    NAE_Shutdown();
    std::remove(fixture);

    std::printf("\n%s\n", failures == 0 ? "All crossfeed checks passed."
                                        : "Some crossfeed checks FAILED.");
    return failures == 0 ? 0 : 1;
}
