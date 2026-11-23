# Neiro UI 设计语言、布局与动效规范

本文件规范 `neiro` 的视觉 Token、专业调音台控制台表面、布局契约与 WebGPU 动效边界。

---

## 1. 视觉美学与语义 Token

- **设计基调**：现代极简专业工业音频美学（暗色单背景、三面板圆角侧栏、悬浮播放条、CS-02 调音台控制台）。
- **强制使用语义 Token**：颜色与主题统一使用 `ui/src/components/ui/tokens.ts` 及 NSH 类名（`token("background")`、`token("card")`、`token("primary")`、`bg-sidebar`、`border-border`、`text-muted-foreground` 等），**避免硬编码十六进制色值**。
- **数字与遥测字体**：采样率、位深、时长、电平 dB 等工程遥测读数使用 `font-mono` 或 `ui/src/components/ui/digits.tsx`（等宽数字防止播放中跳动）。

---

## 2. 硬件控制台表面（`ui/src/components/player/surface/*`）

Output 控制台由六个专业模块组成：
- `source.tsx`（音源输入规格）
- `dsp-matrix.tsx`（DSP 矩阵与路由状态）
- `rack-eq.tsx`（机架均衡器）
- `master-bus.tsx`（主总线指示灯）
- `output-interface.tsx`（输出接口与设备能力）
- `telemetry.tsx`（实时采样与延迟遥测）

所有绿灯 / 旁路（Bit-Perfect vs DSP Active）判定**必须**调用 `isStreamBitPerfect()`，确保多处面板显示完全一致。

---

## 3. 布局、页壳与浮层规范

- **布局派生（`ui/src/components/layout.tsx`）**：
  - 所有页面与底部留白统一读取 `layout()`、`sidebarWidth()`、`barSize()`、`wideTracks()`、`pageEnd()`，禁止在页面里写死与特定壳层耦合的尺寸魔法数字。
- **页壳与滚动记忆**：
  - 页面统一由 `<AppPage>`（`ui/src/components/page.tsx`）包裹，并搭配 `rememberScroll(key)`（`ui/src/state/page-scroll.ts`）保存/恢复滚动位置。
- **互斥浮层（`ui/src/state/overlays.ts` & `ui/src/components/ui/overlays.tsx`）**：
  - Now Playing、Output 控制台、队列面板、插件管理器、服务器弹窗均通过 `overlays.ts` 统一调度互斥开关，组件复用 `<AppDialog>` 与 `<AppSheet>`。
- **可访问性与测试标识**：
  - 可交互控件统一带 `focusRing`、`accessibilityLabel`，关键按钮/列表建议补充 `testID` 以便 `nsh test` 无头测试定位。

---

## 4. 布局动画与 WGSL 光效规范

- **作用域布局动画（`ui/src/components/ui/motion.ts`）**：
  - 优先使用绑定局部 `ViewRef` 的 `animateLayoutOn(() => ref, ...)`（如 `DesktopShell`、播放条、Now Playing、歌词重排），避免全局布局动画把隐藏 tab 一并送入动画队列；遵循 `reduceMotion`。
- **WGSL 光效着色器（`ui/src/components/ui/toolbar-fx.tsx`）**：
  - 光效亮度必须经过 WGSL `tame()` 约束（上限 `0.32`），保证前景播放/暂停等控件对比度；
  - 着色器完全在 GPU 原生帧循环中通过 uniforms（`BASS`、`BEAT`、`BAND0..3`、`LEVEL`、`CLOCK`、`PHASE` 等）驱动，**严禁在 JS 侧用 `requestAnimationFrame` 逐帧推送样式**。
