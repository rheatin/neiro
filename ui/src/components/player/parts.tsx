// The player's parts, shared by the floating bar and Now Playing: the
// transport, the position, the volume and what plays. They read the player
// store and call its intents.

import { cleanup, createMotion, effect, signal, Text, untracked, View, type JSX, type MotionSpec } from "nsh";
import { Button } from "nsh/base";
import { lazyMotion } from "../ui/motion";
import { setNowPlayingOpen } from "../../state/overlays";
import {
  canNext,
  controlsEnabled,
  dragSeek,
  dragVolume,
  durationClock,
  hasTrack,
  isPlaying,
  next,
  player,
  position,
  positionClock,
  positionFill,
  seek,
  setVolume,
  toggle,
  volumeLevel,
} from "../../state/player";
import { ready } from "../../state/engine";
import { shuffle, shuffleSupported, toggleShuffle } from "../../state/shuffle";
import { notify } from "../../state/feedback";
import { Cover } from "../ui/cover";
import { Digits } from "../ui/digits";
import { BarSlider, focusRing, IconButton, PlayButton } from "../ui/controls";
import type { IconName } from "../ui/icons";

/** Shuffle, previous, play / pause, next. Shuffle 是 UI 开关（决定
 * playQueue 的排列，不是引擎状态）；远程来源点了给一句 toast（队列只装
 * 本地路径）。 */
export function TransportControls(props: { large?: boolean; minimal?: boolean; quiet?: boolean }) {
  const icon = () => (props.large ? 22 : 18);
  const usable = () => controlsEnabled();
  return (
    <View class={`flex-row items-center ${props.large ? "gap-6" : "gap-4"}`} role="toolbar" accessibilityLabel="播放">
      <IconButton
        name="shuffle"
        label="随机播放"
        hint
        size={icon()}
        pressed={shuffle()}
        onPress={() => {
          if (!shuffleSupported()) {
            notify("远程曲目需先取流，暂不支持随机播放");
            return;
          }
          toggleShuffle();
        }}
      />
      <IconButton name="skipBack" label="上一曲" size={icon()} disabled onPress={() => {}} />
      <PlayButton quiet={props.quiet} playing={isPlaying()} size={props.large ? 56 : 36} disabled={!ready() || !hasTrack()} onPress={toggle} />
      <IconButton name="skipForward" label="下一曲" size={icon()} disabled={!usable() || !canNext()} onPress={next} />
    </View>
  );
}

/** The playback position: a slider with the elapsed and total times beside
 * it (`inline`) or under it.
 *
 * `center` draws between the two times — the one place on this sheet that is
 * equidistant from both ends of the track, which is where a format verdict
 * belongs. */
export function SeekBar(props: { inline?: boolean; quiet?: boolean; center?: JSX.Element }) {
  const duration = () => Math.max(1, player().durationMs);
  // The times in mono digits.
  const times = () => `text-xs text-muted-foreground ${props.quiet ? "opacity-60" : ""}`;
  const slider = (
    <BarSlider
      quiet={props.quiet}
      label="播放进度"
      value={position()}
      max={duration()}
      fill={positionFill}
      disabled={!controlsEnabled()}
      class="flex-1"
      onChange={dragSeek}
      onChangeEnd={seek}
    />
  );
  if (props.inline) {
    return (
      <View class="w-full flex-row items-center gap-2">
        <View class="w-10 items-end">{elapsedTime()}</View>
        {slider}
        <View class="w-10">{totalTime()}</View>
      </View>
    );
  }
  return (
    <View class="w-full gap-1">
      <View class="flex-row">{slider}</View>
      {/* The clocks take the same width whatever they say: they are the row's
          two ends, and a `—` on one side would otherwise pull the middle slot
          off the row's centre — and the centre is the play button's line. */}
      <View class="flex-row items-center gap-3">
        <View class="w-12 items-start">{elapsedTime()}</View>
        <View class="min-w-0 flex-1 items-center">{props.center}</View>
        <View class="w-12 items-end">{totalTime()}</View>
      </View>
    </View>
  );
}

function elapsedTime() {
  return <Digits class={times0()}>{positionClock()}</Digits>;
}

function totalTime() {
  return <Digits class={times0()}>{player().durationMs > 0 ? durationClock() : "—"}</Digits>;
}

function times0() {
  return "text-xs text-muted-foreground";
}

/** The volume: its level glyph status (a level the engine has not
 * reported disables the slider) and the slider. */
export function VolumeControl(props: { class?: string; collapsible?: boolean; quiet?: boolean }) {
  const icon = (): IconName => {
    const v = volumeLevel();
    if (v === null) return "volume";
    if (v <= 0) return "volumeMute";
    if (v < 0.5) return "volumeLow";
    return "volume";
  };
  // Collapsible: the slider opens while the pointer is on the control,
  // while the keyboard is in it, and while it is dragged; the glyph stays
  // visible.
  const hovering = signal(false);
  const dragging = signal(false);
  const open = () => !props.collapsible || hovering() || dragging();
  return (
    <View class={`group flex-row items-center ${props.class ?? ""}`} onMouseEnter={() => hovering.set(true)} onMouseLeave={() => hovering.set(false)}>
      <View class="w-8 items-center" accessibilityLabel={`音量`} accessibilityState={{ disabled: volumeLevel() === null }}>
        <IconButton name={icon()} label="音量" disabled onPress={() => {}} />
      </View>
      {/* Its room stays; the slider slides out from the glyph and fades in
          (a width change would jump: layout does not animate). */}
      <If
        when={open()}
        else={
          <View class="w-0 overflow-hidden opacity-0" />
        }
      >
        <View class="w-20 overflow-hidden opacity-100 transition duration-300 ease-out motion-reduce:transition-none">
          <BarSlider
            quiet={props.quiet}
            label="音量"
            value={volumeLevel() ?? 0}
            max={1}
            disabled={volumeLevel() === null}
            class="w-full"
            onChange={(v) => {
              dragging.set(true);
              dragVolume(v);
            }}
            onChangeEnd={(v) => {
              dragging.set(false);
              setVolume(v);
            }}
          />
        </View>
      </If>
    </View>
  );
}

/** A new track's arrival: the cover settles in with a small overshoot, the
 * words rise after it. */
export const NEW_COVER: MotionSpec = {
  keyframes: [
    { opacity: 0.4, scale: 0.9 },
    { opacity: 1, scale: 1.04 },
    { opacity: 1, scale: 1 },
  ],
  duration: 420,
  easing: "easeOut",
};
const NEW_WORDS: MotionSpec = {
  keyframes: [
    { opacity: 0, translateY: 6 },
    { opacity: 1, translateY: 0 },
  ],
  duration: 320,
  delay: 80,
  easing: "easeOut",
};

/** What plays: the cover (`cover` points; 0 for none), which opens Now
 * Playing when `openable`, the title and the artist; words Base did not
 * report show as the file's name (never an invented title). */
export function NowPlayingSummary(props: { cover?: number; round?: boolean; openable?: boolean; large?: boolean; center?: boolean }) {
  // Each new track arrives: the cover, then the words (not the first one
  // shown, which comes in with the player).
  const coverIn = lazyMotion();
  const wordsIn = lazyMotion();
  const shownTrack = signal("");
  effect(() => {
    const now = `${player().path}|${player().title}|${player().trackSerial}`;
    if (now !== shownTrack() && hasTrack()) {
      void coverIn.play(NEW_COVER);
      void wordsIn.play(NEW_WORDS);
    }
    untracked(() => shownTrack.set(now));
  });
  cleanup(() => {
    coverIn.stop();
    wordsIn.stop();
  });
  const subtitle = () => {
    const p = player();
    if (p.artist) return p.artist;
    if (p.album) return p.album;
    return "—";
  };
  const title = () => {
    const p = player();
    return p.title || fileNameOf(p.path ?? "") || "没有播放";
  };
  const cover = () => (
    <View style={coverIn.style}>
      <Cover path={player().path ?? ""} size={props.cover ?? 56} round={props.round} />
    </View>
  );
  const words = (selectable: boolean) => (
    <View class={`min-w-0 flex-1 gap-0.5 ${props.center ? "items-center" : ""}`} style={wordsIn.style} selectable={selectable}>
      <Text class={`line-clamp-1 font-bold text-foreground ${props.large ? "text-3xl font-bold" : "text-sm"} ${props.center ? "text-center" : ""}`}>
        {title()}
      </Text>
      <Text class={`line-clamp-1 min-w-0 shrink text-muted-foreground ${props.large ? "text-base" : "text-xs"} ${props.center ? "text-center" : ""}`}>
        {subtitle()}
      </Text>
    </View>
  );
  const withCover = () => props.cover !== 0;
  // Openable: the whole of it (cover and words) opens Now Playing.
  return (
    <If
      when={props.openable && hasTrack()}
      else={
        <View class="min-w-0 flex-1 flex-row items-center gap-3">
          <If when={withCover()}>{cover()}</If>
          {/* Words that are only read can be selected and copied. */}
          {words(!props.openable)}
        </View>
      }
    >
      <Button
        testID="open-now-playing"
        accessibilityLabel={`打开演出页：${title()}`}
        onPress={() => setNowPlayingOpen(true)}
        class={`group min-w-0 flex-1 flex-row items-center gap-3 rounded-full pr-2 active:scale-[0.98] motion-reduce:active:scale-100 ${focusRing}`}
      >
        <If when={withCover()}>
          <View class="transition duration-150 ease-out group-hover:opacity-85 motion-reduce:transition-none">{cover()}</View>
        </If>
        {words(false)}
      </Button>
    </If>
  );
}

/** The file's name (a track Base never named). */
function fileNameOf(path: string): string {
  return path.replace(/\\/g, "/").split("/").pop() || "";
}
