// 「服务器」Tab 的第一种状态：花名册。没有选中的服务器时不能只说
// 「没有连接的服务器」——手机上这里就是选服务器的地方。
import { expect, open, test } from "nsh/test";

const SERVERS = JSON.stringify([
  {
    id: "srv-b",
    label: "Server B",
    kind: "jellyfin",
    baseUrl: "http://192.168.1.10:8096",
    user: "listener",
  },
]);

test("已保存但未选中的服务器列出来，点一台进入它的目录", async () => {
  const app = await open("./servers-roster-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: { "remote:servers": SERVERS, "source:active": "all" },
  });

  await app.waitFor("id:server-roster", { timeout: 10_000 });
  expect(app.shows("text:Server B")).toBe(true);
  expect(app.shows("text:没有连接的服务器")).toBe(false);

  await app.press("label:连接 Server B");
  await app.waitFor("text:移除来源", { timeout: 10_000 });
  expect(app.shows("id:server-roster")).toBe(false);
}, 60_000);
