// The mobile application's navigation: four places, each keeping its own
// stack, the library's categories pushed onto the tab that opened them, and
// Settings pushed from the unfolded sidebar (the gear on a phone's system
// header draws the same route). Opened with `platform: "ios"`, so the app is
// the mobile application at the given width.

import { expect, open, test, type App as TestApp } from "nsh/test";

async function folded(): Promise<TestApp> {
  const app = await open("./mobile-shell-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: { "library:roots": JSON.stringify(["/music"]) },
  });
  await app.waitFor("id:mobile-tab-bar");
  return app;
}

test("four places: the tab bar lists them and no sidebar shows", async () => {
  const app = await folded();
  for (const place of ["首页", "资料库", "服务器", "搜索"]) {
    expect(app.shows(`label:${place}`)).toBe(true);
  }
  expect(app.shows("id:mobile-sidebar")).toBe(false);
  // Settings is a page, not a fifth place: folded, only the header's gear
  // opens it.
  expect(app.count("label:设置")).toBe(1);
});

test("a tab switches to its own page", async () => {
  const app = await folded();
  await app.press("label:资料库");
  await app.waitFor("id:page-library");
  await app.press("label:服务器");
  await app.waitFor("id:page-remote");
  await app.press("label:搜索");
  await app.waitFor("id:page-search");
  await app.press("label:首页");
  await app.waitFor("id:page-home");
}, 60_000);

test("a library category pushes onto the tab and Back returns to it", async () => {
  const app = await folded();
  await app.press("label:资料库");
  await app.waitFor("id:page-library");
  await app.press("label:专辑");
  await app.waitFor("id:page-albums");
  // The tab stays lit: the category is a page of the library, not a place.
  await app.back();
  await app.waitFor("id:page-library");
  expect(app.shows("id:page-albums")).toBe(false);
}, 60_000);

test("a tab keeps its stack, and choosing it again returns to its root", async () => {
  const app = await folded();
  await app.press("label:资料库");
  await app.waitFor("id:page-library");
  await app.press("label:歌曲");
  await app.waitFor("id:page-tracks");
  // Another place and back: the pushed page is still there.
  await app.press("label:首页");
  await app.waitFor("id:page-home");
  await app.press("label:资料库");
  await app.waitFor("id:page-tracks");
  // Choosing the tab again goes back to its root.
  await app.press("label:资料库");
  await app.waitFor("id:page-library");
  expect(app.shows("id:page-tracks")).toBe(false);
}, 60_000);

test("unfolded: the sidebar beside the pages, Settings pushed and popped", async () => {
  const app = await open("./mobile-shell-harness.tsx", {
    platform: "ios",
    width: 900,
    height: 800,
    storage: { "library:roots": JSON.stringify(["/music"]) },
  });
  await app.waitFor("id:mobile-sidebar");
  expect(app.shows("id:mobile-tab-bar")).toBe(false);
  // One way to Settings: the sidebar's, not the header's gear as well.
  expect(app.count("label:设置")).toBe(1);
  await app.press("label:设置");
  await app.waitFor("id:page-settings");
  await app.back();
  await app.waitFor("id:page-home");
  expect(app.shows("id:page-settings")).toBe(false);
}, 60_000);
