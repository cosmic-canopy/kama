# Server benchmark: kama fibers against Node, Go and tokio

_Measured 2026-10-27 · kama 0.9.750 · 15 rounds · the `kama-bench` container (Linux 7.0, aarch64) on an Apple M4_

This is KR-243's measurement: what a kama server written on fibers (KR-3) costs against the servers people write in
Node, Go and Rust's tokio, with a plain C event loop as the ceiling and kama's isolate-per-connection server as the
"before". The harness is `bench/server/`; how to reproduce it is at the end.

## Read this first

These hold before any number below is read.

- **One machine runs both sides.** The container sees 6 vCPUs of a libkrun VM on an Apple M4 (4 performance and 6
  efficiency cores). Every server is pinned to 3 of them (`taskset -c 0-2`) and the load client to the other 3. The
  VM's vCPUs are not pinned to a core type, so a run may land on either kind; the interleaved protocol below is what
  makes a comparison between arms meaningful anyway.
- **Loopback, not a network.** Requests cross the kernel's loopback inside one VM: no NIC, no wire latency. It measures
  what a server spends per request and how it schedules, not a network stack.
- **Where several arms meet the C arm's rate, the client is the limit.** At 100 and 1,000 connections the C loop, the
  kama fiber arm and tokio's thread-per-core arm all serve about 255,000 requests a second, so those configs say only
  that each is at least that fast. Server CPU per request is the column that still separates them there.
- **A fiber keeps more memory per idle connection than a stackless task.** A tokio future or a Node callback holds only
  its state; a fiber holds the stack pages it touched. The idle table shows it.
- **Thread-per-core does not steal work.** The load here is uniform across connections. A skewed load, a few
  connections doing most of the work, is where tokio's and Go's work stealing are strongest, and it is not measured.
- **The kama isolate arm is the "before":** an OS thread per connection, which is what kama had before fibers.

## Setup

| arm | what it is | toolchain |
|---|---|---|
| `c` | an epoll loop per core, nothing between the loop and the socket (`bench/server/c/`); the ceiling | clang 18.1.3, `-O2` |
| `kama-fibers` | an isolate per core, each accepting on its own `SO_REUSEPORT` listener, a fiber per connection (`bench/server/kama/fibers.kama`) | kama 0.9.750, `--release` |
| `kama-isolates` | an isolate (an OS thread) per connection, blocking reads and writes (`bench/server/kama/server.kama`) | kama 0.9.750, `--release` |
| `go` | `net` and `net/http`, a goroutine per connection, `GOMAXPROCS=3` | Go 1.27.3 |
| `tokio` | tokio's default work-stealing runtime, 3 workers; hyper for HTTP | Rust 1.97.1, tokio 1.54.1, hyper 1.12.0 |
| `tokio-percore` | 3 `current_thread` runtimes, each on its own `SO_REUSEPORT` listener: the shape closest to kama's | as `tokio` |
| `node` | `net` and `http`, one process | Node 22.16.0 |
| `node-cluster` | the same under `cluster`, 3 workers | Node 22.16.0 |

**Workloads.** `echo`: each connection writes 64 bytes and reads the same 64 back, over and over, one park per request.
`http`: HTTP/1.1 keep-alive `GET /` without pipelining, answered `200` with `Hello, World!`. `idle`: 10,000 connections
opened, each making one round trip, then held while the server's resident memory is read. Each measured config runs 1 s
of warm-up and 5 s of load.

**The client** is one C program for every arm (`bench/server/client.c`). It verifies every echoed byte and every HTTP
status and body; a wrong reply, an unexpected close or a failed connect counts as an error, and a run with any error is
left out of every number. **This run had none: 945 runs, 0 errors.**

**Protocol** (the house rule: measure interleaved, never sequentially). 15 rounds; in each, every arm runs every config
as fresh processes, the arm order rotated each round. The reference arm, `c`, runs twice per config, and its gap to
itself (`c#2`) is the noise floor. A difference is claimed only when it beats that floor in at least 12 of the 15 paired
rounds. Each number below is the median over the rounds; latency is the client's, from writing a request to reading its
whole reply.

## Results

Against `c` (the "vs ref" columns are the median of the per-round ratio of requests/s, and in how many of the 15 paired
rounds the arm was the faster). RSS is the server's whole process tree at its peak.

### echo, 100 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU s / 1k req | RSS MiB | vs ref: req/s ratio | faster in |
|---|---|---|---|---|---|---|---|---|
| c | 254,693 | 328 | 1180 | 1704 | 0.006 | 1.6 | — | — |
| c#2 | 254,528 | 328 | 1180 | 1638 | 0.006 | 1.6 | 0.999 | 7/15 |
| go | 242,112 | 328 | 1311 | 3408 | 0.007 | 6.8 | 0.953 | 0/15 |
| kama-fibers | 254,421 | 328 | 1049 | 1573 | 0.006 | 2.1 | 1.001 | 10/15 |
| kama-isolates | 171,226 | 492 | 1638 | 2752 | 0.013 | 3.3 | 0.672 | 0/15 |
| node | 109,085 | 885 | 1770 | 2032 | 0.006 | 56.3 | 0.433 | 0/15 |
| node-cluster | 230,301 | 360 | 1245 | 1770 | 0.008 | 209.6 | 0.909 | 0/15 |
| tokio | 240,279 | 360 | 950 | 1311 | 0.007 | 2.7 | 0.949 | 0/15 |
| tokio-percore | 255,045 | 328 | 1049 | 1573 | 0.006 | 2.7 | 1.006 | 9/15 |

### echo, 1000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU s / 1k req | RSS MiB | vs ref: req/s ratio | faster in |
|---|---|---|---|---|---|---|---|---|
| c | 256,863 | 3277 | 9962 | 12583 | 0.007 | 4.9 | — | — |
| c#2 | 257,438 | 3277 | 9962 | 12583 | 0.006 | 4.9 | 1.001 | 8/15 |
| go | 246,948 | 3670 | 8913 | 11534 | 0.007 | 14.2 | 0.960 | 0/15 |
| kama-fibers | 256,200 | 3277 | 8913 | 12059 | 0.007 | 9.2 | 0.994 | 3/15 |
| kama-isolates | 167,974 | 4981 | 15204 | 24117 | 0.014 | 18.5 | 0.652 | 0/15 |
| node | 85,345 | 11534 | 14156 | 17826 | 0.007 | 58.2 | 0.333 | 0/15 |
| node-cluster | 239,267 | 3670 | 9962 | 13107 | 0.008 | 224.5 | 0.929 | 0/15 |
| tokio | 255,936 | 2884 | 13632 | 17826 | 0.006 | 6.8 | 0.996 | 2/15 |
| tokio-percore | 258,090 | 3277 | 9437 | 12059 | 0.006 | 6.9 | 1.006 | 12/15 |

### echo, 10000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU s / 1k req | RSS MiB | vs ref: req/s ratio | faster in |
|---|---|---|---|---|---|---|---|---|
| c | 210,926 | 44040 | 88080 | 117440 | 0.008 | 40.4 | — | — |
| c#2 | 212,294 | 44040 | 92275 | 117440 | 0.008 | 40.4 | 1.005 | 9/15 |
| go | 177,885 | 54526 | 75498 | 83886 | 0.010 | 81.6 | 0.842 | 0/15 |
| kama-fibers | 214,447 | 44040 | 79692 | 117440 | 0.008 | 81.7 | 1.022 | 10/15 |
| kama-isolates | 122,257 | 65012 | 234881 | 1073742 | 0.021 | 170.6 | 0.578 | 0/15 |
| node | 80,869 | 121635 | 142606 | 142606 | 0.008 | 102.7 | 0.384 | 0/15 |
| node-cluster | 186,704 | 46137 | 75498 | 109052 | 0.012 | 262.4 | 0.889 | 0/15 |
| tokio | 202,014 | 28312 | 192938 | 201327 | 0.008 | 48.5 | 0.958 | 2/15 |
| tokio-percore | 222,784 | 41943 | 71303 | 92275 | 0.007 | 48.8 | 1.062 | 15/15 |

### http, 100 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU s / 1k req | RSS MiB | vs ref: req/s ratio | faster in |
|---|---|---|---|---|---|---|---|---|
| c | 253,123 | 328 | 1180 | 1704 | 0.007 | 1.6 | — | — |
| c#2 | 252,760 | 328 | 1180 | 1704 | 0.007 | 1.6 | 0.997 | 4/15 |
| go | 181,040 | 410 | 2359 | 5243 | 0.010 | 14.1 | 0.714 | 0/15 |
| kama-fibers | 253,237 | 328 | 1049 | 1573 | 0.006 | 2.1 | 0.999 | 7/15 |
| kama-isolates | 170,645 | 492 | 1638 | 2752 | 0.013 | 3.3 | 0.675 | 0/15 |
| node | 80,322 | 1180 | 2097 | 2621 | 0.010 | 63.9 | 0.317 | 0/15 |
| node-cluster | 203,409 | 442 | 1180 | 1835 | 0.011 | 225.3 | 0.802 | 0/15 |
| tokio | 227,413 | 393 | 983 | 1245 | 0.008 | 3.9 | 0.899 | 0/15 |
| tokio-percore | 245,069 | 311 | 1049 | 1638 | 0.007 | 3.4 | 0.967 | 0/15 |

### http, 1000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU s / 1k req | RSS MiB | vs ref: req/s ratio | faster in |
|---|---|---|---|---|---|---|---|---|
| c | 255,738 | 3277 | 9962 | 12583 | 0.007 | 4.9 | — | — |
| c#2 | 255,374 | 3277 | 9962 | 12583 | 0.007 | 4.9 | 1.000 | 8/15 |
| go | 189,654 | 4719 | 16253 | 23069 | 0.011 | 40.6 | 0.743 | 0/15 |
| kama-fibers | 254,531 | 3277 | 9437 | 12059 | 0.007 | 9.4 | 0.995 | 1/15 |
| kama-isolates | 167,181 | 4981 | 15729 | 25166 | 0.014 | 18.6 | 0.654 | 0/15 |
| node | 65,640 | 14156 | 17826 | 24117 | 0.011 | 67.8 | 0.256 | 0/15 |
| node-cluster | 196,731 | 4456 | 9962 | 14156 | 0.012 | 245.5 | 0.768 | 0/15 |
| tokio | 238,364 | 3015 | 15204 | 19923 | 0.008 | 14.7 | 0.932 | 0/15 |
| tokio-percore | 246,407 | 3539 | 9437 | 12059 | 0.007 | 12.5 | 0.965 | 0/15 |

### http, 10000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU s / 1k req | RSS MiB | vs ref: req/s ratio | faster in |
|---|---|---|---|---|---|---|---|---|
| c | 211,358 | 44040 | 92275 | 109052 | 0.008 | 40.4 | — | — |
| c#2 | 209,736 | 44040 | 92275 | 117440 | 0.008 | 40.3 | 0.996 | 3/15 |
| go | 149,764 | 65012 | 92275 | 104858 | 0.016 | 310.4 | 0.711 | 0/15 |
| kama-fibers | 211,922 | 44040 | 79692 | 100663 | 0.008 | 83.0 | 1.006 | 9/15 |
| kama-isolates | 121,992 | 62915 | 234881 | 1140851 | 0.021 | 172.0 | 0.579 | 0/15 |
| node | 56,474 | 167772 | 201327 | 209715 | 0.014 | 137.8 | 0.268 | 0/15 |
| node-cluster | 152,726 | 50332 | 75498 | 96469 | 0.017 | 316.4 | 0.727 | 0/15 |
| tokio | 200,093 | 4719 | 226492 | 243270 | 0.010 | 113.7 | 0.946 | 1/15 |
| tokio-percore | 184,674 | 52429 | 79692 | 96469 | 0.011 | 106.7 | 0.877 | 0/15 |

### idle, 10000 connections

| arm | held | RSS MiB | RSS ÷ held, KiB (process baseline included) |
|---|---|---|---|
| c | 10000 | 40.6 | 4.2 |
| c#2 | 10000 | 40.6 | 4.2 |
| go | 10000 | 80.6 | 8.2 |
| kama-fibers | 10000 | 81.9 | 8.4 |
| kama-isolates | 10000 | 140.5 | 14.4 |
| node | 10000 | 83.0 | 8.5 |
| node-cluster | 10000 | 255.0 | 26.1 |
| tokio | 10000 | 48.7 | 5.0 |
| tokio-percore | 10000 | 48.7 | 5.0 |

## What it says

Only differences past the noise floor in at least 12 of 15 paired rounds are stated. The floor: `c#2` against `c` sits
within 0.5% in every config, faster in 3 to 9 of 15 rounds.

- **kama's fiber arm runs within 0.6% of the C loop in every config.** At 100 and 10,000 connections it is within the
  floor; at 1,000 the C loop is ahead by 0.5% to 0.6% (faster in 12 and 14 of 15 rounds). Its server CPU per request
  matches the C loop's, 0.006 to 0.008 s per 1,000.
- **Fibers against kama's isolate per connection:** 1.48 to 1.75 times the requests per second, and at 10,000
  connections a p99.9 of 117 ms for echo and 101 ms for HTTP where the thread per connection reached 1.07 s and 1.14 s.
  Memory per idle connection falls from 14.4 KiB to 8.4.
- **Against Go:** faster in every config, 15 of 15 rounds each, by 3.6% (echo, 1,000 connections) to 41% (HTTP,
  10,000).
- **Against Node:** 2.3 to 3.9 times one Node process, and 7% to 39% faster than `cluster` with 3 workers, 15 of 15 in
  each config.
- **Against tokio's work-stealing runtime:** faster in five configs, by 5% to 11% (13 to 15 of 15 rounds); at echo
  with 1,000 connections the two are within the floor. At 10,000 connections its p50 is lower and its p99 and p99.9
  are 1.7 to 2.8 times the fiber arm's: a stolen task finishes sooner on average, and a starved one later.
- **Against tokio's thread-per-core arm,** the closest shape: kama is 3% faster on HTTP at 100 and 1,000 connections
  and 15% faster at 10,000 (15 of 15 each). On echo, tokio is 1.1% faster at 1,000 connections and 4.8% faster at
  10,000 (15 of 15), with a lower p99 and p99.9 (71 and 92 ms against 80 and 117) at 10,000. At 100 connections the two
  are within the floor.
- **Memory per idle connection:** 8.4 KiB for the fiber arm, process baseline included, beside 8.2 for Go and 8.5 for
  Node; tokio holds 5.0 and the C loop 4.2. That is the stackless task's advantage the caveat above names.

## Reproduce

```sh
bench/run build-image                       # once: the toolchains, procps and util-linux, the tokio arm's crates
bench/run server out.tsv 15                 # every arm, every config, 15 rounds (~100 minutes here)
python3 bench/server/summarize.py out.tsv   # these tables; REF=<arm> compares against another arm
```

Run it with nothing else on the machine, including other containers in the same VM. `SLOTS`, `CONFIGS`, `CORES`,
`WARMUP` and `SECONDS_` narrow a run. The raw rows of this run are `servers.tsv`, beside this file.
