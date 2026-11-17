// 随机播放：统一由 player.ts 的单一事实源与引擎 QueueDirector 保持同步。

import { shuffleMode, toggleShuffle } from "./player";

export const shuffle = shuffleMode;
export { toggleShuffle };

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

