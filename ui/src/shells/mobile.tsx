// The mobile shell: the four-place application on a phone, a foldable, and a tablet.
//
// Under 1000 pt (phones, foldable cover & inner screens, and portrait iPads),
// the shell uses one unified full-width column with a centered, max-width
// floating Liquid Glass capsule TabBar and MiniPlayer at the bottom.
//
// At 1000 pt and above (landscape iPads / wide tablets), a collapsible
// floating rounded Liquid Glass sidebar unfolds on the left with top places,
// library categories, playlists, and remote sources, while MiniPlayer stays
// centered at the bottom.

import { platform, safeArea, Scroll, signal, Text, View } from "nsh";
import { Button } from "nsh/base";
import { Icon, type IconName } from "../components/ui/icons";
import { IconButton, focusRing } from "../components/ui/controls";
import { NowPlayingSummary } from "../components/player/parts";
import { MainStack } from "./main";
import {
  activateTab,
  activeRootName,
  ALBUMS,
  ARTISTS,
  currentRoute,
  LIKED,
  MOBILE_ROOTS,
  open,
  REMOTE,
  SETTINGS,
  TRACKS,
  type Route,
} from "../nav";
import { mobileShell, measureMobileShell } from "../components/layout";
import { hasTrack, next, player, toggle as togglePlayback } from "../state/player";
import { favourites, playlists } from "../state/user-catalog";
import { ALL_ID, selectSource, servers } from "../state/sources";
import { activeRemoteServer, isRemoteConfigured } from "../state/remote";
import { isRemoteKindEnabled } from "../state/plugins";
import { setNewPlaylistOpen } from "../components/library/playlist-dialogs";

const sidebarOpen = signal(true);

/** What plays, and the two controls a phone needs without leaving the page:
 * play/pause and next. The summary opens Now Playing. */
function MiniPlayer() {
  const isIos = platform.os === "ios";
  return (
    <If when={hasTrack()}>
      <View
        class={`mb-2.5 h-14 flex-row items-center gap-1 rounded-full pl-2 pr-1.5 ${
          isIos ? "" : "border border-border bg-card/90"
        }`}
        style={{
          glassEffect: "regular",
          backdropFilter: "blur(20px)",
          boxShadow: [{ x: 0, y: 8, blur: 24, color: "#00000018" }],
        }}
        ios={{ backdropMaterial: "ultraThin" }}
      >
        <NowPlayingSummary openable round cover={40} />
        <IconButton
          name={player().state === "playing" ? "pause" : "play"}
          label={player().state === "playing" ? "暂停" : "播放"}
          size={24}
          onPress={() => togglePlayback()}
        />
        <IconButton name="skipForward" label="下一曲" size={22} onPress={() => next()} />
      </View>
    </If>
  );
}

/** One place in the tab bar or in the unfolded sidebar. */
function Place(props: { root: Route; icon: IconName; label: string; sidebar?: boolean }) {
  const selected = () => activeRootName() === props.root.name && currentRoute().name === props.root.name;
  if (props.sidebar) {
    return (
      <Button
        accessibilityLabel={props.label}
        onPress={() => activateTab(props.root)}
        class={`h-10 flex-row items-center gap-3 rounded-(--radius-inset) px-3 active:opacity-60 ${
          selected() ? "bg-foreground/10" : "hover:bg-row-hover"
        } ${focusRing}`}
      >
        <Icon name={props.icon} size={18} color={selected() ? "foreground" : "muted-foreground"} />
        <Text class={`text-sm ${selected() ? "font-semibold text-foreground" : "text-muted-foreground"}`}>
          {props.label}
        </Text>
      </Button>
    );
  }
  const tabSelected = () => activeRootName() === props.root.name;
  return (
    <Button
      accessibilityLabel={props.label}
      onPress={() => activateTab(props.root)}
      class={`flex-1 items-center justify-center gap-0.5 rounded-full py-1.5 active:opacity-60 ${
        tabSelected() ? "bg-foreground/10" : ""
      }`}
    >
      <Icon name={props.icon} size={20} color={tabSelected() ? "foreground" : "muted-foreground"} />
      <Text class={`text-[10px] ${tabSelected() ? "font-semibold text-foreground" : "text-muted-foreground"}`}>
        {props.label}
      </Text>
    </Button>
  );
}

function SidebarRow(props: {
  icon: IconName;
  label: string;
  selected?: boolean;
  trailing?: string;
  dot?: boolean;
  onPress: () => void;
}) {
  return (
    <Button
      accessibilityLabel={props.label}
      onPress={props.onPress}
      class={`h-10 flex-row items-center gap-3 rounded-(--radius-inset) px-3 active:opacity-60 ${
        props.selected ? "bg-foreground/10" : "hover:bg-row-hover"
      } ${focusRing}`}
    >
      <Icon name={props.icon} size={18} color={props.selected ? "foreground" : "muted-foreground"} />
      <Text
        class={`min-w-0 flex-1 truncate text-sm ${
          props.selected ? "font-semibold text-foreground" : "text-muted-foreground"
        }`}
      >
        {props.label}
      </Text>
      <If when={props.dot}>
        <View class="h-2 w-2 rounded-full bg-emerald-500" />
      </If>
      <If when={props.trailing}>
        <Text class="shrink-0 text-xs tabular-nums text-muted-foreground">{props.trailing}</Text>
      </If>
    </Button>
  );
}

const PLACES: { root: Route; icon: IconName; label: string }[] = [
  { root: MOBILE_ROOTS[0], icon: "home", label: "首页" },
  { root: MOBILE_ROOTS[1], icon: "layers", label: "资料库" },
  { root: MOBILE_ROOTS[2], icon: "server", label: "服务器" },
  { root: MOBILE_ROOTS[3], icon: "search", label: "搜索" },
];

function TabBar() {
  const isIos = platform.os === "ios";
  return (
    <View
      testID="mobile-tab-bar"
      class={`flex-row items-stretch rounded-full p-1.5 ${
        isIos ? "" : "border border-border bg-sidebar/90"
      }`}
      style={{
        glassEffect: "regular",
        backdropFilter: "blur(20px)",
        boxShadow: [{ x: 0, y: 8, blur: 24, color: "#00000018" }],
      }}
      ios={{ backdropMaterial: "ultraThin" }}
    >
      <Each of={PLACES}>
        {(place) => <Place root={place().root} icon={place().icon} label={place().label} />}
      </Each>
    </View>
  );
}

function Sidebar() {
  const isIos = platform.os === "ios";
  const remoteList = () => servers().filter((s) => isRemoteKindEnabled(s.kind));
  const favCount = () => favourites().length;
  const playlistItems = () => playlists();
  const route = () => currentRoute();
  const activeServerId = () => activeRemoteServer()?.id;

  return (
    <View
      testID="mobile-sidebar"
      class="shrink-0"
      style={{
        paddingTop: safeArea().top + 12,
        paddingBottom: safeArea().bottom + 12,
        paddingLeft: safeArea().left + 12,
        paddingRight: 0,
      }}
    >
      <View
        class={`w-64 flex-1 rounded-[24px] p-3 ${isIos ? "" : "border border-border bg-card/90"}`}
        style={{
          glassEffect: "regular",
          backdropFilter: "blur(24px)",
          boxShadow: [{ x: 0, y: 10, blur: 28, color: "#00000018" }],
        }}
        ios={{ backdropMaterial: "regular" }}
      >
        <View class="mb-2 flex-row items-center justify-between px-2 pt-1">
          <Text class="text-base font-bold tracking-tight text-foreground">Neiro</Text>
          <IconButton
            name="sidebar"
            label="收起侧栏"
            size={18}
            onPress={() => sidebarOpen.set(false)}
          />
        </View>

        <Scroll class="min-h-0 flex-1" contentClass="gap-4 py-1">
          <View class="gap-0.5" accessibilityLabel="导航" role="navigation">
            <Each of={PLACES}>
              {(place) => <Place root={place().root} icon={place().icon} label={place().label} sidebar />}
            </Each>
          </View>

          <View class="gap-0.5" accessibilityLabel="曲库" role="navigation">
            <Text class="px-3 pb-1 text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">
              曲库
            </Text>
            <SidebarRow
              icon="heartFilled"
              label="喜欢的歌曲"
              selected={route().name === "liked"}
              trailing={favCount() > 0 ? String(favCount()) : undefined}
              onPress={() => open(LIKED)}
            />
            <SidebarRow
              icon="disc"
              label="专辑"
              selected={route().name === "albums"}
              onPress={() => open(ALBUMS)}
            />
            <SidebarRow
              icon="user"
              label="歌手"
              selected={route().name === "artists"}
              onPress={() => open(ARTISTS)}
            />
            <SidebarRow
              icon="music"
              label="全部曲目"
              selected={route().name === "tracks"}
              onPress={() => {
                selectSource(ALL_ID);
                open(TRACKS);
              }}
            />
          </View>

          <View class="gap-0.5" accessibilityLabel="曲单" role="navigation">
            <Text class="px-3 pb-1 text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">
              曲单
            </Text>
            <Each of={playlistItems()} key={(p) => p.id}>
              {(p) => (
                <SidebarRow
                  icon="queue"
                  label={p().name}
                  selected={route().name === "playlist" && (route() as { id?: number }).id === p().id}
                  trailing={String(p().trackCount)}
                  onPress={() => open({ name: "playlist", id: p().id, playlist: p().name })}
                />
              )}
            </Each>
            <SidebarRow icon="plus" label="新建曲单" onPress={() => setNewPlaylistOpen(true)} />
          </View>

          <If when={remoteList().length > 0}>
            <View class="gap-0.5" accessibilityLabel="来源" role="navigation">
              <Text class="px-3 pb-1 text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">
                来源
              </Text>
              <Each of={remoteList()}>
                {(srv) => (
                  <SidebarRow
                    icon="server"
                    label={srv().label}
                    selected={route().name === "remote" && activeServerId() === srv().id}
                    dot={isRemoteConfigured() && activeServerId() === srv().id}
                    onPress={() => {
                      selectSource(`remote:${srv().id}`);
                      activateTab(REMOTE);
                    }}
                  />
                )}
              </Each>
            </View>
          </If>
        </Scroll>

        <View class="mt-2 border-t border-border/40 pt-2">
          <Button
            accessibilityLabel="设置"
            onPress={() => open(SETTINGS)}
            class={`h-10 flex-row items-center gap-3 rounded-(--radius-inset) px-3 active:opacity-60 hover:bg-row-hover ${focusRing}`}
          >
            <Icon name="settings" size={18} color="muted-foreground" />
            <Text class="text-sm text-muted-foreground">设置</Text>
          </Button>
        </View>
      </View>
    </View>
  );
}

export function MobileShell() {
  const isIos = platform.os === "ios";
  return (
    <View class="flex-1 bg-background" onLayout={(e) => measureMobileShell(e.bounds.size.width)}>
      <If
        when={mobileShell() === "unfolded"}
        else={
          <View class="relative min-h-0 flex-1">
            <MainStack />
            <View
              class="absolute inset-x-0 bottom-0 items-center"
              style={{
                pointerEvents: "box-none",
                paddingBottom: Math.max(12, safeArea().bottom),
                paddingLeft: Math.max(20, safeArea().left + 16),
                paddingRight: Math.max(20, safeArea().right + 16),
              }}
            >
              <View class="w-full" style={{ maxWidth: 400, pointerEvents: "box-none" }}>
                <MiniPlayer />
                <TabBar />
              </View>
            </View>
          </View>
        }
      >
        <View class="min-h-0 flex-1 flex-row" style={{ paddingRight: safeArea().right }}>
          <If when={sidebarOpen()}>
            <Sidebar />
          </If>
          <View class="relative min-w-0 flex-1">
            <MainStack />
            <If when={!sidebarOpen()}>
              <View
                class="absolute items-center justify-center rounded-full"
                style={{
                  top: safeArea().top + 10,
                  left: safeArea().left + 12,
                  glassEffect: "regular",
                  backdropFilter: "blur(20px)",
                  boxShadow: [{ x: 0, y: 6, blur: 18, color: "#00000018" }],
                  ...(isIos ? {} : { borderWidth: 1 }),
                }}
                ios={{ backdropMaterial: "ultraThin" }}
              >
                <IconButton
                  name="sidebar"
                  label="展开侧栏"
                  size={18}
                  onPress={() => sidebarOpen.set(true)}
                />
              </View>
            </If>
            <View
              class="absolute inset-x-0 bottom-0 items-center"
              style={{
                pointerEvents: "box-none",
                paddingBottom: Math.max(12, safeArea().bottom),
                paddingLeft: 20,
                paddingRight: 20,
              }}
            >
              <View class="w-full" style={{ maxWidth: 440, pointerEvents: "box-none" }}>
                <MiniPlayer />
              </View>
            </View>
          </View>
        </View>
      </If>
    </View>
  );
}
