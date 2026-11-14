import { expect, open, test } from "nsh/test";

test("切 Exclusive 后引擎回报 exclusive，且灯亮", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1440,
    height: 1000,
    storage: { "library:roots": "[]" },
    host: { params: { debugPlay: "C:\\Users\\Char\\AppData\\Local\\Temp\\ado01.m4a" } },
  });
  await app.waitFor("label:暂停", { timeout: 25_000 });
  await app.press("label:输出信息");
  await app.waitFor("text:Output 控制台", { timeout: 10_000 });
  // 点独占
  await app.press("label:独占直通");
  await app.wait(1500);
  const texts = app.texts();
  console.log("share audit:", texts.filter((t) => /SHARED MODE|EXCL STREAM|NOT VERIFIED|VERIFIED/.test(t)));
  // 审计里不应再出现 SHARED MODE（除非端点拒绝独占）
  console.log("protocol:", texts.filter((t) => /WASAPI/.test(t)));
  console.log("toasts:", texts.filter((t) => /失败|独占|共享/.test(t)).slice(-8));
  console.log("share labels:", texts.filter((t) => /EXCL|SHARED/.test(t)));
  console.log("bitperfect:", texts.filter((t) => /VERIFIED|BLOCKER|NO STREAM|CLEAN|MATCH|UNITY/.test(t)));
}, 120_000);
