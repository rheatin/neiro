import { expect, open, test } from "nsh/test";
import { same, type Route, HOME, ALBUMS } from "../src/nav";

test("nav: same(a, b) 详尽覆盖 16 类路由变体，防止参数化路由降级回退", () => {
  // 1. remoteAlbum: 必须匹配 serverId, album, artist
  const remAlbA: Route = { name: "remoteAlbum", serverId: "s1", album: "Album 1", artist: "Artist 1" };
  const remAlbB: Route = { name: "remoteAlbum", serverId: "s1", album: "Album 2", artist: "Artist 2" };
  const remAlbDiffServer: Route = { name: "remoteAlbum", serverId: "s2", album: "Album 1", artist: "Artist 1" };
  const remAlbDiffArtist: Route = { name: "remoteAlbum", serverId: "s1", album: "Album 1", artist: "Artist Other" };
  const remAlbSame: Route = { name: "remoteAlbum", serverId: "s1", album: "Album 1", artist: "Artist 1" };

  expect(same(remAlbA, remAlbSame)).toBe(true);
  expect(same(remAlbA, remAlbB)).toBe(false); // Bug #1 根因：原来缺失直接返回 true
  expect(same(remAlbA, remAlbDiffServer)).toBe(false);
  expect(same(remAlbA, remAlbDiffArtist)).toBe(false);

  // 2. remoteArtist: 必须匹配 serverId, artist
  const remArtA: Route = { name: "remoteArtist", serverId: "s1", artist: "Artist 1" };
  const remArtB: Route = { name: "remoteArtist", serverId: "s1", artist: "Artist 2" };
  const remArtDiffServer: Route = { name: "remoteArtist", serverId: "s2", artist: "Artist 1" };
  const remArtSame: Route = { name: "remoteArtist", serverId: "s1", artist: "Artist 1" };

  expect(same(remArtA, remArtSame)).toBe(true);
  expect(same(remArtA, remArtB)).toBe(false); // Bug #1 根因：原来缺失直接返回 true
  expect(same(remArtA, remArtDiffServer)).toBe(false);

  // 3. album: 必须匹配 album, albumArtist
  const locAlbA: Route = { name: "album", album: "Album 1", albumArtist: "Artist 1" };
  const locAlbB: Route = { name: "album", album: "Album 2", albumArtist: "Artist 2" };
  const locAlbDiffArtist: Route = { name: "album", album: "Album 1", albumArtist: "Artist Other" };
  const locAlbSame: Route = { name: "album", album: "Album 1", albumArtist: "Artist 1" };
  const sameTitleArtistX: Route = { name: "album", album: "Greatest Hits", albumArtist: "Queen" };
  const sameTitleArtistY: Route = { name: "album", album: "Greatest Hits", albumArtist: "ABBA" };

  expect(same(locAlbA, locAlbSame)).toBe(true);
  expect(same(locAlbA, locAlbB)).toBe(false);
  expect(same(locAlbA, locAlbDiffArtist)).toBe(false);
  expect(same(sameTitleArtistX, sameTitleArtistY)).toBe(false);

  // 4. artist: 必须匹配 artist
  const locArtA: Route = { name: "artist", artist: "Artist 1" };
  const locArtB: Route = { name: "artist", artist: "Artist 2" };
  const locArtSame: Route = { name: "artist", artist: "Artist 1" };

  expect(same(locArtA, locArtSame)).toBe(true);
  expect(same(locArtA, locArtB)).toBe(false);

  // 5. playlist & remotePlaylist
  const plA: Route = { name: "playlist", id: 1, playlist: "List 1" };
  const plB: Route = { name: "playlist", id: 2, playlist: "List 1" };
  expect(same(plA, plB)).toBe(false);

  const rplA: Route = { name: "remotePlaylist", id: "p1", playlist: "RList 1" };
  const rplB: Route = { name: "remotePlaylist", id: "p2", playlist: "RList 1" };
  expect(same(rplA, rplB)).toBe(false);

  // 6. 无参数根路由
  expect(same(HOME, HOME)).toBe(true);
  expect(same(ALBUMS, ALBUMS)).toBe(true);
  expect(same(HOME, ALBUMS)).toBe(false);
  expect(same(undefined, HOME)).toBe(false);
});

test("UI 导航回退再进验证：远程专辑 A -> back() -> 远程专辑 B 页面与曲目完全更新", async () => {
  const app = await open("./album-nav-harness.tsx", {
    platform: "windows",
    width: 1280,
    height: 800,
  });

  // 1. 进入远程专辑 A
  await app.press("label:打开远程专辑1");
  await app.waitFor("text:Remote Album Alpha", { timeout: 10_000 });
  expect(app.shows("text:Remote Track Alpha 1")).toBe(true);

  // 2. 返回
  await app.press("label:返回");
  await app.wait(400);

  // 3. 进入远程专辑 B
  await app.press("label:打开远程专辑2");
  await app.waitFor("text:Remote Album Beta", { timeout: 10_000 });
  expect(app.shows("text:Remote Track Beta 1")).toBe(true);

  // 验证不保留专辑 A 的标题与曲目
  expect(app.shows("text:Remote Album Alpha")).toBe(false);
  expect(app.shows("text:Remote Track Alpha 1")).toBe(false);
}, 60_000);

test("UI 导航回退再进验证：远程歌手 A -> back() -> 远程歌手 B 页面与曲目完全更新", async () => {
  const app = await open("./album-nav-harness.tsx", {
    platform: "windows",
    width: 1280,
    height: 800,
  });

  // 1. 进入远程歌手 A
  await app.press("label:打开远程歌手1");
  await app.waitFor("text:Artist One", { timeout: 10_000 });
  expect(app.shows("text:Remote Track Alpha 1")).toBe(true);

  // 2. 返回
  await app.press("label:返回");
  await app.wait(400);

  // 3. 进入远程歌手 B
  await app.press("label:打开远程歌手2");
  await app.waitFor("text:Artist Two", { timeout: 10_000 });
  expect(app.shows("text:Remote Track Beta 1")).toBe(true);

  // 验证不保留歌手 A 的标题与曲目
  expect(app.shows("text:Artist One")).toBe(false);
  expect(app.shows("text:Remote Track Alpha 1")).toBe(false);
}, 60_000);

test("UI 导航回退再进验证：本地专辑 A -> back() -> 本地专辑 B 页面与曲目完全更新", async () => {
  const app = await open("./album-nav-harness.tsx", {
    platform: "windows",
    width: 1280,
    height: 800,
    storage: { "library:roots": JSON.stringify(["/music"]) },
  });

  // 1. 进入本地专辑 A
  await app.press("label:打开本地专辑1");
  await app.waitFor("text:Local Album Alpha", { timeout: 10_000 });
  expect(app.shows("text:Local Track Alpha 1")).toBe(true);

  // 2. 返回
  await app.press("label:返回");
  await app.wait(400);

  // 3. 进入本地专辑 B
  await app.press("label:打开本地专辑2");
  await app.waitFor("text:Local Album Beta", { timeout: 10_000 });
  expect(app.shows("text:Local Track Beta 1")).toBe(true);

  // 验证不保留专辑 A 的标题与曲目
  expect(app.shows("text:Local Album Alpha")).toBe(false);
  expect(app.shows("text:Local Track Alpha 1")).toBe(false);
}, 60_000);
