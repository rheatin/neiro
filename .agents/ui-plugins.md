# Neiro UI 界面插件、外壳（Shell）与扩展点规范

本文件定义 `neiro` 界面层的三层插件体系、热插拔外壳（Shell）契约、权限门控及内置扩展点。

---

## 1. 三层插件架构

`PluginInfo.tier` 将系统插件分为三层，由 [plugins.ts](file:///c:/Users/Char/Documents/Repos/neiro-ui/neiro/ui/src/state/plugins.ts) 统一管理并映射到 [plugin-dialog.tsx](file:///c:/Users/Char/Documents/Repos/neiro-ui/neiro/ui/src/components/ui/plugin-dialog.tsx) 的五大标签页（`all` 全部、`ui` 界面与光效、`sources` 音源与分轨、`lyrics` 在线歌词源、`engine` 音频引擎核心）：

| 层级 (`tier`) | 典型类别 (`category`) | 代表插件 |
|---|---|---|
| `ui` | 界面外壳、光效着色器、在线歌词源 | `builtin.shell.industrial-rack`、`builtin.shell.studio-minimal`、`builtin.visualizer.signal-chain`、`builtin.lyrics.*` |
| `base` | 远程连接器、虚拟分轨展开器 | `jellyfin`（合并 `jellyfin_token`）、`navidrome`、`webdav`、`builtin.expander.cue`、`builtin.expander.sacd` |
| `engine` | 底层解码器、重采样与 DSP 插件 | `builtin.decoder.dsd`、`builtin.dsp.soxr`、C ABI 动态 DSP 插件 |

- **锁定核心插件（`LOCKED_PLUGIN_IDS`）**：`DEFAULT_SHELL_ID`、`builtin.expander.cue`、`builtin.expander.sacd`、`builtin.decoder.dsd`、`builtin.dsp.soxr` 不可被动态停用，`isPluginEnabled()` 恒为 `true`。
- **Provider 折叠规则**：`mergePluginList()` 与 `mergeProviderList()` 会将 `jellyfin` 与 `jellyfin_token` 折叠为单一的 `jellyfin` 入口，避免界面出现重复服务器类型。

---

## 2. 可热插拔 UI 外壳（Hot-Swappable Shells）

- **内置外壳**：
  - `DEFAULT_SHELL_ID`（`"builtin.shell.industrial-rack"`）→ `<DesktopShell />`（`ui/src/shells/desktop.tsx`）
  - `STUDIO_MINIMAL_SHELL_ID`（`"builtin.shell.studio-minimal"`）→ `<StudioMinimalShell />`（`ui/src/shells/studio-minimal.tsx`）
  - 当前激活的外壳 ID 存储在 `activeShellId` 并持久化于 `Storage` 键 `neiro:active-shell-id`。
- **外部外壳注册**：
  - 通过 `registerExternalUiShell(id, render)` / `getExternalUiShell(id)` 注册自定义外壳渲染函数。
  - 若外部外壳渲染异常，通过 `shellError` 捕获并自动回退到经典工作台，保证应用不白屏。
- **编写新外壳的契约**：
  1. 必须挂载 `<MainStack />`（`ui/src/shells/main.tsx`）承载主路由页面；
  2. 必须适配 `titleBarArea()`（桌面窗口拖拽与红绿灯避让）或 `safeArea()`；
  3. 遥测/Bit-Perfect 状态必须调用 `isStreamBitPerfect()`，禁止自行推算；
  4. 必须提供切回默认外壳（`switchShell(DEFAULT_SHELL_ID)`）或打开插件管理器（`openModal("plugins")`）的入口，避免用户切入新外壳后被锁死。

---

## 3. 权限门控与生命周期（`NeiroPluginContext`）

通过 `createPluginContext(manifest)` 为插件提供受控沙箱上下文，所有特权操作均通过 `assertPerm(perm)` 校验，缺失权限时抛出 `PermissionDeniedError`：

| 权限标识 | 对应能力 |
|---|---|
| `tags.write` | `writeTags()`、`writeLyrics()`、`setArtwork()` |
| `library.manage` | `addFolder()`、`forgetFolder()` |
| `remote.manage` | `setRemote()` |
| `lyrics.read` | 在线歌词检索（`LYRICS_PLUGINS`） |
| `remote.read` | 远程媒体库读取 |

- 插件注册的副作用或监听器必须通过 `ctx.subscribe(disposer)` 登记，并在卸载/停用时由 `ctx.dispose()` 统一释放。

---

## 4. 内置 UI 扩展点

1. **在线歌词源（`LYRICS_PLUGINS` + `ui/src/lyrics_providers.rs`）**：
   - 包含 `builtin.lyrics.lrclib`、`builtin.lyrics.netease`、`builtin.lyrics.qqmusic`、`builtin.lyrics.kugou`。
   - 新增歌词源需同步在 `LYRICS_PLUGINS` 声明元数据并在 Rust 侧实现检索逻辑。
2. **光效与频谱插件（`builtin.visualizer.signal-chain`）**：
   - 通过 `isVisualizerPluginEnabled()` 控制开关；
   - 14 种 WebGPU WGSL 光效着色器集中定义在 `ui/src/components/ui/toolbar-fx.tsx`（`dots`、`grid`、`wave`、`orb`、`spectrum`、`bars`、`ribbons`、`halo`、`aurora`、`mountains`、`mesh`、`particles`、`strings`、`current`）。

---

## 5. 平台约束

- 动态运行时插件加载与外部 UI 外壳仅限桌面端（Windows / macOS）；iOS 端因 App Store 审核限制应禁用外部代码/外壳动态加载。
