// Real cover art from the files' own embedded pictures: a path's cheap
// `read_artwork_list` first ("is there a picture at all"), then the first
// frame's bytes, cached as a `data:` URI. Base answers every artwork ask on
// one channel keyed by path (the last one asked), so the asks are
// serialized: one list, then at most one read of its bytes, then the next.
// Covers stay in an LRU of 512 paths.

import { signal, untracked } from "nsh";
import { engine } from "../backend/engine";
import type { ArtworkList } from "../backend/types";

/** The most covers kept in memory (the contract's cap). */
const CAPACITY = 512;

/** path → `data:…;base64,…`, or "" when the file has no picture. Remote
 * covers share this cache by their remote `id` (封面缓存接入现有
 * artwork LRU). */
const cache = new Map<string, string>();
/** The covers already asked for this run (never asked twice). */
const asked = new Set<string>();
const version = signal(0);

type CoverJob = { kind: "local"; path: string } | { kind: "remote"; id: string; key: string };

// The one artwork exchange in flight over Base's single-answer channel.
let listPath: string | null = null;
let pendingRemote: string | null = null;
let dataPath: string | null = null;
const queue: CoverJob[] = [];

/** A track's cover as a data URI, "" until it arrives (the component shows
 * its fallback glyph meanwhile). Reading it asks once per path, and stays
 * reactive: a later cache fill re-renders the cover. */
export function coverOf(path: string | undefined): string {
  if (!path) return "";
  if (!asked.has(path)) {
    asked.add(path);
    queue.push({ kind: "local", path });
    void Promise.resolve().then(pump);
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
    queue.push({ kind: "remote", id, key: coverArt });
    void Promise.resolve().then(pump);
  }
  version();
  return cache.get(coverArt) ?? "";
}

/** Feeds one answer into the cache; the LRU loses its oldest while over
 * capacity. */
function settle(path: string, uri: string): void {
  cache.delete(path);
  cache.set(path, uri);
  while (cache.size > CAPACITY) {
    const oldest = cache.keys().next().value;
    if (oldest === undefined) break;
    cache.delete(oldest);
  }
  untracked(() => version.update((n) => n + 1));
}

/** Runs the queue's head: a local list (then its first frame's bytes), or
 * one remote artwork ask. */
function pump(): void {
  if (listPath !== null || dataPath !== null || pendingRemote !== null || queue.length === 0) return;
  const job = queue.shift();
  if (job === undefined) return;
  if (job.kind === "local") {
    try {
      listPath = job.path;
      engine.readArtworkList(job.path);
    } catch (error) {
      console.warn("neiro: 读取封面失败:", String(error));
      listPath = null;
      settle(job.path, "");
      void Promise.resolve().then(pump);
    }
  } else {
    try {
      pendingRemote = job.key;
      engine.fetchRemoteArtwork(job.id);
    } catch (error) {
      console.warn("neiro: 读取远程封面失败:", String(error));
      pendingRemote = null;
      settle(job.key, "");
      void Promise.resolve().then(pump);
    }
  }
}

function onList({ pictures, path }: ArtworkList): void {
  if (path !== listPath) return;
  listPath = null;
  if (!pictures.length) {
    settle(path, "");
  } else {
    try {
      dataPath = path;
      engine.readArtworkBytes(path, 0);
    } catch (error) {
      console.warn("neiro: 读取封面失败:", String(error));
      dataPath = null;
      settle(path, "");
    }
  }
  void Promise.resolve().then(pump);
}

function onData({ path, mime, data }: { path: string; mime: string | null | undefined; data: string | null | undefined }): void {
  if (path !== dataPath) return;
  dataPath = null;
  settle(path, data ? `data:${mime || "image/jpeg"};base64,${data}` : "");
  void Promise.resolve().then(pump);
}

/** One remote artwork answered: data `null` legitimately settles the cover
 * empty (占位), only a *refusal* leaves the exchange hanging. The answer
 * is keyed by the job's own key, since the event carries no such name. */
function onRemoteArtwork(artwork: { id: string; mime: string | null | undefined; data: string | null | undefined } | null): void {
  const key = pendingRemote;
  pendingRemote = null;
  if (key === null) return;
  settle(key, artwork?.data ? `data:${artwork.mime || "image/jpeg"};base64,${artwork.data}` : "");
  void Promise.resolve().then(pump);
}

// The subscriptions live for the process (the engine is a singleton).
engine.subscribeArtworkList((list) => untracked(() => onList(list)));
engine.subscribeArtworkData((data) => untracked(() => onData({ path: data.path, mime: data.mime ?? null, data: data.data ?? null })));
engine.subscribeRemoteArtwork((artwork) => untracked(() => onRemoteArtwork(artwork)));
// A read Base refuses (an unreadable file, a vanished path) must not leave
// the exchange hanging: settle the path empty and keep the queue moving.
engine.subscribeEngineError((error) => {
  if (!/artwork|art/i.test(error.op)) return;
  const pending = listPath !== null ? listPath : dataPath;
  if (pending === null) return;
  if (listPath !== null) {
    listPath = null;
    settle(pending, "");
  } else {
    dataPath = null;
    settle(pending, "");
  }
  void Promise.resolve().then(pump);
});