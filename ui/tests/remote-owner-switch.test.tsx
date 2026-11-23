import { expect, open, test } from "nsh/test";

const SERVERS = JSON.stringify([
  {
    id: "srv-b",
    label: "Server B",
    kind: "jellyfin",
    baseUrl: "http://b.example",
    user: "listener",
  },
]);

test("非当前服务器的远程行先切服务器，不对着错的服务器取流", async () => {
  const app = await open("./remote-owner-switch-harness.tsx", {
    platform: "windows",
    width: 1000,
    height: 700,
    storage: { "remote:servers": SERVERS, "source:active": "all" },
  });

  await app.waitFor("text:Servers: srv-b", { timeout: 10_000 });
  await app.waitFor("text:Fetches: none", { timeout: 5_000 });

  await app.press("label:播放 B 服务器的曲目");
  await app.waitFor("text:Switched: http://b.example", { timeout: 10_000 });
  // 服务器还没就绪：绝不能先对当前（另一台）服务器发 fetch_remote。
  expect(app.shows("text:Fetches: none")).toBe(true);

  await app.press("label:模拟 B 服务器就绪");
  await app.waitFor("text:Fetches: b-1", { timeout: 10_000 });
}, 60_000);

test("直接播放非当前服务器的远程行也先切服务器", async () => {
  const app = await open("./remote-owner-switch-harness.tsx", {
    platform: "windows",
    width: 1000,
    height: 700,
    storage: { "remote:servers": SERVERS, "source:active": "all" },
  });

  await app.waitFor("text:Servers: srv-b", { timeout: 10_000 });
  await app.press("label:播放 B 服务器第二首");
  await app.waitFor("text:Switched: http://b.example", { timeout: 10_000 });
  expect(app.shows("text:Fetches: none")).toBe(true);

  await app.press("label:模拟 B 服务器就绪");
  await app.waitFor("text:Fetches: b-2", { timeout: 10_000 });
}, 60_000);

test("声明了 source 形式的服务器：曲目直接按虚拟路径播放，不取流", async () => {
  const app = await open("./remote-owner-switch-harness.tsx", {
    platform: "windows",
    width: 1000,
    height: 700,
    storage: { "remote:servers": SERVERS, "source:active": "all" },
  });

  await app.waitFor("text:Servers: srv-b", { timeout: 10_000 });
  await app.press("label:声明 source 形式");
  await app.press("label:模拟 B 服务器就绪");
  await app.press("label:播放 B 服务器第二首");

  await app.waitFor("text:Played: remote://b-2.flac", { timeout: 10_000 });
  expect(app.shows("text:Fetches: none")).toBe(true);
}, 60_000);

test("iOS 上即使声明了 source 形式也整文件取流，不按虚拟路径播", async () => {
  const app = await open("./remote-owner-switch-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: { "remote:servers": SERVERS, "source:active": "all" },
  });

  await app.waitFor("text:Servers: srv-b", { timeout: 10_000 });
  await app.press("label:声明 source 形式");
  await app.press("label:模拟 B 服务器就绪");
  await app.press("label:播放 B 服务器第二首");

  await app.waitFor("text:Fetches: b-2", { timeout: 10_000 });
  expect(app.shows("text:Played: none")).toBe(true);
}, 60_000);
