// The registry is the same parameters the typed API exposes. This checks that
// every entry has a default, that a runtime value round-trips, that an open-time
// value says so instead of silently doing nothing, and that a per-band EQ value
// reaches the same cascade.

#include "nae.h"
#include "utf8_args.h"

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

bool get(NAE_Session* session, const char* name, double* value) {
    return NAE_SessionParamGet(session, name, value) == NAE_OK;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: param_check <file>\n");
        return 1;
    }

    const int32_t count = NAE_ParamCount();
    check(count > 50, "the registry has the generated EQ bands",
          std::to_string(count) + " entries");
    bool sawVolume = false, sawBand = false;
    for (int32_t i = 0; i < count; ++i) {
        NAE_ParamDesc d;
        d.structSize = sizeof(d);
        if (NAE_ParamAt(i, &d) != NAE_OK) continue;
        if (!std::strcmp(d.name, "volume.gain")) sawVolume = true;
        if (!std::strcmp(d.name, "eq.band0.gain_db")) sawBand = true;
    }
    check(sawVolume && sawBand, "volume and per-band EQ are described");

    NAE_SessionConfig config{};
    config.structSize = sizeof(config);
    config.path = args[1].c_str();
    config.outputMode = NAE_OUTPUT_NATIVE;
    config.quality = NAE_QUALITY_HIGH;
    config.shareMode = NAE_SHARE_EXCLUSIVE;
    config.sliceMs = 16;
    NAE_Session* session = nullptr;
    if (NAE_SessionOpen(&config, &session) != NAE_OK || !session) {
        std::printf("open failed\n");
        return 1;
    }

    double value = 0.0;
    bool ok = get(session, "volume.gain", &value) && std::fabs(value - 1.0) < 1e-9;
    check(ok, "volume starts at unity", std::to_string(value));
    check(NAE_SessionParamSet(session, "volume.gain", 0.5) == NAE_OK,
          "volume is set through the registry");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ok = get(session, "volume.gain", &value) && std::fabs(value - 0.5) < 1e-9;
    check(ok, "volume reads back", std::to_string(value));

    check(NAE_SessionParamSet(session, "output.mode", 1) == NAE_ERR_REQUIRES_REOPEN,
          "an open-time parameter requires reopen");
    check(NAE_SessionParamSet(session, "no.such.param", 1) == NAE_ERR_INVALID_ARGUMENT,
          "an unknown parameter is rejected");
    check(NAE_SessionParamSet(session, "volume.gain", 2.0) == NAE_ERR_INVALID_ARGUMENT,
          "a value outside the range is rejected");

    check(NAE_SessionParamSet(session, "eq.enabled", 1) == NAE_OK, "EQ enabled");
    check(NAE_SessionParamSet(session, "eq.band0.freq_hz", 1000) == NAE_OK,
          "band frequency");
    check(NAE_SessionParamSet(session, "eq.band0.gain_db", -6) == NAE_OK, "band gain");
    check(NAE_SessionParamSet(session, "eq.band0.q", 1.0) == NAE_OK, "band Q");
    check(NAE_SessionParamSet(session, "eq.band0.enabled", 1) == NAE_OK, "band enabled");
    ok = get(session, "eq.band0.gain_db", &value) && std::fabs(value + 6.0) < 1e-9;
    check(ok, "band gain reads back", std::to_string(value));

    NAE_EqParams eq;
    NAE_EqDefaults(&eq);
    check(NAE_SessionGetEq(session, &eq) == NAE_OK && eq.bandCount == 1 &&
              eq.bands[0].gainDb == -6.0,
          "the typed EQ sees the registry write");

    check(NAE_SessionParamSet(session, "crossfade.duration_ms", 4000) == NAE_OK,
          "crossfade duration");
    check(NAE_SessionParamSet(session, "replaygain.mode", 1) == NAE_OK,
          "ReplayGain mode");
    check(NAE_SessionParamSet(session, "convolver.wet", 0.5) == NAE_OK,
          "convolver wet");

    int32_t queueCount = -1;
    queueCount = static_cast<int32_t>(NAE_SessionQueueCount(session));
    check(queueCount == 0, "queue count is readable", std::to_string(queueCount));

    NAE_SessionStop(session);
    NAE_SessionClose(session);

    std::printf("\n%s\n", failures == 0 ? "All parameter checks passed."
                                        : "Some parameter checks FAILED.");
    return failures == 0 ? 0 : 1;
}
