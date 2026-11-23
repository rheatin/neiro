// 滚动位置的记忆：侧栏切换 tab 时，nav 的根路由替换整个栈，旧页随之卸载；
// 回来时按离开前停下的位置重新打开，而不是从顶部重来。位置按页面的
// “内容身份”记（来源、文件夹一并进键里），同一页面换了内容不会串位。

import { signal, type ListRef, type ScrollEvent, type ScrollRef } from "nsh";

type Scroller = ScrollRef | ListRef;

/** 每个页面容器停下的纵向位置（页面还活着时也一直更新，但只有重新挂载
 * 时才读一次）。 */
const offsets = new Map<string, number>();

/** 离顶部超过这么多点，右下角才浮出“回到顶部”。 */
const AWAY_FROM_TOP = 300;

/** A page's scroll container, remembered: pass the props to the page's
 * `<Scroll>` or `<List>`, and the handle to `AppPage`. */
export interface PageScroll {
  /** Where the container starts (the position it was left at), or `undefined`
   * for a page opened at the top: its resting offset is not 0 on every
   * platform (iOS's system header insets the scroll view), so not asking for
   * an offset is how a page starts at its top. */
  contentOffset: { y: number } | undefined;
  /** The page is opening where it was left, not for the first time: its
   * entrance animations (the rows' wave) are skipped, so it reads as
   * arriving back rather than loading afresh. */
  restored: boolean;
  onScroll: (e: ScrollEvent) => void;
  ref: (node: Scroller) => void;
  /** The page is scrolled away from its top. */
  showTop: () => boolean;
  /** Back to the top, animated. */
  toTop: () => void;
}

/** Remembers the scroll position of the page whose container is
 * `key` (e.g. `"albums:local"`). Call once per page mount. */
export function rememberScroll(key: string): PageScroll {
  const start = offsets.get(key) ?? 0;
  // 按钮等容器真的报出位置（挂载时 contentOffset 先生效，onScroll 随之而
  // 来）再决定出不出：恢复失败（内容变短）时它不会凭空出现。
  const away = signal(false);
  let node: Scroller | null = null;
  return {
    contentOffset: start !== 0 ? { y: start } : undefined,
    restored: start > 0,
    onScroll: (e) => {
      offsets.set(key, e.offset.y);
      away.set(e.offset.y > AWAY_FROM_TOP);
    },
    ref: (next) => {
      // 同页的容器换班（网格↔列表、本地↔远程）：新容器接手，旧的不清掉。
      if (next) node = next;
    },
    showTop: () => away(),
    toTop: () => {
      offsets.set(key, 0);
      away.set(false);
      node?.scrollTo({ y: 0, animated: true });
    },
  };
}
