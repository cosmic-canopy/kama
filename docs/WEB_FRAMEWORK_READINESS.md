# kama web-framework readiness

What kama gives a **web server or framework**: an HTTP server with routing, middleware and JSON, and an event loop
that multiplexes many connections over few threads. Status: ✅ have · 🟡 partial · ❌ missing. Checked against
`0.9.668` (2026-10-18).

**HTTP itself is not std's.** std carries the transports and the building blocks. HTTP/1.1, server-sent events,
the native WebSocket and the browser's `fetch`/`EventSource` are the first-party package **`@kama/http`** (KR-173), as
TLS is `@kama/tls`. TLS, regex, HTTP and ciphers are declared non-goals for std
([ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §1), the same line Rust's std draws. So a gap below is either std's, with a
row, or a package's.

Related: [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §1 (std foundations, and the rows below), §6 (concurrency), and
[ENGINE_READINESS.md](ENGINE_READINESS.md) (the shared FFI/dispatch foundation).

## What kama has

- **Sockets (`std::net`).** `TcpListener`/`TcpStream`, `UdpSocket` and Unix-domain streams and datagrams, blocking
  or non-blocking (`setNonBlocking`), closed by RAII. An accepted stream starts in its listener's mode on every OS,
  so a non-blocking listener hands an event loop non-blocking streams. In the browser, WebSocket, WebTransport and the WebRTC
  DataChannel sit behind the same `ReliableStream` / `DatagramSocket<A>` contracts.
- **A test pipe.** `MemoryStreamPair.open(capacity:)`: two connected in-memory `ReliableStream`s on every target,
  wasm included, with the capacity the test chooses. Natively, `UnixStreamPair` is the same over real sockets.
- **Listener options.** `TcpListener.bindWith(address:, options:)`: the backlog everywhere, and on Linux `balance`,
  several isolates each accepting on their own listener for one port while the kernel spreads connections across them.
- **Readiness (`std::net::Poller`).** It wraps `poll(2)` on Linux and macOS and `select()` on Windows (WSAPoll
  mis-reports a connecting socket). `add`, `wait(timeoutMs)`, and `readyFor(fd)` with `{readable, writable}`. It is the
  substrate a loop is built on.
- **DNS.** `resolve`/`resolveOne` over `getaddrinfo`, IPv4 and IPv6. It blocks; a lookup a loop can poll is
  KR-202.
- **Concurrency.** Isolates (`spawn`), ownership-moving channels, `scope`, `Atomic<T>` and `parallel_for`, native
  (OS threads) and wasm (Web Workers). A `TcpStream` is `Sendable`, so an accepted connection can move to a worker.
- **Time.** `std::time`: a monotonic `Instant`/`Duration`, sleep, and the wall clock.
- **Bytes and text.** `string`, `DynamicArray<uint8>`, views; `std::encoding` (base64 with the URL-safe pair, hex,
  UTF-8); `std::fmt` formatting and `${…}` interpolation; tagged strings (`html"…"` escapes its holes, `sql"…"` sends
  its values out of band).
- **Hashing.** `std::digest`: SHA-1, SHA-256, SHA-512, HMAC and PBKDF2 (enough for `Sec-WebSocket-Accept` and signed
  cookies), and `constantTimeEquals` to check a tag, a token or a cookie against the expected one without leaking
  through timing where they first differ.
- **Serialization.** `@generate(Serializable, Deserializable)` with a JSON back end, and `JsonValue` for a document
  with no schema.
- **Files, processes, errors.** `std::fs` (with `seek`, `readAt`, `writeAt`), `std::process`, `std::io` streams,
  `Result`/`Optional`.
- **Dispatch.** Contracts and virtual methods, so a handler contract with many implementations (middleware, routes)
  works; `fnptr` and `BindableFunctionPtr` register them.
- **Shared state.** `Shared<T>` handles to one value cannot free its storage under each other's borrows: a conflicting
  access traps, as Swift's exclusivity enforcement does (SPEC *Two handles to one value…*). A connection table or a
  router held by many handlers is safe to mutate from a callback.

## What std still owes a server (each a row)

| Need | Status | Row |
|---|---|---|
| A `Poller` wakeup another isolate can trigger | ❌ workaround: a stream pair as a wake channel | KR-201 |
| A DNS lookup a loop can poll, with a deadline | ❌ `resolve` blocks | KR-202 |
| Percent-encoding and a URL type | ❌ ruled for std (strict RFC 3986, plus form encoding); not yet built | KR-204 |
| Running out of descriptors: a kind, and the limit read and raised | ❌ ruled: `IoErrorKind::TooManyOpenFiles` and an explicit POSIX `raiseOpenFileLimit()`; not yet built | KR-210 |
| Compression for `Content-Encoding` and permessage-deflate | ❌ ruled for std: deflate/zlib/gzip both ways, decoders with a required limit; zstd and brotli later | KR-5, KR-212, KR-213 |
| `sendfile` | ❌ file responses copy through user space | KR-207 |
| Coverage-guided fuzzing | ❌ design first | KR-205 |
| A non-blocking `File`; a pipe in the `Poller` on Windows | ❌ | KR-7 |
| An event-loop scheduler and a timer wheel | ❌ libraries over `Poller` and `std::time` | KR-3 |
| Capturing closures | 🟡 sized, not scheduled: a handler object's fields hold the captures | §1 |

## What the package builds

HTTP/1.1 parsing and serving, keep-alive, chunked and streamed bodies with a per-route limit, routing and
middleware, server-sent events, the native WebSocket (RFC 6455, on std's SHA-1 and base64), the browser's `fetch`
and `EventSource`, and HTTPS through `@kama/tls`. Form and multipart body codecs belong there too (percent-encoding
waits on KR-204), and regex is a package of its own.

## Native vs wasm

A server is a native binary. A browser cannot bind a listening socket, so wasm is the client side: WebSocket,
WebTransport and `fetch` to the server. The `ReliableStream` and `DatagramSocket<A>` contracts let one codebase
serve both.
