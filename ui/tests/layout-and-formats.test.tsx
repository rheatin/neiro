// 三处版式/命名改动的回归：窄侧栏纯图标、E-AC-3 Atmos 识别、窄窗歌词独占。

import { open, test, expect } from "nsh/test";
import { codecDisplayName, gradeOf, isAtmos, isLossless, lossyCodec } from "../src/components/ui/format-badges";

const PLAY_FILE = "../analysis_tone.wav";

test("E-AC-3 / Atmos 不再被叫成 AAC，44.1 kHz 不会误判为 HI-RES+", () => {
  expect(codecDisplayName("eac3")).toBe("E-AC-3");
  expect(lossyCodec("track.m4a", "eac3")).toBe("E-AC-3");
  expect(lossyCodec("track.m4a")).toBe("AAC"); // 没有引擎答案时才退回扩展名
  expect(isAtmos("Dolby Digital Plus + Dolby Atmos")).toBe(true);
  expect(isAtmos("Dolby Digital Plus")).toBe(false);
  expect(isAtmos("")).toBe(false);
  expect(isLossless("track.m4a", "eac3")).toBe(false);
  expect(isLossless("track.m4a", "alac")).toBe(true);
  expect(gradeOf(44_100, 16, false)).toBe("CD");
  expect(gradeOf(44_100, 24, false)).toBe("CD");
  expect(gradeOf(44_100, 32, false)).toBe("CD");
  expect(gradeOf(96_000, 24, false)).toBe("HI-RES");
  expect(gradeOf(192_000, 24, false)).toBe("HI-RES+");
});

test("窄侧栏：来源只剩图标，动作仍可达", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 900,
    height: 900,
    storage: { "library:roots": "[]" },
  });
  // With no source added, the rail's 来源 panel is the 加号 alone.
  await app.waitFor("id:rail-source-add");
  const texts = app.texts();
  expect(texts.includes("本地")).toBe(false);
  expect(texts.some((t) => t.includes("还没有文件夹"))).toBe(false);
  expect(texts.includes("远程")).toBe(false);
  expect(app.shows("id:source-server")).toBe(false);
  expect(app.shows("id:source-root")).toBe(false);
  await app.press("id:rail-source-add");
  // The 加号 opens the kind dialog: jellyfin, navidrome, and a local folder.
  await app.waitFor("text:本地路径", { timeout: 5_000 });
  expect(app.shows("text:Jellyfin 密码")).toBe(true);
}, 60_000);

test("宽侧栏：来源文字仍在", async () => {
  const app = await open("../App.tsx", {
    platform: "windows",
    width: 1280,
    height: 837,
    storage: { "library:roots": "[]" },
  });
  // Wide, the 加号 carries its label and the rows their names; the headings
  // 本地/远程 are gone — a source row says what it is by its icon and name.
  await app.waitFor("text:添加来源");
  expect(app.shows("label:添加来源")).toBe(true);
}, 60_000);

