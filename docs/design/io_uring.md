# An io_uring backend for the fiber scheduler — design (KR-253)

**Status: building (§8); steps 1–3 shipped in `0.9.756`–`0.9.758`, and step 4, the measurement, is next.** Decisions 1, 3–6 and 8–11 are ruled as recommended, with 8's bar at 5%, and 9–11 on
2026-10-28.
Decision 7 is ruled as revised, after the first recommendation rested on a false claim (§5). Decision 8 is measured on both machines
(§6). Both directions through the ring, (b′), clears the bar on aarch64. On x86-64 it misses in one cell, 10,000
connections on one core with 4 KiB replies, where it saves 2%. On those numbers the maintainer ruled, on 2026-10-28, to
build (b′) (decision 2, re-ruled), for the most performance kama can have without giving up correctness or safety. The
build is §8. A design doc and a
measurement came before any code, as [ROADMAP_DETAIL §9](../ROADMAP_DETAIL.md#s9-io-uring) asks. This file builds on [fibers.md](fibers.md), whose decisions it cites by number, and
is deleted when the work ships or closes. Facts about other runtimes were read from their source or their maintainers'
own words on 2026-10-28 (*Sources*). Kernel versions are from the liburing man pages. Every proposed spelling is a `kama
fragment`.

## 1. The problem

On Linux the scheduler waits on epoll (fibers.md decision 6). A socket is registered once, edge-triggered, so a park
costs no system call. Since `0.9.752`, a short read parks before the call that could only answer `EAGAIN`, so a
request and its reply cost two system calls, `recv` and `send`, plus a share of one `epoll_wait`. That is tokio's
count, and kama is level with tokio's thread-per-core runtime at 100 and 1,000 connections and 1.1–2.9% behind it at
10,000 ([benchmarks/servers.md](../benchmarks/servers.md)).

What is left is the system calls themselves: one per I/O call, made one fiber at a time. io_uring takes I/O as
submissions into a ring shared with the kernel, so one `io_uring_enter` can submit every fiber's `send` and `recv`
from a turn and wait for completions in the same call. The fastest server runtimes today (Seastar, glommio, monoio)
are built on it. The question this file answers is whether that lever is real for kama, and in what shape.

## 2. What the tree has today (`0.9.754`)

- `include/kama_sched.h` picks one arm at compile time: epoll on Linux, kqueue on macOS and FreeBSD, the public
  `Poller` (AFD, `poll(2)`) elsewhere.
- Every blocking std socket call runs one protocol (kama_os.h): `kama_net_ready` before the system call,
  `kama_net_retry` after it fails, and `kama_net_short`/`kama_net_drained` around a short call.
- Each socket keeps its registration (`kama_sched_io`) beside its descriptor. A socket can move to another isolate,
  and the next user releases the old registration (decision 13).
- Deadlines are absolute, per socket, kept on a per-scheduler timer heap (decision 11).
- A fiber's read buffer is the caller's own, often on the fiber's stack. Nothing in std's surface says where it is.

## 3. What others do

| runtime | rings | what goes through the ring for a socket | when the ring is unavailable |
|---|---|---|---|
| Seastar | one per shard | the system call first; when it would block, `read_some` and `accept` poll through the ring and then submit the operation; `recvmsg`, `sendmsg` and `connect` submit the operation itself | the next backend: linux-aio, then epoll |
| glommio | three per executor | the system call first, then a poll through the ring; accept and connect are ring operations | no fallback; requires 5.8 |
| monoio | one per thread | accept, recv and send as ring operations, into owned buffers | the epoll driver, chosen by an opcode probe |
| tokio-uring | one per thread | accept, connect, read, write, send and receive as ring operations, into owned buffers | none |
| Netty 4.2 | one per event loop | a poll, then `recv` into a buffer it allocates; multishot `recv` with kernel-chosen buffers only when a buffer ring is configured | another transport, chosen by the program |
| libuv | one per loop | none for sockets: the ring batches `epoll_ctl`; file operations went back to the threadpool in 1.49 | the threadpool and epoll |
| Zig 0.16 | one per thread | `Io.Evented` is experimental and "does not yet implement networking"; `Io.Threaded` is the default | — |
| Go | — | none. Issue #31908 is open and unplanned: "I don't see why io_uring will be a huge gain for the common case of a network server" (Ian Lance Taylor) | — |
| Java (Loom) | prototype, one per carrier | a poll through the ring; it "will not perform as well as the epoll based Poller", and batching submissions "added latency and reduced performance" | epoll, the default |

Three things in that table decide the shape of kama's answer:
- **A fallback is not optional.** Seastar and monoio fall back to epoll; glommio and tokio-uring fail. io_uring is
  blocked by default in Docker since 25.0 (`EPERM`), in Podman (`ENOSYS`), in containerd 2.0 and in gVisor. Google
  disables it on ChromeOS and its production servers and makes it unreachable to Android apps, after 60% of the kernel
  exploits submitted to its kCTF program in a year went through it. Since 6.6 a sysctl, `kernel.io_uring_disabled`,
  turns it off for a whole machine.
- **The ones that lead try the system call first.** Seastar and glommio use the ring only once a call would block.
  That is kama's protocol already.
- **Owned buffers are a stackless runtime's workaround, not io_uring's requirement.** A future can be dropped at any
  point while the kernel still holds its buffer, so tokio-uring and monoio take ownership of every buffer, and monoio
  documents that a read cancelled by a timeout can lose data. A fiber cannot be dropped mid-wait (decision 3), so it
  can keep reading into the caller's buffer (decision 3 below).

**Published numbers disagree, and none of them is kama's case.** An echo server on 5.6 measured io_uring 1.5–1.6 times
epoll at 300 and 1,000 clients and level at one (frevib); monoio measured about 2× tokio at 4 cores and close to 3× at
16 (kernel 5.15); a streaming test measured io_uring at a third of epoll for
64-byte messages (Photon, liburing#536), which Jens Axboe questioned as not like for like. None compares against an
edge-triggered epoll that already skips the `EAGAIN` call, which is what kama has. Hence decision 8.

## 4. The model, end to end

A fiber calls `conn.read(into: buf)`. On a Linux scheduler running the ring arm:
1. The system call is tried first, `MSG_DONTWAIT`, unless the last call came back short (`kama_net_drained`), as today.
2. If it would block, the fiber writes a `RECV` submission naming its own buffer and its fiber, and parks. No system
   call is made.
3. When nothing is ready, the scheduler makes one `io_uring_enter`. It submits every queued submission from the turn
   and waits for at least one completion, or for the earliest timer.
4. Each completion names a fiber. The scheduler readies it with the result, and the fiber's `read` returns it.

A deadline that passes while the `RECV` is in flight submits a cancel, and the fiber parks again until the kernel has
posted the `RECV`'s own completion: only then is the buffer the fiber's again. If that completion carries data, the
data is returned. The fiber never leaves a wait while the kernel holds its buffer, which is what a stackless future
cannot promise.

```kama fragment
fn void handle(TcpStream conn) {              // unchanged: the program does not know which arm runs
    conn.setReadDeadline(at: Some(value: Instant.now().after(seconds: 30)));
    match (conn.read(into: buf.view())) { … };
}
```

## 5. Decisions

### Decision 1 — an arm chosen at run time, behind the same surface

**Recommended:** the ring is a fourth arm of `kama_sched.h`, Linux only. The scheduler probes for it when it is made,
and falls back to the epoll arm, which stays permanently. No surface changes: `TcpStream.read` has one meaning
(fibers.md decision 7), and a program written for epoll runs on the ring unchanged.
- **The fallback is not optional.** Docker, Podman, containerd 2.0, gVisor, Google's servers and ChromeOS all refuse
  io_uring by default, and the sysctl lets any administrator do the same. A Linux arm without epoll would fail on the
  default container runtimes.
- **The arm is per scheduler.** Each isolate's scheduler probes once, when it is made, and keeps its answer.
- Seastar and monoio do exactly this. Netty leaves the choice to the program, which is a second way to write a
  server (GOALS #4).

### Decision 2 — what goes through the ring

Three shapes. Decision 8 measures all three before one is chosen.
- **(a) Readiness through the ring:** a multishot poll per socket stands in for the epoll registration, and the I/O stays
  `recv` and `send`. It saves the `epoll_wait` only, which an edge registration already amortizes. The Loom prototype
  measured this shape slower than epoll.
- **(b) The operation through the ring, after the system call would block:** a parked read is a `RECV` into the
  fiber's own buffer, and a write that would block is a `SEND`. One `io_uring_enter` per turn submits every fiber's
  operations and harvests their completions. A parked read's completion carries its data, so the `recv` that follows
  every wake today disappears: an echo request costs its `send` and a share of one `io_uring_enter`, against a `recv`,
  a `send` and a share of one `epoll_wait` today.
- **(c) Kernel-chosen buffers:** a multishot `RECV` per socket (6.0) over a provided buffer ring (5.19). One submission
  serves every read for the socket's life, and data lands in a buffer the kernel picks. `read(into:)` then copies it
  into the caller's buffer, or the surface grows a borrowed-buffer read, which is a second way to read (GOALS #4). Netty
  uses this shape when the program configures a buffer ring.

**Recommended: (b) as the candidate, (a) and (c) as arms of the measurement.** (b) keeps the surface, the buffer and
the protocol, and is what Seastar does for `recvmsg` and `sendmsg`. (c) is the most system-call-thrifty shape and costs
either a copy or a surface change, so it goes to the user as a measurement, not as a default.

A further shape, submitting every `send` to the ring without trying the system call, leaves a request only its share
of the `io_uring_enter`. That trades a system call for a park and a switch on every write, since the fiber must wait
for the kernel to let go of its buffer. Decision 8 measures it as (b′).

**Re-ruled 2026-10-28: (b′),** on both machines' measurements (§6). It is the only shape that clears the bar anywhere,
and (b) fails it on aarch64. A stream read makes its system call first and submits a `RECV` only once that would block,
or after a read came back short. Every stream write is a `SEND`. `POLL_FIRST` stays off: it helps at 10,000 connections
on one core and costs at 1,000 on both machines, and a core of servers.md's 10,000-connection run holds about 3,300.

### Decision 3 — the buffer is the caller's, and a fiber leaves a wait only when the kernel has let go

**Recommended:** a read or write submitted to the ring names the caller's buffer, exactly as the system call does.
Nothing in std's surface changes, and nothing is copied.
- **A deadline, or a scope's cancellation (decision 11), submits a cancel and parks again.** The fiber resumes only when
  the original operation's own completion has arrived. It carries `-ECANCELED`, or a result the kernel finished first.
- **A result that arrived first is returned, not dropped.** The deadline's error then belongs to the next call, which
  fails at once because the deadline has passed (decision 11's rule). No byte is lost, where monoio documents that a
  timed-out read can lose data.
- **The scheduler cannot end with operations in flight.** An isolate ending, or a scheduler being freed, cancels and
  drains every submission first.
- **Why this is fibers' property.** A stackless future's buffer is owned by a future that may be dropped at any await
  point, and `drop` cannot wait for the kernel. So tokio-uring and monoio take ownership of every buffer (withoutboats:
  "the kernel must own the buffer"). A fiber's frame stays put while it is parked, and it decides when it leaves.

### Decision 4 — deadlines stay on the timer heap

**Recommended:** the timer heap (decision 11) stays, and a deadline that passes cancels the submission (decision 3).
A linked timeout per operation (`IORING_OP_LINK_TIMEOUT`, 5.5) would cost a second submission per operation, and
whether it takes an absolute deadline is unconfirmed. The heap already holds every parked fiber's deadline and sets
the wait's timeout, which `io_uring_enter` takes as `IORING_ENTER_EXT_ARG`.

### Decision 5 — descriptors: no fixed files at first

**Recommended:** sockets are passed to the ring by their descriptor. Fixed (registered) files are ring-local ("only
io_uring has access to such files"), and kama's sockets move between isolates (decision 13), so a fixed file would
need deregistering on every hand-off. The ring's own descriptor is registered (5.18), which costs one call per ring.
Fixed files are a later measurement, for a socket that never moves.

Decision 13's release rule carries over unchanged: no operation can be in flight on a socket that moves, because a
socket moves only by value, and a socket with a parked fiber is borrowed by it.

### Decision 6 — the kernel floor, and the probe

**Recommended: 6.1.** The ring is set up with `IORING_SETUP_SINGLE_ISSUER` (6.0) and `IORING_SETUP_DEFER_TASKRUN`
(6.1): one thread submits, and completions run only when that thread asks. That is exactly a scheduler's shape, and
the latency gains the kernel's own commits report rest on it. A kernel older than 6.1 runs the epoll arm.
- **The probe** is one `io_uring_setup` with those flags, then `IORING_REGISTER_PROBE` for each operation the arm
  uses. Any failure is "unavailable": `ENOSYS` (Podman's seccomp, or no io_uring), `EPERM` (Docker's seccomp, or the
  sysctl), `EACCES` (an SELinux policy), `EINVAL` (a kernel without the flags), or an operation missing from the probe.
  The `EACCES` was found by running it: on Podman's Fedora CoreOS machine (kernel 7.0.9), a container with seccomp
  unconfined still gets `EACCES` from `io_uring_setup` until its SELinux label is disabled too.
- **No liburing.** kama ships headers, not libraries, and the ring is three system calls and two mapped regions. The arm
  is written against `<linux/io_uring.h>` and `syscall(2)`, as Zig's is.

### Decision 7 — turning the ring off, and seeing which arm ran

io_uring's record is why every runtime that falls back also lets an operator turn it off: libuv turned its SQPOLL
ring off by default for behaviour that "wasn't providing consistent behaviour … depending on kernel versions, load
shape", and Node turned io_uring off by default in 20.11.1 after CVE-2024-22017. In an incident, an operator needs it
off for one process without a rebuild.

| runtime | turning the ring off for one process, without a rebuild |
|---|---|
| libuv / Node | `UV_USE_IO_URING`; Node defaults it off since 20.11.1 |
| monoio | `MONOIO_FORCE_LEGACY_DRIVER`, or the program picks a driver in code |
| Seastar | the `--reactor-backend=epoll` option every Seastar application takes |
| Netty | the program picks the transport in code |
| glommio, tokio-uring | none, and no fallback |
| any process | seccomp (a container's profile, a systemd filter), or the machine-wide `kernel.io_uring_disabled` (6.6) |

**Ruled 2026-10-28: `KAMA_IO_URING=0`** turns the ring arm off. It is read once, when a scheduler is made, and listed
in [targets.md](../targets.md) beside `KAMA_LOG`, std::log's filter, which is the runtime's existing environment
variable for its own behaviour. The default is on. Neither value changes what a program does, only which kernel
interface carries its I/O, as `KAMA_LOG` changes only what is printed. The first recommendation, no switch, said kama
had no such variable; `KAMA_LOG` is one.

**Which arm ran is visible to the OS**, with no new API: a process running the ring holds an `anon_inode:[io_uring]`
descriptor in `/proc/<pid>/fd`, and the server benchmark records which arm each kama run used from there. A runtime
that falls back silently could otherwise measure epoll while its author believes it measures io_uring.

### Decision 8 — the measurement that decides, before any kama code

**Recommended:** a C harness, `bench/uring/`, in the shape of `bench/poller/` (fibers.md §6): a single-threaded echo
server per arm, no fibers, the same client, the same 15-round interleaved protocol and noise rule (a difference counts
only when it beats the reference's run against itself in 12 of 15 rounds).
- **Arms:** `epoll-et` (today's protocol, including the short-read rule, the reference, run twice); `ring-poll` (a);
  `ring-op` (b); `ring-send` (b′); `ring-multishot` (c).
- **Loads:** 64-byte echo at 100, 1,000 and 10,000 connections, and the 4 KiB raw-HTTP reply at 1,000 and 10,000, on
  one server thread: what one core of servers.md's thread-per-core server does.
- **Measured:** server CPU per request (the thread, plus any io-wq worker the ring ran for it), system calls and ring
  submissions per request. Latency waits for a kama arm in the server benchmark, where the request path is real.
- **How:** `bench/run uring <out.tsv>`, then `python3 bench/uring/summarize.py <out.tsv>`, which applies the bar.
- **Where:** the maintainer's x86-64 KVM box (KR-250's) and the M4 VM. The bench container needs
  `--security-opt seccomp=unconfined` and `--security-opt label=disable` for any ring arm, since Podman's seccomp
  answers `ENOSYS` and Fedora's SELinux container type `EACCES`; `bench/run` already passes both for `WRAP` runs.
- **The bar:** an arm is built only if it beats `epoll-et` on CPU per request by at least 5% at 1,000 and 10,000
  connections, on both machines. Below that, a second Linux arm is maintenance without a return, and this row closes
  with the measurement. Ruled 2026-10-28 at 5%.

If (b) clears the bar, the kama build is: the arm in `kama_sched.h` (setup, probe, submission and completion), the
cancel-and-drain path, the fallback, and the server benchmark rerun with both arms.

### Decision 9 — every other wait goes through the ring too, as a poll

A stream's read and write are operations (decision 2). Everything else a fiber waits on is readiness: `accept`, a
`connect` resolving, a datagram, `sendFile`, the readiness poll std offers, and the remote waker's eventfd.

**Ruled 2026-10-28 as recommended:** on the ring arm, each of those is a one-shot `POLL_ADD` that the waiting fiber
submits. The waker is one
multishot `POLL_ADD` on its eventfd. A scheduler on the ring then has one wait, `io_uring_enter`, and no epoll set beside
it.
- The alternative keeps epoll for readiness and polls its descriptor through the ring. That is two queues in one
  scheduler, one nested in the other, to serve waits that are once per connection or off the request path.
- **The cost is ring-poll's, on these waits only:** a submission per wait, where an edge registration is made once. That
  is level at 1,000 connections and below, and 20% more CPU at 10,000 on one x86-64 core (§6). No stream read or write
  pays it. A datagram server is the case that would, and `RECVMSG`/`SENDMSG` through the ring is its own measurement
  (§7).

### Decision 10 — every submission belongs to a parked fiber that waits for its own completion

**Ruled 2026-10-28 as recommended:** the one rule that makes the ring safe, held by construction rather than by a
drain pass.
- An operation's record lives on the stack of the fiber that submits it, and its completion's `user_data` is that
  record's address. The fiber parks until the completion has arrived. Nothing else ends the wait: any other wake while
  the operation is in flight is absorbed, since a wake is a hint.
- A deadline that passes submits an `ASYNC_CANCEL`. Its own completion names no record and is ignored. The fiber keeps
  waiting for the original operation's completion, and returns the original result when the kernel finished first
  (decision 3).
- So a completion never names freed memory, and a buffer is never the caller's again while the kernel holds it. A
  scheduler ends only after its fibers have, so nothing is in flight when its ring closes. This covers a `POLL_ADD` too.
  It holds no buffer, but its completion names its waiter.
- A socket cannot close or move while a fiber is parked on it, because the parked fiber borrows it (decision 5). A
  debug build checks that no record is in flight when a registration is released.
- **A socket may have several submissions in flight at once.** KR-244's read and write halves park a `RECV` and a
  `SEND` on one descriptor together. Each is its own record on its own fiber's stack. A cancel names its record, never
  the descriptor, which would end the other half's operation too. The registration counts what is in flight rather
  than keeping a slot per direction, and the ring arm uses neither of its `reader` and `writer` slots.

### Decision 11 — a write parks, and its bytes leave at the turn's `io_uring_enter`

This is (b′)'s cost that decision 8 could not count. A `SEND` is submitted when its fiber parks, and the ring sees it at
the scheduler's next `io_uring_enter`. That happens when no fiber is ready, and every `KAMA_SCHED_POLL_EVERY` (61)
fiber runs while fibers are ready. So a reply can wait for up to 61 other fibers' runs. A handler that makes several
writes per response pays a park for each.

**Ruled 2026-10-28 as recommended:** build it as measured, and let the server benchmark judge it (§8 step 4). The kama arm runs on both arms
in the same rounds, and the ring arm must hold p99 latency within the epoll arm's noise at every config. If it does not,
the remedy is decided on that measurement. Options include entering the ring sooner while a turn runs long, or making
the system call first for a write too, which is (b) for writes. Neither is built ahead of the number.

## 6. Measurements (decision 8)

### aarch64: the M4 VM, kernel 7.0.9, 15 rounds (2026-10-28)

`bench/run uring`; raw rows in [benchmarks/uring-aarch64.tsv](../benchmarks/uring-aarch64.tsv). Server CPU per request
against `et`, kama's epoll protocol today, as the median of the paired ratios; every ring arm's ratio held in 15 of
15 rounds unless the cell says otherwise. `et#2`, `et` run again, is the noise floor: 0.99–1.04.

| arm | 100, 64 B | 1,000, 64 B | 1,000, 4 KiB | 10,000, 64 B | 10,000, 4 KiB | the 5% bar |
|---|---|---|---|---|---|---|
| ring-poll (a) | 1.00 (8/15) | 0.99 (9/15) | 1.01 | 1.05 | 1.04 | no |
| ring-op (b) | 0.88 | 0.89 | 0.90 | 1.06 (more, 15/15) | 1.04 | no: fails at 10,000 |
| ring-op-pf | 0.94 | 0.94 | 0.95 (12/15) | 1.04 | 1.02 | no |
| ring-send (b′) | **0.75** | **0.76** | **0.79** | **0.91** | **0.92** | **clears** |
| ring-send-pf | 0.81 | 0.82 | 0.82 | **0.87** | **0.86** | **clears** |
| ring-multishot (c) | 0.89 | 0.92 | 0.95 (14/15) | 1.03 | 1.01 | no |

What it says, on this machine:
- **Readiness through the ring buys nothing.** (a) is level with epoll everywhere, as the Loom prototype found.
- **The ruled candidate, (b), fails the bar.** A read through the ring saves 10–12% at 100 and 1,000 connections and
  costs 4–6% more at 10,000: each `RECV` arms a poll inside the kernel and tears it down, where epoll's registration is
  made once. Kernel-chosen buffers, (c), fail the same way.
- **Both directions through the ring, (b′), clears it everywhere,** 21–25% less CPU at 100 and 1,000 connections and
  8–9% at 10,000: the reply's `write` system call goes too, and every `SEND` of a turn rides one `io_uring_enter`.
- **`POLL_FIRST` helps at 10,000 connections and costs below it** (0.86–0.87 against 0.91–0.92 at 10,000; 0.81–0.82
  against 0.75–0.79 at 100 and 1,000), so it would be chosen by load, or not at all.

What the harness does not count, and what decides whether (b′)'s margin survives in kama:
- **A park per write.** A task here is a struct; a fiber that sends through the ring parks until the `SEND` completes,
  and a switch there and back costs 21.7 ns on this VM (fibers.md §6). With the scheduler's bookkeeping, say 25–50 ns
  a request, against (b′)'s 171 ns saving at 10,000 echo connections: about 6% left there, `-pf` about 10%.
- **Latency.** A reply through the ring leaves at the turn's `io_uring_enter`, not at the `write` call, so a turn of
  40 requests holds the first reply until the 40th is answered. The server benchmark measures that once a kama arm
  exists.
- **Batching.** The ring returns about 40 completions per `io_uring_enter` here, against about 1,000 events per
  `epoll_wait`, which looks like the kernel's cap on deferred task work run per call. At 0.025 calls a request it is
  not where the cost is, but a ring arm would wait with a minimum batch (`min_wait_usec`, 6.13) only if a measurement
  shows it pays.

### x86-64: an i9-13900K in a QEMU/KVM guest, kernel 6.17, 15 rounds (2026-10-28)

`bench/uring/run.sh` run natively, without the container. The raw rows are in
[benchmarks/uring-x86-64.tsv](../benchmarks/uring-x86-64.tsv). Each cell gives the median ratio against `et`, then the
rounds in which the arm used less CPU. `et#2` is 0.99–1.02. Rounds differ more here than on the M4: the CPU mixes
performance and efficiency cores, and the guest cannot see which one it is on.

| arm | 100, 64 B | 1,000, 64 B | 1,000, 4 KiB | 10,000, 64 B | 10,000, 4 KiB | the 5% bar |
|---|---|---|---|---|---|---|
| ring-poll (a) | 0.97 · 9 | 0.99 · 8 | 1.04 · 7 | 1.21 · 0 | 1.20 · 0 | no |
| ring-op (b) | 0.93 · 12 | 0.85 · 14 | 0.86 · 15 | 0.93 · 12 | 0.97 · 13 | no: 3% at 10,000, 4 KiB |
| ring-op-pf | 0.94 · 10 | 0.93 · 12 | 0.94 · 14 | 1.00 · 9 | 1.00 · 9 | no |
| ring-send (b′) | **0.79 · 14** | **0.76 · 14** | **0.81 · 14** | **0.93 · 13** | 0.98 · 12 | no: 2% at 10,000, 4 KiB |
| ring-send-pf | 0.82 · 15 | 0.87 · 13 | 0.90 · 14 | 0.91 · 12 | 0.92 · 11 | no: 11 rounds at 10,000, 4 KiB |
| ring-multishot (c) | 0.95 · 11 | 0.98 · 10 | 1.00 · 7 | 1.16 · 1 | 1.18 · 0 | no |

What it says, against the M4:
- **(b′) saves the same 19–24% at 1,000 connections and below.** At 10,000 on one core it saves 7% with 64-byte replies
  and 2% with 4 KiB, against 8–9% on the M4. The 4 KiB cell is the only one where it misses the bar on either machine.
- **Readiness through the ring costs more here:** 20–21% more CPU at 10,000 connections, where the M4 measured 4–5%.
  Kernel-chosen buffers cost 16–18% more there. Only operations through the ring pay.
- **(b) holds at 10,000 with 64-byte replies here (0.93),** where it cost 6% more on the M4.
- **`POLL_FIRST` has the same shape as on the M4:** it helps at 10,000 and costs at 1,000.
- **The densities bracket the real one.** The harness runs 10,000 connections on one thread. servers.md's thread-per-core
  run spreads 10,000 over 3 cores, about 3,300 a core, between the harness's two densities.

On both machines' numbers, decision 2 is re-ruled to (b′).

## 7. What is out of scope here

- **Regular files and DNS** stay on KR-3 step 7's offload pool, which every OS needs. io_uring reads regular files
  without blocking, and could serve them on Linux later; libuv moved its file operations back to the threadpool in
  1.49 after inconsistent results, so that too would be its own measurement.
- **No operation whose permission check reads the caller's credentials goes through the ring.** A ring keeps the
  credentials of the thread that made it, so after a `setuid()` its operations still run with the old ones: that was
  CVE-2024-22017, in libuv's file operations. `recv`, `send`, `accept` and `connect` on a descriptor already open run
  no such check. A later file arm (`open`, `unlink`, `rename`) would need the ring remade, or its operations
  tagged with the current credentials (`IORING_REGISTER_PERSONALITY`), when they change.
- **Windows** completion I/O (IOCP) is KR-3 step 8, for the operations readiness cannot serve.
- **Zero-copy send** (`SEND_ZC`, 6.0) pays for large sends, not 64-byte echoes.
- **Datagrams through the ring** (`RECVMSG`, `SENDMSG`). Decision 9 has a datagram socket wait with a poll. A datagram
  server, such as DNS or QUIC, would pay that poll on every packet, so moving its operations onto the ring is a
  measurement of its own.

## 8. The build

Each step is one commit, with fixtures on native Linux. The ring arm runs by default wherever the kernel allows it, so
the native suite on Linux runs the ring. A container leg falls back to epoll, because Podman refuses io_uring, so the
sanitizer leg runs epoll.
1. **The arm, readiness only** (`0.9.756`). `kama_sched` grows a ring beside the epoll arm, chosen when the scheduler is made
   (decisions 1, 6 and 7). The work in this step:
   - the probe, and `KAMA_IO_URING=0`;
   - the waker's multishot poll;
   - `kama__sched_wait` as `io_uring_enter` with `EXT_ARG` timeouts;
   - every `kama_sched_io_wait` as a one-shot `POLL_ADD`, under decision 10's rule.

   Std is unchanged, so the whole fiber corpus runs on the ring as shape (a). A new guard checks both arms:
   - `tools/check-io-uring.sh` runs the fiber and socket fixtures under `KAMA_IO_URING=0` as well.
   - On a kernel that allows the ring, it checks that a fiber program holds an `anon_inode:[io_uring]` descriptor, and
     that it holds none under `KAMA_IO_URING=0`.

   [targets.md](../targets.md) lists the variable.
2. **Stream reads as `RECV`** (decision 2, `0.9.757`). One C entry, `kama_net_read`, carries a stream read's whole protocol. Std's
   `TcpStream.read` and `UnixStream.read` call it in place of their loop. On the ring arm, a read that would block, or
   follows a short one, becomes a `RECV` into the caller's buffer. A passed deadline cancels it, and the fiber waits for
   the kernel to let go (decision 3), with a fixture for each.
   - A build with MemorySanitizer unpoisons what a `RECV` wrote, since the kernel's write is invisible to it.
3. **Stream writes as `SEND`**, through `kama_net_write`, with `MSG_NOSIGNAL` as `kama_send` sets it (`0.9.758`).
4. **The server benchmark on both arms, in the same rounds.** `kama-fibers` runs on the ring and `kama-fibers-epoll`
   under `KAMA_IO_URING=0`. The harness records which arm each run used, from `/proc/<pid>/fd`. The benchmark is rerun on
   the x86-64 box, natively, and on the M4 VM, which needs the container's seccomp and SELinux relaxed as `bench/run
   uring` does. It is judged on CPU per request, requests per second and p99 (decision 11). Then servers.md is updated,
   the row closes, and this file is deleted. Its record goes to SPEC.md, targets.md and servers.md.

## Sources

Read 2026-10-28:
- Kernel versions: liburing man pages `io_uring_setup.2`, `io_uring_enter.2`, `io_uring_prep_recv.3`,
  `io_uring_register_buf_ring.3` (github.com/axboe/liburing, `man/`).
- Seastar: `src/core/reactor_backend.cc` (github.com/scylladb/seastar, master): the backend order, and
  `read_some`/`accept` (speculate, poll, submit) against `recvmsg`/`sendmsg` (speculate, submit).
- glommio: `glommio/src/net/stream.rs`, `src/sys/uring.rs`, `src/sys/source.rs`, README (github.com/DataDog/glommio).
- monoio: `docs/en/io-cancel.md`, `docs/en/benchmark.md`, `monoio/src/utils/uring_detect.rs` (github.com/bytedance/monoio).
- tokio-uring: `DESIGN.md`, `src/runtime/driver/op/mod.rs` (github.com/tokio-rs/tokio-uring).
- Netty 4.2: `transport-classes-io_uring` (github.com/netty/netty, branch 4.2).
- libuv: `ChangeLog`, `src/unix/linux.c`, PR #4492 (github.com/libuv/libuv).
- Zig 0.16 release notes (ziglang.org/download/0.16.0/release-notes.html).
- Go: issue #31908 (github.com/golang/go/issues/31908).
- Loom: loom-dev, October 2022 (mail.openjdk.org/pipermail/loom-dev/2022-October/004984.html), and the `io_uring`
  branch's `loom-docs/io_uring.md`.
- Google: "Learnings from kCTF VRP's 42 Linux kernel exploits submissions", June 2023 (security.googleblog.com).
- Docker: moby PR #46762, merged 2023-11-02 for 25.0.0. Podman: `containers/common` `pkg/seccomp/seccomp.json`
  (no `io_uring` entry, `defaultErrnoRet` 38). containerd: PR #9320, in 2.0.0. gVisor: compatibility guide.
- Benchmarks: frevib/io_uring-echo-server `benchmarks.md`; liburing issue #536; liburing wiki "io_uring and networking
  in 2023"; withoutboats, "Notes on io-uring" (without.boats/blog/io-uring).
