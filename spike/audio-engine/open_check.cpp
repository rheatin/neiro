// 从 UTF-8 文件读路径，然后打开它。
//
// # 为什么需要这个仪器
//
// MinGW 的 `main(argc, argv)` 给的**窄** argv 是按 ANSI 码页转换的，不是 UTF-8。所以一个
// 仪器用 argv 收中文路径，那个路径在到达引擎之前就已经被按 GBK 编码了，于是磁盘上的
// 文件名对不上，`fopen` 失败，引擎报 `rc=9 CANNOT_OPEN`。
//
// 而 `rc=9` 说的是「文件打不开」，指向的是文件/格式——但真正的错在**传路径的那一层**。
// 我差点据此记下一个不存在的「DFF 不支持」的产品缺陷：同一个文件复制成 ASCII 名字后，
// 引擎秒开（`dsd_msbf`，352800 Hz，240.6 秒，5.64 Mbps）。
//
// 所以这个仪器绕开 argv：从一个 UTF-8 文件读路径。路径本身由调用方用可靠的方式写进去。
//
// 这也回答一个从没被测过的问题：**引擎本身能不能打开 Unicode 路径。** 今晚所有仪器用的
// 都是 ASCII 路径，而用户的曲库是中文命名的。应用的 Rust 侧传 UTF-8，所以它很可能没问题
// ——但「很可能」不是测过的。
//
// 用法: open_check <utf8-path-file>

#include "nae.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: open_check <utf8-path-file>\n");
        return 1;
    }
    // 读路径文件。去掉行尾的 CR/LF——用二进制读是因为 CRLF 文件在 Windows 上是常态。
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::printf("读不了路径文件 %s\n", argv[1]);
        return 1;
    }
    std::vector<char> buf(4096, 0);
    const size_t n = std::fread(buf.data(), 1, buf.size() - 1, f);
    std::fclose(f);
    std::string path(buf.data(), n);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();
    if (path.empty()) {
        std::printf("路径文件是空的\n");
        return 1;
    }

    std::printf("路径字节数 %zu\n", path.size());
    std::printf("路径 UTF-8    %s\n", path.c_str());

    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    // 可选的第二个参数：强制输出速率。
    //
    // 需要它是为了测「引擎在**重采样路径**上的底噪」：这台设备的原生混合格式里有 44100，
    // 所以 `OUTPUT_NATIVE` 会让 44.1 kHz 的文件走 44100 -> 44100，**根本不重采样**——
    // 于是上一版那个底噪对照测的是一个不重采样的路径，而 DSD 路径是重采样的。
    // 拿不重采样的底噪去比重采样的读数，比的不是同一个东西。
    //
    // 44.1 kHz 的 PCM 在 22.05 kHz 以上**物理为空**，所以强制它上采样到 192000 之后，
    // 22 kHz 以上读到的就是引擎在重采样路径上自己产生的东西。
    int32_t mode = NAE_OUTPUT_NATIVE;
    int32_t fixedRate = 0;
    if (argc >= 3) {
        const int want = std::atoi(argv[2]);
        if (want > 0) {
            mode = NAE_OUTPUT_FIXED;
            fixedRate = want;
        }
    }

    const int32_t rc = NAE_OpenFile(nullptr, path.c_str(), mode, fixedRate, 0, &h, &rate,
                                    &srcRate, &ch, &bits, &valid, &isFloat, &resampling, &dithered,
                                    &bitPerfect, &fileChannels, &durationMs, &latencyMs);
    if (rc != 0 || !h) {
        std::printf("\n  rc=%d %s\n", rc, NAE_GetLastError());
        // rc=9 说的是「文件打不开」。而真正的错可能在传路径的那一层——所以这里把
        // 两件事分开报：引擎报的错，和路径本身是不是 UTF-8。
        bool utf8 = true;
        for (size_t i = 0; i < path.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(path[i]);
            if (c >= 0x80) {
                if ((c & 0xE0) == 0xC0) i += 1;
                else if ((c & 0xF0) == 0xE0) i += 2;
                else if ((c & 0xF8) == 0xF0) i += 3;
                else utf8 = false;
            }
        }
        std::printf("  路径看起来是 %s\n", utf8 ? "合法 UTF-8" : "**不是合法 UTF-8**");
        std::printf("  若路径是合法 UTF-8 而这里报 %d，那问题在引擎打开 Unicode 路径的能力，\n"
                    "  而不是文件格式。\n",
                    rc);
        return 1;
    }

    std::printf("\n  打开成功\n");
    std::printf("  源 %d Hz -> 设备 %d Hz，%d ch（文件 %d ch），%d bit (valid %d)\n", srcRate, rate,
                ch, fileChannels, bits, valid);
    std::printf("  resampling=%d dithered=%d bitPerfect=%d，时长 %.1f s\n", resampling, dithered,
                bitPerfect, durationMs / 1000.0);
    NAE_Close(h);
    return 0;
}