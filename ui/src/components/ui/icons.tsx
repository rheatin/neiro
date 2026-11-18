// The app's icons: one family of glyphs (Tabler Icons, MIT, tabler.io/icons).
// Outline at a 1.5pt stroke; Filled for the transport (play, pause, skip).
// Each path is the inside of a 24×24 SVG, its colour `@`: TSX has no
// inline-SVG element, so the token's value is put into the SVG as a data
// URI and read again when the appearance changes.

import { batch, cleanup, colorScheme, effect, Image, type MotionSpec, reduceMotion, signal, token, untracked, View, type Tokens } from "nsh";
import { lazyMotion } from "./motion";
import { BLUR, DURATION, EASING, SCALE } from "./tokens";

export type IconName =
  | "home" | "search" | "music" | "disc" | "user" | "clock"
  | "chevronLeft" | "chevronRight" | "chevronDown"
  | "folder" | "folderPlus" | "refresh" | "close" | "check" | "sidebar"
  | "volume" | "volumeLow" | "volumeMute" | "info"
  | "play" | "pause" | "skipBack" | "skipForward"
  | "heart" | "heartFilled" | "plus" | "sparkles" | "server" | "shuffle"
  | "repeat" | "repeatOnce"
  | "dots" | "queue" | "settings"
  | "layout" | "wave" | "cloud" | "fileText" | "cpu" | "sliders"
  | "grid" | "list" | "sortAsc" | "sortDesc" | "layers";

// Tabler Icons, outline (1pt in the source files; drawn here at 1.5) and
// filled for the transport. Each string is the inside of a 24×24 SVG.
const BODY: Record<IconName, string> = {
  home: '<path d="M5 12l-2 0l9 -9l9 9l-2 0" /><path d="M5 12v7a2 2 0 0 0 2 2h10a2 2 0 0 0 2 -2v-7" /><path d="M9 21v-6a2 2 0 0 1 2 -2h2a2 2 0 0 1 2 2v6" />',
  search: '<path d="M3 10a7 7 0 1 0 14 0a7 7 0 1 0 -14 0" /><path d="M21 21l-6 -6" />',
  music: '<path d="M3 17a3 3 0 1 0 6 0a3 3 0 0 0 -6 0" /><path d="M13 17a3 3 0 1 0 6 0a3 3 0 0 0 -6 0" /><path d="M9 17v-13h10v13" /><path d="M9 8h10" />',
  disc: '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 1 0 -18 0" /><path d="M11 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" /><path d="M7 12a5 5 0 0 1 5 -5" /><path d="M12 17a5 5 0 0 0 5 -5" />',
  user: '<path d="M8 7a4 4 0 1 0 8 0a4 4 0 0 0 -8 0" /><path d="M6 21v-2a4 4 0 0 1 4 -4h4a4 4 0 0 1 4 4v2" />',
  clock: '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0" /><path d="M12 7v5l3 3" />',
  chevronLeft: '<path d="M15 6l-6 6l6 6" />',
  chevronRight: '<path d="M9 6l6 6l-6 6" />',
  chevronDown: '<path d="M6 9l6 6l6 -6" />',
  folder: '<path d="M5 4h4l3 3h7a2 2 0 0 1 2 2v8a2 2 0 0 1 -2 2h-14a2 2 0 0 1 -2 -2v-11a2 2 0 0 1 2 -2" />',
  queue: '<path d="M4 6h10" /><path d="M4 12h10" /><path d="M4 18h10" /><path d="M17 9v10" /><path d="M20 6l-3 3l3 3" />',
  dots: '<path d="M5 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" /><path d="M11 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" /><path d="M17 12a1 1 0 1 0 2 0a1 1 0 1 0 -2 0" />',
  server: '<path d="M3 4a1 1 0 0 1 1 -1h16a1 1 0 0 1 1 1v4a1 1 0 0 1 -1 1h-16a1 1 0 0 1 -1 -1v-4z" /><path d="M3 16a1 1 0 0 1 1 -1h16a1 1 0 0 1 1 1v4a1 1 0 0 1 -1 1h-16a1 1 0 0 1 -1 -1v-4z" /><path d="M7 7l0 .01" /><path d="M7 19l0 .01" />',
  settings:
    '<path d="M10.325 4.317c.426 -1.756 2.924 -1.756 3.35 0a1.724 1.724 0 0 0 2.573 1.066c1.543 -.94 3.31 .826 2.37 2.37a1.724 1.724 0 0 0 1.065 2.572c1.756 .426 1.756 2.924 0 3.35a1.724 1.724 0 0 0 -1.066 2.573c.94 1.543 -.826 3.31 -2.37 2.37a1.724 1.724 0 0 0 -2.572 1.065c-.426 1.756 -2.924 1.756 -3.35 0a1.724 1.724 0 0 0 -2.573 -1.066c-1.543 .94 -3.31 -.826 -2.37 -2.37a1.724 1.724 0 0 0 -1.065 -2.572c-1.756 -.426 -1.756 -2.924 0 -3.35a1.724 1.724 0 0 0 1.066 -2.573c-.94 -1.543 .826 -3.31 2.37 -2.37c1 .608 2.296 .07 2.572 -1.065z" /><path d="M9 12a3 3 0 1 0 6 0a3 3 0 0 0 -6 0" />',
  // 随机播放（Feather 的 shuffle 几何：两条交叉线加两枚箭头；同为描边
  // 风格，笔画与 Tabler 一致画 1.5）。
  shuffle: '<path d="M16 3h5v5" /><path d="M4 20L21 3" /><path d="M21 16v5h-5" /><path d="M15 15l6 6" /><path d="M4 4l5 5" />',
  repeat: '<path d="M4 12v-3a3 3 0 0 1 3 -3h13m-3 -3l3 3l-3 3" /><path d="M20 12v3a3 3 0 0 1 -3 3h-13m3 3l-3 -3l3 -3" />',
  repeatOnce: '<path d="M4 12v-3a3 3 0 0 1 3 -3h13m-3 -3l3 3l-3 3" /><path d="M20 12v3a3 3 0 0 1 -3 3h-13m3 3l-3 -3l3 -3" /><path d="M11 11l1 -1v4" />',
  folderPlus: '<path d="M12 19h-7a2 2 0 0 1 -2 -2v-11a2 2 0 0 1 2 -2h4l3 3h7a2 2 0 0 1 2 2v3.5" /><path d="M16 19h6" /><path d="M19 16v6" />',
  refresh: '<path d="M20 11a8.1 8.1 0 0 0 -15.5 -2m-.5 -4v4h4" /><path d="M4 13a8.1 8.1 0 0 0 15.5 2m.5 4v-4h-4" />',
  close: '<path d="M18 6l-12 12" /><path d="M6 6l12 12" />',
  check: '<path d="M5 12l5 5l10 -10" />',
  sidebar: '<path d="M4 6a2 2 0 0 1 2 -2h12a2 2 0 0 1 2 2v12a2 2 0 0 1 -2 2h-12a2 2 0 0 1 -2 -2l0 -12" /><path d="M9 4l0 16" />',
  volume: '<path d="M15 8a5 5 0 0 1 0 8" /><path d="M17.7 5a9 9 0 0 1 0 14" /><path d="M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2l3.5 -4.5a.8 .8 0 0 1 1.5 .5v14a.8 .8 0 0 1 -1.5 .5l-3.5 -4.5" />',
  volumeLow: '<path d="M15 8a5 5 0 0 1 0 8" /><path d="M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2l3.5 -4.5a.8 .8 0 0 1 1.5 .5v14a.8 .8 0 0 1 -1.5 .5l-3.5 -4.5" />',
  volumeMute: '<path d="M6 15h-2a1 1 0 0 1 -1 -1v-4a1 1 0 0 1 1 -1h2l3.5 -4.5a.8 .8 0 0 1 1.5 .5v14a.8 .8 0 0 1 -1.5 .5l-3.5 -4.5" /><path d="M16 10l4 4m0 -4l-4 4" />',
  info: '<path d="M3 12a9 9 0 1 0 18 0a9 9 0 0 0 -18 0" /><path d="M12 9h.01" /><path d="M11 12h1v4h1" />',
  play: '<path d="M6 4v16a1 1 0 0 0 1.524 .852l13 -8a1 1 0 0 0 0 -1.704l-13 -8a1 1 0 0 0 -1.524 .852z" />',
  pause: '<path d="M9 4h-2a2 2 0 0 0 -2 2v12a2 2 0 0 0 2 2h2a2 2 0 0 0 2 -2v-12a2 2 0 0 0 -2 -2z" /><path d="M17 4h-2a2 2 0 0 0 -2 2v12a2 2 0 0 0 2 2h2a2 2 0 0 0 2 -2v-12a2 2 0 0 0 -2 -2z" />',
  skipBack: '<path d="M19.496 4.136l-12 7a1 1 0 0 0 0 1.728l12 7a1 1 0 0 0 1.504 -.864v-14a1 1 0 0 0 -1.504 -.864z" /><path d="M4 4a1 1 0 0 1 .993 .883l.007 .117v14a1 1 0 0 1 -1.993 .117l-.007 -.117v-14a1 1 0 0 1 1 -1z" />',
  skipForward: '<path d="M2 5v14c0 .86 1.012 1.318 1.659 .753l8 -7a1 1 0 0 0 0 -1.506l-8 -7c-.647 -.565 -1.659 -.106 -1.659 .753z" /><path d="M13 5v14c0 .86 1.012 1.318 1.659 .753l8 -7a1 1 0 0 0 0 -1.506l-8 -7c-.647 -.565 -1.659 -.106 -1.659 .753z" />',
  // Tabler heart（同一 path，描边 / 填充两用）。
  heart: '<path d="M6.979 3.074a6 6 0 0 1 4.988 1.425l.037 .033l.034 -.03a6 6 0 0 1 4.733 -1.44l.246 .036a6 6 0 0 1 3.364 10.008l-.18 .185l-.048 .041l-7.45 7.379a1 1 0 0 1 -1.313 .082l-.094 -.082l-7.493 -7.422a6 6 0 0 1 3.176 -10.215z" />',
  heartFilled: '<path d="M6.979 3.074a6 6 0 0 1 4.988 1.425l.037 .033l.034 -.03a6 6 0 0 1 4.733 -1.44l.246 .036a6 6 0 0 1 3.364 10.008l-.18 .185l-.048 .041l-7.45 7.379a1 1 0 0 1 -1.313 .082l-.094 -.082l-7.493 -7.422a6 6 0 0 1 3.176 -10.215z" />',
  plus: '<path d="M12 5l0 14" /><path d="M5 12l14 0" />',
  // Tabler sparkles（light 菜单的 ✦）。
  sparkles: '<path d="M16 18a2 2 0 0 1 2 2a2 2 0 0 1 2 -2a2 2 0 0 1 -2 -2a2 2 0 0 1 -2 2zm0 -12a2 2 0 0 1 2 2a2 2 0 0 1 2 -2a2 2 0 0 1 -2 -2a2 2 0 0 1 -2 2zm-7 12a6 6 0 0 1 6 -6a6 6 0 0 1 -6 -6a6 6 0 0 1 6 6z" />',
  layout: '<rect x="4" y="4" width="16" height="16" rx="2" /><line x1="4" y1="10" x2="20" y2="10" /><line x1="10" y1="10" x2="10" y2="20" />',
  wave: '<path d="M4 12h2l2 -6l3 12l3 -8l2 5l2 -3h2" />',
  cloud: '<path d="M6.657 18c-2.572 0 -4.657 -2.007 -4.657 -4.483c0 -2.31 1.78 -4.212 4.062 -4.444c.485 -2.872 3.125 -5.073 6.281 -5.073c3.481 0 6.326 2.67 6.617 6.07c2.253 .248 4.04 2.155 4.04 4.45c0 2.476 -2.085 4.48 -4.657 4.48h-11.686z" />',
  fileText: '<path d="M14 3v4a1 1 0 0 0 1 1h4" /><path d="M17 21h-10a2 2 0 0 1 -2 -2v-14a2 2 0 0 1 2 -2h7l5 5v11a2 2 0 0 1 -2 2z" /><line x1="9" y1="9" x2="10" y2="9" /><line x1="9" y1="13" x2="15" y2="13" /><line x1="9" y1="17" x2="15" y2="17" />',
  cpu: '<rect x="4" y="4" width="16" height="16" rx="1" /><path d="M9 9h6v6h-6z" /><path d="M3 10h1" /><path d="M3 14h1" /><path d="M10 3v1" /><path d="M14 3v1" /><path d="M20 10h1" /><path d="M20 14h1" /><path d="M10 20v1" /><path d="M14 20v1" />',
  sliders: '<line x1="4" y1="10" x2="4" y2="20" /><line x1="4" y1="4" x2="4" y2="7" /><circle cx="4" cy="8" r="2" /><line x1="12" y1="17" x2="12" y2="20" /><line x1="12" y1="4" x2="12" y2="14" /><circle cx="12" cy="16" r="2" /><line x1="20" y1="13" x2="20" y2="20" /><line x1="20" y1="4" x2="20" y2="10" /><circle cx="20" cy="12" r="2" />',
  grid: '<rect x="4" y="4" width="6" height="6" rx="1" /><rect x="14" y="4" width="6" height="6" rx="1" /><rect x="4" y="14" width="6" height="6" rx="1" /><rect x="14" y="14" width="6" height="6" rx="1" />',
  list: '<line x1="9" y1="6" x2="20" y2="6" /><line x1="9" y1="12" x2="20" y2="12" /><line x1="9" y1="18" x2="20" y2="18" /><line x1="5" y1="6" x2="5" y2="6.01" /><line x1="5" y1="12" x2="5" y2="12.01" /><line x1="5" y1="18" x2="5" y2="18.01" />',
  sortAsc: '<path d="M4 6l7 0" /><path d="M4 12l7 0" /><path d="M4 18l9 0" /><path d="M15 9l3 -3l3 3" /><path d="M18 6l0 12" />',
  sortDesc: '<path d="M4 6l9 0" /><path d="M4 12l7 0" /><path d="M4 18l7 0" /><path d="M15 15l3 3l3 -3" /><path d="M18 6l0 12" />',
  layers: '<polygon points="12 2 2 7 12 12 22 7 12 2" /><polyline points="2 17 12 22 22 17" /><polyline points="2 12 12 17 22 12" />',
};

/** The glyphs that draw themselves filled (the transport), the rest stroked. */
const FILLED: Partial<Record<IconName, true>> = { play: true, pause: true, skipBack: true, skipForward: true, heartFilled: true };

/** A glyph's hover morph, as it skips: a fading echo trails it. */
const ease = (t: number) => (t < 0.5 ? 2 * t * t : 1 - (-2 * t + 2) ** 2 / 2);
const env = (t: number) => Math.sin(t * Math.PI);

// Scale about the glyph's centre: the heart beats on hover.
const scaledBody = (body: string, k: number) =>
  `<g transform="translate(${C} ${C}) scale(${k.toFixed(3)}) translate(${-C} ${-C})">${body}</g>`;

const MORPHS: Partial<Record<IconName, [(body: string, t: number) => string, number]>> = {
  skipForward: [(body, t) => `<g opacity="${(0.45 * env(t)).toFixed(3)}" transform="translate(${(3.2 * env(t)).toFixed(2)} 0)">${body}</g>${body}`, 380],
  skipBack: [(body, t) => `<g opacity="${(0.45 * env(t)).toFixed(3)}" transform="translate(${(-3.2 * env(t)).toFixed(2)} 0)">${body}</g>${body}`, 380],
  play: [(body, t) => `<g transform="translate(${(1.3 * env(t)).toFixed(2)} 0)">${body}</g>`, 300],
  pause: [(body, t) => `<g transform="translate(12 0) scale(${(1 - 0.18 * env(t)).toFixed(3)} 1) translate(-12 0)">${body}</g>`, 300],
  heart: [(body, t) => scaledBody(body, 1 + 0.14 * env(t)), 320],
  heartFilled: [(body, t) => scaledBody(body, 1 + 0.14 * env(t)), 320],
};

const C = 12;

const URIS = new Map<string, string>();
const STEPS = 24;

function dataUri(name: IconName, color: string, t = 0): string {
  const morph = MORPHS[name];
  const q = morph && t > 0 && t < 1 ? Math.round(t * STEPS) : 0;
  const key = `${name}|${color}|${q}`;
  let uri = URIS.get(key);
  if (!uri) {
    const body = q > 0 && q < STEPS && morph ? morph[0](BODY[name], q / STEPS) : BODY[name];
    const painted = FILLED[name]
      ? `<g fill="${color}">${body}</g>`
      : `<g fill="none" stroke="${color}" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">${body}</g>`;
    const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="24" height="24">${painted}</svg>`;
    uri = `data:image/svg+xml,${encodeURIComponent(svg)}`;
    URIS.set(key, uri);
  }
  return uri;
}

/** A theme token an icon can draw in. */
export type IconColor = keyof Tokens & string;

export interface IconProps {
  name: IconName;
  /** Points (default 24). */
  size?: number;
  /** A theme token (default `"foreground"`). */
  color?: IconColor;
  /** A colour of its own (`#rrggbb`), over `color`. */
  hex?: string;
  /** How far through its hover morph (`iconMotion().t()`), 0 at rest. */
  t?: number;
}

/** The colour an icon draws in: `hex`, or its token's value now (reading the
 * appearance, so it follows light and dark). */
function colorOf(props: IconProps): string {
  colorScheme();
  return props.hex || String(token((props.color ?? "foreground") as never));
}

/** The named glyph, in a theme token's color. */
export function Icon(props: IconProps) {
  const size = () => props.size ?? 24;
  return <Image src={dataUri(props.name, colorOf(props), props.t)} accessibilityElementsHidden style={{ width: size(), height: size() }} />;
}

// ---- The swap and the hover move ----

/** A glyph swapping for another (transitions.dev's icon swap): both at
 * once, the old one shrinking to a quarter, blurring and fading as the new
 * one grows from a quarter, sharpens and comes in. */
const SWAP_IN: MotionSpec = {
  keyframes: { opacity: [0, 1], scale: [SCALE.swap, 1] },
  duration: DURATION.base,
  easing: EASING.inOut,
};
const SWAP_OUT: MotionSpec = {
  keyframes: { opacity: [1, 0], scale: [1, SCALE.swap] },
  duration: DURATION.base,
  easing: EASING.inOut,
};
/** NSH draws a blur but cannot animate one: it steps, this many times
 * through the swap (a few timeouts per swap, nothing at rest). */
const BLUR_STEPS = 4;

/** `Icon`, whose glyph swaps when `name` changes (play and pause, the
 * volume's levels): the glyph leaving keeps the colour it had. Its first
 * glyph just shows; nothing moves at rest or with reduced motion. `t` is
 * the hover morph of the glyph showing. */
export function SwapIcon(props: IconProps) {
  const incoming = lazyMotion();
  const outgoing = lazyMotion();
  const leaving = signal<{ name: IconName; hex: string } | null>(null);
  const step = signal(BLUR_STEPS);
  let timers: ReturnType<typeof setTimeout>[] = [];
  const stopBlur = () => {
    for (const timer of timers) clearTimeout(timer);
    timers = [];
  };
  let name = untracked(() => props.name);
  let hex = untracked(() => colorOf(props));
  effect(() => {
    const now = props.name;
    if (now === name) return;
    const was = { name, hex: colorOf(props) };
    name = now;
    hex = colorOf(props);
    untracked(() => {
      stopBlur();
      incoming.stop();
      outgoing.stop();
      if (reduceMotion()) {
        leaving.set(null);
        incoming.reset();
        step.set(BLUR_STEPS);
        return;
      }
      batch(() => {
        leaving.set(was);
        step.set(0);
      });
      void incoming.play(SWAP_IN);
      void outgoing.play(SWAP_OUT).then(({ finished }) => finished && leaving.set(null));
      for (let i = 1; i <= BLUR_STEPS; i++) {
        timers.push(setTimeout(() => step.set(i), (DURATION.base * i) / BLUR_STEPS));
      }
    });
  });
  cleanup(() => {
    stopBlur();
    incoming.stop();
    outgoing.stop();
  });
  const size = () => props.size ?? 24;
  const blur = (amount: number) => (amount > 0 ? `blur(${amount.toFixed(2)}px)` : undefined);
  return (
    <View class="items-center justify-center" style={{ width: size(), height: size() }}>
      <View style={incoming.style}>
        <View style={{ filter: blur(BLUR.small * (1 - step() / BLUR_STEPS)) }}>
          <Icon name={props.name} size={size()} color={props.color} hex={props.hex} t={props.t} />
        </View>
      </View>
      <If when={leaving()}>
        {(was) => (
          <View class="absolute inset-0 items-center justify-center" style={outgoing.style}>
            <View style={{ filter: blur((BLUR.small * step()) / BLUR_STEPS) }}>
              <Icon name={was().name} size={size()} hex={was().hex} />
            </View>
          </View>
        )}
      </If>
    </View>
  );
}

/** A glyph's one hover move: where it is (points, degrees) at `t`, 0..1
 * through the move. Each rides a half-sine envelope, so it leaves rest and
 * comes back to it without a jump. */
type Pose = (t: number) => { x?: number; y?: number; rotate?: number; scale?: number };
const deg = (rad: number) => (rad * 180) / Math.PI;

const lift: Pose = (t) => ({ y: -2 * env(t), rotate: deg(-0.12 * env(t)) });
const swing: Pose = (t) => ({ rotate: deg(Math.sin(t * 4 * Math.PI) * env(t) * 0.18) });
const nudge =
  (dx: number, dy: number): Pose =>
  (t) => ({ x: dx * env(t), y: dy * env(t) });
const turn =
  (by: number): Pose =>
  (t) => ({ rotate: by * env(t) });

const MOVES: Partial<Record<IconName, [Pose, number]>> = {
  home: [lift, 300],
  search: [swing, 480],
  music: [lift, 300],
  disc: [swing, 480],
  user: [lift, 300],
  chevronLeft: [nudge(-2, 0), 300],
  chevronRight: [nudge(2, 0), 300],
  chevronDown: [nudge(0, 2), 300],
  folderPlus: [lift, 300],
  refresh: [turn(120), 420],
  close: [turn(90), 300],
  info: [lift, 300],
  heart: [(t) => ({ scale: 1 + 0.14 * env(t) }), 320],
  heartFilled: [(t) => ({ scale: 1 + 0.14 * env(t) }), 320],
};

/** Samples a pose into keyframes the native animation plays (linear
 * between samples: the pose has its own easing). */
function spec(pose: Pose, duration: number): MotionSpec {
  const steps = Math.max(8, Math.round(duration / 16));
  const frames = Array.from({ length: steps + 1 }, (_, i) => {
    const p = pose(i / steps);
    return { translateX: p.x ?? 0, translateY: p.y ?? 0, rotate: `${p.rotate ?? 0}deg`, scale: p.scale ?? 1 };
  });
  return { keyframes: frames, duration, easing: "linear" };
}

const SPECS = new Map<IconName, MotionSpec>();
function specOf(name: IconName): MotionSpec {
  let s = SPECS.get(name);
  if (!s) {
    const [pose, duration] = MOVES[name] ?? [lift, 300];
    s = spec(pose, duration);
    SPECS.set(name, s);
  }
  return s;
}

/** A glyph's hover move: spread `style` on a view around the icon and call
 * `play(name)` as the pointer enters the control (`onMouseEnter`). Each
 * entry plays it once from rest, ending at rest; nothing with reduced
 * motion. */
export function iconMotion() {
  const motion = lazyMotion();
  const t = signal(0);
  let timer: ReturnType<typeof setInterval> | null = null;
  const stopMorph = () => {
    if (timer !== null) clearInterval(timer);
    timer = null;
    t.set(0);
  };
  try {
    cleanup(stopMorph);
  } catch {
    // Outside a component: nothing to stop with.
  }
  return {
    /** Bind it in a binding (`style={hover.style}`): it follows. */
    get style() {
      return motion.style;
    },
    /** How far through the glyph's morph (pass to `Icon`'s `t`). */
    t: () => t(),
    play(name: IconName) {
      if (reduceMotion()) return;
      const morph = MORPHS[name];
      if (morph) {
        stopMorph();
        const start = Date.now();
        timer = setInterval(() => {
          const k = (Date.now() - start) / morph[1];
          if (k >= 1) stopMorph();
          else t.set(k);
        }, 16);
        return;
      }
      motion.stop();
      void motion.play(specOf(name));
    },
  };
}
