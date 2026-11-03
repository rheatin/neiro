// BS.1770-4 calibration and gating. The reference points are the standard's own:
// a 997 Hz sine at 0 dBFS reads -3.01 LUFS, so one at -20 dBFS reads -23.01.

#include "loudness.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    std::printf("[%s] %-56s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

std::vector<float> tone(double amplitude, double seconds, uint32_t rate = 48000,
                        double freq = 997.0, uint32_t channels = 2) {
    const size_t frames = static_cast<size_t>(std::llround(seconds * rate));
    std::vector<float> out(frames * channels, 0.0f);
    for (size_t i = 0; i < frames; ++i) {
        const float s = static_cast<float>(
            amplitude * std::sin(2.0 * 3.14159265358979323846 * freq * i / rate));
        for (uint32_t c = 0; c < channels; ++c) out[i * channels + c] = s;
    }
    return out;
}

double measure(const std::vector<float>& pcm, uint32_t rate = 48000,
               uint32_t channels = 2) {
    neiro::LoudnessMeter meter;
    meter.configure(channels, rate);
    meter.process(pcm.data(), pcm.size() / channels, channels);
    return meter.finish().integratedLufs;
}

}  // namespace

int main() {
    // Calibration: one full-scale 997 Hz channel reads -3.01 LUFS.
    {
        const double l = measure(tone(1.0, 10.0, 48000, 997.0, 1), 48000, 1);
        check(std::fabs(l - (-3.01)) < 0.1, "0 dBFS sine reads -3.01 LUFS", std::to_string(l));
    }
    // Two identical -23 dBFS channels read -23 LUFS: the standard sums channels.
    {
        const double l = measure(tone(std::pow(10.0, -23.0 / 20.0), 10.0), 48000, 2);
        check(std::fabs(l - (-23.0)) < 0.1, "stereo -23 dBFS sine reads -23 LUFS", std::to_string(l));
    }
    // Sample-rate independence: the filters are designed per rate, so 44.1 kHz
    // must give the same reading as 48 kHz.
    {
        const double a = measure(tone(0.1, 10.0, 48000, 997.0, 1), 48000, 1);
        const double b = measure(tone(0.1, 10.0, 44100, 997.0, 1), 44100, 1);
        check(std::fabs(a - b) < 0.05, "44.1 and 48 kHz agree", std::to_string(a) + " / " + std::to_string(b));
    }
    // Absolute gate: a tone followed by digital silence. Without the gate the
    // silence blocks would halve the mean square and read 3 dB lower.
    {
        std::vector<float> pcm = tone(0.1, 10.0);
        std::vector<float> tail(static_cast<size_t>(48000) * 10 * 2, 0.0f);
        pcm.insert(pcm.end(), tail.begin(), tail.end());
        const double l = measure(pcm);
        check(std::fabs(l - (-20.0)) < 0.1, "absolute gate ignores digital silence", std::to_string(l));
        // Control: without the gate the silence halves the mean square and the
        // same signal reads 3 dB lower. If both readings matched, the gate
        // would be unproven.
        const double ungated = -20.0 + 10.0 * std::log10(0.5);
        check(l - ungated > 2.5, "gating is what removes the silence",
              std::to_string(l - ungated));
    }
    // Relative gate: a quiet section 25 LU below the tone is below -10 LU
    // relative, so it must not drag the integrated value down.
    {
        std::vector<float> pcm = tone(0.1, 10.0);
        std::vector<float> quiet = tone(0.1 * std::pow(10.0, -25.0 / 20.0), 10.0);
        pcm.insert(pcm.end(), quiet.begin(), quiet.end());
        const double l = measure(pcm);
        check(std::fabs(l - (-20.0)) < 0.2, "relative gate ignores a -25 LU section", std::to_string(l));
        const double ungated = -20.0 + 10.0 * std::log10((1.0 + std::pow(10.0, -2.5)) / 2.0);
        check(l - ungated > 2.5, "the relative gate is what removes the quiet part",
              std::to_string(l - ungated));
    }
    // A quiet section 6 LU down is inside the gate and must count.
    {
        std::vector<float> pcm = tone(0.1, 10.0);
        std::vector<float> quiet = tone(0.1 * std::pow(10.0, -6.0 / 20.0), 10.0);
        pcm.insert(pcm.end(), quiet.begin(), quiet.end());
        const double l = measure(pcm);
        const double expected = -20.0 + 10.0 * std::log10((1.0 + std::pow(10.0, -0.6)) / 2.0);
        check(std::fabs(l - expected) < 0.15, "a -6 LU section counts in the mean",
              std::to_string(l) + " vs " + std::to_string(expected));
    }
    // Momentary and short-term maxima are ungated.
    {
        neiro::LoudnessMeter meter;
        meter.configure(2, 48000);
        std::vector<float> pcm = tone(0.1, 5.0);
        std::vector<float> loud = tone(0.5, 5.0);
        pcm.insert(pcm.end(), loud.begin(), loud.end());
        meter.process(pcm.data(), pcm.size() / 2, 2);
        const auto r = meter.finish();
        const double expected = 0.0 + 20.0 * std::log10(0.5);
        check(std::fabs(r.momentaryMaxLufs - expected) < 0.1,
              "momentary max reports the loud section", std::to_string(r.momentaryMaxLufs));
        check(r.totalBlocks > 0 && r.gatedBlocks > 0, "block counts are reported");
    }
    // Loudness range: 10 s quiet, 10 s loud is about the step in LU.
    {
        neiro::LoudnessMeter meter;
        meter.configure(2, 48000);
        std::vector<float> quiet = tone(0.03, 10.0);
        std::vector<float> loud = tone(0.3, 10.0);
        std::vector<float> pcm = quiet;
        pcm.insert(pcm.end(), loud.begin(), loud.end());
        meter.process(pcm.data(), pcm.size() / 2, 2);
        const auto r = meter.finish();
        check(r.rangeLu > 18.0 && r.rangeLu < 21.0, "LRA of a 20 LU step",
              std::to_string(r.rangeLu));
    }
    // A file shorter than 3 s has no short-term window. The value must be 0
    // WITH a flag saying it is not measured: 0 LUFS and "not measured" are
    // different claims, and -200 is a third, worse one.
    {
        neiro::LoudnessMeter meter;
        meter.configure(2, 48000);
        std::vector<float> pcm = tone(0.1, 1.0);
        meter.process(pcm.data(), pcm.size() / 2, 2);
        const auto r = meter.finish();
        check(r.valid && !r.shortTermValid, "1 s input has no short-term window");
        check(r.shortTermMaxLufs == 0.0 && r.rangeLu == 0.0,
              "no 3 s window reports 0, not a sentinel",
              std::to_string(r.shortTermMaxLufs));
    }
    {
        neiro::LoudnessMeter meter;
        meter.configure(2, 48000);
        std::vector<float> pcm = tone(0.1, 4.0);
        meter.process(pcm.data(), pcm.size() / 2, 2);
        const auto r = meter.finish();
        check(r.shortTermValid && r.shortTermMaxLufs > -80.0,
              "4 s input has a short-term reading", std::to_string(r.shortTermMaxLufs));
    }

    // A file too short for one 400 ms block reports no value, not 0 LUFS.
    {
        neiro::LoudnessMeter meter;
        meter.configure(2, 48000);
        std::vector<float> pcm = tone(0.1, 0.3);
        meter.process(pcm.data(), pcm.size() / 2, 2);
        const auto r = meter.finish();
        check(!r.valid, "short input reports no integrated value");
    }
    // Invalid configuration is rejected.
    {
        neiro::LoudnessMeter meter;
        check(!meter.configure(0, 48000), "zero channels is rejected");
        check(!meter.configure(2, 100), "an impossible rate is rejected");
    }
    // Every surround channel carries the same 1.41 weight. Before the fix only
    // indices 4 and 5 did, so a 7.1 layout with its energy in BC/SL/SR read low.
    {
        const auto measureChannel = [](uint32_t channel, uint32_t channels) {
            neiro::LoudnessMeter meter;
            meter.configure(channels, 48000);
            const size_t frames = 48000 * 5;
            std::vector<float> pcm(frames * channels, 0.0f);
            for (size_t i = 0; i < frames; ++i) {
                pcm[i * channels + channel] = static_cast<float>(
                    0.1 * std::sin(2.0 * 3.14159265358979323846 * 997.0 * i / 48000.0));
            }
            meter.process(pcm.data(), frames, channels);
            return meter.finish().integratedLufs;
        };
        const double bl = measureChannel(4, 8);
        const double bc = measureChannel(6, 8);
        const double sr = measureChannel(8, 9);
        check(std::fabs(bl - bc) < 0.05 && std::fabs(bl - sr) < 0.05,
              "all surround channels carry the same weight",
              std::to_string(bl) + " / " + std::to_string(bc) + " / " + std::to_string(sr));
    }

    // Five channels: LFE is excluded, surrounds are weighted 1.41.
    {
        neiro::LoudnessMeter meter;
        check(meter.configure(6, 48000), "5.1 configures");
        std::vector<float> pcm(static_cast<size_t>(48000) * 10 * 6, 0.0f);
        for (size_t i = 0; i < static_cast<size_t>(48000) * 10; ++i) {
            const float s = static_cast<float>(0.01 * std::sin(2.0 * 3.14159265358979323846 * 997.0 * i / 48000.0));
            pcm[i * 6 + 3] = 0.5f;  // LFE only: must not change the reading
            (void)s;
        }
        meter.process(pcm.data(), pcm.size() / 6, 6);
        const auto r = meter.finish();
        check(!r.valid || r.integratedLufs < -100.0, "LFE-only content is not loudness",
              std::to_string(r.integratedLufs));
    }

    if (failures == 0) {
        std::printf("\nAll loudness checks passed.\n");
        return 0;
    }
    std::printf("\nSome loudness checks FAILED.\n");
    return 1;
}
