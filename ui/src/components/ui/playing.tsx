// The "playing here" mark: three bars in the theme's foreground. Without a
// meter in the Base protocol there is no bass/mid/treble to follow, so the
// bars keep their own quiet rhythm while it plays and rest, short, when it
// pauses or with reduced motion — animated values, no per-frame JS.

import { Animated, cleanup, createAnimatedValue, effect, reduceMotion, View, type AnimatedValue, type CompositeAnimation } from "nsh";

// Each bar's base share of the mark, and the phases it swings between.
const HEIGHTS = [0.4, 0.7, 0.5];

interface BarPulse {
  value: AnimatedValue;
  start(): void;
  rest(): void;
}

/** One bar swinging between its base and two neighbours' values, with a
 * hold, so the three never look in step. With reduced motion it rests. */
function barPulse(i: number): BarPulse {
  const value = createAnimatedValue(HEIGHTS[i]);
  const base = HEIGHTS[i];
  let running: CompositeAnimation | null = null;
  const stop = () => {
    running?.stop();
    running = null;
  };
  return {
    value,
    start() {
      if (running) return;
      running = Animated.loop(
        Animated.sequence([
          Animated.timing(value, { toValue: base + 0.18, duration: 420 + i * 130, easing: "easeInOut" }),
          Animated.timing(value, { toValue: base - 0.12, duration: 380 + i * 110, easing: "easeInOut" }),
          Animated.timing(value, { toValue: base, duration: 460 + i * 120, easing: "easeInOut" }),
          Animated.delay(900 + i * 350),
        ]),
        { resetBeforeIteration: false },
      );
      void running.start();
    },
    rest() {
      stop();
      value.set(base);
    },
  };
}

/** The mark, `size` points tall (default 14), resting when paused or with
 * reduced motion. */
export function PlayingBars(props: { playing: boolean; size?: number }) {
  const height = () => props.size ?? 14;
  const bars = HEIGHTS.map((_, i) => barPulse(i));
  cleanup(() => {
    for (const bar of bars) bar.rest();
  });
  effect(() => {
    const playing = props.playing && !reduceMotion();
    if (playing) for (const bar of bars) bar.start();
    else for (const bar of bars) bar.rest();
  });
  return (
    <View accessibilityLabel={props.playing ? "正在播放" : "已暂停"} class="flex-row items-end gap-[1.5px]" style={{ width: height(), height: height() }}>
      <Each of={bars}>
        {(bar) => (
          <View
            class="w-[2.5px] rounded-full bg-foreground/85"
            style={{ height: height(), transform: [{ scaleY: bar().value }], transformOrigin: "bottom center" }}
          />
        )}
      </Each>
    </View>
  );
}
