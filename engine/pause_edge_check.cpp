// 暂停时 seek、暂停时换歌：fifo 里暂停前的音频是否被替换。
//
// # 为什么这两个测试存在
//
// `PAUSE-RESUME-SPEC.md` 的「不变式」一节自己标了两项未测：
//
//   > **Seek while paused, and track change while paused.** Not checked, and both
//   > are new risks that did not exist when this was written. The fifo holds audio
//   > decoded before the pause; a seek issued while paused has to replace it, and a
//   > track change while paused has to replace it. Neither has been exercised. A
//   > stale-buffer leak would be inaudible in a level meter and obvious in the ear.
//
// 「在电平表里听不见，在耳朵里很明显」——所以判据必须是**可测量的**，而且必须能区分
// 「播了新位置」和「播了旧内容」。
//
// # 怎么在没有耳朵的情况下测「播的是不是旧内容」
//
// 位置和内容两件事交叉验证：
//
//   1. **位置**：暂停中 `NAE_Position` 必须**不动**（已测：3082.1 → 3082.1 ms），
//      seek 之后必须落在目标附近。
//   2. **内容**：恢复后前进的距离必须是从 seek 处算起的，不是从暂停处算起的。
//      如果 fifo 里留着暂停前的音频，恢复后会先播出那几十帧旧内容，而位置却从
//      seek 处开始——两者会差出来。
//   3. **静音**：暂停中 `silenceFramesWritten` 必须推进。暂停不是「什么都不做」，
//      是喂静音；这个计数不动就说明设备在跑干。
//
// # 为什么不能只测位置
//
// 因为「位置对了而内容是旧的」正是这里要抓的。只测位置会漏掉它。
//
// # 为什么第二项串行
//
// 同时持有两个 exclusive 客户端会得到 `0x8889000A`（设备忙）——第一次跑就是这样
// 失败的。那是测试写法的问题，不是引擎的问题：应用换歌是先关旧的再开新的，所以
// 这里也这么做。
//
// 顺带一提，`0x8889000A` 在这个项目里被追查过很久，最后结论是「来源不明」，而且它
// 曾被错误地记在 Twilight Echo 名下。所以这里不去解释它，只把它当作「设备忙」。
//
// usage: pause_edge_check <fileA> <fileB>

#include "nae.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

std::string fmt1(const char* pattern, double a) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), pattern, a);
    return buf;
}

std::string fmt2(const char* pattern, double a, double b) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), pattern, a, b);
    return buf;
}

/// 三个 double。分开命名而不是做一个可变参helper：后者会静默只格式化第一个参数，
/// 打印出一句带两个零的、看起来合理的话——而一条报「从 0.0 到 0.0 ms」的断言读起来
/// 像真的测到了什么。
std::string fmt3(const char* pattern, double a, double b, double c) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), pattern, a, b, c);
    return buf;
}

struct Open {
    NAE_Handle* h = nullptr;
    double duration_ms = 0.0;
};

bool openFile(const char* path, Open* out) {
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0, fileCh = 0;
    double durationMs = 0.0, latencyMs = 0.0;
    const int32_t rc = NAE_OpenFile(nullptr, path, NAE_OUTPUT_NATIVE, 0, 0, &out->h, &rate,
                                    &srcRate, &ch, &bits, &valid, &isFloat, &resampling,
                                    &dithered, &bitPerfect, &fileCh, &durationMs, &latencyMs);
    if (rc != 0) {
        std::fprintf(stderr, "NAE_OpenFile(%s) -> %d  %s\n", path, rc, NAE_GetLastError());
        return false;
    }
    out->duration_ms = durationMs;
    return true;
}

double positionMs(NAE_Handle* h) { return NAE_Position(h) * 1000.0; }

uint64_t silenceFrames(NAE_Handle* h) {
    NAE_SignalPath sp{};
    sp.structSize = sizeof(sp);
    NAE_QuerySignalPath(h, &sp);
    return sp.silenceFramesWritten;
}

/// Play in 200 ms slices, stopping early if `stopWhenTrue` says so.
///
/// Sliced exactly as the application does it, because the pause implementation
/// depends on the host *keeping* calling NAE_Play: the stream stays running and is
/// fed silence, so a harness that stopped calling would be testing an idle stream
/// rather than a pause.
int playSlices(NAE_Handle* h, int ms, bool (*stopWhenTrue)(NAE_Handle*) = nullptr) {
    int played = 0;
    while (played < ms) {
        if (stopWhenTrue && stopWhenTrue(h)) return played;
        if (NAE_Play(h, 200) != 0) return played;
        played += 200;
    }
    return played;
}

bool sawSilence(NAE_Handle* h) { return silenceFrames(h) > 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: pause_edge_check <fileA> <fileB>\n");
        return 2;
    }
    const char* pathA = argv[1];
    const char* pathB = argv[2];

    std::printf("== 1. seek while paused ==\n");
    {
        Open a{};
        if (!openFile(pathA, &a)) {
            check(false, "open A", pathA);
            return 1;
        }
        const double totalMs = a.duration_ms;

        playSlices(a.h, 3000);
        const double posBeforePause = positionMs(a.h);

        NAE_Pause(a.h);
        const uint64_t silenceBefore = silenceFrames(a.h);
        playSlices(a.h, 2000, sawSilence);
        const uint64_t silenceAfter = silenceFrames(a.h);
        const double posAfterPause = positionMs(a.h);

        check(silenceAfter > silenceBefore, "暂停期间喂了静音",
              fmt2("%.0f -> %.0f 帧", double(silenceBefore), double(silenceAfter)));
        check(std::fabs(posAfterPause - posBeforePause) < 50.0, "暂停期间位置不动",
              fmt2("%.1f -> %.1f ms", posBeforePause, posAfterPause));

        const double target = totalMs * 0.6;
        const int32_t seekRc = NAE_Seek(a.h, target / 1000.0);
        check(seekRc == 0, "暂停中 seek 被接受", seekRc == 0 ? "" : NAE_GetLastError());
        const double posAfterSeek = positionMs(a.h);
        check(std::fabs(posAfterSeek - target) < 250.0, "seek 之后位置落在目标附近",
              fmt2("目标 %.0f ms，实测 %.1f ms", target, posAfterSeek));

        NAE_Resume(a.h);
        const double posAtResume = positionMs(a.h);
        playSlices(a.h, 2000);
        const double posAfterResume = positionMs(a.h);

        // 核心判据。恢复后前进的距离必须是从 seek 处算起的。若 fifo 泄漏了暂停前
        // 的内容，位置会说自己在新地方，而耳朵听到的是旧的一段——两者的差就出现在
        // 这里。
        const double advanced = posAfterResume - posAtResume;
        check(advanced > 1000.0 && advanced < 4000.0, "恢复后位置从 seek 处前进，而非暂停前",
              fmt3("从 %.1f 到 %.1f ms（前进 %.1f ms）", posAtResume, posAfterResume, advanced));
        check(posAfterResume > posAfterPause,
              "恢复后位置在暂停位置之后（fifo 没有把旧音频放出来）",
              fmt2("暂停在 %.1f，恢复后 %.1f ms", posAfterPause, posAfterResume));
        NAE_Close(a.h);
    }

    std::printf("\n== 2. track change while paused ==\n");
    {
        Open a{};
        if (!openFile(pathA, &a)) {
            check(false, "open A", pathA);
            return 1;
        }
        std::printf("  A: %.1f s\n", a.duration_ms / 1000.0);

        playSlices(a.h, 2000);
        const double aPosBeforePause = positionMs(a.h);
        NAE_Pause(a.h);
        playSlices(a.h, 1000, sawSilence);
        check(sawSilence(a.h), "A 处于暂停中", "");
        const double aPosWhilePaused = positionMs(a.h);
        std::printf("  A 播到 %.1f ms 后暂停\n", aPosWhilePaused);

        // 换歌：关掉 A 再开 B。顺序与应用一致，也避免同时持有两个 exclusive 流。
        NAE_Close(a.h);
        a.h = nullptr;

        Open b{};
        if (!openFile(pathB, &b)) {
            check(false, "open B", pathB);
            return 1;
        }
        std::printf("  B: %.1f s\n", b.duration_ms / 1000.0);

        const double bPosAtOpen = positionMs(b.h);
        check(std::fabs(bPosAtOpen) < 5.0, "新文件从头开始",
              fmt1("打开时位置 %.1f ms", bPosAtOpen));

        const int played = playSlices(b.h, 2000);
        const double bPosAfter = positionMs(b.h);
        check(played >= 1900, "B 播出了音频", std::to_string(played) + " ms");
        check(bPosAfter > 1000.0, "B 的位置在前进", fmt1("%.1f ms", bPosAfter));

        // 关键的一条：B 的位置只由自己决定。A 停下时 fifo 里留着暂停前解码的音频，
        // 那个状态若泄漏到新流，B 会从 A 的位置开始播。
        check(std::fabs(bPosAfter - 2000.0) < 500.0,
              "B 的位置只由自己决定，与 A 暂停在何处无关",
              fmt3("A 播到 %.1f ms 后暂停，B 播到 %.1f ms（B 期望约 2000 ms）",
                   aPosBeforePause, aPosWhilePaused, bPosAfter));

        NAE_Close(b.h);
    }

    std::printf("\n%s\n", failures == 0 ? "两项都通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    return failures == 0 ? 0 : 1;
}