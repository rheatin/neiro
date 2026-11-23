// 手机上的「服务器」页：有服务器但还没选中任何一台时，应当列出花名册，
// 而不是说「没有连接的服务器」。引擎在这里只回答空列表。

import { NavStack } from "nsh/base";
import { mount, View } from "nsh";
import { nav } from "../src/nav";
import { engine } from "../src/backend/engine";
import { RemotePage } from "../src/pages/remote";

(engine as any).listRemote = () => {};
(engine as any).listRemotePlaylists = () => {};
(engine as any).setRemote = () => {};
(engine as any).remoteProviders = () =>
  JSON.stringify([{ id: "jellyfin", name: "Jellyfin", fields: [], source: true }]);
(engine as any).remoteStatus = () => ({ configured: false, kind: "", baseUrl: "" });

function RosterHarness() {
  return (
    <View class="flex-1 bg-background">
      <NavStack state={nav} class="flex-1">
        {() => <RemotePage />}
      </NavStack>
    </View>
  );
}

mount(RosterHarness);
