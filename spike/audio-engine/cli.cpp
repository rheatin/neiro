// The CLI host. It is the same library the Rust binding uses; no UI, no
// separate engine path. Every transport command the session exposes is
// reachable here, so a feature that works from Rust cannot be CLI-only and
// vice versa.
//
// Control is a timeline (--commands "0.5:pause;1.5:seek 2;2.0:resume") rather
// than a stdin prompt. That keeps the process single-threaded and scriptable;
// an interactive front end can be added over the same calls later.

#include "nae.h"
#include "utf8_args.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* stateName(int32_t state) {
    switch (state) {
        case NAE_SESSION_IDLE: return "IDLE";
        case NAE_SESSION_READY: return "READY";
        case NAE_SESSION_PLAYING: return "PLAYING";
        case NAE_SESSION_PAUSED: return "PAUSED";
        case NAE_SESSION_DEVICE_LOST: return "DEVICE_LOST";
        case NAE_SESSION_ENDED: return "ENDED";
        case NAE_SESSION_ERROR: return "ERROR";
        default: return "?";
    }
}

bool getState(NAE_Session* session, NAE_SessionState* out) {
    out->structSize = sizeof(*out);
    return NAE_SessionGetState(session, out) == NAE_OK;
}

void printState(const NAE_SessionState& st, bool json) {
    if (json) {
        std::printf(
            "{\"state\":\"%s\",\"position\":%.3f,\"duration\":%.3f,\"rate\":%d,"
            "\"sourceRate\":%d,\"channels\":%d,\"bits\":%d,\"validBits\":%d,"
            "\"bitPerfect\":%d,\"reason\":%d,\"resampling\":%d,\"dithered\":%d,"
            "\"volume\":%.3f,\"eq\":%d,\"crossfeed\":%d,\"replaygain\":%d,"
            "\"rgTrackValid\":%d,\"rgAlbumValid\":%d,"
            "\"rgTrackGainDb\":%.2f,\"rgAlbumGainDb\":%.2f,"
            "\"rgAppliedGain\":%.4f,\"trackSerial\":%llu,\"convolver\":%d,"
            "\"crossfade\":%d}\n",
            stateName(st.state), st.position, st.duration, st.sampleRate, st.sourceRate,
            st.channels, st.bitsPerSample, st.validBits, st.bitPerfect, st.reason,
            st.resampling, st.dithered, st.volume, st.eqEnabled, st.crossfeedEnabled,
            st.replayGainEnabled, st.rgTrackValid, st.rgAlbumValid, st.rgTrackGainDb,
            st.rgAlbumGainDb, st.rgAppliedGain,
            static_cast<unsigned long long>(st.trackSerial), st.convolverEnabled,
            st.crossfadeEnabled);
    } else {
        std::printf("%-12s %8.2f / %8.2f s  %d -> %d Hz  %d ch  %d/%d-bit  bp=%d reason=%d vol=%.2f eq=%d cf=%d rg=%d\n",
                    stateName(st.state), st.position, st.duration, st.sourceRate, st.sampleRate,
                    st.channels, st.validBits, st.bitsPerSample, st.bitPerfect, st.reason,
                    st.volume, st.eqEnabled, st.crossfeedEnabled, st.replayGainEnabled);
        std::printf("             conv=%d xfade=%d\n", st.convolverEnabled,
                    st.crossfadeEnabled);
        std::printf("             track %llu\n", static_cast<unsigned long long>(st.trackSerial));
    }
}

struct TimedCommand {
    double at;
    std::string command;
};

bool parseCommands(const std::string& text, std::vector<TimedCommand>* out) {
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t sep = text.find(';', pos);
        const std::string item = text.substr(pos, sep == std::string::npos ? sep : sep - pos);
        const size_t colon = item.find(':');
        if (colon == std::string::npos) return false;
        TimedCommand tc;
        tc.at = std::atof(item.substr(0, colon).c_str());
        tc.command = item.substr(colon + 1);
        out->push_back(tc);
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    std::sort(out->begin(), out->end(),
              [](const TimedCommand& a, const TimedCommand& b) { return a.at < b.at; });
    return true;
}

void usage() {
    std::printf(
        "usage:\n"
        "  neiro_cli devices\n"
        "  neiro_cli caps\n"
        "  neiro_cli params\n"
        "  neiro_cli probe <file> [options]\n"
        "  neiro_cli play  <file> [options]\n"
        "  neiro_cli dop   <file.dsf|file.dff> [--device id] [--seconds N]\n"
        "\n"
        "options:\n"
        "  --device <id>        endpoint id; empty = the one used last\n"
        "  --mode native|device|fixed    default native\n"
        "  --rate <hz>          for fixed mode\n"
        "  --quality quick|high|very_high   default high\n"
        "  --shared             shared WASAPI instead of exclusive\n"
        "  --slice-ms <n>       4..200, default 16\n"
        "  --volume <0..1>      digital volume, default 1.0 (bypass)\n"
        "  --eq <freq:gain:q>   add a peaking band; repeat for more bands\n"
        "  --eq-preamp <db>     EQ headroom, -24..+24, default 0\n"
        "  --eq-on              enable the cascade (it is enabled by --eq too)\n"
        "  --crossfeed          enable stereo crossfeed\n"
        "  --crossfeed-amount <0..1>     mix, default 0.3\n"
        "  --crossfeed-delay-ms <ms>     delay, default 0.3\n"
        "  --crossfeed-cutoff-hz <hz>    low-pass, default 700\n"
        "  --replaygain off|track|album  ReplayGain policy, default off\n"
        "  --rg-preamp <db>              added to the tag gain, default 0\n"
        "  --rg-fallback <db>            used when the tag is absent, default 0\n"
        "  --rg-no-clip                  do not lower the gain for the peak\n"
        "  --queue <file>                gapless queue; repeat for more tracks\n"
        "  --convolver <ir.wav>          finite impulse response, source rate\n"
        "  --conv-wet <0..1>             wet mix, default 1\n"
        "  --conv-dry <0..1>             dry mix, default 0\n"
        "  --conv-gain <db>              wet gain, default 0\n"
        "  --conv-normalize              normalise the IR energy\n"
        "  --crossfade <ms>               mix the next queued track in, default 0\n"
        "  --crossfade-curve linear|equal_power\n"
        "  --duration <s>       stop after this many seconds of wall time\n"
        "  --commands <list>    e.g. \"0.5:pause;1.5:seek 2;2.0:resume;3.0:stop\"\n"
        "  --json               one JSON object per status line\n"
        "\n"
        "commands for --commands:\n"
        "  play pause resume stop seek <s> status volume <0..1>\n"
        "  get <name>  set <name>=<value>\n");
}

struct Options {
    std::string device;
    int32_t mode = NAE_OUTPUT_NATIVE;
    int32_t rate = 0;
    int32_t quality = NAE_QUALITY_HIGH;
    int32_t share = NAE_SHARE_EXCLUSIVE;
    int32_t sliceMs = 16;
    double duration = 0.0;
    double volume = 1.0;
    std::vector<NAE_EqBand> eqBands;
    double eqPreamp = 0.0;
    bool eqOn = false;
    bool crossfeedOn = false;
    double crossfeedAmount = 0.3;
    double crossfeedDelayMs = 0.3;
    double crossfeedCutoffHz = 700.0;
    int32_t rgMode = NAE_RG_OFF;
    double rgPreamp = 0.0;
    double rgFallback = 0.0;
    bool rgPreventClipping = true;
    std::vector<std::string> queuePaths;
    std::string convolverPath;
    double convWet = 1.0;
    double convDry = 0.0;
    double convGain = 0.0;
    bool convNormalize = false;
    double crossfadeMs = 0.0;
    int32_t crossfadeCurve = NAE_CROSSFADE_EQUAL_POWER;
    std::string commands;
    bool json = false;
};

bool parseOptions(const std::vector<std::string>& args, int start, Options* out) {
    for (int i = start; i < static_cast<int>(args.size()); ++i) {
        const std::string a = args[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= static_cast<int>(args.size())) {
                std::fprintf(stderr, "%s needs a value\n", what);
                return nullptr;
            }
            return args[++i].c_str();
        };
        if (a == "--device") {
            const char* v = need("--device");
            if (!v) return false;
            out->device = v;
        } else if (a == "--mode") {
            const char* v = need("--mode");
            if (!v) return false;
            if (!std::strcmp(v, "native")) out->mode = NAE_OUTPUT_NATIVE;
            else if (!std::strcmp(v, "device")) out->mode = NAE_OUTPUT_DEVICE;
            else if (!std::strcmp(v, "fixed")) out->mode = NAE_OUTPUT_FIXED;
            else return false;
        } else if (a == "--rate") {
            const char* v = need("--rate");
            if (!v) return false;
            out->rate = std::atoi(v);
        } else if (a == "--quality") {
            const char* v = need("--quality");
            if (!v) return false;
            if (!std::strcmp(v, "quick")) out->quality = NAE_QUALITY_FAST;
            else if (!std::strcmp(v, "high")) out->quality = NAE_QUALITY_HIGH;
            else if (!std::strcmp(v, "very_high")) out->quality = NAE_QUALITY_VERY_HIGH;
            else return false;
        } else if (a == "--slice-ms") {
            const char* v = need("--slice-ms");
            if (!v) return false;
            out->sliceMs = std::atoi(v);
        } else if (a == "--volume") {
            const char* v = need("--volume");
            if (!v) return false;
            out->volume = std::atof(v);
        } else if (a == "--eq") {
            const char* v = need("--eq");
            if (!v) return false;
            double freq = 0.0, gain = 0.0, q = 1.0;
            if (std::sscanf(v, "%lf:%lf:%lf", &freq, &gain, &q) != 3) {
                std::fprintf(stderr, "--eq needs freq:gain:q\n");
                return false;
            }
            if (out->eqBands.size() >= NAE_EQ_MAX_BANDS) {
                std::fprintf(stderr, "at most %d EQ bands\n", NAE_EQ_MAX_BANDS);
                return false;
            }
            NAE_EqBand band{};
            band.type = NAE_EQ_PEAKING;
            band.freqHz = freq;
            band.gainDb = gain;
            band.q = q;
            band.enabled = 1;
            out->eqBands.push_back(band);
        } else if (a == "--eq-preamp") {
            const char* v = need("--eq-preamp");
            if (!v) return false;
            out->eqPreamp = std::atof(v);
        } else if (a == "--eq-on") {
            out->eqOn = true;
        } else if (a == "--replaygain") {
            const char* v = need("--replaygain");
            if (!v) return false;
            if (!std::strcmp(v, "off")) out->rgMode = NAE_RG_OFF;
            else if (!std::strcmp(v, "track")) out->rgMode = NAE_RG_TRACK;
            else if (!std::strcmp(v, "album")) out->rgMode = NAE_RG_ALBUM;
            else return false;
        } else if (a == "--rg-preamp") {
            const char* v = need("--rg-preamp");
            if (!v) return false;
            out->rgPreamp = std::atof(v);
        } else if (a == "--rg-fallback") {
            const char* v = need("--rg-fallback");
            if (!v) return false;
            out->rgFallback = std::atof(v);
        } else if (a == "--rg-no-clip") {
            out->rgPreventClipping = false;
        } else if (a == "--crossfade") {
            const char* v = need("--crossfade");
            if (!v) return false;
            out->crossfadeMs = std::atof(v);
        } else if (a == "--crossfade-curve") {
            const char* v = need("--crossfade-curve");
            if (!v) return false;
            if (!std::strcmp(v, "linear")) out->crossfadeCurve = NAE_CROSSFADE_LINEAR;
            else if (!std::strcmp(v, "equal_power")) out->crossfadeCurve = NAE_CROSSFADE_EQUAL_POWER;
            else return false;
        } else if (a == "--convolver") {
            const char* v = need("--convolver");
            if (!v) return false;
            out->convolverPath = v;
        } else if (a == "--conv-wet") {
            const char* v = need("--conv-wet");
            if (!v) return false;
            out->convWet = std::atof(v);
        } else if (a == "--conv-dry") {
            const char* v = need("--conv-dry");
            if (!v) return false;
            out->convDry = std::atof(v);
        } else if (a == "--conv-gain") {
            const char* v = need("--conv-gain");
            if (!v) return false;
            out->convGain = std::atof(v);
        } else if (a == "--conv-normalize") {
            out->convNormalize = true;
        } else if (a == "--queue") {
            const char* v = need("--queue");
            if (!v) return false;
            out->queuePaths.emplace_back(v);
        } else if (a == "--crossfeed") {
            out->crossfeedOn = true;
        } else if (a == "--crossfeed-amount") {
            const char* v = need("--crossfeed-amount");
            if (!v) return false;
            out->crossfeedAmount = std::atof(v);
        } else if (a == "--crossfeed-delay-ms") {
            const char* v = need("--crossfeed-delay-ms");
            if (!v) return false;
            out->crossfeedDelayMs = std::atof(v);
        } else if (a == "--crossfeed-cutoff-hz") {
            const char* v = need("--crossfeed-cutoff-hz");
            if (!v) return false;
            out->crossfeedCutoffHz = std::atof(v);
        } else if (a == "--duration") {
            const char* v = need("--duration");
            if (!v) return false;
            out->duration = std::atof(v);
        } else if (a == "--commands") {
            const char* v = need("--commands");
            if (!v) return false;
            out->commands = v;
        } else if (a == "--shared") {
            out->share = NAE_SHARE_SHARED;
        } else if (a == "--json") {
            out->json = true;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        usage();
        return 1;
    }
    const std::string command = args[1];

    if (command == "help" || command == "--help" || command == "-h") {
        usage();
        return 0;
    }
    if (command == "caps") {
        std::printf("%s\n", NAE_GetEngineCapabilities());
        return 0;
    }
    if (command == "devices") {
        char* json = NAE_EnumerateDevices();
        if (!json) return 1;
        std::printf("%s\n", json);
        NAE_FreeString(json);
        return 0;
    }
    if (command == "params") {
        const bool json = argc > 2 && !std::strcmp(argv[2], "--json");
        const int32_t count = NAE_ParamCount();
        if (json) std::printf("[");
        for (int32_t i = 0; i < count; ++i) {
            NAE_ParamDesc d;
            d.structSize = sizeof(d);
            if (NAE_ParamAt(i, &d) != NAE_OK) continue;
            if (json) {
                std::printf("%s{\"name\":\"%s\",\"type\":%d,\"unit\":\"%s\","
                            "\"min\":%.6g,\"max\":%.6g,\"default\":%.6g,"
                            "\"flags\":%u,\"values\":\"%s\"}",
                            i ? "," : "", d.name, d.type, d.unit ? d.unit : "", d.minValue,
                            d.maxValue, d.defaultValue, d.flags,
                            d.values ? d.values : "");
            } else {
                std::printf("%-28s type=%d range=[%.6g, %.6g] default=%.6g flags=0x%x %s\n",
                            d.name, d.type, d.minValue, d.maxValue, d.defaultValue, d.flags,
                            d.values ? d.values : "");
            }
        }
        if (json) std::printf("]\n");
        return 0;
    }

    if (command == "dop") {
        if (args.size() < 3) {
            usage();
            return 1;
        }
        std::string device;
        double seconds = 0.0;
        for (size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--device" && i + 1 < args.size()) {
                device = args[++i];
            } else if (args[i] == "--seconds" && i + 1 < args.size()) {
                seconds = std::atof(args[++i].c_str());
            } else {
                std::fprintf(stderr, "unknown dop option: %s\n", args[i].c_str());
                return 1;
            }
        }
        NAE_Handle* dop = nullptr;
        int32_t carrier = 0, channels = 0, bits = 0;
        double latency = 0.0;
        const int32_t rc = NAE_OpenDop(device.empty() ? nullptr : device.c_str(),
                                       args[2].c_str(), &dop, &carrier, &channels, &bits,
                                       &latency);
        if (rc != NAE_OK || !dop) {
            char err[512] = {};
            NAE_GetLastErrorText(err, sizeof(err));
            std::fprintf(stderr, "DoP open failed: rc=%d hr=0x%08X %s\n", rc,
                         static_cast<unsigned>(NAE_GetLastHresult()), err);
            return 1;
        }
        std::printf("DoP: carrier %d Hz, %d ch, %d-bit, stream latency %.2f ms\n", carrier,
                    channels, bits, latency);
        int32_t playRc = NAE_OK;
        if (seconds > 0.0) {
            playRc = NAE_PlayDop(dop, static_cast<int32_t>(seconds * 1000.0));
        } else {
            const double duration = NAE_Duration(dop);
            while (NAE_Position(dop) < duration) {
                playRc = NAE_PlayDop(dop, 500);
                if (playRc != NAE_OK) break;
            }
        }
        NAE_Close(dop);
        if (playRc != NAE_OK) {
            char err[512] = {};
            NAE_GetLastErrorText(err, sizeof(err));
            std::fprintf(stderr, "DoP playback failed: rc=%d %s\n", playRc, err);
            return 1;
        }
        return 0;
    }

    if (command != "play" && command != "probe") {
        usage();
        return 1;
    }
    if (args.size() < 3) {
        usage();
        return 1;
    }

    Options options;
    if (!parseOptions(args, 3, &options)) return 1;

    NAE_SessionConfig config{};
    config.structSize = sizeof(config);
    config.deviceId = options.device.empty() ? nullptr : options.device.c_str();
    config.path = args[2].c_str();
    config.outputMode = options.mode;
    config.fixedRate = options.rate;
    config.quality = options.quality;
    config.shareMode = options.share;
    config.sliceMs = options.sliceMs;

    NAE_Session* session = nullptr;
    const int32_t openRc = NAE_SessionOpen(&config, &session);
    if (openRc != NAE_OK) {
        char err[512] = {};
        if (session) {
            NAE_SessionLastError(session, err, sizeof(err));
            NAE_SessionClose(session);
        }
        std::fprintf(stderr, "open failed: rc=%d %s\n", openRc, err);
        return 1;
    }

    for (const std::string& queued : options.queuePaths) {
        const int32_t queueRc = NAE_SessionQueueAdd(session, queued.c_str());
        if (queueRc != NAE_OK) {
            std::fprintf(stderr, "queue rejected %s: rc=%d\n", queued.c_str(), queueRc);
            NAE_SessionClose(session);
            return 1;
        }
    }

    if (options.volume != 1.0) {
        const int32_t volumeRc = NAE_SessionSetVolume(session, options.volume);
        if (volumeRc != NAE_OK) {
            std::fprintf(stderr, "volume rejected: rc=%d\n", volumeRc);
            NAE_SessionClose(session);
            return 1;
        }
    }

    if (options.rgMode != NAE_RG_OFF || options.rgPreamp != 0.0 ||
        options.rgFallback != 0.0) {
        NAE_ReplayGainParams rg;
        NAE_ReplayGainDefaults(&rg);
        rg.mode = options.rgMode;
        rg.preampDb = options.rgPreamp;
        rg.fallbackDb = options.rgFallback;
        rg.preventClipping = options.rgPreventClipping ? 1 : 0;
        const int32_t rgRc = NAE_SessionSetReplayGain(session, &rg);
        if (rgRc != NAE_OK) {
            std::fprintf(stderr, "ReplayGain rejected: rc=%d\n", rgRc);
            NAE_SessionClose(session);
            return 1;
        }
    }

    if (options.crossfadeMs > 0.0) {
        NAE_CrossfadeParams xf;
        NAE_CrossfadeDefaults(&xf);
        xf.enabled = 1;
        xf.durationMs = options.crossfadeMs;
        xf.curve = options.crossfadeCurve;
        const int32_t xfRc = NAE_SessionSetCrossfade(session, &xf);
        if (xfRc != NAE_OK) {
            std::fprintf(stderr, "crossfade rejected: rc=%d\n", xfRc);
            NAE_SessionClose(session);
            return 1;
        }
    }

    if (!options.convolverPath.empty()) {
        NAE_ConvolverParams conv;
        NAE_ConvolverDefaults(&conv);
        conv.enabled = 1;
        std::snprintf(conv.irPath, sizeof(conv.irPath), "%s", options.convolverPath.c_str());
        conv.wet = options.convWet;
        conv.dry = options.convDry;
        conv.gainDb = options.convGain;
        conv.normalize = options.convNormalize ? 1 : 0;
        const int32_t convRc = NAE_SessionSetConvolver(session, &conv);
        if (convRc != NAE_OK) {
            char err[512] = {};
            NAE_SessionLastError(session, err, sizeof(err));
            std::fprintf(stderr, "convolver rejected: rc=%d %s\n", convRc, err);
            NAE_SessionClose(session);
            return 1;
        }
    }

    if (options.crossfeedOn) {
        NAE_CrossfeedParams cf;
        NAE_CrossfeedDefaults(&cf);
        cf.enabled = 1;
        cf.amount = options.crossfeedAmount;
        cf.delayMs = options.crossfeedDelayMs;
        cf.cutoffHz = options.crossfeedCutoffHz;
        const int32_t cfRc = NAE_SessionSetCrossfeed(session, &cf);
        if (cfRc != NAE_OK) {
            std::fprintf(stderr, "crossfeed rejected: rc=%d\n", cfRc);
            NAE_SessionClose(session);
            return 1;
        }
    }

    if (options.eqOn || !options.eqBands.empty() || options.eqPreamp != 0.0) {
        NAE_EqParams eq;
        NAE_EqDefaults(&eq);
        eq.enabled = 1;
        eq.preampDb = options.eqPreamp;
        eq.bandCount = static_cast<int32_t>(options.eqBands.size());
        for (size_t i = 0; i < options.eqBands.size(); ++i) eq.bands[i] = options.eqBands[i];
        const int32_t eqRc = NAE_SessionSetEq(session, &eq);
        if (eqRc != NAE_OK) {
            std::fprintf(stderr, "EQ rejected: rc=%d\n", eqRc);
            NAE_SessionClose(session);
            return 1;
        }
    }

    NAE_SessionState st{};
    getState(session, &st);
    printState(st, options.json);

    if (command == "probe") {
        NAE_SessionClose(session);
        return 0;
    }

    std::vector<TimedCommand> timeline;
    if (!options.commands.empty() && !parseCommands(options.commands, &timeline)) {
        std::fprintf(stderr, "bad --commands: use seconds:command;seconds:command\n");
        NAE_SessionClose(session);
        return 1;
    }

    if (NAE_SessionPlay(session) != NAE_OK) {
        NAE_SessionClose(session);
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    size_t next = 0;
    int32_t lastState = -1;
    double lastPrint = -1.0;
    bool stop = false;

    while (!stop) {
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        while (next < timeline.size() && elapsed >= timeline[next].at) {
            const std::string& c = timeline[next].command;
            if (c == "play") {
                NAE_SessionPlay(session);
            } else if (c == "pause") {
                NAE_SessionPause(session);
            } else if (c == "resume") {
                NAE_SessionResume(session);
            } else if (c == "stop") {
                stop = true;
            } else if (c.rfind("seek ", 0) == 0) {
                NAE_SessionSeek(session, std::atof(c.c_str() + 5));
            } else if (c.rfind("volume ", 0) == 0) {
                NAE_SessionSetVolume(session, std::atof(c.c_str() + 7));
            } else if (c.rfind("get ", 0) == 0) {
                double v = 0.0;
                const char* name = c.c_str() + 4;
                const int32_t rc = NAE_SessionParamGet(session, name, &v);
                if (rc == NAE_OK) {
                    std::printf("%s = %.6g\n", name, v);
                } else {
                    char text[512] = {};
                    NAE_SessionParamGetString(session, name, text, sizeof(text));
                    std::printf("%s = %s (rc=%d)\n", name, text, rc);
                }
            } else if (c.rfind("set ", 0) == 0) {
                const std::string assignment = c.substr(4);
                const size_t eq = assignment.find('=');
                if (eq == std::string::npos) {
                    std::fprintf(stderr, "set needs name=value\n");
                } else {
                    const std::string name = assignment.substr(0, eq);
                    const std::string value = assignment.substr(eq + 1);
                    int32_t rc;
                    if (name.size() >= 3 &&
                        name.compare(name.size() - 3, 3, ".ir") == 0) {
                        rc = NAE_SessionParamSetString(session, name.c_str(), value.c_str());
                    } else {
                        rc = NAE_SessionParamSet(session, name.c_str(), std::atof(value.c_str()));
                    }
                    if (rc != NAE_OK) {
                        std::fprintf(stderr, "set %s failed: rc=%d\n", name.c_str(), rc);
                    }
                }
            } else if (c == "status") {
                getState(session, &st);
                printState(st, options.json);
            } else {
                std::fprintf(stderr, "unknown command: %s\n", c.c_str());
            }
            ++next;
            if (stop) break;
        }
        if (stop) break;

        getState(session, &st);
        if (st.state != lastState || elapsed - lastPrint >= 1.0) {
            printState(st, options.json);
            lastState = st.state;
            lastPrint = elapsed;
        }
        if (st.state == NAE_SESSION_ENDED || st.state == NAE_SESSION_ERROR ||
            st.state == NAE_SESSION_DEVICE_LOST) {
            break;
        }
        if (options.duration > 0.0 && elapsed >= options.duration) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    NAE_SessionStop(session);
    NAE_SessionClose(session);
    return 0;
}
