// The main content's navigation: one stack for every shell (desktop only).
// 首页、专辑、歌手、全部曲目 and 搜索 are roots (choosing one replaces the
// stack); an album's or artist's page is pushed on top. Back and Forward
// move through it; Forward re-opens what Back (or the system's back:
// Escape, Alt+Left, the mouse's back button) took off. The shape of the
// functions is spotish's `nav.ts`; the routes are the library's.

import { batch, effect, signal, untracked } from "nsh";
import { createNavStack } from "nsh/base";

/** A page of the main content. */
export type Route =
  | { name: "home" }
  | { name: "search" }
  | { name: "albums" }
  | { name: "artists" }
  | { name: "tracks" }
  | { name: "liked" }
  | { name: "playlists" }
  | { name: "eq" }
  | { name: "remote" }
  | { name: "settings" }
  | { name: "album"; album: string; albumArtist: string }
  | { name: "artist"; artist: string }
  | { name: "playlist"; id: number; playlist: string }
  | { name: "remotePlaylist"; id: string; playlist: string }
  | { name: "remoteAlbum"; serverId: string; album: string; artist: string }
  | { name: "remoteArtist"; serverId: string; artist: string };

/** The root routes: the same objects every time, so a stack that keeps one
 * keeps its page. */
export const HOME: Route = { name: "home" };
export const SEARCH: Route = { name: "search" };
export const ALBUMS: Route = { name: "albums" };
export const ARTISTS: Route = { name: "artists" };
export const TRACKS: Route = { name: "tracks" };
export const REMOTE: Route = { name: "remote" };
export const LIKED: Route = { name: "liked" };
export const PLAYLISTS: Route = { name: "playlists" };
export const SETTINGS: Route = { name: "settings" };

export const ROOT_ROUTES = [HOME, SEARCH, ALBUMS, ARTISTS, TRACKS, REMOTE, LIKED, PLAYLISTS, SETTINGS];

/** How many times a root was opened (spotish 最新动效的 `reenter`：同一页
 * 被重新选中，列表也重新进场一次；back 回到它不算）。 */
const rootOpenCount = signal(0);
export const rootOpens = () => rootOpenCount();

/** The stack (made once: it keeps its pages across hot updates). */
export const nav = createNavStack<Route>([HOME]);

/** How pages come and go for the change being made: the sidebar's choice
 * is a switch between places (a cross-fade), Back, Forward and opening a
 * card move through the stack (the slide). Each page reads it as it is
 * shown and as it leaves (`AppPage`). */
const motion = signal<"slide" | "fade">("slide");
export const pageAnimation = () => motion();
let settle: ReturnType<typeof setTimeout> | null = null;

/** Opens `route` from the sidebar: a switch, not a step through the
 * stack. */
export function switchTo(route: Route): void {
  if (settle !== null) clearTimeout(settle);
  motion.set("fade");
  open(route);
  settle = setTimeout(() => {
    settle = null;
    motion.set("slide");
  }, 400);
}

/** The most pages kept above the root. Each page in the stack stays alive
 * (its list, its views, its covers), so a stack that only grew would hold
 * every page visited: past this, the oldest go once the new page is in. */
export const MAX_PAGES = 4;
/** After a push's slide, so trimming the bottom does not disturb it. */
const TRIM_AFTER_MS = 600;
let trimTimer: ReturnType<typeof setTimeout> | null = null;

function trimSoon(): void {
  if (trimTimer !== null) clearTimeout(trimTimer);
  trimTimer = setTimeout(() => {
    trimTimer = null;
    const routes = nav.routes();
    if (routes.length <= MAX_PAGES + 1) return;
    // The same route objects: the stack keeps the pages it still has.
    batch(() => {
      intent = { kind: "keep" };
      nav.setRoutes([routes[0], ...routes.slice(routes.length - MAX_PAGES)]);
    });
  }, TRIM_AFTER_MS);
}

/** Routes Back took off, the next Forward last. */
const ahead = signal<Route[]>([]);
/** What the app's own change of the stack means for what is ahead: Forward
 * keeps the rest, opening a page or a root clears it. */
type Intent = { kind: "forward"; route: Route } | { kind: "clear" } | { kind: "keep" } | null;
let intent: Intent = null;
let previous: readonly Route[] = nav.routes();

// Hear every change of the stack, the system's back included: a pop puts
// what it took off ahead; Forward keeps the rest; anything else clears it.
effect(() => {
  const routes = nav.routes();
  untracked(() => {
    const before = previous;
    previous = routes;
    const mine = intent;
    intent = null;
    if (mine?.kind === "clear") {
      ahead.set([]);
      return;
    }
    if (mine?.kind === "keep") return;
    if (mine?.kind === "forward" && routes[routes.length - 1] === mine.route) return;
    const popped = routes.length < before.length && routes.every((r, i) => r === before[i]);
    if (popped) ahead.update((a) => [...a, ...before.slice(routes.length).reverse()]);
    else if (routes.length !== before.length || routes.some((r, i) => r !== before[i])) ahead.set([]);
  });
});

export function same(a: Route | undefined, b: Route): boolean {
  if (!a || a.name !== b.name) return false;
  switch (a.name) {
    case "home":
    case "search":
    case "albums":
    case "artists":
    case "tracks":
    case "liked":
    case "playlists":
    case "eq":
    case "remote":
    case "settings":
      return true;
    case "album":
      return a.album === (b as typeof a).album && a.albumArtist === (b as typeof a).albumArtist;
    case "artist":
      return a.artist === (b as typeof a).artist;
    case "playlist":
      return a.id === (b as typeof a).id;
    case "remotePlaylist":
      return a.id === (b as typeof a).id;
    case "remoteAlbum":
      return (
        a.serverId === (b as typeof a).serverId &&
        a.album === (b as typeof a).album &&
        a.artist === (b as typeof a).artist
      );
    case "remoteArtist":
      return a.serverId === (b as typeof a).serverId && a.artist === (b as typeof a).artist;
    default: {
      const _exhaustive: never = a;
      return _exhaustive;
    }
  }
}

export const isSameRoute = same;

/** The page on top (tracked where it is read). */
export function currentRoute(): Route {
  const routes = nav.routes();
  return routes[routes.length - 1];
}

/** The root the stack starts at (tracked). */
export function currentRoot(): Route {
  return nav.routes()[0];
}

/** Opens `route`: a root replaces the stack. A collection already in the
 * stack is gone back to (Back's motion, its page as it was); the next one
 * ahead is gone forward to; any other is pushed (Forward's motion). */
export function open(route: Route): void {
  const rootNames: Route["name"][] = ["home", "search", "albums", "artists", "tracks", "remote", "liked", "playlists", "settings"];
  if (rootNames.includes(route.name)) {
    openRoot(route);
    return;
  }
  if (same(currentRoute(), route)) return;
  const routes = nav.routes();
  for (let i = routes.length - 2; i >= 0; i--) {
    if (same(routes[i], route)) {
      nav.pop(routes.length - 1 - i);
      return;
    }
  }
  if (same(ahead()[ahead().length - 1], route)) {
    forward();
    return;
  }
  intent = { kind: "clear" };
  nav.push(route);
  trimSoon();
}

/** Makes `root` the stack's root: back to it when it already is, else the
 * stack replaced; nothing ahead. */
export function openRoot(root: Route): void {
  const routes = nav.routes();
  if (routes.length === 1 && same(routes[0], root)) {
    rootOpenCount.update((n) => n + 1);
    return;
  }
  rootOpenCount.update((n) => n + 1);
  intent = { kind: "clear" };
  if (same(routes[0], root)) nav.popToRoot();
  else nav.setRoutes([root]);
}

/** Whether Back has a page to go to (tracked). */
export const canBack = () => nav.routes().length > 1;

/** Whether Forward has a page to go to (tracked). */
export const canForward = () => ahead().length > 0;

export function back(): void {
  if (nav.routes().length > 1) nav.pop();
}

export function forward(): void {
  const next = ahead()[ahead().length - 1];
  if (!next) return;
  ahead.update((a) => a.slice(0, -1));
  intent = { kind: "forward", route: next };
  nav.push(next);
  trimSoon();
}
