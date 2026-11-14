// The sidebar: the 换页 panel (首页), the library panel (专辑、歌手、全部
// 曲目), the 曲库 panel (local roots and remote servers in one card) and the
// 曲单 panel at the foot — a list that grows belongs last. On a narrow
// window the panels become a rail of icons, each with a tooltip; the 曲库
// and 曲单 panels are only on the open sidebar. Every row shares one
// geometry: 44 high, its glyph in the same 30pt column, so the labels line
// up — including the ones whose label used to be `text-base font-bold`.

import { createMotion, effect, reduceMotion, token, untracked, View } from "nsh";
import { Button } from "nsh/base";
import { Text } from "nsh";
import { ALBUMS, ARTISTS, LIKED, PLAYLISTS, REMOTE, TRACKS, currentRoot, switchTo, type Route } from "../../nav";
import { favourites, playlists as playlistList } from "../../state/user-catalog";
import { setNewPlaylistOpen } from "./playlist-dialogs";
import { addRoot, removeRoot, roots, rootsLoaded, rescanAll, scan } from "../../state/catalog";
import type { Playlist } from "../../backend/types";
import { activeRemoteServer, isRemoteConfigured } from "../../state/remote";
import { selectSource, servers } from "../../state/sources";
import { setServerDialogOpen } from "../../state/overlays";
import { Hint, focusRing, LAYER } from "../ui/controls";
import { iconMotion, Icon, type IconName } from "../ui/icons";

/** A sidebar row's height and the gap under it (h-11, pb-1): rows are 48pt
 * apart in both panels. */
const ROW_STEP = 44 + 4;

/** A panel's selection (transitions.dev "Tabs sliding"): one background
 * behind the rows that slides to the selected one (`index`, -1: none),
 * rather than each row lighting its own. With no selection it fades away;
 * from no selection it fades in where it lands. */
function SlidingSelection(props: { index: () => number; top: number }) {
  const motion = createMotion({ initial: { opacity: 0 } });
  let at: number | undefined;
  effect(() => {
    const i = props.index();
    untracked(() => {
      const from = at;
      at = i;
      if (from === i) return;
      const y = props.top + i * ROW_STEP;
      // The first placement, and reduced motion, jump; later ones slide.
      if (from === undefined || reduceMotion()) {
        if (i >= 0) motion.translateY.set(y);
        motion.opacity.set(i >= 0 ? 1 : 0);
        return;
      }
      const timing = { duration: 240, easing: "easeInOut" as const };
      if (i < 0) {
        void motion.opacity.timing(0, timing);
        return;
      }
      if (from < 0) motion.translateY.set(y);
      else void motion.translateY.timing(y, timing);
      void motion.opacity.timing(1, timing);
    });
  });
  return (
    <View
      class="pointer-events-none absolute left-2 right-2 top-0 h-11 rounded-(--radius-inset) bg-secondary"
      style={{ opacity: motion.opacity, transform: [{ translateY: motion.translateY }] }}
    />
  );
}

/** One of the nav or library rows. */
function NavRow(props: { icon: IconName; label: string; route: Route; compact: boolean }) {
  const selected = () => currentRoot().name === props.route.name;
  const hover = iconMotion();
  return (
    <View onMouseEnter={() => hover.play(props.icon)}>
      <Button
        accessibilityLabel={props.label}
        selected={selected()}
        onPress={() => switchTo(props.route)}
        tooltip={props.compact ? <Hint label={props.label} /> : undefined}
        class={`group h-11 flex-row items-center gap-3 rounded-(--radius-inset) transition duration-200 ease-out hover:bg-row-hover active:scale-[0.98] motion-reduce:transition-none ${focusRing} ${
          props.compact ? "justify-center" : "px-2"
        }`}
      >
        {/* As wide as a row's glyph column, so icons and labels line up. */}
        <View class={`w-[30px] items-center ${LAYER} group-active:scale-90`}>
          <View style={hover.style}>
            <Icon name={props.icon} size={22} color={selected() ? "foreground" : "muted-foreground"} />
          </View>
        </View>
        <If when={!props.compact}>
          <Text
            class={`text-base font-bold ${LAYER} group-hover:translate-x-0.5 ${
              selected() ? "text-foreground" : "text-muted-foreground group-hover:text-foreground"
            }`}
          >
            {props.label}
          </Text>
        </If>
      </Button>
    </View>
  );
}

/** The panel that switches pages: 首页. */
function NavPanel(props: { compact: boolean }) {
  const index = () => (currentRoot().name === "home" ? 0 : -1);
  return (
    <View class="relative gap-1 rounded-(--radius-panel) border border-border bg-card p-2" accessibilityLabel="页面">
      <SlidingSelection index={index} top={8} />
      <NavRow icon="home" label="首页" route={{ name: "home" }} compact={props.compact} />
    </View>
  );
}

/** The library panel: 喜欢的歌曲、专辑、歌手、全部曲目. */
function LibraryPanel(props: { compact: boolean }) {
  const index = () => {
    const root = currentRoot().name;
    return (
      root === "liked" ? 0 : root === "albums" ? 1 : root === "artists" ? 2 : root === "tracks" ? 3 : root === "playlists" ? 4 : -1
    );
  };
  const favCount = favourites().length;
  const likedRow = () => (
    <View class="flex-row items-center">
      <NavRow icon="heartFilled" label="喜欢的歌曲" route={LIKED} compact={props.compact} />
      <If when={favCount > 0 && !props.compact}>
        <Text class="-ml-2 shrink-0 pr-2 text-xs tabular-nums text-muted-foreground">{favCount}</Text>
      </If>
    </View>
  );
  return (
    <View class="relative gap-1 rounded-(--radius-panel) border border-border bg-card p-2" accessibilityLabel="曲库" role="navigation">
      <SlidingSelection index={index} top={8} />
      {likedRow()}
      <NavRow icon="disc" label="专辑" route={ALBUMS} compact={props.compact} />
      <NavRow icon="user" label="歌手" route={ARTISTS} compact={props.compact} />
      <NavRow icon="music" label="全部曲目" route={TRACKS} compact={props.compact} />
      <NavRow icon="queue" label="曲单" route={PLAYLISTS} compact={props.compact} />
    </View>
  );
}

/** One watched folder: its path, and an ✕ that removes it on hover. */
function RootRow(props: { path: string }) {
  return (
    <View class="group h-9 flex-row items-center gap-2 rounded-(--radius) px-2 hover:bg-row-hover">
      <View class="w-6 items-center">
        <Icon name="folder" size={16} color="muted-foreground" />
      </View>
      <Text class="min-w-0 flex-1 truncate text-xs text-foreground">
        {props.path}
      </Text>
      <Button
        accessibilityLabel={`移除 ${props.path}`}
        onPress={() => removeRoot(props.path)}
        class={`shrink-0 items-center justify-center rounded-(--radius) opacity-0 transition duration-150 ease-out hover:bg-muted group-hover:opacity-100 active:opacity-60 ${focusRing}`}
        style={{ width: 24, height: 24 }}
      >
        <Icon name="close" size={14} color="muted-foreground" />
      </Button>
    </View>
  );
}

/** 曲库 panel: where tracks come from. The local roots and the remote
 * servers are one question, so they share a card — 本地 first, 远程 under it,
 * separated by a rule rather than by another panel. */
function SourcesPanel() {
  const scanning = () => scan().running;
  const count = () => scan().lastAdded;
  const dialog = setServerDialogOpen;
  const list = () => servers();
  const activeId = () => activeRemoteServer()?.id;
  return (
    <View class="gap-1 rounded-(--radius-panel) border border-border bg-card p-2" accessibilityLabel="曲库">
      <Text class="px-2 pb-1 text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">本地</Text>
      <If when={roots().length > 0}>
        <View class="gap-0.5">
          <Each of={roots()}>
            {(root) => (
              <View class="pb-0.5">
                <RootRow path={root()} />
              </View>
            )}
          </Each>
        </View>
      </If>
      <If when={rootsLoaded() && roots().length === 0}>
        <Text class="px-2 pb-1 text-xs text-muted-foreground">还没有文件夹。添加一个，扫描把里面的文件收进曲库。</Text>
      </If>
      <View class="flex-row items-center gap-1">
        <Button
          accessibilityLabel="添加文件夹"
          onPress={() => void addRoot()}
          class={`group flex-row items-center gap-2 rounded-(--radius) px-2 py-1.5 transition duration-150 ease-out hover:bg-row-hover active:scale-[0.98] ${focusRing}`}
        >
          <Icon name="folderPlus" size={16} color="muted-foreground" />
          <Text class="text-xs font-semibold text-foreground">添加文件夹</Text>
        </Button>
        <If when={roots().length > 0}>
          <Button
            accessibilityLabel={scanning() ? "扫描进行中" : "重新扫描"}
            disabled={scanning()}
            onPress={rescanAll}
            class={`group flex-row items-center gap-2 rounded-(--radius) px-2 py-1.5 transition duration-150 ease-out hover:bg-row-hover active:scale-[0.98] disabled:opacity-40 ${focusRing}`}
          >
            <Icon name="refresh" size={16} color="muted-foreground" />
            <If when={scanning()} else={<Text class="text-xs font-semibold text-foreground">扫描</Text>}>
              <Text class="text-xs font-semibold text-muted-foreground">扫描中…</Text>
            </If>
          </Button>
        </If>
        <If when={!scanning() && count() !== null}>
          <Text class="pl-1 text-[11px] text-muted-foreground">上次 +{count()}</Text>
        </If>
      </View>

      {/* The remote half: one row per configured server, the glyph carrying
          the state instead of a coloured dot. */}
      <View class="my-1 h-px" style={{ backgroundColor: token("border") }} />
      <Text class="px-2 pb-1 text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">远程</Text>
      <If when={list().length === 0}>
        <Button
          accessibilityLabel="连接服务器"
          onPress={() => dialog(true)}
          class={`group flex-row items-center gap-2 rounded-(--radius) px-2 py-1.5 transition duration-150 ease-out hover:bg-row-hover active:scale-[0.98] ${focusRing}`}
        >
          <Icon name="server" size={16} color="muted-foreground" />
          <Text class="text-xs font-semibold text-foreground">连接服务器…</Text>
        </Button>
      </If>
      <If when={list().length > 0}>
        <View class="gap-0.5">
          <Each of={list()}>
            {(server) => {
              const active = () => isRemoteConfigured() && activeId() === server().id;
              return (
                <Button
                  accessibilityLabel={`服务器 ${server().label}${active() ? "，已连接" : ""}`}
                  onPress={() => {
                    selectSource(`remote:${server().id}`);
                    switchTo(REMOTE);
                  }}
                  class={`flex-row items-center gap-2 rounded-(--radius) px-2 py-1.5 transition duration-150 ease-out ${
                    active() ? "bg-secondary" : "hover:bg-row-hover"
                  } ${focusRing}`}
                >
                  <View class="w-6 items-center">
                    <Icon name="server" size={16} color={active() ? "foreground" : "muted-foreground"} />
                  </View>
                  <Text
                    class={`min-w-0 flex-1 truncate text-left text-xs font-semibold ${
                      active() ? "text-foreground" : "text-muted-foreground"
                    }`}
                  >
                    {server().label}
                  </Text>
                </Button>
              );
            }}
          </Each>
        </View>
        <Button
          accessibilityLabel="添加服务器"
          onPress={() => dialog(true)}
          class={`flex-row items-center gap-1.5 rounded-(--radius) px-2 py-1 hover:bg-row-hover ${focusRing}`}
        >
          <Icon name="plus" size={14} color="muted-foreground" />
          <Text class="text-[11px] text-muted-foreground">添加服务器…</Text>
        </Button>
      </If>
    </View>
  );
}

/** The whole sidebar; `compact` is the desktop's icon rail. */
export function Sidebar(props: { compact?: boolean }) {
  const compact = props.compact ?? false;
  return (
    <View class="min-h-0 flex-1 gap-2" role="navigation">
      <NavPanel compact={compact} />
      <LibraryPanel compact={compact} />
      <If when={!compact}>
        <SourcesPanel />
      </If>
    </View>
  );
}
