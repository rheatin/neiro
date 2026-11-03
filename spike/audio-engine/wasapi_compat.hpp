// MinGW-w64 compatibility shim for WASAPI.
//
// MinGW's bundled Windows SDK headers lag behind the real SDK in ways that
// matter here:
//
//   * AUDCLNT_STREAMFLAGS_* are entirely absent from audioclient.h.
//   * Several methods use COM-era signatures (out-parameters) rather than the
//     modern MSDN ones.
//   * PKEY_Device_FriendlyName is not declared.
//
// This header patches all of that in one place so the probe (and later the
// engine) can be written against the modern API. Everything here is additive;
// nothing modifies the SDK headers.
//
// If the engine ever moves to MSVC, this file compiles to nothing.

#pragma once

#ifndef NEIRO_WASAPI_COMPAT_HPP
#define NEIRO_WASAPI_COMPAT_HPP

// WAVEFORMATEXTENSIBLE, WAVE_FORMAT_EXTENSIBLE, KSDATAFORMAT_SUBTYPE_* and the
// SPEAKER_* masks. Needed on both toolchains, so it is outside the MSVC guard.
//
// ksmedia.h rather than mmregapi.h: mmregapi.h is where they live on the real
// SDK, but MinGW-w64 does not ship that header at all and the build here is
// MinGW. ksmedia.h is what mmregapi.h includes, so naming it directly compiles
// under both with no conditional.
#include <ksmedia.h>

#ifdef _MSC_VER
// MSVC has the real SDK headers; no shim needed.
#else

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>

// ---- AUDCLNT stream flags (missing from MinGW) ------------------------------

#ifndef AUDCLNT_STREAMFLAGS_EVENTCALLBACK
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00000004
#endif
#ifndef AUDCLNT_STREAMFLAGS_NOPERSIST
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00000008
#endif
#ifndef AUDCLNT_STREAMFLAGS_AUDCLNT_CALLBACK
#define AUDCLNT_STREAMFLAGS_AUDCLNT_CALLBACK \
    (AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST)
#endif
#ifndef AUDCLNT_STREAMFLAGS_AUTO_RESET
#define AUDCLNT_STREAMFLAGS_AUTO_RESET 0x00000001
#endif
#ifndef AUDCLNT_STREAMFLAGS_RAW
#define AUDCLNT_STREAMFLAGS_RAW 0x00000001
#endif

// ---- PKEY_Device_FriendlyName ----------------------------------------------
//
// Declared in FunctionDiscoveryKeys_devpkey.h on MSVC. The GUID is fixed by
// Microsoft and safe to hard-code.

#ifndef PKEY_Device_FriendlyName
#include <initguid.h>
EXTERN_C const PROPERTYKEY PKEY_Device_FriendlyName;
#endif

// PKEY_AudioEngine_DeviceFormat is declared by
// functiondiscoverykeys_devpkey.h but MinGW ships no definition for it, so
// linking fails. The GUID and property id are fixed by Microsoft.
#ifndef PKEY_AudioEngine_DeviceFormat
namespace {
// {f19f064d-082c-4e27-bc73-6882a1bb8e4c}, pid 0
constexpr PROPERTYKEY kPKEY_AudioEngine_DeviceFormat = {
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
}  // namespace
#define PKEY_AudioEngine_DeviceFormat kPKEY_AudioEngine_DeviceFormat
#endif

// ---- Error codes absent from MinGW -----------------------------------------

#ifndef AUDCLNT_E_EXCLUSIVE_MODE_ONLY_ALLOWED
#define AUDCLNT_E_EXCLUSIVE_MODE_ONLY_ALLOWED AUDCLNT_ERR(0x19)
#endif

// ---- Signature normalisation -------------------------------------------------
//
// These wrappers present the modern MSDN signatures on top of MinGW's
// COM-era ones, so calling code does not have to care which toolchain built it.

namespace neiro::wasapi {

// Milliseconds, at a resolution that can see inside one device period.
//
// GetTickCount64 counts at the system timer tick -- 15.6 ms by default -- and a
// device buffer on this endpoint is 53 ms. Everything worth measuring about a
// buffer happens within a few milliseconds of that, so this clock reports a whole
// class of faults as exactly zero. The first version of the lateness instrument
// used GetTickCount64 and read 0 for a fill it simply could not resolve, which
// looked identical to a pass.
inline double nowMsHr() {
    static const double kFreq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000.0 / kFreq;
}

// IMMDeviceCollection::GetCount() -> UINT GetCount()
inline UINT collectionCount(IMMDeviceCollection* c) {
    UINT n = 0;
    if (FAILED(c->GetCount(&n))) return 0;
    return n;
}

// IMMDevice::GetId(std::wstring&) -> HRESULT GetId(LPWSTR*)
inline HRESULT deviceId(IMMDevice* d, std::wstring& out) {
    LPWSTR raw = nullptr;
    const HRESULT hr = d->GetId(&raw);
    if (SUCCEEDED(hr) && raw) {
        out = raw;
        CoTaskMemFree(raw);
    }
    return hr;
}

// IMMDevice::OpenPropertyStore -> already close to modern; alias for clarity.
inline HRESULT openPropertyStore(IMMDevice* d, IPropertyStore** store) {
    return d->OpenPropertyStore(STGM_READ, store);
}

// IMMDevice::Activate -> modern form drops the CLSCTX templating parameter.
// Takes a void** so both raw pointers and WRL::ComPtr::GetAddressOf() work.
// T** rather than void** so both raw pointers and WRL::ComPtr::GetAddressOf()
// convert without a reinterpret_cast at every call site.
template <typename T>
inline HRESULT activate(IMMDevice* d, REFIID iid, T** out) {
    return d->Activate(iid, CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(out));
}

// ---------------------------------------------------------------------------
// IAudioClient / IAudioRenderClient differences.
//
// MinGW's headers differ from the real SDK in three places:
//
//   * Start() takes no REFERENCE_TIME argument.
//   * IAudioRenderClient::GetBuffer() takes 2 parameters, not 3. It reports
//     neither how many frames may be written nor how many were; the caller
//     gets back the number it asked for.
//   * IAudioClient::GetService() takes (REFIID, void**) rather than a
//     type-templated out-parameter.
//
// These wrappers paper over all three, so the engine source stays correct
// against either toolchain.
// ---------------------------------------------------------------------------

inline HRESULT startClient(IAudioClient* c) {
#ifdef _MSC_VER
    return c->Start(0);
#else
    return c->Start();
#endif
}

inline HRESULT stopClient(IAudioClient* c) {
#ifdef _MSC_VER
    return c->Stop();
#else
    return c->Stop();
#endif
}

// T** rather than void** so WRL::ComPtr::GetAddressOf() converts without a
// reinterpret_cast at every call site.
template <typename T>
inline HRESULT getService(IAudioClient* c, REFIID iid, T** out) {
    return c->GetService(iid, reinterpret_cast<void**>(out));
}

// Request `frames` of buffer space. On MinGW, where GetBuffer does not report
// a frame count, *framesToRead is filled with `frames`.
inline HRESULT getRenderBuffer(IAudioRenderClient* rc, UINT32 frames, BYTE** data,
                               UINT32* framesToRead) {
#ifdef _MSC_VER
    return rc->GetBuffer(frames, data, framesToRead);
#else
    const HRESULT hr = rc->GetBuffer(frames, data);
    if (SUCCEEDED(hr) && framesToRead) *framesToRead = frames;
    return hr;
#endif
}

}  // namespace neiro::wasapi

#endif  // _MSC_VER

// ---------------------------------------------------------------------------
// WAVEFORMATEXTENSIBLE::wValidBitsPerSample moved between SDK generations
// ---------------------------------------------------------------------------
//
// The modern SDK has it as a direct member. Older headers -- and MinGW's copy of
// them -- wrap it in a `Samples` union next to `wSamplesPerBlock` and
// `wReserved`, which is where it lived before the extension was extended. Same
// two bytes at the same offset either way; only the spelling differs.
//
// This matters here more than most toolchain differences, because the valid width
// is what distinguishes Int24In32 from Int32. Reading the wrong member silently
// yields 32 for a 24-in-32 format, and every 24-bit stream would then be reported
// as a 32-bit conversion.
//
// Outside the MSVC guard deliberately: MSVC needs the accessor too, just spelled
// the other way.

namespace neiro::wasapi {

/**
 * Place a quantised sample into a container that may be wider than the signal.
 *
 * A `WAVEFORMATEXTENSIBLE` format with `wBitsPerSample = 32` and
 * `wValidBitsPerSample = 24` means *24 significant bits occupying the top of a
 * 32-bit word* -- left-justified, which is how every 24-in-32 device ever made
 * expects to read it. Writing the quantised value straight through puts those 24
 * bits at the bottom of the word instead. The waveform is still perfectly
 * formed; it is just eight bits low, so the device plays it 256x too quietly.
 *
 * That is the whole fault, and it is invisible to inspection because every layer
 * above it is correct. It was found by measuring the level leaving the render fifo
 * against the level arriving in the device's buffer: 0.9194 became 0.0036.
 *
 * `containerBits <= validBits` returns the value untouched, so 16-in-16 and
 * 24-in-24 -- every path that was never broken -- take the identical route.
 *
 * Unsigned, deliberately. `value << 8` where `value` is a negative `int32_t` is
 * undefined behaviour, and -1 has to arrive as 0xFFFFFF00, not as whatever the
 * compiler decided that day.
 */
inline uint32_t placeInContainer(int32_t value, uint32_t containerBits, uint32_t validBitsArg) {
    if (containerBits <= validBitsArg || validBitsArg == 0) return static_cast<uint32_t>(value);
    return static_cast<uint32_t>(value) << (containerBits - validBitsArg);
}

inline WORD& validBits(WAVEFORMATEXTENSIBLE& wfx) {
#ifdef _MSC_VER
    return wfx.wValidBitsPerSample;
#else
    return wfx.Samples.wValidBitsPerSample;
#endif
}

inline WORD validBits(const WAVEFORMATEXTENSIBLE& wfx) {
#ifdef _MSC_VER
    return wfx.wValidBitsPerSample;
#else
    return wfx.Samples.wValidBitsPerSample;
#endif
}

}  // namespace neiro::wasapi

#endif  // NEIRO_WASAPI_COMPAT_HPP
