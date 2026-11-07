// First contact with libnae.dylib through the C ABI.
//
// Not a substitute for nae_test: it asks only that the library loads, that the
// exports are callable, and that the strings it returns come back through
// NAE_FreeString without touching the host's heap.
#include "nae.h"

#include <cstdio>
#include <cstring>

int main() {
    std::printf("=== capabilities ===\n%s\n", NAE_GetEngineCapabilities());

    char* devices = NAE_EnumerateDevices();
    if (!devices) {
        std::printf("enumerate failed: %s\n", NAE_GetLastError());
        return 1;
    }
    std::printf("=== devices ===\n%s\n", devices);
    NAE_FreeString(devices);

    char* def = NAE_GetDefaultDeviceId();
    if (!def) {
        std::printf("default device: none (%s)\n", NAE_GetLastError());
        return 1;
    }
    std::printf("=== default ===\n%s\n", def);
    NAE_FreeString(def);
    return 0;
}
