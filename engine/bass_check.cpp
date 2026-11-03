#include "routing.h"
#include "nae.h"

#include <algorithm>
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
}  // namespace

int main() {
    NAE_RoutingMatrix m{};
    check(NAE_BuildRoutingMatrix(NAE_ROUTING_UPMIX, NAE_UPMIX_STEREO_TO_5_1_BASS,
                                 NAE_LAYOUT_STEREO, NAE_LAYOUT_5_1, &m) == NAE_OK,
          "stereo->5.1 bass matrix builds");
    m.bassManagement = NAE_BASS_ON;
    m.lfeMode = NAE_LFE_LOW_PASS_SUM;
    m.lfeCrossoverHz = 80.0;
    m.sampleRate = 48000.0;
    m.lfeGainDb = 0.0;

    neiro::RoutingProcessor routing;
    std::string error;
    check(routing.configure(m, 512, &error), "routing configures with bass", error);

    constexpr size_t frames = 48000;
    std::vector<float> in(frames * 2, 0.0f), out(frames * 6, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 40.0 * i / 48000.0));
        in[i * 2] = s;
        in[i * 2 + 1] = s;
    }
    for (size_t off = 0; off < frames; off += 512) {
        const size_t n = std::min<size_t>(512, frames - off);
        routing.process(in.data() + off * 2, n, out.data() + off * 6);
    }

    double lfePeak = 0.0;
    for (size_t i = 1000; i + 1000 < frames; ++i) {
        lfePeak = std::max(lfePeak, static_cast<double>(std::fabs(out[i * 6 + 3])));
    }
    check(lfePeak > 0.05, "LFE carries a 40 Hz tone", std::to_string(lfePeak));

    // reset() must clear the LFE one-pole state. Without that, silence after a
    // reset still carries the tail of the previous program for ~100 ms.
    routing.reset();
    {
        std::vector<float> silence(512 * 2, 0.0f);
        std::vector<float> quiet(512 * 6, 0.0f);
        routing.process(silence.data(), 512, quiet.data());
        double residual = 0.0;
        for (size_t i = 0; i < 512; ++i) {
            residual = std::max(residual, static_cast<double>(std::fabs(quiet[i * 6 + 3])));
        }
        check(residual == 0.0, "reset clears the LFE low-pass state",
              std::to_string(residual));
    }

    // A 5 kHz tone is above the crossover and must not reach the LFE.
    for (size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 5000.0 * i / 48000.0));
        in[i * 2] = s;
        in[i * 2 + 1] = s;
    }
    routing.reset();
    for (size_t off = 0; off < frames; off += 512) {
        const size_t n = std::min<size_t>(512, frames - off);
        routing.process(in.data() + off * 2, n, out.data() + off * 6);
    }
    double lfeHigh = 0.0;
    for (size_t i = 1000; i + 1000 < frames; ++i) {
        lfeHigh = std::max(lfeHigh, static_cast<double>(std::fabs(out[i * 6 + 3])));
    }
    check(lfeHigh < 0.05, "LFE rejects a 5 kHz tone", std::to_string(lfeHigh));

    std::printf("\n%s\n", failures == 0 ? "All bass checks passed."
                                        : "Some bass checks FAILED.");
    return failures == 0 ? 0 : 1;
}
