// 远程页: the server's tracks (the whole list is the source; search
// filters it), its playlists, and the fetch→played round trip per row.
// The container (FLAC/MP3/…, what the engine will fetch) rides beside the
// row's length.
// 
// 三态: 引擎未连接 never gets here; not configured never gets here either
// (the sidebar shows 连接服务器 instead); configured but the server
// unreachable is the error+重试 state below.

import { computed, Scroll, Text, effect, signal, untracked, View, token } from "nsh";
import { Button, Input } from "nsh/base";
import { Digits } from "../components/ui/digits";
import { clock, type RemoteTrack } from "../backend/types";
import {
  cachedRemotePath,
  cachedRemoteTracks,
  filteredRemoteTracks,
  isFetchingRemote,
  loadRemote,
  remoteError,
  remoteAlbumGroups,
  remoteArtistGroups,
  activeRemoteServer,
  reconnectServer,
  removeServerById,
  remoteIsFresh,
  playRemote,
  playAllRemote,
  queueRemoteFrom,
  remoteLoadingState,
  remoteQuery,
  remoteTracks,
  retryRemote,
  setRemoteQuery,
  withRemotePath,
} from "../state/remote";
import { engine } from "../backend/engine";
import { rememberScroll } from "../state/page-scroll";
import { open, switchTo, HOME } from "../nav";
import { focusRing, ActionButton, PlayButton } from "../components/ui/controls";
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
import { layout, pageEnd, wideTracks } from "../components/layout";
import { RemoteCardsRow, type RemoteCardItem } from "../components/library/remote-cards";
import { shuffleMode } from "../state/player";
import { selectSource, ALL_ID } from "../state/sources";

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
  const retried = signal(false);
  effect(() => {
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

  const activeTab = signal<"tracks" | "albums" | "artists" | "playlists">("tracks");
  const srv = () => activeRemoteServer();
  const allTracksList = computed(() => remoteTracks());
  const albumsList = () => remoteAlbumGroups();
  const artistsList = () => remoteArtistGroups();
  const playlists = engine.remotePlaylists;

  const q = () => remoteQuery().trim().toLowerCase();
  const filteredTracks = computed(() => filteredRemoteTracks());
  // 切走 tab 再回来，停在原处（每台服务器各记各的）。
  const scroll = rememberScroll(`remote:${srv()?.id ?? ""}`);
  const filteredAlbums = (): RemoteCardItem[] => {
    const term = q();
    const list = albumsList();
    const matching = term
      ? list.filter((g) => g.album.toLowerCase().includes(term) || g.artist.toLowerCase().includes(term))
      : list;
    return matching.map((g) => ({
      key: `${g.album}\u0000${g.artist}`,
      name: g.album,
      sub: g.artist || `${g.tracks.length} 首`,
      track: g.tracks[0],
      count: g.tracks.length,
    }));
  };
  const filteredArtists = (): RemoteCardItem[] => {
    const term = q();
    const list = artistsList();
    const matching = term ? list.filter((g) => g.artist.toLowerCase().includes(term)) : list;
    return matching.map((g) => ({
      key: g.artist,
      name: g.artist,
      sub: `${g.tracks.length} 首`,
      track: g.tracks[0],
      count: g.tracks.length,
    }));
  };

  return (
    <AppPage title={srv()?.label ?? "远程服务器"} testID="page-remote" scroll={scroll} gear>
      <Scroll class="flex-1" contentClass={pageEnd()} contentOffset={scroll.contentOffset} onScroll={scroll.onScroll} ref={scroll.ref}>
        <If
          when={srv()}
          else={
            <EmptyState
              title="没有连接的服务器"
              detail={
                layout() === "mobile"
                  ? "在设置中连接一个服务器；凭据只留在本机。"
                  : "在侧栏或设置中连接一个服务器；凭据只留在本机。"
              }
            >
              <ActionButton label="连接服务器…" onPress={() => setServerDialogOpen(true)} />
            </EmptyState>
          }
        >
          {/* 1) Source Management Header */}
          <View class="px-6 pt-2 pb-4 gap-2" role="header">
            <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">
              {srv()!.kind.toUpperCase()} · {srv()!.baseUrl} · {engine.remoteStatus().configured ? "已连接" : "未连接"}
            </Text>
            <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
              {srv()!.label}
            </Text>
            <Text class="text-xs text-muted-foreground">
              {allTracksList().length} 首曲目 · {albumsList().length} 张专辑 · {artistsList().length} 位歌手
              {remoteLoadingState() === "loading" ? " · 读取中…" : ""}
            </Text>
            <View class="flex-row flex-wrap items-center gap-2 pt-1">
              <ActionButton
                label={shuffleMode() ? "随机播放" : "顺序播放"}
                icon={shuffleMode() ? "shuffle" : "play"}
                selected={shuffleMode()}
                onPress={() => playAllRemote(allTracksList())}
              />
              <ActionButton label="同步更新" variant="outline" onPress={() => void reconnectServer(srv()!.id)} />
              <ActionButton label="测试连接" variant="outline" onPress={() => engine.probeRemote()} />
              <ActionButton
                label="移除来源"
                variant="outline"
                onPress={() => {
                  void removeServerById(srv()!.id);
                  selectSource(ALL_ID);
                  switchTo(HOME);
                }}
              />
            </View>
          </View>

          {/* 2) In-Source Search Input + Sub-Tabs */}
          <View class="px-6 pb-4 gap-3">
            <View class="flex-row items-center gap-3">
              <Input
                value={remoteQuery()}
                onChange={({ value }) => setRemoteQuery(value)}
                placeholder="搜索此来源内的曲目、专辑或歌手"
                accessibilityLabel="搜索远程服务器"
                returnKeyType="search"
                autoCapitalize="none"
                autoCorrect={false}
                class="flex-1 text-sm text-foreground"
                style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
              />
              <If when={remoteLoadingState() === "loading"}>
                <Spinner label="读取中" />
              </If>
            </View>

            <View class="flex-row items-center gap-1.5 border-b border-border/60 pb-2">
              <Button
                accessibilityLabel={`曲目 ${filteredTracks().length}`}
                selected={activeTab() === "tracks"}
                onPress={() => activeTab.set("tracks")}
                class={`rounded-full px-3 py-1 text-xs font-medium transition duration-150 ease-out ${
                  activeTab() === "tracks" ? "bg-secondary text-foreground font-bold" : "text-muted-foreground hover:bg-row-hover"
                } ${focusRing}`}
              >
                曲目 ({filteredTracks().length})
              </Button>
              <Button
                accessibilityLabel={`专辑 ${filteredAlbums().length}`}
                selected={activeTab() === "albums"}
                onPress={() => activeTab.set("albums")}
                class={`rounded-full px-3 py-1 text-xs font-medium transition duration-150 ease-out ${
                  activeTab() === "albums" ? "bg-secondary text-foreground font-bold" : "text-muted-foreground hover:bg-row-hover"
                } ${focusRing}`}
              >
                专辑 ({filteredAlbums().length})
              </Button>
              <Button
                accessibilityLabel={`歌手 ${filteredArtists().length}`}
                selected={activeTab() === "artists"}
                onPress={() => activeTab.set("artists")}
                class={`rounded-full px-3 py-1 text-xs font-medium transition duration-150 ease-out ${
                  activeTab() === "artists" ? "bg-secondary text-foreground font-bold" : "text-muted-foreground hover:bg-row-hover"
                } ${focusRing}`}
              >
                歌手 ({filteredArtists().length})
              </Button>
              <Button
                accessibilityLabel={`歌单 ${playlists().length}`}
                selected={activeTab() === "playlists"}
                onPress={() => activeTab.set("playlists")}
                class={`rounded-full px-3 py-1 text-xs font-medium transition duration-150 ease-out ${
                  activeTab() === "playlists" ? "bg-secondary text-foreground font-bold" : "text-muted-foreground hover:bg-row-hover"
                } ${focusRing}`}
              >
                歌单 ({playlists().length})
              </Button>
            </View>
          </View>

          {/* Sub-tab views */}
          <If when={activeTab() === "tracks"}>
            <If
              when={remoteLoadingState() === "error" && filteredTracks().length === 0}
              else={<RemoteList rows={filteredTracks()} />}
            >
              <ErrorState
                message="远程曲目读取失败"
                detail={
                  remoteError()
                    ? `服务器错误: ${remoteError()}`
                    : "请检查网络连通性或服务器凭据是否有效。"
                }
                onRetry={retryRemote}
              >
                <View class="flex-row items-center gap-2 pt-2">
                  <ActionButton label="重新连接" onPress={retryRemote} />
                  <ActionButton
                    label="检查配置"
                    variant="outline"
                    onPress={() => setServerDialogOpen(true)}
                  />
                </View>
              </ErrorState>
            </If>
          </If>
          <If when={activeTab() === "albums"}>
            <View class="px-6">
              <RemoteCardsRow variant="album" items={filteredAlbums()} grid />
            </View>
          </If>
          <If when={activeTab() === "artists"}>
            <View class="px-6">
              <RemoteCardsRow variant="artist" items={filteredArtists()} grid />
            </View>
          </If>
          <If when={activeTab() === "playlists"}>
            <PlaylistsStrip />
          </If>
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

function totalDuration(rows: RemoteTrack[]): string {
  const ms = rows.reduce((acc, t) => acc + (t.durationMs || 0), 0);
  const totalSec = Math.floor(ms / 1000);
  const min = Math.floor(totalSec / 60);
  const sec = totalSec % 60;
  if (min >= 60) {
    const hr = Math.floor(min / 60);
    const remainMin = min % 60;
    return `${hr} 小时 ${remainMin} 分钟`;
  }
  return `${min} 分钟 ${sec} 秒`;
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
  const page = () => engine.remoteItems();
  const rows = (): RemoteTrack[] => {
    const p = page();
    return p && p.id === props.id ? p.tracks : [];
  };
  const first = () => rows()[0];
  // 打开过的远程播放列表，回来时停在原处。
  const scroll = rememberScroll(`remotePlaylist:${props.id}`);

  return (
    <AppPage title={props.name || "播放列表"} testID={`page-remote-playlist-${props.id}`} scroll={scroll}>
      <Scroll class="flex-1" contentClass={pageEnd()} contentOffset={scroll.contentOffset} onScroll={scroll.onScroll} ref={scroll.ref}>
        <View class="px-6 pt-4 pb-4 flex-row items-center gap-5" role="header">
          <If when={first()}>
            <RemoteCover id={first()!.id} coverArt={first()!.coverArt} size={112} class="shadow-md" />
          </If>
          <View class="flex-1 min-w-0 gap-1.5">
            <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">播放列表</Text>
            <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
              {props.name || "—"}
            </Text>
            <If when={rows().length > 0}>
              <Text class="text-xs text-muted-foreground">{rows().length} 首 · {totalDuration(rows())}</Text>
              <View class="pt-2 flex-row items-center gap-3">
                <PlayButton playing={false} size={40} onPress={() => queueRemoteFrom(rows(), 0)} />
              </View>
            </If>
          </View>
        </View>
        <If when={rows().length > 0} else={<RowsSkeleton rows={6} />}>
          <View class="px-3">
            <TrackHeader showAlbum={wideTracks()} />
            <Each of={rows()} key={(t) => t.id}>
              {(track, i) => (
                <RemoteTrackRow
                  track={track()}
                  number={i()}
                  showAlbum={wideTracks()}
                  onPlay={() => queueRemoteFrom(rows(), i())}
                />
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
  const rows = (): RemoteTrack[] => {
    const r = remoteTracks();
    const source = r.length > 0 ? r : cachedRemoteTracks(props.serverId);
    const exact = source.filter(
      (track) => (track.album || "") === props.album && (track.artist || "") === props.artist,
    );
    return exact.length > 0 ? exact : source.filter((track) => (track.album || "") === props.album);
  };
  const first = () => rows()[0];
  const scroll = rememberScroll(`remoteAlbum:${props.serverId}\u0000${props.album}\u0000${props.artist}`);
  return (
    <AppPage title={props.album} testID="page-remote-album" scroll={scroll}>
      <Scroll class="flex-1" contentClass={pageEnd()} contentOffset={scroll.contentOffset} onScroll={scroll.onScroll} ref={scroll.ref}>
        <View class="px-6 pt-4 pb-4 flex-row items-center gap-5" role="header">
          <If when={first()}>
            <RemoteCover id={first()!.id} coverArt={first()!.coverArt} size={112} class="shadow-md" />
          </If>
          <View class="flex-1 min-w-0 gap-1.5">
            <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">远程专辑</Text>
            <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
              {props.album || "—"}
            </Text>
            <Text class="text-sm text-muted-foreground">
              {props.artist || first()?.artist || "未知歌手"} · {rows().length} 首 · {totalDuration(rows())}
            </Text>
            <If when={rows().length > 0}>
              <View class="pt-2 flex-row items-center gap-3">
                <PlayButton playing={false} size={40} onPress={() => queueRemoteFrom(rows(), 0)} />
              </View>
            </If>
          </View>
        </View>
        <If
          when={rows().length > 0}
          else={
            <EmptyState title="专辑没有曲目" detail="服务器列表刷新后再试；用首页的来源 Tab 回到这张专辑。" />
          }
        >
          <View class="px-3">
            <TrackHeader showAlbum={wideTracks()} />
            <Each of={rows()} key={(t) => t.id}>
              {(track, i) => (
                <RemoteTrackRow
                  track={track()}
                  number={i()}
                  showAlbum={wideTracks()}
                  onPlay={() => queueRemoteFrom(rows(), i())}
                />
              )}
            </Each>
          </View>
        </If>
      </Scroll>
    </AppPage>
  );
}

/** 远程歌手页：歌手名 + 其曲目行。 */
export function RemoteArtistPage(props: { serverId: string; artist: string }) {
  const rows = (): RemoteTrack[] => {
    const r = remoteTracks();
    const source = r.length > 0 ? r : cachedRemoteTracks(props.serverId);
    return source.filter((track) => (track.artist || "") === props.artist);
  };
  const first = () => rows()[0];
  const scroll = rememberScroll(`remoteArtist:${props.serverId}\u0000${props.artist}`);
  return (
    <AppPage title={props.artist} testID="page-remote-artist" scroll={scroll}>
      <Scroll class="flex-1" contentClass={pageEnd()} contentOffset={scroll.contentOffset} onScroll={scroll.onScroll} ref={scroll.ref}>
        <View class="px-6 pt-4 pb-4 flex-row items-center gap-5" role="header">
          <If when={first()}>
            <RemoteCover id={first()!.id} coverArt={first()!.coverArt} size={112} round class="shadow-md" />
          </If>
          <View class="flex-1 min-w-0 gap-1.5">
            <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">远程歌手</Text>
            <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
              {props.artist || "—"}
            </Text>
            <Text class="text-sm text-muted-foreground">
              {rows().length} 首 · {totalDuration(rows())}
            </Text>
            <If when={rows().length > 0}>
              <View class="pt-2 flex-row items-center gap-3">
                <PlayButton playing={false} size={40} onPress={() => queueRemoteFrom(rows(), 0)} />
              </View>
            </If>
          </View>
        </View>
        <If
          when={rows().length > 0}
          else={
            <EmptyState title="歌手没有曲目" detail="服务器列表刷新后再试；用首页的来源 Tab 回到这位歌手。" />
          }
        >
          <View class="px-3">
            <TrackHeader showAlbum={wideTracks()} />
            <Each of={rows()} key={(t) => t.id}>
              {(track, i) => (
                <RemoteTrackRow
                  track={track()}
                  number={i()}
                  showAlbum={wideTracks()}
                  onPlay={() => queueRemoteFrom(rows(), i())}
                />
              )}
            </Each>
          </View>
        </If>
      </Scroll>
    </AppPage>
  );
}
