import { open, test, expect } from "nsh/test";

test("the 输出信息 drawer opens and paints its sections", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
    storage: { "library:roots": "[]" },
  });
  await app.waitFor("text:全部曲目");
  await app.press("label:输出信息");
  await app.waitFor("text:Output 控制台", { timeout: 10_000 });
  const texts = app.texts();
  expect(texts).toContain("信号链路");
  expect(texts).toContain("输出与设备");
  expect(texts).toContain("Bit-Perfect 校验");
}, 60_000);
