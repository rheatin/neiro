// 歌手: the library's artists as a grid of round cards or virtualized list rows.
// 包含搜索过滤输入框、排序切换（姓名/曲目数/专辑数）、升降序与网格/列表双视图切换。

import { effect, List, Scroll, Text, untracked, View } from "nsh";
import { Button, Input } from "nsh/base";
import {
  addRoot,
  artistCoverTrack,
  artistItemOf,
  artistsState,
  dataEpoch,
  ensureArtists,
  ensureTracks,
  itemTracks,
  openItem,
  retryArtists,
  visibleArtists,
} from "../state/catalog";
import { activeSource } from "../state/sources";
import { remoteArtistGroups, remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { RemoteCardsRow } from "../components/library/remote-cards";
import { AppPage } from "../components/page";
import { CardGrid } from "../components/library/cards";
import { CardsSkeleton, EmptyState, ErrorState } from "../components/ui/feedback";
import { ActionButton, focusRing } from "../components/ui/controls";
import { enter } from "../components/ui/motion";
import { Cover } from "../components/ui/cover";
import { Icon } from "../components/ui/icons";
import { playQueue } from "../state/player";
import type { Artist } from "../backend/types";
import {
  artistFilterQuery,
  artistSortField,
  artistSortOrder,
  artistViewMode,
  sortAndFilterArtists,
  type ArtistSortField,
} from "../state/library-prefs";

const SORT_OPTIONS: { key: ArtistSortField; label: string }[] = [
  { key: "artist", label: "姓名 (A-Z)" },
  { key: "trackCount", label: "曲目数" },
  { key: "albumCount", label: "专辑数" },
];

export function ArtistsPage() {
  effect(() => {
    dataEpoch();
    untracked(() => {
      ensureArtists();
      ensureTracks();
    });
  });

  const rawArtists = () => visibleArtists();
  const sortedArtists = () => sortAndFilterArtists(rawArtists());
  const items = () => sortedArtists().map(artistItemOf);

  const remote = () => activeSource().kind === "remote";
  const remoteGroups = () => {
    const q = artistFilterQuery().trim().toLowerCase();
    let groups = remoteArtistGroups();
    if (q) {
      groups = groups.filter((g) => g.artist.toLowerCase().includes(q));
    }
    const field = artistSortField();
    const order = artistSortOrder();
    groups = [...groups].sort((a, b) => {
      let cmp = 0;
      if (field === "artist") cmp = a.artist.localeCompare(b.artist);
      else if (field === "trackCount") cmp = a.tracks.length - b.tracks.length;
      return order === "desc" ? -cmp : cmp;
    });
    return groups.map((group) => ({
      key: group.artist,
      name: group.artist,
      sub: `${group.tracks.length} 首`,
      count: group.tracks.length,
      track: group.tracks[0],
    }));
  };

  const isListView = () => artistViewMode() === "list";

  const renderToolbar = () => (
    <View class="gap-2.5 pb-4 px-6">
      <View class="flex-row flex-wrap items-center justify-between gap-2">
        <View class="flex-row flex-wrap items-center gap-1.5">
          <Text class="text-xs font-bold text-muted-foreground mr-1">排序:</Text>
          <Each of={SORT_OPTIONS}>
            {(opt) => {
              const selected = () => artistSortField() === opt().key;
              return (
                <Button
                  selected={selected()}
                  onPress={() => {
                    if (selected()) {
                      artistSortOrder.update((o) => (o === "asc" ? "desc" : "asc"));
                    } else {
                      artistSortField.set(opt().key);
                      artistSortOrder.set("asc");
                    }
                  }}
                  accessibilityLabel={`按 ${opt().label} 排序`}
                  class={`rounded-full px-2.5 py-1 text-xs transition ${
                    selected()
                      ? "bg-foreground text-background font-bold shadow-2xs"
                      : "bg-secondary text-muted-foreground hover:text-foreground"
                  } ${focusRing}`}
                >
                  <Text class={`text-xs ${selected() ? "text-background font-bold" : "text-muted-foreground"}`}>
                    {opt().label}
                    {selected() ? (artistSortOrder() === "asc" ? " ↑" : " ↓") : ""}
                  </Text>
                </Button>
              );
            }}
          </Each>
        </View>

        {/* View Mode Toggle: Grid vs List */}
        <View class="flex-row items-center gap-1 rounded-full border border-border/60 bg-secondary/60 p-0.5">
          <Button
            selected={!isListView()}
            onPress={() => artistViewMode.set("grid")}
            accessibilityLabel="网格卡片视图"
            class={`flex-row items-center gap-1 rounded-full px-2.5 py-1 text-xs transition ${
              !isListView()
                ? "bg-background text-foreground font-bold shadow-2xs"
                : "text-muted-foreground hover:text-foreground"
            } ${focusRing}`}
          >
            <Icon name="grid" size={13} color={!isListView() ? "foreground" : "muted-foreground"} />
            <Text class={`text-xs ${!isListView() ? "font-bold text-foreground" : "text-muted-foreground"}`}>
              网格
            </Text>
          </Button>
          <Button
            selected={isListView()}
            onPress={() => artistViewMode.set("list")}
            accessibilityLabel="列表行视图"
            class={`flex-row items-center gap-1 rounded-full px-2.5 py-1 text-xs transition ${
              isListView()
                ? "bg-background text-foreground font-bold shadow-2xs"
                : "text-muted-foreground hover:text-foreground"
            } ${focusRing}`}
          >
            <Icon name="list" size={13} color={isListView() ? "foreground" : "muted-foreground"} />
            <Text class={`text-xs ${isListView() ? "font-bold text-foreground" : "text-muted-foreground"}`}>
              列表
            </Text>
          </Button>
        </View>
      </View>

      {/* Filter Input */}
      <View class="flex-row items-center rounded-(--radius) border border-border/80 bg-secondary/40 px-2.5 py-1 focus-within:border-foreground/40">
        <Icon name="search" size={14} color="muted-foreground" />
        <Input
          value={artistFilterQuery()}
          onChange={({ value }) => artistFilterQuery.set(value)}
          placeholder="搜索歌手姓名…"
          accessibilityLabel="过滤歌手列表"
          class="ml-2 flex-1 text-xs text-foreground"
          autoCapitalize="none"
          autoCorrect={false}
        />
        <If when={artistFilterQuery().length > 0}>
          <Button
            onPress={() => artistFilterQuery.set("")}
            accessibilityLabel="清空过滤"
            class="p-0.5 text-muted-foreground hover:text-foreground"
          >
            <Icon name="close" size={12} color="muted-foreground" />
          </Button>
        </If>
      </View>
    </View>
  );

  return (
    <AppPage title="歌手" testID="page-artists">
      <If
        when={remote()}
        else={
          <View class="flex-1">
            <Text role="header" class="px-6 pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
              歌手
            </Text>
            {renderToolbar()}
            <If
              when={artistsState() === "error"}
              else={
                <If
                  when={sortedArtists().length > 0}
                  else={
                    <If
                      when={artistsState() === "loading"}
                      else={
                        <EmptyState title="歌手是空的" detail="添加一个文件夹，扫描把里面的歌手收进来。">
                          <ActionButton label="添加文件夹" onPress={() => void addRoot()} />
                        </EmptyState>
                      }
                    >
                      <View class="px-6">
                        <CardsSkeleton rows={2} />
                      </View>
                    </If>
                  }
                >
                  <If
                    when={isListView()}
                    else={
                      <Scroll class="flex-1" contentClass="px-6 pb-28">
                        <CardGrid items={items()} />
                      </Scroll>
                    }
                  >
                    <List
                      each={sortedArtists()}
                      keyExtractor={(a) => a.artist}
                      estimatedItemSize={56}
                      class="flex-1"
                      contentClass="px-6 pb-28 gap-1.5"
                    >
                      {(artist) => <ArtistRow artist={artist()} />}
                    </List>
                  </If>
                </If>
              }
            >
              <ErrorState message="歌手没有读出来。" onRetry={retryArtists} />
            </If>
          </View>
        }
      >
        <View class="flex-1">
          <Text role="header" class="px-6 pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
            歌手
          </Text>
          {renderToolbar()}
          <If when={remoteLoadingState() === "error" && remoteTracks().length === 0}>
            <ErrorState message="远程库没有读出来。" onRetry={retryRemote} />
          </If>
          <If when={remoteLoadingState() === "loading" && remoteTracks().length === 0}>
            <View class="px-6">
              <CardsSkeleton rows={2} />
            </View>
          </If>
          <If when={remoteTracks().length > 0}>
            <If
              when={remoteGroups().length > 0}
              else={<EmptyState title="没有歌手" detail="服务器上的曲目还没有歌手名可分。" />}
            >
              <If
                when={isListView()}
                else={
                  <Scroll class="flex-1" contentClass="px-6 pb-28">
                    <RemoteCardsRow grid variant="artist" items={remoteGroups()} />
                  </Scroll>
                }
              >
                <List
                  each={remoteGroups()}
                  keyExtractor={(g) => g.key}
                  estimatedItemSize={56}
                  class="flex-1"
                  contentClass="px-6 pb-28 gap-1.5"
                >
                  {(group) => {
                    const g = () => group();
                    return (
                      <View class="flex-row items-center justify-between rounded-xl border border-border/40 bg-secondary/30 p-2.5">
                        <View class="flex-row items-center gap-3 min-w-0 flex-1">
                          <Cover path="" round fallback="user" size={40} />
                          <View class="min-w-0 flex-1 gap-0.5">
                            <Text class="truncate text-sm font-bold text-foreground">{g().name}</Text>
                            <Text class="text-xs text-muted-foreground">{g().sub}</Text>
                          </View>
                        </View>
                      </View>
                    );
                  }}
                </List>
              </If>
            </If>
          </If>
        </View>
      </If>
    </AppPage>
  );
}

function ArtistRow(props: { artist: Artist }) {
  const a = () => props.artist;
  const item = () => artistItemOf(a());
  const coverTrack = () => artistCoverTrack(a().artist);
  const onPlayAll = () => {
    const tracks = itemTracks(item());
    if (tracks.length > 0) playQueue(tracks, 0);
  };

  return (
    <Button
      accessibilityLabel={`歌手 ${a().artist}，${a().albumCount} 张专辑，${a().trackCount} 首曲目`}
      onPress={() => openItem(item())}
      class={`group flex-row items-center justify-between rounded-xl border border-border/40 bg-secondary/30 p-2.5 transition hover:border-foreground/20 hover:bg-row-hover ${focusRing}`}
    >
      <View class="flex-row items-center gap-3 min-w-0 flex-1">
        <Cover path={coverTrack()} round fallback="user" size={40} />
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class="truncate text-sm font-bold text-foreground">
            {a().artist}
          </Text>
          <Text class="text-xs text-muted-foreground">
            {a().albumCount} 张专辑 · {a().trackCount} 首曲目
          </Text>
        </View>
      </View>

      <Button
        onPress={() => onPlayAll()}
        accessibilityLabel={`播放 ${a().artist} 全部曲目`}
        class={`shrink-0 items-center justify-center rounded-full bg-foreground/10 px-3 py-1.5 opacity-0 transition duration-150 ease-out group-hover:opacity-100 hover:bg-foreground hover:text-background active:scale-95 ${focusRing}`}
      >
        <Text class="text-xs font-bold text-foreground group-hover:text-background">
          ▶ 播放
        </Text>
      </Button>
    </Button>
  );
}
