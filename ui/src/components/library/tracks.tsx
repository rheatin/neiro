/** One track: its number (or the playing mark while it is the one
 * loaded), cover, title and artist, album and length; the red heart on
 * hover (收藏 = `liked` 色实心）， and a trailing "⋯" menu with 加入红心 /
 * 添加到曲单, also opened by a right click on the row. `ContextMenu` cannot
 * be used here: inside a `List` cell it never opens. `onContextMenu` on the
 * row itself plus `Menu.popup` is the path the docs give, and it works in a
 * cell. A Button nested in the row's Button never receives a press, which
 * is why the "⋯" sits beside the row rather than in it. */
/** The container a local file is in, from its extension: `flac`, `m4a`, `ape`.
 * Empty when the path has none. */
function containerOf(path: string | undefined): string {
  const name = (path ?? "").split(/[\\/]/).pop() ?? "";
  const dot = name.lastIndexOf(".");
  if (dot <= 0) return "";
  return name.slice(dot + 1).toLowerCase().slice(0, 5);
}

export function TrackRow(props: {
  track: Track | undefined;
  number: number;
  showAlbum: boolean;
  /** The list's wave: the row comes in with it each time the page is
   * entered, and on its own when it is scrolled into view. */
  wave?: ListWave;
  /** 曲单页的"移除"（行尾的 ✕，hover 露出）。给了它才画。 */
  onRemove?: () => void;
  /** 紧凑行（收藏/曲单的"紧凑"视图）：隐藏序号与封面、行高 32。 */
  compact?: boolean;
  /** 点这一行要播什么。给了它，行就不再只播自己：一张列表里点一首是
   * "从这里开始"，后面那些本来就是接下来的歌。 */
  onPlay?: () => void;
}) {
  // The row slides in from the right and its cover flips open, once per
  // track per entry, made here and bound as plain values: the Button
  // re-reads its style whenever any prop changes, and a motion made in
  // that binding would play again each time.
  const { style: entrance, cover: coverIn } = untracked(() =>
    listItemEntrance({
      wave: props.wave,
      key: () => props.track?.path ?? "",
      index: () => props.number,
      axis: "translateX",
      turns: WAVE.rows,
    }),
  );
  const hover = iconMotion();
  const menuOpen = signal(false);
  const current = () => isCurrentTrack(props.track?.path ?? "");
  const playing = () => current() && isPlaying();
  return (
    <View class="mx-6 flex-row items-center" onContextMenu={(e) => openRowMenu(props.track, e.pagePosition)}>
      <Button
        accessibilityLabel={`${trackTitle(props.track)}, ${trackArtist(props.track)}`}
        selected={current() || isSelected(props.track?.path)}
        disabled={!ready()}
        onPress={() => {
          const track = props.track;
          if (!track) return;
          // 多选中，点行是"选中/取消"，不是播放：选中期间播放会让误触
          // 变成换歌。
          if (selectionActive()) {
            toggleSelected(track.path);
            return;
          }
          if (props.onPlay) props.onPlay();
          else playTrack(track);
        }}
        class={`group min-w-0 flex-1 ${props.compact ? "h-8" : "h-12"} flex-row items-center gap-3 rounded-(--radius) px-3 transition duration-150 ease-out hover:bg-row-hover active:bg-muted selected:bg-row-hover disabled:opacity-50 motion-reduce:transition-none ${focusRing}`}
        style={entrance}
      >
        <If when={selectionActive()}>
          <View class="w-6 items-center">
            <View
              class={`size-4 items-center justify-center rounded-sm border ${
                isSelected(props.track?.path) ? "border-primary bg-primary" : "border-input"
              }`}
            >
              <If when={isSelected(props.track?.path)}>
                <Icon name="check" size={11} color="primary-foreground" />
              </If>
            </View>
          </View>
        </If>
        <If when={!selectionActive()}>
        <If when={!props.compact}>
        <View class="w-6 items-end">
          <If
            when={current()}
            else={
              <View class="relative items-end justify-center">
                <Text class={`text-xs tabular-nums text-muted-foreground ${LAYER} group-hover:opacity-0 group-hover:-translate-y-1`}>
                  {props.track?.trackNo || props.number + 1}
                </Text>
                <View class="absolute right-0 opacity-0 scale-50 transition duration-150 ease-out group-hover:opacity-100 group-hover:scale-100 group-active:scale-90 motion-reduce:transition-none">
                  <View style={hover.style}>
                    <Icon name="play" size={12} color="foreground" />
                  </View>
                </View>
              </View>
            }
          >
            <PlayingBars playing={playing()} />
          </If>
        </View>
        </If>
        </If>
        <If when={!props.compact}>
        <View style={coverIn}>
          <Cover path={props.track?.path ?? ""} size={36} fallback="music" />
        </View>
        </If>
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class={`line-clamp-1 text-sm text-foreground ${current() ? "font-bold" : "font-medium"}`}>
            {trackTitle(props.track) || "—"}
          </Text>
          <Text class="line-clamp-1 flex-1 text-xs text-muted-foreground">{trackArtist(props.track) || "—"}</Text>
        </View>
        <If when={props.showAlbum}>
          <Text class="line-clamp-1 min-w-0 flex-1 text-xs text-muted-foreground">{props.track?.album || "—"}</Text>
        </If>
        <RowHeart path={props.track?.path} />
        <If when={props.onRemove}>
          <Button
            accessibilityLabel="从曲单移除"
            onPress={() => props.onRemove?.()}
            class={`shrink-0 items-center justify-center rounded-(--radius) opacity-0 transition duration-150 ease-out group-hover:opacity-100 hover:bg-muted active:opacity-60 motion-reduce:transition-none ${focusRing}`}
            style={{ width: 24, height: 24 }}
          >
            <Icon name="close" size={14} color="muted-foreground" />
          </Button>
        </If>
        {/* The container, the way a remote row names it: the file's own
            extension is the answer for a file on this disk. */}
        <If when={containerOf(props.track?.path) !== ""}>
          <Text
            accessibilityLabel={`容器 ${containerOf(props.track?.path)}`}
            class="rounded-sm bg-muted-foreground px-1.5 py-0.5 text-center text-[9px] font-mono font-bold lowercase text-background"
            style={{ minWidth: 42 }}
          >
            {containerOf(props.track?.path)}
          </Text>
        </If>
        <Digits class="w-12 text-right text-xs text-muted-foreground">{clock(props.track?.durationMs ?? 0, true)}</Digits>
      </Button>
      <Popover
        open={menuOpen()}
        onOpenChange={(e) => menuOpen.set(e.open)}
        placement="bottom"
        align="end"
        offset={4}
        popupClass=""
        trigger={
          <Button
            accessibilityLabel="曲目操作"
            class={`shrink-0 items-center justify-center rounded-(--radius) p-1 text-muted-foreground opacity-60 transition duration-150 ease-out hover:bg-muted hover:opacity-100 active:opacity-60 motion-reduce:transition-none ${focusRing}`}
          >
            <Icon name="dots" size={16} color="muted-foreground" />
          </Button>
        }
      >
        <MenuSurface items={rowActions(props.track, () => menuOpen.set(false))} />
      </Popover>
    </View>
  );
}

/** A track's actions, in the app's own menu (the native one is the system's
 * look, not this theme's). One list, used by the row's "⋯" and by a right
 * click; `close` runs after an action so the popover it is in shuts. */
export function rowActions(track: Track | undefined, close?: () => void): MenuAction[] {
  const path = track?.path;
  if (!path) return [];
  const run = (action: () => void) => () => {
    close?.();
    if (path.startsWith("remote://")) {
      const rt = remoteTrackForCachePath(path);
      if (rt) {
        withRemotePath(rt, (cachedPath) => {
          if (action === doQueue) addToQueue([cachedPath]);
          else if (action === doFav) toggleFavourite(cachedPath);
          else if (action === doPlayList) openAddToPlaylist(cachedPath);
          else if (action === doSelect) beginSelection(cachedPath);
        });
        return;
      }
    }
    action();
  };
  const doPlayNext = () => track && playNextTrack(track);
  const doPlayLater = () => track && playLaterTrack(track);
  const doQueue = () => addToQueue([path]);
  const doFav = () => toggleFavourite(path);
  const doPlayList = () => openAddToPlaylist(path);
  const doSelect = () => beginSelection(path);
  return [
    { label: "下一首播放", onSelect: run(doPlayNext) },
    { label: "最后播放", onSelect: run(doPlayLater) },
    { label: "加入待播", onSelect: run(doQueue) },
    { label: isFavourite(path) ? "取消收藏" : "加入红心", onSelect: run(doFav), separated: true },
    { label: "添加到曲单…", onSelect: run(doPlayList) },
    { label: "选择", onSelect: run(doSelect), separated: true },
  ];
}

/** The right-click menu at the pointer: the same actions, drawn by the app
 * rather than by the system. */
function openRowMenu(track: Track | undefined, at: { x: number; y: number }): void {
  openContextMenu(at, rowActions(track));
}

// Track lists: the header row and one track's row. A row is used in a
// recycling `List`, so it reads its track where it is used and keeps no
// state of its own.

import { signal, Text, untracked, View } from "nsh";
import { Button } from "nsh/base";

import { clock, trackArtist, trackTitle, type Track } from "../../backend/types";
import { isCurrentTrack, isPlaying, playLaterTrack, playNextTrack, playTrack } from "../../state/player";
import { ready } from "../../state/engine";
import { isFavourite, toggleFavourite } from "../../state/user-catalog";
import { addToQueue, beginSelection, isSelected, selectionActive, toggleSelected } from "../../state/queue";
import { remoteTrackForCachePath, withRemotePath } from "../../state/remote";
import { MenuSurface, openContextMenu, type MenuAction } from "../ui/context-menu";
import { openAddToPlaylist } from "./playlist-dialogs";
import { Cover } from "../ui/cover";
import { Digits } from "../ui/digits";
import { Icon, iconMotion } from "../ui/icons";
import { focusRing, LAYER, popupSurface } from "../ui/controls";
import { listItemEntrance, type ListWave, WAVE } from "../ui/motion";
import { PlayingBars } from "../ui/playing";
import { Popover } from "nsh/base";
import { cycleTrackSort, trackSortField, trackSortOrder, type TrackSortField } from "../../state/library-prefs";

/** The header row above a track list, with clickable interactive sort headers. */
export function TrackHeader(props: { showAlbum: boolean; interactive?: boolean }) {
  const isInteractive = () => props.interactive ?? true;
  const currentField = () => trackSortField();
  const currentOrder = () => trackSortOrder();

  const renderIndicator = (field: TrackSortField) => {
    if (currentField() !== field) return null;
    return (
      <Text class="font-mono text-[10px] font-bold text-primary">
        {currentOrder() === "asc" ? " ↑" : " ↓"}
      </Text>
    );
  };

  return (
    <View class="mx-6 mb-1 h-8 flex-row items-center gap-3 border-b border-border/60 px-3">
      {/* The number column has no heading; 标题 starts where the covers do. */}
      <View class="w-6" />
      <Button
        disabled={!isInteractive()}
        onPress={() => cycleTrackSort("title")}
        accessibilityLabel="按标题排序"
        class={`flex-1 flex-row items-center gap-1 ${focusRing} text-left`}
      >
        <Text
          class={`text-[11px] font-bold uppercase tracking-wider ${
            currentField() === "title" ? "text-primary font-extrabold" : "text-muted-foreground hover:text-foreground"
          }`}
        >
          标题
        </Text>
        {renderIndicator("title")}
      </Button>

      <If when={props.showAlbum}>
        <Button
          disabled={!isInteractive()}
          onPress={() => cycleTrackSort("album")}
          accessibilityLabel="按专辑排序"
          class={`flex-1 flex-row items-center gap-1 ${focusRing} text-left`}
        >
          <Text
            class={`text-[11px] font-bold uppercase tracking-wider ${
              currentField() === "album" ? "text-primary font-extrabold" : "text-muted-foreground hover:text-foreground"
            }`}
          >
            专辑
          </Text>
          {renderIndicator("album")}
        </Button>
      </If>

      <Button
        disabled={!isInteractive()}
        onPress={() => cycleTrackSort("duration")}
        accessibilityLabel="按时长排序"
        class={`w-12 flex-row items-center justify-end gap-0.5 ${focusRing}`}
      >
        <Icon
          name="clock"
          size={14}
          color={currentField() === "duration" ? "primary" : "muted-foreground"}
        />
        {renderIndicator("duration")}
      </Button>
    </View>
  );
}

/** The row's own heart: outlined grey while not liked, red once it is; one
 * per row, resting faint until hover (spotish's rule). */
export function RowHeart(props: { path?: string; always?: boolean }) {
  const liked = () => isFavourite(props.path);
  return (
    <Button
      accessibilityLabel={props.path ? "收藏" : "收藏"}
      selected={liked()}
      onPress={() => toggleFavourite(props.path)}
      class={`group/heart shrink-0 items-center justify-center rounded-(--radius) ${
        props.always ? "" : "opacity-0 transition duration-150 ease-out group-hover:opacity-100 motion-reduce:transition-none"
      } ${focusRing}`}
      style={{ width: 24, height: 24 }}
    >
      <Icon name={liked() ? "heartFilled" : "heart"} size={16} color={liked() ? "liked" : "muted-foreground"} />
    </Button>
  );
}
