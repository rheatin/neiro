// The WASAPI device helpers two translation units share.
//
// The device code is split in two now: `nae.cpp` keeps the *policy* -- which rate
// to ask for, what the verdict is -- and `backend_wasapi.cpp` keeps the *device*
// -- activate, negotiate, start, stop, hand out buffers. Both need the same
// reading of the endpoint's own format, the same COM apartment, and the same
// verdict on "the device is gone". Defining them twice would let the two answers
// drift, which is the failure this project already has a rule about.
//
// Everything here is Windows-only and is included only by Windows translation
// units. `backend_wasapi.cpp` is not compiled at all on macOS.
#pragma once

#ifndef _WIN32
#error "wasapi_device.h describes the Windows device layer and is not portable"
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

#include "wasapi_compat.hpp"

/// The HRESULT behind the last failure, reported by `NAE_GetLastHresult`.
///
/// Defined once, in `nae.cpp`: it is the C ABI's answer, and a second copy would
/// let the engine and the device layer disagree about why the open failed.
extern thread_local HRESULT g_lastHr;

namespace neiro::wasapi {

/// Record and pass through an HRESULT, so a caller can report it later.
///
/// Every device call that can fail goes through this. A bare return code tells a
/// host that something went wrong and nothing else; the number is what
/// `NAE_GetLastHresult` hands back, and it is the only thing that distinguishes
/// "the format was refused" from "another program holds the endpoint".
inline HRESULT remember(HRESULT hr) {
    g_lastHr = hr;
    return hr;
}

/// Did the output device stop existing?
///
/// True for the codes Windows uses when the endpoint is gone rather than merely
/// busy: unplugged, disabled in the Sound control panel, or taken away by another
/// application that asked for exclusive access. `AUDCLNT_E_RESOURCES_INVALIDATED`
/// and `AUDCLNT_E_SERVICE_NOT_RUNNING` are included because they mean the same
/// thing operationally -- the stream cannot continue and the handle is still worth
/// keeping -- even though the cause differs.
///
/// What is deliberately *not* here: `AUDCLNT_E_BUFFER_TOO_LARGE` and
/// `AUDCLNT_E_OUT_OF_ORDER`. Those are a device that is still there and asking for
/// less than was offered, and the correct response is a smaller request on the
/// next slice. Treating them as a lost device would tear down a stream that would
/// have recovered on its own.
///
/// This is the single place that decision is made. A host should not have to know
/// these values, and neither should the render loop's other branches.
inline bool deviceIsGone(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

/// Whether the COM apartment has been initialised on this thread.
///
/// A function-local static in an inline function: one instance for the whole
/// binary, so `nae.cpp` and `backend_wasapi.cpp` cannot each believe they own the
/// apartment.
inline bool& comReady() {
    static thread_local bool ready = false;
    return ready;
}

/// COM apartment lifetime.
///
/// An earlier version called CoUninitialize() at the end of an open. That is
/// wrong: the returned handle holds COM interfaces created in *this* apartment,
/// and tearing the apartment down while those interfaces are still alive makes any
/// later call on them undefined — Start() crashed with an access violation. The
/// apartment has to outlive every handle created in it, so initialisation is
/// per-thread and stays until the host explicitly calls NAE_Shutdown().
inline HRESULT ensureCom() {
    if (comReady()) return S_OK;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means the thread already initialised COM as
    // apartment-threaded; that is fine, we just must not uninitialise it.
    if (hr == RPC_E_CHANGED_MODE) {
        comReady() = true;
        return S_OK;
    }
    if (FAILED(hr)) return hr;
    comReady() = true;
    return S_OK;
}

/// Release the apartment the host asked us to take, if this thread took one.
inline void shutdownCom() {
    if (!comReady()) return;
    comReady() = false;
    CoUninitialize();
}

/// The endpoint's own format, from the property store.
///
/// The device is the authority on what it can carry, and this is where that
/// answer is read: `PKEY_AudioEngine_DeviceFormat` is the format Windows itself
/// would use for the endpoint.
inline bool readNativeFormat(IMMDevice* device, WAVEFORMATEX* out, uint16_t* validBits = nullptr) {
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (FAILED(openPropertyStore(device, store.GetAddressOf())) || !store) return false;

    PROPVARIANT value;
    PropVariantInit(&value);
    if (FAILED(store->GetValue(PKEY_AudioEngine_DeviceFormat, &value))) return false;

    bool ok = false;
    if (validBits) *validBits = 0;
    if (value.vt == VT_BLOB && value.blob.pBlobData &&
        value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        const auto* fmt = reinterpret_cast<const WAVEFORMATEX*>(value.blob.pBlobData);
        if (fmt->wFormatTag == WAVE_FORMAT_PCM || fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE ||
            fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            *out = *fmt;

            // The valid-bits count lives in the EXTENSIBLE tail, which
            // WAVEFORMATEX has no room for. A 24-in-32 endpoint reports
            // wBitsPerSample=32 here, and losing wValidBitsPerSample=24 is
            // exactly what made exclusive mode look impossible on hardware
            // that supports it perfectly well.
            if (validBits && fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                value.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
                *validBits =
                    reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->Samples.wValidBitsPerSample;
            }
            ok = true;
        }
    }
    PropVariantClear(&value);
    return ok;
}

/// Build a WAVEFORMATEXTENSIBLE from a device's own description.
///
/// Passing the device's format through verbatim does not work: a plain
/// WAVE_FORMAT_PCM description is rejected with E_INVALIDARG. Re-wrapping it as
/// extensible with a consistent cbSize is the form drivers accept. (Found the
/// hard way in the wasapi-probe spike.)
inline WAVEFORMATEXTENSIBLE wrap(const WAVEFORMATEX* src, uint16_t validBits) {
    WAVEFORMATEXTENSIBLE out{};
    ZeroMemory(&out, sizeof(out));
    out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    out.Format.nChannels = src->nChannels;
    out.Format.nSamplesPerSec = src->nSamplesPerSec;
    out.Format.wBitsPerSample = src->wBitsPerSample;
    out.Format.nBlockAlign = src->nBlockAlign;
    out.Format.nAvgBytesPerSec = src->nAvgBytesPerSec;
    out.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    // wValidBitsPerSample MUST come from the device's own value, not from
    // wBitsPerSample. Plenty of endpoints report 24 bits of audio inside a
    // 32-bit container. Asking for 32 bits of real data is something the driver
    // cannot supply, and it answers AUDCLNT_E_ENDPOINT_CREATE_FAILED — the same
    // error Windows gives when another application holds the endpoint.
    //
    // That misreading cost real time: exclusive mode was declared impossible on
    // a machine where it works perfectly, and this one line was the cause. The
    // endpoint that exposed it reports bits=32 valid=24; passing its own blob
    // through untouched succeeds where wrap() failed. See format_matrix.cpp.
    //
    // Fall back to the container size only when the device reported nothing,
    // which is the case for a bare WAVEFORMATEX with no valid-bits field.
    out.Samples.wValidBitsPerSample = validBits ? validBits : src->wBitsPerSample;
    out.dwChannelMask =
        src->nChannels == 1 ? SPEAKER_FRONT_LEFT : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    out.SubFormat = (src->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                                                : KSDATAFORMAT_SUBTYPE_PCM;
    return out;
}

}  // namespace neiro::wasapi
