// The library: the folders the user pointed at (remembered between runs),
// the scan that keeps the index up to date, and the three lists the pages
// read — albums, artists and the whole track index (paged: 500 rows per
// answer). Data loads lazily: a page asks when it shows, so nothing loads
// that was never opened.

import { cleanup, computed, effect, lifecycle, platform, signal, Storage, untracked, type Accessor } from "nsh";
import { engine, registerTrackLookup, reportEngineFailure, TRACK_PAGE } from "../backend/engine";
import {
  albumKey,
  type Album,
  type Artist,
  type FolderAccess,
  type PickedFolder,
  type Track,
} from "../backend/types";
import { open } from "../nav";

const remoteTracksAccessor = signal<{ get: () => Track[] }>({ get: () => [] });
const remoteLoadingAccessor = signal<{ state: () => "idle" | "loading" | "error" }>({
  state: () => "idle",
});
let sourceKindFn: (() => "all" | "local" | "remote") | null = null;
let remotePathResolver: ((path: string) => Track | undefined) | null = null;
export function registerRemoteCatalog(
  getTracks: () => Track[],
  resolvePath: (path: string) => Track | undefined,
  getLoadingState?: () => "idle" | "loading" | "error",
): void {
  remoteTracksAccessor.set({ get: getTracks });
  if (getLoadingState) remoteLoadingAccessor.set({ state: getLoadingState });
  remotePathResolver = resolvePath;
}

/** 来源类型由 sources.ts 注册（避免 catalog ↔ sources 循环依赖）。 */
export function registerSourceKind(fn: () => "all" | "local" | "remote"): void {
  sourceKindFn = fn;
}

function currentSourceKind(): "all" | "local" | "remote" {
  return sourceKindFn?.() ?? "all";
}

/** 已注册的远程目录映射成的曲库 Track 列表（「全部」包含所有服务器）。 */
export const activeRemoteTracks: Accessor<Track[]> = computed(() => remoteTracksAccessor().get());

registerTrackLookup((path) => trackForPath(path));

// ---- The watched folders ---------------------------------------------------

const ROOTS_KEY = "library:roots";

/** A folder the library keeps. iOS stores the bookmark and resolves its
 * path every launch (a folder's path changes with the app container's
 * UUID); the desktop stores the path itself, and its id is the path.
 * `path` is memory-only: `null` when the folder cannot be reached now (a
 * stale bookmark, a folder that moved) and the settings row asks to pick it
 * again. */
export interface LibraryRoot {
  id: string;
  bookmark: string;
  label: string;
  path: string | null;
}

/** The folder records, in memory, saved to Storage when they change. */
export const rootRecords = signal<LibraryRoot[]>([]);
/** Roots that need picking again (a stale bookmark, a folder that moved). */
export const staleRoots: Accessor<LibraryRoot[]> = computed(() =>
  rootRecords().filter((record) => record.path === null),
);

/** The library's roots as paths, in memory (what the engine and the root
 * filters take). Never persisted on iOS: `rootRecords` holds those. */
export const roots = signal<string[]>([]);

/** iOS keeps folders by bookmark; the desktop's paths survive on their own. */
function isIOS(): boolean {
  return platform.os === "ios";
}

function basename(path: string): string {
  return path.replace(/\\/g, "/").replace(/\/+$/, "").split("/").pop() || path;
}

function newRootId(): string {
  try {
    return crypto.randomUUID();
  } catch {
    return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
  }
}

/** Records become the two views the rest of the app reads: the records
 * themselves, and the paths of those that resolved. */
function applyRecords(records: LibraryRoot[]): void {
  rootRecords.set(records);
  roots.set(records.flatMap((record) => (record.path === null ? [] : [record.path])));
}

/** What goes to Storage: iOS's records without their resolved paths, or the
 * desktop's paths as they always were. */
function saveRoots(): void {
  const text = isIOS()
    ? JSON.stringify(rootRecords().map(({ id, bookmark, label }) => ({ id, bookmark, label })))
    : JSON.stringify(roots());
  void Storage.setItem(ROOTS_KEY, text).catch((error) =>
    console.warn("neiro: 根目录没有记住:", String(error)),
  );
}

/** Storage's two shapes: paths (the desktop's, and every earlier build) and
 * `{id, bookmark, label}` records (this build's iOS). */
function parseStored(raw: unknown): { paths: string[]; records: LibraryRoot[] } {
  const paths: string[] = [];
  const records: LibraryRoot[] = [];
  if (!Array.isArray(raw)) return { paths, records };
  for (const item of raw) {
    if (typeof item === "string") {
      if (item !== "") paths.push(item);
      continue;
    }
    if (typeof item !== "object" || item === null) continue;
    const record = item as Partial<LibraryRoot>;
    if (typeof record.bookmark !== "string" || record.bookmark === "") continue;
    records.push({
      id: typeof record.id === "string" && record.id !== "" ? record.id : record.bookmark,
      bookmark: record.bookmark,
      label:
        typeof record.label === "string" && record.label !== ""
          ? record.label
          : basename(record.bookmark),
      path: null,
    });
  }
  return { paths, records };
}
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

/** Resolves stored bookmarks for this launch and opens the app's access to
 * each folder (the core reads from its own threads). A bookmark iOS calls
 * stale comes back fresh and is written over the stored one; one that does
 * not resolve stays as a stale record for the settings row. */
async function adoptBookmarks(records: LibraryRoot[]): Promise<void> {
  let changed = false;
  const resolved: LibraryRoot[] = [];
  for (const record of records) {
    let access: FolderAccess | null = null;
    try {
      access = engine.resolveFolderBookmark(record.bookmark);
    } catch (error) {
      reportEngineFailure(error, `无法读取文件夹 ${record.label}`);
    }
    if (access === null) {
      resolved.push({ ...record, path: null });
      continue;
    }
    if (access.bookmark !== record.bookmark) changed = true;
    resolved.push({
      id: record.id,
      bookmark: access.bookmark,
      label: record.label,
      path: access.path,
    });
  }
  applyRecords(resolved);
  if (changed) saveRoots();
  if (roots().length > 0) {
    ensureTracks();
    rescanAll();
  }
}

/** Connect-time seeding (after a retry too): iOS resolves the saved
 * bookmarks (there is no watcher); the desktop watches its paths again and
 * turns the reconcile walk on. Either way the index is rescanned. */
let seeded = false;
effect(() => {
  if (engine.health().status !== "ready" || seeded) return;
  seeded = true;
  void (async () => {
    try {
      const saved = await Storage.getItem(ROOTS_KEY);
      const parsed: unknown = saved ? JSON.parse(saved) : null;
      const stored = parseStored(parsed);
      if (isIOS()) {
        await adoptBookmarks(stored.records);
        // Paths saved by an iOS build that had no bookmarks cannot be
        // resolved again; they stay visible as 需要重新选择, and picking the
        // folder again replaces them.
        if (stored.paths.length > 0) {
          applyRecords([
            ...untracked(rootRecords),
            ...stored.paths.map((path) => ({
              id: path,
              bookmark: path,
              label: basename(path),
              path: null,
            })),
          ]);
        }
      } else {
        const paths = [...stored.paths, ...stored.records.map((record) => record.bookmark)];
        applyRecords(
          paths.map((path) => ({ id: path, bookmark: path, label: basename(path), path })),
        );
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
      }
      rootsLoaded.set(true);
    } catch (error) {
      console.warn("neiro: 曲库根目录没有读回来:", String(error));
      rootsLoaded.set(true);
    }
  })();
});

/** iOS has no filesystem watcher: the scan on launch and the one when the app
 * comes back are what keep the folders honest. */
{
  let was = lifecycle();
  effect(() => {
    const now = lifecycle();
    const cameBack = now === "active" && was !== "active";
    was = now;
    if (!isIOS() || !cameBack) return;
    if (untracked(rootsLoaded) && untracked(roots).length > 0) rescanAll();
  });
}

/** Resolves a bookmark the picker just returned, so the app's access is
 * open before the engine walks the folder. */
function accessOf(bookmark: string): FolderAccess | null {
  try {
    return engine.resolveFolderBookmark(bookmark);
  } catch (error) {
    reportEngineFailure(error, "无法读取选中的文件夹");
    return null;
  }
}

/** iOS: the picker's folder becomes a record keyed by a UUID; the path is
 * kept in memory only. */
async function addBookmarkedRoot(): Promise<LibraryRoot | null> {
  let picked: PickedFolder | null;
  try {
    picked = await engine.pickFolderBookmark();
  } catch (error) {
    reportEngineFailure(error, "打不开文件夹选择框");
    return null;
  }
  if (!picked) return null;
  const access = accessOf(picked.bookmark);
  const path = access?.path ?? picked.path;
  const bookmark = access?.bookmark ?? picked.bookmark;
  const duplicate = untracked(rootRecords).find(
    (record) => record.bookmark === bookmark || record.path === path,
  );
  if (duplicate) return duplicate;
  const record: LibraryRoot = {
    id: newRootId(),
    bookmark,
    label: picked.label || basename(path),
    path,
  };
  applyRecords([...rootRecords(), record]);
  saveRoots();
  rescanAll();
  return record;
}

/** Asks for a folder and adds it (watched, then scanned; iOS keeps it by
 * bookmark). Returns the added root, or null when the user cancelled. */
export async function addRoot(): Promise<LibraryRoot | null> {
  if (isIOS()) return addBookmarkedRoot();
  let picked: string | null;
  try {
    picked = await engine.pickFolder();
  } catch (error) {
    reportEngineFailure(error, "打不开文件夹选择框");
    return null;
  }
  if (!picked || roots().includes(picked)) return null;
  const record: LibraryRoot = { id: picked, bookmark: picked, label: basename(picked), path: picked };
  applyRecords([...rootRecords(), record]);
  saveRoots();
  try {
    engine.watchFolder(picked);
  } catch (error) {
    reportEngineFailure(error, "无法监视文件夹");
  }
  rescanAll();
  return record;
}

/** Picks the stale root `id` again and puts the new bookmark in its place. */
export async function repairRoot(id: string): Promise<void> {
  const record = untracked(rootRecords).find((row) => row.id === id);
  if (!record || record.path !== null) return;
  let picked: PickedFolder | null;
  try {
    picked = await engine.pickFolderBookmark();
  } catch (error) {
    reportEngineFailure(error, "打不开文件夹选择框");
    return;
  }
  if (!picked) return;
  const access = accessOf(picked.bookmark);
  const path = access?.path ?? picked.path;
  const bookmark = access?.bookmark ?? picked.bookmark;
  applyRecords(
    rootRecords().map((row) =>
      row.id === id
        ? { ...row, bookmark, label: picked?.label || row.label, path }
        : row,
    ),
  );
  saveRoots();
  rescanAll();
}

/** Removes a root by its path (the desktop's callers hold paths; iOS's
 * settings rows hold ids and call `removeRootById`). */
export function removeRoot(path: string): void {
  const record = untracked(rootRecords).find(
    (row) => row.path === path || row.id === path,
  );
  if (!record) return;
  removeRootRecord(record);
}

/** Removes the root `id` (a stale one has no path to pass). */
export function removeRootById(id: string): void {
  const record = untracked(rootRecords).find((row) => row.id === id);
  if (!record) return;
  removeRootRecord(record);
}

function removeRootRecord(record: LibraryRoot): void {
  applyRecords(rootRecords().filter((row) => row.id !== record.id));
  saveRoots();
  if (record.path !== null) {
    try {
      // The rows are what the lists read, so a removed folder has to take
      // them with it (this also stops the desktop's watcher).
      engine.forgetFolder(record.path);
    } catch (error) {
      reportEngineFailure(error, "无法移除文件夹");
    }
  }
  if (isIOS() && record.bookmark !== record.path) {
    try {
      engine.releaseFolderBookmark(record.bookmark);
    } catch (error) {
      // An engine that cannot answer holds no access to release.
      console.warn("neiro: 文件夹访问没有释放:", String(error));
    }
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

/** 在**指定**根之下（null = 不按本地根过滤）。首页按来源 Tab 取。 */
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

/** 「全部」= 本地曲库 + 所有已配置远程服务器的实时或缓存列表。 */
export const allLibraryTracks: Accessor<Track[]> = computed(() => {
  if (currentSourceKind() !== "all") return visibleTracks();
  const merged = new Map<string, Track>();
  for (const track of [...visibleTracks(), ...activeRemoteTracks()]) merged.set(track.path, track);
  return [...merged.values()];
});

/** 首页来源 Tab 的本地根（由 sources.ts 注册；null = 不按本地根过滤）。 */
const homeRoot = signal<string | null>(null);
export function setHomeRoot(root: string | null): void {
  homeRoot.set(root);
}

/** The root the list is scoped to, or null for the whole library. */
export const scopedRoot = (): string | null => homeRoot();

/** 首页来源 Tab 的可见层：按 activeSource 的本地根过滤。 */
export const homeTracks: Accessor<Track[]> = computed(() => {
  const root = homeRoot();
  if (currentSourceKind() === "all") return allLibraryTracks();
  if (root === null) return visibleTracks();
  return allTracks().filter((t) => underGivenRoot(t.path, root));
});

/** 「全部」模式下远程列表的加载状态；未注册远程目录时 idle。 */
export const activeRemoteState: Accessor<"idle" | "loading" | "error"> = () =>
  remoteLoadingAccessor().state();

export const tracksTotal = signal(0);
export const tracksState = signal<TracksListState>("idle");
/** Whether the index is whole (the cover-per-album maps exist). */
export const tracksComplete = (): boolean => tracksState() === "complete";

/** 只给测试注入曲目数据（无头测试模拟曲库） */
export function setTracksForTest(tracks: Track[], testRoots?: string[]): void {
  if (testRoots) {
    applyRecords(
      testRoots.map((root) => ({ id: root, bookmark: root, label: basename(root), path: root })),
    );
    rootsLoaded.set(true);
  } else if (roots().length === 0) {
    applyRecords([{ id: "/music", bookmark: "/music", label: "music", path: "/music" }]);
    rootsLoaded.set(true);
  }
  allTracks.set(tracks);
  tracksTotal.set(tracks.length);
  tracksState.set("complete");
}

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
  for (const track of allLibraryTracks()) {
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
  return { albumMap, artistMap, pathMap: new Map(allLibraryTracks().map((track) => [track.path, track] as [string, Track])) };
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
