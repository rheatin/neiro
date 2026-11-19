// The main content's navigation: one stack per root (首页、专辑、歌手 …),
// all kept alive — choosing another root in the sidebar only shows that
// stack, so a page you were on is still there when you come back (its
// list, its scroll, its filters), instead of being rebuilt from the top.
// Back and Forward move through the stack of the root on show; Forward
// re-opens what Back (or the system's back: Escape, Alt+Left, the mouse's
// back button) took off. The shape of the functions is spotish's `nav.ts`;
// the routes are the library's.

import { batch, effect, signal, untracked } from "nsh";
import { createNavStack, type NavStackState } from "nsh/base";

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

const ROOT_NAMES = new Set<Route["name"]>(ROOT_ROUTES.map((r) => r.name));

/** How many times the root on show was opened again (spotish 最新动效的
 * `reenter`：同一 root 被再选中，列表也重新进场一次；切到别的栏目再回来
 * 不算重开，也不算 back 回它）。 */
const rootOpenCount = signal(0);
export const rootOpens = () => rootOpenCount();

/** What the app's own change of a stack means for what is ahead: Forward
 * keeps the rest, opening a page or a root clears it. */
type Intent = { kind: "forward"; route: Route } | { kind: "clear" } | { kind: "keep" } | null;

/** One root's stack and what Back took off it. */
interface Stack {
  state: NavStackState<Route>;
  /** Routes Back took off, the next Forward last (read tracked). */
  ahead: () => Route[];
  setAhead: (routes: Route[]) => void;
  previous: readonly Route[];
  intent: Intent;
  trimTimer: ReturnType<typeof setTimeout> | null;
}

/** The stacks made so far, by root name (made on first visit, then kept). */
const stacks = new Map<Route["name"], Stack>();
/** The roots visited, in the order first visited (tracked): `MainStack`
 * renders one layer per root and shows the active one. */
const visitedRoots = signal<Route[]>([]);
/** The root on show (tracked). */
const activeKey = signal<Route["name"]>("home");

function makeStack(root: Route): Stack {
  const state = createNavStack<Route>([root]);
  const ahead = signal<Route[]>([]);
  const stack: Stack = {
    state,
    ahead: () => ahead(),
    setAhead: (routes) => ahead.set(routes),
    previous: state.routes(),
    intent: null,
    trimTimer: null,
  };
  // Hear every change of the stack, the system's back included: a pop puts
  // what it took off ahead; Forward keeps the rest; anything else clears it.
  effect(() => {
    const routes = state.routes();
    untracked(() => {
      const before = stack.previous;
      stack.previous = routes;
      const mine = stack.intent;
      stack.intent = null;
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
  return stack;
}

/** The stack of `root`, made now if this is its first visit. */
function stackFor(root: Route): Stack {
  let stack = stacks.get(root.name);
  if (!stack) {
    stack = makeStack(root);
    stacks.set(root.name, stack);
    visitedRoots.update((roots) => [...roots, root]);
  }
  return stack;
}

/** The roots visited, for `MainStack`'s layers (tracked). */
export const openedRoots = () => visitedRoots();

/** The stack `root` navigates in (for its `<NavStack>`). */
export const stackOf = (root: Route) => stackFor(root).state;

/** The root on show (tracked). */
export const activeRootName = () => activeKey();

/** The stack on show: the one Back, Forward and the sidebar act on. */
function activeStack(): Stack {
  return stacks.get(activeKey())!;
}

// Home is the stack the app starts in.
stackFor(HOME);

/** 首页's stack: the pieces (and the tests) that bind a `<NavStack>` to one
 * directly use this; `MainStack` shows all of them. */
export const nav = stacks.get("home")!.state;

/** How pages come and go for the change being made: the sidebar's choice
 * is a switch between places, Back, Forward and opening a card move through
 * the stack (the slide). Each page reads it as it is shown and as it leaves
 * (`AppPage`)。 */
const motion = signal<"slide" | "fade">("slide");
export const pageAnimation = () => motion();
let settle: ReturnType<typeof setTimeout> | null = null;

/** Opens `route` from the sidebar: a switch, not a step through the stack. */
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

function trimSoon(stack: Stack): void {
  if (stack.trimTimer !== null) clearTimeout(stack.trimTimer);
  stack.trimTimer = setTimeout(() => {
    stack.trimTimer = null;
    const routes = stack.state.routes();
    if (routes.length <= MAX_PAGES + 1) return;
    // The same route objects: the stack keeps the pages it still has.
    batch(() => {
      stack.intent = { kind: "keep" };
      stack.state.setRoutes([routes[0], ...routes.slice(routes.length - MAX_PAGES)]);
    });
  }, TRIM_AFTER_MS);
}

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

/** The page on top of the stack on show (tracked where it is read). */
export function currentRoute(): Route {
  const routes = activeStack().state.routes();
  return routes[routes.length - 1];
}

/** The root of the stack on show (tracked). */
export function currentRoot(): Route {
  return activeStack().state.routes()[0];
}

/** Opens `route`: a root shows its stack. A collection already in the
 * stack is gone back to (Back's motion, its page as it was); the next one
 * ahead is gone forward to; any other is pushed (Forward's motion). */
export function open(route: Route): void {
  if (ROOT_NAMES.has(route.name)) {
    openRoot(route);
    return;
  }
  const stack = activeStack();
  if (same(currentRoute(), route)) return;
  const routes = stack.state.routes();
  for (let i = routes.length - 2; i >= 0; i--) {
    if (same(routes[i], route)) {
      stack.state.pop(routes.length - 1 - i);
      return;
    }
  }
  const ahead = stack.ahead();
  if (same(ahead[ahead.length - 1], route)) {
    forward();
    return;
  }
  stack.intent = { kind: "clear" };
  stack.state.push(route);
  trimSoon(stack);
}

/** Shows `root`'s stack: the root it is at when it was never left deeper,
 * else back to its root — a tab goes to its section, not to a record you
 * had opened in it. The other stacks go back to their roots (their pages
 * stay, their detail pages do not) and nothing is ahead anywhere. */
export function openRoot(root: Route): void {
  const stack = stackFor(root);
  const key = root.name;
  const shown = activeKey() === key;
  if (shown) {
    // Chosen again: the list is entered anew (its wave), and a detail page
    // on top of it is gone back to.
    rootOpenCount.update((n) => n + 1);
    if (stack.state.routes().length > 1) {
      stack.intent = { kind: "clear" };
      stack.state.popToRoot();
    }
    return;
  }
  batch(() => {
    for (const other of stacks.values()) {
      if (other === stack) continue;
      other.intent = { kind: "clear" };
      const routes = other.state.routes();
      if (routes.length > 1) other.state.setRoutes([routes[0]]);
      other.setAhead([]);
      if (other.trimTimer !== null) {
        clearTimeout(other.trimTimer);
        other.trimTimer = null;
      }
    }
    if (stack.state.routes().length > 1) {
      stack.intent = { kind: "clear" };
      stack.state.setRoutes([stack.state.routes()[0]]);
    }
    stack.setAhead([]);
    activeKey.set(key);
  });
}

/** Whether Back has a page to go to (tracked). */
export const canBack = () => activeStack().state.routes().length > 1;

/** Whether Forward has a page to go to (tracked). */
export const canForward = () => activeStack().ahead().length > 0;

export function back(): void {
  const stack = activeStack();
  if (stack.state.routes().length > 1) stack.state.pop();
}

export function forward(): void {
  const stack = activeStack();
  const ahead = stack.ahead();
  const next = ahead[ahead.length - 1];
  if (!next) return;
  stack.setAhead(ahead.slice(0, -1));
  stack.intent = { kind: "forward", route: next };
  stack.state.push(next);
  trimSoon(stack);
}
