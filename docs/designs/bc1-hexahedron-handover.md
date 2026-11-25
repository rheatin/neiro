# NEIRO BC-1 + K.O.-AMP 35 (`Hexahedron`) — AI 工程交接文档

> **用途**：本交接文档为独立、自包含的工程上下文。接手 AI 可直接基于本仓库 Git 内容继续执行正式 NSH 实现，无需依赖本地私有临时文件。

---

## 1. 交接总览与仓库资产索引

- **当前阶段**：设计范式、空间拓扑、逐面逐键功能蓝图及棚拍级交互原型已全部定稿并完成浏览器实测，**尚未开始写入 `ui/src/` 正式 NSH 代码**。
- **已落盘 Git 资产**：
  - 领域术语表：[GLOSSARY.md](../../GLOSSARY.md)
  - 架构决策记录：
    - [0001-orthogonal-ui-plugin-axes.md](../adr/0001-orthogonal-ui-plugin-axes.md)（**已被 ADR-0003 废止**，仅作历史记录，切勿再实现 `Surface` 换皮层）
    - [0002-mobile-builtin-only-ui-plugins.md](../adr/0002-mobile-builtin-only-ui-plugins.md)（桌面端与移动端对等内置 `Shell` 热切换；外部可执行插件仅限桌面端）
    - [0003-paradigm-shells-over-skinning.md](../adr/0003-paradigm-shells-over-skinning.md)（采用自包含范式 `Shell`，放弃浅层换皮）
    - [0004-hexahedron-hardware-shell-topology.md](../adr/0004-hexahedron-hardware-shell-topology.md)（**核心决定**：`BC-1` + `AMP-35` 六面体硬件壳层拓扑）
  - 定稿交互 Demo 与棚拍母版：
    - [neiro-studio-cube-demo.html](../prototypes/neiro-studio-cube-demo.html)（含 `3D 实体六面体` 与 `正视实体机 + 标注` 双模式）
    - 5 张棚拍母版图：`docs/prototypes/assets/{hero-34,face-top,face-front,face-right,face-left-dossier}.jpg`
- **本地预览 Demo 命令**（在 `neiro/` 根目录执行）：
  ```powershell
  npx http-server "docs/prototypes" -p 18889 -c-1 -o /neiro-studio-cube-demo.html
  ```
- **已清理淘汰方案**：已删除早期被否决的 4 个原型（`ui-sandbox.html`、`neiro-reframed-4views.html`、`neiro-te-broadcast.html`、`neiro-ko-cube-3d.html`），切勿恢复其扁平卡片布局或粗糙程序化 Canvas2D 贴图风格。

---

## 2. 已拍板红线与美学基准（请勿推翻重议）

1. **视觉基准**：严格对标 Teenage Engineering **Broadcast CD (`BC-1`) + `K.O.-AMP 35`** 实体硬件美学：
   - 机身材质：哑光冷灰注塑 ABS（`#D5D7DC`）、135° 棚拍软箱阴影。
   - 按键：炭黑（`#202227`）与骨白（`#F0F1F3`）立体键帽，极少量信号橙（`#FF5400`）机械卡扣与 `JOG` 拨杆。
   - 屏幕：纯黑下沉玻璃窗 + 红光 7 段数码管（`#FF2618`）。
   - 背景：深钴蓝棚拍绒毯（`#0B3262` → `#051833`）。
2. **禁止要求用户安装额外工具**：用户明确不安装 `pen.dev` 等第三方工具，直接使用 NSH 原生能力（`perspective`/`rotateX`/`rotateY`/`AnimatedValue`、WGSL `<Shader>`、`<Canvas>`、`<Image>`）或高保真静态母版+动效叠层完成。
3. **冷启动视角**：默认进入 **`3/4 实拍总览`**（`azimuth +38°`, `elevation +29°`），一屏同时呈现顶面播放 CD、正面唱片仓、右面网罩频谱与控制台。轻点控件原地触发；双击任意面或点击底部切换条旋转至正视焦点；按 `ESC` 退回 `3/4`。
4. **右面穿孔网罩频谱**：右面左侧的 **`24×48` 方形穿孔音箱网罩**已升级为**嵌入式背光 LED 矩阵频谱**（`BARS / MIRROR / PEAK / OFF`），仅点亮穿孔内的红/橙红发光单元，未点亮孔位必须保持透明透出实体暗孔质感。
5. **专辑展开为前景抽碟动作**：单击正面任意 CD 槽位后，在前景桌面滑出展开**牛皮纸碟套 + 可装载光盘（`LOAD DISC ▶`）+ 印刷曲目小册子（`Album Dossier`）**，不翻转到背面。

---

## 3. 六面体空间拓扑与逐面逐键功能规范

```text
                  ┌─────────────────────────┐
                  │   TOP · BC-1 播放界面    │
                  │  CD 仓 / 3×4 键 / 走带   │
                  └────────────┬────────────┘
┌─────────────────┬────────────┴────────────┬─────────────────┬─────────────────┐
│ LEFT · 歌词队列  │   FRONT · CD 专辑仓      │ RIGHT · 输出控制台 │ BACK · I/O 背板  │
│ 玻璃屏 + 6 微动键│  2×3 卡槽 + 顶部筛选     │ 24×48 网罩频谱+EQ│ 音源/插件/DIP 开关│
└─────────────────┴────────────┬────────────┴─────────────────┴─────────────────┘
                               │ (单击正面任意 CD 槽)
                  ┌────────────▼────────────┐
                  │ 前景抽碟 · Album Dossier  │
                  │ 牛皮纸套 + 光盘 + 曲目册  │
                  └─────────────────────────┘
```

### 3.1 顶面 · `BC-1 Deck`（播放界面）

| 编号 | 实体控件 / 区域 | 物理交互 | 对应 Neiro 真实功能 |
|---|---|---|---|
| `01` | **3×4 黑键 + `ENTER` 竖键** | `1–9` 直跳音轨；`ENTER` 确认两位曲号；`T` 切剩余/已播时间；`O` 切循环模式；`C` 复位 | 专辑内直跳音轨、时间与循环模式切换 |
| `02` | **下沉旋转 CD 仓 + 左下抠指槽** | 点击光盘播放/暂停；点抠指槽触发换碟 | 播放状态机；抠指槽切至正面 `CD Magazine` |
| `03` | **红光 7 段数码管 + L/R 电平表** | 只读遥测 | `TRACK / INDEX / MIN / SEC / VARISPEED / LINE LEVEL` + 双列电平 |
| `04` | **信号橙 `JOG` 拨杆** | 上下轻拨 ±5s；按住拖动精准寻道 | 音频 Seek |
| `05` | **`CUE 1 / 2 / 3` + 3 颗红灯** | 短按瞬跳，长按记录当前时间点 | 乐段 A/B/C 书签 |
| `06` | **走带键 `\|◀ / ▶\| / STOP / PLAY`** | 单击触发 | 上一首、下一首、停止归零、播放/暂停 |
| `07` | **`VARISPEED` 与 `LEVEL` 旋钮** | 旋转调节（双击 `VARISPEED` 归 `100.0%`） | 变速微调与主输出音量 |
| `08` | **顶缘丝印状态口** | 只读/点击跳设置 | `OUTPUT / BT / SYNC / DATA / USB / POWER` 实时状态 |

### 3.2 正面 · `CD Magazine`（专辑布局与媒体库）

| 编号 | 实体控件 / 区域 | 物理交互 | 对应 Neiro 真实功能 / 路由 |
|---|---|---|---|
| `01` | **红光 7 段碟数窗 (`006`)** | 只读计数 | 当前筛选条件下的专辑总数 |
| `02` | **筛选实体键** | 点按切换 | `ALL / PCM / DSD` + 扩展 `LOCAL / JELLYFIN / 歌单 / 收藏 / SEARCH`（映射 `albums / playlists / liked / remote / search`） |
| `03` | **`2×3`（宽屏 `3×3`）下沉唱片仓** | 滚轮虚拟化滚动承载 500+ 专辑；单击卡槽触发**前景抽碟**；双击光盘直接装载播放 | `albums` 浏览入口 |

### 3.3 前景抽碟 · `Album Dossier`（专辑展开）

| 编号 | 实体控件 / 区域 | 物理交互 | 对应 Neiro 真实功能 / 路由 |
|---|---|---|---|
| `01` | **牛皮纸碟套 + 光盘 + `LOAD DISC ▶`** | 点击装入顶面 CD 仓并从第 1 轨播放 | 整张专辑装载播放 |
| `02` | **印刷曲目小册子** | 单击曲目直点播放；支持收藏与加入队列 | 完整专辑详情（`album` / `remoteAlbum`）：曲目、采样率、DR 动态范围、Liner Notes |

### 3.4 右面 · `AMP-35 Console`（输出控制台 + 网罩频谱）

| 编号 | 实体控件 / 区域 | 物理交互 | 对应 Neiro 真实功能 / 路由 |
|---|---|---|---|
| `01` | **`24×48` 方形穿孔网罩（背光 LED 频谱）** | 单击循环模式：`BARS → MIRROR → PEAK → OFF` | 60fps 透光实时 FFT 频谱矩阵 |
| `02` | **纯黑红光遥测屏 + 双列 PPM** | 只读实时遥测 | 双声道 PPM、采样率（kHz）、位深（bit）、缓冲延迟（ms） |
| `03` | **4 颗针孔红灯互锁键** | 点按切换 | `DSP`（音效总闸）、`RATE`（源码率跟随）、`EXCL`（独占模式）、`UNITY`（0dB 直通） |
| `04` | **信号橙 `DIRECT` 物理卡扣** | 点按一键锁定/解除 | Bit-Perfect 纯净直通（自动旁路 DSP/EQ 并锁定 Unity） |
| `05` | **中置 `LEVEL` 刻度旋钮** | 拖动/滚轮微调 | 主输出增益（`DIRECT` 激活时提示锁定 0.0 dB） |
| `06` | **8 段实体 EQ 推子 + `32-BAND RACK`** | 上下拖动调节 | 快速 8 段参量均衡；`32-BAND RACK` 跳转 `eq` 全量均衡 |

### 3.5 左面 · `Lyrics & Queue Monitor` & 3.6 背面 · `Rear I/O Patchbay`

- **左面**：中央下沉玻璃屏在 `LYRICS`（滚动歌词+声波纹）与 `QUEUE`（待播队列）间切换；底部 6 颗微动键（`PREV / PLAY / PAUSE / 切屏 / 在线搜词 / NEXT`）。
- **背面**：`LOCAL STORAGE`（`library`）、`NETWORK / REMOTE`（`remote`：Jellyfin / Navidrome / WebDAV）、`EXPANSION SLOTS`（内置 `Shell` 与 `Acoustic FX` 切换）、`SYSTEM DIP SWITCHES`（`settings` 与 `Host Core` 安全恢复）。
- **底面**：静止底座铭牌与安全恢复入口。

---

## 4. 棚拍母版裁剪与叠层参考坐标

以下归一化坐标已在 `neiro-studio-cube-demo.html` 实测对齐（源图宽高 `0..1`）：

- **`FACE_CROPS`**：
  - `top`: `{ x: 0.112, y: 0.112, w: 0.776, h: 0.776 }`
  - `front`: `{ x: 0.138, y: 0.141, w: 0.724, h: 0.723 }`
  - `right`: `{ x: 0.108, y: 0.119, w: 0.784, h: 0.758 }`
  - `left`: `{ x: 0.116, y: 0.196, w: 0.402, h: 0.615 }`
- **关键动态区**：
  - Top CD 圆心 `(0.675, 0.308)` 半径 `0.173`；7 段数码管 `{ x: 0.160, y: 0.520, w: 0.595, h: 0.065 }`
  - Right `24×48` 穿孔网罩 `{ x: 0.160, y: 0.173, w: 0.328, h: 0.652 }`；遥测 `{ x: 0.695, y: 0.205, w: 0.108, h: 0.160 }`；互锁红灯 `y = 0.404, x = [0.573, 0.627, 0.682, 0.737, 0.792]`
  - Left 歌词玻璃屏 `{ x: 0.186, y: 0.373, w: 0.258, h: 0.208 }`

---

## 5. 下一阶段 NSH 正式实现执行指南

### 5.1 开工前向用户确认最后 1 个问题
先请用户运行 `neiro-studio-cube-demo.html` 对比选择：
- **桌面端**是采用 `Mode 1（真 3D 六面体旋转）` 还是 `Mode 2（正视实体机翻面）`（或桌面 3D + 移动端/iPhone Duo 正视翻面混合）。
- 各面按键功能是否有微调意见。

### 5.2 架构落点与必须复用的单所有者状态（严禁重复造轮子）
1. **Shell 注册**：新建 `ui/src/shells/hexahedron.tsx`（及 `ui/src/shells/hexahedron/` 子组件），在 `ui/src/shells/main.tsx`、`ui/src/App.tsx` 与 `ui/src/state/plugins.ts` 注册为内置 `Shell`；按 `ADR-0002` 对 Desktop 与 Mobile（含 `iPhone Duo`）对等开放热切换。
2. **单所有者状态复用**：
   - **Bit-Perfect 唯一真源**：`isStreamBitPerfect()`（`ui/src/state/resolved.ts`）
   - **采样率格式化**：`formatHz()`
   - **核心状态**：`ui/src/state/{player,queue,catalog,remote,eq,lyrics,foley,formats}.ts`
   - **路由映射（`ui/src/nav.ts` 共 17 个路由）**：
     - `home` → `3/4` 总览 / 顶面 `BC-1`
     - `albums / artists / tracks / liked / playlists / search` → 正面 `CD Magazine` 筛选与搜索态
     - `album / remoteAlbum / artist / playlist / remotePlaylist / remoteArtist` → 前景 `Album Dossier` 抽碟展开
     - `eq` → 右面 `AMP-35`（`32-BAND RACK` 展开）
     - `library / remote / settings` → 背面 `Rear I/O Patchbay`
3. **NSH 仓库红线（开工前必读 `neiro/AGENTS.md` 与 `.agents/rules.md`）**：
   - **60Hz FFT 节流**：仅在播放且右面频谱或左面波形可见时订阅 60Hz FFT，切走或暂停立即关闭。
   - **诚实遥测**：引擎未上报字段保持 `null`（UI 显示 `—`），严禁写死示例数字。
   - **虚拟化列表**：正面 500+ 专辑、队列与曲目列表必须使用虚拟化 `<List>`。
   - **`remote://` 安全拦截**：沿用现有缓存/直读链路，不可绕过。
   - **严禁提交 `ui/.neiro_plugins/`**，且不修改 `neiro-core` C ABI。

### 5.3 分阶段实施路线与必跑验证命令
- **Phase 1**：`Hexahedron` Shell 骨架 + `3/4` 入口 + 6 面切换 + 顶面 `BC-1` 真实播放与遥测接入。
- **Phase 2**：正面虚拟化 `CD Magazine` + 前景 `Album Dossier` 抽碟展开 + 17 路由打通。
- **Phase 3**：右面 `AMP-35` 真实输出策略互锁 + 60fps `24×48` 网罩背光频谱 + 左面歌词/队列/搜词 + 背面 I/O。
- **Phase 4**：Desktop / `iPhone Duo` / 窄屏手机适配 + 自动化测试与实机验视。

**每次改完代码必跑验证**：
```bash
(cd ui && nsh check)
cargo test --manifest-path ui/Cargo.toml --lib
cargo test --manifest-path ui/Cargo.toml --test engine_shutdown
(cd ui && nsh test)
```
