// 服务器管理（连接/编辑/删除远程库）的对话框：列表 + 表单两态。
// 表单字段完全由引擎的动态插件注册表（PluginRegistry / engine.remoteProviders()）下发，
// 零硬编码枚举，支持任意新增的远程协议与存储插件。

import { effect, signal, token, Text, View, type JSX } from "nsh";
import { Button, Input } from "nsh/base";
import type { ProviderDescriptor, RemoteKind } from "../../backend/types";
import {
  activeRemoteServer,
  isRemoteConfigured,
  probeRemoteConfig,
  reconnectServer,
  removeServerById,
  type RemoteConfig,
} from "../../state/remote";
import { selectSource, servers, upsertServer, type RemoteServer } from "../../state/sources";
import { setServerDialogOpen, serverDialogOpen } from "../../state/overlays";
import { isRemoteKindEnabled, refreshPlugins, remoteProviders } from "../../state/plugins";
import { reportEngineFailure } from "../../backend/engine";
import { switchTo, REMOTE } from "../../nav";
import { ActionButton, TextLink, focusRing } from "./controls";
import { AppDialog } from "./overlays";
import { Icon } from "./icons";
import { Spinner } from "./feedback";
import { addRoot } from "../../state/catalog";

function remoteProvidersList(): ProviderDescriptor[] {
  return remoteProviders().filter(
    (p) => p.id !== "jellyfin_token" && isRemoteKindEnabled(p.id)
  );
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
        if (!next) editing.set(undefined);
        setServerDialogOpen(next);
      }}
      maxWidthClass="max-w-lg"
      testID="server-dialog"
    >
      <If when={serverDialogOpen()}>
        <ServerDialogContent />
      </If>
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
          onPress={() => {
            editing.set(undefined);
            setServerDialogOpen(false);
          }}
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
  const initialKind = () => {
    if (!existing) return providers()[0]?.id ?? "jellyfin";
    if (existing.kind === "jellyfin_token") return "jellyfin";
    return existing.kind;
  };
  const selectedKind = signal<string>(initialKind());
  const jellyfinAuthMode = signal<"password" | "token">(
    (existing?.token || existing?.kind === "jellyfin_token") ? "token" : "password"
  );
  const values = signal<Record<string, string>>({
    base_url: existing?.baseUrl ?? "",
    user: existing?.user ?? "",
    password: existing?.password ?? "",
    token: existing?.token ?? "",
  });
  const label = signal(existing?.label ?? "");
  const busy = signal(false);
  const testStatus = signal<{ state: "idle" | "testing" | "ok" | "error"; message?: string } | null>(null);

  const currentDescriptor = () => providers().find((p) => p.id === selectedKind()) ?? providers()[0];

  const updateValue = (key: string, val: string) => {
    values.update((v) => ({ ...v, [key]: val }));
    testStatus.set(null);
  };

  const isLocal = () => selectedKind() === "local";

  const normalizeUrl = (raw: string): string => {
    let url = raw.trim();
    if (url && !/^https?:\/\//i.test(url)) {
      url = `http://${url}`;
    }
    return url.replace(/\/+$/, "");
  };

  const buildConfig = (): { server: RemoteServer; cfg: RemoteConfig } | null => {
    const vals = values();
    const url = normalizeUrl(vals.base_url ?? "");
    if (!url) return null;
    const host = hostOf(url);
    const serverLabel = label().trim() || host;
    const userName = (vals.user ?? "").trim();

    let targetKind = selectedKind();
    const cleanParams: Record<string, string> = { ...vals, base_url: url };
    let tokenVal: string | undefined = vals.token?.trim();
    let passwordVal: string | undefined = vals.password?.trim();

    if (targetKind === "jellyfin") {
      if (jellyfinAuthMode() === "password") {
        targetKind = "jellyfin";
        tokenVal = undefined;
        delete cleanParams.token;
      } else {
        targetKind = "jellyfin_token";
        passwordVal = undefined;
        delete cleanParams.password;
      }
    }

    const server: RemoteServer = {
      id: existing?.id ?? `srv-${Date.now().toString(36)}`,
      label: serverLabel,
      kind: targetKind,
      baseUrl: url,
      user: userName,
      ...(tokenVal ? { token: tokenVal } : {}),
      ...(passwordVal ? { password: passwordVal } : {}),
      params: cleanParams,
    };

    const cfg: RemoteConfig = {
      kind: targetKind as RemoteKind,
      baseUrl: url,
      user: userName,
      ...(tokenVal ? { token: tokenVal } : {}),
      ...(passwordVal ? { password: passwordVal } : {}),
      params: cleanParams,
    };

    return { server, cfg };
  };

  const testConnection = async () => {
    const built = buildConfig();
    if (!built) {
      testStatus.set({ state: "error", message: "请输入有效的服务器地址" });
      return;
    }
    testStatus.set({ state: "testing", message: "正在连接并验证凭据…" });
    try {
      const res = await probeRemoteConfig(built.cfg);
      if (res.ok) {
        testStatus.set({
          state: "ok",
          message: `连接成功 (服务器可达，发现 ${res.count} 项)`,
        });
      } else {
        testStatus.set({
          state: "error",
          message: res.message || "连接失败",
        });
      }
    } catch (err) {
      testStatus.set({
        state: "error",
        message: String(err),
      });
    }
  };

  const save = () => {
    const built = buildConfig();
    if (!built) return;

    busy.set(true);

    void (async () => {
      try {
        await upsertServer(built.server);
        selectSource(`remote:${built.server.id}`);
        setServerDialogOpen(false);
        editing.set(undefined);
        switchTo(REMOTE);
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
            testID="input-server-label"
            accessibilityLabel="备注名"
            value={label()}
            onChange={({ value }) => label.set(value)}
            placeholder="可选，留空默认使用主机名"
            autoCapitalize="none"
            autoCorrect={false}
            class="h-8 flex-1 text-sm text-foreground"
            style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
          />
        </Field>

        <If
          when={selectedKind() === "jellyfin"}
          else={
            <Each of={currentDescriptor()?.fields ?? []}>
              {(field) => (
                <Field label={field().label} hint={field().required ? "必填" : "选填"}>
                  <Input
                    testID={`input-field-${field().key}`}
                    accessibilityLabel={field().label}
                    value={values()[field().key] ?? ""}
                    onChange={({ value }) => updateValue(field().key, value)}
                    placeholder={field().placeholder || "请输入"}
                    masked={field().kind === "password"}
                    autoCapitalize="none"
                    autoCorrect={false}
                    class="h-8 flex-1 text-sm text-foreground"
                    style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
                  />
                </Field>
              )}
            </Each>
          }
        >
          <View class="gap-1.5">
            <Text class="text-[11px] font-bold uppercase tracking-wider text-muted-foreground">认证模式</Text>
            <View class="flex-row rounded-(--radius) border border-border bg-secondary p-0.5">
              <Button
                selected={jellyfinAuthMode() === "password"}
                onPress={() => jellyfinAuthMode.set("password")}
                class={`flex-1 rounded-(--radius) py-1.5 transition ${
                  jellyfinAuthMode() === "password"
                    ? "bg-background text-foreground font-bold shadow-xs"
                    : "text-muted-foreground hover:text-foreground"
                } ${focusRing}`}
              >
                <Text class="text-center text-xs">账号密码</Text>
              </Button>
              <Button
                selected={jellyfinAuthMode() === "token"}
                onPress={() => jellyfinAuthMode.set("token")}
                class={`flex-1 rounded-(--radius) py-1.5 transition ${
                  jellyfinAuthMode() === "token"
                    ? "bg-background text-foreground font-bold shadow-xs"
                    : "text-muted-foreground hover:text-foreground"
                } ${focusRing}`}
              >
                <Text class="text-center text-xs">API Token</Text>
              </Button>
            </View>
          </View>

          <Field label="服务器地址" hint="必填">
            <Input
              testID="input-server-url"
              accessibilityLabel="服务器地址"
              value={values().base_url ?? ""}
              onChange={({ value }) => updateValue("base_url", value)}
              placeholder="http://192.168.1.2:8096"
              autoCapitalize="none"
              autoCorrect={false}
              class="h-8 flex-1 text-sm text-foreground"
              style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
            />
          </Field>

          <If when={jellyfinAuthMode() === "password"}>
            <Field label="用户名" hint="选填">
              <Input
                testID="input-username"
                accessibilityLabel="用户名"
                value={values().user ?? ""}
                onChange={({ value }) => updateValue("user", value)}
                placeholder="admin"
                autoCapitalize="none"
                autoCorrect={false}
                class="h-8 flex-1 text-sm text-foreground"
                style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
              />
            </Field>
            <Field label="密码" hint="选填">
              <Input
                testID="input-password"
                accessibilityLabel="密码"
                value={values().password ?? ""}
                onChange={({ value }) => updateValue("password", value)}
                masked
                placeholder="输入密码"
                autoCapitalize="none"
                autoCorrect={false}
                class="h-8 flex-1 text-sm text-foreground"
                style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
              />
            </Field>
          </If>

          <If when={jellyfinAuthMode() === "token"}>
            <Field label="API Key / Token" hint="必填">
              <Input
                testID="input-token"
                accessibilityLabel="API Key / Token"
                value={values().token ?? ""}
                onChange={({ value }) => updateValue("token", value)}
                masked
                placeholder="MediaBrowser Token"
                autoCapitalize="none"
                autoCorrect={false}
                class="h-8 flex-1 text-sm text-foreground"
                style={{ placeholderColor: token("muted-foreground"), caretColor: token("foreground") }}
              />
            </Field>
          </If>
        </If>

        <If when={testStatus()}>
          <View
            class={`flex-row items-center gap-2 rounded-(--radius) px-3 py-2 text-xs font-medium ${
              testStatus()?.state === "ok"
                ? "border border-emerald-500/30 bg-emerald-500/10 text-emerald-500"
                : testStatus()?.state === "error"
                ? "border border-destructive/30 bg-destructive/10 text-destructive"
                : "bg-secondary text-muted-foreground"
            }`}
            testID={
              testStatus()?.state === "ok"
                ? "test-connection-success"
                : testStatus()?.state === "error"
                ? "test-connection-error"
                : "test-connection-testing"
            }
          >
            <If when={testStatus()?.state === "testing"}>
              <Spinner label="探测中" />
            </If>
            <If when={testStatus()?.state === "ok"}>
              <Icon name="check" size={14} hex="#10b981" />
            </If>
            <If when={testStatus()?.state === "error"}>
              <Icon name="close" size={14} color="destructive" />
            </If>
            <Text class="flex-1 select-text text-xs">{testStatus()?.message}</Text>
          </View>
        </If>

        <View class="flex-row items-center justify-between pt-2">
          <TextLink label="取消" small onPress={props.onCancel} />
          <View class="flex-row items-center gap-2">
            <ActionButton
              label={testStatus()?.state === "testing" ? "探测中…" : "测试连接"}
              variant="outline"
              disabled={busy() || testStatus()?.state === "testing"}
              onPress={testConnection}
              testID="btn-test-connection"
            />
            <ActionButton
              label={existing ? "保存并连接" : "连接"}
              disabled={busy() || testStatus()?.state === "testing"}
              onPress={save}
              testID="btn-save-server"
            />
          </View>
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
