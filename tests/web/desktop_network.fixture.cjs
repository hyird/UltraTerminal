const http = require('node:http');
const net = require('node:net');
const { Transform } = require('node:stream');

// Test-only CONNECT tunnel: shape encrypted TCP bytes without terminating TLS,
// changing WebSocket.send, or bypassing the fixture's certificate pin.
async function desktopNetwork(origin) {
  const target = new URL(origin);
  if (target.protocol !== 'https:' || target.hostname !== 'localhost')
    throw new Error('Desktop network fixture only accepts the local HTTPS gateway');
  const sockets = new Set();
  const shapers = new Set();
  let bytesPerSecond = 0, latency = 0, shapedBytes = 0, delayedMilliseconds = 0;
  const server = http.createServer((_, response) => response.writeHead(405).end());
  server.on('connect', (request, client, head) => {
    if (request.url !== target.host) { client.destroy(); return; }
    const upstream = net.connect(Number(target.port || 443), '127.0.0.1');
    for (const socket of [client, upstream]) {
      sockets.add(socket);
      socket.on('close', () => sockets.delete(socket));
    }
    const upload = new Transform({
      transform(chunk, _, callback) {
        const delay = bytesPerSecond ? latency + chunk.length * 1000 / bytesPerSecond : 0;
        if (!delay) { callback(null, chunk); return; }
        shapedBytes += chunk.length;
        delayedMilliseconds += delay;
        this.timer = setTimeout(() => { this.timer = undefined; callback(null, chunk); }, delay);
      },
      destroy(error, callback) { clearTimeout(this.timer); callback(error); },
    });
    shapers.add(upload);
    const close = () => { upload.destroy(); client.destroy(); upstream.destroy(); shapers.delete(upload); };
    client.on('error', close); upstream.on('error', close); upload.on('error', close);
    client.on('close', close); upstream.on('close', close);
    upstream.once('connect', () => {
      client.write('HTTP/1.1 200 Connection Established\r\n\r\n');
      if (head.length) upload.write(head);
      client.pipe(upload).pipe(upstream);
      upstream.pipe(client);
    });
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  return {
    address: `http://127.0.0.1:${server.address().port}`,
    shape(rate, delay) {
      if (!Number.isInteger(rate) || rate < 0 || !Number.isInteger(delay) || delay < 0)
        throw new Error('Invalid traffic shaping parameters');
      bytesPerSecond = rate; latency = delay;
    },
    metrics() { return { shapedBytes, delayedMilliseconds }; },
    async close() {
      for (const shaper of shapers) shaper.destroy();
      for (const socket of sockets) socket.destroy();
      await new Promise(resolve => server.close(resolve));
    },
  };
}
module.exports = { desktopNetwork };
