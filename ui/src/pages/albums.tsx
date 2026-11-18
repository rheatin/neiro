// 专辑: the library's albums as a filling grid. Data loads when the page
// shows (also again when a scan changed the index), covers arrive with
// the index.
// 包含搜索过滤输入框、排序选择器（默认/专辑名/艺人/年份/曲目数）、升降序与标准/紧凑双密度切换。

import { effect, Scroll, Text, untracked, View } from "nsh";
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
} from "../state/catalog";

import { activeSource } from "../state/sources";
import { remoteAlbumGroups, remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { RemoteCardsRow } from "../components/library/remote-cards";
import { AppPage } from "../components/page";
import { CardGrid } from "../components/library/cards";
import { CardsSkeleton, EmptyState, ErrorState } from "../components/ui/feedback";
import { ActionButton, focusRing } from "../components/ui/controls";
import { enter } from "../components/ui/motion";
import { Icon } from "../components/ui/icons";
import {
  albumDensity,
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
    }));
  };

  const isCompact = () => albumDensity() === "compact";

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

        {/* Density Toggle */}
        <View class="flex-row items-center gap-1 rounded-full border border-border/60 bg-secondary/60 p-0.5">
          <Button
            selected={!isCompact()}
            onPress={() => albumDensity.set("standard")}
            accessibilityLabel="标准大图视图"
            class={`rounded-full px-2.5 py-1 text-xs transition ${
              !isCompact()
                ? "bg-background text-foreground font-bold shadow-2xs"
                : "text-muted-foreground hover:text-foreground"
            } ${focusRing}`}
          >
            <Text class={`text-xs ${!isCompact() ? "font-bold text-foreground" : "text-muted-foreground"}`}>
              标准大图
            </Text>
          </Button>
          <Button
            selected={isCompact()}
            onPress={() => albumDensity.set("compact")}
            accessibilityLabel="紧凑网格视图"
            class={`rounded-full px-2.5 py-1 text-xs transition ${
              isCompact()
                ? "bg-background text-foreground font-bold shadow-2xs"
                : "text-muted-foreground hover:text-foreground"
            } ${focusRing}`}
          >
            <Text class={`text-xs ${isCompact() ? "font-bold text-foreground" : "text-muted-foreground"}`}>
              紧凑网格
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

  return (
    <AppPage title="专辑" testID="page-albums">
      <If
        when={remote()}
        else={
          <Scroll class="flex-1" contentClass="px-6 pb-28">
            <Text role="header" class="pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
              专辑
            </Text>
            {renderToolbar()}
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
                  <CardGrid items={items()} compact={isCompact()} />
                </If>
              }
            >
              <ErrorState message="专辑没有读出来。" onRetry={retryAlbums} />
            </If>
          </Scroll>
        }
      >
        <Scroll class="flex-1" contentClass="px-6 pb-28">
          <Text role="header" class="pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
            专辑
          </Text>
          {renderToolbar()}
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
              <RemoteCardsRow grid variant="album" items={remoteGroups()} compact={isCompact()} />
            </If>
          </If>
        </Scroll>
      </If>
    </AppPage>
  );
}
