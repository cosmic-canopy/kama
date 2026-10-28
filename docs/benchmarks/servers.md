# Server benchmark: kama fibers against Node, Go and tokio

_Measured 2026-10-27 and 28 · kama 0.9.753 · 15 rounds · the `kama-bench` container (Linux 7.0, aarch64) in a podman
VM (libkrun, 8 vCPUs, not nested) on an Apple M4_

This is the server comparison KR-243 started and KR-250 completed. It measures what a kama server written on fibers
(KR-3) costs against the servers people write in Node, Go and Rust's tokio, with a plain C event loop beside them and
kama's isolate-per-connection server as the "before". The harness is `bench/server/`, and the end of this page says how
to reproduce it.

## Read this first

Read these before any number below.

- **This run replaces the one of 2026-10-27 (kama 0.9.750), which mostly measured the VM.** podman 5.8 starts its
  libkrun VM with nested virtualization. The guest kernel then runs at EL2 and reads the physical counter, which the
  hypervisor traps: a kernel clock read cost about 1 µs, against about 30 ns without nesting. Loopback TCP reads the
  clock several times a packet, so about half of every server's time was the trap, and the fastest arms all met near
  255,000 requests a second. The VM now starts without nesting (`ps aux | grep krunkit` shows no `--nested`), and the
  same servers run about 3.3 times faster. kama 0.9.752 and 0.9.753 also changed the fiber arm (below).
- **The server is the limit, and the `busy` column shows it.** The container sees 8 vCPUs. Every server is pinned to 3
  of them (`taskset -c 0-2`) and the load client to the other 5. `busy` is the share of the server's 3 cores that was
  not idle during the measured window, read from `/proc/stat` by the client. It is 99–100% for every arm in every
  config except one Node process, which can use one core (34%). In the earlier run, the client on 3 cores capped the
  four fastest arms instead.
- **CPU per request is the server process's own user and system time** over the measured window. The kernel's softirq
  work, which on loopback delivers each packet to the other end, is charged to no process. So `busy` says whether the
  server was saturated, and requests per second is the throughput.
- **Echo is like for like. HTTP is two comparisons.** The C and kama arms find where each request ends (`\r\n\r\n`)
  and write a fixed reply. `go-raw`, `tokio-raw` and `tokio-percore-raw` do exactly that too, on Go's `net` and tokio's
  TCP: that is the runtime comparison. `go` (`net/http`), `tokio` and `tokio-percore` (hyper) and `node` (`http`)
  parse every request line and header: that is the comparison a kama HTTP stack would join. Node has no raw arm, since
  its echo row already is its raw-socket number.
- **Loopback, not a network.** Requests cross the kernel's loopback inside one VM: no NIC, no wire latency. This
  measures what a server spends per request and how it schedules, not a network stack. The VM's vCPUs float over the
  M4's 4 performance and 6 efficiency cores; the interleaved protocol below keeps that from favouring an arm.
- **A fiber keeps more memory per idle connection than a stackless task.** A tokio future or a Node callback holds only
  its state; a fiber holds the stack pages it touched. The idle table shows it (KR-254).
- **Thread-per-core does not steal work.** The skew configs are where that would show (below).
- **x86-64 is not measured yet.** Every number here is aarch64.

## Setup

| arm | what it is | toolchain |
|---|---|---|
| `c` | an epoll loop per core, level-triggered, nothing between the loop and the socket (`bench/server/c/`) | clang 18.1.3, `-O2` |
| `kama-fibers` | an isolate per core, each accepting on its own `SO_REUSEPORT` listener, a fiber per connection (`bench/server/kama/fibers.kama`) | kama 0.9.753, `--release` |
| `kama-isolates` | an isolate (an OS thread) per connection, blocking reads and writes (`bench/server/kama/server.kama`) | kama 0.9.753, `--release` |
| `go`, `go-raw` | `net`, a goroutine per connection, `GOMAXPROCS=3`; `go` serves HTTP with `net/http` | Go 1.27.3 |
| `tokio`, `tokio-raw` | tokio's default work-stealing runtime, 3 workers; `tokio` serves HTTP with hyper | Rust 1.97.1, tokio 1.54.1, hyper 1.12.0 |
| `tokio-percore`, `tokio-percore-raw` | 3 `current_thread` runtimes, each on its own `SO_REUSEPORT` listener: the shape closest to kama's | as `tokio` |
| `node` | `net` and `http`, one process | Node 22.16.0 |
| `node-cluster` | the same under `cluster`, 3 workers | Node 22.16.0 |

Every arm sets `TCP_NODELAY` on its connections (Go and Node by default).

**Workloads.** `echo`: each connection writes 64 bytes and reads the same 64 back, over and over, one park per request.
`http`: HTTP/1.1 keep-alive `GET /` without pipelining, answered `200` with `Hello, World!`. `skew`: echo, except that 2
of the connections are hot, and each of their requests asks the server for a CPU burst (rounds of an FNV-1a mix over
its 64 bytes; 1,500 rounds take about 100 µs here); latency is the light connections' only. `idle`: 10,000 connections
opened, each making one round trip, then held while the server's resident memory is read. Each measured config runs 1 s
of warm-up and 5 s of load.

**The client** is one C program for every arm (`bench/server/client.c`). It verifies every echoed byte and every HTTP
status and body; a wrong reply, an unexpected close or a failed connect counts as an error, and a run with any error is
left out of every number. **This run had none: 1,215 runs, 0 errors.**

**Protocol** (the house rule: measure interleaved, never sequentially). 15 rounds; in each, every arm runs every config
as fresh processes, the arm order rotated each round. The reference arm, `c`, runs twice per config, and its gap to
itself (`c#2`) is the noise floor. A difference is claimed only when it holds in at least 12 of the 15 paired rounds.
Each number below is the median over the rounds; latency is the client's, from writing a request to reading its whole
reply.

## Results

Against `c`: the "vs ref" columns are the median of the per-round ratio, and in how many of the 15 paired rounds the
arm was the faster (requests per second) or the cheaper (CPU per request). RSS is the server's whole process tree at
its peak.

### echo, 100 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 930,454 | 106 | 278 | 459 | 2.09 | 100% | 1.6 | — | — | — | — |
| c#2 | 930,179 | 111 | 278 | 459 | 2.09 | 100% | 1.6 | 1.001 | 8/15 | 0.998 | 9/15 |
| go | 750,551 | 115 | 393 | 1770 | 2.36 | 91% | 6.8 | 0.806 | 0/15 | 1.126 | 0/15 |
| kama-fibers | 969,742 | 90 | 246 | 426 | 1.90 | 99% | 2.1 | 1.040 | 15/15 | 0.910 | 15/15 |
| kama-isolates | 580,750 | 164 | 442 | 885 | 3.87 | 100% | 3.3 | 0.622 | 0/15 | 1.848 | 0/15 |
| node | 266,400 | 344 | 688 | 885 | 2.77 | 34% | 56.2 | 0.285 | 0/15 | 1.328 | 0/15 |
| node-cluster | 643,784 | 131 | 393 | 655 | 3.28 | 100% | 212.9 | 0.690 | 0/15 | 1.570 | 0/15 |
| tokio | 710,027 | 131 | 295 | 377 | 2.20 | 87% | 2.8 | 0.760 | 0/15 | 1.053 | 0/15 |
| tokio-percore | 970,538 | 90 | 238 | 442 | 1.93 | 99% | 2.7 | 1.042 | 15/15 | 0.921 | 15/15 |

### echo, 1000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 970,176 | 918 | 2490 | 3932 | 2.09 | 100% | 4.9 | — | — | — | — |
| c#2 | 968,923 | 918 | 2359 | 3932 | 2.10 | 100% | 4.9 | 0.998 | 6/15 | 1.002 | 5/15 |
| go | 809,583 | 1180 | 2228 | 5505 | 2.45 | 97% | 14.3 | 0.834 | 0/15 | 1.172 | 0/15 |
| kama-fibers | 977,657 | 918 | 2228 | 3932 | 1.97 | 100% | 9.1 | 1.008 | 14/15 | 0.939 | 15/15 |
| kama-isolates | 531,414 | 1704 | 5243 | 10486 | 4.37 | 100% | 18.5 | 0.548 | 0/15 | 2.086 | 0/15 |
| node | 269,641 | 3539 | 4194 | 6554 | 2.81 | 34% | 57.5 | 0.278 | 0/15 | 1.343 | 0/15 |
| node-cluster | 656,127 | 1442 | 3146 | 4456 | 3.34 | 100% | 224.1 | 0.680 | 0/15 | 1.596 | 0/15 |
| tokio | 942,228 | 786 | 4063 | 5767 | 2.03 | 100% | 7.0 | 0.975 | 0/15 | 0.970 | 15/15 |
| tokio-percore | 982,734 | 918 | 2228 | 3932 | 1.97 | 100% | 6.9 | 1.013 | 14/15 | 0.942 | 15/15 |

### echo, 10000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 720,127 | 13107 | 20972 | 33554 | 2.80 | 100% | 40.4 | — | — | — | — |
| c#2 | 719,563 | 13107 | 20972 | 33554 | 2.81 | 100% | 40.3 | 0.999 | 7/15 | 1.001 | 6/15 |
| go | 555,713 | 16777 | 24117 | 31457 | 3.54 | 98% | 81.6 | 0.774 | 0/15 | 1.259 | 0/15 |
| kama-fibers | 707,542 | 13632 | 22020 | 33554 | 2.74 | 100% | 82.1 | 0.981 | 0/15 | 0.978 | 15/15 |
| kama-isolates | 359,351 | 25166 | 67109 | 134218 | 6.42 | 100% | 170.7 | 0.501 | 0/15 | 2.282 | 0/15 |
| node | 135,636 | 71303 | 79692 | 83886 | 3.97 | 34% | 102.1 | 0.189 | 0/15 | 1.414 | 0/15 |
| node-cluster | 495,840 | 16777 | 23069 | 31457 | 4.35 | 100% | 261.0 | 0.691 | 0/15 | 1.548 | 0/15 |
| tokio | 751,324 | 983 | 67109 | 79692 | 2.57 | 100% | 48.6 | 1.046 | 15/15 | 0.918 | 15/15 |
| tokio-percore | 714,117 | 13107 | 20972 | 33554 | 2.72 | 100% | 48.8 | 0.992 | 3/15 | 0.969 | 15/15 |

### http, 100 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 921,126 | 106 | 295 | 459 | 2.13 | 100% | 1.6 | — | — | — | — |
| c#2 | 923,216 | 106 | 278 | 459 | 2.13 | 100% | 1.6 | 1.004 | 10/15 | 0.998 | 9/15 |
| go | 315,511 | 229 | 1311 | 3539 | 6.39 | 93% | 14.4 | 0.342 | 0/15 | 3.007 | 0/15 |
| go-raw | 743,517 | 115 | 393 | 1704 | 2.39 | 91% | 6.8 | 0.807 | 0/15 | 1.122 | 0/15 |
| kama-fibers | 954,899 | 94 | 246 | 442 | 1.93 | 99% | 2.1 | 1.036 | 15/15 | 0.905 | 15/15 |
| kama-isolates | 577,450 | 164 | 459 | 918 | 3.90 | 100% | 3.3 | 0.627 | 0/15 | 1.833 | 0/15 |
| node | 120,383 | 786 | 1049 | 1704 | 6.41 | 34% | 63.7 | 0.131 | 0/15 | 3.006 | 0/15 |
| node-cluster | 312,596 | 278 | 688 | 950 | 7.30 | 100% | 225.9 | 0.340 | 0/15 | 3.420 | 0/15 |
| tokio | 572,478 | 164 | 360 | 475 | 3.18 | 93% | 3.9 | 0.623 | 0/15 | 1.488 | 0/15 |
| tokio-percore | 804,631 | 119 | 295 | 475 | 2.49 | 100% | 3.4 | 0.875 | 0/15 | 1.166 | 0/15 |
| tokio-percore-raw | 956,002 | 94 | 246 | 459 | 1.96 | 99% | 2.7 | 1.039 | 15/15 | 0.921 | 15/15 |
| tokio-raw | 697,987 | 131 | 295 | 377 | 2.25 | 87% | 2.8 | 0.760 | 0/15 | 1.058 | 0/15 |

### http, 1000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 954,051 | 950 | 2490 | 4063 | 2.14 | 100% | 4.9 | — | — | — | — |
| c#2 | 953,963 | 950 | 2490 | 4063 | 2.14 | 100% | 4.9 | 1.001 | 8/15 | 0.997 | 9/15 |
| go | 307,372 | 3015 | 8389 | 12583 | 7.08 | 96% | 40.1 | 0.322 | 0/15 | 3.315 | 0/15 |
| go-raw | 798,134 | 1180 | 2359 | 5767 | 2.50 | 97% | 14.2 | 0.835 | 0/15 | 1.169 | 0/15 |
| kama-fibers | 965,332 | 950 | 2228 | 3932 | 2.00 | 100% | 9.4 | 1.013 | 15/15 | 0.937 | 15/15 |
| kama-isolates | 526,345 | 1704 | 5243 | 10486 | 4.42 | 100% | 18.6 | 0.551 | 0/15 | 2.059 | 0/15 |
| node | 115,117 | 7864 | 9962 | 13107 | 6.70 | 34% | 75.8 | 0.121 | 0/15 | 3.116 | 0/15 |
| node-cluster | 342,414 | 2752 | 5243 | 6554 | 7.14 | 100% | 245.7 | 0.358 | 0/15 | 3.343 | 0/15 |
| tokio | 706,452 | 786 | 5767 | 8126 | 2.97 | 100% | 14.9 | 0.740 | 0/15 | 1.384 | 0/15 |
| tokio-percore | 789,122 | 1180 | 2884 | 3932 | 2.67 | 100% | 12.7 | 0.829 | 0/15 | 1.246 | 0/15 |
| tokio-percore-raw | 974,326 | 918 | 2228 | 3932 | 2.01 | 100% | 6.9 | 1.021 | 15/15 | 0.936 | 15/15 |
| tokio-raw | 931,631 | 786 | 4063 | 5767 | 2.08 | 100% | 7.0 | 0.974 | 0/15 | 0.967 | 15/15 |

### http, 10000 connections

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 710,443 | 13632 | 22020 | 33554 | 2.86 | 100% | 40.4 | — | — | — | — |
| c#2 | 711,858 | 13632 | 22020 | 33554 | 2.86 | 100% | 40.4 | 1.001 | 8/15 | 0.997 | 12/15 |
| go | 256,524 | 37749 | 52429 | 60817 | 8.52 | 96% | 308.9 | 0.360 | 0/15 | 2.969 | 0/15 |
| go-raw | 548,878 | 16777 | 24117 | 30409 | 3.61 | 98% | 81.6 | 0.773 | 0/15 | 1.259 | 0/15 |
| kama-fibers | 690,320 | 13632 | 20972 | 33554 | 2.84 | 100% | 83.5 | 0.971 | 0/15 | 0.991 | 14/15 |
| kama-isolates | 355,827 | 25166 | 67109 | 134218 | 6.48 | 100% | 172.1 | 0.500 | 0/15 | 2.261 | 0/15 |
| node | 81,737 | 113246 | 125829 | 125829 | 8.43 | 34% | 134.7 | 0.115 | 0/15 | 2.955 | 0/15 |
| node-cluster | 295,251 | 10486 | 16777 | 54526 | 8.47 | 100% | 310.5 | 0.416 | 0/15 | 2.956 | 0/15 |
| tokio | 538,050 | 983 | 96469 | 109052 | 3.88 | 99% | 113.1 | 0.755 | 0/15 | 1.360 | 0/15 |
| tokio-percore | 526,214 | 17826 | 23069 | 29360 | 4.05 | 100% | 107.0 | 0.741 | 0/15 | 1.415 | 0/15 |
| tokio-percore-raw | 711,117 | 13107 | 22020 | 32506 | 2.74 | 100% | 48.8 | 0.997 | 6/15 | 0.959 | 15/15 |
| tokio-raw | 745,291 | 1016 | 67109 | 79692 | 2.60 | 100% | 48.8 | 1.047 | 15/15 | 0.909 | 15/15 |

### skew, 1000 connections, 100 µs bursts

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 920,549 | 1049 | 2490 | 3932 | 2.30 | 100% | 4.9 | — | — | — | — |
| c#2 | 919,727 | 1049 | 2490 | 3932 | 2.29 | 100% | 4.9 | 0.999 | 4/15 | 0.999 | 10/15 |
| go | 763,326 | 1245 | 2490 | 5505 | 2.66 | 97% | 9.6 | 0.826 | 0/15 | 1.163 | 0/15 |
| kama-fibers | 877,894 | 1114 | 2359 | 3801 | 2.36 | 100% | 5.4 | 0.954 | 0/15 | 1.027 | 0/15 |
| kama-isolates | 529,176 | 1704 | 5243 | 10486 | 4.39 | 100% | 14.7 | 0.575 | 0/15 | 1.910 | 0/15 |
| node | 222,087 | 4194 | 5767 | 7602 | 3.42 | 34% | 88.0 | 0.241 | 0/15 | 1.485 | 0/15 |
| node-cluster | 580,794 | 1573 | 3277 | 4719 | 3.91 | 100% | 275.4 | 0.630 | 0/15 | 1.700 | 0/15 |
| tokio | 845,371 | 819 | 4981 | 7078 | 2.41 | 100% | 3.1 | 0.917 | 0/15 | 1.053 | 0/15 |
| tokio-percore | 893,192 | 1049 | 2359 | 3932 | 2.34 | 100% | 3.2 | 0.971 | 0/15 | 1.013 | 0/15 |

### idle, 10000 connections

| arm | held | RSS MiB | RSS ÷ held, KiB (process baseline included) |
|---|---|---|---|
| c | 10000 | 40.6 | 4.2 |
| c#2 | 10000 | 40.6 | 4.2 |
| go | 10000 | 80.7 | 8.3 |
| kama-fibers | 10000 | 82.3 | 8.4 |
| kama-isolates | 10000 | 101.6 | 10.4 |
| node | 10000 | 83.3 | 8.5 |
| node-cluster | 10000 | 257.2 | 26.3 |
| tokio | 10000 | 48.7 | 5.0 |
| tokio-percore | 10000 | 48.8 | 5.0 |

### Skew at low load

The skew config above keeps every core saturated, which leaves a work-stealing runtime no idle core to steal onto. Two
more runs of 15 rounds each, with the arms that decide it, put the load where stealing is strongest: 12 connections (2
of them hot), 100 connections, and bursts of 1 ms (`SKEW_ROUNDS=15000`). At 12 connections even the C loop's gap to
itself is wide (`c#2`), because each run's numbers depend on where the kernel puts the two hot connections.

### skew, 12 connections, 100 µs bursts

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 148,081 | 55 | 246 | 344 | 8.60 | 76% | 1.3 | — | — | — | — |
| c#2 | 152,422 | 55 | 229 | 328 | 8.91 | 79% | 1.3 | 1.023 | 9/15 | 1.003 | 7/15 |
| go | 153,924 | 55 | 213 | 344 | 11.55 | 76% | 6.1 | 1.048 | 10/15 | 1.334 | 7/15 |
| kama-fibers | 137,829 | 53 | 221 | 295 | 12.30 | 80% | 1.4 | 0.912 | 6/15 | 1.095 | 5/15 |
| tokio | 181,547 | 55 | 131 | 188 | 10.53 | 88% | 2.4 | 1.206 | 14/15 | 1.224 | 6/15 |
| tokio-percore | 133,660 | 59 | 221 | 311 | 12.74 | 82% | 2.3 | 0.931 | 7/15 | 1.172 | 6/15 |

### skew, 100 connections, 100 µs bursts

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 627,135 | 131 | 426 | 623 | 3.49 | 100% | 1.6 | — | — | — | — |
| c#2 | 620,116 | 131 | 426 | 623 | 3.52 | 100% | 1.6 | 0.998 | 7/15 | 1.006 | 7/15 |
| go | 504,447 | 164 | 590 | 1049 | 4.18 | 97% | 6.4 | 0.804 | 0/15 | 1.199 | 0/15 |
| kama-fibers | 578,934 | 156 | 377 | 524 | 3.81 | 100% | 1.8 | 0.926 | 2/15 | 1.087 | 2/15 |
| tokio | 518,045 | 180 | 377 | 492 | 3.98 | 97% | 2.4 | 0.827 | 0/15 | 1.138 | 0/15 |
| tokio-percore | 582,625 | 156 | 377 | 524 | 3.80 | 100% | 2.4 | 0.928 | 2/15 | 1.089 | 2/15 |

### skew, 12 connections, 1 ms bursts

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 123,904 | 43 | 1245 | 1835 | 12.58 | 82% | 1.3 | — | — | — | — |
| c#2 | 80,283 | 45 | 1245 | 2359 | 26.43 | 84% | 1.3 | 0.795 | 5/15 | 1.438 | 6/15 |
| go | 154,636 | 49 | 655 | 1245 | 14.25 | 89% | 6.0 | 1.237 | 13/15 | 1.133 | 7/15 |
| kama-fibers | 97,241 | 53 | 1245 | 1311 | 21.90 | 88% | 1.4 | 0.737 | 6/15 | 1.481 | 5/15 |
| tokio | 186,943 | 51 | 123 | 475 | 12.52 | 97% | 2.4 | 1.507 | 15/15 | 0.993 | 8/15 |
| tokio-percore | 97,356 | 47 | 1245 | 1835 | 15.22 | 80% | 2.3 | 0.815 | 4/15 | 1.140 | 6/15 |

### skew, 1000 connections, 1 ms bursts

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 619,176 | 1245 | 4063 | 4981 | 3.78 | 100% | 4.9 | — | — | — | — |
| c#2 | 618,613 | 1245 | 4063 | 4981 | 3.79 | 100% | 5.0 | 0.997 | 6/15 | 1.007 | 6/15 |
| go | 530,345 | 1770 | 3539 | 6292 | 4.13 | 98% | 9.5 | 0.856 | 0/15 | 1.093 | 0/15 |
| kama-fibers | 588,986 | 1507 | 3408 | 4981 | 3.87 | 100% | 5.4 | 0.943 | 5/15 | 1.028 | 5/15 |
| tokio | 542,195 | 754 | 11010 | 15204 | 4.13 | 100% | 3.1 | 0.876 | 0/15 | 1.091 | 0/15 |
| tokio-percore | 601,051 | 1442 | 3277 | 5243 | 3.81 | 100% | 3.2 | 0.963 | 5/15 | 1.011 | 5/15 |


A third run, plain echo at 12 and 24 connections with no bursts (2026-10-28, the same arms, 15 rounds), separates the
kernel's uneven placement from the bursts:

### echo, 12 connections (no bursts)

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 240,576 | 47 | 119 | 164 | 3.00 | 73% | 1.3 | — | — | — | — |
| c#2 | 240,721 | 47 | 119 | 172 | 2.98 | 72% | 1.3 | 0.998 | 6/15 | 1.004 | 7/15 |
| go | 235,791 | 47 | 123 | 172 | 3.70 | 66% | 6.1 | 0.984 | 3/15 | 1.225 | 0/15 |
| kama-fibers | 238,646 | 47 | 115 | 156 | 2.66 | 70% | 1.4 | 0.996 | 6/15 | 0.901 | 14/15 |
| tokio | 230,886 | 49 | 111 | 148 | 3.60 | 69% | 2.4 | 0.961 | 1/15 | 1.196 | 0/15 |
| tokio-percore | 236,219 | 47 | 115 | 148 | 2.83 | 70% | 2.4 | 0.981 | 3/15 | 0.958 | 14/15 |

### echo, 24 connections (no bursts)

| arm | req/s | p50 µs | p99 µs | p99.9 µs | CPU µs / req | busy | RSS MiB | vs ref: req/s ratio | faster in | vs ref: CPU ratio | cheaper in |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c | 525,245 | 39 | 131 | 188 | 2.57 | 88% | 1.4 | — | — | — | — |
| c#2 | 525,459 | 39 | 131 | 188 | 2.58 | 89% | 1.4 | 1.013 | 8/15 | 1.000 | 8/15 |
| go | 434,132 | 47 | 148 | 328 | 2.61 | 68% | 6.3 | 0.826 | 0/15 | 1.018 | 0/15 |
| kama-fibers | 551,071 | 37 | 123 | 164 | 2.30 | 88% | 1.5 | 1.043 | 11/15 | 0.893 | 15/15 |
| tokio | 394,320 | 57 | 139 | 180 | 2.96 | 79% | 2.5 | 0.755 | 0/15 | 1.155 | 0/15 |
| tokio-percore | 537,026 | 37 | 123 | 172 | 2.37 | 88% | 2.4 | 1.022 | 9/15 | 0.923 | 15/15 |

## What it says

Only differences that hold in at least 12 of 15 paired rounds are stated. In every saturated config `c#2` sits within
0.4% of `c`.

- **Against tokio's thread-per-core arm, the closest shape:** level on echo at 100 and 1,000 connections and on
  like-for-like HTTP at 100, then 0.5% behind on HTTP at 1,000 (12/15), and 1.1% behind on echo and 2.9% on HTTP at
  10,000 (15/15). On echo at 10,000, kama's CPU per request is within 1% of tokio's (on HTTP, 3.8% above it); what is
  left is the cache footprint of switching among 3,300 fiber stacks a core (KR-254). Against hyper on that same
  runtime, kama's minimal handler serves 1.18 to 1.31 times as many requests.
- **Against the C epoll loop:** 4.0% faster on echo at 100 connections and 0.8% at 1,000 (15/15, 14/15), and 2.0%
  slower at 10,000 (15/15) while spending 2.2% less CPU per request. The loop reads until `EAGAIN`, as most hand-written
  loops do, so it makes the system call that kama 0.9.752 stopped making. It is a plain loop, not a ceiling.
- **Against tokio's work-stealing runtime:** kama is 1.38 times as fast on echo at 100 connections and 3.7% faster at
  1,000, and 1.36 times and 3.8% on raw HTTP. At 10,000 connections tokio serves 6.5% more on echo and 7.9% more on raw
  HTTP (15/15), at three times kama's p99 (67 ms against 22): its p50 is 1 ms, so it serves some connections over and
  over while others wait, and the ones it favours stay in cache.
- **With CPU bursts on a few connections:** saturated, thread-per-core loses nothing. At 1,000 connections
  tokio-percore is 2.0% ahead of kama (13/15), and work-stealing tokio is 3.8% behind it with twice its light-connection
  p99. At low load it loses (the section above): with 12 connections, work-stealing tokio serves 1.29 times kama's
  requests with 100 µs bursts and 1.91 times with 1 ms bursts (15/15 each), at a light-connection p99 of 131 and
  123 µs against 221 and 1,245. Go, which steals and preempts, serves 1.12 and 1.59 times. tokio-percore and the C loop
  fare as kama does, and all three swing from run to run (kama from 36,000 to 137,000 requests a second at 1 ms) by
  where the kernel's `SO_REUSEPORT` hash put the hot connections. A core holding one serves its other connections only
  between bursts, while another core waits. The stealing runtimes stay steady (tokio 182,000 to 189,000). From 100
  connections up, every core's queue is full and kama is ahead of work-stealing tokio: 12% at 100 connections, 9% with
  1 ms bursts at 1,000, at a third of its p99 (3.4 ms against 11.0). Uneven placement alone costs nothing: with no
  bursts, kama is level with Go and 3.3% ahead of work-stealing tokio at 12 connections, and 1.26 and 1.40 times them at
  24 (15/15). It is the burst that holds a core, and KR-255 holds the answer (a job pool the burst is handed to).
- **Against Go:** 1.21 to 1.30 times on echo, 1.21 to 1.28 times `go-raw` on HTTP, and 2.7 to 3.1 times `net/http`.
- **Against Node:** 3.6 to 5.2 times one Node process on echo and 1.42 to 1.51 times `cluster` with 3 workers; on HTTP,
  7.9 to 8.5 times and 2.3 to 3.0 times, against Node's full `http`.
- **Fibers against kama's isolate per connection:** 1.65 to 1.97 times the requests per second, and at 10,000
  connections a p99.9 of 34 ms where the thread per connection reached 134 ms.
- **Memory per idle connection:** 8.4 KiB for the fiber arm, process baseline included, beside 8.3 for Go and 8.5 for
  Node; tokio holds 5.0 and the C loop 4.2. That is the stackless task's advantage the caveat above names (KR-254).
- **What kama 0.9.752 and 0.9.753 changed (KR-251):** a stream read that comes back short now parks the next one
  first, which saves one system call in three per request, and the scheduler prefetches the next fiber's saved frame.
  Each was measured interleaved against the version before it: the first serves 6.5% more echo requests a second at
  1,000 connections and 4.5% more at 10,000, and the second another 0.9% at 10,000 (none at 1,000).

## Reproduce

```sh
bench/run build-image                       # once: the toolchains, the tokio arm's crates, perf and strace
bench/run server out.tsv 15                 # every arm, every config, 15 rounds (about 4 hours here)
python3 bench/server/summarize.py out.tsv   # these tables; REF=<arm> compares against another arm
```

Run it with nothing else on the machine, including other containers in the same VM. **On macOS, make sure the podman
VM is not nested**, or the kernel's clock reads trap (see the top of this page). podman 5.8 adds `--nested` to krunkit
on its own. A wrapper that drops it, found first through `helper_binaries_dir` in `~/.config/containers/containers.conf`,
turns it off:

```sh
#!/bin/sh
for a; do shift; [ "$a" = --nested ] || set -- "$@" "$a"; done
exec /opt/podman/bin/krunkit "$@"
```

`SLOTS`, `CONFIGS`, `CORES`, `WARMUP`, `SECONDS_`, `SKEW_HOT` and `SKEW_ROUNDS` narrow a run, and the low-load skew runs
used `SLOTS="c go tokio tokio-percore kama-fibers c#2"`. `KAMA_B_TREE=<worktree>` adds a `kama-fibers-b` arm built by
that tree's compiler, for an A/B in the same rounds, and `WRAP` runs each server under `perf record` or `strace`. The
raw rows are beside this file: `servers.tsv` (the main run), `servers-skew-100us.tsv`, `servers-skew-1ms.tsv` and
`servers-echo-low.tsv`. On Linux, `run.sh` first measures what a kernel clock read costs (`clockcost.c`) and refuses to
run where it is more than 200 ns, which a trapping VM is.
