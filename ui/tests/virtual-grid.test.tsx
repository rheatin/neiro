// 专辑网格虚拟化：400 张专辑时只渲染视口附近的卡片，滚动到底才渲染末尾的卡片。
import { expect, open, test } from "nsh/test";

test("专辑网格只渲染视口附近的卡片（400 张也不会一次全建）", async () => {
  const app = await open("./virtual-grid-harness.tsx", { platform: "macos", width: 1280, height: 837 });
  await app.press("label:专辑页");
  await app.waitFor("id:page-albums", { timeout: 20_000 });
  await app.waitFor("label:V Album 000, V Artist · 2024", { timeout: 10_000 });

  // 末尾的卡片不在视口附近：虚拟化后根本没有建。
  expect(app.shows("label:V Album 399, V Artist · 2024")).toBe(false);

  // 滚到底，末尾的卡片才出现，开头的不再在。
  await app.scroll("id:page-albums", 12_000);
  await app.waitFor("label:V Album 399, V Artist · 2024", { timeout: 10_000 });
  expect(app.shows("label:V Album 000, V Artist · 2024")).toBe(false);
}, 90_000);
