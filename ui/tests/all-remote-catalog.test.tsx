import { expect, open, test } from "nsh/test";

test("全部目录同时包含本地与仅缓存的远程服务器曲目", async () => {
  const app = await open("./all-remote-catalog-harness.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
  });
  await app.waitFor("text:Local In All", { timeout: 10_000 });
  await app.waitFor("text:Remote In All", { timeout: 10_000 });
  expect(app.shows("text:Local In All")).toBe(true);
  expect(app.shows("text:Remote In All")).toBe(true);
}, 60_000);
