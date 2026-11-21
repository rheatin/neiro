# iOS Phase 0 · S1 引擎静态链接尖刺

**状态：** 工作包，未开工。属于 spike，不是最终设计；`/egm:egm-draft` 的产物可以覆盖本文件的细节。

## 目标

证明 `neiro-ui` 能在 `aarch64-apple-ios` 上编译，并通过**静态链接**调用 `neiro_base` 的 C ABI。只验证接缝，不验证音频、不验证音质、不接真实引擎。

## 不做什么

- 不改 UI 布局、不做 dock、不做页面。
- 不接真实 `neiro_base` 音频路径，不碰 `AVAudioSession`。
- 不动桌面端的运行时加载行为。
- 不碰 `scripts/build-ffmpeg-lgpl-macos.sh`，不提交任何东西。

## 已知事实

- `ui/src/engine.rs` 全程用 `libloading` 运行时加载，`ui/Cargo.toml` 里 `libloading` 是无条件依赖。
- `scripts/fetch-core.sh` 只取 `neiro-core-${version}-macos-${arch}` 的 dylib，没有 iOS 产物。
- `ui/.nsh/rust/ios-app` 与 `ios-host` 已由 NSH 生成，依赖 `neiro-ui`；`nsh run --target ios` 可用，但本机 `ios` runtime 未安装、模拟器列表为空。
- Xcode 27.2 已安装；NSH 0.6.1。

## 任务

### 0. 工具链

- `rustup target add aarch64-apple-ios aarch64-apple-ios-sim`
- 安装 NSH iOS runtime（`nsh component add ios`，或让 `nsh run --target ios` 自行安装）
- 准备一个可用的 iPhone 模拟器

### 1. 造一个 stub 静态库

在 `spike/neiro_base_stub/` 下写最小 C 源：

```c
const char *NAE_BaseVersion(void) { return "stub-0.0.1"; }
const char *NAE_BaseHealthJson(void) { return "{\"state\":\"stub\"}"; }
```

用 `xcrun clang -target arm64-apple-ios -c`（真机）和 `-target arm64-apple-ios-simulator`（模拟器）各编一份，`libtool -static` 成两个 `.a`，再用 `xcodebuild -create-xcframework` 合成 `neiro_base.xcframework`。

验收：`lipo -info` 能看到 device 与 simulator 两个 slice。

### 2. 开出 iOS 接缝

- `ui/Cargo.toml`：把 `libloading` 改成 `[target.'cfg(not(target_os = "ios"))'.dependencies]`。
- `ui/src/engine.rs`：把加载逻辑收进 `#[cfg(not(target_os = "ios"))]` 模块；新增 `#[cfg(target_os = "ios")]` 模块，用 `extern "C"` 声明 `NAE_BaseVersion` / `NAE_BaseHealthJson`，提供一个和桌面同名的内部接口（至少 `version()` 与 `health()`）。
- 链接：用 `build.rs` 输出 `cargo:rustc-link-search=native=...` 与 `cargo:rustc-link-lib=static=neiro_base`，路径指向 stub 的 xcframework slice；不要硬编码到用户目录。

验收：`cargo check --target aarch64-apple-ios` 通过，且 iOS 分支里没有任何 `libloading` 引用。

### 3. 在模拟器里真的调用一次

- 临时把 `layout()` 对 iOS 的返回值从 `"unsupported"` 改成 `"mobile"`，渲染一个最小占位页（只显示 `NAE_BaseVersion()` 的返回值），不要做任何 dock/导航。
- `nsh run --target ios` 启动模拟器，确认占位页显示 stub 版本号。
- 验证完把 `layout()` 的临时改动撤回；这一步只为证明 host → Rust → C 的链路。

验收：模拟器里能看到 stub 版本字符串；没有崩溃；桌面 `nsh check` 与既有测试不受影响。

## 风险

- 真正的 `libneiro_base.a` 必须由 `neiro-core` 侧产出；本 spike 只能证明接缝，不能证明解码/输出。
- C++ 运行时、FFmpeg（LGPL）、SoXR 的静态链接会在真库进来时变成新问题，需要单独的链接实验。
- iOS 上 `dlopen`/插件路径必须全部编译剔除，否则 App Store 审核与运行都会出问题。

## 完成后的下一包

S2：RemoteIO 播放一首本地 FLAC，记录路由、采样率、中断与锁屏行为，产出 audio-session 契约。S1 未通过前不开 S2。
