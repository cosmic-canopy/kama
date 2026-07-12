// std::net::web JS glue — the bodies of kama_net_web.h, linked into the wasm app by emcc (--js-library).
// Kama imports these; the browser/Node provides WebSocket. Pull model: onmessage ENQUEUES inbound bytes,
// kama's non-blocking recv DEQUEUES — so no callbacks into wasm, no Asyncify, no `expose`.
//
// Handles are indices into KamaNet.socks (< 0 = error). recv returns >=0 bytes, -1 empty-but-open
// (WouldBlock), -2 closed (EOF). Works under Node (global WebSocket, v22+) and browsers alike.

mergeInto(LibraryManager.library, {
  $KamaNet: { socks: [] },

  kama_ws_connect__deps: ['$KamaNet'],
  kama_ws_connect: function(urlPtr) {
    var WS = (typeof globalThis !== 'undefined' && globalThis.WebSocket) ? globalThis.WebSocket
           : (typeof WebSocket !== 'undefined' ? WebSocket : null);
    if (!WS) return -1;
    var url = UTF8ToString(urlPtr);
    var s = { ws: null, queue: [], state: 0 };   // 0 Connecting
    try {
      var ws = new WS(url);
      ws.binaryType = 'arraybuffer';
      ws.onopen    = function()   { s.state = 1; };   // Open
      ws.onclose   = function()   { s.state = 3; };   // Closed
      ws.onerror   = function()   { s.state = 3; };
      ws.onmessage = function(ev) {
        var d = ev.data;
        if (d instanceof ArrayBuffer)      s.queue.push(new Uint8Array(d));
        else if (ArrayBuffer.isView(d))    s.queue.push(new Uint8Array(d.buffer, d.byteOffset, d.byteLength));
        else if (typeof d === 'string' && typeof TextEncoder !== 'undefined')
                                           s.queue.push(new TextEncoder().encode(d));
      };
      s.ws = ws;
    } catch (e) { return -1; }
    KamaNet.socks.push(s);
    return KamaNet.socks.length - 1;
  },

  kama_ws_state__deps: ['$KamaNet'],
  kama_ws_state: function(h) {
    var s = KamaNet.socks[h];
    return s ? s.state : 3;
  },

  kama_ws_send__deps: ['$KamaNet'],
  kama_ws_send: function(h, buf, n) {
    var s = KamaNet.socks[h];
    if (!s || !s.ws || s.state !== 1) return -1;
    var bytes = HEAPU8.slice(buf, buf + n);   // copy out of the heap (send is async)
    try { s.ws.send(bytes); } catch (e) { return -1; }
    return 0;
  },

  kama_ws_recv__deps: ['$KamaNet'],
  kama_ws_recv: function(h, buf, n) {
    var s = KamaNet.socks[h];
    if (!s) return -2;
    if (s.queue.length === 0) return (s.state === 3) ? -2 : -1;
    var msg  = s.queue[0];
    var take = (msg.length < n) ? msg.length : n;
    HEAPU8.set(msg.subarray(0, take), buf);
    if (take >= msg.length) s.queue.shift();
    else                    s.queue[0] = msg.subarray(take);
    return take;
  },

  kama_ws_close__deps: ['$KamaNet'],
  kama_ws_close: function(h) {
    var s = KamaNet.socks[h];
    if (s) { if (s.ws) { try { s.ws.close(); } catch (e) {} } KamaNet.socks[h] = null; }
  },

  // ---- WebTransport datagrams (unreliable, unordered — the UDP-equivalent on the web) ----------------
  // connect(url) opens a WebTransport session; state 0 Connecting, 1 Connected (ready), 3 Closed. A background
  // reader pump enqueues inbound datagrams; recv dequeues (-1 empty, -2 closed). For local testing against a
  // self-signed HTTP/3 server, window.__wtCertHash (a Uint8Array SHA-256 of the cert) enables the connection.
  kama_wt_connect__deps: ['$KamaNet'],
  kama_wt_connect: function(urlPtr) {
    var WT = (typeof globalThis !== 'undefined' && globalThis.WebTransport) ? globalThis.WebTransport
           : (typeof WebTransport !== 'undefined' ? WebTransport : null);
    if (!WT) return -1;
    var url = UTF8ToString(urlPtr);
    var s = { wt: null, queue: [], state: 0, writer: null };
    try {
      var opts = (typeof window !== 'undefined' && window.__wtCertHash)
               ? { serverCertificateHashes: [{ algorithm: 'sha-256', value: window.__wtCertHash }] } : undefined;
      var wt = opts ? new WT(url, opts) : new WT(url);
      s.wt = wt;
      wt.ready.then(function() {
        s.state = 1;
        s.writer = wt.datagrams.writable.getWriter();
        var reader = wt.datagrams.readable.getReader();
        (function pump() {
          reader.read().then(function(r) {
            if (r.done) { s.state = 3; return; }
            if (r.value) s.queue.push(new Uint8Array(r.value));
            pump();
          }).catch(function() { s.state = 3; });
        })();
      }).catch(function() { s.state = 3; });
      wt.closed.then(function() { s.state = 3; }).catch(function() { s.state = 3; });
    } catch (e) { return -1; }
    KamaNet.socks.push(s);
    return KamaNet.socks.length - 1;
  },

  kama_wt_state__deps: ['$KamaNet'],
  kama_wt_state: function(h) { var s = KamaNet.socks[h]; return s ? s.state : 3; },

  kama_wt_send__deps: ['$KamaNet'],
  kama_wt_send: function(h, buf, n) {
    var s = KamaNet.socks[h];
    if (!s || !s.writer || s.state !== 1) return -1;
    var bytes = HEAPU8.slice(buf, buf + n);
    try { s.writer.write(bytes); } catch (e) { return -1; }
    return 0;
  },

  kama_wt_recv__deps: ['$KamaNet'],
  kama_wt_recv: function(h, buf, n) {
    var s = KamaNet.socks[h];
    if (!s) return -2;
    if (s.queue.length === 0) return (s.state === 3) ? -2 : -1;
    var msg  = s.queue[0];
    var take = (msg.length < n) ? msg.length : n;
    HEAPU8.set(msg.subarray(0, take), buf);
    if (take >= msg.length) s.queue.shift();
    else                    s.queue[0] = msg.subarray(take);
    return take;
  },

  kama_wt_close__deps: ['$KamaNet'],
  kama_wt_close: function(h) {
    var s = KamaNet.socks[h];
    if (s) { if (s.wt) { try { s.wt.close(); } catch (e) {} } KamaNet.socks[h] = null; }
  },
});
