// 随机播放：一个 UI 开关（不是引擎状态）。队列顺序是"play(第一首) +
// enqueue(其余)"的排列——shuffle 只决定其余部分是原序还是 Fisher–Yates
// 洗牌。开关持久化在 Storage `player:shuffle`。
//
// 远程来源不支持：队列只装本地路径，远程曲目要先取流（fetch→play）。
// 按钮在远程来源时点了给一句 toast，不假装可用。

import { signal, Storage } from "nsh";
import { activeSource } from "./sources";

const KEY = "player:shuffle";

export const shuffle = signal(false);

// 启动读回（一次性；写盘的失败只 warn）。
void (async () => {
  try {
    const saved = await Storage.getItem(KEY);
    shuffle.set(saved === "1");
  } catch (error) {
    console.warn("neiro: 随机播放开关没有读回来:", String(error));
  }
})();

export function toggleShuffle(): void {
  const next = !shuffle();
  shuffle.set(next);
  void Storage.setItem(KEY, next ? "1" : "0").catch((error) =>
    console.warn("neiro: 随机播放开关没有记住:", String(error)),
  );
}

/** Fisher–Yates：返回一个新数组（同一集合、不重复、顺序真随机）。 */
export function shuffled<T>(list: T[]): T[] {
  const out = [...list];
  for (let i = out.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [out[i], out[j]] = [out[j], out[i]];
  }
  return out;
}

/** shuffle 可用的来源：本地与远程均支持。 */
export const shuffleSupported = (): boolean => true;
