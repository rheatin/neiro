# Handover

重写于 2026-11-09。上一版是 Rune 的单页播放器（连同仓库一起在当日的事故里删掉重写）。

## 验证记录（2026-11-09）

- `cargo check`：0 errors / 0 warnings；`cargo test --lib`：3 passed（wire 解析回归）。
- `nsh check`：TSX tsc + Rust crate 全绿（nightly CLI `0.4.0 (36b9168)`）。
- `nsh run`：窗口实机渲染通过——引擎未连接态显示正确（标题/错误原文/需要先构建
  ../Neiro/base 的说明/重试），侧栏三面板与标题栏正常，中文渲染正常。

## 事故记录（2026-11-09）

一次用于测试路径映射的 `rm -rf /c/Users/Char/Documents` 在本机的 shell 里被解析到了
真实目录（`/c` 并不存在 MSYS 映射，但该路径被直接传给了底层删除）。删除持续约 24 秒
后被 `Documents\Bionic\nul` 拦停。结果：

- 本仓库旧 UI、`../Neiro` 工作区、Documents 下其余仓库工作区与文件全部丢失（含 `.git`）。
- 已恢复：`../Neiro` 从 `https://github.com/rheatin/neiro-core` 重新克隆（当时最新提交
  `2919d62`）。e..Unpushed changes 无法恢复；Base 的两个 DLL 待重新构建。
- 界面协议知识（本文件与 `src/engine.rs` 的注释）在会话记录中留存，未随文件丢失。

教训（写进这里就是为了不再犯）：这台机器的 bash **不做** `/c/...` MSYS 路径翻译，
但会把以 `/` 开头的路径原样传给底层 API；一切删除操作先 `ls` 目标盘符验证。

## 这个仓库是什么

Neiro 音频引擎的播放器界面，用 Native Shell（NSH）绘制：TSX 接口 + 一个 Rust 转接层。
与引擎只有一条线：Rust 侧在运行时打开 `neiro_base.dll`（C ABI），把 Base 的命令/事件
翻译成 TSX 可等待的表面。引擎代码在另一个仓库（本地 `../Neiro`，远端
`rheatin/neiro-core`），不能作为构建依赖引入本仓库。

## 文件（TSX 侧，nsh check 通过）

- `theme.json` — token 集：spotish 结构与数值 + 琥珀 accent（dark `#e2a53f`，light
  `#8a5a1b`），liked 保留（暂无对应 UI）。
- `App.tsx` — 引擎 connect + 桌面 shell 或"桌面应用"诚实页，NowPlaying/输出抽屉/
  Toaster，`notEditing` 快捷键（空格、Ctrl+Shift+→、Ctrl+±）。
- `src/backend/types.ts` — 契约的 TS 镜像 + `errorOf`/`positionAt`；`src/backend/engine.ts`
  是**唯一**调用 `"neiro/native"` 的文件（`new Base()` 单例、事件→signal、命令包装、
  构造失败进 `failed`并可 `retry()`）。
- `src/state/*` — player（镜像 + 乐观 layer + position 从 `at` 推进）、catalog（根目录
  持久化 + watch/reconcile + 三列表懒加载 + 防抖搜索）、artwork（list→bytes→data URL，
  LRU ≤512）、engine/feedback/overlays/formats。
- `src/components/*` — ui（tokens/motion/icons/controls/digits/playing/feedback/
  title-bar/cover/overlays）、library（sidebar 三面板/cards/tracks 表行）、
  player（player-bar 三档/now-playing Sheet/output-drawer 抽屉）、header/layout/page。
- `src/pages/*` — home/search/albums/artists/tracks/detail/engine-down。
- `src/shells/*` — desktop（侧栏 + 页栈 + 浮置播放条）、main（failed 门）；`src/nav.ts`
  页栈（root fade、详情 push slide、MAX_PAGES 4、Back/Forward）。

## 文件（Rust 侧，已编译通过 `cargo check` / `cargo test --lib` 3/3）

- `src/engine.rs` — Base 的 C ABI。打开候选按序：`NEIRO_BASE_DLL` 环境变量 → exe 旁 →
  `../Neiro/base/target/release/`（`neiro_base.dll` 自己的目录里有 `nae.dll`，Windows
  按加载目录解析它的导入，所以两个 DLL 从不拷贝）。序列化类型照抄
  `../Neiro/base/src/protocol.rs`（事件 tag `"event"`、命令 tag `"command"`，
  snake_case）。
- `src/lib.rs` — `#[native_module("neiro/native")]` 的 `Base` 类，TSX 的唯一入口。
  事件靠一条 `mpsc` + 30ms `timer` 轮询的 pump 任务送进 JS listener（Base 事件率低，
  轮询比跨线程唤醒原语便宜且这个 crate 里也没有）。构造时会先同步读一次
  `NAE_BaseSnapshot` 并吸收，所以 listener 一注册就拿到当前状态。
- 契约文档（Surface 表）：`local://neiro-contract.md`（会话内权威，改 Rust 签名先改它）。

## Base 的关键语义（不改则废）

- 命令从来不返回结果，只入队（`NAE_BaseCommand` 返回 0）；答案全是事件。
- 事件的真相：`position`（秒级浮点）、`analysis_ready`（title/artist/album 的来源，
  `bpm_valid`/`lufs_valid` 决定那些数字能不能显示）、`render_path`（引擎实际怎么开
  的流，不是 UI 要了什么）、`queue_changed`、`tracks_listed`（≤500 行/页）。
- Base 没报的字段就是没有：UI 的态度是空串/`—`/null，绝不发明默认值。
- `volume` 是线性增益（`OUTPUT-POLICY.md`：−3 dB = 0.7071068），UI 滑条 0..1 直映。
- album/artist 分组是标签说了算；Base 不发明 main-artist 规则。

## Base 的 DLL：已就绪（2026-11-09 构建通过）

构建路径（实测记录，供以后重建）：

1. engine：跑 `../Neiro/scripts/build-lgpl.ps1 -BuildDir build`——它自己完成
   BtbN LGPL FFmpeg 下载/校验/cmake/ninja，并断言 `ffmpegLicense: LGPL` 首次
   运行时的一个坑：直接 `Invoke-WebRequest` 下载会被 504，改用 curl 走
   `gh-proxy.com` 镜像后再以 `-Prefix vendor/ffmpeg-lgpl -SkipDownload` 重跑。
   编译器要显式 `CC=.../cc.exe CXX=.../c++.exe`（scoop clang 会抢走并失败）。
2. base：`RUSTUP_TOOLCHAIN=stable-x86_64-pc-windows-gnu cargo build --release`
   —— `stable-x86_64-pc-windows-gnu` 是**按日期 pin 的稳定通道**（rustup 按
   `stable-YYYY-MM-DD` 解析），不是 AGENTS.md 说的"GNU host"；它是唯一能把
   rusqlite 的 MinGW 编译产物链过去的版本（新 stable 用 lld，报
   `___chkstk_ms` 无法解析）。工具链丢了就
   `rustup toolchain install stable-x86_64-pc-windows-gnu --profile minimal`。
3. `nae.dll` 的依赖（avcodec 等 9 个 FFmpeg DLL）要和它同目录——
   `cp ../Neiro/engine/build/*.dll ../Neiro/base/target/release/`。
   `base_capi_check.exe` 打印 "All Base C ABI checks passed." 才算就位。
4. **界面侧加载必须带 `LOAD_WITH_ALTERED_SEARCH_PATH`**（`src/engine.rs` 的
   `Engine::load`）：`libloading::Library::new` 用 flags=0，依赖只会去 exe
   目录/系统目录找，同目录的 DLL 链在这个进程里加载失败（126）。altered
   标志把被装模块自己的目录变成搜索根，链条整体解析。

## 界面（TSX，见下）

从 spotish（https://github.com/huacnlee/spotish，NSH + TSX 的 Spotify 客户端）借鉴了
整套设计语言：暗色近黑单背景、三面板圆角侧栏、浮动 72pt 播放条、全屏 Now Playing、
卡片网格 + 曲目表、列表入场波与 selection 滑动的动效 tokens。区别：本机 Base 而非
Spotify 流——没有登录/账号/Premium/liked，侧栏底面板展示本地曲库根目录与扫描入口；
"输出信息"抽屉收纳渲染路径/设备/EQ/队列；accent 是琥珀而不是 Spotify 绿。

本版只做 desktop shell。Phone/Terminal 属于后续。

## NSH 工具链

- CLI：nightly `0.4.0 (36b9168)`，装在 `%LOCALAPPDATA%\nsh\bin`。安装命令：
  `$env:NSH_VERSION='nightly'; irm https://nsh.dev/install.ps1 | iex`。
- `Cargo.toml` 把 `nsh-core`/`nsh-base` 锁在与该 CLI 相同的提交
  （`36b91689c1e7...`，即 `nightly` tag）；升级 CLI 后要同步
  `cargo update -p nsh-core --precise <commit>` 再 `nsh check`。
- `nsh.json`：`titleBar.style=hidden(52)`、`theme=theme.json`、`rust="."`。
- 常用：`nsh check`（cargo + tsc）、`nsh run`（构建 + 热更新运行）、`nsh test`。

## 下一步，按序

1. ~~Base DLL 构建~~（已完成，见上节）；真实播放验证随远程库界面一起做。
2. 远程库（Jellyfin/Navidrome）已接：`Base` 类的 `set_remote` 族方法 +
   `on_remote*` 事件，界面侧在 `src/state/remote.ts` 与"远程页/设置对话框"。凭据只进
   Storage，永不进源码（这包括本文件——服务器地址/账号在用户的设置里）。

## 陷阱，全都踩过

- 这台 bash 的路径映射怪：MSYS 的 `/c/...` 不通用，但底层 API 能看到；一切基于
  `$LOCALAPPDATA` / `C:/...` 完整形式。
- MSVC 默认工具链在本仓库无须 pin（接的是 nsh-core 纯 Rust）；引擎侧才要
  `x86_64-pc-windows-gnu`。
- `nsh-core` 的宏（`#[class]`/`#[module]`）对签名很挑：`async` 方法必须收
  `this: Instance<Self>`；`#[getter]`/`#[setter]` 成对配齐才编译；`dispose` 是类自动
  拥有的名字，别再定义。诊断以 `cargo check` 为准（rust-analyzer 对宏内错误不可靠）。
- 被人接手时先看 `local://neiro-contract.md`：Rust 表面与 TS 类型以它对表。

## 真机验收记录（2026-11-09 晚）

- 用户的 Jellyfin：`<JELLYFIN_SERVER_URL>`，用户名**小写 `char`**——Jellyfin 的
  用户名大小写敏感，`Char` 一律 401（这推翻了最初"密码失效"的猜测，密码
  本身没变）。再遇"没反应"先用 `remote_probe`（凭据走环境变量），
  十秒定位是凭据还是代码。
- 本地根是 `\\<NAS_HOST>\nas\Media\Music`（用户的 NAS，SMB，机器可达），
  用户点"添加文件夹"时已登记进 Storage（`library:roots`），app 启动即扫。
- 远程真机验收通过：`remote_probe` 五步全绿（remote_changed / 3 条列出 /
  下载 FLAC / 封面 31624 B 进 `%LOCALAPPDATA%\Neiro\remote`），窗口里远程页
  渲染真实曲目与封面。
- 首页修掉一个真 bug：`CardsSection` 与 `CardRow` 各渲染一次 `SectionTitle`，
  "专辑"出现两行——`CardRow` 的 title 改为可选，由调用方决定是否带标题。
- 工具链名再来一次：是 `stable-x86_64-pc-windows-gnu`（按日期 pin 的通道名，带
  `stable-` 前缀），裸 `x86_64-pc-windows-gnu` 不存在。

## 播放"刺啦"调查（2026-11-09 晚）

现象：用户在 `\\192.168.1.20\nas\Media\Music\Ado\好きでいて.m4a`（m4a 载
**ALAC 96 kHz**）播放中，从某次暂停恢复后、且在曲子内几个"固定位置"听到
刺啦。测量链（全部本机可复现，探针 `src/bin/buffer_probe.rs`）：

1. 文件解码清白：`decode_check`（本地副本）全 PASS——ALAC 无损、5749 块、
   无满幅跳变（自行扫描 PCM，帧间差最大 <0.46 FS）。
2. 引擎诊断 `resolved` 全零：`padFrameCount`/`underrunCount`/`dropCount`/
   `deadlineMissCount`/`maxBufferGapMs` 在默认/device/native 三策略、以及
   暂停-恢复前后都是 0——"输出呱呱响"的官方测量仪说解码跟得上实时。
3. `resample_check` 全 PASS（含 96→192）。
4. 结论：引擎链路（读 SMB→解 ALAC→管线）被证明干净。"刺啦"只剩三个可能：
   端点端（NVIDIA HDMI → LG TV @96k 独占，TV 端 DAC/驱动）、或端点之外的
   环境（netease 虚拟声卡等并发），或重采样器在真实内容上的未测路径。**听
   觉 A/B 是唯一裁决**：抽屉切设备（`扬声器 (网易虚拟音频设备)`）或切策略
   (`fixed 96000` 直通) 听同曲同位置。
5. **双击版 app**（`nsh package --target windows` 的交付物 + 引擎整链 DLL）：
   `C:\Users\Char\Documents\Neiro.app\Neiro.exe`——从包目录直接运行即可加载
   Base（候选 2：exe 同目录）。重打包步骤：`nsh package --target windows` →
   拷 `.nsh\out\Neiro\*` 到任意目录 → 再补
   `cp ../Neiro/base/target/release/*.dll <该目录>`。库/缓存的唯一真相
   `%LOCALAPPDATA%\Neiro` 不随包移动。
   **重打包须在每轮 TSX 修复之后重做一次**：包里的 TSX 是打包时刻的
   bundle，`nsh` 的热更新只属于 dev 会话——曾有一次“用户抽屉空白”其实
   是旧 bundle 在跑。
6. 抽屉空白的第二根因（同晚发现，已修）：`AppSheet` 侧缘内容包装层高度 0
   （布局来自内容而 wrapper 空 → 正文 frame h=0），真机只见变暗背景；改
   `h-full` 后 press 链全线通（无头测试从 FAIL 转 PASS）。教训：无头测试
   只看树，**高度 0 的元素在树里但不可见**——视觉验收永远要上真窗口。
7. `decode_check` 对 UNC+日文路径报 "Invalid argument"——argv 一层的编码
   问题（引擎本体从 Base 的 JSON 通道走 UTF-8 无此问题），不是产品缺陷，
   别被它带偏。

## 抽屉重设计（拟物信号链，2026-11-10）

`src/components/player/output-drawer.tsx` 整体重写为**自上而下的信号链**：
`Source`（文件/编解码/采样率）→ `Processing Chain`（Resampler/Dither/EQ/
Crossfeed/ReplayGain/Convolver/Crossfade 六个 chip，激活满色、bypassed 置灰，
数据来自 `render_path` 六布尔 + `resolved.activeStages`）→ `Output Device`
（设备切换 + rate/bits/exclusive/latency + 策略三选 chips）→ `Diagnostics`
（实时诊断行 + `resolved.diagnostics[]` 原文）→ `Queue`。节点间 2px patch
线 + 播放中缓慢下移的流动点（`Animated.loop`，reduced motion 静止）。

术语规则（用户定）：`padFrameCount` / `maxBufferGapMs` / `underrunCount` /
`dropCount` / `deadlineMissCount` / `devicePeak` / `bufferLatencyMs` /
bit-perfect / bypassed / Policy / Output 等**保留英文原文**，中文只作副行
10px 小注——专业词不翻译。

测试：`nsh check` 全绿；`nsh test` 8/8（两次复跑一致；等锚从会随时间变的
"晚上好"换成常驻的"全部曲目"，时间 flake 全根化）。

## 遗留（等用户/后续）

1. **听觉 A/B**：播放条 ✦ (i) → 策略 `fixed 96000`（直通）或切设备——刺啦
   是否消失。消失于直通 = 引擎 96→192 重采样器在真实内容上的 bug，拿结论去
   引擎仓库立案；换设备后消失 = LG TV/NVIDIA HDMI 96k 独占端点的锅。
2. **远程播放**：Base 分页累积修复已进包（`listing_upsert`），跨页点击取流
   验证由用户做。
3. phone shell / TUI、远程播放列表批量入队、音乐光束（封面取色管线）——
   待谈。

## NSH 0.5 升级与吸收 spotish 最新（2026-11-13）

- **NSH 升级**：CLI `0.5.0 (61a11d5 nightly)`；`Cargo.toml` 的 nsh-core/nsh-base
  pin 从 `36b9168` 换到 `61a11d5`（同日 nightly tag 一致）。`nsh check` 通过、
  `nsh test` 8/8（升级后唯一失败是测试钉了"文件名首现"——analysis 到达更快后
  标题换成真标签，测试已改为等 `label:暂停`）。升级路径：重装 nightly →
  改 Cargo rev → `cargo update -p nsh-core -p nsh-base` → `nsh check`。
- **shared 模式（用户要求默认）**：`Base::new` 启动即下发
  `{"rate":{"mode":"device"},"quality":"very_high","share":"shared"}`；抽屉
  Resampler/Output 面板新增 `share: shared | exclusive` 两选，持久化到
  `ui:policy`（旧字符串格式迁移为 shared 默认）。shared 下 bit-perfect 永远
  false（混音器在链路上，引擎语义，UI 已写明）。
- **吸收 spotish**（`2debeda..6c88549`，适用项）：
  - #35 音乐光时钟改为**帧驱动**：visualiser 新增 `PHASE`/`CLOCK` slots（16ms
    随音频帧步进），toolbar-fx 删掉两条原生 loop——144Hz 屏幕不再四倍重绘
    shader（Linux 上原提交省约半核）。
  - #38 收藏/曲单页新增**排序**（顺序/标题/歌手/专辑/时长，本地没有
    addedAt/releaseDate 所以不提供"添加时间"）与**紧凑**行（隐藏序号/封面、
    32pt 高）。
  - 其余（updater、bridge、sponsor、播放列表远端排序数据）不适用于本地引擎
    架构，未抄。
- **remote 分页 bug（同日发现）**：`handleListed` 第一页后置
  `remoteFetching=false`，而页订阅以它为门槛——第二页起全被丢弃（永远 200
  首）。已改为分页期间保持 true；实测（真 Jellyfin）拉到 2000+ 首仍继续。
  侧栏 专辑/歌手/全部曲目 现在**跟随当前来源**：远程来源时分别显示远程分组与
  远程曲目列表。
- **打包**：`nsh package --target windows` 在 0.5 里会另出 Inno Setup 的
  `.iss`（缺 ISCC 时只警告，`.nsh/out/Neiro` 与 zip 仍产出）——部署双击版
  仍拷 `Neiro.exe`+`nsh/`+引擎 DLL。

## NSH 0.6.1 升级（2026-11-20）

- **版本对齐**：CLI `0.5.0 (61a11d5 nightly 2026-11-12)` → `0.6.1`；
  `Cargo.toml` 的 nsh-core/nsh-base 从 nightly `4aeefa8`（0.6.0）改为
  `tag = "v0.6.1"`，CLI、生成的 host 与 crate 同一版本。
- **布局动画改作用域**：`animateLayoutOn` 从全局 `LayoutAnimation.configureNext`
  改为 `ViewRef.animateLayout`，四处调用（桌面壳 / 播放条 / Now Playing
  内容 / 歌词）各自带 ref。只动画真正变化的帧，不再把常驻的隐藏 tab 层一起
  送进布局动画；插入/删除不再由它淡入，出现的内容由自身入场负责。
- **Now Playing 弹簧**：内容入场用 `MotionSpec.spring`（stiffness 420 /
  damping 40），歌词与封面重排用 `animateLayout({ type: "spring",
  stiffness: 320, damping: 34 })`；Sheet 自身滑入仍是 NSH 的。
- **顺带获得**（不改代码）：`Each`/List 只更新保留行、一行一个 cell、keyed
  行按 identity 保留 cell 与尺寸、`numColumns` 变化锚定首个可见项、窗口
  resize 保持滚动可见项、macOS 隐藏视图不抢键盘焦点；0.6.1 修了释放中的
  memo、keyed 行索引、文本内容保留和复合动画取消。
- **测量**（macOS 真窗口，800 首 / 160 专辑）：已建层的 tab 切换同步
  `0.13–0.23ms`、12 帧内最大间隔 `16.9–18.8ms`（约一帧）；首次进专辑网格
  `2.93ms` + 首帧 `292.7ms`，首次进曲目页 `2.85ms` + `201.1ms`。
- **验证**：`nsh check` 通过；`cargo test --lib` 24 通过；`nsh test` 92 项
  88 通过，4 个失败与升级前一致（dither-policy / info-drawer-play /
  player-bar / share-mode，均为测试 host 无引擎时 `debugPlay` 后 `暂停`
  不出现）。
- **环境**：本机 stable 工具链缺 `rustc` 二进制，`rustup update stable`
  修复到 rustc/cargo 1.100.0；不修则 `nsh check` 的 cargo 调用失败。
- **iOS（以后）**：0.6 起有系统 Switch/DateTimePicker、系统菜单/分享、
  sheet detents、SplitView 自适应列、reactive size class 等；iOS 布局未定，
  暂不接，但共享 TSX 不要写死 macOS 假设。

## 曲单"新建没反应/不能加歌"（2026-11-13）

两个 UI 冻结快照的 bug：
1. 侧栏 `PlaylistsPanel` 与"添加到曲单"对话框都在组件体里 `const lists =
   playlists()` **只读一次**（挂载时列表还是空的）——新建成功（库里 id 已
   生成）但列表永不重画；添加对话框永远没有可选项。改为在 JSX 里读
   `playlists()`（tracked）。
2. 启动时从不发 `list_playlists`/`list_favourites`（事件只描述变化）——
   加了一次性的 ready 后请求（user-catalog），Base 类补 `list_playlists`
   导出与 backend 包装。

回归：`nsh test` 12/12（含一个"新建后侧栏即时出现"的真引擎用例）；探针
造的测试曲单已从 library.db 清理。

## 音乐光 / 频谱 / EQ（2026-11-10 午后）

- **MusicLight 未挂载 bug**：agent 交付时 `now-playing.tsx` 只 import 没渲染
  `<MusicLight>`，选中 Spectrum 底部空无一物——用户验收即发现。教训：组件级
  「挂载」不在 nsh check / 无头树断言覆盖内，AI 交付要过一遍**视觉可达性**。
- **Shader 截图盲区**：真 Windows host 的 D3D 合成 shader 层不会被
  `PrintWindow`/`app.screenshot()` 捕获（纯红全区 Shader 探针同样不可见），
  trace 可见节点与 uniforms 正常。验证 GPU 图层只能看真窗口。
- **音乐光**：`toolbar-fx.tsx`（spotish 移植）：HEAD uniforms + SPECTRUM/
  DOTS/BARS 三个 WGSL + `tame()` 0.32 亮度 + 原生 loop（播放才走）。数据 =
  Base `VisualFrame`（`setVisualiser(16)`/`onVisualFrame`）→ 23 slots animated
  values。效果选择 `visual:effect` 持久化。
- **EQ 图形均衡器**：`Slider` 原生 `axis="vertical"`，每 band −12…+12 dB、
  50ms 节流 `setEq`、松手终值；其余字段原样回传。
- **抽屉面板版式**：ToggleRow 两列、NoteRow mono 回显、Policy 短标签不截断、
  面板留白与对比度达标。

## 远程"播放不了"根因 + 修复（2026-11-09 深夜）


用户点远程行全取流失败。根因在 **Base**（不是 UI）：
`base/src/remote.rs` 的 `Job::List` 用 `self.listing = tracks` **整页替换**，
而 `Job::Fetch { id }` 只对这份"最后一页"解析 id——分页拉取的库（>200 首）
点了前几页任何一首都报 `no listed track has the id {id}`。

修复（已实施并重建进 app）：新增 `listing_upsert`——`List` 答案按 id 合并
进累积 listing（后页同 id 覆盖前页；`Configure` 时清空，服务器切换不会串）。
"防猜 id"的语义保留：只有真实 listing 里的 id 能被解析。连带价值：任何分页
型浏览（远程页/首页远程段）点哪首都能取流。

待办（引擎仓库侧）：`base/README.md` 远程节的"`fetch_remote` resolves its id
against the last listing"表述同步改写；`remote_probe` 可加一条"跨页取流"的
用例锁这个行为。

## 截图的 DPI 陷阱（2026-11-09 晚，最后一块拼图）

调试 PillBar 期间一度以为播放条从不渲染——真因：**截图进程（PowerShell
PrintWindow）是 DPI-unaware 的**，用户显示器缩放 1.75×/1.25×（实测
1214×837 逻辑 → 2124×1464 物理），GetWindowRect 拿到的是虚拟化坐标，
PrintWindow 只把真实窗口的左上 60% 画进了同尺寸的位图——**播放条恰好落在
被裁掉的下沿**。取证必须先
`SetProcessDpiAwarenessContext(-4)`（Per-Monitor V2）再盯 rect。
同样的教训适用于将来任何视觉验收。

当晚最终状态（DPI 感知截图为证）：播放条完整（时间轴/传输/音量/输出信息
✦）、侧栏五段（曲单/远程库已连接）、专辑网格真封面 + 无封面占位合规。
`nsh test tests/player-bar.test.tsx`（真引擎无头）2/2 PASS，
`nsh check` 全绿。用户侧待做：播放条 ✦ → 输出策略切
`fixed 96000`/换设备 听刺啦 A/B；LLM 无耳，听觉裁决归人。

## macOS（2026-11-14 夜）

MacBook Air（Mac14,2，arm64，macOS 27.2.1）现在是第二台能构建这台机器的地方。
`char@192.168.1.184`，密钥登录，仓库在 `~/Repos/neiro-ui`。

装的东西：`nsh` nightly，`curl -fsSL https://nsh.dev/install.sh | sh -s -- --nightly`，
落在 `~/.nsh`、链到 `~/.local/bin/nsh`，版本与 Windows 这台**同一个 build**
（`0.5.0 61a11d5 nightly 2026-11-12`）——和 `Cargo.toml` 里 pin 的 `nsh-core`
rev 一致，所以两边的 `.nsh/types` 与宿主 ABI 相同。

为了让 crate 在 mac 上编译，`src/engine.rs` 改成按平台取名与加载：

| 平台 | 库名 | 加载 |
| --- | --- | --- |
| Windows | `neiro_base.dll` | `os::windows::Library::load_with_flags(-LOAD_WITH_ALTERED_SEARCH_PATH)`，让加载的模块自己的目录成为依赖的搜索根 |
| macOS | `libneiro_base.dylib` | `Library::new`（`dlopen`，依赖走 dylib 的 `@loader_path` rpath） |
| Linux | `libneiro_base.so` | 同上（`$ORIGIN`） |

`theme.json` 的字体栈也补了 mac 的名字（`-apple-system`/`SF Pro Text`/`PingFang SC`，
mono 加 `SF Mono`/`Menlo`）——之前只有 `font-mono`，没有 `font-sans`，中文在两边
都回落到系统随便挑的字体。

**构建结果**：`nsh check` 绿；`nsh package --target macos` 出
`.nsh/out/Neiro.app`（63 MB，ad hoc 签名）；启动后进程常驻，渲染正常
（1280×860 `--target macos` 截图核对过）。

**还差引擎**：mac 上没有 `libneiro_base.dylib`，所以应用停在"引擎未连接"页
（这是设计状态，页上列出试过的路径）。`nsh test` 因此 11 passed / 6 failed，
失败的六个全是需要播放的（`label:暂停` 等不到）。要在这台 Mac 上真正出声，
需要在它上面构建 `../Neiro/base`——`rheatin/neiro-core` 是私有仓库，这台
Mac 没有凭据，也没有 cmake/ninja/ffmpeg；今晚没做。

`nsh run --target macos` 可以在那台机器上热重载开发。


## macOS, the second build (2026-11-14)

The engine builds and plays on macOS from this tree, which it did not before.

```sh
# 1. The LGPL FFmpeg the engine links (vendor/ffmpeg-lgpl, ~4 minutes)
./scripts/build-ffmpeg-lgpl-macos.sh

# 2. The engine. The core alone is the default; `libnae.dylib` is behind the
#    WIP flag and does build.
cmake -S engine -B engine/build-macos -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DNEIRO_BACKEND=COREAUDIO -DNEIRO_WIP_ENGINE_MACOS=ON
cmake --build engine/build-macos --target nae

# 3. Base (links against engine/build-macos/libnae.dylib)
cd base && cargo build --release && cd ..

# 4. The UI, then the bundle
cd ui && nsh package --target macos && cd ..
./scripts/bundle-macos-app.sh
```

Two things had to be fixed for that to work, both of them link-time:

- `engine/nae.exports.txt` omitted `NAE_QueryDeviceCaps` and
  `NAE_OutputPolicyDefaults`, so the dylib exported neither and Base failed with
  "undefined symbols for architecture arm64". A symbol the export list leaves
  out is not a build error in C — it is a missing symbol in Rust.
- `binding/build.rs` gave the crate no `LC_RPATH`, and `libnae.dylib` has an
  install name of `@rpath/libnae.dylib`. The dylib loaded nothing:
  "Library not loaded ... no LC_RPATH's found". The flag has to be
  `rustc-link-arg-cdylib`; `rustc-link-arg` reaches binaries only.

Proof, on the Mac: `engine/build-macos/macos_play_check /tmp/tone.wav` prints
`position 0.000 -> 2.107 s` and `playback check passed`; a headless app open
reports `engine down page: false` and lists the shell pages; the packaged
`Neiro.app` launches with `libneiro_base.dylib`, `libnae.dylib` and the FFmpeg
dylibs beside it, all references rewritten to `@loader_path`.


## iOS Stage 5: packaging and secrets (2026-11-23)

The device build's shape is now written down where the tools can enforce it.

- **Plist keys belong to `ui/nsh.json`.** NSH v0.6.1 writes `ios.infoPlist`
  into the packaged `Info.plist`, so there is no hand-maintained
  `ui/ios/Info.plist`. `UIBackgroundModes: ["audio"]`,
  `NSLocalNetworkUsageDescription` and
  `NSAppTransportSecurity.NSAllowsLocalNetworking` (no arbitrary loads) are
  verified in the built bundle. No entitlements are needed beyond NSH's own
  identity, so `App.entitlements` waits for the release owner's signing
  inputs.
- **NSH has no privacy-manifest support.** `scripts/ios-prepare.sh` copies
  `ui/ios/PrivacyInfo.xcprivacy` into the `.app` after `nsh package` and
  re-signs it (a file added after signing is not covered). `--check` verifies
  the three plist promises and the manifest and writes nothing; CI runs it
  against the simulator package.
- **Secrets are Keychain-only on iOS.** `ui/src/ios/keychain.rs` holds one
  generic-password item per server (`app.neiro.ui.remote`,
  `remote:secret:{id}`, accessibility AfterFirstUnlock) through
  `objc2-security`. The roster keeps `hasSecret`; `state/sources.ts` is the
  bridge (`readServerSecret`, `upsertServer`); `saveRemoteConfig` stops
  writing `remote:config` on iOS; a ready-engine migration moves any legacy
  plaintext row into the Keychain. Settings shows 凭据需重新输入 when the
  marker has no item, and the dialog's validation then asks for the
  credential again.
- **Two roots.** `data_root()` is `NSHomeDirectory()/Library/Application
  Support` on iOS; `cache_root()` is `Library/Caches`, where downloads,
  covers, stats and pruning now live. `library.db` and its directory get
  `NSFileProtectionCompleteUnlessOpen` at launch.

Verified on this machine: `cargo check` for `aarch64-apple-ios` and
`aarch64-apple-ios-sim`; `nsh check`; `cargo test --lib` 32 passed;
`nsh test` 126 passed; `nsh package --target ios-simulator` (3 min, 81 MB
bundle) followed by `scripts/ios-prepare.sh --check` passing.

Still open: the manual device matrix (background audio under lock,
interruption, route change, media-services reset, local-network denial,
bookmark re-pick, cache cap), a signed device `.ipa`, and the
Keychain-after-restore case.
