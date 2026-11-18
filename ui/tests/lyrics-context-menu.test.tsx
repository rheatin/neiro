// 歌词显示界面的右键入口：Now Playing 是一个 Sheet，应用的右键菜单是根层
// Overlay，会被 Sheet 盖住——这条用例锁住"菜单画在 Sheet 自己那层、点得到"。

import { open, test, type App as TestApp } from "nsh/test";

const PLAY_FILE = "../analysis_tone.wav";

async function openPlaying(): Promise<TestApp> {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
    storage: { "library:roots": "[]" },
    host: { params: { debugPlay: PLAY_FILE } },
  });
  await app.waitFor("text:全部曲目");
  // The track is loaded in the bar (the engine may be unable to open an
  // output device in CI); the lyrics panel only needs the current path.
  await app.waitFor("id:open-now-playing", { timeout: 20_000 });
  return app;
}

test("右键歌词面板能选中「在线搜索歌词…」并打开搜索面板", async () => {
  const app = await openPlaying();

  // 打开 Now Playing，再点封面把歌词展开。
  await app.press("id:open-now-playing");
  await app.press("id:now-playing-cover");

  // 右键歌词面板：菜单必须能被点到（被 Sheet 盖住会报 COVERED）。
  await app.rightClick("label:歌词");
  await app.press("text:在线搜索歌词…");

  // 搜索面板真的开了：面板里的标题与已启用源数量。
  await app.waitFor("text:在线歌词", { timeout: 5_000 });
  await app.waitFor("text:4 个歌词源就绪", { timeout: 5_000 });
}, 60_000);
