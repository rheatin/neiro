// The player's surface and the music's light (spotish `toolbar-fx` 的移植，
// 适配 Neiro）：表面是 player 的颜色、不透明、一圈 1px rim；其上一层的光每
// 显示帧由 shader 原生驱动（not per-frame JS）。效果只收三个
//（spectrum / dots / bars——机制已证明，后续可加）。颜色没有封面取色管
// 线，走主题：c0 = `token("primary")`（accent 家族）、c1 = `chart-3`、
// c2 = `chart-4`；亮度由 WGSL `tame()` 约束在 0.32，控件的对比度保住。

import { Animated, type AnimatedValue, type CompositeAnimation, cleanup, createAnimatedValue as AnimatedCreate, effect, untracked, type JSX, reduceMotion, Shader, token, View } from "nsh";
import { BASS, BEAT, BAND0, CLOCK, LEVEL, MID, ONSET, PHASE, SINCE, SLOTS, TREBLE, isPlayingSpectrum, live } from "../../state/visualiser";

// ---- HEAD：uniforms 结构 + band/spectrum/vivid/tame/cycle/ends + pill ----

const HEAD = /* wgsl */ `
struct Uniforms {
  c0: vec4f,
  c1: vec4f,
  c2: vec4f,
  c3: vec4f,
  base: vec4f,
  b0: vec4f,
  b1: vec4f,
  b2: vec4f,
  b3: vec4f,
  beat: f32,
  level: f32,
  bass: f32,
  mid: f32,
  treble: f32,
  strength: f32,
  radius: f32,
  reach: f32,
  phase: f32,
  clock: f32,
  since: f32,
  onset: f32,
}

fn band(i: i32) -> f32 {
  let k = clamp(i, 0, 15);
  let v = select(select(select(uniforms.b3, uniforms.b2, k < 12), uniforms.b1, k < 8), uniforms.b0, k < 4);
  let j = k % 4;
  return select(select(select(v.w, v.z, j < 3), v.y, j < 2), v.x, j < 1);
}

fn spectrum(x: f32) -> f32 {
  let s = clamp(x, 0.0, 1.0) * 15.0;
  let i = i32(floor(s));
  let f = fract(s);
  return mix(band(i), band(i + 1), f * f * (3.0 - 2.0 * f));
}

fn pill(p: vec2f) -> f32 {
  let r = min(uniforms.radius, nsh.size.y * 0.5);
  let c = nsh.size * 0.5;
  let q = abs(p - c) - (c - vec2f(r, r));
  return length(max(q, vec2f(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

fn hash(p: vec2f) -> f32 {
  return fract(sin(dot(p, vec2f(127.1, 311.7))) * 43758.5453);
}

fn vivid(c: vec3f) -> vec3f {
  let hi = max(max(c.r, c.g), c.b);
  let lo = min(min(c.r, c.g), c.b);
  let chroma = smoothstep(0.03, 0.2, hi - lo);
  let pure = (c - vec3f(lo)) / max(hi - lo, 0.001);
  return mix(vec3f(0.85), mix(vec3f(0.3), vec3f(1.0), pure), chroma);
}

fn tame(c: vec3f) -> vec3f {
  let v = vivid(c);
  let lum = dot(v, vec3f(0.2126, 0.7152, 0.0722));
  return clamp(v * (0.32 / max(lum, 0.05)), vec3f(0.0), vec3f(0.9));
}

fn cycle(x: f32) -> vec3f {
  let q = fract(x + uniforms.phase) * 3.0;
  let f = smoothstep(0.0, 1.0, fract(q));
  let i = i32(floor(q));
  let a = select(select(uniforms.c2.rgb, uniforms.c1.rgb, i == 1), uniforms.c0.rgb, i == 0);
  let b = select(select(uniforms.c0.rgb, uniforms.c2.rgb, i == 1), uniforms.c1.rgb, i == 0);
  return mix(a, b, f);
}

fn ends(uv: vec2f) -> f32 {
  return smoothstep(0.0, 0.08, uv.x) * smoothstep(1.0, 0.92, uv.x);
}
`;

// ---- Spectrum（默认）：光从底边升起，频谱镜像自中心 ----------------------------

const SPECTRUM = /* wgsl */ `
fn effect(p: vec2f, uv: vec2f) -> f32 {
  let v = pow(spectrum(abs(uv.x - 0.5) * 2.0), 1.5);
  let up = nsh.size.y - p.y;
  let crest = 2.0 + uniforms.reach * v + 4.0 * uniforms.beat;
  let body = smoothstep(crest + 2.0, crest - 3.0, up) * (0.15 + 0.45 * v) * (0.4 + 0.6 * (1.0 - up / max(crest, 1.0)));
  let line = exp(-pow((up - crest) / 1.1, 2.0)) * (0.35 + 0.65 * v);
  return (body + line) * ends(uv) * (0.5 + 0.5 * uniforms.level);
}

fn glow(p: vec2f, uv: vec2f) -> vec4f {
  return vec4f(cycle(uv.x * 0.75), effect(p, uv));
}
`;

// ---- Dots：点阵墙（spotish 同款参数 pitch 7pt） --------------------------------

const DOTS = /* wgsl */ `
fn glow(p: vec2f, uv: vec2f) -> vec4f {
  let pitch = 7.0;
  let cell = floor(p / pitch);
  let c = (cell + 0.5) * pitch;
  let cx = c.x / nsh.size.x;
  let v = pow(spectrum(abs(cx - 0.5) * 2.0), 1.3);
  let top = 2.0 + uniforms.reach * v + 4.0 * uniforms.beat;
  let up = nsh.size.y - c.y;
  let lit = smoothstep(top + pitch, top - pitch * 0.5, up);
  let near = clamp(up / max(top, 1.0), 0.0, 1.0);
  let twinkle = 0.85 + 0.15 * sin(uniforms.clock * 2.0 + hash(cell) * 6.28);
  let r = pitch * (0.16 + 0.16 * near * lit);
  let d = length(p - c) - r;
  let dot = clamp(0.5 - d * nsh.scale, 0.0, 1.0);
  let a = dot * (0.12 + 0.88 * lit * (0.35 + 0.65 * near)) * twinkle;
  return vec4f(cycle(cx * 0.75 + near * 0.1), a * ends(uv) * (0.55 + 0.45 * uniforms.level));
}
`;

// ---- Bars：底边的圆头柱（镜像自中心） ------------------------------------------

const BARS = /* wgsl */ `
fn glow(p: vec2f, uv: vec2f) -> vec4f {
  let n = max(floor(nsh.size.x / 22.0), 8.0);
  let x = uv.x * n;
  let cx = (floor(x) + 0.5) / n;
  let v = pow(spectrum(abs(cx - 0.5) * 2.0), 1.3);
  let top = 3.0 + uniforms.reach * v + 3.0 * uniforms.beat;
  let up = nsh.size.y - p.y;
  let cellw = nsh.size.x / n;
  let half = cellw * 0.26;
  let dx = abs(fract(x) - 0.5) * cellw;
  let q = vec2f(dx, max(up - (top - half), 0.0));
  let d = length(q) - half;
  let bar = clamp(0.5 - d * nsh.scale, 0.0, 1.0) * step(0.0, up);
  let body = 0.25 + 0.45 * clamp(up / max(top, 1.0), 0.0, 1.0);
  let cap = exp(-pow((up - top + half * 0.5) / 2.0, 2.0)) * smoothstep(half + 1.0, 0.0, dx);
  return vec4f(cycle(cx * 0.75), (bar * body + cap * 0.6) * ends(uv) * (0.6 + 0.4 * uniforms.level));
}
`;

/** 本抽屉发条的尾巴：光在 pill（PlayerGlass）或矩形的边内淡出 + 噪声抖动
 * 抗 banding； tame() 的亮度约束在这里套。 */
const LIGHT_TAIL = /* wgsl */ `
fn shade(uv: vec2f) -> vec4f {
  let p = uv * nsh.size;
  let inside = clamp(-0.5 - pill(p) * nsh.scale, 0.0, 1.0);
  let g = glow(p, uv);
  let light = clamp(g.a, 0.0, 1.0) * uniforms.strength * inside;
  let noise = (fract(52.9829189 * fract(dot(p * nsh.scale, vec2f(0.06711056, 0.00583715)))) - 0.5) / 255.0;
  return vec4f(tame(g.rgb), clamp(light + noise * inside, 0.0, 0.55));
}
`;

/** 矩形（MusicLight）的尾巴：按 zuv full rect 而不是 pill。 */
const RECT_TAIL = /* wgsl */ `
fn shade(uv: vec2f) -> vec4f {
  let g = glow(uv * nsh.size, uv);
  let light = clamp(g.a, 0.0, 1.0) * uniforms.strength;
  let noise = (fract(52.9829189 * fract(dot(uv * nsh.scale, vec2f(0.06711056, 0.00583715)))) - 0.5) / 255.0;
  return vec4f(tame(g.rgb), clamp(light + noise, 0.0, 0.55));
}
`;

/** The lights, by name: each defines `glow`, its colour and strength. */
const LIGHTS = {
  dots: `${HEAD}${DOTS}${LIGHT_TAIL}`,
  spectrum: `${HEAD}${SPECTRUM}${RECT_TAIL}`,
  bars: `${HEAD}${BARS}${RECT_TAIL}`,
} as const;

export type ToolbarEffect = keyof typeof LIGHTS;

/** Each effect's name, for the switch. */
export const EFFECT_NAMES: Record<ToolbarEffect, string> = {
  spectrum: "Spectrum",
  dots: "Dots",
  bars: "Bars",
};
export const TOOLBAR_EFFECTS = Object.keys(LIGHTS) as ToolbarEffect[];

/** How far up each effect reaches in Now Playing, as a share of the room:
 * the bars fill most, spectrum a bit less. */
const REACH: Record<ToolbarEffect, number> = { spectrum: 0.6, dots: 0.8, bars: 0.7 };

// The music's numbers among HEAD's uniforms, by slot.
const BEAT_SLOT = BEAT;
const LEVEL_SLOT = LEVEL;
const BASS_SLOT = BASS;
const MID_SLOT = MID;
const TREBLE_SLOT = TREBLE;
const SINCE_SLOT = SINCE;
const ONSET_SLOT = ONSET;
const PHASE_SLOT = PHASE;
const CLOCK_SLOT = CLOCK;

/** 每一效果的 uniforms（call in a component's body）。颜色的 slot 从
 * theme tokens 读；音乐的数值是 visualiser 里的 animated values（帧驱
 * 动），clock 与 phase 是两条原生 loop（无 JS 每帧）。reduced motion 或非
 * playing：全部静止 0。 */
function lightUniforms(props: { effect: ToolbarEffect; playing: boolean }) {
  // TEMP-VERIFY：debugRest 宿主里 paused 也喂 rest 电平（截图验证用）。
  const on = () => props.playing && !reduceMotion();
  const v = live.values;
  effect(() => {
    if (!on()) {
      for (let i = 0; i < SLOTS; i++) v[i].set(0);
      v[SINCE_SLOT].set(2);
    }
  });
  // Time moves the light while it plays: the clock and the colours' travel are
  // stepped with the music's own frames (visualiser's PHASE/CLOCK slots, ~60
  // a second), not with the display — a 144 Hz screen would otherwise redraw
  // the shader four times per audio frame (spotish #35).
  const bands = (from: number) => [v[BAND0 + from], v[BAND0 + from + 1], v[BAND0 + from + 2], v[BAND0 + from + 3]];
  // 颜色：无封面取色管线 —— primary（accent家族）主色 + chart-3/chart-4 次色。
  const palette = () => {
    const c0 = String(token("primary"));
    const c1 = String(token("chart-3"));
    const c2 = String(token("chart-4"));
    return { c0, c1, c2, c3: c1 };
  };
  return {
    on,
    uniforms: (extra: { strength: number; radius: number; reach: number }) => ({
      ...palette(),
      base: "#00000000",
      b0: bands(0),
      b1: bands(4),
      b2: bands(8),
      b3: bands(12),
      beat: v[BEAT_SLOT],
      level: v[LEVEL_SLOT],
      bass: v[BASS_SLOT],
      mid: v[MID_SLOT],
      treble: v[TREBLE_SLOT],
      ...extra,
      phase: v[PHASE_SLOT] as AnimatedValue,
      clock: v[CLOCK_SLOT] as AnimatedValue,
      since: v[SINCE_SLOT],
      onset: v[ONSET_SLOT],
    }),
  };
}

function createAnimated(): AnimatedValue {
  return AnimatedCreate(0);
}

/** The player's surface behind `children` (`class` gives size/padding):
 * the light over it (cheap, with the music), the controls on top as they
 * are. Data 的绳线是 visualiser 的 animated values（不是每帧 JS）。 */
export function PlayerGlass(props: {
  effect: ToolbarEffect;
  playing: boolean;
  radius?: number;
  class?: string;
  testID?: string;
  children: JSX.Element;
}) {
  const light = lightUniforms(props);
  const radius = () => props.radius ?? 9999;
  return (
    <View testID={props.testID} class={`relative ${props.class ?? ""}`}>
      {/* The surface: the player's colour, opaque, with a faint rim; reads
          nothing behind it, so a page scrolling under it costs no picture. */}
      <View
        accessibilityElementsHidden
        class="absolute inset-0 border border-border"
        style={{ backgroundColor: String(token("player")), borderRadius: radius() }}
      />
      <Shader
        source={LIGHTS[props.effect]}
        accessibilityElementsHidden
        paused={!light.on()}
        class="absolute overflow-hidden inset-0"
        style={{ borderRadius: radius() }}
        uniforms={light.uniforms({ strength: 0.45, radius: radius(), reach: 40 })}
      />
      {props.children}
    </View>
  );
}

/** The music's light along the bottom of its parent (`absolute` first
 * child), the crests reaching up to `reach` points; only that strip
 * draws. */
export function MusicLight(props: { effect: ToolbarEffect; playing: boolean; reach: number; class?: string }) {
  const light = lightUniforms(props);
  const reach = () => props.reach * REACH[props.effect];
  return (
    <Shader
      source={LIGHTS[props.effect]}
      accessibilityElementsHidden
      paused={!light.on()}
      class={`absolute inset-x-0 bottom-0 ${props.class ?? ""}`}
      style={{ height: Math.ceil(reach() + 24) }}
      uniforms={light.uniforms({ strength: 0.7, radius: 0, reach: reach() })}
    />
  );
}

