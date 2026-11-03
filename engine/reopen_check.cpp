// Rapid open/close cycles on an exclusive endpoint.
//
// # Why this exists
//
// The app dies when playback is started while the output panel is mounted. The
// panel is subscribed to the audio event channel, and the sequence that triggers
// it is: a render is in flight, a stop arrives, and a new play is dispatched
// immediately. The audio thread then closes the exclusive stream and reopens the
// same endpoint microseconds later.
//
// That specific transition had never been exercised: the harnesses all open
// once, or close and leave it alone. If the driver has not finished releasing an
// endpoint when the next exclusive Initialize arrives, the symptom would be a
// silent process death rather than an error code — which is exactly what the app
// does.
//
// So this loops open -> play a little -> close -> open again, which is the shape
// of the real sequence, and reports where it stopped.
//
// Usage: reopen_check.exe

#include <windows.h>

#include <wrl.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "wasapi_compat.hpp"

using Microsoft::WRL::ComPtr;

namespace {

int g_fail = 0;

void check(const char* label, bool ok, const std::string& detail) {
    if (!ok) ++g_fail;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

std::string fmt(const char* f, ...) {
    char buf[192];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

using OpenFn = int32_t (*)(const char*, int32_t, int32_t, int32_t, int32_t, void**, int32_t*,
                           int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*,
                           int32_t*, double*);
using PlayFn = int32_t (*)(void*, int32_t);
using CloseFn = void (*)(void*);
using HrFn = int32_t (*)();

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HMODULE lib = LoadLibraryA(".\\nae.dll");
    if (!lib) { std::printf("could not load nae.dll\n"); return 1; }

    auto enumerate = reinterpret_cast<char* (*)()>(GetProcAddress(lib, "NAE_EnumerateDevices"));
    auto freeString = reinterpret_cast<void (*)(char*)>(GetProcAddress(lib, "NAE_FreeString"));
    auto open = reinterpret_cast<OpenFn>(GetProcAddress(lib, "NAE_OpenExclusive"));
    auto play = reinterpret_cast<PlayFn>(GetProcAddress(lib, "NAE_PlaySine"));
    auto close = reinterpret_cast<CloseFn>(GetProcAddress(lib, "NAE_Close"));
    auto lastHr = reinterpret_cast<HrFn>(GetProcAddress(lib, "NAE_GetLastHresult"));

    char* json = enumerate();
    const char* at = std::strstr(json, "\"id\":\"");
    at += 6;
    const char* end = std::strchr(at, '"');
    std::string deviceId(at, static_cast<size_t>(end - at));
    freeString(json);

    std::printf("== rapid reopen on %s ==\n", deviceId.c_str());
    std::printf("  (each iteration renders 60 ms in 44.1 kHz -> device mode)\n\n");

    // The real sequence, repeated. If a driver cannot release and re-take an
    // exclusive endpoint promptly, this is where it shows.
    constexpr int kRounds = 12;
    int opened = 0;
    for (int i = 0; i < kRounds; ++i) {
        void* handle = nullptr;
        int32_t rate = 0, src = 0, ch = 0, bits = 0, valid = 0, isf = 0;
        int32_t res = 0, dith = 0, bp = 0;
        double lat = 0.0;

        const int32_t rc = open(deviceId.c_str(), 44100, /*device mode=*/1, 0, /*high=*/1,
                                &handle, &rate, &src, &ch, &bits, &valid, &isf, &res, &dith, &bp,
                                &lat);
        if (rc != 0 || !handle) {
            check(fmt("round %d opened", i).c_str(), false,
                  fmt("rc=%d hresult=0x%08X", rc, lastHr()));
            break;
        }
        ++opened;

        const int32_t prc = play(handle, 60);
        if (prc != 0) {
            check(fmt("round %d played", i).c_str(), false, fmt("rc=%d", prc));
            close(handle);
            break;
        }

        close(handle);
        std::printf("  round %2d: %d Hz, %d-bit, resampling=%d bitPerfect=%d  ok\n", i, rate,
                    valid, res, bp);
    }

    check("every round opened, played and closed", opened == kRounds,
          fmt("%d of %d completed", opened, kRounds));

    std::printf("\n%s\n", g_fail == 0 ? "No crash across rapid reopen cycles."
                                      : "Reopen cycle FAILED.");
    CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
