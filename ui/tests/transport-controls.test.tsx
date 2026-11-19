import { open, test } from "nsh/test";

test("传输控件的单一模式按钮循环四态", async () => {
  const app = await open("./transport-controls-harness.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
  });

  await app.waitFor("text:Shuffle: off; Repeat: off", { timeout: 10_000 });
  await app.waitFor("label:顺序播放", { timeout: 10_000 });

  await app.press("label:顺序播放");
  await app.waitFor("text:Shuffle: on; Repeat: off", { timeout: 10_000 });
  await app.waitFor("label:随机播放", { timeout: 10_000 });

  await app.press("label:随机播放");
  await app.waitFor("text:Shuffle: off; Repeat: all", { timeout: 10_000 });
  await app.waitFor("label:列表循环", { timeout: 10_000 });

  await app.press("label:列表循环");
  await app.waitFor("text:Shuffle: off; Repeat: one", { timeout: 10_000 });
  await app.waitFor("label:单曲循环", { timeout: 10_000 });

  await app.press("label:单曲循环");
  await app.waitFor("text:Shuffle: off; Repeat: off", { timeout: 10_000 });
  await app.waitFor("label:顺序播放", { timeout: 10_000 });
}, 60_000);
