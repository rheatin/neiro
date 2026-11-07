// Does the engine actually play on macOS?
//
// The smoke test only asks that the library loads. This opens a file through the
// same entry point a host uses, plays it in slices the way a host does, and
// checks that the position moved -- the one consequence that separates "the
// device took frames" from "the engine played a file".
#include "nae.h"

#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: macos_play_check <file>\n");
        return 2;
    }
    NAE_Handle* h = nullptr;
    int32_t rate = 0, src = 0, ch = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 0, fileCh = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, argv[1], NAE_OUTPUT_NATIVE, 0, 0, &h, &rate, &src,
                                    &ch, &bits, &valid, &isFloat, &resampling, &dithered,
                                    &bitPerfect, &fileCh, &durationMs, &latencyMs);
    if (rc != NAE_OK || !h) {
        std::printf("open failed rc=%d %s\n", rc, NAE_GetLastError());
        return 1;
    }
    std::printf("open ok: %d Hz (source %d), %d ch, %d container / %d valid, float=%d\n",
                rate, src, ch, bits, valid, isFloat);
    std::printf("  resampling=%d dithered=%d bitPerfect=%d duration=%.0f ms latency=%.1f ms\n",
                resampling, dithered, bitPerfect, durationMs, latencyMs);

    const double before = NAE_Position(h);
    for (int i = 0; i < 10; ++i) {
        const int32_t playRc = NAE_Play(h, 200);
        if (playRc != NAE_OK) {
            std::printf("play failed at slice %d: rc=%d %s\n", i, playRc, NAE_GetLastError());
            NAE_Close(h);
            return 1;
        }
    }
    const double after = NAE_Position(h);
    std::printf("position %.3f -> %.3f s\n", before, after);

    NAE_Close(h);
    NAE_Shutdown();
    const bool ok = after > before + 0.5;
    std::printf("%s\n", ok ? "playback check passed" : "playback check FAILED (position did not advance)");
    return ok ? 0 : 1;
}
