const assert = require('node:assert/strict');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { chromium } = require('playwright');

async function main() {
  const root = path.resolve(__dirname, '../../web');
  const { createServer } = await import(pathToFileURL(path.join(root, 'node_modules/vite/dist/node/index.js')));
  const server = await createServer({ root, server: { host: '127.0.0.1', port: 0 } });
  await server.listen();
  const browser = await chromium.launch({ executablePath: process.argv[2] || 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true });
  try {
    const page = await browser.newPage();
    await page.route('**/input-fixture', route => route.fulfill({ contentType: 'text/html', body: '<canvas width="640" height="400"></canvas>' }));
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/input-fixture`);
    await page.evaluate(async () => {
      const { DesktopInput } = await import('/src/sessions/desktop/input.session.ts');
      const { ultraterimal } = await import('/src/generated/session.js');
      const canvas = document.querySelector('canvas');
      window.events = [];
      window.input = new DesktopInput(canvas, event => {
        const wire = ultraterimal.wire.InputEvent;
        const decoded = wire.decode(wire.encode(wire.create(event)).finish());
        if (decoded.kind === wire.Kind.KEY) window.events.push([decoded.code, decoded.pressed]);
        else if (decoded.kind === wire.Kind.RELEASE_ALL) window.events.push(['release']);
      });
      canvas.focus();
    });
    for (const [shift, code] of [['ShiftLeft', 0xa0], ['ShiftRight', 0xa1]]) {
      await page.evaluate(() => { window.events = []; });
      await page.keyboard.down(shift);
      await page.keyboard.press('KeyA');
      await page.keyboard.press('Digit1');
      await page.keyboard.up(shift);
      assert.deepEqual(await page.evaluate(() => window.events), [[code, true], [0x41, true], [0x41, false], [0x31, true], [0x31, false], [code, false]]);
      await page.keyboard.down(shift);
      await page.evaluate(() => { window.events = []; document.querySelector('canvas').blur(); });
      await page.keyboard.up(shift);
      assert.deepEqual(await page.evaluate(() => window.events), [['release']]);
      await page.locator('canvas').focus();
    }
    await page.evaluate(() => window.input.dispose());
    console.log('PASS: Chrome left/right Shift chords, protobuf round trip, ordered release and blur cleanup');
  } finally {
    await browser.close();
    await server.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
