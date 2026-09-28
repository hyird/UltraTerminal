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
    for (const ratio of [1, 1.25, 1.5, 2]) {
      const context = await browser.newContext({ deviceScaleFactor: ratio, viewport: { width: Math.floor(2560 / ratio), height: Math.floor(1280 / ratio) } });
      const page = await context.newPage();
      await page.route('**/pixels-fixture', route => route.fulfill({ contentType: 'text/html', body: '<style>html,body{margin:0;height:100%}</style><div class="desktop-viewport" data-scale="native" style="width:100%;height:100%"><div class="desktop-stage"><canvas width="1920" height="1080"></canvas><canvas class="cursor-overlay" width="1920" height="1080"></canvas></div></div>' }));
      await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/pixels-fixture`);
      const native = await page.evaluate(async () => {
        await import('/src/styles/desktop.css');
        const { DesktopSession } = await import('/src/sessions/desktop/desktop.session.ts');
        const { DesktopInput } = await import('/src/sessions/desktop/input.session.ts');
        const canvas = document.querySelector('canvas');
        window.session = new DesktopSession({ id: 'pixels', writeControl: true, channels: [] }, canvas, document.querySelector('.cursor-overlay'), () => {}, () => {});
        window.session.setScaleMode('native');
        window.session.cursorShape = new ImageData(new Uint8ClampedArray(32 * 32 * 4).fill(255), 32, 32);
        window.session.hotspot = { x: 8, y: 12 };
        window.events = [];
        window.input = new DesktopInput(canvas, event => window.events.push(event), { width: 1920, height: 1080 });
        const bounds = canvas.getBoundingClientRect();
        return [bounds.width * devicePixelRatio, bounds.height * devicePixelRatio];
      });
      assert.ok(Math.abs(native[0] - 1920) < 0.05 && Math.abs(native[1] - 1080) < 0.05);
      await page.evaluate(() => {
        document.querySelector('.desktop-viewport').dataset.scale = 'fit';
        window.session.setScaleMode('fit');
      });
      for (const viewport of [{ width: 1000, height: 750 }, { width: 600, height: 900 }, { width: 1600, height: 400 }]) {
        await page.setViewportSize(viewport);
        await page.waitForFunction(() => {
          const bounds = document.querySelector('canvas').getBoundingClientRect();
          return Math.abs(bounds.width - innerWidth) < 0.1 && Math.abs(bounds.height - innerHeight) < 0.1;
        });
        const result = await page.evaluate(async () => {
          const canvas = document.querySelector('canvas');
          const overlay = document.querySelector('.cursor-overlay');
          const viewport = document.querySelector('.desktop-viewport');
          window.session.updateLocalCursor();
          const cursor = canvas.style.getPropertyValue('--desktop-cursor');
          const match = /url\("([^"]+)"\) (\d+) (\d+), default/.exec(cursor);
          if (!match) throw new Error('Local cursor is missing');
          const image = await createImageBitmap(await (await fetch(match[1])).blob());
          const result = {
            overflow: viewport.scrollWidth > viewport.clientWidth || viewport.scrollHeight > viewport.clientHeight,
            native: [canvas.width, canvas.height],
            overlay: [overlay.getBoundingClientRect().width, overlay.getBoundingClientRect().height],
            cursor: [image.width, image.height, Number(match[2]), Number(match[3])],
          };
          image.close();
          return result;
        });
        assert.equal(result.overflow, false);
        assert.deepEqual(result.native, [1920, 1080]);
        assert.deepEqual(result.overlay, [viewport.width, viewport.height]);
        assert.deepEqual(result.cursor, [Math.round(32 * viewport.width / 1920), Math.round(32 * viewport.height / 1080), Math.round(8 * viewport.width / 1920), Math.round(12 * viewport.height / 1080)]);
        for (const [x, y] of [[0.1, 0.2], [0.5, 0.5], [0.9, 0.8]]) {
          await page.evaluate(() => { window.events = []; });
          await page.mouse.click(viewport.width * x, viewport.height * y);
          assert.ok(await page.evaluate(({ x, y }) => window.events.some(event => event.kind === 1 && Math.abs(event.x - 1920 * x) <= 1 && Math.abs(event.y - 1080 * y) <= 1), { x, y }), 'Stretched pointer coordinates differ');
        }
      }
      // Returning to native mode must restore the original physical pixel size.
      const restored = await page.evaluate(() => {
        document.querySelector('.desktop-viewport').dataset.scale = 'native';
        window.session.setScaleMode('native');
        const bounds = document.querySelector('canvas').getBoundingClientRect();
        window.input.dispose();
        window.session.stop();
        return [bounds.width * devicePixelRatio, bounds.height * devicePixelRatio];
      });
      assert.ok(Math.abs(restored[0] - 1920) < 0.05 && Math.abs(restored[1] - 1080) < 0.05);
      console.log(`PASS dpr=${ratio}: native pixels, stretched fit, resize, cursor hotspot and pointer mapping`);
      await context.close();
    }
  } finally {
    await browser.close();
    await server.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
