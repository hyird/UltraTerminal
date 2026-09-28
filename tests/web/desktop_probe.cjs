const assert = require('node:assert/strict');
const path = require('node:path');
const { chromium } = require('playwright');
const { createServer } = require(require.resolve('vite', { paths: [__dirname] }));

async function startSession(browser, origin, withFrame = true, withControl = true) {
  const page = await browser.newPage();
  page.setDefaultTimeout(8000);
  const pageErrors = [];
  page.on('pageerror', error => pageErrors.push(error.message));
  await page.goto(new URL('/src/sessions/desktop/desktop.session.ts', origin).href);
  await page.evaluate(async ({ withFrame, withControl }) => {
    window.fixtureSockets = [];
    window.WebSocket = class {
      static OPEN = 1;
      constructor() { this.readyState = 1; this.bufferedAmount = 0; window.fixtureSockets.push(this); }
      send(data) { this.sent ??= []; this.sent.push(JSON.parse(new TextDecoder().decode(data))); }
      close() { this.readyState = 3; }
    };
    window.VideoDecoder = class {
      static async isConfigSupported() { return { supported: true }; }
      constructor(callbacks) { this.callbacks = callbacks; this.state = 'unconfigured'; }
      configure() { this.state = 'configured'; }
      decode(chunk) {
        const frame = document.createElement('canvas');
        frame.width = 320; frame.height = 200;
        Object.assign(frame, { timestamp: chunk.timestamp, displayWidth: 320, displayHeight: 200, close() {} });
        this.callbacks.output(frame);
      }
      close() { this.state = 'closed'; }
    };
    window.EncodedVideoChunk = class { constructor(value) { Object.assign(this, value); } };
    const { DesktopSession } = await import('/src/sessions/desktop/desktop.session.ts');
    const canvas = document.createElement('canvas');
    const overlay = document.createElement('canvas');
    document.body.append(canvas, overlay);
    window.endedCount = 0;
    window.statuses = [];
    window.session = new DesktopSession({
      id: 'test-session', writeControl: true,
      channels: [{ id: 'control', purpose: 2, ticket: 'x' }, { id: 'video', purpose: 3, ticket: 'y' }],
    }, canvas, overlay, text => statuses.push(text), () => endedCount++);
    const [control, video] = fixtureSockets;
    control.onopen(); video.onopen();
    const send = (socket, channelId, sequence, message) => socket.onmessage({ data: new TextEncoder().encode(JSON.stringify({
      protocolVersion: 1, sessionId: 'test-session', channelId, sequence, generation: message.generation || 0,
      frameId: message.frameId || 0, baseFrameId: 0, ...message,
    })).buffer });
    window.fixtureSend = send;
    if (withControl) send(control, 'control', 1, { body: 'channelReady', channelReady: { purpose: 2 } });
    send(video, 'video', 1, { body: 'channelReady', channelReady: { purpose: 3 } });
    if (withFrame) {
      send(video, 'video', 2, { body: 'videoConfig', generation: 1, videoConfig: { codec: 'avc1.42001f', width: 320, height: 200, description: [], displayModes: [] } });
      send(video, 'video', 3, { body: 'videoFrame', generation: 1, frameId: 1, videoFrame: { keyFrame: true, timestampMicroseconds: 1, data: [1] } });
    }
  }, { withFrame, withControl });
  if (withFrame) await page.waitForFunction(() => document.querySelector('canvas').dataset.control === 'ready').catch(async error => {
    throw new Error(`Desktop input fixture did not initialize: ${pageErrors.join('; ')}; ${JSON.stringify(await page.evaluate(() => ({ status: statuses, sent: fixtureSockets.map(socket => socket.sent), ready: fixtureSockets.length })))}`, { cause: error });
  });
  return page;
}

async function checkConstructionRollback(browser, origin) {
  const page = await browser.newPage();
  await page.goto(new URL('/src/sessions/desktop/desktop.session.ts', origin).href);
  const result = await page.evaluate(async () => {
    const nativeSetTimeout = window.setTimeout.bind(window);
    const nativeClearTimeout = window.clearTimeout.bind(window);
    const timers = new Set();
    window.setTimeout = (callback, delay, ...args) => {
      const timer = nativeSetTimeout(() => {
        timers.delete(timer);
        callback(...args);
      }, delay);
      timers.add(timer);
      return timer;
    };
    window.clearTimeout = timer => {
      timers.delete(timer);
      nativeClearTimeout(timer);
    };
    const observers = [];
    window.ResizeObserver = class {
      constructor() { this.disconnected = false; observers.push(this); }
      observe() {}
      disconnect() { this.disconnected = true; }
    };
    const mediaSignals = [];
    window.matchMedia = () => ({
      addEventListener(_name, _callback, options) { mediaSignals.push(options.signal); },
    });
    const visibilitySignals = [];
    const addDocumentListener = document.addEventListener.bind(document);
    document.addEventListener = (name, callback, options) => {
      if (name === 'visibilitychange') visibilitySignals.push(options.signal);
      addDocumentListener(name, callback, options);
    };
    const sockets = [];
    window.WebSocket = class {
      static OPEN = 1;
      constructor() {
        if (sockets.length === 1) throw new Error('injected second socket failure');
        this.readyState = 1;
        this.closeCount = 0;
        sockets.push(this);
      }
      close() { this.closeCount++; this.readyState = 3; }
    };
    window.VideoDecoder = class {};
    const { DesktopSession } = await import('/src/sessions/desktop/desktop.session.ts');
    let ended = 0;
    let thrown = '';
    try {
      new DesktopSession({
        id: 'failure-session', writeControl: true,
        channels: [
          { id: 'control', purpose: 2, ticket: 'x' },
          { id: 'video', purpose: 3, ticket: 'y' },
        ],
      }, document.createElement('canvas'), document.createElement('canvas'),
      () => {}, () => ended++);
    } catch (error) { thrown = error.message; }
    return {
      thrown,
      ended,
      sockets: sockets.map(socket => ({
        closeCount: socket.closeCount,
        listenersCleared: ['onopen', 'onmessage', 'onerror', 'onclose'].every(name => socket[name] === null),
      })),
      observersDisconnected: observers.length === 1 && observers[0].disconnected,
      signalsAborted: [...mediaSignals, ...visibilitySignals].every(signal => signal.aborted),
      timers: timers.size,
    };
  });
  await page.close();
  assert.equal(result.thrown, 'injected second socket failure');
  assert.equal(result.ended, 0, 'constructor rollback must not invoke ended');
  assert.deepEqual(result.sockets, [{ closeCount: 1, listenersCleared: true }]);
  assert.equal(result.observersDisconnected, true, 'constructor rollback must disconnect ResizeObserver');
  assert.equal(result.signalsAborted, true, 'constructor rollback must abort registered listeners');
  assert.equal(result.timers, 0, 'constructor rollback must clear registered timers');
}

async function main() {
  const webRoot = path.resolve(__dirname, '../../web');
  const vite = await createServer({
    configFile: false,
    root: webRoot,
    resolve: {
      alias: [{
        find: /^\.\.\/\.\.\/generated\/session$/,
        replacement: path.resolve(__dirname, 'session_wire.fixture.js'),
      }],
    },
    server: { host: '127.0.0.1', port: 0 },
    logLevel: 'error',
  });
  let browser;
  try {
    await vite.listen();
    const address = vite.httpServer.address();
    browser = await chromium.launch({
      executablePath: process.argv[2] || 'C:/Program Files/Google/Chrome/Application/chrome.exe',
      headless: true,
    });
    const origin = `http://127.0.0.1:${address.port}/`;
    await checkConstructionRollback(browser, origin);
    const startup = await startSession(browser, origin, false);
    await startup.waitForFunction(() => session.channels.every(channel => channel.ready));
    const beforeFrame = await startup.evaluate(() => ({
      inputDeadline: !!session.inputDeadline, livenessDeadline: !!session.livenessDeadline,
      firstFrameDeadline: !!session.deadline, displayed: session.displayedFrame,
    }));
    assert.deepEqual(beforeFrame, { inputDeadline: false, livenessDeadline: false, firstFrameDeadline: true, displayed: 0 },
      'gateway channel readiness must not start input timers before the Agent can send a frame');
    await startup.waitForTimeout(4200);
    assert.deepEqual(await startup.evaluate(() => ({ statuses, ended: endedCount, frozen: session.inputFrozen })),
      { statuses: [], ended: 0, frozen: false }, 'waiting for the initial desktop image is not an input link failure');
    await startup.evaluate(() => {
      fixtureSend(fixtureSockets[1], 'video', 2, { body: 'videoConfig', generation: 1,
        videoConfig: { codec: 'avc1.42001f', width: 320, height: 200, description: [], displayModes: [] } });
      fixtureSend(fixtureSockets[1], 'video', 3, { body: 'videoFrame', generation: 1, frameId: 1,
        videoFrame: { keyFrame: true, timestampMicroseconds: 1, data: [1] } });
    });
    await startup.waitForFunction(() => document.querySelector('canvas').dataset.control === 'ready');
    assert.deepEqual(await startup.evaluate(() => ({ input: !!session.inputDeadline, liveness: !!session.livenessDeadline, displayed: session.displayedFrame })),
      { input: true, liveness: true, displayed: 1 }, 'first frame must arm input and liveness deadlines');
    await startup.evaluate(() => session.stop());
    await startup.close();
    const videoFirst = await startSession(browser, origin, false, false);
    await videoFirst.waitForFunction(() => session.channels[1].ready);
    await videoFirst.evaluate(() => {
      fixtureSend(fixtureSockets[1], 'video', 2, { body: 'videoConfig', generation: 1,
        videoConfig: { codec: 'avc1.42001f', width: 320, height: 200, description: [], displayModes: [] } });
      fixtureSend(fixtureSockets[1], 'video', 3, { body: 'videoFrame', generation: 1, frameId: 1,
        videoFrame: { keyFrame: true, timestampMicroseconds: 1, data: [1] } });
    });
    await videoFirst.waitForFunction(() => session.displayedFrame === 1);
    const waitingForControl = await videoFirst.evaluate(() => ({
      status: statuses.at(-1), control: document.querySelector('canvas').dataset.control || null,
      startup: session.deadline !== undefined, input: !!session.input,
      inputTimer: !!session.inputDeadline, livenessTimer: !!session.livenessDeadline,
    }));
    assert.deepEqual(waitingForControl, { status: '320 × 200 · 等待控制通道', control: null,
      startup: true, input: false, inputTimer: false, livenessTimer: false },
      'displaying video first must not falsely advertise writable input or finish the startup deadline');
    await videoFirst.evaluate(() => document.querySelector('canvas').dispatchEvent(new KeyboardEvent('keydown', { code: 'F24', bubbles: true })));
    assert.equal(await videoFirst.evaluate(() => fixtureSockets[0].sent.filter(message => message.inputEvent).length), 0,
      'unpaired input socket must not emit a key transition');
    await videoFirst.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 1, { body: 'channelReady', channelReady: { purpose: 2 } }));
    await videoFirst.waitForFunction(() => document.querySelector('canvas').dataset.control === 'ready');
    assert.deepEqual(await videoFirst.evaluate(() => ({ startup: session.deadline !== undefined,
      inputTimer: !!session.inputDeadline, livenessTimer: !!session.livenessDeadline, status: statuses.at(-1) })),
      { startup: false, inputTimer: true, livenessTimer: true, status: '320 × 200 · 可控制' },
      'late control readiness must enable writable input and arm both liveness timers');
    const pairedKeys = await videoFirst.evaluate(() => {
      const canvas = document.querySelector('canvas');
      canvas.focus();
      canvas.dispatchEvent(new KeyboardEvent('keydown', { code: 'F24', bubbles: true }));
      canvas.dispatchEvent(new KeyboardEvent('keyup', { code: 'F24', bubbles: true }));
      return fixtureSockets[0].sent.filter(message => message.inputEvent?.kind === 3)
        .map(message => ({ sequence: message.sequence, generation: message.generation, pressed: message.inputEvent.pressed }));
    });
    assert.deepEqual(pairedKeys.map(message => message.pressed), [true, false],
      'late control pairing must transmit both key transitions instead of silently dropping them');
    assert.equal(pairedKeys[1].sequence, pairedKeys[0].sequence + 1);
    assert.deepEqual(pairedKeys.map(message => message.generation), [1, 1], 'input must retain its video binding generation');
    await videoFirst.evaluate(() => session.stop());
    await videoFirst.close();
    const rebound = await startSession(browser, origin);
    await rebound.evaluate(() => {
      const canvas = document.querySelector('canvas');
      canvas.focus();
      canvas.dispatchEvent(new KeyboardEvent('keydown', { code: 'F24', bubbles: true }));
      fixtureSend(fixtureSockets[1], 'video', 4, { body: 'videoConfig', generation: 2,
        videoConfig: { codec: 'avc1.42001f', width: 320, height: 200, description: [], displayModes: [] } });
      fixtureSend(fixtureSockets[1], 'video', 5, { body: 'videoFrame', generation: 2, frameId: 1,
        videoFrame: { keyFrame: true, timestampMicroseconds: 1, data: [1] } });
    });
    await rebound.waitForFunction(() => document.querySelector('canvas').dataset.generation === '2' && document.querySelector('canvas').dataset.control === 'ready');
    const acrossGeneration = await rebound.evaluate(() => {
      const canvas = document.querySelector('canvas');
      canvas.focus();
      canvas.dispatchEvent(new KeyboardEvent('keydown', { code: 'F24', bubbles: true }));
      canvas.dispatchEvent(new KeyboardEvent('keyup', { code: 'F24', bubbles: true }));
      return fixtureSockets[0].sent.filter(message => message.inputEvent)
        .map(message => ({ kind: message.inputEvent.kind, pressed: !!message.inputEvent.pressed,
          generation: message.generation, sequence: message.sequence }));
    });
    assert.deepEqual(acrossGeneration.map(message => [message.kind, message.pressed, message.generation]),
      [[3, true, 1], [5, false, 1], [3, true, 2], [3, false, 2]],
      'already-submitted key and old-binding release must not be restamped after the next generation');
    assert.ok(acrossGeneration.every((message, index) => index === 0 || message.sequence > acrossGeneration[index - 1].sequence));
    const controlSequences = await rebound.evaluate(() => fixtureSockets[0].sent.map(message => message.sequence));
    assert.ok(controlSequences.every((sequence, index) => sequence === index + 1), 'quality feedback and input share one contiguous channel sequence');
    await rebound.evaluate(() => session.stop());
    await rebound.close();
    const contractFailures = [];
    const page = await startSession(browser, origin);
    await page.evaluate(() => document.querySelector('canvas').focus());
    const keyDown = () => page.keyboard.down('a');
    await keyDown();
    await page.evaluate(() => {
      const canvas = document.querySelector('canvas');
      canvas.setPointerCapture = () => {};
      canvas.hasPointerCapture = () => true;
      canvas.releasePointerCapture = () => {};
      canvas.dispatchEvent(new PointerEvent('pointerdown', { pointerId: 4, pointerType: 'mouse', button: 0, buttons: 1, clientX: 20, clientY: 20, bubbles: true, cancelable: true }));
    });
    await page.waitForTimeout(2100);
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 2, { body: 'desktopProbe', desktopProbe: { nonce: 1 } }));
    await page.waitForTimeout(2100);
    const healthyState = await page.evaluate(() => ({ statuses: statuses.slice(-4), ended: endedCount, socket: typeof fixtureSockets[0].onmessage }));
    assert.equal(healthyState.socket, 'function', `Healthy first probe unexpectedly ended the session: ${JSON.stringify(healthyState)}`);
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 3, { body: 'desktopProbe', desktopProbe: { nonce: 2 } }));
    await page.waitForFunction(() => fixtureSockets[0].sent.some(message => message.desktopProbe?.nonce === 2));
    const duringHealthyProbes = await page.evaluate(() => ({
      ended: endedCount,
      socketsOpen: fixtureSockets.every(socket => socket.readyState === WebSocket.OPEN),
      replies: fixtureSockets[0].sent.filter(message => message.desktopProbe).map(message => message.desktopProbe.nonce),
      inputs: fixtureSockets[0].sent.filter(message => message.inputEvent).map(message => message.inputEvent),
    }));
    assert.equal(duringHealthyProbes.ended, 0, 'normal probes must not end the session');
    assert.equal(duringHealthyProbes.socketsOpen, true, 'normal probes must preserve both WSS channels');
    assert.deepEqual(duringHealthyProbes.replies, [1, 2]);
    assert.ok(duringHealthyProbes.inputs.some(event => event.kind === 3 && event.pressed), 'held key press was not transmitted');
    assert.ok(duringHealthyProbes.inputs.some(event => event.kind === 2 && event.pressed), `drag button press was not transmitted: ${JSON.stringify(duringHealthyProbes.inputs)}`);
    const beforeCongestion = await page.evaluate(() => fixtureSockets[0].sent.length);
    await page.evaluate(() => {
      fixtureSockets[0].bufferedAmount = 65537;
      fixtureSend(fixtureSockets[0], 'control', 4, { body: 'desktopProbe', desktopProbe: { nonce: 3, resetInput: true } });
    });
    const congested = await page.evaluate(() => ({ sent: fixtureSockets[0].sent.length, frozen: session.inputFrozen }));
    assert.equal(congested.frozen, true, 'remote reset must freeze input even while its ACK is queued');
    assert.equal(congested.sent, beforeCongestion, 'backpressured release and ACK must await socket drain');
    await page.keyboard.press('b');
    assert.equal(await page.evaluate(() => fixtureSockets[0].sent.length), beforeCongestion, 'queued reset ACK must not allow new input');
    await page.evaluate(() => { fixtureSockets[0].bufferedAmount = 0; });
    await page.waitForFunction(() => fixtureSockets[0].sent.some(message => message.desktopProbe?.nonce === 3));
    const resetState = await page.evaluate(() => ({
      sent: fixtureSockets[0].sent,
      events: fixtureSockets[0].sent.filter(message => message.inputEvent),
    }));
    const resetAckIndex = resetState.sent.findIndex((message, index) => index >= beforeCongestion && message.desktopProbe?.nonce === 3);
    const releaseIndex = resetState.sent.findIndex((message, index) => index >= beforeCongestion && message.inputEvent?.kind === 5);
    if (releaseIndex < 0 || releaseIndex > resetAckIndex)
      contractFailures.push(`resetInput did not release held key/button state before ACK: ${JSON.stringify(resetState)}`);
    const beforeRecoveryInput = resetState.sent.length;
    await page.keyboard.press('b');
    const stillFrozen = await page.evaluate(() => ({ sent: fixtureSockets[0].sent, frozen: session.inputFrozen, status: statuses.at(-1) }));
    assert.equal(stillFrozen.frozen, true, 'resetInput ACK alone must not restore input');
    assert.equal(stillFrozen.status, '远端正在复位输入，等待恢复确认');
    assert.equal(stillFrozen.sent.length, beforeRecoveryInput, 'input must remain paused after resetInput ACK');
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 5, { body: 'desktopProbe', desktopProbe: { nonce: 4, resetInput: true } }));
    await page.waitForFunction(() => fixtureSockets[0].sent.some(message => message.desktopProbe?.nonce === 4));
    const pendingInput = await page.evaluate(() => fixtureSockets[0].sent.length);
    await page.keyboard.press('b');
    assert.equal(await page.evaluate(() => fixtureSockets[0].sent.length), pendingInput, 'successive resetInput probes must not thaw input');
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 6, { body: 'desktopProbe', desktopProbe: { nonce: 5 } }));
    await page.waitForFunction(() => fixtureSockets[0].sent.some(message => message.desktopProbe?.nonce === 5));
    const staleRecovery = await page.evaluate(() => ({ sent: fixtureSockets[0].sent.length, frozen: session.inputFrozen }));
    assert.equal(staleRecovery.frozen, true, 'a delayed healthy probe without acknowledgement of the reset must not thaw input');
    await page.keyboard.press('b');
    assert.equal(await page.evaluate(() => fixtureSockets[0].sent.length), staleRecovery.sent, 'unconfirmed recovery must not send new input');
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 7, { body: 'desktopProbe', desktopProbe: { nonce: 6, acceptedResponseNonce: 4 } }));
    await page.waitForFunction(() => fixtureSockets[0].sent.some(message => message.desktopProbe?.nonce === 6));
    const resumedAt = await page.evaluate(() => ({ sent: fixtureSockets[0].sent, frozen: session.inputFrozen, status: statuses.at(-1) }));
    const recoveryAckIndex = resumedAt.sent.findIndex(message => message.desktopProbe?.nonce === 6);
    assert.equal(resumedAt.frozen, false, 'only a probe confirming the reset response may restore input');
    assert.equal(resumedAt.status, '远端曾确认恢复，输入可尝试');
    await page.keyboard.press('b');
    const resumed = await page.evaluate(() => fixtureSockets[0].sent);
    const resumedInputIndex = resumed.findIndex((message, index) => index > recoveryAckIndex && message.inputEvent?.kind === 3 && message.inputEvent.pressed);
    assert.ok(resumedInputIndex > recoveryAckIndex, 'new input must follow the later non-reset probe ACK');
    await page.keyboard.down('c');
    await page.waitForTimeout(4200);
    const timedOut = await page.evaluate(() => ({
      ended: endedCount,
      socketsOpen: fixtureSockets.every(socket => socket.readyState === WebSocket.OPEN),
      events: fixtureSockets[0].sent.filter(message => message.inputEvent).map(message => message.inputEvent),
      status: statuses.at(-1),
    }));
    assert.equal(timedOut.ended, 0, 'input liveness timeout must not end the session');
    assert.equal(timedOut.socketsOpen, true, 'input timeout must preserve both WSS channels');
    assert.equal(timedOut.status, '链路缓慢，输入已暂停');
    if (!timedOut.events.some(event => event.kind === 5))
      contractFailures.push(`timeout did not release held key/button state: ${JSON.stringify(timedOut)}`);
    await page.keyboard.up('c');
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 8, { body: 'desktopProbe', desktopProbe: { nonce: 7, acceptedResponseNonce: 6 } }));
    await page.waitForFunction(() => fixtureSockets[0].sent.some(message => message.desktopProbe?.nonce === 7));
    assert.equal(await page.evaluate(() => session.inputFrozen), true, 'a healthy probe sent before local timeout must not thaw input');
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 9, { body: 'desktopProbe', desktopProbe: { nonce: 8, acceptedResponseNonce: 7 } }));
    await page.waitForFunction(() => fixtureSockets[0].sent.some(message => message.desktopProbe?.nonce === 8));
    const localRecovery = await page.evaluate(() => ({ sent: fixtureSockets[0].sent, frozen: session.inputFrozen, status: statuses.at(-1) }));
    const localAckIndex = localRecovery.sent.findIndex(message => message.desktopProbe?.nonce === 8);
    assert.equal(localRecovery.frozen, false, 'only a probe confirming the post-timeout response may restore input');
    assert.equal(localRecovery.status, '远端曾确认恢复，输入可尝试');
    await page.keyboard.press('d');
    const localResumed = await page.evaluate(() => fixtureSockets[0].sent);
    assert.ok(localResumed.some((message, index) => index > localAckIndex && message.inputEvent?.kind === 3 && message.inputEvent.pressed),
      'new input must follow the local-timeout recovery ACK');
    // Replaying a nonce is rejected; a definite WSS close remains fatal as well.
    await page.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 10, { body: 'desktopProbe', desktopProbe: { nonce: 8 } }));
    await page.waitForFunction(() => endedCount === 1);
    await page.close();
    const closePage = await startSession(browser, origin);
    await closePage.evaluate(() => fixtureSockets[0].onclose(new CloseEvent('close')));
    await closePage.waitForFunction(() => endedCount === 1);
    await closePage.close();
    const regressed = await startSession(browser, origin);
    await regressed.evaluate(() => {
      fixtureSend(fixtureSockets[0], 'control', 2, { body: 'desktopProbe', desktopProbe: { nonce: 1 } });
      fixtureSend(fixtureSockets[0], 'control', 3, { body: 'desktopProbe', desktopProbe: { nonce: 2, acceptedResponseNonce: 1 } });
      fixtureSend(fixtureSockets[0], 'control', 4, { body: 'desktopProbe', desktopProbe: { nonce: 3 } });
    });
    await regressed.waitForFunction(() => endedCount === 1);
    await regressed.close();
    const impossible = await startSession(browser, origin);
    await impossible.evaluate(() => fixtureSend(fixtureSockets[0], 'control', 2, { body: 'desktopProbe', desktopProbe: { nonce: 1, acceptedResponseNonce: 1 } }));
    await impossible.waitForFunction(() => endedCount === 1);
    await impossible.close();
    if (contractFailures.length)
      throw new AggregateError(contractFailures.map(message => new Error(message)), 'Desktop probe regression checks failed');
    console.log('PASS: 2s probes preserve held key/drag; 4s silence releases input without closing WSS; stale healthy probes do not thaw input before the Agent confirms a post-freeze ACK; duplicate, regressed or impossible confirmations and definite WSS close end the session');
  } finally {
    await browser?.close();
    await vite.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
