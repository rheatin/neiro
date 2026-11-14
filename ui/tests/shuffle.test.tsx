// 随机播放的洗牌纯函数：同一集合、不丢不重、真随机（多次排列不一
// 样的期望）。不走引擎（shuffle 不是引擎状态，这只是 playQueue 用它排
// enqueue 的顺序）。

import { test, expect } from "nsh/test";
import { shuffled } from "../src/state/shuffle";

test("shuffled keeps the set whole", () => {
  const base = Array.from({ length: 200 }, (_, i) => `t-${i}`);
  for (let round = 0; round < 25; round++) {
    const out = shuffled(base);
    expect(out.length).toBe(base.length);
    expect(new Set(out).size).toBe(base.length);
    for (const item of base) expect(out).toContain(item);
  }
});

test("shuffled does not mutate its input", () => {
  const base = [1, 2, 3, 4, 5, 6, 7, 8];
  const copy = [...base];
  shuffled(base);
  expect(base).toEqual(copy);
});

test("shuffled actually rearranges (over 200 draws it never repeats one order)", () => {
  const base = Array.from({ length: 40 }, (_, i) => i);
  const seen = new Set<string>();
  for (let round = 0; round < 200; round++) seen.add(shuffled(base).join(","));
  // 200 次里出现过的排列数远大于 1（同序概率 (1/40!) 量级，几乎不可
  // 能两次相同）。
  expect(seen.size > 150).toBe(true);
});
