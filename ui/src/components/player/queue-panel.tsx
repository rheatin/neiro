// 待播列表：引擎队列里"当前这条之后"的部分。它不复制队列——每行直接读
// `onQueue` 的镜像，所以引擎的下一曲、删队、清队都立刻反映在这里。
//
// 形状照 spotish 的 Now Playing 队列段：一条带序号的紧凑行（封面 + 标题 +
// 歌手 + 时长），正在播放的那条有标记；每行 hover 露出移除按钮。

import { List, Text, View } from "nsh";
import { Button } from "nsh/base";
import { clock, type Track } from "../../backend/types";
import { clearUpNext, queue, queueIndex, removeFromQueue, upNext } from "../../state/queue";
import { trackForPath } from "../../state/catalog";
import { playQueueAt, playTrack } from "../../state/player";
import { Cover } from "../ui/cover";
import { Digits } from "../ui/digits";
import { Icon } from "../ui/icons";
import { focusRing, ActionButton } from "../ui/controls";
import { EmptyState } from "../ui/feedback";
import { PlayingBars } from "../ui/playing";

/** One queue row: its place, cover, title and artist, and its length. A
 * press plays it (the engine's queue is the order, so playing a later entry
 * is `play` of that path — the rest stays queued after it). */
function QueueRow(props: { path: string; index: number; current: boolean }) {
  const track = (): Track | undefined => trackForPath(props.path);
  return (
    <View class="group w-full flex-row items-center gap-2 rounded-(--radius) px-2 py-1 hover:bg-row-hover">
      <Button
        accessibilityLabel={props.current ? `正在播放：${track()?.title || "曲目"}` : `播放：${track()?.title || "曲目"}`}
        disabled={props.current}
        onPress={() => playQueueAt(props.index)}
        class={`min-w-0 flex-1 flex-row items-center gap-2 text-left ${focusRing}`}
      >
        <View class="w-4 items-center shrink-0">
          <If when={props.current} else={<Text class="text-[11px] tabular-nums text-muted-foreground">{props.index + 1}</Text>}>
            <PlayingBars playing />
          </If>
        </View>
        <Cover path={props.path} size={28} fallback="music" />
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class={`line-clamp-1 text-xs ${props.current ? "font-bold text-foreground" : "text-foreground"}`}>
            {track()?.title || props.path.split(/[\\/]/).pop() || "—"}
          </Text>
          <Text class="line-clamp-1 text-[10px] text-muted-foreground">{track()?.artist || "—"}</Text>
        </View>
        <Digits class="text-[10px] text-muted-foreground shrink-0">{clock(track()?.durationMs ?? 0, true)}</Digits>
      </Button>
      <If when={!props.current}>
        <Button
          accessibilityLabel="移出待播"
          onPress={() => removeFromQueue(props.index)}
          class={`shrink-0 items-center justify-center rounded-(--radius) p-1 opacity-0 transition duration-150 ease-out group-hover:opacity-100 hover:bg-muted active:opacity-60 motion-reduce:transition-none ${focusRing}`}
        >
          <Icon name="close" size={12} color="muted-foreground" />
        </Button>
      </If>
    </View>
  );
}

/** The 待播 panel: what plays now, then everything after it. */
export function QueuePanel() {
  const current = () => {
    const q = queue();
    return q.index === null ? null : q.queue[q.index] ?? null;
  };
  const rest = () => upNext();
  return (
    <View class="w-full gap-3 rounded-(--radius-panel) border border-border bg-card p-3" accessibilityLabel="待播列表">
      <View class="gap-1.5">
        <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground px-1">正在播放</Text>
        <If when={current()} else={<Text class="px-2 py-1 text-[11px] text-muted-foreground">没有正在播放的曲目</Text>}>
          {(path) => <QueueRow path={path()} index={queueIndex() ?? 0} current />}
        </If>
      </View>
      <View class="gap-1.5">
        <View class="flex-row items-center justify-between px-1">
          <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">
            接下来播放 · {rest().length} 首
          </Text>
          <If when={rest().length > 0}>
            <Button
              accessibilityLabel="清空待播"
              onPress={clearUpNext}
              class={`rounded px-1.5 py-0.5 text-[10px] text-muted-foreground hover:bg-muted hover:text-foreground ${focusRing}`}
            >
              清空
            </Button>
          </If>
        </View>
        <If when={rest().length > 0} else={<Text class="px-2 py-1 text-[11px] text-muted-foreground">待播是空的。曲目行右键，加入待播。</Text>}>
          <View class="gap-0.5">
            <Each of={rest()} key={(path) => path}>
              {(path, i) => <QueueRow path={path()} index={(queueIndex() ?? 0) + i() + 1} current={false} />}
            </Each>
          </View>
        </If>
      </View>
    </View>
  );
}
