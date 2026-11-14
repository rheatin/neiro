// 搜索: the field (the header's, on the desktop, bound to the same
// `searchQuery`), then the songs, then the albums and artists that match.
// 来源感知：本地来源 = 引擎搜索（防抖、新问句覆盖旧答）+ 本地
// albums/artists 过滤；远程来源 = 对**已加载**的 remoteTracks() 本地子串
// 过滤（大小写不敏感，先 50 条），无网络请求，不需防抖。该服务器还在
// 拉取时提示"只搜索已加载的部分"。

import { Scroll, Text, token, View } from "nsh";
import { Input } from "nsh/base";
import {
  albumItemOf,
  artistItemOf,
  searchAlbums,
  searchArtists,
  searchQuery,
  searchResults,
  searchRunning,
} from "../state/catalog";
import { activeSource } from "../state/sources";
import {
  remoteFetching,
  remoteAlbumGroups,
  remoteArtistGroups,
  remoteLoadingState,
  playRemote,
  remoteTracks,
} from "../state/remote";
import { AppPage } from "../components/page";
import { pageEnd, wideTracks } from "../components/layout";
import { CardRow } from "../components/library/cards";
import { RemoteCardsRow, type RemoteCardItem } from "../components/library/remote-cards";
import { RemoteTrackRow } from "./remote";
import { TrackHeader, TrackRow } from "../components/library/tracks";
import { playFromRows } from "../state/player";
import { EmptyState, Spinner } from "../components/ui/feedback";
import { SectionTitle } from "../components/ui/controls";

const MAX_TRACKS = 50;

export function SearchPage() {
  const term = () => searchQuery().trim();
  const remote = () => activeSource().kind === "remote";

  // Local: the engine's answer only while its query is still the one asked.
  const songs = () => {
    const reply = searchResults();
    return reply && reply.query === term() ? reply.tracks.slice(0, MAX_TRACKS) : [];
  };
  const albumItems = () => searchAlbums().map(albumItemOf);
  const artistItems = () => searchArtists().map(artistItemOf);

  // Remote: substring filter over the loaded list (case-insensitive).
  const lowerTerm = () => term().toLowerCase();
  const remoteSongs = () => {
    const t = lowerTerm();
    if (!t) return [];
    return remoteTracks()
      .filter(
        (row) =>
          row.title.toLowerCase().includes(t) ||
          row.artist.toLowerCase().includes(t) ||
          row.album.toLowerCase().includes(t),
      )
      .slice(0, MAX_TRACKS);
  };
  const remoteAlbumItems = (): RemoteCardItem[] => {
    const t = lowerTerm();
    if (!t) return [];
    return remoteAlbumGroups()
      .filter((g) => g.album.toLowerCase().includes(t) || g.artist.toLowerCase().includes(t))
      .slice(0, 24)
      .map((g) => ({
        key: `${g.album}\u0000${g.artist}`,
        name: g.album,
        sub: g.artist || `${g.tracks.length} 首`,
        track: g.tracks[0],
        count: g.tracks.length,
      }));
  };
  const remoteArtistItems = (): RemoteCardItem[] => {
    const t = lowerTerm();
    if (!t) return [];
    return remoteArtistGroups()
      .filter((g) => g.artist.toLowerCase().includes(t))
      .slice(0, 24)
      .map((g) => ({
        key: g.artist,
        name: g.artist,
        sub: `${g.tracks.length} 首`,
        track: g.tracks[0],
        count: g.tracks.length,
      }));
  };

  const localTotal = () => songs().length + albumItems().length + artistItems().length;
  const remoteTotal = () => remoteSongs().length + remoteAlbumItems().length + remoteArtistItems().length;
  const remoteLoading = () => remoteLoadingState() === "loading" && remoteTracks().length === 0;
  const remotePartial = () => remoteFetching() && remoteTracks().length > 0;

  return (
    <AppPage title="搜索" testID="page-search">
      <Scroll class="flex-1" contentClass={pageEnd()}>
        <If
          when={term()}
          else={
            <EmptyState
              title={remote() ? "搜索远程库" : "搜索曲库"}
              detail={
                remote()
                  ? "找这首歌服务器上曲目的歌名、专辑或歌手的一部分都行。"
                  : "找歌名、专辑或歌手的一部分都行。"
              }
            />
          }
        >
          <If when={remote()}>
            {/* 远程来源：已加载列表的本地过滤。 */}
            <If
              when={remoteTotal() > 0}
              else={
                <If
                  when={remoteLoading()}
                  else={
                    <EmptyState title="没有找到" detail={`服务器上没有匹配「${term()}」的内容。`} />
                  }
                >
                  <View class="items-center gap-3 py-10">
                    <Spinner label="读取远程列表" />
                    <Text class="text-xs text-muted-foreground">正在读取远程列表…</Text>
                  </View>
                </If>
              }
            >
              <If when={remoteSongs().length > 0}>
                <View class="gap-2">
                  <SectionTitle class="px-6">曲目</SectionTitle>
                  <Each of={remoteSongs()}>
                    {(track, i) => <RemoteTrackRow track={track()} number={i()} />}
                  </Each>
                </View>
              </If>
              <If when={remoteAlbumItems().length > 0}>
                <View class="px-3">
                  <SectionTitle class="px-3 pb-2">专辑</SectionTitle>
                  <RemoteCardsRow variant="album" items={remoteAlbumItems()} />
                </View>
              </If>
              <If when={remoteArtistItems().length > 0}>
                <View class="px-3">
                  <SectionTitle class="px-3 pb-2">歌手</SectionTitle>
                  <RemoteCardsRow variant="artist" items={remoteArtistItems()} />
                </View>
              </If>
              <If when={remotePartial()}>
                <Text class="px-6 pb-4 text-xs text-muted-foreground">
                  只搜索已加载的部分（后台仍在拉取）。
                </Text>
              </If>
            </If>
          </If>
          <If when={!remote()}>
            <If
              when={localTotal() > 0}
              else={
                <If
                  when={searchRunning()}
                  else={<EmptyState title="没有找到" detail={`曲库里没有匹配「${term()}」的内容。`} />}
                >
                  <View class="items-center gap-3 py-10">
                    <Spinner label="搜索中" />
                    <Text class="text-xs text-muted-foreground">正在搜索…</Text>
                  </View>
                </If>
              }
            >
              <If when={songs().length > 0}>
                <View class="gap-2">
                  <SectionTitle class="px-6">曲目</SectionTitle>
                  <TrackHeader showAlbum={wideTracks()} />
                  <Each of={songs()}>
                    {(track, i) => (
                        <TrackRow
                          track={track()}
                          number={i()}
                          showAlbum={wideTracks()}
                          onPlay={() => playFromRows(songs(), i())}
                        />
                      )}
                  </Each>
                </View>
              </If>
              <If when={albumItems().length > 0}>
                <View class="px-3">
                  <CardRow title="专辑" items={albumItems()} />
                </View>
              </If>
              <If when={artistItems().length > 0}>
                <View class="px-3">
                  <CardRow title="歌手" items={artistItems()} />
                </View>
              </If>
            </If>
          </If>
        </If>
      </Scroll>
    </AppPage>
  );
}
