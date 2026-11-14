// An item's picture: the file's own embedded art (`state/artwork`), shown
// at the size it is drawn; while none is known, the quiet tile and its
// fallback glyph.

import { Image, View } from "nsh";
import { coverOf, remoteCoverOf } from "../../state/artwork";
import { Icon, type IconColor, type IconName } from "./icons";

export interface CoverProps {
  /** The file whose embedded picture it shows. */
  path: string;
  /** Points, when the size is not the caller's class's job. */
  size?: number;
  /** Round (artists), else square with the row radius. */
  round?: boolean;
  /** The glyph while there is no picture (music, disc, user). */
  fallback?: IconName;
  /** The glyph's colour token while there is no picture. */
  fallbackColor?: IconColor;
  class?: string;
}

export function Cover(props: CoverProps) {
  const src = () => coverOf(props.path);
  const glyph = () => (props.size ? Math.max(16, props.size * 0.42) : 72);
  return (
    <View
      class={`shrink-0 items-center justify-center overflow-hidden bg-secondary ${
        props.round ? "rounded-full" : "rounded-md"
      } ${props.size ? "" : "aspect-square w-full"} ${props.class ?? ""}`}
      style={props.size ? { width: props.size, height: props.size } : {}}
    >
      <Icon name={props.fallback ?? "music"} size={glyph()} color={props.fallbackColor ?? "muted-foreground"} />
      <If when={src()}>
        <Image src={src()} accessibilityElementsHidden class="absolute inset-0 object-cover" />
      </If>
    </View>
  );
}

/** A remote row's cover: keyed by the server's cover art, with `data =
 * null` legitimately leaving the glyph (服务器没图不是错误)。 */
export function RemoteCover(props: { id: string; coverArt: string | null; size?: number; class?: string }) {
  const src = () => remoteCoverOf(props.id, props.coverArt);
  const glyph = () => Math.max(16, (props.size ?? 36) * 0.42);
  return (
    <View
      class={`shrink-0 items-center justify-center overflow-hidden rounded-md bg-secondary ${props.class ?? ""}`}
      style={props.size ? { width: props.size, height: props.size } : {}}
    >
      <Icon name="music" size={glyph()} color="muted-foreground" />
      <If when={src()}>
        <Image src={src()} accessibilityElementsHidden class="absolute inset-0 object-cover" />
      </If>
    </View>
  );
}
