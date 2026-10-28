# An io_uring backend for the fiber scheduler — design (KR-253)

**Status: ruled 2026-10-28.** Decisions 1–6 and 8 as recommended (8 at 5%); decision 7 as revised, after the first
recommendation rested on a false claim (§5). Next is decision 8's harness. A design doc and a measurement come before
any code, as [ROADMAP_DETAIL §9](../ROADMAP_DETAIL.md#s9-io-uring) asks. The measurement is decision 8, and it can close
the row: if no arm clears its bar, kama keeps epoll on Linux and this file records why, as Go's maintainers concluded
for Go. This file builds on [fibers.md](fibers.md), whose decisions it cites by number, and is deleted when the work
ships or closes. Facts about other runtimes were read from their source or their maintainers' own words on 2026-10-28
(*Sources*). Kernel versions are from the liburing man pages. Every proposed spelling is a `kama fragment`.

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
  sysctl), `EINVAL` (a kernel without the flags), or an operation missing from the probe.
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
- **Loads:** 64-byte echo at 100, 1,000 and 10,000 connections, and the 4 KiB raw-HTTP reply, on 3 server cores, as
  servers.md.
- **Measured:** requests a second, server CPU per request, system calls per request, p99.
- **Where:** the maintainer's x86-64 KVM box (KR-250's) and the M4 VM. The bench container needs
  `--security-opt seccomp=unconfined` for any ring arm, since Podman refuses io_uring otherwise; `bench/run` already
  passes it for `WRAP` runs.
- **The bar:** an arm is built only if it beats `epoll-et` on CPU per request by at least 5% at 1,000 and 10,000
  connections, on both machines. Below that, a second Linux arm is maintenance without a return, and this row closes
  with the measurement. Ruled 2026-10-28 at 5%.

If (b) clears the bar, the kama build is: the arm in `kama_sched.h` (setup, probe, submission and completion), the
cancel-and-drain path, the fallback, and the server benchmark rerun with both arms.

## 6. What is out of scope here

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
