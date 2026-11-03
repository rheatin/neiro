// What did the negotiation actually ask the device, and what did it answer?
//
// mode_check reports `rc=5 hresult=0x00000000`: NAE_ERR_INITIALIZE with no HRESULT
// recorded, which is the shape of the `!negotiated.ok` return -- the path that sets
// the reason text and never touches g_lastHr. So the device refused every
// candidate, and the only way to find out why is to ask the device the same
// questions by hand and print the answers.
//
// This is a diagnostic, not a check: it opens the endpoint, so it cannot run while
// anything else holds it.
//
// # What it is for
//
// Three variables were conflated in the first attempt, and each produced the same
// symptom -- every candidate refused, exclusive playback dead:
//
//   1. the call itself (MinGW declares IsFormatSupported with a share mode the real
//      object does not take; calling through it passes a null format pointer)
//   2. the *form* of the format asked about (WAVE_FORMAT_EXTENSIBLE versus a plain
//      WAVEFORMATEX with WAVE_FORMAT_PCM)
//   3. the HRESULT vocabulary (AUDCLNT_E_EXCLUSIVE_MODE_ONLY is 0x88890012, not
//      the 0x88890019 this project's shim invented for a name MinGW spells
//      differently)
//
// A single "refused" answer cannot separate them. This asks both forms at several
// rates and prints the raw code, which is the only thing that can.
//
// Usage: negotiation_live.exe
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "wasapi_compat.hpp"
#include "exclusive_negotiation.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;
using namespace neiro;

namespace {

/**
 * The real IAudioClient::IsFormatSupported, reached through the vtable.
 *
 * MinGW's declaration has an extra leading share-mode parameter, so calling through
 * it hands the object an integer where it wants a pointer. See
 * `isFormatSupportedForExclusive` in wasapi_compat.hpp.
 */
struct AudioClientVtbl {
    void* slots[6];
    HRESULT(STDMETHODCALLTYPE* IsFormatSupported)(void*, const WAVEFORMATEX*,
                                                  WAVEFORMATEXTENSIBLE**);
};

HRESULT ask(IAudioClient* client, const WAVEFORMATEX& fmt) {
    auto** vtable = *reinterpret_cast<void***>(client);
    auto* prefix = reinterpret_cast<AudioClientVtbl*>(vtable);
    WAVEFORMATEXTENSIBLE* closest = nullptr;
    return prefix->IsFormatSupported(client, &fmt, &closest);
}

const char* name(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case 0x00000000ul: return "S_OK";
        case 0x88890001ul: return "AUDCLNT_E_NOT_INITIALIZED";
        case 0x88890004ul: return "AUDCLNT_E_DEVICE_INVALIDATED";
        case 0x88890008ul: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
        case 0x88890012ul: return "AUDCLNT_E_EXCLUSIVE_MODE_ONLY";
        case 0x8889000Ful: return "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        case 0x80070057ul: return "E_INVALIDARG";
        default: return "";
    }
}

WAVEFORMATEX plain(uint32_t rate, WORD bits, WORD channels = 2) {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = channels;
    f.nSamplesPerSec = rate;
    f.wBitsPerSample = bits;
    f.nBlockAlign = static_cast<WORD>(bits / 8 * channels);
    f.nAvgBytesPerSec = rate * f.nBlockAlign;
    return f;
}

void report(IAudioClient* client, const WAVEFORMATEX& fmt, const char* label) {
    const HRESULT hr = ask(client, fmt);
    std::printf("  %7u  %-12s  %2u ch %2u-bit  align %-3u  0x%08lX  %-28s %s\n",
                fmt.nSamplesPerSec, label, fmt.nChannels, fmt.wBitsPerSample,
                fmt.nBlockAlign, static_cast<unsigned long>(hr), name(hr),
                SUCCEEDED(hr) ? "accepted" : "refused");
}

} // namespace

int main() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::printf("CoInitializeEx: 0x%08lX\n", static_cast<unsigned long>(co));

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        std::printf("no enumerator\n");
        return 1;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
        std::printf("no default render endpoint\n");
        return 1;
    }

    LPWSTR idRaw = nullptr;
    if (SUCCEEDED(device->GetId(&idRaw)) && idRaw) {
        char narrow[128] = {0};
        WideCharToMultiByte(CP_UTF8, 0, idRaw, -1, narrow, sizeof(narrow) - 1, nullptr,
                            nullptr);
        std::printf("endpoint: %s\n", narrow);
        CoTaskMemFree(idRaw);
    }

    ComPtr<IAudioClient> client;
    if (FAILED(wasapi::activate(device.Get(), __uuidof(IAudioClient),
                               client.GetAddressOf())) ||
        !client) {
        std::printf("cannot activate IAudioClient\n");
        return 1;
    }

    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
        std::printf("mix format: %u Hz / %u ch / %u-bit / tag 0x%04X\n\n", mix->nSamplesPerSec,
                    mix->nChannels, mix->wBitsPerSample, mix->wFormatTag);
        CoTaskMemFree(mix);
    }

    std::printf("  rate    form          format                 IsFormatSupported\n");
    std::printf("  ------  ------------  ---------------------  "
                "------------------------------\n");

    // Plain PCM. The form a two-channel stream does not need to be extensible.
    for (uint32_t rate : {44100u, 48000u, 96000u, 192000u}) {
        for (WORD bits : {16, 24, 32}) {
            report(client.Get(), plain(rate, bits), "PCM");
        }
    }

    // The same rates as WAVEFORMATEXTENSIBLE, built the way the negotiation builds
    // them.
    std::printf("\n");
    for (uint32_t rate : {44100u, 48000u, 192000u}) {
        AudioFormat f;
        f.sampleRate = rate;
        f.channels = 2;
        f.bitDepth = 24;
        f.sampleFormat = SampleFormat::Int24In32;
        const WAVEFORMATEXTENSIBLE wfx = buildCandidate(f);
        report(client.Get(), wfx.Format, "extensible24");
    }

    // And what the whole search concludes through the engine's own code path.
    AudioFormat source;
    source.sampleRate = 44100;
    source.channels = 2;
    source.bitDepth = 24;
    source.sampleFormat = SampleFormat::Int24In32;

    const auto predicate = [&device](const WireFormat& wire) {
        // Opened, not asked: see the note at the top of this file. `ask` is the
        // local vtable call, so the diagnostic does not depend on the compat layer
        // agreeing with it about the signature.
        ComPtr<IAudioClient> t;
        HRESULT hr = wasapi::activate(device.Get(), __uuidof(IAudioClient), t.GetAddressOf());
        if (SUCCEEDED(hr) && t) {
            hr = t->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 0, 0, wire.forOpen(), nullptr);
        }
        return SUCCEEDED(hr) ? 0u : static_cast<uint32_t>(hr);
    };
    const NegotiationResult r = negotiateExclusive(source, 0, predicate);

    std::printf("\n  negotiateExclusive: ok=%d  rate=%u  firstFailure=0x%08X\n", r.ok ? 1 : 0,
                r.format.sampleRate, static_cast<unsigned>(r.firstFailure));
    std::printf("  reason: %s\n", r.reason.c_str());


    // The same questions asked by *opening* the endpoint, which is what the rate
    // probe this replaced used to do. Each attempt gets a throwaway client, which
    // must be released before the next or the endpoint is held by our own probe
    // and every later candidate fails for that reason alone.
    //
    // If these succeed where IsFormatSupported refused every format at every rate,
    // then the endpoint will not answer format questions before it has been
    // initialised, and IsFormatSupported is not a usable probe here whatever the
    // interface documentation implies.
    std::printf("\n  the same candidates, opened rather than asked:\n");
    std::printf("     rate  depth  hr          verdict\n");
    for (uint32_t rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u}) {
        for (WORD bits : {16, 32}) {
            ComPtr<IAudioClient> throwaway;
            HRESULT hr = wasapi::activate(device.Get(), __uuidof(IAudioClient),
                                           throwaway.GetAddressOf());
            if (SUCCEEDED(hr) && throwaway) {
                const WAVEFORMATEX f = plain(rate, bits);
                hr = throwaway->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 0, 0, &f, nullptr);
            }
            std::printf("  %7u  %2u-bit  0x%08lX  %-28s %s\n", rate, bits,
                        static_cast<unsigned long>(hr), name(hr),
                        SUCCEEDED(hr) ? "OPENED" : "");
        }
    }
    // The form matters, and the difference is not academic.
    //
    // Every successful open above used a plain WAVEFORMATEX with WAVE_FORMAT_PCM.
    // The negotiation builds WAVEFORMATEXTENSIBLE for everything, because that is
    // the only struct that can express a valid-bit count. For two channels at 16
    // bits it expresses nothing extra, and a driver is entitled to refuse a
    // superfluous extension. Asked as an open rather than a query, here:
    std::printf("\n  plain PCM vs extensible, both opened:\n");
    std::printf("     rate  form           hr          verdict\n");
    for (uint32_t rate : {44100u, 192000u}) {
        {
            ComPtr<IAudioClient> t;
            HRESULT hr = wasapi::activate(device.Get(), __uuidof(IAudioClient), t.GetAddressOf());
            const WAVEFORMATEX f = plain(rate, 16);
            if (SUCCEEDED(hr) && t) hr = t->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 0, 0, &f, nullptr);
            std::printf("  %7u  %-14s 0x%08lX  %s\n", rate, "plain PCM16",
                        static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "OPENED" : "");
        }
        {
            ComPtr<IAudioClient> t;
            HRESULT hr = wasapi::activate(device.Get(), __uuidof(IAudioClient), t.GetAddressOf());
            AudioFormat af; af.sampleRate = rate; af.channels = 2; af.bitDepth = 16;
            af.sampleFormat = SampleFormat::Int16;
            const WAVEFORMATEXTENSIBLE wfx = buildCandidate(af);
            if (SUCCEEDED(hr) && t) hr = t->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 0, 0, &wfx.Format, nullptr);
            std::printf("  %7u  %-14s 0x%08lX  %s\n", rate, "extensible16",
                        static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "OPENED" : "");
        }
    }
    CoUninitialize();
    return 0;
}
