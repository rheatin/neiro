# Neiro UI

Neiro 音频系统的独立桌面客户端宿主，用 [Native Shell](https://nsh.dev)（NSH）绘制：
TSX 界面 + Rust 转接层（`neiro-ui`），通过 Base 的 C ABI (`neiro_base.dll` / `libneiro_base.dylib`) 驱动底层音频引擎与本地/远程曲库。

界面设计遵循现代极简专业工业美学：暗色单背景、三面板圆角侧栏、浮动播放条、全屏 Now Playing、CS-02 调音台控制台（Output Console）、虚拟化曲目表与卡片网格、14 种 WebGPU 音乐光效着色器。

![home](docs/preview.png)

## 构建与运行

前置要求：`nsh` CLI 与 Rust 工具链。

```sh
nsh check        # cargo check + tsc 类型检查
nsh run          # 构建并在桌面窗口运行
nsh test         # Rust 测试 + TSX 无头集成测试
```

### 底层 Core SDK 准备

本仓库为独立 UI 宿主，无需就地编译 C++ 引擎。动态库通过预构建脚本获取：

```sh
# macOS / Linux
./scripts/fetch-core.sh

# Windows (PowerShell)
powershell -ExecutionPolicy Bypass -File scripts/fetch-core.ps1
```

也可将 `neiro_base.dll`（Windows）或 `libneiro_base.dylib`（macOS）直接放入 `ui/prebuilts/` 或设置环境变量 `NEIRO_BASE_DLL=<路径>`。没有引擎时启动会显示可重试的"引擎未连接"引导页，不导致白屏或崩溃。

## 结构

| 层 | 路径 | 职责 |
|---|---|---|
| Rust 转接层 | `src/engine.rs` | 打开 `neiro_base.dll`，处理 C ABI 动态符号、命令分发、双版本兼容降级与参数规范化 |
| Rust TSX 宿主 | `src/lib.rs` | `Base` 类：异步事件泵、生命周期管理、TSX 桥接表面（`onXxx` + 命令） |
| 后端桥接 | `src/backend/engine.ts` | 唯一调用 `"neiro/native"` 的强类型单例；信号镜像与可观测性 |
| 状态与选择器 | `src/state/*` | player（权威播放状态、单所有者 Shuffle/Repeat）、resolved（权威 Bit-Perfect 判定与持久化输出策略）、catalog（本地曲库、分页与看门狗）、remote（Jellyfin/Navidrome 会话与缓存取流）、artwork（LRU 封面缓存） |
| 组件表面 | `src/components/*` | UI 原语、侧栏、播放条、Now Playing、Output 控制台 (`surface/*`) |
| 页面 | `src/pages/*` | 首页、搜索、专辑、歌手、曲目、设置、均衡器、来源详情 |
| 外壳与路由 | `src/shells/*`、`src/nav.ts` | 桌面外壳（DesktopShell）、极简黑胶台外壳（StudioMinimalShell）、插件化外壳拓展 |

## 核心设计与不变量

- **单所有者（Single Owner）状态**：
  音频流 Bit-Perfect 状态以 `ui/src/state/resolved.ts` 的 `isStreamBitPerfect()` 为唯一权威判定；Shuffle / Repeat 以 `ui/src/state/player.ts` 为唯一所有者；用户选择的输出策略由 `applyPolicy` 持久化并在每次启动连接后恢复。
- **C ABI 双版本向下兼容**：
  在 `v0.2.0` 旧版 DLL 与 `v0.2.1`（QueueDirector）双版本间无缝兼容，对新版特性提供自动化 JSON 规范化（`duration_ms: u64`），对旧版缺失符号提供 `-2` 优雅降级。
- **远程虚拟路径拦截**：
  对 Jellyfin / Navidrome / WebDAV 的 `remote://` 虚拟路径，在首曲点播与入队时主动完成缓存落地，杜绝底层解码器直接打开虚拟 URI 报错。
- **按需计算与资源节约**：
  60Hz FFT 频谱帧仅在有实际可见消费者时开启，后台播放自动关闭降低 IPC 与 CPU 负载；大型文件夹与全曲库均采用虚拟化 `<List>` 杜绝大目录卡顿。
