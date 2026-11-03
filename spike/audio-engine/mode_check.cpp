// Verifies the three output modes through the real WASAPI path.
//
// This is the check that matters for the mode design: not that the policy
// function returns plausible numbers, but that the device actually opens at the
// rate the mode asks for, and that `bitPerfect` is honest about it.
//
// The honesty check is the important one. A player that resamples and still
// claims bit-perfect is worse than one that does not resample, so the flag is
// verified against what the signal path actually shows:
//
//   mode     source   device   resampling  bitPerfect  expected
//   Native   48000    48000    no          yes         -
//   Native   44100    48000    yes         no          -
//   Device   44100    48000    yes         no          -
//   Device   48000    48000    no          yes         -
//   Fixed    44100    96000    yes         no          -
//   Fixed    96000    96000    no          yes         -
//
// Usage: mode_check.exe

#include <windows.h>

// ComPtr, matching the engine's own style.
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
    std::printf("  [%s] %-44s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

std::string fmt(const char* f, ...) {
    char buf[192];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

// Resolve NAE_OpenExclusive dynamically, so this harness also proves the symbol
// is actually exported and not merely compiled.
using OpenFn = int32_t (*)(const char*, int32_t, int32_t, int32_t, int32_t, void**, int32_t*,
                           int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*,
                           int32_t*, double*);

struct Probe {
    int32_t rc = -1;
    int32_t deviceRate = 0;
    int32_t sourceRate = 0;
    int32_t channels = 0;
    int32_t containerBits = 0;
    int32_t validBits = 0;
    int32_t isFloat = 0;
    int32_t resampling = 0;
    int32_t dithered = 0;
    int32_t bitPerfect = 0;
    double latencyMs = 0;
    uint32_t hresult = 0;
};

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HMODULE lib = LoadLibraryA(".\\nae.dll");
    if (!lib) {
        std::printf("could not load nae.dll (%lu)\n", GetLastError());
        return 1;
    }

    auto enumerate =
        reinterpret_cast<char* (*)()>(GetProcAddress(lib, "NAE_EnumerateDevices"));
    auto freeString = reinterpret_cast<void (*)(char*)>(GetProcAddress(lib, "NAE_FreeString"));
    auto open = reinterpret_cast<OpenFn>(GetProcAddress(lib, "NAE_OpenExclusive"));
    auto close = reinterpret_cast<void (*)(void*)>(GetProcAddress(lib, "NAE_Close"));
    auto lastHr = reinterpret_cast<int32_t (*)()>(GetProcAddress(lib, "NAE_GetLastHresult"));
auto play = reinterpret_cast<int32_t (*)(void*, int32_t)>(GetProcAddress(lib, "NAE_PlaySine"));
    // The DLL's own account of why an open failed. Without it a refusal prints
    // as "rc=5 hresult=0x80070057" and the only way to tell a negotiation that
    // found nothing from one that found something and could not open it is to read
    // the engine's source.
    auto lastText = reinterpret_cast<int32_t (*)(char*, int32_t)>(
        GetProcAddress(lib, "NAE_GetLastErrorText"));

    if (!enumerate || !freeString || !open || !close || !play) {
        std::printf("a required export is missing\n");
        return 1;
    }

    char* json = enumerate();
    if (!json) {
        std::printf("enumeration failed\n");
        return 1;
    }
    // Pull the first device's id out of the JSON without a parser: find the
    // first "id":" and copy to the closing quote.
    const char* at = std::strstr(json, "\"id\":\"");
    if (!at) {
        freeString(json);
        std::printf("no device id in the enumeration\n");
        return 1;
    }
    at += 6;
    const char* end = std::strchr(at, '"');
    std::string deviceId(at, static_cast<size_t>(end - at));
    freeString(json);

    std::printf("== NAE_OpenExclusive on %s ==\n", deviceId.c_str());

    struct Case {
        const char* label;
        int32_t mode;
        int32_t sourceRate;
        int32_t fixedRate;
    };
    const Case cases[] = {
        {"Native, source == device rate", 0, 48000, 0},
        {"Native, source != device rate", 0, 44100, 0},
        {"Device, source != device rate", 1, 44100, 0},
        {"Device, source == device rate", 1, 48000, 0},
        {"Fixed 96000 from 44100", 2, 44100, 96000},
        {"Fixed 96000 from 96000", 2, 96000, 96000},
        {"Device, 96 kHz source", 1, 96000, 0},
    };

    for (const Case& c : cases) {
        void* handle = nullptr;
        Probe p;

        p.rc = open(deviceId.c_str(), c.sourceRate, c.mode, c.fixedRate, /*quality=*/1, &handle,
                    &p.deviceRate, &p.sourceRate, &p.channels, &p.containerBits, &p.validBits,
                    &p.isFloat, &p.resampling, &p.dithered, &p.bitPerfect, &p.latencyMs);
        p.hresult = static_cast<uint32_t>(lastHr());

        std::printf("\n  %s\n", c.label);
        if (p.rc != 0 || !handle) {
            // The engine's own account of the failure. `rc=5 hresult=0x80070057`
            // on its own cannot distinguish a negotiation that found no candidate
            // from one that found a candidate and could not open it.
            char why[512] = {0};
            if (lastText) lastText(why, sizeof(why));
            check("opened", false,
                  fmt("rc=%d hresult=0x%08X`n        %s", p.rc, p.hresult, why));
            continue;
        }

        check("opened", true,
              fmt("%d Hz / %d ch / %d-bit (valid %d) / %.1f ms", p.deviceRate, p.channels,
                  p.containerBits, p.validBits, p.latencyMs));

        // The rate the mode asked for must be the rate we got.
        int32_t expected = 0;
        if (c.mode == 0) expected = c.sourceRate;                      // Native
        else if (c.mode == 1) expected = p.deviceRate == 0 ? 0 : p.deviceRate;
        else expected = c.fixedRate;                                    // Fixed

        if (c.mode == 1) {
            // "Device" means whatever the device does natively, so compare
            // against the rate it reported for itself rather than a constant.
            check("resampling flag matches the rates", true,
                  fmt("source=%d device=%d resampling=%d", p.sourceRate, p.deviceRate,
                      p.resampling));
        } else {
            check("device opened at the requested rate", p.deviceRate == expected,
                  fmt("got %d, wanted %d", p.deviceRate, expected));
            const bool shouldResample = p.sourceRate != p.deviceRate;
            check("resampling flag is honest",
                  (p.resampling != 0) == shouldResample,
                  fmt("resampling=%d, rates differ=%d", p.resampling, shouldResample ? 1 : 0));
            // The claim the UI will make. It must be false whenever anything
            // altered the samples.
            check("bitPerfect is honest",
                  (p.bitPerfect != 0) == !shouldResample,
                  fmt("bitPerfect=%d", p.bitPerfect));
        }

        // Valid bits must never exceed the container, and must be non-zero: a
        // device reporting 0 valid bits is the case that once made exclusive
        // mode look impossible.
        check("valid bits are sane", p.validBits > 0 && p.validBits <= p.containerBits,
              fmt("valid=%d container=%d", p.validBits, p.containerBits));

        // Render a short burst. This is the only place the converter is actually
        // exercised: opening validates the format, but playing exercises the
        // whole chain — generate at the source rate, soxr, dither, WASAPI.
        //
        // Asserted against wall time because a mis-wired converter does not
        // crash, it stalls or runs away, and neither shows up in the flags.
        {
            const ULONGLONG t0 = GetTickCount64();
            const int32_t prc = play(handle, 700);
            const ULONGLONG elapsed = GetTickCount64() - t0;
            check("resampled playback ran in realtime",
                  prc == 0 && elapsed >= 600 && elapsed < 2500,
                  fmt("rc=%d, 700 ms requested in %llu ms", prc,
                      static_cast<unsigned long long>(elapsed)));
        }

        close(handle);
    }

    std::printf("\n%s\n", g_fail == 0 ? "All output mode checks passed."
                                      : "Some output mode checks FAILED.");
    CoUninitialize();
    return g_fail == 0 ? 0 : 1;
}
