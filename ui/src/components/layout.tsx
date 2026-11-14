// Which shell the window gets (a desktop app keeps the desktop at any
// window size: narrow, its sidebar is a rail and its player bar keeps only
// what fits). Anything that is not a native desktop window — the web, a
// phone, a terminal — renders one honest page: the engine talks to the
// audio devices, and only the desktop window has them.

import { Text, View, platform, signal, windowSize, type JSX } from "nsh";
import { sidebarCollapsed } from "../state/overlays";

export type Layout = "desktop" | "unsupported";

/** The desktop sidebar's widths (a rail, and open), and the shell's gaps
 * around and between it and the pages. */
const RAIL_WIDTH = 72;
const SIDEBAR_WIDE = 274;
const PANEL_INSET = 2 * (8 + 1);
const SHELL_GAPS = 3 * 8;
/** Narrower than this, the desktop's sidebar is a rail. */
const RAIL = 1000;
/** Wider than this, track lists have room for an album column. */
const ALBUM_COLUMN = 1100;
/** With the pages narrower than this, the player bar is compact. */
const COMPACT_BAR = 760;
/** Narrower than this, it is stacked. */
const STACKED_BAR = 520;

/** The layout for the window now (tracked). A native desktop window is the
 * desktop; every other idiom this app does not pretend to support. */
export function layout(): Layout {
  if (platform.desktop && platform.os !== "web" && platform.os !== "ios" && platform.os !== "android") return "desktop";
  return "unsupported";
}

/** How the desktop's player bar is laid out (tracked). */
export const barSize = (): "full" | "compact" | "stacked" => {
  // The pages' width, which the bar floats over.
  const w = windowSize().width - sidebarWidth() - (sidebarShown() ? SHELL_GAPS : 2 * 8);
  return w < STACKED_BAR ? "stacked" : w < COMPACT_BAR ? "compact" : "full";
};

/** The shell's own width, measured. `windowSize()` reports the window on some
 * backends and the screen on others; a rail decided from the wrong one never
 * appears, and the panels squeeze instead. The shell measures itself, and this
 * is what the sidebar switches on. */
const shellWidth = signal(0);
export const measuredShellWidth = () => shellWidth();
export const measureShell = (width: number): void => {
  const w = Math.round(width);
  if (w > 0 && w !== shellWidth()) shellWidth.set(w);
};

/** Whether the desktop's sidebar is a rail of icons (tracked). */
export const railSidebar = () => (measuredShellWidth() || windowSize().width) < RAIL;

/** Whether the desktop's sidebar shows at all (tracked). */
export const sidebarShown = () => !sidebarCollapsed();

/** The desktop sidebar's width now, pt (tracked). */
export const sidebarWidth = () =>
  !sidebarShown() ? 0 : railSidebar() ? RAIL_WIDTH : SIDEBAR_WIDE;

/** Whether there is room for the album column of track lists (tracked). */
export const wideTracks = () => layout() === "desktop" && windowSize().width > ALBUM_COLUMN;

/** The room a page leaves at its end: the player floats over the bottom of
 * the pages, so their last rows scroll clear of it. */
export const pageEnd = () => (barSize() === "stacked" ? "pb-36" : "pb-28");

/** The one page every non-desktop idiom gets: honest, no fake UI. */
export function UnsupportedPage(): JSX.Element {
  return (
    <View class="flex-1 items-center justify-center bg-sidebar px-8">
      <View class="max-w-md gap-3 rounded-(--radius-panel) border border-border bg-card p-8">
        <Text class="text-xl font-bold text-foreground">Neiro 是桌面应用</Text>
        <Text class="text-sm text-muted-foreground">
          这里是{platform.idiom === "terminal" ? "终端" : "浏览器"}环境。Neiro 通过本机音频引擎播放文件，请用桌面窗口运行（nsh run）。
        </Text>
      </View>
    </View>
  );
}
