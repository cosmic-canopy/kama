# kama web-framework readiness gap analysis

This document assesses what kama needs to build a **Node.js-like web/server framework** — an HTTP server with
routing, middleware, JSON, and (the crux) an **async event loop** that multiplexes thousands of connections on a
small number of threads. Status: ✅ have · 🟡 partial · ❌ missing.

The key distinction throughout: **language gaps** (the compiler/runtime must change) vs **library gaps** (pure
kama work, unblocked today). A web framework is *mostly* library work — the HTTP parser, router, and middleware
chain are ordinary kama. The genuine **language**-level questions are the **async/concurrency model** and
**capturing closures**; almost everything else is library or FFI reach.

Related: [ROADMAP.md](ROADMAP.md) §1 (net/std foundation), §6 (concurrency — the isolate model), and
[ENGINE_READINESS.md](ENGINE_READINESS.md) (the shared FFI/dispatch foundation).

## What kama already has (the foundation)

- **Native sockets (`std::net`)** — `TcpListener` (bind/listen/accept) + `TcpStream` (connect/read/writeAll),
  `UdpSocket`, blocking **and** non-blocking modes (`setNonBlocking`), RAII auto-close. Web transports
  (WebSocket/WebTransport) exist for the *browser* side, unified with native via the `ReliableStream` /
  `DatagramSocket` contracts.
- **Readiness multiplexing** — `std::net::Poller` wraps `poll(2)` / `WSAPoll`: register many sockets, `wait(timeoutMs)`,
  query per-fd `{readable, writable}`. This is the substrate an event loop is built *on* (the loop itself is missing).
- **Contracts + virtual dispatch** — first-class interfaces (fat pointers) and inheritance vtables, so a
  `type contract HttpHandler { fn Result<Response, Error> handle(ref Request); }` with many implementations
  (middleware, route handlers) works today. This is the plugin/middleware backbone.
- **Intrinsic serialization + JSON** — `@generate` Serialize/Deserialize with a `std::serialization::json` backend
  (`encode`/`decode::<T>`). JSON request/response bodies work out of the box.
- **UTF-8 strings + growable bytes** — `string` (find/split/substring/trim/replace/startsWith/…, byte index `s[i]`,
  `.chars()` codepoints), `DynamicArray<uint8>` as a growable buffer, and `View<T>` for non-owning slices — enough
  to hand-parse HTTP/1.1 (the `examples/httpd` static server proves the composition).
- **Callables** — free `fnptr` + `BindableFunctionPtr<Sig>` (binds a receiver object) cover handler registration.
- **The error model + files** — `Result`/`Optional` (no exceptions), `std::fs` (read/write/stat/readDir).

A dynamic JSON HTTP API is **buildable today** on a single thread with a hand-written `Poller` loop. What's
missing to make it *Node-like* (ergonomic async, timeouts, scale) is below.

---

## Tier 0 — The hard blockers (what makes it "Node-like" vs a toy)

| Feature | Kind | Status | Why a server needs it | Effort |
|---|---|---|---|---|
| **Async task model / event loop** | **language + runtime** | ❌ missing — only manual `Poller::wait()` polling exists; no scheduler, futures, or `async/await` | Node *is* an event loop. Without a task/scheduler abstraction, every handler that does I/O becomes a hand-rolled state machine threaded through the poll loop — unusable at framework scale. ROADMAP §6 designs a **shared-nothing isolate + channel** model (block on a channel, scheduler runs other ready work — Go/Erlang style, deliberately **not** `async/await` function-coloring). Designed, **not shipped**. **The single biggest gap.** | **XL** |
| **Timers / monotonic clock (`std::time`)** | library (+thin FFI) | ❌ missing — no clock, `sleep`, or `setTimeout` | Connection/request timeouts, keep-alive expiry, rate limiting, session TTL, retry backoff — none are possible without a timer wheel driven off a monotonic clock and the poll timeout. Thin FFI to `clock_gettime`; the timer wheel is kama. | **M** |
| **HTTP/1.1 (+ WebSocket upgrade) parser & server** | library | ❌ missing — only a ~200-line static-file `examples/httpd` proof | Request/response types, header parsing, chunked/`Content-Length` bodies, keep-alive, status constants. **Pure kama library work — not blocked by the language**; it just doesn't exist yet. | **M–L** |
| **TLS / HTTPS** | library (FFI) | ❌ missing | Public-facing servers need TLS. Realistically an FFI binding to a C TLS stack (OpenSSL/BoringSSL/mbedTLS) behind a `ReliableStream`-shaped wrapper, so handlers are transport-agnostic. | **L** |

## Tier 1 — Core ergonomics & reach (needed soon after Tier 0)

| Feature | Kind | Status | Why | Effort |
|---|---|---|---|---|
| **Capturing closures** | **language** | 🟡 partial — free `fnptr` + `BindableFunctionPtr` only; no inline lambda that captures locals | Route handlers and middleware want to capture request context / config inline. Today you thread context through a handler *object* (a `type resource` whose fields hold the captures) and register `BindableFunctionPtr` methods — workable but verbose. Closures under the RAII/move model are a designed Tier-3 ergonomic (ROADMAP §3), not a blocker. | **M–L** |
| **DNS resolution** | library (FFI) | ❌ missing — numeric hosts only (`"127.0.0.1:8080"`) | An HTTP *client* (proxies, upstreams, webhooks) needs name resolution; a bare listener does not. Thin `getaddrinfo` FFI. | **S–M** |
| **Concurrency / threads** | **language + runtime** | ❌ missing — single-threaded; the isolate/channel/`Atomic<T>` model is designed (ROADMAP §6), not shipped | A single event-loop thread saturates one core. Node scales with a worker/cluster model; kama's planned shared-nothing isolates (mapping 1:1 to OS threads / WASM workers) are the equivalent. Reuses `give` (move) for channel transfer. | **XL** |
| **`std::io` / `std::process`** | library | 🟡 partial — `std::fs` shipped; stdout/stderr, argv, env, exit are ad-hoc C FFI in examples | Structured logging, config from env/argv, graceful exit codes. Thin kama wrappers over the C calls already used. | **S** |

## Tier 2 — Framework polish (library / FFI; not language gaps)

| Feature | Status | Why | Effort |
|---|---|---|---|
| **Body codecs**: `x-www-form-urlencoded`, `multipart/form-data` | ❌ missing (JSON ✅) | HTML form posts + file uploads. Pure kama parsers over the byte buffer. | **M** |
| **Regex** | ❌ missing | Route patterns, validation. A byte-oriented engine in kama, or a `<regex.h>`/PCRE FFI. | **M–L** |
| **Crypto / hashing / base64** | ❌ missing (internal FNV-1a is not exposed) | Cookie signing, JWT, `Sec-WebSocket-Accept` (SHA-1), Basic-auth (base64). Library + an OpenSSL FFI for real crypto. | **M** |
| **Middleware / routing DSL** | ✅ ready to build — contracts + virtual dispatch | `type contract HttpHandler` + a chain/dispatcher is ordinary kama; the language support is done. | done (lang) |
| **Connection pooling / keep-alive / streaming bodies** | ❌ missing | Throughput + backpressure. Rides on the event loop + timers above. | **M** |

## Native vs WASM

A Node.js replacement is a **native binary** (kama → C → clang). `TcpListener` + `Poller` are native; a browser
cannot bind a listening socket, so the WASM path is for **clients** (WebSocket/WebTransport to your server), not
for hosting the server. The `ReliableStream`/`DatagramSocket` contracts already let one codebase target both
sides.

---

## Recommended sequence

1. **`std::time`** (monotonic clock + timer wheel) — small, and a precondition for every timeout in the loop.
2. **The async task model** (ROADMAP §6 isolates/channels + a single-thread scheduler over `Poller`) — the
   defining feature; everything ergonomic hangs off it. Land the **single-threaded** loop + scheduler first,
   before multi-core isolates.
3. **HTTP/1.1 library** (parser + request/response + keep-alive) on top of the loop — pure kama, unblocked.
4. **DNS** + **`std::io`/`std::process`** wrappers — fill the client + operational gaps.
5. **TLS** (FFI) and **capturing closures** (language ergonomics) — production-facing polish.
6. **Multi-core isolates**, then the Tier-2 codecs/regex/crypto library reach.

**Bottom line:** the framework is **not blocked by the type system** — contracts, JSON, sockets, and byte
handling are all here, and the HTTP layer is ordinary library work. The two real **language/runtime**
investments are the **async task/event-loop model** (the Node-defining feature) and, secondarily, **capturing
closures** and **threading** — all three already have a design direction in ROADMAP §6. Everything else is
library and FFI reach that can proceed today.
