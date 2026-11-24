// Output 控制台 — 现代精密信号链路面板（Roon Signal Path × Apple Pro Apps 风格）。
// 统一使用 theme.json 语义色彩与响应式布局（桌面右侧抽屉 / iOS 底部 Sheet），
// 保留完整引擎控制、PPM 实时电平、Bit-Perfect 逐项校验与可折叠工程遥测。

import { effect, platform, safeArea, Scroll, signal, Text, untracked, View } from "nsh";
import { Button, Popover } from "nsh/base";
import { engine, reportEngineFailure } from "../../backend/engine";
import { outputOpen, setEqOpenedFromOutput, setOutputOpen } from "../../state/overlays";
import { AppSheet } from "../ui/overlays";
import { ActionButton, focusRing } from "../ui/controls";
import { Icon } from "../ui/icons";
import { notify } from "../../state/feedback";
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
  type RateKind,
} from "../../state/resolved";

const FLOOR_DB = -48;
const SEGMENTS = 24;

const REASONS: Record<number, { title: string; detail: string; fix: string }> = {
  1: { title: "重采样至设备采样率", detail: "输出采样率与源文件不一致", fix: "将采样率策略切回「跟随源」，或更换支持源采样率的设备" },
  2: { title: "重采样至固定采样率", detail: "已启用固定采样率输出", fix: "将采样率策略切回「跟随源」" },
  3: { title: "已应用抖动 (Dither)", detail: "设备位深窄于源文件", fix: "更换支持更高位深的输出设备" },
  4: { title: "声道映射转换", detail: "输出声道数与源文件不同", fix: "使设备声道配置与源文件一致" },
  5: { title: "设备不支持源格式", detail: "硬件拒绝原格式，引擎自动协商替代格式", fix: "更换支持该格式的音频设备" },
  6: { title: "重采样至最接近速率", detail: "设备拒绝源采样率", fix: "更换支持该采样率的设备" },
  7: { title: "系统共享混音模式", detail: "音频经过系统混音器", fix: "切到「独占直通」以绕过系统混音" },
  8: { title: "软件音量非 100%", detail: "数字衰减会改变原始采样值", fix: "将主音量设为 100%" },
  9: { title: "均衡器已开启", detail: "32 段均衡器正在处理信号", fix: "关闭均衡器或点击「一键 Bit-Perfect 直通」" },
  10: { title: "Crossfeed 已开启", detail: "耳机声场交叉馈送正在处理信号", fix: "在高级选项中关闭 Crossfeed" },
  11: { title: "ReplayGain 已开启", detail: "响度增益正在调整电平", fix: "在高级选项中关闭 ReplayGain" },
  12: { title: "卷积器已开启", detail: "IR 卷积正在处理信号", fix: "在高级选项中关闭卷积器" },
  13: { title: "交叉淡入已开启", detail: "曲间交叉淡入已启用", fix: "在高级选项中启用「Gapless 无缝播放」" },
  14: { title: "32-bit 浮点通路精度转换", detail: "32-bit 整型源经 float32 通路传输", fix: "" },
  15: { title: "防削波限幅器激活", detail: "峰值接近 0 dBFS，限幅器已介入", fix: "" },
};

/** 抽屉入口：桌面端右侧面板，移动端/iOS 底部滑出 Sheet。 */
export function OutputSheet() {
  const isMobile = () => layout() === "mobile";
  return (
    <AppSheet
      open={outputOpen()}
      onOpenChange={setOutputOpen}
      side={isMobile() ? "bottom" : "right"}
      closeOnSwipe={isMobile()}
      testID="output"
      popupWidth={460}
    >
      <OutputContent />
    </AppSheet>
  );
}

function StatusPill() {
  const noStream = () => resolvedPath() === null;
  const bitPerfect = () => isStreamBitPerfect();
  return (
    <View
      class={`flex-row items-center gap-1.5 rounded-full px-2.5 py-1 ${
        noStream()
          ? "bg-secondary"
          : bitPerfect()
            ? "bg-emerald-500/12"
            : "bg-primary/12"
      }`}
    >
      <View
        class={`h-1.5 w-1.5 rounded-full ${
          noStream() ? "bg-muted-foreground" : bitPerfect() ? "bg-emerald-500" : "bg-primary"
        }`}
      />
      <Text
        class={`text-[11px] font-semibold ${
          noStream() ? "text-muted-foreground" : bitPerfect() ? "text-emerald-600" : "text-primary"
        }`}
      >
        {noStream() ? "未播放 · NO STREAM" : bitPerfect() ? "Bit-Perfect" : "已处理"}
      </Text>
    </View>
  );
}

function PpmRow(props: { label: string; level: number }) {
  const db = () => (props.level > 0 ? 20 * Math.log10(Math.max(1e-6, props.level)) : FLOOR_DB);
  const lit = () => Math.max(0, Math.min(SEGMENTS, Math.round(((db() - FLOOR_DB) / -FLOOR_DB) * SEGMENTS)));
  return (
    <View class="flex-row items-center gap-2">
      <Text class="w-8 text-[10px] font-mono font-semibold text-muted-foreground">{props.label}</Text>
      <View class="flex-1 flex-row gap-[2px]">
        <Each of={Array.from({ length: SEGMENTS }, (_, i) => i)} key="index">
          {(index) => {
            const on = () => index() < lit();
            const colorClass = () =>
              !on()
                ? "bg-secondary"
                : index() >= SEGMENTS - 3
                  ? "bg-destructive"
                  : index() >= SEGMENTS - 7
                    ? "bg-primary"
                    : "bg-emerald-500";
            return <View class={`h-1.5 flex-1 rounded-xs ${colorClass()}`} />;
          }}
        </Each>
      </View>
    </View>
  );
}

function SignalNode(props: {
  title: string;
  subtitle: string;
  tone: "idle" | "clean" | "active";
  last?: boolean;
  action?: { label: string; onPress: () => void };
}) {
  const dotClass = () =>
    props.tone === "clean"
      ? "bg-emerald-500"
      : props.tone === "active"
        ? "bg-primary"
        : "bg-muted-foreground/40";
  return (
    <View class="flex-row gap-3">
      <View class="w-4 items-center">
        <View class={`mt-1.5 h-2.5 w-2.5 rounded-full ${dotClass()}`} />
        <If when={!props.last}>
          <View class="my-1 w-[1.5px] flex-1 bg-border" />
        </If>
      </View>
      <View class={`min-w-0 flex-1 ${props.last ? "" : "pb-3"}`}>
        <View class="flex-row items-center justify-between gap-2">
          <Text class="text-xs font-semibold text-foreground">{props.title}</Text>
          <If when={props.action}>
            <Button
              accessibilityLabel={props.action!.label}
              onPress={props.action!.onPress}
              class={`rounded-(--radius) px-2 py-0.5 hover:bg-secondary ${focusRing}`}
            >
              <Text class="text-[11px] font-semibold text-primary">{props.action!.label}</Text>
            </Button>
          </If>
        </View>
        <Text class="mt-0.5 text-xs text-muted-foreground">{props.subtitle}</Text>
      </View>
    </View>
  );
}

function SignalPathCard(props: { peak: number; peakL: number | null; peakR: number | null; rms: number }) {
  const path = () => resolvedPath();
  const rp = () => engine.renderPath();
  const p = () => player();
  const hasStream = () => path() !== null || p().state === "playing" || p().state === "paused";

  const sourceTitle = () => (hasStream() ? displayName(p().path, p().title) || "音频源" : "音频源 · 等待播放");
  const sourceSubtitle = () => {
    if (!hasStream()) return "选择一首曲目开始播放后显示完整信号链路";
    const rpVal = path();
    const parts: string[] = [];
    const codec = codecLabel() || sourceFormatLabel();
    if (codec) parts.push(codec);
    if (isDsdSource()) {
      const rate = sourceDsdRate();
      if (rate > 0) parts.push(dsdRateLabel(rate));
      parts.push("1-bit DSD");
    } else if (rpVal && rpVal.sourceRate > 0) {
      parts.push(formatHz(rpVal.sourceRate));
      const bits = sourceBits();
      if (bits > 0) parts.push(`${bits}-bit`);
      if (rpVal.sourceChannels > 0) parts.push(`${rpVal.sourceChannels} ch`);
    }
    return parts.join(" · ") || "已打开音频流";
  };

  const dspSummary = () => {
    const r = rp();
    const s = engine.settings();
    const active: string[] = [];
    if (r?.eqEnabled ?? s?.eqEnabled) active.push("32 段均衡器");
    if (r?.crossfeedEnabled ?? s?.crossfeedEnabled) active.push("Crossfeed");
    if (r?.replayGainEnabled ?? (s && s.replayGainMode !== 0)) active.push("ReplayGain");
    if (r?.convolverEnabled ?? s?.convolverEnabled) active.push("卷积器");
    if (r?.crossfadeEnabled ?? s?.crossfadeEnabled) active.push("交叉淡入");
    if (opraActive()) active.push(`OPRA (${opraSelected()})`);
    if (path()?.resampling) active.push("重采样");
    if (path()?.dithered) active.push("Dither");
    return active.length > 0 ? active.join(" · ") : "全部旁通（无增益/滤波改动）";
  };

  const dspTone = (): "idle" | "clean" | "active" => {
    if (!hasStream()) return "idle";
    const checks = bitPerfectChecks();
    return checks.dspClean() && checks.rateMatch() ? "clean" : "active";
  };

  const outputSubtitle = () => {
    const rpVal = path();
    if (!rpVal) return "—";
    const rate = isDsdSource() && rpVal.dsdRate > 0 ? dsdRateLabel(rpVal.dsdRate) : formatHz(rpVal.sampleRate);
    const bits = isDsdSource()
      ? rpVal.dsdTransport === "dop"
        ? "DoP"
        : "Native DSD"
      : rpVal.isFloat
        ? `${rpVal.validBits || rpVal.containerBits}-bit float`
        : `${rpVal.validBits || rpVal.containerBits}-bit`;
    const backend =
      platform.os === "ios"
        ? "iOS RemoteIO"
        : rpVal.backend === "coreaudio"
          ? "CoreAudio"
          : rpVal.backend === "asio"
            ? "ASIO"
            : "WASAPI";
    const mode = platform.os === "ios" ? "" : rpVal.shareMode === 1 ? " 独占" : " 共享";
    const lat = rpVal.bufferLatencyMs > 0 ? ` · ${rpVal.bufferLatencyMs.toFixed(1)} ms` : "";
    return `${rpVal.deviceName || "默认输出"} · ${rate} · ${bits} · ${backend}${mode}${lat}`;
  };

  const stereo = () => props.peakL !== null && props.peakR !== null;
  const dbOf = (level: number) => (level > 0 ? 20 * Math.log10(Math.max(1e-6, level)) : null);
  const reading = (v: number | null) => (v === null ? "—" : `${v.toFixed(1)} dB`);

  return (
    <View class="gap-3 rounded-(--radius-panel) border border-border bg-card p-4">
      <Text class="text-xs font-bold tracking-wide text-foreground">信号链路</Text>
      <View class="pt-0.5">
        <SignalNode title={sourceTitle()} subtitle={sourceSubtitle()} tone={hasStream() ? "clean" : "idle"} />
        <SignalNode
          title="音频处理"
          subtitle={dspSummary()}
          tone={dspTone()}
          action={{
            label: "均衡器 →",
            onPress: () => {
              setEqOpenedFromOutput(true);
              setOutputOpen(false);
              open({ name: "eq" });
            },
          }}
        />
        <SignalNode
          title="输出链路"
          subtitle={outputSubtitle()}
          tone={hasStream() ? (isStreamBitPerfect() ? "clean" : "active") : "idle"}
          last
        />
      </View>

      <View class="gap-1.5 border-t border-border pt-3">
        <View class="flex-row items-center justify-between">
          <Text class="text-[11px] font-medium text-muted-foreground">实时电平 ({stereo() ? "立体声" : "总峰值"})</Text>
          <Text class="font-mono text-[11px] tabular-nums text-muted-foreground">
            {`PEAK ${reading(dbOf(props.peak))} · RMS ${reading(dbOf(props.rms))}`}
          </Text>
        </View>
        <If when={stereo()} else={<PpmRow label="PEAK" level={props.peak} />}>
          <PpmRow label="L" level={props.peakL ?? 0} />
          <PpmRow label="R" level={props.peakR ?? 0} />
        </If>
      </View>
    </View>
  );
}

function OutputDeviceCard() {
  const isIos = platform.os === "ios";
  const menuOpen = signal(false);
  const locked = () => devicesLocked();
  const devices = () => engine.devices();
  const resolvedId = () => resolvedPath()?.deviceId ?? "";
  const backend = () => resolvedPath()?.backend ?? null;
  const onAsio = () => backend() === "asio";
  const asioDevice = () => devices().find((d) => d.id.startsWith("asio:"));
  const mode = () => shareMode();

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

  return (
    <View class="gap-3 rounded-(--radius-panel) border border-border bg-card p-4">
      <View class="flex-row items-center justify-between">
        <Text class="text-xs font-bold tracking-wide text-foreground">输出与设备</Text>
        <If when={!isIos && mode() === 1}>
          <Button
            accessibilityLabel="释放独占设备"
            onPress={() => {
              try {
                engine.releaseDevice();
              } catch (error) {
                reportEngineFailure(error, "释放设备失败");
              }
            }}
            class={`rounded-(--radius) border border-primary/40 bg-primary/10 px-2 py-0.5 text-[11px] font-semibold text-primary ${focusRing}`}
          >
            释放独占设备
          </Button>
        </If>
      </View>

      <If
        when={!isIos}
        else={
          <View class="gap-1 rounded-(--radius) bg-secondary px-3 py-2.5">
            <Text class="text-xs font-medium text-foreground">
              {resolvedPath()?.deviceName || "iOS 系统音频输出"}
            </Text>
            <Text class="text-[11px] text-muted-foreground">
              由 AVAudioSession / RemoteIO 自动路由，系统音量由设备侧键控制
            </Text>
          </View>
        }
      >
        <View class="flex-row gap-1 rounded-(--radius) bg-secondary p-1">
          <Button
            accessibilityLabel="独占直通"
            onPress={() => {
              const rate = currentRate();
              try {
                applyPolicy(rate.kind, rate.fixedRate, "exclusive");
              } catch (error) {
                reportEngineFailure(error, "切独占失败");
              }
            }}
            class={`flex-1 items-center justify-center rounded-(--radius-inset) py-1.5 transition ${
              mode() === 1 ? "bg-card shadow-xs" : ""
            } ${focusRing}`}
          >
            <Text class={`text-xs font-semibold ${mode() === 1 ? "text-foreground" : "text-muted-foreground"}`}>
              独占直通{mode() === 1 ? " · EXCL" : ""}
            </Text>
          </Button>
          <Button
            accessibilityLabel="系统共享混音"
            onPress={() => {
              const rate = currentRate();
              try {
                applyPolicy(rate.kind, rate.fixedRate, "shared");
              } catch (error) {
                reportEngineFailure(error, "切共享失败");
              }
            }}
            class={`flex-1 items-center justify-center rounded-(--radius-inset) py-1.5 transition ${
              mode() === 2 ? "bg-card shadow-xs" : ""
            } ${focusRing}`}
          >
            <Text class={`text-xs font-semibold ${mode() === 2 ? "text-foreground" : "text-muted-foreground"}`}>
              系统共享
            </Text>
          </Button>
          <Button
            accessibilityLabel={asioDevice() ? "切到 ASIO 驱动" : "未检测到 ASIO 驱动"}
            disabled={!asioDevice()}
            onPress={() => {
              const dev = asioDevice();
              if (!dev) return;
              try {
                engine.setDevice(dev.id);
              } catch (error) {
                reportEngineFailure(error, "切 ASIO 失败");
              }
            }}
            class={`flex-1 items-center justify-center rounded-(--radius-inset) py-1.5 transition ${
              onAsio() ? "bg-card shadow-xs" : ""
            } ${focusRing}`}
          >
            <Text
              class={`text-xs font-semibold ${
                onAsio() ? "text-foreground" : asioDevice() ? "text-muted-foreground" : "text-muted-foreground/40"
              }`}
            >
              ASIO
            </Text>
          </Button>
        </View>

        <View class="gap-1.5">
          <View class="flex-row items-center justify-between">
            <Text class="text-[11px] font-medium text-muted-foreground">输出端点</Text>
            <Button
              accessibilityLabel={locked() ? "播放中锁定枚举" : "刷新设备"}
              disabled={locked()}
              onPress={() => ensureDevices()}
              class={`rounded-(--radius) px-2 py-0.5 ${locked() ? "" : "hover:bg-secondary"} ${focusRing}`}
            >
              <Text class={`text-[11px] ${locked() ? "text-muted-foreground/60" : "text-primary"}`}>
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
              <Button
                accessibilityLabel="选择端点"
                class={`flex-row items-center justify-between rounded-(--radius) border border-border bg-secondary/60 px-3 py-2 hover:bg-secondary ${focusRing}`}
              >
                <View class="min-w-0 flex-1">
                  <Text class="line-clamp-1 text-xs font-semibold text-foreground">{selectedRow().name}</Text>
                  <Text class="line-clamp-1 text-[11px] text-muted-foreground">{selectedRow().detail}</Text>
                </View>
                <Icon name="chevronDown" size={14} color="muted-foreground" />
              </Button>
            }
          >
            <View class="w-80 gap-0.5 rounded-(--radius-card) border border-border bg-popover p-1.5 shadow-xl">
              <Each of={deviceRows()} key={(row) => row.key}>
                {(row) => (
                  <Button
                    accessibilityLabel={`端点 ${row().name}`}
                    onPress={() => {
                      menuOpen.set(false);
                      try {
                        engine.setDevice(row().id);
                      } catch (error) {
                        reportEngineFailure(error, "切换端点失败");
                      }
                    }}
                    class={`flex-row items-center gap-2 rounded-(--radius) px-2.5 py-2 hover:bg-muted ${focusRing}`}
                  >
                    <View class={`h-2 w-2 rounded-full ${row().selected ? "bg-primary" : "bg-muted-foreground/30"}`} />
                    <View class="min-w-0 flex-1">
                      <Text class={`line-clamp-1 text-xs font-medium ${row().selected ? "text-primary" : "text-foreground"}`}>
                        {row().name}
                      </Text>
                      <Text class="line-clamp-1 text-[10px] text-muted-foreground">{row().detail}</Text>
                    </View>
                  </Button>
                )}
              </Each>
            </View>
          </Popover>
        </View>

        <View class="flex-row items-center justify-between border-t border-border pt-3">
          <View class="gap-0.5">
            <Text class="text-xs font-medium text-foreground">主音量</Text>
            <Text class="font-mono text-[11px] tabular-nums text-muted-foreground">
              {Math.round(vol() * 100)}%
              {isStreamBitPerfect() ? " · 直通锁定 100%" : ""}
            </Text>
          </View>
          <VolumeControl />
        </View>
      </If>
    </View>
  );
}

function BitPerfectCard() {
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

  const badge = (label: string, pass: boolean) => (
    <View
      class={`flex-1 flex-row items-center justify-center gap-1.5 rounded-(--radius) border py-2 ${
        pass ? "border-emerald-500/35 bg-emerald-500/10" : "border-border bg-secondary/50"
      }`}
    >
      <View class={`h-1.5 w-1.5 rounded-full ${pass ? "bg-emerald-500" : "bg-muted-foreground/40"}`} />
      <Text class={`text-[11px] font-medium ${pass ? "text-emerald-600" : "text-muted-foreground"}`}>{label}</Text>
    </View>
  );

  return (
    <View class="gap-3 rounded-(--radius-panel) border border-border bg-card p-4">
      <View class="flex-row items-center justify-between gap-2">
        <Text class="text-xs font-bold tracking-wide text-foreground">Bit-Perfect 校验</Text>
        <If when={!isIos}>
          <ActionButton
            label={bitPerfect() ? "直通已启用" : "一键 Bit-Perfect 直通"}
            variant={bitPerfect() ? "outline" : "primary"}
            onPress={goDirect}
          />
        </If>
      </View>

      <View class="flex-row gap-1.5">
        {badge("DSP 旁通", checks.dspClean())}
        {badge("采样一致", checks.rateMatch())}
        <If when={!isIos}>{badge("独占直通", checks.exclusive())}</If>
        {badge("音量 100%", checks.unity())}
      </View>

      <If
        when={noStream()}
        else={
          <If
            when={!bitPerfect()}
            else={
              <View class="rounded-(--radius) border border-emerald-500/30 bg-emerald-500/10 px-3 py-2">
                <Text class="text-xs font-medium text-emerald-600">
                  整条通路未改动：采样率与源文件一致、独占输出、音量 100%、所有 DSP 旁通。
                </Text>
              </View>
            }
          >
            <View class="gap-1.5">
              <Each of={causes()} key="index">
                {(item) => (
                  <View class="rounded-(--radius) border border-primary/30 bg-primary/8 px-3 py-2">
                    <Text class="text-xs font-semibold text-foreground">{item().title}</Text>
                    <If when={item().fix !== ""}>
                      <Text class="mt-0.5 text-[11px] text-muted-foreground">{item().fix}</Text>
                    </If>
                  </View>
                )}
              </Each>
              <If when={causes().length === 0 && lastDiagnostic() !== ""}>
                <Text class="text-xs text-muted-foreground">{lastDiagnostic()}</Text>
              </If>
            </View>
          </If>
        }
      >
        <Text class="text-xs text-muted-foreground">
          当前未播放（NO STREAM）：开始播放后将实时校验整条音频链路。
        </Text>
      </If>
    </View>
  );
}

function SegmentedRow<T extends string | number>(props: {
  label: string;
  options: { value: T; label: string }[];
  value: T;
  disabled?: boolean;
  onPick: (v: T) => void;
}) {
  return (
    <View class="gap-1.5">
      <Text class="text-xs font-medium text-muted-foreground">{props.label}</Text>
      <View class="flex-row gap-1 rounded-(--radius) bg-secondary p-1">
        <Each of={props.options} key={(o) => String(o.value)}>
          {(o) => {
            const active = () => o().value === props.value;
            return (
              <Button
                accessibilityLabel={`${props.label} ${o().label}`}
                disabled={props.disabled}
                onPress={() => props.onPick(o().value)}
                class={`flex-1 items-center justify-center rounded-(--radius-inset) py-1.5 transition ${
                  active() ? "bg-card shadow-xs" : ""
                } ${focusRing}`}
              >
                <Text class={`text-xs ${active() ? "font-semibold text-foreground" : "font-medium text-muted-foreground"}`}>
                  {o().label}
                </Text>
              </Button>
            );
          }}
        </Each>
      </View>
    </View>
  );
}

function AdvancedSection() {
  const expanded = signal(false);
  const isIos = platform.os === "ios";
  const settings = () => engine.settings();
  const path = () => resolvedPath();
  const dsdLocked = () => isDsdSource() && path()?.dsdTransport !== "none" && !path()?.dsdPcmFallback;

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
    <View class="min-w-0 flex-1 rounded-(--radius) bg-secondary/60 px-3 py-2">
      <Text class="text-[11px] text-muted-foreground">{label}</Text>
      <Text class="mt-0.5 line-clamp-1 font-mono text-xs font-semibold text-foreground">{value}</Text>
    </View>
  );

  return (
    <View class="gap-2">
      <Button
        testID="output-advanced-toggle"
        accessibilityLabel="音效处理与工程遥测"
        onPress={() => expanded.update((v) => !v)}
        class={`flex-row items-center justify-between rounded-(--radius-panel) border border-border bg-card px-4 py-3 hover:bg-secondary/40 ${focusRing}`}
      >
        <Text class="text-xs font-bold tracking-wide text-foreground">音效处理与工程遥测</Text>
        <View class="flex-row items-center gap-1.5">
          <Text class="text-[11px] text-muted-foreground">{expanded() ? "收起" : "展开"}</Text>
          <Icon name={expanded() ? "chevronDown" : "chevronRight"} size={14} color="muted-foreground" />
        </View>
      </Button>

      <If when={expanded()}>
        <View class="gap-3 rounded-(--radius-panel) border border-border bg-card p-4">
          <Text class="text-xs font-bold text-foreground">音效处理</Text>
          <View class="flex-row flex-wrap gap-2">
            <Button
              accessibilityLabel="均衡器开关"
              disabled={dsdLocked()}
              onPress={() => {
                const s = settings();
                if (s) engine.setEq(!s.eqEnabled, s.eqPreampDb, s.eqBands);
              }}
              class={`rounded-(--radius) border px-3 py-1.5 text-xs font-medium ${
                settings()?.eqEnabled ? "border-primary bg-primary/10 text-primary" : "border-border text-muted-foreground"
              } ${focusRing}`}
            >
              均衡器 {settings()?.eqEnabled ? "开" : "关"}
            </Button>
            <Button
              accessibilityLabel="Gapless 无缝播放"
              onPress={() => setGaplessEnabled(!gaplessEnabled())}
              class={`rounded-(--radius) border px-3 py-1.5 text-xs font-medium ${
                gaplessEnabled() ? "border-primary bg-primary/10 text-primary" : "border-border text-muted-foreground"
              } ${focusRing}`}
            >
              Gapless 无缝 {gaplessEnabled() ? "开" : "关"}
            </Button>
            <Button
              accessibilityLabel="Crossfeed 开关"
              disabled={dsdLocked()}
              onPress={() => {
                const s = settings();
                if (s) engine.setCrossfeed(!s.crossfeedEnabled, s.crossfeedAmount, s.crossfeedDelayMs, s.crossfeedCutoffHz);
              }}
              class={`rounded-(--radius) border px-3 py-1.5 text-xs font-medium ${
                settings()?.crossfeedEnabled ? "border-primary bg-primary/10 text-primary" : "border-border text-muted-foreground"
              } ${focusRing}`}
            >
              Crossfeed {settings()?.crossfeedEnabled ? "开" : "关"}
            </Button>
          </View>

          <SegmentedRow
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
            <SegmentedRow
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
            <SegmentedRow
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
            <SegmentedRow
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

          <View class="gap-2 border-t border-border pt-3">
            <Text class="text-xs font-bold text-foreground">工程遥测</Text>
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
        </View>
      </If>
    </View>
  );
}

function OutputContent() {
  const isMobile = () => layout() === "mobile";
  const playing = () => engine.player().state === "playing";
  const peak = () => (playing() ? visualPeak() : 0);
  const peakL = () => (playing() ? visualPeakL() : null);
  const peakR = () => (playing() ? visualPeakR() : null);
  const rms = () => (playing() ? visualRms() : 0);

  return (
    <Scroll
      class="flex-1"
      contentClass={`gap-3 bg-background px-4 py-4 ${isMobile() ? "" : "border-l border-border"}`}
      style={{
        paddingBottom: isMobile() ? Math.max(20, safeArea().bottom + 12) : 24,
      }}
    >
      <View class="flex-row items-center justify-between gap-2 px-0.5">
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class="text-[11px] font-semibold tracking-wider text-primary">音频信号链路</Text>
          <Text role="header" class="text-lg font-bold text-foreground">
            Output 控制台
          </Text>
        </View>
        <StatusPill />
        <Button
          accessibilityLabel="关闭输出信息"
          onPress={() => setOutputOpen(false)}
          class={`items-center justify-center rounded-(--radius) p-1.5 hover:bg-muted active:opacity-60 ${focusRing}`}
        >
          <Icon name="close" size={16} color="muted-foreground" />
        </Button>
      </View>

      <SignalPathCard peak={peak()} peakL={peakL()} peakR={peakR()} rms={rms()} />
      <OutputDeviceCard />
      <BitPerfectCard />
      <AdvancedSection />
    </Scroll>
  );
}
