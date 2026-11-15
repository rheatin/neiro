// 05 // TELEMETRY & ROOT-CAUSE DIAGNOSTICS — the transport strip and the
// audit screen.
//
// The audit is the point: a bit-perfect claim that fails says *why*, one line
// per cause, each naming the control that would clear it. The causes are the
// engine's own reason codes plus the app's own two additions (an OPRA layer
// in the path, a manual curve in it), so nothing is inferred.
//
// While playing, the clock figures refresh every 1.5 s from the engine's
// resolved path. Paused, the last frame is kept and labelled as frozen: a
// figure that stopped updating must not read as a live measurement.

import { effect, signal, Text, View } from "nsh";
import { HARDWARE, Led, Silkscreen } from "../../ui/hardware";
import { engine } from "../../../backend/engine";
import { gaplessEnabled, player, positionClock } from "../../../state/player";
import { upNext } from "../../../state/queue";
import { trackForPath } from "../../../state/catalog";
import { clock, type Track } from "../../../backend/types";
import {
  formatHz,
  isDsdSource,
  lastDiagnostic,
  resolvedPath,
  shareMode,
  sourceBits,
} from "../../../state/resolved";
import { opraActive, currentPoints } from "../../../state/opra";

/** The engine's `NAE_REASON_*` values, as the audit names them. */
const REASONS: Record<number, { label: string; detail: string; fix: string }> = {
  0: { label: "RATE UNCHANGED", detail: "采样未被改动", fix: "" },
  1: { label: "RESAMPLED TO DEVICE RATE", detail: "重采样到端点速率", fix: "把 RESAMPLER 切到 NATIVE，或换一个接受源速率的端点" },
  2: { label: "RESAMPLED TO FIXED RATE", detail: "重采样到指定速率", fix: "把 RESAMPLER 切回 NATIVE / DEVICE" },
  3: { label: "DITHERED", detail: "输出位深窄于源，引擎加了抖动", fix: "换更宽位深的端点（引擎自动决定，无开关）" },
  4: { label: "CHANNEL CONVERTED", detail: "声道数被转换", fix: "端点声道数与文件一致时才会通过" },
  5: { label: "DEVICE CANNOT CARRY FORMAT", detail: "端点无法承载该格式", fix: "换端点，或接受重采样" },
  6: { label: "RESAMPLED TO SUPPORTED RATE", detail: "端点拒绝该速率，用了最近的可用速率", fix: "换一个接受源速率的端点" },
  7: { label: "SHARED MODE", detail: "系统混音器在链路上", fix: "切到 EXCLUSIVE / 独占直通" },
  8: { label: "VOLUME NOT UNITY", detail: "软件音量不是 100%", fix: "主音量归 100%（UNITY）" },
  9: { label: "EQ ACTIVE", detail: "均衡器在链路上", fix: "旁通 EQ，或点 BIT-PERFECT DIRECT" },
  10: { label: "CROSSFEED ACTIVE", detail: "Crossfeed 在链路上", fix: "旁通 X-FEED" },
  11: { label: "REPLAYGAIN ACTIVE", detail: "ReplayGain 在链路上", fix: "旁通 R-GAIN" },
  12: { label: "CONVOLVER ACTIVE", detail: "卷积在链路上", fix: "旁通 CONV" },
  13: { label: "CROSSFADE ACTIVE", detail: "交叉淡入在链路上", fix: "旁通 X-FADE" },
  14: { label: "SOURCE PRECISION", detail: "32-bit int source in float32 path", fix: "" },
  15: { label: "CLIP GUARD ACTIVE", detail: "Limiter active", fix: "" },
};

/** One audit row: the cause, its detail, and the control that clears it. */
function AuditRow(props: { code: number; appCause?: boolean }) {
  const reason = () => REASONS[props.code];
  const label = () => (props.appCause ? "APP LAYER" : reason()?.label ?? `REASON ${props.code}`);
  const detail = () => (props.appCause ? "应用侧叠加层在链路上" : reason()?.detail ?? "引擎未给出说明");
  const fix = () => (props.appCause ? "关闭 OPRA 校正层，或点 BIT-PERFECT DIRECT" : reason()?.fix ?? "");
  return (
    <View class="flex-row items-start gap-1.5">
      <View class="pt-0.5">
        <Led lit tone="red" size={5} />
      </View>
      <View class="min-w-0 flex-1 gap-0.5">
        <View class="flex-row items-baseline gap-1.5">
          <Text class="font-mono text-[9px] font-bold" style={{ color: "#F1676C" }}>
            {label()}
          </Text>
          <Text class="min-w-0 flex-1 font-mono text-[8px]" style={{ color: HARDWARE.textDim }}>
            {detail()}
          </Text>
        </View>
        <If when={fix() !== ""}>
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
            → {fix()}
          </Text>
        </If>
      </View>
    </View>
  );
}

/** The cause list: the engine's reason code plus the app's own layers. */
function Audit() {
  const crossfadeOn = () => Boolean(engine.renderPath()?.crossfadeEnabled || engine.settings()?.crossfadeEnabled);
  const bitPerfect = () => (engine.renderPath()?.bitPerfect ?? false) && !crossfadeOn() && !opraActive();
  const reason = () => engine.renderPath()?.reason ?? 0;
  const opra = () => opraActive();
  /** No open stream: there is nothing to audit, which is not the same as a
   * clean path (and must not read as one). */
  const noStream = () => resolvedPath() === null;
  const rows = () => {
    if (bitPerfect()) return [];
    const out: number[] = [];
    if (reason() !== 0) out.push(reason());
    if (crossfadeOn() && !out.includes(13)) out.push(13);
    return out;
  };
  return (
    <View class="gap-1.5">
      <View class="flex-row items-center gap-1.5">
        <Silkscreen tone="faint">SIGNAL PATH AUDIT</Silkscreen>
        <View class="flex-1" />
        <Text class="font-mono text-[8px]" style={{ color: bitPerfect() ? "#34D399" : noStream() ? HARDWARE.textFaint : "#F1676C" }}>
          {bitPerfect()
            ? "[PASS]"
            : noStream()
              ? "[NO STREAM]"
              : `[${rows().length + (opra() ? 1 : 0)} BLOCKER${rows().length + (opra() ? 1 : 0) === 1 ? "" : "S"}]`}
        </Text>
      </View>
      <If
        when={noStream()}
        else={
      <If
        when={!bitPerfect()}
        else={
          <Text class="font-mono text-[9px]" style={{ color: "#34D399" }}>
            ● 全链路未改动：采样与源一致、独占、音量 100%、所有 DSP 与 OPRA 旁通。
          </Text>
        }
      >
        <View class="gap-1">
          <Each of={rows()} key={(code) => code}>
            {(code) => <AuditRow code={code()} />}
          </Each>
          <If when={opra()}>
            <AuditRow code={-1} appCause />
          </If>
          <If when={rows().length === 0 && !opra() && lastDiagnostic() !== ""}>
            <Text class="font-mono text-[9px]" style={{ color: HARDWARE.textDim }}>
              {lastDiagnostic()}
            </Text>
          </If>
        </View>
      </If>
      }
      >
        <Text class="font-mono text-[9px]" style={{ color: HARDWARE.textFaint }}>
          没有打开的流：播放一条曲目后这里列出阻断 bit-perfect 的每一条原因。
        </Text>
      </If>
    </View>
  );
}

/** The live clock readout: four figures, refreshed while it plays. */
function Telemetry() {
  const tick = signal(0);
  effect(() => {
    if (player().state !== "playing") return;
    const timer = setInterval(() => tick.update((n) => n + 1), 1500);
    return () => clearInterval(timer);
  });
  const path = () => (tick(), resolvedPath());
  const playing = () => player().state === "playing";
  const figure = (label: string, value: string, tone?: string) => (
    <View class="min-w-0 flex-1 gap-0.5">
      <Silkscreen tone="faint">{label}</Silkscreen>
      <Text class="line-clamp-1 font-mono text-[10px] font-bold" style={{ color: tone ?? HARDWARE.text }}>
        {value}
      </Text>
    </View>
  );
  const clockLabel = () => {
    const p = path();
    if (!p) return "—";
    if (isDsdSource() && p.dsdRate > 0) return `${(p.dsdRate / 1000).toFixed(1)} kHz carrier`;
    return formatHz(p.sampleRate);
  };
  const depthLabel = () => {
    const p = path();
    if (!p) return "—";
    if (isDsdSource()) return "1-bit DSD";
    // The depth is negotiated with the endpoint (the widest container it
    // takes); what the panel can honestly show is the source's precision, the
    // container it lands in, and whether dither was applied on the way.
    const src = sourceBits();
    const devValid = p.validBits || p.containerBits;
    const devContainer = p.containerBits || devValid;
    const dithered = p.dithered;
    const devStr = p.isFloat
      ? `${devValid}-bit float`
      : devContainer > devValid
        ? `${devValid}/${devContainer}-bit`
        : `${devValid}-bit`;
    if (src > 0 && (src !== devValid || p.isFloat)) {
      return `${src}-bit → ${devStr}${dithered ? " · DITHERED" : ""}`;
    }
    return `${devStr}${dithered ? " · DITHERED" : ""}`;
  };
  const protocol = () => {
    const p = path();
    if (!p) return "—";
    const backend = p.backend === "coreaudio" ? "COREAUDIO" : p.backend === "asio" ? "ASIO" : "WASAPI";
    const share = shareMode() === 2 ? "SHARED" : "EXCL";
    if (isDsdSource()) return p.dsdTransport === "dop" ? `${backend} ${share} · DoP` : `${backend} ${share} · DSD`;
    return `${backend} ${share} · PCM`;
  };
  return (
    <View class="gap-1.5">
      <View class="flex-row items-center gap-1.5">
        <Silkscreen tone="faint">RESOLVED CLOCK TELEMETRY</Silkscreen>
        <View class="flex-1" />
        <View class="flex-row items-center gap-1">
          <Led lit={playing()} tone={playing() ? "emerald" : "amber"} size={5} />
          <Text class="font-mono text-[8px]" style={{ color: playing() ? "#34D399" : HARDWARE.textFaint }}>
            {playing() ? "LIVE" : player().state === "paused" ? "PAUSED — FROZEN" : "IDLE"}
          </Text>
        </View>
      </View>
      <View class="flex-row gap-2 rounded-lg p-2" style={{ backgroundColor: "rgba(0,0,0,0.28)" }}>
        {figure("HW CLOCK", clockLabel(), isDsdSource() ? "#67E8F9" : HARDWARE.amberBright)}
        {figure("BIT DEPTH", depthLabel())}
        {figure("BUFFER", path() ? `${path()!.bufferLatencyMs.toFixed(1)} ms` : "—")}
        {figure("PROTOCOL", protocol())}
      </View>
    </View>
  );
}

/** The gapless queue strip: what plays, then what follows. */
function TransportStrip() {
  const next = () => upNext()[0] ?? null;
  const nextTrack = (): Track | undefined => {
    const path = next();
    return path ? trackForPath(path) : undefined;
  };
  const current = () => player();
  const currentTitle = () => {
    const p = current();
    if (p.title) return p.title;
    const t = trackForPath(p.path ?? "");
    if (t?.title) return t.title;
    return p.path ? p.path.split(/[\\/]/).pop() || "—" : "—";
  };
  const isCrossfade = () => Boolean(engine.settings()?.crossfadeEnabled);
  const isGapless = () => gaplessEnabled() && !isCrossfade();
  return (
    <View class="gap-1">
      <View class="flex-row items-center gap-1.5">
        <Silkscreen tone="faint">GAPLESS QUEUE</Silkscreen>
        <Text class="line-clamp-1 shrink-0 font-mono text-[8px]" style={{ color: isGapless() ? "#34D399" : isCrossfade() ? "#F59E0B" : HARDWARE.textDim }}>
          [{isGapless() ? "SEAMLESS GAPLESS" : isCrossfade() ? "CROSSFADE ACTIVE" : "GAPLESS BYP"}]
        </Text>
        <View class="flex-1" />
        <Text class="font-mono text-[8px]" style={{ color: upNext().length > 0 ? "#34D399" : HARDWARE.textFaint }}>
          {upNext().length > 0 ? `NEXT: ${upNext().length} 首待播` : "QUEUE EMPTY"}
        </Text>
      </View>
      <View class="gap-1 rounded-lg p-2" style={{ backgroundColor: "rgba(0,0,0,0.28)" }}>
        <View class="flex-row items-center gap-2">
          <Led lit={current().state === "playing"} tone="amber" size={5} />
          <Text class="min-w-0 flex-1 line-clamp-1 text-[11px] font-medium" style={{ color: HARDWARE.text }}>
            {currentTitle()}
          </Text>
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
            {positionClock()}
          </Text>
        </View>
        <If when={nextTrack()}>
          {(track) => (
            <View class="flex-row items-center gap-2">
              <View style={{ width: 5 }} />
              <Text class="min-w-0 flex-1 line-clamp-1 text-[11px]" style={{ color: HARDWARE.textDim }}>
                ↳ {track().title || "—"}
              </Text>
              <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
                {clock(track().durationMs, true)}
              </Text>
            </View>
          )}
        </If>
      </View>
    </View>
  );
}

/** The whole 05 module. */
export function TelemetryStage() {
  return (
    <View class="gap-2.5">
      <Telemetry />
      <Audit />
      <TransportStrip />
    </View>
  );
}
