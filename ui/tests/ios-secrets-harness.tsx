// 「来源」页的服务器花名册，配上一个假的钥匙串：iOS 的凭据不进
// Storage，激活服务器时从钥匙串读出来交给引擎。测试从页脚的文字判断
// 引擎收到的凭据是不是密码。

import { NavStack } from "nsh/base";
import { nav } from "../src/nav";
import { engine } from "../src/backend/engine";
import { RemotePage } from "../src/pages/remote";
import { mount, signal, Storage, Text, View } from "nsh";

/** What the engine was sent: "sent-password" or "sent-none". */
const sent = signal("");

const keychain = new Map<string, string>();
const keychainReady = signal(false);
void Storage.getItem("test:keychain")
  .then((raw) => {
    if (!raw) return;
    try {
      const parsed = JSON.parse(raw) as Record<string, unknown>;
      for (const [id, value] of Object.entries(parsed)) {
        if (typeof value === "string") keychain.set(id, value);
        else if (value && typeof value === "object") keychain.set(id, JSON.stringify(value));
      }
    } catch (error) {
      console.warn("keychain seed:", String(error));
    }
  })
  .finally(() => keychainReady.set(true));
const stored = signal("");
engine.secretGet = (id: string) => keychain.get(id) ?? null;
(engine as any).secretSet = (id: string, value: string) => {
  keychain.set(id, value);
  stored.set(`secret-set:${id}`);
};
(engine as any).secretDelete = (id: string) => keychain.delete(id);
(engine as any).listRemote = () => {};
(engine as any).listRemotePlaylists = () => {};
(engine as any).remoteProviders = () =>
  JSON.stringify([{ id: "jellyfin", name: "Jellyfin", fields: [], source: true }]);
(engine as any).remoteStatus = () => ({ configured: false, kind: "", baseUrl: "" });
(engine as any).setRemote = (_kind: string, params: Record<string, string>) => {
  sent.set(params?.password === "hunter2" ? "sent-password" : "sent-none");
};

function SecretsHarness() {
  return (
    <View class="flex-1 bg-background">
      <NavStack state={nav} class="flex-1">
        {() => <RemotePage />}
      </NavStack>
      <Text testID="secret-sent">{sent()}</Text>
      <Text testID="secret-stored">{stored()}</Text>
      <Text testID="keychain-ready">{keychainReady() ? "ready" : "loading"}</Text>
    </View>
  );
}

mount(SecretsHarness);
