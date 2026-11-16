// The app's entry: connects the engine once, picks the shell (a desktop
// window, or one honest page for every other idiom), and mounts what
// shows over every shell — Now Playing, the 输出信息 drawer, the toasts —
// with the keyboard shortcuts. Pages and stores live under `src/`.

import { effect, host, keymap, mount, untracked, View } from "nsh";
import { engine } from "./src/backend/engine";
import { health } from "./src/state/engine";
import { layout, UnsupportedPage } from "./src/components/layout";
import { DesktopShell } from "./src/shells/desktop";
import { StudioMinimalShell } from "./src/shells/studio-minimal";
import { PluginManagerDialog } from "./src/components/ui/plugin-dialog";
import { NowPlayingSheet } from "./src/components/player/now-playing";
import { OutputSheet } from "./src/components/player/output-drawer";
import { enumerateAtStartup } from "./src/components/player/surface/output-interface";
import { Toaster } from "./src/components/ui/feedback";
import { ServerDialog } from "./src/components/ui/server-dialog";
import { LyricsSearchSheet } from "./src/components/player/lyrics-search";
import { LyricsWriteDialog } from "./src/components/player/lyrics-write-dialog";
import { PlaylistDialogs } from "./src/components/library/playlist-dialogs";
import { ContextMenuHost } from "./src/components/ui/context-menu";
import { QueueSheet } from "./src/components/player/queue-sheet";
import { bumpVolume, next, seek, toggle as togglePlayback } from "./src/state/player";
import { loadAdoptedLyrics, subscribeLyrics } from "./src/state/lyrics";
import { outputOpen, setOutputOpen, setPluginDialogOpen } from "./src/state/overlays";
import {
  activeShellId,
  createPluginContext,
  DEFAULT_SHELL_ID,
  getExternalUiShell,
  isStudioMinimalShell,
  type NeiroPluginContext,
  plugins,
  refreshPlugins,
  shellError,
  switchShell,
} from "./src/state/plugins";

// The engine connects once, at the root: a throw becomes the failed
// health (the honest "引擎未连接" page), never an unmounted app.
if (health().status === "starting") engine.connect();

// Lyric text arrives as a tag answer; one subscription carries every answer.
subscribeLyrics();
// Words chosen from the online search are remembered across launches.
void loadAdoptedLyrics();

// Enumerate the endpoints once, while nothing is playing: an endpoint list is a
// fact about the machine that cannot be read while a stream holds a device.
effect(() => {
  if (health().status !== "ready") return;
  untracked(() => {
    enumerateAtStartup();
    refreshPlugins();
  });
});

function Shortcuts() {
  // Media-style keys that leave text editing alone: none of them fires
  // while a text input has the focus. Escape belongs to the overlays and
  // sheets (they own it natively). The protocol has next in one direction
  // only, so there is no 上一曲 binding to give it.
  keymap.bind("space", () => togglePlayback(), { when: "notEditing" });
  keymap.bind("f8", () => togglePlayback(), { when: "notEditing" });
  keymap.bind("f9", () => next(), { when: "notEditing" });
  keymap.bind("f7", () => seek(0), { when: "notEditing" });
  keymap.bind("mod+shift+right", () => next(), { when: "notEditing" });
  keymap.bind("mod+up", () => bumpVolume(0.1), { when: "notEditing" });
  keymap.bind("mod+down", () => bumpVolume(-0.1), { when: "notEditing" });
  // 只给无头测试用：mod+k 开/关"输出信息"抽屉（真机一样绑定，不碍事）。
  keymap.bind("mod+k", () => setOutputOpen(!outputOpen()), { when: "notEditing" });

  // 宿主级保留全局快捷键：无论当前挂载哪个 UI 壳层，随时开启插件/壳层管理器逃生
  keymap.bind("mod+shift+,", () => setPluginDialogOpen(true));
  keymap.bind("shift+escape", () => setPluginDialogOpen(true));
  keymap.bind("f12", () => setPluginDialogOpen(true));
  return <View class="hidden" />;
}

let activeShellCtx: NeiroPluginContext | null = null;

function App() {
  // 一个只给测试的钩子：`open(…, { host: { params: { debugPlay: <path> } } }`
  // 让根 id 在连上后立即播放这一个文件（无头 UI 测试的最短路径，不走扫
  // 描）。真实启动没有 params，是 no-op。
  effect(() => {
    const params = (host.params ?? {}) as Record<string, unknown>;
    const stop = effect(() => {
      if (health().status !== "ready") return;
      untracked(() => {
        const path = params.debugPlay;
        if (typeof path === "string" && path !== "") {
          try {
            engine.play(path);
          } catch (error) {
            console.warn("neiro-test: 播放失败:", String(error));
          }
        }
      });
    });
    return stop;
  });

  effect(() => {
    const currentId = activeShellId();
    if (activeShellCtx) {
      activeShellCtx.dispose();
      activeShellCtx = null;
    }
    const extRenderer = getExternalUiShell(currentId);
    if (extRenderer) {
      try {
        const pInfo = plugins().find((p) => p.id === currentId);
        if (pInfo) {
          const ctx = createPluginContext(pInfo);
          activeShellCtx = ctx;
          extRenderer(ctx);
        }
      } catch (e) {
        console.error("neiro: external shell error, falling back to default:", e);
        shellError.set({ shellId: currentId, message: String(e) });
        switchShell(DEFAULT_SHELL_ID);
      }
    }
  });

  return (
    <View class="flex-1 bg-sidebar">
      <If when={layout() === "desktop"} else={<UnsupportedPage />}>
        <If when={isStudioMinimalShell()} else={<DesktopShell />}>
          <StudioMinimalShell />
        </If>
      </If>
      <NowPlayingSheet />
      <OutputSheet />
      <QueueSheet />
      <ServerDialog />
      <PluginManagerDialog />
      <LyricsSearchSheet />
      <LyricsWriteDialog />
      <PlaylistDialogs />
      <ContextMenuHost />
      <Toaster bottomOffset={96} />
      <Shortcuts />
    </View>
  );
}

mount(App);
