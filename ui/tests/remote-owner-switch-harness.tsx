// 一行远程曲目属于引擎当前没连的服务器时：先切服务器，再取流。
// 这里不连引擎，用 setRemoteStatusForTest 模拟「服务器已就绪」。
import { signal, Text, View, mount } from "nsh";
import { Button } from "nsh/base";
import { engine } from "../src/backend/engine";
import { servers } from "../src/state/sources";
import { remoteProviders } from "../src/state/plugins";
import { remoteTracks, playRemote, setCachedRemoteTracksForTest, withRemotePath } from "../src/state/remote";

remoteTracks.set([]);
setCachedRemoteTracksForTest("srv-b", [
  {
    id: "b-1",
    title: "Server B Track",
    artist: "B Artist",
    album: "B Album",
    container: "flac",
    durationMs: 120000,
    size: null,
    coverArt: null,
  },
  {
    id: "b-2",
    title: "Server B Second",
    artist: "B Artist",
    album: "B Album",
    container: "flac",
    durationMs: 130000,
    size: null,
    coverArt: null,
  },
]);

const fetches = signal<string[]>([]);
const switches = signal<string[]>([]);
const fetched = signal("");
const played = signal("");

(engine as any).fetchRemote = (id: string) => fetches.update((xs) => [...xs, id]);
(engine as any).playContext = (items: any[]) => played.set(items[0]?.path ?? "");
(engine as any).listRemote = () => {};
(engine as any).setRemote = (_kind: string, params: Record<string, string>) => {
  switches.update((xs) => [...xs, params.base_url ?? ""]);
};

const track = { id: "b-1", title: "Server B Track", artist: "B Artist", album: "B Album", container: "flac", durationMs: 120000, size: null, coverArt: null };
const track2 = { id: "b-2", title: "Server B Second", artist: "B Artist", album: "B Album", container: "flac", durationMs: 130000, size: null, coverArt: null };

function Harness() {
  return (
    <View class="flex-1 gap-2 bg-background p-4">
      <Text>{`Servers: ${servers().map((s) => s.id).join(",") || "none"}`}</Text>
      <Text>{`Fetches: ${fetches().join(",") || "none"}`}</Text>
      <Text>{`Switched: ${switches().join(",") || "none"}`}</Text>
      <Text>{`Fetched: ${fetched() || "none"}`}</Text>
      <Text>{`Played: ${played() || "none"}`}</Text>
      <Button
        accessibilityLabel="播放 B 服务器的曲目"
        onPress={() => withRemotePath(track, (path) => fetched.set(path))}
        class="rounded bg-secondary px-3 py-1"
      >
        play b
      </Button>
      <Button
        accessibilityLabel="播放 B 服务器第二首"
        onPress={() => playRemote(track2)}
        class="rounded bg-secondary px-3 py-1"
      >
        play b2
      </Button>
      <Button
        accessibilityLabel="声明 source 形式"
        onPress={() =>
          remoteProviders.set([{ id: "jellyfin", name: "Jellyfin", fields: [], source: true }])
        }
        class="rounded bg-secondary px-3 py-1"
      >
        source
      </Button>
      <Button
        accessibilityLabel="模拟 B 服务器就绪"
        onPress={() => engine.setRemoteStatusForTest({ configured: true, kind: "jellyfin", baseUrl: "http://b.example" })}
        class="rounded bg-secondary px-3 py-1"
      >
        ready
      </Button>
    </View>
  );
}

mount(Harness);
