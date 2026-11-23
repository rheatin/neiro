# Neiro UI 开发规范与红线（Vibe Coding Rules）

在 `neiro` 仓库编写或重构任何代码时，以下规则为不可违反的硬性约束。

---

## 规则 1：单所有者状态（Single Owner State）

- 任何状态不得存在第二份独立维护或重新推算的副本：
  - **Bit-Perfect 状态**：全 UI 表面（`surface/source.tsx`、`surface/master-bus.tsx`、`surface/telemetry.tsx`、`shells/studio-minimal.tsx`、`format-badges.tsx`、`output-interface.tsx`、`pages/settings.tsx`）必须统一调用 `ui/src/state/resolved.ts` 中的 `isStreamBitPerfect()`。
  - **采样率格式**：统一使用 `formatHz`（`ui/src/state/resolved.ts`）。
  - **播放顺序**：`shuffleMode` 与 `repeatMode` 归属 `ui/src/state/player.ts`。
  - **输出策略**：统一经由 `applyPolicy(...)`（`ui/src/state/resolved.ts`）修改与持久化。

---

## 规则 2：诚实遥测（No Fake Values）

- 底层引擎未上报的字段保持 `null`，界面渲染 `"—"` 或隐藏，**严禁编造或回退为虚假的采样率、位深、延迟或 Bit-Perfect 状态**。
- 在缺乏动态插件控制 ABI 符号的降级模式下，宿主**不得**将内置永久解码器/DSP（`builtin.expander.cue`、`builtin.expander.sacd`、`builtin.decoder.dsd`、`builtin.dsp.soxr`）伪报为可开关状态。

---

## 规则 3：引擎生命周期与 `Drop` 安全

- `Engine`（`ui/src/engine.rs`）在销毁时**必须**执行完整的清理链路：
  1. 调用 `unsubscribe` 停止回调转发；
  2. 释放回调上下文 `Box::from_raw(self.ctx)`；
  3. 在有界超时（`SHUTDOWN_WAIT`）内等待 `shutdown` 完成；若超时未退出，必须 `std::mem::forget` 泄漏动态库句柄，**严禁在 Base worker 线程仍在运行时卸载 DLL/dylib**。
- 重构桌面/移动端加载器时，**绝不可**误删 `impl Drop for Engine`。

---

## 规则 4：远程虚拟路径（`remote://`）拦截与并发守卫

- 来自 Jellyfin / Navidrome / WebDAV 的远程曲目携带虚拟路径 `remote://{id}.{ext}`。
- 播放器派发前**必须**拦截 `path.startsWith("remote://")`：
  - 已命中本地缓存的文件直接播放本地路径；
  - 仅当 Provider 描述符明确声明 `source: true` 且当前平台支持 Range 流式读取时，才可直接透传 `remote://` 给 Base；
  - 否则必须在首曲点播与队列填充时通过 `withRemotePath` / `playRemoteByPath` 先完成本地缓存落地，杜绝 C++ 解码器直接打开无法识别的虚拟 URI。
- `ui/src/state/remote.ts` 中 `fetchedActions` 必须维持回调数组（`Array<(path: string) => void>`），防止并发请求丢失处理函数；所有分页响应必须校验 `listEpoch` 丢弃过期页。

---

## 规则 5：UI 性能与资源生命周期

- **列表虚拟化**：大型集合（全曲目、文件夹工作区、远程曲库等）必须使用虚拟化 `<List>`，严禁在 `<Scroll>` 中使用无界 `<Each>` 导致卡顿。
- **60Hz 频谱按需请求**：`visualiserWanted`（`ui/src/state/visualiser.ts`）仅在「正在播放」**且**「存在可见的视觉消费者（Now Playing 打开、Output 控制台打开、Studio Minimal 外壳激活、或工具栏光效非关闭）」时开启；隐藏时必须调用 `engine.setVisualiser(null)`。
- **缓存与订阅清理**：`artwork.ts` 淘汰 LRU 项时必须同步从 `asked` 集合删除以允许回滚重新拉取；后台目录分页必须带超时看门狗（15s）防止丢失 IPC 事件卡死；弹窗/抽屉关闭时必须显式取消异步订阅（如歌词检索）。

---

## 规则 6：跨平台分支与编码卫生

- 平台特有逻辑（如 iOS 的安全作用域书签、Keychain、移动壳层）必须严格隔离在 `platform.os === "ios"`（TSX）或 `#[cfg(target_os = "ios")]`（Rust）守卫之后，不得影响桌面端默认行为。
- **PowerShell 编码红线**：不要用不带 `-Encoding` 的 PowerShell `Get-Content` / `Set-Content` 改写 UTF-8 文件；`.ps1` 脚本必须保持 **纯 ASCII**，修改后运行 `powershell -File scripts/check-encoding.ps1` 检查。

---

## 规则 7：仓库与 PR 整洁

- 临时的评审报告、头脑风暴草稿、审计纪要（如 `critic_report.md`、`zoom_out_report.md`、`design_draft.md` 等）**禁止**留在仓库根目录并合入主分支；正式设计文档请归档到 `docs/designs/`。

---

## 规则 8：iOS 宿主不变量

- **双根目录分离**：`data_root()`（`ui/src/lib.rs`）在 iOS 上为 `Application Support`（保存 `library.db`、设备 ID、书签）；`cache_root()` 为 `Library/Caches`（保存远程下载与封面缓存）。可再生数据读 `cache_root()`，需备份恢复的数据读 `data_root()`；桌面端两者仍保持在同一目录。
- **凭据仅存 Keychain**：iOS 上服务器凭据保存在 Keychain（`service: app.neiro.ui.remote`、`account: remote:secret:{serverId}`、`AfterFirstUnlock`），通过 `Base::secret_get/secret_set/secret_delete` 访问。`remote:servers` 花名册仅保留 `hasSecret`；`ui/src/state/sources.ts`（`readServerSecret`、`upsertServer`）是唯一桥接层。
- **严禁将含凭据配置写入 `Storage`**：`saveRemoteConfig` 在 iOS 上跳过写入 `remote:config`，`upsertServer` 会在存储前剥离 `params` 中的敏感字段。
- **打包 Plist 与隐私清单**：`ios.infoPlist` 由 `ui/nsh.json` 唯一拥有（`UIBackgroundModes: ["audio"]`、`NSLocalNetworkUsageDescription`、`NSAppTransportSecurity.NSAllowsLocalNetworking`），不要手改产物内的 `Info.plist`；打包后通过 `./scripts/ios-prepare.sh <Neiro.app>` 注入 `ui/ios/PrivacyInfo.xcprivacy` 并重签。
- **iOS 走整文件远程缓存**：`servesSource`（`ui/src/state/remote.ts`）在 iOS 上固定返回 `false`，远程曲目需在 `cache_root()` 预算内先下载再播放。
- **文件保护级别**：数据目录与 `library.db` 在启动时设置 `NSFileProtectionCompleteUnlessOpen`（`ui/src/ios/mod.rs`），保证锁屏状态下已开始的播放不中断。

