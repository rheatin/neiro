// 02 // DSP MATRIX & CHANNEL STRIPS — the mixer surface.
//
// Three control shapes, each chosen by what its parameter is:
//
// - A bipolar continuous gain (`EQ PREAMP`, `REPLAYGAIN`) is a long-throw
//   channel fader. It has a magnetic detent at 0 dB: inside ±0.35 dB the
//   output locks to exactly 0.0, the cap lights, and a snap plays. Slow drags
//   are geared down to 0.25× so a tenth of a dB is reachable with a hand.
// - A discrete choice (`RESAMPLER`, `DITHER`) is a tactile key-bed: the key
//   sinks 1.5pt, the choice is the lit key.
// - A one-way ratio or a time (`CROSSFEED`, `CONVOLVER WET`, `CROSSFADE`) is a
//   knurled encoder with a 15-mark LED ring; vertical drag, wheel, and a
//   double click to reset.
//
// Pointer capture is the app's: `onPressDown` starts a drag, `onMouseMove`
// reports where the pointer is with the button held, `onMouseUp` ends it. The
// pointer may leave the control — the drag continues — because the handlers
// live on the panel around it.

import { effect, reduceMotion, signal, Text, View } from "nsh";
import {
  BusConduit,
  EncoderCap,
  FaderCap,
  FaderSlot,
  HARDWARE,
  Led,
  MechKey,
  ModuleHeader,
  Silkscreen,
} from "../../ui/hardware";
import { engine, reportEngineFailure } from "../../../backend/engine";
import { notify } from "../../../state/feedback";
import { currentDither, currentRate, currentShare, dsdPcmFallback, dsdTransport, isDsdSource, policyJson, resolvedPath, type DitherKind } from "../../../state/resolved";
import { playSnap, playTick } from "../../../state/foley";

/** How far one point of vertical travel moves a full-range control. */
const DRAG_RANGE_PT = 150;
/** Below this speed (points per event) the drag is geared down. */
const SLOW_PT = 1.2;
const SLOW_GEAR = 0.25;
/** The 0 dB detent's half-width, dB. */
const DETENT_DB = 0.35;

/** A drag in progress, shared by every control that uses pointer capture. */
interface Drag {
  /** Where the pointer was last, in the root view. */
  y: number;
  x: number;
  /** The value when the drag began. */
  from: number;
  /** When the last move arrived, to tell a slow drag from a fast one. */
  at: number;
}

/** The channel-strip controls' shared state: which one holds the pointer. */
const dragging = signal<string | null>(null);

/** A vertical drag bound to `key`. Returns the handlers a panel puts on its
 * own box so the pointer is captured past the control's edges. */
function useVerticalDrag(key: string, onDelta: (deltaPt: number, slow: boolean) => void, onEnd: () => void) {
  let drag: Drag | null = null;
  return {
    onPressDown: (e: { pagePosition: { x: number; y: number } }) => {
      dragging.set(key);
      drag = { y: e.pagePosition.y, x: e.pagePosition.x, from: 0, at: Date.now() };
    },
    onMouseMove: (e: { pagePosition: { x: number; y: number }; pressedButton: string | null }) => {
      if (!drag || e.pressedButton === null) return;
      const now = Date.now();
      const dy = drag.y - e.pagePosition.y;
      const dt = Math.max(1, now - drag.at);
      const speed = Math.abs(dy) / dt;
      drag.y = e.pagePosition.y;
      drag.x = e.pagePosition.x;
      drag.at = now;
      if (dy !== 0) onDelta(dy, speed < SLOW_PT / 4);
    },
    onMouseUp: () => {
      if (!drag) return;
      drag = null;
      dragging.set(null);
      onEnd();
    },
  };
}

/** The long-throw fader for a bipolar dB gain. */
export function GainFader(props: {
  label: string;
  /** −12…+12 dB. */
  value: number;
  enabled: boolean;
  disabled?: boolean;
  /** `value` changed (live, cheap). */
  onChange: (db: number) => void;
  /** The release: send to the engine and persist. */
  onCommit: (db: number) => void;
  onToggle: () => void;
  /** The engine refused the stage (DSD lock): the controls dim and the
   * fader cannot move. */
  locked?: boolean;
  height?: number;
}) {
  const height = () => props.height ?? 118;
  const range = 24;
  const toNorm = (db: number) => (db + 12) / range;
  const toDb = (norm: number) => norm * range - 12;
  /** The cap's 0…1 from the top (0 = +12 dB). */
  const position = () => 1 - toNorm(props.value);
  const detented = () => Math.abs(props.value) <= DETENT_DB + 1e-9;
  const handlers = useVerticalDrag(
    props.label,
    (dy, slow) => {
      if (props.locked || props.disabled) return;
      const step = ((slow ? SLOW_GEAR : 1) * dy) / DRAG_RANGE_PT;
      const next = props.value + step * range;
      const clamped = Math.max(-12, Math.min(12, next));
      // The magnetic detent: inside the window the value is exactly 0, so the
      // readout and what the engine receives agree.
      const wasOut = !detented();
      const snapped = Math.abs(clamped) <= DETENT_DB ? 0 : clamped;
      if (wasOut && snapped === 0 && !reduceMotion()) playSnap();
      props.onChange(snapped);
    },
    () => props.onCommit(props.value),
  );
  const zero = () => {
    if (props.locked || props.disabled) return;
    props.onChange(0);
    props.onCommit(0);
    playSnap();
  };
  return (
    <View class="items-center gap-2">
      <MechKey
        label={`${props.label} 开关`}
        lit={props.enabled}
        onPress={props.locked ? undefined : props.onToggle}
        disabled={props.disabled || props.locked}
        class="h-5 items-center justify-center rounded px-2"
      >
        <Text class="font-mono text-[8px] font-bold tracking-wider" style={{ color: props.enabled ? HARDWARE.amberBright : HARDWARE.textDim }}>
          {props.enabled ? "ON" : "BYP"}
        </Text>
      </MechKey>
      <Silkscreen class="text-center">{props.label}</Silkscreen>
      <View class="flex-row items-start" style={{ height: height() }}>
        {/* The dB scale, printed to the left of the slot. */}
        <View class="justify-between pr-1" style={{ height: height() }}>
          <Each of={["+12", "+6", "0.0", "-6", "-12"]} key="index">
            {(mark) => (
              <Text class="font-mono text-[7px]" style={{ color: mark() === "0.0" ? HARDWARE.amber : HARDWARE.textFaint }}>
                {mark()}
              </Text>
            )}
          </Each>
        </View>
        <View class="items-center justify-center" style={{ height: height(), width: 34 }} {...handlers}>
          <FaderSlot height={height()} datum={0.5} lit={!detented()} />
          <View style={{ position: "absolute", top: position() * (height() - 18), left: 0, right: 0, alignItems: "center" }}>
            <FaderCap position={position()} dragging={dragging() === props.label} detented={detented()} width={26} />
          </View>
        </View>
      </View>
      <Text
        class="font-mono text-[11px] tabular-nums"
        style={{
          color: props.locked ? HARDWARE.textFaint : detented() ? HARDWARE.amberBright : HARDWARE.text,
          textShadow: detented() && !props.locked ? "0 0 10 rgba(245,166,35,0.5)" : undefined,
        }}
      >
        {props.value > 0 ? "+" : ""}
        {props.value.toFixed(1)}
      </Text>
      <MechKey label={`${props.label} 归零`} onPress={zero} disabled={props.disabled || props.locked} class="h-5 w-8 items-center justify-center rounded">
        <Text class="font-mono text-[8px] font-bold" style={{ color: HARDWARE.textDim }}>
          0
        </Text>
      </MechKey>
    </View>
  );
}

/** A tactile key-bed for a discrete parameter. */
export function StepKeyBed<T extends string | number>(props: {
  label: string;
  options: { value: T; label: string }[];
  value: T;
  disabled?: boolean;
  locked?: boolean;
  onPick: (value: T) => void;
}) {
  return (
    <View class="gap-1.5">
      <View class="flex-row items-center justify-between">
        <Silkscreen>{props.label}</Silkscreen>
        <If when={props.locked}>
          <Silkscreen tone="faint">LOCK</Silkscreen>
        </If>
      </View>
      <View class="flex-row gap-1">
        <Each of={props.options} key={(option) => String(option.value)}>
          {(option) => {
            const lit = () => option().value === props.value;
            return (
              <MechKey
                label={`${props.label} ${option().label}`}
                lit={lit()}
                disabled={props.disabled || props.locked}
                onPress={() => {
                  if (props.disabled || props.locked) return;
                  if (!lit()) playTick();
                  props.onPick(option().value);
                }}
                class="h-7 flex-1 items-center justify-center rounded"
              >
                <Text
                  class="line-clamp-1 font-mono text-[9px] font-bold tracking-wide"
                  style={{ color: lit() ? HARDWARE.amberBright : HARDWARE.textDim }}
                >
                  {option().label}
                </Text>
              </MechKey>
            );
          }}
        </Each>
      </View>
    </View>
  );
}

/** The 15-mark LED ring around an encoder: lit up to the value. */
function EncoderRing(props: { value: number; size: number; engaged: boolean }) {
  const MARKS = 15;
  const from = -135;
  const to = 135;
  const radius = () => props.size / 2 + 3;
  return (
    <View style={{ position: "absolute", width: props.size + 16, height: props.size + 16 }} accessibilityElementsHidden>
      <Each of={Array.from({ length: MARKS }, (_, i) => i)} key="index">
        {(index) => {
          const angle = from + ((to - from) * index()) / (MARKS - 1);
          const rad = (angle * Math.PI) / 180;
          const lit = () => index() / (MARKS - 1) <= props.value + 0.0001;
          return (
            <View
              class="absolute rounded-full"
              style={{
                width: 2,
                height: lit() ? 4 : 3,
                left: (props.size + 16) / 2 + radius() * Math.sin(rad) - 1,
                top: (props.size + 16) / 2 - radius() * Math.cos(rad),
                backgroundColor: lit() ? (props.engaged ? HARDWARE.amberBright : "rgba(255,255,255,0.22)") : "rgba(255,255,255,0.10)",
                boxShadow: lit() && props.engaged ? [{ x: 0, y: 0, blur: 4, color: HARDWARE.amber }] : undefined,
              }}
            />
          );
        }}
      </Each>
    </View>
  );
}

/** A knurled encoder for a one-way ratio or a time. */
export function RatioEncoder(props: {
  label: string;
  /** 0…1 of the encoder's travel. */
  value: number;
  /** What the readout says for a position. */
  readout: (value: number) => string;
  /** A caption under the readout (unit, or what it scales). */
  unit: string;
  enabled: boolean;
  disabled?: boolean;
  locked?: boolean;
  onToggle: () => void;
  onChange: (value: number) => void;
  onCommit: (value: number) => void;
  /** Double click / the reset key returns here. */
  resetTo?: number;
  size?: number;
}) {
  const size = () => props.size ?? 40;
  const angle = () => -135 + 270 * Math.min(1, Math.max(0, props.value));
  const handlers = useVerticalDrag(
    props.label,
    (dy, slow) => {
      if (props.disabled || props.locked) return;
      const next = props.value + ((slow ? SLOW_GEAR : 1) * dy) / DRAG_RANGE_PT;
      props.onChange(Math.max(0, Math.min(1, next)));
      playTick();
    },
    () => props.onCommit(props.value),
  );
  return (
    <View class="items-center gap-1.5">
      <View
        class="items-center justify-center"
        style={{ width: size() + 16, height: size() + 16 }}
        {...handlers}
        onPress={() => {
          if (props.disabled || props.locked) return;
          props.onChange(props.resetTo ?? 0);
          props.onCommit(props.resetTo ?? 0);
          playSnap();
        }}
      >
        <EncoderRing value={props.value} size={size()} engaged={props.enabled && !props.locked} />
        <EncoderCap size={size()} angle={angle()} engaged={props.enabled && !props.locked} dragging={dragging() === props.label} />
      </View>
      <Silkscreen class="text-center">{props.label}</Silkscreen>
      <Text
        class="font-mono text-[11px] tabular-nums"
        style={{ color: props.locked ? HARDWARE.textFaint : props.enabled ? HARDWARE.amberBright : HARDWARE.textDim }}
      >
        {props.readout(props.value)}
      </Text>
      <Silkscreen tone="faint" class="text-center">{props.unit}</Silkscreen>
      <MechKey
        label={`${props.label} 开关`}
        lit={props.enabled}
        disabled={props.disabled || props.locked}
        onPress={props.onToggle}
        class="h-5 items-center justify-center rounded px-2"
      >
        <Text class="font-mono text-[8px] font-bold" style={{ color: props.enabled ? HARDWARE.amberBright : HARDWARE.textDim }}>
          {props.enabled ? "ON" : "BYP"}
        </Text>
      </MechKey>
    </View>
  );
}

/** The 02 module: two faders, two key-beds, three encoders, and the DSD lock
 * plate over the PCM-only controls. */
export function DspMatrix(props: {
  /** The level for the signal bus between bays. */
  flowing: boolean;
  onOpenEq: () => void;
  eqActive: boolean;
}) {
  const settings = () => engine.settings();
  // A DSD source carried as DSD (DoP or native) bypasses the float DSP
  // chain in the engine, so the PCM-only controls cannot act on it.
  const locked = () => isDsdSource() && dsdTransport() !== "none" && !dsdPcmFallback();
  return (
    <View class="gap-2">
      <ModuleHeader
        index="02"
        title="DSP MATRIX & CHANNEL STRIPS"
        right={
          <MechKey
            label="打开 32 段均衡器机架"
            tone="cyan"
            lit={props.eqActive}
            onPress={props.onOpenEq}
            class="h-6 flex-row items-center gap-1.5 rounded px-2"
          >
            <Text class="font-mono text-[8px] font-bold tracking-wider" style={{ color: HARDWARE.cyan }}>
              32-BAND RACK EQ ▲
            </Text>
          </MechKey>
        }
      />
      <If when={locked()}>
        <View class="px-1 pb-1">
          <Silkscreen tone="faint">DSD 直通中：PCM 处理链旁路，控件锁定（关闭 Bit-Perfect 后可用）</Silkscreen>
        </View>
      </If>
      {/* Two columns, as the reference panel: the two channel strips on the
          left, and on the right the key-beds over the encoders. The faders
          stretch to the right column's full height so the two columns end on
          one line. */}
      <View class="flex-row items-stretch gap-2">
        {/* Left: the two bipolar gains, stretched to the column's height. */}
        <View class="items-center gap-1 rounded-lg p-2" style={{ backgroundColor: "rgba(0,0,0,0.25)", width: 132 }}>
          <Silkscreen tone="amber">CHANNEL STRIPS</Silkscreen>
          <View class="w-full flex-row justify-around gap-1">
            <GainFader
              label="EQ PREAMP"
              value={settings()?.eqPreampDb ?? 0}
              enabled={settings()?.eqEnabled ?? false}
              locked={locked()}
              height={190}
              onChange={() => {}}
              onCommit={(db) => {
                const before = settings();
                if (!before) return;
                try {
                  engine.setEq(before.eqEnabled, db, before.eqBands);
                } catch (error) {
                  reportEngineFailure(error, "EQ 预增益下发失败");
                }
              }}
              onToggle={() => {
                const before = settings();
                if (!before) return;
                try {
                  engine.setEq(!before.eqEnabled, before.eqPreampDb, before.eqBands);
                } catch (error) {
                  reportEngineFailure(error, "EQ 切换失败");
                }
              }}
            />
            <GainFader
              label="R-GAIN"
              value={settings()?.replayGainPreampDb ?? 0}
              enabled={(settings()?.replayGainMode ?? 0) !== 0}
              locked={locked()}
              height={190}
              onChange={() => {}}
              onCommit={(db) => {
                const before = settings();
                if (!before) return;
                try {
                  engine.setReplayGain(
                    before.replayGainMode === 0 ? 1 : before.replayGainMode,
                    db,
                    before.replayGainPreventClipping,
                    before.replayGainFallbackDb,
                  );
                } catch (error) {
                  reportEngineFailure(error, "ReplayGain 下发失败");
                }
              }}
              onToggle={() => {
                const before = settings();
                if (!before) return;
                try {
                  engine.setReplayGain(
                    before.replayGainMode === 0 ? 1 : 0,
                    before.replayGainPreampDb,
                    before.replayGainPreventClipping,
                    before.replayGainFallbackDb,
                  );
                } catch (error) {
                  reportEngineFailure(error, "ReplayGain 切换失败");
                }
              }}
            />
          </View>
        </View>
        {/* Right: the key-beds above, the encoders below them. */}
        <View class="min-w-0 flex-1 gap-2">
          <View class="gap-3 rounded-lg p-2" style={{ backgroundColor: "rgba(0,0,0,0.25)" }}>
            <ResamplerBed locked={locked()} />
            <View class="pt-2">
              <StepKeyBed
                label="DITHER"
                locked={locked()}
                value={ditherKey()}
                options={DITHER_KEYS}
                onPick={(value) => setDitherKey(value)}
              />
            </View>
            <Silkscreen tone="faint">OUTPUT CLOCK / WORD LENGTH</Silkscreen>
          </View>
          <View class="gap-1 rounded-lg p-2" style={{ backgroundColor: "rgba(0,0,0,0.25)" }}>
            <View class="flex-row w-full items-center justify-between">
              <Silkscreen tone="amber">MODULATION & SPATIAL</Silkscreen>
              <Silkscreen tone="faint">DRAG ↑↓</Silkscreen>
            </View>
            <View class="flex-row w-full justify-around">
              <RatioEncoder
                label="X-FEED"
                unit="AMOUNT"
                value={settings()?.crossfeedAmount ?? 0}
                readout={(v) => v.toFixed(2)}
                enabled={settings()?.crossfeedEnabled ?? false}
                locked={locked()}
                onChange={() => {}}
                onCommit={(v) => {
                  const before = settings();
                  if (!before) return;
                  try {
                    engine.setCrossfeed(before.crossfeedEnabled, v, before.crossfeedDelayMs, before.crossfeedCutoffHz);
                  } catch (error) {
                    reportEngineFailure(error, "Crossfeed 下发失败");
                  }
                }}
                onToggle={() => {
                  const before = settings();
                  if (!before) return;
                  try {
                    engine.setCrossfeed(!before.crossfeedEnabled, before.crossfeedAmount, before.crossfeedDelayMs, before.crossfeedCutoffHz);
                  } catch (error) {
                    reportEngineFailure(error, "Crossfeed 切换失败");
                  }
                }}
              />
              <RatioEncoder
                label="CONV IR"
                unit="WET"
                value={settings()?.convolverWet ?? 0}
                readout={(v) => v.toFixed(2)}
                enabled={settings()?.convolverEnabled ?? false}
                locked={locked()}
                onChange={() => {}}
                onCommit={(v) => {
                  const before = settings();
                  if (!before) return;
                  try {
                    engine.setConvolver(before.convolverEnabled, before.convolverIrPath, v, before.convolverDry, before.convolverGainDb, before.convolverNormalize);
                  } catch (error) {
                    reportEngineFailure(error, "Convolver 下发失败");
                  }
                }}
                onToggle={() => {
                  const before = settings();
                  if (!before) return;
                  // No impulse response yet: the switch is the way to load one.
                  if (!before.convolverIrPath) {
                    void (async () => {
                      try {
                        const picked = await engine.pickIrFile();
                        if (!picked) return;
                        engine.setConvolver(true, picked, before.convolverWet, before.convolverDry, before.convolverGainDb, before.convolverNormalize);
                        notify(`已加载脉冲响应：${picked.split(/[\\/]/).pop() ?? picked}`);
                      } catch (error) {
                        reportEngineFailure(error, "加载脉冲响应失败");
                      }
                    })();
                    return;
                  }
                  try {
                    engine.setConvolver(!before.convolverEnabled, before.convolverIrPath, before.convolverWet, before.convolverDry, before.convolverGainDb, before.convolverNormalize);
                  } catch (error) {
                    reportEngineFailure(error, "Convolver 切换失败");
                  }
                }}
              />
              <RatioEncoder
                label="X-FADE"
                unit="SEC"
                value={(settings()?.crossfadeDurationMs ?? 0) / 12000}
                readout={(v) => `${(v * 12).toFixed(1)}`}
                enabled={settings()?.crossfadeEnabled ?? false}
                locked={locked()}
                resetTo={0}
                onChange={() => {}}
                onCommit={(v) => {
                  const before = settings();
                  if (!before) return;
                  try {
                    engine.setCrossfade(before.crossfadeEnabled, Math.round(v * 12) * 1000, before.crossfadeCurve);
                  } catch (error) {
                    reportEngineFailure(error, "Crossfade 下发失败");
                  }
                }}
                onToggle={() => {
                  const before = settings();
                  if (!before) return;
                  try {
                    engine.setCrossfade(!before.crossfadeEnabled, before.crossfadeDurationMs, before.crossfadeCurve);
                  } catch (error) {
                    reportEngineFailure(error, "Crossfade 切换失败");
                  }
                }}
              />
            </View>
          </View>
        </View>
      </View>
    </View>
  );
}

// ---- The resampler and dither keys ------------------------------------------
//
// The resampler's key-bed is built from the endpoint's own capability list —
// never from a fixed array. Base probes the rates by really opening the
// endpoint, keeps them on `DeviceInfo.supportedSampleRates`, and this bed
// renders exactly those: a rate the device refuses is not offered, and a
// flagship DAC's 384/768 kHz appear when it reports them.
//
// `OFF` is always the first key and means "do not resample": the engine opens
// at the source rate (its `native` policy), which is the only setting that can
// claim bit-perfectness.
//
// Dither is the engine's own automatic decision — it dithers when the output
// container is narrower than the valid bits — and no command asks for a depth.
// The bed therefore reports what happened and is not turnable.

/** A rate key is the device's own Hz, or `off` for "no resampling". */
type RateKey = "off" | number;

/** Whether `hz` is an exact 2×/4×/8× multiple of `base`. */
function integerMultiple(hz: number, base: number): number | null {
  if (base <= 0 || hz <= base) return null;
  for (const factor of [2, 4, 8]) {
    if (Math.abs(hz - base * factor) < 1) return factor;
  }
  return null;
}

/** The source rate the multiples are measured against: the file's own rate, or
 * 44.1 kHz while a DSD bitstream is being extracted (DSD is 44.1-family). */
function referenceRate(): number {
  const path = resolvedPath();
  if (!path) return 0;
  if (isDsdSource()) return 44100;
  return path.sourceRate || path.sampleRate;
}

function fmtRate(hz: number): string {
  const k = hz / 1000;
  return `${Number.isInteger(k) ? k : k.toFixed(1)}k`;
}

/** The chosen rate for the current endpoint: `off`, or one of its rates.
 *
 * A stored choice is validated against the endpoint's list every time it is
 * read — that is the fallback guard. Switching from a DAC that took 384 kHz to
 * an onboard endpoint that tops out at 48 kHz therefore lands on `off` rather
 * than on a rate the new endpoint would refuse. */
const chosenRate = signal<number | null>(null);

function selectedEndpoint(): string {
  return resolvedPath()?.deviceId ?? "";
}

/** The rates the endpoint the stream is on reports. When no stream is open the
 * engine's current default is the one being configured, so the first device in
 * the list that matches nothing still answers with its own list. */
function deviceRates(): number[] {
  const devices = engine.devices();
  const id = selectedEndpoint();
  const row = devices.find((device) => device.id === id) ?? devices[0];
  return row ? [...row.supportedSampleRates].sort((a, b) => a - b) : [];
}

/** What the engine resolved: the rate mode actually in force. */
function resolvedRateKey(): RateKey {
  const path = engine.renderPath();
  if (!path) return "off";
  if (!path.resampling) return "off";
  return path.sampleRate || "off";
}

/** The key that is lit. A stored choice wins only while the endpoint still
 * offers it; otherwise the resolved value is the truth. */
function rateKey(): RateKey {
  const chosen = chosenRate();
  if (chosen !== null) {
    const supported = deviceRates();
    if (supported.length === 0 || supported.includes(chosen)) return chosen;
    return "off";
  }
  const resolved = resolvedRateKey();
  if (resolved === "off") return "off";
  const supported = deviceRates();
  return supported.length === 0 || supported.includes(resolved) ? resolved : "off";
}

function chooseRate(key: RateKey): void {
  // Only the rate changes: the share mode is carried from the stream as it is.
  // (This used to hardcode `"shared"`, so picking a rate silently dropped an
  // exclusive stream into the system mixer.)
  const share = currentShare();
  chosenRate.set(key === "off" ? null : key);
  try {
    if (key === "off") {
      engine.setPolicy(policyJson("native", 0, share));
    } else {
      engine.setPolicy(policyJson("fixed", key, share));
    }
  } catch (error) {
    chosenRate.set(null);
    reportEngineFailure(error, "切换输出时钟失败");
  }
}

/** The key-bed: `OFF` then the endpoint's rates, one row while they fit in
 * five, two rows past that (a flagship DAC reports eight or more). */
function ResamplerBed(props: { locked: boolean }) {
  const rates = () => deviceRates();
  const keys = (): RateKey[] => ["off", ...rates()];
  const twoRows = () => keys().length > 6;
  // Split into two even rows: 4+4 for eight, 5+4 for nine, and so on.
  const rows = (): RateKey[][] => {
    const all = keys();
    if (!twoRows()) return [all];
    const half = Math.ceil(all.length / 2);
    return [all.slice(0, half), all.slice(half)];
  };
  const reference = () => referenceRate();
  return (
    <View class="gap-1.5">
      <View class="flex-row items-center justify-between">
        <Silkscreen>RESAMPLER</Silkscreen>
        <View class="flex-row items-center gap-1.5">
          <If when={currentShare() === "shared"}>
            {/* The system mixer in the path resamples anything that is not its
                own rate: saying which rate that is explains a second SRC. */}
            <Silkscreen tone="faint">{`OS MIXER: ${pathInputRate()}`}</Silkscreen>
          </If>
          <If when={props.locked}>
            <Silkscreen tone="faint">LOCK</Silkscreen>
          </If>
        </View>
      </View>
      <If
        when={rates().length > 0}
        else={
          <Text class="px-1 text-[8px]" style={{ color: HARDWARE.textFaint }}>
            端点未探测：停止播放后按 ENUM 重新枚举，键组按它报告的采样率生成
          </Text>
        }
      >
        <View class="gap-1">
          <Each of={rows()} key="index">
            {(row) => (
              <View class="flex-row gap-1">
                <Each of={row()} key={(key) => String(key)}>
                  {(key) => {
                    const lit = () => rateKey() === key();
                    const multiple = () => (key() === "off" ? null : integerMultiple(key() as number, reference()));
                    return (
                      <MechKey
                        label={key() === "off" ? "RESAMPLER OFF" : `RESAMPLER ${fmtRate(key() as number)}`}
                        lit={lit()}
                        disabled={props.locked}
                        onPress={() => {
                          if (props.locked) return;
                          if (!lit()) playTick();
                          chooseRate(key());
                        }}
                        class="h-7 flex-1 items-center justify-center rounded"
                      >
                        <View class="items-center">
                          <Text
                            class="line-clamp-1 font-mono text-[9px] font-bold tracking-wide"
                            style={{ color: lit() ? HARDWARE.amberBright : HARDWARE.textDim }}
                          >
                            {key() === "off" ? "OFF" : fmtRate(key() as number)}
                          </Text>
                        </View>
                        {/* The integer-multiple corner mark: this rate is an
                            exact ×2/×4/×8 of the source. */}
                        <If when={multiple() !== null}>
                          <View
                            class="absolute rounded-sm px-0.5"
                            style={{
                              top: 1,
                              right: 1,
                              backgroundColor: "rgba(56,189,248,0.16)",
                              borderWidth: 1,
                              borderColor: "rgba(56,189,248,0.5)",
                            }}
                          >
                            <Text class="font-mono text-[6px] font-bold" style={{ color: "#7DD3FC" }}>
                              {`×${multiple()}`}
                            </Text>
                          </View>
                        </If>
                      </MechKey>
                    );
                  }}
                </Each>
              </View>
            )}
          </Each>
        </View>
      </If>
    </View>
  );
}

/** The rate the OS mixer is running at, as the panel prints it. The resolved
 * path's `sampleRate` is what the device runs at; in shared mode that is also
 * the mixer's rate. */
function pathInputRate(): string {
  const path = resolvedPath();
  if (!path || path.sampleRate <= 0) return "—";
  return fmtRate(path.sampleRate);
}

/** The dither algorithm keys. The depth is the engine's own negotiation (the
 * widest container the endpoint takes), so this bed names the algorithm. */
type DitherKey = "auto" | "tpdf" | "shaped";
const DITHER_KEYS: { value: DitherKey; label: string }[] = [
  { value: "auto", label: "OFF" },
  { value: "tpdf", label: "TPDF" },
  { value: "shaped", label: "SHAPED" },
];

/** The algorithm the stream applies, read back from the engine. */
function ditherKey(): DitherKey {
  const mode = currentDither();
  if (mode === "off") return "auto";
  return mode === "noise_shaped" ? "shaped" : "tpdf";
}

/** Asks the engine for an algorithm. It is an open-time setting, so a live
 * stream reopens at the current position — which is what a rate change does
 * too, and the panel's own lamps report what came back. */
function setDitherKey(key: DitherKey): void {
  const mode: DitherKind = key === "auto" ? "off" : key === "shaped" ? "noise_shaped" : "tpdf";
  const rate = currentRate();
  try {
    engine.setPolicy(policyJson(rate.kind, rate.fixedRate, currentShare(), mode));
  } catch (error) {
    reportEngineFailure(error, "切换抖动算法失败");
  }
}
