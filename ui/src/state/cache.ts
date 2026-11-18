// Storage and cache: the one UI state over the Rust host's buckets. Stats
// are the host's JSON exactly (snake_case); cleaning asks the host, then
// drops this run's in-memory copies so deleted files cannot keep painting.

import { signal } from "nsh";
import { engine } from "../backend/engine";
import { clearArtworkMemory } from "./artwork";
import { notify } from "./feedback";
import { forgetFetchedRemotePaths } from "./remote";

export type CacheKind = "remote-audio" | "remote-artwork" | "remote-catalog" | "local-artwork" | "all";

export interface CacheStats {
  remote_audio_bytes: number;
  remote_audio_files: number;
  remote_artwork_bytes: number;
  remote_artwork_files: number;
  remote_catalog_bytes: number;
  remote_catalog_files: number;
  local_artwork_bytes: number;
  local_artwork_files: number;
}

export const EMPTY_CACHE_STATS: CacheStats = {
  remote_audio_bytes: 0,
  remote_audio_files: 0,
  remote_artwork_bytes: 0,
  remote_artwork_files: 0,
  remote_catalog_bytes: 0,
  remote_catalog_files: 0,
  local_artwork_bytes: 0,
  local_artwork_files: 0,
};

export const cacheStats = signal<CacheStats>(EMPTY_CACHE_STATS);
export const cacheBusy = signal(false);

/** B/KB/MB/GB for people: one decimal from KB up, so a 1.5 MB cache does
 * not round to 2 MB. */
export function formatCacheSize(bytes: number): string {
  const size = Number.isFinite(bytes) && bytes > 0 ? bytes : 0;
  if (size < 1024) return `${size} B`;
  if (size < 1024 * 1024) return `${(size / 1024).toFixed(1)} KB`;
  if (size < 1024 * 1024 * 1024) return `${(size / (1024 * 1024)).toFixed(1)} MB`;
  return `${(size / (1024 * 1024 * 1024)).toFixed(1)} GB`;
}

function statsOf(json: string | null, fallback: CacheStats = EMPTY_CACHE_STATS): CacheStats {
  if (!json) return fallback;
  try {
    const parsed = JSON.parse(json) as Partial<CacheStats>;
    const field = (value: unknown): number => (typeof value === "number" && Number.isFinite(value) ? value : 0);
    return {
      remote_audio_bytes: field(parsed.remote_audio_bytes),
      remote_audio_files: field(parsed.remote_audio_files),
      remote_artwork_bytes: field(parsed.remote_artwork_bytes),
      remote_artwork_files: field(parsed.remote_artwork_files),
      remote_catalog_bytes: field(parsed.remote_catalog_bytes),
      remote_catalog_files: field(parsed.remote_catalog_files),
      local_artwork_bytes: field(parsed.local_artwork_bytes),
      local_artwork_files: field(parsed.local_artwork_files),
    };
  } catch {
    return fallback;
  }
}

function clearMemoryFor(kind: CacheKind): void {
  if (kind === "remote-audio" || kind === "all") forgetFetchedRemotePaths();
  if (kind === "remote-artwork" || kind === "local-artwork" || kind === "all") clearArtworkMemory();
}

export function refreshCacheStats(): void {
  const stats = statsOf(engine.cacheStats(), cacheStats());
  const previous = cacheStats();
  if (
    stats.remote_audio_bytes !== previous.remote_audio_bytes ||
    stats.remote_audio_files !== previous.remote_audio_files ||
    stats.remote_artwork_bytes !== previous.remote_artwork_bytes ||
    stats.remote_artwork_files !== previous.remote_artwork_files ||
    stats.remote_catalog_bytes !== previous.remote_catalog_bytes ||
    stats.remote_catalog_files !== previous.remote_catalog_files ||
    stats.local_artwork_bytes !== previous.local_artwork_bytes ||
    stats.local_artwork_files !== previous.local_artwork_files
  ) {
    cacheStats.set(stats);
  }
}

export async function clearCache(kind: CacheKind): Promise<void> {
  if (cacheBusy()) return;
  cacheBusy.set(true);
  try {
    cacheStats.set(statsOf(engine.clearCache(kind), cacheStats()));
    clearMemoryFor(kind);
  } catch (error) {
    notify(`清理失败：${error instanceof Error ? error.message : String(error)}`);
  } finally {
    cacheBusy.set(false);
  }
}
