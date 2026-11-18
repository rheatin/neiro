// 专辑: the library's albums as a filling grid. Data loads when the page
// shows (also again when a scan changed the index), covers arrive with
// the index.
// 包含搜索过滤输入框、排序选择器（默认/专辑名/艺人/年份/曲目数）、升降序与网格/列表双视图切换。

import { effect, List, Scroll, Text, untracked, View } from "nsh";
import { Button, Input } from "nsh/base";
import {
  addRoot,
  albumItemOf,
  albumsState,
  dataEpoch,
  ensureAlbums,
  ensureTracks,
  retryAlbums,
  visibleAlbums,
  type AlbumItem,
} from "../state/catalog";

import { activeSource } from "../state/sources";
import { queueRemoteFrom, remoteAlbumGroups, remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { playQueue } from "../state/player";
import { RemoteAlbumRow, RemoteCardsRow } from "../components/library/remote-cards";
import { AppPage } from "../components/page";
import { CardGrid } from "../components/library/cards";
import { albumCoverTrack, itemTracks, openItem } from "../state/catalog";
import { CardsSkeleton, EmptyState, ErrorState } from "../components/ui/feedback";
import { ActionButton, focusRing } from "../components/ui/controls";
import { enter } from "../components/ui/motion";
import { Icon } from "../components/ui/icons";
import { Cover } from "../components/ui/cover";
import {
  albumViewMode,
  albumFilterQuery,
  albumSortField,
  albumSortOrder,
  sortAndFilterAlbums,
  type AlbumSortField,
} from "../state/library-prefs";

const SORT_OPTIONS: { key: AlbumSortField; label: string }[] = [
  { key: "default", label: "默认" },
  { key: "album", label: "专辑名称" },
  { key: "artist", label: "艺人姓名" },
  { key: "year", label: "发行年份" },
  { key: "trackCount", label: "曲目数量" },
];

export function AlbumsPage() {
  effect(() => {
    dataEpoch();
    untracked(() => {
      ensureAlbums();
      ensureTracks();
    });
  });

  const rawAlbums = () => visibleAlbums();
  const sortedAlbums = () => sortAndFilterAlbums(rawAlbums());
  const items = () => sortedAlbums().map(albumItemOf);

  const remote = () => activeSource().kind === "remote";
  const remoteGroups = () => {
    const q = albumFilterQuery().trim().toLowerCase();
    let groups = remoteAlbumGroups();
    if (q) {
      groups = groups.filter(
        (g) => g.album.toLowerCase().includes(q) || g.artist.toLowerCase().includes(q)
      );
    }
    const field = albumSortField();
    const order = albumSortOrder();
    if (field !== "default") {
      groups = [...groups].sort((a, b) => {
        let cmp = 0;
        if (field === "album") cmp = a.album.localeCompare(b.album);
        else if (field === "artist") cmp = a.artist.localeCompare(b.artist);
        else if (field === "trackCount") cmp = a.tracks.length - b.tracks.length;
        return order === "desc" ? -cmp : cmp;
      });
    }
    return groups.map((group) => ({
      key: `${group.album}\u0000${group.artist}`,
      name: group.album,
      sub: group.artist || `${group.tracks.length} 首`,
      count: group.tracks.length,
      track: group.tracks[0],
      tracks: group.tracks,
    }));
  };

  const isList = () => albumViewMode() === "list";

  const renderToolbar = () => (
    <View class="gap-2.5 pb-4">
      <View class="flex-row flex-wrap items-center justify-between gap-2">
        <View class="flex-row flex-wrap items-center gap-1.5">
          <Text class="text-xs font-bold text-muted-foreground mr-1">排序:</Text>
          <Each of={SORT_OPTIONS}>
            {(opt) => {
              const selected = () => albumSortField() === opt().key;
              return (
                <Button
                  selected={selected()}
                  onPress={() => {
                    if (selected()) {
                      albumSortOrder.update((o) => (o === "asc" ? "desc" : "asc"));
                    } else {
                      albumSortField.set(opt().key);
                      albumSortOrder.set("asc");
                    }
                  }}
                  accessibilityLabel={`按 ${opt().label} 排序`}
                  class={`flex-row items-center gap-1 rounded-full px-2.5 py-1 text-xs transition ${
                    selected()
                      ? "bg-foreground text-background font-bold shadow-2xs"
                      : "bg-secondary text-muted-foreground hover:text-foreground"
                  } ${focusRing}`}
                >
                  <Text class={`text-xs ${selected() ? "text-background font-bold" : "text-muted-foreground"}`}>
                    {opt().label}
                  </Text>
                  <If when={selected() && albumSortField() !== "default"}>
                    <Icon
                      name={albumSortOrder() === "asc" ? "sortAsc" : "sortDesc"}
                      size={11}
                      color={selected() ? "background" : "foreground"}
                    />
                  </If>
                </Button>
              );
            }}
          </Each>
        </View>

        {/* View Mode Toggle: Grid vs List */}
        <View class="flex-row items-center gap-1 rounded-full border border-border/60 bg-secondary/60 p-0.5">
          <Button
            selected={!isList()}
            onPress={() => albumViewMode.set("grid")}
            accessibilityLabel="网格卡片视图"
            class={`rounded-full px-2.5 py-1 text-xs transition ${
              !isList()
                ? "bg-background text-foreground font-bold shadow-2xs"
                : "text-muted-foreground hover:text-foreground"
            } ${focusRing}`}
          >
            <Text class={`text-xs ${!isList() ? "font-bold text-foreground" : "text-muted-foreground"}`}>
              网格
            </Text>
          </Button>
          <Button
            selected={isList()}
            onPress={() => albumViewMode.set("list")}
            accessibilityLabel="列表行视图"
            class={`rounded-full px-2.5 py-1 text-xs transition ${
              isList()
                ? "bg-background text-foreground font-bold shadow-2xs"
                : "text-muted-foreground hover:text-foreground"
            } ${focusRing}`}
          >
            <Text class={`text-xs ${isList() ? "font-bold text-foreground" : "text-muted-foreground"}`}>
              列表
            </Text>
          </Button>
        </View>
      </View>

      {/* Filter Input */}
      <View class="flex-row items-center rounded-(--radius) border border-border/80 bg-secondary/40 px-2.5 py-1 focus-within:border-foreground/40">
        <Icon name="search" size={14} color="muted-foreground" />
        <Input
          value={albumFilterQuery()}
          onChange={({ value }) => albumFilterQuery.set(value)}
          placeholder="搜索专辑名称或艺人…"
          accessibilityLabel="过滤专辑列表"
          class="ml-2 flex-1 text-xs text-foreground"
          autoCapitalize="none"
          autoCorrect={false}
        />
        <If when={albumFilterQuery().length > 0}>
          <Button
            onPress={() => albumFilterQuery.set("")}
            accessibilityLabel="清空过滤"
            class="p-0.5 text-muted-foreground hover:text-foreground"
          >
            <Icon name="close" size={12} color="muted-foreground" />
          </Button>
        </If>
      </View>
    </View>
  );

  const listHeader = () => (
    <View class="pt-2">
      <Text role="header" class="pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
        专辑
      </Text>
      {renderToolbar()}
    </View>
  );

  return (
    <AppPage title="专辑" testID="page-albums">
      <If
        when={remote()}
        else={
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
                <If
                  when={isList()}
                  else={
                    <Scroll class="flex-1" contentClass="px-6 pb-28">
                      <Text role="header" class="pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
                        专辑
                      </Text>
                      {renderToolbar()}
                      <CardGrid items={items()} />
                    </Scroll>
                  }
                >
                  <List
                    each={items()}
                    keyExtractor={(item) => `${item.kind}:${item.name}\u0000${item.subtitle}`}
                    estimatedItemSize={56}
                    class="flex-1"
                    contentClass="px-6 pb-28 gap-1.5"
                    header={listHeader}
                  >
                    {(item) => <AlbumRow item={item()} />}
                  </List>
                </If>
              </If>
            }
          >
            <ErrorState message="专辑没有读出来。" onRetry={retryAlbums} />
          </If>
        }
      >
        <View class="flex-1">
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
              else={<EmptyState title="没有专辑" detail="服务器上的曲目还没有专辑名可分。" />}
            >
              <If
                when={isList()}
                else={
                  <Scroll class="flex-1" contentClass="px-6 pb-28">
                    <Text role="header" class="pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
                      专辑
                    </Text>
                    {renderToolbar()}
                    <RemoteCardsRow grid variant="album" items={remoteGroups()} />
                  </Scroll>
                }
              >
                <List
                  each={remoteGroups()}
                  keyExtractor={(item) => item.key}
                  estimatedItemSize={56}
                  class="flex-1"
                  contentClass="px-6 pb-28 gap-1.5"
                  header={listHeader}
                >
                  {(item) => <RemoteAlbumRow item={item()} />}
                </List>
              </If>
            </If>
          </If>
        </View>
      </If>
    </AppPage>
  );
}

function AlbumRow(props: { item: AlbumItem }) {
  const item = () => props.item;
  const onPlayAll = () => {
    const tracks = itemTracks(item());
    if (tracks.length > 0) playQueue(tracks, 0);
  };
  return (
    <View class="group flex-row items-center justify-between rounded-xl border border-border/40 bg-secondary/30 p-2.5 transition hover:border-foreground/20 hover:bg-row-hover">
      <Button
        accessibilityLabel={`专辑 ${item().name}，${item().subtitle}`}
        onPress={() => openItem(item())}
        class={`flex-row items-center gap-3 min-w-0 flex-1 text-left ${focusRing}`}
      >
        <Cover path={albumCoverTrack(item().album, item().albumArtist)} size={40} />
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class="truncate text-sm font-bold text-foreground">{item().name}</Text>
          <Text class="truncate text-xs text-muted-foreground">
            {item().subtitle} · {item().count} 首
          </Text>
        </View>
      </Button>
      <Button
        onPress={() => onPlayAll()}
        accessibilityLabel={`播放 ${item().name} 全部曲目`}
        class={`shrink-0 flex-row items-center gap-1 rounded-full bg-foreground/10 px-2.5 py-1.5 opacity-0 transition duration-150 ease-out group-hover:opacity-100 hover:bg-foreground hover:text-background active:scale-95 ${focusRing}`}
      >
        <Icon name="play" size={11} color="foreground" />
        <Text class="text-xs font-bold text-foreground group-hover:text-background">播放</Text>
      </Button>
    </View>
  );
}
