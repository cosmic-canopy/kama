#ifndef KAMA_NET_WEB_H
#define KAMA_NET_WEB_H

// Browser/Node WebSocket (and, later, WebTransport) bridge for the wasm target. The bodies live in
// kama_net_web.js, linked by emcc via `--js-library` (the driver adds it when a program externs this
// header). Kama IMPORTS these — same direction as kama_os.h — so no wasm exports / `expose` / Asyncify.
//
// Handles are int32 indices into a JS-side table; < 0 == error. Non-blocking by construction: browser
// events enqueue inbound messages, and recv DEQUEUES — returning -1 when the queue is empty but the socket
// is open (WouldBlock), or -2 when it has closed (EOF). State ordinals match the WsState enum:
// 0 Connecting, 1 Open, 2 Closing, 3 Closed.

#include <stdint.h>
#include <stddef.h>

extern int32_t kama_ws_connect(const char* url);              // -> handle, or -1
extern int32_t kama_ws_state(int32_t h);                      // 0..3 (see above)
extern int32_t kama_ws_send(int32_t h, const uint8_t* buf, size_t n);   // one binary message; 0 ok, -1 err
extern int32_t kama_ws_recv(int32_t h, uint8_t* buf, size_t n);         // >=0 bytes, -1 empty, -2 closed
extern void    kama_ws_close(int32_t h);

#endif  // KAMA_NET_WEB_H
