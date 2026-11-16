// 服务器管理（连接/编辑/删除远程库）的对话框：列表 + 表单两态。
// 表单字段完全由引擎的动态插件注册表（PluginRegistry / engine.remoteProviders()）下发，
// 零硬编码枚举，支持任意新增的远程协议与存储插件。

import { effect, signal, Text, View, type JSX } from "nsh";
import { Button, Input } from "nsh/base";
import type { ProviderDescriptor } from "../../backend/types";
import {
  activeRemoteServer,
  removeServerById,
} from "../../state/remote";
import { selectSource, servers, upsertServer, type RemoteServer } from "../../state/sources";
import { isRemoteConfigured } from "../../state/remote";
import { setServerDialogOpen, serverDialogOpen } from "../../state/overlays";
import { refreshPlugins, remoteProviders } from "../../state/plugins";
import { reportEngineFailure } from "../../backend/engine";
import { ActionButton, TextLink, focusRing } from "./controls";
import { AppDialog } from "./overlays";
import { Icon } from "./icons";
import { addRoot } from "../../state/catalog";

function remoteProvidersList(): ProviderDescriptor[] {
  return remoteProviders();
}

function hostOf(baseUrl: string): string {
  return baseUrl.replace(/^\w+:\/\//, "").replace(/\/.*$/, "") || baseUrl;
}

const editing = signal<RemoteServer | null | undefined>(undefined);

export function openSourceDialog(): void {
  refreshPlugins();
  editing.set(null);
  setServerDialogOpen(true);
}

export function ServerDialog() {
  effect(() => {
    if (serverDialogOpen()) {
      refreshPlugins();
    }
  });
  return (
    <AppDialog
      open={serverDialogOpen()}
      onOpenChange={(next) => {
        if (next) refreshPlugins();
        setServerDialogOpen(next);
      }}
      maxWidthClass="max-w-lg"
      testID="server-dialog"
    >
      <ServerDialogContent />
    </AppDialog>
  );
}

function ServerDialogContent() {
  const showForm = () => editing() !== undefined || servers().length === 0;
  return (
    <If when={showForm()} else={<ServerList onNew={() => editing.set(null)} onEdit={(s) => editing.set(s)} />}>
      <ServerForm
        server={editing() ?? null}
        onDone={() => editing.set(undefined)}
        onCancel={() => {
          editing.set(undefined);
          if (servers().length === 0) setServerDialogOpen(false);
        }}
      />
    </If>
  );
}

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
                  class={`h-2 w-2 rounded-full ${online(server()) ? "bg-emerald-500" : "bg-muted-foreground/40"}`}
                />
                <View class="min-w-0 flex-1">
                  <Text class="truncate text-sm font-bold text-foreground">{server().label}</Text>
                  <Text class="truncate text-xs text-muted-foreground">
                    {server().kind.toUpperCase()} · {server().baseUrl}
                  </Text>
                </View>
                <Button
                  accessibilityLabel={`编辑 ${server().label}`}
                  onPress={() => props.onEdit(server())}
                  class={`rounded-(--radius) p-1.5 text-xs text-muted-foreground hover:bg-muted hover:text-foreground ${focusRing}`}
                >
                  编辑
                </Button>
                <Button
                  accessibilityLabel={`删除 ${server().label}`}
                  onPress={() => void removeServerById(server().id)}
                  class={`rounded-(--radius) p-1.5 text-xs text-destructive hover:bg-destructive/10 ${focusRing}`}
                >
                  删除
                </Button>
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

function ServerForm(props: { server: RemoteServer | null; onDone: () => void; onCancel: () => void }) {
  const existing = props.server;
  const providers = () => remoteProvidersList();
  const selectedKind = signal<string>(existing?.kind ?? providers()[0]?.id ?? "jellyfin");
  const values = signal<Record<string, string>>({
    base_url: existing?.baseUrl ?? "",
    user: existing?.user ?? "",
    password: existing?.password ?? "",
    token: existing?.token ?? "",
  });
  const label = signal(existing?.label ?? "");
  const busy = signal(false);

  const currentDescriptor = () => providers().find((p) => p.id === selectedKind()) ?? providers()[0];

  const updateValue = (key: string, val: string) => {
    values.update((v) => ({ ...v, [key]: val }));
  };

  const isLocal = () => selectedKind() === "local";

  const save = () => {
    const vals = values();
    const url = (vals.base_url ?? "").trim();
    if (!url) return;
    const host = hostOf(url);
    const serverLabel = label().trim() || host;
    const userName = (vals.user ?? "").trim();

    busy.set(true);
    const server: RemoteServer = {
      id: existing?.id ?? `srv-${Date.now().toString(36)}`,
      label: serverLabel,
      kind: selectedKind(),
      baseUrl: url,
      user: userName,
      ...(vals.token ? { token: vals.token.trim() } : {}),
      ...(vals.password ? { password: vals.password.trim() } : {}),
      params: vals,
    };

    void (async () => {
      try {
        await upsertServer(server);
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
        <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">类型</Text>
        <View class="flex-row flex-wrap gap-1.5">
          <Each of={providers()}>
            {(p) => (
              <Button
                selected={selectedKind() === p().id}
                onPress={() => selectedKind.set(p().id)}
                class={`rounded-(--radius) border px-3 py-2 transition duration-150 ease-out ${
                  selectedKind() === p().id
                    ? "border-primary bg-secondary text-foreground"
                    : "border-border hover:bg-row-hover text-muted-foreground hover:text-foreground"
                } ${focusRing}`}
              >
                <Text class="line-clamp-1 text-center text-xs font-bold">{p().name}</Text>
              </Button>
            )}
          </Each>
          <Button
            selected={selectedKind() === "local"}
            onPress={() => selectedKind.set("local")}
            class={`rounded-(--radius) border px-3 py-2 transition duration-150 ease-out ${
              selectedKind() === "local"
                ? "border-primary bg-secondary text-foreground"
                : "border-border hover:bg-row-hover text-muted-foreground hover:text-foreground"
            } ${focusRing}`}
          >
            <Text class="line-clamp-1 text-center text-xs font-bold">本地路径</Text>
          </Button>
        </View>
      </View>

      <If when={isLocal()}>
        <Text class="text-xs leading-relaxed text-muted-foreground">
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

      <If when={!isLocal()}>
        <Field label="备注名" hint="留空用地址里的主机名">
          <Input
            value={label()}
            onChange={({ value }) => label.set(value)}
            autoCapitalize="none"
            autoCorrect={false}
            class="flex-1 text-sm text-foreground"
          />
        </Field>

        <Each of={currentDescriptor()?.fields ?? []}>
          {(field) => (
            <Field label={field().label} hint={field().required ? "必填" : "选填"}>
              <Input
                value={values()[field().key] ?? ""}
                onChange={({ value }) => updateValue(field().key, value)}
                placeholder={field().placeholder}
                masked={field().kind === "password"}
                autoCapitalize="none"
                autoCorrect={false}
                class="flex-1 text-sm text-foreground"
              />
            </Field>
          )}
        </Each>

        <View class="flex-row items-center justify-between pt-2">
          <TextLink label="取消" small onPress={props.onCancel} />
          <View class="flex-1" />
          <ActionButton label={existing ? "保存并连接" : "连接"} disabled={busy()} onPress={save} />
        </View>
      </If>
    </View>
  );
}

function Field(props: { label: string; hint?: string; children: JSX.Element }) {
  return (
    <View class="gap-1.5">
      <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">{props.label}</Text>
      <View class="flex-row items-center rounded-(--radius) border border-border bg-secondary px-3 py-2 focus-within:border-foreground/40">
        {props.children}
      </View>
      <If when={props.hint}>
        <Text class="text-[11px] text-muted-foreground">{props.hint}</Text>
      </If>
    </View>
  );
}
