// The desktop's shell: the sidebar (a rail on narrow windows, three
// panels on wide ones) down the left, and the pages on the right with the
// player bar floating over their bottom, a pill the pages scroll behind.

import { token, View } from "nsh";
import { barSize, measureShell, railSidebar, sidebarShown, sidebarWidth } from "../components/layout";
import { animateLayoutOn } from "../components/ui/motion";
import { Sidebar } from "../components/library/sidebar";
import { PlayerBar } from "../components/player/player-bar";
import { MainStack } from "./main";
import { nowPlayingOpen } from "../state/overlays";
import { TitleBar, titleBarArea } from "../components/ui/title-bar";
import { NavControls, TrailingControls } from "../components/header";

/** The pages' shell. The sidebar folds away entirely (remembered across
 * runs); the player bar hides under Now Playing, which covers it. */
export function DesktopShell() {
  // Crossing a breakpoint (the sidebar opening or narrows, the bar
  // rearranging), what moves glides there.
  animateLayoutOn(() => `${sidebarWidth()}|${barSize()}`);
  return (
    <View class="flex-1" style={{ backgroundColor: token("background") }}>
      {/* The header's controls, after the name: Back/Forward and the
          sidebar toggle at the left, search at the right. */}
      <TitleBar brand>
        <View class="flex-1 flex-row items-center gap-1">
          <NavControls />
          <View class="flex-1" />
          <TrailingControls />
        </View>
      </TitleBar>
      <View
        class="flex-1 flex-row gap-2 px-2 pb-2"
        style={{ paddingTop: titleBarArea().height > 0 ? 0 : 8 }}
        onLayout={(e) => measureShell(e.bounds.size.width)}
      >
        <If when={sidebarShown()}>
          <View class="shrink-0" style={{ width: sidebarWidth() }}>
            <Sidebar compact={railSidebar()} />
          </View>
        </If>
        <View class="relative min-w-0 flex-1">
          <MainStack class="min-w-0 overflow-hidden flex-1" />
          {/* Not drawn under Now Playing, which covers it. The anchor is
              an explicit style: 各平台的 absolute + inset/base 类名的折叠
              并不一致（真机上 bottom-1 曾经只对 headless 生效，pill 贴到窗
              口底边），写死比解析得来的更牢。 */}
          <If when={!nowPlayingOpen()}>
            <View style={{ position: "absolute", left: 12, right: 12, bottom: 4 }}>
              <PlayerBar />
            </View>
          </If>
        </View>
      </View>
    </View>
  );
}
