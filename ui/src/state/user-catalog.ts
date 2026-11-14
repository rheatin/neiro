// 曲库的用户层（v3）：红心、曲单、最常播放。状态由四个事件镜像进引擎的
// signals；命令由本文件发出（失败 = 一句白话 + 引擎原话的 toast）。红心
// 的状态读全量集合自查；`recordPlay(path, Date.now())` 同一 trackSerial
// 只记一次。

import { effect, untracked } from "nsh";
import { engine, reportEngineFailure } from "../backend/engine";
import { hasTrack, player } from "./player";
import { indexedTracks } from "./catalog";
import { asLocalTrack, remoteTrackForCachePath } from "./remote";
import type { MostPlayed, Playlist, PlaylistEntry, RemoteTrack, Track } from "../backend/types";

// 一次连接后：把曲单与收藏的**当前集合**取回来（事件只描述变化；不请求
// 就没有初始值——侧栏曲单曾因此永远为空）。
let listedUserData = false;
effect(() => {
  if (engine.health().status !== "ready" || listedUserData) return;
  listedUserData = true;
  untracked(() => {
    try {
      engine.listPlaylists();
      engine.listFavourites();
    } catch (error) {
      console.warn("neiro: 曲单/收藏没有请求:", String(error));
    }
  });
});

/** The paths marked liked, as the engine last said (tracked). */
export const favourites: () => string[] = () => engine.favourites().paths;

/** Whether one path is in the set (tracked; 收藏= liked 色实心）。 */
export function isFavourite(path: string | undefined): boolean {
  return !!path && favourites().includes(path);
}

/** Toggles a path's heart; the engine's own answer drives the state. */
export function toggleFavourite(path: string | undefined): void {
  if (!path || !hasTrack()) return;
  const on = !isFavourite(path);
  try {
    engine.setFavourite(path, on);
  } catch (error) {
    reportEngineFailure(error, on ? "收藏失败" : "取消收藏失败");
  }
}

/** The playlists, in the engine's (sorted) order (tracked). */
export const playlists: () => Playlist[] = () => engine.playlists();

export function createPlaylist(name: string): void {
  try {
    engine.createPlaylist(name);
  } catch (error) {
    reportEngineFailure(error, "新建曲单失败");
  }
}

export function renamePlaylist(id: number, name: string): void {
  try {
    engine.renamePlaylist(id, name);
  } catch (error) {
    reportEngineFailure(error, "重命名失败");
  }
}

export function deletePlaylist(id: number): void {
  try {
    engine.deletePlaylist(id);
  } catch (error) {
    reportEngineFailure(error, "删除曲单失败");
  }
}

export function addToPlaylist(id: number, path: string | undefined): void {
  if (!path) return;
  try {
    engine.addToPlaylist(id, path);
  } catch (error) {
    reportEngineFailure(error, "加入曲单失败");
  }
}

export function removeFromPlaylist(id: number, path: string): void {
  try {
    engine.removeFromPlaylist(id, path);
  } catch (error) {
    reportEngineFailure(error, "移除失败");
  }
}

/** Puts one playlist's entries on the engine's single-entries channel; the
 * pages read them through `playlistEntries`. */
export function loadPlaylistEntries(id: number): void {
  try {
    engine.listPlaylistEntries(id);
  } catch (error) {
    reportEngineFailure(error, "读取曲单失败");
  }
}

/** The entries of the playlist the engine last answered, in order. The
 * caller compares pages by id to only show the one asked. */
export function playlistEntries(id: number | null): PlaylistEntry[] {
  const page = engine.playlistEntries();
  if (!page || (id !== null && page.id !== id)) return [];
  return page.entries;
}

/** The most-played rows (the engine carries the count and the order). */
export const mostPlayed: () => MostPlayed[] = () => engine.mostPlayed();

/** Home pulls them itself, once per page entry (`listMostPlayed(12)`). */
export function refreshMostPlayed(limit = 12): void {
  try {
    engine.listMostPlayed(limit);
  } catch (error) {
    console.warn("neiro: 最常播放没有请求:", String(error));
  }
}

/** The tracks of a set of paths, in the paths' order. The whole index is
 * consulted, not the root-filtered view: a favourite or a playlist entry is
 * the user's own mark, and it keeps showing (and playing) after the folder
 * it came from was dropped from the library. A path the index does not know
 * is left out, and the page says how many of the set it could resolve. */
export function tracksForPaths(paths: string[]): Track[] {
  const map = new Map<string, Track>(indexedTracks().map((track) => [track.path, track] as [string, Track]));
  return paths.flatMap((path) => {
    const found = map.get(path);
    if (found) return [found];
    // A downloaded remote track is not in the local index (the engine's
    // scan never saw it): its facts come from the server's listing row.
    const remote = remoteTrackForCachePath(path);
    return remote ? [asLocalTrack(remote, path)] : [];
  });
}

// ---- recordPlay：进入 playing 一次一条 ----------------------------------------

// 同一 trackSerial 只记一次；引擎不造钟，这里也不重复计。
let lastRecordedSerial = -1;

effect(() => {
  const p = player();
  if (p.state !== "playing" || !p.path) return;
  if (p.trackSerial === lastRecordedSerial) return;
  lastRecordedSerial = p.trackSerial;
  untracked(() => {
    try {
      engine.recordPlay(p.path as string, Date.now());
    } catch (error) {
      console.warn("neiro: 播放没有记录:", String(error));
    }
  });
});
