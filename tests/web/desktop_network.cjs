const assert = require('node:assert/strict');
const http = require('node:http');
const net = require('node:net');
const { once } = require('node:events');
const { desktopNetwork } = require('./desktop_network.fixture.cjs');

async function main() {
  const sockets = new Set();
  const echo = net.createServer(socket => {
    sockets.add(socket);
    socket.once('close', () => sockets.delete(socket));
    socket.pipe(socket);
  });
  await new Promise(resolve => echo.listen(0, '127.0.0.1', resolve));
  let proxy, client;
  try {
    const host = `localhost:${echo.address().port}`;
    proxy = await desktopNetwork(`https://${host}`);
    const address = new URL(proxy.address);
    const request = http.request({ hostname: address.hostname, port: address.port, method: 'CONNECT', path: host });
    const connected = once(request, 'connect');
    request.end();
    const [response, socket] = await connected;
    assert.equal(response.statusCode, 200);
    client = socket;
    proxy.shape(4096, 80);
    const payload = Buffer.alloc(1024, 0x5a);
    const parts = [];
    const started = performance.now();
    const received = new Promise((resolve, reject) => {
      socket.on('error', reject);
      socket.on('data', data => {
        parts.push(data);
        if (parts.reduce((sum, part) => sum + part.length, 0) === payload.length) resolve();
      });
    });
    socket.write(payload);
    await received;
    const elapsed = performance.now() - started;
    assert.deepEqual(Buffer.concat(parts), payload);
    assert.ok(elapsed >= 300, `uplink bypassed rate limit: ${elapsed}ms`);
    assert.equal(proxy.metrics().shapedBytes, payload.length);
    assert.ok(proxy.metrics().delayedMilliseconds >= 330);
    // A delayed chunk must not prevent explicit shutdown or keep timers alive.
    proxy.shape(1, 10000);
    socket.write(payload);
    await new Promise(resolve => setTimeout(resolve, 50));
    console.log(JSON.stringify({ passed: true, bytes: payload.length, elapsed_ms: elapsed,
      scope: 'opaque TCP shaping fixture integrity and pacing, not TLS or desktop acceptance' }));
  } finally {
    client?.destroy();
    await proxy?.close();
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => echo.close(resolve));
  }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
