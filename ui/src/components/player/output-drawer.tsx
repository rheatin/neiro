// Output 控制台 — 双饰面精密音频仪器面板（Braun / Nagra × Apple Liquid Glass）。
// 随系统/应用 appearance 切换「阳极氧化银钛 / 哑光黑曜石墨」双材质饰面，
// 内嵌烟熏玻璃光学遥测窗（大字号采样率、脉冲信号总线、32 段立体声 PPM 电平表）、
// 物理微动按键（Bit-Perfect 直通、输出模式、DSP 快捷条）与可折叠工程遥测机架。
// 移动端（含 iPhone Duo）使用 full-screen Sheet + 安全区适配，彻底解决底部折叠问题。

import { colorScheme, createMotion, effect, platform, reduceMotion, safeArea, Scroll, signal, Text, untracked, View, type JSX } from "nsh";
import { Button, Popover } from "nsh/base";
import { engine, reportEngineFailure } from "../../backend/engine";
import { outputOpen, setEqOpenedFromOutput, setOutputOpen } from "../../state/overlays";
import { AppSheet } from "../ui/overlays";
import { focusRing } from "../ui/controls";
import { Icon } from "../ui/icons";
import { notify } from "../../state/feedback";
import { playRelay, playTick } from "../../state/foley";
import { visualPeak, visualPeakL, visualPeakR, visualRms } from "../../state/visualiser";
import { open } from "../../nav";
import { gaplessEnabled, player, setGaplessEnabled, setVolume, volumeLevel } from "../../state/player";
import { upNext } from "../../state/queue";
import { trackForPath } from "../../state/catalog";
import { layout } from "../layout";
import { VolumeControl } from "./parts";
import { bitPerfectChecks } from "./surface/master-bus";
import { devicesLocked, ensureDevices } from "./surface/output-interface";
import { opraActive, opraSelected } from "../../state/opra";
import {
  applyPolicy,
  codecLabel,
  currentDither,
  currentDsdPolicy,
  currentRate,
  currentShare,
  displayName,
  dsdRateLabel,
  formatHz,
  isDsdSource,
  isStreamBitPerfect,
  lastDiagnostic,
  resolvedPath,
  shareMode,
  sourceBits,
  sourceDsdRate,
  sourceFormatLabel,
  type DitherKind,
  type DsdPolicy,
} from "../../state/resolved";

const FLOOR_DB = -48;
const SEGMENTS = 32;

const REASONS: Record<number, { title: string; detail: string; fix: string }> = {
  1: { title: "重采样至设备采样率", detail: "输出采样率与源文件不一致", fix: "将采样率策略切回「跟随源」，或更换支持源采样率的设备" },
  2: { title: "重采样至固定采样率", detail: "已启用固定采样率输出", fix: "将采样率策略切回「跟随源」" },
  3: { title: "已应用抖动 (Dither)", detail: "设备位深窄于源文件", fix: "更换支持更高位深的输出设备" },
  4: { title: "声道映射转换", detail: "输出声道数与源文件不同", fix: "使设备声道配置与源文件一致" },
  5: { title: "设备不支持源格式", detail: "硬件拒绝原格式，引擎自动协商替代格式", fix: "更换支持该格式的音频设备" },
  6: { title: "重采样至最接近速率", detail: "设备拒绝源采样率", fix: "更换支持该采样率的设备" },
  7: { title: "系统共享混音模式", detail: "音频经过系统混音器", fix: "切到「独占直通」以绕过系统混音" },
  8: { title: "软件音量非 100%", detail: "数字衰减会改变原始采样值", fix: "点击右侧 UNITY 将主音量归零衰减" },
  9: { title: "均衡器已开启", detail: "32 段均衡器正在处理信号", fix: "关闭均衡器或点击「一键 Bit-Perfect 直通」" },
  10: { title: "Crossfeed 已开启", detail: "耳机声场交叉馈送正在处理信号", fix: "在下方 DSP 快捷条关闭 Crossfeed" },
  11: { title: "ReplayGain 已开启", detail: "响度增益正在调整电平", fix: "在高级选项中关闭 ReplayGain" },
  12: { title: "卷积器已开启", detail: "IR 卷积正在处理信号", fix: "在高级选项中关闭卷积器" },
  13: { title: "交叉淡入已开启", detail: "曲间交叉淡入已启用", fix: "启用「Gapless 无缝播放」以关闭交叉淡入" },
  14: { title: "32-bit 浮点通路精度转换", detail: "32-bit 整型源经 float32 通路传输", fix: "" },
  15: { title: "防削波限幅器激活", detail: "峰值接近 0 dBFS，限幅器已介入", fix: "" },
};

/** 烟熏光学窗固定深色玻璃与自发光色（浅/深双饰面共用，类似 Braun / Nagra 银黑仪器窗）。 */
const OPTICAL = {
  bg: "radial-gradient(ellipse at 50% 0%, rgba(226,165,63,0.07) 0%, transparent 62%), linear-gradient(180deg, #12151C 0%, #090B0F 100%)",
  text: "#F5F7FA",
  textDim: "#9BA3B2",
  textFaint: "#6B7485",
  amber: "#F5B749",
  emerald: "#34D399",
  red: "#F87171",
  segOff: "#232833",
  divider: "rgba(255,255,255,0.09)",
} as const;

/** 双饰面机加工材质色板：浅色为阳极氧化银钛，深色为哑光黑曜石墨。 */
function consolePalette(dark: boolean) {
  if (!dark) {
    return {
      dark: false,
      chassisGradient: "linear-gradient(180deg, #EDEFF3 0%, #E5E8EE 100%)",
      chassisBorder: "#D0D6E0",
      headerBorder: "rgba(15,23,42,0.08)",
      grabber: "#BCC4D0",
      plateGradient: "linear-gradient(180deg, #FFFFFF 0%, #F7F8FA 100%)",
      plateBorder: "#D5DBE4",
      plateShadow: [
        { x: 0, y: 1, blur: 3, color: "rgba(15,23,42,0.04)" },
        { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.95)", inset: true },
      ],
      bezelOuterBg: "linear-gradient(180deg, #DFE4EC 0%, #CFD5DF 100%)",
      bezelOuterBorder: "#BCC4D2",
      opticalBezel: "#232832",
      opticalOuterShadow: [
        { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.8)" },
        { x: 0, y: 2, blur: 10, color: "rgba(0,0,0,0.88)", inset: true },
      ],
      wellBg: "#E6E9EF",
      wellBorder: "#CED4DE",
      wellShadow: [{ x: 0, y: 1, blur: 2, color: "rgba(15,23,42,0.07)", inset: true }],
      keyGradient: "linear-gradient(180deg, #FFFFFF 0%, #F2F4F7 100%)",
      keyBorder: "#CED5DF",
      keyShadow: [
        { x: 0, y: 1.5, blur: 0, color: "#C6CDD8" },
        { x: 0, y: 2, blur: 4, color: "rgba(15,23,42,0.05)" },
        { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.96)", inset: true },
      ],
      keyPressedShadow: [
        { x: 0, y: 0.5, blur: 0, color: "#C6CDD8" },
        { x: 0, y: 1, blur: 2, color: "rgba(15,23,42,0.10)", inset: true },
      ],
      amberKeyBg: "linear-gradient(180deg, #FBF0DA 0%, #F2DFB8 100%)",
      amberKeyBorder: "#BD7D22",
      amberKeyText: "#6E4208",
      amberKeyShadow: [
        { x: 0, y: 1.5, blur: 0, color: "#CEAB70" },
        { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.85)", inset: true },
      ],
      emeraldKeyBg: "linear-gradient(180deg, #E2F8EE 0%, #C6F0DD 100%)",
      emeraldKeyBorder: "#0D9668",
      emeraldKeyText: "#065F46",
      emeraldKeyShadow: [
        { x: 0, y: 1.5, blur: 0, color: "#7CCFB0" },
        { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.85)", inset: true },
      ],
      textPrimary: "#111317",
      textSecondary: "#525A66",
      textMuted: "#7E8794",
      accent: "#9A6316",
      accentSoft: "rgba(154,99,22,0.10)",
      emerald: "#059669",
      emeraldSoft: "rgba(5,150,105,0.11)",
      emeraldBorder: "rgba(5,150,105,0.34)",
      red: "#DC2626",
      ledOff: "#BCC4D0",
      ledOffBorder: "#9EA7B5",
    };
  }
  return {
    dark: true,
    chassisGradient: "linear-gradient(180deg, #13151A 0%, #0B0D10 100%)",
    chassisBorder: "#272C36",
    headerBorder: "rgba(255,255,255,0.08)",
    grabber: "#434A57",
    plateGradient: "linear-gradient(180deg, #1A1D24 0%, #14161C 100%)",
    plateBorder: "#2B303B",
    plateShadow: [
      { x: 0, y: 3, blur: 10, color: "rgba(0,0,0,0.42)" },
      { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.07)", inset: true },
    ],
    bezelOuterBg: "linear-gradient(180deg, #242832 0%, #1B1E26 100%)",
    bezelOuterBorder: "#343A47",
    opticalBezel: "#0F1116",
    opticalOuterShadow: [
      { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.07)" },
      { x: 0, y: 3, blur: 12, color: "rgba(0,0,0,0.92)", inset: true },
    ],
    wellBg: "#0E1015",
    wellBorder: "#282D37",
    wellShadow: [{ x: 0, y: 1, blur: 4, color: "rgba(0,0,0,0.75)", inset: true }],
    keyGradient: "linear-gradient(180deg, #272B35 0%, #1D2028 100%)",
    keyBorder: "#383E4C",
    keyShadow: [
      { x: 0, y: 1.5, blur: 0, color: "#0B0C0F" },
      { x: 0, y: 2, blur: 6, color: "rgba(0,0,0,0.50)" },
      { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.11)", inset: true },
    ],
    keyPressedShadow: [
      { x: 0, y: 0.5, blur: 0, color: "#0B0C0F" },
      { x: 0, y: 1, blur: 3, color: "rgba(0,0,0,0.75)", inset: true },
    ],
    amberKeyBg: "linear-gradient(180deg, rgba(226,165,63,0.28) 0%, rgba(226,165,63,0.12) 100%)",
    amberKeyBorder: "rgba(226,165,63,0.78)",
    amberKeyText: "#F6C56E",
    amberKeyShadow: [
      { x: 0, y: 1.5, blur: 0, color: "#191308" },
      { x: 0, y: 0, blur: 10, color: "rgba(226,165,63,0.20)" },
      { x: 0, y: 1, blur: 0, color: "rgba(255,220,150,0.26)", inset: true },
    ],
    emeraldKeyBg: "linear-gradient(180deg, rgba(16,185,129,0.28) 0%, rgba(16,185,129,0.12) 100%)",
    emeraldKeyBorder: "rgba(52,211,153,0.78)",
    emeraldKeyText: "#34D399",
    emeraldKeyShadow: [
      { x: 0, y: 1.5, blur: 0, color: "#071A12" },
      { x: 0, y: 0, blur: 10, color: "rgba(16,185,129,0.22)" },
      { x: 0, y: 1, blur: 0, color: "rgba(167,243,208,0.24)", inset: true },
    ],
    textPrimary: "#F2F4F7",
    textSecondary: "#A3ACB9",
    textMuted: "#717987",
    accent: "#E2A53F",
    accentSoft: "rgba(226,165,63,0.14)",
    emerald: "#34D399",
    emeraldSoft: "rgba(16,185,129,0.13)",
    emeraldBorder: "rgba(52,211,153,0.36)",
    red: "#F1676C",
    ledOff: "#333945",
    ledOffBorder: "#1C2028",
  };
}

/** 脉冲动画单例（模块级），避免 visualiser 高频刷新时重置动画。 */
const busPulse = createMotion();
let busPulseActive = false;

function ensureBusPulse(playing: boolean): void {
  if (!playing || reduceMotion()) {
    if (busPulseActive) {
      busPulse.stop();
      busPulseActive = false;
    }
    return;
  }
  if (busPulseActive) return;
  busPulseActive = true;
  void busPulse.play({
    keyframes: { translateX: [-16, 16], opacity: [0.15, 1, 0.15] },
    duration: 1600,
    easing: "linear",
    iterations: "infinite",
  });
}

/** 抽屉入口：桌面端右侧面板，移动端/iOS 使用全屏 Sheet（含 iPhone Duo）。 */
export function OutputSheet() {
  const isMobile = () => layout() === "mobile";
  return (
    <AppSheet
      open={outputOpen()}
      onOpenChange={setOutputOpen}
      side={isMobile() ? "bottom" : "right"}
      full={isMobile()}
      closeOnSwipe={isMobile()}
      testID="output"
      popupWidth={480}
    >
      <OutputContent />
    </AppSheet>
  );
}

function JewelLed(props: { lit: boolean; tone?: "emerald" | "amber" | "red"; size?: number }) {
  const pal = () => consolePalette(colorScheme() === "dark");
  const size = () => props.size ?? 7;
  const color = () =>
    props.tone === "red"
      ? pal().red
      : props.tone === "amber"
        ? pal().accent
        : pal().emerald;
  return (
    <View
      class="rounded-full"
      style={{
        width: size(),
        height: size(),
        backgroundColor: props.lit ? color() : pal().ledOff,
        borderWidth: 1,
        borderColor: props.lit ? color() : pal().ledOffBorder,
        boxShadow: props.lit
          ? [
              { x: 0, y: 0, blur: 5, color: color() },
              { x: 0, y: 0, blur: 10, color: color() },
            ]
          : undefined,
      }}
      accessibilityElementsHidden
    />
  );
}

function DeckPlate(props: { children: JSX.Element; class?: string }) {
  const pal = () => consolePalette(colorScheme() === "dark");
  return (
    <View
      class={`rounded-2xl p-3 ${props.class ?? ""}`}
      style={{
        backgroundImage: pal().plateGradient,
        borderWidth: 1,
        borderColor: pal().plateBorder,
        boxShadow: pal().plateShadow,
      }}
    >
      {props.children}
    </View>
  );
}

function TactileKey(props: {
  children: JSX.Element;
  onPress?: () => void;
  lit?: boolean;
  tone?: "amber" | "emerald";
  disabled?: boolean;
  label: string;
  testID?: string;
  class?: string;
  style?: Record<string, unknown>;
}) {
  const pal = () => consolePalette(colorScheme() === "dark");
  const isEmerald = () => props.tone === "emerald";
  const bg = () =>
    props.disabled
      ? undefined
      : props.lit
        ? isEmerald()
          ? pal().emeraldKeyBg
          : pal().amberKeyBg
        : pal().keyGradient;
  const border = () =>
    props.disabled
      ? pal().wellBorder
      : props.lit
        ? isEmerald()
          ? pal().emeraldKeyBorder
          : pal().amberKeyBorder
        : pal().keyBorder;
  const shadow = () =>
    props.disabled
      ? undefined
      : props.lit
        ? isEmerald()
          ? pal().emeraldKeyShadow
          : pal().amberKeyShadow
        : pal().keyShadow;

  return (
    <View
      testID={props.testID}
      accessibilityLabel={props.label}
      accessibilityState={{ disabled: props.disabled ?? false, selected: props.lit ?? false }}
      onPress={props.disabled ? undefined : props.onPress}
      class={`rounded-xl ${props.class ?? ""} ${focusRing}`}
      style={{
        ...(props.disabled ? { backgroundColor: pal().wellBg, borderStyle: "dashed" as const } : { backgroundImage: bg() }),
        borderWidth: 1,
        borderColor: border(),
        boxShadow: shadow(),
        opacity: props.disabled ? 0.38 : 1,
        ...props.style,
      }}
      activeStyle={{
        transform: [{ translateY: 1.5 }],
        boxShadow: pal().keyPressedShadow,
      }}
    >
      {props.children}
    </View>
  );
}

function PpmRow(props: { label: string; level: number }) {
  const db = () => (props.level > 0 ? 20 * Math.log10(Math.max(1e-6, props.level)) : FLOOR_DB);
  const lit = () => Math.max(0, Math.min(SEGMENTS, Math.round(((db() - FLOOR_DB) / -FLOOR_DB) * SEGMENTS)));
  return (
    <View class="flex-row items-center gap-2">
      <Text class="w-7 font-mono text-[9px] font-bold tracking-wider" style={{ color: OPTICAL.textFaint }}>
        {props.label}
      </Text>
      <View class="flex-1 flex-row gap-[2px]">
        <Each of={Array.from({ length: SEGMENTS }, (_, i) => i)} key="index">
          {(index) => {
            const on = () => index() < lit();
            const color = () =>
              index() >= SEGMENTS - 3
                ? OPTICAL.red
                : index() >= SEGMENTS - 8
                  ? OPTICAL.amber
                  : OPTICAL.emerald;
            return (
              <View
                style={{
                  flex: 1,
                  height: 6,
                  borderRadius: 1.5,
                  backgroundColor: on() ? color() : OPTICAL.segOff,
                  boxShadow: on() ? [{ x: 0, y: 0, blur: 3, color: color() }] : undefined,
                }}
              />
            );
          }}
        </Each>
      </View>
    </View>
  );
}

function StatusPill() {
  const pal = () => consolePalette(colorScheme() === "dark");
  const noStream = () => resolvedPath() === null;
  const bitPerfect = () => isStreamBitPerfect();
  return (
    <View
      class="flex-row items-center gap-1.5 rounded-full px-2.5 py-1"
      style={{
        backgroundColor: noStream()
          ? pal().wellBg
          : bitPerfect()
            ? pal().emeraldSoft
            : pal().accentSoft,
        borderWidth: 1,
        borderColor: noStream()
          ? pal().wellBorder
          : bitPerfect()
            ? pal().emeraldBorder
            : pal().accent,
      }}
    >
      <JewelLed
        lit={!noStream()}
        tone={bitPerfect() ? "emerald" : "amber"}
        size={6}
      />
      <Text
        class="font-mono text-[10px] font-bold tracking-wider"
        style={{
          color: noStream()
            ? pal().textSecondary
            : bitPerfect()
              ? pal().emerald
              : pal().accent,
        }}
      >
        {noStream() ? "NO STREAM" : bitPerfect() ? "BIT-PERFECT" : "PROCESSED"}
      </Text>
    </View>
  );
}

function OpticalTelemetryBay(props: { peak: number; peakL: number | null; peakR: number | null; rms: number }) {
  const pal = () => consolePalette(colorScheme() === "dark");
  const path = () => resolvedPath();
  const rp = () => engine.renderPath();
  const p = () => player();
  const hasStream = () => path() !== null || p().state === "playing" || p().state === "paused";
  const playing = () => p().state === "playing";
  const bitPerfect = () => isStreamBitPerfect();

  effect(() => {
    ensureBusPulse(playing());
  });

  const rateHero = () => {
    if (!hasStream()) return { main: "IDLE", unit: "STANDBY" };
    const rpVal = path();
    if (isDsdSource()) {
      const rate = rpVal?.dsdRate || sourceDsdRate();
      return { main: rate > 0 ? dsdRateLabel(rate) : "DSD", unit: rpVal?.dsdTransport === "dop" ? "DoP" : "1-BIT" };
    }
    const hz = rpVal?.sampleRate || rpVal?.sourceRate || 0;
    if (hz <= 0) return { main: "LIVE", unit: "STREAM" };
    const khz = hz / 1000;
    const text = Number.isInteger(khz) ? String(khz) : khz.toFixed(1);
    return { main: text, unit: "kHz" };
  };

  const specLine = () => {
    if (!hasStream()) return "选择曲目开始播放以激活实时遥测";
    const rpVal = path();
    const parts: string[] = [];
    const codec = codecLabel() || sourceFormatLabel();
    if (codec) parts.push(codec.toUpperCase());
    if (rpVal) {
      if (isDsdSource()) {
        parts.push("1-BIT DSD");
      } else {
        const bits = rpVal.validBits || rpVal.containerBits || sourceBits();
        if (bits > 0) parts.push(`${bits}-BIT${rpVal.isFloat ? " FLOAT" : ""}`);
      }
      if (rpVal.sourceChannels > 0) parts.push(`${rpVal.sourceChannels}CH`);
      if (rpVal.bufferLatencyMs > 0) parts.push(`${rpVal.bufferLatencyMs.toFixed(1)} ms`);
    }
    return parts.join(" · ") || "音频流已打开";
  };

  const protocolTag = () => {
    const rpVal = path();
    if (!rpVal) return platform.os === "ios" ? "AVAudioSession" : shareMode() === 1 ? "EXCL" : "SHARED";
    if (platform.os === "ios") return "AVAudioSession";
    const b = rpVal.backend === "coreaudio" ? "COREAUDIO" : rpVal.backend === "asio" ? "ASIO" : "WASAPI";
    if (rpVal.backend === "asio") return "ASIO";
    return rpVal.shareMode === 1 ? `${b} EXCL` : `${b} SHARED`;
  };

  const srcRateShort = () => {
    const rpVal = path();
    if (!rpVal || rpVal.sourceRate <= 0) return "SRC";
    return `${(rpVal.sourceRate / 1000).toFixed(rpVal.sourceRate % 1000 === 0 ? 0 : 1)}k`;
  };

  const outRateShort = () => {
    const rpVal = path();
    if (!rpVal || rpVal.sampleRate <= 0) return "OUT";
    return `${(rpVal.sampleRate / 1000).toFixed(rpVal.sampleRate % 1000 === 0 ? 0 : 1)}k`;
  };

  const dspActiveCount = () => {
    const r = rp();
    const s = engine.settings();
    let count = 0;
    if (r?.eqEnabled ?? s?.eqEnabled) count++;
    if (r?.crossfeedEnabled ?? s?.crossfeedEnabled) count++;
    if (r?.replayGainEnabled ?? (s && s.replayGainMode !== 0)) count++;
    if (r?.convolverEnabled ?? s?.convolverEnabled) count++;
    if (r?.crossfadeEnabled ?? s?.crossfadeEnabled) count++;
    if (opraActive()) count++;
    return count;
  };

  const stereo = () => props.peakL !== null && props.peakR !== null;
  const showStereoIdle = () => !hasStream();
  const dbOf = (level: number) => (level > 0 ? 20 * Math.log10(Math.max(1e-6, level)) : null);
  const reading = (v: number | null) => (v === null ? "—" : `${v.toFixed(1)} dB`);
  const trackName = () => (hasStream() ? displayName(p().path, p().title) || "正在播放" : "待机 · NO STREAM");

  return (
    <View
      class="rounded-2xl p-[3px]"
      style={{
        backgroundImage: pal().bezelOuterBg,
        borderWidth: 1,
        borderColor: pal().bezelOuterBorder,
        boxShadow: pal().plateShadow,
      }}
    >
      <View
        class="gap-2.5 rounded-[13px] p-3"
        style={{
          backgroundImage: OPTICAL.bg,
          borderWidth: 1,
          borderColor: pal().opticalBezel,
          boxShadow: pal().opticalOuterShadow,
        }}
      >
        {/* 顶行：模块名 + 曲目 + 传输协议芯片 */}
        <View class="flex-row items-center justify-between gap-2">
          <View class="min-w-0 flex-1 flex-row items-center gap-2">
            <Text class="font-mono text-[10px] font-bold tracking-[0.14em]" style={{ color: OPTICAL.amber }}>
              信号链路
            </Text>
            <Text class="line-clamp-1 flex-1 text-xs font-medium" style={{ color: OPTICAL.textDim }}>
              {trackName()}
            </Text>
          </View>
          <View
            class="rounded-md px-2 py-0.5"
            style={{
              backgroundColor: bitPerfect() ? "rgba(52,211,153,0.14)" : "rgba(255,255,255,0.07)",
              borderWidth: 1,
              borderColor: bitPerfect() ? "rgba(52,211,153,0.45)" : "rgba(255,255,255,0.14)",
            }}
          >
            <Text
              class="font-mono text-[9px] font-bold tracking-wider"
              style={{ color: bitPerfect() ? OPTICAL.emerald : OPTICAL.textDim }}
            >
              {protocolTag()}
            </Text>
          </View>
        </View>

        {/* 主光学读数 + 水平信号总线 */}
        <View class="flex-row items-center justify-between gap-3">
          <View class="min-w-0 flex-1 gap-1">
            <View class="flex-row items-baseline gap-1.5">
              <Text
                class="font-mono text-[28px] font-bold leading-none tabular-nums"
                style={{ color: hasStream() ? OPTICAL.text : OPTICAL.textDim }}
              >
                {rateHero().main}
              </Text>
              <Text
                class="font-mono text-xs font-bold tracking-wider"
                style={{ color: hasStream() ? OPTICAL.amber : OPTICAL.textFaint }}
              >
                {rateHero().unit}
              </Text>
            </View>
            <Text class="line-clamp-1 font-mono text-[10px]" style={{ color: OPTICAL.textDim }}>
              {specLine()}
            </Text>
          </View>

          {/* 紧凑可视信号流：SRC → OUT */}
          <View
            class="items-center gap-1 rounded-xl px-2.5 py-2"
            style={{
              backgroundColor: "rgba(255,255,255,0.04)",
              borderWidth: 1,
              borderColor: "rgba(255,255,255,0.11)",
            }}
          >
            <View class="flex-row items-center gap-1.5">
              <Text class="font-mono text-[9px] font-bold" style={{ color: hasStream() ? OPTICAL.amber : OPTICAL.textDim }}>
                {srcRateShort()}
              </Text>
              <View class="relative h-2 w-8 items-center justify-center overflow-hidden">
                <View
                  style={{
                    width: 32,
                    height: 1.5,
                    backgroundColor: hasStream()
                      ? bitPerfect()
                        ? "rgba(52,211,153,0.55)"
                        : "rgba(245,183,73,0.55)"
                      : "rgba(255,255,255,0.18)",
                  }}
                />
                <If when={playing()}>
                  <View
                    style={{
                      position: "absolute",
                      width: 5,
                      height: 5,
                      borderRadius: 3,
                      backgroundColor: bitPerfect() ? OPTICAL.emerald : OPTICAL.amber,
                      ...busPulse.style,
                    }}
                  />
                </If>
              </View>
              <Text class="font-mono text-[8px]" style={{ color: OPTICAL.textDim }}>
                →
              </Text>
              <Text class="font-mono text-[9px] font-bold" style={{ color: hasStream() ? (bitPerfect() ? OPTICAL.emerald : OPTICAL.text) : OPTICAL.textDim }}>
                {outRateShort()}
              </Text>
            </View>
            <Text class="font-mono text-[8px] font-semibold tracking-wider" style={{ color: dspActiveCount() > 0 ? OPTICAL.amber : OPTICAL.textDim }}>
              {dspActiveCount() > 0 ? `DSP ×${dspActiveCount()} ACTIVE` : "DSP BYPASS"}
            </Text>
          </View>
        </View>

        {/* 32 段立体声 PPM 电平表 + 真实对数 dBFS 刻度尺 */}
        <View class="gap-1.5 pt-2" style={{ borderTopWidth: 1, borderTopColor: OPTICAL.divider }}>
          <View class="flex-row items-center justify-between">
            <View class="flex-row items-center gap-1.5">
              <Text class="font-mono text-[9px] font-bold tracking-[0.14em]" style={{ color: OPTICAL.textDim }}>
                PPM LEVEL
              </Text>
              <Text class="font-mono text-[8px]" style={{ color: OPTICAL.textFaint }}>
                {stereo() || showStereoIdle() ? "STEREO" : "MONO"}
              </Text>
            </View>
            <Text class="font-mono text-[10px] tabular-nums" style={{ color: dbOf(props.peak) === null ? OPTICAL.textDim : OPTICAL.amber }}>
              {`PEAK ${reading(dbOf(props.peak))} · RMS ${reading(dbOf(props.rms))}`}
            </Text>
          </View>

          <If when={stereo() || showStereoIdle()} else={<PpmRow label="PEAK" level={props.peak} />}>
            <PpmRow label="L" level={props.peakL ?? 0} />
            <PpmRow label="R" level={props.peakR ?? 0} />
          </If>

          <View style={{ height: 14, marginLeft: 36 }}>
            <Each of={[0, -6, -12, -24, -36, -48]} key="index">
              {(mark) => {
                const at = () => (mark() - FLOOR_DB) / -FLOOR_DB;
                return (
                  <View style={{ position: "absolute", left: `${at() * 100}%`, marginLeft: -12, width: 24, alignItems: "center", gap: 1 }}>
                    <View style={{ width: 1, height: 3, backgroundColor: mark() === 0 ? OPTICAL.amber : "rgba(255,255,255,0.22)" }} />
                    <Text
                      class="font-mono text-[8px]"
                      style={{ color: mark() === 0 ? OPTICAL.amber : OPTICAL.textDim }}
                    >
                      {mark() === 0 ? "0dB" : String(mark())}
                    </Text>
                  </View>
                );
              }}
            </Each>
          </View>
        </View>
      </View>
    </View>
  );
}

function BitPerfectBusCard() {
  const pal = () => consolePalette(colorScheme() === "dark");
  const isIos = platform.os === "ios";
  const checks = bitPerfectChecks();
  const bitPerfect = () => isStreamBitPerfect();
  const noStream = () => resolvedPath() === null;

  const causes = (): { title: string; fix: string }[] => {
    if (noStream() || bitPerfect()) return [];
    const out: { title: string; fix: string }[] = [];
    const reason = engine.renderPath()?.reason ?? 0;
    if (reason !== 0) {
      const info = REASONS[reason];
      out.push(info ? { title: info.title, fix: info.fix } : { title: `原因代码 ${reason}`, fix: "" });
    }
    if ((engine.renderPath()?.crossfadeEnabled || engine.settings()?.crossfadeEnabled) && reason !== 13) {
      out.push({ title: REASONS[13].title, fix: REASONS[13].fix });
    }
    if (opraActive()) {
      out.push({ title: `OPRA 耳机校正 (${opraSelected()})`, fix: "关闭 OPRA 校正或点击一键直通" });
    }
    return out;
  };

  const goDirect = () => {
    const s = engine.settings();
    if (!s) return;
    playRelay();
    const failures: string[] = [];
    const attempt = (label: string, run: () => void) => {
      try {
        run();
      } catch (error) {
        failures.push(label);
        reportEngineFailure(error, label);
      }
    };
    attempt("关闭 EQ", () => engine.setEq(false, s.eqPreampDb, s.eqBands));
    attempt("关闭 Crossfeed", () => engine.setCrossfeed(false, s.crossfeedAmount, s.crossfeedDelayMs, s.crossfeedCutoffHz));
    attempt("关闭 ReplayGain", () => engine.setReplayGain(0, s.replayGainPreampDb, s.replayGainPreventClipping, s.replayGainFallbackDb));
    attempt("关闭 Convolver", () => engine.setConvolver(false, s.convolverIrPath, s.convolverWet, s.convolverDry, s.convolverGainDb, s.convolverNormalize));
    attempt("关闭 Crossfade", () => engine.setCrossfade(false, s.crossfadeDurationMs, s.crossfadeCurve));
    attempt("切独占 / 源速率", () => applyPolicy("native", 0, "exclusive"));
    attempt("音量归 100%", () => setVolume(1));
    if (failures.length === 0) notify("Bit-Perfect 直通已启用：DSP 旁通、独占、音量 100%");
  };

  const lamp = (label: string, pass: boolean) => (
    <View
      class="flex-1 flex-row items-center justify-center gap-1.5 rounded-lg px-1.5 py-2"
      style={{
        backgroundColor: pass ? pal().emeraldSoft : pal().wellBg,
        borderWidth: 1,
        borderColor: pass ? pal().emeraldBorder : pal().wellBorder,
      }}
    >
      <JewelLed lit={pass} tone="emerald" size={6} />
      <Text
        class="line-clamp-1 font-mono text-[10px] font-semibold"
        style={{ color: pass ? pal().emerald : pal().textSecondary }}
      >
        {label}
      </Text>
    </View>
  );

  return (
    <DeckPlate class="gap-3">
      <View class="flex-row items-center justify-between gap-2">
        <Text class="text-xs font-bold tracking-wider" style={{ color: pal().textPrimary }}>
          Bit-Perfect 校验
        </Text>
        <Text
          class="font-mono text-[10px] font-bold tracking-wider"
          style={{
            color: noStream() ? pal().textMuted : bitPerfect() ? pal().emerald : pal().accent,
          }}
        >
          {noStream() ? "STANDBY · NO STREAM" : bitPerfect() ? "VERIFIED" : "BLOCKED"}
        </Text>
      </View>

      <View class="flex-row gap-1.5">
        {lamp("DSP 旁通", checks.dspClean())}
        {lamp("采样一致", checks.rateMatch())}
        <If when={!isIos}>{lamp("独占直通", checks.exclusive())}</If>
        {lamp("音量 100%", checks.unity())}
      </View>

      <If when={!isIos}>
        <TactileKey
          label="一键 Bit-Perfect 直通"
          lit={bitPerfect()}
          tone="emerald"
          onPress={goDirect}
          class="flex-row items-center justify-center gap-2 px-3 py-2.5"
        >
          <JewelLed lit={bitPerfect()} tone={bitPerfect() ? "emerald" : "amber"} size={8} />
          <Text
            class="text-xs font-bold tracking-wide"
            style={{ color: bitPerfect() ? pal().emeraldKeyText : pal().textPrimary }}
          >
            {bitPerfect() ? "Bit-Perfect 直通已锁定 · 整条通路未改动" : "一键 Bit-Perfect 直通"}
          </Text>
        </TactileKey>
      </If>

      <If when={!noStream() && !bitPerfect()}>
        <View class="gap-1.5">
          <Each of={causes()} key="index">
            {(item) => (
              <View
                class="rounded-xl px-3 py-2"
                style={{
                  backgroundColor: pal().accentSoft,
                  borderWidth: 1,
                  borderColor: pal().amberKeyBorder,
                }}
              >
                <Text class="text-xs font-semibold" style={{ color: pal().textPrimary }}>
                  {item().title}
                </Text>
                <If when={item().fix !== ""}>
                  <Text class="mt-0.5 text-[11px]" style={{ color: pal().textSecondary }}>
                    {item().fix}
                  </Text>
                </If>
              </View>
            )}
          </Each>
          <If when={causes().length === 0 && lastDiagnostic() !== ""}>
            <Text class="text-xs" style={{ color: pal().textSecondary }}>
              {lastDiagnostic()}
            </Text>
          </If>
        </View>
      </If>
    </DeckPlate>
  );
}

function OutputInterfaceCard() {
  const pal = () => consolePalette(colorScheme() === "dark");
  const isIos = platform.os === "ios";
  const menuOpen = signal(false);
  const locked = () => devicesLocked();
  const devices = () => engine.devices();
  const resolvedId = () => resolvedPath()?.deviceId ?? "";
  const backend = () => resolvedPath()?.backend ?? null;
  const onAsio = () => backend() === "asio";
  const asioDevice = () => devices().find((d) => d.id.startsWith("asio:"));
  const mode = () => shareMode();
  const exclusiveActive = () => !onAsio() && mode() === 1;
  const sharedActive = () => !onAsio() && mode() !== 1;

  effect(() => {
    if (outputOpen() && !locked()) {
      untracked(() => ensureDevices());
    }
  });

  const deviceRows = () => {
    const id = resolvedId();
    return [
      { key: "default", id: "" as string | null, name: "引擎默认", detail: "上次使用的音频端点", selected: id === "" },
      ...devices().map((d) => ({
        key: d.id,
        id: d.id as string | null,
        name: d.name || d.id,
        detail:
          d.sampleRate > 0
            ? `${formatHz(d.sampleRate)} · ${d.bitsPerSample} bit · ${d.channels} ch`
            : d.id.startsWith("asio:")
              ? "ASIO 驱动 · 打开时协商"
              : "等待协商",
        selected: id === d.id,
      })),
    ];
  };

  const selectedRow = () => deviceRows().find((r) => r.id === resolvedId()) ?? deviceRows()[0];
  const vol = () => volumeLevel() ?? 0;
  const volDb = () => {
    const v = vol();
    if (v <= 0.0001) return "-∞ dB";
    if (Math.abs(v - 1) < 0.002) return "0.0 dB";
    return `${(20 * Math.log10(v)).toFixed(1)} dB`;
  };
  const atUnity = () => Math.abs(vol() - 1) < 0.002;

  return (
    <DeckPlate class="gap-3">
      <View class="flex-row items-center justify-between gap-2">
        <Text class="text-xs font-bold tracking-wider" style={{ color: pal().textPrimary }}>
          输出与设备
        </Text>
        <If
          when={!isIos && mode() === 1}
          else={
            <Text class="font-mono text-[10px] font-bold tracking-wider" style={{ color: pal().textMuted }}>
              {isIos ? "iOS ROUTE" : "INTERFACE"}
            </Text>
          }
        >
          <TactileKey
            label="释放独占设备"
            onPress={() => {
              playTick();
              try {
                engine.releaseDevice();
              } catch (error) {
                reportEngineFailure(error, "释放设备失败");
              }
            }}
            class="px-2.5 py-1"
          >
            <Text class="text-[11px] font-semibold" style={{ color: pal().accent }}>
              释放独占设备
            </Text>
          </TactileKey>
        </If>
      </View>

      <If
        when={!isIos}
        else={
          <View
            class="gap-1.5 rounded-xl px-3.5 py-3"
            style={{
              backgroundColor: pal().wellBg,
              borderWidth: 1,
              borderColor: pal().wellBorder,
              boxShadow: pal().wellShadow,
            }}
          >
            <View class="flex-row items-center justify-between gap-2">
              <View class="flex-row items-center gap-2">
                <JewelLed lit tone="emerald" size={6} />
                <Text class="text-xs font-bold" style={{ color: pal().textPrimary }}>
                  {resolvedPath()?.deviceName || "iOS 系统音频输出"}
                </Text>
              </View>
              <Text class="font-mono text-[10px] font-semibold" style={{ color: pal().accent }}>
                RemoteIO
              </Text>
            </View>
            <Text class="text-[11px]" style={{ color: pal().textSecondary }}>
              由 AVAudioSession 自动路由（支持有线 DAC / AirPlay），系统音量由设备侧键控制
            </Text>
          </View>
        }
      >
        {/* 3 键物理输出模式切换 */}
        <View
          class="flex-row gap-1.5 rounded-xl p-1.5"
          style={{
            backgroundColor: pal().wellBg,
            borderWidth: 1,
            borderColor: pal().wellBorder,
            boxShadow: pal().wellShadow,
          }}
        >
          <TactileKey
            label="独占直通"
            lit={exclusiveActive()}
            tone="emerald"
            onPress={() => {
              playTick();
              const rate = currentRate();
              try {
                applyPolicy(rate.kind, rate.fixedRate, "exclusive");
              } catch (error) {
                reportEngineFailure(error, "切独占失败");
              }
            }}
            class="flex-1 flex-row items-center justify-center gap-1.5 py-2"
          >
            <JewelLed lit={exclusiveActive()} tone="emerald" size={6} />
            <Text class="text-xs font-bold" style={{ color: exclusiveActive() ? pal().emeraldKeyText : pal().textSecondary }}>
              独占直通
            </Text>
            <If when={exclusiveActive()}>
              <Text class="font-mono text-[9px] font-bold" style={{ color: pal().emeraldKeyText }}>
                EXCL
              </Text>
            </If>
          </TactileKey>

          <TactileKey
            label="系统共享混音"
            lit={sharedActive()}
            tone="amber"
            onPress={() => {
              playTick();
              const rate = currentRate();
              try {
                applyPolicy(rate.kind, rate.fixedRate, "shared");
              } catch (error) {
                reportEngineFailure(error, "切共享失败");
              }
            }}
            class="flex-1 flex-row items-center justify-center gap-1.5 py-2"
          >
            <JewelLed lit={sharedActive()} tone="amber" size={6} />
            <Text class="text-xs font-bold" style={{ color: sharedActive() ? pal().amberKeyText : pal().textSecondary }}>
              系统共享
            </Text>
          </TactileKey>

          <TactileKey
            label={asioDevice() ? "切到 ASIO 驱动" : "未检测到 ASIO 驱动"}
            lit={onAsio()}
            tone="amber"
            disabled={!asioDevice()}
            onPress={() => {
              const dev = asioDevice();
              if (!dev) return;
              playTick();
              try {
                engine.setDevice(dev.id);
              } catch (error) {
                reportEngineFailure(error, "切 ASIO 失败");
              }
            }}
            class="flex-1 flex-row items-center justify-center gap-1.5 py-2"
          >
            <If when={!!asioDevice()}>
              <JewelLed lit={onAsio()} tone="amber" size={6} />
            </If>
            <Text
              class="text-xs font-bold"
              style={{ color: onAsio() ? pal().amberKeyText : asioDevice() ? pal().textSecondary : pal().textMuted }}
            >
              ASIO
            </Text>
          </TactileKey>
        </View>

        {/* 硬件输出端点选择器 */}
        <View class="gap-1.5">
          <View class="flex-row items-center justify-between">
            <Text class="text-[11px] font-semibold" style={{ color: pal().textSecondary }}>
              输出端点
            </Text>
            <Button
              accessibilityLabel={locked() ? "播放中锁定枚举" : "刷新设备"}
              disabled={locked()}
              onPress={() => {
                playTick();
                ensureDevices();
              }}
              class={`flex-row items-center gap-1 rounded-md px-2 py-0.5 ${focusRing}`}
            >
              <Icon name="refresh" size={11} color={locked() ? "muted-foreground" : "primary"} />
              <Text
                class="font-mono text-[10px] font-semibold"
                style={{ color: locked() ? pal().textMuted : pal().accent }}
              >
                {locked() ? "播放中锁定枚举" : "可刷新设备"}
              </Text>
            </Button>
          </View>

          <Popover
            open={menuOpen()}
            onOpenChange={(e) => menuOpen.set(e.open)}
            placement="bottom"
            align="start"
            offset={4}
            popupClass=""
            trigger={
              <TactileKey label="选择端点" class="flex-row items-center justify-between px-3 py-2.5">
                <View class="min-w-0 flex-1">
                  <Text class="line-clamp-1 text-xs font-bold" style={{ color: pal().textPrimary }}>
                    {selectedRow().name}
                  </Text>
                  <Text class="line-clamp-1 font-mono text-[10px]" style={{ color: pal().textSecondary }}>
                    {selectedRow().detail}
                  </Text>
                </View>
                <Icon name="chevronDown" size={14} color="muted-foreground" />
              </TactileKey>
            }
          >
            <View class="w-80 gap-0.5 rounded-xl border border-border bg-popover p-1.5 shadow-xl">
              <Each of={deviceRows()} key={(row) => row.key}>
                {(row) => (
                  <Button
                    accessibilityLabel={`端点 ${row().name}`}
                    onPress={() => {
                      menuOpen.set(false);
                      playTick();
                      try {
                        engine.setDevice(row().id);
                      } catch (error) {
                        reportEngineFailure(error, "切换端点失败");
                      }
                    }}
                    class={`flex-row items-center gap-2 rounded-lg px-2.5 py-2 hover:bg-muted ${focusRing}`}
                  >
                    <JewelLed lit={row().selected} tone="amber" size={6} />
                    <View class="min-w-0 flex-1">
                      <Text class={`line-clamp-1 text-xs font-medium ${row().selected ? "text-primary" : "text-foreground"}`}>
                        {row().name}
                      </Text>
                      <Text class="line-clamp-1 font-mono text-[10px] text-muted-foreground">{row().detail}</Text>
                    </View>
                  </Button>
                )}
              </Each>
            </View>
          </Popover>
        </View>

        {/* 精密增益台：主音量 dB 读数 + 滑条 + UNITY 归零微动键 */}
        <View
          class="flex-row items-center justify-between gap-2 rounded-xl px-3 py-2"
          style={{
            backgroundColor: pal().wellBg,
            borderWidth: 1,
            borderColor: pal().wellBorder,
            boxShadow: pal().wellShadow,
          }}
        >
          <View class="gap-0.5">
            <Text class="text-[11px] font-semibold" style={{ color: pal().textSecondary }}>
              主音量
            </Text>
            <View class="flex-row items-baseline gap-1">
              <Text class="font-mono text-xs font-bold tabular-nums" style={{ color: atUnity() ? pal().emerald : pal().textPrimary }}>
                {volDb()}
              </Text>
              <Text class="font-mono text-[10px] tabular-nums" style={{ color: pal().textMuted }}>
                {Math.round(vol() * 100)}%
              </Text>
            </View>
          </View>
          <View class="flex-1 items-center">
            <VolumeControl />
          </View>
          <TactileKey
            label="音量归零衰减 Unity"
            lit={atUnity()}
            tone="emerald"
            onPress={() => {
              playTick();
              setVolume(1);
            }}
            class="px-2 py-1"
          >
            <Text class="font-mono text-[9px] font-bold" style={{ color: atUnity() ? pal().emeraldKeyText : pal().textSecondary }}>
              UNITY
            </Text>
          </TactileKey>
        </View>
      </If>
    </DeckPlate>
  );
}

function SegmentedStrip<T extends string | number>(props: {
  label: string;
  options: { value: T; label: string }[];
  value: T;
  disabled?: boolean;
  onPick: (v: T) => void;
}) {
  const pal = () => consolePalette(colorScheme() === "dark");
  return (
    <View class="gap-1.5">
      <Text class="text-[11px] font-semibold" style={{ color: pal().textSecondary }}>
        {props.label}
      </Text>
      <View
        class="flex-row gap-1 rounded-xl p-1"
        style={{
          backgroundColor: pal().wellBg,
          borderWidth: 1,
          borderColor: pal().wellBorder,
          boxShadow: pal().wellShadow,
        }}
      >
        <Each of={props.options} key={(o) => String(o.value)}>
          {(o) => {
            const active = () => o().value === props.value;
            return (
              <TactileKey
                label={`${props.label} ${o().label}`}
                lit={active()}
                tone="amber"
                disabled={props.disabled}
                onPress={() => {
                  playTick();
                  props.onPick(o().value);
                }}
                class="flex-1 items-center justify-center py-1.5"
              >
                <Text
                  class="text-[11px] font-bold"
                  style={{ color: active() ? pal().amberKeyText : pal().textSecondary }}
                >
                  {o().label}
                </Text>
              </TactileKey>
            );
          }}
        </Each>
      </View>
    </View>
  );
}

function DspAndTelemetryRack() {
  const pal = () => consolePalette(colorScheme() === "dark");
  const expanded = signal(false);
  const isIos = platform.os === "ios";
  const settings = () => engine.settings();
  const rp = () => engine.renderPath();
  const path = () => resolvedPath();
  const dsdLocked = () => isDsdSource() && path()?.dsdTransport !== "none" && !path()?.dsdPcmFallback;
  const eqActive = () => !!(rp()?.eqEnabled ?? settings()?.eqEnabled) || opraActive();

  const nextPath = () => upNext()[0] ?? null;
  const nextTitle = () => {
    const p = nextPath();
    if (!p) return "队列末尾";
    return trackForPath(p)?.title || p.split(/[\\/]/).pop() || p;
  };

  const rateValue = () => {
    const r = currentRate();
    if (r.kind === "native") return "native";
    if (r.kind === "device") return "device";
    return String(r.fixedRate || 48000);
  };

  const pickRate = (v: string) => {
    const share = currentShare();
    if (v === "native") applyPolicy("native", 0, share);
    else if (v === "device") applyPolicy("device", 0, share);
    else applyPolicy("fixed", Number(v), share);
  };

  const pickDither = (d: DitherKind) => {
    const r = currentRate();
    applyPolicy(r.kind, r.fixedRate, currentShare(), d, currentDsdPolicy());
  };

  const pickDsd = (d: DsdPolicy) => {
    const r = currentRate();
    applyPolicy(r.kind, r.fixedRate, currentShare(), currentDither(), d);
  };

  const telemetryCell = (label: string, value: string) => (
    <View
      class="min-w-0 flex-1 rounded-xl px-3 py-2"
      style={{
        backgroundColor: pal().wellBg,
        borderWidth: 1,
        borderColor: pal().wellBorder,
        boxShadow: pal().wellShadow,
      }}
    >
      <Text class="text-[10px] font-medium" style={{ color: pal().textMuted }}>
        {label}
      </Text>
      <Text class="mt-0.5 line-clamp-1 font-mono text-xs font-bold" style={{ color: pal().textPrimary }}>
        {value}
      </Text>
    </View>
  );

  return (
    <View class="gap-2.5">
      {/* 常驻 DSP 快捷控制条：一键进 32 段机架 EQ + Gapless + Crossfeed */}
      <DeckPlate class="gap-2.5">
        <View class="flex-row items-center justify-between">
          <Text class="text-xs font-bold tracking-wider" style={{ color: pal().textPrimary }}>
            快速处理
          </Text>
          <Text class="font-mono text-[10px]" style={{ color: pal().textMuted }}>
            {eqActive() ? "EQ / OPRA ACTIVE" : "BYPASSED"}
          </Text>
        </View>
        <View class="flex-row gap-2">
          <TactileKey
            label="打开 32 段均衡器机架"
            lit={eqActive()}
            tone="amber"
            onPress={() => {
              playTick();
              setEqOpenedFromOutput(true);
              setOutputOpen(false);
              open({ name: "eq" });
            }}
            class="flex-2 flex-row items-center justify-between px-3 py-2"
          >
            <View class="flex-row items-center gap-1.5">
              <JewelLed lit={eqActive()} tone="amber" size={6} />
              <Text class="text-xs font-bold" style={{ color: eqActive() ? pal().amberKeyText : pal().textPrimary }}>
                32 段均衡器
              </Text>
            </View>
            <Text class="font-mono text-xs font-bold" style={{ color: eqActive() ? pal().amberKeyText : pal().textMuted }}>
              →
            </Text>
          </TactileKey>

          <TactileKey
            label="Gapless 无缝播放"
            lit={gaplessEnabled()}
            tone="emerald"
            onPress={() => {
              playTick();
              setGaplessEnabled(!gaplessEnabled());
            }}
            class="flex-1 flex-row items-center justify-center gap-1.5 px-2 py-2"
          >
            <JewelLed lit={gaplessEnabled()} tone="emerald" size={6} />
            <Text class="text-[11px] font-bold" style={{ color: gaplessEnabled() ? pal().emeraldKeyText : pal().textSecondary }}>
              Gapless
            </Text>
          </TactileKey>

          <TactileKey
            label="Crossfeed 开关"
            lit={!!settings()?.crossfeedEnabled}
            tone="amber"
            disabled={dsdLocked()}
            onPress={() => {
              playTick();
              const s = settings();
              if (s) engine.setCrossfeed(!s.crossfeedEnabled, s.crossfeedAmount, s.crossfeedDelayMs, s.crossfeedCutoffHz);
            }}
            class="flex-1 flex-row items-center justify-center gap-1.5 px-2 py-2"
          >
            <JewelLed lit={!!settings()?.crossfeedEnabled} tone="amber" size={6} />
            <Text
              class="text-[11px] font-bold"
              style={{ color: settings()?.crossfeedEnabled ? pal().amberKeyText : pal().textSecondary }}
            >
              Crossfeed
            </Text>
          </TactileKey>
        </View>
      </DeckPlate>

      {/* 可折叠工程策略与遥测机架 */}
      <TactileKey
        testID="output-advanced-toggle"
        label="音效处理与工程遥测"
        onPress={() => {
          playTick();
          expanded.update((v) => !v);
        }}
        class="flex-row items-center justify-between px-4 py-3"
      >
        <Text class="text-xs font-bold tracking-wider" style={{ color: pal().textPrimary }}>
          音效处理与工程遥测
        </Text>
        <View class="flex-row items-center gap-1.5">
          <Text class="font-mono text-[10px] font-semibold" style={{ color: pal().textMuted }}>
            {expanded() ? "收起" : "展开"}
          </Text>
          <Icon name={expanded() ? "chevronDown" : "chevronRight"} size={14} color="muted-foreground" />
        </View>
      </TactileKey>

      <If when={expanded()}>
        <DeckPlate class="gap-3">
          <SegmentedStrip
            label="ReplayGain 响度均衡"
            disabled={dsdLocked()}
            value={settings()?.replayGainMode ?? 0}
            options={[
              { value: 0, label: "关闭" },
              { value: 1, label: "曲目" },
              { value: 2, label: "专辑" },
            ]}
            onPick={(mode) => {
              const s = settings();
              if (s) engine.setReplayGain(mode, s.replayGainPreampDb, s.replayGainPreventClipping, s.replayGainFallbackDb);
            }}
          />

          <If when={!isIos}>
            <SegmentedStrip
              label="输出采样率策略"
              value={rateValue()}
              options={[
                { value: "native", label: "跟随源" },
                { value: "device", label: "设备默认" },
                { value: "48000", label: "48k" },
                { value: "96000", label: "96k" },
                { value: "192000", label: "192k" },
              ]}
              onPick={pickRate}
            />
            <SegmentedStrip
              label="Dither 抖动算法"
              value={currentDither()}
              options={[
                { value: "auto", label: "AUTO" },
                { value: "off", label: "OFF" },
                { value: "tpdf", label: "TPDF" },
                { value: "noise_shaped", label: "SHAPED" },
              ]}
              onPick={pickDither}
            />
            <SegmentedStrip
              label="DSD 传输策略"
              value={currentDsdPolicy()}
              options={[
                { value: "auto", label: "自动" },
                { value: "dop", label: "DoP" },
                { value: "pcm", label: "转 PCM" },
              ]}
              onPick={pickDsd}
            />
          </If>

          <View class="gap-2 pt-1">
            <Text class="font-mono text-[10px] font-bold tracking-wider" style={{ color: pal().textSecondary }}>
              TELEMETRY · 工程遥测
            </Text>
            <View class="flex-row gap-2">
              {telemetryCell("硬件时钟", path() ? formatHz(path()!.sampleRate) : "—")}
              {telemetryCell(
                "位深与抖动",
                path()
                  ? `${path()!.validBits || path()!.containerBits}-bit${path()!.dithered ? " · Dithered" : ""}`
                  : "—",
              )}
            </View>
            <View class="flex-row gap-2">
              {telemetryCell("缓冲延迟", path() ? `${path()!.bufferLatencyMs.toFixed(1)} ms` : "—")}
              {telemetryCell("下一曲队列", nextTitle())}
            </View>
          </View>
        </DeckPlate>
      </If>
    </View>
  );
}

function OutputContent() {
  const pal = () => consolePalette(colorScheme() === "dark");
  const isMobile = () => layout() === "mobile";
  const isIos = platform.os === "ios";
  const playing = () => engine.player().state === "playing";
  const peak = () => (playing() ? visualPeak() : 0);
  const peakL = () => (playing() ? visualPeakL() : null);
  const peakR = () => (playing() ? visualPeakR() : null);
  const rms = () => (playing() ? visualRms() : 0);

  return (
    <View
      class="flex-1"
      style={{
        backgroundImage: pal().chassisGradient,
        borderLeftWidth: isMobile() ? 0 : 1,
        borderLeftColor: pal().chassisBorder,
        paddingTop: isMobile() ? Math.max(12, safeArea().top + 4) : 0,
      }}
    >
      {/* 固定顶部面板标头：移动端含居中抓手 + 安全区，不随内容滚出屏幕 */}
      <View
        class="gap-2 px-4 pt-1.5 pb-2.5"
        style={{ borderBottomWidth: 1, borderBottomColor: pal().headerBorder }}
      >
        <If when={isMobile()}>
          <View class="items-center">
            <View
              class="rounded-full"
              style={{ width: 38, height: 4, backgroundColor: pal().grabber }}
              accessibilityElementsHidden
            />
          </View>
        </If>
        <View class="flex-row items-center justify-between gap-2">
          <View class="min-w-0 flex-1 gap-0.5">
            <Text class="font-mono text-[10px] font-bold tracking-[0.16em]" style={{ color: pal().accent }}>
              SIGNAL MONITOR
            </Text>
            <Text role="header" class="text-lg font-bold" style={{ color: pal().textPrimary }}>
              Output 控制台
            </Text>
          </View>
          <StatusPill />
          <View
            accessibilityLabel="关闭输出信息"
            onPress={() => setOutputOpen(false)}
            class={`items-center justify-center rounded-full active:opacity-70 ${focusRing}`}
            style={{
              width: 34,
              height: 34,
              backgroundImage: isIos ? undefined : pal().keyGradient,
              borderWidth: 1,
              borderColor: pal().keyBorder,
              ...(isIos ? { glassEffect: "regular" } : {}),
            }}
          >
            <Icon name="close" size={15} color="muted-foreground" />
          </View>
        </View>
      </View>

      <Scroll
        class="flex-1"
        contentClass="gap-2 px-4 pt-2"
        style={{
          paddingBottom: isMobile() ? Math.max(32, safeArea().bottom + 18) : 24,
        }}
      >
        <OpticalTelemetryBay peak={peak()} peakL={peakL()} peakR={peakR()} rms={rms()} />
        <BitPerfectBusCard />
        <OutputInterfaceCard />
        <DspAndTelemetryRack />
      </Scroll>
    </View>
  );
}
