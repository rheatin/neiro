// 这台设备能否以 DoP 需要的载体速率开流？
//
// # 为什么这个仪器优先于 DoP 实现
//
// DoP 不是「给 DSD 加一条解码路径」，它是**换一种传输方式**：DSD 比特流不���码，
// 打包进 24-bit PCM 载波（dCS DoP v1.1），每个采样是一个 24 位字：
//
//     bits 23..16 = 标记（0x05 / 0xFA，逐帧交替）
//     bits 15..8  = 时间上较早的那个 DSD 字节
//     bits 7..0   = 时间上较晚的那个 DSD 字节
//
// 所以 DSD64 需要载波是 **352800 Hz**（= 2822400 / 8），DSD128 是 705600，依此类推。
//
// 这台设备的原生混合格式是 192000 Hz。所以问题只有一个，而且它先于所有实现细节：
// **它能不能以 352800 Hz 开流？** 不能的话 DoP 在这台机器上根本不存在，写多少代码
// 都是白写。
//
// 而这个设备的行为不能用「它只有 192 kHz」来推断——这台机器上 `0x8889000A` 已经
// 被追查过很久，结论是「来源不明」，而且它曾被错误地记在 Twilight Echo 名下。所以
// 这里直接试，而不是推断。
//
// # 顺带测的
//
// DSD64 的 DoP 载波是 352800 Hz，DSD64 的原生 DSD 也可能是 352800 Hz（1-bit）。
// 两者都需要 352800，但格式完全不同：一个是 24-bit PCM，一个是 1-bit。所以这里
// 分开试，并且区分「速率开不了」和「速率能开但格式不认」——这两个结论指向完全
// 不同的下一步。
//
// usage: dop_probe [deviceId]

#include "nae.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail) {
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

/// 一个格式化助手，按需带一或两个 double。
///
/// 写成两个重载而不是一个可变参模板，是因为这里出现的两种写法都应该被编译器检查。
/// 之前先有一个一参数的 `fmt`，加第二个调用点时改成两参数，于是别处一参数的调用
/// 报错——而那正是它该做的。
std::string fmt1(const char* pattern, double a) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), pattern, a);
    return buf;
}

/// 两个 double，但参数是 `int`——因为这里要打的是速率。
///
/// 单独存在而不是复用 `fmt1`，是因为这个项目里已经踩过一次：格式串写 `%d` 而参数
/// 是 `double`，在 x86-64 上是未定义行为，打印出的是寄存器里的残留——所以「asked
/// 0, got 0」出现了，而两个值明明都是 176400 和 192000。一个**看起来像测量结果**
/// 的 0，比崩溃难查得多。
std::string fmt2i(const char* pattern, int a, int b) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), pattern, a, b);
    return buf;
}

/// 设备支持的 exclusive 速率，向上面实测的。
std::string supportedRates(const char* deviceId) {
    std::vector<uint32_t> rates(64);
    uint32_t count = 0;
    // 先用默认设备开一次流——`NAE_QuerySupportedRates` 需要一个活着的句柄。
    NAE_Handle* h = nullptr;
    int32_t rate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0;
    double latencyMs = 0.0;
    const int32_t rc = NAE_OpenExclusiveSine(deviceId, &h, &rate, &ch, &bits, &valid, &isFloat,
                                              &latencyMs);
    if (rc != 0) return "(device would not open: " + std::to_string(rc) + ")";
    const uint32_t got = NAE_QuerySupportedRates(h, rates.data(), 64, &count);
    NAE_Close(h);
    std::string out;
    for (uint32_t i = 0; i < got; ++i) {
        if (!out.empty()) out += ", ";
        out += std::to_string(rates[i]);
    }
    return out;
}

std::string fmt3(const char* pattern, double a, double b, double c) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), pattern, a, b, c);
    return buf;
}

/// 试着用 `wantRate` 开一个 exclusive 流，报告结果。
///
/// `NAE_OpenExclusive` 的 `sourceRate` 是**文件**的速率，而设备侧由 `outputMode`
/// 决定。DoP 载体场景下「文件速率」就是载波速率——所以传 352800 就是要求设备以
/// 352800 开流，正是这个问题要问的。
/// 试着用 `fixedRate` 开一个 exclusive 流，报告实际开在哪个速率。
///
/// **必须用 `NAE_OUTPUT_FIXED`。** 这是这个仪器第一版的错误，而它让整次测量变成
/// 了一个什么都没证明的「通过」：
///
///   * `NAE_OUTPUT_NATIVE` 让设备用自己的速率，所以无论 `sourceRate` 传什么，结果
///     都是 192000。第一版传 176400 然后拿到「opened at 192000」并判为 PASS——而那
///     只说明 192 kHz 能开，与 DoP 载波毫无关系。
///   * `OUTPUT_DEVICE` 同样：它让输出跟随**源**，所以 176400 的「文件」会被重采样
///     到设备速率。
///   * 只有 `OUTPUT_FIXED` 会真的**要求** `fixedRate`，并在设备给不了时失败。
///
/// 所以判据必须是「实际开在请求的速率上」，而不是「开成了」。
bool tryOpenFixed(const char* deviceId, int32_t fixedRate, std::string* detail,
                  int32_t* actualOut) {
    NAE_Handle* h = nullptr;
    int32_t rate = 0, srcRate = 0, ch = 0, bits = 0, valid = 0, isFloat = 0, resampling = 0,
            dithered = 0, bitPerfect = 0;
    double latencyMs = 0.0;
    // 源速率给 44100：DoP 载波场景下「文件」是一段已打包的 PCM，而它本身的速率就是
    // 载波速率。所以这里两者都给 fixedRate 才对应真实情况——但源速率不影响设备侧
    // 能否开流，所以给一个无关的 44100 让这个探针不依赖载波速率也能跑。
    const int32_t rc = NAE_OpenExclusive(deviceId, 44100, NAE_OUTPUT_FIXED, fixedRate, 0, &h,
                                          &rate, &srcRate, &ch, &bits, &valid, &isFloat,
                                          &resampling, &dithered, &bitPerfect, &latencyMs);
    if (rc != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", NAE_GetLastHresult());
        *detail = std::string("rc=") + std::to_string(rc) + " hr=" + buf + " " +
                  NAE_GetLastError();
        if (actualOut) *actualOut = 0;
        return false;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "opened at %d Hz, %d ch, %d bit (valid %d), resampling=%d",
                  rate, ch, bits, valid, resampling);
    *detail = buf;
    // `actualOut` 是这个探针的判据所在——`opened` 只说「开成了」，而 DoP 需要的是
    // 「开在**那个**速率上」。第一版把这两个混为一谈，于是无论实际速率是什么都判
    // PASS。所以这里宁可重复赋两次，也不让判据依赖调用方记得读它。
    if (actualOut) {
        *actualOut = rate;
    } else {
        std::fprintf(stderr, "[dop_probe] BUG: actualOut is null\n");
    }
    NAE_Close(h);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* deviceId = argc > 1 ? argv[1] : nullptr;
    std::printf("== 设备支持的 exclusive 速率 ==\n");
    const std::string rates = supportedRates(deviceId);
    std::printf("  %s\n", rates.c_str());
    if (rates.empty()) std::printf("  (没有速率被报告；下面仍然逐个试)\n");

    // DSD64 的 DoP 载波速率。
    //
    // 2822400 是 DSD 的基准时钟，乘 64 得 44.1 kHz 的 DSD64 比特率。DoP 把 16 个
    // DSD 位装进一个 24-bit 字（8 位标记 + 16 位载荷），所以：
    //
    //     DSD64  = 2822400 bps  ->  2822400 / 16 = 176400 Hz
    //
    // 第一版这里写的是 `/ 8`，得出 352800。那是我算错了，而且**算错之后整个测量
    // 还报了 PASS**——因为那一版用的是 `OUTPUT_NATIVE`，设备用自己的速率就开成了，
    // 与请求的速率毫无关系。所以这一版改了两处：除数，以及用 `OUTPUT_FIXED` 让引擎
    // 真的去要求那个速率。
    //
    // 352800 是 DSD64 的**原生 1-bit** 速率，176400 是它的 **DoP 载体**速率。
    // 相差一倍，因为 DoP 每 16 个比特出一个采样，而 1-bit 模式下每个比特就是一个
    // 采样字节。两个都对，用错了地方就会得到一个看起来合理但不存在的数字。
    constexpr int32_t kDopCarrierDsd64 = 2822400 / 16;
    std::printf("\n== DSD64 的 DoP 载体需要 %d Hz ==\n", kDopCarrierDsd64);

    std::string detail;
    int32_t actual = 0;
    const bool opened = tryOpenFixed(deviceId, kDopCarrierDsd64, &detail, &actual);
    std::printf("\n  以 NAE_OUTPUT_FIXED 要求 %d Hz\n  %s\n", kDopCarrierDsd64, detail.c_str());

    // 判据是「实际开在请求的速率上」，不是「开成了」。
    //
    // 这正是第一版的错误：`OUTPUT_FIXED` 之下引擎仍可能返回别的速率，而「opened at
    // 192000」在要求 176400 时是一个**假通过**。
    const bool dopOk = opened && actual == kDopCarrierDsd64;
    check(dopOk, "设备能以 DSD64 的 DoP 载体速率开流",
          opened ? fmt2i("asked %d, got %d", kDopCarrierDsd64, actual) : detail);

    if (!dopOk) {
        std::printf("\n  结论：这台设备不能用 DoP 传 DSD64。\n");
        if (!opened) {
            std::printf("  原因是**开流就失败**——引擎还没走到格式协商那一步。\n");
            std::printf("  所以瓶颈是速率，不是「设备不认 DoP 载体格式」。\n");
        } else {
            std::printf("  引擎开了 %d Hz 而不是要求的 %d Hz——所以「开成了」并不等于\n"
                        "  「能以那个速率开」，这正是第一版踩的坑。\n", actual, kDopCarrierDsd64);
        }
        std::printf("  DoP 在这台机器上不存在，写多少打包代码都是白写。\n");
    }

    // 对照：设备自己声明的速率，用同一个 `OUTPUT_FIXED` 路径。
    //
    // 目的是证明「用 OUTPUT_FIXED 能不能拿到想要的速率」——如果连 192000 都拿不到，
    // 那么上面那个失败是探针写错了，不是设备的问题。
    std::string topDetail;
    int32_t topActual = 0;
    const bool topOk = tryOpenFixed(deviceId, 192000, &topDetail, &topActual);
    std::printf("\n  对照：用同一个 OUTPUT_FIXED 路径要求 192000 Hz\n  %s\n", topDetail.c_str());
    check(topOk && topActual == 192000, "OUTPUT_FIXED 确实能拿到请求的速率（探针没写错）",
          topOk ? fmt2i("asked %d, got %d", 192000, topActual) : topDetail);

    std::printf("\n%s\n", failures == 0 ? "全部通过。" : "有失败；上面标 FAIL 的就是没成立的。");
    // 有意不按 failures 决定退出码：这台设备的答案可能就是「不能」，而那不是仪器
    // 失败。结论由正文给出。
    return 0;
}
