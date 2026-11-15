import { expect, open, test } from "nsh/test";

test("DITHER 是算法开关，不是位深选择器", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1440,
    height: 1000,
    storage: { "library:roots": "[]" },
    host: { params: { debugPlay: "../analysis_tone.wav" } },
  });
  await app.waitFor("label:暂停", { timeout: 25_000 });
  await app.press("label:输出信息");
  await app.waitFor("text:Output 控制台", { timeout: 10_000 });
  await app.wait(1200);
  const texts = app.texts();
  console.log("dither keys:", JSON.stringify(texts.filter((t) => /^(OFF|TPDF|SHAPED|AUTO|16b|24b|32b)$/.test(t))));
  // 位深不再是按键
  expect(texts.some((t) => t === "16b" || t === "24b" || t === "32b")).toBe(false);
  // 遥测里只读展示位深（含 zero-pad / dither 标注）
  console.log("depth line:", JSON.stringify(texts.filter((t) => /Container|Zero-Padded|DITHERED|bit/.test(t)).slice(0, 6)));
}, 120_000);
