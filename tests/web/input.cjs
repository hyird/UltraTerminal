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
    const cases = [
      {
        name: 'Missing right Shift down is recovered from the chord modifier flag',
        keys: [
          ['keydown', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'ShiftRight', key: 'Shift', location: 2 }],
          ['keyup', { code: 'KeyA' }],
        ],
        expected: [[0xa0, true], [0x41, true], [0xa0, false], [0x41, false]],
      },
      {
        name: 'Identified right Shift takes over an inferred left Shift',
        keys: [
          ['keydown', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'KeyA', shiftKey: true }],
          ['keydown', { code: 'ShiftRight', shiftKey: true }],
          ['keyup', { code: 'ShiftRight' }],
        ],
        expected: [[0xa0, true], [0x41, true], [0x41, false], [0xa0, false], [0xa1, true], [0xa1, false]],
      },
      {
        name: 'An inferred Shift remains held while another physical Shift remains down',
        keys: [
          ['keydown', { code: 'Digit1', shiftKey: true }],
          ['keyup', { code: 'Digit1', shiftKey: true }],
          ['keyup', { code: 'ShiftRight', shiftKey: true }],
          ['keyup', { code: 'ShiftLeft' }],
        ],
        expected: [[0xa0, true], [0x31, true], [0x31, false], [0xa0, false]],
      },
      {
        name: 'Unowned releases and repeats must not adopt a Shift',
        keys: [
          ['keyup', { code: 'ShiftRight', shiftKey: true }],
          ['keydown', { code: 'KeyA', shiftKey: true, repeat: true }],
          ['keyup', { code: 'KeyA', shiftKey: true }],
        ],
        expected: [],
      },
      {
        name: 'Missing Shift up is recovered before the next unshifted key',
        keys: [
          ['keydown', { code: 'ShiftRight', shiftKey: true }],
          ['keydown', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'KeyA', shiftKey: true }],
          ['keydown', { code: 'Digit1' }],
          ['keyup', { code: 'Digit1' }],
        ],
        expected: [[0xa1, true], [0x41, true], [0x41, false], [0xa1, false], [0x31, true], [0x31, false]],
      },
      {
        name: 'IME composition must not swallow modifiers or owned releases',
        keys: [
          ['keydown', { code: 'ShiftRight', shiftKey: true, isComposing: true }],
          ['keydown', { code: 'KeyB', shiftKey: true, isComposing: true }],
          ['keyup', { code: 'KeyB', shiftKey: true, isComposing: true }],
          ['keydown', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'KeyA', shiftKey: true, isComposing: true }],
          ['keyup', { code: 'ShiftRight', isComposing: true }],
        ],
        expected: [[0xa1, true], [0x41, true], [0x41, false], [0xa1, false]],
      },
      {
        name: 'Unidentified Shift uses the browser key location',
        keys: [
          ['keydown', { code: 'Unidentified', key: 'Shift', location: 2, shiftKey: true }],
          ['keydown', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'Unidentified', key: 'Shift', location: 2 }],
        ],
        expected: [[0xa1, true], [0x41, true], [0x41, false], [0xa1, false]],
      },
      {
        name: 'Releasing one physical Shift retains the other',
        keys: [
          ['keydown', { code: 'ShiftLeft', shiftKey: true }],
          ['keydown', { code: 'ShiftRight', shiftKey: true }],
          ['keyup', { code: 'ShiftLeft', shiftKey: true }],
          ['keydown', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'KeyA', shiftKey: true }],
          ['keyup', { code: 'ShiftRight' }],
        ],
        expected: [[0xa0, true], [0xa1, true], [0xa0, false], [0x41, true], [0x41, false], [0xa1, false]],
      },
    ];
    for (const test of cases) {
      const events = await page.evaluate(keys => {
        window.events = [];
        const canvas = document.querySelector('canvas');
        for (const [type, options] of keys)
          canvas.dispatchEvent(new KeyboardEvent(type, { ...options, bubbles: true, cancelable: true }));
        window.input.release();
        return window.events;
      }, test.keys);
      assert.deepEqual(events, test.expected, test.name);
    }
    const blurred = await page.evaluate(() => {
      const canvas = document.querySelector('canvas');
      window.events = [];
      canvas.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyA', shiftKey: true }));
      canvas.blur();
      canvas.focus();
      canvas.dispatchEvent(new KeyboardEvent('keyup', { code: 'ShiftRight' }));
      canvas.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyB' }));
      canvas.dispatchEvent(new KeyboardEvent('keyup', { code: 'KeyB' }));
      window.input.dispose();
      return window.events;
    });
    assert.deepEqual(blurred, [[0xa0, true], [0x41, true], ['release'], [0x42, true], [0x42, false]]);
    console.log('PASS: Chrome Shift chords, modifier recovery, composition, key location, protobuf ordering and blur cleanup');
  } finally {
    await browser.close();
    await server.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
