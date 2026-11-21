import { expect, test } from "nsh/test";
import {
  advancePlayContextToPath,
  canPrev,
  cycleRepeatMode,
  notifyRemoteFetchFailed,
  playLaterTrack,
  playNextTrack,
  playQueue,
  playTrack,
  prev,
  registerRemotePlayer,
  repeatMode,
  setRepeatMode,
  shuffleMode,
  toggleShuffle,
  trackToQueueItem,
} from "../src/state/player";
import { isStreamBitPerfect } from "../src/state/resolved";
import { engine } from "../src/backend/engine";
import type { Track } from "../src/backend/types";

test("QueueDirector UI 状态：三态 Repeat 循环 (Off -> All -> One -> Off)", () => {
  engine.connect();
  setRepeatMode("off");
  expect(repeatMode()).toBe("off");

  cycleRepeatMode();
  expect(repeatMode()).toBe("all");

  cycleRepeatMode();
  expect(repeatMode()).toBe("one");

  cycleRepeatMode();
  expect(repeatMode()).toBe("off");
});

test("QueueDirector UI 状态：Shuffle 状态即时切换", () => {
  engine.connect();
  const initial = shuffleMode();
  toggleShuffle();
  expect(shuffleMode()).toBe(!initial);
  toggleShuffle();
  expect(shuffleMode()).toBe(initial);
});

function createDummyTrack(path: string, title: string, artist: string, album: string, durationMs: number): Track {
  return {
    path,
    title,
    artist,
    album,
    albumArtist: artist,
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "Pop",
    durationMs,
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  };
}

test("QueueDirector UI 状态：上一曲由 engine.previous 统一裁决，不再硬编码为 seek(0)", () => {
  engine.connect();
  const dummyTrack = createDummyTrack("/music/track2.flac", "Track 2", "Artist 2", "Album 2", 200000);

  let calledPrevious = false;
  const originalPrevious = (engine as any).previous.bind(engine);
  (engine as any).previous = () => {
    calledPrevious = true;
  };

  try {
    playTrack(dummyTrack);
    expect(canPrev()).toBe(true);
    prev();
    expect(calledPrevious).toBe(true);
  } finally {
    (engine as any).previous = originalPrevious;
  }
});

test("QueueDirector UI 状态：Track 转换为标准的 QueueItem 并支持 Play Next / Play Later", () => {
  engine.connect();
  const dummyTrack = createDummyTrack("/music/track1.flac", "Track 1", "Artist 1", "Album 1", 180000);

  const item = trackToQueueItem(dummyTrack);
  expect(item.id).toBe("/music/track1.flac");
  expect(item.path).toBe("/music/track1.flac");
  expect(item.title).toBe("Track 1");
  expect(item.artist).toBe("Artist 1");
  expect(item.durationMs).toBe(180000);
  expect(item.duration_ms).toBe(180000);

  let sentNext = false;
  let sentLater = false;
  const originalPlayNext = (engine as any).playNext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);

  (engine as any).playNext = (items: any[]) => {
    sentNext = items.length === 1 && items[0].id === dummyTrack.path;
  };
  (engine as any).playLater = (items: any[]) => {
    sentLater = items.length === 1 && items[0].id === dummyTrack.path;
  };

  try {
    playNextTrack(dummyTrack);
    expect(sentNext).toBe(true);

    playLaterTrack(dummyTrack);
    expect(sentLater).toBe(true);
  } finally {
    (engine as any).playNext = originalPlayNext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("Single Owner: isStreamBitPerfect 统一判定在无流态下返回 false", () => {
  expect(isStreamBitPerfect()).toBe(false);
});

test("Remote 虚拟路径拦截：playTrack 拦截 remote:// 前缀并通过钩子解析本地缓存", () => {
  let intercepted = false;
  let playedCachedPath = "";
  const remoteTrack = createDummyTrack("remote://song123.flac", "Remote 123", "Artist", "Album", 150000);

  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path: string, then: (cached: string) => void) => {
      if (path.startsWith("remote://")) {
        intercepted = true;
        then("/tmp/cache/song123.flac");
        return true;
      }
      return false;
    },
  });

  const originalPlayContext = (engine as any).playContext.bind(engine);
  (engine as any).playContext = (items: any[]) => {
    playedCachedPath = items[0]?.path ?? "";
  };

  try {
    playTrack(remoteTrack);
    expect(intercepted).toBe(true);
    expect(playedCachedPath).toBe("/tmp/cache/song123.flac");
  } finally {
    (engine as any).playContext = originalPlayContext;
  }
});

test("Remote source form：源直读的曲目按虚拟路径播放，不下载", () => {
  const paths = ["remote://a.flac", "remote://b.flac", "remote://c.flac"];
  const rows = paths.map((path, i) => createDummyTrack(path, `Source ${i}`, "Artist", "Album", 100000));
  let fetchCalls = 0;
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: () => {
      fetchCalls += 1;
      return true;
    },
    resolveRemotePath: () => null,
    playsSource: (path) => path.startsWith("remote://"),
  });

  const played: string[] = [];
  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = (items: any[]) => { played.push(items[0].path); };
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 0);
    expect(played).toEqual(["remote://a.flac"]);
    expect(later).toEqual(["remote://b.flac", "remote://c.flac"]);
    expect(fetchCalls).toBe(0);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：当前曲推进后按原序补齐整张列表，同时只取流一首", () => {
  const remotePaths = Array.from({ length: 6 }, (_, i) => `remote://song-${i}.flac`);
  const rows = remotePaths.map((path, i) => createDummyTrack(path, `Remote ${i}`, "Artist", "Album", 120000 + i));
  const resolved = new Map<string, string>();
  const deferred = new Map<string, () => void>();
  let pendingCount = 0;
  let activePending = 0;
  let maxActivePending = 0;
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      if (!path.startsWith("remote://")) return false;
      pendingCount += 1;
      activePending += 1;
      maxActivePending = Math.max(maxActivePending, activePending);
      deferred.set(path, () => {
        activePending -= 1;
        then(path.replace("remote://", "/cache/"));
      });
      return true;
    },
    resolveRemotePath: (path) => resolved.get(path) ?? null,
  });

  const played: string[] = [];
  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = (items: any[]) => { played.push(items[0].path); };
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 0);
    expect(later).toEqual([]);
    expect(pendingCount).toBe(1);

    resolved.set(rows[0].path, "/cache/song-0.flac");
    deferred.get(rows[0].path)?.();
    expect(played).toEqual(["/cache/song-0.flac"]);
    for (const row of rows) {
      const cached = row.path.replace("remote://", "/cache/");
      resolved.set(row.path, cached);
      deferred.get(row.path)?.();
      advancePlayContextToPath(cached);
    }
    expect(maxActivePending).toBe(1);
    expect(pendingCount).toBe(rows.length);
    expect(later).toEqual(rows.slice(1).map((row) => row.path.replace("remote://", "/cache/")));
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：混合列表严格按原列表顺序入队", () => {
  const remote = (index: number) => createDummyTrack(`remote://mixed-${index}.flac`, `Remote ${index}`, "Artist", "Album", 100000 + index);
  const local = (index: number) => createDummyTrack(`/music/local-${index}.flac`, `Local ${index}`, "Artist", "Album", 100000 + index);
  const rows = [local(0), remote(0), remote(1), local(1), remote(2), remote(3), local(2)];
  const resolved = new Map<string, string>();
  const deferred = new Map<string, () => void>();
  let pendingCount = 0;

  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      if (!path.startsWith("remote://")) return false;
      pendingCount += 1;
      deferred.set(path, () => then(path.replace("remote://", "/cache/")));
      return true;
    },
    resolveRemotePath: (path) => resolved.get(path) ?? null,
  });

  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = () => { };
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 0);
    for (const row of rows) {
      const cached = row.path.replace("remote://", "/cache/");
      resolved.set(row.path, cached);
      deferred.get(row.path)?.();
      advancePlayContextToPath(row.path.startsWith("remote://") ? cached : row.path);
    }
    expect(later).toEqual(rows.slice(1).map((row) => row.path.startsWith("remote://") ? row.path.replace("remote://", "/cache/") : row.path));
    expect(pendingCount).toBe(rows.filter((row) => row.path.startsWith("remote://")).length);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：点击中间一首后只补后面的行", () => {
  const remote = (index: number) => createDummyTrack(`remote://middle-${index}.flac`, `Remote ${index}`, "Artist", "Album", 100000 + index);
  const rows = Array.from({ length: 7 }, (_, i) => remote(i));
  const resolved = new Map<string, string>();
  const deferred = new Map<string, () => void>();
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      if (!path.startsWith("remote://")) return false;
      deferred.set(path, () => then(path.replace("remote://", "/cache/")));
      return true;
    },
    resolveRemotePath: (path) => resolved.get(path) ?? null,
  });

  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = () => {};
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 4);
    resolved.set(rows[4].path, "/cache/middle-4.flac");
    deferred.get(rows[4].path)?.();
    expect(later).toEqual([]);
    for (const row of rows.slice(5)) {
      const cached = row.path.replace("remote://", "/cache/");
      resolved.set(row.path, cached);
      deferred.get(row.path)?.();
      advancePlayContextToPath(cached);
    }
    expect(later).toEqual(rows.slice(5).map((row) => row.path.replace("remote://", "/cache/")));
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("本地上下文：队列保持在当前曲之后十首以内", () => {
  const rows = Array.from({ length: 20 }, (_, i) => createDummyTrack(`/music/window-${i}.flac`, `Track ${i}`, "Artist", "Album", 100000 + i));
  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = () => {};
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 0);
    expect(later).toEqual(rows.slice(1, 10).map((row) => row.path));
    for (const row of rows.slice(1, 12)) {
      advancePlayContextToPath(row.path);
      const current = rows.findIndex((candidate) => candidate.path === row.path);
      const queued = later.slice(0, Math.max(0, current));
      expect(queued.length).toBeLessThanOrEqual(current + 10);
    }
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：同一列表快速改点，旧 fetch 不得接管播放", () => {
  const rows = [
    createDummyTrack("remote://stale-0.flac", "Remote 0", "Artist", "Album", 100000),
    createDummyTrack("remote://stale-1.flac", "Remote 1", "Artist", "Album", 100001),
  ];
  const resolved = new Map<string, string>();
  const deferred = new Map<string, () => void>();
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      if (!path.startsWith("remote://")) return false;
      const cached = resolved.get(path);
      if (cached) {
        then(cached);
        return true;
      }
      deferred.set(path, () => then(path.replace("remote://", "/cache/")));
      return true;
    },
    resolveRemotePath: (path) => resolved.get(path) ?? null,
    cancelPlayIntent: () => {},
  });

  const played: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = (items: any[]) => { played.push(items[0].path); };
  (engine as any).playLater = () => {};

  try {
    playQueue(rows, 0);
    resolved.set(rows[1].path, "/cache/stale-1.flac");
    playQueue(rows, 1);
    expect(played).toEqual(["/cache/stale-1.flac"]);

    resolved.set(rows[0].path, "/cache/stale-0.flac");
    deferred.get(rows[0].path)?.();
    expect(played).toEqual(["/cache/stale-1.flac"]);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：首曲已缓存时不得把当前曲重复入队", () => {
  const rows = [
    createDummyTrack("remote://cached-0.flac", "Cached 0", "Artist", "Album", 100000),
    createDummyTrack("remote://cached-1.flac", "Cached 1", "Artist", "Album", 100001),
  ];
  const resolved = new Map<string, string>([
    [rows[0].path, "/cache/cached-0.flac"],
    [rows[1].path, "/cache/cached-1.flac"],
  ]);
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      const cached = resolved.get(path);
      if (!cached) return false;
      then(cached);
      return true;
    },
    resolveRemotePath: (path) => resolved.get(path) ?? null,
    cancelPlayIntent: () => {},
  });

  const played: string[] = [];
  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = (items: any[]) => { played.push(items[0].path); };
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 0);
    expect(played).toEqual(["/cache/cached-0.flac"]);
    expect(later).toEqual(["/cache/cached-1.flac"]);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：等待取流时直接播放别的本地曲目，旧 fetch 不得接管", () => {
  const remoteRow = createDummyTrack("remote://other-0.flac", "Remote 0", "Artist", "Album", 100000);
  const localRow = createDummyTrack("/music/local-other.flac", "Local", "Artist", "Album", 100001);
  const resolved = new Map<string, string>();
  const releaseQueue: Array<() => void> = [];
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      if (!path.startsWith("remote://")) return false;
      const cached = resolved.get(path);
      if (cached) {
        then(cached);
        return true;
      }
      releaseQueue.push(() => then(path.replace("remote://", "/cache/")));
      return true;
    },
    resolveRemotePath: (path) => resolved.get(path) ?? null,
    cancelPlayIntent: () => {},
  });

  const played: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = (items: any[]) => { played.push(items[0].path); };
  (engine as any).playLater = () => {};

  try {
    playQueue([remoteRow], 0);
    playTrack(localRow);
    expect(played).toEqual([localRow.path]);

    resolved.set(remoteRow.path, "/cache/other-0.flac");
    releaseQueue.shift()?.();
    expect(played).toEqual([localRow.path]);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：首曲取流失败退到已缓存远程，不重复入队且索引可用", () => {
  const rows = [
    createDummyTrack("remote://fallback-0.flac", "Remote 0", "Artist", "Album", 100000),
    createDummyTrack("remote://fallback-1.flac", "Remote 1", "Artist", "Album", 100001),
  ];
  const cached = new Map<string, string>([[rows[1].path, "/cache/fallback-1.flac"]]);
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      if (path !== rows[1].path) return false;
      then(cached.get(path)!);
      return true;
    },
    resolveRemotePath: (path) => cached.get(path) ?? null,
    cancelPlayIntent: () => {},
  });

  const played: string[] = [];
  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = (items: any[]) => { played.push(items[0].path); };
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 0);
    expect(played).toEqual(["/cache/fallback-1.flac"]);
    expect(later).not.toContain("/cache/fallback-1.flac");

    advancePlayContextToPath("/cache/fallback-1.flac");
    expect(played).toEqual(["/cache/fallback-1.flac"]);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("混合上下文：路径索引缓存远程解析，避免每次推进全表扫描", () => {
  const remoteRows = Array.from({ length: 10_000 }, (_, i) =>
    createDummyTrack(`remote://perf-${i}.flac`, `Remote ${i}`, "Artist", "Album", 100000 + i),
  );
  const localRow = createDummyTrack("/music/perf-local.flac", "Local", "Artist", "Album", 100000);
  const rows = [...remoteRows, localRow];
  let resolverCalls = 0;
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: () => false,
    resolveRemotePath: (path) => {
      resolverCalls += 1;
      return remoteRows.find((row) => row.path === path) ? `/cache/${path.slice(9)}` : null;
    },
    cancelPlayIntent: () => {},
  });

  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = () => {};
  (engine as any).playLater = () => {};

  try {
    playQueue(rows, remoteRows.length);
    const callsBeforeAdvance = resolverCalls;
    advancePlayContextToPath(localRow.path);
    expect(resolverCalls).toBeLessThan(20);
    expect(resolverCalls).toBeGreaterThanOrEqual(callsBeforeAdvance);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("混合上下文：第二首已缓存远程曲目播放后上下文仍然有效", () => {
  const remote0 = createDummyTrack("remote://cached-a.flac", "A", "Artist", "Album", 100000);
  const remote1 = createDummyTrack("remote://cached-b.flac", "B", "Artist", "Album", 100001);
  const locals = Array.from({ length: 13 }, (_, index) =>
    createDummyTrack(`/music/local-${index}.flac`, `L${index}`, "Artist", "Album", 100002 + index),
  );
  const rows = [remote0, remote1, ...locals];
  const cached = new Map<string, string>([
    [remote0.path, "/cache/cached-a.flac"],
    [remote1.path, "/cache/cached-b.flac"],
  ]);
  registerRemotePlayer({
    playRemoteByPath: () => true,
    withRemotePathByPath: (path, then) => {
      const cachedPath = cached.get(path);
      if (!cachedPath) return false;
      then(cachedPath);
      return true;
    },
    resolveRemotePath: (path) => cached.get(path) ?? null,
    cancelPlayIntent: () => {},
  });

  const later: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = () => {};
  (engine as any).playLater = (items: any[]) => later.push(...items.map((item: any) => item.path));

  try {
    playQueue(rows, 0);
    advancePlayContextToPath("/cache/cached-a.flac");
    advancePlayContextToPath("/cache/cached-b.flac");
    for (const local of locals) advancePlayContextToPath(local.path);
    expect(later).toContain(locals[locals.length - 1].path);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});

test("远程上下文：当前曲取流失败时跳到下一首能播的曲目，而不是停死", () => {
  const rows = [
    createDummyTrack("remote://fail-1.flac", "Remote Fail", "Artist", "Album", 100000),
    createDummyTrack("/music/after-fail.flac", "Local After", "Artist", "Album", 100001),
  ];
  registerRemotePlayer({
    playRemoteByPath: () => true,
    // In flight and never lands — the engine reports the failure separately.
    withRemotePathByPath: () => true,
    resolveRemotePath: () => null,
    cancelPlayIntent: () => {},
  });

  const played: string[] = [];
  const originalPlayContext = (engine as any).playContext.bind(engine);
  const originalPlayLater = (engine as any).playLater.bind(engine);
  (engine as any).playContext = (items: any[]) => {
    played.push(items[0].path);
  };
  (engine as any).playLater = () => {};

  try {
    playQueue(rows, 0);
    expect(played).toEqual([]);
    notifyRemoteFetchFailed("fail-1");
    expect(played).toEqual(["/music/after-fail.flac"]);
  } finally {
    (engine as any).playContext = originalPlayContext;
    (engine as any).playLater = originalPlayLater;
  }
});
