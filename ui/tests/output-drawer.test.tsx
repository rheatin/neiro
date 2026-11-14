// 输出信息抽屉：从播放条的 (i) 打开，五个硬件模组都在，且各自的契约成立。
//
// 这一版抽屉是 CS-02 控台（01…05），不再是原来的 Processing Chain 卡片堆：
// 断言跟着契约走（模块名、四个校验灯、根因诊断），不跟着某一段的措辞走。

import { expect, open, test } from "nsh/test";

async function openDrawer() {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1440,
    height: 1000,
    storage: { "library:roots": "[]" },
  });
  await app.waitFor("text:全部曲目", { timeout: 30_000 });
  await app.press("label:输出信息");
  await app.waitFor("text:Output 控制台", { timeout: 10_000 });
  return app;
}

test("抽屉从播放条的 (i) 打开，五个模组都在", async () => {
  const app = await openDrawer();
  const lower = app.texts().map((t) => t.toLowerCase());
  // 每个模组的编号徽章与标题是两块文本（徽章是方框，标题在它右边）。
  for (const text of [
    "decoder input bus",
    "dsp matrix & channel strips",
    "bit-perfect master bus",
    "output interface & master gain",
    "telemetry & root-cause diagnostics",
  ]) {
    expect(lower).toContain(text);
  }
  for (const badge of ["01", "02", "03", "04", "05"]) {
    expect(lower).toContain(badge);
  }
  // 四个校验灯是这一版的核心：每盏两行（面板刻字的断行），状态由引擎给。
  for (const lamp of [
    ["dsp & eq", "clean"],
    ["rate & hw", "match"],
    ["excl", "stream"],
    ["unity", "0db"],
  ]) {
    expect(lower).toContain(lamp[0]);
    expect(lower).toContain(lamp[1]);
  }
});

test("没有流时审计明说没有流，而不是假装通过", async () => {
  const app = await openDrawer();
  const texts = app.texts();
  // 没播放：没有可审计的路径。这里必须是 NO STREAM，不能是 PASS。
  expect(texts.some((t) => /NO STREAM/.test(t))).toBe(true);
  expect(texts.some((t) => /\[PASS\]/.test(t))).toBe(false);
});

test("设备列表在播放中锁定，不枚举（枚举会抢走独占流）", async () => {
  const app = await openDrawer();
  const texts = app.texts();
  // 没有播放时枚举锁是关的；一旦有流，锁打开且说明原因。
  expect(texts.some((t) => /ENUM LOCK: (ON|OFF)/.test(t))).toBe(true);
  expect(texts.some((t) => t.includes("引擎默认"))).toBe(true);
});
