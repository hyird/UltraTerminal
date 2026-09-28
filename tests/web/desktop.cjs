const { chromium } = require('playwright');
const fs = require('node:fs/promises');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const { createInterface } = require('node:readline');
const protobuf = require('../../web/node_modules/protobufjs');
const { desktopNetwork } = require('./desktop_network.fixture.cjs');
async function until(check, message, timeout = 4000) {
  const end = Date.now() + timeout;
  while (!check()) {
    if (Date.now() >= end) throw new Error(message);
    await new Promise(resolve => setTimeout(resolve, 10));
  }
}
async function checkCapturePattern(page, alternate = false) {
  const expected = [[208, 32, 32], [32, 208, 32], [32, 32, 208], [208, 208, 32]];
  if (alternate) expected.reverse();
  await page.waitForFunction(colors => {
    const canvas = document.querySelector('canvas[aria-label="桌面画面"]');
    if (!canvas) return false;
    const context = canvas.getContext('2d');
    return colors.every((color, index) => {
      const sample = context.getImageData(64 + index * 64, 96, 1, 1).data;
      return color.every((value, channel) => Math.abs(sample[channel] - value) < 35);
    });
  }, expected, { timeout: 5000 }).catch(async () => {
    const samples = await page.locator('canvas[aria-label="桌面画面"]').evaluate(canvas =>
      [0, 1, 2, 3].map(index => Array.from(canvas.getContext('2d').getImageData(64 + index * 64, 96, 1, 1).data)));
    throw new Error(`Captured Windows color pattern differs: ${JSON.stringify(samples)}`);
  });
}
async function main() {
  let input = '';
  for await (const part of process.stdin) input += part;
  const settings = JSON.parse(input);
  const motionSeconds = settings.motionSeconds ?? 3;
  if (!Number.isInteger(motionSeconds) || motionSeconds < 3 || motionSeconds > 3600)
    throw new Error('Invalid desktop motion duration');
  const network = process.env.ULTRATERIMAL_DESKTOP_SHAPED === '1' ? await desktopNetwork(settings.origin) : null;
  const browser = await chromium.launch({ executablePath: process.argv[2], headless: true,
    args: [`--ignore-certificate-errors-spki-list=${settings.pin}`,
      ...(network ? [`--proxy-server=${network.address}`, '--proxy-bypass-list=<-loopback>'] : [])],
  }).catch(async error => { await network?.close(); throw error; });
  const observer = spawn(settings.inputObserver, ['--duration', String(150 + motionSeconds)], { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
  const inputEvents = [];
  const nativeTestKeys = [];
  let observerReady = false;
  const lines = createInterface({ input: observer.stdout });
  lines.on('line', line => {
    const event = JSON.parse(line);
    if (event.ready) observerReady = true;
    else {
      inputEvents.push(event);
      if (!event.mouse && event.code === 0x87)
        nativeTestKeys.push({ message: event.message, at_ms: performance.now() });
    }
  });
  try {
    await until(() => observerReady, 'Native input observer did not start');
    const verifyCapturePattern = async (page, alternate = false) => {
      try {
        await checkCapturePattern(page, alternate);
      } catch (error) {
        const observerStopped = observer.exitCode !== null || observer.signalCode !== null;
        const capture = await page.locator('canvas[aria-label="桌面画面"]').evaluate(canvas => ({
          frame: canvas.dataset.frame, generation: canvas.dataset.generation,
          width: canvas.width, height: canvas.height,
        })).catch(() => null);
        await page.screenshot({ path: path.join(settings.screenshots, 'capture-failure.png') }).catch(() => {});
        const status = await page.getByRole('status').allTextContents().catch(() => []);
        const sockets = await page.evaluate(() => window.fixtureDesktopSockets?.size ?? null).catch(() => null);
        const native = spawnSync(path.join(path.dirname(settings.inputObserver), 'agent-capture-tests.exe'), ['--diagnose'],
          { windowsHide: true, timeout: 6000, encoding: 'utf8' });
        const detail = `observer_exited=${observerStopped} code=${observer.exitCode} signal=${observer.signalCode} capture=${JSON.stringify(capture)} status=${JSON.stringify(status)} sockets=${sockets} native=${JSON.stringify({ code: native.status, output: native.stdout?.trim(), error: native.error?.message || native.stderr?.trim() })}; ${error.message}`;
        throw new Error(observerStopped ? `Input observer fixture exited early: ${detail}` : `Desktop capture mismatch: ${detail}`);
      }
    };
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    page.setDefaultTimeout(5000);
    // Keep the real WebSocket implementation; expose only live desktop sockets
    // to the fixture so it can interrupt transport without clicking disconnect.
    await page.addInitScript(() => {
      const NativeWebSocket = window.WebSocket;
      window.fixtureDesktopSockets = new Set();
      window.fixtureMaxDesktopBuffered = 0;
      window.WebSocket = class extends NativeWebSocket {
        send(data) {
          if (window.fixtureDropDesktop && window.fixtureDesktopSockets.has(this)) {
            window.fixtureDropped = (window.fixtureDropped || 0) + 1;
            return;
          }
          if (window.fixtureDesktopSockets.has(this))
            window.fixtureMaxDesktopBuffered = Math.max(window.fixtureMaxDesktopBuffered, this.bufferedAmount);
          super.send(data);
          if (window.fixtureDesktopSockets.has(this))
            window.fixtureMaxDesktopBuffered = Math.max(window.fixtureMaxDesktopBuffered, this.bufferedAmount);
        }
        constructor(...args) {
          super(...args);
          if (new URL(args[0], location.href).pathname === '/api/channels/browser') {
            window.fixtureDesktopSockets.add(this);
            this.addEventListener('close', () => window.fixtureDesktopSockets.delete(this));
          }
        }
      };
    });
    const schema = await protobuf.load(path.resolve(__dirname, '../../protocol/schemas/session.proto'));
    const envelopeType = schema.lookupType('ultraterimal.wire.Envelope');
    let sentInputs = 0;
    const recentInputs = [];
    const browserTestKeys = [];
    const receivedTestProbes = [];
    const resizeResults = [];
    const videoConfigurations = [];
    const displayFeedback = [];
    let keyFrames = 0;
    const probes = [], probeReplies = new Set();
    const probeKey = message => `${message.sessionId}/${message.channelId}/${message.desktopProbe.nonce}`;
    const desktopSockets = new Set();
    page.on('websocket', socket => {
      if (socket.url().includes('/api/channels/browser')) {
        desktopSockets.add(socket);
        socket.on('close', () => desktopSockets.delete(socket));
      }
      socket.on('framesent', ({ payload }) => {
        if (!Buffer.isBuffer(payload)) return;
        const message = envelopeType.decode(payload);
        if (message.inputEvent) {
          ++sentInputs;
          recentInputs.push(message.inputEvent);
          if (recentInputs.length > 16) recentInputs.shift();
          if (message.inputEvent.kind === 3 && message.inputEvent.code === 0x87)
            browserTestKeys.push({ sequence: String(message.sequence), pressed: !!message.inputEvent.pressed, at_ms: performance.now() });
        }
        if (message.qualityFeedback) displayFeedback.push(message.qualityFeedback);
        if (message.desktopProbe) probeReplies.add(probeKey(message));
      });
      socket.on('framereceived', ({ payload }) => {
        if (!Buffer.isBuffer(payload)) return;
        const message = envelopeType.decode(payload);
        if (message.desktopResizeResult) resizeResults.push(message.desktopResizeResult);
        if (message.videoConfig) videoConfigurations.push(message);
        if (message.videoFrame?.keyFrame) ++keyFrames;
        if (message.desktopProbe) {
          probes.push(probeKey(message));
          receivedTestProbes.push({ nonce: String(message.desktopProbe.nonce), reset_input: !!message.desktopProbe.resetInput,
            accepted_response_nonce: String(message.desktopProbe.acceptedResponseNonce || 0), at_ms: performance.now() });
        }
      });
    });
    const devtools = await page.context().newCDPSession(page);
    const f24 = pressed => devtools.send('Input.dispatchKeyEvent', { type: pressed ? 'rawKeyDown' : 'keyUp', key: 'F24', code: 'F24', windowsVirtualKeyCode: 0x87, nativeVirtualKeyCode: 0x87 });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(settings.origin);
    await page.getByRole('heading', { name: '登录工作台' }).waitFor().catch(async error => { console.error(JSON.stringify({ errors, body: await page.locator('body').innerText() })); throw error; });
    await fs.mkdir(settings.screenshots, { recursive: true });
    await page.screenshot({ path: path.join(settings.screenshots, 'login.png') });
    await page.getByLabel('用户名').fill(settings.username);
    await page.getByLabel('密码').fill(settings.password);
    await page.getByRole('button', { name: /^登\s*录$/ }).click();
    await page.getByRole('heading', { name: '我的设备' }).waitFor();
    if (await page.getByRole('menuitem', { name: /异常页面|仪表盘|系统管理/ }).count()) throw new Error('Template demo menus remain');
    const credentialResponse = action => page.waitForResponse(response => response.url().endsWith('/api/devices/windows-login') && response.request().postDataJSON().action === action, { timeout: 15000 });
    let reading = credentialResponse('read');
    await page.getByRole('button', { name: '凭据', exact: true }).click();
    if ((await reading).status() !== 200) throw new Error('Windows credential state unavailable');
    await page.getByLabel('Windows 用户名', { exact: true }).fill('browser-test-account');
    await page.getByLabel('Windows 密码', { exact: true }).fill('test-only-browser-password');
    const saving = credentialResponse('save');
    await page.getByRole('button', { name: /^保\s*存$/ }).click();
    const saved = await saving;
    const savedState = await saved.json();
    if (saved.status() !== 200 || !savedState.saved || Object.hasOwn(savedState, 'password')) throw new Error('Windows credential save/response failed');
    await page.getByText('凭据已加密保存到这台 Agent。', { exact: true }).waitFor();
    if (await page.getByLabel('Windows 密码', { exact: true }).inputValue()) throw new Error('Password remained in form after saving');
    await page.screenshot({ path: path.join(settings.screenshots, 'windows-login.png') });
    await page.keyboard.press('Escape');
    await page.getByRole('dialog').waitFor({ state: 'detached' });
    reading = credentialResponse('read');
    await page.getByRole('button', { name: '凭据', exact: true }).click();
    const readBack = await reading;
    if (readBack.status() !== 200 || (await readBack.json()).username !== 'browser-test-account') throw new Error('Saved account could not be read');
    await page.getByPlaceholder('留空保持现有用户名', { exact: true }).waitFor();
    if (await page.getByLabel('Windows 用户名', { exact: true }).inputValue()) throw new Error('Saved account unexpectedly prefilled the partial-update field');
    const deviceId = saved.request().postDataJSON().device_id;
    const cleared = await page.evaluate(async device_id => {
      const response = await fetch('/api/devices/windows-login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ device_id, action: 'clear' }) });
      return response.status;
    }, deviceId);
    if (cleared !== 200) throw new Error('Windows credential fixture cleanup failed');
    await page.keyboard.press('Escape');
    await page.getByRole('dialog').waitFor({ state: 'detached' });
    if (network) {
      // Record control delivery independently of the later pixel assertions.
      // Black-frame failures remain failures; they must not erase evidence of
      // whether shaped encrypted bytes reached the real Windows input adapter.
      await page.getByRole('button', { name: /^连接.+（(?:在线|离线)）$/ }).click();
      const control = page.locator('canvas[aria-label="桌面画面"][data-control="ready"]');
      await control.waitFor({ timeout: 15000 }).catch(async error => {
        const native = spawnSync(path.join(path.dirname(settings.inputObserver), 'agent-capture-tests.exe'), ['--diagnose'],
          { windowsHide: true, timeout: 6000, encoding: 'utf8' });
        throw new Error(`${error.message}: status=${JSON.stringify(await page.getByRole('status').allTextContents())} video_configs=${videoConfigurations.length} sockets=${desktopSockets.size} native=${JSON.stringify(native.stdout?.trim())} errors=${JSON.stringify(errors)}`);
      });
      await control.focus();
      const sockets = [...desktopSockets];
      const samples = [];
      const before = network.metrics();
      browserTestKeys.length = 0;
      nativeTestKeys.length = 0;
      receivedTestProbes.length = 0;
      await page.evaluate(() => { window.fixtureMaxDesktopBuffered = 0; });
      network.shape(4096, 80);
      try {
        for (let sample = 0; sample < 12; sample++) {
          const started = process.hrtime.bigint();
          await f24(true); await f24(false);
          await until(() => inputEvents.filter(event => !event.mouse && event.code === 0x87).length >= (sample + 1) * 2,
            'Shaped control input did not reach Windows', 8000).catch(async error => {
              const state = await page.getByRole('status').allTextContents();
              const native = spawnSync(path.join(path.dirname(settings.inputObserver), 'agent-capture-tests.exe'), ['--diagnose'],
                { windowsHide: true, timeout: 6000, encoding: 'utf8' });
              throw new Error(`${error.message}: sample=${sample} browser_keys=${JSON.stringify(browserTestKeys)} native_keys=${JSON.stringify(nativeTestKeys)} received_probes=${JSON.stringify(receivedTestProbes.slice(-16))} ack=${probeReplies.size} sockets=${desktopSockets.size} status=${JSON.stringify(state)} metrics=${JSON.stringify(network.metrics())} capture=${JSON.stringify(native.stdout?.trim())} errors=${JSON.stringify(errors)}`);
            });
          samples.push(Number(process.hrtime.bigint() - started) / 1e6);
        }
        const keys = inputEvents.filter(event => !event.mouse && event.code === 0x87);
        const maxBrowserBuffered = await page.evaluate(() => window.fixtureMaxDesktopBuffered);
        if (browserTestKeys.length !== 24 || browserTestKeys.some((event, i) => event.pressed !== (i % 2 === 0)) ||
            keys.length !== 24 || keys.some((event, i) => event.message !== (i % 2 ? 0x101 : 0x100)))
          throw new Error(`Shaped control lost or reordered key transitions: browser_keys=${JSON.stringify(browserTestKeys)} native_keys=${JSON.stringify(nativeTestKeys)} received_probes=${JSON.stringify(receivedTestProbes.slice(-16))} ack=${probeReplies.size} sockets=${desktopSockets.size} max_buffered=${maxBrowserBuffered} metrics=${JSON.stringify(network.metrics())}`);
        if (sockets.length !== 2 || desktopSockets.size !== 2 || sockets.some(socket => !desktopSockets.has(socket)))
          throw new Error('Shaped control replaced a desktop channel');
        const after = network.metrics();
        if (after.shapedBytes <= before.shapedBytes) throw new Error('Browser bypassed shaping');
        const sorted = [...samples].sort((a, b) => a - b);
        const report = { scope: 'real encrypted WSS uplink to Windows input observer, not display latency or packet loss',
          uplink_bytes_per_second: 4096, per_chunk_delay_ms: 80, samples_ms: samples,
          median_ms: (sorted[5] + sorted[6]) / 2, p95_ms: sorted[11], max_ms: sorted[11],
          shaped_bytes: after.shapedBytes - before.shapedBytes, browser_max_buffered_bytes: maxBrowserBuffered,
          browser_key_events: browserTestKeys.length, native_key_events: keys.length,
          reset_input_probes: receivedTestProbes.filter(probe => probe.reset_input).length, reconnects: 0 };
        await fs.writeFile(path.join(settings.screenshots, 'shaped-control.json'), JSON.stringify(report, null, 2));
        console.log(`Shaped real WSS control: ${report.shaped_bytes} encrypted bytes, median=${report.median_ms.toFixed(1)}ms p95=${report.p95_ms.toFixed(1)}ms, 24 ordered transitions, no reconnects`);
      } finally { network.shape(0, 0); }
      await control.focus();
      const beforeClose = inputEvents.filter(event => !event.mouse && event.code === 0x87).length;
      await f24(true);
      await until(() => inputEvents.filter(event => !event.mouse && event.code === 0x87).length === beforeClose + 1,
        'Held key before shaped-session close did not reach Windows', 8000);
      // Programmatic click does not steal focus; the browser must cancel the
      // writer before disposing held input, while Agent closes and releases it.
      const closeStarted = process.hrtime.bigint();
      await page.getByRole('button', { name: '断开连接' }).evaluate(button => button.click());
      await page.getByRole('region', { name: '远程桌面' }).waitFor({ state: 'detached' });
      await until(() => desktopSockets.size === 0, 'Shaped control did not close channels');
      await until(() => inputEvents.filter(event => !event.mouse && event.code === 0x87).length >= beforeClose + 2,
        'Shaped-session close retained the held Windows key', 8000);
      const closeKeys = inputEvents.filter(event => !event.mouse && event.code === 0x87);
      if (closeKeys.length !== beforeClose + 2 || closeKeys.at(-1).message !== 0x101)
        throw new Error(`Shaped-session close did not release exactly its held key: ${JSON.stringify(closeKeys.map(event => event.message))}`);
      console.log(`Held key released after desktop close in ${(Number(process.hrtime.bigint() - closeStarted) / 1e6).toFixed(1)}ms`);
      await f24(false);
      inputEvents.length = 0;
      // The network-control regression must not depend on the local graphics
      // driver's intermittently black DXGI output. Opt in to the longer
      // display-latency run only on a host with verified compositor output.
      if (process.env.ULTRATERIMAL_DESKTOP_SHAPED_PIXEL !== '1') return;
    }
    await page.route('**/api/sessions', async route => {
      if (route.request().postDataJSON().write_control) await route.fulfill({ status: 403, json: {} });
      else await route.continue();
    });
    await page.getByRole('button', { name: /^连接.+（(?:在线|离线)）$/ }).click();
    await page.waitForFunction(() => {
      const canvas = document.querySelector('canvas[aria-label="桌面画面"]');
      return canvas && Number(canvas.dataset.frame) >= 1;
    }, null, { timeout: 15000 }).catch(async () => {
      throw new Error(`Desktop did not display: ${await page.getByRole('status').allTextContents()}; browser errors: ${errors.join('; ')}`);
    });
    const image = await page.locator('canvas[aria-label="桌面画面"]').evaluate(canvas => {
      const bytes = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
      // Check coverage separately from the native color-pattern assertion.
      let opaque = true;
      for (let i = 3; i < bytes.length; i += 4) if (bytes[i] !== 255) { opaque = false; break; }
      return { width: canvas.width, height: canvas.height, frame: Number(canvas.dataset.frame), opaque };
    });
    if (image.width <= 32 || image.height <= 32 || !image.opaque) throw new Error('Browser did not draw a complete decoded desktop frame');
    const beforeSwitch = await page.locator('canvas[aria-label="桌面画面"]').getAttribute('data-generation');
    const originalSockets = [...desktopSockets];
    if (await page.getByRole('combobox', { name: '画面质量' }).count()) throw new Error('Removed quality selector is still visible');
    await until(() => displayFeedback.at(-1)?.preference === 3, 'Default sharp feedback did not reach the Agent');
    const checkPausedProbes = async (start, label) => {
      const received = probes.slice(start);
      if (received.length < 3) throw new Error(`${label} did not receive periodic desktop probes`);
      await until(() => received.every(key => probeReplies.has(key)), `${label} did not confirm every desktop probe`);
      if (desktopSockets.size !== 2 || originalSockets.some(socket => !desktopSockets.has(socket)))
        throw new Error(`${label} replaced the original desktop channels`);
      console.log(`${label}: confirmed ${received.length} probes while video remained paused on the original channels`);
    };
    await page.getByRole('button', { name: '我的设备', exact: true }).click();
    // Let already-sent frames drain, then verify that the real Agent pauses.
    await new Promise(resolve => setTimeout(resolve, 500));
    const workspaceFrame = await page.locator('canvas[aria-label="桌面画面"]').getAttribute('data-frame');
    const workspaceProbeStart = probes.length;
    for (let attempt = 0; attempt < 3; ++attempt) {
      await new Promise(resolve => setTimeout(resolve, 3000));
      if (await page.locator('canvas[aria-label="桌面画面"]').getAttribute('data-frame') !== workspaceFrame)
        throw new Error('Hidden workspace continued video encoding');
    }
    await checkPausedProbes(workspaceProbeStart, 'Hidden workspace');
    await page.getByRole('button', { name: '返回远控', exact: true }).click();
    await page.waitForFunction(previous => Number(document.querySelector('canvas[aria-label="桌面画面"]')?.dataset.frame) > Number(previous), workspaceFrame);
    const resumedGeneration = Number(await page.locator('canvas[aria-label="桌面画面"]').getAttribute('data-generation'));
    if (desktopSockets.size !== 2 || originalSockets.some(socket => !desktopSockets.has(socket)) || !Number.isSafeInteger(resumedGeneration) || resumedGeneration < Number(beforeSwitch))
      throw new Error('Workspace switch replaced desktop channels or regressed the picture baseline');
    const layout = await page.locator('canvas[aria-label="桌面画面"]').evaluate(canvas => {
      const box = canvas.getBoundingClientRect();
      return { ratioError: Math.abs(box.width / box.height - canvas.width / canvas.height), overflow: document.documentElement.scrollHeight > innerHeight || document.documentElement.scrollWidth > innerWidth };
    });
    if (layout.ratioError > 0.01 || layout.overflow) throw new Error(`Desktop layout invalid: ${JSON.stringify(layout)}`);
    await verifyCapturePattern(page);
    await page.waitForFunction(previous => Number(document.querySelector('canvas[aria-label="桌面画面"]')?.dataset.frame) > previous, image.frame);
    // Controlled visibility event, real browser/Agent feedback and encoding.
    // Hidden sessions must pause video until visibility resumes.
    await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', { configurable: true, get: () => true });
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await new Promise(resolve => setTimeout(resolve, 500));
    const backgroundFrame = await page.locator('canvas[aria-label="桌面画面"]').getAttribute('data-frame');
    const backgroundProbeStart = probes.length;
    for (let attempt = 0; attempt < 3; ++attempt) {
      await new Promise(resolve => setTimeout(resolve, 3000));
      if (await page.locator('canvas[aria-label="桌面画面"]').getAttribute('data-frame') !== backgroundFrame)
        throw new Error('Background desktop continued encoding despite visibility feedback');
    }
    await checkPausedProbes(backgroundProbeStart, 'Controlled document visibility');
    await page.evaluate(() => {
      delete document.hidden;
      document.dispatchEvent(new Event('visibilitychange'));
    });
    // Visibility feedback resumes video without reconnecting.
    await page.waitForFunction(previous => Number(document.querySelector('canvas[aria-label="桌面画面"]')?.dataset.frame) > Number(previous), backgroundFrame);
    await page.locator('canvas[aria-label="桌面画面"]').dispatchEvent('keydown', { code: 'F24', key: 'F24' });
    if (inputEvents.length) throw new Error('Read-only desktop injected input');
    await page.getByRole('button', { name: '断开连接' }).click();
    await page.getByRole('region', { name: '远程桌面' }).waitFor({ state: 'detached' });
    await page.unroute('**/api/sessions');
    await page.getByRole('button', { name: /^连接.+（(?:在线|离线)）$/ }).click();
    const canvas = page.locator('canvas[aria-label="桌面画面"][data-control="ready"]');
    await canvas.waitFor({ timeout: 10000 });
    const originalConfiguration = videoConfigurations.at(-1);
    const configurationCount = videoConfigurations.length;
    if (!originalConfiguration?.videoConfig.displayModes.length) throw new Error('Missing driver display modes');
    const currentSize = await canvas.evaluate(element => ({ width: element.width, height: element.height }));
    await page.getByRole('button', { name: '远程分辨率', exact: true }).click();
    await page.getByRole('menuitem', { name: `${currentSize.width} × ${currentSize.height}`, exact: true }).click();
    await until(() => resizeResults.length === 1, 'Browser resolution request received no result');
    if (!resizeResults[0].applied) throw new Error('Windows rejected its current resolution');
    await until(() => videoConfigurations.length > configurationCount, 'Resize did not rebuild capture');
    if (Number(videoConfigurations.at(-1).generation) <= Number(originalConfiguration.generation)) throw new Error('Resize reused the old video generation');
    await page.waitForFunction(generation => document.querySelector('canvas[aria-label="桌面画面"]')?.dataset.generation === generation,
      String(videoConfigurations.at(-1).generation));
    await canvas.waitFor();
    await verifyCapturePattern(page);
    await page.screenshot({ path: path.join(settings.screenshots, 'resized-desktop.png') });
    // This includes automation dispatch and pixel observation overhead. It is
    // a local end-to-end upper bound, not a network RTT or remote CDN result.
    await canvas.focus();
    const inputToDisplay = [];
    for (let sample = 0; sample < 12; ++sample) {
      const started = process.hrtime.bigint();
      await devtools.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: 'F23', code: 'F23', windowsVirtualKeyCode: 0x86, nativeVirtualKeyCode: 0x86 });
      await devtools.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'F23', code: 'F23', windowsVirtualKeyCode: 0x86, nativeVirtualKeyCode: 0x86 });
      await verifyCapturePattern(page, sample % 2 === 0).catch(async error => {
        const probe = spawn(settings.inputObserver, ['--probe'], { windowsHide: true, stdio: 'ignore' });
        const probeExit = await new Promise(resolve => probe.once('exit', resolve));
        await new Promise(resolve => setTimeout(resolve, 100));
        throw new Error(`${error.message}: sample=${sample}, observerProbeExit=${probeExit}, recentInputs=${JSON.stringify(recentInputs)}, nativeEvents=${JSON.stringify(inputEvents.slice(-16))}`);
      });
      inputToDisplay.push(Number(process.hrtime.bigint() - started) / 1e6);
    }
    await until(() => inputEvents.filter(event => !event.mouse && event.code === 0x86).length === 24, 'Latency probe did not release every key');
    inputEvents.length = 0;
    const sortedLatency = [...inputToDisplay].sort((left, right) => left - right);
    const latencyReport = {
      scope: 'local browser dispatch to decoded Windows pixels, including automation overhead',
      width: currentSize.width, height: currentSize.height,
      samples_ms: inputToDisplay,
      median_ms: (sortedLatency[Math.floor((sortedLatency.length - 1) / 2)] + sortedLatency[Math.floor(sortedLatency.length / 2)]) / 2,
      p95_ms: sortedLatency[Math.ceil(sortedLatency.length * 0.95) - 1],
      max_ms: sortedLatency.at(-1),
    };
    await fs.writeFile(path.join(settings.screenshots, 'input-to-display.json'), JSON.stringify(latencyReport, null, 2));
    console.log(`Local input-to-display upper bound: median=${latencyReport.median_ms.toFixed(1)}ms p95=${latencyReport.p95_ms.toFixed(1)}ms`);
    if (network) {
      const originalSockets = [...desktopSockets];
      const samples = [];
      const before = network.metrics();
      network.shape(4096, 80);
      try {
        for (let sample = 0; sample < 12; sample++) {
          const started = process.hrtime.bigint();
          await devtools.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: 'F23', code: 'F23', windowsVirtualKeyCode: 0x86, nativeVirtualKeyCode: 0x86 });
          await devtools.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'F23', code: 'F23', windowsVirtualKeyCode: 0x86, nativeVirtualKeyCode: 0x86 });
          await verifyCapturePattern(page, sample % 2 === 0);
          samples.push(Number(process.hrtime.bigint() - started) / 1e6);
        }
        await until(() => inputEvents.filter(event => !event.mouse && event.code === 0x86).length === 24,
          'Shaped uplink lost a key transition', 8000);
        const transitions = inputEvents.filter(event => !event.mouse && event.code === 0x86);
        if (transitions.some((event, index) => event.message !== (index % 2 ? 0x101 : 0x100)))
          throw new Error('Shaped uplink reordered Windows key transitions');
        if (desktopSockets.size !== 2 || originalSockets.some(socket => !desktopSockets.has(socket)))
          throw new Error('Shaped uplink recreated a desktop channel');
        const after = network.metrics();
        if (after.shapedBytes <= before.shapedBytes || after.delayedMilliseconds <= before.delayedMilliseconds)
          throw new Error('Browser bypassed the TCP shaping proxy');
        const sorted = [...samples].sort((a, b) => a - b);
        const report = {
          scope: 'real TLS/WSS TCP uplink shaping, local gateway and Agent; not packet loss or CDN',
          uplink_bytes_per_second: 4096, per_chunk_delay_ms: 80,
          shaped_bytes: after.shapedBytes - before.shapedBytes,
          scheduled_delay_ms: after.delayedMilliseconds - before.delayedMilliseconds,
          samples_ms: samples, median_ms: (sorted[5] + sorted[6]) / 2,
          p95_ms: sorted[Math.ceil(sorted.length * 0.95) - 1], max_ms: sorted.at(-1),
          key_transitions: transitions.length, reconnects: 0,
        };
        await fs.writeFile(path.join(settings.screenshots, 'shaped-uplink.json'), JSON.stringify(report, null, 2));
        console.log(`Shaped real WSS uplink: ${report.shaped_bytes} encrypted bytes, median=${report.median_ms.toFixed(1)}ms p95=${report.p95_ms.toFixed(1)}ms, no reconnects`);
      } finally { network.shape(0, 0); inputEvents.length = 0; }
      // The full desktop lifecycle is covered by the normal fixture. Finish
      // this dedicated weak-uplink run without consuming its 30 reconnects.
      return;
    }
    // Normal probes must not interrupt a deliberate long key press or drag.
    const heldProbeStart = probes.length;
    await f24(true);
    await until(() => inputEvents.some(event => !event.mouse && event.code === 0x87 && event.message === 0x100), 'Held key did not reach Windows');
    await new Promise(resolve => setTimeout(resolve, 5000));
    if (probes.length - heldProbeStart < 2 || inputEvents.some(event => !event.mouse && event.code === 0x87 && event.message === 0x101))
      throw new Error('A healthy desktop probe released a held key');
    await f24(false);
    await until(() => inputEvents.some(event => !event.mouse && event.code === 0x87 && event.message === 0x101), 'Long key press was not released');
    inputEvents.length = 0;
    const toggleAnimation = async () => {
      await devtools.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key: 'F22', code: 'F22', windowsVirtualKeyCode: 0x85, nativeVirtualKeyCode: 0x85 });
      await devtools.send('Input.dispatchKeyEvent', { type: 'keyUp', key: 'F22', code: 'F22', windowsVirtualKeyCode: 0x85, nativeVirtualKeyCode: 0x85 });
    };
    displayFeedback.length = 0;
    await toggleAnimation();
    let motion;
    try {
      motion = await canvas.evaluate((element, duration) => new Promise(resolve => {
        const started = performance.now();
        let previousFrame = element.dataset.frame;
        let previousTime = started;
        let previousColor = '';
        let colorChanges = 0;
        const intervals = [];
        const tick = now => {
          if (element.dataset.frame !== previousFrame) {
            previousFrame = element.dataset.frame;
            intervals.push(now - previousTime);
            previousTime = now;
            const color = Array.from(element.getContext('2d').getImageData(64, 96, 1, 1).data).join(',');
            if (previousColor && color !== previousColor) ++colorChanges;
            previousColor = color;
          }
          if (now - started < duration) requestAnimationFrame(tick);
          else resolve({ duration_ms: now - started, frames: intervals.length,
            displayed_fps: intervals.length * 1000 / (now - started),
            pixel_changes_per_second: colorChanges * 1000 / (now - started),
            max_gap_ms: intervals.reduce((maximum, interval) => Math.max(maximum, interval), now - previousTime), color_changes: colorChanges, intervals_ms: intervals });
        };
        requestAnimationFrame(tick);
      }), motionSeconds * 1000);
    } finally {
      await toggleAnimation();
    }
    await verifyCapturePattern(page);
    await until(() => inputEvents.filter(event => !event.mouse && event.code === 0x85).length === 4, 'Motion probe did not release every key');
    inputEvents.length = 0;
    if (motion.frames < 2 || motion.color_changes < 2) throw new Error('Continuous native motion did not reach the browser');
    const decodeTimes = displayFeedback.map(feedback => Number(feedback.decodeMicroseconds) / 1000).sort((left, right) => left - right);
    if (!decodeTimes.length) throw new Error('Continuous motion produced no display feedback');
    const decoding = {
      feedback_samples: decodeTimes.length,
      decode_p50_ms: (decodeTimes[Math.floor((decodeTimes.length - 1) / 2)] + decodeTimes[Math.floor(decodeTimes.length / 2)]) / 2,
      decode_p95_ms: decodeTimes[Math.ceil(decodeTimes.length * .95) - 1],
      decode_max_ms: decodeTimes.at(-1),
      max_queue_depth: displayFeedback.reduce((maximum, feedback) => Math.max(maximum, Number(feedback.queueDepth)), 0),
    };
    await fs.writeFile(path.join(settings.screenshots, 'continuous-motion.json'), JSON.stringify({
      scope: `local ${motionSeconds}-second native small-window animation observed at browser animation callbacks; not full-screen motion or CDN`,
      width: currentSize.width, height: currentSize.height, ...motion, ...decoding }, null, 2));
    console.log(`Local continuous motion: ${motion.displayed_fps.toFixed(1)} displayed fps, ${motion.pixel_changes_per_second.toFixed(1)} pixel changes/s, max gap ${motion.max_gap_ms.toFixed(1)}ms`);
    const probe = spawn(settings.inputObserver, ['--probe'], { windowsHide: true, stdio: 'ignore' });
    await new Promise((resolve, reject) => {
      probe.once('error', reject);
      probe.once('exit', code => code === 0 ? resolve() : reject(new Error('Native input probe failed')));
    });
    await until(() => inputEvents.length >= 2, 'Native observer cannot see cross-process Windows input').catch(error => {
      throw new Error(`${error.message}: events=${JSON.stringify(inputEvents)}, observerExit=${observer.exitCode}`);
    });
    inputEvents.length = 0;
    await canvas.focus();
    await f24(true);
    await f24(false);
    await until(() => inputEvents.filter(e => !e.mouse).length >= 2, 'Browser key press did not reach Windows').catch(async error => {
      const focus = await canvas.evaluate(element => ({ focused: document.activeElement === element, hidden: document.hidden, frame: element.dataset.frame }));
      throw new Error(`${error.message}: sentInputs=${sentInputs}, observerExit=${observer.exitCode}, focus=${JSON.stringify(focus)}, status=${await page.getByRole('status').allTextContents()}`);
    });
    const keys = () => inputEvents.filter(e => !e.mouse);
    if (keys()[0].code !== 0x87 || keys()[0].message !== 0x100 || keys()[1].message !== 0x101) throw new Error('F24 order or mapping differs');
    await f24(true);
    await until(() => keys().length >= 3, 'Held key was not injected');
    await page.getByRole('button', { name: /^全\s*屏$/ }).focus();
    await until(() => keys().length >= 4, 'Blur did not release the held key');
    if (keys()[3].message !== 0x101) throw new Error('Blur produced an incorrect key transition');
    await f24(false);
    await canvas.focus();
    await page.keyboard.press('ControlRight');
    await until(() => keys().length >= 6, 'Right modifier did not reach Windows');
    if (keys()[4].code !== 0xa3 || !(keys()[4].flags & 1) || keys()[5].message !== 0x101) throw new Error('Right modifier or extended flag differs');
    const box = await canvas.boundingBox();
    if (!box) throw new Error('Missing desktop layout');
    await page.mouse.move(box.x + box.width * .25, box.y + Math.min(box.height * .25, 100));
    const moves = () => inputEvents.filter(e => e.mouse && e.message === 0x200);
    await until(() => moves().length >= 1, 'Mouse movement did not reach Windows');
    const firstMove = moves().at(-1);
    const moveCount = moves().length;
    await page.mouse.move(box.x + box.width * .375, box.y + Math.min(box.height * .25, 100) + 20);
    await until(() => moves().length > moveCount, 'Second mouse movement was not delivered');
    const secondMove = moves().at(-1);
    if (Math.abs(secondMove.x - firstMove.x - image.width / 8) > 2 || Math.abs(secondMove.y - firstMove.y - 20 * image.height / box.height) > 2) throw new Error('Scaled canvas coordinates do not match physical desktop pixels');
    await f24(true);
    await until(() => keys().length >= 7, 'Held key before drag was not injected');
    await page.mouse.down();
    await until(() => inputEvents.some(e => e.mouse && e.message === 0x201), 'Mouse down did not reach Windows').catch(async error => {
      const focus = await canvas.evaluate(element => ({ focused: document.activeElement === element, hidden: document.hidden, frame: element.dataset.frame }));
      throw new Error(`${error.message}: recentInputs=${JSON.stringify(recentInputs)}, nativeEvents=${JSON.stringify(inputEvents.slice(-16))}, focus=${JSON.stringify(focus)}, status=${await page.getByRole('status').allTextContents()}`);
    });
    await page.mouse.up();
    await until(() => inputEvents.some(e => e.mouse && e.message === 0x202), 'Mouse up did not reach Windows');
    await page.mouse.down();
    await until(() => inputEvents.filter(e => e.mouse && e.message === 0x201).length >= 2, 'Second drag did not start');
    if (keys().length !== 7) throw new Error('Normal mouse capture release also released a held keyboard key');
    await page.getByRole('button', { name: /^全\s*屏$/ }).focus();
    await until(() => inputEvents.filter(e => e.mouse && e.message === 0x202).length >= 2 && keys().length >= 8, 'Blur did not release both key and mouse button');
    if (keys()[7].message !== 0x101) throw new Error('Blur did not release the key held during drag');
    await f24(false);
    await page.mouse.up();
    await canvas.focus();
    await page.mouse.wheel(120, -240);
    await until(() => inputEvents.some(e => e.mouse && e.message === 0x20a) && inputEvents.some(e => e.mouse && e.message === 0x20e), 'Wheel axes did not reach Windows');
    const wheel = inputEvents.find(e => e.mouse && e.message === 0x20a);
    const horizontal = inputEvents.find(e => e.mouse && e.message === 0x20e);
    if ((wheel.code >> 16) !== 240 || (horizontal.code >> 16) !== 120) throw new Error('Wheel direction or scale differs');
    await canvas.focus();
    await f24(true);
    await until(() => keys().length >= 9, 'Final held key did not reach Windows');
    // Remove the focused canvas through the actual disconnect action; cleanup must release ownership.
    await page.getByRole('button', { name: '断开连接' }).evaluate(button => button.click());
    await until(() => keys().length >= 10, 'Disconnect did not release the held key');
    if (keys()[9].message !== 0x101) throw new Error('Disconnect produced an incorrect key transition');
    await f24(false);
    await page.getByRole('region', { name: '远程桌面' }).waitFor({ state: 'detached' });
    await page.unroute('**/api/sessions');
    if (observer.exitCode !== null) throw new Error('Input observer exited prematurely');
    const reconnectCycles = [];
    await until(() => desktopSockets.size === 0, 'Disconnected desktop retained browser sockets');
    for (let cycle = 0; cycle < 30; ++cycle) {
      if (observer.exitCode !== null) throw new Error(`Input observer exited before cycle ${cycle + 1}`);
      const started = process.hrtime.bigint();
      const creation = page.waitForResponse(response => response.url().endsWith('/api/sessions') && response.request().method() === 'POST');
      await page.getByRole('button', { name: /^连接.+（(?:在线|离线)）$/ }).click();
      const created = await creation;
      if (created.status() !== 202) throw new Error(`Cycle ${cycle + 1} session creation returned ${created.status()}: ${await created.text()}`);
      await page.locator('canvas[aria-label="桌面画面"][data-control="ready"]').waitFor({ timeout: 10000 }).catch(async error => {
        throw new Error(`Cycle ${cycle + 1} did not become ready: ${error.message}; status=${JSON.stringify(await page.getByRole('status').allTextContents())}; sockets=${desktopSockets.size}; errors=${JSON.stringify(errors)}`);
      });
      await verifyCapturePattern(page);
      if (desktopSockets.size !== 2) throw new Error('Desktop cycle did not own exactly two browser channels');
      reconnectCycles.push({ cycle: cycle + 1, first_frame_ms: Number(process.hrtime.bigint() - started) / 1e6 });
      inputEvents.length = 0;
      await canvas.focus();
      await f24(true);
      await until(() => keys().length === 1, `Cycle ${cycle + 1} did not inject the held key`);
      if (keys()[0].code !== 0x87 || keys()[0].message !== 0x100) throw new Error('Cycle injected an incorrect key');
      // Keep focus on the canvas so disconnect, rather than blur, releases input.
      await page.getByRole('button', { name: '断开连接' }).evaluate(button => button.click());
      await page.getByRole('region', { name: '远程桌面' }).waitFor({ state: 'detached' });
      await until(() => desktopSockets.size === 0, 'Desktop cycle leaked browser channels');
      await until(() => keys().length >= 2, `Cycle ${cycle + 1} retained the held key after disconnect`);
      if (keys().length !== 2 || keys()[1].code !== 0x87 || keys()[1].message !== 0x101) throw new Error('Cycle did not release exactly its held key');
      await f24(false);
      // This is a lifecycle test, not a request-flood test. Keep the production
      // rate limiter enabled while spreading the repeated session operations.
      const elapsed = Number(process.hrtime.bigint() - started) / 1e6;
      if (elapsed < 3000) await new Promise(resolve => setTimeout(resolve, 3000 - elapsed));
    }
    await page.getByRole('button', { name: /^连接.+（(?:在线|离线)）$/ }).click();
    await canvas.waitFor({ timeout: 10000 });
    await verifyCapturePattern(page);
    inputEvents.length = 0;
    await canvas.focus();
    await f24(true);
    await until(() => keys().length === 1, 'Key before channel failure did not reach Windows');
    const recoveredSession = page.waitForResponse(response => response.url().endsWith('/api/sessions') && response.request().method() === 'POST');
    const interruptionStarted = process.hrtime.bigint();
    await page.evaluate(() => {
      if (window.fixtureDesktopSockets.size !== 2) throw new Error('Expected two live desktop transports');
      for (const socket of window.fixtureDesktopSockets) socket.close(4000, 'fixture transport interruption');
    });
    await until(() => keys().length >= 2, 'Channel failure left a Windows key held');
    if (keys().length !== 2 || keys()[0].code !== 0x87 || keys()[0].message !== 0x100 || keys()[1].code !== 0x87 || keys()[1].message !== 0x101)
      throw new Error('Channel failure did not release exactly its held key');
    if ((await recoveredSession).status() !== 202) throw new Error('Automatic recovery could not obtain a new session');
    await canvas.waitFor({ timeout: 10000 });
    await verifyCapturePattern(page);
    if (desktopSockets.size !== 2) throw new Error('Recovered desktop does not own exactly two channels');
    if (keys().length !== 2) throw new Error('Recovery replayed a stale Windows key');
    const recoveryMs = Number(process.hrtime.bigint() - interruptionStarted) / 1e6;
    await f24(false);
    inputEvents.length = 0;
    await canvas.focus();
    await f24(true);
    await f24(false);
    await until(() => keys().length >= 2, 'Recovered desktop cannot inject fresh input');
    if (keys().length !== 2 || keys()[0].message !== 0x100 || keys()[1].message !== 0x101)
      throw new Error('Recovered desktop injected incorrect key transitions');
    inputEvents.length = 0;
    await f24(true);
    await until(() => keys().length === 1, 'Key before silent failure did not reach Windows');
    const originalLossSockets = [...desktopSockets];
    let lossSessionRequests = 0;
    const countLossRequests = request => {
      if (request.url().endsWith('/api/sessions') && request.method() === 'POST') ++lossSessionRequests;
    };
    page.on('request', countLossRequests);
    await page.evaluate(() => { window.fixtureDropDesktop = true; window.fixtureDropped = 0; });
    const silentStarted = process.hrtime.bigint();
    let silentReleaseMs, droppedMessages;
    try {
      await f24(false);
      await until(() => keys().length >= 2, 'Lost key-up remained held without a transport-close notification', 8000);
      silentReleaseMs = Number(process.hrtime.bigint() - silentStarted) / 1e6;
      // Sustain application-send loss beyond the old 4-second disconnect threshold.
      await new Promise(resolve => setTimeout(resolve, 5000));
      droppedMessages = await page.evaluate(() => window.fixtureDropped);
      if (!droppedMessages || keys().length !== 2 || keys()[1].code !== 0x87 || keys()[1].message !== 0x101)
        throw new Error('Silent loss did not release exactly the lost key-up');
      if (lossSessionRequests || desktopSockets.size !== 2 || originalLossSockets.some(socket => !desktopSockets.has(socket)))
        throw new Error('Short application-send loss recreated the desktop session');
    } finally {
      await page.evaluate(() => { window.fixtureDropDesktop = false; });
    }
    const recoveryProbeStart = probes.length;
    await until(() => {
      const firstPostLossReply = probes.findIndex((probe, index) => index >= recoveryProbeStart && probeReplies.has(probe));
      const current = receivedTestProbes.at(-1);
      return firstPostLossReply >= 0 && probes.length > recoveryProbeStart && probeReplies.has(probes.at(-1)) &&
        current?.reset_input === false && BigInt(current.accepted_response_nonce) >= BigInt(receivedTestProbes[firstPostLossReply].nonce);
    }, 'Agent did not confirm a post-freeze probe reply after application-send loss', 12000);
    if (lossSessionRequests || desktopSockets.size !== 2 || originalLossSockets.some(socket => !desktopSockets.has(socket)))
      throw new Error('Short application-send loss replaced the original channels');
    await canvas.waitFor({ timeout: 10000 });
    await verifyCapturePattern(page);
    await canvas.focus();
    await f24(true);
    await f24(false);
    await until(() => keys().length >= 4, 'Recovered original desktop cannot inject fresh input', 6000);
    if (keys().length !== 4 || keys()[2].message !== 0x100 || keys()[3].message !== 0x101)
      throw new Error('Short-loss recovery replayed old input or corrupted fresh input');
    page.off('request', countLossRequests);
    console.log(`Silent application-send loss released Windows input in ${silentReleaseMs.toFixed(1)}ms without reconnecting; dropped ${droppedMessages} messages`);
    await page.getByRole('button', { name: '断开连接' }).evaluate(button => button.click());
    await page.getByRole('region', { name: '远程桌面' }).waitFor({ state: 'detached' });
    await until(() => desktopSockets.size === 0, 'Recovered desktop leaked browser channels');
    console.log(`Real Agent channel-close recovery released held input and accepted fresh input in ${recoveryMs.toFixed(1)}ms; not a packet-loss test`);
    await fs.writeFile(path.join(settings.screenshots, 'session-cycles.json'), JSON.stringify({
      scope: '30 local real-Agent desktop connect/display/held-key/disconnect cycles at >=3s cadence; includes browser automation overhead',
      cycles: reconnectCycles, remaining_browser_channels: desktopSockets.size,
      channel_close_recovery_ms: recoveryMs,
      silent_input_release_ms: silentReleaseMs,
    }, null, 2));
    await page.screenshot({ path: path.join(settings.screenshots, 'devices.png') });
    await page.setViewportSize({ width: 390, height: 844 });
    await page.screenshot({ path: path.join(settings.screenshots, 'devices-mobile.png') });
    await page.getByRole('button', { name: '账户菜单' }).click();
    await page.getByRole('menuitem', { name: '退出登录' }).click();
    await page.getByRole('heading', { name: '登录工作台' }).waitFor().catch(async error => { console.error(JSON.stringify({ errors, body: await page.locator('body').innerText() })); throw error; });
    if (errors.length) throw new Error(errors.join('; '));
    console.log(`Browser login, real ${image.width}x${image.height} desktop, refresh, native keyboard/mouse/wheel injection, blur/disconnect release and logout passed`);
  } finally {
    lines.close();
    if (observer.exitCode === null) {
      const exited = new Promise(resolve => observer.once('exit', resolve));
      observer.kill();
      await exited;
    }
    await browser.close();
    await network?.close();
  }
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
