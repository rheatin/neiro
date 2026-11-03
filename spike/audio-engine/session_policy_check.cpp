#include "nae.h"
#include "utf8_args.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: session_policy_check <file>\n");
        return 1;
    }

    NAE_SessionConfig config{};
    config.structSize = sizeof(config);
    config.path = args[1].c_str();
    config.outputMode = NAE_OUTPUT_NATIVE;
    config.quality = NAE_QUALITY_HIGH;
    config.shareMode = NAE_SHARE_EXCLUSIVE;
    config.sliceMs = 16;

    NAE_OutputPolicy policy{};
    NAE_OutputPolicyDefaults(&policy);
    policy.ratePolicy = NAE_RATE_NATIVE;
    policy.ditherMode = NAE_DITHER_OFF;
    policy.clipGuardMode = NAE_CLIP_GUARD_OFF;
    policy.bassManagement = NAE_BASS_OFF;

    NAE_Session* session = nullptr;
    int32_t rc = NAE_SessionOpenWithPolicy(&config, &policy, &session);
    check(rc == NAE_OK && session != nullptr, "policy open succeeds");
    if (rc != NAE_OK || !session) {
        std::printf("open failed: %d %s\n", rc, NAE_GetLastError());
        return 1;
    }

    NAE_OutputPolicy got{};
    got.structSize = sizeof(got);
    check(NAE_SessionGetOutputPolicy(session, &got) == NAE_OK, "requested policy is readable");
    check(got.ratePolicy == NAE_RATE_NATIVE, "rate policy round-trips",
          std::to_string(got.ratePolicy));
    check(got.ditherMode == NAE_DITHER_OFF, "dither policy round-trips",
          std::to_string(got.ditherMode));

    NAE_ResolvedSignalPath r{};
    r.structSize = sizeof(r);
    check(NAE_SessionGetResolvedSignalPath(session, &r) == NAE_OK,
          "resolved path after policy open");
    check(r.sampleRate == 44100, "native policy keeps 44.1k",
          std::to_string(r.sampleRate));
    check(r.resampling == 0, "native policy does not resample");
    check(r.ditherActive == 0, "dither off is reflected in the actual path");
    check(r.clipGuardMode == NAE_CLIP_GUARD_OFF && r.clipGuardActive == 0,
          "clip guard off is reflected in the actual path");
    check(r.bufferMode == NAE_BUFFER_AUTO, "buffer mode is reported",
          std::to_string(r.bufferMode));
    check(r.diagnosticCount > 0, "policy path has diagnostics",
          std::to_string(r.diagnosticCount));

    NAE_SessionClose(session);

    // A limiter policy must actually be applied, not just accepted.
    {
        NAE_OutputPolicy limiter{};
        NAE_OutputPolicyDefaults(&limiter);
        limiter.ratePolicy = NAE_RATE_NATIVE;
        limiter.clipGuardMode = NAE_CLIP_GUARD_LIMITER;
        limiter.clipCeilingDbTp = -1.0;
        limiter.clipLookaheadMs = 5.0;
        limiter.clipReleaseMs = 100.0;
        session = nullptr;
        rc = NAE_SessionOpenWithPolicy(&config, &limiter, &session);
        check(rc == NAE_OK && session != nullptr, "limiter policy opens");
        if (session) {
            r = NAE_ResolvedSignalPath{};
            r.structSize = sizeof(r);
            check(NAE_SessionGetResolvedSignalPath(session, &r) == NAE_OK,
                  "limiter path is readable");
            check(r.clipGuardActive == 1, "limiter is active in the actual path");
            check(r.clipCeilingDbTp == -1.0, "limiter ceiling is reported");
            NAE_SessionClose(session);
        }
    }

    // A policy field the session cannot apply must be reported, not ignored.
    NAE_OutputPolicy unsupported{};
    NAE_OutputPolicyDefaults(&unsupported);
    unsupported.ratePolicy = NAE_RATE_NATIVE;
    unsupported.dsdMode = NAE_DSD_MODE_DSD;
    unsupported.clipGuardMode = NAE_CLIP_GUARD_LIMITER;
    unsupported.clipCeilingDbTp = -1.0;
    unsupported.clipLookaheadMs = 5.0;
    unsupported.clipReleaseMs = 100.0;
    session = nullptr;
    rc = NAE_SessionOpenWithPolicy(&config, &unsupported, &session);
    check(rc == NAE_OK && session != nullptr, "unsupported fields do not block PCM playback");
    if (session) {
        r = NAE_ResolvedSignalPath{};
        r.structSize = sizeof(r);
        check(NAE_SessionGetResolvedSignalPath(session, &r) == NAE_OK,
              "unsupported policy path is readable");
        bool dsdWarning = false;
        bool clipWarning = false;
        for (uint32_t i = 0; i < r.diagnosticCount; ++i) {
            const char* msg = r.diagnostics[i].message;
            if (std::strstr(msg, "DSD policy")) dsdWarning = true;
            if (std::strstr(msg, "Clip guard")) clipWarning = true;
        }
        check(dsdWarning, "DSD policy warning is present");
        NAE_SessionClose(session);
        (void)clipWarning;
    }

    std::printf("\n%s\n", failures == 0 ? "All session policy checks passed."
                                        : "Some session policy checks FAILED.");
    return failures == 0 ? 0 : 1;
}
