// 02B // 32-BAND RACK EQ & INDEPENDENT OPRA ENGINE.
//
// The rack's screen draws four lines at once, and says which is which:
//
//   cyan     the manual 32-band curve (what the user drew)
//   magenta  the OPRA / AutoEQ correction (a separate layer)
//   amber    the sum the engine receives
//   dashed   the 0 dB target the AUTO-GAIN button aims at
//
// The engine has one EQ chain, so the amber line is the only one that reaches
// it: the manual and OPRA curves are kept apart in the app and summed on the
// way out. That is why switching headphones never rewrites the faders — the
// stored manual curve is never the sum.
//
// Two gestures: one fader (drag, double-click to zero) and a horizontal
// sweep across the whole rack, which draws a curve under the pointer. The
// sweep is the rack's own trick: it is what a hardware graphic EQ cannot do.

import { Image, signal, Text, View } from "nsh";
import { Scroll } from "nsh";
import { Button, Input } from "nsh/base";
import { HARDWARE, Led, MechKey, Silkscreen } from "../../ui/hardware";
import { ActionButton, focusRing } from "../../ui/controls";
import { AppDialog } from "../../ui/overlays";
import { Icon } from "../../ui/icons";
import { notify } from "../../../state/feedback";
import { playSnap, playTick } from "../../../state/foley";
import {
  EQ_FREQS,
  EQ_GAIN_MAX,
  EQ_GAIN_MIN,
  autoGainCompensate,
  currentPreset,
  effectiveBands,
  eqBands,
  eqDirty,
  eqEnabled,
  eqGains,
  eqHasB,
  eqLive,
  eqPresets,
  eqPreamp,
  applyPreset,
  resetEq,
  savePreset,
  setBandGain,
  setEqEnabled,
  setEqPreamp,
  switchAB,
} from "../../../state/eq";
import {
  currentPoints,
  deleteOpraProfile,
  importAutoEqCsv,
  opraActive,
  opraGainAt,
  opraProfiles,
  opraSelected,
  selectOpraProfile,
  setOpraActive,
} from "../../../state/opra";

const F_MIN = 20;
const F_MAX = 20000;
const PLOT_DB = 12;

const saveOpen = signal(false);
const importOpen = signal(false);

/** Where a frequency sits across the plot, 0..1 on the log axis. */
function logPosition(hz: number): number {
  return Math.log(hz / F_MIN) / Math.log(F_MAX / F_MIN);
}

function fmtFreq(hz: number): string {
  if (hz >= 1000) {
    const k = hz / 1000;
    return `${Number.isInteger(k) ? k : k.toFixed(1)}k`;
  }
  return `${Math.round(hz)}`;
}

function fmtGain(db: number): string {
  return `${db > 0 ? "+" : ""}${db.toFixed(1)}`;
}

/** A curve's path in the plot's own pixels.
 *
 * The SVG is drawn at the box's measured size and its viewBox matches, so the
 * scale is 1:1 in both axes: a 1.2pt stroke stays 1.2pt. Drawing in a 100×100
 * viewBox and stretching it would scale the stroke by the box's width, which
 * turned every curve into a 6pt slab (the renderer has no
 * `vector-effect="non-scaling-stroke"`). */
function curvePathPx(
  gains: number[],
  preampDb: number,
  lo: number,
  hi: number,
  width: number,
  height: number,
): string {
  const logMin = Math.log(F_MIN);
  const logSpan = Math.log(F_MAX / F_MIN);
  return sampleFreqs()
    .map((hz, i) => {
      const x = ((Math.log(hz) - logMin) / logSpan) * width;
      const db = Math.max(lo, Math.min(hi, curveDbAt(gains, preampDb, hz)));
      const y = (1 - (db - lo) / (hi - lo)) * height;
      return `${i === 0 ? "M" : "L"}${x.toFixed(1)} ${y.toFixed(1)}`;
    })
    .join(" ");
}

/** The OPRA correction as a path in the plot's pixels. */
function opraPathPx(lo: number, hi: number, width: number, height: number): string {
  const points = currentPoints();
  if (points.length === 0) return "";
  const logMin = Math.log(F_MIN);
  const logSpan = Math.log(F_MAX / F_MIN);
  return sampleFreqs()
    .map((hz, i) => {
      const x = ((Math.log(hz) - logMin) / logSpan) * width;
      const db = Math.max(lo, Math.min(hi, opraGainAt(hz, points)));
      const y = (1 - (db - lo) / (hi - lo)) * height;
      return `${i === 0 ? "M" : "L"}${x.toFixed(1)} ${y.toFixed(1)}`;
    })
    .join(" ");
}

/** The curve's dB at `hz`, from the 32 bands treated as bells. */
function curveDbAt(gains: number[], preampDb: number, hz: number): number {
  let db = preampDb;
  for (let b = 0; b < EQ_FREQS.length; b++) {
    const centre = Math.log(EQ_FREQS[b]);
    db += gains[b] * Math.exp(-Math.pow((Math.log(hz) - centre) / 0.18, 2));
  }
  return db;
}

/** The frequencies the plot is sampled at: a dense log sweep plus every band
 * centre, so a narrow bell's peak is never stepped over. */
function sampleFreqs(): number[] {
  const list: number[] = [];
  const steps = 240;
  for (let i = 0; i <= steps; i++) list.push(F_MIN * Math.pow(F_MAX / F_MIN, i / steps));
  for (const hz of EQ_FREQS) list.push(hz);
  return list.sort((a, b) => a - b);
}

/** The vertical range the three curves need, with a dB of headroom and a
 * floor of 6 dB so a flat curve is not magnified into noise. */
function plotRange(): { lo: number; hi: number } {
  const manual = eqGains();
  const sum = effectiveBands().map((band) => band.gainDb);
  const pre = eqPreamp();
  const points = currentPoints();
  const opra = opraActive();
  let lo = 0;
  let hi = 0;
  for (const hz of sampleFreqs()) {
    const m = curveDbAt(manual, 0, hz);
    const t = curveDbAt(sum, pre, hz);
    if (m < lo) lo = m;
    if (m > hi) hi = m;
    if (t < lo) lo = t;
    if (t > hi) hi = t;
    if (opra) {
      const o = opraGainAt(hz, points);
      if (o < lo) lo = o;
      if (o > hi) hi = o;
    }
  }
  const span = Math.max(6, hi - lo);
  const mid = (hi + lo) / 2;
  return { lo: Math.round(mid - span / 2 - 1), hi: Math.round(mid + span / 2 + 1) };
}

/** The screen: four lines over the dB/frequency grid. */
function RackScreen() {
  const size = signal({ width: 640, height: 208 });
  const range = () => plotRange();
  const zeroY = () => {
    const { lo, hi } = range();
    return (1 - (0 - lo) / (hi - lo)) * size().height;
  };
  const manual = () => eqGains();
  const sum = () => effectiveBands().map((band) => band.gainDb);
  const svg = () => {
    const { lo, hi } = range();
    const { width, height } = size();
    return (
      `<svg xmlns="http://www.w3.org/2000/svg" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}">` +
      // the 0 dB target
      `<line x1="0" y1="${zeroY().toFixed(1)}" x2="${width}" y2="${zeroY().toFixed(1)}" stroke="#5C6169" stroke-width="1" stroke-dasharray="3 3"/>` +
      // the OPRA layer
      (opraActive() ? `<path d="${opraPathPx(lo, hi, width, height)}" fill="none" stroke="#F472B6" stroke-width="1.4" stroke-linejoin="round"/>` : "") +
      // the manual curve
      `<path d="${curvePathPx(manual(), 0, lo, hi, width, height)}" fill="none" stroke="#38BDF8" stroke-width="1.4" stroke-linejoin="round"/>` +
      // the sum the engine receives, with the area under it filled so a flat
      // rack still reads as a plot rather than as an empty screen
      `<path d="${curvePathPx(sum(), eqPreamp(), lo, hi, width, height)} L${width} ${zeroY().toFixed(1)} L0 ${zeroY().toFixed(1)} Z" fill="rgba(245,166,35,0.10)" stroke="none"/>` +
      `<path d="${curvePathPx(sum(), eqPreamp(), lo, hi, width, height)}" fill="none" stroke="#F5A623" stroke-width="2" stroke-linejoin="round"/>` +
      `</svg>`
    );
  };
  // The ruler a graphic EQ is read against: ±12 dB in 6 dB steps, and a line
  // on every band so a fader and its curve position line up by eye.
  const ticks = () => [12, 6, 0, -6, -12].filter((db) => db <= range().hi && db >= range().lo);
  const yOf = (db: number) => {
    const { lo, hi } = range();
    return (1 - (db - lo) / (hi - lo)) * 100;
  };
  /** The summed response's highest point, and the trim that would take it
   * back to 0 dB — the two numbers a listener needs before touching preamp. */
  const combinedPeak = () => Math.max(...sum(), 0);
  return (
    <View
      class="relative w-full overflow-hidden rounded-md"
      style={{
        height: 208,
        backgroundImage:
          "radial-gradient(ellipse at 50% 0%, rgba(245,166,35,0.05) 0%, transparent 70%), linear-gradient(180deg, #0A0B0D, #0A0B0D)",
        borderWidth: 1,
        borderColor: HARDWARE.screenBorder,
        boxShadow: [
          { x: 0, y: 3, blur: 14, color: "#000000f5", inset: true },
          { x: 0, y: 0, blur: 0, spread: 1, color: "#000000d9", inset: true },
        ],
      }}
      onLayout={(e) => {
        const width = Math.max(1, Math.round(e.bounds.size.width));
        const height = Math.max(1, Math.round(e.bounds.size.height));
        if (width !== size().width || height !== size().height) size.set({ width, height });
      }}
    >
      <Each of={ticks()} key="index">
        {(db) => {
          const { lo, hi } = range();
          return (
            <View
              class="absolute left-0 right-0"
              style={{
                top: `${(1 - (db() - lo) / (hi - lo)) * 100}%`,
                height: 1,
                backgroundColor: db() === 0 ? "rgba(255,255,255,0.14)" : "rgba(255,255,255,0.055)",
              }}
            />
          );
        }}
      </Each>
      <Each of={EQ_FREQS} key="index">
        {(_, index) => (
          <View
            class="absolute bottom-0 top-0"
            style={{
              left: `${logPosition(EQ_FREQS[index()]) * 100}%` as const,
              width: 1,
              backgroundColor: index() % 4 === 0 ? "rgba(255,255,255,0.09)" : "rgba(255,255,255,0.035)",
            }}
          />
        )}
      </Each>
      <Image
        src={`data:image/svg+xml,${encodeURIComponent(svg())}`}
        accessibilityElementsHidden
        style={{ position: "absolute", left: 0, top: 0, width: "100%", height: "100%" }}
      />
      {/* The axis labels, printed on the screen. */}
      <Each of={ticks()} key="index">
        {(db) => (
          <View class="absolute left-1.5" style={{ top: Math.max(1, (yOf(db()) / 100) * size().height - 5) }}>
            <Text class="font-mono text-[8px]" style={{ color: db() === 0 ? HARDWARE.textDim : HARDWARE.textFaint }}>
              {db() === 0 ? "0" : `${db() > 0 ? "+" : ""}${db()}`}
            </Text>
          </View>
        )}
      </Each>

      <View class="absolute right-2 top-1.5 items-end">
        <Text class="font-mono text-[9px]" style={{ color: combinedPeak() > 0.05 ? HARDWARE.amberBright : HARDWARE.textDim }}>
          {`COMBINED PEAK ${fmtGain(combinedPeak())} dB · AUTO-TRIM ${fmtGain(-combinedPeak())} dB`}
        </Text>
      </View>
    </View>
  );
}

/** One of the 32 faders. Its own drag: vertical, geared down when slow, with
 * the 0 dB detent. */
function RackFader(props: { index: number; height: number }) {
  const band = () => eqBands()[props.index];
  const gain = () => band()?.gainDb ?? 0;
  const detented = () => Math.abs(gain()) < 0.05;
  return (
    <View class="items-center gap-0.5" style={{ width: 17 }}>
      <Text class="font-mono text-[7px]" style={{ color: detented() ? HARDWARE.textFaint : HARDWARE.amberBright }}>
        {detented() ? "·" : fmtGain(gain())}
      </Text>
      <View
        class="items-center justify-center"
        style={{ height: props.height, width: 15 }}
        onPress={(e: { clickCount: number }) => {
          // Double click zeroes this band; a single press belongs to the
          // matrix's paint gesture.
          if (e.clickCount < 2) return;
          setBandGain(props.index, 0, true);
          playSnap();
        }}
      >
        <View
          class="items-center"
          style={{
            width: 13,
            height: props.height,
            borderRadius: 2,
            backgroundColor: "#0A0B0D",
            borderWidth: 1,
            borderColor: "#1E2126",
            boxShadow: [{ x: 0, y: 2, blur: 4, color: "#000000f2", inset: true }],
          }}
        >
          {/* the datum line */}
          <View style={{ position: "absolute", left: 0, right: 0, top: props.height / 2, height: 1, backgroundColor: detented() ? "rgba(245,166,35,0.9)" : "rgba(245,166,35,0.3)" }} />
          {/* the cap */}
          <View
            style={{
              position: "absolute",
              top: (1 - (gain() + 12) / 24) * (props.height - 10),
              width: 13,
              height: 10,
              borderRadius: 2,
              backgroundImage: "linear-gradient(180deg, #3D4049 0%, #23262B 48%, #16181C 52%, #2B2D34 100%)",
              borderWidth: 1,
              borderColor: detented() ? HARDWARE.amberBright : "#484C57",
              boxShadow: detented()
                ? [{ x: 0, y: 0, blur: 6, color: "rgba(245,166,35,0.45)" }]
                : [{ x: 0, y: 2, blur: 4, color: "#000000cc" }],
            }}
          />
        </View>
      </View>
      <Text class="font-mono text-[7px]" style={{ color: HARDWARE.textFaint }}>
        {fmtFreq(EQ_FREQS[props.index])}
      </Text>
    </View>
  );
}

/** The whole rack: the 32 faders in one row, with the sweep gesture over the
 * row as a whole. */
function RackFaders() {
  const HEIGHT = 96;
  /** Which fader the sweep is on, and the gain it last wrote. */
  const sweep = signal<{ index: number; db: number } | null>(null);
  const bandAt = (pageX: number, left: number, width: number) => {
    const t = Math.max(0, Math.min(1, (pageX - left) / width));
    return Math.min(EQ_FREQS.length - 1, Math.floor(t * EQ_FREQS.length));
  };
  const layout = signal({ left: 0, width: 1 });
  const dbAt = (pageY: number, top: number, height: number) => {
    const t = Math.max(0, Math.min(1, (pageY - top) / height));
    return Math.max(EQ_GAIN_MIN, Math.min(EQ_GAIN_MAX, (1 - t) * 24 - 12));
  };
  const box = signal({ top: 0, height: HEIGHT });
  return (
    <View
      class="w-full flex-row justify-between"
      onLayout={(e) => {
        layout.set({ left: e.bounds.origin.x, width: Math.max(1, e.bounds.size.width) });
        box.set({ top: e.bounds.origin.y, height: HEIGHT });
      }}
      onPressDown={(e: { pagePosition: { x: number; y: number } }) => {
        // One press paints one band at the pointer's height; holding and
        // moving across the row draws the curve.
        const index = bandAt(e.pagePosition.x, layout().left, layout().width);
        const db = dbAt(e.pagePosition.y, box().top, box().height);
        sweep.set({ index, db });
        setBandGain(index, db, false);
        playTick();
      }}
      onMouseMove={(e: { pagePosition: { x: number; y: number }; pressedButton: string | null }) => {
        const held = sweep();
        if (e.pressedButton === null) {
          if (held) sweep.set(null);
          return;
        }
        // The sweep is armed by the row's own press handler below; while it
        // runs, every move paints the fader under the pointer.
        if (!held) return;
        const index = bandAt(e.pagePosition.x, layout().left, layout().width);
        const db = dbAt(e.pagePosition.y, box().top, box().height);
        if (index === held.index && Math.abs(db - held.db) < 0.1) return;
        sweep.set({ index, db });
        setBandGain(index, db, false);
        playTick();
      }}
      onPressUp={() => {
        if (sweep()) {
          sweep.set(null);
          notify("曲线已手绘");
        }
      }}
    >
      <Each of={EQ_FREQS} key="index">
        {(_, index) => <RackFader index={index()} height={HEIGHT} />}
      </Each>
    </View>
  );
}

/** The OPRA slot: its own switch, the profile selector and the CSV import.
 * It never touches the manual bands. */
function OpraSlot() {
  return (
    <View class="gap-1.5 rounded-lg p-2" style={{ backgroundColor: "rgba(0,0,0,0.28)" }}>
      <View class="flex-row items-center gap-1.5">
        <Led lit={opraActive()} tone="magenta" size={6} />
        <Silkscreen tone="faint">OPRA // 独立耳机校正槽</Silkscreen>
        <View class="flex-1" />
        <Text class="font-mono text-[8px]" style={{ color: opraActive() ? "#FF70AE" : HARDWARE.textFaint }}>
          {opraActive() ? "ACTIVE" : "BYPASS"}
        </Text>
      </View>
      <Text class="text-[8px]" style={{ color: HARDWARE.textFaint }}>
        独立补偿层，与上方 32 段手动 EQ 相加后送引擎；切换或导入曲线不改写手动推子。
      </Text>
      <Scroll horizontal scrollbar={false} class="w-full" contentClass="flex-row gap-1 pb-1">
        <Each of={opraProfiles()} key={(profile) => profile.name}>
          {(profile) => {
            const on = () => opraSelected() === profile().name;
            return (
              <View class="flex-row items-center">
                <MechKey
                  label={`耳机曲线 ${profile().name}`}
                  tone="magenta"
                  lit={on() && opraActive()}
                  onPress={() => selectOpraProfile(profile().name)}
                  class="h-6 flex-row items-center gap-1 rounded px-2"
                >
                  <Text class="font-mono text-[8px]" style={{ color: on() ? "#FF70AE" : HARDWARE.textDim }}>
                    {profile().name}
                  </Text>
                  <If when={!profile().builtin}>
                    <Text
                      accessibilityLabel={`删除曲线 ${profile().name}`}
                      class="font-mono text-[9px]"
                      style={{ color: HARDWARE.textFaint }}
                      onPress={() => deleteOpraProfile(profile().name)}
                    >
                      ×
                    </Text>
                  </If>
                </MechKey>
              </View>
            );
          }}
        </Each>
      </Scroll>
      <View class="flex-row items-center gap-1.5">
        <MechKey
          label={opraActive() ? "旁通 OPRA" : "启用 OPRA"}
          tone="magenta"
          lit={opraActive()}
          onPress={() => setOpraActive(!opraActive())}
          class="h-6 items-center justify-center rounded px-2"
        >
          <Text class="font-mono text-[8px] font-bold" style={{ color: opraActive() ? "#FF70AE" : HARDWARE.textDim }}>
            {opraActive() ? "OPRA ACTIVE" : "OPRA BYPASS"}
          </Text>
        </MechKey>
        <MechKey
          label="导入 AutoEQ CSV"
          onPress={() => importOpen.set(true)}
          class="h-6 items-center justify-center rounded px-2"
        >
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textDim }}>
            导入 AutoEQ CSV
          </Text>
        </MechKey>
      </View>
    </View>
  );
}

/** The rack's preset row: presets, save, reset and the A/B switch. */
function RackPresets() {
  return (
    <View class="gap-1.5">
      <View class="flex-row items-center gap-1.5">
        <Silkscreen tone="faint">PRESET</Silkscreen>
        <If when={eqDirty()}>
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.amber }}>EDITED</Text>
        </If>
        <View class="flex-1" />
        <MechKey label="A/B 对比" lit={eqLive() === "B"} onPress={switchAB} class="h-6 items-center justify-center rounded px-2">
          <Text class="font-mono text-[8px] font-bold" style={{ color: eqLive() === "B" ? HARDWARE.amberBright : HARDWARE.textDim }}>
            A/B · {eqLive()}
          </Text>
        </MechKey>
        <MechKey label="另存为预设" onPress={() => saveOpen.set(true)} class="h-6 items-center justify-center rounded px-2">
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textDim }}>+ 另存为</Text>
        </MechKey>
        <MechKey label="重置均衡器" onPress={resetEq} class="h-6 items-center justify-center rounded px-2">
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textDim }}>重置</Text>
        </MechKey>
      </View>
      <Scroll horizontal scrollbar={false} class="w-full" contentClass="flex-row gap-1 pb-1">
        <Each of={eqPresets()} key={(preset) => preset.name}>
          {(preset) => {
            const on = () => currentPreset() === preset().name;
            return (
              <MechKey
                label={`预设 ${preset().name}`}
                lit={on()}
                onPress={() => applyPreset(preset().name)}
                class="h-6 items-center justify-center rounded px-2"
              >
                <Text class="font-mono text-[8px]" style={{ color: on() ? HARDWARE.amberBright : HARDWARE.textDim }}>
                  {preset().name}
                </Text>
              </MechKey>
            );
          }}
        </Each>
      </Scroll>
    </View>
  );
}

/** 另存为：名字走一个小输入框，存的是当前的 32 段与预增益。 */
function SavePresetDialog() {
  const name = signal("");
  const go = () => {
    const value = name().trim();
    if (!value) return;
    savePreset(value);
    name.set("");
    saveOpen.set(false);
  };
  return (
    <AppDialog open={saveOpen()} onOpenChange={(open) => saveOpen.set(open)} testID="rack-save-preset">
      <View class="gap-3 p-5">
        <Text role="header" class="text-lg font-bold text-foreground">另存为预设</Text>
        <Text class="text-xs text-muted-foreground">保存当前的 32 段增益与预增益。同名会覆盖你自己存的预设。</Text>
        <View class="h-9 flex-row items-center gap-2 rounded-(--radius) border border-border bg-secondary px-3">
          <Input
            value={name()}
            onChange={({ value }) => name.set(value)}
            placeholder="预设名"
            accessibilityLabel="预设名"
            autoCapitalize="none"
            autoCorrect={false}
            autoFocus
            class="flex-1 text-sm text-foreground"
          />
        </View>
        <View class="flex-row items-center justify-between">
          <Text class="text-xs text-muted-foreground" onPress={() => saveOpen.set(false)}>取消</Text>
          <ActionButton label="保存" disabled={!name().trim()} onPress={go} />
        </View>
      </View>
    </AppDialog>
  );
}

/** 导入 AutoEQ CSV：粘贴一段 `frequency,gain` 文本，或选一个文件。 */
function ImportCsvDialog() {
  const text = signal("");
  const name = signal("");
  const error = signal("");
  const go = () => {
    const result = importAutoEqCsv(text(), name());
    if (!result.ok) {
      error.set(result.error ?? "导入失败");
      return;
    }
    error.set("");
    text.set("");
    name.set("");
    importOpen.set(false);
    notify(`已导入 ${result.points} 个校正点`);
  };
  return (
    <AppDialog open={importOpen()} onOpenChange={(open) => importOpen.set(open)} testID="rack-import-csv">
      <View class="gap-3 p-5">
        <Text role="header" class="text-lg font-bold text-foreground">导入 AutoEQ CSV</Text>
        <Text class="text-xs text-muted-foreground">
          每行 `频率,增益`（表头行会被跳过）。导入的曲线成为独立配置，不改写手动 EQ。
        </Text>
        <View class="h-9 flex-row items-center gap-2 rounded-(--radius) border border-border bg-secondary px-3">
          <Input
            value={name()}
            onChange={({ value }) => name.set(value)}
            placeholder="曲线名（例如我的耳机）"
            accessibilityLabel="曲线名"
            autoCapitalize="none"
            autoCorrect={false}
            class="flex-1 text-sm text-foreground"
          />
        </View>
        <View class="min-h-24 rounded-(--radius) border border-border bg-secondary p-2">
          <Input
            value={text()}
            onChange={({ value }) => text.set(value)}
            placeholder={"frequency,gain\n20,3.5\n100,1.2\n1000,0"}
            accessibilityLabel="CSV 内容"
            multiline
            autoCapitalize="none"
            autoCorrect={false}
            class="min-h-20 w-full font-mono text-[11px] text-foreground"
          />
        </View>
        <If when={error() !== ""}>
          <Text class="text-xs" style={{ color: "#F1676C" }}>{error()}</Text>
        </If>
        <View class="flex-row items-center justify-between">
          <Text class="text-xs text-muted-foreground" onPress={() => importOpen.set(false)}>取消</Text>
          <ActionButton label="导入" disabled={text().trim() === ""} onPress={go} />
        </View>
      </View>
    </AppDialog>
  );
}

/** The rack page's body: mounted by the 输出信息 drawer when the EQ stage is
 * expanded, and by the standalone 均衡器 page. */
export function RackEq() {
  const peak = () => {
    const gains = effectiveBands().map((band) => band.gainDb);
    return Math.max(...gains, 0);
  };
  return (
    <View class="gap-2.5">
      <View class="flex-row items-center gap-1.5">
        <Silkscreen tone="amber">RACK UNIT A</Silkscreen>
        <Text class="text-[10px] font-bold uppercase tracking-[0.14em]" style={{ color: HARDWARE.textDim }}>
          32-BAND GRAPHIC EQ & INDEPENDENT OPRA ENGINE
        </Text>
        <View class="flex-1" />
        <MechKey
          label={eqEnabled() ? "关闭均衡器" : "开启均衡器"}
          lit={eqEnabled()}
          onPress={() => {
            // The stage's own switch; the bands stay as they are.
            setEqEnabled(!eqEnabled());
          }}
          class="h-6 items-center justify-center rounded px-2"
        >
          <Text class="font-mono text-[8px] font-bold" style={{ color: eqEnabled() ? HARDWARE.amberBright : HARDWARE.textDim }}>
            {eqEnabled() ? "EQ ON" : "EQ BYP"}
          </Text>
        </MechKey>
        <MechKey label="自动增益补偿" onPress={autoGainCompensate} class="h-6 items-center justify-center rounded px-2">
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textDim }}>AUTO-GAIN</Text>
        </MechKey>
      </View>
      <RackScreen />
      <View class="flex-row flex-wrap items-center gap-3 px-1">
        <View class="flex-row items-center gap-1">
          <View style={{ width: 10, height: 2.5, backgroundColor: HARDWARE.amber, borderRadius: 1 }} />
          <Text class="text-[9px]" style={{ color: HARDWARE.amber }}>总 DSP 合成响应</Text>
        </View>
        <View class="flex-row items-center gap-1">
          <View style={{ width: 10, height: 1.5, backgroundColor: "#38BDF8", borderRadius: 1 }} />
          <Text class="text-[9px]" style={{ color: "#38BDF8" }}>手动 32 段 EQ（含前级）</Text>
        </View>
        <If when={opraActive()}>
          <View class="flex-row items-center gap-1">
            <View style={{ width: 10, height: 1.5, backgroundColor: "#F472B6", borderRadius: 1 }} />
            <Text class="text-[9px]" style={{ color: "#F472B6" }}>OPRA 耳机补偿（独立不干扰）</Text>
          </View>
        </If>
        <View class="flex-1" />
        <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
          PEAK {fmtGain(peak())} dB · PREAMP {fmtGain(eqPreamp())} dB
        </Text>
      </View>
      <View class="flex-row items-center gap-2 px-1">
        <View class="flex-1" />
        <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
          {EQ_FREQS.length} BANDS · 1/3 OCT
        </Text>
      </View>
      <RackPresets />
      <View class="gap-1 rounded-lg p-2" style={{ backgroundColor: "rgba(0,0,0,0.28)" }}>
        <View class="flex-row items-center gap-1.5">
          <Text class="text-[9px] font-bold uppercase tracking-wider" style={{ color: HARDWARE.textDim }}>
            32-BAND 1/3-OCTAVE FADER MATRIX
          </Text>
          <Text class="font-mono text-[8px]" style={{ color: HARDWARE.textFaint }}>
            20 Hz – 20 kHz
          </Text>
          <View class="flex-1" />
          <Text class="text-[8px]" style={{ color: HARDWARE.textFaint }}>
            单根精调 · 按住横向扫过手绘曲线 · 双击单根归零
          </Text>
        </View>
        <RackFaders />
      </View>
      <OpraSlot />
      <SavePresetDialog />
      <ImportCsvDialog />
    </View>
  );
}
