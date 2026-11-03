// The engine's own playback loop.
//
// Everything else in this DLL is a render primitive: the caller decides when to
// call NAE_Play, so the caller's loop is the clock. This file moves that loop
// onto a thread the engine owns. Commands are posted as atomics, the state is
// read as a snapshot, and no host pointer is touched by the audio thread.
//
// Decode runs on its own producer thread and fills a source ring; this loop
// consumes it. See decodeThreadMain in nae.cpp.

#include "nae.h"
#include "decoder.h"
#include "utf8_file.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct NAE_Session {
    std::string deviceId;
    std::string path;
    int32_t outputMode = NAE_OUTPUT_NATIVE;
    int32_t fixedRate = 0;
    int32_t quality = NAE_QUALITY_HIGH;
    int32_t shareMode = NAE_SHARE_EXCLUSIVE;
    int32_t sliceMs = 16;

    NAE_Handle* handle = nullptr;  // owned by the session thread
    std::thread thread;

    std::atomic<int32_t> state{NAE_SESSION_IDLE};
    std::atomic<bool> quit{false};
    std::atomic<bool> pauseCmd{false};
    std::atomic<bool> resumeCmd{false};
    std::atomic<double> seekRequest{-1.0};
    std::atomic<double> volumeRequest{-1.0};
    std::atomic<double> volume{1.0};

    std::mutex eqMutex;
    NAE_EqParams eqParams{};
    std::atomic<bool> eqPending{false};
    std::atomic<int32_t> eqEnabled{0};

    std::mutex crossfeedMutex;
    NAE_CrossfeedParams crossfeedParams{};
    std::atomic<bool> crossfeedPending{false};
    std::atomic<int32_t> crossfeedEnabled{0};

    std::mutex rgMutex;
    NAE_ReplayGainParams rgParams{};
    std::atomic<bool> rgPending{false};
    std::atomic<int32_t> rgEnabled{0};
    std::atomic<int32_t> rgTrackValid{0};
    std::atomic<int32_t> rgAlbumValid{0};
    std::atomic<double> rgTrackGainDb{0.0};
    std::atomic<double> rgAlbumGainDb{0.0};
    std::atomic<double> rgApplied{1.0};

    std::mutex convMutex;
    NAE_ConvolverParams convParams{};
    std::atomic<bool> convPending{false};
    std::atomic<int32_t> convEnabled{0};

    std::mutex crossfadeMutex;
    NAE_CrossfadeParams crossfadeParams{};
    std::atomic<bool> crossfadePending{false};
    std::atomic<int32_t> crossfadeEnabled{0};

    std::mutex queueMutex;
    std::deque<std::string> queue;
    std::atomic<bool> queuePending{false};
    std::atomic<bool> clearNextPending{false};
    std::atomic<uint64_t> trackSerial{0};
    bool nextPreloaded = false;

    std::atomic<double> position{0.0};
    std::atomic<double> duration{0.0};
    std::atomic<int32_t> sampleRate{0};
    std::atomic<int32_t> sourceRate{0};
    std::atomic<int32_t> channels{0};
    std::atomic<int32_t> bitsPerSample{0};
    std::atomic<int32_t> validBits{0};
    std::atomic<int32_t> isFloat{0};
    std::atomic<int32_t> bitPerfect{0};
    std::atomic<int32_t> reason{0};
    std::atomic<int32_t> resampling{0};
    std::atomic<int32_t> dithered{0};
    std::atomic<int32_t> fileChannels{0};
    std::atomic<double> latencyMs{0.0};

    std::atomic<int32_t> openResult{NAE_OK};
    std::mutex openMutex;
    std::condition_variable openCv;
    bool openDone = false;

    std::mutex errMutex;
    std::string lastError;

    NAE_DeviceCaps caps{};
    NAE_OutputPolicy requestedPolicy{};
    NAE_OutputPolicy effectivePolicy{};
    NAE_RoutingMatrix routingMatrix{};
    bool routingActive = false;
    bool dopMode = false;
    bool dsdSourceFile = false;
    bool dsdFallback = false;
    bool nativeUnsupported = false;
    bool dsdExtractRequested = false;
    bool dsdExtracted = false;
    bool dsdDownrateRequested = false;
    int32_t downrateTarget = 0;
    bool downratedActual = false;
    int32_t dsdFileRate = 0;
    bool nativeDsdRequested = false;
    bool nativeDsdMode = false;
    bool allowPcmFallback = true;
    int32_t dsdCarrierRate = 0;
    int32_t dsdSourceRate = 0;
    NAE_ResolvedSignalPath resolved{};
    std::mutex resolvedMutex;
    bool resolvedValid = false;
};

namespace {

uint32_t bitCountForMask(uint32_t mask) {
    uint32_t n = 0;
    while (mask) {
        mask &= mask - 1;
        ++n;
    }
    return n;
}

bool isDsdFile(const char* path) {
    std::FILE* f = neiro::openUtf8(path, "rb");
    if (!f) return false;
    char magic[5] = {};
    std::fread(magic, 1, 4, f);
    std::fclose(f);
    return std::strcmp(magic, "DSD ") == 0 || std::strcmp(magic, "FRM8") == 0;
}

uint32_t maskForChannels(int32_t channels) {
    switch (channels) {
        case 1: return NAE_LAYOUT_MONO;
        case 2: return NAE_LAYOUT_STEREO;
        case 6: return NAE_LAYOUT_5_1;
        case 8: return NAE_LAYOUT_7_1;
        default: return 0;
    }
}

std::string jsonEscape(const char* text) {
    std::string out;
    for (const unsigned char c : std::string(text ? text : "")) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

void addResolvedDiag(NAE_ResolvedSignalPath* r, int32_t severity, int32_t code,
                     uint32_t field, int32_t requested, int32_t resolved,
                     uint32_t value, const char* text) {
    if (r->diagnosticCount >= NAE_MAX_DIAGNOSTICS) return;
    NAE_Diagnostic& d = r->diagnostics[r->diagnosticCount++];
    d.structSize = sizeof(d);
    d.severity = severity;
    d.code = code;
    d.field = field;
    d.requested = requested;
    d.resolved = resolved;
    d.value = value;
    std::snprintf(d.message, sizeof(d.message), "%s", text ? text : "");
}


std::string resolvedJson(const NAE_ResolvedSignalPath& r) {
    std::vector<std::string> fields;
    auto add = [&](const char* key, const std::string& value) {
        fields.push_back(std::string("\"") + key + "\":" + value);
    };
    auto num = [](auto v) { return std::to_string(v); };
    add("resolved", r.resolved ? "true" : "false");
    add("status", num(r.status));
    add("deviceId", "\"" + jsonEscape(r.deviceId) + "\"");
    add("deviceName", "\"" + jsonEscape(r.deviceName) + "\"");
    add("backend", num(r.backend));
    add("shareMode", num(r.shareMode));
    add("sourceRate", num(r.sourceRate));
    add("sourceChannels", num(r.sourceChannels));
    add("sourceChannelMask", num(r.sourceChannelMask));
    add("sampleRate", num(r.sampleRate));
    add("channels", num(r.channels));
    add("channelMask", num(r.channelMask));
    add("sampleFormat", num(r.sampleFormat));
    add("validBits", num(r.validBits));
    add("containerBits", num(r.containerBits));
    add("isFloat", r.isFloat ? "true" : "false");
    add("ratePolicy", num(r.ratePolicy));
    add("requestedRate", num(r.requestedRate));
    add("rateReason", num(r.rateReason));
    add("coerced", r.coerced ? "true" : "false");
    add("resamplerAlgorithm", num(r.resamplerAlgorithm));
    add("resamplerQuality", num(r.resamplerQuality));
    add("resampling", r.resampling ? "true" : "false");
    add("resampleRatio", num(r.resampleRatio));
    add("resampleReason", num(r.resampleReason));
    add("ditherMode", num(r.ditherMode));
    add("ditherActive", r.ditherActive ? "true" : "false");
    add("ditherBits", num(r.ditherBits));
    add("ditherReason", num(r.ditherReason));
    add("dsdModeRequested", num(r.dsdModeRequested));
    add("dsdModeResolved", num(r.dsdModeResolved));
    add("dsdTransportRequested", num(r.dsdTransportRequested));
    add("dsdTransportResolved", num(r.dsdTransportResolved));
    add("dsdRateResolved", num(r.dsdRateResolved));
    add("dsdCarrierRate", num(r.dsdCarrierRate));
    add("dsdPcmFallback", r.dsdPcmFallback ? "true" : "false");
    add("dsdDownrated", r.dsdDownrated ? "true" : "false");
    add("dsdExtracted", r.dsdExtracted ? "true" : "false");
    add("bufferMode", num(r.bufferMode));
    add("bufferFrames", num(r.bufferFrames));
    add("periodFrames", num(r.periodFrames));
    add("eventDriven", r.eventDriven ? "true" : "false");
    add("bufferLatencyMs", num(r.bufferLatencyMs));
    add("totalLatencyMs", num(r.totalLatencyMs));
    add("routingMode", num(r.routingMode));
    add("upmixPreset", num(r.upmixPreset));
    add("downmixPreset", num(r.downmixPreset));
    add("channelConversion", r.channelConversion ? "true" : "false");
    add("routingReason", num(r.routingReason));
    add("phaseShiftActive", r.phaseShiftActive ? "true" : "false");
    add("phaseFilterTaps", num(r.phaseFilterTaps));
    add("phaseFilterOrder", num(r.phaseFilterOrder));
    add("phaseLatencyFrames", num(r.phaseLatencyFrames));
    add("bassManagement", num(r.bassManagement));
    add("lfeMode", num(r.lfeMode));
    add("lfeCrossoverHz", num(r.lfeCrossoverHz));
    add("channelMode", num(r.channelMode));
    add("balance", num(r.balance));
    add("polarityMask", num(r.polarityMask));
    add("clipGuardMode", num(r.clipGuardMode));
    add("clipGuardActive", r.clipGuardActive ? "true" : "false");
    add("volume", num(r.volume));
    add("eqActive", r.eqActive ? "true" : "false");
    add("crossfeedActive", r.crossfeedActive ? "true" : "false");
    add("convolverActive", r.convolverActive ? "true" : "false");
    add("crossfadeActive", r.crossfadeActive ? "true" : "false");
    add("crossfadeDurationMs", num(r.crossfadeDurationMs));
    add("gapless", r.gapless ? "true" : "false");
    add("replayGainMode", num(r.replayGainMode));
    add("replayGainAppliedDb", num(r.replayGainAppliedDb));
    add("activeStages", num(r.activeStages));
    add("bypassedStages", num(r.bypassedStages));
    add("bitPerfect", r.bitPerfect ? "true" : "false");
    add("perfectReason", num(r.perfectReason));
    add("failure", "\"" + jsonEscape(r.failure) + "\"");

    std::string diagnostics = "[";
    for (uint32_t i = 0; i < r.diagnosticCount; ++i) {
        if (i) diagnostics += ",";
        const NAE_Diagnostic& d = r.diagnostics[i];
        diagnostics += "{\"severity\":" + num(d.severity) +
                       ",\"code\":" + num(d.code) +
                       ",\"field\":" + num(d.field) +
                       ",\"requested\":" + num(d.requested) +
                       ",\"resolved\":" + num(d.resolved) +
                       ",\"value\":" + num(d.value) +
                       ",\"message\":\"" + jsonEscape(d.message) + "\"}";
    }
    diagnostics += "]";
    fields.push_back("\"diagnostics\":" + diagnostics);

    std::string out = "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out += ",";
        out += fields[i];
    }
    out += "}";
    return out;
}

// Called on the session thread after open and after a gapless track change.
// It reports the path the engine actually has, not a policy request.
/// Re-read the end-to-end latency from the engine.
///
/// Every DSP stage that delays the signal (linear-phase EQ, convolver, clip
/// guard lookahead) changes this number, and it is read once at open otherwise:
/// a session that enables the linear-phase EQ mid-track would keep reporting
/// the pre-EQ figure.
void refreshLatency(NAE_Session* s) {
    if (!s || !s->handle) return;
    NAE_SignalPath applied{};
    applied.structSize = sizeof(applied);
    if (NAE_QuerySignalPath(s->handle, &applied) != NAE_OK) return;
    s->latencyMs = applied.totalLatencyMs;
    // The stored resolved path carries the same three numbers, and it is what a
    // host reads for a sync figure. Updating only the session field leaves the
    // published one at the open-time value.
    std::lock_guard<std::mutex> lock(s->resolvedMutex);
    if (!s->resolvedValid) return;
    s->resolved.bufferLatencyMs = applied.bufferLatencyMs;
    s->resolved.driverLatencyMs = applied.driverLatencyMs;
    s->resolved.totalLatencyMs = applied.totalLatencyMs;
}

void refreshResolved(NAE_Session* s) {
    if (!s || !s->handle) return;
    NAE_ResolvedSignalPath r{};
    r.structSize = sizeof(r);
    r.version = NAE_RESOLVED_SIGNAL_PATH_VERSION;
    r.resolved = 1;
    r.status = NAE_RESOLVE_OK;
    r.capsGeneration = s->caps.capsGeneration;
    r.backend = s->caps.backend ? s->caps.backend : NAE_BACKEND_WASAPI;
    r.shareMode = s->shareMode == NAE_SHARE_SHARED ? NAE_SHARE_MODE_SHARED
                                                   : NAE_SHARE_MODE_EXCLUSIVE;
    std::snprintf(r.deviceId, sizeof(r.deviceId), "%s", s->caps.id);
    std::snprintf(r.deviceName, sizeof(r.deviceName), "%s", s->caps.name);

    NAE_TrackInfo ti{};
    ti.structSize = sizeof(ti);
    NAE_QueryTrackInfo(s->handle, &ti);
    const int32_t sourceChannels = ti.channels ? ti.channels : s->fileChannels.load();
    const int32_t outputChannels = s->channels.load();
    r.sourceRate = static_cast<uint32_t>(ti.sampleRate ? ti.sampleRate
                                                        : s->sourceRate.load());
    r.sourceChannels = static_cast<uint32_t>(sourceChannels);
    r.sourceChannelMask = maskForChannels(sourceChannels);
    r.sourceFormat = NAE_SAMPLE_FORMAT_F32;
    r.sourceValidBits = 32;
    r.sourceContainerBits = 32;
    r.sampleRate = static_cast<uint32_t>(s->sampleRate.load());
    r.channels = static_cast<uint32_t>(outputChannels);
    r.channelMask = maskForChannels(outputChannels);
    r.sampleFormat = s->isFloat.load() ? NAE_SAMPLE_FORMAT_F32 : NAE_SAMPLE_FORMAT_S24_IN_32;
    r.validBits = static_cast<uint32_t>(s->validBits.load());
    r.containerBits = static_cast<uint32_t>(s->bitsPerSample.load());
    r.isFloat = s->isFloat.load();
    r.ratePolicy = s->outputMode == NAE_OUTPUT_NATIVE ? NAE_RATE_NATIVE
                     : s->outputMode == NAE_OUTPUT_FIXED ? NAE_RATE_FIXED
                                                         : NAE_RATE_DEVICE;
    r.requestedRate = static_cast<uint32_t>(r.sampleRate);
    r.rateReason = NAE_RESOLVE_REASON_DEVICE_NATIVE_RATE;
    r.resampling = s->resampling.load();
    r.resamplerAlgorithm = NAE_RESAMPLE_ALGORITHM_SOXR;
    r.resamplerQuality = s->quality == NAE_QUALITY_FAST ? NAE_RESAMPLE_QUALITY_FAST
                          : s->quality == NAE_QUALITY_VERY_HIGH
                              ? NAE_RESAMPLE_QUALITY_VERY_HIGH
                              : NAE_RESAMPLE_QUALITY_HIGH;
    r.resampleRatio = r.sourceRate ? static_cast<double>(r.sampleRate) / r.sourceRate : 1.0;
    r.resampleReason = r.resampling ? NAE_RESOLVE_REASON_RESAMPLE_REQUIRED
                                    : NAE_RESOLVE_REASON_NO_RESAMPLE;
    r.ditherMode = s->dithered.load() ? NAE_DITHER_TPDF : NAE_DITHER_OFF;
    r.ditherActive = s->dithered.load();
    r.ditherBits = r.validBits;
    r.ditherReason = r.ditherActive ? NAE_RESOLVE_REASON_DITHER_REQUIRED
                                    : NAE_RESOLVE_REASON_NO_DITHER;
    r.dsdModeResolved = NAE_DSD_MODE_PCM;
    r.dsdTransportResolved = NAE_DSD_TRANSPORT_NONE;
    r.dsdReason = NAE_RESOLVE_REASON_NONE;
    r.bufferMode = s->effectivePolicy.bufferMode;
    r.bufferFrames = s->caps.defaultBufferFrames;
    r.periodFrames = s->caps.minPeriodFrames;
    r.eventDriven = s->caps.eventDriven;
    // Three different numbers, from the one place that computes them. The
    // engine already separates the queued buffer from the driver's own
    // latency and adds the DSP stages; copying one value into all three made
    // two of them wrong and gave a sync path the same number three times.
    {
        NAE_SignalPath sp{};
        sp.structSize = sizeof(sp);
        if (NAE_QuerySignalPath(s->handle, &sp) == NAE_OK) {
            r.bufferLatencyMs = sp.bufferLatencyMs;
            r.driverLatencyMs = sp.driverLatencyMs;
            r.totalLatencyMs = sp.totalLatencyMs;
        }
    }
    r.bufferReason = s->effectivePolicy.bufferMode == NAE_BUFFER_LOW_LATENCY
                         ? NAE_RESOLVE_REASON_BUFFER_LOW_LATENCY
                         : s->effectivePolicy.bufferMode == NAE_BUFFER_FIXED
                               ? NAE_RESOLVE_REASON_BUFFER_FIXED
                               : NAE_RESOLVE_REASON_BUFFER_BACKEND_DEFAULT;
    r.routingMode = sourceChannels == outputChannels ? NAE_ROUTING_PASS_THROUGH
                     : sourceChannels > outputChannels ? NAE_ROUTING_DOWNMIX
                                                       : NAE_ROUTING_UPMIX;
    r.channelConversion = sourceChannels != outputChannels ? 1 : 0;
    r.routingReason = sourceChannels == outputChannels
                          ? NAE_RESOLVE_REASON_ROUTING_PASS_THROUGH
                          : sourceChannels > outputChannels
                              ? NAE_RESOLVE_REASON_ROUTING_DOWNMIX
                              : NAE_RESOLVE_REASON_ROUTING_UPMIX;
    r.channelMode = NAE_CHANNEL_MODE_NORMAL;
    r.bassManagement = s->routingActive ? s->routingMatrix.bassManagement : NAE_BASS_OFF;
    r.lfeMode = s->routingActive ? s->routingMatrix.lfeMode : NAE_LFE_SILENT;
    r.lfeCrossoverHz = s->routingActive ? s->routingMatrix.lfeCrossoverHz : 0.0;
    r.lfeGainDb = s->routingActive ? s->routingMatrix.lfeGainDb : 0.0;
    r.clipGuardMode = s->effectivePolicy.clipGuardMode;
    r.clipGuardActive = r.clipGuardMode == NAE_CLIP_GUARD_LIMITER ? 1 : 0;
    r.clipCeilingDbTp = s->effectivePolicy.clipCeilingDbTp;
    r.clipLookaheadMs = s->effectivePolicy.clipLookaheadMs;
    r.clipReason = r.clipGuardActive ? NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_ON
                                     : NAE_RESOLVE_REASON_CLIP_GUARD_AUTO_OFF;
    r.volume = s->volume.load();
    r.eqActive = s->eqEnabled.load();
    r.crossfeedActive = s->crossfeedEnabled.load();
    r.convolverActive = s->convEnabled.load();
    r.crossfadeActive = s->crossfadeEnabled.load();
    r.replayGainMode = s->rgEnabled.load() ? NAE_RG_TRACK : NAE_RG_OFF;
    r.replayGainAppliedDb = 20.0 * std::log10(std::max(1e-9, s->rgApplied.load()));
    r.replayGainReason = s->rgEnabled.load() ? NAE_RESOLVE_REASON_REPLAY_GAIN
                                             : NAE_RESOLVE_REASON_NONE;
    r.bitPerfect = s->bitPerfect.load();
    r.perfectReason = s->reason.load();

    r.sourceIsDsd = s->dsdSourceFile ? 1 : 0;
    r.sourceDsdRate = static_cast<uint32_t>(s->dsdFileRate > 0 ? s->dsdFileRate
                                                                : s->dsdSourceRate);
    if (s->dsdSourceFile && !s->dopMode) {
        r.dsdModeResolved = NAE_DSD_MODE_PCM;
        r.dsdPcmFallback = s->dsdFallback ? 1 : 0;
        r.dsdExtracted = s->dsdExtracted ? 1 : 0;
        if (s->dsdExtracted) {
            r.dsdReason = NAE_RESOLVE_REASON_DSD_EXTRACTED;
            addResolvedDiag(&r, NAE_DIAG_FALLBACK, NAE_DIAG_DSD_EXTRACT,
                            NAE_AUTO_FIELD_DSD_RATE, NAE_DSD_RATE_EXTRACT,
                            NAE_DSD_RATE_EXTRACT, r.sampleRate,
                            "DSD was extracted to PCM at the output rate.");
        } else if (s->dsdFallback) {
            r.dsdReason = NAE_RESOLVE_REASON_DSD_PCM_FALLBACK;
        }
    }
    if (s->nativeDsdMode) {
        r.sourceIsDsd = 1;
        r.sourceDsdRate = static_cast<uint32_t>(s->dsdFileRate > 0 ? s->dsdFileRate
                                                                : s->dsdSourceRate);
        r.sampleRate = static_cast<uint32_t>(s->dsdSourceRate);
        r.channels = 2;
        r.channelMask = NAE_LAYOUT_STEREO;
        r.sampleFormat = NAE_SAMPLE_FORMAT_DSD_U8;
        r.validBits = 1;
        r.containerBits = 8;
        r.isFloat = 0;
        r.dsdModeResolved = NAE_DSD_MODE_DSD;
        r.dsdTransportResolved = NAE_DSD_TRANSPORT_NATIVE;
        r.dsdRateResolved = static_cast<uint32_t>(s->dsdSourceRate);
        r.dsdCarrierRate = 0;
        r.dsdReason = NAE_RESOLVE_REASON_DSD_NATIVE;
        r.bitPerfect = s->bitPerfect.load() ? 1 : 0;
        r.perfectReason = s->reason.load();
        r.activeStages = NAE_STAGE_DSD_TRANSPORT;
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_AUTO_CHOICE,
                        NAE_AUTO_FIELD_DSD_TRANSPORT, NAE_DSD_TRANSPORT_NATIVE,
                        NAE_DSD_TRANSPORT_NATIVE, r.dsdRateResolved,
                        "Native DSD is transported without PCM conversion.");
    }
    if (s->dopMode) {
        r.sourceIsDsd = 1;
        r.sourceDsdRate = static_cast<uint32_t>(s->dsdFileRate > 0 ? s->dsdFileRate
                                                                : s->dsdSourceRate);
        r.sampleRate = static_cast<uint32_t>(s->dsdCarrierRate);
        r.channels = 2;
        r.channelMask = NAE_LAYOUT_STEREO;
        r.sampleFormat = NAE_SAMPLE_FORMAT_S24;
        r.validBits = 24;
        r.containerBits = 24;
        r.isFloat = 0;
        r.dsdModeResolved = NAE_DSD_MODE_DSD;
        r.dsdTransportResolved = NAE_DSD_TRANSPORT_DOP;
        r.dsdRateResolved = static_cast<uint32_t>(s->dsdSourceRate);
        r.dsdCarrierRate = static_cast<uint32_t>(s->dsdCarrierRate);
        r.dsdPcmFallback = 0;
        r.dsdDownrated = s->downratedActual ? 1 : 0;
        r.dsdExtracted = 0;
        r.dsdReason = s->downratedActual ? NAE_RESOLVE_REASON_DSD_DOWNRATED
                                         : NAE_RESOLVE_REASON_DSD_DOP;
        r.resampling = 0;
        r.ditherActive = 0;
        r.ditherMode = NAE_DITHER_OFF;
        r.routingMode = NAE_ROUTING_PASS_THROUGH;
        r.channelConversion = 0;
        r.bitPerfect = s->bitPerfect.load() ? 1 : 0;
        r.perfectReason = s->reason.load();
        r.activeStages = NAE_STAGE_DSD_TRANSPORT;
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_AUTO_CHOICE,
                        NAE_AUTO_FIELD_DSD_TRANSPORT, NAE_DSD_TRANSPORT_DOP,
                        NAE_DSD_TRANSPORT_DOP, r.dsdCarrierRate,
                        "DSD is transported with DoP; the PCM DSP chain is bypassed.");
    }
    if (s->dsdFallback) {
        addResolvedDiag(&r, NAE_DIAG_FALLBACK, NAE_DIAG_DSD_PCM_FALLBACK,
                        NAE_AUTO_FIELD_DSD_MODE, s->requestedPolicy.dsdMode,
                        NAE_DSD_MODE_PCM, 0,
                        "DSD playback fell back to PCM.");
    }
    if (s->downratedActual) {
        addResolvedDiag(&r, NAE_DIAG_FALLBACK, NAE_DIAG_DSD_DOWNRATE,
                        NAE_AUTO_FIELD_DSD_RATE, s->requestedPolicy.dsdRatePolicy,
                        NAE_DSD_RATE_DOWNRATE, r.dsdRateResolved,
                        "The DSD bitstream was decimated to a lower rate.");
    } else if (s->dsdDownrateRequested) {
        addResolvedDiag(&r, NAE_DIAG_WARNING, NAE_DIAG_DSD_DOWNRATE,
                        NAE_AUTO_FIELD_DSD_RATE, s->requestedPolicy.dsdRatePolicy,
                        NAE_DSD_RATE_FOLLOW_SOURCE, 0,
                        "No lower DSD rate with a supported carrier was available; the source rate is kept.");
    }
    if (s->nativeUnsupported) {
        addResolvedDiag(&r, NAE_DIAG_WARNING, NAE_DIAG_FAILURE,
                        NAE_AUTO_FIELD_DSD_TRANSPORT, NAE_DSD_TRANSPORT_NATIVE,
                        NAE_DSD_TRANSPORT_NONE, 0,
                        "Native DSD is not available on this backend.");
    }
    if (r.resampling) r.activeStages |= NAE_STAGE_RESAMPLE;
    if (r.ditherActive) r.activeStages |= NAE_STAGE_DITHER;
    if (r.channelConversion) r.activeStages |= NAE_STAGE_ROUTING;
    if (r.eqActive) r.activeStages |= NAE_STAGE_EQ;
    if (r.crossfeedActive) r.activeStages |= NAE_STAGE_CROSSFEED;
    if (r.convolverActive) r.activeStages |= NAE_STAGE_CONVOLVER;
    if (r.crossfadeActive) r.activeStages |= NAE_STAGE_CROSSFADE;
    if (r.replayGainReason != NAE_RESOLVE_REASON_NONE) r.activeStages |= NAE_STAGE_REPLAYGAIN;
    if (r.volume != 1.0) r.activeStages |= NAE_STAGE_VOLUME;

    if (r.resampling) {
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_RESAMPLER, NAE_AUTO_FIELD_RESAMPLER,
                        NAE_RESAMPLE_ALGORITHM_SOXR, NAE_RESAMPLE_ALGORITHM_SOXR,
                        r.sampleRate, "The output path resamples to the device rate.");
    }
    if (r.ditherActive) {
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_DITHER, NAE_AUTO_FIELD_DITHER,
                        NAE_DITHER_TPDF, NAE_DITHER_TPDF, r.validBits,
                        "TPDF dither is applied for the output bit depth.");
    }
    if (r.channelConversion) {
        addResolvedDiag(&r, NAE_DIAG_FALLBACK, NAE_DIAG_ROUTING_DOWNMIX,
                        NAE_AUTO_FIELD_ROUTING, NAE_ROUTING_AUTO, r.routingMode,
                        r.channels,
                        sourceChannels > outputChannels
                            ? "The decoder downmixes to the device channel count."
                            : "The decoder upmixes to the device channel count.");
    }
    if (r.eqActive) addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_DSP_BYPASSED,
                                    NAE_AUTO_FIELD_BIT_PERFECT, -1, r.perfectReason,
                                    0, "EQ is active.");
    if (r.crossfeedActive) addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_DSP_BYPASSED,
                                           NAE_AUTO_FIELD_BIT_PERFECT, -1, r.perfectReason,
                                           0, "Crossfeed is active.");
    if (r.convolverActive) addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_DSP_BYPASSED,
                                           NAE_AUTO_FIELD_BIT_PERFECT, -1, r.perfectReason,
                                           0, "The convolver is active.");
    if (r.crossfadeActive) addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_DSP_BYPASSED,
                                           NAE_AUTO_FIELD_BIT_PERFECT, -1, r.perfectReason,
                                           0, "Crossfade is active.");
    if (r.replayGainReason != NAE_RESOLVE_REASON_NONE) {
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_DSP_BYPASSED,
                        NAE_AUTO_FIELD_BIT_PERFECT, -1, r.perfectReason,
                        0, "ReplayGain is applying gain.");
    }
    if (!s->dopMode && s->requestedPolicy.dsdMode != NAE_DSD_MODE_AUTO &&
        s->requestedPolicy.dsdMode != NAE_DSD_MODE_PCM) {
        addResolvedDiag(&r, NAE_DIAG_WARNING, NAE_DIAG_FAILURE,
                        NAE_AUTO_FIELD_DSD_MODE, s->requestedPolicy.dsdMode,
                        NAE_DSD_MODE_PCM, 0,
                        "DSD policy is not applied by this session; the file is decoded to PCM.");
    }
    if (!s->routingActive && s->requestedPolicy.routingMode != NAE_ROUTING_AUTO &&
        s->requestedPolicy.routingMode != NAE_ROUTING_PASS_THROUGH) {
        addResolvedDiag(&r, NAE_DIAG_WARNING, NAE_DIAG_ROUTING_DOWNMIX,
                        NAE_AUTO_FIELD_ROUTING, s->requestedPolicy.routingMode,
                        r.routingMode, r.channels,
                        "Explicit routing policy is not applied; channel conversion is the decoder default.");
    }
    bool channelRequested = s->requestedPolicy.channelMode != NAE_CHANNEL_MODE_NORMAL ||
                            s->requestedPolicy.balance != 0.0 ||
                            s->requestedPolicy.polarityMask != 0;
    for (uint32_t i = 0; i < NAE_MAX_CHANNELS && !channelRequested; ++i) {
        if (s->requestedPolicy.delayFrames[i] != 0 || s->requestedPolicy.trimDb[i] != 0.0) {
            channelRequested = true;
        }
    }
    if (s->routingActive && s->routingMatrix.bassManagement == NAE_BASS_ON) {
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_BASS_MANAGEMENT,
                        NAE_AUTO_FIELD_ROUTING, s->requestedPolicy.bassManagement,
                        NAE_BASS_ON, static_cast<uint32_t>(s->routingMatrix.lfeCrossoverHz),
                        "Bass management derives the LFE from the routed sum.");
    }
    if (r.bitPerfect) {
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_BIT_PERFECT,
                        NAE_AUTO_FIELD_BIT_PERFECT, -1, -1, 0,
                        "The path is bit-perfect.");
    } else {
        char text[NAE_MAX_DIAGNOSTIC_MESSAGE];
        std::snprintf(text, sizeof(text), "The path is not bit-perfect (reason %d).",
                      r.perfectReason);
        addResolvedDiag(&r, NAE_DIAG_INFO, NAE_DIAG_BIT_PERFECT,
                        NAE_AUTO_FIELD_BIT_PERFECT, -1, r.perfectReason, 0, text);
    }

    std::lock_guard<std::mutex> lock(s->resolvedMutex);
    s->resolved = r;
    s->resolvedValid = true;
}

void setError(NAE_Session* s, int32_t rc) {
    char buf[512];
    const int32_t n = NAE_GetLastErrorText(buf, static_cast<int32_t>(sizeof(buf)));
    std::lock_guard<std::mutex> lock(s->errMutex);
    if (n > 0) {
        s->lastError.assign(buf, static_cast<size_t>(n));
    } else {
        s->lastError = "engine error " + std::to_string(rc);
    }
}

bool preloadNext(NAE_Session* s) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(s->queueMutex);
        if (s->queue.empty()) return true;
        path = s->queue.front();
    }
    const int32_t rc = NAE_PreloadNext(s->handle, path.c_str());
    if (rc != NAE_OK) {
        setError(s, rc);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s->queueMutex);
        if (!s->queue.empty() && s->queue.front() == path) s->queue.pop_front();
    }
    s->nextPreloaded = true;
    return true;
}

void sessionThread(NAE_Session* s) {
    NAE_Handle* handle = nullptr;
    int32_t rate = 0, sourceRate = 0, channels = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;

    // NAE_OpenFile treats null as "the system default"; it treats an empty
    // string as a device id and fails to resolve it. Keep the distinction.
    const char* device = s->deviceId.empty() ? nullptr : s->deviceId.c_str();

    int32_t rc = NAE_ERR_INITIALIZE;
    if (s->dsdExtractRequested) {
        rc = NAE_OpenFile(device, s->path.c_str(), s->outputMode, s->fixedRate,
                          s->quality, &handle, &rate, &sourceRate, &channels,
                          &bits, &valid, &isFloat, &resampling, &dithered,
                          &bitPerfect, &fileChannels, &durationMs, &latencyMs);
        if (rc == NAE_OK && handle) s->dsdExtracted = true;
    } else if (s->nativeDsdRequested) {
        int32_t dsdRate = 0, nch = 0, nbits = 0;
        double nlat = 0.0;
        const int32_t nativeRc = NAE_OpenNativeDsd(device, s->path.c_str(), &handle,
                                                   &dsdRate, &nch, &nbits, &nlat);
        if (nativeRc == NAE_OK && handle) {
            rc = NAE_OK;
            s->nativeDsdMode = true;
            rate = dsdRate;
            sourceRate = dsdRate;
            channels = nch;
            bits = nbits;
            valid = nbits;
            isFloat = 0;
            resampling = 0;
            dithered = 0;
            bitPerfect = 1;
            fileChannels = nch;
            durationMs = NAE_Duration(handle) * 1000.0;
            latencyMs = nlat;
            s->dsdCarrierRate = 0;
            s->dsdSourceRate = dsdRate;
        } else if (s->allowPcmFallback) {
            s->nativeDsdRequested = false;
            s->nativeUnsupported = true;
            s->dsdFallback = true;
            s->effectivePolicy.dsdMode = NAE_DSD_MODE_PCM;
            rc = NAE_OpenFile(device, s->path.c_str(), s->outputMode, s->fixedRate,
                              s->quality, &handle, &rate, &sourceRate, &channels,
                              &bits, &valid, &isFloat, &resampling, &dithered,
                              &bitPerfect, &fileChannels, &durationMs, &latencyMs);
        } else {
            rc = nativeRc;
        }
    } else if (s->dopMode) {
        int32_t carrier = 0, dch = 0, dbits = 0;
        double dlat = 0.0;
        int32_t dopRc;
        if (s->downrateTarget > 0 && s->downrateTarget < s->dsdSourceRate) {
            dopRc = NAE_OpenDopDownrated(device, s->path.c_str(), s->downrateTarget,
                                         &handle, &carrier, &dch, &dbits, &dlat);
        } else {
            dopRc = NAE_OpenDop(device, s->path.c_str(), &handle, &carrier, &dch,
                                &dbits, &dlat);
        }
        if (dopRc == NAE_OK && handle) {
            rc = NAE_OK;
            rate = carrier;
            sourceRate = carrier;
            channels = dch;
            bits = dbits;
            valid = dbits;
            isFloat = 0;
            resampling = 0;
            dithered = 0;
            bitPerfect = 1;
            fileChannels = dch;
            durationMs = NAE_Duration(handle) * 1000.0;
            latencyMs = dlat;
            s->dsdCarrierRate = carrier;
            s->dsdSourceRate = carrier * 16;
            if (s->dsdFileRate > 0 && s->dsdSourceRate < s->dsdFileRate) {
                s->downratedActual = true;
            }
        } else if (s->allowPcmFallback) {
            s->dopMode = false;
            s->dsdFallback = true;
            s->effectivePolicy.dsdMode = NAE_DSD_MODE_PCM;
            dopRc = NAE_OpenFile(device, s->path.c_str(), s->outputMode, s->fixedRate,
                                 s->quality, &handle, &rate, &sourceRate, &channels,
                                 &bits, &valid, &isFloat, &resampling, &dithered,
                                 &bitPerfect, &fileChannels, &durationMs, &latencyMs);
            rc = dopRc;
        } else {
            rc = dopRc;
        }
    } else if (s->nativeUnsupported && !s->allowPcmFallback) {
        rc = NAE_ERR_DEVICE_CANNOT_CARRY_FORMAT;
    } else if (s->shareMode == NAE_SHARE_SHARED) {
        rc = NAE_OpenFileShared(device, s->path.c_str(), s->outputMode,
                                s->fixedRate, s->quality, &handle, &rate, &sourceRate,
                                &channels, &bits, &valid, &isFloat, &resampling, &dithered,
                                &bitPerfect, &fileChannels, &durationMs, &latencyMs);
    } else if (s->routingActive) {
        rc = NAE_OpenFileWithRouting(device, s->path.c_str(), s->outputMode, s->fixedRate,
                                     s->quality, &s->routingMatrix, &handle, &rate,
                                     &sourceRate, &channels, &bits, &valid, &isFloat,
                                     &resampling, &dithered, &bitPerfect, &fileChannels,
                                     &durationMs, &latencyMs);
    } else {
        rc = NAE_OpenFileWithBuffer(device, s->path.c_str(), s->outputMode, s->fixedRate,
                                    s->quality, s->effectivePolicy.bufferMode,
                                    s->effectivePolicy.bufferMs, &handle, &rate, &sourceRate,
                                    &channels, &bits, &valid, &isFloat, &resampling,
                                    &dithered, &bitPerfect, &fileChannels, &durationMs,
                                    &latencyMs);
    }

    if (rc == NAE_OK && handle) {
        s->handle = handle;
        s->duration = durationMs > 0.0 ? durationMs / 1000.0 : NAE_Duration(handle);
        s->sampleRate = rate;
        s->sourceRate = sourceRate;
        s->channels = channels;
        s->bitsPerSample = bits;
        s->validBits = valid;
        s->isFloat = isFloat;
        s->resampling = resampling;
        s->dithered = dithered;
        s->bitPerfect = bitPerfect;
        s->fileChannels = fileChannels;
        s->latencyMs = latencyMs;

        NAE_SignalPath path{};
        path.structSize = sizeof(path);
        if (NAE_QuerySignalPath(handle, &path) == NAE_OK) {
            s->reason = path.reason;
            s->bitPerfect = path.bitPerfect;
        }
        {
            NAE_ReplayGainInfo info{};
            info.structSize = sizeof(info);
            if (NAE_QueryReplayGain(handle, &info) == NAE_OK) {
                s->rgTrackValid = info.trackValid;
                s->rgTrackGainDb = info.trackGainDb;
                s->rgAlbumValid = info.albumValid;
                s->rgAlbumGainDb = info.albumGainDb;
            }
        }
        if (!s->dopMode) {
            const NAE_OutputPolicy& p = s->effectivePolicy;
            NAE_SetBalance(s->handle, p.balance);
            NAE_SetPolarityMask(s->handle, p.polarityMask);
            if (p.clipGuardMode == NAE_CLIP_GUARD_LIMITER) {
                NAE_SetClipGuard(s->handle, 1, p.clipCeilingDbTp, p.clipLookaheadMs,
                                 p.clipReleaseMs);
            } else if (p.clipGuardMode == NAE_CLIP_GUARD_OFF) {
                NAE_SetClipGuard(s->handle, 0, 0.0, 0.0, 0.0);
            }
            NAE_SetChannelTrim(s->handle, p.trimDb, NAE_MAX_CHANNELS);
            std::vector<int32_t> delays(NAE_MAX_CHANNELS, 0);
            for (uint32_t i = 0; i < NAE_MAX_CHANNELS; ++i) {
                delays[i] = static_cast<int32_t>(p.delayFrames[i]);
            }
            NAE_SetChannelDelay(s->handle, delays.data(), NAE_MAX_CHANNELS);
            const int32_t mode = s->effectivePolicy.ditherMode;
            if (mode == NAE_DITHER_OFF || mode == NAE_DITHER_TPDF ||
                mode == NAE_DITHER_RECTANGULAR || mode == NAE_DITHER_NOISE_SHAPED) {
                const int32_t ditherRc = NAE_SetDitherMode(s->handle, mode);
                if (ditherRc != NAE_OK) setError(s, ditherRc);
            }
            // The DSP stages add delay; the session reports the end-to-end
            // figure, not only the device's.
            refreshLatency(s);
        }
        s->state = NAE_SESSION_READY;
        refreshResolved(s);
        if (!s->dopMode) preloadNext(s);
    } else {
        s->openResult = rc != NAE_OK ? rc : NAE_ERR_INITIALIZE;
        s->state = NAE_SESSION_ERROR;
        setError(s, s->openResult);
    }

    {
        std::lock_guard<std::mutex> lock(s->openMutex);
        s->openDone = true;
    }
    s->openCv.notify_all();

    if (!s->handle) {
        NAE_Shutdown();
        return;
    }

    // A track whose duration the container does not report ends when the source
    // stops advancing. 50 slices is about 0.8 s at the default, long enough that
    // a slow first decode cannot be mistaken for the end.
    int noAdvance = 0;

    while (!s->quit.load(std::memory_order_acquire)) {
        const double target = s->seekRequest.exchange(-1.0);
        if (target >= 0.0) {
            const int32_t seekRc = NAE_Seek(s->handle, target);
            if (seekRc != NAE_OK) setError(s, seekRc);
            s->position = NAE_Position(s->handle);
            noAdvance = 0;
        }

        if (s->clearNextPending.exchange(false)) {
            NAE_ClearNext(s->handle);
            s->nextPreloaded = false;
        }
        if (!s->nextPreloaded && s->queuePending.exchange(false)) {
            preloadNext(s);
        }

        if (s->crossfadePending.exchange(false)) {
            NAE_CrossfadeParams params;
            {
                std::lock_guard<std::mutex> lock(s->crossfadeMutex);
                params = s->crossfadeParams;
            }
            const int32_t fadeRc = NAE_SetCrossfade(s->handle, &params);
            if (fadeRc != NAE_OK) {
                setError(s, fadeRc);
            } else {
                s->crossfadeEnabled = params.enabled != 0;
            }
        }

        if (s->convPending.exchange(false)) {
            NAE_ConvolverParams params;
            {
                std::lock_guard<std::mutex> lock(s->convMutex);
                params = s->convParams;
            }
            const int32_t convRc = NAE_SetConvolver(s->handle, &params);
            if (convRc != NAE_OK) {
                setError(s, convRc);
            } else {
                s->convEnabled = params.enabled != 0;
                refreshLatency(s);
            }
        }

        if (s->rgPending.exchange(false)) {
            NAE_ReplayGainParams params;
            {
                std::lock_guard<std::mutex> lock(s->rgMutex);
                params = s->rgParams;
            }
            const int32_t rgRc = NAE_SetReplayGain(s->handle, &params);
            if (rgRc != NAE_OK) {
                setError(s, rgRc);
            } else {
                s->rgEnabled = params.mode != NAE_RG_OFF;
                s->rgApplied = NAE_GetReplayGainGain(s->handle);
            }
        }

        if (s->crossfeedPending.exchange(false)) {
            NAE_CrossfeedParams params;
            {
                std::lock_guard<std::mutex> lock(s->crossfeedMutex);
                params = s->crossfeedParams;
            }
            const int32_t cfRc = NAE_SetCrossfeed(s->handle, &params);
            if (cfRc != NAE_OK) {
                setError(s, cfRc);
            } else {
                s->crossfeedEnabled = params.enabled != 0;
                refreshLatency(s);
            }
        }

        if (s->eqPending.exchange(false)) {
            NAE_EqParams params;
            {
                std::lock_guard<std::mutex> lock(s->eqMutex);
                params = s->eqParams;
            }
            const int32_t eqRc = NAE_SetEq(s->handle, &params);
            if (eqRc != NAE_OK) {
                setError(s, eqRc);
            } else {
                s->eqEnabled = params.enabled != 0;
                // Linear phase adds taps/2 + a block of latency.
                refreshLatency(s);
            }
        }

        const double volume = s->volumeRequest.exchange(-1.0);
        if (volume >= 0.0) {
            const int32_t volumeRc = NAE_SetVolume(s->handle, volume);
            if (volumeRc != NAE_OK) {
                setError(s, volumeRc);
            } else {
                s->volume = volume;
            }
        }

        if (s->pauseCmd.exchange(false)) NAE_Pause(s->handle);
        if (s->resumeCmd.exchange(false)) NAE_Resume(s->handle);

        const int32_t st = s->state.load();
        if (st == NAE_SESSION_PLAYING || st == NAE_SESSION_PAUSED ||
            st == NAE_SESSION_ENDED) {
            const double before = s->position.load();
            const int32_t playRc = s->nativeDsdMode
                                       ? NAE_PlayNativeDsd(s->handle, s->sliceMs)
                                       : s->dopMode
                                             ? NAE_PlayDop(s->handle, s->sliceMs)
                                             : NAE_Play(s->handle, s->sliceMs);
            const double now = NAE_Position(s->handle);
            s->position = now;
            const uint64_t serial = NAE_GetTrackSerial(s->handle);
            if (serial != s->trackSerial.load()) {
                s->trackSerial = serial;
                s->nextPreloaded = false;
                s->duration = NAE_Duration(s->handle);
                NAE_ReplayGainInfo info{};
                info.structSize = sizeof(info);
                if (NAE_QueryReplayGain(s->handle, &info) == NAE_OK) {
                    s->rgTrackValid = info.trackValid;
                    s->rgTrackGainDb = info.trackGainDb;
                    s->rgAlbumValid = info.albumValid;
                    s->rgAlbumGainDb = info.albumGainDb;
                }
                s->queuePending = true;
                refreshResolved(s);
            }

            if (playRc == NAE_ERR_DEVICE_LOST) {
                s->state = NAE_SESSION_DEVICE_LOST;
                setError(s, playRc);
            } else if (playRc != NAE_OK) {
                s->state = NAE_SESSION_ERROR;
                setError(s, playRc);
            } else if (st == NAE_SESSION_PLAYING) {
                const double dur = s->duration.load();
                if (dur > 0.0 && now >= dur - 0.001) {
                    // Do not end the session while a preloaded file is waiting:
                    // the engine swaps it at the source boundary, and ending here
                    // would stop the stream before it can.
                    if (!s->nextPreloaded) {
                        s->state = NAE_SESSION_ENDED;
                        NAE_Pause(s->handle);
                    }
                } else if (now - before < 1e-9) {
                    if (++noAdvance > 50) {
                        s->state = NAE_SESSION_ENDED;
                        NAE_Pause(s->handle);
                    }
                } else {
                    noAdvance = 0;
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    NAE_Close(s->handle);
    s->handle = nullptr;
    NAE_Shutdown();
    s->state = NAE_SESSION_IDLE;
}

}  // namespace

extern "C" {

static int32_t sessionOpenImpl(const NAE_SessionConfig* config,
                               const NAE_OutputPolicy* requested,
                               bool explicitPolicy,
                               NAE_Session** out) NAE_NOEXCEPT {
    if (!out) return NAE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!config || config->structSize < sizeof(NAE_SessionConfig) || !config->path ||
        !config->path[0]) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

    const int32_t slice = config->sliceMs ? config->sliceMs : 16;
    if (slice < 4 || slice > 200) return NAE_ERR_INVALID_ARGUMENT;
    if (config->outputMode < NAE_OUTPUT_NATIVE || config->outputMode > NAE_OUTPUT_FIXED) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (config->quality < NAE_QUALITY_FAST || config->quality > NAE_QUALITY_VERY_HIGH) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (config->shareMode != NAE_SHARE_EXCLUSIVE && config->shareMode != NAE_SHARE_SHARED) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    if (config->fixedRate < 0) return NAE_ERR_INVALID_ARGUMENT;

    NAE_OutputPolicy wanted{};
    if (explicitPolicy) {
        if (!requested || requested->structSize != sizeof(NAE_OutputPolicy)) {
            return NAE_ERR_INVALID_ARGUMENT;
        }
        wanted = *requested;
    } else {
        NAE_OutputPolicyDefaults(&wanted);
        wanted.ratePolicy = config->outputMode == NAE_OUTPUT_NATIVE
                                ? NAE_RATE_NATIVE
                                : config->outputMode == NAE_OUTPUT_FIXED
                                      ? NAE_RATE_FIXED
                                      : NAE_RATE_DEVICE;
        wanted.fixedRate = static_cast<uint32_t>(config->fixedRate);
        wanted.resampleQuality = config->quality == NAE_QUALITY_FAST
                                     ? NAE_RESAMPLE_QUALITY_FAST
                                     : config->quality == NAE_QUALITY_VERY_HIGH
                                           ? NAE_RESAMPLE_QUALITY_VERY_HIGH
                                           : NAE_RESAMPLE_QUALITY_HIGH;
        wanted.dsdMode = NAE_DSD_MODE_PCM;
        wanted.dsdTransport = NAE_DSD_TRANSPORT_NONE;
        wanted.bassManagement = NAE_BASS_OFF;
        wanted.clipGuardMode = NAE_CLIP_GUARD_OFF;
    }

    NAE_Session* session = nullptr;
    try {
        session = new NAE_Session();
        session->deviceId = config->deviceId ? config->deviceId : "";
        session->path = config->path;
        session->outputMode = config->outputMode;
        session->fixedRate = config->fixedRate;
        session->quality = config->quality;
        session->shareMode = config->shareMode;
        session->sliceMs = slice;
        session->requestedPolicy = wanted;
        session->effectivePolicy = wanted;
        session->allowPcmFallback = wanted.allowPcmFallback != 0;
        const bool sourceDsd = isDsdFile(config->path);
        session->dsdSourceFile = sourceDsd;
        if (sourceDsd) {
            if (wanted.dsdRatePolicy == NAE_DSD_RATE_EXTRACT) {
                session->dsdExtractRequested = true;
                session->dsdFallback = true;
                session->effectivePolicy.dsdMode = NAE_DSD_MODE_PCM;
            } else if (wanted.dsdRatePolicy == NAE_DSD_RATE_DOWNRATE ||
                       wanted.targetDsdRate != 0) {
                session->dsdDownrateRequested = true;
            }
            if (wanted.dsdTransport == NAE_DSD_TRANSPORT_NATIVE) {
                session->nativeUnsupported = true;
                session->nativeDsdRequested = true;
            } else if (wanted.dsdMode == NAE_DSD_MODE_AUTO ||
                       wanted.dsdMode == NAE_DSD_MODE_DSD) {
                session->dopMode = config->shareMode == NAE_SHARE_EXCLUSIVE;
                if (!session->dopMode) session->dsdFallback = true;
            }
        } else if (wanted.dsdMode == NAE_DSD_MODE_DSD ||
                   wanted.dsdMode == NAE_DSD_MODE_PCM_TO_DSD) {
            session->nativeUnsupported = true;
            session->effectivePolicy.dsdMode = NAE_DSD_MODE_PCM;
        }
        {
            NAE_DeviceCaps caps{};
            caps.structSize = sizeof(caps);
            const int32_t share = config->shareMode == NAE_SHARE_SHARED
                                      ? NAE_SHARE_MODE_SHARED
                                      : NAE_SHARE_MODE_EXCLUSIVE;
            if (NAE_QueryDeviceCaps(config->deviceId, share, &caps) == NAE_OK) {
                session->caps = caps;
            }
        }
        uint32_t probedRate = 0;
        uint32_t probedChannels = 0;
        {
            neiro::Decoder probe;
            std::string probeError;
            if (probe.open(config->path, &probeError)) {
                probedRate = probe.info().sample_rate;
                probedChannels = probe.info().channels;
                if (session->dsdSourceFile) {
                    session->dsdSourceRate = static_cast<int32_t>(probedRate) * 8;
                }
            }
        }
        if (session->dsdSourceFile) session->dsdFileRate = session->dsdSourceRate;
        // Bitstream downrate: choose the highest lower DSD rate whose DoP carrier
        // the device lists. Only DoP can carry a downrated stream here; native
        // DSD has no backend to open.
        if (session->dsdDownrateRequested && session->dopMode &&
            session->dsdSourceRate > 2822400 &&
            (session->caps.probed & NAE_CAPS_PROBED_DOP) != 0 &&
            session->caps.dopSupported) {
            const int32_t requestedTarget = wanted.targetDsdRate;
            int32_t chosen = 0;
            for (int32_t cand = session->dsdSourceRate / 2; cand >= 2822400; cand /= 2) {
                if (requestedTarget != 0 && cand != requestedTarget) continue;
                const uint32_t carrier = static_cast<uint32_t>(cand) / 16;
                for (uint32_t i = 0; i < session->caps.dopCarrierCount; ++i) {
                    if (session->caps.dopCarriers[i] == carrier) {
                        chosen = cand;
                        break;
                    }
                }
                if (chosen) break;
            }
            session->downrateTarget = chosen;
        }
        // Auto rate: pick Native when the measured device accepts the source
        // rate, otherwise Device. Without caps, Device is the conservative
        // choice; the actual path is reported separately.
        if (session->effectivePolicy.ratePolicy == NAE_RATE_AUTO) {
            bool supported = false;
            if ((session->caps.probed & NAE_CAPS_PROBED_RATES) != 0 && probedRate) {
                for (uint32_t i = 0; i < session->caps.rateCount; ++i) {
                    if (session->caps.rates[i] == probedRate) {
                        supported = true;
                        break;
                    }
                }
            }
            session->effectivePolicy.ratePolicy = supported ? NAE_RATE_NATIVE : NAE_RATE_DEVICE;
        }
        // Explicit routing presets become a real matrix before the session
        // thread opens the file. Auto downmix is allowed; Auto upmix is not.
        if (config->shareMode == NAE_SHARE_EXCLUSIVE && probedChannels) {
            const uint32_t sourceMask = maskForChannels(static_cast<int32_t>(probedChannels));
            uint32_t targetMask = session->requestedPolicy.targetChannelMask;
            if (targetMask == 0) {
                targetMask = session->caps.nativeChannelMask
                                 ? session->caps.nativeChannelMask
                                 : maskForChannels(session->caps.nativeChannels);
            }
            const uint32_t sourceCount = sourceMask ? bitCountForMask(sourceMask) : 0;
            const uint32_t targetCount = targetMask ? bitCountForMask(targetMask) : 0;
            if (sourceMask && targetMask && sourceMask != targetMask) {
                const int32_t requestedMode = session->requestedPolicy.routingMode;
                int32_t mode = requestedMode;
                if (mode == NAE_ROUTING_AUTO && targetCount < sourceCount) {
                    mode = NAE_ROUTING_DOWNMIX;
                }
                const int32_t preset = mode == NAE_ROUTING_DOWNMIX
                                           ? session->requestedPolicy.downmixPreset
                                           : session->requestedPolicy.upmixPreset;
                if (mode == NAE_ROUTING_DOWNMIX || mode == NAE_ROUTING_UPMIX) {
                    NAE_RoutingMatrix matrix{};
                    if (NAE_BuildRoutingMatrix(mode, preset, sourceMask, targetMask,
                                               &matrix) == NAE_OK) {
                        matrix.bassManagement = session->requestedPolicy.bassManagement;
                        matrix.lfeMode = session->requestedPolicy.lfeMode;
                        matrix.lfeCrossoverHz = session->requestedPolicy.lfeCrossoverHz;
                        matrix.lfeGainDb = session->requestedPolicy.lfeGainDb;
                        // Leave sampleRate at 0: the engine fills it with the
                        // rate the device actually negotiated. The endpoint's
                        // native rate is not necessarily the rate this session
                        // opens, and bass management and the IIR Hilbert both
                        // depend on the real one.
                        matrix.sampleRate = 0.0;
                        session->routingMatrix = matrix;
                        session->routingActive = true;
                    }
                } else if (mode == NAE_ROUTING_MATRIX && session->requestedPolicy.matrixId) {
                    NAE_RoutingMatrix matrix{};
                    matrix.structSize = sizeof(matrix);
                    if (NAE_GetRoutingMatrix(session->requestedPolicy.matrixId, &matrix) == NAE_OK &&
                        matrix.sourceChannels == sourceCount &&
                        matrix.targetChannels == targetCount &&
                        matrix.sourceMask == sourceMask && matrix.targetMask == targetMask) {
                        session->routingMatrix = matrix;
                        session->routingActive = true;
                    }
                }
            }
        }
        switch (session->effectivePolicy.ratePolicy) {
            case NAE_RATE_NATIVE: session->outputMode = NAE_OUTPUT_NATIVE; break;
            case NAE_RATE_FIXED: session->outputMode = NAE_OUTPUT_FIXED; break;
            default: session->outputMode = NAE_OUTPUT_DEVICE; break;
        }
        session->fixedRate = static_cast<int32_t>(session->effectivePolicy.fixedRate);
        switch (session->effectivePolicy.resampleQuality) {
            case NAE_RESAMPLE_QUALITY_FAST: session->quality = NAE_QUALITY_FAST; break;
            case NAE_RESAMPLE_QUALITY_VERY_HIGH: session->quality = NAE_QUALITY_VERY_HIGH; break;
            default: session->quality = NAE_QUALITY_HIGH; break;
        }
        NAE_EqDefaults(&session->eqParams);
        NAE_CrossfeedDefaults(&session->crossfeedParams);
        NAE_ReplayGainDefaults(&session->rgParams);
        NAE_ConvolverDefaults(&session->convParams);
        NAE_CrossfadeDefaults(&session->crossfadeParams);
        session->thread = std::thread(sessionThread, session);
    } catch (...) {
        delete session;
        return NAE_ERR_NO_MEMORY;
    }

    {
        std::unique_lock<std::mutex> lock(session->openMutex);
        if (!session->openCv.wait_for(lock, std::chrono::seconds(10),
                                      [session] { return session->openDone; })) {
            session->quit = true;
            lock.unlock();
            if (session->thread.joinable()) session->thread.join();
            delete session;
            return NAE_ERR_INITIALIZE;
        }
    }

    *out = session;
    return session->openResult.load();
}

int32_t NAE_SessionOpen(const NAE_SessionConfig* config, NAE_Session** out) NAE_NOEXCEPT {
    return sessionOpenImpl(config, nullptr, false, out);
}

int32_t NAE_SessionOpenWithPolicy(const NAE_SessionConfig* config,
                                  const NAE_OutputPolicy* policy,
                                  NAE_Session** out) NAE_NOEXCEPT {
    return sessionOpenImpl(config, policy, true, out);
}

int32_t NAE_SessionGetOutputPolicy(NAE_Session* session,
                                   NAE_OutputPolicy* out) NAE_NOEXCEPT {
    if (!session || !out || out->structSize != sizeof(NAE_OutputPolicy)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    *out = session->requestedPolicy;
    return NAE_OK;
}

int32_t NAE_SessionPlay(NAE_Session* session) NAE_NOEXCEPT {
    if (!session) return NAE_ERR_INVALID_ARGUMENT;
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_PLAYING) return NAE_OK;
    if (st == NAE_SESSION_READY || st == NAE_SESSION_PAUSED) {
        session->state = NAE_SESSION_PLAYING;
        session->resumeCmd = true;
        return NAE_OK;
    }
    if (st == NAE_SESSION_ENDED) {
        session->seekRequest = 0.0;
        session->state = NAE_SESSION_PLAYING;
        session->resumeCmd = true;
        return NAE_OK;
    }
    if (st == NAE_SESSION_DEVICE_LOST) return NAE_ERR_DEVICE_LOST;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    return NAE_ERR_NOT_OPEN;
}

int32_t NAE_SessionPause(NAE_Session* session) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session) return NAE_ERR_INVALID_ARGUMENT;
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_PLAYING) {
        session->state = NAE_SESSION_PAUSED;
        session->pauseCmd = true;
        return NAE_OK;
    }
    if (st == NAE_SESSION_PAUSED || st == NAE_SESSION_READY || st == NAE_SESSION_ENDED) {
        return NAE_OK;
    }
    if (st == NAE_SESSION_DEVICE_LOST) return NAE_ERR_DEVICE_LOST;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    return NAE_ERR_NOT_OPEN;
}

int32_t NAE_SessionResume(NAE_Session* session) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session) return NAE_ERR_INVALID_ARGUMENT;
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_PAUSED) {
        session->state = NAE_SESSION_PLAYING;
        session->resumeCmd = true;
        return NAE_OK;
    }
    if (st == NAE_SESSION_PLAYING) return NAE_OK;
    if (st == NAE_SESSION_DEVICE_LOST) return NAE_ERR_DEVICE_LOST;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    return NAE_ERR_NOT_OPEN;
}

int32_t NAE_SessionSeek(NAE_Session* session, double seconds) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !std::isfinite(seconds) || seconds < 0.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    if (st == NAE_SESSION_DEVICE_LOST) return NAE_ERR_DEVICE_LOST;

    const double dur = session->duration.load();
    if (dur > 0.0 && seconds > dur) seconds = dur;
    session->seekRequest = seconds;
    if (st == NAE_SESSION_ENDED) session->state = NAE_SESSION_PAUSED;
    return NAE_OK;
}

int32_t NAE_SessionStop(NAE_Session* session) NAE_NOEXCEPT {
    if (!session) return NAE_ERR_INVALID_ARGUMENT;
    session->quit = true;
    if (session->thread.joinable()) session->thread.join();
    session->state = NAE_SESSION_IDLE;
    return NAE_OK;
}

int32_t NAE_SessionGetState(NAE_Session* session, NAE_SessionState* out) NAE_NOEXCEPT {
    if (!session || !out) return NAE_ERR_INVALID_ARGUMENT;
    if (out->structSize < sizeof(NAE_SessionState)) return NAE_ERR_INVALID_ARGUMENT;

    std::memset(out, 0, sizeof(*out));
    out->structSize = sizeof(NAE_SessionState);
    out->state = session->state.load();
    out->position = session->position.load();
    out->duration = session->duration.load();
    out->sampleRate = session->sampleRate.load();
    out->sourceRate = session->sourceRate.load();
    out->channels = session->channels.load();
    out->bitsPerSample = session->bitsPerSample.load();
    out->validBits = session->validBits.load();
    out->isFloat = session->isFloat.load();
    const int32_t basePerfect = session->bitPerfect.load();
    const int32_t baseReason = session->reason.load();
    const double volume = session->volume.load();
    const int32_t eq = session->eqEnabled.load();
    const int32_t crossfeed = session->crossfeedEnabled.load();
    const int32_t rg = session->rgEnabled.load();
    const int32_t conv = session->convEnabled.load();
    const int32_t crossfade = session->crossfadeEnabled.load();
    const double rgApplied = session->rgApplied.load();
    if (basePerfect && eq) {
        out->bitPerfect = 0;
        out->reason = NAE_REASON_EQ;
    } else if (basePerfect && crossfeed) {
        out->bitPerfect = 0;
        out->reason = NAE_REASON_CROSSFEED;
    } else if (basePerfect && conv) {
        out->bitPerfect = 0;
        out->reason = NAE_REASON_CONVOLVER;
    } else if (basePerfect && crossfade) {
        out->bitPerfect = 0;
        out->reason = NAE_REASON_CROSSFADE;
    } else if (basePerfect && rgApplied != 1.0) {
        out->bitPerfect = 0;
        out->reason = NAE_REASON_REPLAYGAIN;
    } else if (basePerfect && volume == 1.0) {
        out->bitPerfect = 1;
        out->reason = 0;
    } else if (basePerfect) {
        out->bitPerfect = 0;
        out->reason = NAE_REASON_VOLUME_NOT_UNITY;
    } else {
        out->bitPerfect = 0;
        out->reason = baseReason;
    }
    out->volume = volume;
    out->eqEnabled = eq;
    out->crossfeedEnabled = crossfeed;
    out->replayGainEnabled = rg;
    out->convolverEnabled = conv;
    out->crossfadeEnabled = crossfade;
    out->rgTrackValid = session->rgTrackValid.load();
    out->rgAlbumValid = session->rgAlbumValid.load();
    out->rgTrackGainDb = session->rgTrackGainDb.load();
    out->rgAlbumGainDb = session->rgAlbumGainDb.load();
    out->rgAppliedGain = session->rgApplied.load();
    out->trackSerial = session->trackSerial.load();
    out->resampling = session->resampling.load();
    out->dithered = session->dithered.load();
    out->fileChannels = session->fileChannels.load();
    out->latencyMs = session->latencyMs.load();
    return NAE_OK;
}

int32_t NAE_SessionSetVolume(NAE_Session* session, double gain) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !std::isfinite(gain) || gain < 0.0 || gain > 1.0) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    session->volumeRequest = gain;
    return NAE_OK;
}

double NAE_SessionGetVolume(NAE_Session* session) NAE_NOEXCEPT {
    return session ? session->volume.load() : 1.0;
}

int32_t NAE_SessionSetEq(NAE_Session* session, const NAE_EqParams* params) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !params || params->structSize < sizeof(NAE_EqParams) ||
        params->bandCount < 0 || params->bandCount > NAE_EQ_MAX_BANDS) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    {
        std::lock_guard<std::mutex> lock(session->eqMutex);
        session->eqParams = *params;
        session->eqParams.structSize = sizeof(NAE_EqParams);
    }
    session->eqPending = true;
    return NAE_OK;
}

int32_t NAE_SessionSetCrossfeed(NAE_Session* session,
                                const NAE_CrossfeedParams* params) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !params || params->structSize < sizeof(NAE_CrossfeedParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    {
        std::lock_guard<std::mutex> lock(session->crossfeedMutex);
        session->crossfeedParams = *params;
        session->crossfeedParams.structSize = sizeof(NAE_CrossfeedParams);
    }
    session->crossfeedPending = true;
    return NAE_OK;
}

int32_t NAE_SessionSetReplayGain(NAE_Session* session,
                                 const NAE_ReplayGainParams* params) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !params || params->structSize < sizeof(NAE_ReplayGainParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    {
        std::lock_guard<std::mutex> lock(session->rgMutex);
        session->rgParams = *params;
        session->rgParams.structSize = sizeof(NAE_ReplayGainParams);
    }
    session->rgPending = true;
    return NAE_OK;
}

int32_t NAE_SessionGetReplayGain(NAE_Session* session,
                                 NAE_ReplayGainParams* out) NAE_NOEXCEPT {
    if (!session || !out || out->structSize < sizeof(NAE_ReplayGainParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->rgMutex);
        if (session->rgParams.structSize == 0) {
            NAE_ReplayGainDefaults(out);
        } else {
            *out = session->rgParams;
        }
    }
    out->structSize = sizeof(NAE_ReplayGainParams);
    return NAE_OK;
}

int32_t NAE_SessionGetCrossfeed(NAE_Session* session,
                                NAE_CrossfeedParams* out) NAE_NOEXCEPT {
    if (!session || !out || out->structSize < sizeof(NAE_CrossfeedParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->crossfeedMutex);
        if (session->crossfeedParams.structSize == 0) {
            NAE_CrossfeedDefaults(out);
        } else {
            *out = session->crossfeedParams;
        }
    }
    out->structSize = sizeof(NAE_CrossfeedParams);
    return NAE_OK;
}

int32_t NAE_SessionGetEq(NAE_Session* session, NAE_EqParams* out) NAE_NOEXCEPT {
    if (!session || !out || out->structSize < sizeof(NAE_EqParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->eqMutex);
        if (session->eqParams.structSize == 0) {
            NAE_EqDefaults(out);
        } else {
            *out = session->eqParams;
        }
    }
    out->structSize = sizeof(NAE_EqParams);
    return NAE_OK;
}

int32_t NAE_SessionSetConvolver(NAE_Session* session,
                                const NAE_ConvolverParams* params) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !params || params->structSize < sizeof(NAE_ConvolverParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    {
        std::lock_guard<std::mutex> lock(session->convMutex);
        session->convParams = *params;
        session->convParams.structSize = sizeof(NAE_ConvolverParams);
        session->convParams.irPath[sizeof(session->convParams.irPath) - 1] = '\0';
    }
    session->convPending = true;
    return NAE_OK;
}

int32_t NAE_SessionGetConvolver(NAE_Session* session,
                                NAE_ConvolverParams* out) NAE_NOEXCEPT {
    if (!session || !out || out->structSize < sizeof(NAE_ConvolverParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->convMutex);
        if (session->convParams.structSize == 0) {
            NAE_ConvolverDefaults(out);
        } else {
            *out = session->convParams;
        }
    }
    out->structSize = sizeof(NAE_ConvolverParams);
    return NAE_OK;
}

int32_t NAE_SessionSetCrossfade(NAE_Session* session,
                                const NAE_CrossfadeParams* params) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !params || params->structSize < sizeof(NAE_CrossfadeParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    {
        std::lock_guard<std::mutex> lock(session->crossfadeMutex);
        session->crossfadeParams = *params;
        session->crossfadeParams.structSize = sizeof(NAE_CrossfadeParams);
    }
    session->crossfadePending = true;
    return NAE_OK;
}

int32_t NAE_SessionGetCrossfade(NAE_Session* session,
                                NAE_CrossfadeParams* out) NAE_NOEXCEPT {
    if (!session || !out || out->structSize < sizeof(NAE_CrossfadeParams)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::mutex> lock(session->crossfadeMutex);
        if (session->crossfadeParams.structSize == 0) {
            NAE_CrossfadeDefaults(out);
        } else {
            *out = session->crossfadeParams;
        }
    }
    out->structSize = sizeof(NAE_CrossfadeParams);
    return NAE_OK;
}

int32_t NAE_SessionQueueAdd(NAE_Session* session, const char* path) NAE_NOEXCEPT {
    if (session && session->dopMode) return NAE_ERR_REQUIRES_REOPEN;
    if (!session || !path || !path[0]) return NAE_ERR_INVALID_ARGUMENT;
    const int32_t st = session->state.load();
    if (st == NAE_SESSION_IDLE) return NAE_ERR_NOT_OPEN;
    if (st == NAE_SESSION_ERROR) return NAE_ERR_INITIALIZE;
    {
        std::lock_guard<std::mutex> lock(session->queueMutex);
        if (session->queue.size() >= 256) return NAE_ERR_NO_MEMORY;
        session->queue.emplace_back(path);
    }
    session->queuePending = true;
    return NAE_OK;
}

int32_t NAE_SessionQueueClear(NAE_Session* session) NAE_NOEXCEPT {
    if (!session) return NAE_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(session->queueMutex);
        session->queue.clear();
    }
    session->queuePending = false;
    session->clearNextPending = true;
    return NAE_OK;
}

int32_t NAE_SessionQueueCount(NAE_Session* session) NAE_NOEXCEPT {
    if (!session) return NAE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(session->queueMutex);
    return static_cast<int32_t>(session->queue.size());
}

int32_t NAE_SessionLastError(NAE_Session* session, char* buf, int32_t bufLen) NAE_NOEXCEPT {
    if (!session || !buf || bufLen <= 0) return NAE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(session->errMutex);
    const size_t n = std::min(session->lastError.size(), static_cast<size_t>(bufLen - 1));
    std::memcpy(buf, session->lastError.data(), n);
    buf[n] = '\0';
    return static_cast<int32_t>(n);
}

int32_t NAE_SessionGetResolvedSignalPath(NAE_Session* session,
                                          NAE_ResolvedSignalPath* out) NAE_NOEXCEPT {
    if (!session || !out || out->structSize != sizeof(NAE_ResolvedSignalPath)) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(session->resolvedMutex);
    if (!session->resolvedValid) return NAE_ERR_NOT_OPEN;
    *out = session->resolved;
    return NAE_OK;
}

char* NAE_SessionGetResolvedSignalPathJson(NAE_Session* session) NAE_NOEXCEPT {
    if (!session) return nullptr;
    NAE_ResolvedSignalPath r{};
    {
        std::lock_guard<std::mutex> lock(session->resolvedMutex);
        if (!session->resolvedValid) return nullptr;
        r = session->resolved;
    }
    const std::string json = resolvedJson(r);
    char* text = static_cast<char*>(std::malloc(json.size() + 1));
    if (!text) return nullptr;
    std::memcpy(text, json.c_str(), json.size() + 1);
    return text;
}

void NAE_SessionClose(NAE_Session* session) NAE_NOEXCEPT {
    if (!session) return;
    session->quit = true;
    if (session->thread.joinable()) session->thread.join();
    delete session;
}


// ---- parameter registry ----------------------------------------------------
//
// The typed structures are the contract; this is the same set for a CLI or a
// settings UI. Per-band EQ entries are generated so the table and the typed API
// cannot disagree about how many bands exist.

namespace {

struct ParamTable {
    std::deque<std::string> names;
    std::vector<NAE_ParamDesc> descs;
};

ParamTable makeParamTable() {
    ParamTable t;
    auto add = [&](const char* name, const char* unit, int32_t type, double minValue,
                   double maxValue, double defaultValue, uint32_t flags,
                   const char* values = nullptr) {
        t.names.emplace_back(name);
        NAE_ParamDesc d{};
        d.structSize = sizeof(d);
        d.name = t.names.back().c_str();
        d.unit = unit;
        d.type = type;
        d.minValue = minValue;
        d.maxValue = maxValue;
        d.defaultValue = defaultValue;
        d.flags = flags;
        d.values = values;
        t.descs.push_back(d);
    };

    add("output.mode", "", NAE_PARAM_ENUM, 0, 2, 0,
        NAE_PARAM_OPEN | NAE_PARAM_BREAKS_BP, "native,device,fixed");
    add("output.fixed_rate", "Hz", NAE_PARAM_INT, 0, 384000, 0,
        NAE_PARAM_OPEN | NAE_PARAM_BREAKS_BP);
    add("output.share", "", NAE_PARAM_ENUM, 0, 1, 0,
        NAE_PARAM_OPEN | NAE_PARAM_BREAKS_BP, "exclusive,shared");
    add("resampler.quality", "", NAE_PARAM_ENUM, 0, 2, 1, NAE_PARAM_OPEN,
        "quick,high,very_high");
    add("session.slice_ms", "ms", NAE_PARAM_INT, 4, 200, 16, NAE_PARAM_OPEN);

    add("volume.gain", "", NAE_PARAM_DOUBLE, 0, 1, 1.0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("eq.enabled", "", NAE_PARAM_BOOL, 0, 1, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_STRUCT | NAE_PARAM_BREAKS_BP);
    add("eq.preamp_db", "dB", NAE_PARAM_DOUBLE, -24, 24, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("eq.band_count", "", NAE_PARAM_INT, 0, NAE_EQ_MAX_BANDS, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_STRUCT | NAE_PARAM_BREAKS_BP);

    for (int i = 0; i < NAE_EQ_MAX_BANDS; ++i) {
        const std::string base = "eq.band" + std::to_string(i) + ".";
        const uint32_t flags = NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP;
        add((base + "type").c_str(), "", NAE_PARAM_ENUM, 0, 6, 0, flags,
            "peaking,low_shelf,high_shelf,high_pass,low_pass,notch,all_pass");
        add((base + "freq_hz").c_str(), "Hz", NAE_PARAM_DOUBLE, 10, 24000, 1000, flags);
        add((base + "gain_db").c_str(), "dB", NAE_PARAM_DOUBLE, -24, 24, 0, flags);
        add((base + "q").c_str(), "", NAE_PARAM_DOUBLE, 0.1, 18, 1.0, flags);
        add((base + "enabled").c_str(), "", NAE_PARAM_BOOL, 0, 1, 0, flags);
    }

    add("crossfeed.enabled", "", NAE_PARAM_BOOL, 0, 1, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("crossfeed.amount", "", NAE_PARAM_DOUBLE, 0, 1, 0.3,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("crossfeed.delay_ms", "ms", NAE_PARAM_DOUBLE, 0, 5, 0.3,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("crossfeed.cutoff_hz", "Hz", NAE_PARAM_DOUBLE, 20, 20000, 700,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);

    add("replaygain.mode", "", NAE_PARAM_ENUM, 0, 2, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP, "off,track,album");
    add("replaygain.preamp_db", "dB", NAE_PARAM_DOUBLE, -20, 20, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("replaygain.fallback_db", "dB", NAE_PARAM_DOUBLE, -20, 20, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("replaygain.prevent_clipping", "", NAE_PARAM_BOOL, 0, 1, 1,
        NAE_PARAM_RUNTIME);

    add("convolver.enabled", "", NAE_PARAM_BOOL, 0, 1, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_STRUCT | NAE_PARAM_BREAKS_BP);
    add("convolver.ir", "", NAE_PARAM_STRING, 0, 0, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_STRUCT | NAE_PARAM_BREAKS_BP);
    add("convolver.wet", "", NAE_PARAM_DOUBLE, 0, 1, 1,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("convolver.dry", "", NAE_PARAM_DOUBLE, 0, 1, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("convolver.gain_db", "dB", NAE_PARAM_DOUBLE, -24, 24, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("convolver.normalize", "", NAE_PARAM_BOOL, 0, 1, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_STRUCT);

    add("crossfade.enabled", "", NAE_PARAM_BOOL, 0, 1, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("crossfade.duration_ms", "ms", NAE_PARAM_DOUBLE, 0, 30000, 0,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP);
    add("crossfade.curve", "", NAE_PARAM_ENUM, 0, 1, 1,
        NAE_PARAM_RUNTIME | NAE_PARAM_BREAKS_BP, "linear,equal_power");

    add("queue.count", "", NAE_PARAM_INT, 0, 256, 0, NAE_PARAM_READONLY);
    return t;
}

ParamTable& paramTable() {
    static ParamTable table = makeParamTable();
    return table;
}

const NAE_ParamDesc* findParam(const char* name) {
    if (!name) return nullptr;
    for (const auto& d : paramTable().descs) {
        if (std::strcmp(d.name, name) == 0) return &d;
    }
    return nullptr;
}

bool parseBand(const char* name, int* index, const char** field) {
    if (!name) return false;
    int i = -1;
    char fieldBuf[32] = {};
    if (std::sscanf(name, "eq.band%d.%31s", &i, fieldBuf) != 2) return false;
    if (i < 0 || i >= NAE_EQ_MAX_BANDS) return false;
    *index = i;
    // The caller needs a stable string; the table already owns it, so return it
    // from a small static set of field names.
    static const char* kFields[] = {"type", "freq_hz", "gain_db", "q", "enabled"};
    for (const char* f : kFields) {
        if (std::strcmp(f, fieldBuf) == 0) {
            *field = f;
            return true;
        }
    }
    return false;
}

}  // namespace

int32_t NAE_ParamCount(void) NAE_NOEXCEPT {
    return static_cast<int32_t>(paramTable().descs.size());
}

int32_t NAE_ParamAt(int32_t index, NAE_ParamDesc* out) NAE_NOEXCEPT {
    if (!out || index < 0 || index >= static_cast<int32_t>(paramTable().descs.size())) {
        return NAE_ERR_INVALID_ARGUMENT;
    }
    *out = paramTable().descs[static_cast<size_t>(index)];
    out->structSize = sizeof(NAE_ParamDesc);
    return NAE_OK;
}

int32_t NAE_SessionParamGet(NAE_Session* session, const char* name, double* out) NAE_NOEXCEPT {
    if (!session || !name || !out) return NAE_ERR_INVALID_ARGUMENT;
    const NAE_ParamDesc* desc = findParam(name);
    if (!desc || desc->type == NAE_PARAM_STRING) return NAE_ERR_INVALID_ARGUMENT;

    if (!std::strcmp(name, "output.mode")) { *out = session->outputMode; return NAE_OK; }
    if (!std::strcmp(name, "output.fixed_rate")) { *out = session->fixedRate; return NAE_OK; }
    if (!std::strcmp(name, "output.share")) { *out = session->shareMode; return NAE_OK; }
    if (!std::strcmp(name, "resampler.quality")) { *out = session->quality; return NAE_OK; }
    if (!std::strcmp(name, "session.slice_ms")) { *out = session->sliceMs; return NAE_OK; }
    if (!std::strcmp(name, "volume.gain")) { *out = session->volume.load(); return NAE_OK; }
    if (!std::strcmp(name, "queue.count")) {
        std::lock_guard<std::mutex> lock(session->queueMutex);
        *out = static_cast<double>(session->queue.size());
        return NAE_OK;
    }

    if (!std::strncmp(name, "eq.", 3)) {
        int band = -1;
        const char* field = nullptr;
        NAE_EqParams eq;
        {
            std::lock_guard<std::mutex> lock(session->eqMutex);
            NAE_EqDefaults(&eq);
            eq = session->eqParams;
        }
        if (!std::strcmp(name, "eq.enabled")) { *out = eq.enabled; return NAE_OK; }
        if (!std::strcmp(name, "eq.preamp_db")) { *out = eq.preampDb; return NAE_OK; }
        if (!std::strcmp(name, "eq.band_count")) { *out = eq.bandCount; return NAE_OK; }
        if (!parseBand(name, &band, &field)) return NAE_ERR_INVALID_ARGUMENT;
        if (band >= eq.bandCount) {
            *out = !std::strcmp(field, "freq_hz") ? 1000.0
                   : !std::strcmp(field, "q")    ? 1.0
                                                 : 0.0;
            return NAE_OK;
        }
        const NAE_EqBand& b = eq.bands[band];
        if (!std::strcmp(field, "type")) *out = b.type;
        else if (!std::strcmp(field, "freq_hz")) *out = b.freqHz;
        else if (!std::strcmp(field, "gain_db")) *out = b.gainDb;
        else if (!std::strcmp(field, "q")) *out = b.q;
        else if (!std::strcmp(field, "enabled")) *out = b.enabled;
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_OK;
    }

    if (!std::strncmp(name, "crossfeed.", 10)) {
        NAE_CrossfeedParams cf;
        {
            std::lock_guard<std::mutex> lock(session->crossfeedMutex);
            NAE_CrossfeedDefaults(&cf);
            cf = session->crossfeedParams;
        }
        if (!std::strcmp(name, "crossfeed.enabled")) *out = cf.enabled;
        else if (!std::strcmp(name, "crossfeed.amount")) *out = cf.amount;
        else if (!std::strcmp(name, "crossfeed.delay_ms")) *out = cf.delayMs;
        else if (!std::strcmp(name, "crossfeed.cutoff_hz")) *out = cf.cutoffHz;
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_OK;
    }

    if (!std::strncmp(name, "replaygain.", 11)) {
        NAE_ReplayGainParams rg;
        {
            std::lock_guard<std::mutex> lock(session->rgMutex);
            NAE_ReplayGainDefaults(&rg);
            rg = session->rgParams;
        }
        if (!std::strcmp(name, "replaygain.mode")) *out = rg.mode;
        else if (!std::strcmp(name, "replaygain.preamp_db")) *out = rg.preampDb;
        else if (!std::strcmp(name, "replaygain.fallback_db")) *out = rg.fallbackDb;
        else if (!std::strcmp(name, "replaygain.prevent_clipping")) *out = rg.preventClipping;
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_OK;
    }

    if (!std::strncmp(name, "convolver.", 10)) {
        NAE_ConvolverParams cv;
        {
            std::lock_guard<std::mutex> lock(session->convMutex);
            NAE_ConvolverDefaults(&cv);
            cv = session->convParams;
        }
        if (!std::strcmp(name, "convolver.enabled")) *out = cv.enabled;
        else if (!std::strcmp(name, "convolver.wet")) *out = cv.wet;
        else if (!std::strcmp(name, "convolver.dry")) *out = cv.dry;
        else if (!std::strcmp(name, "convolver.gain_db")) *out = cv.gainDb;
        else if (!std::strcmp(name, "convolver.normalize")) *out = cv.normalize;
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_OK;
    }

    if (!std::strncmp(name, "crossfade.", 10)) {
        NAE_CrossfadeParams xf;
        {
            std::lock_guard<std::mutex> lock(session->crossfadeMutex);
            NAE_CrossfadeDefaults(&xf);
            xf = session->crossfadeParams;
        }
        if (!std::strcmp(name, "crossfade.enabled")) *out = xf.enabled;
        else if (!std::strcmp(name, "crossfade.duration_ms")) *out = xf.durationMs;
        else if (!std::strcmp(name, "crossfade.curve")) *out = xf.curve;
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_OK;
    }

    return NAE_ERR_INVALID_ARGUMENT;
}

int32_t NAE_SessionParamSet(NAE_Session* session, const char* name, double value) NAE_NOEXCEPT {
    if (!session || !name) return NAE_ERR_INVALID_ARGUMENT;
    const NAE_ParamDesc* desc = findParam(name);
    if (!desc || desc->type == NAE_PARAM_STRING) return NAE_ERR_INVALID_ARGUMENT;
    if (desc->flags & NAE_PARAM_READONLY) return NAE_ERR_INVALID_ARGUMENT;
    if (desc->flags & NAE_PARAM_OPEN) return NAE_ERR_REQUIRES_REOPEN;
    if (!std::isfinite(value) || value < desc->minValue || value > desc->maxValue) {
        return NAE_ERR_INVALID_ARGUMENT;
    }

    if (!std::strcmp(name, "volume.gain")) {
        return NAE_SessionSetVolume(session, value);
    }

    if (!std::strncmp(name, "eq.", 3)) {
        NAE_EqParams eq;
        {
            std::lock_guard<std::mutex> lock(session->eqMutex);
            NAE_EqDefaults(&eq);
            eq = session->eqParams;
        }
        int band = -1;
        const char* field = nullptr;
        if (!std::strcmp(name, "eq.enabled")) eq.enabled = static_cast<int32_t>(value);
        else if (!std::strcmp(name, "eq.preamp_db")) eq.preampDb = value;
        else if (!std::strcmp(name, "eq.band_count")) {
            eq.bandCount = static_cast<int32_t>(value);
        } else if (parseBand(name, &band, &field)) {
            if (band >= eq.bandCount) eq.bandCount = band + 1;
            NAE_EqBand& b = eq.bands[band];
            if (!std::strcmp(field, "type")) b.type = static_cast<int32_t>(value);
            else if (!std::strcmp(field, "freq_hz")) b.freqHz = value;
            else if (!std::strcmp(field, "gain_db")) b.gainDb = value;
            else if (!std::strcmp(field, "q")) b.q = value;
            else if (!std::strcmp(field, "enabled")) b.enabled = static_cast<int32_t>(value);
            else return NAE_ERR_INVALID_ARGUMENT;
        } else {
            return NAE_ERR_INVALID_ARGUMENT;
        }
        return NAE_SessionSetEq(session, &eq);
    }

    if (!std::strncmp(name, "crossfeed.", 10)) {
        NAE_CrossfeedParams cf;
        {
            std::lock_guard<std::mutex> lock(session->crossfeedMutex);
            NAE_CrossfeedDefaults(&cf);
            cf = session->crossfeedParams;
        }
        if (!std::strcmp(name, "crossfeed.enabled")) cf.enabled = static_cast<int32_t>(value);
        else if (!std::strcmp(name, "crossfeed.amount")) cf.amount = value;
        else if (!std::strcmp(name, "crossfeed.delay_ms")) cf.delayMs = value;
        else if (!std::strcmp(name, "crossfeed.cutoff_hz")) cf.cutoffHz = value;
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_SessionSetCrossfeed(session, &cf);
    }

    if (!std::strncmp(name, "replaygain.", 11)) {
        NAE_ReplayGainParams rg;
        {
            std::lock_guard<std::mutex> lock(session->rgMutex);
            NAE_ReplayGainDefaults(&rg);
            rg = session->rgParams;
        }
        if (!std::strcmp(name, "replaygain.mode")) rg.mode = static_cast<int32_t>(value);
        else if (!std::strcmp(name, "replaygain.preamp_db")) rg.preampDb = value;
        else if (!std::strcmp(name, "replaygain.fallback_db")) rg.fallbackDb = value;
        else if (!std::strcmp(name, "replaygain.prevent_clipping"))
            rg.preventClipping = static_cast<int32_t>(value);
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_SessionSetReplayGain(session, &rg);
    }

    if (!std::strncmp(name, "convolver.", 10)) {
        NAE_ConvolverParams cv;
        {
            std::lock_guard<std::mutex> lock(session->convMutex);
            NAE_ConvolverDefaults(&cv);
            cv = session->convParams;
        }
        if (!std::strcmp(name, "convolver.enabled")) cv.enabled = static_cast<int32_t>(value);
        else if (!std::strcmp(name, "convolver.wet")) cv.wet = value;
        else if (!std::strcmp(name, "convolver.dry")) cv.dry = value;
        else if (!std::strcmp(name, "convolver.gain_db")) cv.gainDb = value;
        else if (!std::strcmp(name, "convolver.normalize"))
            cv.normalize = static_cast<int32_t>(value);
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_SessionSetConvolver(session, &cv);
    }

    if (!std::strncmp(name, "crossfade.", 10)) {
        NAE_CrossfadeParams xf;
        {
            std::lock_guard<std::mutex> lock(session->crossfadeMutex);
            NAE_CrossfadeDefaults(&xf);
            xf = session->crossfadeParams;
        }
        if (!std::strcmp(name, "crossfade.enabled")) xf.enabled = static_cast<int32_t>(value);
        else if (!std::strcmp(name, "crossfade.duration_ms")) xf.durationMs = value;
        else if (!std::strcmp(name, "crossfade.curve")) xf.curve = static_cast<int32_t>(value);
        else return NAE_ERR_INVALID_ARGUMENT;
        return NAE_SessionSetCrossfade(session, &xf);
    }

    return NAE_ERR_INVALID_ARGUMENT;
}

int32_t NAE_SessionParamGetString(NAE_Session* session, const char* name, char* buf,
                                  int32_t bufLen) NAE_NOEXCEPT {
    if (!session || !name || !buf || bufLen <= 0) return NAE_ERR_INVALID_ARGUMENT;
    if (std::strcmp(name, "convolver.ir") != 0) return NAE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(session->convMutex);
    const size_t n = std::min(std::strlen(session->convParams.irPath),
                              static_cast<size_t>(bufLen - 1));
    std::memcpy(buf, session->convParams.irPath, n);
    buf[n] = '\0';
    return static_cast<int32_t>(n);
}

int32_t NAE_SessionParamSetString(NAE_Session* session, const char* name,
                                  const char* value) NAE_NOEXCEPT {
    if (!session || !name || !value) return NAE_ERR_INVALID_ARGUMENT;
    if (std::strcmp(name, "convolver.ir") != 0) return NAE_ERR_INVALID_ARGUMENT;
    NAE_ConvolverParams cv;
    {
        std::lock_guard<std::mutex> lock(session->convMutex);
        NAE_ConvolverDefaults(&cv);
        cv = session->convParams;
    }
    std::snprintf(cv.irPath, sizeof(cv.irPath), "%s", value);
    return NAE_SessionSetConvolver(session, &cv);
}

}  // extern "C"
