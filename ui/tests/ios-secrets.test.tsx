// iOS 的远程凭据：钥匙串是唯一的保存处，花名册里只有 hasSecret。
// 连接时从钥匙串取；编辑时预填；钥匙串空了就要求重新输入。
import { expect, open, test, type App as TestApp } from "nsh/test";

const SERVERS = JSON.stringify([
  {
    id: "srv-b",
    label: "Server B",
    kind: "jellyfin",
    baseUrl: "http://192.168.1.10:8096",
    user: "listener",
    hasSecret: true,
  },
]);

const KEYCHAIN = JSON.stringify({ "srv-b": { password: "hunter2" } });

/** Storage the app itself owns. The `test:` keys are this test's seeds. */
function appStorage(app: TestApp): string {
  return Object.entries(app.storage())
    .filter(([key]) => !key.startsWith("test:"))
    .map(([, value]) => value)
    .join("\n");
}

async function roster(extra: Record<string, string>): Promise<TestApp> {
  const app = await open("./ios-secrets-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: { "remote:servers": SERVERS, "source:active": "all", test: "x", ...extra },
  });
  await app.waitFor("text:ready", { timeout: 10_000 });
  return app;
}

test("连接一台没有密码的服务器时，密码来自钥匙串而不是花名册", async () => {
  const app = await roster({ "test:keychain": KEYCHAIN });
  await app.waitFor("text:Server B", { timeout: 10_000 });
  await app.press("label:连接 Server B");
  await app.waitFor("text:移除来源", { timeout: 10_000 });
  expect(app.shows("text:sent-password")).toBe(true);
  // The secret never appears in what the app persists.
  expect(appStorage(app).includes("hunter2")).toBe(false);
}, 60_000);

test("编辑服务器从钥匙串预填，保存后花名册仍然没有密码", async () => {
  const app = await open("./ios-secrets-dialog-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: { "remote:servers": SERVERS, "test:keychain": KEYCHAIN },
  });
  await app.waitFor("text:ready", { timeout: 10_000 });
  await app.press("label:编辑 Server B");
  await app.scroll("id:server-form", 800);
  await app.press("id:btn-save-server");
  // Prefilled from the Keychain, so validation passes and the item is
  // rewritten; the roster keeps only the marker.
  await app.waitFor("text:writes:1", { timeout: 10_000 });
  const stored = appStorage(app);
  expect(stored.includes("hunter2")).toBe(false);
  expect(stored.includes('"hasSecret":true')).toBe(true);
}, 60_000);

test("钥匙串里没有凭据时，保存要求重新输入", async () => {
  const app = await open("./ios-secrets-dialog-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
    storage: { "remote:servers": SERVERS },
  });
  await app.waitFor("text:ready", { timeout: 10_000 });
  await app.press("label:编辑 Server B");
  await app.scroll("id:server-form", 800);
  await app.press("id:btn-save-server");
  await app.waitFor("text:请输入密码", { timeout: 5000 });
}, 60_000);
