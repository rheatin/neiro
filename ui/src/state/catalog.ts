// The library: the folders the user pointed at (remembered between runs),
// the scan that keeps the index up to date, and the three lists the pages
// read — albums, artists and the whole track index (paged: 500 rows per
// answer). Data loads lazily: a page asks when it shows, so nothing loads
// that was never opened.

import { cleanup, computed, effect, signal, Storage, untracked, type Accessor } from "nsh";
import { engine, registerTrackLookup, reportEngineFailure, TRACK_PAGE } from "../backend/engine";
import { albumKey, type Album, type Artist, type Track } from "../backend/types";
import { open } from "../nav";

const remoteTracksAccessor = signal<{ get: () => Track[] }>({ get: () => [] });
let remotePathResolver: ((path: string) => Track | undefined) | null = null;
export function registerRemoteCatalog(
  getTracks: () => Track[],
  resolvePath: (path: string) => Track | undefined,
): void {
  remoteTracksAccessor.set({ get: getTracks });
  remotePathResolver = resolvePath;
}

registerTrackLookup((path) => trackForPath(path));

// ---- The watched folders ---------------------------------------------------

const ROOTS_KEY = "library:roots";

/** The library's roots, in memory, saved to Storage when they change. */
export const roots = signal<string[]>([]);
/** Whether the library has been opened and its roots are known (the pages
 * tell 空曲库 and 没有打开 apart on it). */
export const rootsLoaded = signal(false);
/** Whether a scan is running, and how many tracks the last one added
 * (the 扫描中 line's count). */
export const scan = signal<{ running: boolean; lastAdded: number | null }>({ running: false, lastAdded: null });

/** Bumped when the index changed (a scan found or lost files): the pages
 * that already asked ask again. */
export const dataEpoch = signal(0);

type ListState = "idle" | "loading" | "error";
/** The track index has one state more: complete (nothing left to page). */
type TracksListState = "idle" | "loading" | "complete" | "error";

/** Connect-time seeding (after a retry too): watch the saved roots again,
 * turn the reconcile walk on, and rescan so the index is fresh. */
let seeded = false;
effect(() => {
  if (engine.health().status !== "ready" || seeded) return;
  seeded = true;
  void (async () => {
    try {
      const saved = await Storage.getItem(ROOTS_KEY);
      const parsed: unknown = saved ? JSON.parse(saved) : null;
      const paths = Array.isArray(parsed)
        ? parsed.filter((p): p is string => typeof p === "string" && p !== "")
        : [];
      roots.set(paths);
      rootsLoaded.set(true);
      for (const root of paths) {
        try {
          engine.watchFolder(root);
        } catch (error) {
          reportEngineFailure(error, `无法监视 ${root}`);
        }
      }
      try {
        engine.setReconcileInterval(600);
      } catch (error) {
        console.warn("neiro: 对账周期没有设置上:", String(error));
      }
      if (paths.length > 0) {
        ensureTracks();
        rescanAll();
      }
    } catch (error) {
      console.warn("neiro: 曲库根目录没有读回来:", String(error));
      rootsLoaded.set(true);
    }
  })();
});

/** Asks for a folder and adds it (watched, then scanned). */
export async function addRoot(): Promise<void> {
  let picked: string | null;
  try {
    picked = await engine.pickFolder();
  } catch (error) {
    reportEngineFailure(error, "打不开文件夹选择框");
    return;
  }
  if (!picked || roots().includes(picked)) return;
  const next = [...roots(), picked];
  roots.set(next);
  void Storage.setItem(ROOTS_KEY, JSON.stringify(next)).catch((error) =>
    console.warn("neiro: 根目录没有记住:", String(error)),
  );
  try {
    engine.watchFolder(picked);
  } catch (error) {
    reportEngineFailure(error, "无法监视文件夹");
  }
  rescanAll();
}

export function removeRoot(path: string): void {
  const next = roots().filter((root) => root !== path);
  roots.set(next);
  void Storage.setItem(ROOTS_KEY, JSON.stringify(next)).catch((error) =>
    console.warn("neiro: 根目录没有记住:", String(error)),
  );
  try {
    // Unwatching stops the watcher; the rows are what the lists read, so a
    // removed folder has to take them with it.
    engine.forgetFolder(path);
  } catch (error) {
    reportEngineFailure(error, "无法移除文件夹");
  }
  rescanAll();
}

/** Rereads every root (a watcher that cannot name what changed is answered
 * by walking it all again). */
export function rescanAll(): void {
  for (const root of roots()) {
    try {
      engine.scanFolder(root);
    } catch (error) {
      reportEngineFailure(error, "扫描失败");
    }
  }
}

function handleScan(event: { kind: string; added: string[]; modified: string[]; removed: string[]; rescan: boolean }): void {
  if (event.kind === "started") {
    scan.update((s) => ({ ...s, running: true }));
    return;
  }
  if (event.kind === "done") {
    const changed = event.added.length + event.modified.length + event.removed.length;
    scan.set({ running: false, lastAdded: changed });
    if (changed > 0) dataEpoch.update((n) => n + 1);
    return;
  }
  if (event.kind === "changed" && event.rescan) {
    // The watcher could not name what changed: the walk is the honest
    // answer.
    rescanAll();
  }
}
engine.subscribeScan((event) => untracked(() => handleScan(event)));

// A backend attempt answered with one of Base's own errors (op names the
// listing): the page's state turns to "error" and its 重试 asks again.
engine.subscribeEngineError((error) => {
  untracked(() => {
    if (error.op === "list_tracks" && tracksState() === "loading" && allTracks().length === 0) tracksState.set("error");
    if (error.op === "list_albums" && albumsState() === "loading") albumsState.set("error");
    if (error.op === "list_artists" && artistsState() === "loading") artistsState.set("error");
  });
});

// ---- The whole track index ---------------------------------------------------

// scavenging of raw listing (a root removal must shrink what the pages see:
// Base 的浏览索引不清，过滤层在 TSX 侧补上)。
const allTracks = signal<Track[]>([]);
function normalizePath(path: string): string {
  return path.replace(/\//g, "\\").toLowerCase();
}

function underRoot(path: string, root: string): boolean {
  const normalized = normalizePath(path);
  const base = normalizePath(root);
  return normalized === base || normalized.startsWith(base + "\\");
}

/** 在**任一**当前根之下（不区分大小写；Windows 路径的正斜杠等价）。
 * `rootsLoaded()` 为 false 时不过滤（曲库还没开门，诚实显示全部索引）。 */
function underRoots(path: string): boolean {
  if (!rootsLoaded()) return true;
  const normalized = normalizePath(path);
  return untracked(roots).some((root) => underRoot(normalized, root));
}

/** 在**指定**根之下（null = 不过滤 = 全部本地）。首页按来源 Tab 取。 */
function underGivenRoot(path: string, root: string | null): boolean {
  if (!rootsLoaded() || root === null) return true;
  return underRoot(normalizePath(path), root);
}

/** 引擎索引里的每一行，不按根过滤：收藏与曲单存的是 path，是用户的
 * 明确标记，不是浏览视图（根被移除后它仍应显示，并且能播）。 */
export const indexedTracks: Accessor<Track[]> = () => allTracks();

/** 曲库视图的可见集（根过滤层）——所有本地页面/分组从这里取。 */
export const visibleTracks: Accessor<Track[]> = computed(() =>
  rootsLoaded() ? allTracks().filter((t) => underRoots(t.path)) : allTracks(),
);

/** 首页来源 Tab 的本地根（由 sources.ts 注册；null = 全部本地）。 */
const homeRoot = signal<string | null>(null);
export function setHomeRoot(root: string | null): void {
  homeRoot.set(root);
}

/** The root the list is scoped to, or null for the whole library. */
export const scopedRoot = (): string | null => homeRoot();

/** 首页来源 Tab 的可见层：按 activeSource 的本地根过滤。 */
export const homeTracks: Accessor<Track[]> = computed(() => {
  const root = homeRoot();
  if (root === null) return visibleTracks();
  return allTracks().filter((t) => underGivenRoot(t.path, root));
});

export const tracksTotal = signal(0);
export const tracksState = signal<TracksListState>("idle");
/** Whether the index is whole (the cover-per-album maps exist). */
export const tracksComplete = (): boolean => tracksState() === "complete";

let loadingTracks = false;
let tracksWatchdog: ReturnType<typeof setTimeout> | null = null;

function clearTracksWatchdog(): void {
  if (tracksWatchdog !== null) {
    clearTimeout(tracksWatchdog);
    tracksWatchdog = null;
  }
}

function resetTracksWatchdog(): void {
  clearTracksWatchdog();
  tracksWatchdog = setTimeout(() => {
    if (loadingTracks && tracksState() === "loading") {
      console.warn("neiro: 曲库加载超时 (15s)，重置状态允许重试");
      loadingTracks = false;
      tracksState.set(allTracks().length > 0 ? "complete" : "error");
    }
  }, 15_000);
}

/** Loads the track index, one page at a time (the list shows rows as pages
 * land). Nothing happens while a load runs, and the loaded index stays. */
export function ensureTracks(): void {
  if (loadingTracks || tracksState() !== "idle") return;
  loadingTracks = true;
  allTracks.set([]);
  tracksTotal.set(0);
  tracksState.set("loading");
  requestPage(0);
}

/** Asks again after an error (重试), or after a scan (a fresh epoch). */
export function retryTracks(): void {
  clearTracksWatchdog();
  loadingTracks = false;
  tracksState.set("idle");
  ensureTracks();
}

function requestPage(offset: number): void {
  resetTracksWatchdog();
  try {
    engine.listTracks(offset, TRACK_PAGE);
  } catch (error) {
    clearTracksWatchdog();
    console.warn("neiro: list_tracks 没发出去:", String(error));
    if (offset === 0) tracksState.set("error");
    else tracksState.set("complete");
    loadingTracks = false;
  }
}

// Answers land one page at a time on `engine.tracksPage()`; a page in
// order (its offset is where the index stopped) is appended and actioned
// on; anything else is a stale answer, dropped.
effect(() => {
  const page = engine.tracksPage();
  untracked(() => {
    if (!page || tracksState() !== "loading" || loadingTracks === false) return;
    if (page.offset !== allTracks().length) return;
    allTracks.set([...allTracks(), ...page.tracks]);
    tracksTotal.set(page.total);
    if (allTracks().length >= page.total) {
      clearTracksWatchdog();
      tracksState.set("complete");
      loadingTracks = false;
    } else {
      requestPage(allTracks().length);
    }
  });
});

// ---- Albums and artists ------------------------------------------------------

export const albums: Accessor<Album[]> = engine.albums;
export const artists: Accessor<Artist[]> = engine.artists;
export const albumsState = signal<ListState>("idle");
export const artistsState = signal<ListState>("idle");

let loadingAlbums = false;
let loadingArtists = false;

export function ensureAlbums(): void {
  if (loadingAlbums || albumsState() !== "idle") return;
  loadingAlbums = true;
  albumsState.set("loading");
  try {
    engine.listAlbums();
  } catch (error) {
    console.warn("neiro: list_albums 没发出去:", String(error));
    albumsState.set("error");
    loadingAlbums = false;
  }
}

export function ensureArtists(): void {
  if (loadingArtists || artistsState() !== "idle") return;
  loadingArtists = true;
  artistsState.set("loading");
  try {
    engine.listArtists();
  } catch (error) {
    console.warn("neiro: list_artists 没发出去:", String(error));
    artistsState.set("error");
    loadingArtists = false;
  }
}

export function retryAlbums(): void {
  loadingAlbums = false;
  albumsState.set("idle");
  ensureAlbums();
}

export function retryArtists(): void {
  loadingArtists = false;
  artistsState.set("idle");
  ensureArtists();
}

// A loaded list ends its own wait; the engine answers once per ask, and a
// late subscriber to an already-loaded list just gets it.
effect(() => {
  if (engine.albums().length > 0) albumsState.set("idle");
});
effect(() => {
  if (engine.artists().length > 0) artistsState.set("idle");
});

// A scan that found or lost files resets every listing: the next ask
// re-requests (the pages' own effects make them load again).
let lastResetEpoch = 0;
effect(() => {
  const epoch = dataEpoch();
  if (epoch === lastResetEpoch) return;
  lastResetEpoch = epoch;
  untracked(() => {
    loadingTracks = false;
    loadingAlbums = false;
    loadingArtists = false;
    tracksState.set("idle");
    albumsState.set("idle");
    artistsState.set("idle");
  });
});

// ---- Search ----------------------------------------------------------------

export const searchQuery = signal("");

/** Milliseconds of quiet typing before a search runs (Base's match is
 * LIKE). */
const DEBOUNCE_MS = 280;

export const searchResults = signal<{ query: string; tracks: Track[] } | null>(null);
export const searchRunning = signal(false);

// The query asked for. The answer arrives as an event, so it is kept only
// while its query is still the one asked — a newer keystroke supersedes it.
let wantedQuery = "";
let debounceTimer: ReturnType<typeof setTimeout> | null = null;
effect(() => {
  const query = searchQuery().trim();
  if (debounceTimer !== null) clearTimeout(debounceTimer);
  debounceTimer = setTimeout(() => {
    debounceTimer = null;
    if (query === wantedQuery) return;
    wantedQuery = query;
    if (!query) {
      searchResults.set(null);
      searchRunning.set(false);
      return;
    }
    searchRunning.set(true);
    try {
      engine.searchLibrary(query, 50);
    } catch (error) {
      reportEngineFailure(error, "搜索失败");
      searchRunning.set(false);
    }
  }, DEBOUNCE_MS);
  cleanup(() => {
    if (debounceTimer !== null) clearTimeout(debounceTimer);
  });
});

function keepIfCurrent(result: unknown): void {
  const answer = result as { query?: string; tracks?: Track[] } | null;
  if (!answer || typeof answer.query !== "string" || !Array.isArray(answer.tracks)) return;
  if (answer.query !== wantedQuery) return;
  searchResults.set({ query: answer.query, tracks: answer.tracks });
  searchRunning.set(false);
}
effect(() => {
  keepIfCurrent(untracked(() => engine.search()));
});

// A search the engine never answers (a dropped bridge) stops its spinner
// empty, with no fake results.
effect(() => {
  if (!searchRunning()) return;
  const timer = setTimeout(() => {
    if (searchRunning()) searchRunning.set(false);
  }, DEBOUNCE_MS + 3000);
  cleanup(() => clearTimeout(timer));
});

/** The albums and artists the search page shows (the engine searches
 * tracks; the rest filters locally over the loaded lists). */
export const searchAlbums: Accessor<Album[]> = computed(() => {
  const query = wantedQueryOfSearch();
  if (!query) return [];
  const lower = query.toLowerCase();
  return engine
    .albums()
    .filter((album) => album.album.toLowerCase().includes(lower) || album.albumArtist.toLowerCase().includes(lower));
});

export const searchArtists: Accessor<Artist[]> = computed(() => {
  const query = wantedQueryOfSearch();
  if (!query) return [];
  const lower = query.toLowerCase();
  return engine.artists().filter((artist) => artist.artist.toLowerCase().includes(lower));
});

// The wanted query is not a signal (it is a plain variable the debounce
// writes), so the local filters above read it through the same signal the
// debounce does — reading `searchQuery` tracks the field itself.
function wantedQueryOfSearch(): string {
  return searchQuery().trim();
}

// ---- Groupings the album and artist pages read ------------------------------

/** Tracks grouped by album (the exact album/artist pair the index uses),
 * and by artist. Built once per index load. */
const groups = computed(() => {
  const albumMap = new Map<string, Track[]>();
  const artistMap = new Map<string, Track[]>();
  for (const track of visibleTracks()) {
    const normArtist = track.albumArtist || track.artist || "";
    const normAlbumKey = albumKey({ album: track.album, albumArtist: normArtist });
    const normBucket = albumMap.get(normAlbumKey);
    if (normBucket) normBucket.push(track);
    else albumMap.set(normAlbumKey, [track]);

    const rawAlbumKey = albumKey(track);
    if (rawAlbumKey !== normAlbumKey) {
      const rawBucket = albumMap.get(rawAlbumKey);
      if (rawBucket) rawBucket.push(track);
      else albumMap.set(rawAlbumKey, [track]);
    }

    const name = track.artist || track.albumArtist;
    const artistBucket = artistMap.get(name);
    if (artistBucket) artistBucket.push(track);
    else artistMap.set(name, [track]);
  }
  return { albumMap, artistMap, pathMap: new Map(visibleTracks().map((track) => [track.path, track] as [string, Track])) };
});

/** The tracks of one album, in index order ([] while the index is not
 * loaded or the album has none). */
export function tracksOfAlbum(album: string, albumArtist: string): Track[] {
  const map = groups().albumMap;
  const exact = map.get(albumKey({ album, albumArtist }));
  if (exact && exact.length > 0) return exact;
  const emptyArtist = map.get(albumKey({ album, albumArtist: "" }));
  if (emptyArtist && emptyArtist.length > 0) return emptyArtist;
  const local = visibleTracks().filter(
    (t) => t.album === album && (!albumArtist || t.albumArtist === albumArtist || t.artist === albumArtist),
  );
  if (local.length > 0) return local;
  return remoteTracksAccessor()
    .get()
    .filter((t) => t.album === album && (!albumArtist || t.albumArtist === albumArtist || t.artist === albumArtist));
}

/** The tracks of one artist, across their albums. */
export function tracksOfArtist(artist: string): Track[] {
  const exact = groups().artistMap.get(artist);
  if (exact && exact.length > 0) return exact;
  const local = visibleTracks().filter((t) => t.artist === artist || t.albumArtist === artist);
  if (local.length > 0) return local;
  return remoteTracksAccessor()
    .get()
    .filter((t) => t.artist === artist || t.albumArtist === artist);
}

/** A representative track of an album (whose embedded picture the cover
 * component asks for). */
export function albumCoverTrack(album: string, albumArtist: string): string {
  return tracksOfAlbum(album, albumArtist)[0]?.path ?? "";
}

export function artistCoverTrack(artist: string): string {
  return tracksOfArtist(artist)[0]?.path ?? "";
}

/** The track a queue path names ([]-safe: the queue drawer reads its own
 * playing titles too). */
export function trackForPath(path: string): Track | undefined {
  if (!path) return undefined;
  const fromGroup = groups().pathMap.get(path);
  if (fromGroup) return fromGroup;
  const fromAll = allTracks().find((t) => t.path === path);
  if (fromAll) return fromAll;
  if (remotePathResolver) {
    const r = remotePathResolver(path);
    if (r) return r;
  }
  if (untracked(tracksState) === "idle" && engine.health().status === "ready") {
    untracked(() => ensureTracks());
  }
  return undefined;
}

function buildMergedAlbums(map: Map<string, Track[]>): Album[] {
  const existing = engine.albums();
  const out: Album[] = [];
  const seen = new Set<string>();

  for (const alb of existing) {
    const key = albumKey(alb);
    const bucket = map.get(key);
    if (bucket) {
      seen.add(key);
      const resolvedArtist = alb.albumArtist || bucket[0]?.albumArtist || bucket[0]?.artist || "";
      seen.add(albumKey({ album: alb.album, albumArtist: resolvedArtist }));
      out.push({ ...alb, albumArtist: resolvedArtist, trackCount: bucket.length });
    }
  }

  for (const [key, bucket] of map.entries()) {
    if (seen.has(key)) continue;
    const first = bucket[0];
    if (!first || !first.album.trim()) continue;
    const resolvedArtist = first.albumArtist || first.artist || "";
    const normKey = albumKey({ album: first.album, albumArtist: resolvedArtist });
    if (seen.has(normKey)) continue;
    seen.add(key);
    seen.add(normKey);
    out.push({
      album: first.album,
      albumArtist: resolvedArtist,
      year: first.year || 0,
      trackCount: bucket.length,
    });
  }

  return out;
}

function buildMergedArtists(map: Map<string, Track[]>): Artist[] {
  const existing = engine.artists();
  const out: Artist[] = [];
  const seen = new Set<string>();

  for (const art of existing) {
    const bucket = map.get(art.artist);
    if (bucket) {
      seen.add(art.artist);
      const albumSet = new Set(bucket.map((t) => t.album).filter((a) => a.trim() !== ""));
      out.push({
        ...art,
        trackCount: bucket.length,
        albumCount: Math.max(art.albumCount, albumSet.size),
      });
    }
  }

  for (const [name, bucket] of map.entries()) {
    if (seen.has(name) || !name.trim()) continue;
    const albumSet = new Set(bucket.map((t) => t.album).filter((a) => a.trim() !== ""));
    out.push({
      artist: name,
      albumCount: albumSet.size,
      trackCount: bucket.length,
    });
  }

  return out;
}

/** 专辑/歌手过滤到当前根（曲 单页面/卡片的同一层）。 */
export const visibleAlbums: Accessor<Album[]> = computed(() => {
  return buildMergedAlbums(groups().albumMap);
});

/** 首页来源 Tab 的专辑（按 activeSource 的本地根过滤后的分组）。 */
const homeAlbumMap: Accessor<Map<string, Track[]>> = computed(() => {
  const albumMap = new Map<string, Track[]>();
  for (const track of homeTracks()) {
    const normArtist = track.albumArtist || track.artist || "";
    const normAlbumKey = albumKey({ album: track.album, albumArtist: normArtist });
    const normBucket = albumMap.get(normAlbumKey);
    if (normBucket) normBucket.push(track);
    else albumMap.set(normAlbumKey, [track]);

    const rawAlbumKey = albumKey(track);
    if (rawAlbumKey !== normAlbumKey) {
      const rawBucket = albumMap.get(rawAlbumKey);
      if (rawBucket) rawBucket.push(track);
      else albumMap.set(rawAlbumKey, [track]);
    }
  }
  return albumMap;
});

const homeArtistMap: Accessor<Map<string, Track[]>> = computed(() => {
  const artistMap = new Map<string, Track[]>();
  for (const track of homeTracks()) {
    const name = track.artist || track.albumArtist;
    const bucket = artistMap.get(name);
    if (bucket) bucket.push(track);
    else artistMap.set(name, [track]);
  }
  return artistMap;
});

export const homeAlbums: Accessor<Album[]> = computed(() => {
  return buildMergedAlbums(homeAlbumMap());
});

export const homeArtists: Accessor<Artist[]> = computed(() => {
  return buildMergedArtists(homeArtistMap());
});

export const visibleArtists: Accessor<Artist[]> = computed(() => {
  return buildMergedArtists(groups().artistMap);
});

// ---- The items the cards render ------------------------------------------------

/** A card, shaped like spotish `Item` over Base's fields. */
interface LibraryItemBase {
  name: string;
  subtitle: string;
  count: number;
}

export interface AlbumItem extends LibraryItemBase {
  kind: "album";
  album: string;
  albumArtist: string;
}

export interface ArtistItem extends LibraryItemBase {
  kind: "artist";
  artist: string;
}

export type LibraryItem = AlbumItem | ArtistItem;

export function albumItemOf(album: Album): AlbumItem {
  return {
    kind: "album",
    album: album.album,
    albumArtist: album.albumArtist,
    name: album.album || "未知专辑",
    subtitle: `${album.albumArtist || "未知歌手"}${album.year ? ` · ${album.year}` : ""}`,
    count: album.trackCount,
  };
}

export function artistItemOf(artist: Artist): ArtistItem {
  return {
    kind: "artist",
    artist: artist.artist,
    name: artist.artist || "未知歌手",
    subtitle: `${artist.albumCount} 张专辑`,
    count: artist.trackCount,
  };
}

/** The item's tracks (its play disc plays them as a queue). */
export function itemTracks(item: LibraryItem): Track[] {
  return item.kind === "album" ? tracksOfAlbum(item.album, item.albumArtist) : tracksOfArtist(item.artist);
}

/** Opens an item's page (a card's tap). */
export function openItem(item: LibraryItem): void {
  if (item.kind === "album") open({ name: "album", album: item.album, albumArtist: item.albumArtist });
  else open({ name: "artist", artist: item.artist });
}
