// OPRA / AutoEQ: a headphone correction curve kept in its own layer.
//
// The engine has one EQ chain, so the two layers are summed in the app before
// they are sent: the user's 32-band curve stays exactly as they drew it, and
// the selected headphone's correction is added on top. Switching or editing a
// headphone profile never rewrites the manual bands — that is the whole point
// of the separate layer, and it is true here because the manual curve is the
// value that is stored and edited, never the sum.
//
// The profiles are a small built-in set. Importing an AutoEQ CSV replaces the
// selected profile's points; nothing is fetched over the network.

import { signal, Storage } from "nsh";
import { pushEq } from "./eq";

const KEY = "audio:opra";
const ACTIVE_KEY = "audio:opra:active";

/** One correction point: a frequency and a gain to apply there. */
export interface OpraPoint {
  hz: number;
  db: number;
}

/** A headphone profile: a name and its correction points. */
export interface OpraProfile {
  name: string;
  points: OpraPoint[];
  /** The app shipped it (an import makes a copy). */
  builtin?: boolean;
}

/** The app's own starting profiles: a shape each, named for the target they
 * follow. They are corrections, not measurements of a particular unit. */
const BUILTIN: OpraProfile[] = [
  { name: "平直", points: [], builtin: true },
  {
    name: "Harman 目标",
    builtin: true,
    points: [
      { hz: 20, db: 4.0 }, { hz: 60, db: 3.2 }, { hz: 150, db: 1.4 },
      { hz: 400, db: 0.2 }, { hz: 1000, db: 0.0 }, { hz: 2000, db: 1.0 },
      { hz: 3000, db: 2.6 }, { hz: 5000, db: -1.2 }, { hz: 8000, db: -2.4 },
      { hz: 12000, db: -1.0 }, { hz: 20000, db: 1.0 },
    ],
  },
  {
    name: "Diffuse-Field",
    builtin: true,
    points: [
      { hz: 20, db: 1.0 }, { hz: 100, db: 0.4 }, { hz: 1000, db: 0.0 },
      { hz: 2000, db: -0.8 }, { hz: 4000, db: -2.2 }, { hz: 8000, db: -1.4 },
      { hz: 16000, db: 0.6 },
    ],
  },
];

const profiles = signal<OpraProfile[]>([...BUILTIN]);
const selected = signal<string>("平直");
const active = signal(false);

export const opraProfiles = (): OpraProfile[] => profiles();
export const opraSelected = (): string => selected();
/** Whether the correction layer is in the path at all. */
export const opraActive = (): boolean => active() && (currentPoints().length > 0);

/** The selected profile's points (empty when it corrects nothing). */
export function currentPoints(): OpraPoint[] {
  const name = selected();
  return profiles().find((profile) => profile.name === name)?.points ?? [];
}

Storage.getItem(KEY)
  .then((saved) => {
    if (!saved) return;
    const parsed: unknown = JSON.parse(saved);
    if (!Array.isArray(parsed)) return;
    const mine = (parsed as OpraProfile[]).filter(
      (profile) => profile && typeof profile.name === "string" && Array.isArray(profile.points),
    );
    if (mine.length > 0) profiles.set([...BUILTIN, ...mine.map((profile) => ({ ...profile, builtin: false }))]);
  })
  .catch((error) => console.warn("neiro: OPRA 配置没有读回来:", String(error)));

Storage.getItem(ACTIVE_KEY)
  .then((saved) => {
    if (saved !== null) active.set(saved === "1");
  })
  .catch(() => {});

export function setOpraActive(on: boolean): void {
  active.set(on);
  void Storage.setItem(ACTIVE_KEY, on ? "1" : "0").catch(() => {});
  pushEq();
}

export function selectOpraProfile(name: string): void {
  if (!profiles().some((profile) => profile.name === name)) return;
  selected.set(name);
  pushEq();
}

/** Imports an AutoEQ CSV (`frequency,gain` per line, a header tolerated).
 * The import becomes a user profile so a re-import never overwrites a
 * shipped one. */
export function importAutoEqCsv(text: string, name: string): { ok: boolean; points: number; error?: string } {
  const points: OpraPoint[] = [];
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line || /^[a-z]/i.test(line)) continue;
    const parts = line.split(/[,\t;]/);
    if (parts.length < 2) continue;
    const hz = Number(parts[0]);
    const db = Number(parts[1]);
    if (!Number.isFinite(hz) || !Number.isFinite(db) || hz <= 0) continue;
    points.push({ hz, db });
  }
  if (points.length === 0) return { ok: false, points: 0, error: "CSV 里没有可读的 频率,增益 行" };
  points.sort((a, b) => a.hz - b.hz);
  const profile: OpraProfile = { name: name.trim() || "导入的曲线", points, builtin: false };
  profiles.update((list) => {
    const next = [...list.filter((p) => p.name !== profile.name || p.builtin), profile];
    void Storage.setItem(KEY, JSON.stringify(next.filter((p) => !p.builtin))).catch(() => {});
    return next;
  });
  selected.set(profile.name);
  active.set(true);
  void Storage.setItem(ACTIVE_KEY, "1").catch(() => {});
  return { ok: true, points: points.length };
}

/** Deletes a user profile (the app's own stay). */
export function deleteOpraProfile(name: string): void {
  profiles.update((list) => {
    const next = list.filter((p) => p.name !== name || p.builtin);
    void Storage.setItem(KEY, JSON.stringify(next.filter((p) => !p.builtin))).catch(() => {});
    return next;
  });
  if (selected() === name) selected.set("平直");
}

/** The correction as a value at `hz`, interpolated in log frequency between
 * its two nearest points (0 outside them). */
export function opraGainAt(hz: number, points: OpraPoint[]): number {
  if (points.length === 0) return 0;
  if (hz <= points[0].hz) return points[0].db;
  if (hz >= points[points.length - 1].hz) return points[points.length - 1].db;
  for (let i = 1; i < points.length; i++) {
    const b = points[i];
    if (hz > b.hz) continue;
    const a = points[i - 1];
    const t = (Math.log(hz) - Math.log(a.hz)) / (Math.log(b.hz) - Math.log(a.hz));
    return a.db + (b.db - a.db) * t;
  }
  return 0;
}
