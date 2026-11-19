// 播放条与 Now Playing 共用的传输控件：无引擎 headless 挂载。
import { Text, View, mount } from "nsh";
import { repeatMode, shuffleMode } from "../src/state/player";
import { engine } from "../src/backend/engine";
import { TransportControls } from "../src/components/player/parts";

shuffleMode.set(false);
repeatMode.set("off");
engine.setRepeatMode = () => {};
engine.setShuffleMode = () => {};

function TransportControlsHarness() {
  return (
    <View class="flex-1 gap-2 bg-background p-4">
      <Text>Shuffle: {shuffleMode() ? "on" : "off"}; Repeat: {repeatMode()}</Text>
      <TransportControls />
    </View>
  );
}

mount(TransportControlsHarness);
