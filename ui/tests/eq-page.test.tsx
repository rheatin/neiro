import { expect, open, test } from "nsh/test";

test("机架 EQ：预设、A/B、OPRA、另存为", async () => {
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
  console.log("rack texts:", JSON.stringify(app.texts().filter((t) => /RACK|BANDS|PRESET|OPRA|A\/B|PEAK/.test(t)).slice(0, 12)));
  // preset
  await app.press("label:预设 低音增强");
  await app.wait(400);
  console.log("preset on:", app.shows("label:预设 低音增强"));
  // A/B
  await app.press("label:A/B 对比");
  await app.wait(400);
  console.log("ab:", app.texts().filter((t) => /A\/B/.test(t)));
  // OPRA
  await app.press("label:耳机曲线 Harman 目标");
  await app.wait(400);
  await app.press("label:启用 OPRA");
  await app.wait(400);
  console.log("opra:", app.texts().filter((t) => /OPRA/.test(t)));
  // save
  await app.press("label:另存为预设");
  await app.waitFor("label:预设名", { timeout: 8_000 });
  await app.type("label:预设名", "我的机架");
  await app.press("text:保存");
  await app.wait(400);
  expect(app.shows("label:预设 我的机架")).toBe(true);
}, 120_000);
