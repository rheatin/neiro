// 待播列表（引擎的队列，UI 侧只读它、不复制它）与"批量动作"的两块状态：
//
// - `queue`/`queueIndex` 直接读引擎的 `onQueue`，不另存一份；`待播` 是
//   当前这条之后的部分（引擎的 queue 含当前）。
// - 多选：一张页面级的选中集（按 path）。进入多选后行不再"点击即播放"，
//   点击改选中；顶部出现一条批量条（加入待播 / 加入曲单 / 收藏）。
//
// 两个都放这里，因为曲目表、专辑详情、收藏页、远程页都要用。

import { computed, signal } from "nsh";
import { engine, reportEngineFailure } from "../backend/engine";
import { notify } from "./feedback";
import { queue as engineQueue } from "./player";

/** 引擎的队列（含当前这条）。 */
export const queue = computed(() => engineQueue());

/** 当前这条之后的部分：真正意义上的"待播"。 */
export const upNext = computed(() => {
  const q = queue();
  const index = q.index;
  if (index === null) return [];
  return q.queue.slice(index + 1);
});

/** 当前这条在队列里的位置（`null`：没有在播的）。 */
export const queueIndex = computed(() => queue().index);

/** 加入待播：把这些路径追加到引擎队列的末尾。已经在队列里的不重复加
 * （引擎的队列是事实，重复项会在播放时表现为同一首播两遍）。 */
export function addToQueue(paths: string[]): void {
  const list = paths.filter((p) => p !== "");
  if (list.length === 0) return;
  const already = new Set(queue().queue);
  const fresh = list.filter((p) => !already.has(p));
  if (fresh.length === 0) {
    notify("已在待播列表里");
    return;
  }
  try {
    engine.enqueue(fresh);
    notify(`已加入待播：${fresh.length} 首`);
  } catch (error) {
    reportEngineFailure(error, "加入待播失败");
  }
}

/** 从队列里移除一条（引擎拒绝移除正在播放的那条）。 */
export function removeFromQueue(index: number): void {
  try {
    engine.removeFromQueue(index);
  } catch (error) {
    reportEngineFailure(error, "移出待播失败");
  }
}

/** 清空待播（保留正在播放的那条）。 */
export function clearUpNext(): void {
  try {
    engine.clearQueue();
  } catch (error) {
    reportEngineFailure(error, "清空待播失败");
  }
}

// ---- 多选 ---------------------------------------------------------------------

/** 多选是否开着。 */
const selecting = signal(false);
/** 选中的 path，按选择顺序。 */
const selected = signal<string[]>([]);

export const selectionActive = (): boolean => selecting();
export const selectionPaths = (): string[] => selected();
export const selectionCount = (): number => selected().length;
export const isSelected = (path: string | undefined): boolean => !!path && selected().includes(path);

/** 进入多选（可带一个初始选中项：按住 Shift 点行、或从菜单"选择"进入）。 */
export function beginSelection(path?: string): void {
  selecting.set(true);
  if (path) selected.set([path]);
}

export function endSelection(): void {
  selecting.set(false);
  selected.set([]);
}

/** 点一行时的选中切换（多选中）。 */
export function toggleSelected(path: string): void {
  if (!path) return;
  selected.update((list) => (list.includes(path) ? list.filter((p) => p !== path) : [...list, path]));
}

/** 把一批 path 全选/全不选（表头的"全选"）。 */
export function setSelected(paths: string[]): void {
  selected.set([...paths]);
}

