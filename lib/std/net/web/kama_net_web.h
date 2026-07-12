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

// WebTransport datagrams (unreliable, unordered — the UDP-equivalent on the web). Same handle/state/return
// conventions as the WebSocket calls above; send/recv move one datagram.
extern int32_t kama_wt_connect(const char* url);              // https:// URL -> handle, or -1
extern int32_t kama_wt_state(int32_t h);                      // 0 Connecting, 1 Connected, 3 Closed
extern int32_t kama_wt_send(int32_t h, const uint8_t* buf, size_t n);   // one datagram; 0 ok, -1 err
extern int32_t kama_wt_recv(int32_t h, uint8_t* buf, size_t n);         // >=0 bytes, -1 empty, -2 closed
extern void    kama_wt_close(int32_t h);

// WebRTC DataChannel (P2P). The app drives signaling: ship local_sdp to the peer, apply its SDP via
// set_remote. Non-trickle — local_sdp is ready only after ICE gathering (0 until then) and carries all
// candidates, so it's one string each way. dc_state: 0 Connecting, 1 Open, 3 Closed.
extern int32_t kama_rtc_create(void);                                   // -> handle, or -1
extern int32_t kama_rtc_datachannel(int32_t h, int32_t unreliable);     // offerer creates it; 1=UDP-like
extern int32_t kama_rtc_offer(int32_t h);                               // begin an offer (poll local_sdp)
extern int32_t kama_rtc_answer(int32_t h);                              // begin an answer (after set_remote)
extern int32_t kama_rtc_local_sdp(int32_t h, uint8_t* buf, size_t n);   // >0 len, 0 not ready, -2 too small
extern int32_t kama_rtc_set_remote(int32_t h, const uint8_t* buf, size_t n);  // apply peer SDP; 0 ok
extern int32_t kama_rtc_dc_state(int32_t h);                            // 0 Connecting, 1 Open, 3 Closed
extern int32_t kama_rtc_send(int32_t h, const uint8_t* buf, size_t n);  // one datagram; 0 ok, -1 err
extern int32_t kama_rtc_recv(int32_t h, uint8_t* buf, size_t n);        // >=0 bytes, -1 empty, -2 closed
extern void    kama_rtc_close(int32_t h);

#endif  // KAMA_NET_WEB_H
