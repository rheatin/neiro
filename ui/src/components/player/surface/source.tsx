// 01 // SOURCE IN — the input bus monitor.
//
// 布局照参考稿：左边一条 OLED 屏（文件、格式徽标、RATE/CH、CODEC），右边一块
// PPM 电平监视（L/R 两条**横向**电平条 + dBFS 标尺 + 峰值读数）。顶栏右侧是
// 两个格式徽标：当前的 PCM 规格，以及这台机器/这条流能达到的 DSD 规格。
//
// L/R 是电平表，不是控件：它们显示引擎自己测的峰值（visualiser 帧的 `peak`），
// 没有可调的东西。所以它们画成表（无边框、无 pointer、无 hover），并且
// 明确印着 `PPM LEVEL (dBFS)` 和 dB 刻度——看得懂它是读数，不是滑杆。

import { Animated, createAnimatedValue, createMotion, effect, reduceMotion, Text, View } from "nsh";
import { codecDisplayName, isLossless, MetaChip, NowPlayingBadges } from "../../ui/format-badges";
import { HARDWARE, Led, ModuleHeader, Screen, Silkscreen } from "../../ui/hardware";
import { Button } from "nsh/base";
import { player } from "../../../state/player";
import { engine, reportEngineFailure } from "../../../backend/engine";
import {
  applyPolicy,
  codecLabel,
  currentDither,
  currentDsdPolicy,
  currentRate,
  currentShare,
  isDsdFile,
  displayName,
  dsdFamily,
  dsdRateLabel,
  formatHz,
  isDsdSource,
  isStreamBitPerfect,
  policyJson,
  resolvedPath,
  sourceBits,
  sourceDsdRate,
  sourceFormatLabel,
  type DsdPolicy,
} from "../../../state/resolved";

/** The PPM ladder's floor, dBFS. */
const FLOOR_DB = -48;
const SEGMENTS = 32;

/** One PPM bar: a horizontal strip of `SEGMENTS` segments, lit from the left
 * up to the level. `label` names the measurement it draws — `PEAK` or `RMS`,
 * which is what the engine actually reports (one of each for the stream). */
function PpmBar(props: { level: number; label: string }) {
  const db = () => (props.level > 0 ? 20 * Math.log10(Math.max(1e-6, props.level)) : FLOOR_DB);
  const lit = () => Math.max(0, Math.min(SEGMENTS, Math.round(((db() - FLOOR_DB) / -FLOOR_DB) * SEGMENTS)));
  return (
    <View class="flex-row items-center gap-1.5">
      <Text class="w-8 font-mono text-[8px] font-bold" style={{ color: HARDWARE.textFaint }}>
        {props.label}
      </Text>
      <View class="flex-1 flex-row gap-[1px]">
        <Each of={Array.from({ length: SEGMENTS }, (_, i) => i)} key="index">
          {(index) => {
            const on = () => index() < lit();
            // The ladder's own grammar: green body, amber above -12 dB, red at
            // the top three segments.
            const tone = () =>
              index() >= SEGMENTS - 3 ? "#F1676C" : index() >= SEGMENTS - 8 ? HARDWARE.amber : "#3FB950";
            return (
              <View
                style={{
                  flex: 1,
                  height: 5,
                  borderRadius: 1,
                  backgroundColor: on() ? tone() : "#171A1D",
                  boxShadow: on() ? [{ x: 0, y: 0, blur: 2, color: tone() }] : undefined,
                }}
              />
            );
          }}
        </Each>
      </View>
    </View>
  );
}

/** The PPM monitor: one peak ladder, the dB scale, and the RMS as a reading.
 *
 * One ladder, not two. The engine's visualiser tap reports a single broadband
 * `peak` and a single `rms` (`NAE_SpectrumInfo`) — there is no per-channel
 * number in it, so a left/right pair would be an invention. The RMS is shown
 * as a number beside the peak because that is what it is: a different measure
 * of the same signal, not a second channel.
 */
function PpmMonitor(props: {
  peak: number;
  /** Per-channel peaks when the engine reports them; `null` otherwise. */
  peakL: number | null;
  peakR: number | null;
  rms: number;
  playing: boolean;
}) {
  /** Two ladders only when the engine actually reports two channels. A pair
   * drawn from one broadband number would be an invention. */
  const stereo = () => props.peakL !== null && props.peakR !== null;
  const dbOf = (level: number) => (level > 0 ? 20 * Math.log10(Math.max(1e-6, level)) : null);
  const db = () => dbOf(props.peak);
  const rmsDb = () => dbOf(props.rms);
  const reading = (value: number | null) => (value === null ? "—" : `${value.toFixed(1)} dB`);
  return (
    <View class="flex-1 justify-between gap-2">
      <View class="flex-row items-center justify-between">
        <View class="flex-row items-center gap-1.5">
          <Silkscreen tone="faint">PPM LEVEL</Silkscreen>
          {/* Says whether this is a stereo pair or one broadband reading. */}
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
            {stereo() ? "STEREO" : "MONO"}
          </Text>
        </View>
        {/* Both numbers in the header: the two bars are the reading, these are
            the figures a listener quotes. */}
        <Text class="font-mono text-[9px]" style={{ color: db() === null ? HARDWARE.textFaint : HARDWARE.amberBright }}>
          {`PEAK ${reading(db())} · RMS ${reading(rmsDb())}`}
        </Text>
      </View>
      <If when={stereo()} else={<PpmBar level={props.peak} label="PEAK" />}>
        <PpmBar level={props.peakL ?? 0} label="L" />
        <PpmBar level={props.peakR ?? 0} label="R" />
      </If>
      {/* The scale, placed on the same ladder the bar is drawn on: a mark sits
          where its value is, not at an even fraction of the width. Evenly
          spaced labels over a dB ladder make the bar look like it grows past
          the mark it has reached. */}
      <View style={{ height: 10, marginLeft: 38 }}>
        <Each of={[0, -6, -12, -24, -48]} key="index">
          {(mark) => {
            const at = () => (mark() - FLOOR_DB) / -FLOOR_DB;
            return (
              <View style={{ position: "absolute", left: `${at() * 100}%`, marginLeft: -10, width: 20, alignItems: "center" }}>
                <Text class="font-mono text-[8px]" style={{ color: mark() === 0 ? "rgba(245,166,35,0.8)" : HARDWARE.textFaint }}>
                  {mark() === 0 ? "0dB" : String(mark())}
                </Text>
              </View>
            );
          }}
        </Each>
      </View>
    </View>
  );
}

/** The pulse that runs along the wire while the stream is bit-perfect: the
 * signal reaching the far end unchanged. Module scope — this component is
 * re-invoked on every visualiser frame, and a motion started inside it would
 * restart on each one. */
const wirePulse = createMotion();
let wirePulseStarted = false;

function startWirePulse(): void {
  if (wirePulseStarted) return;
  wirePulseStarted = true;
  void wirePulse.play({
    keyframes: { translateX: [-22, 22], opacity: [0, 1, 0] },
    duration: 1900,
    easing: "linear",
    iterations: "infinite",
  });
}

/** One end of the chain: a plate with its own light, not a bordered label.
 * The fill is lit from the top, the foot casts a shadow, and the LED says
 * whether this end is live — the same grammar the console's keycaps use, so
 * the rate boxes belong to the same instrument. */
function RateBox(props: {
  label: string;
  value: string;
  tone: "in" | "out";
  lit: boolean;
  /** The stream reached here untouched: the plate takes the clean edge. */
  clean?: boolean;
}) {
  const amber = props.tone === "in";
  return (
    <View
      class="flex-row items-center gap-2 rounded-md px-2.5"
      style={{
        // Wide enough for the longest rate this shows (`44.1 kHz` at 13pt in
        // the mono face); at 92 it wrapped and the second line was cut off.
        width: 108,
        height: 40,
        backgroundImage: amber
          ? "linear-gradient(180deg, rgba(245,166,35,0.22) 0%, rgba(245,166,35,0.07) 55%, rgba(245,166,35,0.03) 100%)"
          : "linear-gradient(180deg, rgba(255,255,255,0.12) 0%, rgba(255,255,255,0.04) 55%, rgba(255,255,255,0.015) 100%)",
        borderWidth: 1,
        borderColor: props.clean
          ? "rgba(63,185,80,0.85)"
          : amber
            ? "rgba(245,166,35,0.78)"
            : "rgba(255,255,255,0.26)",
        boxShadow: [
          { x: 0, y: 2, blur: 0, color: "#0A0B0D" },
          { x: 0, y: 1, blur: 0, color: "rgba(255,255,255,0.16)", inset: true },
          ...(amber ? [{ x: 0, y: 0, blur: 14, color: "rgba(245,166,35,0.22)" }] : []),
        ],
      }}
      accessibilityLabel={`${props.label} ${props.value}`}
    >
      <Led lit={props.lit} tone={props.clean ? "emerald" : amber ? "amber" : "emerald"} size={5} />
      <View class="min-w-0 flex-1 gap-0.5">
        <Text class="font-mono text-[8px] font-bold tracking-[0.18em]" style={{ color: HARDWARE.textFaint }}>
          {props.label}
        </Text>
        <Text
          class="whitespace-nowrap font-mono text-[13px] font-bold tabular-nums"
          style={{ color: amber ? HARDWARE.amberBright : HARDWARE.text }}
        >
          {props.value}
        </Text>
      </View>
    </View>
  );
}

/** A format chip. `amber` is the stream's own format (a filled, lit chip);
 * `dim` is a format the stream is *not* in — an outline only, deliberately
 * quieter, so it cannot be read as an active state. */
function FormatChip(props: { text: string; tone: "amber" | "dim" }) {
  const dim = () => props.tone === "dim";
  return (
    <View
      class="rounded px-2 py-1"
      style={{
        backgroundColor: dim() ? "transparent" : "rgba(245,166,35,0.10)",
        borderWidth: 1,
        borderColor: dim() ? "#24282C" : "rgba(245,166,35,0.55)",
        borderStyle: dim() ? "dashed" : undefined,
      }}
    >
      <Text
        class="font-mono text-[9px] tracking-wide"
        style={{
          color: dim() ? "#4E545B" : HARDWARE.amberBright,
          fontWeight: dim() ? "400" : "600",
        }}
      >
        {props.text}
      </Text>
    </View>
  );
}

/** The whole 01 module. */
export function SourceStage(props: {
  peak: number;
  peakL: number | null;
  peakR: number | null;
  rms: number;
  playing: boolean;
}) {
  const path = () => resolvedPath();
  const track = () => player();
  const dsd = () => isDsdSource();
  const name = () => displayName(track().path, track().title);
  const pcmChip = () => {
    const p = path();
    if (!p) return "PCM —";
    if (isDsdFile()) return `DSD ${formatHz(sourceDsdRate())} / 1-bit`;
    const depth = sourceBits();
    const rate = formatHz(p.sourceRate || p.sampleRate);
    return depth > 0 ? `PCM ${rate} / ${depth}-bit` : `PCM ${rate}`;
  };
  const dsdChip = () => {
    const p = path();
    const dsdHz = p ? (p.dsdRate > 0 ? p.dsdRate : isDsdFile() ? sourceDsdRate() : 0) : 0;
    if (dsdHz > 0) return `${dsdFamily(dsdHz)} ${dsdRateLabel(dsdHz)} / 1-bit`;
    // No DSD source: the chip says what would be carried, not what is. It is
    // drawn as an outline only (see `FormatChip`'s `dim` tone), which reads as
    // "not in this stream" instead of a lit state.
    return "DSD OFF";
  };
  /** The file's container, from its extension: the one fact the engine's
   * resolved path does not carry, and the one that explains the bit rate. */
  const containerLabel = () => {
    const p = track().path ?? "";
    const match = p.match(/\.([a-z0-9]+)$/i);
    if (!match) return "—";
    const ext = match[1].toUpperCase();
    // The codec is the engine's own answer; the extension only names the
    // box. A bare extension was wrong for `.m4a`, which carries AAC and ALAC.
    const codec = track().codec;
    return codec === "" ? ext : `${ext} · ${codecDisplayName(codec)}`;
  };
  /** The engine's own bit rate for the stream, in kbps. A container that does
   * not report one (WAV, AIFF) says so rather than showing a zero. */
  const bitrateLabel = () => {
    const kbps = track().bitrateKbps;
    if (kbps === null || kbps <= 0) return "—";
    return `${Math.round(kbps)} kbps`;
  };
  /** The channel layout as a listener reads it: 2.0 stereo, 1.0 mono, 5.1.
   * The engine reports a count; the pair is what the count means. */
  const channelLabel = () => {
    const p = path();
    const ch = p?.sourceChannels || p?.channels || 0;
    if (ch <= 0) return "—";
    if (ch === 1) return "1.0 MONO";
    if (ch === 2) return "2.0 STEREO";
    if (ch === 6) return "5.1";
    if (ch === 8) return "7.1";
    return `${ch} CH`;
  };
  /** The depth the source actually carries, as opposed to the output device. */
  const depthLabel = () => {
    const p = path();
    if (!p) return "—";
    if (isDsdFile()) return "1-BIT";
    const bits = sourceBits();
    return bits > 0 ? `${bits}-BIT` : "—";
  };
  /** The file's facts, one chip each. The two names are solid (the library's
   * own container mark); the three measurements are outlined, so a name and a
   * number never look like the same kind of claim. */
  const metaChips = () => {
    const rows: { label: string; tone: "solid" | "quiet" }[] = [];
    const container = containerLabel();
    const codec = track().codec;
    if (container !== "—") {
      const [ext] = container.split(" · ");
      rows.push({ label: ext, tone: "solid" });
      // The codec plate only says something the container plate does not: an
      // mp3 in an mp3 is two ways of writing the same fact.
      const named = codec === "" ? "" : codecDisplayName(codec);
      if (named !== "" && named.toUpperCase() !== ext.toUpperCase()) {
        rows.push({ label: named, tone: "solid" });
      }
    }
    const depth = depthLabel();
    if (depth !== "—") {
      rows.push({ label: depth, tone: "quiet" });
    }
    rows.push({ label: channelLabel(), tone: "quiet" });
    rows.push({ label: bitrateLabel(), tone: "quiet" });
    return rows;
  };

  /** How much the resampler changed the rate, as the factor it multiplied by.
   * `null` when the stream is not being resampled — then the signal takes the
   * arrow with nothing in it, which is the honest picture of a bypass. */
  /** Bit-perfect, as authoritative single-owner state reports it. */
  const bitPerfect = () => isStreamBitPerfect();
  /** A pulse is only honest while a stream is actually running. */
  const pulseRunning = () => bitPerfect() && props.playing && path()?.resampling !== true;

  const srcRatio = () => {
    const p = path();
    if (!p || !p.resampling) return null;
    const src = p.sourceRate || p.sampleRate;
    const out = p.sampleRate;
    if (src <= 0 || out <= 0 || src === out) return "×1";
    return out > src ? `×${(out / src).toFixed(out / src >= 10 ? 0 : 1).replace(/\.0$/, "")}` : `÷${(src / out).toFixed(src / out >= 10 ? 0 : 1).replace(/\.0$/, "")}`;
  };
  return (
    <View class="gap-2">
      <ModuleHeader
        index="01"
        title="SOURCE IN"
        right={
          <View class="flex-row items-center gap-1.5">
            <Silkscreen tone="faint">DECODER INPUT BUS</Silkscreen>
            <FormatChip text={pcmChip()} tone={dsd() ? "dim" : "amber"} />
            <FormatChip text={dsdChip()} tone={dsd() ? "amber" : "dim"} />
          </View>
        }
      />
      <Screen class="p-3">
        {/* Row one: what this is. The name on the left, everything the file
            says about itself on the right — one line, read left to right. */}
        <View class="flex-row items-center gap-2 pb-2.5">
          <Led lit={props.playing} tone="amber" size={7} />
          <Text class="min-w-0 shrink line-clamp-1 text-[13px] font-bold" style={{ color: HARDWARE.text }}>
            {name()}
          </Text>
          <View class="flex-1" />
          {/* Each fact gets its own chip: a container, a codec, a depth, a
              layout and a weight are five different kinds of statement, and
              one grey sentence made them read as a single string. */}
          <View class="shrink-0 flex-row items-center gap-1">
            <Each of={metaChips()}>
              {(chip) => <MetaChip label={chip().label} tone={chip().tone} />}
            </Each>
            {/* HI-RES and LOSSLESS only: the resampling factor is already the
                `SRC ×2` box in the chain below, and saying it twice is what
                made the badge row look like a stray third row. */}
            <NowPlayingBadges hideCodec wrap={false} class="ml-1" />
          </View>
        </View>

        {/* Row two: how it travels, and how loud it is. Divided, because they
            are two different questions about the same stream. */}
        <View class="flex-row items-stretch gap-3">
          {/* Left: the clock and the bus, centred in the height the meter
              takes. The two ends are boxes of the same size, so the chain reads
              as [IN] —[SRC]— [OUT] rather than as two labels with numbers
              drifting beside them. */}
          <View class="min-w-0 flex-1 justify-center">
            <View class="flex-row items-center" style={{ gap: 4 }}>
              <RateBox
                label="IN"
                value={path() ? formatHz(path()!.sourceRate) : "—"}
                tone="in"
                lit={props.playing}
                clean={bitPerfect()}
              />
              {/* The resampler sits on the wire between the two plates, where
                  it acts. A bypass is the wire alone. */}
              <View class="flex-row items-center" style={{ gap: 3 }}>
                <View
                  style={{
                    width: 8,
                    height: bitPerfect() ? 2 : 1,
                    backgroundColor: bitPerfect() ? HARDWARE.amber : "rgba(255,255,255,0.22)",
                    boxShadow: bitPerfect() ? [{ x: 0, y: 0, blur: 6, color: "rgba(245,166,35,0.6)" }] : undefined,
                  }}
                />
                {/* The pulse rides the wire only when nothing was resampled,
                    and the SRC box appears only when something was — they can
                    never both be here, so neither needs a reserved slot. */}
                <If when={pulseRunning()}>
                  <View class="items-center justify-center" style={{ width: 26, height: 14 }}>
                    <View style={wirePulse.style}>
                      <View
                        style={{
                          width: 4,
                          height: 4,
                          borderRadius: 2,
                          backgroundColor: HARDWARE.amberBright,
                          boxShadow: [{ x: 0, y: 0, blur: 5, color: "rgba(245,166,35,0.9)" }],
                        }}
                      />
                    </View>
                  </View>
                </If>
                <If when={srcRatio() !== null}>
                  <View
                    class="items-center justify-center rounded"
                    style={{
                      height: 20,
                      paddingLeft: 5,
                      paddingRight: 5,
                      borderWidth: 1,
                      borderColor: "rgba(245,166,35,0.7)",
                      backgroundColor: "rgba(245,166,35,0.12)",
                    }}
                  >
                    <Text class="font-mono text-[9px] font-bold tracking-wider" style={{ color: HARDWARE.amberBright }}>
                      {`SRC ${srcRatio()}`}
                    </Text>
                  </View>
                </If>
                <View
                  style={{
                    width: 8,
                    height: bitPerfect() ? 2 : 1,
                    backgroundColor: bitPerfect() ? HARDWARE.amber : "rgba(255,255,255,0.22)",
                    boxShadow: bitPerfect() ? [{ x: 0, y: 0, blur: 6, color: "rgba(245,166,35,0.6)" }] : undefined,
                  }}
                />
              </View>
              <RateBox
                label="OUT"
                value={path() ? formatHz(path()!.sampleRate) : "—"}
                tone="out"
                lit={props.playing && (path()?.sampleRate ?? 0) > 0}
                clean={bitPerfect()}
              />
            </View>
          </View>

          {/* Right: the meter. A level readout, not a control. */}
          <View class="min-w-0 flex-1">
            <PpmMonitor
              peak={props.peak}
              peakL={props.peakL}
              peakR={props.peakR}
              rms={props.rms}
              playing={props.playing}
            />
          </View>
        </View>
      </Screen>
      <If when={dsd()}>
        <View class="px-1 flex-row items-center justify-between">
          <Silkscreen tone="faint">
            {path()?.dsdPcmFallback
              ? "DSD → PCM 抽取（Bit-Perfect 已关闭）"
              : path()?.dsdDownrated
                ? `降速至 ${dsdFamily(path()!.dsdRate)}`
                : path()?.dsdTransport === "dop"
                  ? "DoP 载波直通"
                  : "Native DSD 直通"}
          </Silkscreen>
          <View class="flex-row items-center gap-1">
            <Silkscreen tone="amber">POLICY:</Silkscreen>
            <Each of={(["auto", "dop", "pcm"] as const)}>
              {(mode) => {
                const active = () => currentDsdPolicy() === mode();
                return (
                  <Button
                    accessibilityLabel={`DSD 策略 ${mode().toUpperCase()}`}
                    onPress={() => {
                      currentDsdPolicy.set(mode());
                      const rate = currentRate();
                      const share = currentShare();
                      const dither = currentDither();
                      try {
                        applyPolicy(rate.kind, rate.fixedRate, share, dither, mode());
                      } catch (error) {
                        reportEngineFailure(error, "设置 DSD 策略失败");
                      }
                    }}
                    class={`rounded px-1.5 py-0.5 border text-[9px] font-mono font-bold ${
                      active()
                        ? "border-amber-500 bg-amber-500/20 text-amber-500"
                        : "border-border/60 text-muted-foreground hover:bg-muted"
                    }`}
                  >
                    {mode().toUpperCase()}
                  </Button>
                );
              }}
            </Each>
          </View>
        </View>
      </If>
    </View>
  );
}
