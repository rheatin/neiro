// The decode thread must actually be the producer, not a fallback.
//
// NAE_PipelineStats reports `decodeThread`; this opens a file, asserts the flag,
// then plays and checks that the render thread still gets audio and advances.

#include "nae.h"
#include "utf8_args.h"

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
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: decode_thread_check <file>\n");
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
        std::printf("open failed\n");
        return 1;
    }

    char* stats = NAE_PipelineStats(handle);
    const bool active = stats && std::strstr(stats, "\"decodeThread\":1") != nullptr;
    check(active, "decode thread is the producer", stats ? stats : "(null)");
    if (stats) NAE_FreeString(stats);

    double before = NAE_Position(handle);
    for (int i = 0; i < 25; ++i) NAE_Play(handle, 20);
    double after = NAE_Position(handle);
    check(after > before + 0.1, "render thread still gets audio",
          std::to_string(before) + " -> " + std::to_string(after));

    NAE_Close(handle);
    NAE_Shutdown();
    std::printf("\n%s\n", failures == 0 ? "All decode-thread checks passed."
                                        : "Some decode-thread checks FAILED.");
    return failures == 0 ? 0 : 1;
}
