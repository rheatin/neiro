// The player bar the pages scroll behind: a 72pt pill, a step lighter than
// the page, its rim faint and its shadow small. What plays and its
// controls only — the engine is behind the pill, so nothing pretends
// (shuffle and repeat do not exist here and are not drawn).

import { View } from "nsh";
import { IconButton } from "../ui/controls";
import { animateLayoutOn, enter } from "../ui/motion";
import { barSize } from "../layout";
import { NowPlayingSummary, SeekBar, TransportControls, VolumeControl } from "./parts";
import { setOutputOpen, setQueueOpen } from "../../state/overlays";
import { lightEffect } from "../../state/visual";
import { PlayerGlass } from "../ui/toolbar-fx";
import { hasTrack, player } from "../../state/player";
import { isFavourite, toggleFavourite } from "../../state/user-catalog";

/** 播放条的红心：当前曲目收藏与否（`liked` 色实心 / 描边置灰）；点击
 * 调 `setFavourite`，集合到来后差值更新。 */
function PlaybarHeart() {
  const path = () => player().path ?? "";
  const liked = () => isFavourite(path());
  // Always drawn: a heart that appears when playback starts moves the
  // transport under the pointer. Greyed and unpressed is the honest state
  // when there is nothing to like.
  return (
    <IconButton
      name={liked() ? "heartFilled" : "heart"}
      label={liked() ? "取消收藏" : "收藏"}
      pressed={liked()}
      pressedColor="liked"
      size={18}
      disabled={!hasTrack()}
      onPress={() => toggleFavourite(path())}
    />
  );
}

/** The desktop's bar: what plays at the left, the transport over the
 * position in the middle, the volume at the right, in a flat pill
 * floating over the pages; it rises in. When it has too little room it
 * keeps one row (compact), then two (stacked), each arrangement keeping
 * what fits rather than running out of the pill. */
export function PlayerBar() {
  // Crossing a bar-size breakpoint, the contents glide to their new
  // arrangement.
  animateLayoutOn(() => barSize());
  const stacked = () => barSize() === "stacked";
  return (
    <View style={enter("fadeInUp", 120)}>
      <View class={stacked() ? "relative rounded-[36px] shadow-xl" : "relative rounded-full shadow-xl"}>
        {/* PlayerGlass: pill 的表面 + 一层低强度的同一光（PlayerGlass 的
            strength 0.45；surface token player 不透明）——照 spotish。 */}
        <PlayerGlass
          effect={lightEffect()}
          playing={player().state === "playing"}
          radius={stacked() ? 36 : 9999}
          class={
            stacked()
              ? "gap-2 rounded-[36px] px-3.5 pb-2.5 pt-3.5"
              : "flex-row h-[72px] items-center justify-between gap-4 rounded-full pl-3.5 pr-4"
          }
          testID="player"
        >
          <If
            when={barSize() === "full"}
            else={
              <If
                when={stacked()}
                else={
                  // Compact: one row — what plays takes the room on the
                  // left, the transport on the right. No position or volume
                  // (Now Playing has both).
                  <View class="min-w-0 flex-1 flex-row items-center justify-between gap-2">
                    <NowPlayingSummary openable round cover={44} />
                    <TransportControls />
                  </View>
                }
              >
                {/* Stacked: two rows — what plays, centred, over the
                    transport, centred. */}
                <View class="h-11 flex-row items-center">
                  <NowPlayingSummary openable center round cover={44} />
                </View>
                <View class="h-10 flex-row items-center justify-center">
                  <TransportControls />
                </View>
              </If>
            }
          >
            {/* Full: what plays at the left quarter, the transport over an
                inline position in the middle, the volume at the right. */}
            <View class="min-w-0 w-1/4 flex-row items-center gap-1">
              {/* A smaller round cover, concentric with the pill: 14pt from
                  its edge on every side (72 = 44 + 2 × 14). The heart rides
                  beside it. */}
              <NowPlayingSummary openable round cover={44} />
            </View>
            <View class="max-w-180 flex-1 items-center gap-1">
              {/* The heart rides with the transport, as it does on Now
                  Playing: the two screens read the same. */}
              <TransportControls trail={<PlaybarHeart />} />
              <SeekBar inline />
            </View>
            <View class="w-1/4 flex-row items-center justify-end gap-1 pr-2">
              <VolumeControl collapsible />
              <IconButton name="queue" label="待播" hint size={18} onPress={() => setQueueOpen(true)} />
              <IconButton name="info" label="输出信息" hint size={18} onPress={() => setOutputOpen(true)} />
            </View>
          </If>
        </PlayerGlass>
      </View>
    </View>
  );
}
