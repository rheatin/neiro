// 来源模型（首页"来源"Tab）：本地（全部 + 每个根目录）与每个已配置的远
// 程服务器。Base 同一时间只连一个远程（`set_remote*` 替换），所以远程
// 来源的切换 = `activateServer(id)`（配置 + 重拉）；本地来源用 catalog 的
// 根过滤层，无需任何引擎动作。
//
// 服务器花名册存 Storage `remote:servers`（凭据只在 Storage，不进源码）；
// 旧的单条 `remote:config` 迁移为第一项。当前激活来源存 `source:active`。

import { computed, effect, signal, Storage } from "nsh";
import type { RemoteKind } from "../backend/types";
import { addRoot, roots, rootsLoaded, scopedRoot, setHomeRoot } from "./catalog";
import { isRemoteKindEnabled, registerPluginToggleHook } from "./plugins";
import { notify } from "./feedback";

export type SourceKind = "local" | "remote";

export interface RemoteServer {
  id: string;
  label: string;
  kind: RemoteKind;
  baseUrl: string;
  user?: string;
  password?: string;
  token?: string;
  params?: Record<string, string>;
}

export interface Source {
  id: string; // "local:all" | `local:${root}` | `remote:${serverId}`
  kind: SourceKind;
  label: string;
  root?: string;
  serverId?: string;
}

export const LOCAL_ALL_ID = "local:all";
export const LOCAL_ALL: Source = { id: LOCAL_ALL_ID, kind: "local", label: "全部本地" };

const SERVERS_KEY = "remote:servers";
const ACTIVE_KEY = "source:active";

/** 已配置的远程服务器（花名册）。 */
export const servers = signal<RemoteServer[]>([]);
export const serversLoaded = signal(false);

function hostOf(baseUrl: string): string {
  return baseUrl.replace(/^\w+:\/\//, "").replace(/\/.*$/, "") || baseUrl;
}

/** 一个远程服务器的来源视图。 */
function serverSource(server: RemoteServer): Source {
  return { id: `remote:${server.id}`, kind: "remote", label: server.label, serverId: server.id };
}

/** 本地来源：全部 + 每个根目录（根目录 basename 作 label）。 */
export const localSources = computed<Source[]>(() => {
  const list: Source[] = [LOCAL_ALL];
  if (rootsLoaded()) {
    for (const root of roots()) {
      const base = root.replace(/\\/g, "/").split("/").pop() || root;
      list.push({ id: `local:${root}`, kind: "local", label: base, root });
    }
  }
  return list;
});

/** 远程来源：每个已配置服务器（仅展示已启用插件的协议）。 */
export const remoteSources = computed<Source[]>(() =>
  servers().filter((s) => isRemoteKindEnabled(s.kind)).map(serverSource)
);

/** 全部来源（本地在前）。 */
export const allSources = (): Source[] => [...localSources(), ...remoteSources()];

/** 当前激活来源（tracked；未持久化前 = 全部本地）。 */
export const activeSourceId = signal<string>(LOCAL_ALL_ID);

/** 协议被禁用时若当前正激活该协议来源，自动断开并退回全部本地 */
effect(() => {
  const currentId = activeSourceId();
  if (currentId.startsWith("remote:")) {
    const sId = currentId.slice("remote:".length);
    const s = servers().find((srv) => srv.id === sId);
    if (s && !isRemoteKindEnabled(s.kind)) {
      selectSource(LOCAL_ALL_ID);
      remoteDisconnector?.();
      notify("远程插件已停用，已断开连接");
    }
  }
});

/** 协议重新启用时，若存在该协议的服务器，自动重连并激活该来源 */
registerPluginToggleHook((id, enabled) => {
  if (enabled && (id === "jellyfin" || id === "navidrome" || id === "webdav")) {
    const matching = servers().find((s) => s.kind === id || (id === "jellyfin" && s.kind === "jellyfin_token"));
    if (matching) {
      selectSource(`remote:${matching.id}`);
      notify(`已重新启用 ${matching.label}，正在连接…`);
    }
  }
});

export const activeSource = (): Source => {
  const id = activeSourceId();
  return allSources().find((s) => s.id === id) ?? LOCAL_ALL;
};

/** 服务器花名册的增改删（凭据只进 Storage）。 */
export async function upsertServer(server: RemoteServer): Promise<void> {
  const next = servers().filter((s) => s.id !== server.id);
  next.push(server);
  servers.set(next);
  await Storage.setItem(SERVERS_KEY, JSON.stringify(next)).catch((error) =>
    console.warn("neiro: 服务器花名册没有记住:", String(error)),
  );
}

export async function removeServer(id: string): Promise<void> {
  servers.set(servers().filter((s) => s.id !== id));
  await Storage.setItem(SERVERS_KEY, JSON.stringify(servers())).catch((error) =>
    console.warn("neiro: 服务器花名册没有记住:", String(error)),
  );
}

/** 激活来源切换。远程来源的连接动作由 remote.ts 注册（避免模块循环）：
 * `onSourceActivated` 由 remote.ts 在模块加载时设置。 */
let remoteActivator: ((serverId: string) => void) | null = null;
export function registerRemoteActivator(fn: (serverId: string) => void): void {
  remoteActivator = fn;
}

let remoteDisconnector: (() => void) | null = null;
export function registerRemoteDisconnector(fn: () => void): void {
  remoteDisconnector = fn;
}

/** 切换来源并持久化；远程来源触发连接 + 重拉（remote.ts）。 */
export function selectSource(id: string): void {
  const src = allSources().find((s) => s.id === id);
  if (!src) return;
  activeSourceId.set(src.id);
  setHomeRoot(src.kind === "local" && src.root ? src.root : null);
  void Storage.setItem(ACTIVE_KEY, src.id).catch((error) =>
    console.warn("neiro: 来源没有记住:", String(error)),
  );
  if (src.kind === "remote" && src.serverId) remoteActivator?.(src.serverId);
}

// ---- 启动迁移与恢复 -----------------------------------------------------------

let seeded = false;
effect(() => {
  if (seeded) return;
  seeded = true;
  void (async () => {
    try {
      // 1) 花名册：读 `remote:servers`；没有则迁移旧的单条 `remote:config`。
      const raw = await Storage.getItem(SERVERS_KEY);
      let list: RemoteServer[] = [];
      if (raw) {
        const parsed: unknown = JSON.parse(raw);
        if (Array.isArray(parsed)) {
          list = parsed
            .filter((row): row is RemoteServer =>
              !!row && typeof row === "object" &&
              typeof (row as RemoteServer).kind === "string" && (row as RemoteServer).kind !== "" &&
              typeof (row as RemoteServer).baseUrl === "string" && (row as RemoteServer).baseUrl !== "")
            .map((row, i) => ({
              id: typeof row.id === "string" && row.id ? row.id : `srv-${i}`,
              label: typeof row.label === "string" && row.label ? row.label : hostOf(row.baseUrl),
              kind: row.kind,
              baseUrl: row.baseUrl,
              user: typeof row.user === "string" ? row.user : undefined,
              password: typeof row.password === "string" && row.password ? row.password : undefined,
              token: typeof row.token === "string" && row.token ? row.token : undefined,
              params: typeof row.params === "object" ? row.params : undefined,
            }));
        }
      }
      if (list.length === 0) {
        const legacy = await Storage.getItem("remote:config");
        if (legacy) {
          const parsed = JSON.parse(legacy) as Partial<RemoteServer>;
          if (typeof parsed.kind === "string" && parsed.kind !== "" && typeof parsed.baseUrl === "string" && parsed.baseUrl !== "") {
            list = [
              {
                id: `srv-0`,
                label: hostOf(parsed.baseUrl),
                kind: parsed.kind,
                baseUrl: parsed.baseUrl,
                user: typeof parsed.user === "string" ? parsed.user : undefined,
                password: typeof parsed.password === "string" && parsed.password ? parsed.password : undefined,
                token: typeof parsed.token === "string" && parsed.token ? parsed.token : undefined,
                params: typeof parsed.params === "object" ? parsed.params : undefined,
              },
            ];
            void Storage.setItem(SERVERS_KEY, JSON.stringify(list)).catch(() => {});
          }
        }
      }
      servers.set(list);
      serversLoaded.set(true);
      // 2) 激活来源：读 `source:active`；指向远程 → 恢复该服务器。
      const activeId = (await Storage.getItem(ACTIVE_KEY)) ?? LOCAL_ALL_ID;
      activeSourceId.set(activeId);
      if (activeId.startsWith("local:") && activeId !== LOCAL_ALL_ID) {
        setHomeRoot(activeId.slice("local:".length));
      } else if (scopedRoot() === null) {
        // Nothing picked since boot: the stored choice stands. A folder picked
        // while this read was in flight is the newer fact, not this one.
        setHomeRoot(null);
      }
      // 远程来源的连接恢复由 remote.ts 的启动种子做（它等引擎 ready；这里
      // 的 activator 此刻尚未注册——注册发生在 remote.ts 模块初始化）。
    } catch (error) {
      console.warn("neiro: 来源没有读回来:", String(error));
    }
  })();
});

/** 新增本地根（首页 Tab 的 + 文件夹），并切到它的来源。 */
export async function pickAndAddRoot(): Promise<void> {
  const before = roots().length;
  await addRoot();
  if (roots().length > before) selectSource(`local:${roots()[roots().length - 1]}`);
}
