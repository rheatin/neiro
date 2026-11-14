// Which music light plays (播放条与 Now Playing 的同一光)，remembered
// between runs。可加效果后续在 toolbar-fx 的 LIGHTS 里增；Neiro 本版实现
// spectrum / dots / bars 三个。选择 Storage 持久化，默认 spectrum。

import { effect, signal, Storage, untracked } from "nsh";
import { EFFECT_NAMES, TOOLBAR_EFFECTS, type ToolbarEffect } from "../components/ui/toolbar-fx";
import { player } from "./player";

/** What the menu offers: an effect, or a different one each track. */
export type LightChoice = ToolbarEffect | "random";

const KEY = "visual:effect";
const chosen = signal<LightChoice>("spectrum");
// The effect drawn: the one chosen, or Random's pick for this track.
const shown = signal<ToolbarEffect>("spectrum");

/** Any effect but `not`, each as likely as the others. */
function pickOther(not: ToolbarEffect): ToolbarEffect {
  const others = TOOLBAR_EFFECTS.filter((e) => e !== not);
  return others[Math.floor(Math.random() * others.length)] ?? not;
}

function apply(choice: LightChoice): void {
  chosen.set(choice);
  shown.set(choice === "random" ? pickOther(untracked(shown)) : choice);
}

Storage.getItem(KEY)
  .then((saved) => {
    if (saved === "random" || (saved && (TOOLBAR_EFFECTS as string[]).includes(saved))) apply(saved as LightChoice);
  })
  .catch(() => {});

// Random: a new pick each time the track changes.
let lastTrack = "";
effect(() => {
  const track = player().path ?? "";
  if (track === lastTrack) return;
  const first = lastTrack === "";
  lastTrack = track;
  if (first || track === "" || chosen() !== "random") return;
  untracked(() => shown.set(pickOther(shown())));
});

/** The music light drawn now (tracked). */
export const lightEffect = () => shown();

/** Whether Random is chosen (tracked). */
export const lightRandom = () => chosen() === "random";

/** Its name, for the switch's label (tracked): Random says which it picked. */
export const lightName = () => (lightRandom() ? `Random (${EFFECT_NAMES[shown()]})` : EFFECT_NAMES[shown()]);

/** Uses `choice` from now on (Random picks one at once), and remembers it. */
export function setLight(choice: LightChoice): void {
  apply(choice);
  Storage.setItem(KEY, choice).catch((error) => console.warn("neiro: 灯效没有记住:", String(error)));
}
