// 资料库: the Library tab's root lists the same categories the desktop
// sidebar opens, each row pushing the page it names. The counts come from
// the one seeded track, so a row that reads the wrong signal shows.

import { expect, open, test, type App as TestApp } from "nsh/test";

async function library(): Promise<TestApp> {
  const app = await open("./mobile-shell-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: {
      "library:roots": JSON.stringify([{ id: "root-1", bookmark: "/music", label: "music" }]),
    },
  });
  await app.waitFor("id:mobile-tab-bar");
  await app.press("label:资料库");
  await app.waitFor("id:page-library");
  return app;
}

test("the five categories are listed with their counts", async () => {
  const app = await library();
  for (const row of ["专辑", "艺术家", "歌曲", "收藏", "播放列表"]) {
    expect(app.shows(`label:${row}`)).toBe(true);
  }
  // Each row counts what its page lists (the engine's fake index decides
  // the numbers): a row reading the wrong signal shows no count.
  const texts = app.texts();
  const counts = texts.filter((t) => /^\d+ (张|位|首|个)$/.test(t));
  expect(counts.length).toBe(5);
}, 60_000);

test("each category pushes the page the sidebar opens", async () => {
  const cases: [string, string][] = [
    ["专辑", "id:page-albums"],
    ["艺术家", "id:page-artists"],
    ["歌曲", "id:page-tracks"],
    ["收藏", "id:page-liked"],
    ["播放列表", "id:page-playlists"],
  ];
  for (const [row, page] of cases) {
    const app = await library();
    await app.press(`label:${row}`);
    await app.waitFor(page);
    await app.back();
    await app.waitFor("id:page-library");
  }
}, 120_000);
