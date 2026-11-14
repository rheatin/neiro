// The 输出信息 drawer, drawn as the engine's own signal chain — interactive:
// 自上而下的节点（Source → DSP stages → Output），两列蛇形铺 DSP 节点、
// 节点间连线带**箭头**（方向明确）；每个 DSP 节点可点击展开该阶段的设置
// 面板（composed on the Base 表面的 set* 命令，回写以 `SettingsInfo` 的当
// 前参数为底，只翻 enabled/mode，引擎不发明默认）；终点一枚 bit-perfect
// 金色徽章（accent 家族 tokens，点亮 spring + 光斑扫过，reduced motion 静
// 态）。术语一律英文主标签，中文只做副行小注。仅此文件画它自己的形。
//
// TSX 没有内联 SVG 元素：拟物图与徽章都走 Image + data-URI SVG（stroke
// 1.5，token上色，appearance 变更时重读），与 icons.tsx 的做法一致。

import {
  Animated,
  Easing,
  effect,
  reduceMotion,
  Scroll,
  Text,
  View,
  createAnimatedValue,
  type AnimatedValue,
  type JSX,
  signal,
  Storage,
  token,
  untracked,
  createMotion,
} from "nsh";
import { Button, Switch } from "nsh/base";
import { engine, reportEngineFailure } from "../../backend/engine";
import { trackTitle, type Settings, type Track } from "../../backend/types";
import { trackForPath } from "../../state/catalog";
import { formatRate } from "../../state/formats";
import { outputOpen, setEqOpenedFromOutput, setOutputOpen } from "../../state/overlays";
import { AppSheet } from "../ui/overlays";
import { BarSlider, focusRing } from "../ui/controls";
import { Icon } from "../ui/icons";
import { Knob } from "../ui/knob";
import { notify } from "../../state/feedback";
import { Chassis, MechKey, ModuleHeader, Silkscreen, HARDWARE } from "../ui/hardware";
import { SourceStage } from "./surface/source";
import { DspMatrix } from "./surface/dsp-matrix";
import { MasterBus } from "./surface/master-bus";
import { OutputInterface } from "./surface/output-interface";
import { TelemetryStage } from "./surface/telemetry";
import { foleyEnabled, setFoleyEnabled } from "../../state/foley";
import { visualPeak, visualPeakL, visualPeakR, visualRms } from "../../state/visualiser";

import { applyPreset, currentPreset, eqBands, eqDirty, eqEnabled, eqPresets, setEqEnabled } from "../../state/eq";
import { open } from "../../nav";
import { dragVolume, player, setVolume, volumeLevel } from "../../state/player";

/** 抽屉，open 状态归 overlays。 */
export function OutputSheet() {
  return (
    <AppSheet open={outputOpen()} onOpenChange={setOutputOpen} side="right" testID="output" popupWidth={668}>
      <OutputContent />
    </AppSheet>
  );
}

// ---- The SVG 字库（拟物 motif，stroke 1.5；与 icons.tsx 同一渲染法） ----------

const URIS = new Map<string, string>();

/** 24×24 SVG motif as a data URI, painted in `color` (a token's current
 * value; the component re-reads when the appearance changes). */
function motif(body: string, color: string, size = 24): string {
  const key = body + color + size;
  let uri = URIS.get(key);
  if (!uri) {
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24" fill="none"><g fill="none" stroke="${color}" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">${body}</g></svg>`;
    uri = `data:image/svg+xml,${encodeURIComponent(svg)}`;
    URIS.set(key, uri);
  }
  return uri;
}

/** A stage's illustration, `size` points across. */
function StageArt(props: { body: string; size?: number; color?: string }) {
  const size = props.size ?? 34;
  return (
    <Image
      src={motif(props.body, props.color ?? String(token("foreground")), size)}
      accessibilityElementsHidden
      style={{ width: size, height: size }}
    />
  );
}

// nsh 的 Image 是 host 元素，签名对 data-URI 与 spotish icons.tsx 相同。
import { Image } from "nsh";

// The motifs: 每个阶段一个硬件感的图（均为 24×24 的 path 集）。
const ART = {
  // Source：唱片 + 中心孔（file 从哪里出发）。
  source: '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="1.6"/><path d="M17.2 8.6a6 6 0 0 1 0 6.8"/>',
  // Resampler：两条错位波形重合（时间轴重采样）。
  resampler: '<path d="M3 8c2 0 2.6-2.5 4.4-2.5S9.8 8 12 8s2.6-2.5 4.4-2.5S18.8 8 21 8"/><path d="M3 16c2 0 2.6-2.5 4.4-2.5S9.8 16 12 16s2.6-2.5 4.4-2.5S18.8 16 21 16" opacity="0.55"/>',
  // Dither：阶梯量化台阶。
  dither: '<path d="M3 16h3v-3h3V8h4v8h3v-4h4v4"/><circle cx="12" cy="4.5" r="0.7"/>',
  // EQ：三根推子。
  eq: '<path d="M6 4v16M12 4v16M18 4v16"/><path d="M4.2 8h3.6M10.2 15h3.6M16.2 6h3.6"/>',
  // Crossfeed：左右耳互串（两条交叉线）。
  crossfeed: '<path d="M4 5c0 4 6 4 6 7s-6 3-6 7"/><path d="M20 5c0 4-6 4-6 7s6 3 6 7"/><circle cx="4" cy="5" r="1.4"/><circle cx="20" cy="5" r="1.4"/>',
  // ReplayGain：音量弧 + 音符。
  replaygain: '<path d="M3 10a9 9 0 0 1 18 0"/><path d="M3 10h3M18 10h3"/><circle cx="12" cy="17.5" r="2"/><path d="M13.8 16.5V8.5l4-1v6"/>',
  // Convolver：脉冲响应的尖峰波形。
  convolver: '<path d="M3 12c2 0 2.4-6 4-6s1.8 12 3.4 12 1.6-8 3.2-8 1.6 5 3.2 5 1.4-2 3.2-2"/>',
  // Crossfade：两条交叉渐变的波。
  crossfade: '<path d="M3 9c5 0 5 6 10 6s5-3 8-3"/><path d="M3 15c5 0 5-6 10-6s5 3 8 3" opacity="0.55"/><path d="M21 7v2M21 15v2"/>',
  // Output：扬声器。
  output: '<path d="M19 12a7 7 0 1 0-14 0"/>',
  // 申诉组的装饰不算独立阶段。
};

/** 一个节点的拟物图 + 名字的紧凑卡（DSP 网格里的一格）。展开面板是这张卡的
 * **内部**子区域（overflow-hidden 由卡承载），不再浮在卡外。 */
function OutputStageCard(props: {
  title: string;
  art: string;
  active: boolean;
  expanded: boolean;
  playing: boolean;
  onToggle: () => void;
  children: JSX.Element;
}) {
  const activeColor = () => (props.active ? String(token("foreground")) : String(token("muted-foreground")));
  // LED 灯效：8pt 圆点，active = accent 点亮（微光晕），bypassed = 灰
  // （bg-muted）；呼吸只在 playing。
  const led = createAnimatedValue(1);
  effect(() => {
    if (props.active && props.playing && !reduceMotion()) {
      const pulse = Animated.loop(
        Animated.sequence([
          Animated.timing(led, { toValue: 1, duration: 700, easing: "easeInOut" }),
          Animated.timing(led, { toValue: 0.45, duration: 700, easing: "easeInOut" }),
        ]),
      );
      void pulse.start();
      return () => pulse.stop();
    }
    led.set(1);
  });
  const ledDimmed = () => !(props.active && props.playing && !reduceMotion());

  return (
    <View
      class={`w-full gap-2 overflow-hidden rounded-(--radius-card) border p-2.5 transition duration-150 ease-out ${
        props.expanded ? "border-foreground/50 bg-secondary" : "border-border bg-card hover:bg-row-hover"
      } ${focusRing}`}
    >
      <Button
        accessibilityLabel={props.title}
        selected={props.expanded}
        onPress={props.onToggle}
        class={`flex-row items-center gap-2 active:scale-[0.98] motion-reduce:active:scale-100`}
      >
        <View style={props.active ? undefined : { opacity: 0.6 }}>
          <StageArt body={props.art} size={30} color={activeColor()} />
        </View>
        <View class="min-w-0 gap-0.5">
          <Text class={`line-clamp-1 text-sm font-semibold ${props.active ? "text-foreground" : "text-muted-foreground"}`}>
            {props.title}
          </Text>
          <Text class={`text-[10px] text-muted-foreground`}>{props.active ? "active" : "bypassed"}</Text>
        </View>
        <View class="flex-1" />
        {/* LED: 右上角。 */}
        <View
          accessibilityElementsHidden
          class="mr-2 size-2 rounded-full"
          style={{
            backgroundColor: props.active ? String(token("primary")) : String(token("muted")),
            // 呼吸只在 playing；否则静止（DRY：binding 的地方直接引用 value node）。
            opacity: ledDimmed() ? (props.active ? 1 : 0.5) : led,
            boxShadow: props.active ? `0 0 6 ${withAlpha(String(token("primary")), 0.6)}` : "0 0 0 transparent",
          }}
        />
      </Button>
      {/* 面板在卡内的缩放宽块：breite 对齐卡内 padding。 */}
      <If when={props.expanded}>
        <View class="rounded-(--radius) bg-muted/50 p-2">{props.children}</View>
      </If>
    </View>
  );
}

// ---- 基件 -------------------------------------------------------------------

function Fact(props: { label: string; value: string; note?: string }) {
  return (
    <View class="flex-row items-baseline gap-2">
      <Text class="min-w-32 shrink-0 font-mono text-xs text-muted-foreground">{props.label}</Text>
      <View class="min-w-0 flex-1 flex-row items-baseline justify-between gap-2">
        <Text class="min-w-0 flex-1 font-mono text-xs text-foreground">{props.value}</Text>
        <If when={props.note}>
          <Text class="shrink-0 text-[10px] text-muted-foreground">{props.note}</Text>
        </If>
      </View>
    </View>
  );
}

function EmptyNote(props: { text: string }) {
  return <Text class="text-xs text-muted-foreground">{props.text}</Text>;
}

function SectionHeader(props: { title: string; subtitle: string }) {
  return (
    <View class="gap-0.5">
      <Text role="header" class="text-[11px] font-semibold uppercase tracking-wider text-foreground">
        {props.title}
      </Text>
      <Text class="text-[10px] text-muted-foreground">{props.subtitle}</Text>
    </View>
  );
}

// ---- Connector：竖线 + 箭头头部（方向向下），播放中一个移动的流动点 ----------

function FlowDot() {
  const value = createAnimatedValue(-7);
  effect(() => {
    if (reduceMotion()) return;
    const running = Animated.loop(
      Animated.sequence([
        Animated.timing(value, { toValue: 7, duration: 1100, easing: "easeInOut" }),
        Animated.timing(value, { toValue: -7, duration: 1100, easing: "easeInOut" })
      ]),
    );
    void running.start();
    return () => running.stop();
  });
  return (
    <View
      accessibilityElementsHidden
      class="absolute size-1.5 rounded-full bg-primary"
      style={{ transform: [{ translateY: value }] }}
    />
  );
}

function Connector() {
  return (
    <View class="w-full flex-row justify-center" style={{ height: 30 }}>
      <View class="absolute h-full w-full items-center">
        <View class="w-[2px] rounded-full bg-border" style={{ height: 18 }} />
        {/* 三角箭头，accent 色 —— "从上往下"一眼可读。 */}
        <StageArt body={'<path d="M8 8l4 6 4-6z"/><path d="M8 8h8"/>'} size={17} color={String(token("primary"))} />
      </View>
      <FlowDot />
    </View>
  );
}

/** 水平方向的短线 + 右箭头（蛇形行内同级间）。 */
function HorizontalArrow(props: { toLeft?: boolean }) {
  // accent 三角 + 短线：蛇形行内"向右/向左"都一眼可读（head 里 `@` 是
  // motif() 的上色占位)。
  const tail = '<path d="M17 9H8"/><path d="M10.5 5.5L13.5 9l-3 3.5z" fill="currentColor" stroke="currentColor"/>';
  return (
    <View class="items-center justify-center px-0.5" style={{ paddingTop: 22 }}>
      <StageArt body={props.toLeft ? '<path d="M17 9H8"/><path d="M10.5 5.5L13.5 9l-3 3.5z" fill="currentColor" stroke="currentColor"/>' : '<path d="M7 9h9"/><path d="M13.5 5.5L16.5 9l-3 3.5z"/>'} size={17} color={String(token("primary"))} />
    </View>
  );
}

// ---- resolved（引擎的"为什么"）----------------------------------------------

function resolveData() {
  const json = engine.player().resolvedJson;
  if (!json) return undefined;
  try {
    return JSON.parse(json) as {
      deviceId?: string;
      deviceName?: string;
      bitPerfect?: boolean;
      coerced?: boolean;
      activeStages?: number;
      padFrameCount?: number;
      maxBufferGapMs?: number;
      underrunCount?: number;
      dropCount?: number;
      deadlineMissCount?: number;
      devicePeak?: number;
      bufferLatencyMs?: number;
      diagnostics?: { code?: number; severity?: number; message?: string }[];
    };
  } catch {
    return undefined;
  }
}

/** 一行事实（mono 英文），值缺画 —。 */
function SourceFacts() {
  const file = () => trackForPath(player().path ?? "");
  const path = engine.renderPath;
  return (
    <If
      when={path()}
      else={<EmptyNote text="No stream. 播放一条曲目，这条链从源到端点亮起来。" />}
    >
      {(path) => {
        const text = (): string =>
          file()?.title ? trackTitle(file()) : (player().path ?? "").replace(/\\/g, "/").split("/").pop() || "—";
        return (
          <View class="gap-1">
            <Fact label="file" value={text()} />
            <Fact
              label="sampleRate / channels"
              value={
                path().sourceRate > 0
                  ? `${formatRate(path().sourceRate)} · ${path().channels > 0 ? path().channels + " ch" : "—"}`
                  : "—"
              }
            />
            <Fact
              label="codec / bit depth"
              value={
                path().bitsPerSample > 0
                  ? `${path().validBits || path().bitsPerSample} / ${path().bitsPerSample}${path().isFloat ? " float" : ""}`
                  : "—"
              }
            />
          </View>
        );
      }}
    </If>
  );
}

// ---- 展开面板的小组件 ---------------------------------------------------------

function ToggleRow(props: { label: string; on: boolean; note?: string; onToggle: () => void }) {
  return (
    <View class="min-w-0 flex-row items-center gap-2 py-0.5">
      <Text class="min-w-0 flex-1 text-xs font-medium text-foreground">{props.label}</Text>
      <If when={props.note}>
        <Text class="text-[10px] text-muted-foreground">{props.note}</Text>
      </If>
      <Switch
        checked={props.on}
        accessibilityLabel={props.label}
        onChange={() => props.onToggle()}
        trackClass="h-5 w-9 shrink-0 justify-center rounded-full bg-input transition duration-150 ease-out checked:bg-primary"
        thumbClass="size-4 rounded-full bg-background shadow checked:translate-x-4 transition duration-150 ease-out"
      />
    </View>
  );
}

/** 参数回显行（mono 小字，紧贴控件行下）。 */
function NoteRow(props: { text: string }) {
  return (
    <Text class="font-mono text-[10px] leading-relaxed text-muted-foreground">{props.text}</Text>
  );
}

/** 一个 DSP 展开面板的统一外壳。 */
// ---- Source -----------------------------------------------------------------

// ---- Resampler（= 策略三选收进来 + setPolicy） --------------------------------

type PolicyKind = "native" | "device" | "fixed";
const chosenPolicy = signal<PolicyKind | null>(null);
/** WASAPI share mode. `null` = 未选过 → 默认 shared（用户要求；独占会与
 * 浏览器/会议软件互斥，桌面默认共享更友好）。 */
type ShareKind = "exclusive" | "shared";
const chosenShare = signal<ShareKind | null>(null);
const POLICY_KEY = "ui:policy";
let policyRestored = false;

/** The applied share mode: the stored choice, shared by default. */
const shareKind = (): ShareKind => chosenShare() ?? "shared";

function policyJson(rate: PolicyKind, share: ShareKind): string {
  const ratePart =
    rate === "fixed" ? '{ "mode": "fixed", "rate_hz": 96000 }' : `{ "mode": "${rate}" }`;
  return `{ "rate": ${ratePart}, "quality": "very_high", "share": "${share}" }`;
}

const POLICY_LABEL: Record<PolicyKind, string> = { native: "native", device: "device", fixed: "fixed 96k" };
const SHARE_LABEL: Record<ShareKind, string> = { shared: "shared", exclusive: "exclusive" };

function ResamplerPanel() {
  effect(() => {
    if (engine.health().status !== "ready" || policyRestored) return;
    policyRestored = true;
    void (async () => {
      try {
        const saved = await Storage.getItem(POLICY_KEY);
        if (saved === "native" || saved === "device" || saved === "fixed") {
          // 旧格式（只有 rate 的字符串）：迁到 shared 默认。
          chosenPolicy.set(saved);
          engine.setPolicy(policyJson(saved, "shared"));
        } else if (saved) {
          const parsed: unknown = JSON.parse(saved);
          const rate = (parsed as { rate?: string })?.rate;
          const share = (parsed as { share?: string })?.share;
          const nextRate: PolicyKind = rate === "native" || rate === "device" || rate === "fixed" ? rate : "device";
          const nextShare: ShareKind = share === "exclusive" ? "exclusive" : "shared";
          chosenPolicy.set(nextRate);
          chosenShare.set(nextShare);
          engine.setPolicy(policyJson(nextRate, nextShare));
        }
      } catch (error) {
        console.warn("neiro: 输出策略没有读回来:", String(error));
      }
    })();
  });
  return (
    <View class="gap-1.5">
      <Text class="text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">Policy</Text>
      <Text class="text-[10px] text-muted-foreground">输出策略（native = 源速率；device = 端点授予；fixed = 指定 96000）</Text>
      <View class="flex-row gap-1">
        <Each of={["native", "device", "fixed"] as PolicyKind[]}>
          {(kind) => (
            <Button
              selected={chosenPolicy() === kind()}
              onPress={() => {
                chosenPolicy.set(kind());
                try {
                  engine.setPolicy(policyJson(kind(), shareKind()));
                  void (async () => {
                    try {
                      await Storage.setItem(POLICY_KEY, JSON.stringify({ rate: kind(), share: shareKind() }));
                    } catch (error) {
                      console.warn("neiro: 输出策略没有记住:", String(error));
                    }
                  })();
                } catch (error) {
                  reportEngineFailure(error, "切换策略失败");
                  chosenPolicy.set(null);
                }
              }}
              class={`min-w-0 flex-1 rounded-(--radius) px-2 py-1.5 transition duration-150 ease-out ${
                chosenPolicy() === kind() ? "bg-secondary font-semibold text-foreground" : "hover:bg-row-hover text-foreground opacity-80"
              } ${focusRing}`}
            >
              <Text class="text-center text-xs">{POLICY_LABEL[kind()]}</Text>
            </Button>
          )}
        </Each>
      </View>
      {/* Share mode: since Base reopens the stream on a policy change, this
          applies immediately. shared 是默认（用户要求；独占会和浏览器/会议
          软件互斥）。shared 下 bit-perfect 永远 false — 引擎语义。 */}
      <View class="mt-1 flex-row items-center gap-1">
        <Text class="w-10 text-[10px] text-muted-foreground">share</Text>
        <Each of={["shared", "exclusive"] as ShareKind[]}>
          {(kind) => (
            <Button
              selected={shareKind() === kind()}
              onPress={() => {
                chosenShare.set(kind());
                const rate = chosenPolicy() ?? "device";
                try {
                  engine.setPolicy(policyJson(rate, kind()));
                  void Storage.setItem(POLICY_KEY, JSON.stringify({ rate, share: kind() })).catch((error) =>
                    console.warn("neiro: 共享模式没有记住:", String(error)),
                  );
                } catch (error) {
                  reportEngineFailure(error, "切换共享模式失败");
                  chosenShare.set(null);
                }
              }}
              class={`min-w-0 flex-1 rounded-(--radius) px-2 py-1.5 transition duration-150 ease-out ${
                shareKind() === kind() ? "bg-secondary font-semibold text-foreground" : "hover:bg-row-hover text-foreground opacity-80"
              } ${focusRing}`}
            >
              <Text class="text-center text-xs">{SHARE_LABEL[kind()]}</Text>
            </Button>
          )}
        </Each>
      </View>
      <Text class="text-[10px] text-muted-foreground">
        shared 下引擎报告 bit-perfect = false（混音器在链路上，不是 bug）；exclusive 需要端点上没有其他程序占用。
      </Text>
    </View>
  );
}

// ---- Dither（只读，引擎自动；诚实说明） ----------------------------------------

function DitherPanel() {
  return (
    <View class="gap-1.5">
      <Text class="text-xs leading-relaxed text-foreground">Dither 由引擎自动应用（抖着重采样后更高位的输出）。没有用户开关。</Text>
      <If when={engine.renderPath()}>
        {(path) => (
          <Fact
            label="bit depth"
            value={path().validBits || path().bitsPerSample > 0 ? `${path().validBits || path().bitsPerSample} / ${path().bitsPerSample}${path().isFloat ? " float" : ""}` : "—"}
          />
        )}
      </If>
    </View>
  );
}

// ---- EQ（抽屉里只切预设；改频段在独立页面） --------------------------------

/** The drawer's EQ stage: state and presets only. The bands themselves live
 * on the 均衡器 page — a 10-band fader row does not fit a 574pt drawer, and
 * two editors for one value would drift. */
function EqPanel() {
  const preset = () => currentPreset();
  return (
    <View class="gap-2">
      <View class="flex-row items-center gap-2">
        <View class="min-w-0 flex-1">
          <ToggleRow
            label="Equalizer"
            note={eqDirty() ? "已改动" : preset() ?? `${eqBands().length} bands`}
            on={eqEnabled()}
            onToggle={() => setEqEnabled(!eqEnabled())}
          />
        </View>
        <Button
          accessibilityLabel="打开均衡器设置"
          onPress={() => {
            setEqOpenedFromOutput(true);
            setOutputOpen(false);
            open({ name: "eq" });
          }}
          class={`shrink-0 flex-row items-center gap-1 rounded-(--radius) px-2 py-1 hover:bg-row-hover active:bg-muted ${focusRing}`}
        >
          <Text class="text-[10px] text-muted-foreground">设置</Text>
          <Icon name="chevronRight" size={11} color="muted-foreground" />
        </Button>
      </View>
      <Scroll horizontal scrollbar={false} class="w-full" contentClass="flex-row gap-1 pb-1">
        <Each of={eqPresets()} key={(p) => p.name}>
          {(item) => (
            <Button
              accessibilityLabel={`预设 ${item().name}`}
              selected={preset() === item().name}
              onPress={() => applyPreset(item().name)}
              class={`h-7 rounded-(--radius) border px-2.5 transition duration-150 ease-out ${
                preset() === item().name ? "border-primary/60 bg-accent" : "border-border hover:bg-row-hover"
              } ${focusRing}`}
            >
              <Text class={`text-xs ${preset() === item().name ? "font-semibold text-accent-foreground" : "text-foreground"}`}>
                {item().name}
              </Text>
            </Button>
          )}
        </Each>
      </Scroll>
      <NoteRow text="预设在这里切换；频段、预增益与另存为在新的「均衡器」页面。" />
    </View>
  );
}

// ---- Crossfeed（开关；参数原样） ------------------------------------------------

function CrossfeedPanel() {
  const settings = engine.settings;
  const before = settings();
  return (
    <View class="gap-2">
      <ToggleRow
        label="Crossfeed on"
        on={before?.crossfeedEnabled ?? false}
        onToggle={() => {
          if (!before) return;
          try {
            engine.setCrossfeed(!before.crossfeedEnabled, before.crossfeedAmount, before.crossfeedDelayMs, before.crossfeedCutoffHz);
          } catch (error) {
            reportEngineFailure(error, "Crossfeed 切换失败");
          }
        }}
      />
      <NoteRow
        text={
          before?.crossfeedEnabled
            ? `amount ${before.crossfeedAmount.toFixed(2)} · delay ${before.crossfeedDelayMs} ms · cutoff ${Math.round(before.crossfeedCutoffHz)} Hz（以引擎当前值原样回传）`
            : "只翻开关；amount/delay/cutoff 以引擎当前值原样回传。"
        }
      />
    </View>
  );
}

// ---- ReplayGain（模式三选 off/track/album；其余原样） ---------------------------

const RG_LABEL = ["off", "track", "album"] as const;

function ReplayGainPanel() {
  const settings = engine.settings;
  return (
    <View class="gap-1.5">
      <Text class="text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">mode</Text>
      <View class="flex-row gap-1">
        <Each of={[0, 1, 2]}>
          {(mode) => (
            <Button
              selected={settings()?.replayGainMode === mode()}
              onPress={() => {
                const before = settings();
                if (!before) return;
                try {
                  engine.setReplayGain(mode(), before.replayGainPreampDb, before.replayGainPreventClipping, before.replayGainFallbackDb);
                } catch (error) {
                  reportEngineFailure(error, "ReplayGain 模式切换失败");
                }
              }}
              class={`flex-1 rounded-(--radius) px-2 py-1.5 transition duration-150 ease-out ${
                settings()?.replayGainMode === mode() ? "bg-secondary font-semibold text-foreground" : "hover:bg-row-hover text-foreground opacity-80"
              } ${focusRing}`}
            >
              <Text class="text-center text-xs">{RG_LABEL[mode()]}</Text>
            </Button>
          )}
        </Each>
      </View>
      <Fact
        label="preventClipping"
        value={settings()?.replayGainPreventClipping ? "on" : "off"}
      />
    </View>
  );
}

// ---- Convolver（开关；无 IR 不假装能加载） --------------------------------------

function ConvolverPanel() {
  const settings = engine.settings;
  const hasIr = () => !!settings()?.convolverIrPath;
  return (
    <View class="gap-1.5">
      <ToggleRow
        label="Convolver on"
        note={settings()?.convolverEnabled ? `gain ${settings()?.convolverGainDb.toFixed(1)} dB` : undefined}
        on={settings()?.convolverEnabled ?? false}
        onToggle={() => {
          const before = settings();
          if (!before) return;
          try {
            engine.setConvolver(
              !before.convolverEnabled,
              before.convolverIrPath,
              before.convolverWet,
              before.convolverDry,
              before.convolverGainDb,
              before.convolverNormalize,
            );
          } catch (error) {
            reportEngineFailure(error, "Convolver 切换失败");
          }
        }}
      />
      <If
        when={settings()?.convolverIrPath}
        else={
          <Fact label="ir" value="—" note="需要 IR 文件；不发明路径，加载留给引擎" />
        }
      >
        {(ir) => <Fact label="ir path" value={ir()} note={settings()?.convolverNormalize ? "normalized" : undefined} />}
      </If>
    </View>
  );
}

// ---- Crossfade（开关 + 时长回显；curve 原样） -----------------------------------

const CROSSFADE_CURVE = ["linear", "equal power"] as const;

function CrossfadePanel() {
  const settings = engine.settings;
  return (
    <View class="gap-1.5">
      <ToggleRow
        label="Crossfade on"
        note={(() => {
          const before = settings();
          return before?.crossfadeEnabled ? `${(before.crossfadeDurationMs / 1000).toFixed(1)} s · ${CROSSFADE_CURVE[before.crossfadeCurve || 0]}` : undefined;
        })()}
        on={settings()?.crossfadeEnabled ?? false}
        onToggle={() => {
          const before = settings();
          if (!before) return;
          try {
            engine.setCrossfade(!before.crossfadeEnabled, before.crossfadeDurationMs, before.crossfadeCurve);
          } catch (error) {
            reportEngineFailure(error, "交叉淡入切换失败");
          }
        }}
      />
      <Fact
        label="duration / curve"
        value={settings()?.crossfadeEnabled ? `${settings()?.crossfadeDurationMs} ms · ${CROSSFADE_CURVE[settings()?.crossfadeCurve || 0]}` : "—"}
      />
    </View>
  );
}

// ---- Output Device（设备行 + 音量） ----------------------------------------------

const chosen = signal<string | null | undefined>(undefined);

/** Whether a stream is open: enumerating endpoints opens each one in turn,
 * and an exclusive stream loses its device to that (the engine then reports
 * `deviceLost`). The list is a fact about the machine, not about the stream,
 * so a live stream keeps the list it already has. */
function streamLive(): boolean {
  const state = engine.player().state;
  return state === "playing" || state === "paused" || state === "ready";
}

function OutputStage() {
  effect(() => {
    if (!outputOpen()) return;
    if (streamLive()) return;
    untracked(() => {
      try {
        engine.listDevices();
      } catch (error) {
        reportEngineFailure(error, "枚举输出设备失败");
      }
    });
  });
  const devices = engine.devices;
  const current = () => (chosen() === undefined ? null : chosen());
  const opened = () => resolveData()?.deviceId ?? "";
  const vol = () => volumeLevel();
  return (
    <View class="rounded-(--radius-card) border border-border bg-card p-3">
      <View class="flex-row items-start gap-2.5">
        <StageArt body={ART.output} size={30} color={String(token("foreground"))} />
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class="text-[10px] text-muted-foreground">点谁谁亮（独占 / 共享按端点的能力）；音量沿用播放条的滑条</Text>
          <View class="mt-1 gap-1">
            <DeviceRow name="引擎默认" detail="上次使用的设备" selected={current() === null} onSelect={() => void chooseDevice(null)} />
            <Each of={devices()}>
              {(device) => (
                <DeviceRow
                  name={device().name || device().id}
                  detail={`${formatRate(device().sampleRate)} · ${device().bitsPerSample} bit · ${device().channels} ch${device().exclusive ? " · exclusive" : device().shared ? " · shared" : ""}`}
                  selected={opened() ? opened() === device().id : current() === device().id}
                  onSelect={() => void chooseDevice(device().id)}
                />
              )}
            </Each>
            <If when={resolveData()?.deviceName}>
              <Text class="pt-1 text-[11px] text-muted-foreground">{`当前开着:${resolveData()?.deviceName ?? "—"}`}</Text>
            </If>
            <View class="mt-1 flex-row items-center gap-1">
              <Button
                accessibilityLabel={streamLive() ? "刷新设备（需先停止播放）" : "刷新设备"}
                disabled={streamLive()}
                onPress={() => {
                  try {
                    engine.listDevices();
                  } catch (error) {
                    reportEngineFailure(error, "枚举输出设备失败");
                  }
                }}
                class={`flex-row items-center gap-1 rounded-(--radius) px-1.5 py-1 transition duration-150 ease-out hover:bg-row-hover active:opacity-60 disabled:opacity-40 ${focusRing}`}
              >
                <Icon name="refresh" size={12} color="muted-foreground" />
                <Text class="text-[11px] text-muted-foreground">刷新设备</Text>
              </Button>
              <If when={streamLive()}>
                <Text class="text-[10px] text-muted-foreground">播放中不枚举：枚举会打开端点，独占流会被抢走</Text>
              </If>
            </View>
          </View>
          <If when={vol() !== null}>
            <View class="mt-2 flex-row items-center gap-2">
              <Icon name="volume" size={14} color="muted-foreground" />
              <BarSlider
                label="音量"
                value={vol() ?? 0}
                max={1}
                class="flex-1"
                onChange={dragVolume}
                onChangeEnd={(v) => setVolume(v)}
              />
              <Text class="w-9 text-right font-mono text-[10px] text-muted-foreground">{((vol() ?? 0) * 100).toFixed(0)}%</Text>
            </View>
          </If>
        </View>
      </View>
    </View>
  );
}

async function chooseDevice(deviceId: string | null): Promise<void> {
  try {
    engine.setDevice(deviceId);
    chosen.set(deviceId);
  } catch (error) {
    reportEngineFailure(error, "切换设备失败");
  }
}

function DeviceRow(props: { name: string; detail: string; selected: boolean; onSelect: () => void }) {
  return (
    <Button
      accessibilityLabel={props.name}
      selected={props.selected}
      onPress={props.onSelect}
      class={`flex-row items-center gap-2 rounded-(--radius) px-2 py-1.5 hover:bg-row-hover active:bg-muted ${focusRing}`}
    >
      <View class="w-4 items-center">
        <If when={props.selected}>
          <Icon name="check" size={14} color="foreground" />
        </If>
      </View>
      <View class="min-w-0 flex-1 gap-0.5">
        <Text class="line-clamp-1 text-sm text-foreground">{props.name}</Text>
        <Text class="line-clamp-1 text-[11px] text-muted-foreground">{props.detail}</Text>
      </View>
    </Button>
  );
}

// ---- 终点：Bit-perfect 徽章 -----------------------------------------------------

/** 金色徽章（accent 家族 tokens）。点亮 spring + 光斑扫过；reduced motion
 * 一次性静态显示（hover 用户不需要 fireworks）。 */
function BitPerfectMedal() {
  const bitPerfect = () => engine.renderPath()?.bitPerfect ?? false;
  const gold = String(token("primary"));
  const goldSoft = String(token("chart-3"));
  const medalEase: [number, number, number, number] = [0.34, 1.56, 0.64, 1];
  const medalMotion = createMotion({
    enter: {
      keyframes: { scale: [0.88, 1.08, 1], opacity: [0.5, 1] },
      duration: 520,
      easing: medalEase,
    },
  });
  const sweep = createAnimatedValue(-160);
  effect(() => {
    if (reduceMotion() || !bitPerfect()) return;
    sweep.set(-160);
    const sweepRun = Animated.timing(sweep, { toValue: 240, duration: 900, easing: "easeOut" });
    void sweepRun.start();
    return () => sweepRun.stop();
  });
  const POP_MEDAL = {
    keyframes: { scale: [0.88, 1.08, 1], opacity: [0.5, 1] },
    duration: 520,
    easing: medalEase,
  };
  let was = false;
  effect(() => {
    const on = bitPerfect();
    if (on && !was && !reduceMotion()) {
      untracked(() => void medalMotion.play(POP_MEDAL));
    }
    was = on;
  });
  return (
    <View class="w-full items-center gap-1 rounded-(--radius-card) border border-border bg-card p-3">
      <View class="overflow-hidden rounded-(--radius-card)">
        <View class="items-center gap-2 px-2 py-3" style={medalMotion.style}>
          <View
            class="relative items-center justify-center rounded-full border"
            style={{
              width: 116,
              height: 116,
              borderColor: bitPerfect() ? gold : String(token("input")),
              backgroundColor: bitPerfect() ? withAlpha(gold, 0.12) : withAlpha(String(token("foreground")), 0.05),
              boxShadow: bitPerfect() ? `0 0 26 ${withAlpha(goldSoft, 0.45)}` : "0 0 0 transparent",
            }}
          >
            <StageArt
              body={
                bitPerfect()
                  ? '<circle cx="12" cy="12" r="9"/><path d="M8.5 12.2l2.6 2.6 4.6-5.4"/>'
                  : '<circle cx="12" cy="12" r="9"/><path d="M9 9l6 6M15 9l-6 6"/>'
              }
              size={44}
              color={bitPerfect() ? gold : String(token("muted-foreground"))}
            />
            {/* 光斑扫过（一次性，点亮的那拍）：挂在与徽章同一边界里的裁剪层。 */}
            <If when={bitPerfect() && !reduceMotion()}>
              <View
                accessibilityElementsHidden
                class="absolute inset-0"
                style={{ transform: [{ translateX: sweep }] }}
              >
                <View
                  class="h-full w-8 rounded-full"
                  style={{ backgroundColor: withAlpha(goldSoft, 0.5), marginTop: 0, marginLeft: 0, height: 116, width: 10, left: 53 }}
                />
              </View>
            </If>
          </View>
          <View class="items-center gap-0.5">
            <Text class={`text-sm font-extrabold ${bitPerfect() ? "text-foreground" : "text-muted-foreground"}`}>
              bit-perfect {bitPerfect() ? "· lit" : "· off"}
            </Text>
            <If when={reasonText()}>
              <Text class="max-w-md text-center text-[10px] text-muted-foreground">{reasonText()}</Text>
            </If>
          </View>
        </View>
      </View>
    </View>
  );
}

function reasonText(): string | undefined {
  const d = resolveData();
  return d?.diagnostics?.filter((row) => row.message).map((row) => row.message).pop();
}

/** Add alpha to a theme token's colour. Tokens arrive as hex or as
 * `rgb()`/`rgba()`; an unparsed colour keeps its value (never a made-up
 * hex), which is why the medal's off state must not rely on this for its
 * only wash. */
function withAlpha(color: string, a: number): string {
  const text = color.trim();
  // A theme token arrives as #rrggbbaa: its own alpha is replaced.
  const hex8 = /^#([0-9a-f]{6})[0-9a-f]{2}$/i.exec(text);
  if (hex8) {
    return `#${hex8[1]}${Math.round(a * 255).toString(16).padStart(2, "0")}`;
  }
  const hex = /^#([0-9a-f]{6})$/i.exec(text);
  if (hex) {
    return `#${hex[1]}${Math.round(a * 255).toString(16).padStart(2, "0")}`;
  }
  const rgb = /^rgba?\(\s*(\d+)[,\s]+(\d+)[,\s]+(\d+)/i.exec(text);
  if (rgb) {
    return `rgba(${rgb[1]}, ${rgb[2]}, ${rgb[3]}, ${a})`;
  }
  return text;
}

// ---- Diagnostics（术语英文；播放中 1–2s 一跳） ------------------------------------

function useDiagTick(): () => number {
  const tick = signal(0);
  effect(() => {
    if (!outputOpen() || engine.player().state !== "playing") return;
    const interval = setInterval(() => tick.update((n) => n + 1), 1500);
    return () => clearInterval(interval);
  });
  return () => tick();
}

function DiagnosticsStage() {
  const tick = useDiagTick();
  const playing = engine.player().state === "playing";
  const d = (tick(), resolveData());
  const rows = d
    ? [
        { label: "padFrameCount", value: d.padFrameCount },
        { label: "maxBufferGapMs", value: d.maxBufferGapMs },
        { label: "underrunCount", value: d.underrunCount },
        { label: "dropCount", value: d.dropCount },
        { label: "deadlineMissCount", value: d.deadlineMissCount },
        { label: "devicePeak", value: d.devicePeak },
        { label: "bufferLatencyMs", value: d.bufferLatencyMs },
      ]
    : [];
  const counts = rows.slice(0, 6);
  const allZero = counts.every((r) => typeof r.value === "number" && r.value === 0);
  // 引擎的 resolved 只在报的时候才有这些计数；没有就不画（画 — 一排是假
  // 象——那不是"零"，而是"本条流不报告"）。
  const present = counts.filter((row) => typeof row.value === "number");
  return (
    <View class="rounded-(--radius-card) border border-border bg-card p-3">
      <View class="gap-0.5">
        <Text class="text-[10px] text-muted-foreground">播放中才轮询；暂停后的 0 不画（不是事实）。</Text>
      </View>
      <View class="mt-2">
        <If
          when={playing && d && counts.length > 0}
          else={
            <EmptyNote text="播放中才轮询；暂停时引擎重置这些计数，读到的 0 不是事实。" />
          }
        >
          <View class="gap-1">
            <If when={present.length > 0} else={<EmptyNote text="引擎没有在本条流的 resolved 里报告这些计数（不是零，是不报告）。" />}>
              <View class="gap-1">
                <Each of={present}>
                  {(row) => <Fact label={row().label} value={short(row().value, row().label === "maxBufferGapMs" || row().label === "bufferLatencyMs" ? " ms" : "")} />}
                </Each>
                <If when={allZero && present.length === counts.length}>
                  <Text class="text-xs text-foreground">设备计数全零（无受损帧）。</Text>
                </If>
              </View>
            </If>
            <If when={d?.diagnostics && d.diagnostics.length > 0}>
              <View class="mt-2 gap-1 border-t border-border/60 pt-2">
                <Each of={d?.diagnostics ?? []}>
                  {(diag) => <Fact label={`diag ${diag().code ?? "??"}`} value={diag().message ?? "—"} />}
                </Each>
              </View>
            </If>
          </View>
        </If>
      </View>
    </View>
  );
}

function short(n: unknown, suffix = ""): string {
  return typeof n === "number" && Number.isFinite(n) ? `${n}${suffix}` : "—";
}

// ---- Queue -------------------------------------------------------------------

function QueueStage() {
  const queue = engine.queue;
  return (
    <View class="rounded-(--radius-card) border border-border bg-card p-3">
      <View class="gap-0.5">
        <Text class="text-[10px] text-muted-foreground">播放队列（引擎管理）</Text>
      </View>
      <View class="mt-2">
        <If when={queue().queue.length > 0} else={<EmptyNote text="队列是空的。播放一条曲目，引擎把它放进队列。" />}>
          {() => (
            <View class="gap-1">
              <Each of={queue().queue}>
                {(path, index) => {
                  const isCurrent = queue().index === index();
                  return (
                    <View class={`flex-row items-center gap-2 rounded-(--radius) px-2 py-1.5 ${isCurrent ? "bg-accent" : "hover:bg-row-hover"}`}>
                      <Text class="w-4 text-right font-mono text-[11px] text-muted-foreground">{index() + 1}</Text>
                      <Text class={`min-w-0 flex-1 line-clamp-1 text-xs ${isCurrent ? "font-semibold text-foreground" : "text-foreground"}`}>
                        {titleFor(path())}
                      </Text>
                      <If when={isCurrent}>
                        <View class="ml-1 size-1.5 rounded-full bg-primary" />
                      </If>
                    </View>
                  );
                }}
              </Each>
            </View>
          )}
        </If>
      </View>
    </View>
  );
}

/** A queue path's spoken name: the index's title where the file has one, then
 * the playing track's own title, then the file's name. */
function titleFor(path: string): string {
  const fromIndex = trackForPath(path);
  if (fromIndex?.title) return trackTitle(fromIndex);
  if (player().path === path && player().title) return player().title;
  return path.replace(/\\/g, "/").split("/").pop() || path;
}

// ---- 抽屉的根（蛇形两列的 DSP 网格在 Processing Chain 组内） -----------------------

/** 展开面板一次最多一个（点击另一节点直接移过去）。 */
const openStage = signal<string | null>(null);

function toggleStage(key: string) {
  openStage.update((was) => (was === key ? null : key));
}

// The DSP stages, in the chain's own order. Each is one tile of the
// controller grid: LED, engraved name, its parameter on a knob. A stage
// with nothing continuous to turn (Resampler's three-mode policy, Dither's
// engine-side choice) draws a still knob: a knob that turns something the
// engine does not have would be a lie.
const DSP_STAGES: {
  key: string;
  title: string;
  art: string;
  panel: () => JSX.Element;
  active: () => boolean;
  /** The knob: normalized 0…1 ⇄ the engine's own unit. `null`: not
   * turnable. */
  knob: {
    label: string;
    /** What the readout says for a normalized position. */
    readout: (value: number) => string;
    /** A small caption under the value (unit, or the mode). */
    note?: () => string;
    /** Where the knob sits now. */
    value: () => number;
    /** Turned: `commit` is the release. */
    set: (value: number, commit: boolean) => void;
    /** Turned from off: the stage takes effect (the MIDI-controller feel:
     * a knob moved on a strip engages that strip). */
    engage: () => void;
    enabled: () => boolean;
  } | null;
}[] = [
  {
    key: "resampler",
    title: "Resampler",
    art: ART.resampler,
    panel: ResamplerPanel,
    active: () => engine.renderPath()?.resampling ?? false,
    knob: null,
  },
  {
    key: "dither",
    title: "Dither",
    art: ART.dither,
    panel: DitherPanel,
    active: () => engine.renderPath()?.dithered ?? false,
    knob: null,
  },
  {
    key: "eq",
    title: "EQ",
    art: ART.eq,
    panel: EqPanel,
    active: () => engine.settings()?.eqEnabled ?? false,
    knob: {
      label: "EQ 预增益",
      readout: (v) => `${(v * 24 - 12).toFixed(1)}`,
      note: () => "dB preamp",
      value: () => ((engine.settings()?.eqPreampDb ?? 0) + 12) / 24,
      set: (v, commit) => {
        const settings = engine.settings();
        if (!settings) return;
        try {
          engine.setEq(settings.eqEnabled || commit, v * 24 - 12, settings.eqBands);
        } catch (error) {
          reportEngineFailure(error, "EQ 预增益下发失败");
        }
      },
      engage: () => {
        const settings = engine.settings();
        if (!settings || settings.eqEnabled) return;
        try {
          engine.setEq(true, settings.eqPreampDb, settings.eqBands);
        } catch (error) {
          reportEngineFailure(error, "EQ 打开失败");
        }
      },
      enabled: () => engine.settings()?.eqEnabled ?? false,
    },
  },
  {
    key: "crossfeed",
    title: "Crossfeed",
    art: ART.crossfeed,
    panel: CrossfeedPanel,
    active: () => engine.settings()?.crossfeedEnabled ?? false,
    knob: {
      label: "Crossfeed 量",
      readout: (v) => v.toFixed(2),
      note: () => "amount",
      value: () => engine.settings()?.crossfeedAmount ?? 0,
      set: (v, commit) => {
        const settings = engine.settings();
        if (!settings) return;
        try {
          engine.setCrossfeed(settings.crossfeedEnabled || commit, v, settings.crossfeedDelayMs, settings.crossfeedCutoffHz);
        } catch (error) {
          reportEngineFailure(error, "Crossfeed 下发失败");
        }
      },
      engage: () => {
        const settings = engine.settings();
        if (!settings || settings.crossfeedEnabled) return;
        try {
          engine.setCrossfeed(true, settings.crossfeedAmount, settings.crossfeedDelayMs, settings.crossfeedCutoffHz);
        } catch (error) {
          reportEngineFailure(error, "Crossfeed 打开失败");
        }
      },
      enabled: () => engine.settings()?.crossfeedEnabled ?? false,
    },
  },
  {
    key: "replaygain",
    title: "ReplayGain",
    art: ART.replaygain,
    panel: ReplayGainPanel,
    active: () => (engine.settings()?.replayGainMode ?? 0) !== 0,
    knob: {
      label: "ReplayGain 预增益",
      readout: (v) => `${(v * 24 - 12).toFixed(1)}`,
      note: () => "dB preamp",
      value: () => ((engine.settings()?.replayGainPreampDb ?? 0) + 12) / 24,
      set: (v, commit) => {
        const settings = engine.settings();
        if (!settings) return;
        try {
          engine.setReplayGain(
            commit && settings.replayGainMode === 0 ? 1 : settings.replayGainMode,
            v * 24 - 12,
            settings.replayGainPreventClipping,
            settings.replayGainFallbackDb,
          );
        } catch (error) {
          reportEngineFailure(error, "ReplayGain 下发失败");
        }
      },
      engage: () => {
        const settings = engine.settings();
        if (!settings || settings.replayGainMode !== 0) return;
        try {
          engine.setReplayGain(1, settings.replayGainPreampDb, settings.replayGainPreventClipping, settings.replayGainFallbackDb);
        } catch (error) {
          reportEngineFailure(error, "ReplayGain 打开失败");
        }
      },
      enabled: () => (engine.settings()?.replayGainMode ?? 0) !== 0,
    },
  },
  {
    key: "convolver",
    title: "Convolver",
    art: ART.convolver,
    panel: ConvolverPanel,
    active: () => engine.settings()?.convolverEnabled ?? false,
    knob: {
      label: "Convolver 湿声",
      readout: (v) => v.toFixed(2),
      note: () => "wet",
      value: () => engine.settings()?.convolverWet ?? 0,
      set: (v, commit) => {
        const settings = engine.settings();
        if (!settings) return;
        try {
          engine.setConvolver(
            settings.convolverEnabled || commit,
            settings.convolverIrPath,
            v,
            settings.convolverDry,
            settings.convolverGainDb,
            settings.convolverNormalize,
          );
        } catch (error) {
          reportEngineFailure(error, "Convolver 下发失败");
        }
      },
      engage: () => {
        const settings = engine.settings();
        if (!settings || settings.convolverEnabled) return;
        if (!settings.convolverIrPath) {
          notify("Convolver 需要 IR 文件；先在展开面板里看引擎报的路径");
          return;
        }
        try {
          engine.setConvolver(true, settings.convolverIrPath, settings.convolverWet, settings.convolverDry, settings.convolverGainDb, settings.convolverNormalize);
        } catch (error) {
          reportEngineFailure(error, "Convolver 打开失败");
        }
      },
      enabled: () => engine.settings()?.convolverEnabled ?? false,
    },
  },
  {
    key: "crossfade",
    title: "Crossfade",
    art: ART.crossfade,
    panel: CrossfadePanel,
    active: () => engine.settings()?.crossfadeEnabled ?? false,
    knob: {
      label: "Crossfade 时长",
      readout: (v) => `${(v * 12).toFixed(1)}`,
      note: () => "s duration",
      value: () => (engine.settings()?.crossfadeDurationMs ?? 0) / 12000,
      set: (v, commit) => {
        const settings = engine.settings();
        if (!settings) return;
        try {
          engine.setCrossfade(settings.crossfadeEnabled || commit, Math.round(v * 12) * 1000, settings.crossfadeCurve);
        } catch (error) {
          reportEngineFailure(error, "Crossfade 下发失败");
        }
      },
      engage: () => {
        const settings = engine.settings();
        if (!settings || settings.crossfadeEnabled) return;
        try {
          engine.setCrossfade(true, settings.crossfadeDurationMs, settings.crossfadeCurve);
        } catch (error) {
          reportEngineFailure(error, "Crossfade 打开失败");
        }
      },
      enabled: () => engine.settings()?.crossfadeEnabled ?? false,
    },
  },
];

/** The knob positions being dragged right now, by stage key: the knob
 * follows the pointer without waiting for the engine's echo, and the echo
 * takes over once the drag ends. */
const knobs = signal<Record<string, number>>({});

function knobValue(stage: (typeof DSP_STAGES)[number]): number {
  const draft = knobs()[stage.key];
  return draft ?? (stage.knob ? stage.knob.value() : 0);
}

function turnKnob(stage: (typeof DSP_STAGES)[number], value: number, commit: boolean): void {
  if (!stage.knob) return;
  const clamped = Math.min(1, Math.max(0, value));
  knobs.update((all) => ({ ...all, [stage.key]: clamped }));
  stage.knob.set(clamped, commit);
  if (commit) {
    // The engine reports the value it took; its echo replaces the draft.
    setTimeout(() => {
      knobs.update((all) => {
        const next = { ...all };
        delete next[stage.key];
        return next;
      });
    }, 900);
  }
}

/** One tile of the controller grid: LED, engraved name, glyph, readout and
 * the knob. A press on the tile (not on the knob) expands its settings. */
function StageTile(props: { stage: (typeof DSP_STAGES)[number]; expanded: boolean; playing: boolean; onToggle: () => void }) {
  const stage = () => props.stage;
  const active = () => stage().active();
  const enabled = () => stage().knob?.enabled() ?? active();
  const led = createAnimatedValue(1);
  effect(() => {
    if (active() && props.playing && !reduceMotion()) {
      const pulse = Animated.loop(
        Animated.sequence([
          Animated.timing(led, { toValue: 1, duration: 700, easing: "easeInOut" }),
          Animated.timing(led, { toValue: 0.45, duration: 700, easing: "easeInOut" }),
        ]),
      );
      void pulse.start();
      return () => pulse.stop();
    }
    led.set(1);
  });
  const breathing = () => active() && props.playing && !reduceMotion();
  const readout = () => {
    const knob = stage().knob;
    if (knob) return knob.readout(knobValue(stage()));
    // The two stages with nothing to turn still report the engine's own fact:
    // the rate the path lands on, the depth it is dithered to.
    if (stage().key === "resampler") {
      const path = engine.renderPath();
      return path ? formatRate(path.sampleRate) : "—";
    }
    if (stage().key === "dither") {
      const path = engine.renderPath();
      return path && path.bitsPerSample > 0
        ? `${path.validBits || path.bitsPerSample}/${path.bitsPerSample}`
        : "—";
    }
    return "—";
  };
  return (
    <View
      class={`min-w-0 flex-1 gap-2 rounded-(--radius-card) p-2.5 transition duration-150 ease-out`}
      style={{
        // 一块面板：上沿一道内高光、下沿一道内阴影、外面一层浅投影。扁平
        // 的 bg-card 在这些卡上会显得像纸片（对着 demo 看出来的差别）。
        borderWidth: 1,
        borderColor: props.expanded ? withAlpha(String(token("foreground")), 0.4) : String(token("border")),
        backgroundImage: props.expanded
          ? "linear-gradient(180deg, #24282b 0%, #1b1e20 60%, #191c1e 100%)"
          : "linear-gradient(180deg, #1b1e20 0%, #141618 55%, #101213 100%)",
        boxShadow: [
          { x: 0, y: 1, blur: 0, color: "#ffffff0d", inset: true },
          { x: 0, y: -1, blur: 0, color: "#000000b3", inset: true },
          { x: 0, y: 1, blur: 2, color: "#00000080" },
        ],
      }}
    >
      <Button
        accessibilityLabel={stage().title}
        selected={props.expanded}
        onPress={props.onToggle}
        class={`flex-row items-center gap-2 ${focusRing}`}
      >
        <View
          class="size-2 shrink-0 rounded-full"
          style={{
            backgroundColor: active() ? String(token("primary")) : String(token("muted")),
            opacity: breathing() ? led : active() ? 1 : 0.55,
            boxShadow: active() ? `0 0 6 ${withAlpha(String(token("primary")), 0.6)}` : "0 0 0 transparent",
          }}
        />
        <Text
          class={`min-w-0 flex-1 line-clamp-1 text-[11px] font-bold uppercase tracking-[0.14em] ${active() ? "text-foreground" : "text-muted-foreground"}`}
          style={{ textShadow: "0 1px 0 #000, 0 -1px 0 #ffffff0d" }}
        >
          {stage().title}
        </Text>
        <View style={active() ? undefined : { opacity: 0.55 }}>
          <StageArt body={stage().art} size={16} color={String(token("muted-foreground"))} />
        </View>
      </Button>
      <View class="flex-row items-end justify-between gap-2">
        <View class="min-w-0 gap-0.5 pb-0.5">
          <Text
            class="font-mono text-base tabular-nums text-primary"
            style={{
              opacity: active() ? 1 : 0.62,
              textShadow: active() ? "0 0 12 rgba(226,165,63,0.35)" : undefined,
            }}
          >
            {readout()}
          </Text>
          <Text class="text-[9px] uppercase tracking-[0.14em] text-muted-foreground">
            {stage().knob ? stage().knob!.note?.() ?? "" : stage().key === "resampler" ? "output rate" : "bit depth"}
          </Text>
        </View>
        <If when={!!stage().knob}>
          <StageKnob stage={stage()} value={knobValue(stage())} />
        </If>
        <If when={!stage().knob}>
          <View class="pb-1">
            <Text class="text-[9px] uppercase tracking-[0.14em] text-muted-foreground">
              {active() ? "on" : "off"}
            </Text>
          </View>
        </If>
      </View>
    </View>
  );
}

/** The knob of a turnable stage. Its own component so the optional knob is
 * narrowed once, not three times. */
function StageKnob(props: { stage: (typeof DSP_STAGES)[number]; value: number }) {
  const knob = () => props.stage.knob;
  return (
    <Knob
      label={knob()?.label ?? props.stage.title}
      value={props.value}
      size={40}
      onChange={(v: number) => turnKnob(props.stage, v, false)}
      onChangeEnd={(v: number) => {
        knob()?.engage();
        turnKnob(props.stage, v, true);
      }}
    />
  );
}

/** The controller grid: two tiles a row, the expanded stage's panel spanning
 * the full width under the grid (one panel at a time). */
function DspChain() {
  const rows: (typeof DSP_STAGES)[] = [];
  for (let i = 0; i < DSP_STAGES.length; i += 2) rows.push(DSP_STAGES.slice(i, i + 2));
  const expanded = () => DSP_STAGES.find((stage) => stage.key === openStage());
  return (
    <View class="w-full gap-2">
      <Each of={rows} key="index">
        {(row) => (
          <View class="w-full flex-row gap-2">
            <Each of={row()} key={(stage) => stage.key}>
              {(stage) => (
                <StageTile
                  stage={stage()}
                  expanded={openStage() === stage().key}
                  playing={engine.player().state === "playing"}
                  onToggle={() => toggleStage(stage().key)}
                />
              )}
            </Each>
            <If when={row().length === 1}>
              <View class="min-w-0 flex-1" />
            </If>
          </View>
        )}
      </Each>
      <If when={expanded()}>
        {(stage) => (
          <View class="w-full gap-2 rounded-(--radius-card) border border-border bg-card p-3">
            <View class="flex-row items-center justify-between">
              <Text class="text-[11px] font-bold uppercase tracking-[0.12em] text-foreground">{stage().title}</Text>
              <Button
                accessibilityLabel="收起面板"
                onPress={() => openStage.set(null)}
                class={`items-center justify-center rounded-(--radius) p-1 hover:bg-muted active:opacity-60 ${focusRing}`}
              >
                <Icon name="close" size={12} color="muted-foreground" />
              </Button>
            </View>
            {stage().panel()}
          </View>
        )}
      </If>
    </View>
  );
}

function OutputContent() {
  // The two figures the PPM ladder draws: the engine's own sample peak and
  // RMS from the visualiser frames it sends while it plays. Nothing here
  // measures audio, and nothing is drawn when no frame has arrived.
  const playing = () => engine.player().state === "playing";
  const peak = () => (playing() ? visualPeak() : 0);
  const peakL = () => (playing() ? visualPeakL() : null);
  const peakR = () => (playing() ? visualPeakR() : null);
  const rms = () => (playing() ? visualRms() : 0);
  return (
    <Scroll
      class="flex-1 flex-col"
      contentClass="flex flex-col gap-3 border-l border-border bg-sidebar px-3 py-3 pt-[env(safe-area-inset-top)]"
    >
      {/* The instrument's own title bar: the model name, the foley switch and
          the close. */}
      <View class="flex-row items-center gap-2 px-1">
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class="text-[10px] font-bold uppercase tracking-[0.2em]" style={{ color: HARDWARE.amber }}>
            NEIRO · OUTPUT PATH
          </Text>
          <Text role="header" class="text-lg font-extrabold text-foreground">
            Output 控制台
          </Text>
        </View>
        <MechKey
          label={foleyEnabled() ? "关闭机械音效" : "开启机械音效"}
          lit={foleyEnabled()}
          onPress={() => setFoleyEnabled(!foleyEnabled())}
          class="h-6 items-center justify-center rounded px-2"
        >
          <Text class="font-mono text-[8px] font-bold tracking-wider" style={{ color: foleyEnabled() ? HARDWARE.amberBright : HARDWARE.textDim }}>
            FOLEY {foleyEnabled() ? "ON" : "OFF"}
          </Text>
        </MechKey>
        <Button
          accessibilityLabel="关闭输出信息"
          onPress={() => setOutputOpen(false)}
          class={`items-center justify-center rounded-(--radius) p-1 hover:bg-muted active:opacity-60 ${focusRing}`}
        >
          <Icon name="close" size={16} color="muted-foreground" />
        </Button>
      </View>

      <Chassis class="gap-3 p-2.5">
        <SourceStage
          peak={peak()}
          peakL={peakL()}
          peakR={peakR()}
          rms={rms()}
          playing={playing()}
        />
        <DspMatrix
          flowing={player().state === "playing"}
          eqActive={engine.settings()?.eqEnabled ?? false}
          onOpenEq={() => {
            setEqOpenedFromOutput(true);
            setOutputOpen(false);
            open({ name: "eq" });
          }}
        />
        <View class="gap-1.5">
          <ModuleHeader index="03" title="BIT-PERFECT MASTER BUS" right={<Silkscreen tone="faint">SIGNAL INTEGRITY</Silkscreen>} />
          <MasterBus />
        </View>
        <View class="gap-1.5">
          <ModuleHeader index="04" title="OUTPUT INTERFACE & MASTER GAIN" />
          <OutputInterface />
        </View>
        <View class="gap-1.5">
          <ModuleHeader index="05" title="TELEMETRY & ROOT-CAUSE DIAGNOSTICS" />
          <TelemetryStage />
        </View>
      </Chassis>
    </Scroll>
  );
}
