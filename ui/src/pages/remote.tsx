// 远程页: the server's tracks (the whole list is the source; search
// filters it), its playlists, and the fetch→played round trip per row.
// The container (FLAC/MP3/…, what the engine will fetch) rides beside the
// row's length.
// 
// 三态: 引擎未连接 never gets here; not configured never gets here either
// (the sidebar shows 连接服务器 instead); configured but the server
// unreachable is the error+重试 state below.

import { Scroll, Text, effect, signal, untracked, View, token } from "nsh";
import { Button, Input } from "nsh/base";
import { Digits } from "../components/ui/digits";
import { clock, type RemoteTrack } from "../backend/types";
import {
  cachedRemotePath,
  filteredRemoteTracks,
  isFetchingRemote,
  loadRemote,
  remoteError,
  remoteIsFresh,
  playRemote,
  queueRemoteFrom,
  remoteLoadingState,
  remoteQuery,
  remoteTracks,
  retryRemote,
  setRemoteQuery,
  withRemotePath,
} from "../state/remote";
import { engine } from "../backend/engine";
import { open } from "../nav";
import { focusRing, ActionButton } from "../components/ui/controls";
import { setServerDialogOpen } from "../state/overlays";
import { RemoteCover } from "../components/ui/cover";
import { EmptyState, ErrorState, RowsSkeleton, Spinner } from "../components/ui/feedback";
import { openAddToPlaylist } from "../components/library/playlist-dialogs";
import { openContextMenu } from "../components/ui/context-menu";
import { addToQueue } from "../state/queue";
import { isFavourite, toggleFavourite } from "../state/user-catalog";
import { notify } from "../state/feedback";
import { AppPage } from "../components/page";
import { enter } from "../components/ui/motion";
import { Icon } from "../components/ui/icons";
import { TrackHeader } from "../components/library/tracks";
import { wideTracks } from "../components/layout";

const CONTAINER_LABEL = (container: string): string => (container ? container.toUpperCase() : "").slice(0, 5);

/** One remote track: its cover, name+artist+container 徽标, and its play
 * column that fetches (转圈) then plays the cached file. */
export function RemoteTrackRow(props: {
  track: RemoteTrack;
  number: number;
  showAlbum?: boolean;
  /** 点了这一行要播什么(以及后面几首);不给就是只播它自己。 */
  onPlay?: () => void;
}) {
  const track = () => props.track;
  const fetching = () => isFetchingRemote(track().id);
  // 远程行没有本地路径：收藏/加入曲单要等引擎把文件取进缓存，拿到那条
  // 缓存路径再执行。右键菜单和本地行同一套动作。
  const cachedPath = () => cachedRemotePath(track());
  const liked = () => isFavourite(cachedPath() ?? "");
  // 远程行的菜单与本地行同一套（应用的表面，不是系统菜单）。远程行没有
  // 本地路径，需要路径的动作先取流。
  const onMenu = (at: { x: number; y: number }) =>
    openContextMenu(at, [
      {
        label: "加入待播",
        onSelect: () => {
          const path = cachedPath();
          if (path) addToQueue([path]);
          else withRemotePath(track(), (fetched) => addToQueue([fetched]));
        },
      },
      {
        label: liked() ? "取消收藏" : "加入红心",
        separated: true,
        onSelect: () => {
          const path = cachedPath();
          if (path) toggleFavourite(path);
          else withRemotePath(track(), (fetched) => toggleFavourite(fetched));
        },
      },
      {
        label: "添加到曲单…",
        onSelect: () => {
          const path = cachedPath();
          if (path) openAddToPlaylist(path);
          else withRemotePath(track(), (fetched) => openAddToPlaylist(fetched));
        },
      },
    ]);
  return (
    <View class="mx-6 flex-row items-center" onContextMenu={(e) => onMenu(e.pagePosition)}>
    <Button
      accessibilityLabel={`${track().title}, ${track().artist || track().album || "远程"}`}
      onPress={() => (props.onPlay ? props.onPlay() : playRemote(track()))}
      class={`group h-12 flex-1 flex-row items-center gap-3 rounded-(--radius) px-3 transition duration-150 ease-out hover:bg-row-hover active:bg-muted ${focusRing}`}
    >
      <Text class="w-6 text-right text-xs tabular-nums text-muted-foreground">{props.number + 1}</Text>
      <RemoteCover id={track().id} coverArt={track().coverArt} size={36} />
      <View class="min-w-0 flex-1 gap-0.5">
        <Text class="line-clamp-1 text-sm font-medium text-foreground">{track().title || "—"}</Text>
        <Text class="line-clamp-1 flex-1 text-xs text-muted-foreground">{track().artist || track().album || "—"}</Text>
      </View>
      <If when={props.showAlbum === true}>
        <Text class="line-clamp-1 min-w-0 flex-1 text-xs text-muted-foreground">{track().album || "—"}</Text>
      </If>
      <If when={track().container}>
        <Text
          accessibilityLabel={`容器 ${track().container}`}
          class="rounded-sm bg-muted-foreground px-1.5 py-0.5 text-[9px] font-mono font-bold lowercase text-center text-background" style={{ minWidth: 42 }}
        >
          {CONTAINER_LABEL(track().container)}
        </Text>
      </If>
      <View class="w-12 items-end">
        <If when={fetching()} else={<Digits class="text-xs text-muted-foreground">{clock(track().durationMs, true)}</Digits>}>
          <Spinner label="取流中" />
        </If>
      </View>
    </Button>
    </View>
  );
}

/** The server's own列表, as the engine answers it. */
export function RemotePage() {
  // One retry per visit: a server that has just connected can answer its first
  // list with nothing, and the page used to need a second visit to show songs.
  const retried = signal(false);
  effect(() => {
    // The cached listing is what the page shows; the server is only asked
    // again when the cache is empty or past its freshness window.
    untracked(() => {
      if (!remoteIsFresh()) loadRemote();
    });
  });
  effect(() => {
    if (retried()) return;
    if (remoteLoadingState() === "idle" && remoteTracks().length === 0) {
      untracked(() => {
        retried.set(true);
        loadRemote();
      });
    }
  });
  const state = remoteLoadingState();
  const rows = filteredRemoteTracks();
  const kind = () => engine.remoteStatus().kind;
  const baseUrl = () => engine.remoteStatus().baseUrl;

  return (
    <AppPage title="远程库" testID="page-remote">
      <Scroll class="flex-1" contentClass="pb-28">
        <View class="px-6 pt-2 pb-2 gap-1" role="header">
          <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">
            {kind() === "navidrome" ? "Navidrome" : "Jellyfin"} · {baseUrl()}
          </Text>
          <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
            远程库
          </Text>
        </View>
        <If
          when={engine.remoteStatus().configured}
          else={
            <EmptyState title="没有连接的服务器" detail="在侧栏的远程库段连接一个服务器；凭据只留在本机。">
              <ActionButton label="连接服务器…" onPress={() => setServerDialogOpen(true)} />
            </EmptyState>
          }
        >
        <View class="flex-row items-center gap-3 px-6 pb-4">
          <Input
            value={remoteQuery()}
            onChange={({ value }) => setRemoteQuery(value)}
            placeholder="搜索远程曲目"
            accessibilityLabel="搜索远程库"
            returnKeyType="search"
            submitBehavior="submit"
            autoCapitalize="none"
            autoCorrect={false}
            class="flex-1 text-base text-foreground"
            style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
          />
          <If when={remoteLoadingState() === "loading"}>
            <Spinner label="读取远程列表" />
          </If>
        </View>

        <PlaylistsStrip />
        </If>

        <If
          when={state === "error" && rows.length === 0}
          else={<RemoteList rows={rows} />}
        >
          <ErrorState
            message={remoteError() ? `远程库没有读出来：${remoteError()}` : "远程库没有读出来。"}
            onRetry={retryRemote}
          />
        </If>
      </Scroll>
    </AppPage>
  );
}

function RemoteList(props: { rows: RemoteTrack[] }) {
  const total = remoteTracks().length;
  return (
    <If
      when={props.rows.length > 0 || total > 0}
      else={
        <If when={remoteLoadingState() === "loading"} else={<EmptyState title="服务器上没有曲目" detail="服务器连通了，但里的曲目是空的。" />}>
          <RowsSkeleton rows={8} header />
        </If>
      }
    >
      <View class="px-3">
        {/* The same header the local lists draw: a remote row is a track row
            with a different origin, not another kind of list. */}
        <TrackHeader showAlbum={wideTracks()} />
        <If when={remoteQuery().trim()}>
          <Text class="px-3 pb-2 text-xs text-muted-foreground">{props.rows.length} / {total} 首曲目中匹配</Text>
        </If>
        <Each of={props.rows}>
          {(track, i) => (
            <RemoteTrackRow
              track={track()}
              number={i()}
              showAlbum={wideTracks()}
              onPlay={() => queueRemoteFrom(props.rows, i())}
            />
          )}
        </Each>
        <If when={remoteLoadingState() === "loading" && remoteTracks().length > 0}>
          <View class="py-6 items-center justify-center gap-2">
            <Spinner label="读取远程列表" />
            <Text class="text-xs text-muted-foreground">已读 {remoteTracks().length} 首</Text>
          </View>
        </If>
      </View>
    </If>
  );
}

/** 播放列表：名称 + 首数；点开一首一条（下一页拉 `remote_playlist_items`）。 */
function PlaylistsStrip() {
  effect(() => {
      engine.listRemotePlaylists();
  });
  const playlists = engine.remotePlaylists;
  return (
    <If when={playlists().length > 0}>
      <View class="gap-2 px-6 pb-6" accessibilityLabel="远程播放列表">
        <Text class="flex-1 text-[11px] font-bold uppercase tracking-wider text-muted-foreground">播放列表</Text>
        <Scroll horizontal scrollbar={false} style={{ height: 44 }} contentStyle={{ paddingRight: 24 }}>
          <View class="flex-row gap-2 px-2">
            <Each of={playlists()}>
              {(list) => (
                <Button
                  onPress={() => open({ name: "remotePlaylist", id: list().id, playlist: list().name })}
                  class={`flex-row shrink-0 items-center gap-2 rounded-full border border-border bg-secondary px-4 py-2 hover:border-muted-foreground active:scale-[0.98] ${focusRing}`}
                >
                  <Icon name="folder" size={16} color="muted-foreground" />
                  <Text class="text-sm font-bold text-foreground">{list().name || "播放列表"}</Text>
                  <Text class="text-xs text-muted-foreground">{list().trackCount}</Text>
                </Button>
              )}
            </Each>
          </View>
        </Scroll>
      </View>
    </If>
  );
}

/** One playlist's items, their own page over the same rows and fetch. */
export function RemotePlaylistPage(props: { id: string; name: string }) {
  effect(() => {
    untracked(() => {
      try {
        engine.listRemotePlaylistItems(props.id, 0, 200);
      } catch (error) {
        console.warn("neiro: 播放列表没有请求:", String(error));
      }
    });
  });
  const page = engine.remoteItems();
  const rows: RemoteTrack[] = page && page.id === props.id ? page.tracks : [];

  return (
    <AppPage title={props.name || "播放列表"} testID={`page-remote-playlist-${props.id}`}>
      <Scroll class="flex-1" contentClass="pb-28">
        <View class="px-6 pt-2 pb-4 gap-1" role="header">
          <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">播放列表</Text>
          <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
            {props.name || "—"}
          </Text>
          <If when={rows.length > 0}>
            <Text class="text-xs text-muted-foreground">{rows.length} 首</Text>
          </If>
        </View>
        <If when={rows.length > 0} else={<RowsSkeleton rows={6} />}>
          <View class="px-3">
            <Each of={rows}>
              {(track, i) => (
                <RemoteTrackRow track={track()} number={i()} />
              )}
            </Each>
          </View>
        </If>
      </Scroll>
    </AppPage>
  );
}

// ---- 远程专辑/歌手详情页（首页来源 Tab 的推送页） ----------------------------

/** 远程专辑页：专辑名 + 歌手副行 + 组内曲目行。行点 `playRemote`。 */
export function RemoteAlbumPage(props: { serverId: string; album: string; artist: string }) {
  const rows = remoteTracks().filter(
    (track) => (track.album || "") === props.album && (track.artist || "") === props.artist,
  );
  return (
    <AppPage title={props.album} testID="page-remote-album">
      <Scroll class="flex-1" contentClass="pb-28">
        <View class="px-6 pt-2 pb-4 gap-1" role="header">
          <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">远程专辑</Text>
          <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
            {props.album || "—"}
          </Text>
          <Text class="text-sm text-muted-foreground">
            {props.artist || "未知歌手"} · {rows.length} 首
          </Text>
        </View>
        <If
          when={rows.length > 0}
          else={
            <EmptyState title="专辑没有曲目" detail="服务器列表刷新后再试；用首页的来源 Tab 回到这张专辑。" />
          }
        >
          <View class="px-3">
            <Each of={rows}>
              {(track, i) => <RemoteTrackRow track={track()} number={i()} />}
            </Each>
          </View>
        </If>
      </Scroll>
    </AppPage>
  );
}

/** 远程歌手页：歌手名 + 其曲目行。 */
export function RemoteArtistPage(props: { serverId: string; artist: string }) {
  const rows = remoteTracks().filter((track) => (track.artist || "") === props.artist);
  return (
    <AppPage title={props.artist} testID="page-remote-artist">
      <Scroll class="flex-1" contentClass="pb-28">
        <View class="px-6 pt-2 pb-4 gap-1" role="header">
          <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">远程歌手</Text>
          <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
            {props.artist || "—"}
          </Text>
          <Text class="text-sm text-muted-foreground">{rows.length} 首</Text>
        </View>
        <If
          when={rows.length > 0}
          else={
            <EmptyState title="歌手没有曲目" detail="服务器列表刷新后再试；用首页的来源 Tab 回到这位歌手。" />
          }
        >
          <View class="px-3">
            <Each of={rows}>
              {(track, i) => <RemoteTrackRow track={track()} number={i()} />}
            </Each>
          </View>
        </If>
      </Scroll>
    </AppPage>
  );
}
