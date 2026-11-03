#include "nae.h"
#include "utf8_args.h"

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

int main(int argc, char** argv) {
    const auto args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: session_dsd_check <file.dsf|dff> [device]\n");
        return 1;
    }
    NAE_SessionConfig cfg{};
    cfg.structSize = sizeof(cfg);
    cfg.path = args[1].c_str();
    cfg.deviceId = args.size() > 2 ? args[2].c_str() : nullptr;
    cfg.outputMode = NAE_OUTPUT_DEVICE;
    cfg.quality = NAE_QUALITY_HIGH;
    cfg.shareMode = NAE_SHARE_EXCLUSIVE;
    cfg.sliceMs = 16;

    NAE_OutputPolicy policy{};
    NAE_OutputPolicyDefaults(&policy);
    policy.dsdMode = NAE_DSD_MODE_AUTO;
    policy.dsdTransport = NAE_DSD_TRANSPORT_AUTO;
    policy.allowPcmFallback = 1;
    policy.ditherMode = NAE_DITHER_TPDF;
    policy.clipGuardMode = NAE_CLIP_GUARD_OFF;

    NAE_Session* session = nullptr;
    const int32_t rc = NAE_SessionOpenWithPolicy(&cfg, &policy, &session);
    check(rc == NAE_OK && session != nullptr, "DSD session opens or falls back",
          std::to_string(rc));
    if (rc != NAE_OK || !session) {
        std::printf("open failed: %s\n", NAE_GetLastError());
        return 1;
    }
    NAE_ResolvedSignalPath r{};
    r.structSize = sizeof(r);
    check(NAE_SessionGetResolvedSignalPath(session, &r) == NAE_OK,
          "DSD resolved path readable");
    check(r.sourceIsDsd == 1, "source is DSD");
    check(r.dsdModeResolved == NAE_DSD_MODE_DSD ||
              r.dsdPcmFallback == 1,
          "resolved DSD or PCM fallback");
    if (r.dsdModeResolved == NAE_DSD_MODE_DSD) {
        check(r.dsdTransportResolved == NAE_DSD_TRANSPORT_DOP,
              "DSD transport is DoP");
        check(r.bitPerfect == 1, "DoP path is bit-perfect");
    } else {
        check(r.dsdPcmFallback == 1, "PCM fallback is marked");
        bool fallbackDiag = false;
        for (uint32_t i = 0; i < r.diagnosticCount; ++i)
            if (std::string(r.diagnostics[i].message).find("fell back to PCM") != std::string::npos)
                fallbackDiag = true;
        check(fallbackDiag, "PCM fallback diagnostic exists");
    }
    NAE_SessionClose(session);

    // Explicit extract must take the PCM path and be reported as extracted.
    {
        NAE_OutputPolicy extract{};
        NAE_OutputPolicyDefaults(&extract);
        extract.dsdMode = NAE_DSD_MODE_PCM;
        extract.dsdRatePolicy = NAE_DSD_RATE_EXTRACT;
        extract.allowExtractRate = 1;
        session = nullptr;
        const int32_t extractRc = NAE_SessionOpenWithPolicy(&cfg, &extract, &session);
        check(extractRc == NAE_OK && session != nullptr, "DSD extract opens",
              std::to_string(extractRc));
        if (session) {
            r = NAE_ResolvedSignalPath{};
            r.structSize = sizeof(r);
            check(NAE_SessionGetResolvedSignalPath(session, &r) == NAE_OK,
                  "extract path is readable");
            check(r.dsdExtracted == 1, "extract is reported");
            check(r.dsdModeResolved == NAE_DSD_MODE_PCM, "extract resolves to PCM");
            NAE_SessionClose(session);
        }
    }

    // Bitstream downrate: only meaningful above DSD64. The resolved path must
    // report the lowered rate and carrier, and the diagnostic must say the
    // stream was decimated rather than that it was ignored.
    {
        NAE_Session* down = nullptr;
        NAE_OutputPolicy downPolicy{};
        NAE_OutputPolicyDefaults(&downPolicy);
        downPolicy.dsdMode = NAE_DSD_MODE_AUTO;
        downPolicy.dsdTransport = NAE_DSD_TRANSPORT_AUTO;
        downPolicy.dsdRatePolicy = NAE_DSD_RATE_DOWNRATE;
        downPolicy.allowPcmFallback = 1;
        const int32_t rcDown = NAE_SessionOpenWithPolicy(&cfg, &downPolicy, &down);
        check(rcDown == NAE_OK && down != nullptr, "downrate session opens",
              std::to_string(rcDown));
        if (down) {
            NAE_ResolvedSignalPath d{};
            d.structSize = sizeof(d);
            check(NAE_SessionGetResolvedSignalPath(down, &d) == NAE_OK,
                  "downrate resolved path readable");
            if (d.sourceDsdRate > 2822400u) {
                if (d.dsdTransportResolved == NAE_DSD_TRANSPORT_DOP) {
                    check(d.dsdDownrated == 1, "downrate is reported");
                    check(d.dsdRateResolved < d.sourceDsdRate,
                          "resolved DSD rate is lower",
                          std::to_string(d.dsdRateResolved) + " < " +
                              std::to_string(d.sourceDsdRate));
                    check(d.dsdCarrierRate == d.dsdRateResolved / 16,
                          "carrier matches the lowered rate",
                          std::to_string(d.dsdCarrierRate));
                    check(d.dsdReason == NAE_RESOLVE_REASON_DSD_DOWNRATED,
                          "reason says downrated", std::to_string(d.dsdReason));
                    bool diag = false;
                    for (uint32_t i = 0; i < d.diagnosticCount; ++i) {
                        if (std::string(d.diagnostics[i].message).find("decimated") !=
                            std::string::npos) {
                            diag = true;
                        }
                    }
                    check(diag, "downrate diagnostic exists");
                } else {
                    check(d.dsdPcmFallback == 1,
                          "no DoP carrier means an explicit PCM fallback");
                }
            }
            NAE_SessionClose(down);
        }
    }

    // Native DSD is not available on WASAPI: without fallback it must fail,
    // rather than silently playing PCM.
    NAE_OutputPolicy native{};
    NAE_OutputPolicyDefaults(&native);
    native.dsdMode = NAE_DSD_MODE_DSD;
    native.dsdTransport = NAE_DSD_TRANSPORT_NATIVE;
    native.allowPcmFallback = 0;
    session = nullptr;
    const int32_t nativeRc = NAE_SessionOpenWithPolicy(&cfg, &native, &session);
    check(nativeRc != NAE_OK, "native DSD without fallback fails",
          std::to_string(nativeRc));
    if (session) NAE_SessionClose(session);

    std::printf("\n%s\n", failures == 0 ? "All DSD session checks passed."
                                        : "Some DSD session checks FAILED.");
    return failures == 0 ? 0 : 1;
}
