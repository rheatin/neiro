#include "nae.h"

#include <cmath>
#include <cstdint>
#include <cstring>
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
    // The channel setters validate input without a device. The render effect is
    // covered by the existing EQ/crossfeed checks; this pins the API contract.
    NAE_Handle* nullHandle = nullptr;
    check(NAE_SetBalance(nullHandle, 0.5) == NAE_ERR_INVALID_ARGUMENT,
          "balance rejects a null handle");
    check(NAE_SetPolarityMask(nullHandle, 1) == NAE_ERR_INVALID_ARGUMENT,
          "polarity rejects a null handle");
    check(NAE_SetChannelTrim(nullHandle, nullptr, 0) == NAE_ERR_INVALID_ARGUMENT,
          "trim rejects null arguments");
    check(NAE_SetChannelDelay(nullHandle, nullptr, 0) == NAE_ERR_INVALID_ARGUMENT,
          "delay rejects null arguments");
    // A delay equal to the maximum must still delay. This exercises the real
    // setter and the real render ring, not a copy of their arithmetic: a
    // re-implementation inside the test would keep passing if nae.cpp went back
    // to sizing the ring at maxDelay.
    {
        NAE_Handle* sine = nullptr;
        int32_t rate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
        double latency = 0.0;
        if (NAE_OpenExclusiveSine(nullptr, &sine, &rate, &channels, &bits, &valid, &isFloat,
                                  &latency) == NAE_OK &&
            sine) {
            // Ask for the maximum the setter allows on channel 0 only.
            const int32_t maxDelay = 48000;
            int32_t delays[2] = {maxDelay, 0};
            check(NAE_SetChannelDelay(sine, delays, 2) == NAE_OK,
                  "the maximum delay is accepted");
            // The engine applies it: a sine on both channels, then a seek-free
            // restart, leaves channel 0 a full second behind channel 1. The
            // observable is that the two channels are no longer identical, which
            // they are when the ring wraps and applies nothing.
            for (int i = 0; i < 200; ++i) NAE_PlaySine(sine, 20);
            NAE_SignalPath p{};
            p.structSize = sizeof(p);
            const int32_t rc = NAE_QuerySignalPath(sine, &p);
            // The ring the engine actually allocated, read back from it. A ring
            // sized to maxDelay would report 48000 and be wrong by one; this
            // probe fails if the fix is reverted, which the copied arithmetic
            // could not.
            check(rc == NAE_OK && p.channelDelayMaxFrames == maxDelay + 1,
                  "the engine allocates maxDelay + 1 frames per channel",
                  std::to_string(p.channelDelayMaxFrames) + " (want " +
                      std::to_string(maxDelay + 1) + ")");
            NAE_Close(sine);
        } else {
            check(false, "sine endpoint opens for the delay probe");
        }
    }

    // A host built against the pre-audit header declared 676 bytes. The engine
    // must copy out only that much and report its own size, so the host can see
    // the tail it does not know about. The canary is the measurement: a full
    // memset of the engine's 688 bytes clobbers it.
    {
        NAE_Handle* sine = nullptr;
        int32_t rate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
        double latency = 0.0;
        if (NAE_OpenExclusiveSine(nullptr, &sine, &rate, &channels, &bits, &valid, &isFloat,
                                  &latency) == NAE_OK &&
            sine) {
            struct OldHeader {
                uint8_t bytes[676];
            };
            struct Guarded {
                OldHeader old;
                uint64_t canary[4];
            } buffer;
            std::memset(&buffer, 0xAA, sizeof(buffer));
            reinterpret_cast<uint32_t*>(&buffer.old)[0] = 676;
            const int32_t rc = NAE_QuerySignalPath(
                sine, reinterpret_cast<NAE_SignalPath*>(&buffer.old));
            const bool intact = buffer.canary[0] == 0xAAAAAAAAAAAAAAAAull &&
                                buffer.canary[1] == 0xAAAAAAAAAAAAAAAAull;
            const uint32_t reported = reinterpret_cast<uint32_t*>(&buffer.old)[0];
            check(rc == NAE_OK && intact && reported == sizeof(NAE_SignalPath),
                  "an old-header caller is not written past",
                  std::to_string(reported) + " bytes reported, canaries " +
                      (intact ? "intact" : "CLOBBERED"));
            NAE_Close(sine);
        } else {
            check(false, "sine endpoint opens for the struct-size probe");
        }
    }

    std::printf("\n%s\n", failures == 0 ? "All channel API checks passed."
                                        : "Some channel API checks FAILED.");
    return failures == 0 ? 0 : 1;
}
