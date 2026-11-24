// 输出信息抽屉：从播放条的 (i) 打开，现代精密信号链路面板各模块契约成立。

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

test("抽屉从播放条的 (i) 打开，现代信号链路各卡片都在", async () => {
  const app = await openDrawer();
  const texts = app.texts();
  for (const heading of ["信号链路", "输出与设备", "Bit-Perfect 校验", "音效处理与工程遥测"]) {
    expect(texts).toContain(heading);
  }
  for (const badge of ["DSP 旁通", "采样一致", "独占直通", "音量 100%"]) {
    expect(texts).toContain(badge);
  }
});

test("没有流时审计明说没有流，而不是假装通过", async () => {
  const app = await openDrawer();
  const texts = app.texts();
  expect(texts.some((t) => /NO STREAM/.test(t))).toBe(true);
  expect(texts.some((t) => /整条通路未改动/.test(t))).toBe(false);
});

test("设备列表与高级折叠区契约成立", async () => {
  const app = await openDrawer();
  const texts = app.texts();
  expect(texts.some((t) => t.includes("引擎默认"))).toBe(true);
  expect(texts.some((t) => t === "可刷新设备" || t === "播放中锁定枚举")).toBe(true);

  await app.press("id:output-advanced-toggle");
  await app.waitFor("text:Dither 抖动算法", { timeout: 5_000 });
  const expandedTexts = app.texts();
  for (const algo of ["AUTO", "OFF", "TPDF", "SHAPED"]) {
    expect(expandedTexts).toContain(algo);
  }
});

test("移动端与折叠屏展开时 Output 控制台全屏展开", async () => {
  for (const vp of [
    { width: 390, height: 844 },
    { width: 720, height: 840 },
  ]) {
    const app = await open("../App.tsx", {
      platform: "ios",
      width: vp.width,
      height: vp.height,
      safeArea: { top: 47, bottom: 34, left: 0, right: 0 },
      storage: { "library:roots": "[]" },
    });
    await app.waitFor("text:首页", { timeout: 30_000 });
    await app.press("label:设置");
    await app.waitFor("text:打开输出控制台", { timeout: 10_000 });
    await app.press("text:打开输出控制台");
    await app.waitFor("text:Output 控制台", { timeout: 10_000 });

    const texts = app.texts();
    expect(texts).toContain("信号链路");
    expect(texts).toContain("Bit-Perfect 校验");
    expect(texts).toContain("输出与设备");
    expect(app.shows("label:关闭输出信息")).toBe(true);
    await app.press("label:关闭输出信息");
    await app.wait(300);
    expect(app.shows("text:Output 控制台")).toBe(false);
  }
});
