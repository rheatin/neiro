#include "nae.h"
#include <cstdio>
int main(int argc, char** argv) {
    NAE_DeviceCaps c{};
    c.structSize = sizeof(c);
    const char* device = argc > 1 ? argv[1] : nullptr;
    const int32_t rc = NAE_QueryDeviceCaps(device, NAE_SHARE_MODE_EXCLUSIVE, &c);
    if (rc != NAE_OK) {
        std::printf("caps query returned %d: %s\n", rc, NAE_GetLastError());
        return 1;
    }
    std::printf("caps: %s native %u Hz %u ch, rates %u, formats %u, layouts %u, "
                "DoP %d (%u carriers), native DSD %d, probed 0x%X, generation %u\n",
                c.name, c.nativeRate, c.nativeChannels, c.rateCount, c.formatCount,
                c.layoutCount, c.dopSupported, c.dopCarrierCount, c.nativeDsdSupported,
                c.probed, c.capsGeneration);
    if (c.structSize != sizeof(c) || c.backend != NAE_BACKEND_WASAPI ||
        c.rateCount == 0 || c.nativeRate == 0 ||
        (c.probed & NAE_CAPS_PROBED_RATES) == 0) {
        std::printf("caps check FAILED\n");
        return 1;
    }
    // WASAPI has no 1-bit stream format. The flag must be probed and must be 0:
    // an unprobed zero would mean policy cannot tell "not asked" from "no".
    if ((c.probed & NAE_CAPS_PROBED_NATIVE_DSD) == 0) {
        std::printf("caps check FAILED: native DSD capability was not probed\n");
        return 1;
    }
    if (c.nativeDsdSupported != 0) {
        std::printf("caps check FAILED: WASAPI reported native DSD support\n");
        return 1;
    }
    std::printf("caps check passed\n");
    return 0;
}
