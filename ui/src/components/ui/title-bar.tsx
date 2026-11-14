// The window's title bar, drawn by the app (nsh.json hides the system's:
// hidden, 52 points high). One bar runs across the top of the window,
// clear of the window controls (Windows' caption buttons at the right),
// and shows the mark and the app's name, then the header's controls. The
// rest of the bar drags the window (double-click maximizes). In full
// screen the platform reports no title bar area, and the bar goes away.

import { Image, Text, token, View, type JSX } from "nsh";
import { isAvailable, titleBar } from "nsh/desktop";
import { layout } from "../layout";

/** Whether this window's title bar is the app's to draw. */
export const drawsTitleBar = isAvailable;

/** The title bar area the app draws (reactive): its height, and the width
 * the window controls take at its right end; all 0 when it draws none (in
 * full screen). For a view that places itself against the window controls
 * rather than below a `TitleBar`. */
export function titleBarArea(): { height: number; left: number; right: number } {
  if (!drawsTitleBar) return { height: 0, left: 0, right: 0 };
  return { height: titleBar().height, left: titleBar().left, right: titleBar().right };
}

/** Whether the window's title bar holds the header's controls (on the
 * desktops where the app draws its own; tracked, it goes in full
 * screen). */
export const controlsInTitleBar = () => layout() === "desktop" && titleBarArea().height > 0;

/** The mark: one harmonic sine line with an amber overtone node — the same
 * shape the app icon is made from (`icon.png`), drawn from one path so the
 * window and the installer cannot drift apart.
 *
 * The stroke is the theme's foreground and the node its accent, so the mark
 * reads on the dark and the light appearance instead of being pinned to one
 * grey. The plate behind it belongs to the icon, not to a title bar. */
const MARK_PATH =
  "M52 134C72 134 78 86 100 86C122 86 130 170 152 170C174 170 182 114 204 114";
/** The area the wave has swept: the curve closed along the chord between its
 * own ends, which is the region it displaced above and below its axis. The
 * line alone reads as a squiggle; the filled lobe reads as sound. */
const MARK_AREA = `${MARK_PATH} Z`;

function markSource(size: number, plate: boolean): string {
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="${size}" height="${size}" fill="none">` +
    (plate ? `<rect width="256" height="256" rx="56" fill="${token("card")}"/>` : "") +
    `<path d="${MARK_AREA}" fill="${token("chart-1")}" fill-opacity="0.38"/>` +
    `<path d="${MARK_PATH}" stroke="${token("foreground")}" stroke-width="13" stroke-linecap="round" stroke-linejoin="round"/>` +
    `<circle cx="152" cy="86" r="12" fill="${token("chart-1")}"/>` +
    `</svg>`;
  return `data:image/svg+xml,${encodeURIComponent(svg)}`;
}

export function Logo(props: { size?: number }) {
  const s = props.size ?? 18;
  return (
    <Image
      src={markSource(s, false)}
      accessibilityElementsHidden
      style={{ width: s, height: s }}
    />
  );
}

/** The title bar, as wide as the view it heads: the mark and the name
 * after the window controls, then `children` (the header's controls). It
 * draws while the window's title bar area is the app's. */
export function TitleBar(props: { brand?: boolean; children?: JSX.Element }) {
  const left = () => titleBarArea().left + (titleBarArea().left > 0 ? 16 : 14);
  return (
    <If when={titleBarArea().height > 0}>
      <View
        testID="title-bar"
        desktop={{ windowDrag: true }}
        class="shrink-0 flex-row items-center gap-2"
        style={{
          height: titleBarArea().height,
          paddingLeft: left(),
          paddingRight: titleBarArea().right + 14,
        }}
      >
        <If when={props.brand !== false}>
          <Logo size={18} />
          {/* A logotype, not a label: upper case, mono, spaced — the same
              lettering family the console's silkscreen uses. */}
          <Text class="mr-4 font-mono text-[12px] font-bold uppercase tracking-[0.3em] text-foreground">
            NEIRO
          </Text>
        </If>
        {props.children}
      </View>
    </If>
  );
}
