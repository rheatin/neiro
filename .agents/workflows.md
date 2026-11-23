# Neiro UI 验证命令、测试规范与 PR 检查清单

---

## 1. 本地验证命令

在 `neiro` 仓库根目录运行：

```bash
# 1. Rust crate 与 TSX 类型/编译检查
(cd ui && nsh check)

# 2. Rust 单元测试（不依赖外部引擎二进制）
cargo test --manifest-path ui/Cargo.toml --lib

# 3. 引擎关闭防卡死集成测试（需本地已拉取/放置 neiro-core SDK）
cargo test --manifest-path ui/Cargo.toml --test engine_shutdown

# 4. 全部 TSX 无头集成测试
(cd ui && nsh test)
```

Windows 下如修改过 `.ps1` 脚本，还需运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check-encoding.ps1
```

### macOS 与 iOS 本地实机/交叉验证

为珍惜 GitHub Actions 额度，macOS 与 iOS 校验优先在本地 MacBook（`ssh char@192.168.1.184`，仓库位于 `~/Repos/neiro` 与 `~/Repos/neiro-core`）执行：

```bash
# iOS 设备与模拟器目标编译检查
cargo check --manifest-path ui/Cargo.toml --target aarch64-apple-ios
cargo check --manifest-path ui/Cargo.toml --target aarch64-apple-ios-sim

# iOS 模拟器打包、注入隐私清单并校验
(cd ui && nsh package --target ios-simulator)
./scripts/ios-prepare.sh ui/.nsh/out/Neiro.app
./scripts/ios-prepare.sh --check ui/.nsh/out/Neiro.app
```

### CI 触发策略

- `.github/workflows/`（`ios.yml`、`macos.yml`、`windows.yml`）仅在 **打 `v*` Tag**、**PR 合并（`pull_request: closed` 且 `merged == true`）** 与 **手动 `workflow_dispatch`** 时运行；普通分支 push 与未合并 PR 不会消耗云端 CI 额度。

---

## 2. 无头 UI 测试编写规范（`ui/tests/`）

- 测试由 `*-harness.tsx`（注入状态与 mock `engine` 方法）和 `*.test.tsx`（通过 `nsh/test` 的 `open`、`press`、`waitFor`、`shows`、`storage` 驱动）组成。
- **测量真实结果而非固定睡眠**：用 `waitFor` 等待真实 UI 状态或存储结果，禁止用无意义的固定 `sleep` 碰运气。
- **Mock 与真实生命周期对齐**：新增的 `Engine` 方法或生命周期钩子（如 `Drop`、书签解析、Keychain）必须在测试中验证真实行为，避免空操作导致假阳性通过。

---

## 3. PR / Vibe-Coding 提交前检查清单

- [ ] **单所有者**：是否复用了 `isStreamBitPerfect()`、`formatHz`、`applyPolicy()`、`shuffleMode`/`repeatMode`？
- [ ] **C ABI & 生命周期**：`Engine` 的 `Drop` 清理链路（`unsubscribe` + 释放 `ctx` + 有界 `shutdown`）是否完整？跨 ABI 的 `QueueItem` 是否包含 `duration_ms`？旧版 `-2` 降级是否仍生效？
- [ ] **远程路径**：`remote://` 虚拟路径是否在播放前被正确拦截/缓存落地（或仅在 `source: true` 且受支持的平台透传）？
- [ ] **UI 性能**：大列表是否使用虚拟化 `<List>`？60Hz FFT 采样是否在不需要时关闭？
- [ ] **CI 与版本**：若改动了 `NEIRO_CORE_VERSION` 或 `.github/workflows/`，目标 Release 的 Windows / macOS（及 iOS）资产是否齐全？CI Secret 缺失时的跳过逻辑是否安全？
- [ ] **仓库整洁**：根目录没有遗留临时评审/审计 `.md` 草稿，`.ps1` 为纯 ASCII 且无 BOM 损坏。
