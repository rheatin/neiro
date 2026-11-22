// 扫描丢掉的文件：路由规则是纯函数，不连引擎也能测。删掉的文件留在
// 「不可用」里，直到它回来或者根目录离开。

import { expect, test } from "nsh/test";
import { lostAfterScan } from "../src/state/catalog";
import type { Track } from "../src/backend/types";

function track(path: string, title: string): Track {
  return {
    path,
    title,
    artist: "Artist",
    albumArtist: "Artist",
    album: "Album",
    durationMs: 1000,
    discNo: 1,
    trackNo: 1,
    year: 2024,
    genre: "",
    sampleRate: 44100,
    bits: 16,
    channels: 2,
  } as Track;
}

test("a removed file the index still had is kept as lost", () => {
  const before = [track("/m/a.flac", "A"), track("/m/b.flac", "B")];
  const lost = lostAfterScan(before, new Map(), {
    added: [],
    modified: [],
    removed: ["/m/b.flac"],
  });
  expect(lost.get("/m/b.flac")?.title).toBe("B");
  expect(lost.has("/m/a.flac")).toBe(false);
});

test("a file that came back stops being lost", () => {
  const before = [track("/m/a.flac", "A")];
  const stale = new Map([["/m/a.flac", before[0]]]);
  const added = lostAfterScan(before, stale, {
    added: ["/m/a.flac"],
    modified: [],
    removed: [],
  });
  expect(added.size).toBe(0);
  const modified = lostAfterScan(before, stale, {
    added: [],
    modified: ["/m/a.flac"],
    removed: [],
  });
  expect(modified.size).toBe(0);
});

test("a removed path the index never had adds no row", () => {
  const lost = lostAfterScan([], new Map(), {
    added: [],
    modified: [],
    removed: ["/m/gone.flac"],
  });
  expect(lost.size).toBe(0);
});
