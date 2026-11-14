// 曲单相关的小对话框：新建（输入名字，重复名把 on_error 原话 toast）与
// 加入曲单（选一张已有曲单或新建）。句柄是模块级 signals——组件免于每次
// 挂载的交接（红心/右键菜单的调用方只需要 open 函数）。

import { signal, Scroll, Text, View, untracked } from "nsh";
import type { Playlist } from "../../backend/types";
import { Button, Input } from "nsh/base";
import {
  addToPlaylist,
  createPlaylist,
  playlists,
  renamePlaylist,
} from "../../state/user-catalog";
import { AppDialog } from "../ui/overlays";
import { ActionButton, TextLink, focusRing } from "../ui/controls";

/** 新建曲单：名字走小输入框。 */
const newOpen = signal(false);
export const newPlaylistOpen = () => newOpen();
export function setNewPlaylistOpen(open: boolean): void {
  newOpen.set(open);
}

/** 重命名曲单：预填当前名字。 */
const renaming = signal<{ id: number; name: string } | null>(null);
export function openRenamePlaylist(id: number, name: string): void {
  if (typeof id !== "number") return;
  renaming.set({ id, name });
}
export function setRenamePlaylistOpen(open: boolean): void {
  if (!open) renaming.set(null);
}
export const renamePlaylistOpen = () => renaming() !== null;

/** 加入曲单：一条或多条曲目准备入列（批量选中走同一个对话框）。 */
const adding = signal<{ paths: string[] } | null>(null);
export const addToPlaylistOpen = () => adding() !== null;
export function openAddToPlaylist(path: string, more?: string[]): void {
  if (!path) return;
  adding.set({ paths: more && more.length > 0 ? [...more] : [path] });
}
export function setAddToPlaylistOpen(open: boolean): void {
  if (!open) adding.set(null);
}

export function PlaylistDialogs() {
  return (
    <>
      <NewPlaylistDialog />
      <RenamePlaylistDialog />
      <AddToPlaylistDialog />
    </>
  );
}

function NewPlaylistDialog() {
  return (
    <AppDialog open={newOpen()} onOpenChange={(open) => setNewPlaylistOpen(open)} testID="new-playlist">
      <NameBody title="新建曲单" placeholder="曲单名" submitLabel="新建" submit={(name) => createPlaylist(name)} onClose={() => setNewPlaylistOpen(false)} />
    </AppDialog>
  );
}

function RenamePlaylistDialog() {
  return (
    <AppDialog open={renamePlaylistOpen()} onOpenChange={(open) => setRenamePlaylistOpen(open)} testID="rename-playlist">
      <If when={renaming()}>
        {(target) => (
          <NameBody
            title="重命名曲单"
            placeholder={target().name}
            submitLabel="重命名"
            initial={target().name}
            submit={(name) => renamePlaylist(target().id, name)}
            onClose={() => setRenamePlaylistOpen(false)}
          />
        )}
      </If>
    </AppDialog>
  );
}

/** One name field + the two bottom actions, as both dialogs' body. */
function NameBody(props: {
  title: string;
  placeholder?: string;
  submitLabel: string;
  initial?: string;
  submit: (name: string) => void;
  onClose: () => void;
}) {
  const name = signal(props.initial ?? "");
  const text = () => name();
  const go = () => {
    const value = untracked(name).trim();
    if (!value) return;
    props.submit(value);
    props.onClose();
  };
  return (
    <View class="gap-4 p-6">
      <Text role="header" class="text-xl font-bold text-foreground">
        {props.title}
      </Text>
      <View class="h-10 flex-row items-center gap-2 rounded-(--radius) border border-border bg-secondary px-3 focus-within:border-muted-foreground">
        <Input
          value={text()}
          onChange={({ value }) => name.set(value)}
          placeholder={props.placeholder}
          accessibilityLabel={props.title}
          autoCapitalize="none"
          autoCorrect={false}
          autoFocus
          class="flex-1 text-sm text-foreground"
        />
      </View>
      <View class="flex-row items-center justify-between">
        <TextLink label="取消" small onPress={props.onClose} />
        <View class="flex-1" />
        <ActionButton label={props.submitLabel} disabled={!text().trim()} onPress={go} />
      </View>
    </View>
  );
}

function AddToPlaylistDialog() {
  return (
    <AppDialog open={addToPlaylistOpen()} onOpenChange={setAddToPlaylistOpen} testID="add-to-playlist">
      <AddDialogBody />
    </AppDialog>
  );
}

function AddDialogBody() {
  const picked = signal<number | null>(null);
  // 不进 React 式缓存：对话框随 app 挂载，挂载时列表还是空的；每次读
  // store 才能在新建/删除后更新（曾经的 bug：恒空 → 无法添加）。
  const items = () => playlists();
  return (
    <View class="gap-4 p-6">
      <Text role="header" class="text-xl font-bold text-foreground">
        {untracked(adding)?.paths.length && untracked(adding)!.paths.length > 1
          ? `添加到曲单（${untracked(adding)!.paths.length} 首）`
          : "添加到曲单"}
      </Text>
      <If when={playlists().length > 0}>
        <Scroll class="max-h-60 gap-1">
          <Each of={items()}>
            {(list) => (
              <Button
                accessibilityLabel={`选择 ${list().name}`}
                selected={picked() === list().id}
                onPress={() => picked.set(list().id)}
                class={`flex-row items-center gap-3 rounded-(--radius) px-3 py-2 transition duration-150 ease-out ${
                  picked() === list().id ? "bg-accent" : "hover:bg-row-hover"
                } ${focusRing}`}
              >
                <Text class="min-w-0 flex-1 line-clamp-1 text-sm font-medium text-foreground">{list().name}</Text>
                <Text class="text-xs text-muted-foreground">{list().trackCount} 首</Text>
              </Button>
            )}
          </Each>
        </Scroll>
      </If>
      <View class="flex-row items-center justify-between">
        <TextLink label="新建曲单…" small onPress={() => setNewPlaylistOpen(true)} />
        <View class="flex-1" />
        <ActionButton
          label="添加"
          disabled={picked() === null}
          onPress={() => {
            const target = untracked(picked);
            const paths = untracked(adding)?.paths ?? [];
            if (target !== null) {
              for (const path of paths) addToPlaylist(target, path);
            }
            picked.set(null);
            setAddToPlaylistOpen(false);
          }}
        />
      </View>
    </View>
  );
}