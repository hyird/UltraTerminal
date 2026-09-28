const { chromium } = require('playwright');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs/promises');
const path = require('node:path');
const http = require('node:http');

async function main() {
  const [encoder, fixtureDirectory, executablePath] = process.argv.slice(2);
  if (!encoder || !fixtureDirectory || !executablePath) {
    throw new Error('Usage: node software_video.cjs <encoder-test.exe> <build-fixture-directory> <browser.exe>');
  }
  const encoded = spawnSync(encoder, [process.argv[5] === '--automatic' ? '--automatic-fixtures' : '--fixtures', fixtureDirectory], { encoding: 'utf8', timeout: 30000, windowsHide: true });
  if (encoded.error || encoded.status !== 0) {
    throw new Error(`Fixture encoding failed: ${encoded.error || encoded.stderr}`);
  }
  const manifest = JSON.parse(await fs.readFile(path.join(fixtureDirectory, 'manifest.json'), 'utf8'));
  const files = new Map(await Promise.all(manifest.map(async entry => [
    `/${entry.file}`, await fs.readFile(path.join(fixtureDirectory, entry.file))
  ])));
  const server = http.createServer((request, response) => {
    if (request.url === '/') {
      response.setHeader('Content-Type', 'text/html');
      response.end('<!doctype html><title>Ultraterimal codec verification</title>');
    } else if (files.has(request.url)) {
      response.setHeader('Content-Type', 'application/octet-stream');
      response.end(files.get(request.url));
    } else {
      response.writeHead(404).end();
    }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await chromium.launch({ executablePath, headless: true });
    const page = await browser.newPage();
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    const results = await page.evaluate(async entries => {
      if (!globalThis.VideoDecoder) throw new Error('WebCodecs VideoDecoder is unavailable');
      const results = [];
      for (const width of [320, 2560]) {
        const frames = entries.filter(entry => entry.width === width);
        const configuration = { codec: frames[0].codec, optimizeForLatency: true, hardwareAcceleration: 'prefer-software' };
        if (!(await VideoDecoder.isConfigSupported(configuration)).supported) {
          throw new Error(`Browser does not support ${configuration.codec}`);
        }
        // Decode all frames, then independently recover from the requested final keyframe.
        for (const selected of [frames, frames.slice(-1)]) {
          let failure;
          let decoded = 0;
          const decoder = new VideoDecoder({
            error(error) { failure = error; },
            output(frame) {
              try {
                const expected = selected[decoded++];
                if (!expected || frame.timestamp !== expected.timestamp || frame.displayWidth !== expected.width || frame.displayHeight !== expected.height) {
                  throw new Error('Decoded dimensions, timestamp, or output order differ');
                }
                const canvas = new OffscreenCanvas(frame.displayWidth, frame.displayHeight);
                const context = canvas.getContext('2d', { willReadFrequently: true });
                context.drawImage(frame, 0, 0);
                const colors = [[0,0,0], [255,255,255], [255,0,0], [0,255,0], expected.changed ? [128,128,128] : [0,0,255], [128,128,128]];
                for (let bar = 0; bar < colors.length; ++bar) {
                  const sample = context.getImageData(Math.floor((2 * bar + 1) * expected.width / 12), Math.floor(expected.height / 2), 1, 1).data;
                  if (colors[bar].some((value, channel) => Math.abs(value - sample[channel]) > 25)) {
                    throw new Error(`Browser RGB mismatch: ${expected.file}, codec=${expected.codec}, bar ${bar}, received ${Array.from(sample)}, color=${JSON.stringify(frame.colorSpace.toJSON())}`);
                  }
                }
              } catch (error) { failure = error; } finally { frame.close(); }
            }
          });
          try {
            decoder.configure(configuration); // No description: AVC chunks are Annex B with in-band SPS/PPS.
            for (const entry of selected) {
              const data = await (await fetch(`/${entry.file}`)).arrayBuffer();
              decoder.decode(new EncodedVideoChunk({ type: entry.key ? 'key' : 'delta', timestamp: entry.timestamp, data }));
            }
            await decoder.flush();
            if (failure) throw failure;
            if (decoded !== selected.length) throw new Error('Decoder did not output every frame');
            results.push({ width, height: frames[0].height, codec: configuration.codec, decoded });
          } finally {
            if (decoder.state !== 'closed') decoder.close();
          }
        }
      }
      return results;
    }, manifest);
    console.log(JSON.stringify({ browser: browser.version(), results }, null, 2));
  } finally {
    if (browser) await browser.close();
    await new Promise(resolve => server.close(resolve));
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
