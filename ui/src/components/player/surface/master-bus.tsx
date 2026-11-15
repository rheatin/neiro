// 03 // BIT-PERFECT MASTER BUS — four verification lamps and the big direct
// switch.
//
// The four lamps are the four facts a bit-perfect claim rests on, each read
// from the engine's own report (never from what the UI asked for):
//
//   DSP CLEAN   every stage the engine applied is bypassed (EQ, crossfeed,
//               ReplayGain, convolver, crossfade), and no OPRA correction is
//               in the path
//   RATE MATCH  the output rate equals the source rate, or the stream is a
//               DSD bitstream carried as DSD
//   EXCL STREAM the endpoint is exclusive, not the system mixer
//   UNITY 0dB   the engine's applied volume is 1.0
//
// The big switch performs the whole cleanup in one action and is the only
// control that touches all four at once.

import { Animated, createAnimatedValue, effect, reduceMotion, Text, View } from "nsh";
import { HARDWARE, Led, MechKey, Silkscreen } from "../../ui/hardware";
import { engine, reportEngineFailure } from "../../../backend/engine";
import { notify } from "../../../state/feedback";
import { playRelay } from "../../../state/foley";
import { player, setVolume } from "../../../state/player";
import { opraActive, opraSelected } from "../../../state/opra";
import { isDsdSource, policyJson, resolvedPath } from "../../../state/resolved";

/** One verification lamp: a sunk card with the LED over its two-line name,
 * as the reference panel draws it. */
function Lamp(props: { line1: string; line2: string; pass: boolean; amber?: boolean }) {
  const colour = () => (props.pass ? "#34D399" : props.amber ? HARDWARE.amber : "#F1676C");
  return (
    <View
      class="flex-1 items-center gap-1.5 rounded-md py-2.5"
      style={{
        backgroundColor: "rgba(0,0,0,0.3)",
        borderWidth: 1,
        borderColor: props.pass ? "rgba(52,211,153,0.35)" : "rgba(255,255,255,0.07)",
      }}
      accessibilityLabel={`${props.line1} ${props.line2} ${props.pass ? "通过" : "未通过"}`}
    >
      <View
        style={{
          width: 13,
          height: 13,
          borderRadius: 3,
          backgroundColor: props.pass ? "#34D399" : props.amber ? HARDWARE.amber : "#3A3F45",
          boxShadow: props.pass
            ? [{ x: 0, y: 0, blur: 7, color: "#34D399" }, { x: 0, y: 0, blur: 16, color: "rgba(52,211,153,0.5)" }]
            : props.amber
              ? [{ x: 0, y: 0, blur: 6, color: "rgba(245,166,35,0.7)" }]
              : undefined,
          borderWidth: 1,
          borderColor: props.pass ? "#6EE7B7" : "#22262A",
        }}
      />
      <Text class="line-clamp-1 shrink-0 text-center font-mono text-[8px] font-bold tracking-[0.08em]" style={{ color: props.pass ? HARDWARE.text : HARDWARE.textDim }}>
        {props.line1}
      </Text>
      <Text class="line-clamp-1 shrink-0 text-center font-mono text-[8px] font-bold tracking-[0.08em]" style={{ color: props.pass ? HARDWARE.text : HARDWARE.textDim }}>
        {props.line2}
      </Text>
    </View>
  );
}

/** The four checks, as the engine reports them. */
export function bitPerfectChecks() {
  const path = () => resolvedPath();
  const rp = () => engine.renderPath();
  const dspClean = () => {
    const p = path();
    const r = rp();
    if (!p && !r) return false;
    if (opraActive()) return false;
    if (r) {
      return !r.eqEnabled && !r.crossfeedEnabled && !r.replayGainEnabled && !r.convolverEnabled && !r.crossfadeEnabled;
    }
    return !p!.eqActive && !p!.crossfeedActive && !p!.convolverActive && !p!.crossfadeActive;
  };
  const rateMatch = () => {
    const p = path();
    if (!p) return false;
    if (isDsdSource()) return p.dsdTransport !== "none" && !p.dsdPcmFallback;
    return p.sourceRate > 0 && p.sourceRate === p.sampleRate && !p.resampling;
  };
  const exclusive = () => exclusiveOf(path());
  const unity = () => {
    const p = rp();
    return !!p && Math.abs(p.volume - 1) < 0.0005;
  };
  return { dspClean, rateMatch, exclusive, unity };
}

/** Whether the engine is on an exclusive stream. The resolved path carries
 * `NAE_SHARE_MODE_*`: 1 exclusive, 2 shared. */
function exclusiveOf(path: ReturnType<typeof resolvedPath>): boolean {
  return !!path && path.shareMode === 1;
}

/** The whole 03 module. */
export function MasterBus() {
  const checks = bitPerfectChecks();
  const bitPerfect = () => (engine.renderPath()?.bitPerfect ?? false) && checks.dspClean();
  /** The blocking causes, as short panel lines: the engine's reason code and
   * the app's own OPRA layer. */
  const causes = (): string[] => {
    const out: string[] = [];
    const reason = engine.renderPath()?.reason ?? 0;
    const named: Record<number, string> = {
      1: "RESAMPLED TO DEVICE RATE",
      2: "RESAMPLED TO FIXED RATE",
      3: "DITHERED (WORD LENGTH)",
      4: "CHANNEL CONVERTED",
      5: "DEVICE CANNOT CARRY FORMAT",
      6: "RESAMPLED TO SUPPORTED RATE",
      7: "SHARED MODE (SYSTEM MIXER)",
      8: "VOLUME NOT UNITY",
      9: "32-BAND EQ ACTIVE",
      10: "CROSSFEED ACTIVE",
      11: "REPLAYGAIN ACTIVE",
      12: "CONVOLVER ACTIVE",
      13: "CROSSFADE ACTIVE",
      14: "SOURCE PRECISION (32-BIT INT IN FLOAT32 PATH)",
      15: "CLIP GUARD ACTIVE (LIMITER)",
    };
    if (!bitPerfect() && reason !== 0) out.push(named[reason] ?? `REASON ${reason}`);
    if (!bitPerfect() && reason === 0 && (engine.renderPath()?.crossfadeEnabled || engine.settings()?.crossfadeEnabled)) {
      out.push("CROSSFADE ACTIVE");
    }
    if (opraActive()) out.push(`OPRA HEADPHONE CALIBRATION (${opraSelected()})`);
    return out;
  };
  const reason = () => {
    const d = engine.player().resolvedJson;
    if (!d) return "";
    try {
      const raw = JSON.parse(d) as { diagnostics?: { message?: string }[] };
      const rows = (raw.diagnostics ?? []).filter((row) => row.message);
      return rows.length > 0 ? rows[rows.length - 1].message ?? "" : "";
    } catch {
      return "";
    }
  };
  // The verified badge: a slow amber breath while the claim holds.
  const glow = createAnimatedValue(0.4);
  effect(() => {
    if (!bitPerfect() || reduceMotion()) {
      glow.set(bitPerfect() ? 0.85 : 0.3);
      return;
    }
    const run = Animated.loop(
      Animated.sequence([
        Animated.timing(glow, { toValue: 1, duration: 1100, easing: "easeInOut" }),
        Animated.timing(glow, { toValue: 0.55, duration: 1100, easing: "easeInOut" }),
      ]),
    );
    void run.start();
    return () => run.stop();
  });

  /** The direct switch: bypass every DSP stage, take the endpoint
   * exclusively, and pin the volume at unity. Each step is a real command;
   * a step the engine refuses is reported and the rest still run. */
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
    // 1. Every DSP stage off, the bands kept (the user's own curve is not
    //    discarded — it is only bypassed).
    attempt("关闭 EQ", () => engine.setEq(false, s.eqPreampDb, s.eqBands));
    attempt("关闭 Crossfeed", () => engine.setCrossfeed(false, s.crossfeedAmount, s.crossfeedDelayMs, s.crossfeedCutoffHz));
    attempt("关闭 ReplayGain", () => engine.setReplayGain(0, s.replayGainPreampDb, s.replayGainPreventClipping, s.replayGainFallbackDb));
    attempt("关闭 Convolver", () => engine.setConvolver(false, s.convolverIrPath, s.convolverWet, s.convolverDry, s.convolverGainDb, s.convolverNormalize));
    attempt("关闭 Crossfade", () => engine.setCrossfade(false, s.crossfadeDurationMs, s.crossfadeCurve));
    // 2. The endpoint, exclusively, at the source rate.
    attempt("切独占 / 源速率", () => engine.setPolicy(policyJson("native", 0, "exclusive")));
    // 3. Unity gain.
    attempt("音量归 100%", () => setVolume(1));
    if (failures.length === 0) notify("Bit-Perfect 直通已请求：DSP 旁路、独占、音量 100%");
  };

  return (
    <View class="gap-2">
      {/* One row: the four lamps, then the one switch. The lamps are equal
          cards and the switch takes the rest of the row at their height, so
          the three lines of the module land on a single grid. */}
      <View class="flex-row items-stretch gap-2">
        <View class="flex-row gap-1.5" style={{ width: 400 }}>
          <Lamp line1="DSP & EQ" line2="CLEAN" pass={checks.dspClean()} amber={!checks.dspClean()} />
          <Lamp line1="RATE & HW" line2="MATCH" pass={checks.rateMatch()} />
          <Lamp line1="EXCL" line2="STREAM" pass={checks.exclusive()} />
          <Lamp line1="UNITY" line2="0dB" pass={checks.unity()} />
        </View>
        <MechKey
          label="Bit-Perfect 直通"
          lit={bitPerfect()}
          tone="emerald"
          onPress={goDirect}
          class="min-w-0 flex-1 items-center justify-center gap-1 rounded-lg"
        >
          <View class="flex-row items-center gap-2">
            <Led lit={bitPerfect()} tone={bitPerfect() ? "emerald" : "amber"} size={9} />
            <Text class="line-clamp-1 font-mono text-[11px] font-bold tracking-[0.14em]" style={{ color: bitPerfect() ? "#34D399" : HARDWARE.text }}>
              BIT-PERFECT
            </Text>
          </View>
        </MechKey>
      </View>
      {/* The causes: one line each, across the module. */}
      <View class="gap-1">
        <Text class="font-mono text-[8px] tracking-wider" style={{ color: HARDWARE.textFaint }}>
          {causes().length === 0
            ? "NON-BIT-PERFECT ROOT CAUSES: NONE"
            : `NON-BIT-PERFECT ROOT CAUSES (${causes().length}):`}
        </Text>
        <If when={bitPerfect()}>
          <View class="rounded px-2 py-1" style={{ backgroundColor: "rgba(16,185,129,0.10)", borderWidth: 1, borderColor: "rgba(16,185,129,0.45)" }}>
            <Text class="font-mono text-[9px] font-bold" style={{ color: "#34D399" }}>
              ● 整条通路未改动：采样与源一致、独占、音量 100%、所有 DSP 与 OPRA 旁通
            </Text>
          </View>
        </If>
        <Each of={causes()} key="index">
          {(cause) => (
            <View
              class="rounded px-2 py-1"
              style={{
                backgroundColor: "rgba(245,166,35,0.10)",
                borderWidth: 1,
                borderColor: "rgba(245,166,35,0.45)",
              }}
            >
              <Text class="font-mono text-[9px] font-bold" style={{ color: HARDWARE.amberBright }}>
                {cause()}
              </Text>
            </View>
          )}
        </Each>
      </View>
    </View>
  );
}
