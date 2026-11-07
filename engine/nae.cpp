#include "nae.h"

// The device half of this file is Windows-only for now. Everything below the
// platform boundary -- decode, resample, dither, routing, position accounting --
// is plain C++ and is what macOS and iOS will run unchanged; keeping the
// Windows includes behind the guard is the first step to proving that, because
// the compiler then names every remaining Windows dependency instead of the
// file simply refusing to open.
#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <mmsystem.h>   // waveOut, unrelated to the scheduler
#include <avrt.h>       // AvSetMmThreadCharacteristicsW -- the multimedia class scheduler
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#endif

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

#ifdef _WIN32
#include "wasapi_compat.hpp"
#include "wasapi_device.h"
#endif
#include "dither.h"
#include "audio_backend.h"
#include "output_stream.h"

// ---- the few Windows primitives the platform-neutral code still names --------
//
// Not a device shim: these are timing and error-shape names that appear in code
// which is not Windows-specific (a wait, a yield, a success test). Naming them
// once keeps the boundary at the device rather than smeared across the file.
#ifndef _WIN32
#include <sched.h>
#include <ctime>
#include <unistd.h>
using HRESULT = int32_t;
using BYTE = uint8_t;
using UINT32 = uint32_t;
using ULONGLONG = unsigned long long;
constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
// No API on this platform produces these; they exist so the shared branches
// still name the case they are distinguishing.
constexpr HRESULT AUDCLNT_E_BUFFER_TOO_LARGE = -2004287484;
constexpr HRESULT AUDCLNT_E_DEVICE_INVALIDATED = -2004287480;
constexpr HRESULT AUDCLNT_E_RESOURCES_INVALIDATED = -2004287310;
constexpr HRESULT AUDCLNT_E_SERVICE_NOT_RUNNING = -2004287484;
inline void Sleep(unsigned ms) { usleep(ms * 1000u); }
inline void SwitchToThread() { sched_yield(); }

// Windows raises the render thread to the multimedia class here. macOS has an
// equivalent (pthread_set_qos_class_self_np), but it is not wired yet, and a
// function that silently does nothing is better than one that claims a priority
// it did not get. Named so the gap is visible.
inline void ensureAudioThreadQos() {}

// Windows spins on the multimedia timer for one tick. There is no such tick
// here; yielding is the honest thing and does not claim a delay it never
// measured.
inline void spinForBuffer() { SwitchToThread(); }



// Windows waits on the device's own event here. macOS has that wait inside the
// stream (`waitForRoom`); this helper exists for the one call site that asks for
// a duration rather than for room, so it sleeps for it.
inline void waitForDeviceBuffer(double waitMs) {
    if (waitMs <= 0.0) return;
    usleep(static_cast<useconds_t>(waitMs * 1000.0));
}

// The same high-resolution clock the Windows build uses for its timing
// instrumentation. It measures durations, not device frames, so the meaning is
// identical on both platforms.
namespace neiro::wasapi {
inline double nowMsHr() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

// Same arithmetic as the Windows version: a 24-bit value in a 32-bit container
// is left-justified. Kept because the sample-placement code compiles on both
// platforms even when the device takes floats.
inline uint32_t placeInContainer(int32_t value, uint32_t containerBits, uint32_t validBits) {
    if (containerBits <= validBits || validBits == 0) return static_cast<uint32_t>(value);
    return static_cast<uint32_t>(value) << (containerBits - validBits);
}

// Milliseconds since the process started, at the system tick. The Windows build
// uses GetTickCount64 for the same purpose; the resolution is what the call
// sites expect, not better.
inline ULONGLONG GetTickCount64() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<ULONGLONG>(ts.tv_sec) * 1000ULL +
           static_cast<ULONGLONG>(ts.tv_nsec) / 1000000ULL;
}
}  // namespace neiro::wasapi
#ifndef _WIN32
// The call sites use it unqualified, as they did on Windows.
using neiro::wasapi::GetTickCount64;
#endif
#endif

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
#include "nae_internal.h"

#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif



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

// Shared by code on both sides of the device boundary: the error text a host
// reads, and the HRESULT two diagnostics print. Declared here -- outside the
// platform guard -- so the neutral half can use them without dragging the device
// block along. `HRESULT` exists on both sides: windows.h defines it on one, the
// compatibility block above defines it on the other.
thread_local HRESULT g_lastHr = S_OK;
thread_local std::string g_lastErrorText;


#ifdef _WIN32




namespace {


// The device helpers both translation units share -- the COM apartment, the
// endpoint's native format, the wire-format builder, and the one verdict on
// "the device is gone". They live in `wasapi_device.h` so the policy in this
// file and the stream in `backend_wasapi.cpp` cannot answer differently.
using neiro::wasapi::ensureCom;
using neiro::wasapi::readNativeFormat;
using neiro::wasapi::remember;
using neiro::wasapi::wrap;


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

// The device period belongs to the device layer now: `backend_wasapi.cpp`
// creates the render event and owns it, and the loop asks `waitForRoom`.

// Raise this thread to the multimedia class.
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

}  // namespace
#endif  // _WIN32

// Seam-distribution sizes, at file scope.
//
// They are needed in two places -- the render loop, which accumulates into the
// arrays, and NAE_QuerySignalPath, which summarises them -- and a `static constexpr`
// member of NAE_Handle is not in scope in a free function. Declaring them as
// members as well does not help: the member version shadows the file-scope one and
// the arrays end up sized by whichever declaration a given use site resolved to.
// leak COM types into consumers.

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

#ifdef NEIRO_HAVE_FFMPEG
// Decode-ahead producer.
//
// The producer owns `decoder`; it never touches the device. It fills a source
// ring to one second, then waits. The render thread consumes blocks through
// readDecodedFrames and still owns resampling, gapless/crossfade and the DSP.
// This is deliberately only the decode move: the source-side switch logic is
// unchanged, and the render loop keeps working if the thread cannot start.
static void decodeThreadMain(NAE_Handle* handle) {
    const size_t channels = handle->fmt.channels ? handle->fmt.channels : 1;
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
    const size_t channels = handle->fmt.channels ? handle->fmt.channels : 1;
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
    handle->sourceFramesReal = handle->nextSourceFramesReal;
    handle->sourcePos = 0;
    // And the seek base, or B inherits A's offset.
    //
    // `seekBaseFrames` is the distance into the track that a seek moved to, added
    // to `sourcePos` when reporting a position. It was reset only by `NAE_Seek`,
    // so a gapless transition *after* a seek carried A's offset into B: measured
    // 3.327 s for track B about 200 ms after it started, on a track that had
    // never been seeked. The position of a track is a fact about that track.
    handle->seekBaseFrames = 0;
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

constexpr double kEnvelopeHopMs = 5.0;
constexpr size_t kEnvelopeFrames = 4096;

static constexpr size_t kSpectrumFft = 4096;

static void feedSpectrum(NAE_Handle* handle, const double* samples, size_t frames) {
    if (!handle) return;
    if (handle->spectrumRing.empty()) handle->spectrumRing.assign(kSpectrumFft, 0.0);
    if (frames == 0) return;
    std::lock_guard<std::mutex> guard(handle->spectrumLock);
    // A rate change invalidates the window: a ring holding samples from two rates
    // is not a measurement of either, so it is emptied rather than blended.
    const uint32_t rate = handle->fmt.rate;
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
        bool firstMode = true;
        for (size_t i = 0; i < sizeof(kOpenableShareModes) / sizeof(kOpenableShareModes[0]); ++i) {
            // Every mode in the table, on every platform.
            //
            // There used to be a `#ifndef _WIN32` filter here that dropped
            // exclusive, justified by `NAE_QueryDeviceCaps` reporting
            // `exclusiveSupported = 0` on CoreAudio. That is no longer true --
            // CoreAudio reports 1 because hog mode *is* this platform's
            // exclusive, and `NAE_OpenFile` opens with it -- so the filter made
            // the engine advertise the one mode it could not promote: a host
            // reading `shareModes` would never ask for exclusive, while the
            // device list said the device had it.
            // A flag, not the index: skipping an entry must not leave its comma
            // behind, which produced `[,{...}]` and invalid JSON.
            if (!firstMode) modes += ',';
            firstMode = false;
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
        // The backend this build actually uses. Saying "wasapi" on CoreAudio is a
        // claim the host reads and cannot check, and it contradicted the caps.
#ifdef _WIN32
        const char* backendName = "wasapi";
#else
        const char* backendName = "coreaudio";
#endif
        std::string json = std::string("{\"backend\":\"") + backendName +
                           "\",\"shareModes\":[" + modes +
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

#ifdef _WIN32
int32_t NAE_GetLastHresult(void) NAE_NOEXCEPT { return static_cast<int32_t>(g_lastHr); }
#else
// No COM on this platform, so there is no HRESULT to report. Zero is the honest
// answer: it says "no device call supplied one", not "the last call succeeded".
int32_t NAE_GetLastHresult(void) NAE_NOEXCEPT { return 0; }
#endif

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
#ifdef _WIN32
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
#else
    // macOS: the same JSON, filled from the backend that already knows how to
    // ask the HAL. Going through `createPlatformBackend` rather than calling
    // AudioObject* here keeps one copy of the device knowledge -- the enumeration
    // this returns and the one `caps_check` reads are the same code.
    auto backend = neiro::createPlatformBackend();
    std::string json = "[";
    if (backend) {
        std::string error;
        const auto devices = backend->enumerate(&error);
        bool first = true;
        for (const auto& device : devices) {
            NAE_DeviceCaps caps{};
            caps.structSize = sizeof(caps);
            if (!backend->queryCaps(device.id, NAE_SHARE_MODE_EXCLUSIVE, &caps, &error)) {
                continue;
            }
            if (!first) json += ",";
            first = false;
            char flags[160];
            std::snprintf(flags, sizeof(flags),
                          "\"sampleRate\":%u,\"bitsPerSample\":%u,\"validBits\":%u,"
                          "\"channels\":%u,\"exclusive\":%s,\"shared\":%s",
                          caps.nativeRate, caps.nativeContainerBits, caps.nativeValidBits,
                          caps.nativeChannels, caps.exclusiveSupported ? "true" : "false",
                          caps.sharedSupported ? "true" : "false");
            json += "{\"id\":\"" + jsonEscape(device.id) + "\",\"name\":\"" +
                    jsonEscape(device.name) + "\"," + flags + "}";
        }
    }
    json += "]";
#endif

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
    handle->sourceFramesReal =
        static_cast<int64_t>(info.duration_ms / 1000.0 * info.sample_rate);
    handle->totalSourceFrames =
        handle->sourceFramesReal + static_cast<int64_t>(info.sample_rate) / 2;
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

// The device the handle drives is `handle->stream`: one `OutputStream` per
// platform, owning its own COM clients, render event and format. The render loop
// below is identical on both platforms, and it never names a device API.

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
    if (!handle->stream || !handle->stream->running()) return NAE_OK;
    const auto st = handle->stream->stop();
    if (st == neiro::StreamStatus::DeviceLost) {
        g_lastErrorText = "the output device was removed or is being used by another program";
        return NAE_ERR_DEVICE_LOST;
    }
    if (st != neiro::StreamStatus::Ok) {
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
    const double rate = handle->fmt.rate;
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
    const uint32_t channels = handle->fmt.channels;
    const uint32_t rate = handle->fmt.rate;
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
    const uint32_t channels = handle->fmt.channels;
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
    const int64_t rate = static_cast<int64_t>(handle->fmt.rate);
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
            if (!neiro::computeEqBiquad(band, static_cast<double>(handle->fmt.rate),
                               &coeffs[i])) {
                return NAE_ERR_INVALID_ARGUMENT;
            }
            enabled[i] = true;
        }
    }

    const size_t channels = handle->fmt.channels ? handle->fmt.channels : 1;
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
                             handle->fmt.rate, perChannel,
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
    const double fs = static_cast<double>(handle->fmt.rate);
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

    const int64_t rate = static_cast<int64_t>(handle->fmt.rate);
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
    if (!next->open(path, &error, handle->fmt.channels)) {
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
    handle->nextSourceFramesReal =
        static_cast<int64_t>(info.duration_ms / 1000.0 * info.sample_rate);
    handle->nextTotalSourceFrames =
        handle->nextSourceFramesReal + static_cast<int64_t>(info.sample_rate) / 2;
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
        handle->nextPrefetched.size() / static_cast<size_t>(handle->fmt.channels);
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
        const size_t streamChannels = handle->fmt.channels ? handle->fmt.channels : 1;
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
#ifdef _WIN32
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
    if (!readNativeFormat(device.Get(), &native)) {
        return NAE_ERR_NO_NATIVE_FORMAT;
    }
    if (native.nChannels != channels) {
        g_lastErrorText = "the endpoint's channel count does not match the DSD stream";
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }

    neiro::StreamFormatRequest want;
    want.deviceId = deviceId ? deviceId : "";
    want.rate = carrier;
    want.channels = channels;
    want.validBits = 24;
    want.isFloat = false;
    want.shareMode = NAE_SHARE_MODE_EXCLUSIVE;
    // DoP does not need low latency; a 50 ms buffer gives the packer and the
    // disk read room. The endpoint's default here was 10 ms, which is tight
    // enough that a file read shows up as crackle.
    want.bufferFrames = (carrier * 50) / 1000;

    // 24-in-32 first, then 24-in-24: a device may take the 24 valid bits only in
    // one of the two containers. If neither opens, the endpoint cannot carry DoP
    // and the caller must say so rather than play shifted noise.
    neiro::OpenedFormat opened;
    std::string error;
    std::shared_ptr<neiro::OutputStream> stream;
    for (const uint32_t container : {32u, 24u}) {
        want.containerBits = container;
        stream = neiro::openWasapiStream(want, &opened, &error);
        if (stream) break;
    }
    if (!stream) {
        g_lastErrorText = "the endpoint refused a 24-valid-bit DoP carrier at this rate";
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }

    auto handle = std::make_unique<NAE_Handle>();
    handle->stream = stream;
    handle->fmt.rate = opened.rate;
    handle->fmt.channels = opened.channels;
    handle->fmt.containerBits = opened.containerBits;
    handle->fmt.validBits = opened.validBits;
    handle->fmt.blockAlign = opened.channels * ((opened.containerBits + 7u) / 8u);
    handle->fmt.isFloat = opened.isFloat;
    handle->bufferFrames = opened.bufferFrames;
    // The driver's own figure, which is what this path has always reported as
    // `latencyMs`; the buffer's duration is a separate number.
    handle->latencyMs = stream->driverLatencyMs();
    handle->bufferDurationMs = opened.rate
                                   ? 1000.0 * static_cast<double>(opened.bufferFrames) /
                                         static_cast<double>(opened.rate)
                                   : 0.0;
    handle->requestedBits = 24;
    handle->grantedBits = 24;
    handle->exclusive = true;
    *out = handle.release();
    return NAE_OK;
}
#else
// No native DSD output on this platform: CoreAudio's HAL carries PCM, so a DSF
// reaches the device as DoP or as decimated PCM, and neither goes through here.
// Saying so is the whole implementation -- inventing a path that cannot work
// would be worse than the honest refusal, and the caller already has
// `NAE_ERR_NOT_SUPPORTED` for exactly this.
static int32_t openDopDevice(const char* deviceId, uint32_t carrier, uint32_t channels,
                             NAE_Handle** out) {
    (void)deviceId;
    (void)carrier;
    (void)channels;
    if (out) *out = nullptr;
    g_lastErrorText =
        "this platform has no native DSD output; DSD plays through DoP or as decimated PCM";
    return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
}
#endif

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
    handle->sourceFramesReal = frames;
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

    const uint32_t containerBits = handle->fmt.containerBits;
    const uint32_t validBits = handle->fmt.validBits;
    if (containerBits < 24 || validBits != 24) {
        g_lastErrorText = "DoP needs a 24-bit carrier";
        return NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    }
    const size_t bytesPerSample = containerBits / 8;
    const size_t bytesPerFrame = handle->fmt.blockAlign;
    const size_t ch = handle->fmt.channels;
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

    if (!handle->stream) return NAE_ERR_NOT_OPEN;
    neiro::OutputStream& stream = *handle->stream;
    if (!handle->clientRunning) {
        // Prefill a half buffer with valid DoP silence before starting.
        uint8_t* raw = nullptr;
        uint32_t chunk = 0;
        const uint32_t prefill = std::max<uint32_t>(1, handle->bufferFrames / 2);
        if (stream.acquire(prefill, &raw, &chunk) == neiro::StreamStatus::Ok && raw) {
            for (uint32_t f = 0; f < chunk; ++f) {
                const uint8_t marker = dop::nextMarker(&handle->dopMarker);
                for (size_t c = 0; c < ch; ++c) {
                    writeWord(raw + f * bytesPerFrame + c * bytesPerSample,
                              dop::word(marker, 0, 0));
                }
            }
            stream.commit(chunk, false);
        }
        const auto started = stream.start();
        if (started == neiro::StreamStatus::DeviceLost) return NAE_ERR_DEVICE_LOST;
        if (started != neiro::StreamStatus::Ok) return NAE_ERR_INITIALIZE;
        // The same bookkeeping the sine path does: without it every slice would
        // prefill half a buffer with packer silence and start the device again,
        // which is the seam this loop's comment below is about.
        handle->clientRunning = true;
    }

    const int64_t target = static_cast<int64_t>(milliseconds) * carrier / 1000;
    int64_t written = 0;
    bool ended = false;
    const bool traceDop = std::getenv("NAE_TRACE_DOP") != nullptr;
    while (written < target) {
        const double bufferStart = neiro::wasapi::nowMsHr();
        if (!stream.waitForRoom(2000)) ++handle->underrunCount;
        // What is ours to write: the whole buffer minus what the device has
        // already committed. A backend that cannot report its own padding -- the
        // macOS ring, where the ring *is* the buffer -- answers zero and the
        // whole thing is addressable.
        const uint32_t queued = stream.queuedFrames();
        const uint32_t capacity =
            queued < handle->bufferFrames ? handle->bufferFrames - queued : 0;
        if (traceDop && capacity < handle->bufferFrames) {
            std::fprintf(stderr, "[dop] queued=%u capacity=%u\n", queued, capacity);
        }
        if (capacity == 0) {
            // The device has nothing free until the next period. Wait for it
            // rather than spinning; the wait is bounded by the device.
            stream.waitForRoom(1);
            continue;
        }
        uint8_t* raw = nullptr;
        uint32_t chunk = 0;
        const auto got = stream.acquire(capacity, &raw, &chunk);
        if (got == neiro::StreamStatus::NoRoom) {
            if (traceDop) {
                std::fprintf(stderr, "[dop] not-ready after %.2f ms\n",
                             neiro::wasapi::nowMsHr() - bufferStart);
            }
            continue;
        }
        if (got == neiro::StreamStatus::DeviceLost) return NAE_ERR_DEVICE_LOST;
        if (got != neiro::StreamStatus::Ok) return NAE_ERR_INITIALIZE;
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
        stream.commit(chunk, false);
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
        // The stream's stop, not a platform call: this function compiles on
        // every backend, and "the slice ended" has to be said the same way on
        // all of them.
        if (stream.running()) stream.stop();
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
                                             : handle->fmt.rate;
    if (!rate) return 0.0;
    // Frames since the start of the file, not since the last seek. See
    // seekBaseFrames on the handle.
    const double pos = static_cast<double>(handle->seekBaseFrames + handle->sourcePos) /
                       static_cast<double>(rate);
    // The source's own length is the ceiling.
    //
    // `totalSourceFrames` is half a second longer than the file on purpose, and
    // `sourcePos` keeps advancing through that padded tail. Dividing by the pad
    // limit made the position run past the end of every track: measured 6500.0 ms
    // for a 6000.0 ms file and 1500.0 for a 1000.0 ms one, while
    // `silenceFramesWritten` stayed at 0 and `padFrameCount` accounted for 609
    // frames -- three fields describing one moment and disagreeing. A position
    // past the end is not a fact about the source, so it is not reported.
    if (handle->sourceFramesReal > 0) {
        const double end = static_cast<double>(handle->sourceFramesReal) /
                           static_cast<double>(rate);
        if (pos > end) return end;
    }
    return pos;
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
#ifdef _WIN32
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
    neiro::wasapi::RateCache& cache = neiro::wasapi::rateCache();
    std::vector<uint32_t> cachedRates;
    {
        std::lock_guard<std::mutex> lock(neiro::wasapi::rateCacheMutex());
        if (cache.deviceId != deviceId || cache.nativeRate != native.nSamplesPerSec) {
            bool probed = false;
            cache.rates = neiro::wasapi::probeSupportedRates(device.Get(), native, nativeValidBits, &probed);
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

    // What to ask the device for. The request carries the numbers the resolver
    // chose; the factory owns everything that talks to WASAPI -- activation,
    // `Initialize` in either share mode, the render event, the buffer size and
    // the render client -- and reports back only what the device granted.
    neiro::StreamFormatRequest want;
    want.deviceId = deviceId;
    want.rate = target.rate;
    want.channels = native.nChannels;
    want.containerBits = native.wBitsPerSample;
    want.validBits = nativeValidBits ? nativeValidBits : native.wBitsPerSample;
    // `wrap()` picks the subformat from this one tag, so the request uses the
    // same rule: the wire format is unchanged from when this path built it by
    // hand.
    want.isFloat = native.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    want.shareMode = (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE) ? NAE_SHARE_MODE_EXCLUSIVE
                                                               : NAE_SHARE_MODE_SHARED;
    // Buffer policy. Auto/Stable let the backend choose; LowLatency asks for the
    // smallest period the device accepts; Fixed asks for the exact duration.
    // A refused explicit request falls back to the backend default and is
    // reported, rather than failing playback.
    if (bufferMode == NAE_BUFFER_LOW_LATENCY) {
        want.bufferFrames = (target.rate * 10) / 1000;  // 10 ms
    } else if (bufferMode == NAE_BUFFER_FIXED && bufferMs > 0) {
        want.bufferFrames = (target.rate * bufferMs) / 1000;
    }

    // Read before `Initialize`, because that call is allowed to write back into
    // the format it was handed. Keeping the "before" value is the only way to
    // tell a device that agreed with us from one that quietly changed the depth.
    const uint16_t requestedBits = static_cast<uint16_t>(want.containerBits);

    neiro::OpenedFormat opened;
    std::string error;
    auto stream = neiro::openWasapiStream(want, &opened, &error);

    if (!stream && target.rate != native.nSamplesPerSec) {
        // The chosen rate was refused even though the probe said it would be
        // accepted. Rather than stop, fall back to the endpoint's own advertised
        // rate: the one format the driver is obliged to accept, and the one
        // `readNativeFormat` just handed us.
        //
        // This is a race rather than a theory. The probe releases its throwaway
        // client before the real open, so a device reconfigured in between -- a TV
        // renegotiating over HDMI, a driver reloading -- is exactly the case where
        // a list measured a moment ago is no longer true. The cost of the branch
        // is one extra open; the alternative is not playing at all.
        want.rate = native.nSamplesPerSec;
        // The old retry asked for the device's own period, so an explicit buffer
        // request is not carried into it.
        want.bufferFrames = 0;
        error.clear();
        stream = neiro::openWasapiStream(want, &opened, &error);
        if (stream) {
            // The reason changes with it. The listener asked for one rate, was
            // refused, and got another. "Resampled to the device rate" is true
            // here, but it was the *driver* that chose this rate and not their
            // setting, and the interface has to be able to say so.
            target.rate = opened.rate;
            target.resampling = effectiveSourceRate != target.rate;
            target.bitPerfect = false;
            target.coercedToSupportedRate = true;
            target.reason = neiro::PerfectReason::ResampledToSupportedRate;
        }
    }

    if (!stream) {
        // The single most useful thing this engine can say, and the device layer
        // already said it: the request, the result and the HRESULT. Naming a
        // cause here would be a guess -- `AUDCLNT_E_ENDPOINT_CREATE_FAILED` is
        // used by Windows both for a format the endpoint will not accept and for
        // an endpoint another application already holds.
        g_lastErrorText = error.empty() ? "the device refused to open" : error;
        return NAE_ERR_INITIALIZE;
    }
    // A successful open can still have something to say: an explicit buffer
    // request that was refused and replaced by the device default. This path has
    // always reported that and continued.
    if (!error.empty()) g_lastErrorText = error;

    // Both widths, kept apart on purpose. `containerBits` is the word the driver
    // takes; `validBits` is how many of its bits are audio.
    const uint16_t deviceValidBits = static_cast<uint16_t>(
        opened.validBits ? opened.validBits : opened.containerBits);

    auto* handle = new (std::nothrow) NAE_Handle();
    if (!handle) {
        return NAE_ERR_NO_MEMORY;
    }
    handle->stream = stream;
    // `opened` was measured after `Initialize`, so it holds whatever the device
    // agreed to -- the format the render loop must write, not the format that was
    // asked for. Keep both, because the difference between them is the single most
    // useful thing to know when the output comes out wrong.
    handle->fmt.rate = opened.rate;
    handle->fmt.channels = opened.channels;
    handle->fmt.containerBits = opened.containerBits;
    handle->fmt.validBits = opened.validBits;
    handle->fmt.blockAlign = opened.channels * ((opened.containerBits + 7u) / 8u);
    handle->fmt.isFloat = opened.isFloat;
    handle->grantedBits = static_cast<uint16_t>(opened.containerBits);
    handle->requestedBits = requestedBits;
    handle->bufferFrames = opened.bufferFrames;
    handle->latencyMs = opened.latencyMs;
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
        if (!handle->resampler.prepare(static_cast<int>(opened.channels), handle->sourceRate,
                                      opened.rate, q, &err)) {
            delete handle;
            remember(E_FAIL);
            return NAE_ERR_INITIALIZE;
        }
    }

    // Dither is sized to valid bits, and only engages when the device is
    // narrower than the pipeline's internal precision. A 24-in-32 endpoint
    // therefore passes through untouched, which is correct: there is
    // nothing to dither.
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
        static_cast<int32_t>(opened.channels) != handle->fileChannels) {
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

    if (outSampleRate) *outSampleRate = static_cast<int32_t>(opened.rate);
    if (outSourceRate) *outSourceRate = static_cast<int32_t>(effectiveSourceRate);
    if (channels) *channels = static_cast<int32_t>(opened.channels);
    if (bitsPerSample) *bitsPerSample = static_cast<int32_t>(opened.containerBits);
    if (validBits) *validBits = static_cast<int32_t>(deviceValidBits);
    if (isFloat) *isFloat = opened.isFloat ? 1 : 0;
    if (resampling) *resampling = handle->resampling ? 1 : 0;
    if (dithered) *dithered = handle->dithered ? 1 : 0;
    if (bitPerfect) *bitPerfect = handle->bitPerfect ? 1 : 0;
    if (latencyMs) *latencyMs = handle->latencyMs;

    *out = handle;
    return NAE_OK;
}
#else
// The macOS open is in open_coreaudio.cpp. The split is a file boundary, not an
// `#ifdef` boundary: a change to one platform's open path lands in a file the
// other platform does not compile.
#endif


char* duplicateString(const std::string& in) {
    char* out = static_cast<char*>(std::malloc(in.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, in.c_str(), in.size() + 1);
    return out;
}

// ---- restored from git HEAD ----
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

#ifdef _WIN32
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
#endif

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
        local.sampleRate = static_cast<int32_t>(handle->fmt.rate);
        local.channels = static_cast<int32_t>(handle->fmt.channels);
        local.bitsPerSample = static_cast<int32_t>(handle->fmt.containerBits);
        local.validBits = static_cast<int32_t>(handle->fmt.validBits);
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
    local.sampleRate = static_cast<int32_t>(handle->fmt.rate);
    local.channels = static_cast<int32_t>(handle->fmt.channels);
    local.bitsPerSample = static_cast<int32_t>(handle->fmt.containerBits);
    local.validBits = static_cast<int32_t>(handle->fmt.validBits);
    local.isFloat = (handle->fmt.isFloat) ? 1 : 0;
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
    // listener has not heard; the driver part is what the backend reports for the
    // stream, which is downstream of us.
    //
    // The driver figure is read on every call rather than cached at open, because
    // the device is allowed to change it while the stream runs and a cached number
    // would be a number about the past. A backend that cannot answer -- the macOS
    // ring has no such figure -- returns zero rather than a guess: "we do not
    // know" and "the driver says nothing" must not add up to the same display.
    const double driverLatencyMs = handle->stream ? handle->stream->driverLatencyMs() : 0.0;
    local.bufferFrames = static_cast<int32_t>(handle->bufferFrames);
    local.channelDelayMaxFrames = static_cast<int32_t>(handle->channelDelayMax);
    local.bufferLatencyMs = handle->latencyMs;
    local.driverLatencyMs = driverLatencyMs;
    // Stages that delay the signal add to the end-to-end figure. A linear-phase
    // EQ and the convolver hold samples back, and the clip guard delays by its
    // lookahead; a host that syncs video to `totalLatencyMs` needs them.
    {
        const double rate = handle->fmt.rate;
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
    local.bufferDurationMs = handle->fmt.rate
                              ? 1000.0 * static_cast<double>(handle->bufferFrames) /
                                    static_cast<double>(handle->fmt.rate)
                              : 0.0;
    local.deviceLostCount = handle->deviceLostCount;
    local.recoveryCount = handle->recoveryCount;
    return NAE_OK;
}



int32_t NAE_PlaySine(NAE_Handle* handle, int32_t milliseconds) NAE_NOEXCEPT {
    if (!handle || !handle->stream) return NAE_ERR_NOT_OPEN;

    const int32_t channels = static_cast<int32_t>(handle->fmt.channels);
    const uint32_t deviceRate = handle->fmt.rate;
    // The tone is generated at the SOURCE rate, not the device rate, so the
    // converter is genuinely exercised rather than bypassed.
    const uint32_t sourceRate = handle->sourceRate ? handle->sourceRate : deviceRate;
    const bool isFloat = handle->fmt.isFloat;
    const uint32_t containerBits = handle->fmt.containerBits;
    const uint32_t validBits = handle->fmt.validBits
                                   ? handle->fmt.validBits
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
    const bool haveDevice = handle->stream != nullptr;
    if (haveDevice && firstStartOfStream) {
        const uint32_t bpf = handle->fmt.blockAlign;
        const uint32_t prefill = std::max<uint32_t>(1, handle->bufferFrames / 2);
        neiro::OutputStream& prefillStream = *handle->stream;
        uint8_t* prefillData = nullptr;
        uint32_t prefillChunk = 0;
        if (bpf > 0 &&
            prefillStream.acquire(prefill, &prefillData, &prefillChunk) ==
                neiro::StreamStatus::Ok &&
            prefillData && prefillChunk > 0) {
            std::memset(prefillData, 0,
                        static_cast<size_t>(prefillChunk) * static_cast<size_t>(bpf));
            prefillStream.commit(prefillChunk, true);
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
    const auto startStatus = handle->stream->start();
    if (startStatus != neiro::StreamStatus::Ok) {
        // The device can vanish between a successful Initialize and Start -- a
        // narrow window, but a USB removal inside it is a real event and it used
        // to be reported as "the stream could not be initialised", which is both
        // untrue and unactionable. Same treatment as the three call sites in the
        // render loop.
        if (startStatus == neiro::StreamStatus::DeviceLost) {
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
static constexpr size_t kSpectrumFft = 4096;


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

    const uint32_t bytesPerFrame = handle->fmt.blockAlign;
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

        ensureAudioThreadQos();

        // Wait for the period, then ask how much room there is. The wait is the
        // device's own notification where the backend has one -- a high-resolution
        // timer or `Sleep(1)` cannot see inside a 5.7 ms buffer -- and asking a
        // moment before the period rolls over is how the request lands on the
        // wrong side of the deadline.
        if (!handle->stream->waitForRoom(2000)) ++handle->underrunCount;
        if (sourceDone && fifo.size() / ch == 0) {
            if (!atBoundary()) break;
        }

        // Frames the device still has queued.
        //
        // This is also the first thing a lost device is visible through: the
        // stream cannot read its own padding once the endpoint is disabled, so it
        // answers zero and the `acquire` below fails with AUDCLNT_E_RESOURCES_
        // INVALIDATED, which is where the device-lost verdict is reached.
        const uint32_t padding = handle->stream->queuedFrames();

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
            (handle->fmt.rate > 0)
                ? (static_cast<double>(padding) * 1000.0 /
                   static_cast<double>(handle->fmt.rate))
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
            neiro::OutputStream& stream = *handle->stream;
            uint8_t* silentRaw = nullptr;
            uint32_t silentChunk = 0;
            const auto silentStatus = stream.acquire(capacity, &silentRaw, &silentChunk);
            if (silentStatus == neiro::StreamStatus::NoRoom) {
                if (!stream.waitForRoom(2000)) ++handle->underrunCount;
                if (++stalledIterations > 2000) {
                    g_lastErrorText = "the output device stopped accepting audio";
                    break;
                }
                continue;
            }
            if (silentStatus != neiro::StreamStatus::Ok) {
                if (silentStatus == neiro::StreamStatus::DeviceLost) {
                    deviceLost = true;
                    g_lastErrorText = "the output device was removed or is being used by "
                                      "another program; playback is paused at the current "
                                      "position";
                }
                break;
            }
            if (stream.commit(silentChunk, true) == neiro::StreamStatus::DeviceLost) {
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

        neiro::OutputStream& stream = *handle->stream;
        uint8_t* raw = nullptr;
        uint32_t chunk = 0;
        const auto bufStatus = stream.acquire(capacity, &raw, &chunk);
        if (bufStatus == neiro::StreamStatus::NoRoom) {
            // "Not right now", not "never again".
            //
            // Asking for the whole buffer before the engine has one free is the
            // normal case in exclusive mode, and WASAPI says so with
            // AUDCLNT_E_BUFFER_TOO_LARGE rather than by blocking. Treating it as
            // fatal is what stopped the loop after exactly one buffer: the trace
            // read `devWritten=9728 budget=288000` with an hresult of zero, which
            // is a renderer that gave up rather than one that failed.
            //
            // Wait to be told, rather than asking. Two seconds is a backstop, not a cadence:
            // with the event available the wait ends when the device says so.
            if (!stream.waitForRoom(2000)) ++handle->underrunCount;
            if (++stalledIterations > 2000) {
                g_lastErrorText = "the output device stopped accepting audio";
                break;
            }
            continue;
        }
        if (bufStatus != neiro::StreamStatus::Ok) {
            // A device that is gone ends the stream now, with its own error code,
            // rather than being folded into the short-render check below. That
            // check already returned an error for this case, so the difference is
            // not that the failure becomes visible -- it is that the failure stops
            // looking like every other one. A host that sees NAE_ERR_DEVICE_LOST
            // knows to keep the handle, tell the user, and wait; a host that sees
            // NAE_ERR_INITIALIZE would tear the stream down, and the user would
            // lose their place to an unplugged cable.
            if (bufStatus == neiro::StreamStatus::DeviceLost) {
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

        const auto relStatus = stream.commit(static_cast<uint32_t>(framesToWrite), false);
        if (relStatus != neiro::StreamStatus::Ok) {
            // The same distinction as GetBuffer above. ReleaseBuffer is where a
            // preemption actually tends to surface: the device accepted the
            // request and then lost the endpoint before the buffer was handed
            // over, so the audio in `raw` never reached it and the frames were
            // already erased from the fifo. The accounting below still balances,
            // because those frames are counted as consumed -- which is exactly
            // why this needs its own code rather than being left to the
            // short-render check to notice.
            if (relStatus == neiro::StreamStatus::DeviceLost) {
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

    // 0 = the device accepted the stop, 1 = it refused. A plain int rather than
    // HRESULT because this line is shared with macOS, where no such type exists;
    // the only consumer is the trace below.
    int stopRefused = 0;
    if (shouldStop) {
        if (handle->stream && handle->stream->running()) {
            const auto st = handle->stream->stop();
            stopRefused = st == neiro::StreamStatus::Ok ? 0 : 1;
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
        std::fprintf(stderr, "[nae] done source=%lld/%lld device=%lld iters=%d stopRefused=%d\n",
                     static_cast<long long>(sourcePos),
                     static_cast<long long>(sourceLimit),
                     static_cast<long long>(deviceFramesWritten), iterations, stopRefused);
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

#ifdef _WIN32
void NAE_Shutdown(void) NAE_NOEXCEPT {
    neiro::wasapi::shutdownCom();
}
#else
// Nothing to tear down: there is no COM apartment on this platform. The device
// is released when the last stream is destroyed.
void NAE_Shutdown(void) NAE_NOEXCEPT {}
#endif


#ifdef _WIN32
int32_t NAE_QueryDeviceCaps(const char* deviceId, int32_t shareMode,
                            NAE_DeviceCaps* out) NAE_NOEXCEPT {
    // The device knowledge lives with the device layer. This used to be the
    // second place that asked a WASAPI endpoint about itself, beside the open;
    // two readers of one device is how `supportedShareModes` once ended up
    // contradicting `exclusiveSupported`.
    return neiro::wasapi::queryWasapiCaps(deviceId, shareMode, out);
}
#else
// macOS: the backend already knows how to ask the HAL. Threading through it
// keeps one copy of the device knowledge, so the caps this returns and the ones
// `backend_check` prints cannot disagree.
int32_t NAE_QueryDeviceCaps(const char* deviceId, int32_t shareMode,
                            NAE_DeviceCaps* out) NAE_NOEXCEPT {
    if (!out || out->structSize != sizeof(NAE_DeviceCaps)) return NAE_ERR_INVALID_ARGUMENT;
    auto backend = neiro::createPlatformBackend();
    if (!backend) {
        g_lastErrorText = "no audio backend on this platform";
        return NAE_ERR_NO_DEVICE;
    }
    std::string error;
    if (!backend->queryCaps(deviceId ? deviceId : "", shareMode, out, &error)) {
        g_lastErrorText = error.empty() ? "no such output device" : error;
        return NAE_ERR_NO_DEVICE;
    }
    return NAE_OK;
}
#endif



int32_t NAE_QuerySupportedRates(NAE_Handle* handle, uint32_t* rates, uint32_t capacity,
                                uint32_t* outCount) NAE_NOEXCEPT {
    if (outCount) *outCount = 0;

#ifdef _WIN32
    std::vector<uint32_t> cached;
    bool probed = false;
    {
        std::lock_guard<std::mutex> lock(neiro::wasapi::rateCacheMutex());
        const neiro::wasapi::RateCache& cache = neiro::wasapi::rateCache();
        probed = cache.probed;
        cached = cache.rates;
    }
#else
    // No probe cache here: the rate list comes from the device's own
    // `NAE_DeviceCaps`, which `queryCaps` reads on demand. A second cache would
    // be a second answer to the same question.
    std::vector<uint32_t> cached;
    bool probed = false;
    {
        NAE_DeviceCaps caps{};
        caps.structSize = sizeof(caps);
        auto backend = neiro::createPlatformBackend();
        std::string error;
        if (backend && backend->queryCaps("", NAE_SHARE_MODE_EXCLUSIVE, &caps, &error)) {
            probed = caps.rateCount > 0;
            for (uint32_t i = 0; i < caps.rateCount; ++i) cached.push_back(caps.rates[i]);
        }
    }
#endif
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
constexpr double kMetricsWindowMs = 400.0;
constexpr double kMetricsHopMs = 100.0;

static inline bool cancelRequested(const int32_t* flag) {
    if (!flag) return false;
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(flag, __ATOMIC_RELAXED) != 0;
#else
    return *flag != 0;
#endif
}

double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double pos = q * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}
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
uint32_t NAE_GetAnalyzerVersion(void) NAE_NOEXCEPT { return NAE_ANALYZER_VERSION; }



}  // extern "C"

// ---- restored from git HEAD after an accidental deletion ----