import { expect, open, test } from "nsh/test";
import { engine } from "../src/backend/engine";
import {
  activeShellId,
  createPluginContext,
  isPluginEnabled,
  isRemoteKindEnabled,
  isVisualizerPluginEnabled,
  PermissionDeniedError,
  plugins,
  refreshPlugins,
  switchShell,
  togglePlugin,
} from "../src/state/plugins";
import type { PluginInfo } from "../src/backend/types";

test("权限沙箱检查：无 tags.write 权限调用 writeTags 抛出 PermissionDeniedError", async () => {
  const restrictedPlugin: PluginInfo = {
    id: "test.restricted",
    name: "Restricted Plugin",
    version: "1.0.0",
    tier: "ui",
    category: "ui.visualizer",
    description: "Read-only visualizer",
    author: "Test",
    builtin: false,
    enabled: true,
    requiresRestart: false,
    permissions: ["dsp.read"],
  };

  const ctx = createPluginContext(restrictedPlugin);
  expect(ctx.hasPermission("dsp.read")).toBe(true);
  expect(ctx.hasPermission("tags.write")).toBe(false);

  let caught = false;
  try {
    await ctx.writeTags("song.flac", { title: "Hacked" });
  } catch (e) {
    if (e instanceof PermissionDeniedError) {
      caught = true;
    }
  }
  expect(caught).toBe(true);

  // With granted permission
  const authorizedPlugin: PluginInfo = {
    ...restrictedPlugin,
    id: "test.authorized",
    permissions: ["tags.write"],
  };
  const authCtx = createPluginContext(authorizedPlugin);
  expect(authCtx.hasPermission("tags.write")).toBe(true);

  let authFailed = false;
  try {
    await authCtx.writeTags("song.flac", { title: "OK" });
  } catch {
    authFailed = true;
  }
  expect(authFailed).toBe(false);
});

test("一键切换 UI 壳层：可在 Industrial Rack 与 Studio Minimal Deck 之间热插拔切换", async () => {
  switchShell("builtin.shell.industrial-rack");
  expect(activeShellId()).toBe("builtin.shell.industrial-rack");

  switchShell("builtin.shell.studio-minimal");
  expect(activeShellId()).toBe("builtin.shell.studio-minimal");

  switchShell("builtin.shell.industrial-rack");
  expect(activeShellId()).toBe("builtin.shell.industrial-rack");
});

test("PluginRegistry 跨三层插件全量纳管 (UI / Base / Engine) 与核心锁定规则", async () => {
  engine.connect();
  refreshPlugins();
  const list = plugins();
  expect(list.length).toBe(14);

  const tiers = new Set(list.map((p) => p.tier));
  expect(tiers.has("ui")).toBe(true);
  expect(tiers.has("base")).toBe(true);
  expect(tiers.has("engine")).toBe(true);

  const ids = list.map((p) => p.id);
  expect(ids.includes("builtin.shell.industrial-rack")).toBe(true);
  expect(ids.includes("builtin.shell.studio-minimal")).toBe(true);
  expect(ids.includes("builtin.visualizer.signal-chain")).toBe(true);
  expect(ids.includes("jellyfin")).toBe(true);
  expect(ids.includes("navidrome")).toBe(true);
  expect(ids.includes("webdav")).toBe(true);
  expect(ids.includes("builtin.expander.cue")).toBe(true);
  expect(ids.includes("builtin.expander.sacd")).toBe(true);
  expect(ids.includes("builtin.decoder.dsd")).toBe(true);
  expect(ids.includes("builtin.dsp.soxr")).toBe(true);
  expect(ids.includes("builtin.lyrics.lrclib")).toBe(true);
  expect(ids.includes("builtin.lyrics.netease")).toBe(true);
  expect(ids.includes("builtin.lyrics.qqmusic")).toBe(true);
  expect(ids.includes("builtin.lyrics.kugou")).toBe(true);

  // Locked core plugins
  const lockedIds = list.filter((p) => p.locked).map((p) => p.id);
  expect(lockedIds.includes("builtin.shell.industrial-rack")).toBe(true);
  expect(lockedIds.includes("builtin.expander.cue")).toBe(true);
  expect(lockedIds.includes("builtin.expander.sacd")).toBe(true);
  expect(lockedIds.includes("builtin.decoder.dsd")).toBe(true);
  expect(lockedIds.includes("builtin.dsp.soxr")).toBe(true);
});

test("插件启停联动逻辑：可视化与远程插件真实响应与恢复", async () => {
  expect(isVisualizerPluginEnabled()).toBe(true);
  togglePlugin("builtin.visualizer.signal-chain", false);
  expect(isVisualizerPluginEnabled()).toBe(false);
  togglePlugin("builtin.visualizer.signal-chain", true);
  expect(isVisualizerPluginEnabled()).toBe(true);

  expect(isRemoteKindEnabled("jellyfin")).toBe(true);
  togglePlugin("jellyfin", false);
  expect(isRemoteKindEnabled("jellyfin")).toBe(false);
  togglePlugin("jellyfin", true);
  expect(isRemoteKindEnabled("jellyfin")).toBe(true);
});

test("UI 渲染与交互验证：Grid插件矩阵、Jellyfin归一双模式与五大分类", async () => {
  const app = await open("../App.tsx", {
    platform: "macos",
    width: 1280,
    height: 840,
    storage: { "library:roots": "[]" },
  });

  // Wait for main window to mount and open Settings page
  await app.waitFor("label:设置", { timeout: 8000 });
  await app.press("label:设置");
  await app.waitFor("label:管理插件与界面壳层", { timeout: 5000 });

  // 1. Open Plugin Manager Dialog
  await app.press("label:管理插件与界面壳层");
  await app.waitFor("text:插件与界面壳层管理器", {
    timeout: 5000,
  });
  expect(app.shows("text:Industrial Rack (Default)")).toBe(true);
  expect(app.shows("text:Studio Minimal Deck")).toBe(true);
  expect(app.shows("text:14 项已注册")).toBe(true);

  // 2. Switch to "音源与分轨" tab and verify single Jellyfin and expanders
  await app.press("text:音源与分轨");
  await app.waitFor("text:Jellyfin 媒体服务器", { timeout: 5000 });
  expect(app.shows("text:Jellyfin 媒体服务器")).toBe(true);
  expect(app.shows("text:Navidrome / Subsonic")).toBe(true);
  expect(app.shows("text:WebDAV / NAS")).toBe(true);
  expect(app.shows("text:CUE Sheet 分轨解析器")).toBe(true);
  expect(app.shows("text:Scarlet Book SACD ISO 光盘解析器")).toBe(true);

  // 3. Switch to "在线歌词源" tab and verify the 4 lyrics plugins
  await app.press("text:在线歌词源");
  await app.waitFor("text:LRCLIB 同步歌词", { timeout: 5000 });
  expect(app.shows("text:LRCLIB 同步歌词")).toBe(true);
  expect(app.shows("text:网易云音乐歌词")).toBe(true);
  expect(app.shows("text:QQ 音乐歌词")).toBe(true);
  expect(app.shows("text:酷狗音乐歌词")).toBe(true);

  // 4. Hot-swap to Studio Minimal Deck shell
  await app.press("label:切换至 Studio Minimal Deck");
  await app.press("label:关闭");
  await app.waitFor("label:切回经典工作台", { timeout: 5000 });
  expect(app.shows("text:切回 Classic Deck")).toBe(true);

  // Switch back to Classic Deck
  await app.press("label:切回经典工作台");
  await app.waitFor("label:添加来源", { timeout: 5000 });

  // 5. Open Server Dialog ("添加来源") and verify Jellyfin single entry and segmented switcher
  await app.press("label:添加来源");
  await app.waitFor("text:连接服务器", { timeout: 5000 });
  expect(app.shows("text:Jellyfin 媒体服务器")).toBe(true);
  expect(app.shows("text:Navidrome / Subsonic")).toBe(true);
  expect(app.shows("text:WebDAV / NAS")).toBe(true);
  expect(app.shows("text:本地路径")).toBe(true);

  // Verify Jellyfin [ 账号密码 | API Token ] switcher
  expect(app.shows("text:账号密码")).toBe(true);
  expect(app.shows("text:API Token")).toBe(true);
  await app.press("text:API Token");
  await app.waitFor("text:API KEY / TOKEN", { timeout: 5000 });
  expect(app.shows("text:API KEY / TOKEN")).toBe(true);
}, 60_000);
