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
bool openWith(const char* path, int32_t mode, uint32_t ms, const char* device,
              double* latency, std::string* error) {
    NAE_Handle* h = nullptr;
    int32_t rate = 0, src = 0, ch = 0, bits = 0, valid = 0, flt = 0, rs = 0, dith = 0,
            perfect = 0, fch = 0;
    double dur = 0, lat = 0;
    const int32_t rc = NAE_OpenFileWithBuffer(device, path, NAE_OUTPUT_DEVICE, 0,
                                              NAE_QUALITY_HIGH, mode, ms, &h, &rate, &src,
                                              &ch, &bits, &valid, &flt, &rs, &dith,
                                              &perfect, &fch, &dur, &lat);
    if (rc != NAE_OK || !h) {
        char buf[256] = {};
        NAE_GetLastErrorText(buf, sizeof(buf));
        if (error) *error = buf;
        return false;
    }
    if (latency) *latency = lat;
    NAE_Close(h);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    const auto args = utf8Args(argc, argv);
    if (args.size() < 2) {
        std::printf("usage: buffer_policy_check <file> [device]\n");
        return 1;
    }
    const char* path = args[1].c_str();
    const char* device = args.size() > 2 ? args[2].c_str() : nullptr;

    double autoLatency = 0.0;
    std::string error;
    check(openWith(path, NAE_BUFFER_AUTO, 0, device, &autoLatency, &error),
          "auto buffer opens", error);
    check(autoLatency > 0.0, "auto buffer reports latency",
          std::to_string(autoLatency) + " ms");

    double lowLatency = 0.0;
    error.clear();
    check(openWith(path, NAE_BUFFER_LOW_LATENCY, 0, device, &lowLatency, &error),
          "low-latency buffer opens or falls back", error);
    check(lowLatency > 0.0, "low-latency reports latency",
          std::to_string(lowLatency) + " ms");

    double fixedLatency = 0.0;
    error.clear();
    check(openWith(path, NAE_BUFFER_FIXED, 50, device, &fixedLatency, &error),
          "fixed 50 ms buffer opens", error);
    check(fixedLatency > 0.0, "fixed buffer reports latency",
          std::to_string(fixedLatency) + " ms");

    std::printf("\n%s\n", failures == 0 ? "All buffer policy checks passed."
                                        : "Some buffer policy checks FAILED.");
    return failures == 0 ? 0 : 1;
}
