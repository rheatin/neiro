// The only platform boundary in the engine.
//
// The core (policy/resolver/diagnostics, routing, dither, resampler, decoder)
// includes no platform header. A backend enumerates devices, reports caps,
// opens one stream and moves frames. It does not know about EQ, ReplayGain,
// routing policy, Base or diagnostics.
//
// The existing WASAPI code is still inside nae.cpp; this interface is the shape
// it is being extracted to. CoreAudio and AAudio implement the same interface.

#pragma once

#include "nae_policy.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neiro {

struct BackendDevice {
    std::string id;
    std::string name;
    bool isDefault = false;
};

// The backend pulls this many frames from the core. It runs on the backend's
// realtime callback, so the core side must not allocate, lock or block.
// Returning fewer frames than requested means end of source.
//
// The buffer is the FINAL interleaved device bytes, in the format the stream was
// opened with -- not floats for the backend to convert.
//
// That is deliberate, and it is the difference between one DSP and two. Dither
// and requantisation are stages of the engine's canonical order
// (`OUTPUT-POLICY.md §5`), and they are audible: a 24-in-32 endpoint must not be
// dithered as though it were 24-bit. If this callback returned floats, every
// backend would have to requantise itself, so the stage would live in
// `backend_wasapi.cpp` on Windows and nowhere at all on macOS, where the device
// format is already float32. One copy of the DSP is the rule; the stage stays in
// the core and a backend only moves bytes.
//
// `bytesPerFrame` is `request.resolved`'s channel count times the container
// width, so a backend that needs the layout has it from the open request.
using RenderCallback = std::function<size_t(void* out, size_t frames)>;

struct BackendOpenRequest {
    std::string deviceId;
    std::string path;  // source file; empty means a generated tone
    NAE_SourceInfo source{};
    NAE_OutputPolicy policy{};
    NAE_ResolvedSignalPath resolved{};  // filled by the resolver, not the backend
    uint32_t bufferFrames = 0;
    uint32_t periodFrames = 0;
    bool eventDriven = false;
    RenderCallback render;
};

// The backend owns the device handle and the render callback. The core owns
// sample processing and dithering, and writes the final interleaved device bytes
// in the format this stream was opened with; the backend only moves them.
class BackendStream {
  public:
    virtual ~BackendStream() = default;

    virtual bool start(std::string* error) = 0;
    virtual void stop() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;

    // Pull/negotiate is backend-specific; this hook is deliberately minimal.
    // Device-lost is a first-class state, not a generic error.
    virtual bool deviceLost() const = 0;
    virtual double latencyMs() const = 0;
};

class AudioBackend {
  public:
    virtual ~AudioBackend() = default;

    virtual const char* name() const = 0;
    virtual std::vector<BackendDevice> enumerate(std::string* error) = 0;
    virtual bool queryCaps(const std::string& deviceId, int32_t shareMode,
                           NAE_DeviceCaps* out, std::string* error) = 0;
    virtual std::unique_ptr<BackendStream> open(const BackendOpenRequest& request,
                                                std::string* error) = 0;
    virtual void shutdown() = 0;
};

// Platform factory: WASAPI on Windows, CoreAudio on macOS, AAudio/OpenSL on
// Android. The core never calls it; the host chooses one backend at build time.
std::unique_ptr<AudioBackend> createPlatformBackend();

}  // namespace neiro
