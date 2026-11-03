// 用引擎播放一个文件若干秒。只为耳朵用，所以只报最少的信息。
//
// 存在的理由：自写 DSD 解码器默认关闭，而判断那段「8-44 kHz 平在 -11 dB」的平台是不是
// 真实信号，最后一步要靠耳朵——频谱测量在这里已经走到头了：组件本身测到阻带 -156 dB，
// 而引擎路径上有一段解释不了的平台。
//
// 用法: play <file> <seconds>

#include "nae.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: play <file> <seconds>\n");
        return 1;
    }
    const char* path = argv[1];
    const int seconds = std::atoi(argv[2]);

    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc =
        NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &h, &rate, &srcRate, &ch, &bits,
                     &valid, &isFloat, &resampling, &dithered, &bitPerfect, &fileChannels,
                     &durationMs, &latencyMs);
    if (rc != 0 || !h) {
        std::printf("打不开 %s  rc=%d %s\n", path, rc, NAE_GetLastError());
        return 1;
    }
    const char* mode = std::getenv("NEIRO_DSD_DECODER") ? "自写 DSD 级联" : "FFmpeg DSD 解码器";
    std::printf("播放 %s\n", path);
    std::printf("  路径 %s\n", mode);
    std::printf("  源 %d Hz -> 设备 %d Hz，%d ch，%d bit (valid %d)\n", srcRate, rate, ch, bits,
                valid);
    std::printf("  resampling=%d dithered=%d bitPerfect=%d，时长 %.1f s\n", resampling, dithered,
                bitPerfect, durationMs / 1000.0);
    std::printf("  播放 %d 秒…\n", seconds);
    std::fflush(stdout);

    for (int i = 0; i < seconds * 2; ++i) NAE_Play(h, 500);
    NAE_Close(h);
    std::printf("  播完了。\n");
    return 0;
}