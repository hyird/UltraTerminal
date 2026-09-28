const { chromium } = require("playwright");
const http = require("node:http");
const fs = require("node:fs/promises");
const path = require("node:path");
const assert = require("node:assert/strict");

// Current production UI with isolated API/socket fixtures; no Windows input.
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
        { ".html": "text/html", ".js": "text/javascript", ".css": "text/css" }[
          path.extname(filename)
        ] || "application/octet-stream";
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
    let authenticated = false,
      opens = 0,
      rejection = 0;
    const closed = [],
      modes = [],
      connections = [],
      requestedDevices = [];
    const devices = [
      {
        id: "dev-design-01",
        name: "设计工作站",
        online: true,
        can_manage: true,
        last_seen: "2026-09-18T10:12:00Z",
      },
      {
        id: "dev-office-03",
        name: "办公室电脑",
        online: false,
        can_manage: true,
        last_seen: null,
      },
    ];
    await page.route("**/api/**", async (route) => {
      const endpoint = new URL(route.request().url()).pathname;
      let status = 200,
        value = {};
      if (endpoint === "/api/me") {
        status = authenticated ? 200 : 401;
        value = { id: "ui-user", username: "workspace-test" };
      } else if (endpoint === "/api/login") {
        authenticated = true;
        value = { id: "ui-user", username: "workspace-test" };
      } else if (endpoint === "/api/logout") authenticated = false;
      else if (endpoint === "/api/devices") value = devices;
      else if (endpoint === "/api/enrollments") {
        const body = route.request().postDataJSON();
        assert.equal(body.device_id, "d-" + "b".repeat(64));
        assert.equal(body.platform, "windows-x64");
        status = 201;
        value = {
          code: "a".repeat(64),
          device_id: body.device_id,
          web_origin: "https://test.a-z.xin",
          native_host: "test.a-z.xin",
          native_address: "103.236.69.112",
          native_port: 7443,
          expires_in: 600,
        };
      }
      else if (endpoint === "/api/devices/create") {
        value = { id: "d-" + "b".repeat(64), name: route.request().postDataJSON().name, online: false, enrolled: false, can_manage: true, last_seen: null };
        devices.push(value);
        status = 201;
      }
      else if (endpoint === "/api/devices/rename") {
        const body = route.request().postDataJSON();
        const device = devices.find((item) => item.id === body.device_id);
        assert.ok(device);
        device.name = body.name;
        value = { id: device.id, name: device.name };
      } else if (endpoint === "/api/devices/delete") {
        const body = route.request().postDataJSON();
        const index = devices.findIndex((item) => item.id === body.device_id);
        assert.ok(index >= 0);
        devices.splice(index, 1);
        value = { id: body.device_id, deleted: true };
      } else if (endpoint === "/api/devices/restart-permission")
        value = { allowed: false };
      else if (endpoint === "/api/devices/restart/current")
        value = { id: "", status: "none" };
      else if (endpoint === "/api/sessions") {
        opens++;
        const requestBody = route.request().postDataJSON();
        requestedDevices.push(requestBody.device_id);
        modes.push(requestBody.write_control);
        if (rejection) status = rejection;
        value = {
          id: `ui-session-${opens}`,
          channels: [
            { id: "video", purpose: 2, ticket: "fixture" },
            { id: "input", purpose: 3, ticket: "fixture" },
          ],
        };
      } else if (/^\/api\/sessions\/ui-session-\d+\/close$/.test(endpoint))
        closed.push(endpoint);
      else if (/^\/api\/sessions\/ui-session-\d+$/.test(endpoint))
        value = { state: "closed", end_reason: "connection_lost" };
      else throw new Error(`Unexpected request ${endpoint}`);
      await route.fulfill({ status, json: value });
    });
    await page.routeWebSocket("**/api/desktop/{control,video}", (socket) => {
      connections.push(socket);
      socket.onMessage(() => {});
    });
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.getByRole("heading", { name: "登录工作台" }).waitFor();
    await page.getByLabel("用户名").fill("workspace-test");
    await page.getByLabel("密码").fill("ui-fixture-password");
    await page.getByRole("button", { name: /^登\s*录$/ }).click();
    await page.getByRole("heading", { name: "我的设备" }).waitFor();
    await page.evaluate(() => {
      Object.defineProperty(navigator, "clipboard", {
        configurable: true,
        value: { writeText: async (value) => { window.__copied = value; } },
      });
    });
    await page.getByRole("button", { name: "添加设备" }).click();
    await page.getByLabel("新设备名称").fill("MSI测试设备");
    await page.getByRole("button", { name: /^添\s*加$/ }).click();
    await page.getByRole("button", { name: "管理MSI测试设备" }).click();
    await page.getByRole("menuitem", { name: "安装命令" }).click();
    await page.getByRole("combobox", { name: "安装平台" }).click();
    await page.getByText("Linux · 暂未支持", { exact: true }).waitFor();
    await page.keyboard.press("Escape");
    await page.getByRole("button", { name: "复制一键安装命令" }).click();
    await page.getByText("安装命令已复制", { exact: true }).waitFor();
    const copied = await page.evaluate(() => window.__copied);
    assert.match(copied, /irm.*install\.ps1.*\| iex/);
    assert.ok(copied.startsWith('powershell -NoProfile -c "irm '));
    assert.ok(copied.includes("a".repeat(64)));
    assert.ok(!copied.includes("\n") && copied.length < 180);
    await page.getByRole("button", { name: "关闭", exact: true }).click();
    await page.getByRole("button", { name: "管理MSI测试设备" }).click();
    await page.getByRole("menuitem", { name: "删除" }).click();
    await page.getByRole("button", { name: /^删\s*除$/ }).click();
    await page.getByRole("button", { name: "管理MSI测试设备" }).waitFor({ state: "detached" });
    const connect = page.getByRole("button", {
      name: /^连接.+（(?:在线|离线)）$/,
    });
    await connect.last().waitFor();
    assert.equal(await connect.count(), 2);
    assert.equal(
      await connect.last().isDisabled(),
      false,
      "Offline devices should still allow a connection attempt",
    );
    assert.match(await connect.last().getAttribute("aria-label"), /离线/);
    rejection = 409;
    await connect.last().click();
    await page
      .getByRole("alert")
      .filter({ hasText: "设备当前不可用" })
      .waitFor();
    assert.ok(
      requestedDevices.includes("dev-office-03"),
      "Offline connection attempt was not sent for the selected device",
    );
    assert.equal(
      await page.getByRole("heading", { name: "我的设备" }).count(),
      1,
      "Failed offline attempt should leave the device list usable",
    );
    rejection = 0;
    await page
      .getByRole("searchbox", { name: "搜索设备" })
      .fill("不存在的设备");
    await page.getByText("没有匹配的设备", { exact: true }).waitFor();
    await page.getByRole("searchbox", { name: "搜索设备" }).fill("");
    await connect.first().waitFor();
    const layoutBefore = await page.locator("main").boundingBox();
    await connect.first().click();
    const region = page.getByRole("region", { name: "远程桌面" });
    const connecting = page.getByRole("status").filter({ hasText: "正在连接" });
    const recovering = page.getByRole("status").filter({ hasText: "正在重连" });
    const disconnect = page.getByRole("button", {
      name: "断开连接",
      exact: true,
    });
    await connecting.waitFor();
    assert.deepEqual(
      await page.locator("main").boundingBox(),
      layoutBefore,
      "Opening remote desktop changed the application layout",
    );
    assert.equal(
      await page.evaluate(() => document.fullscreenElement !== null),
      false,
    );
    assert.equal(opens, 2);
    assert.equal(connections.length, 2);
    for (const viewport of [
      { width: 1440, height: 900 },
      { width: 1024, height: 600 },
      { width: 390, height: 844 },
    ]) {
      await page.setViewportSize(viewport);
      assert.equal(
        await page.evaluate(
          () =>
            document.documentElement.scrollHeight <= innerHeight &&
            document.documentElement.scrollWidth <= innerWidth,
        ),
        true,
        "Remote workspace scrolls",
      );
      assert.ok(
        (await page
          .locator(".desktop-viewport")
          .evaluate((e) => e.clientHeight / e.closest("main").clientHeight)) >
          0.75,
        "Layout consumes too much desktop height",
      );
    }
    await page.setViewportSize({ width: 1440, height: 900 });
    // Both peer channels can close together; only one replacement session is allowed.
    for (let cycle = 0; cycle < 3; ++cycle) {
      const before = opens;
      const channels = connections.slice(-2);
      await Promise.all(
        channels.map((socket) =>
          socket.close({ code: 1011, reason: "fixture interruption" }),
        ),
      );
      await recovering.waitFor();
      await connecting.waitFor();
      assert.equal(
        opens,
        before + 1,
        "Interrupted desktop did not create exactly one replacement",
      );
      assert.ok(
        closed.includes(`/api/sessions/ui-session-${before}/close`),
        "Old session was not closed before replacement",
      );
      assert.equal(
        connections.length,
        2 * (opens - 1),
        "Recovery duplicated browser channels",
      );
    }
    const beforeCancel = opens;
    await connections
      .at(-1)
      .close({ code: 1011, reason: "cancel pending recovery" });
    await recovering.waitFor();
    await disconnect.click();
    await region.waitFor({ state: "detached" });
    await page.waitForTimeout(1500);
    assert.equal(
      opens,
      beforeCancel,
      "Explicit disconnect did not cancel recovery",
    );
    // Each tab owns its channels and recovery; switching never recreates them.
    await connect.first().click();
    await connecting.waitFor();
    const firstSession = opens;
    await page.getByRole("tab", { name: "我的设备", exact: true }).click();
    await connect.last().click();
    await connecting.waitFor();
    const secondSession = opens;
    assert.equal(secondSession, firstSession + 1);
    const beforeSwitch = connections.length;
    const firstTab = page.getByRole("tab", { name: "设计工作站", exact: true });
    const secondTab = page.getByRole("tab", {
      name: "办公室电脑",
      exact: true,
    });
    await firstTab.click();
    await secondTab.click();
    await firstTab.click();
    assert.equal(opens, secondSession, "Switching tabs created a session");
    assert.equal(
      connections.length,
      beforeSwitch,
      "Switching tabs recreated channels",
    );
    assert.equal(await page.locator(".session-content").count(), 2);
    assert.equal(await page.locator(".session-content:visible").count(), 1);
    // Reconnecting a hidden tab must not steal selection or stop the foreground one.
    await Promise.all(
      connections
        .slice(-2)
        .map((socket) =>
          socket.close({ code: 1011, reason: "background interruption" }),
        ),
    );
    const background = page
      .locator(".session-content")
      .filter({ hasText: "办公室电脑" });
    await background
      .getByText("正在重连…", { exact: true })
      .waitFor({ state: "attached" });
    await background
      .getByText("正在连接远程桌面…", { exact: true })
      .waitFor({ state: "attached" });
    assert.equal(opens, secondSession + 1);
    assert.equal(await firstTab.getAttribute("aria-selected"), "true");
    assert.equal(
      closed.includes(`/api/sessions/ui-session-${firstSession}/close`),
      false,
    );
    const backgroundSession = opens;
    await page
      .locator(".ant-tabs-tab")
      .filter({ has: secondTab })
      .getByRole("button")
      .click();
    await secondTab.waitFor({ state: "detached" });
    assert.equal(await firstTab.getAttribute("aria-selected"), "true");
    assert.ok(
      closed.includes(`/api/sessions/ui-session-${backgroundSession}/close`),
    );
    assert.equal(await page.locator(".session-content").count(), 1);
    await page.getByRole("tab", { name: "我的设备", exact: true }).click();
    await connect.first().click();
    assert.equal(
      opens,
      backgroundSession,
      "Opening an existing device duplicated its tab",
    );
    assert.equal(await firstTab.getAttribute("aria-selected"), "true");
    await disconnect.click();
    await region.waitFor({ state: "detached" });
    assert.ok(
      closed.includes(`/api/sessions/ui-session-${firstSession}/close`),
    );
    // Revoked authorization must stop retries instead of silently opening read-only.
    await connect.first().click();
    await connecting.waitFor();
    const beforeDenial = opens;
    rejection = 403;
    await connections
      .at(-1)
      .close({ code: 1011, reason: "authorization revoked" });
    await page
      .getByRole("alert")
      .filter({ hasText: "远程控制授权已失效" })
      .waitFor();
    await page.waitForTimeout(1500);
    assert.equal(
      opens,
      beforeDenial + 1,
      "Authorization denial kept retrying or downgraded control",
    );
    assert.equal(modes.at(-1), true);
    await disconnect.click();
    await region.waitFor({ state: "detached" });
    rejection = 0;
    await connect.first().click();
    await connecting.waitFor();
    const managedSession = opens;
    await page.getByRole("tab", { name: "我的设备", exact: true }).click();
    await page
      .getByRole("button", { name: "管理设计工作站", exact: true })
      .click();
    await page.getByRole("menuitem", { name: "重命名", exact: true }).click();
    await page
      .getByRole("textbox", { name: "设备名称", exact: true })
      .fill("新的设备名称");
    await page
      .getByRole("dialog")
      .getByRole("button", { name: /^保\s*存$/ })
      .click();
    await page.getByRole("dialog").waitFor({ state: "hidden" });
    await page
      .getByRole("tab", { name: "新的设备名称", exact: true })
      .waitFor();
    assert.equal(
      opens,
      managedSession,
      "Rename recreated the active connection",
    );
    await page
      .getByRole("button", { name: "管理新的设备名称", exact: true })
      .click();
    await page.getByRole("menuitem", { name: "删除", exact: true }).click();
    await page
      .getByRole("dialog")
      .getByRole("button", { name: /^取\s*消$/ })
      .click();
    await page.getByRole("dialog").waitFor({ state: "hidden" });
    assert.equal(devices.length, 2);
    await page
      .getByRole("button", { name: "管理新的设备名称", exact: true })
      .click();
    await page.getByRole("menuitem", { name: "删除", exact: true }).click();
    await page
      .getByRole("dialog")
      .getByRole("button", { name: /^删\s*除$/ })
      .click();
    await page.getByRole("dialog").waitFor({ state: "hidden" });
    await page
      .getByRole("tab", { name: "新的设备名称", exact: true })
      .waitFor({ state: "detached" });
    assert.equal(devices.length, 1);
    assert.ok(
      closed.includes(`/api/sessions/ui-session-${managedSession}/close`),
    );
    await page.getByRole("button", { name: "账户菜单" }).click();
    await page.getByRole("menuitem", { name: "退出登录" }).click();
    await page.getByRole("heading", { name: "登录工作台" }).waitFor();
    assert.deepEqual(errors, []);
    console.log(
      "PASS: current template login/search/offline actions, compact viewport, three dual-channel failures, independent multi-tab recovery and switching, disconnect cancellation and revoked authorization",
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
