// Feedback surfaces: the toasts, a page's error and empty states, the
// skeletons that stand in for content while it loads, and the reveal that
// hands over from one to the other.

import {
  Animated,
  cleanup,
  createAnimatedValue,
  createMotion,
  effect,
  Overlay,
  reduceMotion,
  safeArea,
  signal,
  Text,
  untracked,
  View,
  type AnimatedValue,
  type JSX,
} from "nsh";
import { Button } from "nsh/base";
import { chooseToast, dismissToast, toasts } from "../../state/feedback";
import { ActionButton, focusRing } from "./controls";
import { Icon } from "./icons";
import { REVEAL_IN, REVEAL_OUT } from "./motion";

/** The toasts: bottom right, above the player. */
export function Toaster(props: { bottomOffset?: number }) {
  return (
    <Overlay
      style={{
        pointerEvents: "box-none",
        left: 0,
        right: 0,
        bottom: 0,
        paddingHorizontal: 16,
        paddingBottom: safeArea().bottom + (props.bottomOffset ?? 16),
        alignItems: "flex-end",
      }}
    >
      <View class="w-90 max-w-full gap-2">
        <Each of={toasts()}>
          {(toast) => <ToastItem id={toast().id} />}
        </Each>
      </View>
    </Overlay>
  );
}

function ToastItem(props: { id: string }) {
  return (
    <If when={toasts().find((t) => t.id === props.id)}>
      {(toast) => (
        <View class="flex-row items-center gap-3 rounded-(--radius-card) border border-border bg-popover px-4 py-3 shadow-xl">
          <Text class="min-w-0 flex-1 text-sm font-medium text-popover-foreground">{toast().text}</Text>
          <If when={toast().action}>
            {(action) => (
              <Button onPress={() => chooseToast(props.id)} class={`shrink-0 rounded-full bg-primary px-3 py-1 active:opacity-70 ${focusRing}`}>
                <Text class="text-sm font-bold text-primary-foreground">{action().label}</Text>
              </Button>
            )}
          </If>
          <Button accessibilityLabel="关闭提示" onPress={() => dismissToast(props.id)} class={`shrink-0 rounded-(--radius) p-1 active:opacity-60 ${focusRing}`}>
            <Icon name="close" size={12} color="muted-foreground" />
          </Button>
        </View>
      )}
    </If>
  );
}

/** A page that couldn't show its content: one plain sentence, and Retry. */
export function ErrorState(props: {
  message: string;
  detail?: string;
  onRetry?: () => void;
  children?: JSX.Element;
}) {
  return (
    <View role="alert" class="items-center gap-3 px-6 py-16">
      <Text class="text-center text-lg font-bold text-foreground">{props.message}</Text>
      <If when={props.detail}>
        <Text class="max-w-md text-center text-sm text-muted-foreground">{props.detail}</Text>
      </If>
      <If when={props.children} else={props.onRetry ? <ActionButton label="重试" onPress={props.onRetry} /> : null}>
        {props.children}
      </If>
    </View>
  );
}

/** A page's empty state: a short title and what to do. */
export function EmptyState(props: { title: string; detail?: string; children?: JSX.Element }) {
  return (
    <View class="items-center gap-2 px-6 py-16">
      <Text class="text-center text-lg font-bold text-foreground">{props.title}</Text>
      <If when={props.detail}>
        <Text class="max-w-md text-center text-sm text-muted-foreground">{props.detail}</Text>
      </If>
      {props.children}
    </View>
  );
}

// ---- Skeletons and the reveal ----
//
// While content loads, a skeleton shaped like it pulses (each shape dims to
// half and back, once a second); when the content arrives the skeleton
// fades out as the content fades in over it, over the same 360ms,
// ease-in-out. Content already there when the page opens shows at once:
// no skeleton, no reveal.

/** The pulse the shapes made in one turn share: one opacity played once per
 * skeleton, stopped when its last shape goes. With reduced motion the
 * shapes rest fully shown. */
let forming: { value: AnimatedValue; users: number } | null = null;

function Block(props: { class: string }) {
  let pulse = forming;
  if (!pulse) {
    const made = { value: createAnimatedValue(1), users: 0 };
    if (!reduceMotion()) {
      // One pulse per turn: the shapes dim to half and back once a second.
      void made.value.keyframes([1, 0.5, 1], { duration: 1000, easing: "easeInOut", iterations: "infinite" });
    }
    forming = made;
    void Promise.resolve().then(() => {
      if (forming === made) forming = null;
    });
    pulse = made;
  }
  const joined = pulse;
  joined.users += 1;
  cleanup(() => {
    joined.users -= 1;
    if (joined.users === 0) void joined.value.stop();
  });
  return <View class={`rounded-sm bg-muted ${props.class}`} style={{ opacity: joined.value }} />;
}

/** Content that loads: `placeholder` (a skeleton, or an error) while there
 * is no `data`, then the reveal. `children` gets the data and whether it
 * was there at once (`instant`: a list's rows start their wave at once;
 * after a reveal, as the skeleton fades: `AFTER_REVEAL`). */
export function Reveal<T>(props: {
  data: T | undefined | null;
  placeholder: JSX.Element;
  class?: string;
  children: (data: () => T, instant: boolean) => JSX.Element;
}) {
  const instant = untracked(() => props.data != null);
  // Whether the placeholder has finished leaving.
  const gone = signal(instant);
  effect(() => {
    if (props.data == null) untracked(() => gone.set(false));
  });
  const contentMotion = untracked(() => (instant ? undefined : createMotion({ enter: REVEAL_IN }).style));
  const leavingMotion = createMotion();
  effect(() => {
    if (props.data == null) return;
    untracked(() => void leavingMotion.play(REVEAL_OUT).then(() => gone.set(true)));
  });
  cleanup(() => leavingMotion.stop());
  return (
    <View class={`relative ${props.class ?? ""}`}>
      <If when={props.data}>
        {(data) => (
          <View class={props.class ?? ""} style={contentMotion}>
            {props.children(data as () => T, instant)}
          </View>
        )}
      </If>
      <If when={!gone()}>
        <View class={props.data == null ? "" : "pointer-events-none absolute inset-x-0 top-0"} style={leavingMotion.style}>
          {props.placeholder}
        </View>
      </If>
    </View>
  );
}

// ---- The skeleton shapes ----

/** Track rows loading, as a track list lays them out. */
export function RowsSkeleton(props: { rows?: number; header?: boolean }) {
  const rows = Array.from({ length: props.rows ?? 8 }, (_, i) => i);
  const widths = ["w-2/5", "w-1/2", "w-1/3", "w-3/5"];
  const seconds = ["w-1/4", "w-1/5", "w-1/3", "w-1/4"];
  return (
    <View accessibilityLabel="加载中" testID="skeleton">
      <If when={props.header}>
        <View class="mx-6 mb-1 h-8 border-b border-border/60" />
      </If>
      <Each of={rows}>
        {(i) => (
          <View class="mx-6 h-12 flex-row items-center gap-3 px-3">
            <View class="w-6 items-end">
              <Block class="h-3 w-3" />
            </View>
            <Block class="size-9" />
            <View class="min-w-0 flex-1 gap-1.5">
              <Block class={`h-3 ${widths[i() % 4]}`} />
              <Block class={`h-2.5 ${seconds[i() % 4]}`} />
            </View>
            <View class="w-12 items-end">
              <Block class="h-3 w-8" />
            </View>
          </View>
        )}
      </Each>
    </View>
  );
}

/** A collection page loading: its header (cover, kind, name, play button)
 * over track rows. */
export function CollectionSkeleton() {
  return (
    <View accessibilityLabel="加载中" testID="skeleton">
      <View class="flex-row items-end gap-4 pl-[30px] pr-6 pb-4 pt-1">
        <Block class="size-24 rounded-(--radius)" />
        <View class="min-w-0 flex-1 gap-2 pb-1">
          <Block class="h-2.5 w-14" />
          <Block class="h-7 w-72 max-w-full" />
          <Block class="h-3 w-40" />
        </View>
        <Block class="size-11 shrink-0 rounded-full" />
      </View>
      <RowsSkeleton header />
    </View>
  );
}

/** Rows of cards loading, shaped like a card row. */
export function CardsSkeleton(props: { rows?: number }) {
  const rows = Array.from({ length: props.rows ?? 2 }, (_, i) => i);
  return (
    <View accessibilityLabel="加载中" class="gap-8" testID="skeleton">
      <Each of={rows}>
        {() => (
          <View class="gap-2">
            <Block class="mx-6 h-6 w-48" />
            <View class="h-[200px] flex-row gap-3 overflow-hidden px-3 pt-1">
              <Each of={[0, 1, 2, 3, 4, 5, 6, 7]}>
                {() => (
                  <View class="w-36 shrink-0 gap-2 p-2.5">
                    <Block class="aspect-square w-full rounded-(--radius)" />
                    <View class="gap-1.5">
                      <Block class="h-3 w-4/5" />
                      <Block class="h-2.5 w-1/2" />
                    </View>
                  </View>
                )}
              </Each>
            </View>
          </View>
        )}
      </Each>
    </View>
  );
}

/** A small spinner for work behind content that still shows (a search
 * running again). Still with reduced motion. */
export function Spinner(props: { label: string }) {
  const motion = createMotion();
  void motion.play("spin", { duration: 900, iterations: "infinite", easing: "linear" });
  cleanup(() => motion.stop());
  return (
    <View
      role="progressbar"
      accessibilityLabel={props.label}
      class="size-4 rounded-full border-2 border-muted-foreground border-t-transparent"
      style={motion.style}
    />
  );
}
