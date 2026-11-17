// Playback: the player's state as the UI shows it, and the intents that
// change it (spotish's store, over the Base protocol's vocabulary — no
// liked, shuffle or repeat: the engine has none, and none is invented). The
// store mirrors Base, moves the position on locally between its position
// events, and shows a control's change at once; the engine's word beats it
// once it catches up.

import { Animated, cleanup, computed, type CompositeAnimation, createAnimatedValue, effect, signal, Storage, untracked } from "nsh";
import { engine, reportEngineFailure } from "../backend/engine";
import {
  clock,
  positionAt,
  EMPTY_PLAYER,
  type Player,
  type QueueItem,
  type RepeatMode,
  type Track,
} from "../backend/types";
import { notify } from "./feedback";
import { activeSource } from "./sources";
import { trackForPath } from "./catalog";

let remoteHooks: {
  playRemoteByPath: (path: string) => boolean;
  withRemotePathByPath: (path: string, then: (fetched: string) => void) => boolean;
  resolveRemotePath?: (path: string) => string | null;
} | null = null;
export function registerRemotePlayer(hooks: NonNullable<typeof remoteHooks>): void {
  remoteHooks = hooks;
}

// ---- The player as shown ----------------------------------------------------

/** A control's change on its way: shown until the engine agrees, or a
 * moment after its request is done, or a time limit. */
interface Pending {
  change: Partial<Player>;
  started: number;
  settledAt: number | null;
}

const SETTLE_GRACE = 1500;
const PENDING_LIMIT = 8000;

const pending = signal<Pending[]>([]);

function agrees(state: Player, change: Partial<Player>): boolean {
  for (const key of Object.keys(change) as (keyof Player)[]) {
    if (key === "at") continue;
    if (key === "positionMs") {
      if (Math.abs(positionAt(state, Date.now()) - (change.positionMs ?? 0)) > 2000) return false;
      continue;
    }
    if (state[key] !== change[key]) return false;
  }
  return true;
}

/** Drops the changes the engine caught up with, or that are over. */
function prune(): void {
  const state = engine.player();
  const now = Date.now();
  const kept = pending().filter(
    (p) =>
      !agrees({ ...EMPTY_PLAYER, ...state }, p.change) &&
      now - p.started < PENDING_LIMIT &&
      (p.settledAt === null || now - p.settledAt < SETTLE_GRACE),
  );
  if (kept.length !== pending().length) pending.set(kept);
}

effect(() => {
  engine.player();
  untracked(() => prune());
});

/** The optimistic layer (values still in flight over the engine's). */
const optimistic = computed<Partial<Player>>(() => Object.assign({}, ...pending().map((p) => p.change)));

/** The player as the UI shows it: the engine's state with the values of
 * controls still on their way over it. */
export const player = computed<Player>(() => ({ ...EMPTY_PLAYER, ...engine.player(), ...optimistic() }));

// ---- The position -----------------------------------------------------------

const now = signal(Date.now());

// While playing, the clock ticks so the shown times move on: once a
// second, just as the shown second changes (the bar moves natively
// through `positionFill`).
effect(() => {
  const p = player();
  if (p.state !== "playing") return;
  let timer: ReturnType<typeof setTimeout> | null = null;
  const tick = () => {
    const t = Date.now();
    now.set(t);
    const at = positionAt(p, t);
    if (p.durationMs > 0 && at >= p.durationMs) return;
    timer = setTimeout(tick, 1000 - (at % 1000) + 4);
  };
  tick();
  cleanup(() => {
    if (timer !== null) clearTimeout(timer);
  });
});

const seekDraft = signal<number | null>(null);
const volumeDraft = signal<number | null>(null);

/** The position to show: the drag in progress, else the player's. */
export const position = (): number => seekDraft() ?? positionAt(player(), now());

/** The position as a share of the track (0..1), an animated value moving
 * natively while it plays, with no JS per frame: linear timing to the end
 * over the time left, set again on every change of where the engine is.
 * The position bar draws its range from it. */
export const positionFill = createAnimatedValue(0);
let fillAnchor = "";
let fillRun: CompositeAnimation | null = null;
/** Longer than any track: the end is held this long before the hold
 * repeats (a deadline, not frames). */
const HOLD_MS = 86_400_000;
effect(() => {
  const draft = seekDraft();
  const p = player();
  const duration = p.durationMs;
  const anchor = `${draft}|${p.positionMs}|${p.at}|${p.state}|${duration}`;
  if (anchor === fillAnchor) return;
  fillAnchor = anchor;
  const max = Math.max(1, duration);
  const at = draft ?? positionAt(p, Date.now());
  const from = Math.min(1, Math.max(0, at / max));
  fillRun?.stop();
  fillRun = null;
  positionFill.set(from);
  if (draft === null && p.state === "playing" && duration > 0 && from < 1) {
    fillRun = Animated.loop(
      Animated.sequence([
        Animated.timing(positionFill, { toValue: 1, duration: Math.max(1, duration - at), easing: "linear" }),
        Animated.delay(HOLD_MS),
      ]),
      { resetBeforeIteration: false },
    );
    void fillRun.start();
  }
});

/** The volume to show: the drag in progress, else the engine's (a value
 * the render path has not reported is not invented — the slider disables
 * instead). */
export const volumeLevel = (): number | null => {
  const p = player();
  if (volumeDraft() !== null) return volumeDraft();
  return p.volume;
};

// ---- The queue and the ready checks ------------------------------------------

/** The engine's queue (what `play(path)` put there, and its position). */
export const queue = computed(() => engine.queue());

/** Where many views follow, each on its own: a position event changes
 * none of them (every row asks whether its path is the one playing). */
const loadedPath = computed(() => player().path);
const playingNow = computed(() => player().state === "playing");
const loadedContent = computed(() => !!(player().path || player().title));

/** Whether something is loaded at all (stopped shows an empty bar). */
export const hasTrack = (): boolean => loadedContent();

/** Whether the controls can act (tracked). */
export const controlsEnabled = (): boolean => loadedContent() && player().state !== "error" && player().state !== "deviceLost";

/** Whether `path` is the track loaded (tracked). */
export function isCurrentTrack(path: string): boolean {
  if (!path) return false;
  const current = loadedPath();
  if (!current) return false;
  if (current === path) return true;
  if (path.startsWith("remote://") || current.startsWith("remote://")) {
    const nameOf = (p: string) => {
      if (p.startsWith("remote://")) return p.slice("remote://".length);
      return p.replace(/\\/g, "/").split("/").pop() ?? "";
    };
    return nameOf(path) !== "" && nameOf(path) === nameOf(current);
  }
  return false;
}

/** Whether the loaded track is playing (tracked). */
export const isPlaying = (): boolean => playingNow();

/** The engine's queue has an entry past the current one (checked per
 * queue change, not guessed). */
export const canNext = (): boolean => {
  const q = queue();
  return q.index !== null && q.index + 1 < q.queue.length;
};

export const canPrev = (): boolean => loadedContent();

export const gaplessEnabled = (): boolean => !engine.settings()?.crossfadeEnabled;

export function setGaplessEnabled(enabled: boolean): void {
  const before = engine.settings();
  if (!before) return;
  try {
    engine.setCrossfade(!enabled, before.crossfadeDurationMs, before.crossfadeCurve);
  } catch (error) {
    reportEngineFailure(error, enabled ? "启用 Gapless 失败" : "停用 Gapless 失败");
  }
}

// ---- The intents ---------------------------------------------------------------

/** Sends a control: the change shows at once, the request goes out at the
 * same time, and the engine's word catches up. A refusal drops the change
 * and says so (one plain sentence). */
function send(change: Partial<Player>, action: () => void, label = "播放失败"): void {
  const entry: Pending | null = Object.keys(change).length > 0 ? { change, started: Date.now(), settledAt: null } : null;
  if (entry) pending.update((list) => [...list, entry]);
  try {
    action();
  } catch (error) {
    if (entry) pending.update((list) => list.filter((p) => p !== entry));
    reportEngineFailure(error, label);
  }
}

export function toggle(): void {
  const p = player();
  if (!loadedContent()) return;
  if (p.state === "playing") {
    send({ state: "paused" }, () => engine.pause(), "暂停失败");
    return;
  }
  if (p.state === "paused") {
    send({ state: "playing" }, () => engine.resume(), "继续播放失败");
    return;
  }
  // `deviceLost` / `error`: the stream is gone, so `resume` has nothing to
  // resume and the transport would be dead for the rest of the session. The
  // engine's own way back is to open the track again, which is what `play`
  // does; it starts from 0, which is honest about the stream that was lost.
  if (p.path) {
    send({ state: "playing", positionMs: 0, at: Date.now() }, () => engine.play(p.path as string), "重新播放失败");
  }
}

/** Plays (the system's media controls); nothing when it plays already. */
export function play(): void {
  if (!isPlaying()) toggle();
}

export function pause(): void {
  if (isPlaying()) toggle();
}

export function trackToQueueItem(t: Track): QueueItem {
  return {
    id: t.path,
    path: t.path,
    title: t.title,
    artist: t.artist,
    durationMs: t.durationMs,
    duration_ms: t.durationMs,
  };
}

const SHUFFLE_KEY = "player:shuffle";
const REPEAT_KEY = "player:repeat";

export const repeatMode = signal<RepeatMode>("off");
export const shuffleMode = signal<boolean>(false);

// 启动读回持久化的 repeat 和 shuffle 设置，并在连接后同步给引擎
void (async () => {
  try {
    const savedShuffle = await Storage.getItem(SHUFFLE_KEY);
    if (savedShuffle !== null) {
      shuffleMode.set(savedShuffle === "1");
    }
    const savedRepeat = await Storage.getItem(REPEAT_KEY);
    if (savedRepeat === "off" || savedRepeat === "all" || savedRepeat === "one") {
      repeatMode.set(savedRepeat);
    }
  } catch (error) {
    console.warn("neiro: 播放模式持久化读取失败:", String(error));
  }
})();

// 当引擎就绪时，将持久化的模式同步给引擎
let syncedInitialModes = false;
effect(() => {
  if (engine.health().status !== "ready" || syncedInitialModes) return;
  syncedInitialModes = true;
  untracked(() => {
    try {
      if (shuffleMode()) engine.setShuffleMode(true);
      if (repeatMode() !== "off") engine.setRepeatMode(repeatMode());
    } catch (e) {
      console.warn("neiro: 初始化同步 shuffle/repeat 失败:", String(e));
    }
  });
});

// 当引擎上报 snapshot 时，单一事实源自动同步回信号
effect(() => {
  const snap = engine.queue().snapshot;
  if (!snap) return;
  untracked(() => {
    if (snap.repeatMode && snap.repeatMode !== repeatMode()) {
      repeatMode.set(snap.repeatMode);
    }
    if (snap.shuffleEnabled !== undefined && snap.shuffleEnabled !== shuffleMode()) {
      shuffleMode.set(snap.shuffleEnabled);
    }
  });
});

export function cycleRepeatMode(): void {
  const current = repeatMode();
  const next: RepeatMode = current === "off" ? "all" : current === "all" ? "one" : "off";
  setRepeatMode(next);
}

export function setRepeatMode(mode: RepeatMode): void {
  repeatMode.set(mode);
  void Storage.setItem(REPEAT_KEY, mode).catch((e) => console.warn("neiro: repeat 没有记住:", String(e)));
  try {
    engine.setRepeatMode(mode);
  } catch (e) {
    reportEngineFailure(e, "设置循环模式失败");
  }
}

export function toggleShuffle(): void {
  const next = !shuffleMode();
  shuffleMode.set(next);
  void Storage.setItem(SHUFFLE_KEY, next ? "1" : "0").catch((e) => console.warn("neiro: shuffle 没有记住:", String(e)));
  try {
    engine.setShuffleMode(next);
  } catch (e) {
    reportEngineFailure(e, "设置随机播放失败");
  }
}

export function playNextTrack(track: Track): void {
  if (track.path.startsWith("remote://")) {
    if (
      remoteHooks?.withRemotePathByPath(track.path, (cachedPath) => {
        playNextTrack({ ...track, path: cachedPath });
      })
    ) {
      return;
    }
  }
  engine.playNext([trackToQueueItem(track)]);
  notify(`已设为下一首播放：${track.title || "曲目"}`);
}

export function playLaterTrack(track: Track): void {
  if (track.path.startsWith("remote://")) {
    if (
      remoteHooks?.withRemotePathByPath(track.path, (cachedPath) => {
        playLaterTrack({ ...track, path: cachedPath });
      })
    ) {
      return;
    }
  }
  engine.playLater([trackToQueueItem(track)]);
  notify(`已设为稍后播放：${track.title || "曲目"}`);
}

/** Plays one track: the queue becomes it and it starts. What shows the
 * moment it is picked is itself, playing, until the engine names its own
 * state. */
export function playTrack(track: Track): void {
  if (track.path.startsWith("remote://")) {
    if (
      remoteHooks?.withRemotePathByPath(track.path, (cachedPath) => {
        playTrack({ ...track, path: cachedPath });
      })
    ) {
      return;
    }
  }
  const at = Date.now();
  send(
    {
      path: track.path,
      title: track.title,
      artist: track.artist,
      album: track.album,
      state: "playing",
      positionMs: 0,
      durationMs: track.durationMs,
      at,
    },
    () => {
      engine.playContext([trackToQueueItem(track)], 0);
    },
    "播放失败",
  );
}

/** 点一首歌时,后面跟着进队列的曲目数。
 *
 * 十首:一张列表里点歌的人要的是"接着放",不是"把四千首放进来"——队列面
 * 板要能一眼看完,而且引擎的队列是可编辑的,长队只会让编辑变难。 */
export const QUEUE_AHEAD = 10;

/** 从列表点第 `index` 首:它开始播,后面 `QUEUE_AHEAD` 首按列表顺序进队
 * 列。换列表就换一批——队列属于"我现在在听的这张列表"。 */
export function playFromRows(rows: Track[], index: number): void {
  if (rows.length === 0) return;
  playQueue(rows, index);
}

/** 播放全部：play(点击那首/第一首) + enqueue(其余)。shuffle 开 → 其余
 * Fisher–Yates 洗牌（同一集合不重复）；关 → 原序。支持本地、远程与混合队列。 */
export function playQueue(rows: Track[], firstIndex = 0): void {
  if (rows.length === 0) return;
  const idx = Math.min(Math.max(firstIndex, 0), rows.length - 1);
  const first = rows[idx];

  if (first.path.startsWith("remote://")) {
    if (
      remoteHooks?.withRemotePathByPath(first.path, (cachedFirstPath) => {
        const updated = [...rows];
        updated[idx] = { ...first, path: cachedFirstPath };
        playQueue(updated, idx);
      })
    ) {
      return;
    }
  }

  // 首曲已是本地/已缓存。检查首曲之后的待播项是否存在尚未缓存的 remote:// 曲目：
  const subsequentRows = rows.slice(idx + 1);
  const hasUncachedRemote = subsequentRows.some(
    (t) => t.path.startsWith("remote://") && !remoteHooks?.resolveRemotePath?.(t.path),
  );

  if (hasUncachedRemote) {
    // 首曲启动后对后续最多 QUEUE_AHEAD（10 首）按序通过 withRemotePathByPath 串行拉取并调用 engine.playLater 入队
    engine.playContext([trackToQueueItem(first)], 0);
    const rest = subsequentRows.slice(0, QUEUE_AHEAD);
    const step = (i: number) => {
      if (i >= rest.length) return;
      const t = rest[i];
      if (t.path.startsWith("remote://")) {
        const resolved = remoteHooks?.resolveRemotePath?.(t.path);
        if (resolved) {
          try {
            engine.playLater([trackToQueueItem({ ...t, path: resolved })]);
          } catch (e) {
            console.warn("neiro: 远程排队失败:", String(e));
          }
          step(i + 1);
        } else {
          const handled = remoteHooks?.withRemotePathByPath(t.path, (cachedPath) => {
            try {
              engine.playLater([trackToQueueItem({ ...t, path: cachedPath })]);
            } catch (e) {
              console.warn("neiro: 远程排队失败:", String(e));
            }
            step(i + 1);
          });
          if (!handled) {
            step(i + 1);
          }
        }
      } else {
        try {
          engine.playLater([trackToQueueItem(t)]);
        } catch (e) {
          console.warn("neiro: 排队失败:", String(e));
        }
        step(i + 1);
      }
    };
    step(0);
  } else {
    const validItems: QueueItem[] = [];
    let adjustedIndex = 0;
    for (let i = 0; i < rows.length; i++) {
      const t = rows[i];
      if (i === idx) {
        adjustedIndex = validItems.length;
        validItems.push(trackToQueueItem(t));
      } else if (t.path.startsWith("remote://")) {
        const resolved = remoteHooks?.resolveRemotePath?.(t.path);
        if (resolved) {
          validItems.push(trackToQueueItem({ ...t, path: resolved }));
        }
      } else {
        validItems.push(trackToQueueItem(t));
      }
    }

    if (validItems.length > 0) {
      engine.playContext(validItems, adjustedIndex);
    }
  }
}

export function playQueueAt(index: number): void {
  const q = queue();
  if (index < 0 || index >= q.queue.length) return;
  const target = q.queue[index];
  const after = q.queue.slice(index + 1);
  const tr = trackForPath(target);
  if (tr) {
    playTrack(tr);
  } else {
    send(
      {
        path: target,
        state: "playing",
        positionMs: 0,
        at: Date.now(),
      },
      () => engine.play(target),
      "播放失败",
    );
  }
  if (after.length > 0) {
    try {
      engine.enqueue(after);
    } catch (e) {
      reportEngineFailure(e, "入队失败");
    }
  }
}

export function prev(): void {
  if (!canPrev()) return;
  try {
    engine.previous();
  } catch (error) {
    reportEngineFailure(error, "上一曲失败");
  }
}

export function next(): void {
  if (!canNext()) return;
  try {
    engine.next();
  } catch (error) {
    reportEngineFailure(error, "下一曲失败");
  }
}

/** The position drag moved: shown, not sent. */
export function dragSeek(ms: number): void {
  seekDraft.set(ms);
}

/** Seeks to `ms` (the drag ended there). */
export function seek(ms: number): void {
  seekDraft.set(null);
  send({ positionMs: ms, at: Date.now() }, () => engine.seek(ms / 1000), "跳转失败");
}

/** The volume drag moved: shown, not sent. */
export function dragVolume(value: number | null): void {
  volumeDraft.set(value);
}

/** Sets the volume (the drag ended there). */
export function setVolume(value: number): void {
  volumeDraft.set(null);
  send({ volume: value }, () => engine.setVolume(value), "音量失败");
}

/** Volume up one notch (the keyboard's command). */
export function bumpVolume(amount: number): void {
  const current = volumeLevel();
  if (current === null) return;
  setVolume(Math.min(1, Math.max(0, current + amount)));
}

/** `m:ss` of where it is now, for the clock beside the bar. */
export const positionClock = (): string => clock(position());

export const durationClock = () => clock(player().durationMs, hasTrack());
