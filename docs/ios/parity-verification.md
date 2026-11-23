# iOS 与 macOS 完整性验证

**日期：** 2026-11-21
**范围：** 一个 Rust crate（`neiro-ui`）＋一套 TSX 视图，出两个平台的可用产物：iOS 模拟器应用与 macOS 窗口应用。
**目的：** 验证"尽可能用一套代码出多个平台的视图"在接缝上成立——共享层确实共享，平台差异只收敛在少数几个已知位置。这是**接缝验证**：iOS 侧不验证解码、音质与真实 `neiro_base` 音频路径。

## 1. 共享面与平台专属面

iOS 与 macOS 共用同一份 `ui/src/**`（组件、页面、状态、格式化器）。平台差异被收进四处：

| 层 | 内容 | iOS | macOS |
| --- | --- | --- | --- |
| 视图 `ui/src/**` | 组件、页面、状态、格式化器、虚拟列表 | 共用 | 共用 |
| 布局判据 `layout()` | `ui/src/components/layout.tsx` | 不变，仍返回 `unsupported` | 返回 `desktop` |
| 挂载分支 `ui/App.tsx` | 选择哪个壳层 | `platform.os === "ios"` → iOS 尖刺页 | `<DesktopShell>` |
| 引擎加载 `ui/src/engine.rs` | 同一个 `Engine` 接口，两种加载器 | `engine/ios.rs`：`extern "C"` 链接期解析，无 `libloading` | `engine/desktop.rs`：运行时 `dlopen`，保留 `-2` 回退 |
| 链接 `ui/build.rs` | 产物接缝 | 选 xcframework slice，发 `-l static=neiro_base` | 空操作（桌面在运行期找 dylib） |
| 依赖 `ui/Cargo.toml` | `libloading` 归属 | 不参与 iOS 构建（按目标条件排除） | 参与 |
| 引擎接口 `base_abi_version()` | `ui/src/lib.rs` → `backend/engine.ts` | 返回 core ABI 版本号 | 返回 0（桌面没有该 ABI） |

**共享层没有因为 iOS 改动而出现平台分支。** 唯一的分支在挂载处（`App.tsx` 一行 `platform.os === "ios"`）和加载器处（`engine.rs` 的 cfg 分派），其余——播放、曲库、远程、EQ、歌词、虚拟列表、格式化——两个平台跑的是同一份代码。

## 2. iOS 验证结果

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 切片选择（设备 / 模拟器） | 通过 | `build.rs` 按 `TARGET` 选 `ios-arm64` / `ios-arm64-simulator`，CI 里对两个切片都做 `nm` 校验 |
| 静态链接 | 通过 | 二进制内 19 个 `_NAE_Base*` 全部为 `T`（定义）；`nm -u` 无未定义的 `NAE_*` |
| 无动态引擎 | 通过 | `otool -L` 无 `neiro_base` 动态库条目，只有系统框架（UIKit、Metal、Foundation 等） |
| 无 `libloading` | 通过 | iOS target 下找不到任何 `libloading` 产物；残留的 `_dlopen/_dlsym` 来自 `nsh-platform-ios`、`nsh-platform-apple`、`objc2`，与引擎加载无关 |
| 链接 ABI 与启动 | 通过 | 模拟器（iPhone 18 Pro · iOS 27.2）启动，页面显示 **core ABI 2 · engine ready**，截图 `ios-spike-simulator.png` |
| 设备 target 编译 | 通过 | `cargo check --manifest-path ui/Cargo.toml --target aarch64-apple-ios` 通过（真机切片，37 秒） |
| 缺产物即失败 | 通过 | 把 `NEIRO_BASE_IOS_XC` 指向不存在的路径，构建立刻失败并点名 `ios-arm64/libneiro_base.a`，不再退化成符号缺失报错 |

`nsh run --target ios` 在模拟器上的实测：

```
✓ built the Rust crate neiro-ui
✓ built App.tsx (1009.7 KB js, 4.1 MB bytecode, 140ms)
✓ built the development host for the iOS simulator (31.2s)
• iOS Simulator (iPhone 18 Pro) connected, build 1
warn iOS Simulator: font family `Noto Sans SC Medium` is not available; using the system font
```

唯一警告是字体缺失，回退到系统字体，不影响接缝验证。

### 这一路踩到的两个坑

1. **桩库缺符号。** `NAE_BaseRemoteProviders` 与 `NAE_BaseListDirectory` 是设计文档后来要求 iOS 必须链接的符号，桩库当时只有 19 个里的 17 个。补上后链接即通。**早期"链接失败"的现象不是 `build.rs` 没跑，是桩库缺符号**——`build.rs` 的 `run/stdout` 一直是正确输出。
2. **缺产物只告警。** `build.rs` 原本对缺 xcframework 只发 `cargo:warning`，该警告在 `nsh` 下被吞掉，最后以一堆"undefined symbol"报错，看不出根因。现已改为直接失败并指出缺哪个 slice：`nsh` 会吞警告，而链接报错只说符号不说原因。契约（`core-contract.md` §5）同步改为"构建脚本失败"。

## 3. macOS 验证结果

| 检查 | 结果 |
| --- | --- |
| `(cd ui && nsh check)` | 通过（`neiro-ui` 检查 + 构建，1 个警告） |
| `cargo test --manifest-path ui/Cargo.toml --lib` | 24/24 通过 |
| `(cd ui && nsh test)` | 92 个测试，91 通过，1 个偶发失败（见下） |
| 应用启动 | 通过。窗口打开后停在首页：左侧栏（首页 / 设置 / 曲库）与首页卡片网格均正常渲染，截图 `macos-app.png` |

**一次失败，已定位为测试资产缺失，不是代码回归：** `tests/` 里依赖真实播放的四个测试（`dither-policy`、`info-drawer-play`、`player-bar`、`share-mode`）全部卡在"永远等不到 `label:暂停`"。根因是仓库根的 `analysis_tone.wav` 不存在——它被 `.gitignore` 忽略，本就是本地生成的夹具，CI 也不跑 `nsh test`（`.github/workflows/*.yml` 只跑 `cargo test --lib` 与 `nsh check`）。生成一个 30 秒 44.1 kHz 立体声正弦波后，这四个测试全部通过。

**一个偶发失败，记为 flaky：** `lyrics-context-menu` 在全量跑时失败一次（右键歌词面板短暂显示"换一份歌词…"而不是"在线搜索歌词…"），单独连跑两次均通过。与本次改动无关。

`nsh run --target macos` 的实测输出（除字体回退警告外无其他警告）：

```
✓ built the Rust crate neiro-ui (59.0s, 1 warning: -v shows it)
✓ built App.tsx (1009.7 KB js, 4.1 MB bytecode, 140ms)
✓ built the development host for macOS (1m 27s, 2 warnings: -v shows them)
• macOS (Chars-MacBook-Air) connected, build 1
warn macOS: font family `Noto Sans SC Medium` is not available; using the system font
warn macOS: font family `Consolas` is not available; using the system font
```

启动后窗口停在首页：左侧栏与首页卡片网格正常渲染（`macos-app.png`）。桌面端没有因 iOS 改动出现任何可见回归。

## 4. 复现命令

从仓库根执行：

```bash
export RUSTUP_HOME="$HOME/Library/Caches/puccinialin/rustup"
export CARGO_HOME="$HOME/Library/Caches/puccinialin/cargo"
export PATH="$HOME/.local/bin:$RUSTUP_HOME/toolchains/stable-aarch64-apple-darwin/bin:$CARGO_HOME/bin:/opt/homebrew/bin:$PATH"

# iOS：构建 + 启动（用本地桩库，指向一个已构建的 xcframework）
NEIRO_BASE_IOS_XC=$PWD/spike/ios_stub/neiro_base.xcframework \
  nsh run --target ios

# 桌面：类型检查、单测、无头 UI 测试
(cd ui && nsh check)
cargo test --manifest-path ui/Cargo.toml --lib
(cd ui && nsh test)

# 静态链接证据（二进制路径随 `nsh run --target ios` 产出）
nm -gU ui/target/aarch64-apple-ios-sim/debug/nsh-app-neiro-ui-ios-host | grep NAE
otool -L ui/target/aarch64-apple-ios-sim/debug/nsh-app-neiro-ui-ios-host
```

## 5. 未做与缺口

- **iOS 不验证音频。** 本次链接的是桩库（`spike/ios_stub`），不是 `neiro-core` 的真实产物；解码、RemoteIO、`AVAudioSession` 所有权、中断与路由事件都还没有接。真实 `neiro_base.xcframework` 由 `scripts/fetch-core-ios.sh` 取，签名后由 `docs/ios/core-contract.md` 验收。
- **iOS 只有占位页。** 挂载处显示 ABI 与引擎健康，没有曲库、播放条、远程面板等视图。设计文档（`docs/designs/neiro-ios.md`）的四个移动端根（HOME/LIBRARY/REMOTE/SEARCH）尚未实现，本阶段只证明接缝可链接、可调用。
- **未验证项：** 布局展开宽度（700/660 pt 与 40 pt 迟滞）、虚拟列表在 iOS 的回收行为、`layout()` 在折叠屏（iPhone Duo）展开/折叠下的切换。
