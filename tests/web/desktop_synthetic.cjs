const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const path = require('node:path');
const { chromium } = require('playwright');
const protobuf = require('../../web/node_modules/protobufjs');
const { desktopNetwork } = require('./desktop_network.fixture.cjs');

async function until(check, label, timeout = 7000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const value = await check();
    if (value) return value;
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  throw new Error(label);
}

async function main() {
  let input = '';
  for await (const chunk of process.stdin) input += chunk;
  const settings = JSON.parse(input);
  const network = await desktopNetwork(settings.origin);
  let browser;
  try {
    browser = await chromium.launch({ executablePath: process.argv[2], headless: true,
      args: [`--ignore-certificate-errors-spki-list=${settings.pin}`,
        `--proxy-server=${network.address}`, '--proxy-bypass-list=<-loopback>'],
    });
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    page.setDefaultTimeout(12000);
    const errors = [], browserKeys = [], sentInputs = [], sockets = new Set();
    page.on('pageerror', error => errors.push(error.message));
    const schema = await protobuf.load(path.resolve(__dirname, '../../protocol/schemas/session.proto'));
    const envelope = schema.lookupType('ultraterimal.wire.Envelope');
    page.on('websocket', socket => {
      if (!/\/api\/desktop\/(control|video)$/.test(socket.url())) return;
      sockets.add(socket);
      socket.on('close', () => sockets.delete(socket));
      socket.on('framesent', ({ payload }) => {
        if (!Buffer.isBuffer(payload)) return;
        const packet = envelope.decode(payload);
        if (packet.inputEvent) {
          sentInputs.push({ sequence: Number(packet.sequence), kind: packet.inputEvent.kind,
            code: packet.inputEvent.code, pressed: !!packet.inputEvent.pressed });
          if (packet.inputEvent.kind === 3 && packet.inputEvent.code === 0x87)
            browserKeys.push({ sequence: Number(packet.sequence), pressed: !!packet.inputEvent.pressed });
        }
      });
    });
    await page.goto(settings.origin);
    await page.getByRole('heading', { name: '登录工作台' }).waitFor();
    await page.getByLabel('用户名').fill(settings.username);
    await page.getByLabel('密码').fill(settings.password);
    await page.getByRole('button', { name: /^登\s*录$/ }).click();
    await page.getByRole('heading', { name: '我的设备' }).waitFor();
    await page.getByRole('button', { name: /^连接.+（(?:在线|离线)）$/ }).click();
    const canvas = page.locator('canvas[aria-label="桌面画面"][data-control="ready"]');
    await canvas.waitFor({ timeout: 15000 }).catch(async error => {
      throw new Error(`${error.message}; browser=${errors.join('; ')}; statuses=${JSON.stringify(await page.getByRole('status').allTextContents())}`);
    });
    await canvas.focus();
    await page.evaluate(() => {
      window.fixtureKeyEvents = [];
      const record = event => fixtureKeyEvents.push({ type: event.type, code: event.code, focused: document.activeElement?.getAttribute('aria-label'), at: performance.now() });
      document.addEventListener('keydown', record, true);
      document.addEventListener('keyup', record, true);
      document.querySelector('canvas[aria-label="桌面画面"]').addEventListener('blur', record);
      window.addEventListener('blur', record);
      document.addEventListener('visibilitychange', record);
    });
    assert.equal(sockets.size, 2, 'Synthetic Agent did not pair two real browser WSS channels');
    const originalSockets = [...sockets];
    const before = network.metrics();
    network.shape(4096, 80);
    const devtools = await page.context().newCDPSession(page);
    const f24 = pressed => devtools.send('Input.dispatchKeyEvent', { type: pressed ? 'rawKeyDown' : 'keyUp',
      key: 'F24', code: 'F24', windowsVirtualKeyCode: 0x87, nativeVirtualKeyCode: 0x87 });
    try {
      for (let index = 0; index < 12; index++) {
        await f24(true);
        await f24(false);
      }
      const agentKeys = await until(async () => {
        const text = await fs.readFile(settings.observations, 'utf8').catch(error => {
          if (error.code === 'ENOENT') return '';
          throw error;
        });
        const keys = text.trim().split('\n').filter(Boolean).map(line => {
          const [sequence, pressed] = line.split(' ').map(Number);
          return { sequence, pressed: !!pressed };
        });
        return keys.length >= 24 ? keys : null;
      }, '24 F24 transitions did not reach synthetic Agent').catch(async error => {
        const dom = await page.evaluate(() => ({ events: fixtureKeyEvents, active: document.activeElement?.getAttribute('aria-label'), hidden: document.hidden }));
        const observed = await fs.readFile(settings.observations, 'utf8').catch(() => 'not created');
        throw new Error(`${error.message}: browser=${JSON.stringify(browserKeys)}, sent=${JSON.stringify(sentInputs)}, dom=${JSON.stringify(dom)}, agent=${JSON.stringify(observed)}, sockets=${sockets.size}, statuses=${JSON.stringify(await page.getByRole('status').allTextContents())}, errors=${errors.join('; ')}`);
      });
      const ordered = events => events.length === 24 && events.every((event, index) =>
        event.pressed === (index % 2 === 0) && (index === 0 || event.sequence > events[index - 1].sequence));
      assert.ok(ordered(browserKeys), `Browser reordered input: ${JSON.stringify(browserKeys)}`);
      assert.ok(ordered(agentKeys), `Gateway or Agent transport reordered input: ${JSON.stringify(agentKeys)}`);
      assert.ok(network.metrics().shapedBytes > before.shapedBytes, 'Encrypted WSS uplink bypassed the shaper');
      assert.ok(originalSockets.every(socket => sockets.has(socket)), 'Desktop WSS channels reconnected during shaping');
      assert.deepEqual(errors, [], 'Browser raised an uncaught exception');
      console.log(`Synthetic desktop transport: ${agentKeys.length} ordered F24 transitions reached Agent native input; ${network.metrics().shapedBytes - before.shapedBytes} shaped TLS bytes, 0 reconnects (no DXGI/Windows injection)`);
    } finally { network.shape(0, 0); }
    await page.getByRole('button', { name: '断开连接' }).click();
    await page.getByRole('region', { name: '远程桌面' }).waitFor({ state: 'detached' });
    await until(() => sockets.size === 0, 'Synthetic desktop WSS channels did not close');
  } finally {
    await browser?.close();
    await network.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
