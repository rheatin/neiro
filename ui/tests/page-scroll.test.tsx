// 切走 tab 再回来：页面停在离开时的位置（nav 的根路由替换会卸载页面），
// 滚离顶部时右下角浮出“回到顶部”，点一下回到顶。
import { expect, open, test } from "nsh/test";

test("切走 tab 再回来，页面停在原来的位置；回到顶部按钮可以回顶", async () => {
  const app = await open("./page-scroll-harness.tsx", {
    platform: "windows",
    width: 1280,
    height: 800,
  });

  // 1. 曲目页在顶部：没有“回到顶部”
  await app.press("label:曲目页");
  await app.waitFor("id:page-tracks", { timeout: 10_000 });
  await app.waitFor("text:Track 000", { timeout: 10_000 });
  expect(app.shows("label:回到顶部")).toBe(false);

  // 2. 滚下去，按钮浮出
  await app.scroll("id:page-tracks", 900);
  await app.waitFor("label:回到顶部", { timeout: 10_000 });

  // 3. 切到别的 tab，再切回来：容器回到原来的位置（它报告的位置仍滚离顶部，
  //    按钮才在），而不是从顶部重来
  await app.press("label:首页页");
  await app.waitFor("id:page-home", { timeout: 10_000 });
  expect(app.shows("label:回到顶部")).toBe(false);

  await app.press("label:曲目页");
  await app.waitFor("id:page-tracks", { timeout: 10_000 });
  await app.waitFor("label:回到顶部", { timeout: 10_000 });

  // 4. 点它回到顶：第一行回来，按钮消失
  await app.press("label:回到顶部");
  await app.waitFor("text:Track 000", { timeout: 10_000 });
  await app.waitFor("label:回到顶部", { state: "gone", timeout: 10_000 });
  expect(app.shows("label:回到顶部")).toBe(false);
}, 90_000);
