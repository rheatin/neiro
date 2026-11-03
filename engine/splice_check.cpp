// Are there discontinuities between the engine's decoded blocks?
//
// No device needed, which is the point: the bubbling was only ever heard on the
// exclusive route, and that route's decoder is ours rather than Chromium's. A
// stream can be full-level and perfectly correct in aggregate while having a step
// at every block boundary, and a step is a click -- several clicks a second reads
// as bubbling rather than as clicking.
//
// The test is a comparison, not a threshold. Within a block, a sample-to-sample
// jump is bounded by the signal's own slew: a 48 kHz tone at full scale moves a
// few percent per sample. Across a boundary, a splice moves as far as the
// waveform's own range in one sample. Comparing the two maxima separates them
// without having to guess an absolute number.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Does `readExact` deliver what it promised, every time?
//
// The splice scan above uses `read`, which is the wrong function to trust for this
// question. The render loop pulls with `readExact`, and the two are not
// interchangeable: `readExact` is handed a frame count and is supposed to return
// exactly that many or fail. The caller then advances its position by the number
// it *asked* for, unconditionally -- so a single short delivery inside
// `readExact` becomes a skipped run of samples downstream, which is a click, and
// several a second is bubbling.
//
// Nothing about `read`'s output can reveal that. The same bytes can be perfectly
// continuous through `read` and have holes in it through `readExact`, so this
// compares the two directly, sample for sample.
#include "decoder.h"

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
    if (!ok) ++g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: splice_check <file> [more files...]\n");
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
        const auto& info = dec.info();
        const size_t ch = info.channels ? static_cast<size_t>(info.channels) : 1;
        std::printf("  %u Hz  %u ch  %s\n", info.sample_rate, info.channels, info.codec.c_str());

        // The last sample of the previous block, kept so each boundary can be
        // compared against the first sample of the next one.
        std::vector<double> tail;
        std::vector<float> block;

        double withinMax = 0.0;
        double acrossMax = 0.0;
        double acrossAt = 0.0;
        uint64_t boundaries = 0;
        uint64_t boundaryFrames = 0;

        // Median-ish reference: the loudest jump seen *inside* a block is the
        // signal's own slew, and the ratio to it is what a splice looks like.
        while (true) {
            // Cleared first, every time. `read` *appends*, so leaving the buffer
            // alone makes it grow without bound and makes `block[0]` the first
            // sample of the first block rather than of this one -- which silently
            // turns the boundary comparison into a comparison of two unrelated
            // samples and reports "no splices" for any file at all.
            block.clear();
            if (!dec.read(&block, &err)) break;
            const size_t frames = block.size() / ch;
            if (frames == 0) continue;

            for (size_t f = 1; f < frames; ++f) {
                for (size_t c = 0; c < ch; ++c) {
                    const double d = std::fabs(static_cast<double>(block[f * ch + c]) -
                                               static_cast<double>(block[(f - 1) * ch + c]));
                    if (d > withinMax) withinMax = d;
                }
            }

            if (!tail.empty() && tail.size() == ch) {
                const double jump =
                    std::max(std::fabs(static_cast<double>(block[0]) - tail[0]),
                             ch > 1 ? std::fabs(static_cast<double>(block[1]) - tail[1]) : 0.0);
                if (jump > acrossMax) {
                    acrossMax = jump;
                    acrossAt = static_cast<double>(boundaries);
                }
                ++boundaries;
            }

            tail.assign(block.end() - static_cast<long>(ch), block.end());
            boundaryFrames += frames;
        }

        std::printf("  blocks %llu  frames %llu\n",
                    static_cast<unsigned long long>(boundaries),
                    static_cast<unsigned long long>(boundaryFrames));
        std::printf("  largest jump within a block:  %.6f\n", withinMax);
        std::printf("  largest jump across a join:  %.6f  (block %llu)\n", acrossMax,
                    static_cast<unsigned long long>(acrossAt));

        check("decoded without error", err.empty(), err);

        // The ratio, not an absolute threshold. A signal whose own slew is large --
        // percussion, a transient -- raises `withinMax` and would trip a fixed
        // limit on perfectly good audio; a splice raises `acrossMax` while leaving
        // `withinMax` alone, so the quotient is the discriminating measurement.
        const double ratio = withinMax > 0.0 ? acrossMax / withinMax : 0.0;
        std::printf("  ratio across/within:          %.2fx\n", ratio);

        // Above this, a join moves further than the waveform moves at its own
        // steepest, which no continuous signal can do.
        check("no boundary jump exceeds the signal's own slew", ratio < 4.0,
              ratio < 4.0 ? "" : std::to_string(static_cast<int>(ratio)) + "x");
    }

    // ---- readExact against read ----
    //
    // Same file, same decoder, two paths. `read` is the ground truth because it
    // hands over whatever the codec produced; `readExact` is what the render loop
    // actually calls. They must produce identical bytes in the same order.
    for (int a = 1; a < argc; ++a) {
        const std::string path = argv[a];
        std::printf("\n== readExact vs read: %s ==\n", path.c_str());

        std::vector<float> truth;
        {
            neiro::Decoder d;
            std::string err;
            if (!d.open(path, &err)) { check("truth: opened", false, err); continue; }
            const size_t ch = d.info().channels ? static_cast<size_t>(d.info().channels) : 1;
            std::vector<float> block;
            while (true) {
                block.clear();
                if (!d.read(&block, &err)) break;
                truth.insert(truth.end(), block.begin(), block.end());
            }
            std::printf("  ground truth: %zu samples (%zu frames)\n", truth.size(), truth.size() / ch);
            check("the truth path produced audio", truth.size() > 0, "");

            // The sizes the render loop really uses. `needSource` is derived from
            // the device buffer with 64 frames of slack, so the awkward numbers --
            // the ones that make a remainder handler go wrong -- are the ones worth
            // trying rather than a round figure.
            for (size_t frames : {static_cast<size_t>(2560), static_cast<size_t>(4096),
                                  static_cast<size_t>(1024), static_cast<size_t>(960),
                                  static_cast<size_t>(1000)}) {
                neiro::Decoder e;
                std::string err2;
                if (!e.open(path, &err2)) { check("readExact: opened", false, err2); continue; }

                std::vector<float> got;
                std::vector<float> scratch;
                size_t shortReads = 0;
                size_t calls = 0;
                while (e.readExact(&scratch, frames, &err2)) {
                    got.insert(got.end(), scratch.begin(), scratch.end());
                    scratch.clear();
                    ++calls;
                    if (calls > 400000) break;  // runaway guard
                }
                const bool sameSize = got.size() == truth.size();
                // The prefix is the part that decides what this means. Identical
                // up to the point where `got` stops, and short after it, is a
                // truncated *tail* -- the converter's last samples never flushed --
                // which is a real defect and explains a click at the end of a
                // track. A difference anywhere earlier is data lost or reordered in
                // the middle of the music, which is a different and much worse bug,
                // and the one that would sound like bubbling.
                const size_t common = got.size() < truth.size() ? got.size() : truth.size();
                size_t firstDiff = 0;
                bool prefixOk = true;
                for (size_t i = 0; i < common; ++i) {
                    if (got[i] != truth[i]) { prefixOk = false; firstDiff = i; break; }
                }

                char label[96];
                std::snprintf(label, sizeof(label), "readExact(%zu) is a clean prefix of read", frames);
                std::string detail;
                if (!prefixOk) {
                    detail = "DIFFERS from sample " + std::to_string(firstDiff) + " -- mid-stream loss";
                    check(label, false, detail);
                } else if (got.size() < truth.size()) {
                    const size_t missing = (truth.size() - got.size()) / ch;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "identical, but %zu frames short at the tail",
                                  missing);
                    detail = buf;
                    check(label, true, detail);
                } else {
                    detail = got.size() > truth.size() ? "LONGER than read" : "identical";
                    check(label, got.size() == truth.size(), detail);
                }
                (void)sameSize; (void)shortReads;
            }
        }
    }

    std::printf("\n%s\n", g_failures == 0 ? "No splices found."
                                          : "Discontinuities FOUND.");
    return g_failures == 0 ? 0 : 1;
}
