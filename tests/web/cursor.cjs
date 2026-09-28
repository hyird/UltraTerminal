const assert = require('node:assert/strict');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { chromium } = require('playwright');

async function main() {
  const root = path.resolve(__dirname, '../../web');
  process.chdir(root);
  const { createServer } = await import(pathToFileURL(path.join(root, 'node_modules/vite/dist/node/index.js')));
  const server = await createServer({ root, server: { host: '127.0.0.1', port: 0 } });
  await server.listen();
  const browser = await chromium.launch({ executablePath: process.argv[2] || 'C:/Program Files/Google/Chrome/Application/chrome.exe', headless: true });
  try {
    const page = await browser.newPage();
    await page.route('**/cursor-fixture', route => route.fulfill({ contentType: 'text/html', body: '<div class="desktop-stage" style="width:640px"><canvas width="1280" height="800"></canvas><canvas class="cursor-overlay" width="1280" height="800"></canvas></div>' }));
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/cursor-fixture`);
    await page.evaluate(async () => {
      await import('/src/styles/index.css');
      const { DesktopSession } = await import('/src/sessions/desktop/desktop.session.ts');
      const { ultraterimal } = await import('/src/generated/session.js');
      const canvas = document.querySelector('canvas');
      const overlay = document.querySelector('.cursor-overlay');
      window.session = new DesktopSession({ id: 'cursor-test', writeControl: true, channels: [] }, canvas, overlay, () => {}, () => {});
      canvas.dataset.control = 'ready';
      const rgba = new Uint8Array(32 * 32 * 4).fill(255);
      await window.session.receive({ purpose: 2, ready: true, id: 'input', received: 1 }, ultraterimal.wire.Envelope.create({ protocolVersion: 1, sessionId: 'cursor-test', channelId: 'input', sequence: 2, cursorUpdate: { width: 32, height: 32, hotspotX: 4, hotspotY: 6, x: 100, y: 100, visible: true, rgba } }));
    });
    await page.mouse.move(200, 100);
    assert.notEqual(await page.locator('canvas').first().evaluate(c => getComputedStyle(c).cursor), 'none');
    assert.equal(await page.locator('.cursor-overlay').evaluate(c => getComputedStyle(c).visibility), 'hidden');
    await page.locator('canvas').first().evaluate(c => { c.tabIndex = 0; c.focus(); });
    await page.waitForFunction(() => getComputedStyle(document.querySelector('canvas')).cursor.includes('2 3, default'));
    assert.equal(await page.locator('.cursor-overlay').evaluate(c => getComputedStyle(c).visibility), 'hidden');
    await page.mouse.move(400, 200);
    assert.match(await page.locator('canvas').first().evaluate(c => getComputedStyle(c).cursor), /data:image\/png/);
    await page.locator('.desktop-stage').evaluate(c => c.style.width = '1280px');
    await page.waitForFunction(() => getComputedStyle(document.querySelector('canvas')).cursor.includes('4 6, default'));
    assert.equal(await page.locator('.cursor-overlay').evaluate(c => c.getContext('2d').getImageData(96, 94, 1, 1).data[3]), 0);
    await page.locator('canvas').first().evaluate(c => c.blur());
    await page.mouse.move(0, 0);
    assert.equal(await page.locator('.cursor-overlay').evaluate(c => getComputedStyle(c).visibility), 'hidden');
    await page.evaluate(() => {
      const stage = document.querySelector('.desktop-stage');
      const viewport = document.createElement('div');
      viewport.className = 'desktop-viewport';
      viewport.style.cssText = 'width:800px;height:400px;flex:none';
      stage.replaceWith(viewport);
      viewport.append(stage);
      stage.style.removeProperty('width');
      window.session.viewportResize.observe(viewport);
      window.session.sizeDesktop();
    });
    const fit = await page.locator('.desktop-stage').evaluate(c => ({ width: c.clientWidth, height: c.clientHeight, scroll: c.parentElement.scrollHeight > c.parentElement.clientHeight }));
    assert.deepEqual(fit, {width:1280,height:800,scroll:true});
    await page.locator('.desktop-viewport').evaluate(c => c.style.width = '320px');
    assert.equal(await page.locator('.desktop-stage').evaluate(c => c.clientWidth),1280);
    const point = await page.evaluate(async () => {
      const { DesktopInput } = await import('/src/sessions/desktop/input.session.ts');
      const canvas = document.querySelector('canvas');
      window.inputs=[];
      window.input=new DesktopInput(canvas,e=>window.inputs.push(e),{width:1280,height:800});
      const viewport=document.querySelector('.desktop-viewport');
      viewport.scrollLeft=300; viewport.scrollTop=200;
      const bounds=viewport.getBoundingClientRect();
      return {x:bounds.left+40,y:bounds.top+40,remoteX:viewport.scrollLeft+40,remoteY:viewport.scrollTop+40};
    });
    await page.mouse.click(point.x,point.y);
    assert.ok(await page.evaluate(p=>window.inputs.some(e=>e.x===p.remoteX&&e.y===p.remoteY),point),'Scrolled input coordinates differ from original desktop pixels');
    await page.evaluate(()=>window.input.dispose());
    await page.locator('canvas').first().evaluate(c => delete c.dataset.control);
    assert.notEqual(await page.locator('canvas').first().evaluate(c => getComputedStyle(c).cursor), 'none');
    assert.equal(await page.locator('.cursor-overlay').evaluate(c => getComputedStyle(c).visibility), 'visible');
    assert.equal(await page.locator('.cursor-overlay').evaluate(c => c.getContext('2d').getImageData(96, 94, 1, 1).data[3]), 0);
    await page.evaluate(() => window.session.stop());
    assert.equal(await page.locator('canvas').first().evaluate(c => c.style.getPropertyValue('--desktop-cursor')), '');
    console.log('PASS: writable session never paints remote cursor, focus/blur/leave, scaled local hotspot, native pixel size, scrolling and input coordinates, cleanup');
  } finally {
    await browser.close();
    await server.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
