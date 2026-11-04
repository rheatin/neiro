#include "nae.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <mmsystem.h>   // waveOut, unrelated to the scheduler
#include <avrt.h>       // AvSetMmThreadCharacteristicsW -- the multimedia class scheduler
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Only for `avutil_license`: the capabilities string reports the licence of the
// FFmpeg that is actually linked, rather than one copied from a document.
#ifdef NEIRO_HAVE_FFMPEG
// `avutil.h` does not wrap its declarations in `extern "C"`, so a C++ include
// produces a mangled reference to `avutil_license` and the link fails on a
// symbol that is present in the DLL under its plain C name. The declaration is
// repeated here with C linkage rather than patched into a vendored header.
extern "C" const char* avutil_license(void);
#endif

#include "wasapi_compat.hpp"
#include "dither.h"
#include "routing.h"
#include "clip_guard.h"
#include "partitioned_convolver.h"
#include "loudness.h"
#include "dsd_downrate.h"
#include "eq_coeffs.h"
#include "linear_phase_eq.h"

#ifdef NEIRO_HAVE_FFMPEG
// The decoder hides every FFmpeg type behind plain float samples, so including it
// here does not make FFmpeg part of this file's dependencies -- but the
// capability still has to be advertised, and the file-open path has to exist for
// the build to be useful.
#include "decoder.h"
#include "dff_reader.h"
#include "dsf_reader.h"
#include "utf8_file.h"
#include "dop_pack.h"
#endif
#include "output_mode.h"
#include "resampler.h"

using Microsoft::WRL::ComPtr;

namespace {

thread_local HRESULT g_lastHr = S_OK;

// Why the last call failed, in words.
//
// A bare error code tells a caller that something went wrong and nothing else.
// For a music player the useful message is "the file is not audio" rather than
// "0x0208...", and the host has no way to produce the first from the second.
thread_local std::string g_lastErrorText;
thread_local bool g_comReady = false;
static std::atomic<uint32_t> g_capsGeneration{1};

HRESULT remember(HRESULT hr) {
    g_lastHr = hr;
    return hr;
}

// Did the output device stop existing?
//
// True for the codes Windows uses when the endpoint is gone rather than merely
// busy: unplugged, disabled in the Sound control panel, or taken away by another
// application that asked for exclusive access. AUDCLNT_E_RESOURCES_INVALIDATED
// and AUDCLNT_E_SERVICE_NOT_RUNNING are included because they mean the same
// thing operationally -- the stream cannot continue and the handle's worth
// keeping -- even though the cause differs.
//
// What is deliberately *not* here: AUDCLNT_E_BUFFER_TOO_LARGE and
// AUDCLNT_E_OUT_OF_ORDER. Those are a device that is still there and asking for
// less than was offered, and the correct response is a smaller request on the
// next slice. Treating them as a lost device would tear down a stream that would
// have recovered on its own.
//
// This is the single place that decision is made. A host should not have to know
// these values, and neither should the render loop's other branches.
bool deviceIsGone(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING;
}

// COM apartment lifetime.
//
// An earlier version called CoUninitialize() at the end of
// NAE_OpenExclusiveSine. That is wrong: the returned handle holds COM
// interfaces created in *this* apartment, and tearing the apartment down while
// those interfaces are still alive makes any later call on them undefined —
// Start() crashed with an access violation. The apartment has to outlive every
// handle created in it, so initialisation is per-thread and stays until the
// host explicitly calls NAE_Shutdown().
HRESULT ensureCom() {
    if (g_comReady) return S_OK;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // RPC_E_CHANGED_MODE means the thread already initialised COM as
    // apartment-threaded; that is fine, we just must not uninitialise it.
    if (hr == RPC_E_CHANGED_MODE) {
        g_comReady = true;
        return S_OK;
    }
    if (FAILED(hr)) return hr;
    g_comReady = true;
    return S_OK;
}

// ---- 能力声明的来源 --------------------------------------------------------
//
// `NAE_GetEngineCapabilities` 里那个 `shareModes` 曾经是硬编码的
// `"[\"shared\",\"exclusive\"]"`。它是一句**声称**，不是一次测量：引擎里根本
// 没有打开 shared 流的代码路径（`tryInitialize` 的 shared 分支只在设备枚举
// 时被调用，用来回答「这设备能不能以 shared 打开」，见下面 919/921 两行）。
//
// 一句声称比没有更糟：调用方会照着它去选一条不存在的路线，然后在别处收到一个
// 说不清的失败。所以这份清单改成从下面这个表生成，表里**只有真的能开流的模式**。
//
// 为什么是表而不是常量：下一个人实现 shared 时，必须往这张表里加一行，而加一行
// 就意味着他同时接上了开流路径。字符串和实现在两个地方的时候，只有仪器能抓住
// 它们不同步——见 `capability_check.cpp`，它会核对 DLL 导出的开流入口。
struct ShareModeEntry {
    const char* name;
    // The value passed to `IAudioClient::Initialize`.
    //
    // It lives here rather than at the call site so that the table cannot claim a
    // mode the open path does not use. `capability_check` verifies the *name*
    // against the export table; this pairing is what keeps the name and the code
    // from drifting apart inside this file.
    AUDCLNT_SHAREMODE mode;
    // The exported entry point that opens a stream in this mode.
    //
    // Named explicitly rather than derived by convention. The convention
    // `NAE_Open` + Capitalised(mode) was what the instrument originally assumed,
    // and it happened to be wrong: the file entry point is `NAE_OpenFileShared`,
    // not `NAE_OpenShared`. The instrument caught that as "declared but not
    // openable" -- correctly, because from outside the DLL there is no way to
    // know which symbol opens what, and a list that does not say is a list
    // nobody can act on.
    const char* entryPoint;
};

constexpr ShareModeEntry kOpenableShareModes[] = {
    {"exclusive", AUDCLNT_SHAREMODE_EXCLUSIVE, "NAE_OpenExclusive"},
    {"shared", AUDCLNT_SHAREMODE_SHARED, "NAE_OpenFileShared"},
};

struct DeviceInfo {
    std::string id;
    std::string name;
    uint32_t sampleRate = 0;
    uint32_t bitsPerSample = 0;
    uint32_t validBits = 0;
    uint32_t channels = 0;
    bool hasNativeFormat = false;
    bool sharedOpens = false;
    bool exclusiveOpens = false;
};

bool readNativeFormat(IMMDevice* device, WAVEFORMATEX* out, uint16_t* validBits = nullptr) {
    ComPtr<IPropertyStore> store;
    if (FAILED(neiro::wasapi::openPropertyStore(device, &store)) || !store) return false;

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
                *validBits = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt)->Samples.wValidBitsPerSample;
            }
            ok = true;
        }
    }
    PropVariantClear(&value);
    return ok;
}

// Wait for the device to want another buffer, without missing its deadline.
//
// `Sleep(1)` is not a one-millisecond sleep. The Windows timer resolution is 1 to
// 15 ms and whichever it happens to be is what you get, so polling with `Sleep(1)`
// on this machine means sleeping long enough to miss a buffer deadline --
// repeatedly, and with no error to show for it.
//
// Measured on the endpoint this engine actually opens: the device's buffer is
// about 275 frames, which is 5.7 ms at 48 kHz, while the engine's waits were
// `bufferMs * 0.25` clamped up to 1 ms and a bare `Sleep(1)`. Both became a 15 ms
// nap. The device ran dry at every buffer boundary and the symptom was a click at
// the buffer rate: about 175 a second, identical throughout the track, unchanged
// by the volume control, and absent from every counter the engine keeps -- because
// from the render loop's side each write still was a full, correct, continuous
// buffer. It simply arrived too late.
//
// `Sleep(0)` yields the rest of the timeslice and returns in well under a
// millisecond, which is short enough to make a 5.7 ms deadline. Spinning does hold
// a core for the duration of playback; an event-driven client is the right answer
// rather than this, and in exclusive mode that means IAudioClient3. The yield is
// bounded and falls back to a real sleep, so a device that has genuinely stopped
// cannot become a busy loop.
// Wait, precisely, for the device to want another buffer.
//
// `Sleep` cannot do this. The default Windows timer granularity is about 15.6 ms,
// which is longer than the buffer this endpoint hands out, so every `Sleep(1)`
// overshoots the deadline by up to fifteen milliseconds and the shortfall
// accumulates once per buffer. Measured: 40960 frames delivered per second against
// a 48000 rate, 85.3% -- the correct signal, played at the wrong speed. Nothing
// else reported it: the fifo accounting balances, and every frame written is a
// real frame.
//
// The fix is a high-resolution waitable timer, which is what Twilight Echo's
// `WasapiExclusiveBackend::renderLoop` does and why. Its comment is the whole
// argument: "the default Windows timer granularity (~15.6ms) exceeds a typical
// 10ms exclusive buffer, which turns every period into an underrun. A
// high-resolution waitable timer keeps the cadence without winmm/timeBeginPeriod."
// That last clause matters -- it is a way to get sub-millisecond wakeups without
// changing the system timer resolution for every process on the machine.
//
// Falls back to the old spin where the flag is unsupported, which is pre-1803
// Windows. Not a hypothetical: this engine ships on whatever the user has.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void waitForDeviceBuffer(double waitMs) {
    if (waitMs < 0.1) waitMs = 0.1;
    if (waitMs > 25.0) waitMs = 25.0;

    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                                 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(waitMs * 10000.0);
        if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }

    // No high-resolution timer: yield until the wall clock says the wait is over.
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(waitMs + 1.0);
    while (GetTickCount64() < deadline) {
        Sleep(0);
    }
}

// Event-driven rendering.
//
// `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` asks the audio engine to signal an event when
// the next buffer period is due, rather than leaving the client to discover it by
// polling. Twilight Echo's exclusive backend uses exactly this -- its stream flags
// are `NOPERSIST | EVENTCALLBACK` unless it is deliberately in push mode -- and its
// render loop then waits on that event alongside the timer and the samples-ready
// signal from the decode thread.
//
// This engine polled. That is not merely slower: polling can only ever ask "is it
// ready yet", and the answer's precision is bounded by how often the question is
// asked. The engine can ask the device exactly when it will be ready, and it does
// not, which is a plausible account of a delivered rate below the negotiated one
// that no amount of timer precision could fix -- a high-resolution timer makes the
// *asking* precise and cannot make the *answering* any more frequent than the
// device's own period.
//
// `SetEventHandle` must be called before `Initialize`, which is why this is a
// function rather than a line at the top of the render loop.
inline HANDLE createRenderEvent() {
    // Auto-reset: WASAPI re-signals it for the next period, and a manual-reset
    // event left signalled would make every subsequent wait return immediately.
    return CreateEventW(nullptr, FALSE, FALSE, nullptr);
}


//
// Twilight Echo does this at the top of its render loop for the same reason it
// uses a high-resolution timer: an audio deadline is a deadline, and the default
// foreground quantum on a busy desktop can be longer than the buffer. Without it
// the scheduler is free to deschedule the thread that is supposed to be refilling
// the device's buffer, and the result looks exactly like a device that will not
// keep time.
//
// Once per thread rather than per call, because the setting belongs to the thread
// and the render loop is entered thousands of times a track. Failure is ignored on
// purpose: MMCSS is unavailable in some session configurations, and a thread with
// no QoS is worse than no attempt only in theory.
void ensureAudioThreadQos() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    DWORD taskIndex = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
}

// A brief yield-and-retry, for "the buffer is not free yet".
inline void spinForBuffer() {
    const ULONGLONG until = GetTickCount64() + 1;
    while (GetTickCount64() < until) {
        Sleep(0);
    }
}

// Build a WAVEFORMATEXTENSIBLE from a device's own description.
//
// Passing the device's format through verbatim does not work: a plain
// WAVE_FORMAT_PCM description is rejected with E_INVALIDARG. Re-wrapping it as
// extensible with a consistent cbSize is the form drivers accept. (Found the
// hard way in the wasapi-probe spike.)
WAVEFORMATEXTENSIBLE wrap(const WAVEFORMATEX* src, uint16_t validBits) {
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

// Try Initialize in the given share mode; on success the format actually in
// effect is written to `granted`.
bool tryInitialize(IMMDevice* device, bool exclusive, const WAVEFORMATEX& native,
                   uint16_t nativeValidBits, WAVEFORMATEXTENSIBLE* outFmt) {
    ComPtr<IAudioClient> client;
    if (FAILED(remember(neiro::wasapi::activate(device, __uuidof(IAudioClient),
                                                 client.GetAddressOf()))))
        return false;

    WAVEFORMATEXTENSIBLE want = wrap(&native, nativeValidBits);
    const AUDCLNT_SHAREMODE share =
        exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    // Polling mode. AUDCLNT_STREAMFLAGS_RAW is rejected by most endpoints with
    // AUDCLNT_E_INVALID_STREAM_FLAG, so it stays opt-in and probed, never assumed.
    const HRESULT hr = remember(client->Initialize(share, 0, 0, 0, &want.Format, nullptr));
    if (FAILED(hr)) return false;
    if (outFmt) *outFmt = want;
    return true;
}

// The rates worth asking about.
//
// WASAPI offers no way to enumerate what an endpoint will accept, so the only
// honest source is trying. These are the rates a driver is plausibly willing to
// open; the list is not exhaustive and does not pretend to be -- it is a
// candidate set, and a rate that is missing from it is treated as unsupported
// only when the probe succeeds, so a rate nobody thought of here costs nothing.
constexpr uint32_t kCandidateRates[] = {
    8000,   11025,  16000,  22050,  32000,  44100,  48000,  64000,
    88200,  96000,  176400, 192000, 352800, 384000, 768000,
};

// Cache of the last probe.
//
// Keyed by device id *and* by the endpoint's native rate, because the two
// together identify the format situation, and a rate list belongs to a specific
// endpoint in a specific state. A cache keyed on the device alone would keep
// answering for a monitor that has since been unplugged and replaced by one
// with a different list, which is the failure this whole change exists to stop.
//
// Guarded: two sessions opened concurrently both reach the probe, and an
// unguarded std::string + std::vector here is a data race, not just a slow
// duplicate probe. The lock also serialises the exclusive Initialize calls,
// which would otherwise fight over the endpoint.
struct RateCache {
    std::string deviceId;
    uint32_t nativeRate = 0;
    std::vector<uint32_t> rates;
    bool probed = false;
};

RateCache& rateCache() {
    static RateCache cache;
    return cache;
}

std::mutex& rateCacheMutex() {
    static std::mutex m;
    return m;
}

// Ask the driver which of the candidate rates it will open exclusively.
//
// The subtlety that makes this correct rather than merely plausible: a probe
// where *nothing* succeeds usually means the endpoint is held by another
// application, not that the driver dislikes every rate. Those look identical from
// outside -- both surface as AUDCLNT_E_ENDPOINT_CREATE_FAILED -- and they have
// opposite consequences. Believing the second and caching an empty list would
// make the engine permanently refuse to open the device, on a machine where it
// works, for the rest of the process. So an empty result is reported as "not
// probed" and the caller keeps its old behaviour.
std::vector<uint32_t> probeSupportedRates(IMMDevice* device, const WAVEFORMATEX& native,
                                          uint16_t nativeValidBits, bool* probed) {
    std::vector<uint32_t> found;
    *probed = false;

    for (const uint32_t rate : kCandidateRates) {
        ComPtr<IAudioClient> probe;
        if (FAILED(neiro::wasapi::activate(device, __uuidof(IAudioClient), probe.GetAddressOf())) ||
            !probe) {
            return {};
        }

        WAVEFORMATEXTENSIBLE want = wrap(&native, nativeValidBits);
        want.Format.nSamplesPerSec = rate;
        const uint16_t bytesPerSample = static_cast<uint16_t>(want.Format.wBitsPerSample / 8);
        want.Format.nBlockAlign = static_cast<WORD>(bytesPerSample * want.Format.nChannels);
        want.Format.nAvgBytesPerSec = want.Format.nSamplesPerSec * want.Format.nBlockAlign;

        // `probe` is released by the ComPtr at the end of this iteration, before
        // the next candidate is tried. Holding it while trying the next rate
        // would make the endpoint busy with our own probe and every later
        // candidate would fail for that reason alone.
        if (SUCCEEDED(probe->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 0, 0, &want.Format,
                                        nullptr))) {
            found.push_back(rate);
        }
    }

    // Nothing at all worked, including the endpoint's own advertised rate. That
    // is contention or a device that has gone away, not a capability answer.
    if (found.empty()) return {};

    *probed = true;
    std::sort(found.begin(), found.end());
    return found;
}

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

// Seam-distribution sizes, at file scope.
//
// They are needed in two places -- the render loop, which accumulates into the
// arrays, and NAE_QuerySignalPath, which summarises them -- and a `static constexpr`
// member of NAE_Handle is not in scope in a free function. Declaring them as
// members as well does not help: the member version shadows the file-scope one and
// the arrays end up sized by whichever declaration a given use site resolved to.
constexpr size_t kSeamQuantiles = 8;
/// Log-spaced buckets from 0.01 to 100, i.e. -2..+2 decades.
constexpr size_t kSeamBuckets = 41;
/// Seams retained for the quantiles. 4096 is about 35 minutes at this buffer size:
/// long enough to catch a periodic artefact, short enough not to be a leak.
constexpr size_t kSeamSamples = 4096;


// Handle definition lives here rather than in the header: the C ABI must not
// leak COM types into consumers.
struct NAE_Handle {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    WAVEFORMATEXTENSIBLE fmt{};
    UINT32 bufferFrames = 0;
    double latencyMs = 0.0;

    // ---- conversion state ----
    //
    // These live on the handle rather than being rebuilt per block. soxr carries
    // filter history across calls, so recreating it would put an audible seam in
    // the output every block; the dither generator carries a random state for
    // the same reason.

    // The rate the source material is at, before conversion.
    uint32_t sourceRate = 0;

    // The file's own channel count, which differs from the device's whenever a
    // conversion happened. NAE_QuerySignalPath reports the file's, because the
    // listener's question is "what is in the file" and the device's number
    // answers a different one.
    int32_t fileChannels = 0;

    // Present only when a converter is actually in the path. When rates match,
    // `resampler.ready()` is false and samples pass through untouched — which is
    // what makes the bit-perfect claim true rather than merely intended.
    neiro::Resampler resampler;

    // Dither is sized to the device's *valid* bits, not the container size. A
    // 24-in-32 device must not dither as though it were 24-bit.
    neiro::Dither dither{32};
    bool dithered = false;

    // Reported back to the UI. The one value that decides whether "bit-perfect"
    // may be said out loud, and beside it the name of the verdict, which decides
    // what the UI may say about it.
    bool bitPerfect = false;
    bool resampling = false;
    neiro::PerfectReason reason = neiro::PerfectReason::DeviceCannotCarryFormat;

    // Which share mode this stream was actually opened in.
    //
    // Stored rather than assumed, because `NAE_QuerySignalPath` used to answer
    // `exclusive = 1` unconditionally -- a hardcoded claim that happened to be
    // true while exclusive was the only mode there was. It became false the moment
    // shared existed, and nothing caught it except an instrument that opened a
    // shared stream and read the field back.
    bool exclusive = true;

    // 采样实际是怎么被解出来的。见 `NAE_QueryDecodePath`。
    //
    // 存在句柄上而不是每次去问解码器，是因为解码器在打开之后就可能被换掉（换曲、seek），
    // 而界面要显示的是**当前**这条路径。打开时存一份，之后只读。
    std::string decodePath;
    // The listener's setting did not choose this rate; the driver did. Kept apart
    // from `reason` because the interface has to say both things and they are
    // different: the reason says what happened to the samples, this says whose
    // decision it was.
    bool coercedToSupportedRate = false;

    // ---- telemetry ----
    //
    // Read by NAE_QuerySignalPath. Counted on the audio thread and only ever
    // incremented, so no lock is needed and the read is a snapshot that may be a
    // few callbacks stale -- which for "did this happen during the track" is the
    // only question being asked.
    uint64_t callbackCount = 0;
    uint64_t deadlineMissCount = 0;
    double peakCallbackMs = 0.0;
    uint64_t underrunCount = 0;
    uint64_t dropCount = 0;
    // Device frames the render loop had to invent because the fifo ran dry
    // mid-buffer. Non-zero means the decoder is not keeping up in real time, and
    // it is the number that turns "the output burbles" into a measured statement.
    uint64_t padFrameCount = 0;
    // Signalled by the audio engine when the next buffer period is due. Null when
    // the device refused `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, in which case the
    // render loop falls back to pacing itself with a high-resolution timer.
    HANDLE renderEvent = nullptr;
    // Worst wall-clock gap between two consecutive device-buffer writes, and the
    // device buffer's own duration in ms.
    //
    // This is the measurement that can see a missed deadline. Every other counter
    // here reports what the engine *did*, and a buffer written perfectly but late
    // is indistinguishable from one written on time. A gap larger than
    // `bufferDurationMs` means the device ran dry waiting, no matter how correct
    // the samples were -- which is the whole of "steady clicking at the buffer
    // rate", a fault that was invisible until this existed.
    double maxBufferGapMs = 0.0;

    /// Milliseconds the render thread spent inside topUp() in its worst period.
    ///
    /// Our render loop decodes from inside the device period; Twilight Echo's runs
    /// the decoder on its own thread. Whatever this costs is spent out of the
    /// runway the device gave us, so this is the number that has to be small.
    double maxDecodeStallMs = 0.0;

    /// Milliseconds from reading the padding to handing the buffer over, worst case.
    double maxFillMs = 0.0;

    /// Milliseconds by which the worst buffer missed its runway, and how often.
    ///
    /// Zero means every buffer reached the device before the device ran out of
    /// audio to play. A positive value is the device having played stale audio,
    /// which is what a bubble is.
    double maxLateMs = 0.0;
    int64_t lateCount = 0;
    double bufferDurationMs = 0.0;

    // Device frames actually delivered, per second of wall clock.
    //
    // Everything else here counts frames the engine *wrote*. This counts frames
    // the device was actually handed per second of real time, and it is the only
    // figure that can distinguish "playing at the right speed" from "playing
    // correctly and slowly". The fifo accounting balances either way.
    double deviceRateActual = 0.0;
    double rateWindowStart = 0.0;
    int64_t rateWindowFrames = 0;

    /// Nothing to hold here any more.
    ///
    /// This slot held the "discard the rate window on pause" flag, which was the
    /// second of two attempts to keep `deviceRateActual` from reporting a pause as
    /// playback at half rate. Both are gone; the reason is LESSONS.md 58 and the
    /// working definition is in the field's own documentation. Nothing is kept here
    /// because a field whose only content is "this used to be wrong" is a field
    /// nobody will read twice.
    /// Set once the first 3 s of the stream have elapsed and been discarded.
    bool rateWarmupDone = false;

    /// Device frames still to be faded in, after a start.
    ///
    /// A resume begins from silence -- the prefill fills the device buffer with
    /// zeros -- so a stream that starts at a non-zero phase steps out of silence and
    /// clicks. The ramp is applied to the first few milliseconds of real audio.
    int64_t fadeInRemaining = 0;
    int64_t fadeInTotal = 0;
    int64_t fadeInFrame = 0;

    /// Channel trims (dB), balance (-1..1), polarity mask, and per-channel delay
    /// in device frames. Applied after EQ/crossfeed, before volume.
    double channelTrimDb[NAE_MAX_CHANNELS]{};
    double balance = 0.0;
    uint32_t polarityMask = 0;
    std::vector<int32_t> channelDelay;  // device frames per channel
    std::vector<size_t> channelDelayPos;
    std::vector<float> channelDelayBuf;
    uint32_t channelDelayMax = 0;
    /// Clip guard. Configured by the session policy; inactive by default.
    neiro::ClipGuard clipGuard;
    /// Digital volume, linear 0..1, ramped per device frame.
    ///
    /// 1.0 is exact bypass: the render loop does not multiply at all, so a
    /// bit-perfect stream stays bit-perfect. Any other value is reported as
    /// NAE_REASON_VOLUME_NOT_UNITY.
    double volume = 1.0;
    double volumeTarget = 1.0;
    double volumeRampDelta = 0.0;
    int64_t volumeRampRemaining = 0;

    /// Equaliser cascade. Coefficients are recomputed in NAE_SetEq; the render
    /// loop only runs them. `eqZ*` are transposed direct-form-II state, one pair
    /// per band per channel.
    bool eqEnabled = false;
    double eqPreamp = 1.0;
    int32_t eqBandCount = 0;
    bool eqBandEnabled[NAE_EQ_MAX_BANDS]{};
    uint32_t eqBandChannelMask[NAE_EQ_MAX_BANDS]{};
    bool eqLinear = false;
    neiro::LinearPhaseEq linearEq;
    neiro::EqBiquad eqCoeffs[NAE_EQ_MAX_BANDS]{};
    std::vector<double> eqZ1;
    std::vector<double> eqZ2;
    NAE_EqParams eqParams{};

    /// Stereo crossfeed: a circular delay line of two channels, a one-pole
    /// low-pass per side, and the mix amount. Only used when the stream is 2ch.
    bool crossfeedEnabled = false;
    double crossfeedAmount = 0.3;
    double crossfeedDelayMs = 0.3;
    double crossfeedCutoffHz = 700.0;
    std::vector<double> crossfeedDelay;
    size_t crossfeedDelayFrames = 0;
    size_t crossfeedPos = 0;
    double crossfeedLpL = 0.0;
    double crossfeedLpR = 0.0;
    double crossfeedAlpha = 0.0;
    NAE_CrossfeedParams crossfeedParams{};

    /// FIR convolver. The IR is applied in the source domain, before the
    /// resampler, so it must be at the source rate.
    NAE_ConvolverParams convParams{};
    bool convEnabled = false;
    size_t convTaps = 0;
    size_t convChannels = 0;
    neiro::PartitionedConvolver conv;
    std::vector<float> convDryL;
    std::vector<float> convDryR;
    /// Error text for the render loop, reused so no string is constructed per
    /// device buffer.
    std::string renderErr;

    /// Render-loop scratch, sized once per channel count.
    std::vector<double> frameHeld;
    std::vector<double> frameOut;
    std::vector<float> frameFloat;

    std::vector<float> convInBuf;
    std::vector<float> convDryOut;
    /// The dry-path delay ring: kConvLatency frames x ch, one block.
    std::vector<float> convDryBuf;
    size_t convDryPos = 0;
    double convWet = 1.0;
    double convDry = 0.0;
    double convGain = 1.0;

    /// Crossfade: the next file's first frames are mixed into the current end.
    NAE_CrossfadeParams crossfadeParams{};
    bool crossfadeEnabled = false;
    double crossfadeDurationMs = 0.0;
    int32_t crossfadeCurve = NAE_CROSSFADE_EQUAL_POWER;
    int64_t crossfadeFrames = 0;
    bool crossfadeActive = false;
    std::vector<float> crossfadeBuf;
    size_t crossfadeBufPos = 0;
    size_t crossfadeBufFrames = 0;
    int64_t crossfadeConsumed = 0;

    /// ReplayGain policy and the gain it resolves to. `rgEnabled` is the mode;
    /// the multiply is skipped when `rgGain` is exactly 1.0.
    NAE_ReplayGainInfo rgInfo{};
    NAE_ReplayGainParams rgParams{};
    bool rgEnabled = false;
    double rgGain = 1.0;
    double rgTarget = 1.0;
    double rgDelta = 0.0;
    int64_t rgRamp = 0;

    /// Whether this handle has ever started a stream.
    ///
    /// Distinguishes "the beginning of a track", where silence before the first
    /// sample is right, from "a resume", where it is a 26.67 ms hole in the middle
    /// of the music. Both go through the same Start call.
    bool streamEverStarted = false;

    /// Whether the render loop should hand the device silence instead of audio.
    ///
    /// This is what makes a pause possible at all. Stopping the client and starting
    /// it again is audible on this endpoint -- measured, see NAE_Pause in nae.h --
    /// so a pause keeps the stream running and feeds it zeros. There is then no
    /// boundary for the device to react to, which is the entire point.
    bool silencing = false;

    /// Device frames handed over as silence, reported as `silenceFramesWritten`.
    int64_t silenceFramesWritten = 0;

    // ---- output spectrum tap ----
    //
    // A ring of the most recent `spectrumRing.size()` channel-0 device frames,
    // filled from inside the render loop and read by NAE_QuerySpectrum.
    //
    // The render loop's only cost here is copying one double per frame; the FFT
    // happens on whichever thread calls the query. That split matters, because the
    // render loop's entire budget is the runway the device gave it and a 4096-point
    // transform inside a 5.7 ms period is a deadline miss waiting to happen.
    //
    // `spectrumLock` is the one lock in the handle. Everything else on this struct
    // is either written-only by the render thread or atomically readable, which is
    // possible because those are all monotonic counters. A ring buffer is neither:
    // the reader has to see a consistent window, so it needs the writer held off.
    std::mutex spectrumLock;
    std::vector<double> spectrumRing;
    size_t spectrumWrite = 0;
    size_t spectrumFilled = 0;
    uint64_t spectrumFramesFed = 0;
    /// Rate the ring was filled at. Recorded rather than read from the signal path
    /// because the tap is filled before any conversion to the device's own format,
    /// and after a rate change a window holding samples from two rates would be a
    /// measurement of nothing.
    uint32_t spectrumRate = 0;

    // ---- 时域包络（节拍检测用）----
    //
    // 与频谱共用同一个 `spectrumRing`。环形缓冲里本来就是时域样本，频谱是读它时
    // 做的变换，所以这里**不复制一份**——第二份环形缓冲意味着两处状态要保持同步，
    // 而它们唯一的区别只是聚合方式。
    //
    // 包络本身是一串聚合后的帧，环形缓冲满了就被覆盖，所以它记录的是环形缓冲的
    // 历史，而不是独立的长期存储。窗口长度因此是固定的（见 kEnvelopeFrames），
    // 这是有意的：BPM 检测需要的是最近几秒，不是整首歌。
    std::vector<float> envelope;
    size_t envelopeFill = 0;
    uint32_t envelopeRate = 0;
    // 环形缓冲被消费了多少次，用来判断包络是否已经稳定。
    uint64_t envelopeEpoch = 0;
    // 跨调用的余数：**攒够 `hop` 个样本才写一帧**。
    //
    // 原来是 `for (offset = 0; offset < frames; offset += hop)`——每一帧的边界都对齐到
    // **本次调用**的 0，而且末尾不足一帧的那一段**照样写成一帧**。调用越碎，写出来的帧
    // 就越短，而 `frame_hop_ms` 一直报 5 ms。实测（`probe-tempo-window`，三次独立运行）：
    // 包络的真实帧率是**每秒 358 ~ 421 帧**，而 5 ms 的意思是 200 帧。
    //
    // 节拍估计器的时间基就是这个 5 ms，所以它算出来的每个 BPM 都带着一个 1.8 ~ 2.1 倍的、
    // 而且还在飘的偏差——界面上「同一段音频、同一个窗给出 103 ~ 197」就是这么来的。
    //
    // 改成跨调用累加之后，一帧的定义回到它该有的样子：**连续的 `hop` 个样本**。
    double envelopeAccum = 0.0;
    size_t envelopeAccumN = 0;

    uint64_t deviceLostCount = 0;
    uint64_t recoveryCount = 0;

    // Measured levels, for "the sound is too quiet, where did it go". See the
    // matching block in nae.h for why these are two numbers and not one.
    std::atomic<float> fifoPeak{0.0f};
    std::atomic<float> writtenPeak{0.0f};
    // Peak read back out of the device's own buffer, interpreted as the device
    // will interpret it. writtenPeak is computed before packing and cannot see
    // a wrong shift or a wrong offset; this can.
    std::atomic<float> devicePeak{0.0f};

    // What was asked for, and what the device handed back. `Initialize` writes
    // into the WAVEFORMATEX it was given, so a difference here is the device
    // changing the depth under us -- and the one case where writing the requested
    // depth would be writing the wrong number of bytes per sample.
    uint16_t requestedBits = 0;
    uint16_t grantedBits = 0;

    // Per-call pipeline accounting, for the last `NAE_Play`.
    //
    // This is the same data `NAE_TRACE` prints, exposed so a benchmark can read
    // it instead of scraping a log. The numbers exist because the alternative is
    // reasoning about a playback-rate bug, and reasoning is what produced four
    // wrong conclusions in a row before the counters were added.
    //
    // The invariant worth checking first: `sourceAdvance * deviceRate /
    // sourceRate` must equal `deviceWritten`. When it does not, the device is
    // being fed frames the source never paid for.
    double lastSourceAdvance = 0.0;
    double lastDeviceWritten = 0.0;
    double lastFifoFrames = 0.0;
    double lastProduced = 0.0;
    double lastConsumed = 0.0;
    double lastShortRender = 0.0;

    /// Whether IAudioClient is currently started.
    ///
    /// The bubbling, and the reason this flag exists.
    ///
    /// NAE_Play is a *slice* call: the host calls it every 200 ms
    /// (the Tauri host's SLICE_MS, since removed) and each call was doing
    /// prefill-with-zeros, Start, render, Stop. The zero prefill is a full-scale
    /// discontinuity in a continuous waveform, and it happened five times a
    /// second -- which is exactly the periodic click that was reported as
    /// "bubbling" and could not be found for a whole day.
    ///
    /// Nothing inside a single call could see it. Every counter read healthy:
    /// worstLate 0, padFrameCount 0, worstFill 0.4 ms against a 53 ms runway, and
    /// splice_check found no discontinuity across all 14M decoded frames. Each
    /// call was individually perfect; the fault was in the seam between calls,
    /// and every instrument in this engine measures within one call.
    ///
    /// It was also invisible to the obvious experiment. Deepening the fifo
    /// (NEIRO_FIFO_OVERFEED=4) changed nothing and left deviceRateActual at the
    /// same 98.5%, because what was lost was not audio we failed to supply --
    /// it was audio the device was told to discard.
    bool clientRunning = false;

    /// How many times the client has been stopped and restarted.
    ///
    /// This is the number that would have found it. It is exposed rather than
    /// kept private because a counter that cannot be read cannot accuse anything,
    /// and the whole reason this took a day is that the restart count was
    /// structurally invisible.
    uint64_t clientRestartCount = 0;

    /// The last sample of the previous written buffer, for the seam measurement.
    double seamTail = 0.0;
    bool haveSeamTail = false;

    /// Largest step the signal takes between adjacent samples, normalised.
    ///
    /// The reference the seam is judged against. A waveform that simply continued
    /// has a seam step the same order as this; a break is several times larger.
    double slewRef = 0.0;

    /// Worst seam step over the peak, and the slew it should be compared with.
    double worstSeamRatio = 0.0;
    // The distribution of that ratio, not just its maximum.
    //
    // A maximum with no distribution cannot be read. The recorded values across
    // sessions were 0.95, 1.04, 2.63, 3.46 and 11.68, and nothing said whether a
    // worst-case of 11.68 is a real boundary artefact or the largest legitimate
    // step in a particular passage -- and the difference decides whether the seam
    // needs fixing at all. A maximum answers "has this ever happened"; a
    // distribution answers "how often, and is this seam worse than the others".
    //
    // Percentiles rather than a histogram because the question is about ordering,
    // not about shape, and a fixed set of quantiles is comparable between runs
    // without a bin-width argument. The buckets below are logarithmic because the
    // ratio spans two orders of magnitude between a clean stream and a broken one.
    //
    // `seamCount` is the sample count for these quantiles, and it is already
    // reported -- so a percentile can always be checked against how many seams it
    // was computed from.
    double seamQuantiles[kSeamQuantiles] = {};
    uint64_t seamHistogram[kSeamBuckets] = {};
    uint64_t seamRatioTotal = 0.0;  // fixed point, thousandths, to keep it integral
    // Every seam's ratio, kept only if the count is small enough to be useful. The
    // point is to have the raw numbers when something looks wrong, and a cap keeps
    // that bounded: 4096 seams is 35 minutes at this buffer size, which is long
    // enough to catch a periodic artefact and short enough not to be a leak.
    float seamSample[kSeamSamples];
    size_t seamSampleCount = 0;
    double worstSeamSlew = 0.0;
    uint64_t seamCount = 0;

    // ---- stream state ----
    //
    // These live on the handle, not in the play function, and that placement is
    // load-bearing. The host renders in ~200 ms slices, each a separate
    // NAE_PlaySine call on the same handle. When this state was local, every
    // slice began with an empty fifo, sourcePos at 0 and the tone's phase at 0 —
    // so a 1 kHz test tone restarted every 200 ms, which puts a phase
    // discontinuity (a click) at 5 Hz, and real audio would have replayed from
    // the top of the file five times a second.
    //
    // A handle is one playback session. "Play for 200 ms more" is the correct
    // meaning of the call, and these are what make it mean that.
    std::vector<float> fifo;
    int64_t sourcePos = 0;
    int64_t phase = 0;

    // Where `sourcePos` was zeroed, in source frames.
    //
    // A seek resets `sourcePos` and then the decoder picks up from the new place,
    // so `sourcePos` counts frames played *since the seek*, not from the start of
    // the file. Reporting that directly would put a progress bar back at zero
    // after a jump to 3:20 and let it crawl forward from there while the music is
    // already three minutes in. Adding the base makes position mean "where in the
    // track the listener is", which is the only thing the UI wants to show.
    int64_t seekBaseFrames = 0;

    // ---- file source ----
    //
    // Present when playback was opened on a file rather than a tone. Null means
    // the test tone is the source, and only then is the synthesis path taken.
    //
    // The decoder is owned here so its FFmpeg types never reach the C ABI, for
    // the same reason the resampler is: a caller that had to know about an
    // AVFrame could not survive an FFmpeg upgrade.
#ifdef NEIRO_HAVE_FFMPEG
    std::unique_ptr<neiro::Decoder> decoder;

    // Decode-ahead. The producer owns the decoder and fills `decodeRing`; the
    // render thread only consumes source frames. The producer never touches the
    // device, and the render thread never calls readExact while it is running.
    std::mutex decodeMutex;
    std::condition_variable decodeCv;
    std::deque<float> decodeRing;
    bool decodeEof = false;
    bool decodeQuit = false;
    bool decodeRunning = false;
    std::thread decodeThread;

    // The file's channel count before the engine's downmix, kept so the UI can
    // report what the user's file actually is.
    uint32_t sourceChannels = 0;
    // Optional source->device routing. When active, the decoder keeps the file's
    // channel count and topUp routes before the resampler/fifo.
    neiro::RoutingProcessor routing;
    bool routingActive = false;
    std::vector<float> routingIn;
    std::vector<float> routingOut;
    // The file's true length in frames. 0 for a tone, where there is no end.
    int64_t totalSourceFrames = 0;

    // 文件自己的信息，播放期间不变。
    //
    // 之前只有 `sourceChannels` 和时长被 open 的 out-param 带出去，其余全部在
    // open 时丢掉——于是 `track` 事件只带 channels 和 durationMs，本地文件的
    // fact strip 少了九个值。
    //
    // 而这些字段本来就在手上：`decoder->info()` 有采样率、位深、编码、声道。所以
    // 这不是「数据不存在」，是**没有人把它带上来**。存在而没上报，和不存在一样让
    // 界面只能说不知道——而后者会让人以为播放器没读那个字段。
    int32_t fileSampleRate = 0;
    /// 文件的位深，位/采样。0 表示容器没有报。
    ///
    /// 这是**容器声称的**，不是引擎实际用的：解码器总是产出 float32（见 nae.h
    /// 里 `bitPerfect` 那一段的说明）。所以它描述用户的音乐，不描述信号通路——
    /// 两者必须分开报。混在一起就会把「文件是 24 bit」说成「设备收到 24 bit」，
    /// 而这个引擎对那个说法有专门的、更严格的定义。
    int32_t fileBitsPerSample = 0;
    /// Bits the source actually carries (`StreamInfo::raw_bits_per_sample`).
    ///
    /// Separate from `fileBitsPerSample` above because that one is the container
    /// the decoder produces: a 24-bit FLAC and a 32-bit WAV both report 32 there.
    /// This is the number that decides whether the float32 path can carry the
    /// file without dropping bits. 0 means the demuxer did not say.
    int32_t fileRawBits = 0;
    /// The open-time verdict, before the source's depth is considered.
    ///
    /// `applySourcePrecision` only clears, so a gapless swap has to start from
    /// this rather than from the previous track's verdict.
    bool sourceBitPerfect = false;
    neiro::PerfectReason sourceReason = neiro::PerfectReason::DeviceCannotCarryFormat;
    /// 文件时长，毫秒。0 表示容器没有报。
    ///
    /// 之前时长只经由 open 的 out-param 传出去，而那个值只被**打开时**调用 open
    /// 的那一方读到——界面上过了几秒再问就问不到了。句柄上留一份是为了让
    /// `NAE_QueryTrackInfo` 在播放期间任何时刻都能回答。
    int32_t fileDurationMs = 0;
    /// 容器或编码名，如 "flac" / "aac"。空表示没有。
    ///
    /// 固定长度内嵌数组而不是 std::string：这个值要经过 C ABI，而 std::string 的
    /// 布局不是 ABI。它也不需要释放——与 open 的 out-param 里那个 `char*` 刻意
    /// 不同，那个由引擎分配、调用方 `NAE_FreeString` 释放。两处都像对方管内存是
    /// 一种很难查的错，所以做法必须明显不同。
    char fileCodec[32] = {};

    // The next file, opened ahead of time. Same source rate and channel count
    // only: the device stream and the resampler are not rebuilt for it.
    std::unique_ptr<neiro::Decoder> nextDecoder;
    int64_t nextTotalSourceFrames = 0;
    int32_t nextFileChannels = 0;
    int32_t nextFileSampleRate = 0;
    int32_t nextFileBitsPerSample = 0;
    int32_t nextFileRawBits = 0;
    int32_t nextFileDurationMs = 0;
    char nextFileCodec[32] = {};
    std::string nextDecodePath;
    NAE_ReplayGainInfo nextRgInfo{};
    std::vector<float> nextPrefetched;
    size_t nextPrefetchedFrames = 0;
    std::vector<float> prefetched;
    size_t prefetchedPos = 0;
    size_t prefetchedFrames = 0;

    // Set when the decoder ran out, so a short final block plays as silence
    // rather than as a truncated or repeated tail.
    bool sourceExhausted = false;
    // Set once the fifo has hit its ceiling and production has been stopped.
    bool fifoCapped = false;
#endif

    /// Increments when a preloaded file takes over. A tone stays at 0.
    uint64_t trackSerial = 0;
    /// Set by NAE_SkipToNext, consumed by the next render slice.
    ///
    /// The switch has to happen where the fifo and the resampler are owned -- on
    /// the render thread, between slices -- so the request is a flag rather than
    /// a swap performed by the caller.
    std::atomic<bool> skipRequested{false};

    // ---- DoP ----
    //
    // DoP is a transport, not a decoder: raw DSD bits are packed into 24-bit
    // carrier words and written straight to the device. NAE_Play and the whole
    // float DSP chain are bypassed; volume/EQ would corrupt the marker.
    bool dopMode = false;
    std::unique_ptr<dsf::File> dopDsf;
    std::unique_ptr<dff::File> dopDff;
    uint64_t dopBlock = 0;
    uint32_t dopBlockFrames = 0;
    uint32_t dopFrameInBlock = 0;
    std::vector<uint8_t> dopRaw;
    size_t dopRawPos = 0;
    size_t dopRawBytes = 0;
    uint64_t dopDataOffset = 0;
    uint32_t dopCarrierRate = 0;
    int32_t dopStreamRate = 0;
    uint8_t dopMarker = 0x05;
    int64_t dopFramesPlayed = 0;
    int64_t dopFramesTotal = 0;
    /// Host-owned native DSD sink. `nativeWrite` is called from
    /// NAE_PlayNativeDsd with interleaved 1-bit bytes.
    bool nativeSinkMode = false;
    NAE_DsdWriteFn nativeWrite = nullptr;
    void* nativeContext = nullptr;
    bool nativePaused = false;
    bool nativeEnded = false;
    uint32_t nativeRate = 0;
    uint32_t nativeChannels = 0;
    int32_t nativeBitOrderMsbFirst = 0;
    double nativeDurationMs = 0.0;
    std::unique_ptr<neiro::dsd::Downrate> nativeDownrate;
    std::vector<uint8_t> nativeIn;
    std::vector<uint8_t> nativeOut;

    /// Bitstream downrate (null when the source rate is played as is).
    std::unique_ptr<neiro::dsd::Downrate> dopDownrate;
    std::vector<uint8_t> dopDownOut;
    std::vector<uint8_t> dopDownIn;
    /// Read index into dopDownOut. Erasing from the front once per DoP frame
    /// was O(n) per frame and measured 15 ms of memmove per second of audio;
    /// the buffer is compacted only when the read index passes half of it.
    size_t dopDownRead = 0;
    uint32_t dopPairChannel = 0;
};

#ifdef NEIRO_HAVE_FFMPEG
// Decode-ahead producer.
//
// The producer owns `decoder`; it never touches the device. It fills a source
// ring to one second, then waits. The render thread consumes blocks through
// readDecodedFrames and still owns resampling, gapless/crossfade and the DSP.
// This is deliberately only the decode move: the source-side switch logic is
// unchanged, and the render loop keeps working if the thread cannot start.
static void decodeThreadMain(NAE_Handle* handle) {
    const size_t channels = handle->fmt.Format.nChannels ? handle->fmt.Format.nChannels : 1;
    const size_t targetFrames =
        std::max<size_t>(8192, static_cast<size_t>(handle->sourceRate));
    std::vector<float> block;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(handle->decodeMutex);
            if (handle->decodeQuit) break;
            if (handle->decodeRing.size() / channels >= targetFrames) {
                handle->decodeCv.wait(lock, [handle, channels, targetFrames] {
                    return handle->decodeQuit ||
                           handle->decodeRing.size() / channels < targetFrames;
                });
                continue;
            }
        }
        if (!handle->decoder) {
            std::lock_guard<std::mutex> lock(handle->decodeMutex);
            handle->decodeEof = true;
            handle->decodeCv.notify_all();
            break;
        }
        block.clear();
        std::string error;
        const bool more = handle->decoder->readExact(&block, 4096, &error);
        {
            std::lock_guard<std::mutex> lock(handle->decodeMutex);
            if (!block.empty()) {
                handle->decodeRing.insert(handle->decodeRing.end(), block.begin(),
                                          block.end());
            }
            if (!more) handle->decodeEof = true;
        }
        handle->decodeCv.notify_all();
        if (!more) break;
        if (block.empty()) std::this_thread::yield();
    }
}

/// Downgrade an open-time "bit-perfect" verdict when the source carries more
/// precision than the float32 path keeps.
///
/// The device is negotiated long before the file is opened, so `fileRawBits` is
/// not known when the verdict is first computed. This runs once the decoder is
/// attached and only ever clears the flag.
///
/// Why 24: every stage between the decoder and the device is float32, which has
/// 24 significant bits. A 24-bit source survives exactly; a 32-bit integer
/// source loses its low eight bits on the way in and never gets them back,
/// whatever the endpoint accepts. The endpoint's valid-bit count is a different
/// question and answering it instead is how the flag came to claim perfection
/// for a file it had already truncated.
///
/// Conservative in the same direction as the rest of the flag: a float32 source
/// into a float32 endpoint would survive, but this path does not track whether
/// the source is float, and an unmeasured claim of perfection is the failure
/// this field exists to prevent.
/// Clamp a caller's declared struct size to what this build can write.
///
/// Zero means "this build's size" -- the convention every harness here uses.
/// Larger than the build's own size means the caller expects fields this DLL
/// does not have; writing them would be writing zeroes and calling it data, so
/// the limit stays at the build's size and `structSize` reports the build's
/// size rather than echoing a promise that was not kept.
static uint32_t declaredLimit(uint32_t declared, uint32_t full) {
    if (declared == 0 || declared > full) return full;
    if (declared < sizeof(uint32_t)) return static_cast<uint32_t>(sizeof(uint32_t));
    return declared;
}

/// Copies a local struct out with the caller's limit, on scope exit.
///
/// Used by the query exports so a host built against an older, smaller struct
/// cannot have its buffer overrun by the tail this build appended. The
/// destructor runs on every return path, which is why it is a guard rather than
/// a copy at the end.
///
/// `structSize` is set to the number of bytes actually written, which is what
/// `nae.h` promises: "Out: the size the engine actually wrote". Reporting the
/// build's full size while writing fewer bytes would tell the caller that fields
/// exist which were never filled.
template <typename T>
struct StructCommit {
    T* dst;
    const T* src;
    uint32_t limit;
    ~StructCommit() {
        std::memcpy(dst, src, limit);
        // The size field is the first member of every one of these structs.
        *reinterpret_cast<uint32_t*>(dst) = limit;
    }
};

static void applySourcePrecision(NAE_Handle* handle) {
    if (!handle || !handle->bitPerfect) return;
    if (handle->fileRawBits <= 24) return;
    handle->bitPerfect = false;
    handle->reason = static_cast<neiro::PerfectReason>(NAE_REASON_SOURCE_PRECISION);
}

static void startDecodeThread(NAE_Handle* handle) {
    if (!handle || !handle->decoder || handle->decodeRunning) return;
    if (handle->decodeThread.joinable()) handle->decodeThread.join();
    {
        std::lock_guard<std::mutex> lock(handle->decodeMutex);
        handle->decodeQuit = false;
        handle->decodeEof = false;
        handle->decodeRing.clear();
    }
    try {
        handle->decodeThread = std::thread(decodeThreadMain, handle);
        handle->decodeRunning = true;
    } catch (...) {
        handle->decodeRunning = false;
    }
}

static void stopDecodeThread(NAE_Handle* handle) {
    if (!handle) return;
    {
        std::lock_guard<std::mutex> lock(handle->decodeMutex);
        handle->decodeQuit = true;
        handle->decodeRing.clear();
        handle->decodeEof = false;
    }
    handle->decodeCv.notify_all();
    if (handle->decodeThread.joinable()) handle->decodeThread.join();
    handle->decodeRunning = false;
}

// Called from the render thread. Waits for `frames` source frames, EOF or stop;
// returns how many were available. The caller zero-pads a short result.
static size_t readDecodedFrames(NAE_Handle* handle, size_t frames,
                                std::vector<float>* out) {
    if (!handle || !out || frames == 0) return 0;
    const size_t channels = handle->fmt.Format.nChannels ? handle->fmt.Format.nChannels : 1;
    std::unique_lock<std::mutex> lock(handle->decodeMutex);
    handle->decodeCv.wait(lock, [handle, frames, channels] {
        return handle->decodeRing.size() / channels >= frames || handle->decodeEof ||
               handle->decodeQuit;
    });
    const size_t available = handle->decodeRing.size() / channels;
    const size_t take = std::min(frames, available);
    if (take > 0) {
        out->insert(out->end(), handle->decodeRing.begin(),
                    handle->decodeRing.begin() + static_cast<long>(take * channels));
        handle->decodeRing.erase(handle->decodeRing.begin(),
                                 handle->decodeRing.begin() + static_cast<long>(take * channels));
        handle->decodeCv.notify_all();
    }
    return take;
}

// The swap is deliberately at the source boundary, not at the file open: the
// fifo already holds the tail of the current file, so the next decoder's first
// frame follows it without touching the device stream.
static void swapNextDecoder(NAE_Handle* handle) {
    if (!handle || !handle->nextDecoder) return;
    stopDecodeThread(handle);
    handle->decoder = std::move(handle->nextDecoder);
    handle->totalSourceFrames = handle->nextTotalSourceFrames;
    handle->sourcePos = 0;
    handle->sourceExhausted = false;
    handle->fileChannels = handle->nextFileChannels;
    handle->fileSampleRate = handle->nextFileSampleRate;
    handle->fileBitsPerSample = handle->nextFileBitsPerSample;
    handle->fileRawBits = handle->nextFileRawBits;
    // Restore the open-time verdict before re-applying the source rule.
    //
    // `applySourcePrecision` only clears; it never sets. Inheriting the previous
    // track's verdict therefore made a wide file poison every later one: 32-bit
    // A followed by 24-bit B kept reporting `SOURCE_PRECISION` for B, which is a
    // clean 24-bit source on a path the engine called perfect at open. The
    // listener hears B correctly and the host reports it as truncated, forever,
    // for the rest of the queue.
    handle->bitPerfect = handle->sourceBitPerfect;
    handle->reason = handle->sourceReason;
    applySourcePrecision(handle);
    handle->fileDurationMs = handle->nextFileDurationMs;
    std::memcpy(handle->fileCodec, handle->nextFileCodec, sizeof(handle->fileCodec));
    handle->fileCodec[sizeof(handle->fileCodec) - 1] = '\0';
    handle->decodePath = handle->nextDecodePath;
    handle->rgInfo = handle->nextRgInfo;
    handle->prefetched = std::move(handle->nextPrefetched);
    handle->prefetchedPos = 0;
    handle->prefetchedFrames = handle->nextPrefetchedFrames;
    handle->nextPrefetchedFrames = 0;
    handle->nextTotalSourceFrames = 0;
    handle->nextFileChannels = 0;
    handle->nextFileSampleRate = 0;
    handle->nextFileBitsPerSample = 0;
    handle->nextFileRawBits = 0;
    handle->nextFileDurationMs = 0;
    std::memset(handle->nextFileCodec, 0, sizeof(handle->nextFileCodec));
    handle->nextDecodePath.clear();
    handle->nextRgInfo = {};
    ++handle->trackSerial;
    startDecodeThread(handle);
}
#endif

namespace {

}  // namespace

extern "C" {

const char* NAE_GetEngineCapabilities(void) NAE_NOEXCEPT {
    // 从 `kOpenableShareModes` 生成，不是手写的字符串。见那张表的注释：这里曾经
    // 硬编码了 `"[\"shared\",\"exclusive\"]"`，而引擎打不开 shared。硬编码的版本
    // 换成一个「正确」的字面值也不算修好——那还是凭空的声称，只是值变了；生成出来
    // 的版本和那张表同源，加模式只能通过加表项，而加表项要求同时接上开流路径。
    //
    // 每个模式**连同它的导出入口**一起报出来，`shareModes` 保持成字符串数组不变。
    // 报入口的理由：只报模式名的话，外部无法核对——`shared` 该对应
    // `NAE_OpenShared` 还是 `NAE_OpenFileShared`，从字符串里看不出来。把入口
    // 说出来，这条声明才**可被验证**而不是只能被相信。
    static const std::string json = [] {
        std::string modes;
        for (size_t i = 0; i < sizeof(kOpenableShareModes) / sizeof(kOpenableShareModes[0]); ++i) {
            if (i) modes += ',';
            modes += "{\"mode\":\"";
            modes += kOpenableShareModes[i].name;
            modes += "\",\"entryPoint\":\"";
            modes += kOpenableShareModes[i].entryPoint;
            modes += "\"}";
        }
        // The FFmpeg licence, read from the library that is actually linked
        // rather than restated from a document. This build has been GPL before
        // and will be LGPL later; a host deciding whether it may distribute
        // nae.dll needs the answer for the binary in front of it, not for the
        // one the README was written against.
        //
        // `avutil_license` reports the most restrictive licence the build
        // enables ("GPL v3", "LGPL version 2.1 or later", ...). It is compiled
        // out when FFmpeg is absent, so the field says "none" there.
#ifdef NEIRO_HAVE_FFMPEG
        const char* ffmpegLicense = avutil_license();
#else
        const char* ffmpegLicense = "none";
#endif
        std::string json = std::string("{\"backend\":\"wasapi\",\"shareModes\":[") + modes +
                           "],\"rawStream\":false,\"ffmpegLicense\":\"" +
                           (ffmpegLicense ? ffmpegLicense : "unknown") +
                           "\",\"version\":\"0.1.0-spike\"}";
        return json;
    }();
    return json.c_str();
}

const char* NAE_GetLastError(void) NAE_NOEXCEPT {
    static char buf[64];
    std::snprintf(buf, sizeof(buf), "hresult=0x%08X", static_cast<unsigned>(g_lastHr));
    return buf;
}

int32_t NAE_GetLastHresult(void) NAE_NOEXCEPT { return static_cast<int32_t>(g_lastHr); }

char* enumerateDevicesImpl();  // may throw; wrapped below

int32_t openExclusiveSineImpl(const char* deviceId, NAE_Handle** out, int32_t* sampleRate,
                              int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                              int32_t* isFloat, double* latencyMs);  // may throw; wrapped below

// The two exports below are the only ones that allocate, so they are the only
// ones that can fail on OOM. The impl functions are free to throw; the exports
// are noexcept, so they convert an allocation failure into an ordinary error
// code instead of letting it unwind into the host's frames.
char* NAE_EnumerateDevices(void) NAE_NOEXCEPT {
    try {
        return enumerateDevicesImpl();
    } catch (...) {
        return nullptr;
    }
}

char* enumerateDevicesImpl() {
    if (FAILED(ensureCom())) return nullptr;

    std::string json = "[";

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&enumerator))) &&
        enumerator) {
        ComPtr<IMMDeviceCollection> collection;
        if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) &&
            collection) {
            const UINT count = neiro::wasapi::collectionCount(collection.Get());
            bool first = true;
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IMMDevice> device;
                if (FAILED(collection->Item(i, &device)) || !device) continue;

                DeviceInfo info;

                LPWSTR rawId = nullptr;
                if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
                    int needed = WideCharToMultiByte(CP_UTF8, 0, rawId, -1, nullptr, 0, nullptr, nullptr);
                    if (needed > 1) {
                        std::vector<char> buf(static_cast<size_t>(needed));
                        WideCharToMultiByte(CP_UTF8, 0, rawId, -1, buf.data(), needed, nullptr, nullptr);
                        info.id.assign(buf.data());
                    }
                    CoTaskMemFree(rawId);
                }

                ComPtr<IPropertyStore> store;
                if (SUCCEEDED(neiro::wasapi::openPropertyStore(device.Get(), &store)) && store) {
                    PROPVARIANT name;
                    PropVariantInit(&name);
                    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) &&
                        name.vt == VT_LPWSTR && name.pwszVal) {
                        int needed = WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, nullptr, 0,
                                                          nullptr, nullptr);
                        if (needed > 1) {
                            std::vector<char> buf(static_cast<size_t>(needed));
                            WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, buf.data(), needed,
                                                nullptr, nullptr);
                            info.name.assign(buf.data());
                        }
                    }
                    PropVariantClear(&name);
                }

                WAVEFORMATEX native{};
                uint16_t nativeValidBits = 0;
                if (readNativeFormat(device.Get(), &native, &nativeValidBits)) {
                    info.hasNativeFormat = true;
                    info.sampleRate = native.nSamplesPerSec;
                    info.bitsPerSample = native.wBitsPerSample;
                    // Report the bits that actually carry audio, not the
                    // container size. A 24-in-32 endpoint shows as 24 here,
                    // which is what the user cares about.
                    info.validBits = nativeValidBits ? nativeValidBits : native.wBitsPerSample;
                    info.channels = native.nChannels;
                    info.exclusiveOpens = tryInitialize(device.Get(), true, native, nativeValidBits, nullptr);
                }
                info.sharedOpens = tryInitialize(device.Get(), false, native, nativeValidBits, nullptr);

                if (!first) json += ",";
                first = false;
                char flags[160];
                std::snprintf(flags, sizeof(flags),
                              "\"sampleRate\":%u,\"bitsPerSample\":%u,\"validBits\":%u,\"channels\":%u,"
                              "\"exclusive\":%s,\"shared\":%s",
                              info.sampleRate, info.bitsPerSample, info.validBits, info.channels,
                              info.exclusiveOpens ? "true" : "false",
                              info.sharedOpens ? "true" : "false");
                json += "{\"id\":\"" + jsonEscape(info.id) + "\",\"name\":\"" +
                        jsonEscape(info.name) + "\"," + flags + "}";
            }
        }
    }

    json += "]";

    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
}

void NAE_FreeString(char* s) NAE_NOEXCEPT { std::free(s); }

// The share mode a stream is opened in.
//
// It is a parameter rather than a constant because there are now two entry
// points and the two differ in exactly this one thing. Everything else -- the
// negotiation, the rate choice, the decoder, the render loop -- is shared, so
// threading a mode through is the whole of the change and there is no second
// implementation to drift.
//
// The mode also decides what may be *claimed*: a shared stream goes through the
// Windows mixer, so `bitPerfect` is forced false with `PerfectReason::SharedMode`
// whatever the rate negotiation concluded. A shared stream that happens to run at
// the source rate is still not bit-perfect, and reporting it as such is the exact
// claim this project does not make.
int32_t openExclusiveImpl(const char* deviceId, int32_t sourceRate, int32_t outputMode,
                            int32_t fixedRate, int32_t quality, AUDCLNT_SHAREMODE shareMode,
                            NAE_Handle** out,
                            int32_t* outSampleRate, int32_t* outSourceRate,
                            int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                            int32_t* isFloat, int32_t* resampling, int32_t* dithered,
                            int32_t* bitPerfect, double* latencyMs,
                            int32_t bufferMode, uint32_t bufferMs);  // may throw

// The original single-purpose entry point, kept so the harnesses keep
// working. Equivalent to NAE_OpenExclusive with sourceRate 0, meaning
// "whatever the endpoint's native rate is" — see openExclusiveImpl.
int32_t openExclusiveSineImpl(const char* deviceId, NAE_Handle** out, int32_t* sampleRate,
                              int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                              int32_t* isFloat, double* latencyMs) {
    return openExclusiveImpl(deviceId, /*sourceRate=*/0, NAE_OUTPUT_DEVICE,
                            /*fixedRate=*/0, NAE_QUALITY_HIGH, AUDCLNT_SHAREMODE_EXCLUSIVE,
                            out, sampleRate, nullptr,
                            channels, bitsPerSample, validBits, isFloat, nullptr,
                            nullptr, nullptr, latencyMs, NAE_BUFFER_AUTO, 0);
}

int32_t NAE_OpenExclusiveSine(const char* deviceId, NAE_Handle** out, int32_t* sampleRate,
                              int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                              int32_t* isFloat, double* latencyMs) NAE_NOEXCEPT {
    try {
        return openExclusiveSineImpl(deviceId, out, sampleRate, channels, bitsPerSample,
                                     validBits, isFloat, latencyMs);
    } catch (...) {
        // OOM while building the handle. Report it as an ordinary error
        // instead of letting the exception unwind into the host.
        if (out) *out = nullptr;
        return NAE_ERR_NO_MEMORY;
    }
}

// ---------------------------------------------------------------------------
// File playback.
//
// A separate entry point rather than an extra argument on NAE_OpenExclusive,
// for one reason: the device must be configured for the *file's* rate, and that
// rate is only known after the file has been read. Deciding it in two steps --
// probe, then open -- would let a caller pass a rate that disagrees with the
// file, and the bit-perfect flag would then describe a stream that was never
// what was on disk.
// ---------------------------------------------------------------------------

#ifdef NEIRO_HAVE_FFMPEG
int32_t openFileImpl(const char* deviceId, const char* path, int32_t outputMode,
                     int32_t fixedRate, int32_t quality, AUDCLNT_SHAREMODE shareMode,
                     NAE_Handle** out,
                     int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                     int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                     int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                     int32_t* fileChannels, double* durationMs, double* latencyMs,
                     const NAE_RoutingMatrix* routingMatrix,
                     int32_t bufferMode, uint32_t bufferMs);  // may throw

int32_t NAE_OpenFile(const char* deviceId, const char* path, int32_t outputMode,
                     int32_t fixedRate, int32_t quality, NAE_Handle** out,
                     int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                     int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                     int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                     int32_t* fileChannels, double* durationMs, double* latencyMs) NAE_NOEXCEPT {
    try {
        return openFileImpl(deviceId, path, outputMode, fixedRate, quality,
                            AUDCLNT_SHAREMODE_EXCLUSIVE, out, sampleRate, sourceRateOut,
                            channels, bitsPerSample, validBits, isFloat, resampling, dithered,
                            bitPerfect, fileChannels, durationMs, latencyMs, nullptr,
                            NAE_BUFFER_AUTO, 0);
    } catch (...) {
        if (out) *out = nullptr;
        return NAE_ERR_NO_MEMORY;
    }
}

int32_t NAE_OpenFileShared(const char* deviceId, const char* path, int32_t outputMode,
                           int32_t fixedRate, int32_t quality, NAE_Handle** out,
                           int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                           int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                           int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                           int32_t* fileChannels, double* durationMs,
                           double* latencyMs) NAE_NOEXCEPT {
    try {
        return openFileImpl(deviceId, path, outputMode, fixedRate, quality,
                            AUDCLNT_SHAREMODE_SHARED, out, sampleRate, sourceRateOut,
                            channels, bitsPerSample, validBits, isFloat, resampling, dithered,
                            bitPerfect, fileChannels, durationMs, latencyMs, nullptr,
                            NAE_BUFFER_AUTO, 0);
    } catch (...) {
        if (out) *out = nullptr;
        return NAE_ERR_NO_MEMORY;
    }
}

int32_t NAE_OpenFileWithBuffer(const char* deviceId, const char* path,
                               int32_t outputMode, int32_t fixedRate, int32_t quality,
                               int32_t bufferMode, uint32_t bufferMs, NAE_Handle** out,
                               int32_t* sampleRate, int32_t* sourceRateOut,
                               int32_t* channels, int32_t* bitsPerSample,
                               int32_t* validBits, int32_t* isFloat,
                               int32_t* resampling, int32_t* dithered,
                               int32_t* bitPerfect, int32_t* fileChannels,
                               double* durationMs, double* latencyMs) NAE_NOEXCEPT {
    try {
        return openFileImpl(deviceId, path, outputMode, fixedRate, quality,
                            AUDCLNT_SHAREMODE_EXCLUSIVE, out, sampleRate, sourceRateOut,
                            channels, bitsPerSample, validBits, isFloat, resampling, dithered,
                            bitPerfect, fileChannels, durationMs, latencyMs, nullptr,
                            bufferMode, bufferMs);
    } catch (...) {
        if (out) *out = nullptr;
        return NAE_ERR_NO_MEMORY;
    }
}

int32_t NAE_OpenFileWithRouting(const char* deviceId, const char* path,
                                int32_t outputMode, int32_t fixedRate, int32_t quality,
                                const NAE_RoutingMatrix* routing, NAE_Handle** out,
                                int32_t* sampleRate, int32_t* sourceRateOut,
                                int32_t* channels, int32_t* bitsPerSample,
                                int32_t* validBits, int32_t* isFloat,
                                int32_t* resampling, int32_t* dithered,
                                int32_t* bitPerfect, int32_t* fileChannels,
                                double* durationMs, double* latencyMs) NAE_NOEXCEPT {
    if (!routing) return NAE_ERR_INVALID_ARGUMENT;
    try {
        return openFileImpl(deviceId, path, outputMode, fixedRate, quality,
                            AUDCLNT_SHAREMODE_EXCLUSIVE, out, sampleRate, sourceRateOut,
                            channels, bitsPerSample, validBits, isFloat, resampling, dithered,
                            bitPerfect, fileChannels, durationMs, latencyMs, routing,
                            NAE_BUFFER_AUTO, 0);
    } catch (...) {
        if (out) *out = nullptr;
        return NAE_ERR_NO_MEMORY;
    }
}

// `NAE_OpenFile` and `NAE_OpenFileShared` are the same function with a different
// share mode, so there is one body and the mode is a parameter.
//
// The earlier shape -- one entry point, hardcoded exclusive -- is why the engine
// advertised a mode it could not open, and why the capability string had to be
// corrected by hand. Two entry points that both call one implementation cannot
// disagree about what the implementation does.
int32_t openFileImpl(const char* deviceId, const char* path, int32_t outputMode,
                     int32_t fixedRate, int32_t quality, AUDCLNT_SHAREMODE shareMode,
                     NAE_Handle** out,
                     int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                     int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                     int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                     int32_t* fileChannels, double* durationMs, double* latencyMs,
                     const NAE_RoutingMatrix* routingMatrix,
                     int32_t bufferMode, uint32_t bufferMs) {
    if (out) *out = nullptr;
    if (!path) {
        g_lastErrorText = "no file given";
        return NAE_ERR_INVALID_ARGUMENT;
    }

    auto dec = std::make_unique<neiro::Decoder>();
    std::string err;
    // Probe with the file's own channel count. The downmix to the device's count
    // is configured after the device is open, because until then there is no
    // device count to convert to.
    if (!dec->open(path, &err)) {
        g_lastErrorText = err;
        return NAE_ERR_CANNOT_OPEN;
    }

    const int32_t fileRate = static_cast<int32_t>(dec->info().sample_rate);
    if (fileRate <= 0) {
        g_lastErrorText = "file reports no sample rate";
        return NAE_ERR_CANNOT_OPEN;
    }

    const int32_t rc = openExclusiveImpl(deviceId, fileRate, outputMode, fixedRate, quality,
                                         shareMode, out,
                                         sampleRate, sourceRateOut, channels, bitsPerSample,
                                         validBits, isFloat, resampling, dithered, bitPerfect,
                                         latencyMs, bufferMode, bufferMs);
    if (rc != NAE_OK) return rc;

    NAE_Handle* handle = *out;

    // Re-open the decoder asking for the device's channel count. libswresample
    // does the conversion -- mono upmix, surround downmix -- using the layouts
    // and coefficients it already has, which is not something to hand-roll.
    const uint32_t deviceChannels = static_cast<uint32_t>(*channels);
    if (routingMatrix) {
        if (routingMatrix->sourceChannels != dec->info().channels ||
            routingMatrix->targetChannels != deviceChannels) {
            g_lastErrorText = "routing matrix source/target does not match the file/device";
            NAE_Close(handle);
            *out = nullptr;
            return NAE_ERR_INVALID_ARGUMENT;
        }
        // The processor needs the rate the device actually negotiated, not the
        // rate the caller put in the matrix: the caller may have filled it from
        // the endpoint's native format while the policy opened another rate.
        // Bass-management alpha and the IIR Hilbert's pole set both depend on
        // this number, and using the wrong one is inaudible until it is wrong.
        NAE_RoutingMatrix resolvedMatrix = *routingMatrix;
        resolvedMatrix.sampleRate = static_cast<double>(*sampleRate);
        std::string routeError;
        if (!handle->routing.configure(resolvedMatrix, 65536, &routeError)) {
            g_lastErrorText = "routing matrix: " + routeError;
            NAE_Close(handle);
            *out = nullptr;
            return NAE_ERR_INVALID_ARGUMENT;
        }
        handle->routingActive = true;
        handle->bitPerfect = false;
        handle->reason = static_cast<neiro::PerfectReason>(NAE_REASON_CHANNEL_CONVERTED);
        if (bitPerfect) *bitPerfect = 0;
    } else if (deviceChannels != dec->info().channels) {
        auto converted = std::make_unique<neiro::Decoder>();
        if (!converted->open(path, &err, deviceChannels)) {
            g_lastErrorText = "could not convert " + std::string(path) + " to " +
                              std::to_string(deviceChannels) + " channels: " + err;
            NAE_Close(handle);
            *out = nullptr;
            return NAE_ERR_CANNOT_OPEN;
        }
        dec = std::move(converted);
    }

    const auto& info = dec->info();
    handle->sourceChannels = info.channels;

    // A channel conversion is processing, and processing is the opposite of
    // bit-perfect.
    //
    // Everything else in the engine decides `bitPerfect` from the sample rate and
    // the dither, so a 5.1 file at 48 kHz played on a 48 kHz stereo device came
    // out claiming to be bit-perfect -- while libswresample had been folding six
    // channels into two. The rate and the dither flags were both telling the
    // truth; they were just not the whole truth.
    //
    // The badge in the UI is driven solely by this flag, so a wrong value here is
    // not a rounding error. It is the engine making a claim it cannot support.
    if (info.channels != deviceChannels) {
        handle->bitPerfect = false;
        // openExclusiveImpl already reported the rate-and-dither answer through
        // this out-param, and it cannot have known about the channel conversion.
        // Leaving it set would mean the handle and the caller's copy disagree,
        // and the caller's copy is what the UI is given.
        if (bitPerfect) *bitPerfect = 0;
    }
    // Length in frames, from the container's own duration. Half a second of slack
    // on the end: an estimate that is slightly long costs a moment of silence,
    // while one that is slightly short cuts the track off mid-phrase, and only
    // one of those is a defect a listener would call a bug.
    handle->totalSourceFrames =
        static_cast<int64_t>(info.duration_ms / 1000.0 * info.sample_rate) +
        static_cast<int64_t>(info.sample_rate) / 2;
    handle->decoder = std::move(dec);
    // 解码路径在**这里**记，而不是在 `openExclusiveImpl` 里——那一处在 `NAE_OpenFile`
    // 之外，`handle->decoder` 还没挂上，于是读到的是空。
    //
    // 第一版就犯了这个：源速率正确（176400，证明后置抽取确实跑了）而路径是空的。而空路径
    // 在界面上的含义是「这个信号没有自己的路径」——一个关于**信号**的陈述，所以这个错会
    // 被显示成事实，而不是被显示成故障。
    handle->decodePath.clear();
    handle->decodePath = info.decode_path;
    // Kept on the handle as well as returned: NAE_QuerySignalPath has to be able
    // to say what the *file* had, and the out-param is only read by the caller of
    // the open that happened minutes or hours ago.
    handle->fileChannels = static_cast<int32_t>(info.channels);
    handle->rgInfo.trackValid = info.rg_track_valid ? 1 : 0;
    handle->rgInfo.trackGainDb = info.rg_track_gain_db;
    handle->rgInfo.trackPeak = info.rg_track_peak;
    handle->rgInfo.albumValid = info.rg_album_valid ? 1 : 0;
    handle->rgInfo.albumGainDb = info.rg_album_gain_db;
    handle->rgInfo.albumPeak = info.rg_album_peak;
    handle->rgInfo.structSize = sizeof(NAE_ReplayGainInfo);
    // 其余字段也一并留下，供 NAE_QueryTrackInfo 读取。见这些字段的注释：它们本来
    // 就在 `info` 里，之前只是没有被带上来。
    handle->fileSampleRate = static_cast<int32_t>(info.sample_rate);
    handle->fileBitsPerSample = static_cast<int32_t>(info.bits_per_sample);
    handle->fileRawBits = static_cast<int32_t>(info.raw_bits_per_sample);
    handle->fileDurationMs = static_cast<int32_t>(info.duration_ms);
    // 固定长度，截断而不溢出。见字段注释里关于不用 std::string 的理由。
    std::snprintf(handle->fileCodec, sizeof(handle->fileCodec), "%s", info.codec.c_str());
    applySourcePrecision(handle);
    if (!handle->routingActive) startDecodeThread(handle);

    // The caller's copy has to agree with the handle's.
    //
    // `openExclusiveImpl` filled `*bitPerfect` from the device-side verdict, and
    // that ran before the file was known. `applySourcePrecision` can only clear
    // it, so without this write-back a 32-bit source is reported bit-perfect
    // through the open out-param while `NAE_QuerySignalPath` on the same handle
    // says it is not -- two answers to one question, and the binding builds
    // `Stream::path.bit_perfect` from the out-param, so the wrong one is what
    // reaches a UI. The channel-conversion branch above already does this for
    // the same reason.
    if (bitPerfect) *bitPerfect = handle->bitPerfect ? 1 : 0;

    if (fileChannels) *fileChannels = static_cast<int32_t>(info.channels);
    if (durationMs) *durationMs = info.duration_ms;
    return NAE_OK;
}
#endif  // NEIRO_HAVE_FFMPEG

int32_t NAE_QueryDecodePath(NAE_Handle* handle, char* buf, int32_t bufLen) NAE_NOEXCEPT {
    // 缓冲区由调用方提供，引擎不分配——所以**只有一种所有权约定**，而两种约定并存的
    // 跨 ABI 接口迟早出现跨边界的 free。定长拷贝，截断而不溢出。
    if (!buf || bufLen <= 0) return NAE_ERR_INVALID_ARGUMENT;
    buf[0] = '\0';
    if (!handle) return NAE_OK;
    const std::string& src = handle->decodePath;
    const size_t n = std::min(static_cast<size_t>(bufLen - 1), src.size());
    if (n) std::memcpy(buf, src.data(), n);
    buf[n] = '\0';
    return NAE_OK;
}

int32_t NAE_QueryTags(NAE_Handle* handle, NAE_Tags* out) NAE_NOEXCEPT {
    if (!out) return NAE_ERR_INVALID_ARGUMENT;
    // Built in a local and copied out with the caller's limit.
    //
    // A host built against an older, smaller `NAE_Tags` declares a size this
    // build no longer matches, and the tail this build appended must not land in
    // that buffer. Reporting the declared size while writing past it -- which the
    // first version of this fix did -- is the same overrun with a friendlier
    // number on it. `NAE_QuerySignalPath` is the pattern: build a local, clamp
    // the copy to `min(declared, full)`.
    const uint32_t full = static_cast<uint32_t>(sizeof(NAE_Tags));
    NAE_Tags local{};
    local.structSize = full;
    const uint32_t limit = declaredLimit(out->structSize, full);
    StructCommit<NAE_Tags> commit{out, &local, limit};

    if (!handle) return NAE_OK;
#ifdef NEIRO_HAVE_FFMPEG
    if (!handle->decoder) return NAE_OK;
    const auto& info = handle->decoder->info();
    // 定长拷贝，截断而不溢出。见字段注释里关于「为什么不用 char*」的理由。
    const auto copyField = [](char* dst, size_t cap, const std::string& src) {
        const size_t n = std::min(cap - 1, src.size());
        std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
    };
    copyField(local.year, sizeof(local.year), info.year);
    copyField(local.genre, sizeof(local.genre), info.genre);
    copyField(local.album, sizeof(local.album), info.album);
    copyField(local.artist, sizeof(local.artist), info.artist);
    local.bitrateKbps = static_cast<uint32_t>(info.bitrate_kbps);
#endif
    return NAE_OK;
}

int32_t NAE_QueryTrackInfo(NAE_Handle* handle, NAE_TrackInfo* out) NAE_NOEXCEPT {
    if (!out) return NAE_ERR_INVALID_ARGUMENT;
    // Local plus a clamped copy, for the reason `NAE_QueryTags` gives: a
    // smaller declared size must not be overrun by the fields this build added.
    const uint32_t full = static_cast<uint32_t>(sizeof(NAE_TrackInfo));
    NAE_TrackInfo local{};
    local.structSize = full;
    const uint32_t limit = declaredLimit(out->structSize, full);
    StructCommit<NAE_TrackInfo> commit{out, &local, limit};

    if (!handle) return NAE_OK;

    std::lock_guard<std::mutex> guard(handle->spectrumLock);
    local.channels = handle->fileChannels;
    local.sampleRate = handle->fileSampleRate;
    local.bitsPerSample = handle->fileBitsPerSample;
    local.durationMs = handle->fileDurationMs;
    // 已经是一个以 NUL 结尾的定长缓冲，直接拷进同样定长的目标。不需要长度检查，
    // 也不需要释放——这正是选固定数组而不是 `char*` 的理由。
    std::memcpy(local.codec, handle->fileCodec, sizeof(local.codec));
    local.codec[sizeof(local.codec) - 1] = '\0';
    return NAE_OK;
}

int32_t NAE_Play(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT {
    if (handle && handle->dopMode) return NAE_ERR_INVALID_ARGUMENT;
    // The same render loop serves both sources; which one feeds it is decided per
    // block from whether the handle has a decoder. See the topUp comment.
    return NAE_PlaySine(handle, milliseconds);
}

int32_t NAE_Stop(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
    if (handle->nativeSinkMode) {
        handle->nativeEnded = true;
        return NAE_OK;
    }
    if (!handle->clientRunning) return NAE_OK;
    const HRESULT hr = neiro::wasapi::stopClient(handle->client.Get());
    if (FAILED(hr)) {
        if (deviceIsGone(hr)) {
            g_lastErrorText = "the output device was removed or is being used by another program";
            return NAE_ERR_DEVICE_LOST;
        }
        g_lastErrorText = "the output device refused to stop";
        return NAE_ERR_INITIALIZE;
    }
    handle->clientRunning = false;
    return NAE_OK;
}

int32_t NAE_Pause(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;

    // Idempotent, and safe before the stream has ever started.
    //
    // Nothing is stopped here, deliberately. That is the whole reason this function
    // exists: the Stop/Start pair is audible on this endpoint (100 cycles, one
    // artefact each; the same 100 with the stop removed were clean) and the samples
    // across it are continuous, so the device is the source. Keeping the client
    // running means the device never gets a boundary to react to.
    handle->silencing = true;
    if (handle->nativeSinkMode) handle->nativePaused = true;
    return NAE_OK;
}

int32_t NAE_Resume(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
    if (!handle->silencing) return NAE_OK;
    handle->silencing = false;
    if (handle->nativeSinkMode) handle->nativePaused = false;

    // The ramp is armed here rather than at Start, because there is no Start.
    //
    // It was originally armed on resume for a reason that turned out to be wrong --
    // it was aimed at the silent prefill, which is no longer released on a resume.
    // It has a real job now. The device has just been handed nothing but silence,
    // so audio resumes out of actual zero, and if the first sample is not zero that
    // is a real step. 8 ms of ramp, in device frames, applied to the first buffer
    // of real audio.
    const double rate = handle->fmt.Format.nSamplesPerSec;
    if (rate > 0) {
        handle->fadeInTotal = static_cast<int64_t>((rate * 8) / 1000);
        handle->fadeInRemaining = handle->fadeInTotal;
        handle->fadeInFrame = 0;
    }
    return NAE_OK;
}

int32_t NAE_SetClipGuard(NAE_Handle* handle, int32_t enabled, double ceilingDbTp,
                         double lookaheadMs, double releaseMs) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
    if (!enabled) {
        handle->clipGuard.set_active(false);
        return NAE_OK;
    }
    const uint32_t channels = handle->fmt.Format.nChannels;
    const uint32_t rate = handle->fmt.Format.nSamplesPerSec;
    if (!handle->clipGuard.configure(channels, rate, ceilingDbTp, lookaheadMs, releaseMs)) {
        g_lastErrorText = "clip guard settings are out of range";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (handle->bitPerfect) {
        handle->bitPerfect = false;
        handle->reason = static_cast<neiro::PerfectReason>(NAE_REASON_CLIP_GUARD);
    }
    return NAE_OK;
}

double NAE_GetClipGuardReductionDb(NAE_Handle* handle) NAE_NOEXCEPT {
    return handle ? handle->clipGuard.reduction_db() : 0.0;
}

int32_t NAE_SetDitherMode(NAE_Handle* handle, int32_t mode) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
    neiro::Dither::Mode selected;
    switch (mode) {
        case NAE_DITHER_OFF: selected = neiro::Dither::Mode::Off; break;
        case NAE_DITHER_TPDF: selected = neiro::Dither::Mode::TPDF; break;
        case NAE_DITHER_RECTANGULAR: selected = neiro::Dither::Mode::Rectangular; break;
        case NAE_DITHER_NOISE_SHAPED: selected = neiro::Dither::Mode::NoiseShaped; break;
        default: return NAE_ERR_INVALID_ARGUMENT;
    }
    handle->dither.set_mode(selected);
    handle->dithered = handle->dither.active();
    if (handle->bitPerfect && handle->dithered) {
        handle->bitPerfect = false;
        handle->reason = static_cast<neiro::PerfectReason>(NAE_REASON_DITHERED);
    }
    return NAE_OK;
}

int32_t NAE_GetDitherMode(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return NAE_DITHER_OFF;
    switch (handle->dither.mode()) {
        case neiro::Dither::Mode::Off: return NAE_DITHER_OFF;
        case neiro::Dither::Mode::Rectangular: return NAE_DITHER_RECTANGULAR;
        case neiro::Dither::Mode::NoiseShaped: return NAE_DITHER_NOISE_SHAPED;
        default: return NAE_DITHER_TPDF;
    }
}

int32_t NAE_SetChannelTrim(NAE_Handle* handle, const double* trimDb,
                           uint32_t count) NAE_NOEXCEPT {
    if (!handle || !trimDb) return NAE_ERR_INVALID_ARGUMENT;
    if (count > NAE_MAX_CHANNELS) return NAE_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < count; ++i) {
        if (!std::isfinite(trimDb[i]) || trimDb[i] < -96.0 || trimDb[i] > 24.0) {
            return NAE_ERR_INVALID_ARGUMENT;
        }
        handle->channelTrimDb[i] = trimDb[i];
    }
    return NAE_OK;
}

int32_t NAE_SetBalance(NAE_Handle* handle, double balance) NAE_NOEXCEPT {
    if (!handle || !std::isfinite(balance) || balance < -1.0 || balance > 1.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    handle->balance = balance;
    return NAE_OK;
}

int32_t NAE_SetPolarityMask(NAE_Handle* handle, uint32_t mask) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
    handle->polarityMask = mask;
    return NAE_OK;
}

int32_t NAE_SetChannelDelay(NAE_Handle* handle, const int32_t* frames,
                            uint32_t count) NAE_NOEXCEPT {
    if (!handle || !frames) return NAE_ERR_INVALID_ARGUMENT;
    if (count > NAE_MAX_CHANNELS) return NAE_ERR_INVALID_ARGUMENT;
    const uint32_t channels = handle->fmt.Format.nChannels;
    if (channels > NAE_MAX_CHANNELS) return NAE_ERR_INVALID_ARGUMENT;
    uint32_t maxDelay = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (frames[i] < 0 || frames[i] > 48000) return NAE_ERR_INVALID_ARGUMENT;
        maxDelay = std::max<uint32_t>(maxDelay, static_cast<uint32_t>(frames[i]));
    }
    handle->channelDelay.assign(channels, 0);
    handle->channelDelayPos.assign(channels, 0);
    // maxDelay + 1, not maxDelay: the read index is
    // (pos + ring - delay) % ring, so delay == ring reads the slot just
    // written and applies no delay at all. One extra frame makes the maximum
    // delay the maximum delay.
    const uint32_t ring = maxDelay + 1;
    handle->channelDelayBuf.assign(static_cast<size_t>(channels) * ring, 0.0f);
    handle->channelDelayMax = ring;
    for (uint32_t i = 0; i < count && i < channels; ++i) handle->channelDelay[i] = frames[i];
    return NAE_OK;
}

int32_t NAE_SetVolume(NAE_Handle* handle, double gain) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
    if (!std::isfinite(gain) || gain < 0.0 || gain > 1.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (gain == handle->volumeTarget) return NAE_OK;

    handle->volumeTarget = gain;
    const int64_t rate = static_cast<int64_t>(handle->fmt.Format.nSamplesPerSec);
    const int64_t rampFrames = std::max<int64_t>(1, rate / 50);  // 20 ms
    handle->volumeRampRemaining = rampFrames;
    handle->volumeRampDelta =
        (gain - handle->volume) / static_cast<double>(rampFrames);
    return NAE_OK;
}

double NAE_GetVolume(NAE_Handle* handle) NAE_NOEXCEPT {
    return handle ? handle->volumeTarget : 1.0;
}

void NAE_EqDefaults(NAE_EqParams* out) NAE_NOEXCEPT {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
}

int32_t NAE_SetEq(NAE_Handle* handle, const NAE_EqParams* params) NAE_NOEXCEPT {
    if (!handle || !params || params->structSize < sizeof(NAE_EqParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (params->bandCount < 0 || params->bandCount > NAE_EQ_MAX_BANDS) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(params->preampDb) || params->preampDb < -24.0 ||
        params->preampDb > 24.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (params->phase != NAE_EQ_PHASE_MINIMUM && params->phase != NAE_EQ_PHASE_LINEAR) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

    // Compute into a local cascade first, so a rejected band leaves the handle
    // exactly as it was rather than half-updated.
    neiro::EqBiquad coeffs[NAE_EQ_MAX_BANDS]{};
    bool enabled[NAE_EQ_MAX_BANDS]{};
    for (int32_t i = 0; i < params->bandCount; ++i) {
        const NAE_EqBand& band = params->bands[i];
        if (band.type < NAE_EQ_PEAKING || band.type > NAE_EQ_ALL_PASS) {
            return NAE_ERR_INVALID_ARGUMENT;
        }
        if (band.enabled) {
            if (!neiro::computeEqBiquad(band, static_cast<double>(handle->fmt.Format.nSamplesPerSec),
                               &coeffs[i])) {
                return NAE_ERR_INVALID_ARGUMENT;
            }
            enabled[i] = true;
        }
    }

    const size_t channels = handle->fmt.Format.nChannels ? handle->fmt.Format.nChannels : 1;
    handle->eqLinear = false;
    if (params->enabled && params->phase == NAE_EQ_PHASE_LINEAR) {
        std::vector<std::vector<neiro::LinearPhaseBandSpec>> perChannel(channels);
        for (int32_t i = 0; i < params->bandCount; ++i) {
            const NAE_EqBand& band = params->bands[i];
            if (!enabled[i]) continue;
            for (size_t c = 0; c < channels; ++c) {
                if (band.channelMask != 0 && !(band.channelMask & (1u << c))) continue;
                neiro::LinearPhaseBandSpec spec;
                spec.type = band.type;
                spec.freqHz = band.freqHz;
                spec.gainDb = band.gainDb;
                spec.q = band.q;
                perChannel[c].push_back(spec);
            }
        }
        neiro::LinearPhaseEq built;
        if (!built.configure(static_cast<uint32_t>(channels),
                             handle->fmt.Format.nSamplesPerSec, perChannel,
                             std::pow(10.0, params->preampDb / 20.0))) {
            g_lastErrorText =
                "the linear-phase EQ could not be built: check the band count, the "
                "channel count and the sample rate";
            return NAE_ERR_INVALID_ARGUMENT;
        }
        handle->linearEq = std::move(built);
        handle->eqLinear = true;
    }

    handle->eqParams = *params;
    handle->eqParams.structSize = sizeof(NAE_EqParams);
    handle->eqEnabled = params->enabled != 0;
    handle->eqPreamp = std::pow(10.0, params->preampDb / 20.0);
    handle->eqBandCount = params->bandCount;
    for (int32_t i = 0; i < NAE_EQ_MAX_BANDS; ++i) {
        handle->eqBandEnabled[i] = enabled[i];
        handle->eqCoeffs[i] = coeffs[i];
        handle->eqBandChannelMask[i] = params->bands[i].channelMask;
    }

    const size_t states = static_cast<size_t>(params->bandCount) * channels;
    handle->eqZ1.assign(states, 0.0);
    handle->eqZ2.assign(states, 0.0);

    return NAE_OK;
}

int32_t NAE_GetEq(NAE_Handle* handle, NAE_EqParams* out) NAE_NOEXCEPT {
    if (!handle || !out || out->structSize < sizeof(NAE_EqParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (handle->eqParams.structSize == 0) {
        NAE_EqDefaults(out);
    } else {
        *out = handle->eqParams;
    }
    out->structSize = sizeof(NAE_EqParams);
    return NAE_OK;
}

void NAE_CrossfeedDefaults(NAE_CrossfeedParams* out) NAE_NOEXCEPT {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->amount = 0.3;
    out->delayMs = 0.3;
    out->cutoffHz = 700.0;
}

int32_t NAE_SetCrossfeed(NAE_Handle* handle,
                         const NAE_CrossfeedParams* params) NAE_NOEXCEPT {
    if (!handle || !params || params->structSize < sizeof(NAE_CrossfeedParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(params->amount) || params->amount < 0.0 || params->amount > 1.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(params->delayMs) || params->delayMs < 0.0 || params->delayMs > 5.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const double fs = static_cast<double>(handle->fmt.Format.nSamplesPerSec);
    if (!std::isfinite(params->cutoffHz) || params->cutoffHz < 20.0 ||
        params->cutoffHz >= fs * 0.45) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

    const size_t frames = std::max<size_t>(
        1, static_cast<size_t>(params->delayMs * fs / 1000.0 + 0.5));
    handle->crossfeedParams = *params;
    handle->crossfeedParams.structSize = sizeof(NAE_CrossfeedParams);
    handle->crossfeedEnabled = params->enabled != 0;
    handle->crossfeedAmount = params->amount;
    handle->crossfeedDelayMs = params->delayMs;
    handle->crossfeedCutoffHz = params->cutoffHz;
    handle->crossfeedDelayFrames = frames;
    handle->crossfeedDelay.assign(frames * 2, 0.0);
    handle->crossfeedPos = 0;
    handle->crossfeedLpL = 0.0;
    handle->crossfeedLpR = 0.0;
    handle->crossfeedAlpha =
        1.0 - std::exp(-2.0 * 3.14159265358979323846 * params->cutoffHz / fs);
    return NAE_OK;
}

int32_t NAE_GetCrossfeed(NAE_Handle* handle, NAE_CrossfeedParams* out) NAE_NOEXCEPT {
    if (!handle || !out || out->structSize < sizeof(NAE_CrossfeedParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (handle->crossfeedParams.structSize == 0) {
        NAE_CrossfeedDefaults(out);
    } else {
        *out = handle->crossfeedParams;
    }
    out->structSize = sizeof(NAE_CrossfeedParams);
    return NAE_OK;
}

void NAE_ReplayGainDefaults(NAE_ReplayGainParams* out) NAE_NOEXCEPT {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->mode = NAE_RG_OFF;
    out->preventClipping = 1;
}

int32_t NAE_QueryReplayGain(NAE_Handle* handle, NAE_ReplayGainInfo* out) NAE_NOEXCEPT {
    if (!out || out->structSize < sizeof(NAE_ReplayGainInfo)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (!handle) {
        std::memset(out, 0, sizeof(*out));
        out->structSize = sizeof(NAE_ReplayGainInfo);
        return NAE_OK;
    }
    *out = handle->rgInfo;
    out->structSize = sizeof(NAE_ReplayGainInfo);
    return NAE_OK;
}

int32_t NAE_SetReplayGain(NAE_Handle* handle,
                          const NAE_ReplayGainParams* params) NAE_NOEXCEPT {
    if (!handle || !params || params->structSize < sizeof(NAE_ReplayGainParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (params->mode < NAE_RG_OFF || params->mode > NAE_RG_ALBUM) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(params->preampDb) || params->preampDb < -20.0 ||
        params->preampDb > 20.0 || !std::isfinite(params->fallbackDb) ||
        params->fallbackDb < -20.0 || params->fallbackDb > 20.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

    double db = 0.0;
    double peak = 0.0;
    bool tagged = false;
    if (params->mode == NAE_RG_TRACK && handle->rgInfo.trackValid) {
        db = handle->rgInfo.trackGainDb;
        peak = handle->rgInfo.trackPeak;
        tagged = true;
    } else if (params->mode == NAE_RG_ALBUM && handle->rgInfo.albumValid) {
        db = handle->rgInfo.albumGainDb;
        peak = handle->rgInfo.albumPeak;
        tagged = true;
    }

    double target = 1.0;
    if (params->mode != NAE_RG_OFF) {
        if (!tagged) db = params->fallbackDb;
        db += params->preampDb;
        if (params->preventClipping && peak > 0.0) {
            const double maxDb = -20.0 * std::log10(peak);
            if (db > maxDb) db = maxDb;
        }
        db = std::max(-60.0, std::min(20.0, db));
        target = std::pow(10.0, db / 20.0);
    }

    handle->rgParams = *params;
    handle->rgParams.structSize = sizeof(NAE_ReplayGainParams);
    handle->rgEnabled = params->mode != NAE_RG_OFF;
    handle->rgTarget = target;

    const int64_t rate = static_cast<int64_t>(handle->fmt.Format.nSamplesPerSec);
    const int64_t rampFrames = std::max<int64_t>(1, rate / 50);  // 20 ms
    if (target == handle->rgGain) {
        handle->rgRamp = 0;
        handle->rgDelta = 0.0;
    } else {
        handle->rgRamp = rampFrames;
        handle->rgDelta = (target - handle->rgGain) / static_cast<double>(rampFrames);
    }
    return NAE_OK;
}

double NAE_GetReplayGainGain(NAE_Handle* handle) NAE_NOEXCEPT {
    return handle ? handle->rgTarget : 1.0;
}

int32_t NAE_GetReplayGain(NAE_Handle* handle, NAE_ReplayGainParams* out) NAE_NOEXCEPT {
    if (!handle || !out || out->structSize < sizeof(NAE_ReplayGainParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (handle->rgParams.structSize == 0) {
        NAE_ReplayGainDefaults(out);
    } else {
        *out = handle->rgParams;
    }
    out->structSize = sizeof(NAE_ReplayGainParams);
    return NAE_OK;
}

int32_t NAE_PreloadNext(NAE_Handle* handle, const char* path) NAE_NOEXCEPT {
    if (handle && handle->routingActive) return NAE_ERR_REQUIRES_REOPEN;
#ifdef NEIRO_HAVE_FFMPEG
    if (!handle || !path || !path[0]) return NAE_ERR_INVALID_ARGUMENT;
    if (!handle->decoder) return NAE_ERR_NOT_OPEN;
    std::unique_ptr<neiro::Decoder> next(new neiro::Decoder());
    std::string error;
    if (!next->open(path, &error, handle->fmt.Format.nChannels)) {
        g_lastErrorText = error;
        return NAE_ERR_CANNOT_OPEN;
    }
    const auto& info = next->info();
    if (static_cast<int32_t>(info.sample_rate) != handle->sourceRate ||
        static_cast<int32_t>(info.channels) != handle->fileChannels) {
        g_lastErrorText =
            "gapless requires the same sample rate and channel count as the current file";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    handle->nextTotalSourceFrames =
        static_cast<int64_t>(info.duration_ms / 1000.0 * info.sample_rate) +
        static_cast<int64_t>(info.sample_rate) / 2;
    handle->nextFileChannels = static_cast<int32_t>(info.channels);
    handle->nextFileSampleRate = static_cast<int32_t>(info.sample_rate);
    handle->nextFileBitsPerSample = static_cast<int32_t>(info.bits_per_sample);
    handle->nextFileRawBits = static_cast<int32_t>(info.raw_bits_per_sample);
    handle->nextFileDurationMs = static_cast<int32_t>(info.duration_ms);
    std::snprintf(handle->nextFileCodec, sizeof(handle->nextFileCodec), "%s",
                  info.codec.c_str());
    handle->nextDecodePath = info.decode_path;
    handle->nextRgInfo = {};
    handle->nextRgInfo.trackValid = info.rg_track_valid ? 1 : 0;
    handle->nextRgInfo.trackGainDb = info.rg_track_gain_db;
    handle->nextRgInfo.trackPeak = info.rg_track_peak;
    handle->nextRgInfo.albumValid = info.rg_album_valid ? 1 : 0;
    handle->nextRgInfo.albumGainDb = info.rg_album_gain_db;
    handle->nextRgInfo.albumPeak = info.rg_album_peak;
    handle->nextRgInfo.structSize = sizeof(NAE_ReplayGainInfo);

    // Decode the first block now, not at the seam. Opening the file ahead of
    // time is not enough: the first read is still a decoder call, and the fifo
    // can run dry during it. This is the block that carries the seam.
    handle->nextPrefetched.clear();
    std::string readError;
    next->readExact(&handle->nextPrefetched, 4096, &readError);
    handle->nextPrefetchedFrames =
        handle->nextPrefetched.size() / static_cast<size_t>(handle->fmt.Format.nChannels);
    handle->nextDecoder = std::move(next);
    return NAE_OK;
#else
    (void)handle;
    (void)path;
    return NAE_ERR_INVALID_ARGUMENT;
#endif
}

int32_t NAE_ClearNext(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
#ifdef NEIRO_HAVE_FFMPEG
    handle->nextDecoder.reset();
    handle->nextTotalSourceFrames = 0;
    handle->nextDecodePath.clear();
    handle->nextRgInfo = {};
    handle->nextPrefetched.clear();
    handle->nextPrefetchedFrames = 0;
#endif
    return NAE_OK;
}

int32_t NAE_SkipToNext(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
#ifdef NEIRO_HAVE_FFMPEG
    // Nothing preloaded means there is no next track yet. Reporting that here is
    // better than accepting the request and letting the render loop discover
    // there was nothing to switch to -- the caller could not tell those apart.
    if (!handle->nextDecoder) {
        g_lastErrorText = "no next track is preloaded";
        return NAE_ERR_NOT_OPEN;
    }
    handle->skipRequested.store(true);
    return NAE_OK;
#else
    (void)handle;
    return NAE_ERR_NOT_OPEN;
#endif
}

uint64_t NAE_GetTrackSerial(NAE_Handle* handle) NAE_NOEXCEPT {
    return handle ? handle->trackSerial : 0;
}

void NAE_ConvolverDefaults(NAE_ConvolverParams* out) NAE_NOEXCEPT {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->wet = 1.0;
    out->dry = 0.0;
}

int32_t NAE_SetConvolver(NAE_Handle* handle,
                         const NAE_ConvolverParams* params) NAE_NOEXCEPT {
    if (!handle || !params || params->structSize < sizeof(NAE_ConvolverParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(params->wet) || params->wet < 0.0 || params->wet > 1.0 ||
        !std::isfinite(params->dry) || params->dry < 0.0 || params->dry > 1.0 ||
        !std::isfinite(params->gainDb) || params->gainDb < -24.0 ||
        params->gainDb > 24.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
#ifdef NEIRO_HAVE_FFMPEG
    const bool hasPath = params->irPath[0] != '\0';
    if (params->enabled && !hasPath && handle->convTaps == 0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (hasPath) {
        neiro::Decoder ir;
        std::string error;
        if (!ir.open(params->irPath, &error, 0)) {
            g_lastErrorText = error;
            return NAE_ERR_CANNOT_OPEN;
        }
        const auto& info = ir.info();
        if (static_cast<int32_t>(info.sample_rate) != handle->sourceRate) {
            g_lastErrorText = "the convolver IR must be at the source sample rate";
            return NAE_ERR_INVALID_ARGUMENT;
        }
        std::vector<float> pcm;
        std::vector<float> block;
        while (ir.read(&block, &error)) {
            pcm.insert(pcm.end(), block.begin(), block.end());
            block.clear();
        }
        const size_t irChannels = info.channels ? info.channels : 1;
        const size_t frames = pcm.size() / irChannels;
        if (frames == 0 || frames > NAE_CONVOLVER_MAX_TAPS) {
            g_lastErrorText = "the convolver IR is empty or longer than 4096 frames";
            return NAE_ERR_INVALID_ARGUMENT;
        }
        // One IR per channel of the IR file. A mono IR applies to every stream
        // channel; a 5.1 IR keeps all six instead of being folded to L/R.
        std::vector<std::vector<float>> perChannel(irChannels,
                                                  std::vector<float>(frames, 0.0f));
        for (size_t f = 0; f < frames; ++f) {
            for (size_t c = 0; c < irChannels; ++c) {
                perChannel[c][f] = pcm[f * irChannels + c];
            }
        }
        if (params->normalize) {
            for (auto& irChannel : perChannel) {
                double sum = 0.0;
                for (float v : irChannel) sum += static_cast<double>(v) * v;
                if (sum > 0.0) {
                    const float scale = static_cast<float>(1.0 / std::sqrt(sum));
                    for (float& v : irChannel) v *= scale;
                }
            }
        }
        const size_t streamChannels = handle->fmt.Format.nChannels ? handle->fmt.Format.nChannels : 1;
        if (irChannels != 1 && irChannels != streamChannels) {
            // A mono IR applies to every channel; any other count must match the
            // stream. Folding a 5.1 IR into two channels (or padding a stereo IR
            // over six) would be a filter nobody asked for.
            g_lastErrorText = "the convolver IR has " + std::to_string(irChannels) +
                              " channels; a mono IR or one matching the stream is required";
            return NAE_ERR_INVALID_ARGUMENT;
        }
        const std::vector<std::vector<float>> streamIrs =
            irChannels == 1
                ? std::vector<std::vector<float>>(streamChannels, perChannel[0])
                : perChannel;
        if (!handle->conv.configure(streamIrs)) {
            g_lastErrorText = "the convolver IR could not be partitioned";
            return NAE_ERR_INVALID_ARGUMENT;
        }
        handle->convChannels = streamIrs.size();
        handle->convTaps = frames;
        handle->conv.reset();
        handle->convDryBuf.clear();
        handle->convDryPos = 0;
    }
#else
    (void)params;
    return NAE_ERR_INVALID_ARGUMENT;
#endif
    handle->convParams = *params;
    handle->convParams.structSize = sizeof(NAE_ConvolverParams);
    handle->convParams.irPath[sizeof(handle->convParams.irPath) - 1] = '\0';
    handle->convEnabled = params->enabled != 0;
    handle->convWet = params->wet;
    handle->convDry = params->dry;
    handle->convGain = std::pow(10.0, params->gainDb / 20.0);
    return NAE_OK;
}

int32_t NAE_GetConvolver(NAE_Handle* handle, NAE_ConvolverParams* out) NAE_NOEXCEPT {
    if (!handle || !out || out->structSize < sizeof(NAE_ConvolverParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (handle->convParams.structSize == 0) {
        NAE_ConvolverDefaults(out);
    } else {
        *out = handle->convParams;
    }
    out->structSize = sizeof(NAE_ConvolverParams);
    return NAE_OK;
}

void NAE_CrossfadeDefaults(NAE_CrossfadeParams* out) NAE_NOEXCEPT {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->curve = NAE_CROSSFADE_EQUAL_POWER;
}

int32_t NAE_SetCrossfade(NAE_Handle* handle,
                         const NAE_CrossfadeParams* params) NAE_NOEXCEPT {
    if (handle && handle->routingActive) return NAE_ERR_REQUIRES_REOPEN;
    if (!handle || !params || params->structSize < sizeof(NAE_CrossfadeParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (!std::isfinite(params->durationMs) || params->durationMs < 0.0 ||
        params->durationMs > 30000.0 ||
        (params->curve != NAE_CROSSFADE_LINEAR &&
         params->curve != NAE_CROSSFADE_EQUAL_POWER)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    handle->crossfadeParams = *params;
    handle->crossfadeParams.structSize = sizeof(NAE_CrossfadeParams);
    handle->crossfadeEnabled = params->enabled != 0;
    handle->crossfadeDurationMs = params->durationMs;
    handle->crossfadeCurve = params->curve;
    handle->crossfadeFrames =
        static_cast<int64_t>(params->durationMs * handle->sourceRate / 1000.0 + 0.5);
    handle->crossfadeActive = false;
    handle->crossfadeBuf.clear();
    handle->crossfadeBufPos = 0;
    handle->crossfadeBufFrames = 0;
    handle->crossfadeConsumed = 0;
    return NAE_OK;
}

int32_t NAE_GetCrossfade(NAE_Handle* handle, NAE_CrossfadeParams* out) NAE_NOEXCEPT {
    if (!handle || !out || out->structSize < sizeof(NAE_CrossfadeParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (handle->crossfadeParams.structSize == 0) {
        NAE_CrossfadeDefaults(out);
    } else {
        *out = handle->crossfadeParams;
    }
    out->structSize = sizeof(NAE_CrossfadeParams);
    return NAE_OK;
}


// DoP needs a 24-valid-bit carrier, not the endpoint's native container. The
// generic open wraps the native format (this DAC reports 32 valid 32), so DoP
// gets its own initialization: 24-in-32 first, then 24-in-24. If neither opens,
// the endpoint cannot carry DoP and the caller must say so rather than play
// shifted noise.
static int32_t openDopDevice(const char* deviceId, uint32_t carrier, uint32_t channels,
                             NAE_Handle** out) {
    if (!out) return NAE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (FAILED(ensureCom())) return NAE_ERR_COM;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        return NAE_ERR_NO_DEVICE;
    }
    ComPtr<IMMDevice> device;
    if (deviceId && deviceId[0]) {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, deviceId, -1, nullptr, 0);
        if (needed <= 1) return NAE_ERR_INVALID_ARGUMENT;
        std::wstring wide(static_cast<size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, deviceId, -1, wide.data(), needed);
        if (FAILED(enumerator->GetDevice(wide.c_str(), device.GetAddressOf())) || !device) {
            return NAE_ERR_NO_DEVICE;
        }
    } else if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                          device.GetAddressOf())) ||
               !device) {
        return NAE_ERR_NO_DEVICE;
    }

    WAVEFORMATEX native{};
    uint16_t nativeValid = 0;
    if (!readNativeFormat(device.Get(), &native, &nativeValid)) {
        return NAE_ERR_NO_NATIVE_FORMAT;
    }
    if (native.nChannels != channels) {
        g_lastErrorText = "the endpoint's channel count does not match the DSD stream";
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }

    ComPtr<IAudioClient> client;
    if (FAILED(neiro::wasapi::activate(device.Get(), __uuidof(IAudioClient),
                                       client.GetAddressOf())) ||
        !client) {
        return NAE_ERR_ACTIVATE;
    }
    HANDLE renderEvent = createRenderEvent();
    if (renderEvent && FAILED(client->SetEventHandle(renderEvent))) {
        CloseHandle(renderEvent);
        renderEvent = nullptr;
    }
    const DWORD streamFlags = renderEvent ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;

    WAVEFORMATEXTENSIBLE fmt{};
    auto build = [&](uint16_t containerBits) {
        fmt = {};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = static_cast<WORD>(channels);
        fmt.Format.nSamplesPerSec = carrier;
        fmt.Format.wBitsPerSample = containerBits;
        fmt.Format.nBlockAlign = static_cast<WORD>(channels * (containerBits / 8));
        fmt.Format.nAvgBytesPerSec = carrier * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 24;
        fmt.dwChannelMask = channels == 1 ? SPEAKER_FRONT_LEFT
                                          : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    };

    HRESULT initHr = E_FAIL;
    // DoP does not need low latency; a 50 ms buffer gives the packer and the
    // disk read room. The endpoint's default here was 10 ms, which is tight
    // enough that a file read shows up as crackle.
    const REFERENCE_TIME bufferDuration = 50 * 10000;
    build(32);
    initHr = remember(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, streamFlags,
                                         bufferDuration, 0, &fmt.Format, nullptr));
    if (FAILED(initHr)) {
        initHr = remember(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, streamFlags, 0, 0,
                                             &fmt.Format, nullptr));
    }
    if (FAILED(initHr)) {
        build(24);
        initHr = remember(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, streamFlags,
                                             bufferDuration, 0, &fmt.Format, nullptr));
        if (FAILED(initHr)) {
            initHr = remember(client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, streamFlags, 0, 0,
                                                 &fmt.Format, nullptr));
        }
    }
    if (FAILED(initHr)) {
        g_lastErrorText =
            "the endpoint refused a 24-valid-bit DoP carrier at this rate";
        if (renderEvent) CloseHandle(renderEvent);
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }

    UINT32 bufferFrames = 0;
    if (FAILED(remember(client->GetBufferSize(&bufferFrames))) || bufferFrames == 0) {
        if (renderEvent) CloseHandle(renderEvent);
        return NAE_ERR_INITIALIZE;
    }
    ComPtr<IAudioRenderClient> render;
    if (FAILED(remember(client->GetService(IID_PPV_ARGS(&render)))) || !render) {
        if (renderEvent) CloseHandle(renderEvent);
        return NAE_ERR_INITIALIZE;
    }
    REFERENCE_TIME latency = 0;
    client->GetStreamLatency(&latency);

    auto handle = std::make_unique<NAE_Handle>();
    handle->device = device;
    handle->client = client;
    handle->render = render;
    handle->renderEvent = renderEvent;
    handle->fmt = fmt;
    handle->bufferFrames = bufferFrames;
    handle->latencyMs = static_cast<double>(latency) / 10000.0;
    handle->bufferDurationMs =
        static_cast<double>(bufferFrames) * 1000.0 / static_cast<double>(carrier);
    handle->requestedBits = 24;
    handle->grantedBits = 24;
    handle->exclusive = true;
    *out = handle.release();
    return NAE_OK;
}

static int32_t openDopWithTarget(const char* deviceId, const char* path,
                                 int32_t targetDsdRate, NAE_Handle** out,
                                 int32_t* carrierRate, int32_t* channels, int32_t* bits,
                                 double* latencyMs) NAE_NOEXCEPT {
    if (!out || !path) return NAE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (carrierRate) *carrierRate = 0;
    if (channels) *channels = 0;
    if (bits) *bits = 0;
    if (latencyMs) *latencyMs = 0.0;

    std::FILE* probe = neiro::openUtf8(path, "rb");
    if (!probe) {
        g_lastErrorText = std::string("cannot open ") + path;
        return NAE_ERR_CANNOT_OPEN;
    }
    char magic[5] = {};
    std::fread(magic, 1, 4, probe);
    std::fclose(probe);
    const bool isDsf = std::strcmp(magic, "DSD ") == 0;
    const bool isDff = std::strcmp(magic, "FRM8") == 0;
    if (!isDsf && !isDff) {
        g_lastErrorText = "DoP needs a DSF or uncompressed DFF file";
        return NAE_ERR_INVALID_ARGUMENT;
    }

    uint32_t dsdRate = 0;
    uint32_t dsdChannels = 0;
    int64_t frames = 0;
    std::unique_ptr<dsf::File> dsfFile;
    std::unique_ptr<dff::File> dffFile;
    if (isDsf) {
        dsfFile = std::make_unique<dsf::File>();
        if (!dsfFile->open(path)) {
            g_lastErrorText = dsfFile->error;
            return NAE_ERR_CANNOT_OPEN;
        }
        dsdRate = dsfFile->fmt.dsdRate;
        dsdChannels = dsfFile->fmt.channels;
        frames = static_cast<int64_t>(dsfFile->totalBlocks()) * dsfFile->fmt.blockSize / 2;
    } else {
        dffFile = std::make_unique<dff::File>();
        if (!dffFile->open(path)) {
            g_lastErrorText = dffFile->error;
            return NAE_ERR_CANNOT_OPEN;
        }
        dsdRate = dffFile->fmt.sampleRate;
        dsdChannels = dffFile->fmt.channels;
        frames = static_cast<int64_t>(dffFile->totalDopFrames());
    }
    if (dsdRate == 0 || dsdChannels == 0 || frames <= 0) {
        g_lastErrorText = "the DSD file reports no audio";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (dsdChannels != 2) {
        g_lastErrorText = "DoP is implemented for stereo DSD only";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    uint32_t streamRate = dsdRate;
    std::unique_ptr<neiro::dsd::Downrate> downrate;
    if (targetDsdRate > 0 && static_cast<uint32_t>(targetDsdRate) < dsdRate) {
        streamRate = static_cast<uint32_t>(targetDsdRate);
        downrate = std::make_unique<neiro::dsd::Downrate>();
        std::string error;
        if (!downrate->configure(dsdRate, streamRate, dsdChannels, isDff, &error)) {
            g_lastErrorText = error;
            return NAE_ERR_INVALID_ARGUMENT;
        }
        frames /= static_cast<int64_t>(downrate->factor());
    } else if (targetDsdRate > 0 && static_cast<uint32_t>(targetDsdRate) > dsdRate) {
        g_lastErrorText = "the target DSD rate is above the source rate";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t carrier = static_cast<int32_t>(streamRate / 16);

    NAE_Handle* handle = nullptr;
    const int32_t rc = openDopDevice(deviceId, static_cast<uint32_t>(carrier), dsdChannels,
                                     &handle);
    if (rc != NAE_OK || !handle) return rc != NAE_OK ? rc : NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;

    handle->dopMode = true;
    handle->dopDownrate = std::move(downrate);
    handle->dopDsf = std::move(dsfFile);
    handle->dopDff = std::move(dffFile);
    handle->dopCarrierRate = static_cast<uint32_t>(carrier);
    handle->dopFramesTotal = frames;
    handle->dopFramesPlayed = 0;
    handle->dopMarker = 0x05;
    handle->sourceRate = static_cast<uint32_t>(carrier);
    handle->totalSourceFrames = frames;
    handle->sourcePos = 0;
    handle->fileChannels = static_cast<int32_t>(dsdChannels);
    handle->fileSampleRate = static_cast<int32_t>(dsdRate);
    handle->dopStreamRate = static_cast<int32_t>(streamRate);
    handle->fileBitsPerSample = 1;
    handle->fileDurationMs =
        static_cast<int32_t>(frames * 1000.0 / static_cast<double>(carrier));
    std::snprintf(handle->fileCodec, sizeof(handle->fileCodec), "%s", isDsf ? "dsf" : "dff");
    handle->decodePath = isDsf ? "dop:dsf" : "dop:dff";
    handle->bitPerfect = true;
    handle->reason = neiro::PerfectReason::BitPerfect;
    handle->exclusive = true;
    if (handle->dopDsf) {
        // Zero forces the first readBlockRaw in NAE_PlayDop. Setting this to
        // blockSize/2 made the first frame use an empty buffer and end the file
        // immediately -- measured as a 10 s request returning in 0.05 s.
        handle->dopBlockFrames = 0;
        handle->dopFrameInBlock = 0;
        handle->dopBlock = 0;
    }

    *out = handle;
    if (carrierRate) *carrierRate = carrier;
    if (channels) *channels = static_cast<int32_t>(dsdChannels);
    if (bits) *bits = 24;
    if (latencyMs) *latencyMs = handle->latencyMs;
    return NAE_OK;
}

int32_t NAE_OpenNativeDsd(const char* deviceId, const char* path, NAE_Handle** out,
                          int32_t* dsdRate, int32_t* channels, int32_t* bits,
                          double* latencyMs) NAE_NOEXCEPT {
    if (out) *out = nullptr;
    if (dsdRate) *dsdRate = 0;
    if (channels) *channels = 0;
    if (bits) *bits = 0;
    if (latencyMs) *latencyMs = 0.0;
    if (!path) return NAE_ERR_INVALID_ARGUMENT;

    std::FILE* probe = neiro::openUtf8(path, "rb");
    if (!probe) return NAE_ERR_CANNOT_OPEN;
    char magic[5] = {};
    std::fread(magic, 1, 4, probe);
    std::fclose(probe);
    if (std::strcmp(magic, "DSD ") != 0 && std::strcmp(magic, "FRM8") != 0) {
        g_lastErrorText = "native DSD needs a DSF or uncompressed DFF file";
        return NAE_ERR_INVALID_ARGUMENT;
    }

    // The answer comes from the backend's capability probe, not from this
    // function: a backend that exposes a 1-bit stream sets the flag and the rate
    // list, and this path becomes reachable without changing the engine. WASAPI
    // never sets it (no standard DSD format), so on this backend the answer is
    // an explicit "the endpoint cannot carry it", which Session turns into the
    // PCM fallback it owns.
    NAE_DeviceCaps caps{};
    caps.structSize = sizeof(caps);
    const int32_t capsRc = NAE_QueryDeviceCaps(deviceId, NAE_SHARE_MODE_EXCLUSIVE, &caps);
    if (capsRc != NAE_OK) return capsRc;
    if ((caps.probed & NAE_CAPS_PROBED_NATIVE_DSD) == 0) {
        g_lastErrorText = "native DSD capability was not probed for this endpoint";
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }
    if (!caps.nativeDsdSupported) {
        g_lastErrorText =
            "this endpoint exposes no native DSD format; use DoP or PCM fallback";
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }
    // A backend that advertises native DSD must also provide its stream path;
    // none is compiled in. Do not open a DoP carrier here: the caller asked for
    // native and Session owns fallback policy.
    g_lastErrorText = "the native DSD stream path is not compiled into this backend";
    return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
}

int32_t NAE_OpenDop(const char* deviceId, const char* path, NAE_Handle** out,
                    int32_t* carrierRate, int32_t* channels, int32_t* bits,
                    double* latencyMs) NAE_NOEXCEPT {
    return openDopWithTarget(deviceId, path, 0, out, carrierRate, channels, bits, latencyMs);
}

int32_t NAE_OpenDopDownrated(const char* deviceId, const char* path, int32_t targetDsdRate,
                             NAE_Handle** out, int32_t* carrierRate, int32_t* channels,
                             int32_t* bits, double* latencyMs) NAE_NOEXCEPT {
    if (targetDsdRate <= 0) return NAE_ERR_INVALID_ARGUMENT;
    return openDopWithTarget(deviceId, path, targetDsdRate, out, carrierRate, channels, bits,
                             latencyMs);
}

namespace {

// Read up to `bytesPerChannel` source bytes per channel from a DSF or DFF
// opened for a native sink, interleaved row-major. Returns the bytes actually
// produced (short only at end of file).
size_t readNativeSinkChunk(NAE_Handle* handle, size_t bytesPerChannel,
                           std::vector<uint8_t>* out) {
    const size_t ch = handle->nativeChannels;
    out->clear();
    if (bytesPerChannel == 0 || ch == 0) return 0;
    if (handle->dopDsf) {
        const size_t blockSize = handle->dopDsf->fmt.blockSize;
        size_t produced = 0;
        while (produced < bytesPerChannel) {
            if (handle->dopRawPos >= handle->dopRawBytes) {
                if (!handle->dopDsf->readBlockRaw(handle->dopBlock, &handle->dopRaw)) {
                    break;
                }
                ++handle->dopBlock;
                if (handle->dopRaw.size() < blockSize * ch) break;
                handle->dopRawPos = 0;
                handle->dopRawBytes = blockSize;
            }
            const size_t take =
                std::min(bytesPerChannel - produced, handle->dopRawBytes - handle->dopRawPos);
            const size_t base = out->size();
            out->resize(base + take * ch);
            for (size_t i = 0; i < take; ++i) {
                for (size_t c = 0; c < ch; ++c) {
                    (*out)[base + i * ch + c] =
                        handle->dopRaw[c * blockSize + handle->dopRawPos + i];
                }
            }
            handle->dopRawPos += take;
            produced += take;
        }
        return produced;
    }
    if (handle->dopDff) {
        const uint64_t remaining =
            handle->dopDff->fmt.dataBytes - handle->dopDataOffset;
        const size_t want = static_cast<size_t>(
            std::min<uint64_t>(remaining, bytesPerChannel * ch));
        if (want == 0) return 0;
        const size_t whole = want - (want % ch);
        if (whole == 0) return 0;
        out->resize(whole);
        if (!handle->dopDff->readData(handle->dopDataOffset, out->data(), whole)) {
            out->clear();
            return 0;
        }
        handle->dopDataOffset += whole;
        return whole / ch;
    }
    return 0;
}

}  // namespace

int32_t NAE_OpenNativeDsdSink(const char* path, int32_t targetDsdRate,
                              NAE_DsdWriteFn write, void* context, NAE_Handle** out,
                              int32_t* dsdRate, int32_t* channels,
                              int32_t* bitOrderMsbFirst,
                              double* durationMs) NAE_NOEXCEPT {
    if (out) *out = nullptr;
    if (dsdRate) *dsdRate = 0;
    if (channels) *channels = 0;
    if (bitOrderMsbFirst) *bitOrderMsbFirst = 0;
    if (durationMs) *durationMs = 0.0;
    if (!path || !write) {
        g_lastErrorText = "a native DSD sink needs a path and a write callback";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (targetDsdRate < 0) return NAE_ERR_INVALID_ARGUMENT;

    std::FILE* probe = neiro::openUtf8(path, "rb");
    if (!probe) {
        g_lastErrorText = std::string("cannot open ") + path;
        return NAE_ERR_CANNOT_OPEN;
    }
    char magic[5] = {};
    std::fread(magic, 1, 4, probe);
    std::fclose(probe);
    const bool isDsf = std::strcmp(magic, "DSD ") == 0;
    const bool isDff = std::strcmp(magic, "FRM8") == 0;
    if (!isDsf && !isDff) {
        g_lastErrorText = "a native DSD sink needs a DSF or uncompressed DFF file";
        return NAE_ERR_INVALID_ARGUMENT;
    }

    uint32_t sourceRate = 0;
    uint32_t sourceChannels = 0;
    uint64_t bytesPerChannel = 0;
    std::unique_ptr<dsf::File> dsfFile;
    std::unique_ptr<dff::File> dffFile;
    if (isDsf) {
        dsfFile = std::make_unique<dsf::File>();
        if (!dsfFile->open(path)) {
            g_lastErrorText = dsfFile->error;
            return NAE_ERR_CANNOT_OPEN;
        }
        sourceRate = dsfFile->fmt.dsdRate;
        sourceChannels = dsfFile->fmt.channels;
        bytesPerChannel = dsfFile->totalBlocks() * dsfFile->fmt.blockSize;
    } else {
        dffFile = std::make_unique<dff::File>();
        if (!dffFile->open(path)) {
            g_lastErrorText = dffFile->error;
            return NAE_ERR_CANNOT_OPEN;
        }
        sourceRate = dffFile->fmt.sampleRate;
        sourceChannels = dffFile->fmt.channels;
        bytesPerChannel = dffFile->fmt.dataBytes / (sourceChannels ? sourceChannels : 1);
    }
    if (sourceRate == 0 || sourceChannels == 0 || bytesPerChannel == 0) {
        g_lastErrorText = "the DSD file reports no audio";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (sourceChannels != 2) {
        g_lastErrorText = "native DSD is implemented for stereo only";
        return NAE_ERR_INVALID_ARGUMENT;
    }

    uint32_t streamRate = sourceRate;
    std::unique_ptr<neiro::dsd::Downrate> downrate;
    if (targetDsdRate > 0 && static_cast<uint32_t>(targetDsdRate) < sourceRate) {
        streamRate = static_cast<uint32_t>(targetDsdRate);
        downrate = std::make_unique<neiro::dsd::Downrate>();
        std::string error;
        if (!downrate->configure(sourceRate, streamRate, sourceChannels, isDff, &error)) {
            g_lastErrorText = error;
            return NAE_ERR_INVALID_ARGUMENT;
        }
        bytesPerChannel /= downrate->factor();
    } else if (targetDsdRate > 0 && static_cast<uint32_t>(targetDsdRate) > sourceRate) {
        g_lastErrorText = "the target DSD rate is above the source rate";
        return NAE_ERR_INVALID_ARGUMENT;
    }

    NAE_Handle* handle = nullptr;
    try {
        handle = new NAE_Handle();
    } catch (...) {
        g_lastErrorText = "no memory for the native DSD sink";
        return NAE_ERR_NO_MEMORY;
    }
    handle->nativeSinkMode = true;
    handle->nativeWrite = write;
    handle->nativeContext = context;
    handle->nativeRate = streamRate;
    handle->nativeChannels = sourceChannels;
    handle->nativeBitOrderMsbFirst = isDff ? 1 : 0;
    handle->nativeDownrate = std::move(downrate);
    handle->dopDsf = std::move(dsfFile);
    handle->dopDff = std::move(dffFile);
    handle->dopRawPos = 0;
    handle->dopRawBytes = 0;
    handle->dopBlock = 0;
    const double seconds = static_cast<double>(bytesPerChannel) * 8.0 /
                           static_cast<double>(streamRate);
    handle->nativeDurationMs = seconds * 1000.0;
    handle->sourceRate = streamRate;
    handle->fileChannels = static_cast<int32_t>(sourceChannels);
    handle->fileSampleRate = static_cast<int32_t>(sourceRate);
    handle->fileBitsPerSample = 1;
    handle->fileDurationMs = static_cast<int32_t>(handle->nativeDurationMs);
    std::snprintf(handle->fileCodec, sizeof(handle->fileCodec), "%s", isDsf ? "dsf" : "dff");
    handle->decodePath = isDsf ? "native:dsf" : "native:dff";
    handle->bitPerfect = true;
    handle->reason = neiro::PerfectReason::BitPerfect;

    *out = handle;
    if (dsdRate) *dsdRate = static_cast<int32_t>(streamRate);
    if (channels) *channels = static_cast<int32_t>(sourceChannels);
    if (bitOrderMsbFirst) *bitOrderMsbFirst = handle->nativeBitOrderMsbFirst;
    if (durationMs) *durationMs = handle->nativeDurationMs;
    return NAE_OK;
}

int32_t NAE_PlayNativeDsd(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_INVALID_ARGUMENT;
    if (!handle->nativeSinkMode) {
        g_lastErrorText = "this handle has no native DSD path; open it with "
                          "NAE_OpenNativeDsd or NAE_OpenNativeDsdSink";
        return NAE_ERR_NOT_OPEN;
    }
    if (milliseconds <= 0) return NAE_ERR_INVALID_ARGUMENT;
    if (handle->nativePaused || handle->nativeEnded) return NAE_OK;

    const size_t ch = handle->nativeChannels;
    const uint32_t factor = handle->nativeDownrate ? handle->nativeDownrate->factor() : 1;
    const int64_t wantBytes = static_cast<int64_t>(milliseconds) *
                              static_cast<int64_t>(handle->nativeRate) / 8000;
    int64_t produced = 0;
    while (produced < wantBytes) {
        const size_t chunk = static_cast<size_t>(
            std::min<int64_t>(4096, wantBytes - produced));
        const size_t sourceBytes =
            readNativeSinkChunk(handle, chunk * factor, &handle->nativeIn);
        if (sourceBytes == 0) {
            handle->nativeEnded = true;
            break;
        }
        const uint8_t* bytes = handle->nativeIn.data();
        size_t bytesPerChannel = sourceBytes;
        if (handle->nativeDownrate) {
            handle->nativeOut.clear();
            handle->nativeDownrate->feed(handle->nativeIn.data(), handle->nativeIn.size(),
                                         &handle->nativeOut);
            if (handle->nativeOut.empty()) continue;
            bytes = handle->nativeOut.data();
            bytesPerChannel = handle->nativeOut.size() / ch;
        }
        if (bytesPerChannel == 0) continue;
        const int32_t rc =
            handle->nativeWrite(handle->nativeContext, bytes,
                                static_cast<int32_t>(bytesPerChannel),
                                static_cast<int32_t>(ch));
        if (rc != 0) {
            g_lastErrorText = "the native DSD sink rejected a write";
            return rc;
        }
        produced += static_cast<int64_t>(bytesPerChannel);
    }
    handle->sourcePos += produced;
    return NAE_OK;
}

int32_t NAE_PlayDop(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT {
    if (!handle || !handle->dopMode) return NAE_ERR_NOT_OPEN;
    if (milliseconds <= 0) return NAE_ERR_INVALID_ARGUMENT;

    const uint32_t containerBits = handle->fmt.Format.wBitsPerSample;
    const uint32_t validBits = handle->fmt.Samples.wValidBitsPerSample;
    if (containerBits < 24 || validBits != 24) {
        g_lastErrorText = "DoP needs a 24-bit carrier";
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }
    const size_t bytesPerSample = containerBits / 8;
    const size_t bytesPerFrame = handle->fmt.Format.nBlockAlign;
    const size_t ch = handle->fmt.Format.nChannels;
    const uint32_t carrier = handle->dopCarrierRate;

    // Diagnostic switches for bringing up a new DAC. The defaults are the spec:
    // DFF MSB-first, DSF reversed. These exist so a wrong order can be heard
    // and identified rather than guessed at from a noise description.
    static const bool reverseDff = std::getenv("NEIRO_DOP_DFF_REVERSE") != nullptr;
    static const bool swapBytes = std::getenv("NEIRO_DOP_SWAP_BYTES") != nullptr;
    static const bool swapChannels = std::getenv("NEIRO_DOP_SWAP_CHANNELS") != nullptr;


    auto writeWord = [&](BYTE* p, uint32_t word) {
        if (containerBits == 24) {
            p[0] = static_cast<BYTE>(word);
            p[1] = static_cast<BYTE>(word >> 8);
            p[2] = static_cast<BYTE>(word >> 16);
        } else {
            const uint32_t placed = word << 8;
            std::memcpy(p, &placed, sizeof(placed));
        }
    };

    // Refill the downrated byte stream until at least two rows (one DoP frame
    // per channel) are available. Bytes are interleaved: row r, channel c is at
    // r*ch + c.
    auto refillDown = [&]() -> bool {
        // Compact once the consumed prefix is at least half the buffer, so the
        // memmove happens once per refill instead of once per frame.
        if (handle->dopDownRead > 0 &&
            handle->dopDownRead * 2 >= handle->dopDownOut.size()) {
            handle->dopDownOut.erase(
                handle->dopDownOut.begin(),
                handle->dopDownOut.begin() +
                    static_cast<std::ptrdiff_t>(handle->dopDownRead));
            handle->dopDownRead = 0;
        }
        while ((handle->dopDownOut.size() - handle->dopDownRead) / ch < 2) {
            // Reused buffer: this runs inside the render callback.
            std::vector<uint8_t>& in = handle->dopDownIn;
            if (handle->dopDsf) {
                if (!handle->dopDsf->readBlockRaw(handle->dopBlock, &handle->dopRaw)) {
                    return false;
                }
                ++handle->dopBlock;
                const size_t blockSize = handle->dopDsf->fmt.blockSize;
                if (handle->dopRaw.size() < blockSize * ch) return false;
                in.resize(blockSize * ch);
                for (size_t i = 0; i < blockSize; ++i) {
                    for (size_t c = 0; c < ch; ++c) {
                        in[i * ch + c] = handle->dopRaw[c * blockSize + i];
                    }
                }
            } else if (handle->dopDff) {
                const uint64_t remaining = handle->dopDff->fmt.dataBytes - handle->dopDataOffset;
                if (remaining == 0) return false;
                size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, 32768));
                want -= want % ch;
                if (want == 0) return false;
                in.resize(want);
                if (!handle->dopDff->readData(handle->dopDataOffset, in.data(), want)) {
                    return false;
                }
                handle->dopDataOffset += want;
            } else {
                return false;
            }
            handle->dopDownrate->feed(in.data(), in.size(), &handle->dopDownOut);
        }
        return true;
    };

    auto nextPair = [&](size_t channel, uint8_t* first, uint8_t* second) -> bool {
        if (handle->dopDownrate) {
            if ((handle->dopDownOut.size() - handle->dopDownRead) / ch < 2 &&
                !refillDown()) {
                return false;
            }
            if ((handle->dopDownOut.size() - handle->dopDownRead) / ch < 2) return false;
            *first = handle->dopDownOut[handle->dopDownRead + channel];
            *second = handle->dopDownOut[handle->dopDownRead + ch + channel];
            if (++handle->dopPairChannel == ch) {
                handle->dopPairChannel = 0;
                handle->dopDownRead += 2 * ch;
            }
            return true;
        }
        if (handle->dopDsf) {
            if (handle->dopFrameInBlock >= handle->dopBlockFrames) {
                if (!handle->dopDsf->readBlockRaw(handle->dopBlock, &handle->dopRaw)) {
                    return false;
                }
                ++handle->dopBlock;
                handle->dopFrameInBlock = 0;
                handle->dopBlockFrames = handle->dopDsf->fmt.blockSize / 2;
            }
            const size_t base = channel * handle->dopDsf->fmt.blockSize +
                                handle->dopFrameInBlock * 2;
            if (base + 1 >= handle->dopRaw.size()) return false;
            *first = handle->dopRaw[base];
            *second = handle->dopRaw[base + 1];
            return true;
        }
        if (handle->dopDff) {
            if (handle->dopRawPos + 2 * ch > handle->dopRawBytes) {
                const uint64_t remaining = handle->dopDff->fmt.dataBytes - handle->dopDataOffset;
                if (remaining == 0) return false;
                const size_t want = static_cast<size_t>(
                    std::min<uint64_t>(remaining, 16384));
                handle->dopRaw.resize(want);
                if (!handle->dopDff->readData(handle->dopDataOffset, handle->dopRaw.data(),
                                              want)) {
                    return false;
                }
                handle->dopDataOffset += want;
                handle->dopRawBytes = want;
                handle->dopRawPos = 0;
            }
            *first = handle->dopRaw[handle->dopRawPos + channel];
            *second = handle->dopRaw[handle->dopRawPos + ch + channel];
            return true;
        }
        return false;
    };

    if (!handle->clientRunning) {
        // Prefill a half buffer with valid DoP silence before starting.
        BYTE* raw = nullptr;
        UINT32 chunk = 0;
        const UINT32 prefill = std::max<UINT32>(1, handle->bufferFrames / 2);
        if (SUCCEEDED(neiro::wasapi::getRenderBuffer(handle->render.Get(), prefill, &raw,
                                                     &chunk)) &&
            raw) {
            for (UINT32 f = 0; f < chunk; ++f) {
                const uint8_t marker = dop::nextMarker(&handle->dopMarker);
                for (size_t c = 0; c < ch; ++c) {
                    writeWord(raw + f * bytesPerFrame + c * bytesPerSample,
                              dop::word(marker, 0, 0));
                }
            }
            remember(handle->render->ReleaseBuffer(chunk, 0));
        }
        const HRESULT startHr = remember(handle->client->Start());
        if (FAILED(startHr)) {
            if (deviceIsGone(startHr)) return NAE_ERR_DEVICE_LOST;
            return NAE_ERR_INITIALIZE;
        }
        handle->clientRunning = true;
    }

    const int64_t target = static_cast<int64_t>(milliseconds) * carrier / 1000;
    int64_t written = 0;
    bool ended = false;
    const bool traceDop = std::getenv("NAE_TRACE_DOP") != nullptr;
    while (written < target) {
        const double bufferStart = neiro::wasapi::nowMsHr();
        if (handle->renderEvent) {
            if (WaitForSingleObject(handle->renderEvent, 2000) != WAIT_OBJECT_0) {
                ++handle->underrunCount;
            }
        }
        UINT32 padding = 0;
        const HRESULT padHr = remember(handle->client->GetCurrentPadding(&padding));
        if (FAILED(padHr)) {
            // A device that has gone away fails this call every time, so a bare
            // `continue` spins forever with `written` never advancing -- the
            // session thread hangs inside the device period and `NAE_SessionStop`
            // never joins. This is the same call `NAE_PlaySine` had to be taught
            // about ("the one that was missed, and it is the one that fires first
            // in practice"), and the DoP path never learned it: NAE_ERR_DEVICE_LOST
            // was unreachable here.
            if (traceDop) std::fprintf(stderr, "[dop] padding failed hr=0x%08X\n",
                                       static_cast<unsigned>(padHr));
            if (deviceIsGone(padHr)) {
                g_lastErrorText = "the output device was removed or is being used by "
                                  "another program; playback is paused at the current "
                                  "position";
                return NAE_ERR_DEVICE_LOST;
            }
            continue;
        }
        const UINT32 capacity =
            padding < handle->bufferFrames ? handle->bufferFrames - padding : 0;
        if (traceDop && capacity < handle->bufferFrames) {
            std::fprintf(stderr, "[dop] padding=%u capacity=%u\n", padding, capacity);
        }
        if (capacity == 0) {
            if (handle->renderEvent) WaitForSingleObject(handle->renderEvent, 1);
            else SwitchToThread();
            continue;
        }
        BYTE* raw = nullptr;
        UINT32 chunk = 0;
        const HRESULT bufHr = neiro::wasapi::getRenderBuffer(handle->render.Get(),
                                                             capacity, &raw, &chunk);
        if (bufHr == AUDCLNT_E_BUFFER_TOO_LARGE || bufHr == S_FALSE) {
            if (traceDop) {
                std::fprintf(stderr, "[dop] not-ready hr=0x%08X after %.2f ms\n",
                             static_cast<unsigned>(bufHr),
                             neiro::wasapi::nowMsHr() - bufferStart);
            }
            continue;
        }
        if (FAILED(bufHr)) {
            if (deviceIsGone(bufHr)) return NAE_ERR_DEVICE_LOST;
            return NAE_ERR_INITIALIZE;
        }
        if (!raw || chunk == 0) continue;

        UINT32 filled = 0;
        for (; filled < chunk; ++filled) {
            const uint8_t frameMarker = dop::nextMarker(&handle->dopMarker);
            bool complete = true;
            for (size_t c = 0; c < ch; ++c) {
                const uint8_t marker = frameMarker;
                uint8_t first = 0, second = 0;
                const size_t readChannel = swapChannels ? (c ^ 1) : c;
                if (!nextPair(readChannel, &first, &second)) {
                    complete = false;
                    break;
                }
                uint32_t word;
                if (handle->dopDsf) {
                    word = dop::dsfWord(marker, first, second);
                } else if (swapBytes) {
                    word = dop::word(marker, second, first);
                } else if (reverseDff) {
                    word = dop::dsfWord(marker, first, second);
                } else {
                    word = dop::dffWord(marker, first, second);
                }
                writeWord(raw + filled * bytesPerFrame + c * bytesPerSample, word);
            }
            if (!complete) {
                // Finish the buffer with DoP silence, then end.
                for (size_t c = 0; c < ch; ++c) {
                    writeWord(raw + filled * bytesPerFrame + c * bytesPerSample,
                              dop::word(frameMarker, 0, 0));
                }
                ended = true;
                break;
            }
            if (handle->dopDsf) ++handle->dopFrameInBlock;
            else handle->dopRawPos += 2 * ch;
        }
        remember(handle->render->ReleaseBuffer(chunk, 0));
        if (traceDop) {
            std::fprintf(stderr, "[dop] chunk=%u filled=%u total=%.2f ms\n", chunk, filled,
                         neiro::wasapi::nowMsHr() - bufferStart);
        }
        written += chunk;
        handle->dopFramesPlayed =
            std::min(handle->dopFramesTotal, handle->dopFramesPlayed + static_cast<int64_t>(chunk));
        handle->sourcePos = handle->dopFramesPlayed;
        if (ended) break;
    }

    // Stop only when the file is actually over.
    //
    // This used to stop whenever the slice budget was met, and the session
    // calls this every 16 ms against a 50 ms buffer: the next call then found
    // the stream stopped, prefilled half a buffer with packer silence and
    // restarted it. DoP inside a session therefore played a fraction of the
    // track at nowhere near real time, with a stop/start boundary 60 times a
    // second -- and the half-buffer prefill is silence the engine does not
    // count as audio. The CLI's single-call path never crossed a slice, which
    // is why the hardware result in VERIFICATION.md looked clean.
    //
    // The same rule the sine loop follows: a slice that ends mid-track leaves
    // the client running, because "stopped" has to be said by NAE_Stop rather
    // than inferred from a call returning.
    if (ended) {
        if (handle->clientRunning) {
            neiro::wasapi::stopClient(handle->client.Get());
            handle->clientRunning = false;
        }
    }
    return NAE_OK;
}

int32_t NAE_Seek(NAE_Handle* handle, double seconds) NAE_NOEXCEPT {
    if (!handle) return NAE_ERR_NOT_OPEN;
    if (handle->dopMode) return NAE_ERR_INVALID_ARGUMENT;
    if (handle->nativeSinkMode) {
        g_lastErrorText = "a native DSD sink cannot seek";
        return NAE_ERR_INVALID_ARGUMENT;
    }
#ifdef NEIRO_HAVE_FFMPEG
    if (!handle->decoder) {
        g_lastErrorText = "cannot seek: this stream is a generated tone";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    stopDecodeThread(handle);
    std::string err;
    if (!handle->decoder->seek(seconds, &err)) {
        g_lastErrorText = err;
        return NAE_ERR_CANNOT_OPEN;
    }
    // Drop everything queued. The frames in the fifo are audio from before the
    // seek; playing them would make the jump audible as the old music continuing
    // over the new position.
    handle->fifo.clear();
    handle->sourcePos = 0;
    handle->sourceExhausted = false;

    // And the resampler's own history, which is the part that is easy to forget.
    //
    // soxr is a polyphase filter: it holds the last few input frames of every
    // phase and blends them into each output sample. Those frames belong to the
    // music *before* the seek. Carrying them across means the first few
    // milliseconds after a jump are the tail of the old position mixed into the
    // new one -- an audible smear exactly where the listener is most likely to be
    // listening, because they jumped there on purpose.
    //
    // `reset()` discards the history without putting the resampler into its
    // flushing state, which is the distinction that matters: `flush()` is
    // permanent and calling it here is what used to write to a null pointer.
    if (handle->resampler.ready()) {
        handle->resampler.reset();
    }
    // The base is the *requested* position, not where the container actually
    // landed. A compressed seek is approximate, and reporting the request is the
    // honest choice: the listener asked for 1:00 and the bar should read 1:00,
    // not jump to some packet boundary they never asked for.
    handle->seekBaseFrames = static_cast<int64_t>(seconds * handle->sourceRate);
    startDecodeThread(handle);
    return NAE_OK;
#else
    (void)seconds;
    g_lastErrorText = "this build has no decoder";
    return NAE_ERR_INVALID_ARGUMENT;
#endif
}

double NAE_Position(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return 0.0;
    if (handle->nativeSinkMode) {
        const uint32_t rate = handle->nativeRate ? handle->nativeRate : 1;
        return static_cast<double>(handle->sourcePos) * 8.0 / static_cast<double>(rate);
    }
    const uint32_t rate = handle->sourceRate ? handle->sourceRate
                                             : handle->fmt.Format.nSamplesPerSec;
    if (!rate) return 0.0;
    // Frames since the start of the file, not since the last seek. See
    // seekBaseFrames on the handle.
    return static_cast<double>(handle->seekBaseFrames + handle->sourcePos) / rate;
}

double NAE_Duration(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return 0.0;
    if (handle->nativeSinkMode) return handle->nativeDurationMs / 1000.0;
    if (handle->dopMode) {
        if (handle->dopCarrierRate && handle->dopFramesTotal > 0) {
            return static_cast<double>(handle->dopFramesTotal) /
                   static_cast<double>(handle->dopCarrierRate);
        }
        return 0.0;
    }
#ifdef NEIRO_HAVE_FFMPEG
    if (handle->decoder && handle->sourceRate) {
        // The decoder's own duration, not the padded frame count: the UI's
        // progress bar should end where the music ends, not half a second after.
        const double ms = handle->decoder->info().duration_ms;
        if (ms > 0.0) return ms / 1000.0;
    }
#endif
    return 0.0;
}

int32_t NAE_GetLastErrorText(char* buf, int32_t bufLen) NAE_NOEXCEPT {
    if (!buf || bufLen <= 0) return 0;
    const std::string& s = g_lastErrorText;
    const size_t n = s.size() < static_cast<size_t>(bufLen) - 1 ? s.size()
                                                               : static_cast<size_t>(bufLen) - 1;
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    // The byte count, not NAE_OK. The header documents it that way, and a caller
    // that reads the return value as a length is the only way to tell "the engine
    // had nothing to say" (0) from "here is the message" — which returning
    // NAE_OK made impossible, since NAE_OK is itself 0. That mismatch is why
    // every error reached the UI as its bare code until it was found.
    return static_cast<int32_t>(n);
}

char* NAE_PipelineStats(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return nullptr;
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "{\"fifoFrames\":%.0f,\"sourceAdvance\":%.0f,\"deviceWritten\":%.0f,"
                  "\"produced\":%.0f,\"consumed\":%.0f,\"shortRender\":%.0f,"
                  "\"decodeThread\":%d}",
                  handle->lastFifoFrames, handle->lastSourceAdvance,
                  handle->lastDeviceWritten, handle->lastProduced, handle->lastConsumed,
                  handle->lastShortRender, handle->decodeRunning ? 1 : 0);
    // Allocated with malloc so the host can release it with NAE_FreeString, the
    // same contract as NAE_EnumerateDevices. A caller that frees this with its own
    // deallocator is relying on the two heaps happening to agree.
    char* out = static_cast<char*>(std::malloc(std::strlen(buf) + 1));
    if (!out) return nullptr;
    std::memcpy(out, buf, std::strlen(buf) + 1);
    return out;
}

int32_t NAE_OpenExclusive(const char* deviceId, int32_t sourceRate, int32_t outputMode,
                          int32_t fixedRate, int32_t quality, NAE_Handle** out,
                          int32_t* sampleRate, int32_t* sourceRateOut, int32_t* channels,
                          int32_t* bitsPerSample, int32_t* validBits, int32_t* isFloat,
                          int32_t* resampling, int32_t* dithered, int32_t* bitPerfect,
                          double* latencyMs) NAE_NOEXCEPT {
    try {
        return openExclusiveImpl(deviceId, sourceRate, outputMode, fixedRate, quality,
                                 AUDCLNT_SHAREMODE_EXCLUSIVE, out,
                                 sampleRate, sourceRateOut, channels, bitsPerSample, validBits,
                                 isFloat, resampling, dithered, bitPerfect, latencyMs,
                                 NAE_BUFFER_AUTO, 0);
    } catch (...) {
        if (out) *out = nullptr;
        return NAE_ERR_NO_MEMORY;
    }
}

// The share mode a stream is opened in.
//
// It is a parameter rather than a constant because there are now two entry
// points and the two differ in exactly this one thing. Everything else -- the
// negotiation, the rate choice, the decoder, the render loop -- is shared, so
// threading a mode through is the whole of the change and there is no second
// implementation to drift.
//
// The mode also decides what may be *claimed*: a shared stream goes through the
// Windows mixer, so `bitPerfect` is forced false with `PerfectReason::SharedMode`
// whatever the rate negotiation concluded. A shared stream that happens to run at
// the source rate is still not bit-perfect, and reporting it as such is the exact
// claim this project does not make.
int32_t openExclusiveImpl(const char* deviceId, int32_t sourceRate, int32_t outputMode,
                            int32_t fixedRate, int32_t quality, AUDCLNT_SHAREMODE shareMode,
                            NAE_Handle** out,
                            int32_t* outSampleRate, int32_t* outSourceRate,
                            int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                            int32_t* isFloat, int32_t* resampling, int32_t* dithered,
                            int32_t* bitPerfect, double* latencyMs,
                            int32_t bufferMode, uint32_t bufferMs) {
    if (!out) return NAE_ERR_NO_DEVICE;
    *out = nullptr;

    // A null device id means "whatever Windows is using right now".
    //
    // This is the product's first choice in the output dropdown, and it has to be
    // resolved here, at open time, rather than from a value the UI captured
    // earlier: a user can plug in headphones between choosing and pressing play,
    // and a stale id would then send the audio to the speaker they just unplugged.
    //
    // The sine path already did this, so the two entry points disagreed about
    // what a null id meant -- one treated it as "the default", the other as an
    // error, and the file path inherited the stricter reading.
    std::string resolvedId;
    // NAE_GetDefaultDeviceId hands back a malloc'd string. There are a dozen exit
    // paths below, so the free is a guard rather than a line at each one -- a
    // missed free here is a leak on every play, and a leak on every play is the
    // kind of bug that only shows up in a long session.
    std::unique_ptr<char, void (*)(void*)> defaultIdFree(nullptr, [](void* p) { std::free(p); });
    if (!deviceId) {
        char* def = NAE_GetDefaultDeviceId();
        if (!def || !*def) {
            if (def) std::free(def);
            g_lastErrorText = "Windows reports no default output device";
            return NAE_ERR_NO_DEVICE;
        }
        defaultIdFree.reset(def);
        resolvedId = def;
        deviceId = resolvedId.c_str();
    }

    // Zero every out-param before doing any work, so a failure still leaves
    // the caller with defined values rather than whatever was on the stack.
    if (outSampleRate) *outSampleRate = 0;
    if (outSourceRate) *outSourceRate = 0;
    if (channels) *channels = 0;
    if (bitsPerSample) *bitsPerSample = 0;
    if (validBits) *validBits = 0;
    if (isFloat) *isFloat = 0;
    if (resampling) *resampling = 0;
    if (dithered) *dithered = 0;
    if (bitPerfect) *bitPerfect = 0;
    if (latencyMs) *latencyMs = 0.0;

    if (FAILED(ensureCom())) return NAE_ERR_COM;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        return NAE_ERR_NO_DEVICE;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(std::wstring(deviceId, deviceId + strlen(deviceId)).c_str(),
                                     &device)) ||
        !device) {
        return NAE_ERR_NO_DEVICE;
    }

    WAVEFORMATEX native{};
    uint16_t nativeValidBits = 0;
    if (!readNativeFormat(device.Get(), &native, &nativeValidBits)) {
        return NAE_ERR_NO_NATIVE_FORMAT;
    }

    // Decide the rate to open at. This is the whole point of the mode
    // parameter: the endpoint's native rate is only the default, not a
    // constraint. A 44.1 kHz file on a 192 kHz interface is a conversion, not a
    // reason to reformat the device down to 44.1 kHz.
    //
    // sourceRate 0 means "use the endpoint's native rate", which is what the
    // older sine-only entry point asks for. Resolved here because the native
    // rate is only known once readNativeFormat has run.
    const uint32_t effectiveSourceRate =
        sourceRate > 0 ? static_cast<uint32_t>(sourceRate) : native.nSamplesPerSec;

    // Ask the driver what it will actually open, once per device.
    //
    // This happens *before* the rate is chosen, because choosing first and
    // opening second is what produced the DSD failure: a DSD64 file decodes to
    // 352.8 kHz, the mode says "352.8 kHz", Initialize refuses, and the listener
    // is told their file is unsupported. It is not; the driver simply does not do
    // 352.8 kHz, and every other file in the library plays.
    RateCache& cache = rateCache();
    std::vector<uint32_t> cachedRates;
    {
        std::lock_guard<std::mutex> lock(rateCacheMutex());
        if (cache.deviceId != deviceId || cache.nativeRate != native.nSamplesPerSec) {
            bool probed = false;
            cache.rates = probeSupportedRates(device.Get(), native, nativeValidBits, &probed);
            cache.deviceId = deviceId;
            cache.nativeRate = native.nSamplesPerSec;
            cache.probed = probed;
        }
        // A copy, so the probe's result is read after the lock is released: the
        // rates are used by the resolver below and the cache may be replaced by
        // another open in between.
        cachedRates = cache.rates;
    }
    const neiro::SupportedRates supported{cachedRates.data(),
                                          static_cast<uint32_t>(cachedRates.size())};

    neiro::OutputTarget target;
    const auto mode = static_cast<neiro::OutputMode>(
        outputMode >= 0 && outputMode <= 2 ? outputMode
                                          : static_cast<int32_t>(neiro::OutputMode::Device));
    if (!neiro::resolveOutputTarget(mode, effectiveSourceRate, native.nSamplesPerSec,
                                    static_cast<uint32_t>(fixedRate > 0 ? fixedRate : 0),
                                    supported, &target)) {
        g_lastErrorText = "no output target satisfies the requested mode";
        return NAE_ERR_INITIALIZE;
    }

    ComPtr<IAudioClient> client;
    if (FAILED(neiro::wasapi::activate(device.Get(), __uuidof(IAudioClient), client.GetAddressOf())) ||
        !client) {
        return NAE_ERR_ACTIVATE;
    }

    WAVEFORMATEXTENSIBLE fmt = wrap(&native, nativeValidBits);

    // The target rate replaces the endpoint's own. Everything else -- valid
    // bits, channel mask, subformat, block align -- is recomputed for it,
    // because carrying a 44.1 kHz blockAlign with a 192 kHz sample rate
    // produces a format the driver rejects.
    if (target.rate != native.nSamplesPerSec) {
        fmt.Format.nSamplesPerSec = target.rate;
        const uint16_t bytesPerSample = static_cast<uint16_t>(fmt.Format.wBitsPerSample / 8);
        fmt.Format.nBlockAlign = static_cast<WORD>(bytesPerSample * fmt.Format.nChannels);
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
    }

    // Read before `Initialize`, because that call is allowed to write back into the
    // format it was handed. Keeping the "before" value is the only way to tell a
    // device that agreed with us from one that quietly changed the depth.
    const uint16_t requestedBits = fmt.Format.wBitsPerSample;

    // The device's own period: 0, 0.
    //
    // An explicit period was tried here and made things worse, which is worth
    // recording because the reasoning behind it was sound and the measurement
    // still said no. Asking for 20 ms did work in the narrow sense -- the device
    // honoured it and the buffer shrank from 50.67 ms to 21.33 ms -- but the
    // delivered rate went from 85.3% to 68.3%, because the device's actual grant
    // interval is longer than whatever buffer size it reports.
    //
    //   reported 50.67 ms -> granted every 62.5 ms  (85.3% of real time)
    //   reported 21.33 ms -> granted every 31.2 ms  (68.3% of real time)
    //
    // The same 32%-ish shortfall in both, which says the period is set by the
    // display link rather than by this request. So the two faults are separate and
    // I was wrong to keep trying to fix them as one: the under-feed is a property
    // of this endpoint, and the stale-buffer bubbling is ours.
    //
    // Note also that `Initialize` may only be called once per client. The old code
    // had a second Initialize behind `if (frames > desired)`, and that guard was
    // not a logic error to be corrected -- calling it again returns
    // AUDCLNT_E_NOT_INITIALIZED, which reads as "the device refused the format".
    // Event-driven, so the engine tells us when the next period is due instead of us
    // polling for it. `SetEventHandle` has to happen first.
    HANDLE renderEvent = createRenderEvent();
    // One argument, not two: the cookie-bearing overload belongs to IAudioClient3,
// and IAudioClient::SetEventHandle takes only the handle.
    if (renderEvent && FAILED(client->SetEventHandle(renderEvent))) {
        CloseHandle(renderEvent);
        renderEvent = nullptr;
    }
    const DWORD streamFlags = renderEvent ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;

    // Buffer policy. Auto/Stable let the backend choose; LowLatency asks for the
    // smallest period the device accepts; Fixed asks for the exact duration.
    // A refused explicit request falls back to the backend default and is
    // reported, rather than failing playback.
    REFERENCE_TIME requestedDuration = 0;
    if (bufferMode == NAE_BUFFER_LOW_LATENCY) {
        requestedDuration = 10 * 10000;  // 10 ms
    } else if (bufferMode == NAE_BUFFER_FIXED && bufferMs > 0) {
        requestedDuration = static_cast<REFERENCE_TIME>(bufferMs) * 10000;
    }

    HRESULT initHr = client->Initialize(shareMode, streamFlags, requestedDuration, 0,
                                        &fmt.Format, nullptr);
    if (FAILED(initHr) && requestedDuration != 0) {
        initHr = client->Initialize(shareMode, streamFlags, 0, 0, &fmt.Format, nullptr);
        if (SUCCEEDED(initHr)) {
            g_lastErrorText = "the device refused the requested buffer; the backend default is used";
        }
    }

    if (FAILED(initHr) && target.rate != native.nSamplesPerSec) {
        // The chosen rate was refused even though the probe said it would be
        // accepted. Rather than stop, fall back to the endpoint's own advertised
        // rate: the one format the driver is obliged to accept, and the one
        // `readNativeFormat` just handed us.
        //
        // This is a race rather than a theory. The probe releases its throwaway
        // client before the real open, so a device reconfigured in between -- a TV
        // renegotiating over HDMI, a driver reloading -- is exactly the case where
        // a list measured a moment ago is no longer true. The cost of the branch
        // is one extra Initialize; the alternative is not playing at all.
        //
        // Re-initialising the same client is only valid because the first attempt
        // failed, which leaves the client uninitialised.
        WAVEFORMATEXTENSIBLE fallbackFmt = wrap(&native, nativeValidBits);
        const HRESULT retry = client->Initialize(shareMode, 0, 0, 0,
                                                  &fallbackFmt.Format, nullptr);
        if (SUCCEEDED(retry)) {
            initHr = retry;
            fmt = fallbackFmt;
            // The reason changes with it. The listener asked for one rate, was
            // refused, and got another. "Resampled to the device rate" is true
            // here, but it was the *driver* that chose this rate and not their
            // setting, and the interface has to be able to say so.
            target.rate = fmt.Format.nSamplesPerSec;
            target.resampling = effectiveSourceRate != target.rate;
            target.bitPerfect = false;
            target.coercedToSupportedRate = true;
            target.reason = neiro::PerfectReason::ResampledToSupportedRate;
        }
    }

    if (FAILED(remember(initHr))) {
        // The single most useful thing this engine can say. Exclusive mode
        // failing has two very different causes with two very different fixes,
        // and the HRESULT alone does not distinguish them for a user:
        //
        //   * the format was rejected -- retrying will not help, a different
        //     device or a different output mode might;
        //   * something else has the endpoint open -- closing that program will.
        //
        // AUDCLNT_E_ENDPOINT_CREATE_FAILED (0x8889000F) is the confusing one:
        // Windows uses it both for a bad format and for "another app holds this
        // device", so it cannot be read as either on its own.
        char detail[128];
        std::snprintf(detail, sizeof(detail), "0x%08X", static_cast<unsigned>(g_lastHr));
        // State the facts and stop. AUDCLNT_E_ENDPOINT_CREATE_FAILED is used by
        // Windows both for a format the endpoint will not accept and for an
        // endpoint another application already holds, so naming either cause here
        // is a guess -- and this project has already made that guess and been
        // wrong. A DSD1024 file in Native mode asks for 5 644 800 Hz, which the
        // HDMI endpoint cannot provide, and it arrives here under exactly the same
        // code as a device held by another program.
        //
        // The caller knows more than this function does: it knows which rate was
        // asked for and whether the endpoint supports it, so the honest message is
        // the request and the result, and the diagnosis belongs one layer up.
        g_lastErrorText = std::string("exclusive mode refused (") + detail + "): " +
                          std::to_string(fmt.Format.nSamplesPerSec) + " Hz, " +
                          std::to_string(fmt.Format.wBitsPerSample) + "-bit " +
                          std::to_string(fmt.Format.nChannels) + "ch on this endpoint";
        return NAE_ERR_INITIALIZE;
    }

    ComPtr<IAudioRenderClient> render;
    if (FAILED(remember(neiro::wasapi::getService(client.Get(), __uuidof(IAudioRenderClient),
                                                  render.GetAddressOf())))) {
        return NAE_ERR_ACTIVATE;
    }

    // The device period, asked for explicitly.
    // `Initialize` was already called above, once. It may not be called again --
    // doing so returns AUDCLNT_E_NOT_INITIALIZED, which reads as the device
    // refusing the format rather than the client refusing to initialise twice.
    // That is what the second Initialize behind `if (frames > desired)` used to do,
    // and its guard was not a logic error to be corrected.
    //
    // An explicit hnsBufferDuration was tried here and made it worse, which is worth
    // recording because the reasoning was sound and the measurement still said no.
    // Asking for 20 ms did work narrowly -- the device honoured it and the buffer
    // shrank from 50.67 ms to 21.33 ms -- but the delivered rate fell from 85.3% to
    // 68.3%, because the device's real grant interval is longer than whatever buffer
    // size it reports:
    //
    //   reported 50.67 ms, granted every 62.5 ms  ->  85.3% of real time
    //   reported 21.33 ms, granted every 31.2 ms  ->  68.3% of real time
    //
    // So the period here belongs to the display link, not to this request, and the
    // under-feed cannot be negotiated away. The two faults are separate: the
    // under-feed is a property of this endpoint, the stale-buffer bubbling is ours.
    const bool tracing = getenv("NAE_TRACE") != nullptr;
    UINT32 frames = 0;
    HRESULT hr = remember(client->GetBufferSize(&frames));
    if (FAILED(hr) || frames == 0) {
        return NAE_ERR_INITIALIZE;
    }
    if (tracing) {
        std::fprintf(stderr, "[nae] granted frames=%u (%.2f ms) rate=%u\n", frames,
                     target.rate ? 1000.0 * frames / target.rate : 0.0, target.rate);
    }

    auto* handle = new (std::nothrow) NAE_Handle();
    if (!handle) {
        return NAE_ERR_NO_MEMORY;
    }
    handle->device = device;
    handle->client = client;
    handle->render = render;
    handle->fmt = fmt;
    // `fmt` has been through `Initialize`, so it now holds whatever the device
    // agreed to -- the format the render loop must write, not the format that was
    // asked for. Keep both, because the difference between them is the single most
    // useful thing to know when the output comes out wrong.
    handle->grantedBits = fmt.Format.wBitsPerSample;
    handle->requestedBits = requestedBits;
    handle->bufferFrames = frames;
    handle->renderEvent = renderEvent;
    handle->latencyMs = 1000.0 * static_cast<double>(frames) / target.rate;
    handle->sourceRate = effectiveSourceRate;
    handle->resampling = target.resampling;
    handle->bitPerfect = target.bitPerfect;
    // The rate decision is the first word on *why*; the conversions that follow
    // can each overrule it, so the final verdict is not copied here but settled
    // after the resampler and dither are configured.
    handle->reason = target.reason;

    if (target.resampling) {
        std::string err;
        const auto q = static_cast<neiro::ResamplerQuality>(
            quality >= 0 && quality <= 2 ? quality : static_cast<int32_t>(neiro::ResamplerQuality::High));
        if (!handle->resampler.prepare(fmt.Format.nChannels, handle->sourceRate,
                                      target.rate, q, &err)) {
            delete handle;
            remember(E_FAIL);
            return NAE_ERR_INITIALIZE;
        }
    }

    // Dither is sized to valid bits, and only engages when the device is
    // narrower than the pipeline's internal precision. A 24-in-32 endpoint
    // therefore passes through untouched, which is correct: there is
    // nothing to dither.
    const uint16_t deviceValidBits = fmt.Samples.wValidBitsPerSample
                                         ? fmt.Samples.wValidBitsPerSample
                                         : fmt.Format.wBitsPerSample;
    handle->dither.set_valid_bits(deviceValidBits);
    handle->dithered = handle->dither.active();

    // Settle the verdict now that every conversion is known.
    //
    // The rate decision was the first word, not the last: a path can be at the
    // right rate and still requantise, and a path can be at the wrong rate with a
    // channel change on top. Reporting the rate reason in that case would be
    // true and useless -- the listener would fix the rate and still not be
    // bit-perfect.
    //
    // The order is deliberate. A channel change loses information the rate cannot
    // restore, so it outranks resampling. A rate the *driver* forced outranks
    // dither, which is the opposite of the order these were first written in and
    // for a good reason: for a DSD file the forced rate is the whole story --
    // "your 352.8 kHz file is playing at 192 kHz because this display cannot do
    // 352.8 kHz" -- while the dither that follows is a consequence of it. Naming
    // the dither would send the listener to a bit-depth control when the thing
    // they need to know is that their hardware cannot carry the file.
    // Shared mode first, because it is the only condition here that the user
    // *chose* and the only one that is not about this stream's own format.
    //
    // Order matters: a shared stream that also got resampled would otherwise be
    // reported as `ResampledToDeviceRate`, sending the listener to an output-mode
    // control that cannot fix it. In shared mode the Windows mixer is in the path
    // whatever we negotiated, and that is the thing worth saying.
    if (shareMode == AUDCLNT_SHAREMODE_SHARED) {
        handle->bitPerfect = false;
        handle->reason = neiro::PerfectReason::SharedMode;
    } else if (handle->fileChannels != 0 &&
        static_cast<int32_t>(fmt.Format.nChannels) != handle->fileChannels) {
        handle->bitPerfect = false;
        handle->reason = neiro::PerfectReason::ChannelConverted;
    } else if (target.coercedToSupportedRate) {
        handle->bitPerfect = false;
        handle->reason = neiro::PerfectReason::ResampledToSupportedRate;
    } else if (handle->dithered) {
        handle->bitPerfect = false;
        handle->reason = neiro::PerfectReason::Dithered;
    } else if (target.resampling) {
        handle->bitPerfect = false;
        handle->reason = target.reason;
    } else {
        handle->bitPerfect = true;
        handle->reason = neiro::PerfectReason::BitPerfect;
    }
    handle->coercedToSupportedRate = target.coercedToSupportedRate;
    // The verdict before the file's own depth is considered. A gapless swap
    // restores this before re-applying the source rule, because that rule only
    // clears -- inheriting the previous track's answer made one wide file
    // poison every later one in the queue.
    handle->sourceBitPerfect = handle->bitPerfect;
    handle->sourceReason = handle->reason;
    // 记下实际开流用的模式，供 `NAE_QuerySignalPath` 如实上报。
    handle->exclusive = (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE);

    if (outSampleRate) *outSampleRate = static_cast<int32_t>(fmt.Format.nSamplesPerSec);
    if (outSourceRate) *outSourceRate = static_cast<int32_t>(effectiveSourceRate);
    if (channels) *channels = static_cast<int32_t>(fmt.Format.nChannels);
    if (bitsPerSample) *bitsPerSample = static_cast<int32_t>(fmt.Format.wBitsPerSample);
    if (validBits) *validBits = static_cast<int32_t>(deviceValidBits);
    if (isFloat) *isFloat = (fmt.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? 1 : 0;
    if (resampling) *resampling = handle->resampling ? 1 : 0;
    if (dithered) *dithered = handle->dithered ? 1 : 0;
    if (bitPerfect) *bitPerfect = handle->bitPerfect ? 1 : 0;
    if (latencyMs) *latencyMs = handle->latencyMs;

    *out = handle;
    return NAE_OK;
}

int32_t NAE_QuerySignalPath(NAE_Handle* handle, NAE_SignalPath* out) NAE_NOEXCEPT {
    // The caller's declared size, read before anything is written.
    //
    // This struct has grown twice, and an host built against an older, smaller
    // header must not have its stack overrun by the tail this build appended.
    // Everything is built in a local and copied out with the caller's limit;
    // `structSize` is set to this build's own size so a caller can see which
    // fields were written.
    if (!out) return NAE_ERR_INVALID_ARGUMENT;
    uint32_t declared = out->structSize;
    const uint32_t full = static_cast<uint32_t>(sizeof(NAE_SignalPath));
    if (declared == 0 || declared > full) declared = full;
    if (declared < sizeof(uint32_t)) declared = sizeof(uint32_t);
    NAE_SignalPath local{};
    local.structSize = full;
    struct Commit {
        NAE_SignalPath* dst;
        const NAE_SignalPath* src;
        uint32_t limit;
        ~Commit() { std::memcpy(dst, src, limit); }
    } commit{out, &local, declared};

    if (!handle) return NAE_OK;

    if (handle->nativeSinkMode) {
        // A host-owned native DSD sink has no WASAPI format; report the stream
        // it is actually writing. Returning the zeroed struct with NAE_OK made
        // a live sink indistinguishable from "not open".
        local.sourceRate = static_cast<int32_t>(handle->nativeRate);
        local.sourceChannels = static_cast<int32_t>(handle->nativeChannels);
        local.sampleRate = static_cast<int32_t>(handle->nativeRate);
        local.channels = static_cast<int32_t>(handle->nativeChannels);
        local.bitsPerSample = 8;
        local.validBits = 1;
        local.isFloat = 0;
        local.resampling = 0;
        local.dithered = 0;
        local.bitPerfect = 1;
        local.reason = 0;
        local.exclusive = 0;
        local.requestedBits = 8;
        local.grantedBits = 1;
        local.bufferLatencyMs = 0.0;
        local.driverLatencyMs = 0.0;
        local.totalLatencyMs = 0.0;
        local.bufferFrames = 0;
        return NAE_OK;
    }
    if (handle->dopMode) {
        local.sourceRate = handle->fileSampleRate;
        local.sourceChannels = handle->fileChannels;
        local.sampleRate = static_cast<int32_t>(handle->fmt.Format.nSamplesPerSec);
        local.channels = static_cast<int32_t>(handle->fmt.Format.nChannels);
        local.bitsPerSample = static_cast<int32_t>(handle->fmt.Format.wBitsPerSample);
        local.validBits = static_cast<int32_t>(handle->fmt.Samples.wValidBitsPerSample);
        local.isFloat = 0;
        local.resampling = 0;
        local.dithered = 0;
        local.bitPerfect = 1;
        local.reason = 0;
        local.exclusive = 1;
        local.requestedBits = local.bitsPerSample;
        local.grantedBits = local.validBits;
        local.bufferLatencyMs = handle->bufferDurationMs;
        local.driverLatencyMs = handle->latencyMs;
        local.totalLatencyMs = handle->bufferDurationMs + handle->latencyMs;
        local.bufferFrames = static_cast<int32_t>(handle->bufferFrames);
        return NAE_OK;
    }
    local.sourceRate = static_cast<int32_t>(handle->sourceRate);
    local.sourceChannels = static_cast<int32_t>(handle->fileChannels);
    local.sampleRate = static_cast<int32_t>(handle->fmt.Format.nSamplesPerSec);
    local.channels = static_cast<int32_t>(handle->fmt.Format.nChannels);
    local.bitsPerSample = static_cast<int32_t>(handle->fmt.Format.wBitsPerSample);
    local.validBits = static_cast<int32_t>(handle->fmt.Samples.wValidBitsPerSample);
    local.isFloat = (handle->fmt.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ? 1 : 0;
    local.resampling = handle->resampling ? 1 : 0;
    local.dithered = handle->dithered ? 1 : 0;
    local.bitPerfect = handle->bitPerfect ? 1 : 0;
    local.exclusive = handle->exclusive ? 1 : 0;
    local.reason = static_cast<int32_t>(handle->reason);
    // Volume is a runtime choice, so it is applied here rather than folded into
    // the open-time flag. A more fundamental reason (shared, channel, rate) is
    // kept: naming the volume would send the listener to the wrong control.
    if (local.bitPerfect && handle->eqEnabled) {
        local.bitPerfect = 0;
        local.reason = NAE_REASON_EQ;
    } else if (local.bitPerfect && handle->crossfeedEnabled) {
        local.bitPerfect = 0;
        local.reason = NAE_REASON_CROSSFEED;
    } else if (local.bitPerfect && handle->convEnabled) {
        local.bitPerfect = 0;
        local.reason = NAE_REASON_CONVOLVER;
    } else if (local.bitPerfect && handle->crossfadeEnabled &&
               handle->crossfadeFrames > 0) {
        local.bitPerfect = 0;
        local.reason = NAE_REASON_CROSSFADE;
    } else if (local.bitPerfect && (handle->rgTarget != 1.0 || handle->rgRamp > 0)) {
        local.bitPerfect = 0;
        local.reason = NAE_REASON_REPLAYGAIN;
    } else if (local.bitPerfect &&
               (handle->volume != 1.0 || handle->volumeRampRemaining > 0)) {
        local.bitPerfect = 0;
        local.reason = NAE_REASON_VOLUME_NOT_UNITY;
    }
    local.coercedToSupportedRate = handle->coercedToSupportedRate ? 1 : 0;
    local.fifoPeak = handle->fifoPeak.load(std::memory_order_relaxed);
    local.writtenPeak = handle->writtenPeak.load(std::memory_order_relaxed);
    local.devicePeak = handle->devicePeak.load(std::memory_order_relaxed);
    local.requestedBits = handle->requestedBits;
    local.grantedBits = handle->grantedBits;

    // Latency, split. The buffer part is the audio we have queued and the
    // listener has not heard; the driver part is what WASAPI reports for the
    // stream, which is downstream of us.
    //
    // `GetStreamLatency` is read on every call rather than cached at open,
    // because the device is allowed to change it while the stream runs and a
    // cached number would be a number about the past. A failure is reported as
    // zero rather than folded into the buffer figure: "we do not know" and "the
    // driver says nothing" must not add up to the same display.
    REFERENCE_TIME driverTicks = 0;
    double driverLatencyMs = 0.0;
    if (handle->client) {
        const UINT32 rate = handle->fmt.Format.nSamplesPerSec;
        if (SUCCEEDED(handle->client->GetStreamLatency(&driverTicks)) && rate > 0) {
            // REFERENCE_TIME is 100-nanosecond units.
            driverLatencyMs = static_cast<double>(driverTicks) / 10000.0;
        }
    }
    local.bufferFrames = static_cast<int32_t>(handle->bufferFrames);
    local.channelDelayMaxFrames = static_cast<int32_t>(handle->channelDelayMax);
    local.bufferLatencyMs = handle->latencyMs;
    local.driverLatencyMs = driverLatencyMs;
    // Stages that delay the signal add to the end-to-end figure. A linear-phase
    // EQ and the convolver hold samples back, and the clip guard delays by its
    // lookahead; a host that syncs video to `totalLatencyMs` needs them.
    {
        const double rate = handle->fmt.Format.nSamplesPerSec;
        double dspMs = 0.0;
        if (rate > 0.0) {
            if (handle->eqEnabled && handle->eqLinear) {
                dspMs += 1000.0 * static_cast<double>(handle->linearEq.latencyFrames()) / rate;
            }
            if (handle->convEnabled) {
                dspMs += 1000.0 * static_cast<double>(neiro::PartitionedConvolver::blockSize()) / rate;
            }
            if (handle->clipGuard.active()) {
                dspMs += 1000.0 * static_cast<double>(handle->clipGuard.lookahead_frames()) / rate;
            }
        }
        local.totalLatencyMs = handle->latencyMs + driverLatencyMs + dspMs;
    }

    local.callbackCount = handle->callbackCount;
    local.deadlineMissCount = handle->deadlineMissCount;
    local.peakCallbackMs = handle->peakCallbackMs;
    local.underrunCount = handle->underrunCount;
    local.dropCount = handle->dropCount;
    local.padFrameCount = handle->padFrameCount;
    local.maxBufferGapMs = handle->maxBufferGapMs;
    local.maxDecodeStallMs = handle->maxDecodeStallMs;
    local.maxFillMs = handle->maxFillMs;
    local.maxLateMs = handle->maxLateMs;
    local.lateCount = handle->lateCount;
    local.worstSeamRatio = handle->worstSeamRatio;
    local.seamSlewRef = handle->slewRef / std::max(1e-9, static_cast<double>(handle->fifoPeak.load(std::memory_order_relaxed)));
    local.seamCount = handle->seamCount;

    // The distribution, summarised at query time from the retained samples.
    //
    // Computed here rather than tracked incrementally because a quantile cannot be
    // maintained in constant memory: it depends on the whole set. The retained
    // sample is capped, so this describes the *first* `seamSampleCount` seams
    // rather than all of them -- stated because a quantile over a capped prefix is
    // not the same number as one over everything, and `seamSampleCount` is
    // reported so the reader can tell which they are looking at.
    local.seamMean = handle->seamCount > 0
                        ? static_cast<double>(handle->seamRatioTotal) / 1000.0 /
                              static_cast<double>(handle->seamCount)
                        : 0.0;
    local.seamSampleCount = static_cast<uint32_t>(handle->seamSampleCount);
    for (size_t q = 0; q < kSeamQuantiles; ++q) {
        local.seamQuantiles[q] = 0.0;
    }
    if (handle->seamSampleCount > 0) {
        // A local copy, because nth_element reorders and the retained order is the
        // order the seams happened in -- which is itself information, and a caller
        // wanting it should get it undisturbed.
        std::vector<float> sorted(handle->seamSample,
                                  handle->seamSample + handle->seamSampleCount);
        std::sort(sorted.begin(), sorted.end());
        for (size_t q = 0; q < kSeamQuantiles; ++q) {
            // 0%, 12.5%, 25%, ... 50%, ... 100%. The top quantile is deliberately
            // the maximum of the retained sample, so it can be compared against
            // `worstSeamRatio` -- and a difference between them means the cap was
            // reached, which is exactly the case a reader needs to notice.
            const double frac = static_cast<double>(q) /
                                static_cast<double>(kSeamQuantiles - 1);
            size_t idx = static_cast<size_t>(frac * static_cast<double>(sorted.size() - 1));
            if (idx >= sorted.size()) idx = sorted.size() - 1;
            local.seamQuantiles[q] = static_cast<double>(sorted[idx]);
        }
    }
    for (size_t b = 0; b < kSeamBuckets; ++b) {
        local.seamHistogram[b] = handle->seamHistogram[b];
    }
    local.seamHistogramMax = kSeamBuckets;
    local.deviceRateActual = handle->deviceRateActual;
    local.silenceFramesWritten = static_cast<uint64_t>(handle->silenceFramesWritten);
    local.bufferDurationMs = handle->fmt.Format.nSamplesPerSec
                              ? 1000.0 * static_cast<double>(handle->bufferFrames) /
                                    static_cast<double>(handle->fmt.Format.nSamplesPerSec)
                              : 0.0;
    local.deviceLostCount = handle->deviceLostCount;
    local.recoveryCount = handle->recoveryCount;
    return NAE_OK;
}

int32_t NAE_QuerySupportedRates(NAE_Handle* handle, uint32_t* rates, uint32_t capacity,
                                uint32_t* outCount) NAE_NOEXCEPT {
    if (outCount) *outCount = 0;

    std::vector<uint32_t> cached;
    bool probed = false;
    {
        std::lock_guard<std::mutex> lock(rateCacheMutex());
        const RateCache& cache = rateCache();
        probed = cache.probed;
        cached = cache.rates;
    }
    if (probed && !cached.empty()) {
        if (outCount) *outCount = static_cast<uint32_t>(cached.size());
        if (rates && capacity > 0) {
            const uint32_t n = cached.size() < capacity
                                   ? static_cast<uint32_t>(cached.size())
                                   : capacity;
            std::memcpy(rates, cached.data(), n * sizeof(uint32_t));
        }
        return NAE_OK;
    }

    // Not probed yet, or the probe found nothing at all -- which is contention
    // rather than a capability answer. NAE_OK with a count of zero, and the
    // engine keeps its previous behaviour. A hard error here would be wrong: the
    // caller has no way to distinguish "this device supports no rates" from "I
    // could not ask", and treating the second as the first stops playback.
    if (!handle) return NAE_OK;

    return NAE_OK;
}

// The share mode a stream is opened in.
//
// It is a parameter rather than a constant because there are now two entry
// points and the two differ in exactly this one thing. Everything else -- the
// negotiation, the rate choice, the decoder, the render loop -- is shared, so
// threading a mode through is the whole of the change and there is no second
// implementation to drift.
//
// The mode also decides what may be *claimed*: a shared stream goes through the
// Windows mixer, so `bitPerfect` is forced false with `PerfectReason::SharedMode`
// whatever the rate negotiation concluded. A shared stream that happens to run at
// the source rate is still not bit-perfect, and reporting it as such is the exact
// claim this project does not make.
int32_t openExclusiveImpl(const char* deviceId, int32_t sourceRate, int32_t outputMode,
                            int32_t fixedRate, int32_t quality, AUDCLNT_SHAREMODE shareMode,
                            NAE_Handle** out,
                            int32_t* outSampleRate, int32_t* outSourceRate,
                            int32_t* channels, int32_t* bitsPerSample, int32_t* validBits,
                            int32_t* isFloat, int32_t* resampling, int32_t* dithered,
                            int32_t* bitPerfect, double* latencyMs,
                            int32_t bufferMode, uint32_t bufferMs);  // may throw

/// Add device frames to the delivered-rate window, and publish when it is long enough.
///
/// A free function rather than a block inside the render loop, because two paths now
/// feed it: the audio path and the paused path that hands the device silence. When
/// only one of them did, the window was defined by whichever path happened to hold
/// the code, and the other one silently fell outside the definition.
///
/// Counts every frame the device was handed, silence included. The claim this makes
/// is "device frames per second of wall clock", and the device consumes silence at
/// the rate it consumes audio -- a device clock running slow is just as visible
/// through a pause as through music. `silenceFramesWritten` is the field that says a
/// pause happened; one field, one claim, rather than one field carrying both.
///
/// Two corrections are folded in here, both because the window used to include the
/// start-up transient and so described the first moments of a stream rather than the
/// stream. It read 98.5% before the bubbling fix and 98.4% after it: the click went
/// away and this did not move, which is what made it suspect. The same error had
/// produced a "0.9701 of real time" reading sitting next to a drift of 0.6 ms with no
/// visible contradiction between them.
///
/// So the first 3 s is discarded outright -- opening a stream costs real time and that
/// cost is not playback -- and the clock is QueryPerformanceCounter rather than
/// GetTickCount64, whose 15.6 ms tick is coarse against a 53 ms buffer.
static void chargeRateWindow(NAE_Handle* handle, int64_t frames) {
    if (!handle) return;
    const double now = neiro::wasapi::nowMsHr();
    if (handle->rateWindowStart == 0.0) {
        handle->rateWindowStart = now;
        handle->rateWindowFrames = 0;
    }
    handle->rateWindowFrames += frames;
    const double elapsed = now - handle->rateWindowStart;
    if (elapsed >= 3000.0) {
        handle->rateWindowStart = now;
        handle->rateWindowFrames = 0;
        handle->rateWarmupDone = true;
    } else if (handle->rateWarmupDone && elapsed >= 500.0) {
        handle->deviceRateActual =
            1000.0 * static_cast<double>(handle->rateWindowFrames) / elapsed;
        handle->rateWindowStart = now;
        handle->rateWindowFrames = 0;
    }
}

/// Window length for the output tap, in device frames.
///
/// A power of two because the transform is radix-2, and 4096 because it is the
/// smallest length that gives useful resolution across the whole audible band at
/// every rate this engine opens: at 44.1 kHz one bin is 10.8 Hz, which is narrow
/// enough to see a single harmonic, and at 192 kHz it is 46.9 Hz.
///
/// It is also the length that decides what the display can resolve. 2048 at 44.1
/// kHz is 21.5 Hz per bin, which smears adjacent harmonics together, and 8192 is
/// 92 ms of history -- long enough that a drum hit is gone before the frame that
/// would show it is drawn. 4096 is 93 ms at 44.1 kHz and 21 ms at 192 kHz, and the
/// asymmetry is stated rather than hidden: the display is sharper on a
/// high-rate path because the same window is shorter in time there.
static constexpr size_t kSpectrumFft = 4096;

/// 包络的帧间隔，毫秒。
///
/// 与 `src/scripts/audio/tempo.js` 的 `HOP_MS` 取同一个值。两条路由必须用同一个
/// 帧间隔，否则 exclusive 和 Web Audio 会给出不同的 BPM，而两者都看起来是对的——
/// 那正是这个项目最难查的那类 bug。数值选 5 ms 是沿用前端的既有取值，不是重新
/// 调优的结果：BPM 的分辨力由后面的自相关决定，帧间隔只需要细过一个最短的音符间隔
/// （十六分音符在 180 BPM 下是 83 ms，5 ms 是它的 1/16）。
constexpr double kEnvelopeHopMs = 5.0;

/// 包络保留的帧数。
///
/// 4096 帧 × 5 ms = 20.5 秒。选它是因为节拍检测的自相关窗口需要覆盖至少 4 个
/// 最长拍周期：60 BPM 是 1 秒，4 个是 4 秒，20 秒有五倍余量。
///
/// 也因为它和 `kSpectrumFft` 同为 4096，这个数字不是巧合调出来的，而是让两条
/// 通路的心跳一致，方便对照。
constexpr size_t kEnvelopeFrames = 4096;

/// Feed `frames` device frames of channel 0 into the tap. `samples` may be null,
/// which means silence.
///
/// Silence is fed rather than skipped. A pause that left the previous window in
/// place would leave a spectrum on screen that looked exactly like live audio,
/// and a paused player showing live audio is a claim it is not making. The cost
/// is one store per frame, which is the same cost the audio branch pays.
static void feedSpectrum(NAE_Handle* handle, const double* samples, size_t frames) {
    if (!handle) return;
    if (handle->spectrumRing.empty()) handle->spectrumRing.assign(kSpectrumFft, 0.0);
    if (frames == 0) return;
    std::lock_guard<std::mutex> guard(handle->spectrumLock);
    // A rate change invalidates the window: a ring holding samples from two rates
    // is not a measurement of either, so it is emptied rather than blended.
    const uint32_t rate = handle->fmt.Format.nSamplesPerSec;
    if (handle->spectrumRate != rate) {
        handle->spectrumRate = rate;
        handle->spectrumRing.assign(kSpectrumFft, 0.0);
        handle->spectrumWrite = 0;
        handle->spectrumFilled = 0;
    }
    if (frames >= kSpectrumFft) {
        // Longer than the window: only the tail can be in it.
        const size_t tail = frames - kSpectrumFft;
        samples = samples ? samples + tail : nullptr;
        handle->spectrumWrite = 0;
        handle->spectrumFilled = 0;
        frames = kSpectrumFft;
    }
    const size_t fedFrames = frames;
    if (std::getenv("NAE_TRACE_SPECTRUM")) {
        std::fprintf(stderr, "[nae:feed] n=%zu first:", frames);
        for (size_t i = 0; i < 3 && i < frames; ++i) {
            std::fprintf(stderr, " %.5f", samples ? samples[i] : 0.0);
        }
        std::fprintf(stderr, "  write=%zu filled=%zu totalFed=%llu\n", handle->spectrumWrite,
                     handle->spectrumFilled,
                     static_cast<unsigned long long>(handle->spectrumFramesFed));
    }
    for (size_t i = 0; i < frames; ++i) {
        handle->spectrumRing[handle->spectrumWrite] = samples ? samples[i] : 0.0;
        if (++handle->spectrumWrite == kSpectrumFft) handle->spectrumWrite = 0;
        if (handle->spectrumFilled < kSpectrumFft) ++handle->spectrumFilled;
    }
    // The real count, counted before the clamp above shortened it.
    //
    // `framesFed` is the only way a caller tells a live reading from a leftover
    // one, so it has to count device frames and nothing else. It was reading the
    // post-clamp value, which on a path that feeds short blocks -- the sine path
    // feeds 4 to 28 frames at a time -- undercounts by more than two orders of
    // magnitude. A staleness check built on that would have declared a live
    // stream dead.
    handle->spectrumFramesFed += fedFrames;

    // 包络在这里算，而不是交给读者算——否则就得把原始样本交给读者，而那恰恰是整套
    // 架构要防的事。
    //
    // 用平均绝对值而不是 RMS：便宜三分之一，而且起始点是能量事件。前端 `tempo.js`
    // 选的是同一个，两者一致才能让两条路由的答案**可比**，而不只是「差不多」。
    if (handle->envelope.empty()) {
        handle->envelope.assign(kEnvelopeFrames, 0.0f);
        handle->envelopeFill = 0;
        handle->envelopeEpoch = 0;
        // 环重建了，攒了一半的那一帧也扔掉——否则它会跨过一段不属于自己的音频。
        handle->envelopeAccum = 0.0;
        handle->envelopeAccumN = 0;
    }
    if (handle->envelopeRate != rate) {
        handle->envelopeRate = rate;
        handle->envelope.assign(kEnvelopeFrames, 0.0f);
        handle->envelopeFill = 0;
        handle->envelopeEpoch = 0;
        // 环重建了，攒了一半的那一帧也扔掉——否则它会跨过一段不属于自己的音频。
        handle->envelopeAccum = 0.0;
        handle->envelopeAccumN = 0;
    }
    // 设备速率下的 5 ms。做了保护：速率为零会除零，得出零帧——而零帧读起来是
    // 「没有包络」，不是一个错误。
    const size_t hop =
        rate ? static_cast<size_t>(static_cast<double>(rate) * kEnvelopeHopMs / 1000.0) : 0;
    if (hop > 0) {
        // `samples` 为 nullptr 表示静音，而静音是被喂进去而不是跳过的。
        // 跳过会让包络里留着上一段音乐，于是暂停的播放器会继续报告一首已经停下
        // 的歌的节拍——而 `framesFed` 也不再推进，读者无法分辨这两者。
        // **一帧 = 连续的 `hop` 个样本，跨调用攒够才写。**（见 handle 上那两个字
        // 段的注释：原来每帧都从本次调用的 0 开始，末尾不足一帧也写成帧，于是帧率
        // 跟着调用频率走而不是跟着采样率走。）
        //
        // `samples` 为 nullptr 表示静音，而静音是被喂进去而不是跳过的。跳过会让包络
        // 里留着上一段音乐，于是暂停的播放器会继续报告一首已经停下的歌的节拍——而
        // `framesFed` 也不再推进，读者无法分辨这两者。
        for (size_t i = 0; i < frames; ++i) {
            if (samples != nullptr) handle->envelopeAccum += std::fabs(samples[i]);
            if (++handle->envelopeAccumN < hop) continue;
            handle->envelope[handle->envelopeFill++] = static_cast<float>(
                handle->envelopeAccum / static_cast<double>(hop));
            if (handle->envelopeFill >= kEnvelopeFrames) handle->envelopeFill = 0;
            ++handle->envelopeEpoch;
            handle->envelopeAccum = 0.0;
            handle->envelopeAccumN = 0;
        }
    }
}

/// In-place iterative radix-2 Cooley-Tukey on a power-of-two length.
///
/// Not the fastest transform available, and it does not need to be: it runs on
/// whichever thread calls NAE_QuerySpectrum, never on the render thread, at
/// roughly twenty times a second. A 4096-point transform is about fifty thousand
/// butterflies, which is under a millisecond.
///
/// `re` and `im` must be the same length and a power of two. Anything else leaves
/// them untouched, so a caller that got the size wrong gets a wrong answer rather
/// than a wrong answer *and* a corrupted buffer.
static void fftRadix2(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    if (n < 2 || im.size() != n || (n & (n - 1)) != 0) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * 3.14159265358979323846 / static_cast<double>(len);
        const double wr = std::cos(ang);
        const double wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0;
            double ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const double ur = re[i + k];
                const double ui = im[i + k];
                const double xr = re[i + k + len / 2];
                const double xi = im[i + k + len / 2];
                const double vr = xr * cr - xi * ci;
                const double vi = xr * ci + xi * cr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

int32_t NAE_QuerySpectrum(NAE_Handle* handle, NAE_SpectrumInfo* info, float* bins,
                          int32_t capacity) NAE_NOEXCEPT {
    if (!info) return NAE_ERR_INVALID_ARGUMENT;
    // A null buffer is legal only when the capacity is zero -- that is the
    // documented "tell me the shape" call. Null with a non-zero capacity is a
    // caller that believes it passed a buffer, and writing through it is a null
    // dereference on the first element.
    if (!bins && capacity > 0) return NAE_ERR_INVALID_ARGUMENT;
    // Local plus a clamped copy, for the reason `NAE_QueryTags` gives: a
    // smaller declared size must not be overrun by the fields this build added.
    const uint32_t full = static_cast<uint32_t>(sizeof(NAE_SpectrumInfo));
    NAE_SpectrumInfo local{};
    local.struct_size = full;
    const uint32_t limit = declaredLimit(info->struct_size, full);
    StructCommit<NAE_SpectrumInfo> commit{info, &local, limit};
    if (!handle) return NAE_OK;

    std::vector<double> re(kSpectrumFft, 0.0);
    std::vector<double> im(kSpectrumFft, 0.0);
    uint64_t fed = 0;
    size_t filled = 0;
    size_t write = 0;
    {
        std::lock_guard<std::mutex> guard(handle->spectrumLock);
        if (handle->spectrumRing.empty()) return NAE_OK;
        fed = handle->spectrumFramesFed;
        filled = handle->spectrumFilled;
        write = handle->spectrumWrite;
        local.sampleRate = handle->spectrumRate;
        // Read the ring oldest-first. `write` is where the next sample goes, so
        // the oldest sample is the one currently under it once the ring is full.
        if (filled == kSpectrumFft) {
            for (size_t i = 0; i < kSpectrumFft; ++i) {
                re[i] = handle->spectrumRing[(write + i) % kSpectrumFft];
            }
        } else {
            // Not full yet: the first `filled` entries are the only real ones.
            for (size_t i = 0; i < filled; ++i) re[i] = handle->spectrumRing[i];
        }
    }

    local.fftSize = static_cast<uint32_t>(kSpectrumFft);
    local.binCount = static_cast<uint32_t>(kSpectrumFft / 2);
    local.validBits = handle->dither.valid_bits();
    local.framesFed = fed;
    if (filled == 0) return NAE_OK;

    // Diagnostic, under NAE_TRACE_SPECTRUM rather than NAE_TRACE: the existing
    // trace is about the render loop's decisions and this is about the tap's
    // contents, and they get read for different faults.
    //
    // It exists because the tap was wrong twice in a way that looked completely
    // plausible -- once reading a single ring slot 4096 times, which reported a
    // clean staircase as if it were audio, and once counting clamped frames as
    // real ones. Both produced a spectrum display that would have drawn
    // something. Six sampled positions and the mean are enough to tell a window
    // holding music from one holding a constant, which is the whole question.
    if (std::getenv("NAE_TRACE_SPECTRUM") != nullptr) {
        double mean = 0.0;
        size_t argmax = 0;
        double amax = 0.0;
        for (size_t i = 0; i < filled; ++i) {
            mean += re[i];
            if (std::fabs(re[i]) > amax) {
                amax = std::fabs(re[i]);
                argmax = i;
            }
        }
        mean /= static_cast<double>(filled);
        std::fprintf(stderr,
                     "[nae:spectrum] filled=%zu rate=%u fed=%llu write=%zu mean=%.6f amax=%.6f "
                     "@%zu\n",
                     filled, local.sampleRate, (unsigned long long)fed, write, mean, amax, argmax);
        // The ring itself, walked from `write` exactly as the read path walks it, and
        // also from index 0. Two walks because they must agree: if they do, the
        // read path is faithful to the ring and the fault is in what was fed; if
        // they do not, the fault is in the read.
        std::fprintf(stderr, "[nae:spectrum] fromWrite:");
        for (size_t i = 0; i < 12 && i < filled; ++i) {
            std::fprintf(stderr, " %.5f", handle->spectrumRing[(write + i) % kSpectrumFft]);
        }
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "[nae:spectrum] fromZero:");
        for (size_t i = 0; i < 12 && i < filled; ++i) {
            std::fprintf(stderr, " %.5f", handle->spectrumRing[i]);
        }
        std::fprintf(stderr, "\n");
        // How far apart two identical values are, which is the period whatever
        // the phase. Searched rather than sampled at a guessed stride: a stride
        // of 96 at 1 kHz/192 kHz lands on every zero crossing and reports a flat
        // line for a perfectly good tone, which is the mistake that made the
        // first two runs of this instrument unreadable.
        // The period, by counting samples between upward zero crossings.
        //
        // Not by looking for a repeat of the first value: a continuous tone's
        // window essentially never contains two samples equal to within 1e-9, and
        // the earlier version of this reported a confident "32 samples" for a
        // tone that was a correct 1 kHz. A detector that cannot be wrong has to
        // find a feature rather than a coincidence -- zero crossings are a
        // feature, and averaging several of them beats trusting one.
        //
        // Averaged, because a crossing lands between two samples and rounding to
        // the nearer one costs up to half a sample per crossing.
        double firstCross = -1.0;
        double lastCross = -1.0;
        int crossings = 0;
        for (size_t i = 1; i < kSpectrumFft; ++i) {
            const double a = handle->spectrumRing[(write + i - 1) % kSpectrumFft];
            const double b = handle->spectrumRing[(write + i) % kSpectrumFft];
            if (a <= 0.0 && b > 0.0) {
                // Linear interpolation for the sub-sample position.
                const double frac = (b == a) ? 0.0 : (0.0 - a) / (b - a);
                const double at = static_cast<double>(i - 1) + frac;
                if (firstCross < 0.0) firstCross = at;
                lastCross = at;
                ++crossings;
            }
        }
        const double measuredHz =
            (crossings > 1)
                ? static_cast<double>(crossings - 1) * local.sampleRate /
                      static_cast<double>(lastCross - firstCross)
                : 0.0;
        std::fprintf(stderr,
                     "[nae:spectrum] crossings=%d measuredHz=%.3f (want 1000.000)\n", crossings,
                     measuredHz);
    }

    // Unnormalised level first, so `peak` and `rms` describe the signal rather
    // than the window that was applied to it.
    double sumSquares = 0.0;
    double peak = 0.0;
    for (size_t i = 0; i < filled; ++i) {
        const double v = re[i];
        sumSquares += v * v;
        const double a = std::fabs(v);
        if (a > peak) peak = a;
    }
    local.peak = peak;
    local.rms = std::sqrt(sumSquares / static_cast<double>(filled));

    // Hann, over the samples actually present rather than over the full window,
    // so a partially filled ring is not scaled by a window that assumes more
    // history than there is.
    for (size_t i = 0; i < filled; ++i) {
        const double w =
            filled < 2 ? 1.0
                      : 0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846 *
                                               static_cast<double>(i) /
                                               static_cast<double>(filled - 1)));
        re[i] *= w;
    }

    fftRadix2(re, im);

    // A real signal's spectrum is symmetric, so the second half carries no
    // information. The factor of 2/n makes a full-scale sine read 1.0 in its bin,
    // which is what lets a dB display use 0 dBFS as its reference.
    const double scale = 2.0 / static_cast<double>(filled);
    const int32_t want = static_cast<int32_t>(local.binCount);
    const int32_t give = capacity < want ? capacity : want;
    for (int32_t k = 0; k < give; ++k) {
        const double m = std::sqrt(re[static_cast<size_t>(k)] * re[static_cast<size_t>(k)] +
                                   im[static_cast<size_t>(k)] * im[static_cast<size_t>(k)]) *
                         scale;
        bins[k] = static_cast<float>(m);
    }
    return NAE_OK;
}

// `NAE_EnvelopeInfo` 的布局，**写成字面数字**。
//
// `rust/src/ffi.rs` 里那组 `offset_of!` 断言的是**同一组数字**。这一组是**这一次补上的**
// ——它原来一条都没有，而 Rust 那一侧的断言当时写的是「相邻」关系（`a + 4 == b`），
// **那种形式对字段重排是免疫的**：两边都把 `sample_rate` / `frame_hop_ms` 的位置写反了，
// 而两边各自的断言都成立、`sizeof` 都是 48，于是编译通过、运行时才开始出垃圾数。
//
// 量到的后果：`frame_hop_ms` 读成 2.12e-318（44100 的位模式当 double），`sample_rate`
// 读成 0，前端那个 BPM 估计器于是永远返回 null——界面上「BPM —」的根因。
static_assert(offsetof(NAE_EnvelopeInfo, struct_size) == 0, "NAE_EnvelopeInfo 布局变了");
static_assert(offsetof(NAE_EnvelopeInfo, frame_count) == 4, "NAE_EnvelopeInfo 布局变了");
static_assert(offsetof(NAE_EnvelopeInfo, frame_hop_ms) == 8, "NAE_EnvelopeInfo 布局变了");
static_assert(offsetof(NAE_EnvelopeInfo, sample_rate) == 16, "NAE_EnvelopeInfo 布局变了");
static_assert(offsetof(NAE_EnvelopeInfo, framesFedPad) == 20, "NAE_EnvelopeInfo 布局变了");
static_assert(offsetof(NAE_EnvelopeInfo, frames_fed) == 24, "NAE_EnvelopeInfo 布局变了");
static_assert(offsetof(NAE_EnvelopeInfo, window_ms) == 32, "NAE_EnvelopeInfo 布局变了");
static_assert(offsetof(NAE_EnvelopeInfo, frame_valid) == 40, "NAE_EnvelopeInfo 布局变了");
static_assert(sizeof(NAE_EnvelopeInfo) == 48, "NAE_EnvelopeInfo 的 sizeof 变了");

int32_t NAE_QueryEnvelope(NAE_Handle* handle, NAE_EnvelopeInfo* info, float* envelope,
                          int32_t capacity) NAE_NOEXCEPT {
    if (info == nullptr) return NAE_ERR_INVALID_ARGUMENT;
    // Null is legal only with a zero capacity, as in NAE_QuerySpectrum.
    if (!envelope && capacity > 0) return NAE_ERR_INVALID_ARGUMENT;
    // Local plus a clamped copy, for the reason `NAE_QueryTags` gives.
    const uint32_t full = static_cast<uint32_t>(sizeof(NAE_EnvelopeInfo));
    NAE_EnvelopeInfo local{};
    local.struct_size = full;
    const uint32_t limit = declaredLimit(info->struct_size, full);
    StructCommit<NAE_EnvelopeInfo> commit{info, &local, limit};
    if (handle == nullptr) return NAE_OK;

    // 频谱那边用的同一把锁，因为两者读的都是渲染循环写的状态。包络是从同一个环形
    // 缓冲推导出来的，所以不能各自加锁——否则读者可能看到一个已经换了采样率而
    // 另一个还没换。
    std::lock_guard<std::mutex> guard(handle->spectrumLock);
    if (handle->envelope.empty()) return NAE_OK;

    local.frame_count = static_cast<uint32_t>(handle->envelope.size());
    local.frame_hop_ms = kEnvelopeHopMs;
    local.sample_rate = handle->envelopeRate;
    // 故意用频谱那个计数器：两者描述的是同一个环形缓冲，所以调用方比较这两个数
    // 等于拿一个东西跟它自己比。给两个计数器只会引出一个问题——为什么频谱过期了
    // 而包络没有——而那个问题只可能有一个答案，且答案必然是其中一个有 bug。
    local.frames_fed = handle->spectrumFramesFed;
    local.window_ms = static_cast<double>(local.frame_count) * kEnvelopeHopMs;

    // **顺序是有保证的：最老的在前。**
    //
    // 这里原来是 `envelope[(envelopeFill + i) % size]`，也就是从**写指针**开始拷。
    // 写满之后写指针处正好是最老的一帧（对的），**没写满的时候那是还没写过的区域**
    // ——于是读者拿到的是一个「旋转 + 补零」的数组，而它没办法从 4096 个 float 里
    // 看出旋转了多少。实测（`scripts/probe-tempo-window.mjs`）：非零段的起点在
    // 1983 / 1243 / 420 / 0 之间动，节拍估计器在同一段音频上给出的数在 72 ~ 200
    // 之间跳，置信度从没超过 0.5。
    //
    // 环本身仍然是一个环，只是**出口**归一化：没写满时最老的一帧在下标 0，
    // 写满之后在写指针处。`frame_valid` 把「有几个是真的」一起报出去——原来那个字段
    // 位置报的是**容量**，而容量和有效帧数在环没写满时不相等。
    // **`envelopeFill` 不是「写过多少帧」。** 它是环上的一个下标，绕完一圈就回到小值
    // ——原来这里用 `min(envelopeFill, size)` 当「有几帧是真的」，于是**环绕一圈之后
    // 「有效帧数」会从四千多掉回几百**（实测 `probe-tempo-window`：11.1 秒那次 3977，
    // 13.1 秒那次掉到 701）。`envelopeEpoch` 是**累计写过多少帧**，只增不减，那才是
    // 这个数；`envelopeFill` 只在算「最老的一帧在哪」时有用。
    const size_t size = handle->envelope.size();
    const size_t filled = static_cast<size_t>(
        std::min<uint64_t>(handle->envelopeEpoch, static_cast<uint64_t>(size)));
    const size_t oldest = handle->envelopeEpoch >= size
                              ? static_cast<size_t>(handle->envelopeEpoch % size)
                              : 0;
    const int32_t valid =
        capacity < static_cast<int32_t>(filled) ? capacity : static_cast<int32_t>(filled);
    for (int32_t i = 0; i < valid; ++i) {
        envelope[i] = handle->envelope[(oldest + static_cast<size_t>(i)) % size];
    }
    local.frame_valid = valid > 0 ? static_cast<uint32_t>(valid) : 0u;
    return NAE_OK;
}

#ifdef NEIRO_HAVE_FFMPEG
namespace {

/// 短窗 RMS 的口径：窗 400 ms、跳 100 ms。
///
/// **`src/scripts/audio/spectrum.js` 里有一份逐字对齐的镜像**，用的就是这两个数。
/// 两条路线（引擎那趟整首解码 / Web Audio 手里的 AudioBuffer）要给同一个读数，改这里
/// 就要改那里——和 peaks 那一份镜像同一个理由（见下面那段注释）。
constexpr double kMetricsWindowMs = 400.0;
constexpr double kMetricsHopMs = 100.0;

/// 线性插值分位数（`q` 在 0..1）。输入按值拷贝：调用方要拿同一个数组问好几个 q。
///
/// 定义取最普通的那种：把 n 个值排序，位置 `q * (n - 1)`，落在这两个下标之间就插值。
/// 不是「取第 floor(q*n) 个」——后者在 n 小的时候会把 p95 和 p10 指到同一个值上。
double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = q * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}


// Offline tempo estimate from a 5 ms onset envelope. This is intentionally a
// separate function from the live envelope tap: the host needs BPM for a file it
// has not played.
void estimateBpm(const std::vector<double>& envelope, double hopMs, double* bpm,
                 double* confidence) {
    *bpm = 0.0;
    *confidence = 0.0;
    if (envelope.size() < 40) return;
    std::vector<double> onset(envelope.size(), 0.0);
    for (size_t i = 1; i < envelope.size(); ++i) {
        onset[i] = std::max(0.0, envelope[i] - envelope[i - 1]);
    }
    const double hopSec = hopMs / 1000.0;
    const int minLag = std::max(1, static_cast<int>(std::lround(60.0 / (200.0 * hopSec))));
    const int maxLag = std::max(minLag + 1, static_cast<int>(std::lround(60.0 / (60.0 * hopSec))));
    std::vector<double> score(static_cast<size_t>(maxLag) + 2, 0.0);
    for (int lag = minLag; lag <= maxLag; ++lag) {
        double num = 0.0, e1 = 0.0, e2 = 0.0;
        for (size_t i = 0; i + static_cast<size_t>(lag) < onset.size(); ++i) {
            num += onset[i] * onset[i + static_cast<size_t>(lag)];
            e1 += onset[i] * onset[i];
            e2 += onset[i + static_cast<size_t>(lag)] * onset[i + static_cast<size_t>(lag)];
        }
        score[lag] = (e1 > 0.0 && e2 > 0.0) ? num / std::sqrt(e1 * e2) : 0.0;
    }
    int best = minLag;
    for (int lag = minLag + 1; lag <= maxLag; ++lag) {
        if (score[lag] > score[best]) best = lag;
    }
    if (score[best] <= 0.0) return;
    double lag = static_cast<double>(best);
    if (best > minLag && best < maxLag) {
        const double a = score[best - 1];
        const double b = score[best];
        const double c = score[best + 1];
        const double den = a - 2.0 * b + c;
        if (std::fabs(den) > 1e-12) lag += 0.5 * (a - c) / den;
    }
    double beats = 60.0 / (lag * hopSec);
    // Prefer the musical range; halve/double an octave only when it lands there.
    while (beats > 180.0 && beats / 2.0 >= 60.0) beats /= 2.0;
    while (beats < 70.0 && beats * 2.0 <= 200.0) beats *= 2.0;
    *bpm = beats;
    *confidence = std::min(1.0, score[best]);
}

/// 把整个文件解一遍，填出 `buckets` 个交错的 min/max，以及两个短窗响度指标。
///
/// 语义与 `src/scripts/audio/spectrum.js` 的 `computePeaks` **逐字对齐**：
/// 只取**第 0 声道**，每桶的 lo/hi **都从 0 起**，分桶边界按帧数均分。
///
/// 为什么要对齐而不是「改好一点」：`paintTimeline` 和导出都用那份 peaks。两条路给出
/// 不同形状的波形时，**看起来不一样的那个会被当成坏了一个**，而它其实只是另一套
/// 约定。要改就两边一起改。
// The cancellation flag is written by the host, on another thread, through
// whatever its own atomics look like (Rust's AtomicI32 in this repository's
// host). Reading it with a plain load would be a data race against that store;
// the relaxed load is the cheapest thing that is not one, and it is all this
// needs -- the flag is advisory, and a block of extra work is harmless.
static inline bool cancelRequested(const int32_t* flag) {
    if (!flag) return false;
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(flag, __ATOMIC_RELAXED) != 0;
#else
    return *flag != 0;
#endif
}

bool computePeaksFromFile(const std::string& path, uint32_t buckets,
                          std::vector<float>* out, bool* complete,
                          NAE_PeaksInfo* meta, NAE_AnalysisInfo* analysis,
                          std::string* error, const int32_t* cancel = nullptr,
                          bool* cancelled = nullptr) {
    if (cancelled) *cancelled = false;
    neiro::Decoder dec;
    if (!dec.open(path, error)) return false;

    const auto& info = dec.info();
    const uint32_t ch = info.out_channels ? info.out_channels : 1;
    // 分桶边界要用文件自己的帧数，而 `neiro::Decoder` 只报时长和采样率。这里用和
    // `openExclusiveImpl` 里 `totalSourceFrames` **同一个**公式（那边也向上取了半个
    // 采样率的帧数），这样两条路的分桶边界落在同一处。
    const int64_t totalFrames =
        static_cast<int64_t>(info.duration_ms / 1000.0 * info.sample_rate) +
        static_cast<int64_t>(info.sample_rate) / 2;
    if (totalFrames <= 0 || ch == 0) {
        if (error) *error = "容器没有报时长，分桶边界算不出来";
        return false;
    }
    const double per = static_cast<double>(totalFrames) / static_cast<double>(buckets);
    if (analysis) {
        std::memset(analysis, 0, sizeof(*analysis));
        analysis->structSize = sizeof(*analysis);
        analysis->analyzerVersion = NAE_ANALYZER_VERSION;
        analysis->channels = static_cast<int32_t>(ch);
        analysis->sampleRate = static_cast<int32_t>(info.sample_rate);
        analysis->bitsPerSample = static_cast<int32_t>(info.bits_per_sample);
        analysis->durationMs = static_cast<int32_t>(info.duration_ms);
        std::snprintf(analysis->codec, sizeof(analysis->codec), "%s", info.codec.c_str());
        const auto copyTag = [](char* dst, size_t cap, const std::string& src) {
            if (cap == 0) return;
            const size_t n = std::min(cap - 1, src.size());
            std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        };
        copyTag(analysis->title, sizeof(analysis->title), info.title);
        copyTag(analysis->artist, sizeof(analysis->artist), info.artist);
        copyTag(analysis->album, sizeof(analysis->album), info.album);
        copyTag(analysis->albumArtist, sizeof(analysis->albumArtist), info.album_artist);
        copyTag(analysis->track, sizeof(analysis->track), info.track);
        copyTag(analysis->disc, sizeof(analysis->disc), info.disc);
        copyTag(analysis->year, sizeof(analysis->year), info.year);
        copyTag(analysis->genre, sizeof(analysis->genre), info.genre);
        copyTag(analysis->composer, sizeof(analysis->composer), info.composer);
        copyTag(analysis->comment, sizeof(analysis->comment), info.comment);
        analysis->bitrateKbps = info.bitrate_kbps;
        analysis->bitrateFromTag = info.bitrate_from_tag ? 1 : 0;
        analysis->rgTrackValid = info.rg_track_valid ? 1 : 0;
        analysis->rgTrackGainDb = info.rg_track_gain_db;
        analysis->rgTrackPeak = info.rg_track_peak;
        analysis->rgAlbumValid = info.rg_album_valid ? 1 : 0;
        analysis->rgAlbumGainDb = info.rg_album_gain_db;
        analysis->rgAlbumPeak = info.rg_album_peak;
    }
    if (meta) {
        meta->duration_ms = info.duration_ms;
        meta->sample_rate = info.sample_rate;
        meta->channels = ch;
        // 先写「没算」。下面每一条出口都可能在填数之前返回，而 0 **在这里**是唯一诚实
        // 的初值：它说的是「这两个数不存在」，不是「这首歌是 0 dBFS」。
        meta->metrics_window_ms = 0;
        meta->metrics_hop_ms = 0;
        meta->loudness_db = 0.0;
        meta->dynamic_range_db = 0.0;
    }

    // ---- 短窗 RMS：和 peaks **同一趟**解码里累加 ----
    //
    // 这一趟的主要代价是解码（量过：典型曲目约 0.5 秒），所以顺手算出声级几乎免费。
    //
    // 做法是经典的「先按跳长分段、再把几段并成一个窗」：窗 400 ms / 跳 100 ms 就是每段
    // 100 ms，四个连续段合起来是一个窗。反过来对每一帧开一个 400 ms 的窗，这一趟的内层
    // 循环会多出四倍。
    // 整数帧长，而且**四舍五入**：`spectrum.js` 那份镜像用的是 `Math.round`，两边必须
    // 在同一些帧上分段。一个用 double、一个用整数时，44.1 kHz 上恰好一致，别的采样率上
    // 会差一个样本——而它只要挪动一个窗，两个读数就都变了。
    neiro::LoudnessMeter loudnessMeter;
    const bool measureLoudness =
        (meta || analysis) && loudnessMeter.configure(ch, static_cast<uint32_t>(info.sample_rate));

    const int64_t hopFrames = std::max<int64_t>(
        1, static_cast<int64_t>(std::lround(kMetricsHopMs / 1000.0 * info.sample_rate)));
    std::vector<double> segSq;
    std::vector<int64_t> segN;

    const int64_t onsetHopFrames = std::max<int64_t>(
        1, static_cast<int64_t>(std::lround(5.0 / 1000.0 * info.sample_rate)));
    std::vector<double> onsetEnvelope;
    double onsetSum = 0.0;
    int64_t onsetCount = 0;

    int64_t frame = 0;
    std::vector<float> block;
    // `read` **流结束时返回 false**，而短读是正常的（一次一个包），所以终止条件是
    // 「没有新样本」，不是「这一块比预期短」。
    for (;;) {
        // Between blocks, not inside one: a block is one packet, so the caller's
        // worst-case delay is one packet rather than the whole file.
        //
        // The flag is read, never written. It exists as a separate export because
        // the decode call is blocking: without a flag the caller owns, nobody
        // could reach a decode that is already running.
        if (cancelRequested(cancel)) {
            if (cancelled) *cancelled = true;
            if (error) *error = "cancelled";
            return false;
        }
        block.clear();
        const bool more = dec.read(&block, error);
        if (block.empty()) {
            // 正常结束：false + 空。中途出错也会走到这里，所以顺带看 error 有没有话。
            *complete = !more && (error == nullptr || error->empty());
            break;
        }
        const size_t frames = block.size() / ch;
        if (measureLoudness) loudnessMeter.process(block.data(), frames, ch);
        for (size_t i = 0; i < frames; ++i) {
            const float v = block[i * ch];
            uint32_t b = per > 0.0 ? static_cast<uint32_t>(static_cast<double>(frame) / per) : 0u;
            if (b >= buckets) b = buckets - 1;
            const size_t k = static_cast<size_t>(b) * 2;
            if (v < (*out)[k]) (*out)[k] = v;
            else if (v > (*out)[k + 1]) (*out)[k + 1] = v;

            const size_t seg = static_cast<size_t>(frame / hopFrames);
            if (seg >= segSq.size()) {
                segSq.resize(seg + 1, 0.0);
                segN.resize(seg + 1, 0);
            }
            // **所有声道的算术平均**，不是第 0 声道。peaks 只看第 0 声道是「画一条
            // 波形」的约定（和 JS 那一份逐字对齐），而「这首歌有多响」说的是听者听到的
            // 那一路：只量左声道，一个人声偏左的混音会读得偏轻或偏响。
            double mono = 0.0;
            for (uint32_t c = 0; c < ch; ++c) mono += block[i * ch + c];
            mono /= static_cast<double>(ch);
            segSq[seg] += mono * mono;
            segN[seg] += 1;
            if (analysis) {
                onsetSum += std::fabs(mono);
                if (++onsetCount >= onsetHopFrames) {
                    onsetEnvelope.push_back(onsetSum / static_cast<double>(onsetCount));
                    onsetSum = 0.0;
                    onsetCount = 0;
                }
            }
            ++frame;
        }
        if (!more) { *complete = (error == nullptr || error->empty()); break; }
    }

    // ---- 合成窗、开门限、取分位数 ----
    //
    // 只在前一趟**真的解到了东西**时才算。解不开的文件这里段数是 0，于是
    // `metrics_window_ms` 保持 0（= 没有这两个数），而不是给出一对来自零个窗的
    // 「0.0 dB」——那读起来是一个测量结果。
    if ((meta || analysis) && !segSq.empty()) {
        const int64_t wantSegments =
            std::max<int64_t>(1, static_cast<int64_t>(std::lround(kMetricsWindowMs / kMetricsHopMs)));
        const size_t perWindow = static_cast<size_t>(wantSegments);
        std::vector<double> levels;
        levels.reserve(segSq.size());
        for (size_t k = 0; k + perWindow <= segSq.size(); ++k) {
            double sum = 0.0;
            int64_t n = 0;
            for (size_t j = 0; j < perWindow; ++j) {
                sum += segSq[k + j];
                n += segN[k + j];
            }
            if (n <= 0) continue;
            const double ms = sum / static_cast<double>(n);
            // 数字静音的对数是负无穷，而一个 -inf 会把「动态范围」变成无穷大。压在
            // -120 dBFS 上：那在 16 bit 的底噪之下，真实内容碰不到它。
            levels.push_back(ms > 0.0 ? std::max(-120.0, 10.0 * std::log10(ms)) : -120.0);
        }
        if (!levels.empty()) {
            // **门限**：丢掉比「最响那 5% 的分位」低 40 dB 以上的窗。片头片尾的数字
            // 静音和段间底噪都落在这一档下面，而它们会把 p10 拉到 -120 dB——于是
            // 「动态范围」报出一个 100+ dB 的数，那个数说的是这个文件有多安静，不是
            // 它有多少动态。
            const double gate = percentile(levels, 0.95) - 40.0;
            std::vector<double> kept;
            kept.reserve(levels.size());
            for (const double v : levels) {
                if (v >= gate) kept.push_back(v);
            }
            if (!kept.empty()) {
                const double loudness = percentile(kept, 0.5);
                const double dynamicRange =
                    percentile(kept, 0.95) - percentile(kept, 0.10);
                if (meta) {
                    meta->metrics_window_ms =
                        static_cast<uint32_t>(std::lround(kMetricsWindowMs));
                    meta->metrics_hop_ms =
                        static_cast<uint32_t>(std::lround(kMetricsHopMs));
                    // 响度取**中位数**：比均值稳——一段独奏、一段安静的前奏都不至于把它拖走。
                    meta->loudness_db = loudness;
                    // 动态范围取两个分位数之差，两端都取分位数而不是极值：单个离群的窗
                    // （一下爆音、一段两秒的静音）不该定义一整首歌的动态。
                    meta->dynamic_range_db = dynamicRange;
                }
                if (analysis) {
                    analysis->metricsWindowMs =
                        static_cast<uint32_t>(std::lround(kMetricsWindowMs));
                    analysis->metricsHopMs =
                        static_cast<uint32_t>(std::lround(kMetricsHopMs));
                    analysis->loudnessDb = loudness;
                    analysis->dynamicRangeDb = dynamicRange;
                }
            }
        }
    }
    if (measureLoudness && (meta || analysis)) {
        const neiro::LoudnessMeter::Result lr = loudnessMeter.finish();
        if (meta) {
            meta->lufs_integrated = lr.integratedLufs;
            meta->lufs_momentary_max = lr.momentaryMaxLufs;
            meta->lufs_short_term_max = lr.shortTermMaxLufs;
            meta->lufs_range = lr.rangeLu;
            meta->lufs_valid = lr.valid ? 1 : 0;
            meta->lufs_short_term_valid = lr.shortTermValid ? 1 : 0;
        }
        if (analysis) {
            analysis->lufsIntegrated = lr.integratedLufs;
            analysis->lufsMomentaryMax = lr.momentaryMaxLufs;
            analysis->lufsShortTermMax = lr.shortTermMaxLufs;
            analysis->lufsRange = lr.rangeLu;
            analysis->lufsValid = lr.valid ? 1 : 0;
            analysis->lufsShortTermValid = lr.shortTermValid ? 1 : 0;
        }
    }
    if (analysis) {
        estimateBpm(onsetEnvelope, 5.0, &analysis->bpm, &analysis->bpmConfidence);
        analysis->bpmValid =
            (analysis->bpmConfidence >= 0.2 && analysis->bpm >= 30.0 &&
             analysis->bpm <= 300.0)
                ? 1
                : 0;
    }
    return true;
}

}  // namespace
#endif  // NEIRO_HAVE_FFMPEG

// `NAE_PeaksInfo` 的布局，**写成字面数字**。
//
// `rust/src/ffi.rs` 里那组 `offset_of!` 断言的是**同一组数字**。两边各写各的期望值，
// 所以改了一边而没改另一边时，编译会失败 —— 这才是跨语言那一层的检查。
//
// 为什么这个结构体需要它：这一版加了一对 `double` 响度指标（`loudness_db` /
// `dynamic_range_db`），于是**偏移和 sizeof 全都变了**（40 → 56）。那正是这组断言存在
// 的场景：只改一边的话，另一边的 `offset_of!` 会当场编译不过，而不是在运行时把
// 「响度」读成「采样率」。
static_assert(offsetof(NAE_PeaksInfo, struct_size) == 0, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, bucket_count) == 4, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, complete) == 8, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, reserved0) == 12, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, duration_ms) == 16, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, sample_rate) == 24, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, channels) == 28, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, metrics_window_ms) == 32, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, metrics_hop_ms) == 36, "NAE_PeaksInfo 布局变了");
// 40 是 8 的倍数，所以两个 `double` 之间没有内部 padding；48 + 8 = 56，也没有尾部
// padding（结构体对齐是 8，56 是 8 的倍数）。
static_assert(offsetof(NAE_PeaksInfo, loudness_db) == 40, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, dynamic_range_db) == 48, "NAE_PeaksInfo 布局变了");
static_assert(offsetof(NAE_PeaksInfo, lufs_integrated) == 56, "NAE_PeaksInfo layout changed");
static_assert(offsetof(NAE_PeaksInfo, lufs_momentary_max) == 64, "NAE_PeaksInfo layout changed");
static_assert(offsetof(NAE_PeaksInfo, lufs_short_term_max) == 72, "NAE_PeaksInfo layout changed");
static_assert(offsetof(NAE_PeaksInfo, lufs_range) == 80, "NAE_PeaksInfo layout changed");
static_assert(offsetof(NAE_PeaksInfo, lufs_valid) == 88, "NAE_PeaksInfo layout changed");
static_assert(offsetof(NAE_PeaksInfo, lufs_short_term_valid) == 92, "NAE_PeaksInfo layout changed");
static_assert(sizeof(NAE_PeaksInfo) == 96,
              "NAE_PeaksInfo 的 sizeof 变了 —— 尾部 padding 是两边最容易不一致的地方");

int32_t NAE_ComputePeaks(const char* path, uint32_t buckets, NAE_PeaksInfo* info,
                         float* out, int32_t capacity) NAE_NOEXCEPT {
    if (info == nullptr) return NAE_ERR_INVALID_ARGUMENT;

    // Local plus a clamped copy, for the reason `NAE_QueryTags` gives.
    const uint32_t full = static_cast<uint32_t>(sizeof(NAE_PeaksInfo));
    NAE_PeaksInfo local{};
    local.struct_size = full;
    const uint32_t limit = declaredLimit(info->struct_size, full);
    StructCommit<NAE_PeaksInfo> commit{info, &local, limit};

    // 桶数/容量不够是**调用方的错**，不截断：截断会让它按错的桶数去画，而那看起来
    // 和「这首歌就是这样」没有区别。
    //
    // `buckets * 2` is computed in `uint32_t`, so a value near the top wraps
    // before the cast: `0x40000001` doubles to `0x80000002`, which casts to a
    // negative `int32_t`, and the comparison against `capacity` is then false for
    // every non-negative capacity -- the guard passed and the copy wrote 2^31
    // floats through the caller's pointer. The bound is checked before any
    // arithmetic.
    if (path == nullptr || path[0] == '\0' || buckets == 0 || out == nullptr ||
        buckets > static_cast<uint32_t>(INT32_MAX / 2) ||
        capacity < static_cast<int32_t>(buckets * 2)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

#ifdef NEIRO_HAVE_FFMPEG
    std::vector<float> result(static_cast<size_t>(buckets) * 2, 0.0f);
    bool complete = false;
    std::string error;
    // 打不开路径是**故障**，不是「还没有」——所以报错而不是回清零。回清零会把它
    // 显示成「这首歌没有波形」，一个关于音乐的陈述。
    // Into `local`: the commit guard copies it out at the end, so writing to
    // the caller's buffer here would be overwritten by the local's zeroes.
    if (!computePeaksFromFile(path, buckets, &result, &complete, &local, nullptr, &error)) {
        return NAE_ERR_CANNOT_OPEN;
    }

    std::copy(result.begin(), result.end(), out);
    local.bucket_count = buckets;
    local.complete = complete ? 1u : 0u;
    return NAE_OK;
#else
    return NAE_OK;
#endif
}

uint32_t NAE_GetAnalyzerVersion(void) NAE_NOEXCEPT { return NAE_ANALYZER_VERSION; }

static int32_t analyzeFileImpl(const char* path, uint32_t buckets,
                               NAE_AnalysisInfo* info, float* peaks,
                               int32_t capacity, int32_t* peaksWritten,
                               const int32_t* cancel) NAE_NOEXCEPT {
    // The same overflow as NAE_ComputePeaks: `buckets * 2` wraps in uint32_t
    // before the cast, so a value near the top made the guard pass and the copy
    // write through the caller's pointer.
    if (!info || !peaks || !path || !path[0] || buckets == 0 ||
        buckets > static_cast<uint32_t>(INT32_MAX / 2) ||
        capacity < static_cast<int32_t>(buckets * 2)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

    const uint32_t full = static_cast<uint32_t>(sizeof(NAE_AnalysisInfo));
    NAE_AnalysisInfo local{};
    local.structSize = full;
    const uint32_t limit = declaredLimit(info->structSize, full);
    StructCommit<NAE_AnalysisInfo> commit{info, &local, limit};
    local.analyzerVersion = NAE_ANALYZER_VERSION;
    if (peaksWritten) *peaksWritten = 0;

#ifdef NEIRO_HAVE_FFMPEG
    std::vector<float> result(static_cast<size_t>(buckets) * 2, 0.0f);
    bool complete = false;
    std::string error;
    bool cancelled = false;
    // Into `local`, not into the caller's buffer: the commit guard copies the
    // local out at the end, so anything written to `info` here would be
    // overwritten by zeroes -- which is what made every field read as 0.
    if (!computePeaksFromFile(path, buckets, &result, &complete, nullptr, &local, &error,
                              cancel, &cancelled)) {
        g_lastErrorText = error;
        return cancelled ? NAE_ERR_CANCELLED : NAE_ERR_CANNOT_OPEN;
    }
    std::copy(result.begin(), result.end(), peaks);
    local.bucketCount = buckets;
    local.complete = complete ? 1u : 0u;
    if (peaksWritten) *peaksWritten = static_cast<int32_t>(buckets * 2);
    return NAE_OK;
#else
    return NAE_OK;
#endif
}

int32_t NAE_AnalyzeFile(const char* path, uint32_t buckets, NAE_AnalysisInfo* info,
                        float* peaks, int32_t capacity,
                        int32_t* peaksWritten) NAE_NOEXCEPT {
    return analyzeFileImpl(path, buckets, info, peaks, capacity, peaksWritten, nullptr);
}

int32_t NAE_AnalyzeFileCancelable(const char* path, uint32_t buckets,
                                  NAE_AnalysisInfo* info, float* peaks,
                                  int32_t capacity, int32_t* peaksWritten,
                                  const int32_t* cancel) NAE_NOEXCEPT {
    return analyzeFileImpl(path, buckets, info, peaks, capacity, peaksWritten, cancel);
}

// `NAE_AnalysisInfo` layout, written as literal numbers. `rust/src/ffi.rs`
// asserts the same numbers with offset_of!; editing one side without the other
// fails the build instead of reading a tag as a gain.
static_assert(offsetof(NAE_AnalysisInfo, structSize) == 0, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, analyzerVersion) == 4, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, complete) == 8, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, bucketCount) == 12, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, channels) == 16, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, sampleRate) == 20, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, bitsPerSample) == 24, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, durationMs) == 28, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, codec) == 32, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, title) == 64, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, artist) == 192, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, album) == 320, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, albumArtist) == 448, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, track) == 576, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, disc) == 592, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, year) == 608, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, genre) == 624, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, composer) == 688, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, comment) == 816, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, bitrateKbps) == 1072, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, bitrateFromTag) == 1076, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, rgTrackValid) == 1080, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, rgTrackGainDb) == 1088, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, rgTrackPeak) == 1096, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, rgAlbumValid) == 1104, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, rgAlbumGainDb) == 1112, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, rgAlbumPeak) == 1120, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, loudnessDb) == 1128, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, dynamicRangeDb) == 1136, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, metricsWindowMs) == 1144, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, metricsHopMs) == 1148, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, bpm) == 1152, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, bpmConfidence) == 1160, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, bpmValid) == 1168, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, lufsIntegrated) == 1176, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, lufsMomentaryMax) == 1184, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, lufsShortTermMax) == 1192, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, lufsRange) == 1200, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, lufsValid) == 1208, "NAE_AnalysisInfo layout changed");
static_assert(offsetof(NAE_AnalysisInfo, lufsShortTermValid) == 1212, "NAE_AnalysisInfo layout changed");
static_assert(sizeof(NAE_AnalysisInfo) == 1216, "NAE_AnalysisInfo layout changed");

static_assert(offsetof(NAE_SignalPath, seamHistogramMax) == 672,
              "NAE_SignalPath layout changed");
static_assert(offsetof(NAE_SignalPath, channelDelayMaxFrames) == 676,
              "NAE_SignalPath layout changed");
static_assert(offsetof(NAE_SignalPath, tailPad) == 680, "NAE_SignalPath layout changed");
static_assert(offsetof(NAE_SignalPath, tailPad2) == 684, "NAE_SignalPath layout changed");
static_assert(sizeof(NAE_SignalPath) == 688, "NAE_SignalPath layout changed");

// Session structs the Rust Base mirrors. Same rule: literal offsets on both
// sides, so a field inserted here fails the Rust build rather than turning a
// render-path flag into a position.
static_assert(offsetof(NAE_SessionConfig, structSize) == 0, "NAE_SessionConfig layout changed");
static_assert(offsetof(NAE_SessionConfig, deviceId) == 8, "NAE_SessionConfig layout changed");
static_assert(offsetof(NAE_SessionConfig, path) == 16, "NAE_SessionConfig layout changed");
static_assert(offsetof(NAE_SessionConfig, outputMode) == 24, "NAE_SessionConfig layout changed");
static_assert(offsetof(NAE_SessionConfig, fixedRate) == 28, "NAE_SessionConfig layout changed");
static_assert(offsetof(NAE_SessionConfig, quality) == 32, "NAE_SessionConfig layout changed");
static_assert(offsetof(NAE_SessionConfig, shareMode) == 36, "NAE_SessionConfig layout changed");
static_assert(offsetof(NAE_SessionConfig, sliceMs) == 40, "NAE_SessionConfig layout changed");
static_assert(sizeof(NAE_SessionConfig) == 48, "NAE_SessionConfig layout changed");

static_assert(offsetof(NAE_SessionState, structSize) == 0, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, state) == 4, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, position) == 8, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, duration) == 16, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, sampleRate) == 24, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, sourceRate) == 28, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, channels) == 32, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, bitsPerSample) == 36, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, validBits) == 40, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, isFloat) == 44, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, bitPerfect) == 48, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, reason) == 52, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, resampling) == 56, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, dithered) == 60, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, fileChannels) == 64, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, latencyMs) == 72, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, volume) == 80, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, eqEnabled) == 88, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, crossfeedEnabled) == 92, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, replayGainEnabled) == 96, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, convolverEnabled) == 100, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, crossfadeEnabled) == 104, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, rgTrackValid) == 108, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, rgAlbumValid) == 112, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, rgTrackGainDb) == 120, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, rgAlbumGainDb) == 128, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, rgAppliedGain) == 136, "NAE_SessionState layout changed");
static_assert(offsetof(NAE_SessionState, trackSerial) == 144, "NAE_SessionState layout changed");
static_assert(sizeof(NAE_SessionState) == 152, "NAE_SessionState layout changed");

static_assert(offsetof(NAE_EqBand, type) == 0, "NAE_EqBand layout changed");
static_assert(offsetof(NAE_EqBand, freqHz) == 8, "NAE_EqBand layout changed");
static_assert(offsetof(NAE_EqBand, gainDb) == 16, "NAE_EqBand layout changed");
static_assert(offsetof(NAE_EqBand, q) == 24, "NAE_EqBand layout changed");
static_assert(offsetof(NAE_EqBand, enabled) == 32, "NAE_EqBand layout changed");
static_assert(offsetof(NAE_EqBand, channelMask) == 36, "NAE_EqBand layout changed");
static_assert(sizeof(NAE_EqBand) == 40, "NAE_EqBand layout changed");
static_assert(offsetof(NAE_EqParams, structSize) == 0, "NAE_EqParams layout changed");
static_assert(offsetof(NAE_EqParams, enabled) == 4, "NAE_EqParams layout changed");
static_assert(offsetof(NAE_EqParams, preampDb) == 8, "NAE_EqParams layout changed");
static_assert(offsetof(NAE_EqParams, bandCount) == 16, "NAE_EqParams layout changed");
static_assert(offsetof(NAE_EqParams, bands) == 24, "NAE_EqParams layout changed");
static_assert(offsetof(NAE_EqParams, phase) == 1304, "NAE_EqParams layout changed");
static_assert(sizeof(NAE_EqParams) == 1312, "NAE_EqParams layout changed");
static_assert(offsetof(NAE_CrossfeedParams, structSize) == 0, "NAE_CrossfeedParams layout changed");
static_assert(offsetof(NAE_CrossfeedParams, enabled) == 4, "NAE_CrossfeedParams layout changed");
static_assert(offsetof(NAE_CrossfeedParams, amount) == 8, "NAE_CrossfeedParams layout changed");
static_assert(offsetof(NAE_CrossfeedParams, delayMs) == 16, "NAE_CrossfeedParams layout changed");
static_assert(offsetof(NAE_CrossfeedParams, cutoffHz) == 24, "NAE_CrossfeedParams layout changed");
static_assert(sizeof(NAE_CrossfeedParams) == 32, "NAE_CrossfeedParams layout changed");
static_assert(offsetof(NAE_ReplayGainParams, structSize) == 0, "NAE_ReplayGainParams layout changed");
static_assert(offsetof(NAE_ReplayGainParams, mode) == 4, "NAE_ReplayGainParams layout changed");
static_assert(offsetof(NAE_ReplayGainParams, preampDb) == 8, "NAE_ReplayGainParams layout changed");
static_assert(offsetof(NAE_ReplayGainParams, preventClipping) == 16, "NAE_ReplayGainParams layout changed");
static_assert(offsetof(NAE_ReplayGainParams, fallbackDb) == 24, "NAE_ReplayGainParams layout changed");
static_assert(sizeof(NAE_ReplayGainParams) == 32, "NAE_ReplayGainParams layout changed");
static_assert(offsetof(NAE_ConvolverParams, structSize) == 0, "NAE_ConvolverParams layout changed");
static_assert(offsetof(NAE_ConvolverParams, enabled) == 4, "NAE_ConvolverParams layout changed");
static_assert(offsetof(NAE_ConvolverParams, irPath) == 8, "NAE_ConvolverParams layout changed");
static_assert(offsetof(NAE_ConvolverParams, wet) == 520, "NAE_ConvolverParams layout changed");
static_assert(offsetof(NAE_ConvolverParams, dry) == 528, "NAE_ConvolverParams layout changed");
static_assert(offsetof(NAE_ConvolverParams, gainDb) == 536, "NAE_ConvolverParams layout changed");
static_assert(offsetof(NAE_ConvolverParams, normalize) == 544, "NAE_ConvolverParams layout changed");
static_assert(sizeof(NAE_ConvolverParams) == 552, "NAE_ConvolverParams layout changed");
static_assert(offsetof(NAE_CrossfadeParams, structSize) == 0, "NAE_CrossfadeParams layout changed");
static_assert(offsetof(NAE_CrossfadeParams, enabled) == 4, "NAE_CrossfadeParams layout changed");
static_assert(offsetof(NAE_CrossfadeParams, durationMs) == 8, "NAE_CrossfadeParams layout changed");
static_assert(offsetof(NAE_CrossfadeParams, curve) == 16, "NAE_CrossfadeParams layout changed");
static_assert(sizeof(NAE_CrossfadeParams) == 24, "NAE_CrossfadeParams layout changed");

int32_t NAE_PlaySine(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT {
    if (!handle || !handle->client) return NAE_ERR_NOT_OPEN;

    const int32_t channels = static_cast<int32_t>(handle->fmt.Format.nChannels);
    const uint32_t deviceRate = handle->fmt.Format.nSamplesPerSec;
    // The tone is generated at the SOURCE rate, not the device rate, so the
    // converter is genuinely exercised rather than bypassed.
    const uint32_t sourceRate = handle->sourceRate ? handle->sourceRate : deviceRate;
    const bool isFloat = handle->fmt.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    const uint32_t containerBits = handle->fmt.Format.wBitsPerSample;
    const uint32_t validBits = handle->fmt.Samples.wValidBitsPerSample
                                   ? handle->fmt.Samples.wValidBitsPerSample
                                   : containerBits;

    static const bool trace = getenv("NAE_TRACE") != nullptr;

    // Test hook: lift the fifo ceiling so an overfeed can actually grow.
    //
    // The canary and the growth measurement turned out to be in direct conflict,
    // which was only visible because both were built. The ceiling sits around
    // 30 000 device frames, and the overfeed control settles the fifo at
    // 22 000-26 000 -- pressed against the cap. The cap does exactly its job: the
    // fifo stops growing and becomes a constant. But that means the control can
    // no longer demonstrate that the measurement detects growth, because growth
    // is what the cap removed.
    //
    // So the cap has to come off for the control to test anything, which is also
    // the honest configuration: the growth detector is meant to catch a fifo
    // creeping over a long run, in a regime where nothing is clamping it. Testing
    // it underneath a clamp tests the clamp.
    //
    // Production is untouched when this is unset. With it set, the same run that
    // grew unboundedly before the canary existed will do so again -- which is
    // the point, and is why it takes an explicit environment variable.
    static const bool capDisabled = getenv("NEIRO_FIFO_CAP_OFF") != nullptr;

    // Test hook: multiply every source request by this, so the fifo grows faster
    // than the device drains it. Set only to prove that a fifo-growth measurement
    // can actually see growth; see the use at the request site.
    static const int64_t overfeed = [] {
        const char* e = getenv("NEIRO_FIFO_OVERFEED");
        const long v = e ? strtol(e, nullptr, 10) : 1;
        return (v > 1 && v <= 64) ? static_cast<int64_t>(v) : 1;
    }();

    // Share of a full device-buffer drain to wait for when there is no room.
    //
    // Read once per process rather than per slice: getenv on the render path is a
    // scan of the environment, and a variable that cannot change while the app is
    // running does not belong there. Overridable so latency_bench can sweep it.
    static const double waitFraction = [] {
        const char* e = getenv("NEIRO_WAIT_FRACTION");
        const double v = e ? std::strtod(e, nullptr) : 0.25;
        // Clamped so a typo cannot turn the wait into a multi-second stall or a
        // busy loop, either of which looks like a hang rather than a setting.
        return (v > 0.0 && v <= 1.0) ? v : 0.25;
    }();
    if (trace)
        std::fprintf(stderr,
                     "[nae] play: source=%u device=%u ch=%d bits=%u valid=%u buffer=%u "
                     "resample=%d dither=%d bitPerfect=%d\n",
                     sourceRate, deviceRate, channels, containerBits, validBits,
                     handle->bufferFrames, handle->resampling ? 1 : 0, handle->dithered ? 1 : 0,
                     handle->bitPerfect ? 1 : 0);

    // Prefill the device's buffer before the stream starts.
    //
    // Twilight Echo's exclusive backend does this between configuring the stream
    // and Start, with `exclusiveInitialRenderFrames` -- the whole buffer when not
    // in push mode -- GetBuffer, memset to zero, and ReleaseBuffer with
    // AUDCLNT_BUFFERFLAGS_SILENT.
    //
    // Without it the device begins playing a buffer nobody has written. WASAPI does
    // not zero it, so whatever was in that memory becomes the first thing the DAC
    // hears, at whatever level it happened to hold. It is one artefact rather than a
    // repeating one, which is worth saying plainly: this is not a candidate for the
    // steady bubbling, it is the start-of-track click that the steady bubbling has
    // been sitting in front of.
    //
    // The half-buffer used below matches push mode's initial render size. Which of
    // the two to use is not measured on this device -- only one of them has been
    // tried -- so it is named rather than implied to be derived.
    // Prefill only the first time a stream is ever started on this handle.
    //
    // At the start of a track, silence before the first sample is correct, and this
    // is what commit e75dd76 added it for. On a *resume* it is not correct: the stop
    // discarded the device buffer and this writes half of one again, which is 26.67
    // ms of silence in the middle of continuous audio. Measured at 100 stop/play
    // cycles, one artefact per cycle, audible from the very first boundary, and
    // unchanged by the fade-in added afterwards -- because the fade is applied to
    // the first real buffer, and this silence has already reached the device by then.
    //
    // So the resume path writes its own first buffer, faded in from zero, and the
    // device never sees a gap.
    const bool firstStartOfStream = !handle->streamEverStarted;
    handle->streamEverStarted = true;
    if (handle->render && firstStartOfStream) {
        const uint32_t bpf = handle->fmt.Format.nBlockAlign;
        const UINT32 prefill = std::max<UINT32>(1, handle->bufferFrames / 2);
        BYTE* prefillData = nullptr;
        UINT32 prefillChunk = 0;
        const HRESULT getHr = neiro::wasapi::getRenderBuffer(
            handle->render.Get(), prefill, &prefillData, &prefillChunk);
        if (bpf > 0 && SUCCEEDED(getHr) && prefillData && prefillChunk > 0) {
            std::memset(prefillData, 0,
                        static_cast<size_t>(prefillChunk) * static_cast<size_t>(bpf));
            remember(handle->render->ReleaseBuffer(prefillChunk, AUDCLNT_BUFFERFLAGS_SILENT));
        }
    }

    // Start the client only when it is not already running.
    //
    // The prefill above and this Start used to run on every call. Because
    // NAE_Play is a slice call -- the host invokes it every 200 ms -- that put a
    // buffer of zeroes and a stream restart into a continuous waveform five times
    // a second. The prefill is a full-scale discontinuity, not a small one, and
    // nothing measured from inside a single call could see it.
    //
    // A resume must be a resume: the device still holds the frames that were
    // queued but not yet played, and they are the right frames.
    if (handle->clientRunning) {
        // Nothing to do. The stream is already running and its buffer is the
        // one the device is draining.
    } else {
    const HRESULT startHr = neiro::wasapi::startClient(handle->client.Get());
    if (FAILED(startHr)) {
        // The device can vanish between a successful Initialize and Start -- a
        // narrow window, but a USB removal inside it is a real event and it used
        // to be reported as "the stream could not be initialised", which is both
        // untrue and unactionable. Same treatment as the three call sites in the
        // render loop.
        if (deviceIsGone(startHr)) {
            g_lastErrorText =
                "the output device was removed or is being used by another program";
            return NAE_ERR_DEVICE_LOST;
        }
        g_lastErrorText = "the output device refused to start";
        return NAE_ERR_INITIALIZE;
    }
    handle->clientRunning = true;
    // A resume begins from silence, because the prefill above filled the device
    // buffer with zeros. If the stream then starts at a non-zero phase, the first
    // sample out of silence is a step, and it is audible as a click at every stop
    // and resume.
    //
    // Measured, not assumed: 100 stop/play cycles produced a bubble per cycle while
    // the fifo-to-fifo seam ratio read 0.88, i.e. the samples the engine writes are
    // continuous. The step is between the silent prefill and the first real buffer,
    // and the seam instrument could not see it because the prefill goes out through
    // a ReleaseBuffer that never touches the fifo.
    //
    // The cure is a short ramp rather than a shorter buffer: 5 to 10 ms, which is
    // long enough to be inaudible as a fade and short enough not to be heard as the
    // track starting late.
    handle->fadeInTotal = (deviceRate * 8) / 1000;
    handle->fadeInRemaining = handle->fadeInTotal;
    handle->fadeInFrame = 0;
    }

    // -20 dBFS was too quiet to judge. Asked to tell whether a periodic click was
    // present in a 1 kHz tone, the answer at 0.1 amplitude is not "no artefact",
    // it is "I could not hear it either way" -- and that is the same as having no
    // measurement. 0.5 sits 6 dB below clipping with a sine's own 3 dB crest
    // factor, so it is loud enough to hear and cannot itself be the fault.
    const char* ampEnv = std::getenv("NAE_SINE_AMPLITUDE");
    const double amplitude = ampEnv ? std::atof(ampEnv) : 0.5;
    if (!(amplitude > 0.0) || amplitude > 1.0) {
        g_lastErrorText = "NAE_SINE_AMPLITUDE must be in (0, 1]";
        return NAE_ERR_INVALID_ARGUMENT;
    }
    // The tone is 1000 Hz. Stated as a constant and divided by the source rate,
// because the frequency has to appear in the expression somewhere and it did
// not.
//
// This read `2 * pi / sourceRate`, which makes `phase` -- a count of *samples* --
// advance through 2*pi once every `sourceRate` samples. That is one cycle per
// second: the engine was emitting a 1 Hz tone while every comment in this file,
// and nae.h, and the probe that checks it, described 1 kHz. Nothing crashed and
// nothing looked wrong; the waveform was a perfectly good sine, just a thousand
// times too low to hear as a tone.
//
// It survived because both the fault and its cause are invisible from outside.
// A 1 Hz sine is smooth and periodic, so "is the output continuous?" answers yes.
// Asking the spectrum tap made it obvious: the window held 4096 samples spanning
// 0.134 rad, where 1 kHz at 192 kHz spans 21.3 cycles.
//
// The measured consequence is worth keeping, because the wrong conclusion was
// reached twice before this. The ring's first value, 0.22531, is
// `0.25 * sin(34281 * 2*pi / 192000)` to five decimals -- so the generator, the
// feed and the ring were all correct, and the only wrong thing was the number
// multiplying the phase. Reading "the ring holds a ramp" as "the tap is broken"
// would have sent the work into the tap, which was never at fault.
static constexpr double kToneHz = 1000.0;
const double twoPiOverSource = 2.0 * 3.14159265358979323846 * kToneHz /
                              static_cast<double>(sourceRate);

    // How far into the source this call is allowed to read.
    //
    // For a tone it is this call's worth: "play 200 ms of sine" means generating
    // 200 ms of sine, and the handle's running `phase` carries the continuity
    // across calls.
    //
    // For a file it is the end of the file, not the end of this call. The
    // `milliseconds` argument then only decides when the call *returns* to the
    // host -- the device buffer is what paces the loop, not the argument -- and
    // treating it as a source limit is what made sliced playback replay the file
    // from the beginning five times a second.
    //
    // The limit is this call's worth of source, measured from where this call
    // starts -- not from the beginning of the stream, and not re-derived from
    // `sourcePos` as it advances.
    //
    // It used to be `milliseconds * sourceRate / 1000 + handle->sourcePos`, read
    // once before the loop, and compared against `sourcePos` inside the loop. That
    // is off by exactly the distance already travelled, so on the second and later
    // slices the condition `sourcePos >= sourceLimit` was true before a single
    // sample was generated: `sourceLimit` had been computed as "start plus one
    // call's worth", and `sourcePos` had already reached that. Every slice after
    // the first rendered silence.
    //
    // This is the fault recorded in this file and in the repo as "at 0.9 amplitude
    // it emitted one transient and then silence", and it is why the sine path was
    // written off as an unreliable control. The waveform generator itself was
    // always correct; nothing was listening to it after the first buffer. It was
    // found by reading the spectrum tap rather than by ear, because a control that
    // emits silence reports "no artefact", which reads exactly like a pass.
    //
    // `sourceStart` is captured before the loop and the progress is measured
    // against it, so a slice continues the last one instead of restarting it.
    const int64_t sourceStart = handle->sourcePos;
    int64_t sourceLimit = sourceStart +
                          (static_cast<int64_t>(milliseconds) *
                           static_cast<int64_t>(sourceRate)) /
                              1000;
#ifdef NEIRO_HAVE_FFMPEG
    if (handle->decoder) {
        sourceLimit = handle->totalSourceFrames;
        if (sourceLimit <= 0) sourceLimit = INT64_MAX;  // unknown length: run until told to stop
    }
#endif

    const uint32_t bytesPerFrame = handle->fmt.Format.nBlockAlign;
    const uint32_t bytesPerSample = containerBits / 8;
    const size_t ch = static_cast<size_t>(channels);

    // The device's own buffer, in frames. This is the unit the render loop
    // works in; everything above it deals in source frames.
    const size_t deviceBlock = handle->bufferFrames;

    // ---- the ring buffer ----
    //
    // Converted frames waiting to go to the device, and source frames not yet
    // fed to the resampler. Both are needed, and the earlier version had neither.
    //
    // What it did instead: ask the device how much room was free, generate
    // exactly that much source, convert it, and write whatever came out. That
    // couples the *resampler's input rate* to *the device's instantaneous free
    // space*, which is tiny and jittery. soxr, given 11 frames at a time, buffers
    // them and returns nothing — so the loop spun, burning source frames, and
    // finished having rendered about 10% of the audio while every format flag
    // still reported "resampling: yes".
    //
    // The fix is the ordinary one: decouple them. Feed the resampler steadily,
    // buffer its output, and serve the device from the buffer.
    //
    // `fifo`, `sourcePos` and `phase` are the handle's, so a slice continues the
    // last one rather than starting over. See the comment on the handle.
    std::vector<float>& fifo = handle->fifo;
    // Channel 0 of the current device buffer, on its way into the spectrum tap.
    // A local rather than a handle field because it belongs to this call, and
    // kept out of the loop body so it is allocated at most once per stream.
    std::vector<double> spectrumChannel;
    int64_t& sourcePos = handle->sourcePos;
    int64_t& phase = handle->phase;
    if (fifo.capacity() < deviceBlock * 2 * static_cast<size_t>(ch)) {
        fifo.reserve(deviceBlock * 2 * static_cast<size_t>(ch));
    }
    // The fifo is left exactly as the previous slice finished it. Everything
    // written into it is whole frames, so it is always frame-aligned, and
    // discarding the head here would drop audio the device has not heard yet.

    std::vector<float> sourceChunk;
    sourceChunk.reserve(deviceBlock * static_cast<size_t>(ch));

    int iterations = 0;
    int64_t deviceFramesWritten = 0;

    // Per-call FIFO accounting, for the trace. `fifoProduced` counts what the
    // resampler handed over, `fifoConsumed` what the device took; both in device
    // frames, matching the fifo's own unit.
    const int64_t sourcePosAtStart = sourcePos;
    const size_t fifoAtStart = fifo.size() / ch;
    size_t fifoProduced = 0;
    size_t fifoConsumed = 0;
    bool sourceDone = sourcePos >= sourceLimit;
    // Whether this handle is playing a file rather than generating a tone.
    bool haveFile = false;
#ifdef NEIRO_HAVE_FFMPEG
    haveFile = handle->decoder != nullptr;
#endif

    // Swap the preloaded file at the source boundary. Returns false when there
    // is nothing to swap to; the caller then treats the track as finished.
    auto atBoundary = [&]() -> bool {
#ifdef NEIRO_HAVE_FFMPEG
        if (!handle->nextDecoder) return false;
        swapNextDecoder(handle);
        sourceLimit = handle->totalSourceFrames > 0 ? handle->totalSourceFrames : INT64_MAX;
        sourceDone = false;
        return true;
#else
        (void)sourceLimit;
        (void)sourceDone;
        return false;
#endif
    };

    // A manual skip. The rest of this track is discarded rather than played
    // out: without clearing the fifo the device would deliver up to a second of
    // the track the listener just skipped past before the new one started.
    if (handle->skipRequested.exchange(false)) {
        fifo.clear();
        // The resampler keeps un-consumed input and soxr's own filter delay,
        // all of it fed from the track just skipped. Carrying it means the head
        // of the new track is convolved with the tail of the old one -- a
        // smear of the wrong file at the exact moment the listener skipped to
        // get away from it. `NAE_Seek` resets for the same reason; a natural
        // gapless boundary deliberately does not, because there continuity is
        // the point.
        if (handle->resampler.ready()) {
            handle->resampler.reset();
        }
        sourcePos = sourceLimit;
        sourceDone = true;
        atBoundary();
    }

    // Feed the resampler until it has produced at least `want` device frames, or
    // the source has run out. Bounded, so a pathological ratio cannot spin here.
    // Set when a device call reported that the endpoint is gone, which is not the
    // same thing as the stream finishing early. It changes the return code and
    // nothing else: the handle, the decoder, the source position and the queue
    // are all still valid, and the caller can resume from here.
    bool deviceLost = false;

    auto topUp = [&](size_t want, std::string* err) -> bool {
        const uint32_t sourceCh = handle->routingActive ? handle->sourceChannels : ch;
        // The resampler only emits output once it has a whole input block, so a
        // request smaller than one block produces nothing at all no matter how
        // many times it is repeated. `minSource` is that floor, and every request
        // is clamped up to it.
        //
        // Without this, `topUp` would consume source, hand it to a resampler that
        // cannot use it yet, and return true -- reporting a satisfied request it
        // had not satisfied. The render loop would then spin without ever writing
        // to the device, and the whole thing would look like a slow, stuttering
        // player rather than the arithmetic error it is.
        const int64_t minSource =
            handle->resampler.ready() ? static_cast<int64_t>(handle->resampler.input_block_frames())
                                      : 1;

        // The fifo ceiling, in device frames.
        //
        // Scaled from the resampler's own geometry rather than fixed, so it moves
        // with the block size and the rate ratio instead of becoming a constant
        // that is safe for one configuration and wrong for another. The multiple
        // is deliberately generous: this is meant to be unreachable, because a
        // limit that is ever hit in normal operation is a stutter, not a warning.
        const size_t fifoCap =
            capDisabled
                ? static_cast<size_t>(-1)
                : static_cast<size_t>(handle->resampler.ready()
                                          ? handle->resampler.input_block_frames() * 8
                                          : static_cast<size_t>(deviceRate)) *
                          4 +
                      8192;

        for (int guard = 0; guard < 64; ++guard) {
            // Swap at the source boundary before asking whether the fifo is
            // already full. The fifo is the buffer across the seam; waiting for
            // it to drain first is what turns a gapless boundary into a gap.
            if (sourcePos >= sourceLimit) {
                if (!atBoundary()) return true;
            }
            if (fifo.size() / ch >= want) return true;

            // A canary, not a working limit. Steady state across every format and
            // rate measured is 328-2289 device frames, and this ceiling sits
            // several times above that, so a healthy stream never reaches it.
            //
            // It exists because the fifo lives on the handle and nothing else
            // constrains it. If a future change makes the loop produce faster than
            // the device drains, the symptom would otherwise be memory that grows
            // for as long as the track plays while every format flag still reads
            // correctly. Hitting this stops production and records why, which
            // turns a slow leak into a visible event.
            //
            // Not an error return: the audio already queued is valid and the
            // device is still draining it, so failing here would turn a slow
            // accumulation into an actual dropout.
            if (fifo.size() / ch >= fifoCap) {
                if (!handle->fifoCapped) {
                    handle->fifoCapped = true;
                    g_lastErrorText =
                        "playback buffer reached its ceiling; production stopped to keep it "
                        "bounded";
                    if (trace)
                        std::fprintf(stderr,
                                     "[nae] FIFO CAPPED at %zu frames (cap %zu) at source "
                                     "%lld/%lld -- production stopped\n",
                                     fifo.size() / ch, fifoCap,
                                     static_cast<long long>(sourcePos),
                                     static_cast<long long>(sourceLimit));
                }
                return true;
            }

            // Generate enough source to yield `want - have` device frames, with
            // slack for the resampler's own latency.
            const size_t have = fifo.size() / ch;
            const size_t needDevice = want > have ? want - have : 0;
            int64_t needSource = (static_cast<int64_t>(needDevice) * sourceRate + deviceRate - 1) /
                                     deviceRate +
                                 64;
            if (needSource < minSource) needSource = minSource;

            // Test hook. Deliberately present in the shipped binary, because a
            // diagnostic that only exists in a debug build is a diagnostic that
            // cannot be run against the thing that misbehaves -- and the point of
            // it is to prove the measurement can see a fifo that grows. A flat
            // line from an instrument that cannot detect growth is worse than no
            // line at all, and the only way to tell them apart is to make it grow
            // on purpose and check that the number moves.
            if (overfeed > 1) needSource *= overfeed;
            const int64_t available = sourceLimit - sourcePos;
            if (needSource > available) needSource = available;
            if (needSource <= 0) return true;

            sourceChunk.resize(static_cast<size_t>(needSource) * ch);

            if (haveFile) {
                if (handle->routingActive) {
                    handle->routingIn.clear();
                    const bool got_full = handle->decoder &&
                        handle->decoder->readExact(&handle->routingIn,
                                                   static_cast<size_t>(needSource), err);
                    const size_t gotSourceFrames =
                        handle->routingIn.size() / (sourceCh ? sourceCh : 1);
                    if (gotSourceFrames == 0) {
                        handle->sourceExhausted = true;
                        sourcePos = sourceLimit;
                        if (atBoundary()) continue;
                        return true;
                    }
                    handle->routingIn.resize(static_cast<size_t>(needSource) * sourceCh, 0.0f);
                    handle->routingOut.resize(static_cast<size_t>(needSource) * ch);
                    handle->routing.process(handle->routingIn.data(),
                                            static_cast<size_t>(needSource),
                                            handle->routingOut.data());
                    sourceChunk.assign(handle->routingOut.begin(), handle->routingOut.end());
                    if (!got_full) handle->sourceExhausted = true;
                } else {
                // Pull from the file.
                //
                // readExact *appends*, so the buffer is cleared first: the resize
                // above sized it, and leaving that in place would have every
                // block's samples land after a run of zeros.
                sourceChunk.clear();
                int64_t remaining = needSource;
                if (handle->prefetchedPos < handle->prefetchedFrames && remaining > 0) {
                    const size_t take = std::min(
                        static_cast<size_t>(remaining),
                        handle->prefetchedFrames - handle->prefetchedPos);
                    sourceChunk.insert(
                        sourceChunk.end(),
                        handle->prefetched.begin() +
                            static_cast<long>(handle->prefetchedPos * ch),
                        handle->prefetched.begin() +
                            static_cast<long>((handle->prefetchedPos + take) * ch));
                    handle->prefetchedPos += take;
                    remaining -= static_cast<int64_t>(take);
                    if (handle->prefetchedPos >= handle->prefetchedFrames) {
                        handle->prefetched.clear();
                        handle->prefetchedPos = 0;
                        handle->prefetchedFrames = 0;
                    }
                }
                bool got_full = true;
                if (remaining > 0) {
                    if (handle->decodeRunning) {
                        const size_t got = readDecodedFrames(
                            handle, static_cast<size_t>(remaining), &sourceChunk);
                        got_full = got == static_cast<size_t>(remaining);
                    } else {
                        got_full = handle->decoder->readExact(
                            &sourceChunk, static_cast<size_t>(remaining), err);
                    }
                }
                if (!got_full) {
                    // Short read: the end of the track, not a failure. Keep what
                    // arrived, zero-pad to the requested length so the resampler
                    // still receives a full block, and let `sourcePos` reaching
                    // `sourceLimit` stop the stream cleanly a moment later.
                    const size_t got = sourceChunk.size() / ch;
                    if (got == 0) {
                        handle->sourceExhausted = true;
                        sourcePos = sourceLimit;
                        // The decoder ended on this read. Swap before returning:
                        // returning with an empty fifo makes the render loop pad
                        // one whole buffer while the next file waits.
                        if (atBoundary()) continue;
                        return true;
                    }
                    sourceChunk.resize(got * ch, 0.0f);
                    sourceChunk.resize(static_cast<size_t>(needSource) * ch, 0.0f);
                } else if (sourceChunk.size() != static_cast<size_t>(needSource) * ch) {
                    // Belt and braces. `readExact` promises to hand back exactly
                    // what was asked for and keeps the remainder, so this should be
                    // unreachable -- but the entire failure mode it guards against
                    // is the pipeline receiving more audio than the source
                    // position accounts for, and that is far too expensive a bug
                    // to leave resting on a callee's good behaviour.
                    sourceChunk.resize(static_cast<size_t>(needSource) * ch);
                }
                }
            } else {
                // Synthesise the test tone.
                //
                // The phase is the handle's, not this call's. Resetting it per
                // call would restart the waveform at every slice boundary, and
                // 200 ms is not a whole number of 1 kHz cycles, so that is an
                // audible discontinuity rather than a harmless restart.
                for (int64_t f = 0; f < needSource; ++f) {
                    const double s =
                        amplitude * std::sin(static_cast<double>(phase) * twoPiOverSource);
                    ++phase;
                    for (size_t c = 0; c < ch; ++c) {
                        sourceChunk[static_cast<size_t>(f) * ch + c] = static_cast<float>(s);
                    }
                }
                // Diagnostic, NAE_TRACE_SPECTRUM.
                //
                // Placed at the generator rather than at the tap, because the
                // question this answers is "is the tone being made correctly",
                // and by the time a reading reaches the tap it has been through
                // the resampler, the fifo and the fade-in -- three places that
                // can turn a correct 1 kHz into something else without any of
                // them being wrong.
                //
                // `twoPiOverSource` and `amplitude` are printed because they are
                // the two constants that decide what comes out, and a probe that
                // only reads the output cannot tell a wrong constant from a
                // right one followed by a wrong rate.
                if (std::getenv("NAE_TRACE_SPECTRUM")) {
                    std::fprintf(stderr,
                                 "[nae:tone] needSource=%lld sourceRate=%u twoPiOverSource=%.12g "
                                 "amp=%.9g phase=%lld resampler=%d\n",
                                 static_cast<long long>(needSource), sourceRate, twoPiOverSource,
                                 amplitude, static_cast<long long>(phase),
                                 handle->resampler.ready() ? 1 : 0);
                    std::fprintf(stderr, "[nae:tone] first:");
                    for (int64_t f = 0; f < needSource && f < 10; ++f) {
                        std::fprintf(stderr, " %.6f", static_cast<double>(sourceChunk[static_cast<size_t>(f) * ch]));
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            bool crossfadeTookOver = false;
#ifdef NEIRO_HAVE_FFMPEG
            if (haveFile && handle->crossfadeEnabled && handle->nextDecoder &&
                handle->crossfadeFrames > 0 && handle->fileDurationMs > 0) {
                const int64_t endFrames = static_cast<int64_t>(
                    std::llround(handle->fileDurationMs / 1000.0 * sourceRate));
                const int64_t fadeFrames =
                    std::min<int64_t>(handle->crossfadeFrames, endFrames);
                const int64_t fadeStart = endFrames - fadeFrames;
                const size_t frames = sourceChunk.size() / ch;
                if (frames > 0 && fadeFrames > 0 &&
                    sourcePos + static_cast<int64_t>(frames) > fadeStart) {
                    if (!handle->crossfadeActive) {
                        handle->crossfadeActive = true;
                        handle->crossfadeBuf = std::move(handle->nextPrefetched);
                        handle->nextPrefetchedFrames = 0;
                        handle->crossfadeBufPos = 0;
                        handle->crossfadeBufFrames = handle->crossfadeBuf.size() / ch;
                        handle->crossfadeConsumed = 0;
                    }
                    bool crossed = false;
                    for (size_t f = 0; f < frames; ++f) {
                        const int64_t pos = sourcePos + static_cast<int64_t>(f);
                        if (pos < fadeStart) continue;
                        if (pos >= endFrames) crossed = true;
                        if (handle->crossfadeBufPos >= handle->crossfadeBufFrames) {
                            handle->crossfadeBuf.clear();
                            handle->crossfadeBufPos = 0;
                            std::string readErr;
                            handle->nextDecoder->readExact(&handle->crossfadeBuf, 4096,
                                                           &readErr);
                            handle->crossfadeBufFrames =
                                handle->crossfadeBuf.size() / ch;
                            if (handle->crossfadeBufFrames == 0) break;
                        }
                        const size_t bp = handle->crossfadeBufPos++;
                        ++handle->crossfadeConsumed;
                        double t = static_cast<double>(pos - fadeStart) /
                                   static_cast<double>(fadeFrames);
                        t = std::max(0.0, std::min(1.0, t));
                        double outGain = 1.0 - t;
                        double inGain = t;
                        if (handle->crossfadeCurve == NAE_CROSSFADE_EQUAL_POWER) {
                            outGain = std::cos(t * 3.14159265358979323846 / 2.0);
                            inGain = std::sin(t * 3.14159265358979323846 / 2.0);
                        }
                        for (size_t c = 0; c < ch; ++c) {
                            const double cur = sourceChunk[f * ch + c];
                            const double nxt = handle->crossfadeBuf[bp * ch + c];
                            sourceChunk[f * ch + c] = static_cast<float>(
                                pos >= endFrames ? nxt : cur * outGain + nxt * inGain);
                        }
                    }
                    if (crossed) {
                        const size_t remaining =
                            handle->crossfadeBufFrames - handle->crossfadeBufPos;
                        swapNextDecoder(handle);
                        if (remaining > 0) {
                            handle->prefetched.assign(
                                handle->crossfadeBuf.begin() +
                                    static_cast<long>(handle->crossfadeBufPos * ch),
                                handle->crossfadeBuf.end());
                            handle->prefetchedPos = 0;
                            handle->prefetchedFrames = remaining;
                        }
                        handle->sourcePos = handle->crossfadeConsumed;
                        sourceLimit = handle->totalSourceFrames > 0
                                          ? handle->totalSourceFrames
                                          : INT64_MAX;
                        sourceDone = false;
                        crossfadeTookOver = true;
                        handle->crossfadeActive = false;
                        handle->crossfadeBuf.clear();
                        handle->crossfadeBufPos = 0;
                        handle->crossfadeBufFrames = 0;
                        handle->crossfadeConsumed = 0;
                    }
                }
            }
#endif

            if (handle->convEnabled && handle->convTaps > 0 && !sourceChunk.empty()) {
                const size_t frames = sourceChunk.size() / ch;
                constexpr size_t kConvLatency = neiro::PartitionedConvolver::blockSize();
                const size_t drySize = kConvLatency * ch;
                if (handle->convDryBuf.size() != drySize) {
                    handle->convDryBuf.assign(drySize, 0.0f);
                    handle->convDryPos = 0;
                }
                // The wet path is delayed by one block. Delay the dry path by
                // the same amount, per channel, so a mix does not comb-filter.
                // No allocation in the render path: the buffers are reused and
                // only grow when the source chunk does.
                if (handle->convInBuf.size() < sourceChunk.size()) {
                    handle->convInBuf.resize(sourceChunk.size());
                    handle->convDryOut.resize(sourceChunk.size());
                }
                std::vector<float>& convIn = handle->convInBuf;
                std::vector<float>& dryOut = handle->convDryOut;
                for (size_t f = 0; f < frames; ++f) {
                    const size_t pos = handle->convDryPos;
                    for (size_t c = 0; c < ch; ++c) {
                        const size_t idx = pos * ch + c;
                        dryOut[f * ch + c] = handle->convDryBuf[idx];
                        handle->convDryBuf[idx] = sourceChunk[f * ch + c];
                        convIn[f * ch + c] = sourceChunk[f * ch + c];
                    }
                    handle->convDryPos = (pos + 1) % kConvLatency;
                }
                handle->conv.process(convIn.data(), frames, ch);
                const double wet = handle->convWet * handle->convGain;
                const double dry = handle->convDry;
                for (size_t f = 0; f < frames; ++f) {
                    for (size_t c = 0; c < ch; ++c) {
                        const size_t pos = f * ch + c;
                        sourceChunk[pos] = static_cast<float>(dryOut[pos] * dry +
                                                              convIn[pos] * wet);
                    }
                }
            }

            if (!crossfadeTookOver) sourcePos += needSource;

            if (handle->resampler.ready()) {
                const size_t before = fifo.size();
                if (!handle->resampler.process(sourceChunk, &fifo, err)) return false;
                fifoProduced += (fifo.size() - before) / ch;
            } else {
                // No converter: source samples ARE the device samples. Copy
                // straight into the fifo, which is the bit-perfect path.
                fifo.insert(fifo.end(), sourceChunk.begin(), sourceChunk.end());
            }
        }
        return true;
    };

    // How much device output this call is allowed to produce.
    //
    // `milliseconds` is a time budget for the *call*, and it has to be enforced
    // here rather than inferred from the source running out. It was not enforced
    // at all: the loop's only exit was the source being exhausted, which for a
    // tone coincided with the budget (a tone's source limit was set from
    // `milliseconds`) and so looked correct, while for a file it meant a single
    // 200 ms call consumed a five-minute track in one go. The tone was hiding it.
    //
    // Measured in device frames rather than source frames on purpose. The device
    // is what keeps real time, and this budget is about wall clock.
    const int64_t targetFrames =
        (static_cast<int64_t>(milliseconds) * static_cast<int64_t>(deviceRate)) / 1000;

    // Counts iterations that produced no device output. Without this, a device
    // that stops accepting buffers -- unplugged mid-track, or claimed by another
    // program -- leaves the loop sleeping and topping up forever, because nothing
    // it does can advance `deviceFramesWritten` toward the budget it is waiting
    // for. Two seconds of no progress is a stall, not slowness.
    int stalledIterations = 0;

    // Wall clock of the previous device-buffer write, for the deadline measurement.
    ULONGLONG lastWriteTick = 0;

    while (deviceFramesWritten < targetFrames) {
        // Recomputed every pass, not set once before the loop. It used to be
        // initialised and then never updated, so the loop condition
        // `!sourceDone || fifo non-empty` was permanently true and the only exit
        // was an explicit break -- which is exactly the bug the budget above
        // would otherwise have hidden.
        sourceDone = sourcePos >= sourceLimit;

        if (sourceDone && fifo.size() / ch == 0) {
            if (!atBoundary()) break;
        }

        UINT32 padding = 0;
        ensureAudioThreadQos();

        // Wait for the period before measuring padding. The padding is what tells
        // us how much room is left, and asking a moment before the period rolls
        // over is how the request lands on the wrong side of the deadline.
        if (handle->renderEvent) {
            if (WaitForSingleObject(handle->renderEvent, 2000) != WAIT_OBJECT_0) {
                ++handle->underrunCount;
            }
            if (sourceDone && fifo.size() / ch == 0) {
                if (!atBoundary()) break;
            }
        }

        const HRESULT padHr = remember(handle->client->GetCurrentPadding(&padding));        if (FAILED(padHr)) {
            // The same device-lost handling as GetBuffer and ReleaseBuffer below.
            //
            // This call site was the one that was missed, and it is the one that
            // fires first in practice. GetCurrentPadding asks how many frames are
            // sitting in the device buffer; a device that has been disabled or
            // unplugged cannot answer, and it is reached at the top of every
            // iteration -- before the buffer is ever requested. So a disabled
            // device produced AUDCLNT_E_RESOURCES_INVALIDATED here, fell through
            // to the short-render check, and came back as NAE_ERR_INITIALIZE with
            // the HRESULT buried in the text. The two calls that *were* wrapped
            // never got the chance to run.
            //
            // Found by the device-loss test rather than by reading the code,
            // which is the argument for having run it: every reasoning pass over
            // "which calls can fail when the device disappears" had listed the
            // two that write audio and skipped the one that measures.
            if (deviceIsGone(padHr)) {
                deviceLost = true;
                g_lastErrorText = "the output device was removed or is being used by "
                                  "another program; playback is paused at the current "
                                  "position";
            }
            break;
        }

        // The runway, and the clock that judges it.
        //
        // `padding` is how many frames the device still has queued. At the rate it
        // is consuming them, that is `padding / rate` seconds of audio before it
        // runs dry. Everything this loop does from here to `ReleaseBuffer` has to
        // fit inside that, or the device plays what it already had -- stale audio,
        // heard as a click.
        //
        // This is the figure every existing counter was blind to. `padFrameCount`
        // counts the fifo running dry, and it stays 0 because the fifo *is* full;
        // the audio is simply handed over late. `maxBufferGapMs` measures the gap
        // between one write and the next, which says nothing about whether this
        // one arrived in time. `underrunCount` counts a wait that timed out, and
        // the wait succeeds every time.
        //
        // Twilight Echo's engine runs the same device, the same format and the same
        // mode, and is clean on the same file. Its render loop never decodes: the
        // decoder runs on its own thread. This loop calls `readExact` from inside
        // the period, so whatever the decode costs is spent out of the runway.
        const double runwayMs =
            (handle->fmt.Format.nSamplesPerSec > 0)
                ? (static_cast<double>(padding) * 1000.0 /
                   static_cast<double>(handle->fmt.Format.nSamplesPerSec))
                : 0.0;
        const double fillStartMs = neiro::wasapi::nowMsHr();

        // How much of the buffer is ours to write right now: whatever the device has not    // already queued.
    //
    // This was "the whole buffer" for a few commits, on the reasoning that
    // `GetCurrentPadding` is a shared-mode quantity with no meaning in exclusive.
    // That reasoning was wrong, and the reference implementation says so directly:
    // Twilight Echo's `WasapiExclusiveBackend.cpp` computes
    // `bufferFrameCount - GetCurrentPadding()` in its exclusive render loop, at
    // both the steady-state and the catch-up call sites. The device tracks its own
    // exclusive buffers and padding is how much of one is already committed, so
    // subtracting it is not only harmless here, it is the correct request.
    //
    // Asking for the whole buffer anyway overshot into territory the device would
    // not sustain, and playback ran at 40960 frames per second against 48000.
// The whole buffer, always.
    //
    // This is what Twilight Echo's `exclusiveRenderFrames` does on its default
    // path: `if (!pushMode) return bufferFrameCount;` -- padding is only subtracted
    // in push mode. `exclusiveInitialRenderFrames` is the same shape. So the
    // reference does not ask for a partial buffer on the ordinary path either, and
    // asking for less than `GetBufferSize` leaves the remainder of the device's
    // buffer holding whatever the previous fill put there.
    //
    // Asking for the whole buffer was tried once before and played at 85.3%, which
    // was read as the endpoint's own shortcoming. It was not: that run was under
    // `Sleep(1)` pacing, and the same request now that the cadence comes from a
    // high-resolution waitable timer is a different experiment. The earlier result
    // was real, and the explanation attached to it was wrong.
    // What is ours to write: the buffer minus what the device has already committed.
    //
    // Subtracting padding is what Twilight Echo's `exclusiveRenderFrames` does in
    // push mode, and it is what plays at real time here: 48722 frames per second
    // against a 48000 rate. Requesting the whole buffer instead -- which is what
    // that same function does on its non-push path, and what was tried here twice --
    // measures 40960, or 85.3%, both times.
    //
    // That is the endpoint's own behaviour rather than a pacing fault, and the two
    // runs bracket the fix for it: the first predates the high-resolution timer, the
    // second postdates it, and both landed on the same number. So push mode is the
    // shape this endpoint sustains, and the choice is written down rather than left
    // looking arbitrary.
    // the combination the reference actually runs, and it has not been measured here.
    // What is ours to write: the buffer minus what the device has already committed.
    // What is ours to write: the buffer minus what the device has already committed.
    //
    // Twilight Echo chooses between two request shapes by mode, and this endpoint
    // wants the push-mode one. Asked for the whole buffer, it delivers 40960 frames
    // per second against a 48000 rate -- 85.3% -- and it does that under every pacing
    // arrangement tried here: Sleep polling, a high-resolution waitable timer, and
    // `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`. Three independent measurements, one
    // number, so this is the device's behaviour and not the loop's.
    //
    // Subtracting padding delivers 48718 against 48000, which is real time to within
    // the measurement's window granularity. So the request shape is settled by
    // measurement rather than by which mode the reference happens to default to,
    // and the reason is written here so it does not look like an arbitrary choice.
    const UINT32 capacity =
        handle->bufferFrames > padding ? handle->bufferFrames - padding : 0;
        if (capacity == 0) {
            // Only reachable if the open recorded a zero-sized buffer, which the
            // open path already rejects. Kept because a divide-by-zero later
            // would be a worse thing to debug than a branch nobody expects.
            // The device buffer is full: there is nothing to write, so there is
            // nothing to produce. Wait.
            //
            // This branch used to call `topUp(bufferFrames)` "so the converter
            // keeps a steady input rate". That was backwards. A full device buffer
            // means the consumer has stopped, so every source frame pulled here
            // went straight into the fifo and was paid for by nobody. Because the
            // resampler only emits in whole input blocks, each of those pulls was
            // much larger than the demand that would follow it, and the fifo grew
            // without bound: a DSD stream ran the playhead at 122% of real time.
            //
            // There is no starvation to avoid. soxr holds its filter state between
            // calls, and `topUp` pulls whole blocks on demand, so a converter that
            // is not being fed for a few milliseconds loses nothing.
            //
            // How long to wait is arithmetic, not a guess. The device consumes
            // frames at a known rate, so a full buffer drains in
            // bufferFrames / deviceRate and any fraction of that is a known
            // duration. Polling instead -- fill every free slot, sleep 1 ms, ask
            // again -- took about fifty attempts per buffer cycle and held a core
            // at 100% for the whole track, which is a poor trade for a loop that
            // is supposed to be paced by the device's clock.
            //
            // `waitFraction` is the share of a full buffer drain to wait for, and
            // is overridable because the right value is a trade between wake-ups
            // and burst size rather than something to be guessed at once. Measured
            // with latency_bench; see README.
            const double bufferMs =
                deviceRate ? static_cast<double>(handle->bufferFrames) * 1000.0 / deviceRate : 1.0;
            double waitMs = bufferMs * waitFraction;
            if (waitMs < 0.05) waitMs = 0.05;
            if (waitMs > 25.0) waitMs = 25.0;
            // `Sleep(1)` is not a one-millisecond sleep. The Windows timer
            // resolution is 1 to 15 ms and whichever it happens to be is what you
            // get.
            //
            // Measured on the endpoint this engine actually opens: the device's
            // buffer is about 275 frames -- 5.7 ms at 48 kHz -- while this wait was
            // `bufferMs * 0.25`, clamped up to 1 ms and then rounded to
            // `Sleep(1)`. A 15 ms nap against a 5.7 ms deadline means the device
            // runs dry at every buffer boundary, and the symptom is a click at the
            // buffer rate: about 175 a second, identical throughout the track,
            // unchanged by the volume control, and absent from every counter the
            // engine keeps -- because from this loop's side each write still is a
            // full, correct, continuous buffer. It simply arrived too late.
            waitForDeviceBuffer(waitMs);

            if (++stalledIterations > 2000) {
                g_lastErrorText = "the output device stopped accepting audio";
                break;
            }
            continue;
        }

        // Paused: hand the device silence, and touch nothing else.
        //
        // Placed before `topUp` on purpose. `topUp` is the only thing in this loop
        // that consumes source, and a pause that decoded would move
        // NAE_Position -- so a 30 s pause would drop 30 s of the track on the floor
        // and resume somewhere the listener never asked for. The fifo is also left
        // alone, which is what makes the resume land on the same sample.
        //
        // The device still gets GetBuffer/ReleaseBuffer every period. That is the
        // point: a running stream that is fed nothing runs dry and clicks, and a
        // dry stream is the artefact this whole exercise is about.
        //
        // AUDCLNT_BUFFERFLAGS_SILENT rather than a memset. The contents of the
        // buffer are then never read, so this cannot be the "prefill seam" the seam
        // instrument is blind to -- there is no seam, there is no audio there.
        if (handle->silencing) {
            BYTE* silentRaw = nullptr;
            UINT32 silentChunk = 0;
            const HRESULT silentHr = neiro::wasapi::getRenderBuffer(
                handle->render.Get(), capacity, &silentRaw, &silentChunk);
            if (silentHr == AUDCLNT_E_BUFFER_TOO_LARGE || silentHr == S_FALSE) {
                if (handle->renderEvent) {
                    if (WaitForSingleObject(handle->renderEvent, 2000) != WAIT_OBJECT_0) {
                        ++handle->underrunCount;
                    }
                } else {
                    spinForBuffer();
                }
                if (++stalledIterations > 2000) {
                    g_lastErrorText = "the output device stopped accepting audio";
                    break;
                }
                continue;
            }
            if (FAILED(silentHr)) {
                if (deviceIsGone(silentHr)) {
                    deviceLost = true;
                    g_lastErrorText = "the output device was removed or is being used by "
                                      "another program; playback is paused at the current "
                                      "position";
                }
                break;
            }
            const HRESULT relHr =
                handle->render->ReleaseBuffer(silentChunk, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(relHr) && deviceIsGone(relHr)) {
                deviceLost = true;
                g_lastErrorText = "the output device was removed or is being used by "
                                  "another program; playback is paused at the current "
                                  "position";
                break;
            }
            deviceFramesWritten += silentChunk;
            handle->silenceFramesWritten += static_cast<int64_t>(silentChunk);
            // Counted in the rate window as well, deliberately. See the field's
            // documentation: `deviceRateActual` claims device frames per second of
            // wall clock, and the device is consuming these frames at exactly the
            // rate it would consume audio. Leaving them out made the figure read
            // half speed through a pause, which is a claim about playback that was
            // not true. The pause is reported by `silenceFramesWritten` instead --
            // one field, one claim, rather than one field carrying both.
            chargeRateWindow(handle, silentChunk);
            // The tap is fed the silence too, not skipped.
            //
            // Skipping it would leave the previous window in the ring, and the
            // display would carry on showing a spectrum that looks exactly like
            // live music while nothing is playing. A paused player drawing a live
            // spectrum is claiming to be playing, and `framesFed` is what tells a
            // caller the reading is genuine silence rather than a leftover.
            feedSpectrum(handle, nullptr, static_cast<size_t>(silentChunk));
            continue;
        }

        // A reused string: `topUp` writes into it and an empty string does not
        // allocate, but the construction inside the render loop did.
        handle->renderErr.clear();
        const double decodeStartMs = neiro::wasapi::nowMsHr();
        const bool toppedUp = topUp(capacity, &handle->renderErr);
        const double decodeMs = neiro::wasapi::nowMsHr() - decodeStartMs;
        if (decodeMs > handle->maxDecodeStallMs) handle->maxDecodeStallMs = decodeMs;
        if (!toppedUp) {
            if (trace) std::fprintf(stderr, "[nae] topUp: %s\n", handle->renderErr.c_str());
            break;
        }
        if (fifo.size() / ch == 0) {
            // Nothing left to give and the fifo is dry.
            //
            // Do NOT flush the resampler here. Flushing is permanent: soxr's
            // _soxr_input() returns NULL once the resampler is in flushing
            // state, and soxr_input() then hands that NULL to its deinterleave
            // as the destination -- so the next block of input writes to address
            // zero and the process dies with no message.
            //
            // That is not hypothetical. The host renders in 200 ms slices, each
            // one a separate NAE_PlaySine call on this same handle, so flushing
            // at the end of every slice made the *second* slice fatal. The
            // engine's own harnesses render in a single call per stream and
            // never saw it.
            //
            // The tail is flushed once, by NAE_Close, when the stream really is
            // finished. See NAE_Flush's comment in nae.h.
            if (sourcePos >= sourceLimit && !atBoundary()) break;
            continue;
        }

        BYTE* raw = nullptr;
        UINT32 chunk = 0;
        const HRESULT bufHr =
            neiro::wasapi::getRenderBuffer(handle->render.Get(), capacity, &raw, &chunk);
        if (bufHr == AUDCLNT_E_BUFFER_TOO_LARGE || bufHr == S_FALSE) {
            // "Not right now", not "never again".
            //
            // Asking for a full buffer before the engine has one free is the
            // normal case in exclusive mode, and WASAPI says so with
            // AUDCLNT_E_BUFFER_TOO_LARGE rather than by blocking. Treating it as
            // fatal is what stopped the loop after exactly one buffer: the trace
            // read `devWritten=9728 budget=288000` with an hresult of zero, which
            // is a renderer that gave up rather than one that failed.
            //
            // The other half of the fix is asking for the whole buffer. The old
            // code asked for `bufferFrames - GetCurrentPadding`, and padding is a
            // shared-mode quantity with no meaning here; on this endpoint it came
            // back at about 89% of the buffer, so every buffer was one ninth
            // written and the device played back the previous fill's leftovers.
            // That was the bubbling.
            // Wait to be told, rather than asking. Two seconds is a backstop, not a cadence:
            // with the event available the wait ends when the device says so.
            if (handle->renderEvent) {
                if (WaitForSingleObject(handle->renderEvent, 2000) != WAIT_OBJECT_0) {
                    ++handle->underrunCount;
                }
            } else {
                spinForBuffer();
            }
            if (++stalledIterations > 2000) {
                g_lastErrorText = "the output device stopped accepting audio";
                break;
            }
            continue;
        }
        if (FAILED(bufHr)) {
            // A device that is gone ends the stream now, with its own error code,
            // rather than being folded into the short-render check below. That
            // check already returned an error for this case, so the difference is
            // not that the failure becomes visible -- it is that the failure stops
            // looking like every other one. A host that sees NAE_ERR_DEVICE_LOST
            // knows to keep the handle, tell the user, and wait; a host that sees
            // NAE_ERR_INITIALIZE would tear the stream down, and the user would
            // lose their place to an unplugged cable.
            if (deviceIsGone(bufHr)) {
                deviceLost = true;
                g_lastErrorText = "the output device was removed or is being used by "
                                  "another program; playback is paused at the current "
                                  "position";
                break;
            }
            break;
        }
        if (chunk == 0 || raw == nullptr) {
            // Same fault as the wait on the other branch: `Sleep(1)` on this
            // machine is a 15 ms nap, and this device's buffer is 5.7 ms. Waiting
            // for a quarter of a buffer drain is right in principle and only if
            // the wait can actually be that short.
            const double ms = deviceRate ? static_cast<double>(handle->bufferFrames) * 1000.0 /
                                              deviceRate
                                        : 5.0;
            spinForBuffer();
            if (++stalledIterations > 2000) {
                g_lastErrorText = "the output device stopped accepting audio";
                break;
            }
            continue;
        }
        ++iterations;

        // The buffer is always filled completely, and never released short.
        //
        // `framesToWrite = min(have, chunk)` -- releasing only what the fifo
        // happened to hold -- is what produced the bubbling. WASAPI does not zero
        // the unwritten remainder of a buffer, so a short release plays back
        // whatever the *previous* fill left there: a fragment of stale audio,
        // spliced in wherever the fifo ran dry. It does not sound like a gap, it
        // sounds like a bubble, which is why it was not read as an underrun.
        //
        // Measured: the engine's own `underrunCount` and `dropCount` were both 0
        // while the output burbled, because this path was never counted as an
        // event at all. The only evidence was `callbackCount` arriving at about 3
        // per second against the ~19 the buffer size implies, in bursts.
        //
        // Holding the last sample rather than writing zeros is deliberate. A zero
        // pad is a step discontinuity in the waveform and that is a click; holding
        // is a flat line at whatever the signal was doing when it ran out, which
        // is the least audible thing available. The real fault is the fifo running
        // dry -- decoding happens on this thread -- and this contains the symptom
        // rather than pretending to be the cure.
        const size_t have = fifo.size() / ch;
        const size_t realFrames = have < chunk ? have : chunk;
        const size_t framesToWrite = chunk;

        // What to pad with: silence.
        //
        // Holding the last sample was the original choice, on the reasoning that a
        // flat line is less audible than a gap. Twilight Echo's
        // `renderFloatCallbackWithTailSilence` does the opposite -- it zero-fills any
        // tail the callback under-rendered -- and `typedCallback`'s integer path
        // memsets the remainder for the same reason. Silence is what a device
        // expects to be told, and a held sample is a DC step at the seam.
        //
        // Measured `padFrameCount` is 0 over a 14 s run, so this branch does not
        // currently fire on this device and changing it is correctness rather than
        // a fix. It is here so the two engines agree on the semantics.
        // Reused across buffers: this runs inside the render callback, whose
        // budget is the device period, and a vector construction here spends
        // part of it on the allocator.
        if (handle->frameOut.size() != ch) {
            handle->frameOut.assign(ch, 0.0);
            handle->frameFloat.assign(ch, 0.0f);
        }
        if (handle->frameHeld.size() != ch) handle->frameHeld.assign(ch, 0.0);
        std::vector<double>& held = handle->frameHeld;
        std::vector<double>& frameOut = handle->frameOut;
        std::vector<float>& frameFloat = handle->frameFloat;
        // Channel 0 of this buffer, collected for the tap. Declared outside the
        // loop and only grown, because allocating inside the render loop spends
        // the runway the device gave this thread.
        if (spectrumChannel.size() < framesToWrite) spectrumChannel.resize(framesToWrite);

        // The two measured levels. Cheap, unconditional, and only worth having
        // because "too quiet" has been impossible to place: every layer of the
        // output path *looked* correct by inspection, and inspection cannot tell
        // you that a device opened for 16 bits is being handed 4 bytes a sample.
        //
        // `writePeak` is the container's own full scale, so `writtenPeak / 65536`
        // is the signal level in 16-bit terms regardless of what was negotiated --
        // one number to compare against `fifoPeak`, whatever depth was chosen.
        float fifoPeak = handle->fifoPeak.load(std::memory_order_relaxed);
        float writePeak = handle->writtenPeak.load(std::memory_order_relaxed);
        // Normalised against the *valid* bits, not the container.
        //
        // A 24-in-32 sample is 0x7FFFFF00 once it is placed correctly, and
        // measuring that against the 32-bit container's 2^31 reports 0.0039 for a
        // full-scale signal -- which reads exactly like a fault and is not one.
        // The device only ever sees `validBits` of signal, so that is the only
        // denominator that makes `writtenPeak` comparable with `fifoPeak`.
        const uint32_t validBits = handle->dither.valid_bits();
        const double writeFull = std::ldexp(1.0, static_cast<int>(validBits) - 1);

        auto processSample = [&](double s, size_t c) -> double {
            if (handle->rgGain != 1.0) s *= handle->rgGain;
            if (c < NAE_MAX_CHANNELS) {
                if (handle->channelTrimDb[c] != 0.0) {
                    s *= std::pow(10.0, handle->channelTrimDb[c] / 20.0);
                }
                if (c < 2 && handle->balance != 0.0) {
                    s *= (c == 0) ? (1.0 - std::max(0.0, handle->balance))
                                  : (1.0 + std::min(0.0, handle->balance));
                }
                if (handle->polarityMask & (1u << c)) s = -s;
            }
            if (!handle->eqEnabled) return s;
            if (handle->eqLinear) return s;  // applied per frame, after all channels
            s *= handle->eqPreamp;
            for (int32_t b = 0; b < handle->eqBandCount; ++b) {
                if (!handle->eqBandEnabled[b]) continue;
                const uint32_t mask = handle->eqBandChannelMask[b];
                if (mask != 0 && !(mask & (1u << c))) continue;
                const size_t idx = static_cast<size_t>(b) * ch + c;
                const neiro::EqBiquad& q = handle->eqCoeffs[b];
                const double y = q.b0 * s + handle->eqZ1[idx];
                handle->eqZ1[idx] = q.b1 * s - q.a1 * y + handle->eqZ2[idx];
                handle->eqZ2[idx] = q.b2 * s - q.a2 * y;
                s = y;
            }
            return s;
        };

        frameOut.assign(ch, 0.0);
        frameFloat.assign(ch, 0.0f);
        for (size_t f = 0; f < framesToWrite; ++f) {
            // Volume is a per-frame ramp, not per sample: advancing it in the
            // channel loop would make it run twice as fast in stereo.
            if (handle->volumeRampRemaining > 0) {
                handle->volume += handle->volumeRampDelta;
                if (--handle->volumeRampRemaining <= 0) {
                    handle->volume = handle->volumeTarget;
                    handle->volumeRampRemaining = 0;
                }
            }
            if (handle->rgRamp > 0) {
                handle->rgGain += handle->rgDelta;
                if (--handle->rgRamp <= 0) {
                    handle->rgGain = handle->rgTarget;
                    handle->rgRamp = 0;
                }
            }

            // Crossfeed needs both channels at once, so it is computed here and
            // the channel loop below consumes the result. Only stereo; a
            // multichannel stream is left alone rather than folded by accident.
            const bool cross = handle->crossfeedEnabled && ch == 2 &&
                               handle->crossfeedDelayFrames > 0;
            double crossL = 0.0;
            double crossR = 0.0;
            if (cross) {
                const double inL =
                    processSample((f < realFrames) ? static_cast<double>(fifo[f * ch]) : held[0], 0);
                const double inR =
                    processSample((f < realFrames) ? static_cast<double>(fifo[f * ch + 1]) : held[1], 1);
                const size_t frames = handle->crossfeedDelayFrames;
                const size_t pos = handle->crossfeedPos;
                const double delayedL = handle->crossfeedDelay[pos];
                const double delayedR = handle->crossfeedDelay[frames + pos];
                handle->crossfeedDelay[pos] = inL;
                handle->crossfeedDelay[frames + pos] = inR;
                handle->crossfeedPos = (pos + 1) % frames;
                handle->crossfeedLpL +=
                    handle->crossfeedAlpha * (delayedL - handle->crossfeedLpL);
                handle->crossfeedLpR +=
                    handle->crossfeedAlpha * (delayedR - handle->crossfeedLpR);
                crossL = inL + handle->crossfeedAmount * handle->crossfeedLpR;
                crossR = inR + handle->crossfeedAmount * handle->crossfeedLpL;
            }

            frameOut.assign(ch, 0.0);
            for (size_t c = 0; c < ch; ++c) {
                double s;
                if (cross) {
                    s = c == 0 ? crossL : crossR;
                } else {
                    const double raw =
                        (f < realFrames) ? static_cast<double>(fifo[f * ch + c]) : held[c];
                    s = processSample(raw, c);
                }
                if (handle->volume != 1.0) s *= handle->volume;
                // The fade in. Eight milliseconds from silence after every start, and
                // it is counted down in device frames rather than reset per buffer so
                // a short buffer cannot restart the ramp.
                if (handle->fadeInRemaining > 0) {
                    ++handle->fadeInFrame;
                    const double total = std::max<int64_t>(1, handle->fadeInTotal);
                    double g = static_cast<double>(handle->fadeInFrame) / static_cast<double>(total);
                    if (g > 1.0) g = 1.0;
                    s *= g;
                    if (--handle->fadeInRemaining <= 0) {
                        handle->fadeInRemaining = 0;
                        handle->fadeInFrame = 0;
                    }
                }
                // Per-channel delay. Only when configured; the buffer is a
                // per-channel ring of `channelDelayMax` frames. A channel with a
                // shorter delay reads a fixed offset into that ring, so all
                // channels stay time-aligned.
                if (handle->channelDelayMax > 0 && c < handle->channelDelay.size()) {
                    const uint32_t delay = static_cast<uint32_t>(handle->channelDelay[c]);
                    const size_t ring = handle->channelDelayMax;
                    const size_t pos = handle->channelDelayPos[c];
                    const size_t base = c * ring;
                    handle->channelDelayBuf[base + pos] = static_cast<float>(s);
                    const size_t read =
                        (pos + ring - delay) % ring;
                    s = handle->channelDelayBuf[base + read];
                    handle->channelDelayPos[c] = (pos + 1) % ring;
                }
                frameOut[c] = s;
                const float a = static_cast<float>(s < 0.0 ? -s : s);
                if (a > fifoPeak) fifoPeak = a;
                // Diagnostic: one line per device buffer on the tone path.
                //
                // The question is why the ring ended up holding 4096 samples
                // spanning 1/47 of a cycle, which is only possible if the
                // generator produced far fewer samples than real time needs. So
                // this counts iterations and written frames against what the call
                // was asked for, and the two are what a 10x shortfall looks like
                // from inside.
                if (std::getenv("NAE_TRACE_SPECTRUM")) {
                    std::fprintf(stderr,
                                 "[nae:slice] framesToWrite=%zu realFrames=%zu fifo=%zu "
                                 "sourcePos=%lld sourceLimit=%lld\n",
                                 framesToWrite, realFrames, fifo.size() / ch,
                                 static_cast<long long>(sourcePos),
                                 static_cast<long long>(sourceLimit));
                }
                // The spectrum tap, here and nowhere else.
                //
                // This is the last point at which the samples are still the
                // engine's own: after resampling, after the fade-in, before
                // quantisation and dither. So the bins describe the signal the
                // device is about to receive at the rate it is about to receive
                // it, and a resampled DSD64 track reports what the resampler
                // actually produced rather than what the file claimed.
                //
                // Channel 0 only, matching the Web Audio route's
                // `getChannelData(0)` so the two pictures are comparable. Dither
                // is below this: it is about -96 dBFS, and a display floored
                // anywhere near that is not showing the noise floor of the
                // material.
                if (c == 0) {
                    if (spectrumChannel.empty()) spectrumChannel.assign(framesToWrite, 0.0);
                    spectrumChannel[f] = s;
                }
            }

            if (handle->eqEnabled && handle->eqLinear) {
                float eqFrame[NAE_MAX_CHANNELS]{};
                for (size_t c = 0; c < ch && c < NAE_MAX_CHANNELS; ++c) {
                    eqFrame[c] = static_cast<float>(frameOut[c]);
                }
                handle->linearEq.process(eqFrame, 1, static_cast<uint32_t>(ch));
                for (size_t c = 0; c < ch && c < NAE_MAX_CHANNELS; ++c) {
                    frameOut[c] = eqFrame[c];
                }
                // The spectrum tap sits before this point; overwrite it so the
                // bins describe what the equaliser produced.
                if (!spectrumChannel.empty()) spectrumChannel[f] = frameOut[0];
            }

            // Clip guard, once per frame after all channels are known. It is
            // applied before dither and before the sample leaves the engine, and
            // only when the policy enabled it.
            if (handle->clipGuard.active()) {
                frameFloat.assign(frameOut.begin(), frameOut.end());
                handle->clipGuard.process(frameFloat.data(), 1);
                for (size_t c = 0; c < ch; ++c) frameOut[c] = frameFloat[c];
            }
            for (size_t c = 0; c < ch; ++c) {
                const double s = frameOut[c];
                if (c == 0) {
                    if (spectrumChannel.empty()) spectrumChannel.assign(framesToWrite, 0.0);
                    spectrumChannel[f] = s;
                }
                BYTE* p = raw + f * static_cast<size_t>(bytesPerFrame) + c * bytesPerSample;
                if (isFloat) {
                    const float v = static_cast<float>(s);
                    std::memcpy(p, &v, sizeof(v));
                    continue;
                }
                const int32_t v = handle->dither.quantize(s);
                const double av = std::fabs(static_cast<double>(v)) / writeFull;
                if (av > writePeak) writePeak = static_cast<float>(av);
                if (containerBits == 16) {
                    const auto q = static_cast<int16_t>(v);
                    std::memcpy(p, &q, sizeof(q));
                } else if (containerBits == 24) {
                    p[0] = static_cast<BYTE>(v);
                    p[1] = static_cast<BYTE>(v >> 8);
                    p[2] = static_cast<BYTE>(v >> 16);
                } else {
                    const uint32_t placed =
                        neiro::wasapi::placeInContainer(v, containerBits, validBits);
                    std::memcpy(p, &placed, sizeof(placed));
                }
            }
        }
        // The seam, measured against the signal's own slew rather than a threshold
        // chosen for one piece of music.
        //
        // A discontinuity in a smooth signal is a step much larger than the steps
        // the signal takes on its own. So the reference is computed from the audio
        // itself: the largest adjacent-sample difference inside this buffer and the
        // last few, divided by the peak. A seam whose step is several times that is
        // a break, and a ratio near 1 is a waveform that simply continued.
        //
        // This is the measurement the stop and resume boundary needs, and it is the
        // one that could not have existed before: the bubbling lived in a seam
        // between two calls, and every instrument in this engine measured inside a
        // single call.
        {
            // Both sides normalised by the same peak, so the ratio is a pure shape
            // measurement and does not depend on the level.
            const double ref = std::max(1e-9, static_cast<double>(fifoPeak));
            double within = 0.0;
            for (size_t f = 1; f < realFrames; ++f) {
                for (size_t c = 0; c < ch; ++c) {
                    const double d = std::fabs(static_cast<double>(fifo[(f - 1) * ch + c]) -
                                               static_cast<double>(fifo[f * ch + c]));
                    if (d > within) within = d;
                }
            }
            if (within > handle->slewRef) handle->slewRef = within;

            if (realFrames > 0) {
                if (handle->haveSeamTail) {
                    const double step =
                        std::fabs(static_cast<double>(fifo[0]) - handle->seamTail);
                    ++handle->seamCount;
                    const double ratio = step / ref;
                    if (ratio > handle->worstSeamRatio) {
                        handle->worstSeamRatio = ratio;
                        handle->worstSeamSlew = within / ref;
                    }

                    // Every seam, not only the worst. See the field's comment: a
                    // maximum with no distribution cannot be read.
                    if (ratio > 0.0 && std::isfinite(ratio)) {
                        handle->seamRatioTotal +=
                            static_cast<uint64_t>(ratio * 1000.0);
                        // -2 decades to +2, in 41 buckets of 0.1 decades.
                        const double log10 = std::log10(ratio);
                        if (log10 >= -2.0 && log10 <= 2.0) {
                            size_t b = static_cast<size_t>((log10 + 2.0) * 10.0);
                            if (b >= kSeamBuckets) b = kSeamBuckets - 1;
                            ++handle->seamHistogram[b];
                        }
                    }
                    if (handle->seamSampleCount < kSeamSamples) {
                        handle->seamSample[handle->seamSampleCount++] =
                            static_cast<float>(ratio);
                    }
                }
                handle->seamTail = fifo[(realFrames - 1) * ch];
                handle->haveSeamTail = true;
            }
        }

        // The tap is fed only the real frames, for the same reason: the pad frames
        // were invented to fill a buffer and are not what the device is playing
        // from, so a spectrum that included them would be describing the pad.
          // Diagnostic: the block sizes the tap is being fed in. They turned out to
          // be 2 to 28 frames on this endpoint, thousands of times per buffer
          // period, which is worth being able to see -- a tap that assumed one
          // call per buffer would have been wrong here and right nowhere else.
          if (std::getenv("NAE_TRACE_SPECTRUM")) std::fprintf(stderr, "[nae:tap] realFrames=%zu framesToWrite=%zu ch=%zu\n", realFrames, framesToWrite, ch);

        feedSpectrum(handle, spectrumChannel.data(), realFrames);

        // Only the real frames left the fifo. The pad frames were invented for this
        // buffer and must not be consumed again on the next one.
        fifo.erase(fifo.begin(), fifo.begin() + static_cast<long>(realFrames * ch));
        fifoConsumed += realFrames;
        handle->fifoPeak.store(fifoPeak, std::memory_order_relaxed);
        handle->writtenPeak.store(writePeak, std::memory_order_relaxed);

        // Read the bytes back out of the device's buffer and interpret them the
        // way the device will.
        //
        // Every other level figure here is computed from the float value *before*
        // it becomes bytes, which means a wrong offset or a wrong shift would still
        // report a perfect number. That is not a hypothetical: the 256x quiet fault
        // was invisible to `writtenPeak` because `writtenPeak` was measuring the
        // quantised integer, not the container the integer was placed in. This
        // closes that gap by reading `raw` back and applying the container's own
        // alignment, so a mismatch between "what we meant to write" and "what is
        // in the buffer" becomes a number rather than an earworm.
        if (!isFloat && bytesPerSample >= 2 && bytesPerFrame > 0) {
            float devicePeak = handle->devicePeak.load(std::memory_order_relaxed);
            const double devFull = std::ldexp(1.0, static_cast<int>(validBits) - 1);
            // A shift of 32 or more is undefined behaviour, and a container that
            // reports zero valid bits has no full scale to divide by. Both mean the
            // figure cannot be computed honestly, so it is not computed.
            if (validBits == 0 || validBits > containerBits) {
                handle->devicePeak.store(0.0f, std::memory_order_relaxed);
            } else {
            const uint32_t devShift = (containerBits - validBits);
            const size_t probeFrames = std::min<size_t>(framesToWrite, 256);
            for (size_t f = 0; f < probeFrames; ++f) {
                for (size_t c = 0; c < ch; ++c) {
                    const BYTE* p =
                        raw + f * static_cast<size_t>(bytesPerFrame) + c * bytesPerSample;
                    int32_t word = 0;
                    if (bytesPerSample == 2) {
                        int16_t q;
                        std::memcpy(&q, p, sizeof(q));
                        word = q;
                    } else if (bytesPerSample == 3) {
                        word = static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                                    (static_cast<uint32_t>(p[1]) << 8) |
                                                    (static_cast<uint32_t>(p[2]) << 16));
                        if (word & 0x00800000) word |= static_cast<int32_t>(0xFF000000u);
                    } else {
                        std::memcpy(&word, p, sizeof(word));
                    }
                    // Undo the container's justification the way a DAC would.
                    //
                    // Sign-extend before taking the magnitude. The first version
                    // shifted the raw word as unsigned, so a sample of -1 LSB --
                    // 0xFFFFFF00 once placed in a 32-bit container, and an entirely
                    // ordinary value that a waveform passes through every cycle near
                    // zero -- became 0xFFFFFF, and 0xFFFFFF / 2^23 is 1.9999999. It
                    // reported a normalised peak of 2.0, which cannot exist, and it
                    // looked exactly like a fault in the packing.
                    //
                    // It was my arithmetic, and the giveaway was that it reproduced
                    // identically on three unrelated runs at three different levels.
                    // A real fault in the write path does not do that. An arithmetic
                    // slip does.
                    const int32_t shifted =
                        (devShift > 0) ? (static_cast<int32_t>(word) >> devShift)
                                        : static_cast<int32_t>(word);
                    const double a = std::fabs(static_cast<double>(shifted)) / devFull;
                    if (a > devicePeak) devicePeak = static_cast<float>(a);
                }
            }
            handle->devicePeak.store(devicePeak, std::memory_order_relaxed);
            }
        }

        // How late was this buffer? The gap since the previous write is the only
        // thing that distinguishes "on time" from "correct but too late", and the
        // device buffer's own duration is the line it has to stay under.
        {
            const ULONGLONG now = GetTickCount64();
            if (lastWriteTick != 0) {
                const double gap = static_cast<double>(now - lastWriteTick);
                if (gap > handle->maxBufferGapMs) handle->maxBufferGapMs = gap;
            }
            lastWriteTick = now;
        }

        // Did this buffer land inside its runway? This is the measurement the
        // bubbling needed and did not have. A positive `lateMs` means the device
        // was already out of audio before this loop finished handing it over, so
        // what the listener heard for those milliseconds was the previous
        // buffer's tail rather than this one's head.
        {
            const double fillMs = neiro::wasapi::nowMsHr() - fillStartMs;
            if (fillMs > handle->maxFillMs) handle->maxFillMs = fillMs;
            const double lateMs = fillMs - runwayMs;
            if (lateMs > 0.0) {
                ++handle->lateCount;
                if (lateMs > handle->maxLateMs) handle->maxLateMs = lateMs;
            }
        }

        const HRESULT relHr = handle->render->ReleaseBuffer(static_cast<UINT32>(framesToWrite), 0);
        if (FAILED(remember(relHr))) {
            // The same distinction as GetBuffer above. ReleaseBuffer is where a
            // preemption actually tends to surface: the device accepted the
            // request and then lost the endpoint before the buffer was handed
            // over, so the audio in `raw` never reached it and the frames were
            // already erased from the fifo. The accounting below still balances,
            // because those frames are counted as consumed -- which is exactly
            // why this needs its own code rather than being left to the
            // short-render check to notice.
            if (deviceIsGone(relHr)) {
                deviceLost = true;
                g_lastErrorText =
                    "the output device was removed or is being used by another program; "
                    "playback is paused at the current position";
            }
            break;
        }
        // Frames that came from the source, so the fifo accounting still balances. The
        // device was handed `framesToWrite` of them, but only this many were paid
        // for by decoded audio.
        deviceFramesWritten += static_cast<int64_t>(realFrames);

        // Frames per second of wall clock, over a window long enough not to jitter.
        //
        // Two corrections, both because the window used to include the start-up
        // transient and so reported a number that described the first moments of a
        // stream rather than the stream.
        //
        // It read 98.5% before the bubbling fix and 98.4% after it. The click went
        // away and this did not move, which is what made it suspect: a figure that
        // survives the removal of the fault it appeared to describe is measuring
        // something else. The same error had already produced a "0.9701 of real
        // time" reading sitting next to a drift of 0.6 ms with no visible
        // contradiction between them.
        //
        // So: the first window is discarded outright, because opening the stream
        // costs real time and that cost is not playback. And the clock is
        // QueryPerformanceCounter, not GetTickCount64, whose 15.6 ms tick is coarse
        // enough to matter against a 53 ms buffer.
        chargeRateWindow(handle, realFrames);
        if (realFrames < framesToWrite) {
            handle->padFrameCount += static_cast<uint64_t>(framesToWrite - realFrames);
        }
        // Real progress: the device took audio, so the stall counter resets. Only
        // a run of *consecutive* unproductive iterations is a stall.
        stalledIterations = 0;
    }

    // FIFO accounting, printed under NAE_TRACE.
    //
    // These four numbers have to balance, and when they do not, the render loop
    // has been feeding the device frames the source never paid for:
    //
    //   fifoEnd == fifoStart + produced - consumed
    //
    // `produced` counts device frames handed over by the resampler, `consumed`
    // counts device frames handed to the device. Comparing `consumed` against
    // `sourcePos` advance is the check that matters: the device's consumption and
    // the source's supply must stand in the source-to-device ratio, and when they
    // do not, playback is running at the wrong speed no matter what the format
    // flags say.
    if (trace)
        std::fprintf(stderr,
                     "[nae] acct src=%lld->%lld (%+lld) devWritten=%lld fifo=%zu->%zu "
                     "produced=%zu consumed=%zu budget=%lld\n",
                     static_cast<long long>(sourcePosAtStart),
                     static_cast<long long>(sourcePos),
                     static_cast<long long>(sourcePos - sourcePosAtStart),
                     static_cast<long long>(deviceFramesWritten), fifoAtStart, fifo.size() / ch,
                     fifoProduced, fifoConsumed, static_cast<long long>(targetFrames));

    // Kept on the handle so a benchmark can read the numbers rather than scrape
    // the log above. `lastShortRender` is 1 when the last call failed to deliver
    // its budget, which is the only way a caller can tell a stalled device from a
    // finished track.
    handle->lastSourceAdvance = static_cast<double>(sourcePos - sourcePosAtStart);
    handle->lastDeviceWritten = static_cast<double>(deviceFramesWritten);
    handle->lastFifoFrames = static_cast<double>(fifo.size() / ch);
    handle->lastProduced = static_cast<double>(fifoProduced);
    handle->lastConsumed = static_cast<double>(fifoConsumed);
    handle->lastShortRender =
        (deviceFramesWritten < targetFrames && sourcePos < sourceLimit) ? 1.0 : 0.0;

    // Stop only when the stream has actually finished, or when something went
    // wrong that leaves no stream worth keeping.
    //
    // This used to be unconditional, at the end of every call. Since the host
    // calls NAE_Play in 200 ms slices, that stopped and restarted the device five
    // times a second, and the prefill on the way back in put a buffer of zeroes
    // into a continuous waveform each time. That is the bubbling: a full-scale
    // discontinuity, periodic, volume-independent, and absent from every counter,
    // because each call on its own was healthy and the fault lived in the seam.
    //
    // The stream legitimately stops when the source is exhausted, when the device
    // is gone, and when the call could not deliver its budget with source left to
    // give -- that last one is a real failure and leaving the client running would
    // hold the endpoint for a track that is no longer progressing.
    bool sourceFinished = sourcePos >= sourceLimit && fifo.size() / ch == 0;
#ifdef NEIRO_HAVE_FFMPEG
    // A preloaded file is the rest of this stream. Stopping the client here
    // would discard the fifo tail and start a new stream for the next file,
    // which is the gap gapless exists to remove.
    if (handle->nextDecoder) sourceFinished = false;
#endif
    const bool cannotContinue =
        (deviceFramesWritten < targetFrames && sourcePos < sourceLimit);
    const bool shouldStop = sourceFinished || cannotContinue || deviceLost;

    HRESULT stopHr = S_OK;
    if (shouldStop) {
        if (handle->clientRunning) {
            stopHr = neiro::wasapi::stopClient(handle->client.Get());
            handle->clientRunning = false;
            ++handle->clientRestartCount;
        }
        // A stop discards the device buffer, so the next stream does not continue
        // this one. Measuring a seam across it would report a boundary as a
        // waveform defect, and measuring without reporting it would hide the
        // boundary that actually exists -- which is the click a resume produces.
        handle->haveSeamTail = false;
    } else {
        // A slice boundary, not an end. The device keeps draining the frames it
        // already holds; stopping here is what produced the artefact.
    }

    // A device that disappeared is reported ahead of every other verdict,
    // including the short-render check below.
    //
    // The order matters. A lost device almost always *also* fails the budget --
    // the loop left early, so deviceFramesWritten is short of targetFrames -- and
    // the short-render branch would return NAE_ERR_INITIALIZE, which tells the
    // host the stream failed. It did not fail. The file is fine, the decoder is
    // fine, the position is fine; the thing that went away is a piece of hardware,
    // and the handle is still good. Returning NAE_ERR_DEVICE_LOST first is what
    // lets a host keep the handle and resume rather than discard the user's place.
    //
    // `g_lastErrorText` is set at the call site and deliberately not overwritten
    // here. Stopping a client whose endpoint has gone is expected to fail, and
    // letting that failure's HRESULT replace the explanation would leave the user
    // reading "stop failed (0x88890008)" instead of "the device was removed".
    if (deviceLost) {
        // The one event the counters exist for: the endpoint went away mid-track
        // and the engine paused with the position kept. Counted here rather than
        // at the four call sites that set the flag, so it is incremented exactly
        // once per lost device no matter which of them noticed first.
        ++handle->deviceLostCount;
        if (trace)
            std::fprintf(stderr,
                         "[nae] DEVICE LOST at src=%lld/%lld devWritten=%lld/%lld "
                         "fifo=%zu -- pausing, position kept\n",
                         static_cast<long long>(sourcePos),
                         static_cast<long long>(sourceLimit),
                         static_cast<long long>(deviceFramesWritten),
                         static_cast<long long>(targetFrames), fifo.size() / ch);
        return NAE_ERR_DEVICE_LOST;
    }

    // Did the loop actually finish, or did something abort it?
    //
    // Reporting NAE_OK unconditionally is how a stream can fail completely and
    // still be reported as playing: every `break` above leaves the loop early,
    // `stopClient` succeeds, and the caller is told everything is fine while
    // almost no audio reached the device. The host then keeps calling in a tight
    // loop, each call returning immediately, and playback runs at a small
    // fraction of real time with no error anywhere -- the position still advances,
    // the signal path still reads correctly, and the listener hears a stutter.
    //
    // So a call that did not deliver its budget is a failure -- but only when there
    // was still source to deliver. At the end of a track the source runs out
    // mid-slice and the remaining budget cannot be filled by anything; that is a
    // finished track, not a failure. The first version of this check did not
    // distinguish the two and reported the last slice of every file as an error.
    //
    // A caller that cancelled deliberately also lands here -- the engine has no
    // cancel flag of its own, and a stop request is a host-side decision. That is
    // fine: the caller knows why it stopped and ignores the error. What must not
    // happen is the reverse, where a real device failure is indistinguishable from
    // a clean stop.
    const bool sourceExhausted = sourcePos >= sourceLimit;
    if (deviceFramesWritten < targetFrames && !sourceExhausted) {
        // A call that could not deliver what it was asked for, with source still
        // to deliver. This is the event a listener hears as a click or a gap, and
        // it is the number that tells a listener whether the problem is theirs
        // (the settings) or the machine's (this).
        ++handle->dropCount;
        // A drop after a lost device is the *same* event seen twice: the device
        // went away, this call consequently could not fill the buffer, and the
        // device-lost branch above already returned before reaching here. So this
        // branch is only reached for a short render that was not a lost device,
        // which is the distinction worth counting.
        char detail[128];
        std::snprintf(detail, sizeof(detail), "0x%08X", static_cast<unsigned>(g_lastHr));
        g_lastErrorText = std::string("rendering stopped after ") +
                          std::to_string(deviceFramesWritten) + " of " +
                          std::to_string(targetFrames) + " frames (hresult " + detail + ")";
        if (trace)
            std::fprintf(stderr, "[nae] SHORT RENDER %s\n", g_lastErrorText.c_str());
        return NAE_ERR_INITIALIZE;
    }

    if (trace)
        std::fprintf(stderr, "[nae] done source=%lld/%lld device=%lld iters=%d stopHr=0x%08X\n",
                     static_cast<long long>(sourcePos),
                     static_cast<long long>(sourceLimit),
                     static_cast<long long>(deviceFramesWritten), iterations, stopHr);
    // A call that delivered its whole budget after the device had previously been
    // lost. That pairing is what "it came back by itself" means, and the host
    // needs both numbers to say it: a lost device with no recovery is a device
    // that is still gone.
    if (handle->deviceLostCount > 0 && handle->recoveryCount == 0 && deviceFramesWritten > 0) {
        ++handle->recoveryCount;
    }
    // Counted here rather than per device write, because the number that matters
    // is "how many times were we called", which is the sampling rate of the other
    // two figures.
    ++handle->callbackCount;
    return NAE_OK;
}

void NAE_Close(NAE_Handle* handle) NAE_NOEXCEPT {
    if (!handle) return;
    // The one place the resampler may be flushed. A filter holds its last frames
    // back until it is told the source has ended, and this is the only point at
    // which that is true.
    //
    // The result is discarded: a stream is being torn down and there is nowhere
    // left to deliver the tail to. Draining it would only delay the close.
    if (handle->resampler.ready()) {
        std::vector<float> discard;
        std::string err;
        handle->resampler.flush(&discard, &err);
    }
    if (handle) {
        stopDecodeThread(handle);
        handle->dopDsf.reset();
        handle->dopDff.reset();
    }
    delete handle;
}


int32_t NAE_QueryDeviceCaps(const char* deviceId, int32_t shareMode,
                            NAE_DeviceCaps* out) NAE_NOEXCEPT {
    if (!out || out->structSize != sizeof(NAE_DeviceCaps)) return NAE_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->version = NAE_DEVICE_CAPS_VERSION;
    out->backend = NAE_BACKEND_WASAPI;
    out->shareMode = (shareMode == NAE_SHARE_MODE_SHARED) ? NAE_SHARE_MODE_SHARED
                                                          : NAE_SHARE_MODE_EXCLUSIVE;
    out->hotplug = 1;
    out->exclusiveSupported = 1;
    out->sharedSupported = 1;
    out->supportedShareModes = (1u << NAE_SHARE_MODE_EXCLUSIVE) |
                               (1u << NAE_SHARE_MODE_SHARED);

    if (FAILED(ensureCom())) return NAE_ERR_COM;

    std::unique_ptr<char, void (*)(void*)> defaultIdFree(nullptr, [](void* p) { std::free(p); });
    std::string resolvedId;
    if (!deviceId || !deviceId[0]) {
        char* def = NAE_GetDefaultDeviceId();
        if (!def || !*def) {
            if (def) std::free(def);
            return NAE_ERR_NO_DEVICE;
        }
        defaultIdFree.reset(def);
        resolvedId = def;
        deviceId = resolvedId.c_str();
        out->isDefault = 1;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&enumerator))) ||
        !enumerator) {
        return NAE_ERR_NO_DEVICE;
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDevice(std::wstring(deviceId, deviceId + std::strlen(deviceId)).c_str(),
                                     &device)) ||
        !device) {
        return NAE_ERR_NO_DEVICE;
    }

    std::snprintf(out->id, sizeof(out->id), "%s", deviceId);
    std::snprintf(out->name, sizeof(out->name), "%s", deviceId);

    WAVEFORMATEX native{};
    uint16_t nativeValid = 0;
    if (!readNativeFormat(device.Get(), &native, &nativeValid)) return NAE_ERR_NO_NATIVE_FORMAT;
    out->probed |= NAE_CAPS_PROBED_NATIVE | NAE_CAPS_PROBED_FORMATS | NAE_CAPS_PROBED_LAYOUTS;
    out->nativeRate = native.nSamplesPerSec;
    out->nativeChannels = native.nChannels;
    out->nativeValidBits = nativeValid ? nativeValid : native.wBitsPerSample;
    out->nativeContainerBits = native.wBitsPerSample;
    out->maxChannels = native.nChannels;
    if (native.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        out->nativeFormat = native.wBitsPerSample == 64 ? NAE_SAMPLE_FORMAT_F64
                                                        : NAE_SAMPLE_FORMAT_F32;
    } else if (out->nativeValidBits == 16) {
        out->nativeFormat = NAE_SAMPLE_FORMAT_S16;
    } else if (out->nativeValidBits == 24 && out->nativeContainerBits == 32) {
        out->nativeFormat = NAE_SAMPLE_FORMAT_S24_IN_32;
    } else if (out->nativeValidBits == 24) {
        out->nativeFormat = NAE_SAMPLE_FORMAT_S24;
    } else if (out->nativeValidBits == 32) {
        out->nativeFormat = NAE_SAMPLE_FORMAT_S32;
    } else {
        out->nativeFormat = NAE_SAMPLE_FORMAT_UNKNOWN;
    }
    out->formatCount = 1;
    out->formats[0].format = out->nativeFormat;
    out->formats[0].validBits = out->nativeValidBits;
    out->formats[0].containerBits = out->nativeContainerBits;
    out->layoutCount = 1;
    switch (native.nChannels) {
        case 1: out->nativeChannelMask = NAE_LAYOUT_MONO; break;
        case 2: out->nativeChannelMask = NAE_LAYOUT_STEREO; break;
        case 6: out->nativeChannelMask = NAE_LAYOUT_5_1; break;
        case 8: out->nativeChannelMask = NAE_LAYOUT_7_1; break;
        default: out->nativeChannelMask = 0; break;
    }
    out->layouts[0] = out->nativeChannelMask;

    bool probed = false;
    std::vector<uint32_t> rates;
    {
        // The probe takes fresh exclusive clients, so it is serialised against
        // every other probe in the process: two sequences at once fight over
        // the endpoint and each misreads the other as "no rates".
        std::lock_guard<std::mutex> lock(rateCacheMutex());
        rates = probeSupportedRates(device.Get(), native, nativeValid, &probed);
    }
    // The probe opens exclusive streams. Do not report its result for a shared
    // query: shared can accept a different set, and claiming exclusive's answer
    // for shared is the same class of lie as a hardcoded capability string.
    if (probed && out->shareMode == NAE_SHARE_MODE_EXCLUSIVE) {
        out->probed |= NAE_CAPS_PROBED_RATES | NAE_CAPS_PROBED_DOP;
        out->rateCount = static_cast<uint32_t>(
            std::min<size_t>(rates.size(), NAE_MAX_DEVICE_RATES));
        for (uint32_t i = 0; i < out->rateCount; ++i) out->rates[i] = rates[i];
        for (uint32_t carrier : {176400u, 352800u, 705600u}) {
            if (std::find(rates.begin(), rates.end(), carrier) == rates.end()) continue;
            if (out->dopCarrierCount < NAE_MAX_DOP_CARRIERS) {
                out->dopCarriers[out->dopCarrierCount++] = carrier;
            }
        }
        out->dopSupported = out->dopCarrierCount > 0 ? 1 : 0;
    }
    // Native DSD. WASAPI defines no 1-bit stream format, so this backend can
    // answer the question but the answer is no. Reporting it as probed is the
    // point: 0 with the bit clear means "not asked", which policy must treat
    // differently from "asked and unavailable".
    out->probed |= NAE_CAPS_PROBED_NATIVE_DSD;
    out->nativeDsdSupported = 0;
    out->nativeDsdRateCount = 0;
    out->capsGeneration = ++g_capsGeneration;
    return NAE_OK;
}

void NAE_Shutdown(void) NAE_NOEXCEPT {
    if (!g_comReady) return;
    g_comReady = false;
    CoUninitialize();
}

// Copies a std::string onto the heap with malloc, so it pairs with
// NAE_FreeString's free. The two allocators must match: the host may well have
// a different CRT, and freeing across that boundary is undefined.
char* duplicateString(const std::string& in) {
    char* out = static_cast<char*>(std::malloc(in.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, in.c_str(), in.size() + 1);
    return out;
}

char* NAE_GetDefaultDeviceId(void) NAE_NOEXCEPT {
    try {
        if (FAILED(ensureCom())) return nullptr;

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&enumerator))) ||
            !enumerator) {
            return nullptr;
        }

        // eConsole, not eMultimedia: this is the endpoint a user means by
        // "the sound", and it is what a music player should follow.
        ComPtr<IMMDevice> device;
        const HRESULT hr = remember(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
        if (FAILED(hr) || !device) return nullptr;

        // deviceId wraps GetId, which allocates with CoTaskMem; it copies into
        // a std::wstring and frees it, so nothing leaks across the boundary.
        std::wstring id;
        if (FAILED(neiro::wasapi::deviceId(device.Get(), id)) || id.empty()) return nullptr;

        const int needed =
            WideCharToMultiByte(CP_UTF8, 0, id.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) return nullptr;

        std::string utf8(static_cast<size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, id.c_str(), -1, utf8.data(), needed, nullptr, nullptr);
        // WideCharToMultiByte wrote a terminating NUL at index needed-1, which
        // is one past the string, so the std::string stays well-formed.
        return duplicateString(utf8);
    } catch (...) {
        return nullptr;
    }
}

}  // extern "C"
