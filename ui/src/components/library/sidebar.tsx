// The sidebar: 换页 panel (首页), the library panel (专辑、歌手、全部
// 曲目) and the 来源 panel (local roots and remote servers). On a narrow
// window every panel becomes a rail of icons, each with a tooltip — no
// labels, so nothing is ever cut in half. Every row shares one geometry:
// 44 high, its glyph in the same 30pt column, so the icons line up.

import { createMotion, effect, reduceMotion, signal, token, untracked, View } from "nsh";
import { Button } from "nsh/base";
import { Text } from "nsh";
import { ALBUMS, ARTISTS, LIKED, PLAYLISTS, REMOTE, TRACKS, currentRoot, switchTo, type Route } from "../../nav";
import { favourites } from "../../state/user-catalog";
import { setNewPlaylistOpen } from "./playlist-dialogs";
import { removeRoot, roots, scopedRoot, setHomeRoot } from "../../state/catalog";
import type { Playlist } from "../../backend/types";
import { activeRemoteServer, isRemoteConfigured, removeServerById } from "../../state/remote";
import { allSources, LOCAL_ALL_ID, selectSource, servers } from "../../state/sources";
import { openSourceDialog } from "../ui/server-dialog";
import { Hint, focusRing, LAYER } from "../ui/controls";
import { iconMotion, Icon, type IconName } from "../ui/icons";
import { openContextMenu } from "../ui/context-menu";

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
function NavRow(props: {
  icon: IconName;
  label: string;
  route: Route;
  compact: boolean;
  /** Runs before the page changes: a row that also means "the whole library"
   * has to clear a scoped source, or the page stays where it was. */
  action?: () => void;
}) {
  const selected = () => currentRoot().name === props.route.name;
  const hover = iconMotion();
  return (
    <View onMouseEnter={() => hover.play(props.icon)}>
      <Button
        accessibilityLabel={props.label}
        selected={selected()}
        onPress={() => {
          props.action?.();
          switchTo(props.route);
        }}
        tooltip={props.compact ? <Hint label={props.label} /> : undefined}
        class={`group h-11 flex-row items-center rounded-(--radius-inset) transition duration-200 ease-out hover:bg-row-hover active:scale-[0.98] motion-reduce:transition-none ${focusRing} ${
          props.compact ? "justify-center" : "gap-3 px-2"
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
            class={`min-w-0 flex-1 truncate text-base font-bold ${LAYER} group-hover:translate-x-0.5 ${
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
      root === "liked" ? 0
      : root === "albums" ? 1
      : root === "artists" ? 2
      : root === "tracks" ? (scopedRoot() === null ? 3 : -1)
      : root === "playlists" ? 4
      : -1
    );
  };
  const favCount = favourites().length;
  const likedRow = () => (
    <View class="w-full flex-row items-center">
      <View class="min-w-0 flex-1">
        <NavRow icon="heartFilled" label="喜欢的歌曲" route={LIKED} compact={props.compact} />
      </View>
      <If when={favCount > 0 && !props.compact}>
        <Text class="shrink-0 pl-1 pr-2 text-xs tabular-nums text-muted-foreground">{favCount}</Text>
      </If>
    </View>
  );
  return (
    <View class="relative gap-1 rounded-(--radius-panel) border border-border bg-card p-2" accessibilityLabel="曲库" role="navigation">
      <SlidingSelection index={index} top={8} />
      {likedRow()}
      <NavRow icon="disc" label="专辑" route={ALBUMS} compact={props.compact} />
      <NavRow icon="user" label="歌手" route={ARTISTS} compact={props.compact} />
      <NavRow
        icon="music"
        label="全部曲目"
        route={TRACKS}
        compact={props.compact}
        action={() => selectSource(LOCAL_ALL_ID)}
      />
      <NavRow icon="queue" label="曲单" route={PLAYLISTS} compact={props.compact} />
    </View>
  );
}



function SourcesPanel(props: { compact?: boolean }) {
  const compact = () => props.compact === true;
  const list = () => servers();
  const activeId = () => activeRemoteServer()?.id;
  const active = (id: string) => () => isRemoteConfigured() && activeId() === id;
  return (
    <View class="gap-1 rounded-(--radius-panel) border border-border bg-card p-2" accessibilityLabel="来源" role="navigation">
      {/* Servers above the 加号, folders below it, the 加号 between them. With
          nothing added, the 加号 is the whole panel. */}
      <Each of={list()}>
        {(server) => (
          <SourceRow
            compact={compact()}
            icon="server"
            label={server().label}
            detail={server().baseUrl}
            active={active(server().id)()}
            testID="source-server"
            onRemove={() => void removeServerById(server().id)}
            onPress={() => {
              selectSource(`remote:${server().id}`);
              switchTo(REMOTE);
            }}
          />
        )}
      </Each>
      <Each of={roots()}>
        {(root) => (
          <SourceRow
            compact={compact()}
            icon="folder"
            label={leafOf(root())}
            detail={root()}
            active={scopedRoot() === root()}
            testID="source-root"
            onRemove={() => removeRoot(root())}
            onPress={() => {
              // One source state, chosen the same way the page's source tabs
              // choose it: by the id `allSources()` gives that root.
              const source = allSources().find((entry) => entry.root === root());
              if (source) selectSource(source.id);
              else setHomeRoot(root());
              switchTo(TRACKS);
            }}
          />
        )}
      </Each>
      <SourceRow
        compact={compact()}
        icon="plus"
        label="添加来源"
        testID="rail-source-add"
        onPress={() => openSourceDialog()}
      />

    </View>
  );
}

/** One source: a server or a folder. Narrow, it is the icon alone — the label
 * lives in the tooltip and the accessible name. */
/** The last segment of a path: what the folder is called, not where it is. */
function leafOf(path: string): string {
  const parts = path.split(/[\\/]/).filter((part) => part !== "");
  return parts.length > 0 ? parts[parts.length - 1] : path;
}

function SourceRow(props: {
  compact: boolean;
  icon: IconName;
  label: string;
  /** The full fact behind a short label: a path, a server URL. */
  detail?: string;
  onPress: () => void;
  /** Right click: 移除 this source. */
  onRemove?: () => void;
  active?: boolean;
  testID?: string;
}) {
  return (
    // The menu lives on the wrapper: ButtonProps has no onContextMenu.
    <View
      onContextMenu={
        props.onRemove
          ? (e) => openContextMenu(e.pagePosition, [{ label: `移除 ${props.label}`, onSelect: props.onRemove! }])
          : undefined
      }
    >
    <Button
      testID={props.testID}
      accessibilityLabel={props.detail ?? props.label}
      tooltip={<Hint label={props.detail ?? props.label} />}
      onPress={props.onPress}
      class={`h-11 flex-row items-center rounded-(--radius-inset) transition duration-200 ease-out hover:bg-row-hover active:scale-[0.98] motion-reduce:transition-none ${focusRing} ${
        props.active ? "bg-secondary" : ""
      } ${props.compact ? "justify-center" : "gap-3 px-2"}`}
    >
      <View class="w-[30px] items-center">
        <Icon name={props.icon} size={22} color={props.active ? "foreground" : "muted-foreground"} />
      </View>
      <If when={!props.compact}>
        <Text
          class={`min-w-0 flex-1 truncate text-left text-xs font-bold ${
            props.active ? "text-foreground" : "text-muted-foreground"
          }`}
        >
          {props.label}
        </Text>
      </If>
    </Button>
    </View>
  );
}

/** The whole sidebar; `compact` is the desktop's icon rail. */
export function Sidebar(props: { compact?: boolean }) {
  /** What this column actually got, which is the only width that matters:
   * a flex parent or a display scale can leave less than the shell asked for,
   * and a 130pt column cannot hold a label however wide the window is. */
  const narrow = signal(false);
  const compact = () => (props.compact ?? false) || narrow();
  return (
    <View
      class="min-h-0 flex-1 gap-2"
      role="navigation"
      onLayout={(e) => narrow.set(e.bounds.size.width < 200)}
    >
      <NavPanel compact={compact()} />
      <LibraryPanel compact={compact()} />
      <SourcesPanel compact={compact()} />
    </View>
  );
}
