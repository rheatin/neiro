// Sustained-playback measurement.
//
// latency_bench answers "what is the steady state over a few seconds", which is
// the right question for latency and the wrong one for accumulation. Everything
// it reports -- FIFO median, drift, CPU -- is a summary that hides its own
// history: a FIFO creeping up by one frame per minute and a FIFO that jumped
// once during start-up produce nearly the same median, and a drift that grows
// linearly looks like a small constant bias until you subtract the two.
//
// So this samples instead of summarising. Every `--sample-ms` of wall time it
// prints one line, and each line carries the minimum, median and maximum FIFO
// over that interval rather than the median alone. Accumulation shows up in the
// maximum first, because a FIFO that grows a little each cycle still returns to
// the bottom of its sawtooth -- it is the peaks that creep.
//
// Drift is reported in seconds, not percent, and cumulatively. A percentage is
// unreadable at the scale that matters: +0.08% of ten minutes is 0.5 seconds of
// slip between the file position and the audio, which a listener notices as a
// drifting progress bar. Whether that 0.08% is a fixed offset or a rate error
// decides whether it is cosmetic or a bug, and only a time series can tell: a
// constant bias has a flat slope, a rate error has a straight one.
//
// Two things this deliberately does not do:
//
//   * It does not average the CPU over the whole run. Per-interval CPU is
//     printed, because a decoder that leaks work per sample shows up as a
//     climbing CPU column long before it shows up in a total.
//
//   * It does not judge the result. It prints the series and the fitted slopes;
//     the thresholds live in the pass/fail lines so that a number and the
//     standard applied to it are read together rather than one at a time.
//
// `std::clock()` is not used anywhere. On Windows it returns wall time, so
// cpu/wall is 1.000 for an idle process and for a saturated one alike.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nae.h"

#ifdef _WIN32
// psapi.h for PROCESS_MEMORY_COUNTERS, at PSAPI_VERSION 2.
//
// The version matters and is easy to miss: at version 1 this header declares
// GetProcessMemoryInfo, which lives in psapi.lib and would need another link
// dependency for one struct read. Version 2 declares K32GetProcessMemoryInfo,
// which kernel32 exports directly, so the whole thing costs a struct read and
// nothing to link.
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

const int kSliceMs = 200;

double nowSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// CPU seconds consumed by *this thread*. See latency_bench for why std::clock()
// is wrong here; the short version is that it makes an idle loop look saturated.
double threadCpuSeconds() {
#ifdef _WIN32
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) * 1e-7;
#else
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}

// Resident set size, in bytes.
//
// The number that matters for "does this leak" is not the private heap but what
// the process has actually committed and been charged for, and a decoder that
// accumulates a FIFO grows both. Read once per interval: GetProcessMemoryInfo
// walks the working set and is not free, and calling it per 200 ms slice would
// show up in the CPU column it is supposed to be auditing.
std::size_t residentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return pmc.WorkingSetSize;
#endif
    return 0;
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

double field(const std::string& json, const char* key, double fallback = 0.0) {
    const std::string needle = std::string("\"") + key + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string::npos) return fallback;
    return std::strtod(json.c_str() + at + needle.size(), nullptr);
}

// Least-squares slope of y against x, in units of y per second.
//
// The pass criterion is "slope near zero", so the units matter: a slope quoted
// per-sample is perSample * sampleCount per minute and the two are trivially
// confused. Reported per second so it can be read directly as frames/second.
struct Trend {
    double slope = 0.0;
    double intercept = 0.0;
    double first = 0.0;
    double last = 0.0;
    double residualSigma = 0.0;  // RMS of (observed - fitted), in y units
    int n = 0;
};

Trend fitTrend(const std::vector<double>& t, const std::vector<double>& y, std::size_t from) {
    Trend out;
    if (from >= t.size()) from = 0;
    const std::size_t count = t.size() - from;
    if (count < 2) return out;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::size_t i = from; i < t.size(); ++i) {
        sx += t[i];
        sy += y[i];
        sxx += t[i] * t[i];
        sxy += t[i] * y[i];
    }
    const double n2 = static_cast<double>(count);
    const double denom = n2 * sxx - sx * sx;
    if (std::fabs(denom) < 1e-12) return out;
    out.slope = (n2 * sxy - sx * sy) / denom;
    out.intercept = (sy - out.slope * sx) / n2;
    out.first = y[from];
    out.last = y[t.size() - 1];
    out.n = static_cast<int>(count);

    // How much the series wanders around its own fitted line. This is the number
    // a slope has to beat to mean anything, and it is the reason a fixed
    // threshold cannot be trusted on its own: a series that swings +/-2% will
    // hand back a small positive or negative slope depending on where the window
    // happens to start, and neither reading is a trend.
    double ss = 0.0;
    for (std::size_t i = from; i < t.size(); ++i) {
        const double resid = y[i] - (out.slope * t[i] + out.intercept);
        ss += resid * resid;
    }
    out.residualSigma = std::sqrt(ss / static_cast<double>(count));
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf(
            "usage: sustain_check <file> [--label N] [--device ID] [--mode N] [--seconds N]\n"
            "                     [--sample-ms N] [--trend-seconds N] [--overfeed N]\n"
            "\n"
            "  --seconds      how long to play. Default 600 (ten minutes).\n"
            "  --sample-ms    interval between printed lines. Default 30000.\n"
            "  --trend-seconds  window over which the growth slopes are fitted.\n"
            "                   Default the second half of the run, so early\n"
            "                   start-up transient is excluded from the verdict.\n"
            "  --overfeed     sets NEIRO_FIFO_OVERFEED for this process. Used only\n"
            "                   to prove the instrument can see a FIFO that grows.\n");
        return 2;
    }

    const char* path = argv[1];
    const char* label = argValue(argc, argv, "--label", "sustain");
    const char* deviceId = argValue(argc, argv, "--device", nullptr);
    const int32_t mode = std::atoi(argValue(argc, argv, "--mode", "1"));
    const int wantSeconds = std::atoi(argValue(argc, argv, "--seconds", "600"));
    const int sampleMs = std::atoi(argValue(argc, argv, "--sample-ms", "30000"));
    const int trendSeconds = std::atoi(argValue(argc, argv, "--trend-seconds", "0"));
    const int overfeed = std::atoi(argValue(argc, argv, "--overfeed", "0"));

    if (overfeed > 1) {
#ifdef _WIN32
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", overfeed);
        _putenv_s("NEIRO_FIFO_OVERFEED", buf);
        // The cap has to come off too, or the control measures the cap instead of
        // the growth detector. The ceiling is around 30 000 device frames and the
        // overfeed settles the fifo at 22 000-26 000, pressed against it -- the
        // canary is doing its job, and a fifo that is clamped flat is a fifo
        // whose growth has been removed.
        _putenv_s("NEIRO_FIFO_CAP_OFF", "1");
        std::printf("[sustain] NEGATIVE CONTROL: NEIRO_FIFO_OVERFEED=%d, "
                    "NEIRO_FIFO_CAP_OFF=1\n"
                    "[sustain] The FIFO is being fed faster than the device drains it, and\n"
                    "[sustain] the ceiling is lifted so the growth is actually reachable.\n"
                    "[sustain] A flat series from here would mean the instrument is blind.\n",
                    overfeed);
#endif
    }

    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, deviceLatencyMs = 0.0;

    const int32_t rc =
        NAE_OpenFile(deviceId, path, mode, 0, 2, &h, &rate, &srcRate, &channels, &bits, &valid,
                     &isFloat, &resampling, &dithered, &bitPerfect, &fileChannels, &durationMs,
                     &deviceLatencyMs);
    if (rc != NAE_OK) {
        char err[256] = {0};
        NAE_GetLastErrorText(err, sizeof(err));
        std::printf("[sustain] OPEN FAILED rc=%d  %s\n", rc, err);
        return 1;
    }

    const double durationSec = durationMs / 1000.0;
    const int warmupSlices = 5;
    const double ratio = srcRate > 0 ? static_cast<double>(rate) / srcRate : 1.0;

    std::printf("[sustain] %-10s  %u->%u Hz  %dch %d-bit  resample=%d  %.1f s of audio\n",
                label, srcRate, rate, channels, bits, resampling, durationSec);
    std::printf("[sustain] want %d s of wall time, sampled every %d ms\n", wantSeconds,
                sampleMs);
    if (overfeed <= 1)
        std::printf("[sustain] pass criteria: fifoMax slope ~0, rss slope ~0, short=0,\n"
                    "[sustain] drift slope ~0 (a flat offset is fine, a ramp is not)\n");
    std::printf("\n");

    std::printf("%5s %10s %8s %8s %8s %11s %10s %7s %6s\n", "t(s)", "fifoMin", "fifoMed",
                "fifoMax", "rss(MB)", "drift(s)", "cpu%", "short", "audio(s)");

    std::vector<double> tSec, fifoMax, rssMb, driftSec;
    std::vector<double> intervalFifo;
    double totalSourceAdvance = 0.0, totalDeviceWritten = 0.0;
    int shortRenders = 0, slices = 0, sampleCount = 0;
    double cpuAccum = 0.0;

    const double cpuStart = threadCpuSeconds();
    const double wallStart = nowSeconds();
    // Relative, like `wall` below. This started life as `wallStart`, which is
    // absolute -- a steady_clock reading near 85000 s -- so every comparison
    // against the relative `wall` was off by that constant and the sampling
    // branch never ran once. The tool reported "0 samples" and four PASS lines
    // rather than a number, which is the only reason it was noticed at all.
    double sampleStart = 0.0;
    double cpuAtSampleStart = cpuStart;
    double audioAtSampleStart = 0.0;

    const int totalSlices = wantSeconds * 1000 / kSliceMs;
    for (int i = 0; i < warmupSlices + totalSlices; ++i) {
        if (NAE_Play(h, kSliceMs) != NAE_OK) ++shortRenders;
        ++slices;

        char* raw = NAE_PipelineStats(h);
        if (raw) {
            const std::string json(raw);
            NAE_FreeString(raw);
            if (field(json, "shortRender") > 0.5) ++shortRenders;
            totalSourceAdvance += field(json, "sourceAdvance");
            totalDeviceWritten += field(json, "deviceWritten");
            if (i >= warmupSlices) intervalFifo.push_back(field(json, "fifoFrames"));
        }

        const double wall = nowSeconds() - wallStart;
        if (wall - sampleStart >= sampleMs / 1000.0) {
            const double elapsed = wall - sampleStart;
            const double cpu = threadCpuSeconds();
            const double intervalCpu = cpu - cpuAtSampleStart;

            // The invariant, expressed in seconds rather than frames: how much
            // audio the device has been given, minus how much the source
            // actually paid for at the declared rate ratio. A positive number is
            // audio that exists in the output but not in the file.
            const double paidFor = totalSourceAdvance * ratio;
            const double driftFrames = totalDeviceWritten - paidFor;
            const double driftS = rate > 0 ? driftFrames / rate : 0.0;
            const double rss = static_cast<double>(residentBytes()) / (1024.0 * 1024.0);
            const double audio = NAE_Position(h);

            std::vector<double> sorted = intervalFifo;
            std::sort(sorted.begin(), sorted.end());
            const double fMin = sorted.empty() ? 0.0 : sorted.front();
            const double fMax = sorted.empty() ? 0.0 : sorted.back();
            const double fMed = median(sorted);

            std::printf("%5.0f %10.0f %8.0f %8.0f %8.1f %+11.4f %9.1f%% %7d %10.2f\n", wall, fMin,
                        fMed, fMax, rss, driftS,
                        elapsed > 0 ? intervalCpu / elapsed * 100.0 : 0.0, shortRenders, audio);
            std::fflush(stdout);

            tSec.push_back(wall);
            fifoMax.push_back(fMax);
            rssMb.push_back(rss);
            driftSec.push_back(driftS);
            ++sampleCount;

            intervalFifo.clear();
            sampleStart = wall;
            cpuAtSampleStart = cpu;
            audioAtSampleStart = audio;
            cpuAccum = cpu - cpuStart;
        }

        if (durationSec > 0.0 && NAE_Position(h) >= durationSec - 0.15) {
            std::printf("[sustain] reached end of file at %.1f s wall (%.1f s requested)\n",
                        nowSeconds() - wallStart, static_cast<double>(wantSeconds));
            break;
        }
    }

    const double wall = nowSeconds() - wallStart;
    const double cpu = threadCpuSeconds() - cpuStart;

    // The trend window defaults to the back half. Start-up fills the FIFO from
    // empty and settles the resampler, and fitting through that transient
    // produces a slope that describes the first seconds and is then quoted as if
    // it described the run.
    const int trendFrom =
        trendSeconds > 0 ? sampleCount - std::max(2, trendSeconds * 1000 / sampleMs)
                         : sampleCount / 2;
    const std::size_t from = static_cast<std::size_t>(std::max(0, trendFrom));

    const Trend tf = fitTrend(tSec, fifoMax, from);
    const Trend tr = fitTrend(tSec, rssMb, from);
    const Trend td = fitTrend(tSec, driftSec, from);

    std::printf("\n[sustain] ran %.1f s wall, %d slices, %.1f s audio, %d samples\n", wall, slices,
                audioAtSampleStart, sampleCount);
    std::printf("[sustain] thread CPU %.2f s / %.2f s wall (%.1f%% of one core), short renders %d\n",
                cpu, wall, wall > 0 ? cpu / wall * 100.0 : 0.0, shortRenders);
    std::printf("[sustain] overall realtime ratio %.4f\n",
                audioAtSampleStart > 0 ? audioAtSampleStart / wall : 0.0);

    // Split-half slopes over the same window, used as a sign-agreement check: a
    // fit whose two halves point opposite ways is chasing a phase, not a trend.
    const std::size_t mid = from + (tSec.size() - from) / 2;
    const Trend th1 = fitTrend(tSec, fifoMax, from), th2 = fitTrend(tSec, fifoMax, mid);
    const Trend rh1 = fitTrend(tSec, rssMb, from), rh2 = fitTrend(tSec, rssMb, mid);
    const Trend dh1 = fitTrend(tSec, driftSec, from), dh2 = fitTrend(tSec, driftSec, mid);

    std::printf("\n[sustain] growth over the last %.0f s (%d samples from t=%.0f):\n",
                tSec.empty() ? 0.0 : tSec.back() - (from < tSec.size() ? tSec[from] : 0.0),
                tf.n, from < tSec.size() ? tSec[from] : 0.0);
    std::printf("[sustain]   fifoMax  %8.1f -> %8.1f frames   slope %+9.3f frames/s   "
                "scatter +/-%.0f   halves %+.3f / %+.3f\n",
                tf.first, tf.last, tf.slope, tf.residualSigma, th1.slope, th2.slope);
    std::printf("[sustain]   rss      %8.1f -> %8.1f MB      slope %+9.3f MB/s   "
                "scatter +/-%.2f   halves %+.4f / %+.4f\n",
                tr.first, tr.last, tr.slope, tr.residualSigma, rh1.slope, rh2.slope);
    std::printf("[sustain]   drift    %+8.4f -> %+8.4f s     slope %+9.5f s/s   "
                "scatter +/-%.4f   halves %+.5f / %+.5f\n",
                td.first, td.last, td.slope, td.residualSigma, dh1.slope, dh2.slope);

    // A slope is only evidence of growth if the change it predicts across the
    // window is bigger than the series' own wander.
    //
    // A fixed frames/second threshold cannot do this on its own, and the first
    // ten-minute run showed why: fifoMax came back at +0.059 frames/s against a
    // 0.05 limit, a FAIL, while the twenty samples sat between 3189 and 3261 --
    // a +/-2% sawtooth around 3200 whose highest point happened to be last. That
    // is not a trend, it is a phase. Comparing the predicted change against the
    // scatter is what separates the two, and it does not cost sensitivity: the
    // deliberate overfeed control grew at 42 frames/s, which predicts 2550 frames
    // over its window against a scatter of a few hundred, so it is still caught
    // by a wide margin.
    const double window = tSec.empty() ? 0.0 : tSec.back() - tSec[from];
    std::printf("\n[sustain] is each slope bigger than the series' own scatter?\n");
    // Each series is judged against its own scatter and its own halves.
    //
    // The first version of this took the halves as captured variables, which
    // meant the fifo's -- so the rss line reported "halves disagree in sign
    // (+0.008 / -0.040)" while printing the rss slope of +0.0006 MB/s. The
    // conclusion happened to survive because rss's own halves agreed, but a
    // verdict printed with the wrong evidence beside it is a verdict nobody can
    // check, and the RSS verdict was passing on a comparison that had not been
    // made. The halves are parameters now.
    auto verdict = [&](const char* name, const Trend& tr2, const Trend& h1, const Trend& h2,
                       double limit) {
        const double predicted = std::fabs(tr2.slope * window);
        const double sigma = tr2.residualSigma;
        // Two sigma, and the halves must at least agree in sign. A slope whose
        // two halves point opposite ways is a fit chasing a phase.
        const bool halvesDisagree = (tr2.n >= 4) && (h1.slope * h2.slope < 0.0);
        if (predicted < 2.0 * sigma) {
            std::printf("[sustain] [NO TREND] %-8s predicts %+.1f over %.0f s, under 2x the "
                        "scatter (%+.1f)\n",
                        name, tr2.slope * window, window, sigma);
            if (halvesDisagree)
                std::printf("[sustain]            and the two halves disagree in sign "
                            "(%+.3f / %+.3f), which is what a sawtooth does\n",
                            h1.slope, h2.slope);
            return true;
        }
        if (halvesDisagree) {
            std::printf("[sustain] [NO TREND] %-8s predicts %+.1f over %.0f s, but the "
                        "halves disagree in sign (%+.3f / %+.3f)\n",
                        name, tr2.slope * window, window, h1.slope, h2.slope);
            return true;
        }
        if (std::fabs(tr2.slope) > limit) {
            std::printf("[sustain] [FAIL]     %-8s grew %.3f/s (limit %.2f), predicting %+.1f "
                        "over the window against scatter %+.1f\n",
                        name, tr2.slope, limit, tr2.slope * window, sigma);
            return false;
        }
        std::printf("[sustain] [PASS]     %-8s slope %+.3f/s is a real trend but under the "
                    "%.2f limit\n", name, tr2.slope, limit);
        return true;
    };

    // Split-half slopes, over the same window, as a sign-agreement check.

    // A run that collected no series has measured nothing, and saying so is the
    // whole point of the tool.
    //
    // This case actually happened: a 20-second file was asked to play for 150
    // seconds, hit end-of-file at 19.8, and the tool printed four [PASS] lines
    // and exited 0. Every slope was a fit over zero points, which is not a flat
    // line but an absence of one -- and an absence of evidence reported as
    // evidence of absence is the failure this project keeps paying for. A run
    // that cannot produce a series fails, loudly, and says how many samples it
    // managed to collect.
    if (sampleCount < 3) {
        std::printf("\n[sustain] [FAIL] only %d samples collected; a slope fitted to "
                    "fewer than 3 points\n[sustain]        is not a measurement. The run "
                    "probably hit end-of-file\n[sustain]        early -- use a file at "
                    "least as long as --seconds.\n",
                    sampleCount);
        NAE_Close(h);
        return 1;
    }

    // Verdicts. A FIFO creeping up at 0.3 frames/s adds 180 frames over ten
    // minutes, which is under 6 ms at 192 kHz -- inaudible, but it is a leak
    // and it would be a 2-second FIFO in four hours. The point of naming a
    // threshold is that "it looks fine" is not a measurement.
    const double fifoLimit = 0.05;   // frames/s
    const double rssLimit = 0.05;    // MB/s
    const double driftLimit = 1e-4;  // s/s, i.e. 6 ms of slip per minute

    bool pass = true;
    if (!verdict("fifoMax", tf, th1, th2, fifoLimit)) pass = false;
    if (!verdict("rss", tr, rh1, rh2, rssLimit)) pass = false;
    if (!verdict("drift", td, dh1, dh2, driftLimit)) pass = false;
    if (shortRenders != 0) {
        std::printf("[sustain] [FAIL] %d short renders\n", shortRenders);
        pass = false;
    } else {
        std::printf("[sustain] [PASS] no short renders\n");
    }

    NAE_Close(h);
    return pass ? 0 : 1;
}
