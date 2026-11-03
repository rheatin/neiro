// The convolver is a finite impulse response, so the check uses an IR whose
// response is known: a single 0.5 sample at position 0. That is a 0.5 gain and no
// tail, so the measured tone must halve. Disabling restores it.
//
// The source and the IR are both generated at 44.1 kHz stereo: the IR is
// required to be at the source rate, because the convolution runs before the
// resampler.

#include "nae.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
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

bool writeWav(const char* path, uint32_t frames, bool impulseIr) {
    constexpr uint32_t rate = 44100;
    constexpr uint16_t channels = 2;
    constexpr uint16_t bits = 16;
    std::vector<uint8_t> wav;
    wav.reserve(44 + frames * channels * 2);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(&wav, 36 + frames * channels * 2);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    put32(&wav, 16);
    put16(&wav, 1);
    put16(&wav, channels);
    put32(&wav, rate);
    put32(&wav, rate * channels * (bits / 8));
    put16(&wav, channels * (bits / 8));
    put16(&wav, bits);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(&wav, frames * channels * (bits / 8));

    for (uint32_t i = 0; i < frames; ++i) {
        double left = 0.0;
        double right = 0.0;
        if (impulseIr) {
            if (i == 0) {
                left = 0.5;
                right = 0.5;
            }
        } else {
            const double t = static_cast<double>(i) / rate;
            const double s = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t);
            left = s;
            right = s;
        }
        put16(&wav, static_cast<uint16_t>(static_cast<int16_t>(left * 32767.0)));
        put16(&wav, static_cast<uint16_t>(static_cast<int16_t>(right * 32767.0)));
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t written = std::fwrite(wav.data(), 1, wav.size(), f);
    std::fclose(f);
    return written == wav.size();
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

int main() {
    const char* source = "convolver_source.wav";
    const char* ir = "convolver_ir.wav";
    if (!writeWav(source, 44100 * 2, false) || !writeWav(ir, 1024, true)) {
        std::printf("could not write fixtures\n");
        return 1;
    }

    NAE_Handle* handle = nullptr;
    int32_t rate = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    if (NAE_OpenFile(nullptr, source, NAE_OUTPUT_NATIVE, 0, NAE_QUALITY_HIGH, &handle, &rate,
                     &sourceRate, &channels, &bits, &valid, &isFloat, &resampling, &dithered,
                     &bitPerfect, &fileChannels, &durationMs, &latencyMs) != NAE_OK ||
        !handle) {
        std::printf("open failed\n");
        std::remove(source);
        std::remove(ir);
        return 1;
    }

    double unityPeak = 0.0, convPeak = 0.0, restoredPeak = 0.0;
    int32_t bpUnity = 0, reasonUnity = -1, bpConv = 0, reasonConv = -1, bpRestored = 0,
            reasonRestored = -1;
    check(measure(handle, &unityPeak, &bpUnity, &reasonUnity), "unity measurement runs");
    check(unityPeak > 0.001, "unity tone has signal", std::to_string(unityPeak));

    NAE_ConvolverParams params;
    NAE_ConvolverDefaults(&params);
    params.enabled = 1;
    std::snprintf(params.irPath, sizeof(params.irPath), "%s", ir);
    params.wet = 1.0;
    params.dry = 0.0;
    params.gainDb = 0.0;
    params.normalize = 0;
    check(NAE_SetConvolver(handle, &params) == NAE_OK, "convolver is accepted");
    check(measure(handle, &convPeak, &bpConv, &reasonConv), "convolver measurement runs");
    check(std::fabs(convPeak / unityPeak - 0.5) < 0.02,
          "the 0.5 delta IR halves the tone",
          "ratio " + std::to_string(convPeak / unityPeak));
    if (bpUnity) {
        check(bpConv == 0 && reasonConv == NAE_REASON_CONVOLVER,
              "convolver reports not bit-perfect",
              "bitPerfect=" + std::to_string(bpConv) + " reason=" +
                  std::to_string(reasonConv));
    }

    NAE_ConvolverParams readback;
    NAE_ConvolverDefaults(&readback);
    check(NAE_GetConvolver(handle, &readback) == NAE_OK && readback.enabled == 1,
          "convolver readback is enabled");

    params.enabled = 0;
    check(NAE_SetConvolver(handle, &params) == NAE_OK, "convolver can be disabled");
    check(measure(handle, &restoredPeak, &bpRestored, &reasonRestored),
          "restored measurement runs");
    check(std::fabs(restoredPeak / unityPeak - 1.0) < 0.05, "disabling restores the level",
          "ratio " + std::to_string(restoredPeak / unityPeak));
    if (bpUnity) {
        check(bpRestored == 1 && reasonRestored == 0, "disabling restores bit-perfect",
              "bitPerfect=" + std::to_string(bpRestored) + " reason=" +
                  std::to_string(reasonRestored));
    }

    params.enabled = 1;
    params.wet = 2.0;
    check(NAE_SetConvolver(handle, &params) == NAE_ERR_INVALID_ARGUMENT,
          "wet above one is rejected");

    NAE_Close(handle);
    NAE_Shutdown();
    std::remove(source);
    std::remove(ir);

    std::printf("\n%s\n", failures == 0 ? "All convolver checks passed."
                                        : "Some convolver checks FAILED.");
    return failures == 0 ? 0 : 1;
}
