import { expect, test } from "nsh/test";
import { EMPTY_PLAYER, type Player } from "../src/backend/types";
import { shouldAdvanceClock } from "../src/state/player";

function makePlayer(overrides: Partial<Player> = {}): Player {
  return { ...EMPTY_PLAYER, ...overrides };
}

test("shouldAdvanceClock 仅在引擎实际出声推进后才允许走表", () => {
  const remotePath = "remote://abc.m4a";
  const shownLoading = makePlayer({
    state: "playing",
    path: remotePath,
    positionMs: 0,
    durationMs: 240_000,
  });

  // 引擎仍停留在 stopped 或上一首曲目
  expect(shouldAdvanceClock(shownLoading, makePlayer({ state: "stopped", path: null, positionMs: 0 }))).toBe(false);
  expect(
    shouldAdvanceClock(shownLoading, makePlayer({ state: "playing", path: "C:/Music/prev.flac", positionMs: 42_000 })),
  ).toBe(false);

  // 引擎刚切到新曲目但尚未渲染出第一帧（positionMs === 0）
  expect(shouldAdvanceClock(shownLoading, makePlayer({ state: "playing", path: remotePath, positionMs: 0 }))).toBe(
    false,
  );

  // 引擎已实际出声（positionMs > 0），且支持 remote:// 与本地缓存文件basename匹配
  expect(shouldAdvanceClock(shownLoading, makePlayer({ state: "playing", path: remotePath, positionMs: 120 }))).toBe(
    true,
  );
  expect(
    shouldAdvanceClock(
      shownLoading,
      makePlayer({ state: "playing", path: "C:/Cache/remote/audio/server/abc.m4a", positionMs: 150 }),
    ),
  ).toBe(true);

  // 乐观 seek 尚未被引擎追上时不提前推进，追上后恢复
  const shownSeek = makePlayer({ state: "playing", path: remotePath, positionMs: 90_000, durationMs: 240_000 });
  expect(shouldAdvanceClock(shownSeek, makePlayer({ state: "playing", path: remotePath, positionMs: 10_000 }))).toBe(
    false,
  );
  expect(shouldAdvanceClock(shownSeek, makePlayer({ state: "playing", path: remotePath, positionMs: 90_100 }))).toBe(
    true,
  );

  // 暂停或停止不走表
  expect(shouldAdvanceClock(shownLoading, makePlayer({ state: "paused", path: remotePath, positionMs: 5_000 }))).toBe(
    false,
  );
});
