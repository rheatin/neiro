// Motion tokens: the one set of durations, easings, distances and scales
// every animation in Neiro uses (spotish's, per its DESIGN.md Motion 节).
// Change a value here, not in a component.

/** Durations, ms. */
export const DURATION = {
  /** A press, a hover wash, a colour change. */
  instant: 120,
  /** Most state changes: a control's hover and press layers. */
  fast: 180,
  /** Something moving into place: a selection sliding, an icon swapping. */
  base: 240,
  /** Larger surfaces and reveals: a skeleton giving way to content. */
  slow: 360,
  /** Unhurried entrances: a list's rows and cards coming in. */
  slower: 520,
} as const;

/** Easings, as NSH names them (`createMotion`'s `easing`). */
export const EASING = {
  out: "easeOut",
  inOut: "easeInOut",
  in: "easeIn",
} as const;

/** Distances, points: how far things travel as they come in. */
export const DISTANCE = {
  small: 4,
  base: 8,
  large: 16,
} as const;

/** Scales: how much things grow or shrink in motion. */
export const SCALE = {
  press: 0.95,
  enter: 0.9,
  hover: 1.04,
  pressSoft: 0.97,
  /** Where a glyph swapping for another starts and ends. */
  swap: 0.25,
} as const;

/** Blurs, points (NSH draws `filter: blur()` but cannot transition one:
 * motion steps it, see `SwapIcon`). */
export const BLUR = { small: 2 } as const;

/** A long, soft settle, as cubic-Bézier points — the press and the list
 * wave's curve. */
export const SMOOTH_OUT: [number, number, number, number] = [0.22, 1, 0.36, 1];

/** Tailwind easing classes for the same tokens. */
export const EASE_CLASS = {
  out: "ease-out",
  inOut: "ease-in-out",
  smoothOut: "ease-[cubic-bezier(0.22,1,0.36,1)]",
} as const;

/** Tailwind duration classes for the same tokens. */
export const DURATION_CLASS = {
  instant: "duration-120",
  fast: "duration-180",
  base: "duration-240",
  slow: "duration-360",
} as const;
