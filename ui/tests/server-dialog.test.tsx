// 连接表单：按了「连接」必须有回话。iOS 的 http 只通往局域网。
import { expect, open, test } from "nsh/test";

async function dialog() {
  const app = await open("./server-dialog-harness.tsx", {
    platform: "ios",
    width: 390,
    height: 844,
  });
  await app.waitFor("id:server-form", { timeout: 10_000 });
  return app;
}

test("空地址在按下连接时说出来", async () => {
  const app = await dialog();
  await app.scroll("id:server-form", 800);
  await app.press("id:btn-save-server");
  await app.waitFor("id:server-form-error", { timeout: 5000 });
  expect(app.shows("text:请输入服务器地址")).toBe(true);
}, 60_000);

test("iOS 拒绝公网地址走明文 http", async () => {
  const app = await dialog();
  await app.type("id:input-server-url", "http://example.com:8096");
  await app.scroll("id:server-form", 800);
  await app.press("id:btn-save-server");
  await app.waitFor("text:iOS 只允许局域网服务器使用 http，其他地址请用 https", {
    timeout: 5000,
  });
}, 60_000);

test("密码模式缺用户名/密码时不发请求", async () => {
  const app = await dialog();
  await app.type("id:input-server-url", "http://192.168.1.10:8096");
  await app.scroll("id:server-form", 800);
  await app.press("id:btn-save-server");
  await app.waitFor("text:请输入用户名", { timeout: 5000 });
}, 60_000);

test("Token 模式缺 Token 时说明缺什么", async () => {
  const app = await dialog();
  await app.type("id:input-server-url", "http://192.168.1.10:8096");
  await app.press("text:API Token");
  await app.scroll("id:server-form", 800);
  await app.press("id:btn-save-server");
  await app.waitFor("text:请输入 API Token", { timeout: 5000 });
}, 60_000);
