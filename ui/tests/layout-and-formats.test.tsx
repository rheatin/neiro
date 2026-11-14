// 三处版式/命名改动的回归：窄侧栏纯图标、E-AC-3 Atmos 识别、窄窗歌词独占。

import { open, test, expect } from "nsh/test";
import { codecDisplayName, isAtmos, isLossless, lossyCodec } from "../src/components/ui/format-badges";

const PLAY_FILE = "C:\\Users\\Char\\AppData\\Local\\Temp\\ado01.m4a";

test("E-AC-3 / Atmos 不再被叫成 AAC", () => {
  expect(codecDisplayName("eac3")).toBe("E-AC-3");
  expect(lossyCodec("track.m4a", "eac3")).toBe("E-AC-3");
  expect(lossyCodec("track.m4a")).toBe("AAC"); // 没有引擎答案时才退回扩展名
  expect(isAtmos("Dolby Digital Plus + Dolby Atmos")).toBe(true);
  expect(isAtmos("Dolby Digital Plus")).toBe(false);
  expect(isAtmos("")).toBe(false);
  expect(isLossless("track.m4a", "eac3")).toBe(false);
  expect(isLossless("track.m4a", "alac")).toBe(true);
});

test("窄侧栏：来源只剩图标，动作仍可达", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 900,
    height: 900,
    storage: { "library:roots": "[]" },
  });
  // One 曲库 entry and one 加号; the two kinds of source live behind the 加号.
  await app.waitFor("id:rail-source-add");
  const texts = app.texts();
  expect(texts.includes("本地")).toBe(false);
  expect(texts.some((t) => t.includes("还没有文件夹"))).toBe(false);
  expect(texts.includes("远程")).toBe(false);
  expect(app.shows("id:rail-source-library")).toBe(true);
  await app.press("id:rail-source-add");
  await app.waitFor("id:rail-source-add-folder", { timeout: 5_000 });
  expect(app.shows("id:rail-source-add-server")).toBe(true);
  expect(app.shows("label:添加文件夹")).toBe(true);
}, 60_000);

test("宽侧栏：来源文字仍在", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
    storage: { "library:roots": "[]" },
  });
  await app.waitFor("text:本地");
  expect(app.texts().includes("远程")).toBe(true);
  expect(app.shows("id:rail-source-add-folder")).toBe(false);
}, 60_000);

