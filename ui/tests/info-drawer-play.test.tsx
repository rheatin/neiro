import { expect, open, test } from "nsh/test";

const PLAY_FILE = "../analysis_tone.wav";

test("打开输出信息抽屉不影响播放", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
    storage: { "library:roots": "[]" },
    host: { params: { debugPlay: PLAY_FILE } },
  });
  await app.waitFor("label:暂停", { timeout: 20_000 });
  await app.press("label:输出信息");
  await app.waitFor("text:Output 控制台", { timeout: 10_000 });
  await app.wait(3000);
  // 仍在播放：传输键还是"暂停"（"播放" 也是工具栏的标签，不能拿它当反证）。
  expect(app.shows("label:暂停")).toBe(true);
  // 关掉抽屉后依旧能暂停/继续（传输没死）。
  await app.press("label:关闭输出信息");
  await app.wait(600);
  await app.press("label:暂停");
  await app.waitFor("label:播放", { timeout: 10_000 });
  await app.press("label:播放");
  await app.waitFor("label:暂停", { timeout: 10_000 });
}, 120_000);
