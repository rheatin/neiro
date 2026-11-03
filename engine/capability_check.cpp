// 能力声明 vs DLL 实际导出的开流入口。
//
// # 为什么需要这个仪器
//
// `NAE_GetEngineCapabilities` 曾经返回硬编码的
// `"shareModes":["shared","exclusive"]`，而引擎里**没有任何打开 shared 流的代码
// 路径**。`AUDCLNT_SHAREMODE_SHARED` 在整个引擎里只出现在 `tryInitialize` 里，
// 而 `tryInitialize` 只有两个调用点，都在设备枚举中（`nae.cpp` 的 919/921），
// 只用来回答「这设备能不能以 shared 打开」——从来没有被用来**打开**一个。
// 真正开流的地方是硬编码的 `AUDCLNT_SHAREMODE_EXCLUSIVE`。
//
// 也就是说那是一条**声称**，而且它就写在最容易被信任的位置：能力声明里。
// 这正是本项目自己的规矩（「不许声称没测过的能力」）要拦的东西。
//
// # 这个仪器查的是什么
//
// **不是**查「声明和生成声明的那段代码一不一致」——那只是把谎读回自己一遍。
// 查的是**声明 vs DLL 真实的导出表**：每一个被声明的 share mode，都必须有一个
// 对应的导出开流入口能真的以那个模式打开流。
//
// 命名约定：`shareModes` 里的 `exclusive` 对应导出 `NAE_OpenExclusive`，
// `shared` 对应 `NAE_OpenShared`。这个仪器用 `GetProcAddress` 去问 DLL 真实的
// 导出表，所以它和那段生成 JSON 的代码**没有共享任何东西**。
//
// 独立来源：这个断言的期望值来自 DLL 的导出表，那是被测对象之外的另一个事实。
// 字符串说什么不算数，导出表里有没有才算数。
//
// # 这个仪器能失败吗（反证）
//
// 改之前它会失败，而且理由具体：声明了 `shared`，但 `NAE_OpenShared` 不在导出
// 表里。改完之后它通过。要让它再次失败，把 `kOpenableShareModes` 里加回
// `{"shared"}` —— 表生成了字符串，但导出表里没有对应的入口，于是 FAIL。
//
// 换句话说：**只加表项不改 ABI 会被抓住**，这正是把字符串改成表驱动的意义。
//
// 用法: capability_check [dll路径]

#include "nae.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(const char* label, bool ok, const std::string& detail = {}) {
    if (!ok) ++g_failures;
    std::printf("  [%s] %-44s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}

/** One declared share mode, and the exported entry point that opens it. */
struct ShareMode {
    std::string mode;
    std::string entryPoint;
};

/** Read a quoted string starting at or after `from`. */
bool readString(const std::string& json, size_t from, std::string& out) {
    const size_t open = json.find('"', from);
    if (open == std::string::npos) return false;
    const size_t close = json.find('"', open + 1);
    if (close == std::string::npos) return false;
    out = json.substr(open + 1, close - open - 1);
    return true;
}

// 取出 `"shareModes":[...]` 里的每一项，形式是 `{ "mode": ..., "entryPoint": ... }`。
//
// 为什么是对象而不是字符串：只报模式名的话，外部**无法核对**——`shared` 到底对应
// `NAE_OpenShared` 还是 `NAE_OpenFileShared`，从字符串里看不出来。这个仪器第一版
// 按 `NAE_Open` + 首字母大写 的约定去猜，猜错了，然后正确地报了「声明了却打不开」。
// 让引擎把入口一起说出来，这条声明才**可被验证**，而不是只能被相信。
std::vector<ShareMode> shareModesFrom(const std::string& json) {
    std::vector<ShareMode> out;
    const size_t key = json.find("\"shareModes\"");
    if (key == std::string::npos) return out;
    const size_t open = json.find('[', key);
    if (open == std::string::npos) return out;
    const size_t close = json.find(']', open);
    if (close == std::string::npos) return out;
    const std::string body = json.substr(open + 1, close - open - 1);

    size_t i = 0;
    while (i < body.size()) {
        const size_t modeAt = body.find("\"mode\"", i);
        if (modeAt == std::string::npos) break;
        const size_t entryAt = body.find("\"entryPoint\"", modeAt);
        if (entryAt == std::string::npos) break;
        ShareMode m;
        // 从键名**之后**开始找引号。从 `modeAt` 开始会先撞上 `"mode"` 自己的
        // 收尾引号，于是读回来的是 `entryPoint` 这个键名本身——第一版就这么错的，
        // 表现为「声明了却打不开」，而看上去像是导出表出了问题。
        if (!readString(body, modeAt + strlen("\"mode\""), m.mode)) break;
        if (!readString(body, entryAt + strlen("\"entryPoint\""), m.entryPoint)) break;
        out.push_back(m);
        i = entryAt;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dllPath = (argc > 1) ? argv[1] : ".\\nae.dll";
    std::printf("== 加载 %s ==\n", dllPath);
    HMODULE lib = LoadLibraryA(dllPath);
    if (!lib) {
        std::printf("  LoadLibrary 失败: %lu\n", GetLastError());
        return 1;
    }

    // 动态解析，和 test_main.cpp 一样：不链接导入库，走真实的动态加载路径。
    using CapabilitiesFn = const char* (*)();
    auto capabilities =
        reinterpret_cast<CapabilitiesFn>(GetProcAddress(lib, "NAE_GetEngineCapabilities"));
    if (!capabilities) {
        std::printf("  NAE_GetEngineCapabilities 解析不到\n");
        FreeLibrary(lib);
        return 1;
    }

    const char* raw = capabilities();
    check("NAE_GetEngineCapabilities 返回非空", raw != nullptr);
    if (!raw) {
        FreeLibrary(lib);
        return 1;
    }
    const std::string json(raw);
    std::printf("\n== 能力声明原文 ==\n  %s\n", json.c_str());

    const std::vector<ShareMode> modes = shareModesFrom(json);
    std::printf("\n== 逐项核对：声明的模式必须真的有导出入口 ==\n");

    if (modes.empty()) {
        // 空列表本身是另一类谎：宣称自己什么都开不了会让调用方以为引擎坏了。
        // 这台机器上 exclusive 是能开的（其余仪器天天在用），所以空列表是错的。
        check("shareModes 非空", false, "声明了一个都打不开的引擎");
        FreeLibrary(lib);
        std::printf("\n%d 失败\n", g_failures);
        return 1;
    }

    for (const ShareMode& m : modes) {
        check((m.mode + " 声明了入口名").c_str(), !m.entryPoint.empty(),
              m.entryPoint.empty() ? "声明里没给入口名，外部无从核对" : m.entryPoint);
        if (m.entryPoint.empty()) continue;
        const bool hasEntry = GetProcAddress(lib, m.entryPoint.c_str()) != nullptr;
        check((m.mode + " 的入口在导出表里").c_str(), hasEntry,
              hasEntry ? m.entryPoint
                       : (m.entryPoint + " 不在 DLL 导出表里 —— 声明了却打不开"));
    }

    // 反向不查：导出表里有 `NAE_OpenExclusiveSine` 这类不属于 share mode 的
    // 入口，不该因为「没被声明」就算失败。能力声明是**允许**的集合，不是全集。

    FreeLibrary(lib);
    std::printf("\n%s（%d 失败）\n", g_failures == 0 ? "全部通过" : "有失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
