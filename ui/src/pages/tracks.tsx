// 全部曲目: the whole index in one virtualized, recycling list. The index
// streams in page by page (500 rows each); rows show as they land, and
// the list scrolls on as far as what arrived. A scan that found or lost
// files restarts the load.

import { effect, List, Scroll, signal, Text, token, untracked, View } from "nsh";
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
  removeRoot,
  retryTracks,
  scopedRoot,
  tracksState,
  tracksTotal,
} from "../state/catalog";
import { AppPage } from "../components/page";
import { TrackHeader, TrackRow } from "../components/library/tracks";
import { playFromRows, playQueue } from "../state/player";
import { EmptyState, ErrorState, RowsSkeleton } from "../components/ui/feedback";
import { ActionButton, focusRing } from "../components/ui/controls";
import { createListWave, enter, type ListWave } from "../components/ui/motion";
import { currentRoute, rootOpens, switchTo, HOME } from "../nav";
import { activeSource, LOCAL_ALL_ID, selectSource } from "../state/sources";
import { queueRemoteFrom, remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { RemoteTrackRow } from "./remote";
import { barSize, wideTracks } from "../components/layout";
import { SelectionBar } from "../components/library/selection-bar";
import { selectionActive } from "../state/queue";
import { CardGrid } from "../components/library/cards";
import { shuffled } from "../state/shuffle";
import { engine } from "../backend/engine";
import { albumKey, type Album, type Artist, type Track } from "../backend/types";

export function TracksPage() {
  effect(() => {
    dataEpoch();
    untracked(() => ensureTracks());
  });
  // The rows' wave, played as the page is entered (rows scrolled to come
  // in on their own).
  const wave = createListWave({ isTop: () => currentRoute().name === "tracks", reenter: rootOpens });
  const state = tracksState();
  const list = homeTracks();
  const showAlbum = wideTracks();

  const remote = () => activeSource().kind === "remote";
  /** A folder picked in the sidebar is this page's subject: it says which one
   * instead of claiming to be every track. */
  const rootName = () => {
    const root = scopedRoot();
    if (root === null) return "";
    const parts = root.split(/[\\/]/).filter((part) => part !== "");
    return parts.length > 0 ? parts[parts.length - 1] : root;
  };
  const title = () => (rootName() === "" ? "全部曲目" : rootName());
  const paths = () => list.map((t) => t.path);
  const isScoped = () => scopedRoot() !== null;
  return (
    <AppPage title={title()} testID="page-tracks">
      <If
        when={isScoped()}
        else={
          <If
            when={remote()}
            else={
          <List
            each={list}
            keyExtractor={(t) => t.path}
            estimatedItemSize={48}
            empty={() => <EmptyTracks state={state} />}
            header={() => (
              <View class="pt-2">
                <TracksHead count={homeTracks().length} total={scopedRoot() === null ? Math.max(tracksTotal(), list.length) : homeTracks().length} state={scopedRoot() === null ? (list.length > 0 && state === "idle" ? "complete" : state) : "complete"} />
                <TrackHeader showAlbum={showAlbum} />
              </View>
            )}
            footer={() => (
              <If when={state === "loading" && list.length > 0}>
                <View class="py-6 items-center">
                  <Text class="text-xs text-muted-foreground">正在读曲库…</Text>
                </View>
              </If>
            )}
            class="flex-1"
            contentClass="pb-28"
          >
            {(track, index) => (
              <TrackRow
                track={track()}
                number={index()}
                showAlbum={showAlbum}
                wave={wave}
                onPlay={() => playFromRows(list, index())}
              />
            )}
          </List>
            }
          >
            <List
              each={remoteTracks()}
              keyExtractor={(t) => t.id}
              estimatedItemSize={48}
              class="flex-1"
              contentClass="pb-28"
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
                <View class="px-6 pb-4 pt-2">
                  <Text class="text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
                    {`${remoteTracks().length} 首远程曲目`}
                  </Text>
                </View>
              )}
              footer={() => (
                <If when={remoteLoadingState() === "loading" && remoteTracks().length > 0}>
                  <View class="py-6 items-center">
                    <Text class="text-xs text-muted-foreground">正在读远程库…</Text>
                  </View>
                </If>
              )}
            >
              {(track, index) => <RemoteTrackRow track={track()} number={index()} onPlay={() => queueRemoteFrom(remoteTracks(), index())} />}
            </List>
          </If>
        }
      >
        <ScopedFolderWorkspace root={scopedRoot()!} wave={wave} />
      </If>
      <If when={selectionActive()}>
        <SelectionBar allPaths={paths()} bottom={barSize() === "stacked" ? 128 : 96} />
      </If>
    </AppPage>
  );
}

function ScopedFolderWorkspace(props: { root: string; wave: ListWave }) {
  const root = props.root;
  const leaf = () => root.replace(/\\/g, "/").split("/").pop() || root;
  const query = signal("");
  const activeTab = signal<"tracks" | "albums" | "artists">("tracks");

  const sourceTracks = () => homeTracks();
  const sourceAlbums = () => homeAlbums();
  const sourceArtists = () => homeArtists();

  const q = () => query().trim().toLowerCase();
  const filteredTracks = (): Track[] => {
    const term = q();
    if (!term) return sourceTracks();
    return sourceTracks().filter(
      (t) =>
        t.title.toLowerCase().includes(term) ||
        t.artist.toLowerCase().includes(term) ||
        t.album.toLowerCase().includes(term),
    );
  };
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
          <ActionButton label="播放全部" onPress={() => playFromRows(sourceTracks(), 0)} />
          <ActionButton
            label="随机播放"
            variant="outline"
            onPress={() => playQueue(shuffled(sourceTracks()), 0)}
          />
          <ActionButton label="重新扫描" variant="outline" onPress={() => engine.scanFolder(root)} />
          <ActionButton
            label="移除来源"
            variant="outline"
            onPress={() => {
              removeRoot(root);
              selectSource(LOCAL_ALL_ID);
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
        <Scroll class="flex-1" contentClass="pb-28">
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
        each={filteredTracks()}
        keyExtractor={(t) => t.path}
        estimatedItemSize={48}
        class="flex-1"
        contentClass="pb-28"
        empty={() => (
          <View class="py-12 items-center">
            <Text class="text-sm text-muted-foreground">没有曲目</Text>
          </View>
        )}
        header={() => renderHeader(true)}
      >
        {(track, index) => (
          <View class="px-3">
            <TrackRow
              track={track()}
              number={index()}
              showAlbum={wideTracks()}
              wave={props.wave}
              onPlay={() => playFromRows(filteredTracks(), index())}
            />
          </View>
        )}
      </List>
    </If>
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
      <Text class="px-6 pb-4 text-3xl font-bold text-foreground" style={enter("fadeInUp")}>
        {label()}
      </Text>
    </If>
  );
}

function EmptyTracks(props: { state: "idle" | "loading" | "complete" | "error" }) {
  // While the first page is on its way: skeleton rows shaped like the
  // table; an error: 重试; an empty (scanned to nothing or before any
  // scan): what to do.
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
