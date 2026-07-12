// Minimal dependency-free WebSocket echo server for the wasm net test leg (node built-ins only: http +
// crypto). Does the RFC 6455 handshake, unmasks client frames, and echoes the payload back as an unmasked
// binary frame. Enough for tests/net_ws_loopback (small binary messages). Usage: node ws_echo.js [port].
'use strict';
const http = require('http');
const crypto = require('crypto');
const PORT = parseInt(process.argv[2] || '47670', 10);
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

const server = http.createServer();
server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'] || '';
  const accept = crypto.createHash('sha1').update(key + GUID).digest('base64');
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\n' +
    'Connection: Upgrade\r\n' +
    'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n'
  );
  socket.on('data', (buf) => {
    let off = 0;
    while (off + 2 <= buf.length) {
      const b0 = buf[off], b1 = buf[off + 1];
      const opcode = b0 & 0x0f;
      const masked = (b1 & 0x80) !== 0;
      let len = b1 & 0x7f, p = off + 2;
      if (len === 126) { len = buf.readUInt16BE(p); p += 2; }
      else if (len === 127) { len = Number(buf.readBigUInt64BE(p)); p += 8; }
      let mask = null;
      if (masked) { mask = buf.slice(p, p + 4); p += 4; }
      if (p + len > buf.length) break;                 // partial frame; wait for more
      const payload = Buffer.alloc(len);
      for (let i = 0; i < len; i++) payload[i] = buf[p + i] ^ (masked ? mask[i & 3] : 0);
      off = p + len;
      if (opcode === 0x8) { socket.end(); return; }    // close frame
      const out = Buffer.alloc(2 + payload.length);    // echo as a binary frame (payload < 126 for the test)
      out[0] = 0x82; out[1] = payload.length;
      payload.copy(out, 2);
      socket.write(out);
    }
  });
  socket.on('error', () => {});
});
server.listen(PORT, '127.0.0.1', () => { console.error('ws_echo listening on 127.0.0.1:' + PORT); });
