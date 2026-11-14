import { expect, open, test } from "nsh/test";

test("从控台进 EQ，返回回到控台", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1440,
    height: 1000,
    storage: { "library:roots": "[]" },
  });
  await app.waitFor("text:全部曲目", { timeout: 20_000 });
  await app.press("label:输出信息");
  await app.waitFor("text:Output 控制台", { timeout: 10_000 });
  await app.press("label:打开 32 段均衡器机架");
  await app.waitFor("id:page-eq", { timeout: 10_000 });
  console.log("on eq page:", app.shows("id:page-eq"));
  // 返回：应回到控台（抽屉重新打开），而不是播放页
  await app.press("label:返回");
  await app.wait(600);
  console.log("eq page gone:", !app.shows("id:page-eq"));
  console.log("drawer back:", app.shows("text:Output 控制台"));
  expect(app.shows("text:Output 控制台")).toBe(true);
}, 120_000);
