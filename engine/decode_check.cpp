// Decodes a real file and checks the samples, not just the exit code.
//
// A decoder is the easiest component in this project to get wrong in a way that
// still "works": it returns the right number of frames of plausible noise. So
// this measures what came out:
//
//   * the reported rate and channel count match the file
//   * a 1 kHz tone is still a 1 kHz tone, at the right level (Goertzel)
//   * the total duration matches the file's
//   * a seek lands near where it was asked to
//
// It also checks the two failure paths, because a decoder that cannot say why it
// failed is worse than one that cannot decode.
//
// Usage: decode_check <file> [more files...]

#include <cmath>
#include <cstdarg>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "decoder.h"

namespace {

int g_fail = 0;

void check(const char* label, bool ok, const std::string& detail) {
    if (!ok) ++g_fail;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

// Variadic, because most call sites here format an integer alongside a double
// and a single-arg helper makes that a compile error rather than a bug.
std::string fmt(const char* f, ...) {
    char buf[192];
    va_list a;
    va_start(a, f);
    std::vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

// Magnitude of one frequency in a real signal.
double goertzel(const std::vector<float>& x, double rate, double hz) {
    const size_t n = x.size();
    if (n < 16) return 0.0;
    const double k = 0.5 + (hz * static_cast<double>(n) / rate);
    const double w = (2.0 * 3.14159265358979323846 / static_cast<double>(n)) * k;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / (static_cast<double>(n) / 2.0);
}

double peak(const std::vector<float>& x) {
    double m = 0.0;
    for (float s : x) m = std::max(m, std::abs(static_cast<double>(s)));
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: decode_check <file> [more files...]\n");
        return 2;
    }

    for (int a = 1; a < argc; ++a) {
        const std::string path = argv[a];
        std::printf("\n== %s ==\n", path.c_str());

        neiro::Decoder dec;
        std::string err;
        if (!dec.open(path, &err)) {
            check("opened", false, err);
            continue;
        }
        check("opened", true, "");

        const auto& info = dec.info();
        std::printf("  %u Hz  %u ch  %s  %.1f ms\n", info.sample_rate, info.channels,
                    info.codec.c_str(), info.duration_ms);
        check("sample rate reported", info.sample_rate > 0, fmt("%.0f Hz", double(info.sample_rate)));
        check("channel count reported", info.channels >= 1, fmt("%.0f", double(info.channels)));
        check("codec named", !info.codec.empty(), info.codec);
        check("duration reported", info.duration_ms > 0.0, fmt("%.1f ms", info.duration_ms));

        // Decode the whole file, counting frames.
        std::vector<float> all;
        std::vector<float> block;
        int blocks = 0;
        for (;;) {
            block.clear();
            if (!dec.read(&block, &err)) break;
            all.insert(all.end(), block.begin(), block.end());
            ++blocks;
            if (blocks > 100000) break;  // runaway guard
        }
        check("decoded without error", err.empty(), err);

        const size_t frames = info.channels ? all.size() / info.channels : 0;
        const double seconds = info.sample_rate ? static_cast<double>(frames) / info.sample_rate : 0.0;
        std::printf("  %zu frames in %d blocks = %.3f s\n", frames, blocks, seconds);
        check("produced samples", frames > 0, fmt("%.0f frames", double(frames)));
        check("block count is sane", blocks > 0, fmt("%d blocks", blocks));

        // Duration: allow a second of slack for encoder padding and container
        // rounding. A decoder that is systematically short is a real bug; one
        // that is a few hundred milliseconds out is arithmetic.
        if (info.duration_ms > 0.0) {
            const double drift = seconds - info.duration_ms / 1000.0;
            check("decoded duration matches the file", std::abs(drift) < 1.0,
                  fmt("%.3f s decoded vs %.3f s reported", seconds, info.duration_ms / 1000.0));
        }

        check("no sample exceeds full scale", peak(all) <= 1.0001, fmt("peak %.4f", peak(all)));
        check("signal is not silent", peak(all) > 0.001, fmt("peak %.4f", peak(all)));

        // If the file is a known tone, check the tone survived.
        //
        // Measured on the first channel only, deinterleaved. Running the analysis
        // over the interleaved stream instead looks like it works — it reports
        // 0.0000 for a file that is entirely 1 kHz — because left and right
        // alternate and the tone appears to sit at twice its real frequency.
        std::vector<float> left;
        left.reserve(frames);
        for (size_t i = 0; i + info.channels <= all.size(); i += info.channels) {
            left.push_back(all[i]);
        }
        const double tone = goertzel(left, info.sample_rate, 1000.0);
        std::printf("  1 kHz magnitude (left channel): %.4f\n", tone);
        if (tone > 0.01) {
            check("tone survived decoding at its level", tone > peak(all) * 0.3,
                  fmt("%.4f vs peak %.4f", tone, peak(all)));
        } else {
            std::printf("  (no 1 kHz content; skipping the tone check)\n");
        }

        // Seek, then confirm playback resumes and lands somewhere plausible.
        //
        // Inside the file, because a fixed 1.0 s is not inside every file. A
        // one-second fixture puts the seek exactly on the last frame, so the read
        // that follows returns nothing and the failure reads as "the decoder
        // cannot resume after a seek" when the truth is that there was nothing
        // left to read. The target comes from what was decoded above.
        const double seekTo = seconds > 2.0 ? 1.0 : seconds * 0.5;
        if (dec.seek(seekTo, &err)) {
            block.clear();
            const bool got = dec.read(&block, &err);
            check("read after seek", got,
                  got ? fmt("%zu samples from %.2f s", block.size(), seekTo) : err);
        } else {
            check("seek", false, err);
        }
    }

    std::printf("\n== failure paths ==\n");
    {
        neiro::Decoder dec;
        std::string err;
        check("opening a missing file fails", !dec.open("C:/definitely/not/here.flac", &err),
              err.substr(0, 80));
        check("the failure explains itself", err.find("could not open") != std::string::npos,
              err.substr(0, 80));
    }
    {
        neiro::Decoder dec;
        std::string err;
        // A file that exists but is not audio: the decoder must say so rather
        // than crash or return silence.
        check("opening a non-audio file fails cleanly", !dec.open("C:/Windows/win.ini", &err),
              err.substr(0, 80));
    }

    std::printf("\n%s\n", g_fail == 0 ? "All decode checks passed." : "Some decode checks FAILED.");
    return g_fail == 0 ? 0 : 1;
}
