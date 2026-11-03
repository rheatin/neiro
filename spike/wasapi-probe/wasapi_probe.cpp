// WASAPI capability probe.
//
// Purpose: answer the three questions that gate the whole HiFi plan, using a
// throwaway program rather than any project code.
//
//   1. Can we enumerate render endpoints?
//   2. Can we open one in SHARED mode, and at what sample rate / bit depth?
//   3. Can we open one in EXCLUSIVE mode, and do we get the device's *native*
//      format instead of the mixer's resampled one?
//
// The answer to (3) decides whether bit-perfect playback — and therefore the
// whole signal-path story — is achievable on this machine.
//
// Build:  cmake -B build -G Ninja && cmake --build build
// Run:    ./build/wasapi_probe.exe

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "wasapi_compat.hpp"

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void check(const char* label, bool ok, const std::string& detail = {}) {
    if (!ok) ++g_failures;
    std::printf("  [%s] %-44s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

std::string describe(const WAVEFORMATEX* f) {
    if (!f) return "(none)";
    unsigned valid = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
        valid = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->Samples.wValidBitsPerSample;
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%u Hz / %u ch / %u-bit (valid %u)", f->nSamplesPerSec,
                  f->nChannels, f->wBitsPerSample, valid);
    return buf;
}

struct Endpoint {
    ComPtr<IMMDevice> device;
    std::wstring name;
};

std::vector<Endpoint> enumerateOutputs() {
    std::vector<Endpoint> out;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator))))
        return out;

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) ||
        !collection)
        return out;

    const UINT count = neiro::wasapi::collectionCount(collection.Get());
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        Endpoint ep;
        ep.device = device;

        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(neiro::wasapi::openPropertyStore(device.Get(), &store)) && store) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR && name.pwszVal)
                ep.name = name.pwszVal;
            PropVariantClear(&name);
        }
        if (ep.name.empty()) ep.name = L"(unnamed endpoint)";
        out.push_back(std::move(ep));
    }
    return out;
}

struct OpenResult {
    bool opened = false;
    HRESULT hr = S_OK;
    WAVEFORMATEX granted{};
    std::string error;
};

OpenResult tryOpen(const ComPtr<IMMDevice>& device, bool exclusive, const WAVEFORMATEX* requested) {
    OpenResult res;

    ComPtr<IAudioClient> client;
    res.hr = neiro::wasapi::activate(device.Get(), __uuidof(IAudioClient),
                                    client.GetAddressOf());
    if (FAILED(res.hr) || !client) {
        res.error = "Activate(IAudioClient) failed";
        return res;
    }

    // Always hand Initialize() a WAVEFORMATEXTENSIBLE. A device's own format
    // may be plain PCM or already extensible; passing either through verbatim
    // does not work (plain PCM gets E_INVALIDARG, and the extensible one has
    // to keep its cbSize consistent). Wrapping the device's numbers in a
    // freshly built extensible struct is the form drivers accept.
    WAVEFORMATEXTENSIBLE want{};
    ZeroMemory(&want, sizeof(want));
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    want.Format.nChannels = 2;
    want.Format.nSamplesPerSec = 48000;
    want.Format.wBitsPerSample = 24;
    want.Format.nBlockAlign = 6;
    want.Format.nAvgBytesPerSec = 48000 * 6;
    want.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    want.Samples.wValidBitsPerSample = 24;
    want.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    want.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

    if (requested) {
        want.Format.nChannels = requested->nChannels;
        want.Format.nSamplesPerSec = requested->nSamplesPerSec;
        want.Format.wBitsPerSample = requested->wBitsPerSample;
        want.Format.nBlockAlign = requested->nBlockAlign;
        want.Format.nAvgBytesPerSec = requested->nAvgBytesPerSec;
        want.Samples.wValidBitsPerSample = requested->wBitsPerSample;
        want.dwChannelMask =
            requested->nChannels == 1 ? SPEAKER_FRONT_LEFT
                                     : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        if (requested->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            want.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }

    const AUDCLNT_SHAREMODE share = exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    // Flags = 0 selects polling mode, which is the most widely supported and
    // what a pull-style engine wants anyway.
    //
    // AUDCLNT_STREAMFLAGS_RAW was tried first and rejected by every endpoint
    // here with AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021). Microsoft
    // documents RAW as "not supported by all Windows versions", so the engine
    // must treat it as opt-in and probe for it, never assume it.
    const DWORD flags = 0;

    res.hr = client->Initialize(share, flags, 0, 0, &want.Format, nullptr);
    if (FAILED(res.hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Initialize hr=0x%08X", res.hr);
        res.error = buf;
        return res;
    }

    WAVEFORMATEX* granted = nullptr;
    if (FAILED(client->GetMixFormat(&granted)) || granted == nullptr) {
        res.error = "GetMixFormat failed";
        return res;
    }
    res.granted = *granted;
    CoTaskMemFree(granted);
    res.opened = true;
    return res;
}

// An if-chain rather than a switch: several of these constants are only
// defined by the compat shim, and a duplicate case value would be a hard
// compile error for no benefit.
// Read the endpoint's *native* format from PKEY_AudioEngine_DeviceFormat.
//
// This is the key to exclusive mode: asking for a made-up format (e.g. a
// hardcoded 48 kHz / 24-bit) makes most endpoints answer
// AUDCLNT_E_UNSUPPORTED_FORMAT, because exclusive mode means "exactly this
// format or nothing" and the device has its own idea of what it is. The blob
// behind this property is a WAVEFORMATEX describing what the hardware actually
// wants.
bool readNativeFormat(IMMDevice* device, WAVEFORMATEX* out) {
    ComPtr<IPropertyStore> store;
    if (FAILED(neiro::wasapi::openPropertyStore(device, &store)) || !store) return false;

    PROPVARIANT value;
    PropVariantInit(&value);
    if (FAILED(store->GetValue(PKEY_AudioEngine_DeviceFormat, &value))) return false;

    bool ok = false;
    if (value.vt == VT_BLOB && value.blob.pBlobData &&
        value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        auto* fmt = reinterpret_cast<const WAVEFORMATEX*>(value.blob.pBlobData);
        // Only PCM-family formats are directly usable; anything else (Dolby,
        // S/PDIF passthrough) has to be negotiated differently.
        if (fmt->wFormatTag == WAVE_FORMAT_PCM || fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE ||
            fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            *out = *fmt;
            ok = true;
        }
    }
    PropVariantClear(&value);
    return ok;
}

const char* hrText(HRESULT hr) {
    if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) return "EXCLUSIVE_MODE_NOT_ALLOWED";
    if (hr == AUDCLNT_E_EXCLUSIVE_MODE_ONLY_ALLOWED) return "EXCLUSIVE_MODE_ONLY_ALLOWED";
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return "DEVICE_INVALIDATED";
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return "DEVICE_IN_USE (another app holds it)";
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return "UNSUPPORTED_FORMAT";
    if (hr == AUDCLNT_E_INVALID_SIZE) return "INVALID_SIZE";
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) return "SERVICE_NOT_RUNNING";
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) return "BUFFER_SIZE_NOT_ALIGNED";
    if (hr == E_NOTIMPL) return "E_NOTIMPL";
    if (hr == REGDB_E_CLASSNOTREG) return "REGDB_E_CLASSNOTREG";
    return nullptr;
}

}  // namespace

int main() {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("CoInitializeEx: hr=0x%08X (%s)\n", comInit, SUCCEEDED(comInit) ? "ok" : "failed");
    std::printf("audio service: ");
    {
        ComPtr<IMMDeviceEnumerator> e;
        std::printf("%s\n\n", SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&e)))
                                       ? "running"
                                       : "NOT RUNNING");
    }

    std::printf("== 1. endpoint enumeration ==\n");
    auto endpoints = enumerateOutputs();
    check("at least one active render endpoint", !endpoints.empty(),
          std::to_string(endpoints.size()) + " found");
    if (endpoints.empty()) {
        std::printf("\nNo render endpoints; nothing further to test.\n");
        CoUninitialize();
        return 1;
    }
    for (const auto& ep : endpoints) std::printf("       - %ls\n", ep.name.c_str());

    int exclusiveOk = 0;
    for (const auto& ep : endpoints) {
        std::printf("\n== device: %ls ==\n", ep.name.c_str());

        // What does the hardware itself say it is?
        WAVEFORMATEX nativeFmt{};
        const bool hasNative = readNativeFormat(ep.device.Get(), &nativeFmt);
        if (hasNative)
            std::printf("       native format: %s\n", describe(&nativeFmt).c_str());
        else
            std::printf("       native format: not available\n");

        const OpenResult shared = tryOpen(ep.device, /*exclusive=*/false, nullptr);
        if (shared.opened) {
            check("shared mode opens", true, describe(&shared.granted));
        } else {
            const char* named = hrText(shared.hr);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "%s%s", shared.error.c_str(), named ? named : "");
            check("shared mode opens", false, buf);
        }

        // First attempt: a guessed 48k/24 request. This is expected to fail on
        // most hardware, so it is reported for information rather than counted
        // as a failure — it is the mistake the engine must not make.
        const OpenResult exclGeneric = tryOpen(ep.device, /*exclusive=*/true, nullptr);
        if (exclGeneric.opened) {
            std::printf("  [INFO] generic 48k/24 exclusive request: accepted\n");
        } else {
            const char* named = hrText(exclGeneric.hr);
            char buf[200];
            std::snprintf(buf, sizeof(buf), "generic 48k/24 request refused: %s%s",
                          exclGeneric.error.c_str(), named ? named : "");
            std::printf("  [INFO] %s\n", buf);
        }

        // Second attempt, and the one that matters: ask for exactly what the
        // device reports as native. UNSUPPORTED_FORMAT against a guessed format
        // means "that is not what I am", not "never".
        if (hasNative) {
            const OpenResult exclNative = tryOpen(ep.device, /*exclusive=*/true, &nativeFmt);
            if (exclNative.opened) {
                exclusiveOk++;
                check("EXCLUSIVE opens (device native format)", true,
                      std::string("requested ") + describe(&nativeFmt).c_str());
            } else {
                const char* named = hrText(exclNative.hr);
                char buf[200];
                std::snprintf(buf, sizeof(buf), "%s%s", exclNative.error.c_str(), named ? named : "");
                check("EXCLUSIVE opens (device native format)", false, buf);
            }
        }
    }

    std::printf("\n== summary ==\n");
    std::printf("  endpoints: %zu, exclusive-capable: %d\n", endpoints.size(), exclusiveOk);
    if (exclusiveOk > 0)
        std::printf("  VERDICT: bit-perfect exclusive output is AVAILABLE on this machine.\n");
    else
        std::printf("  VERDICT: no endpoint granted exclusive mode; bit-perfect is NOT available.\n");
    std::printf("  NOTE: GetMixFormat() is not meaningful in exclusive mode; the format that\n");
    std::printf("        actually applies is the one requested from PKEY_AudioEngine_DeviceFormat.\n");

    std::printf("\n%s\n", g_failures == 0 ? "All probe checks passed." : "Some probe checks FAILED.");
    CoUninitialize();
    return g_failures == 0 ? 0 : 1;
}
