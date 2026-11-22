// 本地文件夹在 iOS 上的两种状态：能解析的启动时被扫描，失效的显示
// 「需要重新选择」并能当场重选；存储里只有 bookmark，没有绝对路径。
// 回前台会再扫一次（iOS 没有文件系统 watcher）。

import { expect, open, test, type App as TestApp } from "nsh/test";

const ROOTS: Record<string, string> = {
  "library:roots": JSON.stringify([
    { id: "a1", bookmark: "BMK-OK", label: "Music" },
    { id: "a2", bookmark: "BMK-BAD", label: "Old" },
  ]),
};

async function settings(): Promise<TestApp> {
  const app = await open("./ios-import-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: ROOTS,
  });
  await app.waitFor("text:Music");
  return app;
}

test("a resolved folder scans; a stale one asks to be picked again", async () => {
  const app = await settings();
  expect(app.shows("text:Old")).toBe(true);
  expect(app.shows("text:需要重新选择")).toBe(true);
  // The stale record has no path to show: only the resolved one prints one.
  expect(app.texts().filter((text) => text === "需要重新选择").length).toBe(1);
  expect(app.texts().includes("scans:1")).toBe(true);
  // What is stored is the bookmark, never the container path.
  const stored = Object.values(app.storage()).join("\n");
  expect(stored.includes("BMK-OK") && stored.includes("BMK-BAD")).toBe(true);
  expect(stored.includes("/music")).toBe(false);
}, 60_000);

test("picking the folder again replaces its bookmark", async () => {
  const app = await settings();
  await app.press("label:重新选择 Old");
  await app.waitFor("text:新目录");
  expect(app.shows("text:需要重新选择")).toBe(false);
  const stored = Object.values(app.storage()).join("\n");
  expect(stored.includes("BMK-NEW")).toBe(true);
  expect(stored.includes("BMK-BAD")).toBe(false);
  // The resolved path stays in memory: it must not be written.
  expect(stored.includes("/new")).toBe(false);
}, 60_000);

test("coming back to the foreground scans again", async () => {
  const app = await settings();
  await app.setLifecycle("background");
  await app.setLifecycle("active");
  await app.waitFor("text:scans:2", { timeout: 5000 });
}, 60_000);
