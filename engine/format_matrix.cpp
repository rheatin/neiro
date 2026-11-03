// Format matrix probe.
//
// Answers one question that guessing cannot: when an exclusive open fails, is
// it because another application holds the endpoint, or because we are asking
// for a format the driver will not accept?
//
// The two have opposite remedies and look identical from the outside — both
// surface as AUDCLNT_E_ENDPOINT_CREATE_FAILED. They are distinguished by
// brute force:
//
//   * every combination fails  -> contention. The endpoint is genuinely busy,
//     and no amount of format negotiation will help.
//   * some combinations work  -> format. We are asking for something the
//     driver rejects, and the fix is in wrap() or in the format chooser.
//
// It also reports the raw native format as hex, because the fields that matter
// (SubFormat, channel mask, valid bits) are exactly the ones a WAVEFORMATEX
// return type throws away.
//
// Usage: format_matrix.exe [deviceIdSubstring]

#include <windows.h>

// ComPtr, for the same reason the engine uses it: manual Release calls are how
// COM leaks happen.
#include <wrl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wasapi_compat.hpp"

namespace {

int g_fail = 0;

void line() { std::printf("  %s\n", std::string(58, '-').c_str()); }

const char* hrName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case AUDCLNT_E_NOT_INITIALIZED: return "AUDCLNT_E_NOT_INITIALIZED";
        case AUDCLNT_E_ALREADY_INITIALIZED: return "AUDCLNT_E_ALREADY_INITIALIZED";
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE: return "AUDCLNT_E_WRONG_ENDPOINT_TYPE";
        case AUDCLNT_E_DEVICE_INVALIDATED: return "AUDCLNT_E_DEVICE_INVALIDATED";
        case AUDCLNT_E_NOT_STOPPED: return "AUDCLNT_E_NOT_STOPPED";
        case AUDCLNT_E_BUFFER_TOO_LARGE: return "AUDCLNT_E_BUFFER_TOO_LARGE";
        case AUDCLNT_E_OUT_OF_ORDER: return "AUDCLNT_E_OUT_OF_ORDER";
        case AUDCLNT_E_UNSUPPORTED_FORMAT: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
        case AUDCLNT_E_INVALID_SIZE: return "AUDCLNT_E_INVALID_SIZE";
        case AUDCLNT_E_DEVICE_IN_USE: return "AUDCLNT_E_DEVICE_IN_USE";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
        case AUDCLNT_E_SERVICE_NOT_RUNNING: return "AUDCLNT_E_SERVICE_NOT_RUNNING";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_NOTIMPL: return "E_NOTIMPL";
        case E_POINTER: return "E_POINTER";
        case E_NOINTERFACE: return "E_NOINTERFACE";
        case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        default: return "(unrecognised)";
    }
}

void report(const char* label, HRESULT hr) {
    std::printf("  %-34s 0x%08X  %s\n", label, static_cast<unsigned>(hr), hrName(hr));
    if (FAILED(hr)) ++g_fail;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string toUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

void dumpBlob(const char* label, const void* data, size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::printf("  %s (%zu bytes):\n    ", label, bytes);
    for (size_t i = 0; i < bytes; ++i) {
        std::printf("%02X ", p[i]);
        if ((i + 1) % 16 == 0 && i + 1 < bytes) std::printf("\n    ");
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string want = argc > 1 ? argv[1] : std::string();

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&enumerator)))) {
        std::printf("could not create the device enumerator\n");
        return 1;
    }

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    UINT count = neiro::wasapi::collectionCount(collection.Get());

    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        LPWSTR rawId = nullptr;
        device->GetId(&rawId);
        std::wstring id;
        if (rawId) { id = rawId; CoTaskMemFree(rawId); }

        Microsoft::WRL::ComPtr<IPropertyStore> store;
        std::string name;
        if (SUCCEEDED(neiro::wasapi::openPropertyStore(device.Get(), &store)) && store) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                name = toUtf8(v.pwszVal);
            PropVariantClear(&v);
        }

        if (!want.empty() && name.find(want) == std::string::npos) continue;

        std::printf("\n== %s ==\n", name.c_str());
        std::printf("  id: %s\n", toUtf8(id).c_str());

        // Raw native format, as the property store holds it. This is the blob
        // that WAVEFORMATEX truncates.
        std::vector<unsigned char> blob;
        {
            Microsoft::WRL::ComPtr<IPropertyStore> ps;
            if (SUCCEEDED(neiro::wasapi::openPropertyStore(device.Get(), &ps)) && ps) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &v)) &&
                    v.vt == VT_BLOB && v.blob.pBlobData) {
                    blob.assign(v.blob.pBlobData, v.blob.pBlobData + v.blob.cbSize);
                }
                PropVariantClear(&v);
            }
        }
        if (blob.empty()) {
            std::printf("  no native format reported\n");
            continue;
        }
        dumpBlob("native format", blob.data(), blob.size());

        const auto* ex = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(blob.data());
        std::printf(
            "  tag=0x%04X ch=%u rate=%u bits=%u align=%u valid=%u mask=0x%08X\n",
            ex->Format.wFormatTag, ex->Format.nChannels, ex->Format.nSamplesPerSec,
            ex->Format.wBitsPerSample, ex->Format.nBlockAlign,
            ex->Samples.wValidBitsPerSample, ex->dwChannelMask);
        {
            GUID sub = ex->SubFormat;
            std::printf("  subformat={%08X-%04X-%04X-%02X%02X-", sub.Data1, sub.Data2,
                        sub.Data3, sub.Data4[0], sub.Data4[1]);
            for (int k = 2; k < 8; ++k) std::printf("%02X", sub.Data4[k]);
            std::printf("}  %s\n",
                        sub == KSDATAFORMAT_SUBTYPE_PCM ? "PCM"
                        : sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? "IEEE_FLOAT"
                                                                 : "(other)");
        }

        // ---- the matrix -------------------------------------------------
        std::printf("\n  %-34s %-10s %s\n", "attempt", "hresult", "name");
        line();

        auto attempt = [&](const char* label, const WAVEFORMATEXTENSIBLE& fmt, bool exclusive) {
            Microsoft::WRL::ComPtr<IAudioClient> client;
            HRESULT hr = neiro::wasapi::activate(device.Get(), __uuidof(IAudioClient),
                                                 client.GetAddressOf());
            if (FAILED(hr)) { report(label, hr); return; }
            const AUDCLNT_SHAREMODE mode =
                exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
            WAVEFORMATEXTENSIBLE copy = fmt;
            report(label, client->Initialize(mode, 0, 0, 0, &copy.Format, nullptr));
        };

        // Baseline: exactly what wrap() produces today.
        {
            WAVEFORMATEXTENSIBLE f{};
            f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            f.Format.nChannels = ex->Format.nChannels;
            f.Format.nSamplesPerSec = ex->Format.nSamplesPerSec;
            f.Format.wBitsPerSample = ex->Format.wBitsPerSample;
            f.Format.nBlockAlign = ex->Format.nBlockAlign;
            f.Format.nAvgBytesPerSec = ex->Format.nAvgBytesPerSec;
            f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            f.Samples.wValidBitsPerSample = ex->Format.wBitsPerSample;
            f.dwChannelMask = ex->Format.nChannels == 1
                                  ? SPEAKER_FRONT_LEFT
                                  : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
            f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            attempt("wrap() as implemented [excl]", f, true);
            attempt("wrap() as implemented [shared]", f, false);
        }

        // The device's own extensible description, passed through untouched.
        {
            WAVEFORMATEXTENSIBLE f = *ex;
            attempt("native blob verbatim [excl]", f, true);
            attempt("native blob verbatim [shared]", f, false);
        }

        // Bit depths, both sub-formats, over a few rates. If any of these
        // succeeds exclusively then the endpoint is not busy and the problem is
        // entirely our format negotiation.
        const uint32_t rates[] = {44100, 48000, 96000, 192000};
        const uint16_t depths[] = {16, 24, 32};
        const GUID subs[] = {KSDATAFORMAT_SUBTYPE_PCM, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT};

        for (GUID sub : subs) {
            const bool isFloat = sub == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
            for (uint32_t rate : rates) {
                for (uint16_t depth : depths) {
                    // Skip combinations that make no sense: float is 32 or 64
                    // only, and integer 24-in-32 needs the container to be 32.
                    if (isFloat && depth != 32) continue;
                    if (!isFloat && depth == 32) continue;  // covered by the float case

                    WAVEFORMATEXTENSIBLE f{};
                    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                    f.Format.nChannels = 2;
                    f.Format.nSamplesPerSec = rate;
                    f.Format.wBitsPerSample = depth;
                    f.Format.nBlockAlign = static_cast<WORD>(2 * (depth / 8));
                    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
                    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                    f.Samples.wValidBitsPerSample = depth;
                    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                    f.SubFormat = sub;

                    char label[64];
                    std::snprintf(label, sizeof(label), "%u Hz / %u-bit / %s [excl]", rate,
                                  depth, isFloat ? "float" : "int");
                    attempt(label, f, true);
                }
            }
        }

        std::printf("\n  %d attempt(s) failed\n", g_fail);
        g_fail = 0;
    }

    CoUninitialize();
    return 0;
}
