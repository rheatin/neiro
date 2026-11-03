#include "audio_backend.h"

#include <cstdio>
#include <string>

int main() {
    auto backend = neiro::createPlatformBackend();
    if (!backend) {
        std::printf("no platform backend\n");
        return 1;
    }
    std::printf("backend: %s\n", backend->name());
    std::string error;
    auto devices = backend->enumerate(&error);
    std::printf("devices: %zu%s\n", devices.size(), error.empty() ? "" : (" (" + error + ")").c_str());
    if (devices.empty()) return 1;
    NAE_DeviceCaps caps{};
    caps.structSize = sizeof(caps);
    if (!backend->queryCaps(devices[0].id, NAE_SHARE_MODE_EXCLUSIVE, &caps, &error)) {
        std::printf("caps failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("caps: %s native %u Hz, rates %u, DoP %d\n", caps.name, caps.nativeRate,
                caps.rateCount, caps.dopSupported);
    if (caps.nativeRate == 0 || caps.rateCount == 0) {
        std::printf("caps check FAILED\n");
        return 1;
    }
    backend->shutdown();
    std::printf("backend check passed\n");
    return 0;
}
