const { chromium } = require("playwright");
const http = require("node:http");
const fs = require("node:fs/promises");
const path = require("node:path");
const assert = require("node:assert/strict");

async function main() {
  const root = path.resolve(__dirname, "../../build/web");
  const server = http.createServer(async (request, response) => {
    try {
      const pathname = new URL(request.url, "http://localhost").pathname;
      const filename = path.resolve(
        root,
        "." + (pathname === "/" ? "/index.html" : pathname),
      );
      if (!filename.startsWith(root + path.sep))
        throw new Error("Outside web root");
      const content = await fs.readFile(filename);
      const type =
        {
          ".html": "text/html",
          ".js": "text/javascript",
          ".css": "text/css",
          ".svg": "image/svg+xml",
        }[path.extname(filename)] || "application/octet-stream";
      response.writeHead(200, { "Content-Type": type });
      response.end(content);
    } catch {
      response.writeHead(404);
      response.end();
    }
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  let browser;
  try {
    browser = await chromium.launch({
      executablePath:
        process.argv[2] ||
        "C:/Program Files/Google/Chrome/Application/chrome.exe",
      headless: true,
    });
    const page = await browser.newPage({
      viewport: { width: 1440, height: 900 },
    });
    page.setDefaultTimeout(5000);
    const errors = [];
    page.on("pageerror", (error) => errors.push(error.message));

    let authenticated = false;
    const restartAllowed = true;
    let sessionRequests = 0;
    const restartRequests = [];
    await page.route("**/api/**", async (route) => {
      const request = route.request();
      const endpoint = new URL(request.url()).pathname;
      let status = 200;
      let value = {};
      if (endpoint === "/api/me") {
        status = authenticated ? 200 : 401;
        value = { id: "restart-user", username: "restart-test" };
      } else if (endpoint === "/api/login") {
        authenticated = true;
        value = { id: "restart-user", username: "restart-test" };
      } else if (endpoint === "/api/devices") {
        value = [
          {
            id: "restart-device-01",
            name: "重启测试设备",
            online: true,
            last_seen: null,
          },
        ];
      } else if (
        endpoint === "/api/devices/restart-permission" &&
        request.method() === "GET"
      ) {
        assert.equal(
          new URL(request.url()).searchParams.get("deviceId"),
          "restart-device-01",
        );
        value = { allowed: restartAllowed };
      } else if (endpoint === "/api/sessions") {
        const body = request.postDataJSON();
        assert.equal(body.device_id, "restart-device-01");
        sessionRequests++;
        value = {
          id: `restart-session-${sessionRequests}`,
          channels: [
            { id: "video", purpose: 2, ticket: "fixture" },
            { id: "input", purpose: 3, ticket: "fixture" },
          ],
        };
      } else if (
        /^\/api\/sessions\/restart-session-\d+\/close$/.test(endpoint)
      ) {
        value = {};
      } else if (endpoint === "/api/devices/restart") {
        const body = request.postDataJSON();
        restartRequests.push(body);
        assert.equal(body.deviceId, "restart-device-01");
        if (restartRequests.length === 1) {
          status = 503;
          value = { error: "fixture unavailable" };
        } else if (restartRequests.length === 2) {
          await route.abort("timedout");
          return;
        } else value = { id: "restart-operation-01", status: "requested" };
      } else {
        throw new Error(
          `Unexpected API request ${request.method()} ${endpoint}`,
        );
      }
      await route.fulfill({ status, json: value });
    });
    await page.routeWebSocket("**/api/channels/browser", (socket) => {
      socket.onMessage(() => {});
    });

    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.getByRole("heading", { name: "登录工作台" }).waitFor();
    await page.getByLabel("用户名").fill("restart-test");
    await page.getByLabel("密码").fill("fixture-password");
    await page.getByRole("button", { name: /^登\s*录$/ }).click();
    await page.getByRole("heading", { name: "我的设备" }).waitFor();
    await page.getByRole("button", { name: /^连接.+在线/ }).click();
    await page.getByRole("region", { name: "远程桌面" }).waitFor();
    const restart = page.getByRole("button", { name: "重启设备", exact: true });
    await page.waitForFunction(() =>
      [...document.querySelectorAll("button")].some(
        (button) => button.textContent.includes("重启设备") && !button.disabled,
      ),
    );
    // Stale workflow state from the prior release must never disable restart.
    await page.evaluate(() =>
      sessionStorage.setItem(
        "device-restart:restart-user:restart-device-01",
        JSON.stringify({ key: "old", id: "old", status: "unknown" }),
      ),
    );
    for (const label of [
      "授权重启权限",
      "人工结案",
      "查询状态",
      "核对/重新提交同一请求",
    ]) {
      assert.equal(
        await page.getByRole("button", { name: label, exact: true }).count(),
        0,
      );
    }
    async function confirmRestart() {
      const count = restartRequests.length;
      await restart.click();
      const dialog = page.getByRole("dialog");
      await dialog.waitFor();
      assert.equal(restartRequests.length, count);
      assert.equal(await restart.isEnabled(), true);
      await dialog.getByRole("button", { name: "立即重启" }).click();
      await dialog.waitFor({ state: "hidden" });
      assert.equal(await restart.isEnabled(), true);
      assert.equal(await restart.textContent(), "重启设备");
    }
    await restart.click();
    await page
      .getByRole("dialog")
      .getByRole("button", { name: /^取\s*消$/ })
      .click();
    await page.getByRole("dialog").waitFor({ state: "hidden" });
    assert.equal(restartRequests.length, 0);
    assert.equal(sessionRequests, 1);
    await confirmRestart();
    await page.getByText("重启指令未确认，请重试").last().waitFor();
    assert.equal(restartRequests.length, 1);
    assert.equal(await page.getByRole("dialog").count(), 0);
    assert.equal(await restart.isEnabled(), true);
    await confirmRestart();
    await page.getByText("重启指令未确认，请重试").last().waitFor();
    assert.equal(restartRequests.length, 2);
    assert.equal(
      restartRequests[1].idempotencyKey,
      restartRequests[0].idempotencyKey,
    );
    await confirmRestart();
    await page.getByText("重启指令已发送").last().waitFor();
    assert.equal(restartRequests.length, 3);
    assert.equal(
      restartRequests[2].idempotencyKey,
      restartRequests[0].idempotencyKey,
    );
    assert.equal(await restart.isEnabled(), true);
    await confirmRestart();
    await page.getByText("重启指令已发送").last().waitFor();
    assert.equal(restartRequests.length, 4);
    assert.notEqual(
      restartRequests[3].idempotencyKey,
      restartRequests[0].idempotencyKey,
    );
    assert.equal(await page.getByRole("dialog").count(), 0);
    assert.deepEqual(errors, []);
    console.log(
      "PASS: single confirmation, toast feedback, unchanged restart button, no workflow, explicit retry idempotency, old records never lock the button",
    );
  } finally {
    await browser?.close();
    await new Promise((resolve) => server.close(resolve));
  }
}
main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
