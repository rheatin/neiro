// Output 控制台 (CS-02 Mastering Console) 抽屉容器与表面装配。
// 表面 5 个模块均在 surface/* 中实现并在此组装。

import { Scroll, Text, View } from "nsh";
import { Button } from "nsh/base";
import { engine } from "../../backend/engine";
import { outputOpen, setEqOpenedFromOutput, setOutputOpen } from "../../state/overlays";
import { AppSheet } from "../ui/overlays";
import { focusRing } from "../ui/controls";
import { Icon } from "../ui/icons";
import { Chassis, HARDWARE, MechKey, ModuleHeader, Silkscreen } from "../ui/hardware";
import { SourceStage } from "./surface/source";
import { DspMatrix } from "./surface/dsp-matrix";
import { MasterBus } from "./surface/master-bus";
import { OutputInterface } from "./surface/output-interface";
import { TelemetryStage } from "./surface/telemetry";
import { foleyEnabled, setFoleyEnabled } from "../../state/foley";
import { visualPeak, visualPeakL, visualPeakR, visualRms } from "../../state/visualiser";
import { open } from "../../nav";
import { player } from "../../state/player";

/** 抽屉，open 状态归 overlays。 */
export function OutputSheet() {
  return (
    <AppSheet open={outputOpen()} onOpenChange={setOutputOpen} side="right" testID="output" popupWidth={668}>
      <OutputContent />
    </AppSheet>
  );
}

function OutputContent() {
  // The two figures the PPM ladder draws: the engine's own sample peak and
  // RMS from the visualiser frames it sends while it plays. Nothing here
  // measures audio, and nothing is drawn when no frame has arrived.
  const playing = () => engine.player().state === "playing";
  const peak = () => (playing() ? visualPeak() : 0);
  const peakL = () => (playing() ? visualPeakL() : null);
  const peakR = () => (playing() ? visualPeakR() : null);
  const rms = () => (playing() ? visualRms() : 0);
  return (
    <Scroll
      class="flex-1 flex-col"
      contentClass="flex flex-col gap-3 border-l border-border bg-sidebar px-3 py-3 pt-[env(safe-area-inset-top)]"
    >
      {/* The instrument's own title bar: the model name, the foley switch and
          the close. */}
      <View class="flex-row items-center gap-2 px-1">
        <View class="min-w-0 flex-1 gap-0.5">
          <Text class="text-[10px] font-bold uppercase tracking-[0.2em]" style={{ color: HARDWARE.amber }}>
            NEIRO · OUTPUT PATH
          </Text>
          <Text role="header" class="text-lg font-bold text-foreground">
            Output 控制台
          </Text>
        </View>
        <MechKey
          label={foleyEnabled() ? "关闭机械音效" : "开启机械音效"}
          lit={foleyEnabled()}
          onPress={() => setFoleyEnabled(!foleyEnabled())}
          class="h-6 items-center justify-center rounded px-2"
        >
          <Text class="font-mono text-[8px] font-bold tracking-wider" style={{ color: foleyEnabled() ? HARDWARE.amberBright : HARDWARE.textDim }}>
            FOLEY {foleyEnabled() ? "ON" : "OFF"}
          </Text>
        </MechKey>
        <Button
          accessibilityLabel="关闭输出信息"
          onPress={() => setOutputOpen(false)}
          class={`items-center justify-center rounded-(--radius) p-1 hover:bg-muted active:opacity-60 ${focusRing}`}
        >
          <Icon name="close" size={16} color="muted-foreground" />
        </Button>
      </View>

      <Chassis class="gap-3 p-2.5">
        <SourceStage
          peak={peak()}
          peakL={peakL()}
          peakR={peakR()}
          rms={rms()}
          playing={playing()}
        />
        <DspMatrix
          flowing={player().state === "playing"}
          eqActive={engine.settings()?.eqEnabled ?? false}
          onOpenEq={() => {
            setEqOpenedFromOutput(true);
            setOutputOpen(false);
            open({ name: "eq" });
          }}
        />
        <View class="gap-1.5">
          <ModuleHeader index="03" title="BIT-PERFECT MASTER BUS" right={<Silkscreen tone="faint">SIGNAL INTEGRITY</Silkscreen>} />
          <MasterBus />
        </View>
        <View class="gap-1.5">
          <ModuleHeader index="04" title="OUTPUT INTERFACE & MASTER GAIN" />
          <OutputInterface />
        </View>
        <View class="gap-1.5">
          <ModuleHeader index="05" title="TELEMETRY & ROOT-CAUSE DIAGNOSTICS" />
          <TelemetryStage />
        </View>
      </Chassis>
    </Scroll>
  );
}
