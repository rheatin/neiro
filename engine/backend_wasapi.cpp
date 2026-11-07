// The WASAPI device layer, on its own.
//
// This used to be the body of the Windows branch of `openExclusiveImpl` in
// `nae.cpp`, plus a `WasapiStream` adapter struct that wrapped the handle's COM
// pointers in the `OutputStream` interface. Both are gone. What is here now is a
// stream that owns what it opens -- the two COM clients, the render event and the
// granted format -- and a factory that negotiates the format and reports only
// what the device granted.
//
// The split is the one the macOS port forced: `nae.cpp` keeps the *policy* (which
// rate, what verdict, which engine state follows) and this file keeps the
// *device*. The engine code above the interface has no platform branch, so the
// DSP, the dither and the position accounting are one copy on both systems.
#include "output_stream.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "nae_policy.h"
#include "wasapi_compat.hpp"
#include "wasapi_device.h"

namespace neiro {
namespace {

using Microsoft::WRL::ComPtr;

/// REFERENCE_TIME is 100-nanosecond units.
constexpr REFERENCE_TIME kReferenceUnitsPerSecond = 10000000;

/// The endpoint named by `deviceId`, or the system default when it is empty.
///
/// A null id means "whatever Windows is using right now", and it is resolved
/// here, at open time, rather than from a value a host captured earlier: a user
/// can plug in headphones between choosing and pressing play, and a stale id
/// would then send the audio to the speaker they just unplugged.
HRESULT resolveDevice(const std::string& deviceId, ComPtr<IMMDevice>* out) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator) return hr;
    if (deviceId.empty()) {
        return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, out->GetAddressOf());
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, nullptr, 0);
    if (needed <= 1) return E_INVALIDARG;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, deviceId.c_str(), -1, wide.data(), needed);
    return enumerator->GetDevice(wide.c_str(), out->GetAddressOf());
}

/// The wire format to ask for: the endpoint's own description, with the fields
/// the request overrides.
///
/// When the request carries the device's own numbers -- which is what the
/// ordinary open does -- this produces exactly `wrap(native, validBits)`, the
/// format that path used to build by hand. The overrides exist for the one caller
/// that asks for a format the endpoint does not advertise: DoP needs 24 valid
/// bits in a 32- or 24-bit container, not whatever the DAC's default is.
WAVEFORMATEXTENSIBLE wireFormat(const WAVEFORMATEX& native, uint16_t nativeValidBits,
                                const StreamFormatRequest& want) {
    WAVEFORMATEXTENSIBLE fmt = wasapi::wrap(&native, nativeValidBits);

    const uint16_t container = static_cast<uint16_t>(want.containerBits);
    if (want.channels != fmt.Format.nChannels || container != fmt.Format.wBitsPerSample) {
        fmt.Format.nChannels = static_cast<WORD>(want.channels);
        fmt.Format.wBitsPerSample = container;
        // Recompute the layout rather than carrying the old one: a 44.1 kHz
        // blockAlign under a 192 kHz rate, or a 32-bit align under a 24-bit
        // container, is a format the driver rejects.
        fmt.Format.nBlockAlign =
            static_cast<WORD>((want.containerBits / 8) * want.channels);
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.dwChannelMask = want.channels == 1 ? SPEAKER_FRONT_LEFT
                                               : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    }
    wasapi::validBits(fmt) = static_cast<WORD>(want.validBits ? want.validBits
                                                              : want.containerBits);
    fmt.SubFormat = want.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;

    // The target rate replaces the endpoint's own, and the derived byte rate
    // follows it.
    if (want.rate != fmt.Format.nSamplesPerSec) {
        fmt.Format.nSamplesPerSec = want.rate;
        const uint16_t bytesPerSample = static_cast<uint16_t>(fmt.Format.wBitsPerSample / 8);
        fmt.Format.nBlockAlign = static_cast<WORD>(bytesPerSample * fmt.Format.nChannels);
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
    }
    return fmt;
}

/// Owns one render event for the duration of the open, so an early return cannot
/// leak it. The event outlives this guard once the stream takes it.
struct EventGuard {
    HANDLE handle = nullptr;
    ~EventGuard() {
        if (handle) CloseHandle(handle);
    }
    HANDLE release() {
        HANDLE h = handle;
        handle = nullptr;
        return h;
    }
};

/// The device, as the render loop drives it: room, a window, a commit.
///
/// Everything it reports comes from the device. The engine keeps its own copy of
/// the numbers it needs, but the authority on the format and on how many frames
/// are free is here, because that is where the device's answer arrives.
class WasapiOutputStream final : public OutputStream {
  public:
    WasapiOutputStream(ComPtr<IAudioClient> client, ComPtr<IAudioRenderClient> render,
                       HANDLE eventHandle, const OpenedFormat& granted)
        : client_(std::move(client)),
          render_(std::move(render)),
          event_(eventHandle),
          granted_(granted),
          bytesPerFrame_(granted.channels * ((granted.containerBits + 7u) / 8u)) {}

    ~WasapiOutputStream() override {
        // Leave the device as it was found. A client released while started stops
        // anyway, but saying so is the difference between a documented shutdown
        // and hoping the release order is right.
        if (running_.load(std::memory_order_acquire)) {
            wasapi::stopClient(client_.Get());
            running_.store(false, std::memory_order_release);
        }
        if (event_) CloseHandle(event_);
    }

    uint32_t capacityFrames() const override { return granted_.bufferFrames; }
    uint32_t bytesPerFrame() const override { return bytesPerFrame_; }
    uint32_t channels() const override { return granted_.channels; }
    uint32_t rate() const override { return granted_.rate; }
    double latencyMs() const override { return granted_.latencyMs; }

    /// Asked for on every call, not cached at open: the device is allowed to
    /// change it while the stream runs, and a cached number would be a number
    /// about the past.
    double driverLatencyMs() const override {
        REFERENCE_TIME ticks = 0;
        if (FAILED(client_->GetStreamLatency(&ticks))) return 0.0;
        return static_cast<double>(ticks) / 10000.0;
    }

    /// Frames already committed to the device.
    ///
    /// WASAPI tracks its own committed frames per buffer in both share modes, so
    /// the render loop can ask for `capacityFrames() - queuedFrames()` and get the
    /// room that is actually free. Zero on failure, which the loop reads as "the
    /// whole buffer is free"; `acquire` then reports the real reason.
    uint32_t queuedFrames() const override {
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return 0;
        return padding;
    }

    StreamStatus acquire(uint32_t want, uint8_t** data, uint32_t* frames) override {
        if (!data || !frames) return StreamStatus::Error;
        *data = nullptr;
        *frames = 0;
        BYTE* raw = nullptr;
        UINT32 chunk = 0;
        const HRESULT hr = wasapi::getRenderBuffer(render_.Get(), want, &raw, &chunk);
        // "Not right now": the device has less than `want` free. Not an error,
        // and the caller retries after the next period.
        if (hr == AUDCLNT_E_BUFFER_TOO_LARGE || hr == S_FALSE) {
            return StreamStatus::NoRoom;
        }
        if (FAILED(hr)) {
            setError("the device refused a buffer");
            return wasapi::deviceIsGone(hr) ? StreamStatus::DeviceLost : StreamStatus::Error;
        }
        *data = raw;
        *frames = chunk;
        return chunk ? StreamStatus::Ok : StreamStatus::NoRoom;
    }

    StreamStatus commit(uint32_t frames, bool silent) override {
        const HRESULT hr =
            render_->ReleaseBuffer(frames, silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
        if (FAILED(hr)) {
            setError("the device refused a commit");
            return wasapi::deviceIsGone(hr) ? StreamStatus::DeviceLost : StreamStatus::Error;
        }
        written_.fetch_add(frames, std::memory_order_relaxed);
        return StreamStatus::Ok;
    }

    bool waitForRoom(uint32_t timeoutMs) override {
        if (event_) {
            // The device's own notification, which is the only thing precise
            // enough for a buffer of a few milliseconds.
            const bool room = WaitForSingleObject(event_, timeoutMs) == WAIT_OBJECT_0;
            // A wait that expires means the device did not come back for more
            // audio. The caller counts it as an underrun; so does the stream, so
            // the number is visible through the interface as well.
            if (!room) underruns_.fetch_add(1, std::memory_order_relaxed);
            return room;
        }
        // No render event: the device refused one (or the event could not be
        // created), so there is nothing to wait on. Sleeping for the caller's
        // timeout would turn one buffer period into two seconds -- measured at
        // 60 s of wall time for a 1.5 s tone -- so yield for a single millisecond
        // instead. Playback in this mode is timer-paced by the caller: its "the
        // buffer is full" branch waits a fraction of the buffer's own duration.
        (void)timeoutMs;
        const ULONGLONG until = GetTickCount64() + 1;
        while (GetTickCount64() < until) Sleep(0);
        return true;
    }

    StreamStatus start() override {
        const HRESULT hr = wasapi::startClient(client_.Get());
        if (FAILED(hr)) {
            setError("the device refused to start");
            return wasapi::deviceIsGone(hr) ? StreamStatus::DeviceLost : StreamStatus::Error;
        }
        running_.store(true, std::memory_order_release);
        return StreamStatus::Ok;
    }

    StreamStatus stop() override {
        if (!running_.load(std::memory_order_acquire)) return StreamStatus::Ok;
        const HRESULT hr = wasapi::stopClient(client_.Get());
        running_.store(false, std::memory_order_release);
        if (FAILED(hr)) {
            setError("the device refused to stop");
            return wasapi::deviceIsGone(hr) ? StreamStatus::DeviceLost : StreamStatus::Error;
        }
        return StreamStatus::Ok;
    }

    bool running() const override { return running_.load(std::memory_order_acquire); }

    int64_t framesWritten() const override {
        return written_.load(std::memory_order_relaxed);
    }

    uint64_t underruns() const override { return underruns_.load(std::memory_order_relaxed); }

    const char* lastError() const override {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return error_.c_str();
    }

  private:
    void setError(const char* what) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        error_ = what;
    }

    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE event_ = nullptr;
    OpenedFormat granted_{};
    uint32_t bytesPerFrame_ = 0;
    std::atomic<int64_t> written_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<bool> running_{false};
    mutable std::mutex errorMutex_;
    std::string error_;
};

}  // namespace

std::shared_ptr<OutputStream> openWasapiStream(const StreamFormatRequest& want,
                                               OpenedFormat* opened, std::string* error) {
    if (opened) *opened = OpenedFormat{};
    if (error) error->clear();

    if (FAILED(wasapi::ensureCom())) {
        if (error) *error = "the COM apartment could not be initialised";
        return nullptr;
    }
    if (want.rate == 0 || want.channels == 0 || want.containerBits == 0) {
        if (error) *error = "the format request is incomplete";
        return nullptr;
    }

    ComPtr<IMMDevice> device;
    const HRESULT deviceHr = resolveDevice(want.deviceId, &device);
    if (FAILED(deviceHr) || !device) {
        // No id, no device. A play carrying no device id can only land on the
        // endpoint used last; if it is gone, that is a failure, never a quiet
        // choice of a different endpoint.
        if (error) *error = "no output device matches this id";
        return nullptr;
    }

    WAVEFORMATEX native{};
    uint16_t nativeValidBits = 0;
    if (!wasapi::readNativeFormat(device.Get(), &native, &nativeValidBits)) {
        if (error) *error = "the endpoint reports no usable native format";
        return nullptr;
    }

    // An explicit buffer duration, when the caller asked for one. Zero asks for
    // the device's own period -- the default every ordinary open uses, and the
    // measurement on this endpoint says asking for a shorter one makes the
    // delivered rate worse rather than better.
    REFERENCE_TIME requestedDuration = 0;
    if (want.bufferFrames > 0) {
        requestedDuration = static_cast<REFERENCE_TIME>(
            (static_cast<int64_t>(want.bufferFrames) * kReferenceUnitsPerSecond) /
            static_cast<int64_t>(want.rate));
    }

    const AUDCLNT_SHAREMODE shareMode = want.shareMode == NAE_SHARE_MODE_EXCLUSIVE
                                            ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                            : AUDCLNT_SHAREMODE_SHARED;

    // Event-driven, so the engine is told when the next period is due instead of
    // polling for it. The order is the API's, not a preference:
    //
    //   1. create the event,
    //   2. `Initialize` with AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
    //   3. `SetEventHandle` -- only after `Initialize` has succeeded,
    //   4. `Start`, when the caller says so.
    //
    // Calling `SetEventHandle` before `Initialize` is refused with
    // AUDCLNT_E_NOT_INITIALIZED, which is what this code used to do: the event
    // was silently dropped and the stream ran in polling mode.
    //
    // Event callbacks are an optimisation, not a requirement. An endpoint that
    // refuses them -- this one answers AUDCLNT_E_BUFFER_SIZE_ERROR to an
    // event-driven exclusive `Initialize` -- must still play, so a refusal rebuilds
    // the client without the flag instead of failing the open.
    EventGuard event;
    std::string pacingNote;
    event.handle = CreateEventW(nullptr, /*manualReset=*/FALSE, /*initialState=*/FALSE, nullptr);
    DWORD streamFlags = event.handle ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
    // No event means no event callbacks, and the stream paces on a timer. Say
    // which of the three reasons applied, every time: a silent timer fallback is
    // indistinguishable from a healthy run."""
    if (!event.handle) {
        pacingNote = "the endpoint gave no render event";
    }

    // One `Initialize` attempt, plus the fallback to the backend's own period when
    // an explicit one is refused. A refused buffer is reported, not fatal.
    WAVEFORMATEXTENSIBLE fmt = wireFormat(native, nativeValidBits, want);
    bool usedDefaultBuffer = false;
    const auto initialize = [&](IAudioClient* client, DWORD flags) -> HRESULT {
        usedDefaultBuffer = false;
        // Rebuilt each time: `Initialize` is allowed to write back into the
        // format it was handed, so a retry must start from the request again.
        fmt = wireFormat(native, nativeValidBits, want);
        HRESULT hr = client->Initialize(shareMode, flags, requestedDuration, 0,
                                        &fmt.Format, nullptr);
        if (FAILED(hr) && requestedDuration != 0) {
            // `Initialize` may only be called again after a failure, which leaves
            // the client uninitialised.
            fmt = wireFormat(native, nativeValidBits, want);
            hr = client->Initialize(shareMode, flags, 0, 0, &fmt.Format, nullptr);
            if (SUCCEEDED(hr)) usedDefaultBuffer = true;
        }
        return hr;
    };

    ComPtr<IAudioClient> client;
    // `Initialize` is once per client, so every retry needs a new one.
    const auto newClient = [&]() -> bool {
        client.Reset();
        return SUCCEEDED(wasapi::activate(device.Get(), __uuidof(IAudioClient),
                                          client.GetAddressOf())) &&
               client;
    };
    if (!newClient()) {
        if (error) *error = "the endpoint would not give an audio client";
        return nullptr;
    }

    HRESULT initHr = initialize(client.Get(), streamFlags);

    if (FAILED(initHr) && streamFlags != 0) {
        pacingNote = "the endpoint refused an event-driven stream";
        // The endpoint would not take an event-driven stream at all. Drop the
        // event and rebuild: polling is what this engine has always used.
        CloseHandle(event.handle);
        event.handle = nullptr;
        streamFlags = 0;
        if (!newClient()) {
            if (error) *error = "the endpoint would not give a second audio client";
            return nullptr;
        }
        initHr = initialize(client.Get(), streamFlags);
    }

    // The event handle is attached only after a successful `Initialize`.
    if (SUCCEEDED(initHr) && event.handle && FAILED(client->SetEventHandle(event.handle))) {
        // The client now advertises event callbacks without an event to signal,
        // and an initialised client cannot be re-configured. Start over without
        // them: the stream then falls back to the caller's timer pacing.
        //
        // The reason is recorded here too. It was not, at first: this branch
        // reported `pacing=timer` with an empty note, which is the one state the
        // report exists to make impossible.
        pacingNote = "the endpoint refused the render event handle";
        CloseHandle(event.handle);
        event.handle = nullptr;
        streamFlags = 0;
        if (!newClient()) {
            if (error) *error = "the endpoint would not give a second audio client";
            return nullptr;
        }
        initHr = initialize(client.Get(), streamFlags);
    }

    if (FAILED(initHr)) {
        wasapi::remember(initHr);
        // State the facts and stop. AUDCLNT_E_ENDPOINT_CREATE_FAILED is used by
        // Windows both for a format the endpoint will not accept and for an
        // endpoint another application already holds, so naming either cause here
        // would be a guess. The caller knows which rate it asked for and whether
        // the endpoint supports it; the diagnosis belongs one layer up.
        char detail[128];
        std::snprintf(detail, sizeof(detail), "0x%08X", static_cast<unsigned>(g_lastHr));
        if (error) {
            *error = std::string("exclusive mode refused (") + detail + "): " +
                     std::to_string(fmt.Format.nSamplesPerSec) + " Hz, " +
                     std::to_string(fmt.Format.wBitsPerSample) + "-bit " +
                     std::to_string(fmt.Format.nChannels) + "ch on this endpoint";
        }
        return nullptr;
    }
    if (usedDefaultBuffer && error) {
        *error = "the device refused the requested buffer; the backend default is used";
    }
    ComPtr<IAudioRenderClient> render;
    if (FAILED(wasapi::remember(wasapi::getService(
            client.Get(), __uuidof(IAudioRenderClient), render.GetAddressOf()))) ||
        !render) {
        if (error) *error = "the endpoint would not give a render client";
        return nullptr;
    }

    UINT32 frames = 0;
    if (FAILED(wasapi::remember(client->GetBufferSize(&frames))) || frames == 0) {
        if (error) *error = "the endpoint reported no buffer";
        return nullptr;
    }

    // The granted format, measured *after* `Initialize`: that call is allowed to
    // write back into the format it was handed, and the difference between what
    // was asked for and what the device agreed to is the single most useful thing
    // to know when the output comes out wrong.
    OpenedFormat granted;
    granted.rate = fmt.Format.nSamplesPerSec;
    granted.channels = fmt.Format.nChannels;
    granted.containerBits = fmt.Format.wBitsPerSample;
    granted.validBits = wasapi::validBits(fmt);
    granted.isFloat = fmt.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    granted.bufferFrames = frames;
    granted.latencyMs = granted.rate ? 1000.0 * static_cast<double>(frames) /
                                           static_cast<double>(granted.rate)
                                     : 0.0;

    // Event-driven only counts if the handle was accepted; a refused
    // `SetEventHandle` above has already cleared it.
    granted.eventDriven = event.handle != nullptr;
    granted.pacingNote = granted.eventDriven ? std::string() : pacingNote;

    if (getenv("NAE_TRACE") != nullptr) {
        char pacing[192];
        if (granted.pacingNote.empty()) {
            std::snprintf(pacing, sizeof(pacing), "%s",
                          granted.eventDriven ? "event" : "timer");
        } else {
            std::snprintf(pacing, sizeof(pacing), "%s (%s)",
                          granted.eventDriven ? "event" : "timer",
                          granted.pacingNote.c_str());
        }
        std::fprintf(stderr, "[nae] granted frames=%u (%.2f ms) rate=%u pacing=%s\n",
                     granted.bufferFrames, granted.latencyMs, granted.rate, pacing);
    }

    if (opened) *opened = granted;
    return std::make_shared<WasapiOutputStream>(std::move(client), std::move(render),
                                                event.release(), granted);
}

}  // namespace neiro
