// 设置页的本地文件夹段，配上假的 bookmark 解析：iOS 的桥在
// `engine.resolveFolderBookmark` 后面，测试在这里给出每次启动会看到的结果。
// 扫描计数画在页脚，测试从它的文字判断前台重扫是否发生。

import { NavStack } from "nsh/base";
import { signal, Text, View, mount } from "nsh";
import { nav } from "../src/nav";
import { engine } from "../src/backend/engine";
import { health } from "../src/state/engine";
import { SettingsPage } from "../src/pages/settings";

const scans = signal(0);

(engine as any).resolveFolderBookmark = (bookmark: string) => {
  if (bookmark === "BMK-OK") return { path: "/music", bookmark: "BMK-OK", stale: false };
  if (bookmark === "BMK-NEW") return { path: "/new", bookmark: "BMK-NEW", stale: false };
  return null;
};
(engine as any).scanFolder = (_path: string) => scans.update((n) => n + 1);
(engine as any).pickFolderBookmark = async () => ({
  path: "/new",
  bookmark: "BMK-NEW",
  label: "新目录",
});

if (health().status === "starting") engine.connect();

function ImportHarness() {
  return (
    <View class="flex-1 bg-background">
      <NavStack state={nav} class="flex-1">
        {() => <SettingsPage />}
      </NavStack>
      <Text testID="scan-count">{`scans:${scans()}`}</Text>
    </View>
  );
}

mount(ImportHarness);
