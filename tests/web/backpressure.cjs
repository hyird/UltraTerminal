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
  const browser = await chromium.launch({ executablePath: process.argv[2], headless: true });
  try {
    const page = await browser.newPage();
    await page.route('**/backpressure-fixture', route => route.fulfill({ contentType: 'text/html', body: '<canvas></canvas><canvas class="cursor-overlay"></canvas>' }));
    await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/backpressure-fixture`);
    const result = await page.evaluate(async () => {
      const { DesktopSession } = await import('/src/sessions/desktop/desktop.session.ts');
      const { ultraterimal } = await import('/src/generated/session.js');
      let decoder;
      // Deterministically pause decoder output while the production receive
      // path admits the complete window already sent by the Agent.
      window.VideoDecoder = class {
        static async isConfigSupported() { return { supported: true }; }
        state = 'configured';
        frames = [];
        constructor(callbacks) { this.callbacks = callbacks; decoder = this; }
        configure() {}
        decode(chunk) { this.frames.push(chunk); }
        close() { this.state = 'closed'; }
        flushFrames(canvas) {
          for (const chunk of this.frames.splice(0)) {
            this.callbacks.output(new VideoFrame(canvas, { timestamp: chunk.timestamp }));
          }
        }
      };
      const canvas = document.querySelector('canvas');
      let ended = false;
      const session = new DesktopSession({ id: 'pressure', writeControl: false, channels: [] }, canvas,
        document.querySelector('.cursor-overlay'), () => {}, () => { ended = true; });
      const channel = { purpose: 3, ready: true, id: 'video', received: 1 };
      let sequence = 1;
      const receive = body => session.receive(channel, ultraterimal.wire.Envelope.create({
        protocolVersion: 1, sessionId: 'pressure', channelId: 'video', sequence: ++sequence, generation: 1, ...body,
      }));
      const send = id => receive({ frameId: id, videoFrame: { timestampMicroseconds: id, keyFrame: id === 1, data: new Uint8Array([0, 0, 0, 1, 0x65]) } });
      try {
        await receive({ videoConfig: { codec: 'avc1.42e01f', width: 32, height: 32 } });
        for (let id = 1; id <= 16; ++id) await send(id);
        const queued = decoder.frames.length;
        decoder.flushFrames(canvas);
        const resumedAt = canvas.dataset.frame;
        await send(17);
        decoder.flushFrames(canvas);
        const continuedAt = canvas.dataset.frame;
        let nextFrame = 18;
        let peakPending = 0;
        for (let cycle = 0; cycle < 64; ++cycle) {
          for (let count = 0; count < 16; ++count) await send(nextFrame++);
          peakPending = Math.max(peakPending, session.pending.size);
          if (decoder.frames.length !== 16 || session.pending.size !== 16)
            throw new Error('Repeated stall did not retain exactly one bounded window');
          decoder.flushFrames(canvas);
          if (Number(canvas.dataset.frame) !== nextFrame - 1 || session.pending.size !== 0)
            throw new Error('Repeated recovery retained pending frames or failed to display the latest frame');
          if (ended || canvas.dataset.generation !== '1')
            throw new Error('Repeated recovery ended the session or reset its baseline');
        }
        const recoveredAt = canvas.dataset.frame;
        for (let count = 0; count < 16; ++count) await send(nextFrame++);
        let overflowRejected = false;
        try { await send(nextFrame); } catch { overflowRejected = true; }
        return { queued, resumedAt, continuedAt, recoveredAt, peakPending, overflowRejected, ended, generation: canvas.dataset.generation };
      } finally {
        session.stop();
      }
    });
    assert.deepEqual(result, { queued: 16, resumedAt: '16', continuedAt: '17', recoveredAt: '1041', peakPending: 16, overflowRejected: true, ended: false, generation: '1' });
    const startup = await page.evaluate(async () => {
      const { DesktopSession } = await import('/src/sessions/desktop/desktop.session.ts');
      const { ultraterimal } = await import('/src/generated/session.js');
      const wire = ultraterimal.wire;
      const sockets = [];
      window.WebSocket = class {
        static OPEN = 1;
        readyState = 1;
        bufferedAmount = 0;
        sent = [];
        constructor() { sockets.push(this); }
        send(data) { this.sent.push(wire.Envelope.decode(data)); }
        close() { this.readyState = 3; }
        deliver(body) {
          if (!this.onmessage) throw new Error('Startup closed a legal in-flight window');
          const data = new Uint8Array(wire.Envelope.encode(body).finish());
          this.onmessage({ data: data.buffer });
        }
      };
      let decoder, releaseConfig, checking;
      const configuring = new Promise(resolve => { checking = resolve; });
      const supported = new Promise(resolve => { releaseConfig = resolve; });
      window.VideoDecoder = class {
        static async isConfigSupported() { checking(); await supported; return { supported: true }; }
        state = 'configured';
        frames = [];
        constructor(callbacks) { this.callbacks = callbacks; decoder = this; }
        configure() {}
        decode(chunk) { this.frames.push(chunk); }
        close() { this.state = 'closed'; }
        display(canvas) {
          for (const chunk of this.frames.splice(0)) this.callbacks.output(new VideoFrame(canvas, { timestamp: chunk.timestamp }));
        }
      };
      const canvas = document.querySelector('canvas');
      let ended = false;
      const session = new DesktopSession({ id: 'startup', writeControl: false, channels: [
        { id: 'input', purpose: 2, ticket: 'fixture' }, { id: 'video', purpose: 3, ticket: 'fixture' },
      ] }, canvas, document.querySelector('.cursor-overlay'), () => {}, () => { ended = true; });
      try {
        sockets.forEach(socket => socket.onopen());
        let sequence = 0;
        const video = body => sockets[1].deliver({ protocolVersion: 1, sessionId: 'startup', channelId: 'video', sequence: ++sequence, ...body });
        video({ channelReady: { purpose: 3 } });
        video({ generation: 1, videoConfig: { codec: 'avc1.42e01f', width: 32, height: 32 } });
        await configuring;
        for (let id = 1; id <= 16; ++id) video({ generation: 1, frameId: id,
          videoFrame: { timestampMicroseconds: id, keyFrame: id === 1, data: new Uint8Array([0, 0, 0, 1, 0x65]) } });
        releaseConfig();
        await session.channels[1].work;
        decoder.display(canvas);
        const beforeReady = sockets[0].sent.filter(message => message.qualityFeedback).length;
        session.setVisible(false);
        sockets[0].deliver({ protocolVersion: 1, sessionId: 'startup', channelId: 'input', sequence: 1, channelReady: { purpose: 2 } });
        await session.channels[0].work;
        session.setVisible(true);
        return { ended, frame: canvas.dataset.frame, beforeReady,
          feedback: sockets[0].sent.filter(message => message.qualityFeedback).map(message => ({
            frame: Number(message.qualityFeedback.displayedFrame), generation: Number(message.qualityFeedback.displayedGeneration),
            background: message.qualityFeedback.background,
            preference: message.qualityFeedback.preference,
          })), queued: session.channels[1].queued };
      } finally {
        releaseConfig();
        session.stop();
      }
    });
    assert.deepEqual(startup, { ended: false, frame: '16', beforeReady: 0, queued: 0, feedback: [
      { frame: 16, generation: 1, background: true, preference: 3 }, { frame: 16, generation: 1, background: false, preference: 3 },
    ] });
    console.log('PASS: complete Agent window survives decoder stall, resumes without baseline reset, and rejects overflow');
    console.log('PASS: 64 repeated stalls drain 1024 frames to the latest display with no pending frames or generation reset');
    console.log('PASS: delayed configuration retains startup frames and late control readiness replays the latest display/visibility feedback');
  } finally {
    await browser.close();
    await server.close();
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
