// 引擎未连接: the one page the whole UI becomes when `new Base()` failed.
// What it says is the engine's own error text (never paraphrased), plus
// the one fact the user needs: build ../Neiro/base first. 重试 rebuilds
// the backend.

import { platform, Text, View } from "nsh";
import { retryEngine, healthError } from "../state/engine";
import { ActionButton } from "../components/ui/controls";

/** The engine's library, named the way this platform loads it. */
function engineLibrary(): string {
  if (platform.os === "macos") return "libneiro_base.dylib";
  if (platform.os === "windows") return "neiro_base.dll";
  return "libneiro_base.so";
}

export function EngineDownPage() {
  const error = healthError();
  return (
    <View class="flex-1 items-center justify-center bg-sidebar px-8">
      <View class="flex w-full max-w-lg flex-col gap-4 rounded-(--radius-panel) border border-border bg-card p-8">
        <View class="flex-row items-center gap-3">
          <View class="items-end">
            <View class="flex-row items-end gap-[3px]">
              <View class="h-4 w-[3px] rounded-full bg-muted" />
              <View class="h-6 w-[3px] rounded-full bg-muted" />
              <View class="h-8 w-[3px] rounded-full bg-muted" />
            </View>
          </View>
          <Text role="header" class="text-2xl font-bold text-foreground">
            引擎未连接
          </Text>
        </View>
        <Text class="text-sm leading-relaxed text-muted-foreground">
          {`Neiro 的音频引擎（${engineLibrary()}）没有找到，界面无法播放也没法读曲库。`}
        </Text>
        <Text class="font-mono text-xs text-muted-foreground">
          {error}
        </Text>
        <Text class="text-sm text-muted-foreground">需要先构建 ../Neiro/base：在那个仓库执行 cargo build --release，之后点重试。</Text>
        <View class="flex-row">
          <ActionButton label="重试" variant="primary" onPress={retryEngine} />
        </View>
      </View>
    </View>
  );
}
