// 服务器对话框，单独挂着：连接表单的校验必须自己说清楚，不能按了「连接」
// 什么也没发生。引擎的插件表在这里是固定的 Jellyfin 一条。

import { mount, View } from "nsh";
import { ServerDialog } from "../src/components/ui/server-dialog";
import { engine } from "../src/backend/engine";
import { setServerDialogOpen } from "../src/state/overlays";
import { servers } from "../src/state/sources";

// The dialog refreshes the provider registry when it opens; without a Base
// that refresh would empty it again, so the engine answers with Jellyfin.
(engine as any).remoteProviders = () =>
  JSON.stringify([{ id: "jellyfin", name: "Jellyfin", fields: [], source: true }]);

servers.set([]);
setServerDialogOpen(true);

function Harness() {
  return (
    <View class="flex-1 bg-background">
      <ServerDialog />
    </View>
  );
}

mount(Harness);
