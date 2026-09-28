const assert = require('node:assert/strict');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
const { chromium } = require('playwright');

async function main() {
  const root = path.resolve(__dirname, '../../web');
  const { createServer } = await import(pathToFileURL(path.join(root, 'node_modules/vite/dist/node/index.js')));
  const server = await createServer({ root, server: { host: '127.0.0.1', port: 0 } });
  await server.listen();
  let browser;
  try {
    browser = await chromium.launch({ executablePath: process.argv[2], headless: true });
    const page = await browser.newPage();
    await page.route('**/outbound-fixture', route => route.fulfill({ contentType: 'text/html', body: '<canvas></canvas><canvas id="overlay"></canvas>' }));
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/outbound-fixture`);
    const result = await page.evaluate(async () => {
      const { DesktopSession } = await import('/src/sessions/desktop/desktop.session.ts');
      const { ultraterimal } = await import('/src/generated/session.js');
      const { DesktopInput } = await import('/src/sessions/desktop/input.session.ts');
      const wire = ultraterimal.wire;
      const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
      const fixtures = [];
      function fixture() {
        const sent = [];
        let failures = 0, closes = 0;
        const statuses = [];
        const canvas = document.createElement('canvas');
        const overlay = document.createElement('canvas');
        document.body.append(canvas, overlay);
        const session = new DesktopSession({ id: 'weak-network', writeControl: true, channels: [] },
          canvas, overlay, message => statuses.push(message), () => { failures++; });
        const socket = { readyState: WebSocket.OPEN, bufferedAmount: 65537,
          send(bytes) { sent.push(wire.Envelope.decode(bytes)); },
          close() { closes++; this.readyState = WebSocket.CLOSED; } };
        const channel = { socket, id: 'control', purpose: 2, ready: true, sent: 0, received: 0 };
        session.channels.push(channel);
        const send = body => session.send(channel, body);
        const move = x => send({ inputEvent: { kind: wire.InputEvent.Kind.POINTER_MOVE, x } });
        const key = pressed => send({ inputEvent: { kind: wire.InputEvent.Kind.KEY, code: 65, pressed } });
        fixtures.push(session);
        const hold = () => {
          session.input = new DesktopInput(canvas, event => send({ inputEvent: event }));
          canvas.focus();
          canvas.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyA', bubbles: true }));
        };
        return { session, socket, sent, channel, send, move, key, hold, statuses,
          get closes() { return closes; }, get failures() { return failures; } };
      }
      function check(condition, message) { if (!condition) throw new Error(message); }
      try {
        const f = fixture();
        for (let i = 0; i < 1000; i++) f.move(i);
        f.key(true);
        f.move(1001); f.move(1002);
        f.key(false);
        f.send({ qualityFeedback: { displayedFrame: 1 } });
        f.send({ qualityFeedback: { displayedFrame: 2 } });
        check(f.sent.length === 0 && f.failures === 0, 'short stall must not disconnect');
        check(f.channel.sent === 0, 'sequence must be assigned only on send');
        f.socket.bufferedAmount = 0;
        await wait(80);
        check(f.sent.length === 5, 'coalesced message count');
        check(f.sent[0].inputEvent.x === 999 && f.sent[1].inputEvent.pressed &&
          f.sent[2].inputEvent.x === 1002 && !f.sent[3].inputEvent.pressed &&
          Number(f.sent[4].qualityFeedback.displayedFrame) === 2, 'input barriers and feedback order');
        check(f.sent.every((m, i) => Number(m.sequence) === i + 1), 'contiguous wire sequence');
        const flowing = fixture();
        flowing.socket.bufferedAmount = 0;
        flowing.key(true);
        check(flowing.sent.length === 1, 'healthy path must send synchronously');
        const throttled = fixture();
        const capture = throttled.socket.send;
        throttled.socket.send = bytes => { capture(bytes); throttled.socket.bufferedAmount += 20000; };
        throttled.key(true); throttled.key(false);
        throttled.socket.bufferedAmount = 0;
        await wait(80);
        check(throttled.sent.length === 1 && throttled.failures === 0, 'draining must respect renewed socket pressure');
        throttled.socket.bufferedAmount = 0;
        await wait(80);
        check(throttled.sent.length === 2 && !throttled.sent[1].inputEvent.pressed, 'release must resume in order');
        const overflow = fixture();
        for (let i = 0; i < 129; i++) overflow.key(true);
        check(overflow.failures === 1, 'explicit bounded overflow');
        const stale = fixture(); stale.move(1);
        await wait(600); stale.move(2);
        await wait(500);
        check(stale.failures === 1, 'coalescing must not renew deadline');
        stale.socket.bufferedAmount = 0;
        await wait(80);
        check(stale.sent.length === 0, 'stale input must not replay');
        const stopped = fixture(); stopped.key(true); stopped.session.stop();
        stopped.socket.bufferedAmount = 0;
        await wait(80);
        check(stopped.sent.length === 0 && stopped.failures === 0, 'stop cancels pending work');
        const held = fixture(); held.hold();
        held.socket.bufferedAmount = 0; held.session.stop();
        check(held.sent.length === 0 && held.closes === 1, 'disconnect must not flush held-key backlog');
        const rebound = fixture(); rebound.hold(); rebound.move(1000);
        rebound.socket.bufferedAmount = 0;
        await rebound.session.receive({ purpose: 3, ready: true, id: 'video', received: 0 }, wire.Envelope.create({
          protocolVersion: 1, sessionId: 'weak-network', channelId: 'video', sequence: 1, generation: 2,
          videoConfig: { width: 32, height: 32, codec: 'avc1.42e01f' },
        }));
        check(rebound.failures === 1 && rebound.sent.length === 0 && rebound.closes === 1,
          'generation boundary must reject old-coordinate input without flushing it');
        const heldAtBoundary = fixture(); heldAtBoundary.socket.bufferedAmount = 0;
        heldAtBoundary.hold();
        heldAtBoundary.socket.bufferedAmount = 65537;
        await heldAtBoundary.session.receive({ purpose: 3, ready: true, id: 'video', received: 0 }, wire.Envelope.create({
          protocolVersion: 1, sessionId: 'weak-network', channelId: 'video', sequence: 1, generation: 2,
          videoConfig: { width: 32, height: 32, codec: 'avc1.42e01f' },
        }));
        check(heldAtBoundary.failures === 1 && heldAtBoundary.sent.length === 1 &&
          heldAtBoundary.sent[0].inputEvent.pressed && heldAtBoundary.closes === 1,
          'held-key release queued at generation boundary must close, not cross into new binding');
        const reset = fixture(); reset.hold();
        for (let i = 1; i < 128; i++) reset.key(true);
        await reset.session.receive(reset.channel, wire.Envelope.create({
          protocolVersion: 1, sessionId: 'weak-network', channelId: 'control', sequence: 1,
          desktopProbe: { nonce: 1, resetInput: true },
        }));
        check(reset.failures === 1 && reset.session.inputDeadline === undefined &&
          reset.session.livenessDeadline === undefined && reset.statuses.at(-1).includes('拥塞'),
          'failed release must not arm timers or replace failure status');
        const ready = fixture(); ready.channel.ready = false; ready.session.displayedFrame = 1;
        ready.socket.readyState = WebSocket.CLOSED;
        await ready.session.receive(ready.channel, wire.Envelope.create({
          protocolVersion: 1, sessionId: 'weak-network', channelId: 'control', sequence: 1,
          channelReady: { purpose: 2 },
        }));
        check(ready.failures === 1 && ready.session.inputDeadline === undefined &&
          ready.session.livenessDeadline === undefined, 'failed initial feedback must not rearm timers');
        const expired = fixture(); expired.hold();
        for (let i = 1; i < 128; i++) expired.key(true);
        let timeoutCallback;
        const originalTimeout = window.setTimeout;
        window.setTimeout = (callback, ms, ...args) => {
          if (ms === 4000) timeoutCallback = callback;
          return originalTimeout(callback, ms, ...args);
        };
        try { expired.session.armInputDeadline(); } finally { window.setTimeout = originalTimeout; }
        timeoutCallback();
        check(expired.failures === 1 && expired.statuses.at(-1).includes('拥塞'),
          'failed timeout release must retain terminal status');
        const closed = fixture(); closed.socket.readyState = WebSocket.CLOSED; closed.key(true);
        check(closed.failures === 1, 'closed transport');
        const broken = fixture(); broken.socket.bufferedAmount = 0;
        broken.socket.send = () => { throw new Error('send failure'); }; broken.key(true);
        check(broken.failures === 1, 'transport exception');
        const pointerCanvas = document.createElement('canvas');
        document.body.append(pointerCanvas);
        const pointerEvents = [];
        const pointerInput = new DesktopInput(pointerCanvas, event => pointerEvents.push(event), pointerCanvas, () => true);
        pointerCanvas.focus();
        const bounds = pointerCanvas.getBoundingClientRect();
        const movePointer = x => pointerCanvas.dispatchEvent(new PointerEvent('pointermove', {
          pointerType: 'mouse', clientX: bounds.left + x, clientY: bounds.top + 1, buttons: 0,
        }));
        for (let i = 0; i < 100; i++) movePointer(i);
        check(pointerEvents.length === 0, 'pointer burst should coalesce before sending');
        await wait(40);
        check(pointerEvents.length === 1 && pointerEvents[0].x === 99, 'pointer burst must keep the latest position');
        movePointer(100);
        pointerCanvas.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyA', bubbles: true }));
        check(pointerEvents.length === 3 && pointerEvents[1].x === 100 && pointerEvents[2].kind === wire.InputEvent.Kind.KEY,
          'keypress must flush latest pointer position without timer delay');
        pointerInput.dispose();
        return { passed: true, coalescedMoves: 1000, transmitted: f.sent.length };
      } finally { for (const session of fixtures) session.stop(); }
    });
    assert.equal(result.passed, true);
    console.log(JSON.stringify(result));
  } finally { await browser?.close(); await server.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
