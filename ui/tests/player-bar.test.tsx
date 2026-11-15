// 播放条的无头复现（真 bug：真机窗口里 PlayerBar 从不出现）。走真引擎：
// 开窗到 ready，`host.params.debugPlay` 让 App 直接播一个文件，然后断言
// 播放条在树里（它的 accessibilityLabel / 文本）。不走扫描。

import { open, test, expect, type App as TestApp } from "nsh/test";

const PLAY_FILE = "../analysis_tone.wav";

const WINDOW = { width: 1280, height: 837 };

async function openApp(): Promise<TestApp> {
  return open("../App.tsx", {
    platform: "windows",
    ...WINDOW,
    storage: { "library:roots": "[]" },
    host: { params: { debugPlay: PLAY_FILE } },
  });
}

test("the player bar mounts once a track plays", async () => {
  const app = await openApp();

  // 引擎连上、页面就位（侧栏的固定行）。
  await app.waitFor("text:全部曲目");

  // 播放条在树里：抽屉按钮、红心、pill 本体。
  await app.waitFor("label:输出信息", { timeout: 20_000 });
  expect(app.shows("label:音量")).toBe(true);
  expect(app.shows("id:player")).toBe(true);
});

test("the bar keeps what plays", async () => {
  const app = await openApp();
  await app.waitFor("text:全部曲目");
  await app.waitFor("id:player", { timeout: 10_000 });
  await app.waitFor("label:输出信息");
  // 播放中：传输键换成"暂停"（标题来自引擎的 analysis，名字可能是标签里
  // 的真标题，不能钉文件名）。
  await app.waitFor("label:暂停", { timeout: 20_000 });
}, 45_000);
