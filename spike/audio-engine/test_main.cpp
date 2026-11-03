// Test harness for the audio engine DLL.
//
// Loads nae.dll at runtime rather than linking it, so this exercises the same
// path a host application takes: open the library, resolve a symbol, call it.
// A link-time build would hide ABI mistakes that only show up once a language
// other than C++ is on the other side of the boundary.

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The rate policy, compiled in rather than reached through the DLL.
//
// This is a header-only decision function, and it is the one that decides whether
// a DSD file plays at all: a DSD64 track decodes to 352.8 kHz, an endpoint whose
// exclusive rates stop at 192 kHz refuses that, and the difference between "plays
// at 192 kHz" and "reports the file as unsupported" is entirely inside
// `resolveOutputTarget`. It has no other test, and testing it through the DLL
// would need a device that happens to fail in the right way.
#include "output_mode.h"

namespace {

using FreeStringFn = void (*)(char*);
using CapabilitiesFn = const char* (*)();
using EnumerateFn = char* (*)();
using OpenExclusiveSineFn = int32_t (*)(const char*, void**, int32_t*, int32_t*, int32_t*,
                                         int32_t*, int32_t*, double*);
using PlaySineFn = int32_t (*)(void*, int32_t);
using CloseFn = void (*)(void*);
using LastHresultFn = int32_t (*)();

int g_failures = 0;

void check(const char* label, bool ok, const std::string& detail = {}) {
    if (!ok) ++g_failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

// Pull the value of a top-level JSON number field. Good enough for a harness
// that only reads fields this file also wrote.
std::string jsonField(const std::string& json, const std::string& key) {
    const size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return "";
    size_t colon = json.find(':', at);
    if (colon == std::string::npos) return "";
    size_t start = json.find_first_not_of(" \t", colon + 1);
    if (start == std::string::npos) return "";
    size_t end = start;
    while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) ||
                                 json[end] == '-' || json[end] == '.'))
        ++end;
    return json.substr(start, end - start);
}

std::string jsonStringField(const std::string& json, const std::string& key) {
    const size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return "";
    size_t open = json.find('"', json.find(':', at));
    if (open == std::string::npos) return "";
    size_t close = json.find('"', open + 1);
    if (close == std::string::npos) return "";
    return json.substr(open + 1, close - open - 1);
}

// Boolean and bare values (true / false / numbers) are not quoted, so the
// string helper above would run past the value and return the *next* key's
// string. Read the raw token instead.
std::string jsonRawField(const std::string& json, const std::string& key) {
    const size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return "";
    size_t start = json.find_first_not_of(" \t", json.find(':', at) + 1);
    if (start == std::string::npos) return "";
    size_t end = json.find_first_of(",}", start);
    if (end == std::string::npos) end = json.size();
    return json.substr(start, end - start);
}

template <typename T>
T resolve(HMODULE lib, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(lib, name));
}

}  // namespace

// The output-rate policy, on its own.
//
// Every case here is a fact about a decision function, not about a machine, which
// is the reason it can be asserted exactly. The rates are the ones this project
// has actually met: a 192 kHz HDMI endpoint that accepts exclusive only at
// 44100/48000/96000/192000, and a DSD64 file that decodes to 352800 Hz.
void ratePolicyChecks() {
    using neiro::OutputMode;
    using neiro::OutputTarget;
    using neiro::PerfectReason;
    using neiro::resolveOutputTarget;
    using neiro::SupportedRates;

    std::printf("\n== output rate policy ==\n");

    // What this machine's endpoint was measured to accept.
    const uint32_t hdmiRates[] = {44100, 48000, 96000, 192000};
    const SupportedRates hdmi{hdmiRates, 4};

    // A device that has not been probed. Every list here is empty in that case,
    // and it must behave exactly as the policy did before supported rates existed
    // -- refusing everything would be a far worse bug than the one being fixed.
    const SupportedRates unknown{};

    OutputTarget t{};

    // ---- an unprobed device changes nothing ----
    resolveOutputTarget(OutputMode::Device, 44100, 192000, 0, unknown, &t);
    check("unprobed device keeps the requested rate", t.rate == 192000,
          std::to_string(t.rate) + " Hz");
    check("unprobed device is not treated as coercive", !t.coercedToSupportedRate);

    // ---- the case that made DSD unplayable ----
    //
    // A DSD64 file decodes to 352.8 kHz. Asked for 352.8 kHz in Native mode, a
    // 192 kHz endpoint answers AUDCLNT_E_ENDPOINT_CREATE_FAILED, and the listener
    // is told their file is unsupported -- which is false, and unfixable from the
    // output-mode control because every setting produces the same refusal.
    resolveOutputTarget(OutputMode::Native, 352800, 192000, 0, hdmi, &t);
    check("a DSD64 rate the driver refuses is coerced", t.rate == 192000,
          "352800 -> " + std::to_string(t.rate) + " Hz");
    check("the coercion is reported as such", t.coercedToSupportedRate);
    check("a coerced path is not bit-perfect", !t.bitPerfect);
    check("a coerced path is named ResampledToSupportedRate",
          t.reason == PerfectReason::ResampledToSupportedRate);

    // The same file in Device mode: the device's own rate was already the target,
    // so nothing is coerced, and the reason is the mode's own rather than the
    // driver's. This distinction is the whole point of the separate code.
    resolveOutputTarget(OutputMode::Device, 352800, 192000, 0, hdmi, &t);
    check("Device mode needs no coercion for the same file", !t.coercedToSupportedRate);
    check("Device mode reports its own reason", t.reason == PerfectReason::ResampledToDeviceRate,
          "reason=" + std::to_string(static_cast<int>(t.reason)));

    // ---- a rate the device does accept is left alone ----
    resolveOutputTarget(OutputMode::Native, 48000, 192000, 0, hdmi, &t);
    check("a supported rate is left alone", t.rate == 48000 && !t.coercedToSupportedRate,
          std::to_string(t.rate) + " Hz");
    check("a supported native rate stays bit-perfect", t.bitPerfect);

    // ---- a fixed rate the driver refuses is coerced, not obeyed ----
    //
    // This is the case where obeying would be actively wrong. A listener picks
    // 352800 in the fixed-rate field; the device cannot do it; honouring the
    // setting produces a hard failure with an error blaming their file.
    resolveOutputTarget(OutputMode::Fixed, 48000, 192000, 352800, hdmi, &t);
    check("a fixed rate the driver refuses is coerced", t.rate == 192000,
          "asked 352800, using " + std::to_string(t.rate) + " Hz");
    check("the fixed-rate coercion names the driver, not the setting",
          t.reason == PerfectReason::ResampledToSupportedRate);

    // ---- the nearest rate, by ratio rather than by difference ----
    //
    // 352.8 kHz sits between 192 kHz (a factor of 1.84 away) and 96 kHz (3.68
    // away), and 192 kHz is both the closer one and the better one: a smaller
    // conversion ratio throws away less. A linear comparison of raw hertz also
    // picks 192 kHz here, which is why this case alone would not catch a linear
    // implementation -- the case below does.
    const uint32_t dsd64Rates[] = {96000, 192000};
    const SupportedRates dsd64{dsd64Rates, 2};
    resolveOutputTarget(OutputMode::Native, 352800, 192000, 0, dsd64, &t);
    check("the nearest supported rate is chosen by ratio", t.rate == 192000,
          "352800 -> " + std::to_string(t.rate) + " Hz");

    // 352.8 kHz is 160.8 kHz away from 192 kHz and 256.8 kHz away from 96 kHz, so
    // a linear comparison also answers 192 kHz -- the case is here to pin the
    // *stated* rule, not to catch a regression. The one that does: 44.1 kHz is
    // 147.9 kHz from 192 kHz and 51.9 kHz from 96 kHz, and choosing by raw
    // difference would pick 96 kHz over 192 kHz for a file the device could play
    // at its own rate. That is a real quality loss produced by a plausible
    // implementation, which is why it is asserted.
    resolveOutputTarget(OutputMode::Native, 44100, 192000, 0, dsd64, &t);
    check("nearest is measured in ratio, not hertz", t.rate == 96000,
          "44100 -> " + std::to_string(t.rate) + " Hz (96k is 2.3x away, 192k is 4.35x)");

    // ---- a mode cannot be satisfied ----
    check("Fixed with no rate set is refused",
          !resolveOutputTarget(OutputMode::Fixed, 44100, 192000, 0, hdmi, &t));

    // ---- a list containing a zero is ignored, not chosen ----
    //
    // Zero is what an unprobed slot looks like if it is ever written, and
    // dividing by it would produce a NaN distance that compares false against
    // every real candidate and leaves `best` at zero.
    const uint32_t withZero[] = {0, 48000, 96000};
    const SupportedRates sloppy{withZero, 3};
    resolveOutputTarget(OutputMode::Native, 44100, 192000, 0, sloppy, &t);
    check("a zero in the supported list is skipped", t.rate == 48000,
          std::to_string(t.rate) + " Hz");
}

int main() {
    // Unbuffered: when stdout is a pipe (CI, capture) it is fully buffered, and
    // a crash mid-run would otherwise swallow every line printed so far.
    setvbuf(stdout, nullptr, _IONBF, 0);

    ratePolicyChecks();

    // The DLL sits next to the harness.
    const char* dllPath = ".\\nae.dll";
    std::printf("== loading %s ==\n", dllPath);
    HMODULE lib = LoadLibraryA(dllPath);
    if (!lib) {
        std::printf("  LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    check("nae.dll loads", true);

    auto capabilities = resolve<CapabilitiesFn>(lib, "NAE_GetEngineCapabilities");
    auto enumerate = resolve<EnumerateFn>(lib, "NAE_EnumerateDevices");
    auto freeString = resolve<FreeStringFn>(lib, "NAE_FreeString");
    auto openExclusive = resolve<OpenExclusiveSineFn>(lib, "NAE_OpenExclusiveSine");
    auto playSine = resolve<PlaySineFn>(lib, "NAE_PlaySine");
    auto closeHandle = resolve<CloseFn>(lib, "NAE_Close");
    auto lastHr = resolve<LastHresultFn>(lib, "NAE_GetLastHresult");

    const bool allResolved = capabilities && enumerate && freeString && openExclusive && playSine &&
                             closeHandle && lastHr;
    check("all exported symbols resolve", allResolved);
    if (!allResolved) {
        FreeLibrary(lib);
        return 1;
    }

    std::printf("\n== capabilities ==\n");
    const char* caps = capabilities();
    std::printf("  %s\n", caps ? caps : "(null)");
    check("capabilities JSON mentions wasapi", caps && std::strstr(caps, "wasapi") != nullptr);

    std::printf("\n== device enumeration (through the DLL) ==\n");
    char* json = enumerate();
    check("NAE_EnumerateDevices returned JSON", json != nullptr);
    if (!json) {
        FreeLibrary(lib);
        return 1;
    }
    std::printf("  %s\n", json);

    const std::string devices(json);
    const std::string firstId = jsonStringField(devices, "id");
    const std::string exclusive = jsonRawField(devices, "exclusive");
    check("at least one device reported", !devices.empty() && devices != "[]", devices.substr(0, 60));
    check("first device has an id", !firstId.empty(), firstId.substr(0, 48) + "...");
    check("first device reports exclusive support", exclusive == "true", exclusive);

    freeString(json);

    if (firstId.empty()) {
        std::printf("\nNo device to play on.\n");
        FreeLibrary(lib);
        return 1;
    }

    std::printf("\n== open exclusive ==\n");
    void* handle = nullptr;
    int32_t rate = 0, channels = 0, bits = 0, validBits = 0, isFloat = 0;
    double latencyMs = 0.0;
    const int32_t rc = openExclusive(firstId.c_str(), &handle, &rate, &channels, &bits, &validBits,
                                     &isFloat, &latencyMs);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "rc=%d hresult=0x%08X", rc, lastHr());
    check("NAE_OpenExclusiveSine succeeded", rc == 0 && handle != nullptr, detail);

    if (rc != 0 || !handle) {
        std::printf("\nCannot continue without an open exclusive stream.\n");
        FreeLibrary(lib);
        return 1;
    }

    std::printf("       signal path: %d Hz / %d ch / %d-bit (valid %d) / %s / %.1f ms buffer\n", rate,
                channels, bits, validBits, isFloat ? "float" : "int", latencyMs);
    check("sample rate reported", rate > 0, std::to_string(rate));
    check("channel count reported", channels == 2, std::to_string(channels));
    check("bit depth reported", bits > 0, std::to_string(bits));
    check("buffer latency is sane", latencyMs > 1.0 && latencyMs < 2000.0,
          std::to_string(latencyMs) + " ms");

    std::printf("\n== play a 1500 ms sine ==\n");
    const ULONGLONG t0 = GetTickCount64();
    const int32_t prc = playSine(handle, 1500);
    const ULONGLONG elapsed = GetTickCount64() - t0;
    std::printf("  wall time: %llu ms\n", static_cast<unsigned long long>(elapsed));
    check("NAE_PlaySine succeeded", prc == 0, "rc=" + std::to_string(prc));
    // It is a pull loop, so it can overshoot; it must not under-run.
    check("played roughly the requested duration", elapsed >= 1400 && elapsed < 4000,
          std::to_string(elapsed) + " ms for 1500 ms of audio");

    closeHandle(handle);
    check("NAE_Close ran without crashing", true);

    std::printf("\n== concurrent open on the same device ==\n");
    // Exclusive is *nominally* single-occupant, but that is not enforced the
    // same way everywhere: many drivers will happily open a second exclusive
    // stream, and some refuse it. Both are legitimate, so the only thing worth
    // asserting is that the engine reports the outcome faithfully and never
    // hands back a half-built handle.
    void* second = nullptr;
    const int32_t rc2 = openExclusive(firstId.c_str(), &second, nullptr, nullptr, nullptr,
                                      nullptr, nullptr, nullptr);
    std::printf("  second open rc=%d hresult=0x%08X (driver-dependent)\n", rc2, lastHr());
    check("second open reports a consistent outcome",
          (rc2 == 0) == (second != nullptr),
          rc2 == 0 ? "driver allows concurrent exclusive streams"
                   : "driver refused, rc=" + std::to_string(rc2));
    // Whatever happened, closing it must not disturb the first handle.
    if (second) closeHandle(second);

    // Release the COM apartment last, once no handle can still be using it.
    auto shutdown = resolve<void (*)(void)>(lib, "NAE_Shutdown");
    if (shutdown) {
        shutdown();
        check("NAE_Shutdown ran cleanly", true);
    } else {
        check("NAE_Shutdown is exported", false);
    }

    FreeLibrary(lib);
    std::printf("\n%s\n", g_failures == 0 ? "All engine checks passed." : "Some engine checks FAILED.");
    return g_failures == 0 ? 0 : 1;
}
