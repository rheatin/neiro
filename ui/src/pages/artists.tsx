// 歌手: the library's artists as a grid of round cards. Same shape as
// 专辑's page, over the artist list.

import { effect, Scroll, Text, untracked, View } from "nsh";
import {
  addRoot,
  artistItemOf,
  artists,
  artistsState,
  dataEpoch,
  ensureArtists,
  retryArtists,
  visibleArtists,
} from "../state/catalog";
import { activeSource } from "../state/sources";
import { remoteArtistGroups, remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { RemoteCardsRow } from "../components/library/remote-cards";
import { AppPage } from "../components/page";
import { CardGrid } from "../components/library/cards";
import { CardsSkeleton, EmptyState, ErrorState } from "../components/ui/feedback";
import { ActionButton } from "../components/ui/controls";
import { enter } from "../components/ui/motion";

export function ArtistsPage() {
  effect(() => {
    dataEpoch();
    untracked(() => ensureArtists());
  });
  const items = visibleArtists().map(artistItemOf);
  const remote = () => activeSource().kind === "remote";
  const remoteGroups = () =>
    remoteArtistGroups().map((group) => ({
      key: group.artist,
      name: group.artist,
      sub: `${group.tracks.length} 首`,
      count: group.tracks.length,
      track: group.tracks[0],
    }));

  return (
    <AppPage title="歌手" testID="page-artists">
      <If
        when={remote()}
        else={
      <Scroll class="flex-1" contentClass="px-6 pb-28">
        <Text role="header" class="pb-6 text-3xl font-extrabold text-foreground" style={enter("fadeInUp")}>
          歌手
        </Text>
        <If
          when={artistsState() === "error"}
          else={
            <If
              when={items.length > 0}
              else={
                <If
                  when={artistsState() === "loading"}
                  else={
                    <EmptyState title="歌手是空的" detail="添加一个文件夹，扫描把里面的歌手收进来。">
                      <ActionButton label="添加文件夹" onPress={() => void addRoot()} />
                    </EmptyState>
                  }
                >
                  <CardsSkeleton rows={2} />
                </If>
              }
            >
              <CardGrid items={items} />
            </If>
          }
        >
          <ErrorState message="歌手没有读出来。" onRetry={retryArtists} />
        </If>
      </Scroll>
        }
      >
      <Scroll class="flex-1" contentClass="px-6 pb-28">
        <Text role="header" class="pb-6 text-3xl font-extrabold text-foreground" style={enter("fadeInUp")}>
          歌手
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
            else={<EmptyState title="没有歌手" detail="服务器上的曲目还没有歌手名可分。" />}
          >
            <RemoteCardsRow variant="artist" items={remoteGroups()} />
          </If>
        </If>
      </Scroll>
      </If>
    </AppPage>
  );
}
