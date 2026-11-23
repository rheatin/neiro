import { expect, open, test } from "nsh/test";
import { engine } from "../src/backend/engine";
import { cacheStats, clearCache, EMPTY_CACHE_STATS, formatCacheSize } from "../src/state/cache";
import { clearArtworkMemory } from "../src/state/artwork";
import { forgetFetchedRemotePaths } from "../src/state/remote";

test("formatCacheSize 使用人类可读单位", () => {
  expect(formatCacheSize(0)).toBe("0 B");
  expect(formatCacheSize(1023)).toBe("1023 B");
  expect(formatCacheSize(1536)).toBe("1.5 KB");
  expect(formatCacheSize(1536 * 1024)).toBe("1.5 MB");
  expect(formatCacheSize(1536 * 1024 * 1024)).toBe("1.5 GB");
});

test("clearCache 更新 host stats 并清理内存缓存", async () => {
  const stats = {
    remote_audio_bytes: 2,
    remote_audio_files: 1,
    remote_artwork_bytes: 4,
    remote_artwork_files: 2,
    remote_catalog_bytes: 0,
    remote_catalog_files: 0,
    local_artwork_bytes: 8,
    local_artwork_files: 3,
  };
  cacheStats.set(stats);
  let cleared = "";
  const originalStats = engine.cacheStats;
  const originalClear = engine.clearCache;
  engine.cacheStats = () => JSON.stringify(EMPTY_CACHE_STATS);
  engine.clearCache = (kind: string) => {
    cleared = kind;
    return JSON.stringify(EMPTY_CACHE_STATS);
  };

  try {
    await clearCache("remote-audio");
    expect(cleared).toBe("remote-audio");
    expect(cacheStats().remote_audio_bytes).toBe(0);
    expect(cacheStats().remote_audio_files).toBe(0);
  } finally {
    engine.cacheStats = originalStats;
    engine.clearCache = originalClear;
    clearArtworkMemory();
    forgetFetchedRemotePaths();
  }
});

test("clearCache 失败时保留统计并提示", async () => {
  const stats = { ...EMPTY_CACHE_STATS, remote_audio_bytes: 12, remote_audio_files: 2 };
  cacheStats.set(stats);
  const originalClear = engine.clearCache;
  engine.clearCache = () => null;
  try {
    await clearCache("remote-audio");
    expect(cacheStats().remote_audio_bytes).toBe(stats.remote_audio_bytes);
    expect(cacheStats().remote_audio_files).toBe(stats.remote_audio_files);
  } finally {
    engine.clearCache = originalClear;
  }
});

test("存储与缓存 UI：清理按钮、播放禁用和全部清理确认", async () => {
  const app = await open("./settings-cache-harness.tsx", {
    platform: "windows",
    width: 1440,
    height: 1000,
    storage: { "library:roots": "[]" },
    host: { params: { debugPlay: "../analysis_tone.wav" } },
  });
  await app.waitFor("id:cache-card", { timeout: 10_000 });
  await app.wait(200);

  expect(app.shows("id:cache-clear-remote-audio")).toBe(true);
  expect(app.shows("id:cache-clear-remote-artwork")).toBe(true);
  expect(app.shows("id:cache-clear-remote-catalog")).toBe(true);
  expect(app.shows("id:cache-clear-local-artwork")).toBe(true);
  expect(app.shows("id:cache-clear-analysis")).toBe(true);
  expect(app.shows("id:cache-budget-0")).toBe(true);
  expect(app.shows("id:cache-budget-unlimited")).toBe(true);
  expect(app.shows("id:cache-clear-all")).toBe(true);

  await app.press("id:cache-clear-all");
  await app.waitFor("id:cache-clear-all-dialog", { timeout: 5_000 });
  expect(app.shows("text:确认全部清理？")).toBe(true);
}, 60_000);
