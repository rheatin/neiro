// Lyrics in Now Playing: the file's own words, with the line that is playing
// marked when the file timestamped them.
//
// Nothing here invents a time. A file with plain lines is shown as plain
// lines; a file with LRC stamps highlights as the stream passes them. Before
// the engine has answered, the panel says it is reading rather than showing an
// empty box that could be mistaken for "no lyrics".

import { effect, Scroll, Text, token, untracked, View } from "nsh";
import { animateLayoutOn } from "../ui/motion";
import { player } from "../../state/player";
import {
  activeLine,
  askWriteLyrics,
  ensureLyrics,
  lyricOffset,
  nudgeLyricOffset,
  setLyricOffset,
  setLyricsSearchOpen,
  trackLyrics,
} from "../../state/lyrics";
import { seek } from "../../state/player";
import { openContextMenu } from "../ui/context-menu";
import { Button } from "nsh/base";
import { focusRing } from "../ui/controls";

/** How far a line is from the one playing, as a weight. The line at the
 * centre is the only one at full strength; the two either side of it are
 * legible but quiet, and the rest fade out — so the eye settles on the middle
 * without the surrounding words disappearing. */
const RAMP = [1, 0.55, 0.32, 0.2, 0.12];

function lineTone(distance: number): { opacity: number; color: string | undefined } {
  if (distance < 0) return { opacity: 0.55, color: token("muted-foreground") };
  const step = Math.min(distance, RAMP.length - 1);
  return {
    opacity: RAMP[step],
    color: distance === 0 ? token("primary") : token("muted-foreground"),
  };
}

export function LyricsPanel(props: { large?: boolean; hideLabel?: boolean }) {
  // One read per track: the answer is cached in the store, so reopening the
  // sheet or rewinding does not re-ask.
  effect(() => {
    const path = player().path ?? "";
    untracked(() => ensureLyrics(path));
  });
  const lyrics = () => trackLyrics();
  const active = () => activeLine(lyrics().lines, player().positionMs + lyricOffset());
  // Each new active line moves the window by one row; the layout animation is
  // what makes that read as the words sliding rather than jumping.
  animateLayoutOn(() => `${active()}|${lyrics().synced}|${lyrics().path}`);
  /** The lines in view: the one playing, centred, with four either side. A
   * timestamped file follows the playback this way instead of scrolling a
   * wall of text; an untimestamped one has no centre to follow, so it shows
   * every line evenly. */
  const window = () => {
    const lines = lyrics().lines;
    if (!lyrics().synced) return lines.map((line, index) => ({ line, index }));
    // Before the first timestamp nothing is playing yet, but the file *has*
    // timings: it belongs in the synced layout, centred on its first line.
    // Falling back to the untimed layout made the opening seconds look like a
    // different panel — small, left, two lines.
    const at = Math.max(active(), 0);
    const span = props.large ? 4 : 3;
    const from = Math.max(0, at - span);
    const to = Math.min(lines.length, at + span + 1);
    return lines.slice(from, to).map((line, offset) => ({ line, index: from + offset }));
  };
  return (
    <View
      class={`min-h-24 flex-1 justify-start ${props.large ? "gap-3" : "gap-1.5"}`}
      accessibilityLabel="歌词"
      onContextMenu={(e: { pagePosition: { x: number; y: number } }) =>
        openContextMenu(e.pagePosition, [
          { label: lyrics().present ? "换一份歌词…" : "在线搜索歌词…", onSelect: () => setLyricsSearchOpen(true) },
          { label: "写入文件…", onSelect: () => askWriteLyrics(), disabled: !lyrics().present },
          { label: "歌词提前 0.5 秒", onSelect: () => nudgeLyricOffset(-500), separated: true, disabled: !lyrics().synced },
          { label: "歌词延后 0.5 秒", onSelect: () => nudgeLyricOffset(500), disabled: !lyrics().synced },
          {
            label: lyricOffset() === 0 ? "已同步" : `已偏移 ${(lyricOffset() / 1000).toFixed(1)}s，重置`,
            onSelect: () => setLyricOffset(0),
            disabled: lyricOffset() === 0,
          },
        ])
      }
    >
      <If
        when={lyrics().present}
        else={
          <View class="flex-1 items-center justify-center">
            <Text class="text-sm text-muted-foreground">
              {player().path ? "这首歌没有内嵌歌词。" : "没有播放中的曲目。"}
            </Text>
          </View>
        }
      >
        <If
          when={lyrics().synced}
          else={
            // No timestamps: the same centred column, every line at one
            // weight, scrolled by hand. Nothing marks a "now", so nothing
            // follows the playback — but the panel is the same panel.
            <Scroll class="min-h-0 flex-1" contentClass="min-h-full flex-col items-center justify-center gap-2 px-1">
              <Each of={lyrics().lines}>
                {(line) => (
                  <Text
                    class={`text-center leading-[1.45] ${props.large ? "text-2xl" : "text-sm"} text-muted-foreground`}
                  >
                    {line().text}
                  </Text>
                )}
              </Each>
            </Scroll>
          }
        >
          <View class="min-h-0 flex-1 items-center justify-center gap-2 px-1">
            {/* Keyed by the line's own index: keyed by position in the
                slice, every row would change content when the window moves and
                the layout animation would draw the old and new line on top of
                each other. */}
            <Each of={window()} key={(row) => String(row.index)}>
              {(row) => {
                const distance = () => row().index - active();
                const tone = () => lineTone(Math.abs(distance()));
                const at = () => row().line.at;
                const seekHere = () => {
                  const seconds = at();
                  if (seconds === null) return;
                  seek(Math.max(0, seconds * 1000 - lyricOffset()));
                };
                const common = `text-center transition duration-500 ease-out ${
                  props.large ? "leading-[1.45]" : "leading-snug"
                } ${distance() === 0 ? (props.large ? "text-3xl font-bold" : "text-base font-bold") : props.large ? "text-2xl" : "text-sm"}`;
                return (
                  <If
                    when={at() !== null}
                    else={
                      <Text class={common} style={{ opacity: tone().opacity, color: tone().color }}>
                        {row().line.text}
                      </Text>
                    }
                  >
                    <Button
                      accessibilityLabel={`跳到 ${row().line.text}`}
                      onPress={seekHere}
                      class={`rounded-(--radius-inset) px-1 ${focusRing}`}
                    >
                      <Text class={`${common} hover:text-foreground`} style={{ opacity: tone().opacity, color: tone().color }}>
                        {row().line.text}
                      </Text>
                    </Button>
                  </If>
                );
              }}
            </Each>
          </View>
        </If>
      </If>
    </View>
  );
}
