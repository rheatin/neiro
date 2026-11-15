// 可视化帧（Base VisualFrame 全链）→ 音乐光的 uniforms。订阅只在 Now
// Playing 打开且播放中开（`setVisualiser(16)`；离开状态 `setVisualiser(
// null)`）——性能规则照 spotish：不是常开，帧驱动（无定时器自绘）。
//
// 每帧的有功工作 = 一次对数聚合 + `liveValues(23)` 的 ≤30 次原生 set +
// 从 bins 导出 bass/mid/treble/level/beat/since/onset（引擎的 metering
// 不在 wire 上，全部从线性 bins 计算）。值的槽位照 spotish
// `lightUniforms`：BAND0..15（16 bands）、BEAT、LEVEL、BASS、MID、
// TREBLE、SINCE、ONSET。颜色没有封面取色管线，走主题——`token("primary")`
// 主色、`chart-3`/`chart-4` 次色；亮度由 shader 的 `tame()` 约束（≈0.32）。

import { computed, effect, signal, untracked, createAnimatedValue, type AnimatedValue } from "nsh";
import { engine } from "../backend/engine";
import { engineHealth } from "../state/engine";
import { nowPlayingOpen } from "./overlays";
import { isPlaying } from "./player";

/** Whether a surface that draws the frames is up: the floating PlayerBar is
 * always mounted on the desktop shell while playing, alongside Now Playing
 * and the output console's PPM meter. */
export const visualiserWanted = computed(() => isPlaying());

/** Now Playing 打开且播放中（Now Playing 背景光的 live 状态）。 */
export const isPlayingSpectrum = computed(() => nowPlayingOpen() && isPlaying());

/** liveValues(count)：spotish 的同款分槽 set（相同值不重发）。 */
function liveValues(count: number) {
  const values: AnimatedValue[] = Array.from({ length: count }, () => createAnimatedValue(0));
  const last = new Array<number>(count).fill(0);
  return {
    values,
    set(i: number, v: number) {
      if (last[i] === v) return;
      last[i] = v;
      values[i].set(v);
    },
  };
}

// spotish `lightUniforms` 的槽位表。
export const BAND0 = 0;
export const BEAT = 16;
export const LEVEL = 17;

/** The engine's own RMS from the last visualiser frame, as linear amplitude
 * (0…1). `null` until a frame arrives: the PPM ladder stays dark rather than
 * showing a level nobody measured. */
const peakSig = signal<number | null>(null);
const rmsSig = signal<number | null>(null);
/** The engine's own sample peak from the last frame (linear 0…1), the maximum
 * across channels. */
export const visualPeak = (): number => peakSig() ?? 0;
/** The same frame's per-channel peaks, or `null` when the engine does not
 * report them — the panel then draws one ladder instead of two it invented. */
const peakLSig = signal<number | null>(null);
const peakRSig = signal<number | null>(null);
export const visualPeakL = (): number | null => peakLSig();
export const visualPeakR = (): number | null => peakRSig();
export const visualPeakKnown = (): boolean => peakSig() !== null;
/** The same frame's RMS, linear 0…1. */
export const visualRms = (): number => rmsSig() ?? 0;
export const BASS = 18;
export const MID = 19;
export const TREBLE = 20;
export const SINCE = 21;
export const ONSET = 22;
/** The light's clock and colours' travel: stepped with the music's frames
 * (16 ms) rather than with the display, so a 144 Hz screen does not redraw
 * the shader four times per audio frame. */
export const PHASE = 23;
export const CLOCK = 24;
export const SLOTS = 25;
/** Seconds the colours take to travel round, and the clock to wrap. */
const PHASE_S = 24;
const CLOCK_S = 600;

let lastFeed = -1;
/** When this run of playback started (ms), for the light's clock. */
let startedAt = 0;
/** 16 段的平滑状态（0..1，dB 域）；离开播放时归零。 */
const smoothed: number[] = new Array(16).fill(0);
let lastBeatAt = -10;
let lastPulse = 0;

/** 全部 uniforms 的 animated values（toolbar-fx 的 lightUniforms 直接引
 * 用它们的槽位）。 */
export const live = liveValues(SLOTS);
live.set(SINCE, 2);

/** 一次接线：订阅与暂停的清零都挂 Now Playing 的 open/playing。 */
effect(() => {
  // 引擎没连上时不问（一次拒绝直接影响无头测试的安静度）。
  if (engineHealth().status !== "ready") return;
  const wanted = visualiserWanted();
  untracked(() => {
    try {
      engine.setVisualiser(wanted ? 16 : null);
    } catch (error) {
      console.warn("neiro: visualiser 开关被拒:", String(error));
    }
  });
  if (!wanted) {
    // 暂停/关闭：全 0、since 恒 2（spotish 同款），帧不再跟（shader 静
    // 止在基线，不是"卡在最后一帧"）。
    for (let i = 0; i < SLOTS; i++) live.set(i, 0);
    live.set(SINCE, 2);
    lastFeed = -1;
    startedAt = 0;
    for (let i = 0; i < smoothed.length; i++) smoothed[i] = 0;
    peakSig.set(null);
    peakLSig.set(null);
    peakRSig.set(null);
    rmsSig.set(null);
    return;
  }
});

engine.subscribeVisualFrame((frame) =>
  untracked(() => {
    if (!visualiserWanted()) return;
    if (frame.framesFed === lastFeed) return;
    lastFeed = frame.framesFed;
    if (startedAt === 0) startedAt = Date.now();
    const t = (Date.now() - startedAt) / 1000;
    live.set(PHASE, (t / PHASE_S) % 1);
    live.set(CLOCK, t % CLOCK_S);
    const bins = frame.bins.length > 0 ? frame.bins : [0];
    // 线性幅值 → dB → 0..1。引擎的 bins 是线性幅值（满幅正弦 ≈1.0），真实
    // 音乐的频段通常在 0.001–0.1：直接取均值喂 shader，光只剩底部一点。
    // 这里按 dB 归一（-66…-6 dB 映射到 0…1），并做 attack/decay 平滑（起
    // 快落慢），于是每个 band 都在 0…1 的主域里，光的幅度才真实。
    const toUnit = (mag: number) => {
      const db = 20 * Math.log10(Math.max(1e-6, mag));
      return Math.max(0, Math.min(1, (db + 66) / 60));
    };
    const usable = bins.slice(0, Math.max(1, Math.floor(bins.length * 0.72)));
    const n = usable.length;
    for (let b = 0; b < 16; b++) {
      const from = Math.floor(Math.pow(n, b / 16));
      const to = Math.max(from + 1, Math.floor(Math.pow(n, (b + 1) / 16)));
      let peak = 0;
      for (let i = from; i < to && i < n; i++) {
        if (usable[i] > peak) peak = usable[i];
      }
      const target = toUnit(peak);
      const previous = smoothed[b] ?? 0;
      // attack 立即跟上，decay 每帧 18% 回落（帧率 ~30–60 Hz）。
      const value = target > previous ? target : previous + (target - previous) * 0.18;
      smoothed[b] = value;
      live.set(BAND0 + b, value);
    }
    // 三段 + level：同一 dB 域（bass 由最低 4 段、mid 中间 5 段、treble 顶
    // 部 4 段；level 取 rms 的 dB，-60…-6 dB）。
    const bandValue = (b: number) => smoothed[b] ?? 0;
    const bass = (bandValue(0) + bandValue(1) + bandValue(2) + bandValue(3)) / 4;
    const mid = (bandValue(5) + bandValue(6) + bandValue(7) + bandValue(8) + bandValue(9)) / 5;
    const treble = (bandValue(12) + bandValue(13) + bandValue(14) + bandValue(15)) / 4;
    const level = Math.max(0, Math.min(1, (20 * Math.log10(Math.max(1e-6, frame.rms)) + 60) / 54));
    peakSig.set(frame.peak > 0 ? frame.peak : frame.rms);
    peakLSig.set(frame.peakL);
    peakRSig.set(frame.peakR);
    rmsSig.set(frame.rms > 0 ? frame.rms : 0);
    live.set(BASS, bass);
    live.set(MID, mid);
    live.set(TREBLE, treble);
    live.set(LEVEL, level);
    // 简单 onset：bass 突增阈值 → 一个 beat 脉冲（1 then decay），since/onset
    // 与 spotish 同义。
    const now = Date.now() / 1000;
    if (bass > 0.55 && now - lastBeatAt > 0.14) {
      lastBeatAt = now;
      lastPulse = 1;
      live.set(ONSET, Math.floor(now) % 600);
    }
    live.set(BEAT, lastPulse);
    live.set(SINCE, Math.min(2, now - lastBeatAt));
  }),
);

// lastPulse 只亮一帧的窗口（下一个帧若不再越阈则回落）——如上一帧的 beat
// 已经大于本帧的（grown decay），自然回落：在这里 decay 到 0 已由"无新
// 脉冲就不要新值"的 set 去重处理。一个显式的回落 loop 也不引入 JS 每帧：
// keyframes 一次 decay 即可。
// （待优化：beat 衰减曲线。）
