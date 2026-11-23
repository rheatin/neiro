# AGENTS.md — Neiro Standalone UI Host

本仓库（`neiro`）是 Neiro 高保真音频系统的独立客户端宿主，基于 [Native Shell](https://nsh.dev)（NSH）TSX 与 Rust 宿主集成 crate（`neiro-ui`）构建。

前端与宿主仅通过 `neiro-core` 发布的 C ABI（桌面端 `neiro_base.dll` / `libneiro_base.dylib`，移动端静态链接 `neiro_base.xcframework`）与底层引擎通信。预构建 SDK 通过 `scripts/fetch-core.sh` / `scripts/fetch-core.ps1`（iOS 为 `scripts/fetch-core-ios.sh`）获取，**切勿在本仓库假设存在本地 C++ 引擎编译或 Git 子模块**。

---

## 规范路由表（`.agents/`）

修改代码前，请按任务类型查阅对应的规范文档：

| 主题 | 文档 | 说明 |
|---|---|---|
| 总体架构、目录分工、状态归属、导航 | [`.agents/architecture.md`](.agents/architecture.md) | 四层架构、单所有者状态注册表、`NavStack` 多根栈 |
| 核心不变量、编码红线、性能与跨平台规范 | [`.agents/rules.md`](.agents/rules.md) | 必须遵守的红线（含 `Drop` 安全、`remote://`、iOS 宿主不变量） |
| 界面插件、多外壳（Shell）切换、权限门控、歌词源、光效 | [`.agents/ui-plugins.md`](.agents/ui-plugins.md) | 三层插件、`DesktopShell`/`StudioMinimalShell`/外部外壳契约 |
| UI 设计系统、Token、控制台表面、布局与 WGSL 动效 | [`.agents/ui-design-system.md`](.agents/ui-design-system.md) | 语义 Token、硬件控制台六大表面、GPU 着色器约束 |
| `neiro-core` C ABI 契约、版本兼容与 SDK 拉取 | [`.agents/core-abi.md`](.agents/core-abi.md) | 必选/可选符号、`duration_ms` 规范化、`v0.2.0` `-2` 降级与 iOS ABI v2 |
| 本地验证命令、无头测试写法、PR 检查清单 | [`.agents/workflows.md`](.agents/workflows.md) | `nsh check`、`cargo test`、`nsh test`、MacBook iOS 验证与 CI 策略 |
| 历史交接与排障记录 | [`ui/HANDOVER.md`](ui/HANDOVER.md) | NSH 升级记录、已知问题与测量基线 |

---

## 不可妥协的核心摘要

1. **单所有者状态**：Bit-Perfect 判定唯一归属 `isStreamBitPerfect()`（`ui/src/state/resolved.ts`）；采样率格式化归 `formatHz`；Shuffle/Repeat 归 `ui/src/state/player.ts`；输出策略归 `applyPolicy()`。
2. **诚实遥测**：引擎未上报的字段保持 `null`（UI 显示 `—`），严禁伪造；降级模式下不得把永久内置解码器/DSP 伪报为可开关。
3. **Engine `Drop` 与 C ABI 兼容**：重构 `ui/src/engine.rs` 时必须保留 `Drop for Engine`（`unsubscribe` + 释放 `ctx` + 有界 `shutdown` + 超时泄漏库句柄防卸载崩溃）；跨边界 `QueueItem` 必须归一化为 snake_case `duration_ms: u64` 并保留 `-2` 降级。
4. **远程 `remote://` 与性能**：播放前必须拦截 `remote://`（仅在 Provider 声明 `source: true` 且非 iOS 时直读，否则先缓存落地）；大列表必须用虚拟化 `<List>`；60Hz FFT 仅在播放且存在可见视觉消费者时开启。
5. **仓库与编码卫生**：`.ps1` 脚本仅用纯 ASCII；不用未指定编码的 PowerShell 命令读写 UTF-8 文件；不向根目录提交临时评审/审计 `.md` 草稿。

---

## 快速验证命令

```bash
(cd ui && nsh check)
cargo test --manifest-path ui/Cargo.toml --lib
cargo test --manifest-path ui/Cargo.toml --test engine_shutdown
(cd ui && nsh test)
```
