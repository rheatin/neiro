// The app's styled controls, each wrapping a base component once (spotish's
// rule). Pages and app components compose these; they do not restyle base
// controls. Neiro is a desktop app only, so there is no terminal branch.

import { effect, reduceMotion, signal, untracked, Text, View, type AnimatedValue, type JSX } from "nsh";
import { Button, Slider, Tooltip } from "nsh/base";
import { SwapIcon, iconMotion, type IconColor, type IconName } from "./icons";
import { lazyMotion } from "./motion";
import { DISTANCE, DURATION, DURATION_CLASS, EASE_CLASS, SCALE, SMOOTH_OUT } from "./tokens";

/** The focus ring every control shares. */
export const focusRing = "focus:outline focus:outline-2 focus:outline-ring";

/** The popup surface every menu, popover and tooltip-like panel shares. */
export const popupSurface = "rounded-(--radius-card) border border-border bg-popover p-1 shadow-lg";

/** A menu row: compact, the same padding, radius and states everywhere. */
export const menuItem =
  "h-8 flex-row items-center rounded-(--radius) px-2.5 hover:bg-muted focus:bg-muted active:bg-muted disabled:opacity-40";

/** How an animated layer of a control moves between its states (rest,
 * hover, press): quick, eased, still with reduced motion. */
export const LAYER = `transition ${DURATION_CLASS.fast} ${EASE_CLASS.out} motion-reduce:transition-none`;

/** How a control, or a layer of it, presses in and settles back. */
const PRESS = `transition ${DURATION_CLASS.fast} ${EASE_CLASS.smoothOut} motion-reduce:transition-none`;

/** How a wash or a colour comes and goes: quicker than a press. */
const WASH = `transition ${DURATION_CLASS.instant} ${EASE_CLASS.out} motion-reduce:transition-none`;

/** A whole button pressed: a little smaller, not on reduced motion. */
const PRESSED = `active:scale-[${SCALE.pressSoft}] disabled:active:scale-100 motion-reduce:active:scale-100`;

/** A label coming in after it changed (the label morph, without its
 * blur): up a little from below as it fades in. */
const LABEL_IN = {
  keyframes: { opacity: [0, 1], translateY: [DISTANCE.small, 0] },
  duration: DURATION.slow,
  easing: SMOOTH_OUT,
};

/** A hint bubble, shown while the pointer rests on a control and as the
 * keyboard focus reaches it. */
export function Hint(props: { label: string }) {
  return (
    <Tooltip class="rounded-(--radius) bg-popover px-2 py-1 shadow-md" placement="right">
      <Text class="text-xs font-medium text-popover-foreground">{props.label}</Text>
    </Tooltip>
  );
}

export interface IconButtonProps {
  name: IconName;
  /** What it does: screen readers say it, and a tooltip can repeat it. */
  label: string;
  /** Points (default 18). */
  size?: number;
  /** A toggle that is on: drawn at full strength (the pressed colour unless
   * `pressedColor` says another — the liked heart's red)。 */
  pressed?: boolean;
  /** While `pressed`, this token's colour instead of plain strength. */
  pressedColor?: IconColor;
  disabled?: boolean;
  /** Shows `label` in a tooltip (for icons whose meaning is not obvious). */
  hint?: boolean;
  /** Selected (a navigation destination): screen readers say so. */
  selected?: boolean;
  onPress: () => void;
}

/** A borderless icon button: brighter on hover, a dot while pressed
 * (unless `pressedColor` says another colour—a heart's `liked` red—
 * which is drawn itself, dot-less). */
export function IconButton(props: IconButtonProps) {
  const size = () => props.size ?? 18;
  const color = (): IconColor => {
    if (!props.pressed) return "foreground";
    return props.pressedColor ?? "foreground";
  };
  const hover = iconMotion();
  return (
    <View onMouseEnter={() => !props.disabled && hover.play(props.name)}>
      <Button
        accessibilityLabel={props.label}
        disabled={props.disabled ?? false}
        selected={props.selected ?? false}
        onPress={props.onPress}
        tooltip={props.hint ? <Hint label={props.label} /> : undefined}
        class={`group relative items-center justify-center rounded-full disabled:opacity-30 ${focusRing}`}
        style={{ width: size() + 16, height: size() + 16 }}
      >
        {/* Layer 1: a translucent disc that settles in under the pointer and
            deepens as it presses: whatever is behind shows through. */}
        <View
          class={`absolute inset-0 rounded-full bg-foreground/10 opacity-0 scale-[${SCALE.enter}] ${PRESS} group-hover:scale-100 group-hover:opacity-100 group-active:scale-[${SCALE.press}] group-active:bg-foreground/15 group-disabled:opacity-0`}
        />
        {/* Layer 2: the glyph brightens (its drawing morphs, `iconMotion`),
            then sinks as it presses; a new glyph swaps in (`SwapIcon`). */}
        <View
          class={`${PRESS} group-active:scale-[${SCALE.press}] group-disabled:scale-100 ${
            props.pressed ? "opacity-100" : "opacity-70 group-hover:opacity-100"
          }`}
        >
          <View style={hover.style}>
            <SwapIcon name={props.name} t={hover.t()} size={size()} color={color()} />
          </View>
        </View>
        <If when={(props.pressed ?? false) && !props.pressedColor}>
          {/* #25 软盘底：pressed 的 treated 折叠，取代原小圆点（liked 心
              不变，无 pressedColor 的 pressed即 soft disc）。 */}
          <View class="absolute inset-0 rounded-full bg-primary/15 transition duration-150 ease-out motion-reduce:transition-none" />
        </If>
      </Button>
    </View>
  );
}

/** The round play / pause button, `size` across. `quiet`: no border or
 * shadow, a faint disc (Now Playing, where nothing else leads). */
export function PlayButton(props: { playing: boolean; size?: number; disabled?: boolean; quiet?: boolean; onPress: () => void }) {
  const size = () => props.size ?? 36;
  const label = () => (props.playing ? "暂停" : "播放");
  const hover = iconMotion();
  return (
    <View onMouseEnter={() => !props.disabled && hover.play(props.playing ? "pause" : "play")}>
      <Button
        accessibilityLabel={label()}
        disabled={props.disabled ?? false}
        onPress={props.onPress}
        class={`group items-center justify-center overflow-hidden rounded-full ${
          props.quiet ? "bg-foreground/8" : "bg-background/80 shadow-md hover:shadow-lg active:shadow-sm"
        } ${PRESS} ${PRESSED} disabled:opacity-40 ${focusRing}`}
        style={{ width: size(), height: size() }}
      >
        {/* A shade that comes in on hover and deepens as it presses. */}
        <View class={`absolute inset-0 rounded-full bg-foreground opacity-0 ${WASH} group-hover:opacity-10 group-active:opacity-15 group-disabled:opacity-0`} />
        <View class={`${PRESS} group-active:scale-[${SCALE.press}] group-disabled:scale-100`}>
          <View style={hover.style}>
            <SwapIcon name={props.playing ? "pause" : "play"} t={hover.t()} size={size() * 0.4} color="foreground" />
          </View>
        </View>
      </Button>
    </View>
  );
}

/** A labeled button: `primary` for the one emphasized commitment of an
 * area (`bg-foreground`, spotish's rule), `outline` for the others. */
export function ActionButton(props: {
  label: string;
  variant?: "primary" | "outline";
  disabled?: boolean;
  onPress: () => void;
}) {
  const primary = () => props.variant === "primary";
  // A new label comes in (not the first: that one just shows).
  const labelMotion = lazyMotion();
  let shown = untracked(() => props.label);
  effect(() => {
    const now = props.label;
    if (now === shown) return;
    shown = now;
    if (!untracked(() => reduceMotion())) void labelMotion.play(LABEL_IN);
  });
  return (
    <Button
      disabled={props.disabled ?? false}
      onPress={props.onPress}
      class={`group h-9 items-center justify-center overflow-hidden rounded-full px-5 ${PRESS} ${PRESSED} disabled:opacity-40 ${focusRing} ${
        primary() ? "bg-foreground shadow-sm hover:shadow-md" : "border border-border bg-secondary hover:border-muted-foreground"
      }`}
    >
      {/* A wash that comes in over the button on hover and deepens as it
          presses. */}
      <View
        class={`absolute inset-0 rounded-full opacity-0 ${WASH} group-hover:opacity-10 group-active:opacity-15 group-disabled:opacity-0 ${
          primary() ? "bg-background" : "bg-foreground"
        }`}
      />
      <Text class={`text-sm font-bold ${primary() ? "text-background" : "text-foreground"}`} style={labelMotion.style}>
        {props.label}
      </Text>
    </Button>
  );
}

/** A quiet text button for a link that leaves the app (the `link` role):
 * underlined on hover, pressed in as a whole. */
export function TextLink(props: { label: string; onPress: () => void; small?: boolean; class?: string }) {
  return (
    <Button
      role="link"
      onPress={props.onPress}
      class={`group self-start rounded-sm ${PRESS} ${PRESSED} active:opacity-70 ${focusRing} ${props.class ?? ""}`}
    >
      <Text class={`font-bold text-primary hover:underline ${props.small ? "text-xs" : "text-sm"}`}>{props.label}</Text>
    </Button>
  );
}

/** A slider (position, volume): a track with a filled range, no thumb —
 * the bar itself is the control (it still takes the focus and screen
 * readers through the slider). */
export function BarSlider(props: {
  label: string;
  value: number;
  max: number;
  disabled?: boolean;
  /** Thin and faint, for where nothing else leads (Now Playing). */
  quiet?: boolean;
  class?: string;
  onChange: (value: number) => void;
  onChangeEnd: (value: number) => void;
  /** Where the range ends, 0..1, as an animated value that moves natively
   * (the position while it plays): the range follows it on the compositor,
   * every display frame, with no JS. */
  fill?: AnimatedValue;
  children?: JSX.Element;
}) {
  const fill = props.fill;
  // The range drawn natively from `fill`; the slider's own range stays for
  // its layout and states, transparent.
  const drawn = !!fill;
  return (
    <Slider
      accessibilityLabel={props.label}
      value={props.value}
      min={0}
      max={props.max}
      step={0}
      disabled={props.disabled ?? false}
      class={props.class}
      trackClass="group h-5 justify-center"
      // The range fills the track, so both share one centre line. One
      // height at rest and on hover (the taller one): it never jumps.
      indicatorClass={`rounded-full ${props.quiet ? "h-1.5 bg-foreground/8" : "h-2 bg-foreground/12"}`}
      rangeClass={`h-full rounded-full ${props.quiet ? "bg-foreground/35" : "bg-foreground/55"} disabled:bg-muted-foreground ${drawn ? "opacity-0" : ""}`}
      thumbClass="size-0 opacity-0"
      onChange={({ value }) => typeof value === "number" && props.onChange(value)}
      onChangeEnd={({ value }) => typeof value === "number" && props.onChangeEnd(value)}
    >
      {drawn ? (
        // Over the indicator (the track centres it), presses going through.
        <View class="pointer-events-none absolute inset-0 justify-center">
          <NativeRange fill={fill} quiet={props.quiet ?? false} disabled={props.disabled ?? false} />
        </View>
      ) : undefined}
    </Slider>
  );
}

/** The range of a `BarSlider` with `fill`: a pill as wide as the track
 * slides in from the left (its round end the range's), clipped by the
 * track. The compositor moves it every display frame from `fill`; nothing
 * is redrawn as it plays. */
function NativeRange(props: { fill: AnimatedValue; quiet: boolean; disabled: boolean }) {
  const width = signal(0);
  const fill = props.fill;
  return (
    <View
      class={`w-full overflow-hidden rounded-full ${props.quiet ? "h-1.5" : "h-2"}`}
      onLayout={(e) => width.set(e.bounds.size.width)}
    >
      <View
        class={`absolute inset-0 overflow-hidden rounded-full ${
          props.disabled ? "bg-muted-foreground" : props.quiet ? "bg-foreground/35" : "bg-foreground/55"
        }`}
        // Out of sight until the track has a width to slide along.
        style={{ opacity: width() > 0 ? 1 : 0, translate: fill.mul(width()).sub(width()) }}
      />
    </View>
  );
}

/** A section's title. */
export function SectionTitle(props: { children: JSX.Element; class?: string }) {
  return (
    <Text role="header" class={`text-lg font-bold text-foreground ${props.class ?? ""}`}>
      {props.children}
    </Text>
  );
}
