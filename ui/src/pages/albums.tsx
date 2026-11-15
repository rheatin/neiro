// 专辑: the library's albums as a filling grid. Data loads when the page
// shows (also again when a scan changed the index), covers arrive with
// the index.
// 
// 空曲库: one empty state with the 添加文件夹 action.

import { effect, Scroll, Text, untracked, View } from "nsh";
import {
  addRoot,
  albumItemOf,
  albumsState,
  dataEpoch,
  ensureAlbums,
  ensureTracks,
  retryAlbums,
  visibleAlbums,
} from "../state/catalog";

import { activeSource } from "../state/sources";
import { remoteAlbumGroups, remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { RemoteCardsRow } from "../components/library/remote-cards";
import { AppPage } from "../components/page";
import { CardGrid } from "../components/library/cards";
import { CardsSkeleton, EmptyState, ErrorState } from "../components/ui/feedback";
import { ActionButton } from "../components/ui/controls";
import { enter } from "../components/ui/motion";

export function AlbumsPage() {
  effect(() => {
    dataEpoch();
    untracked(() => {
      ensureAlbums();
      ensureTracks();
    });
  });
  const items = () => visibleAlbums().map(albumItemOf);
  // 来源是远程时，这一页展示该服务器的专辑（由曲目按 album+artist 分组），
  // 与首页的远程段同一数据、同一详情路由。
  const remote = () => activeSource().kind === "remote";
  const remoteGroups = () =>
    remoteAlbumGroups().map((group) => ({
      key: `${group.album}\u0000${group.artist}`,
      name: group.album,
      sub: group.artist || `${group.tracks.length} 首`,
      count: group.tracks.length,
      track: group.tracks[0],
    }));

  return (
    <AppPage title="专辑" testID="page-albums">
      <If
        when={remote()}
        else={
      <Scroll class="flex-1" contentClass="px-6 pb-28">
        <Text role="header" class="pb-6 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
          专辑
        </Text>
        <If
          when={albumsState() === "error"}
          else={
            <If
              when={items().length > 0}
              else={
                <If
                  when={albumsState() === "loading"}
                  else={
                    <EmptyState title="专辑是空的" detail="添加一个文件夹，扫描把里面的专辑收进来。">
                      <ActionButton label="添加文件夹" onPress={() => void addRoot()} />
                    </EmptyState>
                  }
                >
                  <CardsSkeleton rows={2} />
                </If>
              }
            >
              <CardGrid items={items()} />
            </If>
          }
        >
          <ErrorState message="专辑没有读出来。" onRetry={retryAlbums} />
        </If>
      </Scroll>
        }
      >
      <Scroll class="flex-1" contentClass="px-6 pb-28">
        <Text role="header" class="pb-6 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
          专辑
        </Text>
        <If when={remoteLoadingState() === "error" && remoteTracks().length === 0}>
          <ErrorState message="远程库没有读出来。" onRetry={retryRemote} />
        </If>
        <If when={remoteLoadingState() === "loading" && remoteTracks().length === 0}>
          <CardsSkeleton rows={2} />
        </If>
        <If when={remoteTracks().length > 0}>
          <If
            when={remoteGroups().length > 0}
            else={<EmptyState title="没有专辑" detail="服务器上的曲目还没有专辑名可分。" />}
          >
            <RemoteCardsRow grid variant="album" items={remoteGroups()} />
          </If>
        </If>
      </Scroll>
      </If>
    </AppPage>
  );
}
