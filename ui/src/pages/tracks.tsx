// 全部曲目: the whole index in one virtualized, recycling list. The index
// streams in page by page (500 rows each); rows show as they land, and
// the list scrolls on as far as what arrived. A scan that found or lost
// files restarts the load.

import { effect, List, Text, untracked, View } from "nsh";
import {
  addRoot,
  dataEpoch,
  ensureTracks,
  retryTracks,
  tracksState,
  tracksTotal,
  homeTracks,
  scopedRoot,
} from "../state/catalog";
import { AppPage } from "../components/page";
import { TrackHeader, TrackRow } from "../components/library/tracks";
import { EmptyState, ErrorState, RowsSkeleton } from "../components/ui/feedback";
import { ActionButton } from "../components/ui/controls";
import { createListWave, enter } from "../components/ui/motion";
import { currentRoute, rootOpens } from "../nav";
import { activeSource } from "../state/sources";
import { remoteLoadingState, remoteTracks, retryRemote } from "../state/remote";
import { RemoteTrackRow } from "./remote";
import { barSize, wideTracks } from "../components/layout";
import { SelectionBar } from "../components/library/selection-bar";
import { selectionActive } from "../state/queue";

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
  return (
    <AppPage title={title()} testID="page-tracks">
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
            <TracksHead count={homeTracks().length} total={scopedRoot() === null ? tracksTotal() : homeTracks().length} state={scopedRoot() === null ? state : "complete"} />
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
        {(track, index) => <TrackRow track={track()} number={index()} showAlbum={showAlbum} wave={wave} />}
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
          {(track, index) => <RemoteTrackRow track={track()} number={index()} />}
        </List>
      </If>
      <If when={selectionActive()}>
        <SelectionBar allPaths={paths()} bottom={barSize() === "stacked" ? 128 : 96} />
      </If>
    </AppPage>
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
