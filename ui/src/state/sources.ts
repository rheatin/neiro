// 来源模型（首页"来源"Tab）：全部、本地（全部 + 每个根目录）与每个已配置
// 的远
// 程服务器。Base 同一时间只连一个远程（`set_remote*` 替换），所以远程
// 来源的切换 = `activateServer(id)`（配置 + 重拉）；本地来源用 catalog 的
// 根过滤层，无需任何引擎动作。
//
// 服务器花名册存 Storage `remote:servers`（凭据不进源码：桌面在行内，iOS
// 在钥匙串，行里只有 hasSecret）；旧的单条 `remote:config` 迁移为第一项。
// 当前激活来源存 `source:active`。

import { computed, effect, platform, signal, Storage } from "nsh";
import type { RemoteKind } from "../backend/types";
import { engine } from "../backend/engine";
import {
  addRoot,
  registerSourceKind,
  rootRecords,
  rootsLoaded,
  scopedRoot,
  setHomeRoot,
} from "./catalog";
import { isRemoteKindEnabled, registerPluginToggleHook } from "./plugins";
import { notify } from "./feedback";

export type SourceKind = "all" | "local" | "remote";

export interface RemoteServer {
  id: string;
  label: string;
  kind: RemoteKind;
  baseUrl: string;
  user?: string;
  /** Desktop only: the credential sits in the roster. On iOS it is stripped
   * into the Keychain and this stays undefined. */
  password?: string;
  /** Desktop only, as `password`. */
  token?: string;
  /** iOS: a secret was saved for this server. The value itself is not here;
   * a restore that lost the Keychain leaves this marker pointing at nothing,
   * and the settings page then asks for the credential again. */
  hasSecret?: boolean;
  params?: Record<string, string>;
}

export interface Source {
  id: string; // "all" | "local:all" | `local:${root}` | `remote:${serverId}`
  kind: SourceKind;
  label: string;
  root?: string;
  serverId?: string;
}

/** 「全部」= 本地曲库 + 所有已配置远程服务器的列表（实时优先，缓存兜底）。 */
export const ALL_ID = "all";
export const ALL: Source = { id: ALL_ID, kind: "all", label: "全部" };

export const LOCAL_ALL_ID = "local:all";
export const LOCAL_ALL: Source = { id: LOCAL_ALL_ID, kind: "local", label: "本地" };

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

/** 本地来源：全部 + 每个解析出来的根目录。id 跟随记录（iOS 是 uuid，
 * 桌面是路径），label 是用户选目录时看到的名字。没解析出来的根
 * （书签失效）不出现在来源里：它没有可浏览的内容。 */
export const localSources = computed<Source[]>(() => {
  const list: Source[] = [ALL, LOCAL_ALL];
  if (rootsLoaded()) {
    for (const record of rootRecords()) {
      if (record.path === null) continue;
      list.push({ id: `local:${record.id}`, kind: "local", label: record.label, root: record.path });
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

/** 当前激活来源（tracked；未持久化前 = 全部）。 */
export const activeSourceId = signal<string>(ALL_ID);

/** 协议被禁用时若当前正激活该协议来源，自动断开并退回「全部」 */
effect(() => {
  const currentId = activeSourceId();
  if (currentId.startsWith("remote:")) {
    const sId = currentId.slice("remote:".length);
    const s = servers().find((srv) => srv.id === sId);
    if (s && !isRemoteKindEnabled(s.kind)) {
      selectSource(ALL_ID);
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
  return allSources().find((s) => s.id === id) ?? ALL;
};

registerSourceKind(() => activeSource().kind);

// ---- Remote secrets -------------------------------------------------------
//
// One owner: on iOS a server's credential is a Keychain item and the roster
// row holds only `hasSecret`. On the desktop the roster row is the only
// place, as it always has been. Everything that hands credentials to the
// engine or to the dialog reads them through `readServerSecret`, so neither
// platform has a second copy.

/** iOS keeps secrets in the Keychain; the desktop keeps them in the row. */
export const secretsInKeychain = platform.os === "ios";

/** The provider keys that are credentials, when the caller does not know
 * the provider's fields (the two shapes Base itself ships). */
const DEFAULT_SECRET_KEYS = ["password", "token"];

/** The keychain blob for `server`: a JSON map of secret params, or an empty
 * map. On iOS an unreadable item and a restore that lost it look the same —
 * the user is asked to re-enter. */
export function readServerSecret(server: RemoteServer): Record<string, string> {
  if (!secretsInKeychain) {
    const legacy: Record<string, string> = {};
    if (server.password) legacy.password = server.password;
    if (server.token) legacy.token = server.token;
    return legacy;
  }
  const raw = engine.secretGet(server.id);
  if (!raw) return {};
  try {
    const parsed: unknown = JSON.parse(raw);
    if (!parsed || typeof parsed !== "object") return {};
    const secret: Record<string, string> = {};
    for (const [key, value] of Object.entries(parsed as Record<string, unknown>)) {
      if (typeof value === "string" && value) secret[key] = value;
    }
    return secret;
  } catch {
    return {};
  }
}

/** Writes the secret blob; `false` means the Keychain refused it. */
function writeServerSecret(id: string, secret: Record<string, string>): boolean {
  try {
    if (Object.keys(secret).length > 0) {
      engine.secretSet(id, JSON.stringify(secret));
    } else {
      engine.secretDelete(id);
    }
    return true;
  } catch (error) {
    console.warn("neiro: 凭据没有写进钥匙串:", String(error));
    return false;
  }
}

/** Deletes one server's secret (including a desktop row's, which has none). */
export function deleteServerSecret(id: string): void {
  if (!secretsInKeychain) return;
  try {
    engine.secretDelete(id);
  } catch (error) {
    console.warn("neiro: 凭据没有从钥匙串删除:", String(error));
  }
}

/** The server's credentials as the engine needs them: the saved secret plus
 * the row's own legacy copy. */
export function serverCredentials(server: RemoteServer): Record<string, string> {
  return {
    ...(server.password ? { password: server.password } : {}),
    ...(server.token ? { token: server.token } : {}),
    ...(server.params ?? {}),
    ...readServerSecret(server),
  };
}

/** Strips `secretKeys` out of the row and into the Keychain (iOS). The
 * desktop keeps the row as it is. */
function withStoredSecrets(server: RemoteServer, secretKeys: string[]): RemoteServer {
  if (!secretsInKeychain) return server;
  const params: Record<string, string> = { ...(server.params ?? {}) };
  const secret: Record<string, string> = {};
  const row: RemoteServer = { ...server, params };
  delete row.password;
  delete row.token;
  for (const key of secretKeys) {
    const value = key === "password" ? server.password : key === "token" ? server.token : params[key];
    if (typeof value === "string" && value) secret[key] = value;
    delete params[key];
  }
  const saved = writeServerSecret(server.id, secret);
  row.hasSecret = saved && Object.keys(secret).length > 0;
  return row;
}

/** 服务器花名册的增改删（凭据只进 Storage）。 */
export async function upsertServer(
  server: RemoteServer,
  secretKeys: string[] = DEFAULT_SECRET_KEYS,
): Promise<void> {
  const row = withStoredSecrets(server, secretKeys);
  const next = servers().filter((s) => s.id !== row.id);
  next.push(row);
  servers.set(next);
  await Storage.setItem(SERVERS_KEY, JSON.stringify(next)).catch((error) =>
    console.warn("neiro: 服务器花名册没有记住:", String(error)),
  );
}

export async function removeServer(id: string): Promise<void> {
  deleteServerSecret(id);
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
      const storedId = (await Storage.getItem(ACTIVE_KEY)) ?? ALL_ID;
      // 旧默认 `local:all` 的语义就是“全部”；一次性迁移到新的 ALL_ID。
      const activeId = storedId === LOCAL_ALL_ID ? ALL_ID : storedId;
      activeSourceId.set(activeId);
      if (activeId.startsWith("local:") && activeId !== LOCAL_ALL_ID) {
        const record = rootRecords().find((row) => `local:${row.id}` === activeId);
        setHomeRoot(record?.path ?? null);
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

// A roster row written before the Keychain existed (a legacy `remote:config`
// migrated into `srv-0`, or a roster saved under an older build) still carries
// its password in the clear. Once the engine is ready — the Keychain bridge
// needs it — those move into the Keychain and the row keeps only the marker.
let secretsMigrated = false;
effect(() => {
  if (!secretsInKeychain || secretsMigrated) return;
  if (!serversLoaded() || engine.health().status !== "ready") return;
  secretsMigrated = true;
  const plain = servers().filter(
    (row) =>
      !!row.password ||
      !!row.token ||
      DEFAULT_SECRET_KEYS.some((key) => !!row.params?.[key]),
  );
  if (plain.length === 0) return;
  const next = servers().map((row) => (plain.includes(row) ? withStoredSecrets(row, DEFAULT_SECRET_KEYS) : row));
  servers.set(next);
  void Storage.setItem(SERVERS_KEY, JSON.stringify(next)).catch((error) =>
    console.warn("neiro: 服务器花名册没有记住:", String(error)),
  );
  void Storage.removeItem("remote:config").catch(() => {});
});

/** 本地来源在启动时可能比根目录记录先恢复（记录等引擎就绪才读）。记录
 * 到了之后把作用域补上：来源 id 是 `local:{id}`，路径只在内存里。 */
effect(() => {
  const id = activeSourceId();
  if (!id.startsWith("local:") || id === LOCAL_ALL_ID) return;
  const record = rootRecords().find((row) => `local:${row.id}` === id);
  if (record?.path && scopedRoot() === null) setHomeRoot(record.path);
});

/** 新增本地根（首页 Tab 的 + 文件夹），并切到它的来源。 */
export async function pickAndAddRoot(): Promise<void> {
  const added = await addRoot();
  if (added) selectSource(`local:${added.id}`);
}
