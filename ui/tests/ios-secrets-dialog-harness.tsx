// 服务器对话框 + 假的钥匙串：iOS 上编辑一台服务器时，掩码字段应当从
// 钥匙串预填；钥匙串里没有（换了设备、备份恢复了）时应当要求重新输入。

import { mount, signal, Storage, Text, View } from "nsh";
import { ServerDialog } from "../src/components/ui/server-dialog";
import { engine } from "../src/backend/engine";
import { setServerDialogOpen } from "../src/state/overlays";

const keychain = new Map<string, string>();
const writes = signal(0);
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

engine.secretGet = (id: string) => keychain.get(id) ?? null;
engine.secretSet = (id: string, value: string) => {
  keychain.set(id, value);
  writes.update((n) => n + 1);
};
engine.secretDelete = (id: string) => keychain.delete(id);
(engine as any).remoteProviders = () =>
  JSON.stringify([{ id: "jellyfin", name: "Jellyfin", fields: [], source: true }]);
(engine as any).remoteStatus = () => ({ configured: false, kind: "", baseUrl: "" });
(engine as any).listRemote = () => {};
(engine as any).listRemotePlaylists = () => {};
(engine as any).setRemote = () => {};

setServerDialogOpen(true);

function SecretsDialogHarness() {
  return (
    <View class="flex-1 bg-background">
      <ServerDialog />
      <Text testID="secret-writes">{`writes:${writes()}`}</Text>
      <Text testID="keychain-ready">{keychainReady() ? "ready" : "loading"}</Text>
    </View>
  );
}

mount(SecretsDialogHarness);
