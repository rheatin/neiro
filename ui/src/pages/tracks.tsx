// 全部曲目: the whole index in one virtualized, recycling list. The index
// streams in page by page (500 rows each); rows show as they land, and
// the list scrolls on as far as what arrived. A scan that found or lost
// files restarts the load.
// 包含 Hi-Fi 格式过滤胶囊栏（全部/Hi-Res/DSD/无损/喜欢）、批量管理模式联动、播放全部与随机播放。

import { computed, effect, List, Scroll, signal, Text, token, untracked, View } from "nsh";
import { Button, Input } from "nsh/base";
import {
  addRoot,
  albumItemOf,
  artistItemOf,
  dataEpoch,
  ensureTracks,
  homeAlbums,
  homeArtists,
  homeTracks,
  activeRemoteState,
  missingTracks,
  removeRoot,
  retryTracks,
  scopedRoot,
  tracksState,
  tracksTotal,
} from "../state/catalog";
import { AppPage } from "../components/page";
import { TrackHeader, TrackRow } from "../components/library/tracks";
import { playAll, playFromRows, shuffleMode } from "../state/player";
import { EmptyState, ErrorState, RowsSkeleton } from "../components/ui/feedback";
import { ActionButton, focusRing } from "../components/ui/controls";
import { createListWave, enter, type ListWave } from "../components/ui/motion";
import { currentRoute, rootOpens, switchTo, HOME } from "../nav";
import { activeSource, ALL_ID, selectSource } from "../state/sources";
import { rememberScroll, type PageScroll } from "../state/page-scroll";
import { playAllRemote, queueRemoteFrom, remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { RemoteTrackRow } from "./remote";
import { barSize, pageEnd, wideTracks } from "../components/layout";
import { SelectionBar } from "../components/library/selection-bar";
import { beginSelection, endSelection, selectionActive } from "../state/queue";
import { CardGrid } from "../components/library/cards";
import { engine } from "../backend/engine";
import { Icon } from "../components/ui/icons";
import {
  albumKey,
  trackArtist,
  trackTitle,
  type Album,
  type Artist,
  type RemoteTrack,
  type Track,
} from "../backend/types";
import {
  sortAndFilterRemoteTracks,
  sortAndFilterTracks,
  trackFilterPill,
  trackFilterQuery,
  type TrackFilterPill,
} from "../state/library-prefs";

function TrackToolbar(props: {
  play: () => void;
}) {
  const currentPill = () => trackFilterPill();
  const PILLS: { key: TrackFilterPill; label: string; icon?: "heart" }[] = [
    { key: "all", label: "全部" },
    { key: "hires", label: "Hi-Res" },
    { key: "dsd", label: "DSD" },
    { key: "lossless", label: "无损" },
    { key: "favourite", label: "已喜欢", icon: "heart" },
  ];

  const selecting = () => selectionActive();

  return (
    <View class="gap-2.5 px-6 pb-2 pt-1">
      {/* Action Buttons & Filter Pills Row */}
      <View class="flex-row flex-wrap items-center justify-between gap-2">
        <View class="flex-row items-center gap-1.5">
          <ActionButton
            label={shuffleMode() ? "随机播放" : "顺序播放"}
            icon={shuffleMode() ? "shuffle" : "play"}
            selected={shuffleMode()}
            onPress={props.play}
          />
          <ActionButton
            label={selecting() ? "完成管理" : "批量管理"}
            icon="check"
            variant={selecting() ? "primary" : "outline"}
            onPress={() => {
              if (selecting()) endSelection();
              else beginSelection();
            }}
          />
        </View>

        {/* Filter Pills */}
        <View class="flex-row items-center gap-1 rounded-full border border-border/60 bg-secondary/60 p-0.5">
          <Each of={PILLS}>
            {(pill) => {
              const selected = () => currentPill() === pill().key;
              return (
                <Button
                  selected={selected()}
                  onPress={() => trackFilterPill.set(pill().key)}
                  accessibilityLabel={`过滤 ${pill().label}`}
                  class={`flex-row items-center gap-1 rounded-full px-2.5 py-1 text-xs transition ${
                    selected()
                      ? "bg-background text-foreground font-bold shadow-2xs"
                      : "text-muted-foreground hover:text-foreground"
                  } ${focusRing}`}
                >
                  <If when={pill().icon}>
                    <Icon name={pill().icon!} size={11} color={selected() ? "foreground" : "muted-foreground"} />
                  </If>
                  <Text
                    class={`text-xs ${
                      selected() ? "font-bold text-foreground" : "text-muted-foreground"
                    }`}
                  >
                    {pill().label}
                  </Text>
                </Button>
              );
            }}
          </Each>
        </View>
      </View>

      {/* Filter Input */}
      <View class="flex-row items-center rounded-(--radius) border border-border/80 bg-secondary/40 px-2.5 py-1 focus-within:border-foreground/40">
        <Icon name="search" size={14} color="muted-foreground" />
        <Input
          value={trackFilterQuery()}
          onChange={({ value }) => trackFilterQuery.set(value)}
          placeholder="在曲目中过滤…"
          accessibilityLabel="过滤曲目列表"
          class="ml-2 flex-1 text-xs text-foreground"
          autoCapitalize="none"
          autoCorrect={false}
        />
        <If when={trackFilterQuery().length > 0}>
          <Button
            onPress={() => trackFilterQuery.set("")}
            accessibilityLabel="清空过滤"
            class="p-0.5 text-muted-foreground hover:text-foreground"
          >
            <Icon name="close" size={12} color="muted-foreground" />
          </Button>
        </If>
      </View>
    </View>
  );
}

export function TracksPage() {
  effect(() => {
    dataEpoch();
    untracked(() => ensureTracks());
  });
  // 切走 tab 再回来，停在原处（来源、文件夹各记各的）。
  const scroll = rememberScroll(`tracks:${activeSource().kind}:${scopedRoot() ?? ""}`);
  const state = () => tracksState();
  const rawList = computed(() => homeTracks());
  const list = computed(() => sortAndFilterTracks(rawList(), { query: trackFilterQuery() }));
  const showAlbum = wideTracks();

  const remote = () => activeSource().kind === "remote";
  const rawRemote = computed(() => remoteTracks());
  const filteredRemote = computed(() => sortAndFilterRemoteTracks(rawRemote(), { query: trackFilterQuery() }));

  /** A folder picked in the sidebar is this page's subject: it says which one
   * instead of claiming to be every track. */
  const rootName = () => {
    const root = scopedRoot();
    if (root === null) return "";
    const parts = root.split(/[\\/]/).filter((part) => part !== "");
    return parts.length > 0 ? parts[parts.length - 1] : root;
  };
  const title = () => (rootName() === "" ? "全部曲目" : rootName());
  const paths = () => list().map((t) => t.path);
  const isScoped = () => scopedRoot() !== null;
  // The rows' wave, played as the page is entered (rows scrolled to come
  // in on their own; not when the page is returned to).
  const wave = createListWave({ isTop: () => currentRoute().name === "tracks", reenter: rootOpens, quiet: scroll.restored });

  return (
    <AppPage title={title()} testID="page-tracks" scroll={scroll}>
      <If
        when={isScoped()}
        else={
          <If
            when={remote()}
            else={
              <List
                each={list()}
                keyExtractor={(t) => t.path}
                estimatedItemSize={48}
                empty={() => <EmptyTracks state={state()} />}
                header={() => (
                  <View class="pt-2">
                    <TracksHead
                      count={list().length}
                      total={
                        activeSource().kind === "all"
                          ? rawList().length
                          : scopedRoot() === null
                            ? Math.max(tracksTotal(), rawList().length)
                            : rawList().length
                      }
                      state={
                        scopedRoot() === null
                          ? rawList().length > 0 && state() === "idle"
                            ? "complete"
                            : state()
                          : "complete"
                      }
                    />
                    <If when={activeSource().kind === "all" && activeRemoteState() === "loading"}>
                      <Text class="px-6 pb-2 text-xs text-muted-foreground">远程库读取中…</Text>
                    </If>
                    <If when={activeSource().kind === "all" && activeRemoteState() === "error"}>
                      <Text class="px-6 pb-2 text-xs text-muted-foreground">远程库未连上</Text>
                    </If>
                    <TrackToolbar
                      play={() => playAll(list())}
                    />
                    <TrackHeader showAlbum={showAlbum} />
                  </View>
                )}
                footer={() => (
                  <If when={state() === "loading" && list().length > 0}>
                    <View class="py-6 items-center">
                      <Text class="text-xs text-muted-foreground">正在读曲库…</Text>
                    </View>
                  </If>
                )}
                class="flex-1"
                contentClass={pageEnd()}
                contentOffset={scroll.contentOffset}
                onScroll={scroll.onScroll}
                ref={scroll.ref}
              >
                {(track, index) => (
                  <TrackRow
                    track={track()}
                    number={index()}
                    showAlbum={showAlbum}
                    wave={wave}
                    onPlay={() => playFromRows(list(), index())}
                  />
                )}
              </List>
            }
          >
            <List
              each={filteredRemote()}
              keyExtractor={(t) => t.id}
              estimatedItemSize={48}
              class="flex-1"
              contentClass={pageEnd()}
              contentOffset={scroll.contentOffset}
              onScroll={scroll.onScroll}
              ref={scroll.ref}
              empty={() => (
                <If
                  when={remoteLoadingState() === "error"}
                  else={
                    <If
                      when={remoteLoadingState() === "loading"}
                      else={
                        <EmptyState title="远程库是空的" detail="这台服务器没有曲目，或还没拉取完成。">
                          <ActionButton label="重试" onPress={() => retryRemote()} />
                        </EmptyState>
                      }
                    >
                      <RowsSkeleton rows={8} header />
                    </If>
                  }
                >
                  <ErrorState message="远程曲目没有读出来。" onRetry={retryRemote} />
                </If>
              )}
              header={() => (
                <View class="pt-2">
                  <View class="px-6 pb-2">
                    <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
                      {`${filteredRemote().length} 首远程曲目`}
                    </Text>
                  </View>
                  <TrackToolbar
                    play={() => playAllRemote(filteredRemote())}
                  />
                  <TrackHeader showAlbum={showAlbum} />
                </View>
              )}
              footer={() => (
                <If when={remoteLoadingState() === "loading" && filteredRemote().length > 0}>
                  <View class="py-6 items-center">
                    <Text class="text-xs text-muted-foreground">正在读远程库…</Text>
                  </View>
                </If>
              )}
            >
              {(track, index) => (
                <RemoteTrackRow
                  track={track()}
                  number={index()}
                  onPlay={() => queueRemoteFrom(filteredRemote(), index())}
                />
              )}
            </List>
          </If>
        }
      >
        <ScopedFolderWorkspace root={scopedRoot()!} wave={wave} scroll={scroll} />
      </If>
      <If when={selectionActive()}>
        <SelectionBar allPaths={paths()} bottom={barSize() === "stacked" ? 128 : 96} />
      </If>
    </AppPage>
  );
}

function ScopedFolderWorkspace(props: { root: string; wave: ListWave; scroll: PageScroll }) {
  const root = props.root;
  const leaf = () => root.replace(/\\/g, "/").split("/").pop() || root;
  const query = signal("");
  const activeTab = signal<"tracks" | "albums" | "artists">("tracks");

  const sourceTracks = () => homeTracks();
  const sourceAlbums = () => homeAlbums();
  const sourceArtists = () => homeArtists();

  const q = () => query().trim().toLowerCase();
  const filteredTracks = computed(() => sortAndFilterTracks(sourceTracks(), { query: q() }));
  // 扫描找不到的文件跟着曲目列表一起显示（标成不可用）；它们不在引擎
  // 索引里，所以不能混进 sourceTracks()。
  const missingRows = (): Track[] => sortAndFilterTracks(missingTracks(), { query: q() });
  const trackRows = (): { track: Track; missing: boolean }[] => [
    ...filteredTracks().map((track) => ({ track, missing: false })),
    ...missingRows().map((track) => ({ track, missing: true })),
  ];
  const filteredAlbums = (): Album[] => {
    const term = q();
    if (!term) return sourceAlbums();
    return sourceAlbums().filter(
      (a) => a.album.toLowerCase().includes(term) || a.albumArtist.toLowerCase().includes(term),
    );
  };
  const filteredArtists = (): Artist[] => {
    const term = q();
    if (!term) return sourceArtists();
    return sourceArtists().filter((a) => a.artist.toLowerCase().includes(term));
  };

  const renderHeader = (showTrackHeader = false) => (
    <View>
      {/* 1) Source Management Header */}
      <View class="px-6 pt-2 pb-4 gap-2" role="header">
        <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">
          本地文件夹 · {root}
        </Text>
        <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
          {leaf()}
        </Text>
        <Text class="text-xs text-muted-foreground">
          {sourceTracks().length} 首曲目 · {sourceAlbums().length} 张专辑 · {sourceArtists().length} 位歌手
        </Text>
        <View class="flex-row flex-wrap items-center gap-2 pt-1">
          <ActionButton
            label={shuffleMode() ? "随机播放" : "顺序播放"}
            icon={shuffleMode() ? "shuffle" : "play"}
            selected={shuffleMode()}
            onPress={() => playAll(sourceTracks())}
          />
          <ActionButton label="重新扫描" variant="outline" onPress={() => engine.scanFolder(root)} />
          <ActionButton
            label="移除来源"
            variant="outline"
            onPress={() => {
              removeRoot(root);
              selectSource(ALL_ID);
              switchTo(HOME);
            }}
          />
        </View>
      </View>

      {/* 2) Search input + sub-tabs */}
      <View class="px-6 pb-4 gap-3">
        <Input
          value={query()}
          onChange={({ value }) => query.set(value)}
          placeholder="搜索此来源内的曲目、专辑或歌手"
          accessibilityLabel="搜索本地文件夹"
          returnKeyType="search"
          autoCapitalize="none"
          autoCorrect={false}
          class="flex-1 text-sm text-foreground"
          style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
        />

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
        </View>
      </View>
      <If when={showTrackHeader}>
        <View class="px-3">
          <TrackHeader showAlbum={wideTracks()} />
        </View>
      </If>
    </View>
  );

  return (
    <If
      when={activeTab() === "tracks"}
      else={
        <Scroll class="flex-1" contentClass={pageEnd()} contentOffset={props.scroll.contentOffset} onScroll={props.scroll.onScroll} ref={props.scroll.ref}>
          {renderHeader(false)}
          <If when={activeTab() === "albums"}>
            <View class="px-6">
              <CardGrid items={filteredAlbums().map(albumItemOf)} />
            </View>
          </If>
          <If when={activeTab() === "artists"}>
            <View class="px-6">
              <CardGrid items={filteredArtists().map(artistItemOf)} />
            </View>
          </If>
        </Scroll>
      }
    >
      <List
        each={trackRows()}
        keyExtractor={(row) => row.track.path}
        estimatedItemSize={48}
        class="flex-1"
        contentClass={pageEnd()}
        contentOffset={props.scroll.contentOffset}
        onScroll={props.scroll.onScroll}
        ref={props.scroll.ref}
        empty={() => (
          <View class="py-12 items-center">
            <Text class="text-sm text-muted-foreground">没有曲目</Text>
          </View>
        )}
        header={() => renderHeader(true)}
      >
        {(row, index) => (
          <View class="px-3">
            <If when={row().missing}>
              <LostTrackRow track={row().track} number={index()} />
            </If>
            <If when={!row().missing}>
              <TrackRow
                track={row().track}
                number={index()}
                showAlbum={wideTracks()}
                wave={props.wave}
                onPlay={() => playFromRows(filteredTracks(), index())}
              />
            </If>
          </View>
        )}
      </List>
    </If>
  );
}

/** 一个扫描找不到的文件：还在列表里，标成不可用，不给播（点了也没有
 * 文件可开）。下一次扫描找回它，这一行就回到普通行。 */
function LostTrackRow(props: { track: Track; number: number }) {
  return (
    <View class="mx-6 flex-row items-center gap-3 py-2 opacity-50" accessible={false}>
      <Text class="w-6 text-right text-xs text-muted-foreground">{props.number + 1}</Text>
      <View class="min-w-0 flex-1">
        <Text class="truncate text-sm text-foreground">{trackTitle(props.track)}</Text>
        <Text class="truncate text-xs text-muted-foreground">{trackArtist(props.track)}</Text>
      </View>
      <Text class="text-xs text-muted-foreground">不可用</Text>
    </View>
  );
}

/** The number over the table: 总数 while loading (X / Y 首曲目), the
 * whole count once done. */
function TracksHead(props: { count: number; total: number; state: string }) {
  // The whole count once done; while loading with pages in, X / Y.
  const label = () =>
    props.state === "complete"
      ? `${props.total} 首曲目`
      : props.state === "loading" && props.total > 0
        ? `${props.total} 首曲目 · 读到第 ${props.count} 首`
        : "";
  return (
    <If when={label()}>
      <Text class="px-6 pb-2 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
        {label()}
      </Text>
    </If>
  );
}

function EmptyTracks(props: { state: "idle" | "loading" | "complete" | "error" }) {
  return (
    <If
      when={props.state === "error"}
      else={
        <If
          when={props.state === "loading"}
          else={
            <EmptyState title="曲库是空的" detail="添加一个音乐文件夹，扫描把里面的文件收进曲库。">
              <ActionButton label="添加文件夹" onPress={() => void addRoot()} />
            </EmptyState>
          }
        >
          <RowsSkeleton rows={8} header />
        </If>
      }
    >
      <ErrorState message="曲目没有读出来。" onRetry={retryTracks} />
    </If>
  );
}
