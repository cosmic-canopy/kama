// Minimal WebSocket signaling relay for the WebRTC test: pairs the first two clients that connect and
// forwards each binary message from one to the other (used to shuttle WebRTC SDP offer/answer between two
// peers). Node built-ins only (http + crypto). Handles frames up to 64 KB (SDP is ~1-2 KB). Usage:
//   node sig_relay.js [port]
'use strict';
const http = require('http');
const crypto = require('crypto');
const PORT = parseInt(process.argv[2] || '47690', 10);
const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

// Encode a server->client binary frame (unmasked). Supports 7-bit and 16-bit payload lengths.
function frame(payload) {
  const len = payload.length;
  let head;
  if (len < 126) { head = Buffer.from([0x82, len]); }
  else { head = Buffer.alloc(4); head[0] = 0x82; head[1] = 126; head.writeUInt16BE(len, 2); }
  return Buffer.concat([head, payload]);
}

const peers = [];
const server = http.createServer();
server.on('upgrade', (req, socket) => {
  const key = req.headers['sec-websocket-key'] || '';
  const accept = crypto.createHash('sha1').update(key + GUID).digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
               'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
               'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  peers.push(socket);
  let buf = Buffer.alloc(0);
  socket.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
      if (buf.length < 2) break;
      const b1 = buf[1], masked = (b1 & 0x80) !== 0;
      let len = b1 & 0x7f, p = 2;
      if (len === 126) { if (buf.length < 4) break; len = buf.readUInt16BE(2); p = 4; }
      else if (len === 127) { if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); p = 10; }
      let mask = null;
      if (masked) { if (buf.length < p + 4) break; mask = buf.slice(p, p + 4); p += 4; }
      if (buf.length < p + len) break;
      const opcode = buf[0] & 0x0f;
      const payload = Buffer.alloc(len);
      for (let i = 0; i < len; i++) payload[i] = buf[p + i] ^ (masked ? mask[i & 3] : 0);
      buf = buf.slice(p + len);
      if (opcode === 0x8) { socket.end(); return; }
      const other = peers.find((s) => s !== socket && !s.destroyed);   // relay to the other peer
      if (other) other.write(frame(payload));
    }
  });
  socket.on('error', () => {});
  socket.on('close', () => { const i = peers.indexOf(socket); if (i >= 0) peers.splice(i, 1); });
});
server.listen(PORT, '127.0.0.1', () => { console.error('sig_relay listening on 127.0.0.1:' + PORT); });
