// 待播列表：引擎队列里"当前这条之后"的部分。它不复制队列——每行直接读
// `onQueue` 的镜像，所以引擎的下一曲、删队、清队都立刻反映在这里。
//
// 形状照 spotish 的 Now Playing 队列段：一条带序号的紧凑行（封面 + 标题 +
// 歌手 + 时长），正在播放的那条有标记；每行 hover 露出移除按钮。

import { List, Text, View } from "nsh";
import { Button } from "nsh/base";
import { clock, type Track } from "../../backend/types";
import {
  clearUpNext,
  clearUserQueue,
  contextQueue,
  queue,
  queueIndex,
  queueSnapshot,
  removeFromQueue,
  removeUserItem,
  upNext,
  userQueue,
} from "../../state/queue";
import { trackForPath } from "../../state/catalog";
import { playQueue, playQueueAt, playTrack } from "../../state/player";
import { Cover } from "../ui/cover";
import { Digits } from "../ui/digits";
import { Icon } from "../ui/icons";
import { focusRing, ActionButton } from "../ui/controls";
import { EmptyState } from "../ui/feedback";
import { PlayingBars } from "../ui/playing";

/** One queue row: its place, cover, title and artist, and its length. A
 * press plays it (the engine's queue is the order, so playing a later entry
 * is `play` of that path — the rest stays queued after it). */
function QueueRow(props: {
  path: string;
  title?: string;
  artist?: string;
  durationMs?: number;
  index: number;
  current: boolean;
  canRemove?: boolean;
  onPlay?: () => void;
  onRemove?: () => void;
}) {
  const track = (): Track | undefined => trackForPath(props.path);
  const displayTitle = () => props.title || track()?.title || props.path.split(/[\\/]/).pop() || "—";
  const displayArtist = () => props.artist || track()?.artist || "—";
  const displayDuration = () => props.durationMs ?? track()?.durationMs ?? 0;

  return (
    <View class="group w-full flex-row items-center gap-2 rounded-(--radius) px-2 py-1 hover:bg-row-hover">
      <Button
        accessibilityLabel={props.current ? `正在播放：${displayTitle()}` : `播放：${displayTitle()}`}
        disabled={props.current}
        onPress={() => {
          if (props.onPlay) props.onPlay();
          else playQueueAt(props.index);
        }}
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
            {displayTitle()}
          </Text>
          <Text class="line-clamp-1 text-[10px] text-muted-foreground">{displayArtist()}</Text>
        </View>
        <Digits class="text-[10px] text-muted-foreground shrink-0">{clock(displayDuration(), true)}</Digits>
      </Button>
      <If when={!props.current && props.canRemove !== false}>
        <Button
          accessibilityLabel="移出待播"
          onPress={() => {
            if (props.onRemove) props.onRemove();
            else removeFromQueue(props.index);
          }}
          class={`shrink-0 items-center justify-center rounded-(--radius) p-1 opacity-0 transition duration-150 ease-out group-hover:opacity-100 hover:bg-muted active:opacity-60 motion-reduce:transition-none ${focusRing}`}
        >
          <Icon name="close" size={12} color="muted-foreground" />
        </Button>
      </If>
    </View>
  );
}

/** The 待播 panel: what plays now, then user queue (Up Next), then context queue. */
export function QueuePanel() {
  const current = () => {
    const snap = queueSnapshot();
    if (snap?.current) return snap.current.path;
    const q = queue();
    return q.index === null ? null : q.queue[q.index] ?? null;
  };

  const snap = () => queueSnapshot();
  const uQueue = () => userQueue();
  const cQueue = () => contextQueue();
  const rest = () => upNext();

  return (
    <View class="w-full gap-3 rounded-(--radius-panel) border border-border bg-card p-3" accessibilityLabel="待播列表">
      {/* 正在播放 */}
      <View class="gap-1.5">
        <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground px-1">正在播放</Text>
        <If when={current()} else={<Text class="px-2 py-1 text-[11px] text-muted-foreground">没有正在播放的曲目</Text>}>
          {(path) => <QueueRow path={path()} index={queueIndex() ?? 0} current />}
        </If>
      </View>

      {/* 插播队列 (Up Next) */}
      <If when={uQueue().length > 0}>
        <View class="gap-1.5">
          <View class="flex-row items-center justify-between px-1">
            <Text class="text-[11px] font-bold uppercase tracking-wider text-primary">
              插播队列 (Up Next) · {uQueue().length} 首
            </Text>
            <Button
              accessibilityLabel="清空插播"
              onPress={clearUserQueue}
              class={`rounded px-1.5 py-0.5 text-[10px] text-muted-foreground hover:bg-muted hover:text-foreground ${focusRing}`}
            >
              清空
            </Button>
          </View>
          <View class="gap-0.5">
            <Each of={uQueue()} key={(item) => item.id}>
              {(item, i) => (
                <View class="group w-full flex-row items-center gap-2 rounded-(--radius) px-2 py-1 hover:bg-row-hover">
                  <View class="w-4 items-center shrink-0">
                    <Text class="text-[11px] tabular-nums text-primary font-medium">{i() + 1}</Text>
                  </View>
                  <Cover path={item().path} size={28} fallback="music" />
                  <View class="min-w-0 flex-1 gap-0.5">
                    <Text class="line-clamp-1 text-xs font-medium text-foreground">
                      {item().title || item().path.split(/[\\/]/).pop() || "—"}
                    </Text>
                    <Text class="line-clamp-1 text-[10px] text-muted-foreground">{item().artist || "—"}</Text>
                  </View>
                  <Digits class="text-[10px] text-muted-foreground shrink-0">{clock(item().durationMs ?? 0, true)}</Digits>
                  <Button
                    accessibilityLabel="移出插播"
                    onPress={() => removeUserItem(item().id)}
                    class={`shrink-0 items-center justify-center rounded-(--radius) p-1 opacity-0 transition duration-150 ease-out group-hover:opacity-100 hover:bg-muted active:opacity-60 motion-reduce:transition-none ${focusRing}`}
                  >
                    <Icon name="close" size={12} color="muted-foreground" />
                  </Button>
                </View>
              )}
            </Each>
          </View>
        </View>
      </If>

      {/* 接下来播放 (上下文队列) */}
      <View class="gap-1.5">
        <View class="flex-row items-center justify-between px-1">
          <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">
            接下来播放 · {snap() ? cQueue().length : rest().length} 首
          </Text>
          <If when={!snap() && rest().length > 0}>
            <Button
              accessibilityLabel="清空待播"
              onPress={clearUpNext}
              class={`rounded px-1.5 py-0.5 text-[10px] text-muted-foreground hover:bg-muted hover:text-foreground ${focusRing}`}
            >
              清空
            </Button>
          </If>
        </View>

        <If
          when={snap() ? cQueue().length > 0 : rest().length > 0}
          else={<Text class="px-2 py-1 text-[11px] text-muted-foreground">没有更多待播曲目。曲目行右键选择"下一首播放"或"最后播放"。</Text>}
        >
          <View class="gap-0.5">
            <If when={snap() && cQueue().length > 0} else={
              <Each of={rest()} key={(path, i) => `${path}_${i}`}>
                {(path, i) => <QueueRow path={path()} index={(queueIndex() ?? 0) + i() + 1} current={false} />}
              </Each>
            }>
              <Each of={cQueue()} key={(item) => item.id}>
                {(item, i) => {
                  const playContextFromHere = () => {
                    const all = cQueue();
                    const tracks: Track[] = all.map((it) => trackForPath(it.path) ?? {
                      path: it.path,
                      title: it.title ?? "",
                      artist: it.artist ?? "",
                      album: "",
                      albumArtist: "",
                      discNo: 0,
                      trackNo: 0,
                      year: 0,
                      genre: "",
                      durationMs: it.durationMs ?? 0,
                      sampleRate: 0,
                      bits: 0,
                      channels: 0,
                    });
                    playQueue(tracks, i());
                  };
                  return (
                    <QueueRow
                      path={item().path}
                      title={item().title}
                      artist={item().artist}
                      durationMs={item().durationMs}
                      index={i()}
                      current={false}
                      canRemove={false}
                      onPlay={playContextFromHere}
                    />
                  );
                }}
              </Each>
            </If>
          </View>
        </If>
      </View>
    </View>
  );
}
