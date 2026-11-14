// 喜欢的歌曲 / 曲单详情：一个 liked-at 心形色块的头（spotish Liked
// Songs）+ 曲目表。两张页面共用一份躯干：文件层的数据 facts 从整个曲目
// 索引来（在这里存曲单的是 paths），红心、右键和 row 的其他功能与本地
// 表一致。

import { effect, List, Text, untracked, View } from "nsh";
import { Button } from "nsh/base";
import { clock, trackTitle, type Track } from "../backend/types";
import { engine } from "../backend/engine";
import {
  deletePlaylist,
  favourites,
  loadPlaylistEntries,
  playlistEntries,
  removeFromPlaylist,
  tracksForPaths,
} from "../state/user-catalog";
import { ensureTracks, tracksState, visibleTracks } from "../state/catalog";

import { isCurrentTrack, playQueue, playTrack, toggle } from "../state/player";
import { openRenamePlaylist } from "../components/library/playlist-dialogs";
import { AppPage } from "../components/page";
import { barSize, pageEnd, wideTracks } from "../components/layout";
import { SelectionBar } from "../components/library/selection-bar";
import { selectionActive } from "../state/queue";
import { TrackHeader, TrackRow } from "../components/library/tracks";
import { CollectionSkeleton, EmptyState, ErrorState } from "../components/ui/feedback";
import { Icon } from "../components/ui/icons";
import { signal } from "nsh";
import { PlayButton, focusRing } from "../components/ui/controls";
import { createListWave, enter } from "../components/ui/motion";
import { currentRoute, open } from "../nav";
import { playlists as playlistList } from "../state/user-catalog";
import { setNewPlaylistOpen } from "../components/library/playlist-dialogs";
import { ActionButton } from "../components/ui/controls";

/** 收藏页：paths → 每一次 index 的行 — 喜欢里按收藏时间（合入顺序）排
 * 列。曲库索引未到时骨架。 */
export function LikedPage() {
  return <CollectionPage title="喜欢的歌曲" kind={{ type: "liked" }} />;
}

export function PlaylistPage(props: { id: number; playlist: string }) {
  return <CollectionPage title={props.playlist} kind={{ type: "playlist", id: props.id }} />;
}

/** 曲单 index: the sidebar's 曲单 row opens this, the way 专辑 and 歌手 open
 * theirs. One row per playlist, the same shape the library's rows use. */
export function PlaylistsPage() {
  const lists = () => playlistList();
  return (
    <AppPage title="曲单" testID="page-playlists">
      <View class={`flex-1 gap-3 px-6 pt-2 ${pageEnd()}`}>
        <View class="flex-row items-center gap-3">
          <Text role="header" class="text-3xl font-bold text-foreground">
            曲单
          </Text>
          <View class="flex-1" />
          <ActionButton label="新建曲单" onPress={() => setNewPlaylistOpen(true)} />
        </View>
        <If
          when={lists().length > 0}
          else={<EmptyState title="还没有曲单" detail="新建一张，从这里放想听的曲目。" />}
        >
          <Each of={lists()} key={(list) => list.id}>
            {(list) => (
              <Button
                accessibilityLabel={`曲单 ${list().name}`}
                onPress={() => open({ name: "playlist", id: list().id, playlist: list().name })}
                class={`flex-row items-center gap-3 rounded-(--radius) px-3 py-2.5 hover:bg-row-hover active:scale-[0.99] ${focusRing}`}
              >
                <Icon name="music" size={18} color="muted-foreground" />
                <Text class="min-w-0 flex-1 truncate text-sm font-bold text-foreground">{list().name || "—"}</Text>
                <Text class="shrink-0 text-xs tabular-nums text-muted-foreground">{list().trackCount}</Text>
              </Button>
            )}
          </Each>
        </If>
      </View>
    </AppPage>
  );
}

type CollectionKind = { type: "liked" } | { type: "playlist"; id: number };

/** The sorts a collection offers; "order" is the collection's own order. */
const SORTS: [SortKey, string][] = [
  ["order", "顺序"],
  ["title", "标题"],
  ["artist", "歌手"],
  ["album", "专辑"],
  ["duration", "时长"],
];
type SortKey = "order" | "title" | "artist" | "album" | "duration";

function CollectionPage(props: { title: string; kind: CollectionKind }) {
  // 收藏页需要整个索引；曲单页需要一条曲目的表 + 整个索引。两者对曲目
  // 表的实际行是 paths 的映射。
  effect(() => {
    untracked(() => {
      ensureTracks();
      if (props.kind.type === "playlist") loadPlaylistEntries(props.kind.id);
    });
  });
  const wave = createListWave({ isTop: () => {
    const route = currentRoute();
    return route.name === "playlist" || route.name === "liked";
  } });
  const liked = () => props.kind.type === "liked";

  const paths = () => (liked() ? [...favourites()] : playlistEntries(props.kind.type === "playlist" ? props.kind.id : null).map((entry) => entry.path));
  const order = () => tracksForPaths(paths());
  // 排序（spotish #38 的适用部分：本地曲目没有 addedAt/releaseDate，因此
  // 取能诚实的键）。顺序 = 曲单自己的顺序（默认）。
  const sortKey = signal<SortKey>("order");
  const compactRows = signal(false);
  const rows = () => {
    const list = [...order()];
    const key = sortKey();
    if (key === "title") list.sort((a, b) => (a.title || a.path).localeCompare(b.title || b.path));
    else if (key === "artist") list.sort((a, b) => a.artist.localeCompare(b.artist) || a.title.localeCompare(b.title));
    else if (key === "album") list.sort((a, b) => a.album.localeCompare(b.album) || a.trackNo - b.trackNo);
    else if (key === "duration") list.sort((a, b) => a.durationMs - b.durationMs);
    return list;
  };
  const complete = () => tracksState() === "complete";
  // The count is what the list can show: a path the index does not know (a
  // file that moved, a remote download whose server is disconnected) is not
  // a row, and counting it would promise a row that never comes.
  const count = () => rows().length;
  const totalTime = () => rows().reduce((sum, t) => sum + t.durationMs, 0);
  const first = () => rows()[0];
  const playingHere = () => !!first() && isCurrentTrack(first()!.path);

  return (
    <AppPage title={props.title} testID={liked() ? "page-liked" : `page-playlist-${props.kind.type === "playlist" ? props.kind.id : ""}`}>
      <List
        each={complete() ? rows() : []}
        keyExtractor={(t) => t.path}
        estimatedItemSize={48}
        header={
          complete() ? (
            <View class="pt-2">
              <UserHeader
                title={props.title}
                liked={liked()}
                count={count()}
                totalTime={totalTime()}
                playing={playingHere()}
                firstAvailable={!!first()}
                rows={complete() ? rows() : []}
              />
              <View class="flex-row flex-wrap items-center gap-1 px-6 pb-2">
                <Each of={SORTS}>
                  {(entry) => (
                    <Button
                      selected={sortKey() === entry()[0]}
                      onPress={() => sortKey.set(entry()[0])}
                      class={`rounded-(--radius) px-2 py-1 transition duration-150 ease-out ${
                        sortKey() === entry()[0] ? "bg-secondary font-bold text-foreground" : "text-muted-foreground hover:bg-row-hover"
                      } ${focusRing}`}
                    >
                      <Text class="text-[11px]">{entry()[1]}</Text>
                    </Button>
                  )}
                </Each>
                <View class="flex-1" />
                <Button
                  accessibilityLabel={compactRows() ? "退出紧凑视图" : "紧凑视图"}
                  onPress={() => compactRows.set(!compactRows())}
                  class={`rounded-(--radius) px-2 py-1 transition duration-150 ease-out ${
                    compactRows() ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-row-hover"
                  } ${focusRing}`}
                >
                  <Text class="text-[11px]">紧凑</Text>
                </Button>
              </View>
              <TrackHeader showAlbum={wideTracks()} />
            </View>
          ) : undefined
        }
        footer={() => <View class={pageEnd()} />}
        empty={() => (
          <If when={complete()} else={<CollectionSkeleton />}>
            <EmptyState title={liked() ? "还没有喜欢的曲目" : "这张曲单是空的"} detail={liked() ? "点曲目行或播放条的红心，曲目会在这里。" : "右键曲目行，添加到这张曲单！"} />
          </If>
        )}
        class="flex-1"
      >
        {(track, index) => (
          <TrackRow
            track={track()}
            number={index()}
            showAlbum={wideTracks()}
            wave={wave}
            compact={compactRows()}
            onRemove={props.kind.type === "playlist" && track() ? () => removeFromPlaylist(props.kind.type === "playlist" ? props.kind.id : 0, track()!.path) : undefined}
          />
        )}
      </List>
      <If when={selectionActive()}>
        <SelectionBar allPaths={rows().map((t) => t.path)} bottom={barSize() === "stacked" ? 128 : 96} />
      </If>
    </AppPage>
  );
}

/** The head: heart (色块) / 曲单 icon, name, facts and the play button
 * (从头播). With the playlist's重命名/删除 on the right (curly braces of
 * buttons on hover). */
function UserHeader(props: {
  title: string;
  liked: boolean;
  count: number;
  totalTime: number;
  playing: boolean;
  firstAvailable: boolean;
  rows: Track[];
}) {
  const playlistId = () => {
    const route = currentRoute();
    return route.name === "playlist" ? route.id : null;
  };
  const coverIn = untracked(() => enter("zoomIn"));
  const wordsIn = enter("fadeInUp", 150);
  const restIn = enter("fadeInUp", 250);
  const playIn = enter("zoomIn");
  return (
    <View class="flex-row items-end gap-4 px-6 pb-4 pt-4">
      {/* 色块：红心 tile for收藏，封面 tile for一张曲单。 */}
      <View style={coverIn}>
        <View
          class={`flex size-24 items-center justify-center rounded-(--radius) shadow-xl ${
            props.liked ? "bg-liked" : "bg-secondary"
          }`}
        >
          <Icon
            name={props.liked ? "heartFilled" : "music"}
            size={36}
            color={props.liked ? "liked-foreground" : "muted-foreground"}
          />
        </View>
      </View>
      <View class="min-w-0 flex-1 gap-1 self-stretch justify-center">
        <View style={wordsIn}>
          <Text role="header" class="line-clamp-1 text-3xl font-bold text-foreground">
            {props.title || "—"}
          </Text>
        </View>
        <View class="gap-1" style={restIn}>
          <Text class="text-xs text-muted-foreground">
            {props.count} 首{props.totalTime > 0 ? ` · ${clock(props.totalTime)}` : ""}
          </Text>
        </View>
      </View>
      <View class="shrink-0 flex-row items-center gap-1" style={playIn}>
        <If when={!props.liked && playlistId() !== null}>
          <Button accessibilityLabel="重命名曲单" onPress={() => openRenamePlaylist(playlistId() as number, props.title)} class={`rounded-(--radius) p-2 hover:bg-row-hover ${focusRing}`}>
            <Text class="text-xs text-foreground">重命名</Text>
          </Button>
          <Button
            accessibilityLabel="删除曲单"
            onPress={() => {
              if (typeof playlistId() === "number") {
                deletePlaylist(playlistId() as number);
                // 曲单页推在栈上：删除以后回到曲库主页面。
                open({ name: "tracks" });
              }
            }}
            class={`rounded-(--radius) p-2 hover:bg-row-hover ${focusRing}`}
          >
            <Text class="text-xs text-destructive">删除</Text>
          </Button>
        </If>
        <PlayButton
          playing={props.playing}
          size={44}
          disabled={!props.firstAvailable}
          onPress={() => {
            if (!props.rows[0]) return;
            if (isCurrentTrack(props.rows[0].path)) toggle();
            else playQueue(props.rows, 0);
          }}
        />
      </View>
    </View>
  );
}
