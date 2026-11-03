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

}  // namespace neiro::wasapi

#endif  // _MSC_VER
#endif  // NEIRO_WASAPI_COMPAT_HPP
