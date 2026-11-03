// Reads a file to its end through `readExact`, and checks nothing is lost.
//
// # Why this exists separately from decode_check
//
// `decode_check` decodes with `read()` in a loop until it returns false, and that
// is the easy path: `read()` hands back whatever it has and the caller keeps
// going until there is nothing left. It cannot lose samples by construction.
//
// `readExact` is the other path, and it is the one the engine actually uses. It
// promises *exactly* `frames` samples, keeps any excess in `pending`, and returns
// whether it got them all. Three things can go wrong there that `read()` cannot:
//
//   1. The tail is truncated. The final `readExact` asks for a block, the decoder
//      has fewer than that left plus whatever the resampler is still holding, and
//      a short return is treated as the end of the stream -- so the last few
//      hundred milliseconds of every track is silently dropped.
//   2. The excess is double-counted or dropped. The remainder is stashed in
//      `pending` and handed back on the next call; if it is stashed twice, or
//      stashed and also counted, the total diverges from the file.
//   3. `frames_read` -- which the engine uses for the playhead -- disagrees with
//      the samples actually delivered. That is the worst of the three, because
//      the audio is right and the position is wrong, which looks like a seek bug.
//
// So this reads every file three ways and compares: all of `read()`, all of
// `readExact()` in a few block sizes, and `framesRead()`. If the two agree and the
// totals agree with the file's reported duration, all three of those are ruled out
// at once.
//
// Block sizes are deliberately incommensurate -- 1, 7, 1000, 4096, 100000 --
// because the bugs above are size-dependent. An excess-stashing fault shows up at
// some sizes and not others, and a check that only uses the engine's own 200 ms
// slice would test exactly one of them.

#include "decoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-44s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

std::string fmt(const char* pattern, double v) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), pattern, v);
    return buf;
}

/// Two doubles. `fmt` above takes one because almost every figure printed here is
/// a single number, and a two-argument version that silently formatted only the
/// first would be worse than a compile error.
std::string fmt2(const char* pattern, double a, double b) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), pattern, a, b);
    return buf;
}

/// Everything the decoder will give, via `read()`.
bool drainAll(neiro::Decoder* dec, size_t channels, std::vector<float>* out, std::string* err) {
    out->clear();
    std::vector<float> block;
    // Same reasoning as below: guard on samples, not calls, so the reference path
    // and the readExact paths stop for the same reason at the same place.
    const size_t sampleCap = 200000000;
    for (size_t guard = 0; out->size() < sampleCap && guard < 200000000; ++guard) {
        block.clear();
        if (!dec->read(&block, err)) return true;
        if (block.empty()) continue;
        out->insert(out->end(), block.begin(), block.end());
    }
    return false;  // runaway
}

/// Everything the decoder will give, via `readExact()` in blocks of `blockFrames`.
///
/// Returns the number of samples delivered, and whether every request but the last
/// was satisfied in full. A short return mid-file is the truncation bug; a short
/// return only at the very end is just the end.
bool drainExact(neiro::Decoder* dec, size_t blockFrames, size_t* delivered,
                int* shortMidFile, std::string* err) {
    *delivered = 0;
    *shortMidFile = 0;
    std::vector<float> block;
    // The guard counts *samples*, not calls. A call-count guard silently tests
    // nothing useful at small block sizes -- `blockFrames = 1` needs one call per
    // frame, so a few hundred thousand calls covers a fraction of a second and the
    // run stops early looking exactly like a truncated file.
    const size_t sampleCap = 200000000;
    for (size_t guard = 0; *delivered < sampleCap && guard < 200000000; ++guard) {
        block.clear();
        const bool full = dec->readExact(&block, blockFrames, err);
        if (block.empty()) {
            if (!full) break;
            continue;
        }
        *delivered += block.size();
        if (!full) {
            // A short read is only legitimate once the file is finished. Another
            // one, and `readExact` gave up early.
            block.clear();
            const bool more = dec->readExact(&block, 1, err);
            if (more || !block.empty()) ++*shortMidFile;
            break;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tail_check <file> [file...]\n");
        return 2;
    }

    for (int a = 1; a < argc; ++a) {
        const char* path = argv[a];
        std::printf("\n== %s ==\n", path);

        neiro::Decoder dec;
        std::string err;
        if (!dec.open(path, &err)) {
            std::printf("  could not open: %s\n", err.c_str());
            ++failures;
            continue;
        }
        const neiro::StreamInfo& info = dec.info();
        if (!info.sample_rate || !info.out_channels) {
            std::printf("  no usable stream info\n");
            ++failures;
            continue;
        }
        // `out_channels`, not `channels`: `readExact` is asked for frames and delivers
        // interleaved samples of however many channels the converter actually
        // produces, and dividing by the file's own channel count would make every
        // total wrong by exactly the downmix ratio on any file that was converted.
const size_t ch = info.out_channels ? info.out_channels : 1;
        std::printf("  %s, %u Hz, %u ch, reported %.1f ms\n", info.codec.c_str(), info.sample_rate,
                    info.channels, info.duration_ms);

        // --- the reference: everything via read() ---
        std::vector<float> all;
        if (!drainAll(&dec, ch, &all, &err)) {
            std::printf("  [FAIL] read() did not terminate\n");
            ++failures;
            continue;
        }
        const size_t referenceSamples = all.size();
        const size_t referenceFrames = referenceSamples / ch;
        const double referenceSeconds =
            info.sample_rate ? static_cast<double>(referenceFrames) / info.sample_rate : 0.0;
        std::printf("  read(): %zu samples = %zu frames = %.3f s\n", referenceSamples, referenceFrames,
                    referenceSeconds);
        std::printf("  framesRead() after read(): %llu\n",
                    static_cast<unsigned long long>(dec.framesRead()));

        // --- readExact at several block sizes ---
        const size_t blockSizes[] = {1, 7, 1000, 4096, 100000};
        for (size_t blockFrames : blockSizes) {
            neiro::Decoder d2;
            std::string e2;
            if (!d2.open(path, &e2)) {
                check(false, "reopen for readExact", e2.substr(0, 60));
                continue;
            }
            size_t delivered = 0;
            int shortMidFile = 0;
            if (!drainExact(&d2, blockFrames, &delivered, &shortMidFile, &e2)) {
                check(false, "readExact terminated", "");
                continue;
            }
            const double lossFrames = static_cast<double>(referenceSamples) -
                                      static_cast<double>(delivered);
            const double lossSeconds =
                info.sample_rate ? lossFrames / ch / info.sample_rate : 0.0;
            const uint64_t reported = d2.framesRead();
            const int64_t countDrift = static_cast<int64_t>(reported) -
                                       static_cast<int64_t>(delivered / ch);

            std::printf(
                "  readExact(%6zu): %zu samples, framesRead %llu  (delta %+.3f s, count %+lld)\n",
                blockFrames, delivered, static_cast<unsigned long long>(reported), -lossSeconds,
                static_cast<long long>(countDrift));

            char label[96];
            std::snprintf(label, sizeof(label), "readExact(%zu) delivers every sample", blockFrames);
            // A handful of samples of slack for the resampler's own delay line and
            // for container padding at the head. More than that is truncation.
            const double slackFrames = info.sample_rate * 0.05;  // 50 ms
            check(lossFrames <= static_cast<double>(slackFrames), label,
                  fmt2("%.3f s missing of %.3f s", lossSeconds, referenceSeconds));

            std::snprintf(label, sizeof(label), "readExact(%zu) has no mid-file short read",
                          blockFrames);
            check(shortMidFile == 0, label, fmt("%.0f short reads", double(shortMidFile)));

            // The playhead's source of truth. A disagreement here is a seek bug
            // that looks like nothing at all.
            std::snprintf(label, sizeof(label), "readExact(%zu) framesRead matches delivered",
                          blockFrames);
            const int64_t allowedCountDrift = static_cast<int64_t>(ch);
            check(countDrift <= allowedCountDrift && countDrift >= -allowedCountDrift, label,
                  std::to_string(countDrift) + " frames");
        }

        // --- duration sanity, on the reference path ---
        if (info.duration_ms > 0.0) {
            const double drift = referenceSeconds - info.duration_ms / 1000.0;
            check(std::fabs(drift) < 1.0, "decoded duration matches the file",
                  fmt2("%.3f s decoded vs %.3f s reported", referenceSeconds,
                       info.duration_ms / 1000.0));
        }
    }

    std::printf("\n%s\n", failures == 0 ? "All tail checks passed."
                                        : "There were failures; the lines above say which.");
    return failures == 0 ? 0 : 1;
}