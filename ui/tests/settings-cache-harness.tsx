import { NavStack } from "nsh/base";
import { Text, View, mount } from "nsh";
import { nav } from "../src/nav";
import { engine } from "../src/backend/engine";
import { SettingsPage } from "../src/pages/settings";
import { health } from "../src/state/engine";

if (health().status === "starting") engine.connect();

function SettingsHarness() {
  return (
    <View class="flex-1 bg-background p-4">
      <View class="flex-row items-center justify-between bg-secondary p-2" role="header">
        <Text>设置</Text>
      </View>
      <NavStack state={nav} class="flex-1">
        {() => <SettingsPage />}
      </NavStack>
    </View>
  );
}

mount(SettingsHarness);
