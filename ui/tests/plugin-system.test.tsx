import { expect, test } from "nsh/test";
import { engine } from "../src/backend/engine";
import {
  activeShellId,
  createPluginContext,
  PermissionDeniedError,
  plugins,
  refreshPlugins,
  switchShell,
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

test("PluginRegistry 跨三层插件全量纳管 (UI / Base / Engine)", async () => {
  engine.connect();
  refreshPlugins();
  const list = plugins();
  expect(list.length).toBeGreaterThanOrEqual(10);

  const tiers = new Set(list.map((p) => p.tier));
  expect(tiers.has("ui")).toBe(true);
  expect(tiers.has("base")).toBe(true);
  expect(tiers.has("engine")).toBe(true);

  const ids = list.map((p) => p.id);
  expect(ids.includes("builtin.shell.industrial-rack")).toBe(true);
  expect(ids.includes("builtin.shell.studio-minimal")).toBe(true);
  expect(ids.includes("jellyfin")).toBe(true);
  expect(ids.includes("webdav")).toBe(true);
  expect(ids.includes("builtin.expander.cue")).toBe(true);
  expect(ids.includes("builtin.expander.sacd")).toBe(true);
  expect(ids.includes("builtin.decoder.dsd")).toBe(true);
  expect(ids.includes("builtin.dsp.soxr")).toBe(true);
});
