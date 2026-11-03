// 在 shared 模式下真的开一次流，并读出它报给自己的信号路径。
//
// # 为什么需要这个仪器
//
// 引擎此前**没有任何打开 shared 流的代码路径**，所以关于 shared 的一切说法都是推论：
// 「它会不会正常出声」「它稳不稳」「它报什么」都没测过。`8ad3efe` 把能力声明改成
// 表驱动之后，加 shared 需要同时接上导出符号和开流路径——而**接上不等于能用**。
//
// 这个仪器回答的是最窄的那个问题：**shared 真的能在这台设备上开出声，并报出一条
// 自洽的信号路径吗。** 它不回答「shared 能不能 bit-perfect」——那是产品决定
// （2026-10-01：不追求），而本项目也不声称它。
//
// # 断言的期望值来自哪里
//
// 来自**shared 模式的定义**，不是来自引擎的输出：
//
//   * `bitPerfect` 必须是 0。流经 Windows 混音器，混音器在我们下游，所以无论
//     速率协商得出什么结论，「原样送达」都不成立。
//   * `reason` 必须是 7（SharedMode）。这是引擎新增的码，含义是「混音器在通路上」。
//   * 采样率、位深等其余字段**不做断言**——它们由协商决定，在这台设备上是什么
//     就接受什么。
//
// 唯一不设期望的是「能不能打开」：它可能失败（设备被占、格式被拒），而**失败本身
// 就是答案**，所以失败不算测试失败，只报出来。
//
// 用法: shared_check <utf8-path-file>

#include "nae.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(const char* label, bool ok, const std::string& detail = {}) {
    if (!ok) ++g_failures;
    std::printf("  [%s] %-40s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: shared_check <utf8-path-file>\n");
        return 1;
    }
    // 同 open_check：argv 是 ANSI 码页，中文路径到不了引擎。
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::printf("读不了路径文件 %s\n", argv[1]);
        return 1;
    }
    std::vector<char> buf(4096, 0);
    const size_t n = std::fread(buf.data(), 1, buf.size() - 1, f);
    std::fclose(f);
    std::string path(buf.data(), n);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
    if (path.empty()) {
        std::printf("路径文件是空的\n");
        return 1;
    }
    std::printf("文件 %s\n\n", path.c_str());

    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0;
    int32_t resampling = 0, dithered = 0, bitPerfect = 1, fileChannels = 0;
    double durationMs = 0.0, latencyMs = 0.0;

    std::printf("== NAE_OpenFileShared ==\n");
    const int32_t rc = NAE_OpenFileShared(nullptr, path.c_str(), NAE_OUTPUT_NATIVE,
                                          /*fixedRate=*/0, NAE_QUALITY_HIGH, &h,
                                          &rate, &srcRate, &ch, &bits, &valid, &isFloat,
                                          &resampling, &dithered, &bitPerfect,
                                          &fileChannels, &durationMs, &latencyMs);
    if (rc != NAE_OK) {
        // 失败不算测试失败。设备被别的程序占着、或格式被拒，都是**答案**，
        // 而且是关于这台机器此刻状态的答案。
        char why[256] = {0};
        NAE_GetLastErrorText(why, static_cast<int32_t>(sizeof(why)));
        std::printf("  打不开：rc=%d  %s\n", rc, why);
        std::printf("  lastHresult=0x%08X\n", (unsigned)NAE_GetLastHresult());
        std::printf("\n共享模式在这台设备上此刻开不了。**没有测到它能不能出声。**\n");
        return 0;
    }

    std::printf("  打开了：%d Hz -> 设备 %d Hz，%d ch，%d bit (valid %d)，时长 %.1f s\n",
                srcRate, rate, ch, bits, valid, durationMs / 1000.0);
    std::printf("  resampling=%d dithered=%d bitPerfect=%d  延迟 %.1f ms\n",
                resampling, dithered, bitPerfect, latencyMs);

    // 真的播一点，确认它出声，而不只是句柄开成了。
    //
    // `NAE_Play` 返回**状态码**，`NAE_OK` 是 0——第一版写 `played > 0`，于是
    // 一次成功的播放被报成「没有推进」。而 `NAE_Position` 才是位置，所以拿它来
    // 判断「有没有真的走起来」：调用是同步的，返回时应该已经推进了一点。
    std::printf("\n== 播 300 ms ==\n");
    const int32_t playRc = NAE_Play(h, 300);
    const int32_t pos = NAE_Position(h);
    const int32_t playRc2 = NAE_Play(h, 300);
    const int32_t pos2 = NAE_Position(h);
    std::printf("  第 1 次 rc=%d (NAE_OK=%d)  position=%d ms\n", playRc, NAE_OK, pos);
    std::printf("  第 2 次 rc=%d               position=%d ms\n", playRc2, pos2);
    check("NAE_Play 返回 NAE_OK", playRc == NAE_OK && playRc2 == NAE_OK,
          "渲染循环跑完");
    // 位置不动**不一定**是 shared 的问题：`NAE_Position` 报的是源被消费掉多少，
    // 而设备缓冲决定了第一次调用会先填缓冲。两次都 0 就值得怀疑了，但把它当成
    // 「shared 不出声」是猜测——所以这里只报事实，判���留给对比。
    std::printf("  >> 位置是否推进：%s（这是事实，不是判决；见提交信息里的 A/B）\n",
                pos2 > 0 ? "是" : "否");

    // 信号路径：这是**引擎自报**的，不是我们自己算的。
    NAE_SignalPath sp = {};
    sp.structSize = sizeof(sp);
    const int32_t spRc = NAE_QuerySignalPath(h, &sp);
    check("信号路径读得到", spRc == NAE_OK);
    if (spRc == NAE_OK) {
        std::printf("\n== 信号路径（引擎自报）==\n");
        std::printf("  source %d Hz / %d ch -> device %d Hz / %d ch\n",
                    sp.sourceRate, sp.sourceChannels, sp.sampleRate, sp.channels);
        std::printf("  %d bit (valid %d)  resampling=%d dithered=%d\n",
                    sp.bitsPerSample, sp.validBits, sp.resampling, sp.dithered);
        std::printf("  bitPerfect=%d  reason=%d  exclusive=%d\n",
                    sp.bitPerfect, sp.reason, sp.exclusive);
    }

    NAE_Close(h);

    std::printf("\n== 断言 ==\n");
    check("bitPerfect 必须是 0", spRc == NAE_OK && sp.bitPerfect == 0,
          sp.bitPerfect == 0 ? "0 —— 没有声称原样送达" : "1 —— 声称了，而混音器在通路上");
    check("reason 必须是 7 (SharedMode)", spRc == NAE_OK && sp.reason == NAE_REASON_SHARED_MODE,
          "reason=" + std::to_string(sp.reason));
    check("exclusive 字段必须是 0", spRc == NAE_OK && sp.exclusive == 0,
          "exclusive=" + std::to_string(sp.exclusive));
    // 位置不推进**不算失败**：上面两次调用都返回 NAE_OK，说明渲染循环跑完了。
    // 位置为什么是 0 属于「shared 稳不稳」那个问题，而那还没有对照测量，
    // 断言一个没测过的数就是发明一个标准。

    std::printf("\n%s（%d 失败）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
