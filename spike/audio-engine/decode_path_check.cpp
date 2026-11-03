// 检查 `NAE_QueryDecodePath`：它报的是采样**实际**是怎么解出来的。
//
// # 为什么这个字段存在
//
// DSD 有三条保真度差别很大的路，而**解码器名字不描述它做了什么**：FFmpeg 的
// `dsd_msbf` 与 `dsd_lsbf_planar` 都叫 DSD 解码器，但一个带了后置抽取滤波、一个没有。
// 从名字推断 DSD 音质是猜。
//
// 所以路径必须是**跑完之后写下来的**，而不是从 codec 名字推出来的。
//
// # 判据里最要紧的一条：空字符串与读不出来必须能分辨
//
//   * 空字符串 = 「这个信号没有自己的路径」（正弦、纯 PCM）——关于**信号**的陈述。
//   * 非 NAE_OK = 「读不出来」——关于**播放器**的故障。
//
// 两者混在一起时，界面就会把「播放器坏了」显示成「这个文件没有额外处理」，而那会把
// 一个故障说成一个事实。
//
// 用法: decode_path_check <utf8-path-file> | <none>

#include "nae.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

std::string readPathFile(const char* p) {
    std::FILE* f = std::fopen(p, "rb");
    if (!f) return "";
    std::vector<char> buf(4096, 0);
    const size_t n = std::fread(buf.data(), 1, buf.size() - 1, f);
    std::fclose(f);
    std::string s(buf.data(), n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("== 解码路径报告 ==\n");

    // --- 正弦：没有解码器，所以路径应当为空，而**返回 NAE_OK** ---
    {
        NAE_Handle* h = nullptr;
        int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
                dithered = 0, bitPerfect = 0;
        double latencyMs = 0.0;
        const int32_t rc = NAE_OpenExclusiveSine(nullptr, &h, &rate, &ch, &bits, &valid, &isFloat,
                                                  &latencyMs);
        if (rc != 0 || !h) {
            check(false, "正弦能打开", "rc=" + std::to_string(rc));
        } else {
            char buf[NAE_DECODE_PATH_MAX];
            const int32_t q = NAE_QueryDecodePath(h, buf, sizeof(buf));
            check(q == NAE_OK, "正弦：返回 NAE_OK（不是错误）", "rc=" + std::to_string(q));
            check(buf[0] == '\0', "正弦：路径为空（这是关于信号的陈述）",
                  std::string("「") + buf + "」");
            // 空缓冲区必须被拒绝，而不是崩。
            char tiny[1];
            const int32_t q2 = NAE_QueryDecodePath(h, tiny, 0);
            check(q2 != NAE_OK, "零长度缓冲区被拒绝", "rc=" + std::to_string(q2));
            NAE_Close(h);
        }
    }

    if (argc >= 2 && std::string(argv[1]) != "none") {
        const std::string path = readPathFile(argv[1]);
        if (path.empty()) {
            check(false, "路径文件非空", "空的");
        } else {
            NAE_Handle* h = nullptr;
            int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0,
                    resampling = 0, dithered = 0, bitPerfect = 0, fileChannels = 0;
            double durationMs = 0.0, latencyMs = 0.0;
            const int32_t rc =
                NAE_OpenFile(nullptr, path.c_str(), NAE_OUTPUT_NATIVE, 0, 0, &h, &rate, &srcRate,
                             &ch, &bits, &valid, &isFloat, &resampling, &dithered, &bitPerfect,
                             &fileChannels, &durationMs, &latencyMs);
            if (rc != 0 || !h) {
                check(false, "文件能打开", "rc=" + std::to_string(rc));
            } else {
                char buf[NAE_DECODE_PATH_MAX];
                const int32_t q = NAE_QueryDecodePath(h, buf, sizeof(buf));
                std::printf("\n  文件 %s\n", path.c_str());
                std::printf("  解码路径  %s\n", buf);
                std::printf("  源 %d Hz -> 设备 %d Hz\n", srcRate, rate);
                check(q == NAE_OK, "文件：返回 NAE_OK", "rc=" + std::to_string(q));
                check(std::string(buf).find("ffmpeg:") == 0,
                      "路径以 ffmpeg: 开头（它说了用谁的解码器）", buf);
                // 截断而不溢出：给一个只有 8 字节的缓冲区。
                char small[8];
                NAE_QueryDecodePath(h, small, sizeof(small));
                check(small[sizeof(small) - 1] == '\0', "短缓冲区截断且有结尾 NUL",
                      std::string("「") + small + "」");
                NAE_Close(h);
            }
        }
    }

    std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    return failures == 0 ? 0 : 1;
}