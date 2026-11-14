// The ASIO device layer.
//
// # Why `IASIO` is declared here by hand
//
// There is no ASIO SDK on the machine this was written on. The interface is
// therefore written out below from the published ASIO host interface: `IASIO`
// derives from `IUnknown` and adds twenty-one methods, in the order ASIO numbers
// them.
//
// **The vtable order is the risk in this file.** A COM interface has no name at
// run time -- a call is "the function pointer at slot N" -- so one method
// inserted or omitted above shifts every call after it. Nothing would fail to
// compile and nothing would return an error: `start()` would land in `stop()`,
// `createBuffers()` in `getSampleRate()`, and the symptom would be silent nonsense
// rather than a message. If a future ASIO revision is adopted, this declaration is
// what to re-check first, and the check is done against a real driver.
//
// The order below was verified against `rthdasio64.dll` 3.1.13.1 on this
// machine: the driver's own vtable is twenty-four entries -- three `IUnknown`
// methods and these twenty-one -- and `getDriverName` returned "Realtek ASIO"
// through slot 4.
//
// # Three things about drivers that are not in the spec
//
//   * `CoCreateInstance` must be given the *CLSID as the interface id*. That is
//     the ASIO host convention -- the registered CLSID doubles as the interface
//     id -- and this driver answers `E_NOINTERFACE` for `IID_IUnknown`.
//   * The driver is registered with `ThreadingModel=Apartment`. Creating it from a
//     multi-threaded apartment makes COM host it in a side apartment and marshal
//     every call through a proxy built for an interface COM knows nothing about,
//     which corrupts the arguments: `ASIOInit` took the window handle into a
//     marshaller that read it as an interface pointer and overflowed the stack.
//     So the object is created on, and only ever called from, a single-threaded
//     apartment of our own (`HostThread` below). Creating it from a
//     single-threaded apartment on the *calling* thread works as well, but the
//     engine's apartment is multi-threaded and stays that way.
//   * The apartment owns a window, because `ASIOInit` takes one and some drivers
//     post messages to it.
//
// One more trap, this one ours rather than theirs: `IASIO` must not live in an
// anonymous namespace. GCC devirtualizes calls on a class with internal linkage
// -- it can see every translation unit that could derive from it, and that is
// just this one -- so it emitted `__cxa_pure_virtual` for `ASIOInit` and the
// process died with "pure virtual method called" on the first open. The
// declaration lives in `asio_sdk` below for that reason and the comment there
// says so again.
//
// # What this layer does and does not do
//
// It moves bytes. The render callback hands it the core's FINAL interleaved device
// words; it copies them into the driver's per-channel buffers and nothing else --
// no conversion, no second dither stage, no volume. The one unavoidable transform
// is the de-interleave: ASIO gives one pointer per channel while the core produces
// interleaved frames, so a scatter per period is the shape of this API rather than
// a choice.
#include "backend_asio.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "audio_backend.h"
#include "output_stream.h"

/// The HRESULT behind the last failure, defined once in `nae.cpp`.
///
/// Declared here rather than pulled in from `wasapi_device.h`, because this file
/// has no business including the WASAPI layer; the declaration is what is shared.
extern thread_local HRESULT g_lastHr;

namespace neiro::asio {

// ---------------------------------------------------------------------------
// ASIO, as the driver sees it -- at namespace scope, not in the anonymous
// namespace below.
//
// That distinction is not cosmetic. GCC devirtualizes calls on a class with
// internal linkage, because it can see every translation unit that could derive
// from it, and for an anonymous-namespace class that is only this file. With
// `IASIO` declared there it concluded that `init` could only ever be the pure
// thunk and emitted `__cxa_pure_virtual` where the call should have gone to the
// driver -- the process died with "pure virtual method called" on the first
// `ASIOInit`, while the same sequence in a standalone program worked. The
// driver's implementation is in another module, which the compiler has no way to
// know about; external linkage is what keeps the call virtual.
//
// Only the methods this backend calls are listed, but the *order* is ASIO's: a
// subset would shift every slot after the first omission.
// ---------------------------------------------------------------------------
namespace asio_sdk {

using ASIOBool = long;
using ASIOError = long;
using ASIOSampleRate = double;
/// ASIO counts samples in 64 bits. Every driver in the field uses the plain
/// 64-bit count and every host has read the spec that way since ASIO 2.0.
using ASIOSamples = int64_t;
using ASIOTimeStamp = int64_t;

constexpr ASIOBool kAsioFalse = 0;
constexpr ASIOBool kAsioTrue = 1;

// ASIOError, from asiosys.h.
constexpr ASIOError kAseOk = 0;
constexpr ASIOError kAseSuccess = 0x3f4847a0;
constexpr ASIOError kAseNotPresent = -1000;
constexpr ASIOError kAseHwMalfunction = -999;
constexpr ASIOError kAseInvalidParameter = -998;
constexpr ASIOError kAseInvalidMode = -997;
constexpr ASIOError kAseInvalidSampleRate = -995;

// ASIOSampleType, from asio.h.
constexpr long kAsioStInt32Lsb = 2;
constexpr long kAsioStInt32Lsb24 = 8;

struct ASIOChannelInfo {
    long channel;
    ASIOBool isInput;
    long channelGroup;
    long type;
    char name[32];
};

struct ASIOBufferInfo {
    ASIOBool isInput;
    long channelNum;
    void* buffers[2];
};

/// Opaque to us: it belongs to `bufferSwitchTimeInfo`, which is never installed.
struct ASIOTime;

constexpr long kAsioSelectorSupported = 1;
constexpr long kAsioEngineVersion = 2;
constexpr long kAsioResetRequest = 3;
constexpr long kAsioBufferSizeChange = 5;
constexpr long kAsioResyncRequest = 6;
constexpr long kAsioLatenciesChanged = 7;
constexpr long kAsioSupportsTimeInfo = 8;
constexpr long kAsioSupportsTimeCode = 9;

struct ASIOCallbacks {
    void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate sRate);
    long (*asioMessage)(long selector, long value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex,
                                      ASIOBool directProcess);
};

/// The driver's host interface. The slot numbers are the contract; see the top of
/// the file before touching this.
struct IASIO : public IUnknown {
    virtual ASIOBool init(void* sysRef) = 0;                                      // 3
    virtual void getDriverName(char* name) = 0;                                   // 4
    virtual long getDriverVersion() = 0;                                          // 5
    virtual void getErrorMessage(char* string) = 0;                               // 6
    virtual ASIOError start() = 0;                                                // 7
    virtual ASIOError stop() = 0;                                                 // 8
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;  // 9
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;   // 10
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize,
                                    long* granularity) = 0;                       // 11
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;               // 12
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;              // 13
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;               // 14
    virtual ASIOError getClockSources(void* clocks, long* numSources) = 0;        // 15
    virtual ASIOError setClockSource(long reference) = 0;                         // 16
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;  // 17
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;                  // 18
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels,
                                    long bufferSize, ASIOCallbacks* callbacks) = 0;  // 19
    virtual ASIOError disposeBuffers() = 0;                                       // 20
    virtual ASIOError controlPanel() = 0;                                         // 21
    virtual ASIOError future(long selector, void* opt) = 0;                       // 22
    virtual ASIOError outputReady() = 0;                                          // 23
};

}  // namespace asio_sdk

namespace {

using asio_sdk::ASIOBool;
using asio_sdk::ASIOBufferInfo;
using asio_sdk::ASIOCallbacks;
using asio_sdk::ASIOChannelInfo;
using asio_sdk::ASIOError;
using asio_sdk::ASIOSampleRate;
using asio_sdk::ASIOSamples;
using asio_sdk::ASIOTime;
using asio_sdk::ASIOTimeStamp;
using asio_sdk::IASIO;
using namespace asio_sdk;  // the constants: ASE_*/ASIO sample types and selectors

const char* errorName(ASIOError code) {
    switch (code) {
        case kAseOk: return "ASE_OK";
        case kAseSuccess: return "ASE_SUCCESS";
        case kAseNotPresent: return "ASE_NotPresent";
        case kAseHwMalfunction: return "ASE_HWMalfunction";
        case kAseInvalidParameter: return "ASE_InvalidParameter";
        case kAseInvalidMode: return "ASE_InvalidMode";
        case kAseInvalidSampleRate: return "ASE_InvalidRate";
        default: return "an unlisted ASIOError";
    }
}

/// "ASE_NotPresent (-1000)": the name, because it is what a reader recognises, and
/// the number, because it distinguishes codes the table does not carry.
std::string describe(ASIOError code) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s (%ld)", errorName(code), static_cast<long>(code));
    return buffer;
}

/// The sample type name for a refusal message.
const char* sampleTypeName(long type) {
    switch (type) {
        case 0: return "ASIOSTInt16LSB";
        case 1: return "ASIOSTInt24LSB";
        case 2: return "ASIOSTInt32LSB";
        case 3: return "ASIOSTFloat32LSB";
        case 4: return "ASIOSTFloat64LSB";
        case 5: return "ASIOSTInt32LSB16";
        case 6: return "ASIOSTInt32LSB18";
        case 7: return "ASIOSTInt32LSB20";
        case 8: return "ASIOSTInt32LSB24";
        case 9: return "ASIOSTDSDInt8LSB1";
        case 10: return "ASIOSTDSDInt8MSB1";
        case 11: return "ASIOSTDSDInt8NER8";
        case 16: return "ASIOSTInt16MSB";
        case 17: return "ASIOSTInt24MSB";
        case 18: return "ASIOSTInt32MSB";
        case 19: return "ASIOSTFloat32MSB";
        case 20: return "ASIOSTFloat64MSB";
        case 21: return "ASIOSTInt32MSB16";
        case 22: return "ASIOSTInt32MSB18";
        case 23: return "ASIOSTInt32MSB20";
        case 24: return "ASIOSTInt32MSB24";
        default: return "an unlisted ASIOSampleType";
    }
}

// ---------------------------------------------------------------------------
// The registry: the only device identity ASIO has.
//
// There is no enumeration interface in ASIO. A driver registers itself under
// `HKLM\SOFTWARE\ASIO\<name>` with a `CLSID` value, and a host that wants a list
// reads that key. Both registry views are read because a 32-bit driver registers
// under `Wow6432Node`; this process is 64-bit and cannot load one, but hiding it
// would read as "the driver is not installed".
// ---------------------------------------------------------------------------

struct RegisteredDriver {
    std::string name;
    std::string clsid;  // "{A80362FF-CE76-4DD9-874A-704C57BF0D6A}", as registered
};

void readAsioKey(HKEY root, const wchar_t* path, std::vector<RegisteredDriver>* out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;

    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD nameLength = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
        if (RegEnumKeyExW(key, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        const std::wstring wideName(name, nameLength);

        wchar_t clsid[128];
        DWORD clsidBytes = static_cast<DWORD>(sizeof(clsid));
        if (RegGetValueW(key, wideName.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, clsid,
                         &clsidBytes) != ERROR_SUCCESS) {
            continue;
        }

        // The key name is the device name; every driver seen so far repeats it in
        // `Description`, so the key name is the source.
        const int nameChars = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(nameLength),
                                                 nullptr, 0, nullptr, nullptr);
        const int clsidChars = WideCharToMultiByte(CP_UTF8, 0, clsid, -1, nullptr, 0, nullptr,
                                                  nullptr);
        if (nameChars <= 0 || clsidChars <= 1) continue;

        RegisteredDriver driver;
        driver.name.assign(static_cast<size_t>(nameChars), '\0');
        WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(nameLength), driver.name.data(),
                            nameChars, nullptr, nullptr);
        driver.clsid.assign(static_cast<size_t>(clsidChars - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, clsid, -1, driver.clsid.data(), clsidChars, nullptr,
                            nullptr);
        out->push_back(std::move(driver));
    }
    RegCloseKey(key);
}

std::vector<RegisteredDriver> registeredDrivers() {
    std::vector<RegisteredDriver> drivers;
    readAsioKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", &drivers);
    readAsioKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\ASIO", &drivers);
    // A driver visible in both views is one device. Sorted by CLSID first so the
    // duplicate is adjacent; the 64-bit view was read first, so that copy stays.
    std::stable_sort(drivers.begin(), drivers.end(),
                     [](const RegisteredDriver& a, const RegisteredDriver& b) {
                         return a.clsid < b.clsid;
                     });
    drivers.erase(std::unique(drivers.begin(), drivers.end(),
                              [](const RegisteredDriver& a, const RegisteredDriver& b) {
                                  return a.clsid == b.clsid;
                              }),
                  drivers.end());
    std::sort(drivers.begin(), drivers.end(),
              [](const RegisteredDriver& a, const RegisteredDriver& b) {
                  return a.name < b.name || (a.name == b.name && a.clsid < b.clsid);
              });
    return drivers;
}

std::string deviceIdFor(const std::string& clsid) { return "asio:" + clsid; }

/// The CLSID behind an `asio:{...}` id.
bool clsidFromDeviceId(const char* deviceId, CLSID* out) {
    if (!isAsioDeviceId(deviceId)) return false;
    const char* text = deviceId + std::strlen("asio:");
    if (*text == '\0') return false;
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (needed <= 1) return false;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), needed);
    return SUCCEEDED(CLSIDFromString(wide.c_str(), out));
}

// ---------------------------------------------------------------------------
// The apartment the driver lives in.
//
// One thread, one apartment, one window, and every `IASIO` call made from it.
// Calls from the render thread are posted here and waited for. The realtime
// callback is the exception: the driver calls *us* from its own thread, and the
// copy there involves no driver call beyond `outputReady`, which the driver
// expects from that thread.
// ---------------------------------------------------------------------------

class HostThread {
  public:
    HostThread() = default;
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;
    ~HostThread() { stop(); }

    /// Starts the apartment and creates its window. False means the apartment
    /// itself could not be entered, and `*error` says why.
    bool start(std::string* error) {
        if (thread_.joinable()) return true;
        thread_ = std::thread([this] { run(); });
        std::unique_lock<std::mutex> lock(mutex_);
        readyCv_.wait(lock, [this] { return ready || startFailed_; });
        if (!startFailed_) return true;
        const std::string message = startError_;
        lock.unlock();
        if (thread_.joinable()) thread_.join();
        if (error) *error = message;
        return false;
    }

    /// The window handed to `ASIOInit`.
    HWND window() const { return window_; }

    /// Runs `fn` on the apartment thread and returns its result.
    ///
    /// Serialised: two callers would otherwise overwrite one another's slot and
    /// the second would receive the first's answer. Blocking on a driver call is
    /// acceptable for control (open, start, stop); nothing realtime calls through
    /// here.
    template <typename F>
    std::invoke_result_t<F> call(F&& fn) {
        using R = std::invoke_result_t<F>;
        std::lock_guard<std::mutex> serialise(callMutex_);
        if (!thread_.joinable()) return R{};

        std::optional<R> result;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            pending_ = [&] { result.emplace(fn()); };
            jobDone_ = false;
            SetEvent(wake_);
            done_.wait(lock, [this] { return jobDone_; });
        }
        return std::move(*result);
    }

    void stop() {
        if (!thread_.joinable()) {
            if (wake_) {
                CloseHandle(wake_);
                wake_ = nullptr;
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quitting_ = true;
        }
        if (wake_) SetEvent(wake_);
        thread_.join();
        if (wake_) {
            CloseHandle(wake_);
            wake_ = nullptr;
        }
        window_ = nullptr;
    }

  private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        startFailed_ = true;
        startError_ = message;
        readyCv_.notify_all();
    }

    void run() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            // The thread is ours, so a changed mode cannot happen; treat it as
            // fatal rather than continuing in an apartment the driver did not ask
            // for.
            char text[160];
            std::snprintf(text, sizeof(text),
                          "the ASIO apartment could not be entered (CoInitializeEx 0x%08lX)",
                          static_cast<unsigned long>(hr));
            fail(text);
            return;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &HostThread::windowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"NeiroAsioHostWindow";
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            CoUninitialize();
            fail("the ASIO host window class could not be registered");
            return;
        }

        // A real top-level window rather than `HWND_MESSAGE`: some drivers follow
        // device broadcasts, and a message-only window never receives one.
        window_ = CreateWindowExW(0, windowClass.lpszClassName, L"neiro asio host", WS_POPUP, 0, 0,
                                  0, 0, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (!window_) {
            CoUninitialize();
            fail("the ASIO host window could not be created");
            return;
        }
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_) {
            DestroyWindow(window_);
            window_ = nullptr;
            CoUninitialize();
            fail("the ASIO host event could not be created");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready = true;
        }
        readyCv_.notify_all();

        for (;;) {
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_) {
                    job = std::move(pending_);
                    pending_ = nullptr;
                }
            }
            if (job) {
                job();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    jobDone_ = true;
                }
                done_.notify_all();
                continue;
            }

            bool quitting = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                quitting = quitting_;
            }
            if (quitting) break;

            // Wait for either a posted call or a window message. The message half
            // is not decoration: `ASIOInit` is handed this window, and a driver
            // that posts to it expects it dispatched.
            const DWORD which =
                MsgWaitForMultipleObjectsEx(1, &wake_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (which == WAIT_OBJECT_0 + 1) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        }

        DestroyWindow(window_);
        window_ = nullptr;
        CoUninitialize();
    }

    std::thread thread_;
    HWND window_ = nullptr;
    HANDLE wake_ = nullptr;

    std::mutex mutex_;
    std::condition_variable readyCv_;
    std::condition_variable done_;
    bool ready = false;
    bool startFailed_ = false;
    bool quitting_ = false;
    bool jobDone_ = false;
    std::string startError_;
    std::function<void()> pending_;
    /// Held for the whole of a call, so two threads cannot share the slot above.
    std::mutex callMutex_;
};

// ---------------------------------------------------------------------------
// The stream.
// ---------------------------------------------------------------------------

/// The driver's audio thread needs no `this`: ASIO's callback signature carries no
/// user pointer. A process therefore has at most one ASIO stream at a time, which
/// `openDevice` enforces by refusing a second rather than discovering the problem
/// when the second `createBuffers` fails.
std::atomic<class AsioStream*> g_activeStream{nullptr};

/// One owner for "a driver is open in this process".
///
/// ASIO's interface has no per-stream context to hand the callback, so two open
/// streams would have one driver's callbacks land on the other's buffers. The
/// second open is refused rather than left to fail somewhere less legible.
struct StreamRegistry {
    static std::atomic<bool>& claimed() {
        static std::atomic<bool> flag{false};
        return flag;
    }
    static bool take() {
        bool expected = false;
        return claimed().compare_exchange_strong(expected, true);
    }
    static void release() { claimed().store(false); }
};

/// Holds the claim for the duration of an open, and gives it up on any return --
/// an early failure that forgot to would make every later open in the process fail
/// with "a stream is already open".
class ClaimGuard {
  public:
    ClaimGuard() { held_ = StreamRegistry::take(); }
    ClaimGuard(const ClaimGuard&) = delete;
    ClaimGuard& operator=(const ClaimGuard&) = delete;
    ~ClaimGuard() {
        if (held_ && !committed_) StreamRegistry::release();
    }
    bool held() const { return held_; }
    /// The stream now owns the claim; it releases it when the driver is released.
    void commit() { committed_ = true; }

  private:
    bool held_ = false;
    bool committed_ = false;
};

class AsioStream final : public BackendStream {
  public:
    /// `scratch` is allocated by the caller and moved in, so nothing in this
    /// constructor allocates: a throw after `host` had been moved would leave the
    /// driver open and holding the device with nobody left to release it.
    AsioStream(std::unique_ptr<HostThread> host, IASIO* driver, std::vector<ASIOBufferInfo> buffers,
               std::vector<uint8_t> scratch, uint32_t channels, uint32_t bufferFrames,
               uint32_t rate)
        : host_(std::move(host)),
          driver_(driver),
          buffers_(std::move(buffers)),
          channels_(channels),
          bufferFrames_(bufferFrames),
          rate_(rate),
          bytesPerFrame_(channels * 4),
          // Allocated once, at open: the callback may not allocate, and this is
          // the one path where that is not negotiable.
          scratch_(std::move(scratch)) {}

    ~AsioStream() override {
        running_.store(false, std::memory_order_release);
        host_->call([this] { return driver_->stop(); });
        // Unpublished before the driver's buffers are released: `ASIOStop` is
        // documented to return after the last callback, but a driver that fires one
        // anyway then finds nothing to write to instead of a pointer into memory
        // that is about to be freed.
        AsioStream* expected = this;
        g_activeStream.compare_exchange_strong(expected, nullptr);
        host_->call([this] {
            driver_->disposeBuffers();
            return kAseOk;
        });
        // Released in the apartment that created it, and then the apartment goes.
        host_->call([this] {
            driver_->Release();
            return 0;
        });
        driver_ = nullptr;
        host_->stop();
        // The driver is gone, so the next open may take it.
        StreamRegistry::release();
    }

    /// Installed between `createBuffers` and `start`, the only window in which the
    /// driver cannot call it: ASIO has no callbacks before `ASIOStart`.
    void setRender(RenderCallback render) { render_ = std::move(render); }

    bool start(std::string* error) override {
        // Set before the call: a driver is allowed to fire its first callback from
        // inside `ASIOStart`, and a callback that sees "not running" writes
        // silence into a buffer that goes straight to the device.
        running_.store(true, std::memory_order_release);
        const ASIOError code = host_->call([this] { return driver_->start(); });
        if (code != kAseOk) {
            running_.store(false, std::memory_order_release);
            if (error) *error = "the ASIO driver refused ASIOStart: " + describe(code);
            return false;
        }
        return true;
    }

    void stop() override {
        // Cleared first, so a callback already in flight writes silence rather
        // than pulling from a ring the caller is about to dismantle.
        running_.store(false, std::memory_order_release);
        host_->call([this] { return driver_->stop(); });
    }

    void pause() override { stop(); }
    void resume() override {
        std::string ignored;
        start(&ignored);
    }

    bool deviceLost() const override {
        return lost_.load(std::memory_order_relaxed) ||
               rateChanged_.load(std::memory_order_relaxed);
    }

    double latencyMs() const override {
        // The buffer's own duration: what the device holds on our behalf.
        return rate_ ? (1000.0 * static_cast<double>(bufferFrames_)) /
                           static_cast<double>(rate_)
                     : 0.0;
    }

    /// The driver's own figure, read on demand rather than cached at open: a
    /// driver may change it when its buffer size or clock changes.
    double driverLatencyMs() const {
        long input = 0;
        long output = 0;
        const ASIOError code = host_->call(
            [this, &input, &output] { return driver_->getLatencies(&input, &output); });
        if (code != kAseOk || !rate_) return 0.0;
        return 1000.0 * static_cast<double>(output) / static_cast<double>(rate_);
    }

    /// The driver's thread. No allocation, no lock, no driver call except
    /// `outputReady`, which the driver expects here.
    void bufferSwitch(long index) {
        if (index < 0 || index > 1) return;
        const size_t frames = bufferFrames_;
        const size_t bytesPerFrame = bytesPerFrame_;

        if (!running_.load(std::memory_order_acquire)) {
            for (uint32_t channel = 0; channel < channels_; ++channel) {
                if (void* half = buffers_[channel].buffers[index]) std::memset(half, 0, frames * 4);
            }
            return;
        }

        size_t got = render_ ? render_(scratch_.data(), frames) : 0;
        if (got > frames) got = frames;
        if (got < frames) {
            // A short pull is a gap, not a quiet passage: the driver has already
            // committed to playing these frames. The gap is counted by the ring
            // that ran dry, and only there: one counter for one event.
            std::memset(scratch_.data() + got * bytesPerFrame, 0, (frames - got) * bytesPerFrame);
        }

        for (uint32_t channel = 0; channel < channels_; ++channel) {
            auto* destination = static_cast<uint32_t*>(buffers_[channel].buffers[index]);
            if (!destination) continue;
            const uint8_t* source = scratch_.data() + static_cast<size_t>(channel) * 4;
            for (size_t frame = 0; frame < frames; ++frame) {
                // memcpy rather than a cast: the window is a byte buffer, and this
                // keeps the load defined whatever its alignment.
                uint32_t word = 0;
                std::memcpy(&word, source + frame * bytesPerFrame, sizeof(word));
                destination[frame] = word;
            }
        }
        if (outputReady_.load(std::memory_order_relaxed)) {
            if (driver_->outputReady() != kAseOk) {
                // The driver answered once and said it does not implement this.
                // Calling it every period would be a virtual call for nothing.
                outputReady_.store(false, std::memory_order_relaxed);
            }
        }
    }

    /// From `ASIOCallbacks`, on the driver's thread.
    void onRateChanged() {
        // The stream is no longer at the rate it was opened at, so every number
        // derived from that rate -- position, the rate window, the buffer's
        // duration -- would describe a stream that is not playing. Reporting the
        // stream as lost is what makes the caller reopen instead of carrying on
        // with a figure about the past.
        rateChanged_.store(true, std::memory_order_relaxed);
    }

    void onResetRequest() {
        // A reset request says the driver's buffer size or format changed
        // underneath the buffers we are filling. Same verdict, same reason.
        lost_.store(true, std::memory_order_relaxed);
    }

  private:
    std::unique_ptr<HostThread> host_;
    IASIO* driver_ = nullptr;
    std::vector<ASIOBufferInfo> buffers_;
    RenderCallback render_;
    uint32_t channels_ = 0;
    uint32_t bufferFrames_ = 0;
    uint32_t rate_ = 0;
    uint32_t bytesPerFrame_ = 0;
    std::vector<uint8_t> scratch_;

    std::atomic<bool> running_{false};
    std::atomic<bool> lost_{false};
    std::atomic<bool> rateChanged_{false};
    std::atomic<bool> outputReady_{true};
};

/// The callbacks the driver is handed. `bufferSwitchTimeInfo` is null on purpose:
/// a driver that sees it non-null calls that instead, and this host has no use for
/// timestamps it would only have to convert.
ASIOCallbacks g_callbacks{
    [](long index, ASIOBool) {
        if (AsioStream* stream = g_activeStream.load(std::memory_order_acquire)) {
            stream->bufferSwitch(index);
        }
    },
    [](ASIOSampleRate) {
        if (AsioStream* stream = g_activeStream.load(std::memory_order_acquire)) {
            stream->onRateChanged();
        }
    },
    [](long selector, long value, void*, double*) -> long {
        if (AsioStream* stream = g_activeStream.load(std::memory_order_acquire)) {
            if (selector == kAsioResetRequest) stream->onResetRequest();
        }
        switch (selector) {
            case kAsioSelectorSupported:
                // ASIO 2.0 asks which of its own messages the host understands.
                return value == kAsioEngineVersion || value == kAsioResetRequest ||
                               value == kAsioBufferSizeChange || value == kAsioResyncRequest ||
                               value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo ||
                               value == kAsioSupportsTimeCode
                           ? 1
                           : 0;
            case kAsioEngineVersion: return 2;
            // Both answered "no": this host fills `bufferSwitch` and asks for
            // neither time code nor the time-info variant.
            case kAsioSupportsTimeInfo: return 0;
            case kAsioSupportsTimeCode: return 0;
            default: return 0;
        }
    },
    nullptr,
};

// ---------------------------------------------------------------------------
// Opening the driver.
// ---------------------------------------------------------------------------

struct DeviceOpen {
    std::unique_ptr<AsioStream> stream;
    OpenedFormat format;
};

/// The driver object for `clsid`, created in the apartment.
IASIO* createDriver(HostThread* host, const CLSID& clsid, std::string* error) {
    struct Created {
        IASIO* driver = nullptr;
        HRESULT hr = E_FAIL;
    };
    const Created created = host->call([&clsid] {
        Created out;
        // The CLSID is the interface id as well as the class id: that is the ASIO
        // host convention, and this driver answers E_NOINTERFACE for
        // IID_IUnknown. See the file header.
        IUnknown* unknown = nullptr;
        out.hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                  reinterpret_cast<void**>(&unknown));
        out.driver = reinterpret_cast<IASIO*>(unknown);
        return out;
    });
    if (!created.driver) {
        g_lastHr = created.hr;
        if (error) {
            char text[240];
            std::snprintf(text, sizeof(text),
                          "the ASIO driver could not be created: CoCreateInstance returned "
                          "0x%08lX (a driver registered only in the 32-bit view cannot be "
                          "loaded by this 64-bit process)",
                          static_cast<unsigned long>(created.hr));
            *error = text;
        }
        return nullptr;
    }
    return created.driver;
}

/// Initialises the driver, and reports its own reason when it will not.
///
/// `ASIOInit` returns no error code -- just `ASIOFalse` -- so the driver's error
/// text and the answer to `ASIOGetChannels` are what carry the reason. On the
/// Realtek driver this is how "no present device" arrives: `ASIOInit` returns false
/// with an empty message and every later call answers `ASE_NotPresent`.
bool initialiseDriver(HostThread* host, IASIO* driver, std::string* error) {
    const ASIOBool ok =
        host->call([driver, window = host->window()] { return driver->init(window); });
    if (ok == kAsioTrue) return true;

    char text[124] = {};
    host->call([driver, &text] {
        driver->getErrorMessage(text);
        return 0;
    });
    long inputs = 0;
    long outputs = 0;
    const ASIOError channels = host->call(
        [driver, &inputs, &outputs] { return driver->getChannels(&inputs, &outputs); });

    // The driver's own name and version, so the message identifies what refused
    // rather than only the registry entry that pointed at it.
    char name[64] = {};
    const long version = host->call([driver, &name] {
        driver->getDriverName(name);
        return driver->getDriverVersion();
    });

    if (error) {
        char identity[96];
        std::snprintf(identity, sizeof(identity), "the ASIO driver '%s' (version %ld) refused ASIOInit",
                      name[0] ? name : "(unnamed)", version);
        std::string message = identity;
        message += text[0] ? std::string(": ") + text : " (it offered no error text)";
        if (channels != kAseOk) {
            message += "; ASIOGetChannels then answered " + describe(channels) +
                       ", which is the driver reporting its device absent: disabled, "
                       "unplugged, or held by another program";
        }
        *error = message;
    }
    return false;
}

/// Opens the driver, negotiates a format, and creates its buffers.
///
/// The order is the driver's own: rate, then buffer size, then buffers. A rate the
/// driver will not take is refused here by name, and `nae.cpp` decides whether to
/// fall back -- the policy stays on that side of the interface.
DeviceOpen openDevice(const BackendOpenRequest& request, std::string* error) {
    DeviceOpen result;
    if (error) error->clear();

    CLSID clsid{};
    if (!clsidFromDeviceId(request.deviceId.c_str(), &clsid)) {
        if (error) *error = "the device id is not an ASIO id: expected \"asio:{CLSID}\"";
        return result;
    }
    const uint32_t wantChannels =
        static_cast<uint32_t>(request.resolved.channels > 0 ? request.resolved.channels : 2);
    const uint32_t wantRate =
        static_cast<uint32_t>(request.resolved.sampleRate > 0 ? request.resolved.sampleRate : 0);

    ClaimGuard claim;
    if (!claim.held()) {
        if (error) *error = "an ASIO stream is already open in this process";
        return result;
    }

    auto host = std::make_unique<HostThread>();
    if (!host->start(error)) return result;

    IASIO* driver = createDriver(host.get(), clsid, error);
    if (!driver) {
        host->stop();
        return result;
    }

    // Every early return past this point releases the driver in its own apartment
    // and then tears the apartment down; a leaked apartment is a leaked thread and
    // a driver object nobody can call again. The guard gives up the claim too.
    auto abandon = [&host, driver] {
        host->call([driver] {
            driver->Release();
            return 0;
        });
        host->stop();
    };

    if (!initialiseDriver(host.get(), driver, error)) {
        abandon();
        return result;
    }

    long inputs = 0;
    long outputs = 0;
    ASIOError code =
        host->call([driver, &inputs, &outputs] { return driver->getChannels(&inputs, &outputs); });
    (void)inputs;  // the input count is the driver's; this backend renders only
    if (code != kAseOk) {
        if (error) *error = "ASIOGetChannels answered " + describe(code);
        abandon();
        return result;
    }
    if (outputs <= 0) {
        if (error) *error = "the ASIO driver reports no output channels";
        abandon();
        return result;
    }
    const uint32_t channels = std::min<uint32_t>(wantChannels, static_cast<uint32_t>(outputs));

    // Rate. `canSampleRate` is the driver's own answer, so a request is refused by
    // the device rather than by a table here.
    double granted = 0.0;
    host->call([driver, &granted] {
        driver->getSampleRate(&granted);
        return 0;
    });
    if (wantRate > 0) {
        const ASIOError can =
            host->call([driver, wantRate] { return driver->canSampleRate(wantRate); });
        if (can != kAseOk) {
            if (error) {
                char text[220];
                std::snprintf(text, sizeof(text),
                              "the ASIO driver refused %u Hz: ASIOCanSampleRate answered %s",
                              wantRate, describe(can).c_str());
                *error = text;
            }
            abandon();
            return result;
        }
        code = host->call(
            [driver, wantRate] { return driver->setSampleRate(static_cast<double>(wantRate)); });
        if (code != kAseOk) {
            if (error) {
                char text[220];
                std::snprintf(text, sizeof(text),
                              "the ASIO driver refused ASIOSetSampleRate(%u): %s", wantRate,
                              describe(code).c_str());
                *error = text;
            }
            abandon();
            return result;
        }
        granted = static_cast<double>(wantRate);
    }
    if (granted < 1.0) {
        if (error) *error = "the ASIO driver reports no sample rate";
        abandon();
        return result;
    }

    // Buffer size: the request when there is one, the driver's preference
    // otherwise, clamped and aligned to what the driver says it will take.
    long minFrames = 0;
    long maxFrames = 0;
    long preferredFrames = 0;
    long granularity = 0;
    code = host->call([driver, &minFrames, &maxFrames, &preferredFrames, &granularity] {
        return driver->getBufferSize(&minFrames, &maxFrames, &preferredFrames, &granularity);
    });
    if (code != kAseOk) {
        if (error) *error = "ASIOGetBufferSize answered " + describe(code);
        abandon();
        return result;
    }
    long frames = request.bufferFrames > 0 ? static_cast<long>(request.bufferFrames)
                                           : (preferredFrames > 0 ? preferredFrames : minFrames);
    if (minFrames > 0 && frames < minFrames) frames = minFrames;
    if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;
    if (granularity > 1 && frames % granularity) frames += granularity - frames % granularity;
    if (frames <= 0) {
        if (error) *error = "the ASIO driver reports no usable buffer size";
        abandon();
        return result;
    }

    // The channel type is the driver's, not ours: `createBuffers` has no format
    // argument. What is checked is that it is a 32-bit integer container, because
    // that is the only thing this layer writes -- a float driver fed integer words
    // plays full-scale noise, so a refusal is the only honest answer.
    for (uint32_t channel = 0; channel < channels; ++channel) {
        ASIOChannelInfo info{};
        info.channel = static_cast<long>(channel);
        info.isInput = kAsioFalse;
        code = host->call([driver, &info] { return driver->getChannelInfo(&info); });
        if (code != kAseOk) {
            if (error) {
                char text[220];
                std::snprintf(text, sizeof(text), "ASIOGetChannelInfo(output %u) answered %s",
                              channel, describe(code).c_str());
                *error = text;
            }
            abandon();
            return result;
        }
        if (info.type != kAsioStInt32Lsb && info.type != kAsioStInt32Lsb24) {
            if (error) {
                char text[300];
                std::snprintf(text, sizeof(text),
                              "the driver reports %s for output channel %u; this backend moves "
                              "32-bit integer words (ASIOSTInt32LSB) only and does not fall back "
                              "to another type",
                              sampleTypeName(info.type), channel);
                *error = text;
            }
            abandon();
            return result;
        }
    }

    std::vector<ASIOBufferInfo> buffers(channels);
    for (uint32_t channel = 0; channel < channels; ++channel) {
        buffers[channel].isInput = kAsioFalse;
        buffers[channel].channelNum = static_cast<long>(channel);
        buffers[channel].buffers[0] = nullptr;
        buffers[channel].buffers[1] = nullptr;
    }

    code = host->call([driver, &buffers, channels, frames] {
        return driver->createBuffers(buffers.data(), static_cast<long>(channels), frames,
                                     &g_callbacks);
    });
    if (code != kAseOk) {
        if (error) {
            char text[240];
            std::snprintf(text, sizeof(text),
                          "ASIOCreateBuffers refused %u channels x %ld frames: %s", channels,
                          frames, describe(code).c_str());
            *error = text;
        }
        abandon();
        return result;
    }

    // The driver answered with the two halves it allocated. A null one would make
    // the callback write through nothing, so it is checked here rather than
    // dereferenced on the audio thread.
    for (uint32_t channel = 0; channel < channels; ++channel) {
        if (!buffers[channel].buffers[0] || !buffers[channel].buffers[1]) {
            if (error) {
                char text[180];
                std::snprintf(text, sizeof(text),
                              "ASIOCreateBuffers returned no buffer for output channel %u",
                              channel);
                *error = text;
            }
            host->call([driver] {
                driver->disposeBuffers();
                return kAseOk;
            });
            abandon();
            return result;
        }
    }

    const uint32_t rate = static_cast<uint32_t>(granted + 0.5);
    result.format.rate = rate;
    result.format.channels = channels;
    result.format.containerBits = 32;
    // The core's data carries 24 valid bits in that container; claiming 32 would
    // be a depth the rest of the pipeline does not have.
    result.format.validBits = 24;
    result.format.isFloat = false;
    result.format.bufferFrames = static_cast<uint32_t>(frames);
    result.format.latencyMs =
        rate ? (1000.0 * static_cast<double>(frames)) / static_cast<double>(rate) : 0.0;
    result.format.eventDriven = true;

    // The callback's scratch buffer, allocated here rather than in the
    // constructor: out of memory at this point still leaves `host` in our hands to
    // release the driver with.
    std::vector<uint8_t> scratch;
    try {
        scratch.assign(static_cast<size_t>(frames) * channels * 4, 0);
    } catch (...) {
        abandon();
        throw;
    }
    result.stream = std::make_unique<AsioStream>(std::move(host), driver, std::move(buffers),
                                                 std::move(scratch), channels,
                                                 static_cast<uint32_t>(frames), rate);
    // The request's render callback is the contract `BackendOpenRequest` documents,
    // so it is wired here as well as by `openAsioStream`, which installs a ring
    // drain in its place. A host that drives the `AudioBackend` interface directly
    // therefore plays the frames it asked for instead of silence.
    result.stream->setRender(request.render);
    // The callback finds the object here. Published after construction and before
    // the stream is handed out, so no callback can see a half-built stream.
    g_activeStream.store(result.stream.get(), std::memory_order_release);
    claim.commit();  // the stream releases the claim when it releases the driver
    return result;
}

// ---------------------------------------------------------------------------
// The ring the render loop drives.
//
// ASIO pulls from its callback while `nae.cpp`'s loop pushes buffers; the adapter
// between the two is the same shape `output_stream_coreaudio.cpp` uses, for the
// same reason: the loop decodes, resamples and dithers, none of which may run on an
// audio thread. The ring keeps that work where it is and leaves the callback with
// a copy.
//
// It is a second copy of that file's ring rather than a shared helper because the
// two files are never compiled together and the macOS one cannot be built here to
// prove a shared header still fits it.
// ---------------------------------------------------------------------------

class AsioOutputStream final : public OutputStream {
  public:
    AsioOutputStream(uint32_t ringFramesHint, uint32_t channels, uint32_t bytesPerFrame,
                     uint32_t rate)
        : ringFrames_([ringFramesHint] {
              uint32_t frames = 1;
              while (frames < ringFramesHint) frames <<= 1;  // the wrap is a mask on the audio thread
              return frames;
          }()),
          mask_(ringFrames_ - 1),
          ring_(static_cast<size_t>(ringFrames_) * bytesPerFrame, 0),
          channels_(channels),
          bytesPerFrame_(bytesPerFrame),
          rate_(rate) {}

    void attach(std::unique_ptr<AudioBackend> backend, std::unique_ptr<BackendStream> stream) {
        backend_ = std::move(backend);
        stream_ = std::move(stream);
    }

    uint32_t capacityFrames() const override { return ringFrames_; }
    uint32_t bytesPerFrame() const override { return bytesPerFrame_; }
    uint32_t channels() const override { return channels_; }
    uint32_t rate() const override { return rate_; }
    double latencyMs() const override { return stream_ ? stream_->latencyMs() : 0.0; }
    double driverLatencyMs() const override {
        // Read per call rather than cached: an ASIO driver may change its own
        // buffering when it renegotiates, and a cached figure would be a number
        // about the past. Zero when the driver cannot answer, never a guess.
        auto* asio = dynamic_cast<AsioStream*>(stream_.get());
        return asio ? asio->driverLatencyMs() : 0.0;
    }
    int64_t framesWritten() const override { return pulled_.load(std::memory_order_relaxed); }
    bool running() const override { return running_.load(std::memory_order_relaxed); }
    uint64_t underruns() const override { return underruns_.load(std::memory_order_relaxed); }
    const char* lastError() const override { return error_.c_str(); }

    /// Frames the ring still holds: the audio the loop has queued and the listener
    /// has not heard. The loop subtracts this from `capacityFrames()` to get the
    /// room it may fill.
    uint32_t queuedFrames() const override {
        return static_cast<uint32_t>(writePos_.load(std::memory_order_relaxed) -
                                     readPos_.load(std::memory_order_acquire));
    }

    StreamStatus acquire(uint32_t want, uint8_t** data, uint32_t* frames) override {
        if (!data || !frames) return StreamStatus::Error;
        *data = nullptr;
        *frames = 0;
        if (stream_ && stream_->deviceLost()) {
            error_ = "the ASIO driver reported this stream no longer valid";
            return StreamStatus::DeviceLost;
        }
        if (want > ringFrames_) return StreamStatus::TooLarge;
        const uint64_t write = writePos_.load(std::memory_order_relaxed);
        const uint64_t read = readPos_.load(std::memory_order_acquire);
        const uint64_t freeFrames = ringFrames_ - (write - read);
        if (freeFrames == 0) return StreamStatus::NoRoom;
        // Only the contiguous run to the end of the ring; a caller wanting more
        // asks again after committing. That keeps the window one span.
        const uint32_t offset = static_cast<uint32_t>(write & mask_);
        const uint32_t toEnd = ringFrames_ - offset;
        const uint32_t n =
            static_cast<uint32_t>(std::min<uint64_t>(std::min<uint32_t>(want, toEnd), freeFrames));
        *data = ring_.data() + static_cast<size_t>(offset) * bytesPerFrame_;
        *frames = n;
        return StreamStatus::Ok;
    }

    StreamStatus commit(uint32_t frames, bool silent) override {
        if (frames == 0) return StreamStatus::Ok;
        const uint64_t write = writePos_.load(std::memory_order_relaxed);
        if (silent) {
            const uint32_t offset = static_cast<uint32_t>(write & mask_);
            std::memset(ring_.data() + static_cast<size_t>(offset) * bytesPerFrame_, 0,
                        static_cast<size_t>(frames) * bytesPerFrame_);
        }
        writePos_.store(write + frames, std::memory_order_release);
        room_.notify_one();
        return StreamStatus::Ok;
    }

    bool waitForRoom(uint32_t timeoutMs) override {
        std::unique_lock<std::mutex> lock(mutex_);
        return room_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
            const uint64_t write = writePos_.load(std::memory_order_acquire);
            const uint64_t read = readPos_.load(std::memory_order_acquire);
            return ringFrames_ - (write - read) > 0;
        });
    }

    StreamStatus start() override {
        if (!stream_) return StreamStatus::Error;
        std::string error;
        if (!stream_->start(&error)) {
            error_ = error.empty() ? "the ASIO driver refused ASIOStart" : error;
            return StreamStatus::Error;
        }
        running_.store(true, std::memory_order_relaxed);
        return StreamStatus::Ok;
    }

    StreamStatus stop() override {
        if (stream_) stream_->stop();
        running_.store(false, std::memory_order_relaxed);
        return StreamStatus::Ok;
    }

    /// The driver's thread, through the render callback the backend installed.
    size_t pullBytes(void* out, size_t frames) {
        const uint64_t read = readPos_.load(std::memory_order_relaxed);
        const uint64_t write = writePos_.load(std::memory_order_acquire);
        const uint64_t available = write - read;
        const size_t n = static_cast<size_t>(std::min<uint64_t>(frames, available));
        auto* destination = static_cast<uint8_t*>(out);
        if (n > 0) {
            const uint32_t offset = static_cast<uint32_t>(read & mask_);
            const size_t first = std::min<size_t>(n, ringFrames_ - offset);
            std::memcpy(destination, ring_.data() + static_cast<size_t>(offset) * bytesPerFrame_,
                        first * bytesPerFrame_);
            if (first < n) {
                std::memcpy(destination + first * bytesPerFrame_, ring_.data(),
                            (n - first) * bytesPerFrame_);
            }
            readPos_.store(read + n, std::memory_order_release);
        }
        if (n < frames) {
            // The ring ran dry: silence out, and the gap is counted here as well,
            // because this is the end that knows the device asked and got nothing.
            std::memset(destination + n * bytesPerFrame_, 0, (frames - n) * bytesPerFrame_);
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }
        pulled_.fetch_add(static_cast<int64_t>(n), std::memory_order_relaxed);
        room_.notify_one();
        return n;
    }

  private:
    std::unique_ptr<AudioBackend> backend_;
    std::unique_ptr<BackendStream> stream_;
    uint32_t ringFrames_ = 0;
    uint32_t mask_ = 0;
    std::vector<uint8_t> ring_;
    uint32_t channels_ = 0;
    uint32_t bytesPerFrame_ = 0;
    uint32_t rate_ = 0;

    std::atomic<uint64_t> writePos_{0};
    std::atomic<uint64_t> readPos_{0};
    std::atomic<int64_t> pulled_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::condition_variable room_;
    std::string error_;
};

/// Bumped on every successful ASIO caps query, so a host can tell a fresh answer
/// from a cached one. One counter per device family: this one belongs to the
/// drivers the registry lists.
std::atomic<uint32_t> g_asioCapsGeneration{1};

}  // namespace

// ---------------------------------------------------------------------------
// The interface `nae.cpp` and a host see.
// ---------------------------------------------------------------------------

bool isAsioDeviceId(const char* deviceId) {
    static constexpr char kPrefix[] = "asio:";
    if (!deviceId) return false;
    return std::strncmp(deviceId, kPrefix, sizeof(kPrefix) - 1) == 0;
}

std::vector<BackendDevice> enumerateDevices(std::string* error) {
    if (error) error->clear();
    std::vector<BackendDevice> devices;
    for (const RegisteredDriver& driver : registeredDrivers()) {
        BackendDevice device;
        device.id = deviceIdFor(driver.clsid);
        device.name = driver.name;
        // ASIO has no notion of a system default device. Naming one would be
        // choosing for the caller.
        device.isDefault = false;
        devices.push_back(std::move(device));
    }
    return devices;
}

/// The `AudioBackend` half: what a host asks about a device.
class AsioBackend final : public AudioBackend {
  public:
    const char* name() const override { return "asio"; }

    std::vector<BackendDevice> enumerate(std::string* error) override {
        return enumerateDevices(error);
    }

    bool queryCaps(const std::string& deviceId, int32_t shareMode, NAE_DeviceCaps* out,
                   std::string* error) override {
        return queryAsioCaps(deviceId.c_str(), shareMode, out, error) == NAE_OK;
    }

    std::unique_ptr<BackendStream> open(const BackendOpenRequest& request,
                                        std::string* error) override {
        return openDevice(request, error).stream;
    }

    void shutdown() override {}
};

int32_t queryAsioCaps(const char* deviceId, int32_t shareMode, NAE_DeviceCaps* out,
                      std::string* error) NAE_NOEXCEPT {
    if (error) error->clear();
    if (!out || out->structSize != sizeof(NAE_DeviceCaps)) return NAE_ERR_INVALID_ARGUMENT;

    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(*out);
    out->version = NAE_DEVICE_CAPS_VERSION;
    out->backend = NAE_BACKEND_ASIO;
    // ASIO has one mode. Reporting a shared one would let a host offer a choice
    // the backend cannot honour.
    out->shareMode = NAE_SHARE_MODE_EXCLUSIVE;
    out->exclusiveSupported = 1;
    out->sharedSupported = 0;
    out->supportedShareModes = 1u << NAE_SHARE_MODE_EXCLUSIVE;
    // Not `hogSupported`: that field means macOS's "nobody else may open it"
    // without the format guarantees Windows exclusive mode adds. ASIO is the
    // Windows kind -- the driver owns the device and converts nothing -- so it is
    // `exclusiveSupported` and only that.
    out->hogSupported = 0;
    // The registry entry is static: a driver does not come and go with a USB
    // cable. Whether its hardware is present is what `probed` answers.
    out->hotplug = 0;
    out->eventDriven = 1;
    (void)shareMode;

    CLSID clsid{};
    if (!clsidFromDeviceId(deviceId, &clsid)) {
        if (error) *error = "the device id is not an ASIO id: expected \"asio:{CLSID}\"";
        return NAE_ERR_INVALID_ARGUMENT;
    }

    std::snprintf(out->id, sizeof(out->id), "%s", deviceId);
    // The name comes from the registry key, which is the driver's own label.
    for (const RegisteredDriver& driver : registeredDrivers()) {
        if (deviceIdFor(driver.clsid) == deviceId) {
            std::snprintf(out->name, sizeof(out->name), "%s", driver.name.c_str());
            break;
        }
    }
    if (out->name[0] == '\0') std::snprintf(out->name, sizeof(out->name), "%s", deviceId);

    auto host = std::make_unique<HostThread>();
    if (!host->start(error)) return NAE_ERR_NO_DEVICE;

    IASIO* driver = createDriver(host.get(), clsid, error);
    if (!driver) return NAE_ERR_NO_DEVICE;

    if (!initialiseDriver(host.get(), driver, error)) {
        host->call([driver] {
            driver->Release();
            return 0;
        });
        host->stop();
        // The registry says the device exists; the driver says its hardware does
        // not. No capability is reported, because none was measured: `probed`
        // stays 0 and the caller reads the reason. That is what "say nothing when
        // the source cannot answer" means for a caps query.
        return NAE_ERR_NO_DEVICE;
    }

    long inputs = 0;
    long outputs = 0;
    ASIOError code =
        host->call([driver, &inputs, &outputs] { return driver->getChannels(&inputs, &outputs); });
    (void)inputs;
    if (code == kAseOk && outputs > 0) {
        out->probed |= NAE_CAPS_PROBED_NATIVE | NAE_CAPS_PROBED_FORMATS | NAE_CAPS_PROBED_LAYOUTS;
        out->nativeChannels = static_cast<uint32_t>(outputs);
        out->maxChannels = out->nativeChannels;
        out->nativeFormat = NAE_SAMPLE_FORMAT_S24_IN_32;
        out->nativeContainerBits = 32;
        out->nativeValidBits = 24;
        out->formatCount = 1;
        out->formats[0].format = NAE_SAMPLE_FORMAT_S24_IN_32;
        out->formats[0].containerBits = 32;
        out->formats[0].validBits = 24;
        switch (out->nativeChannels) {
            case 1: out->nativeChannelMask = NAE_LAYOUT_MONO; break;
            case 2: out->nativeChannelMask = NAE_LAYOUT_STEREO; break;
            case 6: out->nativeChannelMask = NAE_LAYOUT_5_1; break;
            case 8: out->nativeChannelMask = NAE_LAYOUT_7_1; break;
            default: out->nativeChannelMask = 0; break;
        }
        out->layouts[0] = out->nativeChannelMask;
        out->layoutCount = 1;

        double rate = 0.0;
        host->call([driver, &rate] {
            driver->getSampleRate(&rate);
            return 0;
        });
        out->nativeRate = static_cast<uint32_t>(rate + 0.5);

        long minFrames = 0;
        long maxFrames = 0;
        long preferredFrames = 0;
        long granularity = 0;
        if (host->call([driver, &minFrames, &maxFrames, &preferredFrames, &granularity] {
                return driver->getBufferSize(&minFrames, &maxFrames, &preferredFrames,
                                             &granularity);
            }) == kAseOk) {
            out->probed |= NAE_CAPS_PROBED_BUFFER;
            out->minBufferFrames = minFrames > 0 ? static_cast<uint32_t>(minFrames) : 0;
            out->maxBufferFrames = maxFrames > 0 ? static_cast<uint32_t>(maxFrames) : 0;
            out->defaultBufferFrames = preferredFrames > 0
                                           ? static_cast<uint32_t>(preferredFrames)
                                           : out->minBufferFrames;
        }

        // The rate list is the driver's own answer, one candidate at a time. The
        // candidates are the set the WASAPI probe uses; a rate nobody listed costs
        // nothing, because `canSampleRate` is the authority.
        static constexpr uint32_t kCandidates[] = {8000,   11025, 16000,  22050,  32000,  44100,
                                                   48000,  64000, 88200,  96000,  176400, 192000,
                                                   352800, 384000, 768000};
        std::vector<uint32_t> rates;
        for (const uint32_t candidate : kCandidates) {
            if (host->call([driver, candidate] {
                    return driver->canSampleRate(static_cast<double>(candidate));
                }) == kAseOk) {
                rates.push_back(candidate);
            }
        }
        if (!rates.empty()) {
            out->probed |= NAE_CAPS_PROBED_RATES;
            out->rateCount =
                static_cast<uint32_t>(std::min<size_t>(rates.size(), NAE_MAX_DEVICE_RATES));
            for (uint32_t i = 0; i < out->rateCount; ++i) out->rates[i] = rates[i];
        }

        long inputLatency = 0;
        long outputLatency = 0;
        if (host->call([driver, &inputLatency, &outputLatency] {
                return driver->getLatencies(&inputLatency, &outputLatency);
            }) == kAseOk &&
            out->nativeRate > 0) {
            out->reportedLatencyMs =
                1000.0 * static_cast<double>(outputLatency) / static_cast<double>(out->nativeRate);
        }
    }

    // Two capabilities that are *asked and unavailable*, not unasked. Both are
    // reported as probed with a zero, which is what lets policy tell the two apart.
    // DoP is a WASAPI transport here, and native DSD has no stream path in this
    // engine on any backend.
    out->probed |= NAE_CAPS_PROBED_DOP | NAE_CAPS_PROBED_NATIVE_DSD | NAE_CAPS_PROBED_SHARE;
    out->dopSupported = 0;
    out->dopCarrierCount = 0;
    out->nativeDsdSupported = 0;
    out->nativeDsdRateCount = 0;

    host->call([driver] {
        driver->Release();
        return 0;
    });
    host->stop();

    out->capsGeneration = ++g_asioCapsGeneration;
    return NAE_OK;
}

std::shared_ptr<OutputStream> openAsioStream(const StreamFormatRequest& want, OpenedFormat* opened,
                                             std::string* error) {
    if (opened) *opened = OpenedFormat{};
    if (error) error->clear();

    if (want.channels == 0 || want.rate == 0) {
        if (error) *error = "the format request is incomplete";
        return nullptr;
    }
    if (want.isFloat || want.containerBits != 32) {
        // Named rather than quietly converted: a float request reaching an integer
        // driver is a wiring mistake upstream, and guessing which end is wrong
        // would hide it.
        char text[220];
        std::snprintf(text, sizeof(text),
                      "this backend moves 32-bit integer words; the request asked for %u-bit %s",
                      want.containerBits, want.isFloat ? "float" : "integer");
        if (error) *error = text;
        return nullptr;
    }

    BackendOpenRequest request;
    request.deviceId = want.deviceId;
    request.resolved.sampleRate = static_cast<int32_t>(want.rate);
    request.resolved.channels = static_cast<int32_t>(want.channels);
    request.resolved.containerBits = 32;
    request.resolved.validBits = 24;
    request.resolved.isFloat = 0;
    request.resolved.shareMode = NAE_SHARE_MODE_EXCLUSIVE;
    request.resolved.sampleFormat = NAE_SAMPLE_FORMAT_S24_IN_32;
    request.bufferFrames = want.bufferFrames;

    DeviceOpen device = openDevice(request, error);
    if (!device.stream) return nullptr;

    // Four device buffers of runway, rounded up to a power of two and never below
    // 2048 frames: the loop wakes on a timer, and a ring barely larger than one
    // period turns every late wake-up into a gap in the output.
    const uint32_t deviceFrames = device.format.bufferFrames;
    const uint32_t ringHint = std::max<uint32_t>(deviceFrames * 4u, 2048u);

    auto stream = std::make_shared<AsioOutputStream>(ringHint, device.format.channels,
                                                     device.format.channels * 4u,
                                                     device.format.rate);
    std::weak_ptr<AsioOutputStream> weak = stream;
    device.stream->setRender([weak](void* out, size_t frames) -> size_t {
        if (auto self = weak.lock()) return self->pullBytes(out, frames);
        return 0;
    });

    auto backend = std::make_unique<AsioBackend>();
    if (opened) {
        *opened = device.format;
        // The ring is what the loop fills, so its size is the buffer the engine
        // reports: `bufferFrames - queuedFrames()` has to be room in the ring, not
        // room in the driver's own buffer.
        opened->bufferFrames = stream->capacityFrames();
        opened->latencyMs = device.format.rate
                                ? (1000.0 * static_cast<double>(stream->capacityFrames())) /
                                      static_cast<double>(device.format.rate)
                                : 0.0;
        opened->eventDriven = true;
    }

    stream->attach(std::move(backend), std::move(device.stream));
    return stream;
}

}  // namespace neiro::asio
