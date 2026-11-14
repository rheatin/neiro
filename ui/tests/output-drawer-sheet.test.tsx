import { open, test, expect } from "nsh/test";

test("the 输出信息 drawer opens and paints its sections", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
    storage: { "library:roots": "[]" },
  });
  await app.waitFor("text:全部曲目");
  // 通过 (i)（真机同一链路）；等控台自己的模组标题挂上为止。
  await app.press("label:输出信息");
  const lower = () => app.texts().map((t) => t.toLowerCase());
  await app.waitUntil(() => lower().some((t) => t === "decoder input bus"), { timeout: 10_000 });
  // The surface paints its own chassis: the five module headers are the
  // contract this test holds, not one control's label.
  expect(lower().filter((t) => t.startsWith("0")).length).toBeGreaterThanOrEqual(4);
}, 60_000);
