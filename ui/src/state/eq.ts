// The equaliser: one owner for its bands, its presets and the engine call.
//
// 布局是应用自己的（引擎不发明频段：`eq_bands` 收到什么就回什么），所以
// 这里是 10 段 + preamp 的图形 EQ——预设正是围绕这套频段才有意义。预设存
// 在 Storage；引擎只收到 enabled / preamp / bands 三个事实。
//
// 抽屉里只切预设；改频段在单独的 EQ 页（`pages/eq.tsx`）。

import { effect, signal, Storage, untracked } from "nsh";
import { engine, reportEngineFailure } from "../backend/engine";
import type { EqBand } from "../backend/types";
import { currentPoints, opraActive, opraGainAt } from "./opra";

/** The band centres, Hz: 32 bands, a third of an octave apart, 20 Hz…20 kHz
 * (the engine's `NAE_EQ_MAX_BANDS` is 32, so this is the widest layout the
 * engine can carry). */
export const EQ_FREQS = [
  20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630,
  800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000,
  12500, 16000, 18000, 20000,
] as const;

export const EQ_GAIN_MIN = -12;
export const EQ_GAIN_MAX = 12;

const BANDS_KEY = "audio:eq";
const PRESETS_KEY = "audio:eq:presets";
const CURRENT_KEY = "audio:eq:current";

/** A stored preset: ten gains and the preamp that goes with them. */
export interface EqPreset {
  name: string;
  preampDb: number;
  gains: number[];
  /** The app shipped it (cannot be deleted; 另存为 makes a copy). */
  builtin?: boolean;
}

/** The presets the app ships. Their gains are a shape, not a measurement:
 * the names say what they do, and the faders show exactly what is sent. */
/** A preset's gains, from a handful of anchor points on the log frequency
 * axis: the curve is the anchors interpolated, so a preset is readable as a
 * shape rather than as 32 numbers nobody can check. */
function fromAnchors(anchors: [number, number][]): number[] {
  return EQ_FREQS.map((hz) => {
    if (hz <= anchors[0][0]) return anchors[0][1];
    const last = anchors[anchors.length - 1];
    if (hz >= last[0]) return last[1];
    for (let i = 1; i < anchors.length; i++) {
      const [bHz, bDb] = anchors[i];
      if (hz > bHz) continue;
      const [aHz, aDb] = anchors[i - 1];
      const t = (Math.log(hz) - Math.log(aHz)) / (Math.log(bHz) - Math.log(aHz));
      return Math.round((aDb + (bDb - aDb) * t) * 10) / 10;
    }
    return 0;
  });
}

const BUILTIN: EqPreset[] = [
  { name: "平直", preampDb: 0, gains: fromAnchors([[20, 0], [20000, 0]]), builtin: true },
  { name: "低音增强", preampDb: -2.5, gains: fromAnchors([[20, 6], [60, 5.5], [150, 3], [400, 0.5], [2000, 0], [20000, 0]]), builtin: true },
  { name: "人声", preampDb: -1.5, gains: fromAnchors([[20, -1.5], [200, 0], [800, 2], [2500, 3.5], [6000, 1.5], [20000, -1]]), builtin: true },
  { name: "明亮", preampDb: -2, gains: fromAnchors([[20, -1], [500, 0], [2000, 1], [6000, 3.5], [12000, 4], [20000, 2.5]]), builtin: true },
  { name: "夜听", preampDb: -4, gains: fromAnchors([[20, -4], [80, -2.5], [300, 1], [1000, 2], [3000, 0.5], [8000, -2.5], [20000, -4.5]]), builtin: true },
  { name: "Harman 房间", preampDb: -3, gains: fromAnchors([[20, 4], [70, 3], [200, 1], [800, -0.5], [2500, 1.5], [6000, -1.5], [20000, -2]]), builtin: true },
];

const bands = signal<EqBand[]>(defaultBands());
const enabled = signal(false);
const preamp = signal(0);
const presetsStore = signal<EqPreset[]>([...BUILTIN]);
/** The preset the bands came from, or `null` once a fader moved. */
const current = signal<string | null>(null);
/** A/B compare: slot A is the live bands, slot B is the alternate curve.
 * `live` says which one the engine is playing. */
const slotB = signal<number[] | null>(null);
const live = signal<"A" | "B">("A");
/** Whether the bands have been read from the engine/storage yet. */
const ready = signal(false);
/** Whether the user has already changed something: the one-time seed must
 * not overwrite a preset applied before it ran (that race showed a preset
 * chip lit while every fader read 0). */
let touched = false;

export const eqBands = (): EqBand[] => bands();
export const eqEnabled = (): boolean => enabled();
export const eqPreamp = (): number => preamp();
export const eqPresets = (): EqPreset[] => presetsStore();
export const currentPreset = (): string | null => current();
export const eqReady = (): boolean => ready();

/** `gains` of the bands as the UI holds them. */
export const eqGains = (): number[] => bands().map((band) => band.gainDb);

function defaultBands(): EqBand[] {
  return EQ_FREQS.map((freqHz) => ({
    kind: 0,
    freqHz,
    gainDb: 0,
    q: 1,
    enabled: true,
    channelMask: 0,
  }));
}

/** The bands the engine receives: the user's manual curve plus the OPRA
 * correction, summed per band. The manual curve stored and edited above is
 * never touched by this — that is what makes the OPRA layer non-interfering.
 * The engine has one EQ chain, so the sum is the only way to have both. */
export function effectiveBands(): EqBand[] {
  const manual = bands();
  if (!opraActive()) return manual;
  const points = currentPoints();
  return manual.map((band) => ({
    ...band,
    gainDb: Math.max(EQ_GAIN_MIN, Math.min(EQ_GAIN_MAX, band.gainDb + opraGainAt(band.freqHz, points))),
  }));
}

/** Sends the current bands and state; the engine echoes what it took. */
function push(commit: boolean): void {
  try {
    engine.setEq(enabled(), preamp(), effectiveBands());
    if (commit) {
      void Storage.setItem(BANDS_KEY, JSON.stringify(bands())).catch((error) =>
        console.warn("neiro: EQ 没有记住:", String(error)),
      );
    }
  } catch (error) {
    reportEngineFailure(error, "EQ 下发失败");
  }
}

// A drag arrives faster than the engine should be asked: coalesce to 50 ms,
// the release sends at once (the drawer's EQ did the same).
let queued: ReturnType<typeof setTimeout> | null = null;
function pushSoon(): void {
  if (queued) clearTimeout(queued);
  queued = setTimeout(() => {
    queued = null;
    push(false);
  }, 50);
}

/** Reads the engine's bands once it reports settings, else storage, else
 * the app's layout. Never overwrites bands the engine already has. */
effect(() => {
  if (ready()) return;
  const settings = engine.settings();
  if (!settings) return;
  ready.set(true);
  untracked(() => {
    enabled.set(settings.eqEnabled);
    if (touched) return;
    preamp.set(settings.eqPreampDb);
    if (settings.eqBands.length > 0 && !touched) {
      bands.set(settings.eqBands.map((band) => ({ ...band })));
      return;
    }
    if (touched) return;
    void (async () => {
      let next = defaultBands();
      try {
        const saved = await Storage.getItem(BANDS_KEY);
        if (saved) {
          const parsed: unknown = JSON.parse(saved);
          if (Array.isArray(parsed) && parsed.length > 0) next = parsed as EqBand[];
        }
      } catch (error) {
        console.warn("neiro: EQ 布局没有读回来:", String(error));
      }
      bands.set(next);
      // The engine has no bands and the UI owns the layout: send it once so
      // the chain has something to apply.
      push(true);
    })();
  });
});

// Presets persist separately from the bands.
Storage.getItem(PRESETS_KEY)
  .then((saved) => {
    if (!saved) return;
    const parsed: unknown = JSON.parse(saved);
    if (!Array.isArray(parsed)) return;
    const mine = (parsed as EqPreset[]).filter(
      (preset) =>
        preset &&
        typeof preset.name === "string" &&
        Array.isArray(preset.gains) &&
        preset.gains.length === EQ_FREQS.length,
    );
    if (mine.length > 0) presetsStore.set([...BUILTIN, ...mine.map((preset) => ({ ...preset, builtin: false }))]);
  })
  .catch((error) => console.warn("neiro: EQ 预设没有读回来:", String(error)));

Storage.getItem(CURRENT_KEY)
  .then((saved) => {
    if (saved) current.set(saved);
  })
  .catch(() => {});

/** Turns the EQ on or off (a stage toggle, not a preset). */
export function setEqEnabled(on: boolean): void {
  touched = true;
  enabled.set(on);
  push(true);
}

/** Moves one band's gain. `commit` is the release (persist + send now). */
export function setBandGain(index: number, gainDb: number, commit: boolean): void {
  touched = true;
  const clamped = Math.min(EQ_GAIN_MAX, Math.max(EQ_GAIN_MIN, gainDb));
  bands.update((list) => list.map((band, i) => (i === index ? { ...band, gainDb: clamped } : band)));
  current.set(null);
  if (commit) {
    if (queued) {
      clearTimeout(queued);
      queued = null;
    }
    push(true);
  } else {
    pushSoon();
  }
}

/** Sets the preamp (dB). */
export function setEqPreamp(db: number, commit: boolean): void {
  touched = true;
  const clamped = Math.min(EQ_GAIN_MAX, Math.max(EQ_GAIN_MIN, db));
  preamp.set(clamped);
  if (commit) {
    if (queued) {
      clearTimeout(queued);
      queued = null;
    }
    push(true);
  } else {
    pushSoon();
  }
}

/** 自动增益补偿：preamp = −max(gain)，峰值不因提升而削顶。 */
export function autoGainCompensate(): void {
  const peak = bands().reduce((max, band) => Math.max(max, band.gainDb), 0);
  setEqPreamp(-peak, true);
  current.set(null);
}

/** Applies a preset by name (bands + preamp together). */
export function applyPreset(name: string): void {
  touched = true;
  const preset = presetsStore().find((p) => p.name === name);
  if (!preset) return;
  bands.update((list) =>
    list.map((band, i) => ({
      ...band,
      gainDb: Math.min(EQ_GAIN_MAX, Math.max(EQ_GAIN_MIN, preset.gains[i] ?? 0)),
    })),
  );
  preamp.set(preset.preampDb);
  current.set(name);
  push(true);
  void Storage.setItem(CURRENT_KEY, name).catch(() => {});
}

/** 重置：平直，预增益归零（保留预设列表）。 */
/** Re-sends the bands after the OPRA layer changed (its curve is summed in
 * on the way out, so a change needs one push). */
export function pushEq(): void {
  push(true);
}

export function resetEq(): void {
  touched = true;
  bands.set(defaultBands());
  preamp.set(0);
  current.set("平直");
  push(true);
  void Storage.setItem(CURRENT_KEY, "平直").catch(() => {});
}

/** 另存为：把当前 bands + preamp 存成一个预设。同名的用户预设被替换。 */
export function savePreset(name: string): void {
  const trimmed = name.trim();
  if (!trimmed) return;
  const preset: EqPreset = { name: trimmed, preampDb: preamp(), gains: eqGains(), builtin: false };
  presetsStore.update((list) => {
    const withoutDup = list.filter((p) => p.name !== trimmed || p.builtin);
    const next = [...withoutDup, preset];
    const mine = next.filter((p) => !p.builtin);
    void Storage.setItem(PRESETS_KEY, JSON.stringify(mine)).catch((error) =>
      console.warn("neiro: EQ 预设没有记住:", String(error)),
    );
    return next;
  });
  current.set(trimmed);
  void Storage.setItem(CURRENT_KEY, trimmed).catch(() => {});
}

/** Deletes a user preset (the app's own stay). */
export function deletePreset(name: string): void {
  presetsStore.update((list) => {
    const next = list.filter((p) => p.name !== name || p.builtin);
    void Storage.setItem(
      PRESETS_KEY,
      JSON.stringify(next.filter((p) => !p.builtin)),
    ).catch(() => {});
    return next;
  });
  if (current() === name) current.set(null);
}

// ---- A/B ---------------------------------------------------------------------

export const eqLive = (): "A" | "B" => live();
export const eqHasB = (): boolean => slotB() !== null;

/** Stores the live curve as A and puts `gains` in B (the caller's "copy to
 * B" — a curve to compare against). */
export function setSlotB(gains: number[]): void {
  slotB.set([...gains]);
}

/** Switches which curve the engine plays. The first switch copies the live
 * curve into B so there is something to compare against. */
export function switchAB(): void {
  const a = eqGains();
  if (slotB() === null) slotB.set([...a]);
  const b = slotB();
  if (!b) return;
  if (live() === "A") {
    slotB.set([...a]);
    applyGains(b);
    live.set("B");
  } else {
    applyGains(a);
    live.set("A");
  }
}

/** Puts `gains` on the bands and sends them (no preset bookkeeping). */
function applyGains(gains: number[]): void {
  bands.update((list) =>
    list.map((band, i) => ({
      ...band,
      gainDb: Math.min(EQ_GAIN_MAX, Math.max(EQ_GAIN_MIN, gains[i] ?? 0)),
    })),
  );
  current.set(null);
  push(true);
}

/** Whether the current bands differ from the preset that was applied. */
export const eqDirty = (): boolean => {
  const name = current();
  if (!name) return true;
  const preset = presetsStore().find((p) => p.name === name);
  if (!preset) return true;
  return (
    Math.abs(preset.preampDb - preamp()) > 0.001 ||
    preset.gains.some((gain, i) => Math.abs(gain - (bands()[i]?.gainDb ?? 0)) > 0.001)
  );
};
