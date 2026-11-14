// 服务器管理（连接/编辑/删除远程库）的对话框：列表 + 表单两态。列表列出
// 每台已配置的服务器（label、类型、地址、状态点），每行 编辑/删除；尾部
// 新建。表单：Jellyfin 密码 / Jellyfin Token /
// Navidrome 三选一，地址（无尾斜杠）、备注名、用户、密码或
// Token。凭据只进 Storage（TSX 侧），从来不见源码或日志；连接被拒时
// 引擎的说法原样进 toast。

import { signal, Text, View, token, type JSX } from "nsh";
import { Button, Input } from "nsh/base";
import type { RemoteKind } from "../../backend/types";
import {
  activeRemoteServer,
  removeServerById,
  saveRemoteConfig,
  type RemoteConfig,
} from "../../state/remote";
import { selectSource, servers, upsertServer, type RemoteServer } from "../../state/sources";
import { isRemoteConfigured } from "../../state/remote";
import { setServerDialogOpen, serverDialogOpen } from "../../state/overlays";
import { reportEngineFailure } from "../../backend/engine";
import { ActionButton, TextLink, focusRing } from "./controls";
import { AppDialog } from "./overlays";
import { Icon } from "./icons";
import { addRoot } from "../../state/catalog";

/** The three server kinds the engine speaks, as unmasked forms here: the
 * labels carry the real names. */
type ServerKind = "jf-password" | "jf-token" | "nd" | "local";

const KIND_LABEL: Record<ServerKind, string> = {
  "jf-password": "Jellyfin 密码",
  "jf-token": "Jellyfin Token",
  nd: "Navidrome",
  local: "本地路径",
};

const KIND_ORDER: ServerKind[] = ["jf-password", "jf-token", "nd", "local"];

function remoteKindOf(kind: ServerKind): RemoteKind {
  return kind === "nd" ? "navidrome" : "jellyfin";
}

/** A folder on this machine is a source too, so the same 加号 asks for it. */
function isLocal(kind: ServerKind): boolean {
  return kind === "local";
}

function kindOfServer(server: RemoteServer | null): ServerKind {
  if (!server) return "jf-password";
  if (server.kind === "navidrome") return "nd";
  return server.token ? "jf-token" : "jf-password";
}

function hostOf(baseUrl: string): string {
  return baseUrl.replace(/^\w+:\/\//, "").replace(/\/.*$/, "") || baseUrl;
}

/** The form's "which kind" state, at module scope so the sidebar's 加号 can
 * open the dialog straight on it: the 加号 asks for a source, and the kinds are
 * that dialog's tabs, not a second menu in front of it. */
const editing = signal<RemoteServer | null | undefined>(undefined);

/** Opens the source dialog on a blank form (the tabs are the kinds). */
export function openSourceDialog(): void {
  editing.set(null);
  setServerDialogOpen(true);
}

export function ServerDialog() {
  return (
    <AppDialog open={serverDialogOpen()} onOpenChange={(next) => setServerDialogOpen(next)} testID="server-dialog">
      <ServerDialogContent />
    </AppDialog>
  );
}

function ServerDialogContent() {
  /** undefined = 列表；null = 新建；RemoteServer = 编辑。 */
  return (
    <If when={editing() !== undefined} else={<ServerList onNew={() => editing.set(null)} onEdit={(s) => editing.set(s)} />}>
      <ServerForm
        server={editing() ?? null}
        onDone={() => editing.set(undefined)}
        onCancel={() => editing.set(undefined)}
      />
    </If>
  );
}

/** 列表态：每台服务器一行（label + 类型 + 地址 + 状态点），行尾 编辑 /
 * 删除；尾部 新建服务器…。 */
function ServerList(props: { onNew: () => void; onEdit: (server: RemoteServer) => void }) {
  const list = () => servers();
  const activeId = () => activeRemoteServer()?.id;
  const online = (server: RemoteServer) => isRemoteConfigured() && activeId() === server.id;
  return (
    <View class="gap-4 p-6" testID="server-list">
      <View class="flex-row items-center justify-between">
        <Text role="header" class="text-xl font-bold text-foreground">
          服务器
        </Text>
        <Button
          accessibilityLabel="关闭"
          onPress={() => setServerDialogOpen(false)}
          class={`items-center justify-center rounded-(--radius) p-1 hover:bg-muted active:opacity-60 ${focusRing}`}
        >
          <Icon name="close" size={16} color="muted-foreground" />
        </Button>
      </View>

      <If
        when={list().length > 0}
        else={<Text class="py-6 text-center text-sm text-muted-foreground">还没有服务器。新建一台来连接远程库。</Text>}
      >
        <View class="gap-1.5">
          <Each of={list()}>
            {(server) => (
              <View
                class="flex-row items-center gap-2 rounded-(--radius) border border-border bg-secondary px-3 py-2"
                accessibilityLabel={`服务器 ${server().label}`}
              >
                <View
                  class="h-2 w-2 shrink-0 rounded-full"
                  style={{ backgroundColor: online(server()) ? token("chart-1") : token("muted-foreground") }}
                />
                <View class="min-w-0 flex-1">
                  <Text class="line-clamp-1 text-sm font-semibold text-foreground">{server().label}</Text>
                  <Text class="line-clamp-1 text-[11px] text-muted-foreground">
                    {KIND_LABEL[kindOfServer(server())]} · {server().baseUrl}
                  </Text>
                </View>
                <TextLink label="编辑" small onPress={() => props.onEdit(server())} />
                <TextLink
                  label="删除"
                  small
                  onPress={() => {
                    void (async () => {
                      try {
                        await removeServerById(server().id);
                      } catch (error) {
                        reportEngineFailure(error, "删除失败");
                      }
                    })();
                  }}
                />
              </View>
            )}
          </Each>
        </View>
      </If>

      <View class="flex-row items-center justify-end pt-2">
        <ActionButton label="新建服务器…" onPress={props.onNew} />
      </View>
    </View>
  );
}

/** 表单态（新建/编辑）：保存 = 写花名册 + 下发引擎 + 切到该来源。 */
function ServerForm(props: { server: RemoteServer | null; onDone: () => void; onCancel: () => void }) {
  const existing = props.server;
  const kind = signal<ServerKind>(kindOfServer(existing));
  const baseUrl = signal(existing?.baseUrl ?? "");
  const label = signal(existing?.label ?? "");
  const user = signal(existing?.user ?? "");
  const secret = signal(existing?.token ?? existing?.password ?? "");
  const busy = signal(false);

  const secretLabel = () => (kind() === "jf-token" ? "Token" : "密码");
  const save = () => {
    const url = baseUrl().trim().replace(/\/+$/, "");
    const secretValue = secret().trim();
    const userName = user().trim();
    if (!url || !userName || !secretValue) return;
    busy.set(true);
    const server: RemoteServer = {
      id: existing?.id ?? `srv-${Date.now().toString(36)}`,
      label: label().trim() || hostOf(url),
      kind: remoteKindOf(kind()),
      baseUrl: url,
      user: userName,
      ...(kind() === "jf-token" ? { token: secretValue } : { password: secretValue }),
    };
    void (async () => {
      try {
        await upsertServer(server);
        await saveRemoteConfig({
          kind: server.kind,
          baseUrl: server.baseUrl,
          user: server.user ?? "",
          password: server.password,
          token: server.token,
        } satisfies RemoteConfig);
        selectSource(`remote:${server.id}`);
        setServerDialogOpen(false);
      } catch (error) {
        reportEngineFailure(error, "连接失败");
      } finally {
        busy.set(false);
      }
    })();
  };

  return (
    <View class="gap-4 p-6" testID="server-form">
      <View class="flex-row items-center justify-between">
        <Text role="header" class="text-xl font-bold text-foreground">
          {existing ? "编辑服务器" : "连接服务器"}
        </Text>
        <Button
          accessibilityLabel="关闭"
          onPress={() => setServerDialogOpen(false)}
          class={`items-center justify-center rounded-(--radius) p-1 hover:bg-muted active:opacity-60 ${focusRing}`}
        >
          <Icon name="close" size={16} color="muted-foreground" />
        </Button>
      </View>

      <View class="gap-2">
        <Text class="text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">类型</Text>
        <View class="flex-row gap-1">
          <Each of={KIND_ORDER}>
            {(key) => (
              <Button
                selected={kind() === key()}
                onPress={() => kind.set(key())}
                class={`flex-1 rounded-(--radius) px-2.5 py-2 transition duration-150 ease-out ${
                  kind() === key() ? "bg-secondary text-foreground" : "hover:bg-row-hover text-foreground opacity-80"
                } ${focusRing}`}
              >
                <Text class="line-clamp-1 text-center text-xs font-semibold">{KIND_LABEL[key()]}</Text>
              </Button>
            )}
          </Each>
        </View>
      </View>

      <If when={isLocal(kind())}>
        <Text class="text-sm leading-relaxed text-muted-foreground">
          选一个文件夹，扫描把里面的文件收进本地曲库。可以加多个。
        </Text>
        <ActionButton
          label="选择文件夹…"
          onPress={() => {
            setServerDialogOpen(false);
            void addRoot();
          }}
        />
      </If>

      <If when={!isLocal(kind())}>
      <Field label="服务器地址" hint="结尾的斜杠自动去掉，如 http://192.168.1.2:8096">
        <Input
          value={baseUrl()}
          onChange={({ value }) => baseUrl.set(value)}
          placeholder="http://192.168.1.2:8096"
          autoCapitalize="none"
          autoCorrect={false}
          class="flex-1 text-sm text-foreground"
        />
      </Field>
      <Field label="备注名" hint="留空用地址里的主机名">
        <Input
          value={label()}
          onChange={({ value }) => label.set(value)}
          autoCapitalize="none"
          autoCorrect={false}
          class="flex-1 text-sm text-foreground"
        />
      </Field>
      <Field label="用户">
        <Input
          value={user()}
          onChange={({ value }) => user.set(value)}
          autoCapitalize="none"
          autoCorrect={false}
          class="flex-1 text-sm text-foreground"
        />
      </Field>
      <Field label={secretLabel()}>
        <Input
          value={secret()}
          onChange={({ value }) => secret.set(value)}
          masked
          autoCapitalize="none"
          autoCorrect={false}
          class="flex-1 text-sm text-foreground"
        />
      </Field>

      <View class="flex-row items-center justify-between pt-2">
        <TextLink label="取消" small onPress={props.onCancel} />
        <View class="flex-1" />
        <ActionButton label={existing ? "保存并连接" : "连接"} disabled={busy()} onPress={save} />
      </View>
      </If>
    </View>
  );
}

/** One labeled field: the label over the input, in a shared quiet field
 * surface. */
function Field(props: { label: string; hint?: string; children: JSX.Element }) {
  return (
    <View class="gap-1.5">
      <Text class="text-[11px] font-semibold uppercase tracking-wider text-muted-foreground">{props.label}</Text>
      <View class="h-10 flex-row items-center gap-2 rounded-(--radius) border border-border bg-secondary px-3 focus-within:border-muted-foreground">
        {props.children}
      </View>
      <If when={props.hint}>
        <Text class="text-[11px] text-muted-foreground">{props.hint}</Text>
      </If>
    </View>
  );
}
