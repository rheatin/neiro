// 曲库展示与排序首选项 (Single-Owner Store)
// 包含曲目、专辑、歌手的排序字段、升降序、格式/红心胶囊过滤、密度与视图模式。
// 全部持久化至 Storage，并提供纯函数计算选择器供本地多根目录与远程挂载库共享。

import { effect, signal, Storage } from "nsh";
import type { Album, Artist, RemoteTrack, Track } from "../backend/types";
import { isFavourite } from "./user-catalog";
import { isLossless } from "../components/ui/format-badges";

// ---- 曲目偏好 ---------------------------------------------------------------

export type TrackSortField = "default" | "title" | "artist" | "album" | "duration" | "rate" | "year";
export type SortOrder = "asc" | "desc";
export type TrackFilterPill = "all" | "hires" | "dsd" | "lossless" | "favourite";

export const trackSortField = signal<TrackSortField>("default");
export const trackSortOrder = signal<SortOrder>("asc");
export const trackFilterPill = signal<TrackFilterPill>("all");
export const trackFilterQuery = signal<string>("");

export function cycleTrackSort(field: TrackSortField): void {
  if (trackSortField() === field) {
    if (trackSortOrder() === "asc") {
      trackSortOrder.set("desc");
    } else {
      trackSortField.set("default");
      trackSortOrder.set("asc");
    }
  } else {
    trackSortField.set(field);
    trackSortOrder.set("asc");
  }
}

// ---- 专辑偏好 ---------------------------------------------------------------

export type AlbumSortField = "default" | "album" | "artist" | "year" | "trackCount";
export type AlbumDensity = "standard" | "compact";

export const albumSortField = signal<AlbumSortField>("default");
export const albumSortOrder = signal<SortOrder>("asc");
export const albumDensity = signal<AlbumDensity>("standard");
export const albumFilterQuery = signal<string>("");

export function cycleAlbumSort(field: AlbumSortField): void {
  if (albumSortField() === field) {
    if (albumSortOrder() === "asc") {
      albumSortOrder.set("desc");
    } else {
      albumSortField.set("default");
      albumSortOrder.set("asc");
    }
  } else {
    albumSortField.set(field);
    albumSortOrder.set("asc");
  }
}

// ---- 歌手偏好 ---------------------------------------------------------------

export type ArtistSortField = "artist" | "trackCount" | "albumCount";
export type ArtistViewMode = "grid" | "list";

export const artistSortField = signal<ArtistSortField>("artist");
export const artistSortOrder = signal<SortOrder>("asc");
export const artistViewMode = signal<ArtistViewMode>("grid");
export const artistFilterQuery = signal<string>("");

export function cycleArtistSort(field: ArtistSortField): void {
  if (artistSortField() === field) {
    trackSortOrder.update((o) => (o === "asc" ? "desc" : "asc"));
    artistSortOrder.update((o) => (o === "asc" ? "desc" : "asc"));
  } else {
    artistSortField.set(field);
    artistSortOrder.set("asc");
  }
}

// ---- 持久化 -----------------------------------------------------------------

const STORAGE_KEY = "neiro:library-prefs:v1";

interface StoredPrefs {
  trackSortField?: TrackSortField;
  trackSortOrder?: SortOrder;
  trackFilterPill?: TrackFilterPill;
  albumSortField?: AlbumSortField;
  albumSortOrder?: SortOrder;
  albumDensity?: AlbumDensity;
  artistSortField?: ArtistSortField;
  artistSortOrder?: SortOrder;
  artistViewMode?: ArtistViewMode;
}

let seeded = false;
effect(() => {
  if (seeded) return;
  seeded = true;
  void (async () => {
    try {
      const raw = await Storage.getItem(STORAGE_KEY);
      if (!raw) return;
      const data = JSON.parse(raw) as StoredPrefs;
      if (data.trackSortField) trackSortField.set(data.trackSortField);
      if (data.trackSortOrder) trackSortOrder.set(data.trackSortOrder);
      if (data.trackFilterPill) trackFilterPill.set(data.trackFilterPill);
      if (data.albumSortField) albumSortField.set(data.albumSortField);
      if (data.albumSortOrder) albumSortOrder.set(data.albumSortOrder);
      if (data.albumDensity) albumDensity.set(data.albumDensity);
      if (data.artistSortField) artistSortField.set(data.artistSortField);
      if (data.artistSortOrder) artistSortOrder.set(data.artistSortOrder);
      if (data.artistViewMode) artistViewMode.set(data.artistViewMode);
    } catch {}
  })();
});

function savePrefs(): void {
  const data: StoredPrefs = {
    trackSortField: trackSortField(),
    trackSortOrder: trackSortOrder(),
    trackFilterPill: trackFilterPill(),
    albumSortField: albumSortField(),
    albumSortOrder: albumSortOrder(),
    albumDensity: albumDensity(),
    artistSortField: artistSortField(),
    artistSortOrder: artistSortOrder(),
    artistViewMode: artistViewMode(),
  };
  void Storage.setItem(STORAGE_KEY, JSON.stringify(data)).catch(() => {});
}

effect(() => {
  trackSortField();
  trackSortOrder();
  trackFilterPill();
  albumSortField();
  albumSortOrder();
  albumDensity();
  artistSortField();
  artistSortOrder();
  artistViewMode();
  if (seeded) savePrefs();
});

// ---- 纯函数计算选择器 -------------------------------------------------------

export function isDsdTrack(t: { path?: string; sampleRate?: number }): boolean {
  const p = (t.path ?? "").toLowerCase();
  return p.endsWith(".dsf") || p.endsWith(".dff") || p.endsWith(".iso") || (t.sampleRate ?? 0) >= 2_822_400;
}

export function isHiResTrack(t: { path?: string; sampleRate?: number; bits?: number }): boolean {
  if (isDsdTrack(t)) return false;
  const rate = t.sampleRate ?? 0;
  const bits = t.bits ?? 0;
  return (bits >= 24 && rate >= 88_200) || rate >= 176_400;
}

export function isLosslessAudio(t: { path?: string; bits?: number }): boolean {
  const p = (t.path ?? "").toLowerCase();
  if (isLossless(p)) return true;
  return (
    p.endsWith(".flac") ||
    p.endsWith(".alac") ||
    p.endsWith(".wav") ||
    p.endsWith(".wave") ||
    p.endsWith(".ape") ||
    p.endsWith(".wv") ||
    p.endsWith(".dsf") ||
    p.endsWith(".dff") ||
    p.endsWith(".aif") ||
    p.endsWith(".aiff")
  );
}

export function sortAndFilterTracks(
  tracks: Track[],
  options?: {
    sortField?: TrackSortField;
    sortOrder?: SortOrder;
    filterPill?: TrackFilterPill;
    query?: string;
  }
): Track[] {
  const field = options?.sortField ?? trackSortField();
  const order = options?.sortOrder ?? trackSortOrder();
  const pill = options?.filterPill ?? trackFilterPill();
  const q = (options?.query ?? trackFilterQuery()).trim().toLowerCase();

  let list = tracks;

  // 1. Text Search Filter
  if (q) {
    list = list.filter(
      (t) =>
        t.title.toLowerCase().includes(q) ||
        t.artist.toLowerCase().includes(q) ||
        t.album.toLowerCase().includes(q) ||
        t.path.toLowerCase().includes(q)
    );
  }

  // 2. Hi-Fi / Favourite Pill Filter
  if (pill !== "all") {
    switch (pill) {
      case "hires":
        list = list.filter(isHiResTrack);
        break;
      case "dsd":
        list = list.filter(isDsdTrack);
        break;
      case "lossless":
        list = list.filter(isLosslessAudio);
        break;
      case "favourite":
        list = list.filter((t) => isFavourite(t.path));
        break;
    }
  }

  // 3. Sorting
  if (field === "default") {
    return list;
  }

  const sorted = [...list].sort((a, b) => {
    let cmp = 0;
    switch (field) {
      case "title":
        cmp = a.title.localeCompare(b.title, undefined, { sensitivity: "base" });
        break;
      case "artist":
        cmp = a.artist.localeCompare(b.artist, undefined, { sensitivity: "base" });
        break;
      case "album":
        cmp = a.album.localeCompare(b.album, undefined, { sensitivity: "base" });
        break;
      case "duration":
        cmp = a.durationMs - b.durationMs;
        break;
      case "rate":
        cmp = a.sampleRate - b.sampleRate;
        break;
      case "year":
        cmp = (a.year || 0) - (b.year || 0);
        break;
    }
    return order === "desc" ? -cmp : cmp;
  });

  return sorted;
}

export function sortAndFilterRemoteTracks(
  tracks: RemoteTrack[],
  options?: {
    sortField?: TrackSortField;
    sortOrder?: SortOrder;
    filterPill?: TrackFilterPill;
    query?: string;
  }
): RemoteTrack[] {
  const field = options?.sortField ?? trackSortField();
  const order = options?.sortOrder ?? trackSortOrder();
  const pill = options?.filterPill ?? trackFilterPill();
  const q = (options?.query ?? trackFilterQuery()).trim().toLowerCase();

  let list = tracks;

  if (q) {
    list = list.filter(
      (t) =>
        t.title.toLowerCase().includes(q) ||
        t.artist.toLowerCase().includes(q) ||
        t.album.toLowerCase().includes(q) ||
        t.container.toLowerCase().includes(q)
    );
  }

  if (pill !== "all") {
    switch (pill) {
      case "hires":
      case "lossless": {
        const lossy = ["mp3", "aac", "m4a", "ogg", "opus", "wma"];
        list = list.filter((t) => !lossy.includes((t.container ?? "").toLowerCase()));
        break;
      }
      case "dsd": {
        const dsd = ["dsf", "dff", "iso"];
        list = list.filter((t) => dsd.includes((t.container ?? "").toLowerCase()));
        break;
      }
      case "favourite":
        list = list.filter((t) => isFavourite(`remote://${t.id}`));
        break;
    }
  }

  if (field === "default") {
    return list;
  }

  const sorted = [...list].sort((a, b) => {
    let cmp = 0;
    switch (field) {
      case "title":
        cmp = a.title.localeCompare(b.title, undefined, { sensitivity: "base" });
        break;
      case "artist":
        cmp = a.artist.localeCompare(b.artist, undefined, { sensitivity: "base" });
        break;
      case "album":
        cmp = a.album.localeCompare(b.album, undefined, { sensitivity: "base" });
        break;
      case "duration":
        cmp = (a.durationMs ?? 0) - (b.durationMs ?? 0);
        break;
    }
    return order === "desc" ? -cmp : cmp;
  });

  return sorted;
}

export function sortAndFilterAlbums(
  albums: Album[],
  options?: {
    sortField?: AlbumSortField;
    sortOrder?: SortOrder;
    query?: string;
  }
): Album[] {
  const field = options?.sortField ?? albumSortField();
  const order = options?.sortOrder ?? albumSortOrder();
  const q = (options?.query ?? albumFilterQuery()).trim().toLowerCase();

  let list = albums;

  if (q) {
    list = list.filter(
      (a) =>
        a.album.toLowerCase().includes(q) ||
        a.albumArtist.toLowerCase().includes(q)
    );
  }

  if (field === "default") {
    return list;
  }

  const sorted = [...list].sort((a, b) => {
    let cmp = 0;
    switch (field) {
      case "album":
        cmp = a.album.localeCompare(b.album, undefined, { sensitivity: "base" });
        break;
      case "artist":
        cmp = a.albumArtist.localeCompare(b.albumArtist, undefined, { sensitivity: "base" });
        break;
      case "year":
        cmp = (a.year || 0) - (b.year || 0);
        break;
      case "trackCount":
        cmp = (a.trackCount || 0) - (b.trackCount || 0);
        break;
    }
    return order === "desc" ? -cmp : cmp;
  });

  return sorted;
}

export function sortAndFilterArtists(
  artists: Artist[],
  options?: {
    sortField?: ArtistSortField;
    sortOrder?: SortOrder;
    query?: string;
  }
): Artist[] {
  const field = options?.sortField ?? artistSortField();
  const order = options?.sortOrder ?? artistSortOrder();
  const q = (options?.query ?? artistFilterQuery()).trim().toLowerCase();

  let list = artists;

  if (q) {
    list = list.filter((a) => a.artist.toLowerCase().includes(q));
  }

  const sorted = [...list].sort((a, b) => {
    let cmp = 0;
    switch (field) {
      case "artist":
        cmp = a.artist.localeCompare(b.artist, undefined, { sensitivity: "base" });
        break;
      case "trackCount":
        cmp = (a.trackCount || 0) - (b.trackCount || 0);
        break;
      case "albumCount":
        cmp = (a.albumCount || 0) - (b.albumCount || 0);
        break;
    }
    return order === "desc" ? -cmp : cmp;
  });

  return sorted;
}
