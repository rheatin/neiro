// Measures what the playback pipeline costs in latency, and what it costs in CPU.
//
// # Why this exists
//
// The steady-state FIFO depth is the pipeline's own contribution to output
// latency: frames decoded and converted that the device has not played yet. The
// device's own buffer is reported separately by the engine, so this is the part
// that is *ours* to reduce.
//
// The depth is a consequence of policy, not a constant. Three things set it:
//
//   * the resampler's fixed input block -- it emits in whole blocks, so the FIFO
//     cannot hold less than one block's worth of output;
//   * the render loop's refill threshold -- how far the FIFO is topped up to;
//   * the `capacity == 0` behaviour -- whether the loop produces when the device
//     has no room, which is pure waste and ran the playhead at 122% of real time
//     until it was removed.
//
// "The FIFO is 11 000 frames" and "the FIFO is 11 000 frames and 7 000 of them are
// removable" are very different places to be, and only a measurement tells them
// apart.
//
// # What it reports
//
//   fifo      median steady-state FIFO depth, in frames and in ms
//   ratio     audio seconds per wall second; 1.000 is correct
//   short     calls that did not deliver their budget
//   drift     how far sourceAdvance * deviceRate / sourceRate misses deviceWritten
//
// `drift` is the correctness check. It is the invariant from NAE_PipelineStats,
// evaluated across the whole run: if the pipeline ever hands the device frames the
// source did not pay for, this is where it shows up.
//
// # Usage
//
//   latency_bench <file> [--label NAME] [--device ID] [--mode 0|1|2] [--rate N]
//                  [--quality 0|1|2] [--seconds N]

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nae.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

const int kSliceMs = 200;

double nowSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// CPU seconds consumed by *this thread*, not elapsed time.
//
// `std::clock()` looks like the obvious choice and is wrong here: on Windows it
// returns elapsed wall-clock time, so `clock_delta / wall` is 1.000 for an idle
// process and for a saturated one alike. Every "this burns 100% of a core"
// reading taken with it was meaningless, and it stayed meaningless across a
// ten-fold change in decode cost, which is what finally gave it away.
//
// GetThreadTimes is the real instrument: kernel plus user time for the calling
// thread, so a loop that sleeps reports near zero and a loop that spins reports
// near the wall clock.
double threadCpuSeconds() {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    // FILETIME is 100 ns ticks.
    return static_cast<double>(k.QuadPart + u.QuadPart) * 1e-7;
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}

std::string fmt(const char* f, ...) {
    char buf[256];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

const char* argValue(int argc, char** argv, const char* name, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return fallback;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Pulls one number out of the JSON by key. Deliberately not a parser: the shape is
// fixed and owned by this project's own C++ side, and a hand-rolled JSON reader
// here would be more code than the thing it reads.
double field(const std::string& json, const char* key, double fallback = 0.0) {
    const std::string needle = std::string("\"") + key + "\":";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return fallback;
    return std::strtod(json.c_str() + at + needle.size(), nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: latency_bench <file> [--label N] [--device ID] [--mode N] "
                    "[--rate N] [--quality N] [--seconds N]\n");
        return 2;
    }
    const char* path = argv[1];
    const char* label = argValue(argc, argv, "--label", "current");
    const char* deviceId = argValue(argc, argv, "--device", nullptr);
    const int32_t mode = std::atoi(argValue(argc, argv, "--mode", "1"));
    const int32_t fixedRate = std::atoi(argValue(argc, argv, "--rate", "0"));
    const int32_t quality = std::atoi(argValue(argc, argv, "--quality", "2"));
    const int wantSeconds = std::atoi(argValue(argc, argv, "--seconds", "6"));

    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, deviceLatencyMs = 0.0;

    const int32_t rc =
        NAE_OpenFile(deviceId, path, mode, fixedRate, quality, &h, &rate, &srcRate, &channels,
                     &bits, &valid, &isFloat, &resampling, &dithered, &bitPerfect, &fileChannels,
                     &durationMs, &deviceLatencyMs);
    if (rc != NAE_OK) {
        char err[256] = {0};
        NAE_GetLastErrorText(err, sizeof(err));
        std::printf("%-20s OPEN FAILED rc=%d  %s\n", label, rc, err);
        return 1;
    }

    // Five slices of warm-up. The opening slices prime the pipeline -- the FIFO
    // fills from empty and the resampler is still settling -- and including them
    // measures the start-up transient rather than what the player runs at.
    const int warmupSlices = 5;
    const int wantSlices = wantSeconds * 1000 / kSliceMs;
    const double durationSec = durationMs / 1000.0;

    // A benchmark is only a benchmark over enough slices. Asked for 6 s on a 1 s
    // file, the loop below breaks at the end of the source and the medians are
    // computed over whatever ran -- numbers that look like a measurement and are
    // an artefact of the fixture. Say so instead.
    if (durationSec > 0.0 && durationSec < wantSeconds + 0.1) {
        std::fprintf(stderr,
                     "fixture too short: %.1f s of audio, but %d s of playback were "
                     "requested. Pass --seconds %d or use a longer file.\n",
                     durationSec, wantSeconds, static_cast<int>(durationSec));
        NAE_Close(h);
        return 2;
    }

    std::vector<double> fifoDepths;
    int shortRenders = 0;
    int slices = 0;
    double totalSourceAdvance = 0.0;
    double totalDeviceWritten = 0.0;

    // CPU time of this process, so the wall-clock ratio can be read against how
    // much work it took. A configuration that halves the latency by doubling the
    // CPU is not obviously a win, and the two numbers together are what say so.
    const double cpuStart = threadCpuSeconds();

    const double wallStart = nowSeconds();
    for (int i = 0; i < warmupSlices + wantSlices; ++i) {
        if (NAE_Play(h, kSliceMs) != NAE_OK) ++shortRenders;
        ++slices;

        char* raw = NAE_PipelineStats(h);
        if (raw) {
            const std::string json(raw);
            NAE_FreeString(raw);
            if (field(json, "shortRender") > 0.5) ++shortRenders;
            totalSourceAdvance += field(json, "sourceAdvance");
            totalDeviceWritten += field(json, "deviceWritten");
            if (i >= warmupSlices) fifoDepths.push_back(field(json, "fifoFrames"));
        }

        if (durationSec > 0.0 && NAE_Position(h) >= durationSec - 0.15) break;
    }
    const double wall = nowSeconds() - wallStart;
    const double cpu = threadCpuSeconds() - cpuStart;
    const double audio = NAE_Position(h);

    const double fifo = median(fifoDepths);
    const double ratio = wall > 0.0 ? audio / wall : 0.0;

    // Spread, not just the middle. The FIFO is a sawtooth of one resampler block,
    // so the median alone would hide whether a change made the peak deeper -- and
    // a deeper peak is what turns into an underrun on a machine with a busier
    // scheduler than this one.
    std::vector<double> sorted = fifoDepths;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) {
        if (sorted.empty()) return 0.0;
        size_t i = static_cast<size_t>(p * (sorted.size() - 1) + 0.5);
        return sorted[std::min(i, sorted.size() - 1)];
    };
    const double p5 = pct(0.05), p95 = pct(0.95);

    // The correctness invariant, evaluated over the whole run rather than per
    // slice: the source the pipeline consumed must account for the device frames
    // it wrote, at the declared rate ratio. If this drifts, the device is being
    // fed frames the source never paid for, whatever the FIFO says.
    const double expectedDevice = totalSourceAdvance * rate / (srcRate ? srcRate : 1);
    const double drift = totalDeviceWritten > 0.0
                             ? (expectedDevice - totalDeviceWritten) / totalDeviceWritten * 100.0
                             : 0.0;

    std::printf(
        "%-20s fifo med=%6.0f p5=%6.0f p95=%6.0f (%4.1f ms)  deviceBuf=%5.1f ms  ratio=%5.3f  "
        "cpu=%5.2fs/%5.2fs (%3.0f%%)  short=%d  drift=%+5.2f%%  %u->%u Hz\n",
        label, fifo, p5, p95, rate ? fifo * 1000.0 / rate : 0.0, deviceLatencyMs, ratio, cpu,
        wall, wall > 0.0 ? cpu / wall * 100.0 : 0.0, shortRenders, drift, srcRate, rate);

    NAE_Close(h);
    NAE_Shutdown();
    return 0;
}
