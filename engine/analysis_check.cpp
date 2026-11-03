// Analysis plane: one path-based decode must produce format, tags, loudness,
// dynamic range, peaks and a confident BPM.
//
// The click fixture is 120 BPM; the tagged fixture carries every tag field the
// analysis struct exposes.

#include "nae.h"
#include "utf8_args.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {
int failures = 0;

void put16(std::vector<std::uint8_t>* out, std::uint16_t v) {
    out->push_back(static_cast<std::uint8_t>(v));
    out->push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

bool writeClickWav(const char* path) {
    constexpr std::uint32_t rate = 44100;
    constexpr std::uint32_t seconds = 30;
    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bits = 16;
    const std::uint32_t frames = rate * seconds;
    std::vector<std::uint8_t> wav;
    wav.reserve(44 + frames * channels * 2);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(&wav, 36 + frames * channels * 2);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    put32(&wav, 16);
    put16(&wav, 1);
    put16(&wav, channels);
    put32(&wav, rate);
    put32(&wav, rate * channels * (bits / 8));
    put16(&wav, channels * (bits / 8));
    put16(&wav, bits);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(&wav, frames * channels * (bits / 8));
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        const double phase = t - std::floor(t / 0.5) * 0.5;
        const double amp = t < 15.0 ? 0.8 : 0.08;  // two levels give a real DR
        const double s = phase < 0.002
                             ? amp * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * phase)
                             : 0.0;
        put16(&wav, static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 32767.0)));
        put16(&wav, static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 32767.0)));
    }
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t written = std::fwrite(wav.data(), 1, wav.size(), f);
    std::fclose(f);
    return written == wav.size();
}
bool writeToneWav(const char* path) {
    constexpr std::uint32_t rate = 48000;
    constexpr std::uint16_t channels = 2;
    constexpr std::uint16_t bits = 16;
    const std::uint32_t frames = rate * 10;
    std::vector<std::uint8_t> wav;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put32(&wav, 36 + frames * channels * (bits / 8));
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    put32(&wav, 16);
    put16(&wav, 1);
    put16(&wav, channels);
    put32(&wav, rate);
    put32(&wav, rate * channels * (bits / 8));
    put16(&wav, channels * (bits / 8));
    put16(&wav, bits);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put32(&wav, frames * channels * (bits / 8));
    // -23 dBFS so the expected reading is exactly -23 LUFS for two identical
    // channels, which is the standard's own convention.
    const double amp = std::pow(10.0, -23.0 / 20.0);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double s = amp * std::sin(2.0 * 3.14159265358979323846 * 997.0 * i / rate);
        put16(&wav, static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 32767.0)));
        put16(&wav, static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 32767.0)));
    }
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const size_t written = std::fwrite(wav.data(), 1, wav.size(), f);
    std::fclose(f);
    return written == wav.size();
}

void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}
bool analyze(const char* path, NAE_AnalysisInfo* info, std::vector<float>* peaks,
             int32_t* written) {
    std::memset(info, 0, sizeof(*info));
    info->structSize = sizeof(*info);
    peaks->assign(2000, 0.0f);
    return NAE_AnalyzeFile(path, 1000, info, peaks->data(),
                           static_cast<int32_t>(peaks->size()), written) == NAE_OK;
}
}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = utf8Args(argc, argv);
    const char* clickPath = "analysis_click.wav";
    if (!writeClickWav(clickPath)) {
        std::printf("could not write %s\n", clickPath);
        return 1;
    }

    NAE_AnalysisInfo info;
    std::vector<float> peaks;
    int32_t written = 0;
    check(analyze(clickPath, &info, &peaks, &written), "click fixture analyzes");
    check(info.analyzerVersion == NAE_ANALYZER_VERSION, "analyzer version",
          std::to_string(info.analyzerVersion));
    check(info.complete == 1, "whole file decoded");
    check(info.durationMs > 29000 && info.durationMs < 31000, "duration",
          std::to_string(info.durationMs));
    check(info.sampleRate == 44100 && info.channels == 2, "format",
          std::to_string(info.sampleRate) + " Hz / " + std::to_string(info.channels) +
              " ch");
    check(info.bpmValid == 1, "BPM is valid", "confidence " + std::to_string(info.bpmConfidence));
    check(std::fabs(info.bpm - 120.0) < 2.0, "BPM is 120", std::to_string(info.bpm));
    check(info.bpmConfidence > 0.3, "BPM confidence",
          std::to_string(info.bpmConfidence));
    check(info.loudnessDb < 0.0, "loudness is a level", std::to_string(info.loudnessDb));
    check(info.dynamicRangeDb > 0.0, "dynamic range is positive",
          std::to_string(info.dynamicRangeDb));
    check(info.bucketCount == 1000 && written == 2000, "peaks count",
          std::to_string(written));
    bool peakSeen = false;
    for (float v : peaks) {
        if (!std::isfinite(v) || v < -1.01f || v > 1.01f) {
            check(false, "peaks stay in range", std::to_string(v));
            break;
        }
        if (std::fabs(v) > 0.05f) peakSeen = true;
    }
    check(peakSeen, "peaks contain signal");
    check(info.lufsValid == 1, "LUFS is measured");
    check(info.lufsShortTermValid == 1, "a 30 s file has a short-term window");
    check(info.lufsIntegrated < 0.0 && info.lufsIntegrated > -60.0,
          "LUFS integrated is a level", std::to_string(info.lufsIntegrated));

    // BS.1770 calibration through the C ABI: a stereo 997 Hz sine at -23 dBFS
    // reads -23 LUFS.
    const char* tonePath = "analysis_tone.wav";
    if (!writeToneWav(tonePath)) {
        std::printf("could not write %s\n", tonePath);
        return 1;
    }
    NAE_AnalysisInfo tone;
    std::vector<float> tonePeaks;
    int32_t toneWritten = 0;
    check(analyze(tonePath, &tone, &tonePeaks, &toneWritten), "tone fixture analyzes");
    check(tone.lufsValid == 1, "tone LUFS is valid");
    check(std::fabs(tone.lufsIntegrated - (-23.0)) < 0.1,
          "tone reads -23 LUFS", std::to_string(tone.lufsIntegrated));
    check(std::fabs(tone.lufsMomentaryMax - (-23.0)) < 0.1,
          "momentary max reads -23 LUFS", std::to_string(tone.lufsMomentaryMax));

    if (args.size() > 1) {
        NAE_AnalysisInfo tags;
        std::vector<float> tagPeaks;
        int32_t tagWritten = 0;
        check(analyze(args[1].c_str(), &tags, &tagPeaks, &tagWritten),
              "tagged fixture analyzes");
        check(std::strcmp(tags.title, "Test Title") == 0, "title", tags.title);
        check(std::strcmp(tags.artist, "Test Artist") == 0, "artist", tags.artist);
        check(std::strcmp(tags.album, "Test Album") == 0, "album", tags.album);
        check(std::strcmp(tags.albumArtist, "Test Artist") == 0, "album artist",
              tags.albumArtist);
        check(std::strcmp(tags.track, "3/12") == 0, "track", tags.track);
        check(std::strcmp(tags.disc, "1/2") == 0, "disc", tags.disc);
        check(std::strcmp(tags.year, "2026") == 0, "year", tags.year);
        check(std::strcmp(tags.genre, "Test Genre") == 0, "genre", tags.genre);
        check(std::strcmp(tags.composer, "Test Composer") == 0, "composer", tags.composer);
        check(std::strcmp(tags.comment, "Hello") == 0, "comment", tags.comment);
        check(tags.rgTrackValid == 1 && std::fabs(tags.rgTrackGainDb + 6.0) < 0.1,
              "ReplayGain track", std::to_string(tags.rgTrackGainDb));
    }

    std::remove(clickPath);
    std::printf("\n%s\n", failures == 0 ? "All analysis checks passed."
                                        : "Some analysis checks FAILED.");
    return failures == 0 ? 0 : 1;
}
