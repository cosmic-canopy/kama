# kama httpd

A tiny **static-file HTTP/1.1 server written in [Kama](https://kama-lang.org)** — the
`python -m http.server` spirit, as a single native binary. It's a worked example of the standard
library composing into something real: `std::net` (TCP), `std::fs` (files), and the string library,
in ~200 lines, no framework.

It's also handy: use it to preview a static site locally.

## Build & run

Assuming the `kama` compiler is on your `PATH`:

```sh
kama build httpd.kama -o httpd
./httpd                 # serves ./public on http://127.0.0.1:8080
```

Open <http://127.0.0.1:8080> — you should see the page in [`public/`](public/).

## Configuration

All optional, via environment variables:

| Variable     | Default       | Meaning                                  |
| ------------ | ------------- | ---------------------------------------- |
| `HTTPD_ROOT` | `public`      | directory to serve                       |
| `HTTPD_PORT` | `8080`        | TCP port                                 |
| `HTTPD_HOST` | `127.0.0.1`   | bind address (`0.0.0.0` = all interfaces) |

```sh
HTTPD_ROOT=./my-site HTTPD_PORT=9000 ./httpd
```

### Preview the kama-lang.org site

From a checkout of the Kama repo, one command builds the deployable `_site/`, builds this server, and
serves it (host-native if you have a runnable `kama`, else in the dev container with the port published):

```sh
./dev serve                 # http://localhost:8080
./dev serve 9000            # a different port
./dev serve 8080 site       # serve the raw source instead of the built _site/
```

Or drive it by hand against the raw source in [`site/`](../../site):

```sh
kama build httpd.kama -o httpd
HTTPD_ROOT=../../site ./httpd     # http://127.0.0.1:8080
```

## What it does

- `GET` a path → reads `<root>/<path>` and returns `200` with a `Content-Type` (by extension) and a
  correct `Content-Length`; binary files (images, wasm) are served byte-for-byte.
- `/` and any trailing `/` resolve to `index.html`.
- Missing file → `404`, a path containing `..` → `400`, a non-`GET` method → `405`.

## Scope (honest)

A dev/preview server, not a production edge. It is single-threaded and blocking, one request per
connection (`Connection: close`), reads the request in a single `recv` (fine for `GET`s), and does no
keep-alive, directory listings, query-string decoding, or URL-percent-decoding. Numeric host only.

## How it maps to the standard library

- **`std::net`** — `TcpListener::bind` / `accept`, `TcpStream::read` / `writeAll`. Each connection is a
  `TcpStream` resource that closes on drop (RAII) at the end of the `match` arm that handled it.
- **`std::fs`** — `readFile(path:)` returns `Result<List<uint8>, IoError>`; a `NotFound` becomes a 404.
- **strings** — `split` / `startsWith` / `endsWith` / `contains` to parse the request line and pick a
  MIME type; the response is assembled as a `List<uint8>` (headers as bytes + the file bytes).
- **env** — `envOr(name:, dflt:)` (prelude floor) reads `PORT`/`ROOT` config with a fallback.
- **FFI** — `puts` for the startup banner, declared inline with `extern`.

## Seeding a standalone repo

This directory is self-contained — it depends only on an installed `kama`, not on the compiler repo.
Copy it out and initialize a repo:

```sh
cp -r examples/httpd ~/kama-httpd && cd ~/kama-httpd && git init
```
