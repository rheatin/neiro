import { expect, open, test } from "nsh/test";

test("曲目工具栏的播放按钮切换共享 shuffle 状态并只触发一次播放", async () => {
  const app = await open("./shuffle-toolbar-harness.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
    storage: { "player:shuffle": "0" },
  });
  await app.waitFor("text:顺序播放", { timeout: 10_000 });
  await app.waitFor("text:Play calls: 0; Shuffle: off", { timeout: 10_000 });

  await app.press("label:顺序播放");
  await app.waitFor("text:Play calls: 1; Shuffle: on", { timeout: 10_000 });
  await app.waitFor("text:随机播放", { timeout: 10_000 });

  await app.press("label:随机播放");
  await app.waitFor("text:Play calls: 2; Shuffle: off", { timeout: 10_000 });
  await app.waitFor("text:顺序播放", { timeout: 10_000 });
}, 60_000);
