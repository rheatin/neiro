# Neiro HiFi 能力指南

[English](HIFI-GUIDE.md) · **中文**

术语保留英文原词：`dither`、`gapless`、`resampling` 这些词在音频工程里就是它们的名字，译成中文反而会遮住它指的是什么。每个术语第一次出现时在括号里给中文解释，之后直接用英文。

这份文档逐项回答三个问题：**它改了什么**、**怎么证明它在工作**、**听众能听出什么**。

它不是实现说明。解析规则、阶段顺序、ABI 契约在 `engine/OUTPUT-POLICY.md`，这里只引用，不重述。

---

## 0. 读之前必须知道的一件事

**`bitPerfect` 这个名字，比它实际证明的事情宽。**

`engine/output_mode.h` 写得很直接：

> `bitPerfect` is a bad name for what this computes and the reason code carries
> the honest label... It is true here and it means one thing only: no rate
> conversion was needed. It says nothing about depth, channel count or sample
> format, and the decoder's float32 intermediate means the samples are re-packed
> even when this is true.

所以本文出现 `bit-perfect`（比特完美）时，指的是 `OUTPUT-POLICY.md §6` 定义的那一整套条件同时成立，**而不是**"文件里的字节原封不动到达 DAC"。decoder（解码器）内部是 float32，样本必然经过一次重打包。

还有一层，对有损源（MP3/AAC）：**`bitPerfect = true` 说的是解码出来的样本没有再被改动**，不是说源头无损。一个有损文件也可以走一条零处理的路径，这两件事不冲突。我们实测的 21 个文件里，MP3 在 native 策略下也是 `true`，那是正确的。

任何声称自己是 "bit-perfect player" 的地方，都值得问一句：**它验的是哪一条**。

---

## 1. exclusive mode（独占模式）与 shared mode（共享模式）

**改了什么.** exclusive mode 直接打开 WASAPI endpoint，绕过 Windows mixer（混音器）；shared mode 让 mixer 留在路径里。

**怎么区分.** `render_path.share`，以及 reason code `7 = SHARED_MODE`。

shared mode 单独一个 reason code，不是因为它更糟，是因为**它是用户的选择**。`nae.h` 写明：其他原因都指向"出了问题"或"哪个开关能改"，只有这一条是听众为了能同时听别的程序而主动选的路线。所以 `bitPerfect = 0`，而"Windows 是否其实保留了样本"这件事**至今未测**——两个方向都不下结论。

**听众体会.** exclusive mode 期间其他程序没有声音，切歌时可能听到设备 re-lock（继电器声、短静默）。这就是它的代价。

---

## 2. sample rate（采样率）策略：Native / Device / Fixed

**改了什么.** 决定用谁的 sample rate。

| 策略 | 谁说了算 | 是否转换 |
|---|---|---|
| Native | 文件的 sample rate，设备被改成它 | 否 |
| Device | endpoint 的 sample rate，文件被转换过去 | 是（当两者不同） |
| Fixed | 用户指定的固定值 | 是（当文件不同） |

**怎么区分.** `policy` 是**请求**，`render_path` 是**设备实际答应的**，两个字段分开报告。这是刻意的：`fixed` 可能被设备拒绝，引擎会报告它实际授予的 sample rate，以及一个 coercion reason（强制原因）。

Native 模式下 `bitPerfect` 与 `resampling` 都是常量——这是对的。早期版本拿源 sample rate 去比 endpoint 的 native sample rate，于是 Native 模式同时声称"在 resampling"和"bit-perfect"，两句不可能同时为真。

**听众体会.** Native 下换不同 sample rate 的歌会听到设备 re-lock；Device 下不会 re-lock，但一定经过了 resampling。

---

## 3. resampling（重采样，soxr，三档质量）

**改了什么.** 只在必要时发生，用 soxr 做有理比转换。

**怎么区分.** `resampling = true`，并且 reason code 告诉你是**哪一种**：

- `1 = RESAMPLED_TO_DEVICE_RATE`：为了迁就设备
- `2 = RESAMPLED_TO_FIXED_RATE`：为了迁就你指定的固定值
- `6 = RESAMPLED_TO_SUPPORTED_RATE`：设备不接受这个 sample rate，退到它能接受的

三条分开，因为**修法不同**。1 和 2 是去改 output mode，6 是设备能力问题——照着 1 的修法去处理 6 是白费功夫。

**听众体会.** 44.1→192 的插值在 transient（瞬态）密集的段落（鼓刷、钢琴击弦）能听出"软"一点；质量档位的差别主要在极高频、极低频和 transient 上。

---

## 4. dither（抖动）

**改了什么.** 只有在输出 bit depth 窄于内部精度时才加。内部是 float32，输出 16/24 位整数时需要；输出 32 位整数或 float 时不需要。

**怎么区分.** `dithered = true`，reason code `3 = DITHERED`。

**听众体会.** 16 位输出、极安静的段落（录音底噪、渐弱尾音）里，不加 dither 会有颗粒感和 quantisation（量化）台阶；dither 把 quantisation distortion（量化失真）换成了低电平 noise（噪声），听感上更"干净"，而不是更"安静"。

---

## 5. bit depth（位深）、container bits 与 valid bits

**改了什么.** 区分"容器装了多少位"和"里面真有多少位有效数据"。

**怎么区分.** `bits_per_sample`（container）与 `valid_bits`（有效）。

**听众体会.** 这一项听众听不出来，但它是很多"为什么不够干净"的根因。**24-in-32** 在 HDMI 和部分 USB 路径上很常见：设备声称 32 位，实际有效 24 位。把它当成"32 位"去比较，结论会错。

我们实测的默认 endpoint 报的是 `192000 Hz / 32 bit (24 valid)`。

---

## 6. channel conversion（声道转换）与 routing（路由）

**改了什么.** 源声道数 ≠ 设备声道数时的下混/上混，包含 LtRt 矩阵（杜比逻辑的编码方向）。

**怎么区分.** reason code `4 = CHANNEL_CONVERTED`；`active_stages` 里会出现 routing 相关项。

**听众体会.** stereo 在 mono 设备上会损失 stereo image（声场）；5.1 下混到 stereo 会损失环绕信息；LtRt 是有意的编码，不是降级。

---

## 7. DSP 链：EQ / crossfeed / convolver / ReplayGain / crossfade / volume

**改了什么.** 每一个都改样本，因此每一个都有自己的 reason code。

| 能力 | 改了什么 | reason code | 听众体会 |
|---|---|---|---|
| EQ | frequency response（频响） | `9` | 音色变化，最直接 |
| crossfeed | 把一路延迟低通后混进另一路 | `10` | 耳机头中效应减轻，stereo image 靠前 |
| convolver | 用 FIR（finite impulse response，有限冲激响应）替换信号 | `12` | 整条响应被替换，结构性最强 |
| ReplayGain | 一个 gain（增益） | `11` | 曲目间响度拉平 |
| crossfade | 上一首结尾与下一首开头混合 | `13` | 切歌处没有硬切 |
| volume | digital volume（数字音量） | `8` | 电平——但注意它发生在**数字域** |

**一条容易误解的规则**：ReplayGain 解析到 0 dB 时**仍然算 bit-perfect**，因为没有乘法发生。

**优先级.** `OUTPUT-POLICY.md §6` 规定为 **device / shared / format → routing → DSP → volume**，而不是 reason code 的编号顺序。所以"为什么不是 bit-perfect"给出的永远是**最根本**的那一条。

**怎么区分.** 看 `reason` 与 `active_stages`；`bypassed_stages` 是同一结构里的另一半。

---

## 8. DSD

**三条路，代价完全不同。**

| 路径 | 载体 | 是否无损 | 怎么区分 |
|---|---|---|---|
| Native | 设备驱动原生接受 DSD | 是 | 设备 caps 声明 `nativeDsdSupported` |
| DoP（DSD over PCM） | 把 DSD 装进 PCM 帧传输 | 是（载体是 PCM） | `NAE_OpenDop` |
| 降频抽取 | 保留每 D 位中的一位 | **有损** | `NAE_OpenDopDownrated` |

抽取不是"每 D 个位取一个"那么简单——那会把带外 noise 一起搬进来。实现对 DSD256/512 先做 decimation（抽取）、再 requantise（重新量化），然后以较低的 carrier（载波）打包成 DoP。

**DoP 不是"降低要求"，而是把 DSD 装进一个 PCM 载体**：DSD64 需要 endpoint 接受 176.4 kHz、24 valid bits。endpoint 不接受就是不接受，DoP 帮不上忙。`engine/README.md` 记录 DoP 只在一个 endpoint（`扬声器 (WALKMAN)`）上验证过——这是**能力边界，不是通用保证**。

**我们实测**：DSD64 源报告 `176400 Hz`，在一个只接受 192 kHz 的 endpoint 上得到 `reason 6`（退到支持的 sample rate）。在支持 176.4 的 DAC 上不会发生。

**听众体会.** native / DoP 与抽取的差别在极高频和空间感；抽取的损失是**可测量**的，不只是可听。

---

## 9. gapless（无缝播放）

**改了什么.** 下一首在上一首结束前就 decode 就绪，接缝处不关流、不重开设备。

**怎么区分.** **两个字段，缺一不可**：

- `track_serial` 在 `state` 保持 `PLAYING` 的同时递增 —— 说明这是接缝，不是新开流
- `position` 归零到新曲目 —— 说明换了曲目，不是原曲继续

只看其中一个都会被骗：serial 涨了但 position 没归零是错位；position 归零但 state 掉了是重开流。

**听众体会.** 现场专辑、古典乐章之间不该有静音缝，也不该有咔哒。

**这条检查本身曾经是坏的**：它把"B 曲必须恰好 1 秒"写死在断言里（`> 0.9 && < 1.1`），于是任何别的夹具都被报成引擎失败，而用法说明里没写这个前提。现在改成**先量出 B 的真实时长再比较**，用法里写明 A 必须在约 2.5 秒内结束。1 秒和 3 秒的 B 都验证通过。

---

## 10. buffer（缓冲）、latency（延迟）与实时性

**改了什么.** 设备 buffer 大小、每周期填充、以及 decode 是否会拖慢 render thread（渲染线程）。

**怎么区分.** 三个数字：

- `bufferDurationMs` / `latencyMs` —— 设备给的
- `maxBufferGapMs` —— 两次写入之间最长的空档
- `maxDecodeStallMs` —— render thread 在 **decode** 上花掉的最长时间

最后一项是结构性的：这个引擎在 render loop 里 decode，而对照实现（Twilight Echo）把 decoder 放在独立线程。decode 花的每一毫秒，都是从设备给出的时间预算里扣掉的。

**听众体会.** buffer 大 → 抗 jitter（抖动）强、latency 高、切歌迟钝；buffer 小 → 跟手，但一有停顿就是咔哒。`maxDecodeStallMs` 偏大的人会在安静段落听到偶发杂音，而其他计数器全都正常——因为每一次写入本身都完整正确，只是**晚到了**。

**latency 数字不能跨平台比。** macOS 的构成是 FIFO（first in, first out，先进先出队列）深度加设备 buffer，和 Windows 不是同一个和。

---

## 11. 分析：DR / BPM / peaks / spectrum

**改了什么.** 在独立线程 decode 一遍，结果进 LRU（least recently used，最近最少使用）cache（缓存），键是 `path + size + mtime + analyzerVersion`。

**怎么区分.** `analysis_ready { cached }`。`cached = true` 说明是复用，不是重新 decode。

**一条硬规则**：`bpm_valid = false` 时界面**必须什么都不显示**，不能给一个看着合理的数字。分析器对过短的信号或过弱的自相关峰会说"这条不可用"。

**听众体会.** DR（dynamic range，动态范围）用来看母带的压缩程度；BPM 用来选歌；spectrum（频谱）是装饰，但必须是**真的**——一个假的 spectrum 和没有 spectrum，在听感上没有区别，在信任上差别很大。

---

## 12. 失败与 device lost（设备丢失）

**分层，不合并。**

| 情况 | 报告 | 之后 |
|---|---|---|
| 设备消失 | `DEVICE_LOST` | 播放暂停；**handle、decoder、position 仍然有效** |
| 文件打不开 | 具体错误 | 不假装成功 |
| 短渲染 | 失败 | 不是"成功但音频少一点" |
| 无设备可选 | 错误 | **绝不替调用者换一个 endpoint** |

最后一条是产品决定：一次不带 device id 的播放只能落在上次用过的 endpoint 上；它没了就是 device-lost，不是"那就用板载声卡吧"。

**听众体会.** 拔掉 DAC，音乐停住并等你插回来，而不是悄悄切到笔记本喇叭继续放。

---

## 13. 用户 / 测试者自己怎么验证

### 13.1 只用耳朵（需要已知内容的测试碟）

1. **Native 与 Device 的差别**：同一首 44.1 kHz 的歌，两种策略各放一遍；Device 模式下若 endpoint 是 192 kHz，不会听到设备 re-lock。
2. **dither 的差别**：16 位输出，找一段极安静的尾音，比较开关 dither。
3. **gapless**：放一张现场专辑，接缝处不该有静音或咔哒。
4. **设备的 24-in-32**：不用听，看 `valid_bits`。

### 13.2 用数字（可复现）

我们在真实内容上跑过的结果，可作为期望值参考：

```
21 个真实文件（flac / wav / m4a / ape / mp3 / tak），native 策略：
  FLAC · WAV · ALAC · APE · MP3 · TAK   全部 bitPerfect = true, resampling = false
  DSF（176400 Hz）在 192 kHz endpoint 上  reason = 6, bitPerfect = false
```

引擎自带的检查（`engine/build/` 下，均需真实 endpoint）：

| 检查 | 期望 |
|---|---|
| `nae_test` | All engine checks passed |
| `mode_check` | All output mode checks passed |
| `reopen_check` | 快速重开不崩 |
| `decode_thread_check <file>` | render thread 仍然拿到音频 |
| `pause_edge_check <A> <B>`（两个文件都要 ≥ 5 秒） | 暂停点不影响另一首的位置 |
| `gapless_check <A> <B>` | A 需在约 2.5 秒内结束；对 B 的时长无假设 |

---

## 14. 哪些是"测出来的"，哪些只能"听出来"

| 只能测 | 只能听 |
|---|---|
| sample rate、bit depth、valid bits | EQ 的音色是否讨喜 |
| 是否发生了 resampling、dither | dither 在安静段落的差别 |
| 是否 bit-perfect、reason code | resampling 质量档位的差异 |
| 接缝是否发生（serial + position） | 接缝处有没有可闻的咔哒 |
| buffer / latency / decode stall（毫秒） | 系统是否"跟手" |
| DR、BPM、peaks、spectrum | 视觉上是否可信 |

左边每一项都有对应的字段；右边每一项都没有替代品。

**一条原则**：能测的，就不该由耳朵来宣称。说"听起来是 bit-perfect"没有意义——`reason` 是 0 就是 0。
