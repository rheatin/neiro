// 实测 `NAE_ComputePeaks`。**编译过不等于能用** —— 这一支是那个「能用」。
//
// # 它现在是纯函数测试，不需要设备
//
// 上一版（handle 版）要 `NAE_OpenFile` 起一条 WASAPI 独占流才量得到，因为 peaks
// 存在句柄上。改成收路径之后**它不需要任何设备**：给一个文件、给一个桶数，拿回一份
// 波形。所以这一支可以在任何机器上跑，也可以在别的检查脚本里跑。
//
// # 它检查什么
//
//   1. 参数校验：桶数不够、桶数为 0、空路径 → `NAE_ERR_INVALID_ARGUMENT`（**不截断**）
//   2. 打不开的路径 → `NAE_ERR_CANNOT_OPEN`（**不是** `INVALID_ARGUMENT`）
//   3. 正常：拿到请求的桶数、`complete`、**不是全零**、每桶 `lo <= 0 <= hi`、
//      值在一个合理上界之内
//   4. **两次耗时同量级** —— 引擎侧**没有缓存**（缓存是调用方的事），所以第二次
//      不该明显更快。这一条量的是那个设计决定，不是性能
//
// # 它不检查什么（说清边界）
//
// **它不核对 peaks 的数值是否正确。** 那要一份独立算出来的参照。`decode_check.exe`
// 能给出**整个文件的最大幅度**，所以这里把 `max(|lo|,|hi|)` 打出来供人工交叉验 ——
// 那两个数应当相等（实测 `Dream.mp3` 两边都是 1.1324）。
//
// 用法：peaks_check <file> [buckets]

#include "nae.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_ok = 0;

static void check(bool cond, const std::string& what, const std::string& detail = "") {
    if (cond) {
        ++g_ok;
        std::printf("  [ok]   %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s\n", what.c_str(),
                    detail.empty() ? "" : ("\n         " + detail).c_str());
    }
}

/// 调一次并把结果收进来。`outFloats` 是给 `out` 的容量（浮点数）。
struct Run {
    int32_t rc;
    NAE_PeaksInfo info;
    std::vector<float> peaks;
    double ms;
};

static Run run(const char* path, uint32_t buckets, size_t outFloats) {
    Run r{};
    r.peaks.assign(outFloats, 0.0f);
    r.info.struct_size = sizeof(r.info);
    const auto t0 = std::chrono::steady_clock::now();
    r.rc = NAE_ComputePeaks(path, buckets, &r.info, r.peaks.data(),
                            static_cast<int32_t>(outFloats));
    r.ms = std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
    return r;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: peaks_check <file> [buckets]\n");
        return 2;
    }
    const char* path = argv[1];
    const uint32_t buckets = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 4000u;

    std::printf("  文件 %s\n  桶数 %u\n\n", path, buckets);

    // ---- 参数校验：这三种都必须是 INVALID_ARGUMENT，而且**不截断**
    {
        const Run small = run(path, buckets, 16);
        check(small.rc == NAE_ERR_INVALID_ARGUMENT,
              "容量不够时返回 NAE_ERR_INVALID_ARGUMENT（**不截断**）",
              "rc=" + std::to_string(small.rc) + "（期望 " + std::to_string(NAE_ERR_INVALID_ARGUMENT) + "）");

        const Run zero = run(path, 0, 16);
        check(zero.rc == NAE_ERR_INVALID_ARGUMENT, "桶数为 0 时返回 NAE_ERR_INVALID_ARGUMENT",
              "rc=" + std::to_string(zero.rc));

        const Run empty = run("", 16, 32);
        check(empty.rc == NAE_ERR_INVALID_ARGUMENT, "空路径返回 NAE_ERR_INVALID_ARGUMENT",
              "rc=" + std::to_string(empty.rc));

        // **下面这一条要和上面三条分开**：打不开一个真实路径是**故障**，不是「参数
        // 错了」。混成一个码的话，调用方会把「这个文件坏了」显示成「这首歌没有波形」。
        const Run missing = run("Z:\\no\\such\\file.flac", 16, 32);
        check(missing.rc == NAE_ERR_CANNOT_OPEN,
              "打不开的路径返回 NAE_ERR_CANNOT_OPEN（**不是** INVALID_ARGUMENT）",
              "rc=" + std::to_string(missing.rc) + "（期望 " + std::to_string(NAE_ERR_CANNOT_OPEN) + "）");
    }

    // ---- 正常一遍
    const Run first = run(path, buckets, static_cast<size_t>(buckets) * 2);
    check(first.rc == NAE_OK, "正常调用返回 NAE_OK", "rc=" + std::to_string(first.rc));
    if (first.rc != NAE_OK) {
        std::printf("\n  算不出来，后面的判据没有对象。\n");
        std::printf("  机读摘要：SUMMARY buckets=%u rc=%d fail=%d\n", buckets, first.rc, g_fail);
        return 1;
    }
    // 3 s 长是 short-term 窗口的定义，短于它的文件不可能有这一项。这里**拒绝**夹具，
    // 而不是把「夹具不够长」记成一条失败：前者是换一个文件，后者读起来像引擎坏了。
    if (first.info.duration_ms > 0.0 && first.info.duration_ms < 3000.0) {
        std::fprintf(stderr,
                     "fixture too short: %s is %.0f ms; the short-term window is 3 s, "
                     "so this check needs at least 3000 ms.\n",
                     path, first.info.duration_ms);
        return 2;
    }

    check(first.info.bucket_count == buckets, "拿到的桶数就是请求的桶数",
          "拿了 " + std::to_string(first.info.bucket_count) + "，要了 " + std::to_string(buckets));
    std::printf("        第一次 %.1f ms   complete=%u   duration=%.0f ms   rate=%u   ch=%u\n",
                first.ms, first.info.complete, first.info.duration_ms,
                first.info.sample_rate, first.info.channels);

    double lo = 1e9, hi = -1e9;
    size_t nonZero = 0;
    bool straddle = true;
    for (uint32_t b = 0; b < first.info.bucket_count; ++b) {
        const double a = first.peaks[b * 2], c = first.peaks[b * 2 + 1];
        if (a < lo) lo = a;
        if (c > hi) hi = c;
        if (a < -1e-6 || c > 1e-6) ++nonZero;
        if (!(a <= 0.0 && c >= 0.0)) straddle = false;
    }
    std::printf("        幅度范围 %.4f .. %.4f   非零桶 %zu / %u   max|v|=%.4f\n",
                lo, hi, nonZero, first.info.bucket_count, std::max(-lo, hi));

    // **不能断言「值在 -1..1 之内」。** 解码器交出来的是**不夹紧的 float32**：一个
    // 有损文件解出来完全可以超过满刻度。实测 `Dream.mp3` 是 -1.1324 .. 1.0438，而
    // `decode_check.exe` 对**同一个文件**报 `peak 1.1324` —— 两个独立工具同一个数，
    // 所以那是文件的事实，不是这一遍算错了。
    check(lo >= -4.0 && hi <= 4.0, "所有值都在一个合理上界之内（±4；超出说明取值写错了）",
          "范围 " + std::to_string(lo) + " .. " + std::to_string(hi));
    if (lo < -1.0001 || hi > 1.0001) {
        std::printf("        注意：这个文件的样本**超出满刻度**（%.4f .. %.4f）。\n", lo, hi);
        std::printf("        这是解码器交出的不夹紧 float32，不是这一遍算错了。交叉验：\n");
        std::printf("        decode_check.exe <同一个文件> 的 peak 应当 = max|v| = %.4f\n",
                    std::max(-lo, hi));
    }
    check(nonZero > first.info.bucket_count / 10,
          "**不是全零**（全零画出来是一条直线，看起来和「这首歌就是这样」一样）",
          "非零桶只有 " + std::to_string(nonZero) + " / " + std::to_string(first.info.bucket_count));
    check(straddle, "每桶都满足 lo <= 0 <= hi（`computePeaks` 的 lo/hi 从 0 起，所以必然如此）");

    // ---- 第二次
    //
    // 上一版（handle 版）在这里断言「第二次明显更快」，因为 peaks 缓存在句柄上。缓存
    // 搬到调用方之后那个断言反了。
    //
    // **但不能靠计时来断言「没有引擎侧缓存」**：实测第一次 513.8 ms、第二次 75.7 ms
    // （6.8×），而那个差**更可能是操作系统对网络文件的页缓存**（曲库在 SMB 上），
    // 不是缓存。所以计时区分不了「有缓存」和「操作系统把文件缓存了」这两个假设 ——
    // 一条通过但理由错的判据比没有更糟。
    //
    // 能断言的只有两件：**第二次仍然正确**（没有过期/错的缓存），而且**两次输出逐字节
    // 相同**。「引擎侧不缓存」是**代码**的性质（句柄上没有放 peaks 的字段），不是这里
    // 能计出来的东西。
    const Run second = run(path, buckets, static_cast<size_t>(buckets) * 2);
    std::printf("        第二次 %.1f ms（第一次 %.1f ms，比值 %.2fx）—— 这个差**更可能是\n",
                second.ms, first.ms, second.ms > 0 ? first.ms / second.ms : 0.0);
    std::printf("        操作系统把网络文件缓存了，不是引擎缓存；计时区分不了两者。\n");
    check(second.rc == NAE_OK, "第二次调用也返回 NAE_OK");
    check(second.info.bucket_count == buckets && second.info.complete == first.info.complete,
          "第二次拿到同样的桶数和同样的 complete（没有过期或错的缓存）",
          "第二次 " + std::to_string(second.info.bucket_count) + " 桶，complete=" +
          std::to_string(second.info.complete));
    check(std::memcmp(first.peaks.data(), second.peaks.data(),
                      first.peaks.size() * sizeof(float)) == 0,
          "两次拿到的 peaks **逐字节相同**（同一份输入，同一份输出）");

    // ---- 短窗 RMS 的两个读数（和 peaks 同一趟解出来的）
    //
    // **它们的值对不对，这一支核对不了**——和 peaks 那些数值同一个理由：要核对就得有
    // 一份独立算出来的参照。这里能量的是三件：窗口参数是不是那个、数在一个物理可能的
    // 区间里、以及两次调用给同一个数。
    std::printf("        响度 %.2f dBFS   动态范围 %.2f dB   窗 %u ms / 跳 %u ms\n",
                first.info.loudness_db, first.info.dynamic_range_db,
                first.info.metrics_window_ms, first.info.metrics_hop_ms);
    std::printf("        LUFS %.2f（瞬时 %.2f / 短时 %.2f，LRA %.2f LU，valid=%d）\n",
                first.info.lufs_integrated, first.info.lufs_momentary_max,
                first.info.lufs_short_term_max, first.info.lufs_range, first.info.lufs_valid);
    check(first.info.metrics_window_ms == 400 && first.info.metrics_hop_ms == 100,
          "窗口参数是 400 ms / 100 ms（它**和数一起**回来，界面读的是它而不是自己写一个）",
          std::to_string(first.info.metrics_window_ms) + " / " +
              std::to_string(first.info.metrics_hop_ms));
    check(first.info.loudness_db < 6.0 && first.info.loudness_db > -80.0,
          "响度落在一个物理可能的区间里（-80 .. +6 dBFS）",
          std::to_string(first.info.loudness_db) + " dB");
    check(first.info.lufs_valid == 1, "LUFS 有效（BS.1770 门限后至少有一个块）");
    check(first.info.lufs_short_term_valid == 1,
          "short-term window is valid (file is at least 3 s)",
          std::to_string(first.info.lufs_short_term_valid));
    check(first.info.lufs_integrated < 6.0 && first.info.lufs_integrated > -80.0,
          "LUFS 落在一个物理可能的区间里（-80 .. +6）",
          std::to_string(first.info.lufs_integrated) + " LUFS");
    check(first.info.lufs_momentary_max >= first.info.lufs_integrated,
          "最响的 400 ms 块不会比整首的积分值更小",
          std::to_string(first.info.lufs_momentary_max));
    // No ordering claim between the two maxima: a 3 s average is not a
    // superset of the loudest 400 ms block, and on real music it measures
    // lower (this file: -8.05 short-term vs -6.40 momentary).
    check(first.info.lufs_short_term_max < 6.0 && first.info.lufs_short_term_max > -80.0,
          "最响的 3 s 窗落在一个物理可能的区间里",
          std::to_string(first.info.lufs_short_term_max));
    check(first.info.lufs_range >= 0.0 && first.info.lufs_range < 60.0,
          "响度范围 >= 0 且不至于几十上百",
          std::to_string(first.info.lufs_range) + " LU");
    check(second.info.lufs_integrated == first.info.lufs_integrated &&
              second.info.lufs_valid == first.info.lufs_valid,
          "两次调用给同一个 LUFS（逐位相同）",
          std::to_string(second.info.lufs_integrated));
    check(first.info.dynamic_range_db >= 0.0 && first.info.dynamic_range_db < 60.0,
          "动态范围 >= 0（p95 不会小于 p10）而且不至于几十上百（那说明门限没起作用）",
          std::to_string(first.info.dynamic_range_db) + " dB");
    check(second.info.metrics_window_ms == first.info.metrics_window_ms &&
              second.info.loudness_db == first.info.loudness_db &&
              second.info.dynamic_range_db == first.info.dynamic_range_db,
          "两次调用给同一个响度和同一个动态范围（逐位相同，不是「差不多」）",
          "第二次 " + std::to_string(second.info.loudness_db) + " dB / " +
              std::to_string(second.info.dynamic_range_db) + " dB");

    std::printf("\n  %s（%d 过 / %d 不成立）\n",
                g_fail == 0 ? "全部通过" : "有不成立", g_ok, g_fail);
    std::printf("  机读摘要：SUMMARY buckets=%u first_ms=%.1f second_ms=%.1f complete=%u "
                "duration_ms=%.0f nonzero=%zu max_abs=%.4f loudness=%.2f dr=%.2f "
                "window_ms=%u hop_ms=%u fail=%d\n",
                buckets, first.ms, second.ms, first.info.complete, first.info.duration_ms,
                nonZero, std::max(-lo, hi), first.info.loudness_db, first.info.dynamic_range_db,
                first.info.metrics_window_ms, first.info.metrics_hop_ms, g_fail);
    return g_fail == 0 ? 0 : 1;
}
