// The ASIO device layer, on its own.
//
// The same split `backend_wasapi.h`/`wasapi_device.h` and `backend_coreaudio.cpp`
// use: `nae.cpp` keeps the policy -- which rate to ask for, what the verdict is --
// and this file keeps the device. The engine above the interface therefore has no
// ASIO branch, which is what keeps one render loop and one copy of the DSP.
//
// Two halves live behind it, for the same reason CoreAudio needs two files:
//
//   * `AudioBackend` + `BackendStream`, which is what a host asks about a device
//     and what owns the driver object and its buffers;
//   * `OutputStream`, which is the shape `nae.cpp`'s render loop drives
//     ("room, window, commit"). ASIO calls *you* from its own thread, so the
//     adapter between the two is a ring -- exactly as on macOS.
//
// ASIO is not a second DSP: the render callback receives the core's final
// interleaved device bytes and this layer only moves them into the driver's
// per-channel buffers.
#pragma once

#ifndef _WIN32
#error "backend_asio.h describes the Windows ASIO device layer and is not portable"
#endif

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio_backend.h"
#include "nae.h"
#include "output_stream.h"

namespace neiro::asio {

/// True for a device id this backend owns: `asio:{CLSID}`.
///
/// The prefix is the whole routing rule. `nae.cpp` asks this once, at open time,
/// and every id that does not start with `asio:` goes to WASAPI untouched -- so
/// adding this backend cannot change the behaviour of a WASAPI endpoint id.
bool isAsioDeviceId(const char* deviceId);

/// The drivers registered under `HKLM\SOFTWARE\ASIO` and its `Wow6432Node`.
///
/// The id is the registered CLSID with the `asio:` prefix and the name is the key
/// name, because that is the only identity the driver registers. A 32-bit-only
/// driver found under `Wow6432Node` cannot be loaded by this 64-bit process; it is
/// still listed, and opening it fails with the HRESULT the loader returned rather
/// than being dropped silently.
std::vector<BackendDevice> enumerateDevices(std::string* error);

/// Everything the engine asks an ASIO driver about itself.
///
/// One owner, next to the open: the rate list this reports and the rate the stream
/// negotiates cannot come from different code.
///
/// A driver that will not initialise (its hardware is absent, or another program
/// holds it) is still enumerated -- the registry entry exists -- but none of its
/// capabilities were measured, so the query fails with `NAE_ERR_NO_DEVICE` and the
/// driver's own reason in `error`, with `probed` and the native numbers left at 0.
/// Returning `NAE_OK` there would make "not measured" look like "measured and
/// zero", which is the one thing this project does not do.
int32_t queryAsioCaps(const char* deviceId, int32_t shareMode, NAE_DeviceCaps* out,
                      std::string* error) NAE_NOEXCEPT;

/// The ASIO stream in the shape the render loop drives.
///
/// `want.containerBits` must be 32 and `want.isFloat` false: this backend moves
/// 32-bit integer words, which is the container the core renders (24 valid bits,
/// left-justified -- `placeInContainer` shifts by 8 and `dither_check` measures
/// it). The driver's channel type is its own choice and there is no argument to
/// `createBuffers` that sets it, so the accepted types are the two that are that
/// container: `ASIOSTInt32LSB` and `ASIOSTInt32LSB24`. Anything else is refused
/// with the driver's own type name in `*error`, never converted -- a float driver
/// fed integer words plays noise at full scale.
///
/// A null return means the driver refused and `*error` says which call and which
/// code. Nothing here reports success for a stream that has no frames.
std::shared_ptr<OutputStream> openAsioStream(const StreamFormatRequest& want,
                                             OpenedFormat* opened, std::string* error);

}  // namespace neiro::asio
