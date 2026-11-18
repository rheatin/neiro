// Real cover art from the files' own embedded pictures and remote servers.
// Local and remote pipelines are completely decoupled:
// - Local: cheap read_artwork_list -> read_artwork_bytes -> data: URI
// - Remote: fetchRemoteArtwork -> remote_artwork -> data: URI
// Both pipelines are bounded by their own watchdogs so a dropped answer
// never hangs either pipeline.
// Covers stay in an LRU of 512 entries.

import { effect, signal, untracked } from "nsh";
import { engine } from "../backend/engine";
import type { ArtworkList } from "../backend/types";

/** The most covers kept in memory (the contract's cap). */
const CAPACITY = 512;

/** key → `data:…;base64,…`, or "" when the item has no picture. */
const cache = new Map<string, string>();
/** The covers already asked for this run (never asked twice). */
const asked = new Set<string>();
const version = signal(0);

// ---- Local Pipeline --------------------------------------------------------
const localQueue: string[] = [];
let listPath: string | null = null;
let dataPath: string | null = null;
let localTimer: ReturnType<typeof setTimeout> | null = null;

function clearLocalTimer(): void {
  if (localTimer !== null) {
    clearTimeout(localTimer);
    localTimer = null;
  }
}

function startLocalTimer(path: string): void {
  clearLocalTimer();
  localTimer = setTimeout(() => {
    localTimer = null;
    if (listPath === path || dataPath === path) {
      listPath = null;
      dataPath = null;
      settle(path, "");
      void Promise.resolve().then(pumpLocal);
    }
  }, 5000);
}

function pumpLocal(): void {
  if (listPath !== null || dataPath !== null || localQueue.length === 0) return;
  const path = localQueue.shift();
  if (path === undefined) return;
  try {
    listPath = path;
    startLocalTimer(path);
    engine.readArtworkList(path);
  } catch (error) {
    console.warn("neiro: 读取封面失败:", String(error));
    clearLocalTimer();
    listPath = null;
    settle(path, "");
    void Promise.resolve().then(pumpLocal);
  }
}

function onList({ pictures, path }: ArtworkList): void {
  if (path !== listPath) return;
  clearLocalTimer();
  listPath = null;
  if (!pictures.length) {
    settle(path, "");
    // A read file with no picture is an answer worth keeping: the next run
    // need not open it again.
    engine.coverCachePut(path, "none");
    void Promise.resolve().then(pumpLocal);
  } else {
    try {
      dataPath = path;
      startLocalTimer(path);
      engine.readArtworkBytes(path, 0);
    } catch (error) {
      console.warn("neiro: 读取封面失败:", String(error));
      clearLocalTimer();
      dataPath = null;
      settle(path, "");
      void Promise.resolve().then(pumpLocal);
    }
  }
}

function onData({
  path,
  mime,
  data,
}: {
  path: string;
  mime: string | null | undefined;
  data: string | null | undefined;
}): void {
  if (path !== dataPath) return;
  clearLocalTimer();
  dataPath = null;
  const uri = data ? `data:${mime || "image/jpeg"};base64,${data}` : "";
  settle(path, uri);
  engine.coverCachePut(path, uri === "" ? "none" : uri);
  void Promise.resolve().then(pumpLocal);
}

// ---- Remote Pipeline -------------------------------------------------------
interface RemoteJob {
  id: string;
  key: string;
}

const remoteQueue: RemoteJob[] = [];
let pendingRemote: RemoteJob | null = null;
let remoteTimer: ReturnType<typeof setTimeout> | null = null;

function clearRemoteTimer(): void {
  if (remoteTimer !== null) {
    clearTimeout(remoteTimer);
    remoteTimer = null;
  }
}

function startRemoteTimer(job: RemoteJob): void {
  clearRemoteTimer();
  remoteTimer = setTimeout(() => {
    remoteTimer = null;
    if (pendingRemote && pendingRemote.key === job.key) {
      const stale = pendingRemote;
      pendingRemote = null;
      settle(stale.key, "");
      void Promise.resolve().then(pumpRemote);
    }
  }, 8000);
}

function pumpRemote(): void {
  if (pendingRemote !== null || remoteQueue.length === 0) return;
  if (!engine.remoteStatus().configured) return;
  const job = remoteQueue.shift();
  if (job === undefined) return;
  try {
    pendingRemote = job;
    startRemoteTimer(job);
    engine.fetchRemoteArtwork(job.id);
  } catch (error) {
    console.warn("neiro: 读取远程封面失败:", String(error));
    clearRemoteTimer();
    pendingRemote = null;
    settle(job.key, "");
    void Promise.resolve().then(pumpRemote);
  }
}

// When remote connection completes, automatically drain queued remote covers!
effect(() => {
  if (engine.remoteStatus().configured) {
    untracked(() => pumpRemote());
  }
});

function onRemoteArtwork(
  artwork: { id: string; mime: string | null | undefined; data: string | null | undefined } | null,
): void {
  const job = pendingRemote;
  pendingRemote = null;
  clearRemoteTimer();
  if (job === null) return;
  settle(job.key, artwork?.data ? `data:${artwork.mime || "image/jpeg"};base64,${artwork.data}` : "");
  void Promise.resolve().then(pumpRemote);
}

// ---- Public Accessors ------------------------------------------------------

/** A track's cover as a data URI, "" until it arrives (the component shows
 * its fallback glyph meanwhile). Reading it asks once per path, and stays
 * reactive: a later cache fill re-renders the cover. */
export function coverOf(path: string | undefined): string {
  if (!path || path.startsWith("remote://")) return "";
  const known = cache.get(path);
  if (known !== undefined) {
    version();
    return known;
  }
  if (!asked.has(path)) {
    // A previous run's answer, if the file still is what it was (the key
    // carries its mtime and size): this skips reading the file and Base.
    const disk = engine.coverCacheGet(path);
    if (disk !== "") {
      asked.add(path);
      // Through `settle`, so a burst of disk hits still honours the memory
      // cap and a later eviction is re-readable from disk.
      settle(path, disk === "none" ? "" : disk);
      version();
      return cache.get(path) ?? "";
    }
    asked.add(path);
    localQueue.push(path);
    void Promise.resolve().then(pumpLocal);
  }
  version();
  return cache.get(path) ?? "";
}

/** A remote cover as a data URI, or "" (the row's glyph stands in). `data
 * === null` is "the server has no picture" — settled as "", not an
 * error. `id` is the track's server id, `coverArt` its name. */
export function remoteCoverOf(id: string, coverArt: string | null): string {
  if (!id || !coverArt) return "";
  if (!asked.has(coverArt)) {
    asked.add(coverArt);
    remoteQueue.push({ id, key: coverArt });
    void Promise.resolve().then(pumpRemote);
  }
  version();
  return cache.get(coverArt) ?? "";
}

/** Drops this run's remote/local cover answers and pending asks: a disk
 * clean must not leave deleted images on screen. */
export function clearArtworkMemory(): void {
  cache.clear();
  asked.clear();
  localQueue.length = 0;
  remoteQueue.length = 0;
  untracked(() => version.update((n) => n + 1));
}

/** Feeds one answer into the cache; the LRU loses its oldest while over
 * capacity. */
function settle(key: string, uri: string): void {
  cache.delete(key);
  cache.set(key, uri);
  while (cache.size > CAPACITY) {
    const oldest = cache.keys().next().value;
    if (oldest === undefined) break;
    cache.delete(oldest);
    asked.delete(oldest);
  }
  untracked(() => version.update((n) => n + 1));
}

// ---- Subscriptions ---------------------------------------------------------
engine.subscribeArtworkList((list) => untracked(() => onList(list)));
engine.subscribeArtworkData((data) =>
  untracked(() =>
    onData({
      path: data.path,
      mime: data.mime ?? null,
      data: data.data ?? null,
    }),
  ),
);
engine.subscribeRemoteArtwork((artwork) => untracked(() => onRemoteArtwork(artwork)));

engine.subscribeEngineError((error) => {
  if (/remote_artwork/i.test(error.op)) {
    if (pendingRemote !== null) {
      clearRemoteTimer();
      const job = pendingRemote;
      pendingRemote = null;
      if (!engine.remoteStatus().configured) {
        // Re-queue at the front so it is fetched once connection establishes
        remoteQueue.unshift(job);
      } else {
        settle(job.key, "");
      }
      void Promise.resolve().then(pumpRemote);
    }
    return;
  }
  if (/artwork|art/i.test(error.op)) {
    clearLocalTimer();
    const pending = listPath !== null ? listPath : dataPath;
    if (pending !== null) {
      listPath = null;
      dataPath = null;
      settle(pending, "");
      void Promise.resolve().then(pumpLocal);
    }
  }
});
