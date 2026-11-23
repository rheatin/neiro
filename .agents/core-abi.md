# `neiro` 与 `neiro-core` 跨仓 C ABI 与 SDK 契约

`neiro` 作为独立 UI 宿主，与 `neiro-core` 的全部交互均跨越 `nae_base.h` 定义的 C ABI 边界。

---

## 1. SDK 版本锁定与获取

- **版本锁定文件**：根目录 `NEIRO_CORE_VERSION`（当前主线为 `v0.3.2`）。
- **预构建拉取脚本**：
  - macOS / Linux：`./scripts/fetch-core.sh`
  - Windows：`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/fetch-core.ps1`
  - iOS：`./scripts/fetch-core-ios.sh`（支持 `--core-dir` 指定本地 `neiro-core` 仓库或 dist 目录）
- **本地联调覆盖**：
  - 放入 `ui/prebuilts/`，或设置 `NEIRO_BASE_DLL` / `NEIRO_CORE_DIST` 指向本地 `neiro-core` 构建产物。
- **CI 私有仓库鉴权**：
  - `rheatin/neiro-core` 为私有仓库，CI 拉取 Release 资产依赖 `secrets.CORE_RELEASE_TOKEN`；未配置时 `macos.yml` / `windows.yml` 会跳过引擎集成测试并给出 notice 提示。
- **升级版本前的发布检查**：升级 `NEIRO_CORE_VERSION` 前，必须确认 `rheatin/neiro-core` 对应 Tag 的 Release 已完整包含 **macOS arm64**、**Windows x64**（以及启用 iOS 门禁时的 **iOS XCFramework**）全部资产。

---

## 2. C ABI 符号矩阵与双版本兼容

### 必选基础符号（Missing 即报错 `E_ENGINE`）
- `NAE_BaseStartWithLibrary`
- `NAE_BaseCommand`
- `NAE_BaseSubscribe`
- `NAE_BaseUnsubscribe`
- `NAE_BaseSnapshot`
- `NAE_BaseFreeString`
- `NAE_BaseShutdown`

### 可选增强符号（`QueueDirector` / 插件 / 远程 / 目录）
- 队列控制：`NAE_BasePlayContext`、`NAE_BasePlayNext`、`NAE_BasePlayLater`、`NAE_BaseSetRepeatMode`、`NAE_BaseSetShuffleMode`、`NAE_BasePrevious`、`NAE_BaseRemoveQueueItem`、`NAE_BaseClearUserQueue`、`NAE_BaseMoveUserQueueItem`
- 远程与目录：`NAE_BaseRemoteProviders`、`NAE_BaseListDirectory`
- 插件控制：`NAE_BaseListPlugins`、`NAE_BaseSetPluginEnabled`、`NAE_BaseReloadPlugins`

### 兼容性铁律
1. **QueueItem JSON 规范化**：`ui/src/engine.rs` 中的 `normalize_queue_items_json` 必须将传入的 `QueueItem` 统一清洗并输出 snake_case 的 `duration_ms: u64`（即使 TSX 侧传入 camelCase `durationMs`）。
2. **`v0.2.0` `-2` 优雅降级**：若加载的动态库缺少上述可选符号或返回 `NAE_BASE_ERR_PARSE = -2`，`ui/src/engine.rs` 必须自动降级为 `v0.2.0` 原语命令（`play`、`enqueue`、`clear_queue`、`seek 0.0`），不得向用户抛出致命错误。
3. **iOS 静态链接 ABI v2 扩展（如合入 iOS 端口）**：`NAE_BaseAbiVersion`（要求返回 `2`）、`NAE_BaseAudioCapabilities`、`NAE_BaseReinitializeAudio`；iOS 构建需在编译前校验 xcframework 存在并链接系统音频框架。
4. **缓存策略与分析持久化扩展（向后兼容 JSON / Schema v6）**：
   - 新增 JSON 命令：`set_cache_policy`（`audio_budget_bytes: Option<u64>`，`0` 为仅流式、`null` 为不限）与 `clear_analysis_cache`（旧版 core 返回 `-2` 时由宿主静默忽略）。
   - `Snapshot` 新增 `#[serde(default)]` 字段：`analysis_cache_entries`、`analysis_cache_bytes`。
   - `library.db` `SCHEMA_VERSION = 6` 新增 `analysis_cache` 表持久化波形峰值与分析元数据。
