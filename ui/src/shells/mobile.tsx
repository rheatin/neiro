// The mobile shell: the four-place application on a phone and on a foldable.
//
// Folded, it is one column — the page, the mini player, the tab bar — each
// row inset by the safe area NSH reports, so nothing sits under the home
// indicator. The page's title, its back button and the gear are not drawn
// here: iOS gives every `NavPage` a system header, and a second header above
// it would be a duplicate row that scrolls nothing.
//
// Unfolded, the same column moves beside a sidebar that lists the same four
// places; the tab bar is gone because the sidebar is the tab bar. Which shell
// is drawn comes from the measured width (`layout.tsx`), not a hinge API: the
// cover screen and an iPhone fold, the inner screen and an iPad do not.
//
// The tabs are places, not routes: `activateTab` keeps each tab's stack (see
// `nav.ts`), which is the one thing the desktop's `openRoot` deliberately
// does not do.

import { platform, safeArea, Text, View } from "nsh";
import { Button } from "nsh/base";
import { Icon, type IconName } from "../components/ui/icons";
import { IconButton, focusRing } from "../components/ui/controls";
import { NowPlayingSummary } from "../components/player/parts";
import { MainStack } from "./main";
import { activateTab, activeRootName, MOBILE_ROOTS, open, SETTINGS, type Route } from "../nav";
import { mobileShell, measureMobileShell } from "../components/layout";
import { hasTrack, next, player, toggle as togglePlayback } from "../state/player";

/** What plays, and the two controls a phone needs without leaving the page:
 * play/pause and next. The summary opens Now Playing. */
function MiniPlayer() {
  const isIos = platform.os === "ios";
  return (
    <If when={hasTrack()}>
      <View
        class={`mx-5 mb-2.5 h-14 flex-row items-center gap-1 rounded-full pl-2 pr-1.5 ${
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
  const selected = () => activeRootName() === props.root.name;
  if (props.sidebar) {
    return (
      <Button
        accessibilityLabel={props.label}
        onPress={() => activateTab(props.root)}
        class={`h-11 flex-row items-center gap-3 rounded-(--radius-inset) px-3 active:opacity-60 ${selected() ? "bg-secondary" : "hover:bg-row-hover"} ${focusRing}`}
      >
        <Icon name={props.icon} size={20} color={selected() ? "foreground" : "muted-foreground"} />
        <Text class={`text-sm ${selected() ? "font-semibold text-foreground" : "text-muted-foreground"}`}>
          {props.label}
        </Text>
      </Button>
    );
  }
  return (
    <Button
      accessibilityLabel={props.label}
      onPress={() => activateTab(props.root)}
      class={`flex-1 items-center justify-center gap-0.5 rounded-full py-1.5 active:opacity-60 ${
        selected() ? "bg-foreground/10" : ""
      }`}
    >
      <Icon name={props.icon} size={20} color={selected() ? "foreground" : "muted-foreground"} />
      <Text class={`text-[10px] ${selected() ? "font-semibold text-foreground" : "text-muted-foreground"}`}>
        {props.label}
      </Text>
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
      class={`mx-5 flex-row items-stretch rounded-full p-1.5 ${
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
  return (
    <View
      testID="mobile-sidebar"
      class="w-56 shrink-0 gap-1 border-r border-border bg-sidebar px-2"
      style={{
        paddingTop: safeArea().top + 8,
        paddingBottom: safeArea().bottom + 8,
        paddingLeft: safeArea().left + 8,
      }}
    >
      <Each of={PLACES}>
        {(place) => <Place root={place().root} icon={place().icon} label={place().label} sidebar />}
      </Each>
      <View class="flex-1" />
      <Button
        accessibilityLabel="设置"
        onPress={() => open(SETTINGS)}
        class={`h-11 flex-row items-center gap-3 rounded-(--radius-inset) px-3 active:opacity-60 hover:bg-row-hover ${focusRing}`}
      >
        <Icon name="settings" size={20} color="muted-foreground" />
        <Text class="text-sm text-muted-foreground">设置</Text>
      </Button>
    </View>
  );
}

export function MobileShell() {
  return (
    <View class="flex-1 bg-background" onLayout={(e) => measureMobileShell(e.bounds.size.width)}>
      <If
        when={mobileShell() === "unfolded"}
        else={
          <View class="relative min-h-0 flex-1">
            <MainStack />
            <View
              class="absolute inset-x-0 bottom-0"
              style={{
                pointerEvents: "box-none",
                paddingBottom: Math.max(12, safeArea().bottom),
              }}
            >
              <MiniPlayer />
              <TabBar />
            </View>
          </View>
        }
      >
        <View class="min-h-0 flex-1 flex-row" style={{ paddingRight: safeArea().right }}>
          <Sidebar />
          <View class="relative min-w-0 flex-1">
            <MainStack />
            <View
              class="absolute inset-x-0 bottom-0"
              style={{
                pointerEvents: "box-none",
                paddingBottom: Math.max(8, safeArea().bottom),
              }}
            >
              <MiniPlayer />
            </View>
          </View>
        </View>
      </If>
    </View>
  );
}
