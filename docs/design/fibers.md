# Fibers and thread-per-core — design (KR-3)

**Status: ruled 2026-10-22, all fifteen decisions as recommended, each to be verified as it lands.** Nothing here is built yet; the lock window it builds on shipped first (`Locked<T>`, `@parks` and the walk, `0.9.730`–`0.9.733`, SPEC *Shared mutable state*). This is the in-flight campaign document the
[ROADMAP_DETAIL](../ROADMAP_DETAIL.md#s6-fibers) table names: it is deleted when the work ships, once
[SPEC.md](../SPEC.md) carries the record. The numbered decisions are listed again, one line each, in
ROADMAP_DETAIL §6, where their rulings are recorded. Facts were read from the tree at `0.9.719` and from primary
sources on 2026-10-22 (§ *Sources*). Every proposed spelling is a `kama fragment`: none of it compiles today.

## 1. The problem

A server holding 10,000 connections today needs either 10,000 isolates (OS threads, 8 MiB of stated stack each) or a
state machine written by hand over `Poller`. A hand-written state machine is what Rust's `async` makes the compiler
write, and what every Rust user then pays for in `Pin`, `Send + 'static` bounds and coloured functions.

What is missing is **cheap suspension**, not threads. A fiber is a stack the runtime switches to and from in user
space. A fiber blocked on a socket costs its stack's resident pages and a control block, not an OS thread, and the
code it runs is ordinary blocking-shaped kama.

```kama fragment
fn void handle(TcpStream conn) {              // the code a fiber runs is the code a thread would run
    while (true) {
        match (conn.read(into: buf.view())) {     // parks this fiber, and the core runs another
            case Ok(value: n): { if (n == 0) { return; } … }
            case Err(error: e): { return; }
        };
    }
}
```

## 2. What the tree has today (`0.9.719`)

| Piece | State | Where |
|---|---|---|
| Isolate | a pthread with a stated stack (8 MiB on 64-bit, 2 MiB on 32-bit, 256 KiB on wasm); `spawn(stack: n)` | `include/kama_isolate.h:28-62` |
| `scope` | joins every bare `spawn` at its closing brace; children are counted by reading the block | `emitScopeCleanup`, `src/kama.cemit.cpp:6650` |
| Isolate-local state | `KAMA_ISOLATE_LOCAL` = `_Thread_local`. It holds the `Shared` access record (`kama_accesses`), the `@onPanic` chain (`kama_recover_top`), `kama__panicking_here`, and every mutable module `static` | `include/kama_runtime.h:135-139`, `:441`, `:448`, `:585` |
| `Shared` exclusivity | an intrusive list of nodes on the stack. `kama_access_end` pops the HEAD and assumes strict LIFO | `kama_runtime.h:2372-2407` |
| Panic | aborts the whole process; no unwinding. `@onPanic` is a setjmp region that must be `@noheap` | `kama_runtime.h:587-616` |
| Channel | pthread mutex + two condvars. `Receiver.wakeOnSend(waker:)` holds ONE waker | `include/kama_channel.h:60-377` |
| Poller | `add`/`remove`/`wait`/`waker`, level-triggered on epoll, kqueue, AFD over IOCP, and `poll(2)` | `lib/std/net/poll.kama`, `include/kama_os.h:2944-3674` |
| Sockets | `setNonBlocking(true)` turns `read`/`write`/`accept` into `Err(WouldBlock)` | `lib/std/net/net.kama` |
| Stack switching, guard pages, SIGSEGV handler | **none**: no asm, no ucontext, no `mmap` in the runtime. An isolate has only pthread's default guard page | — |
| wasm | emscripten's main loop; no Asyncify, no JSPI | `include/kama_app.h:20-41`, `src/kama.driver.cpp:13711-13792` |

## 3. What others do

| | Unit | Scheduler | Colouring | Stacks | Cross-thread | What the user pays |
|---|---|---|---|---|---|---|
| Go | goroutine | M:N, work-stealing | none | growable, 2 KiB minimum | anything (shared memory) | a GC; data races are possible |
| Java 21+ | virtual thread | ForkJoinPool, work-stealing, FIFO (JEP 444) | none: the same blocking APIs park | heap chunks, copied | anything | races; pinning in native frames (JEP 491 removed it for `synchronized`) |
| Erlang | process | per-core, preemptive by reductions | none | growable heaps | messages only | a VM |
| OCaml 5 Eio | fiber, by effects | one per domain, **no stealing** | none (direct style) | runtime-managed | explicit `Executor_pool` | — |
| Seastar (C++) | future; `seastar::thread` is stackful, 128 KiB | one reactor per core, shared-nothing | futures | fixed | messages | callback-shaped code |
| glommio, monoio (Rust) | task | thread-per-core | `async` | stackless | none | a niche API |
| tokio (Rust) | task | work-stealing | `async` | stackless | `Send + 'static` | `Pin`, colouring, an ecosystem split per executor |
| Zig 0.16 | `Io.async` | `Io.Threaded` shipped; `Io.Evented` (fibers, M:N) experimental | none, but **every I/O function takes an explicit `Io`** | fixed | — | an `Io` parameter threaded through every signature |
| Swift, C#, Kotlin | task / coroutine | pools | `async` / `suspend` | stackless | `Sendable` (Swift) | colouring |

**Recommended: thread-per-core, shared-nothing between cores, cooperative fibers within one.** Each isolate runs a
scheduler over its own `Poller`. A fiber never leaves its isolate, so it needs no `Sendable` bound, has no colouring
and no `Pin`, and freedom from data races keeps holding by construction. This is Eio's shape (one scheduler per
domain, no stealing) and Seastar's (one reactor per core) with Loom's ergonomics (the blocking API itself parks).
Neither of those has kama's guarantee that nothing crosses a core without `Sendable`.

## 4. The model, end to end

One isolate per core. Each accepts on its own listener, or receives sockets from an acceptor isolate where the OS
does not balance a shared port. Each runs its connections as fibers.

```kama fragment
type resource Core implements Sendable {
    TcpListener listener;
    public ctor make(TcpListener listener) { this.listener = give listener; }
    public unsafe fn void serve() {
        scope {                                            // this isolate runs a scheduler until its fibers end
            while (true) {
                match (this.listener.accept()) {           // parks the accepting fiber, not the core
                    case Ok(value: c): { spawn fiber handle(conn: give c); }   // a fiber child, reaped when it ends
                    case Err(error: e): { … }
                };
            }
        }
    }
}

fn uint8 main() {
    DynamicArray<Core> cores = …;                          // one per cpuCount(), each with a `balance` listener (Linux)
    scope { parallel_spawn (ref Core c in cores) { c.serve(); } }
    return 0;
}
```

A client of a database or a cache is the other named case. It keeps one connection per core, multiplexed, and its
callers are fibers on that core's scheduler, so no request crosses an isolate at all.

What each piece costs (the decisions below derive these):

| Thing | Cost |
|---|---|
| a fiber | a 256 KiB stack **reservation** (decision 2), of which a parked connection keeps 1–2 pages resident (4 KiB on x86-64 and most Linux arm64, 16 KiB on Apple silicon), plus a control block of about 128 B |
| a park on a socket | the failed `read` that reported WouldBlock, and nothing more on epoll and kqueue; one IOCTL on Windows (decision 6, measured) |
| a context switch | about 10 ns each way with a hand-written switch (arm64, measured in §6); `swapcontext` costs 250–360 ns, because it makes a system call every time |
| a cross-core wake | one `Waker.wake()`: a system call only on the clear-to-set transition, coalesced (`0.9.719`) |

## 5. Decisions

Each decision gives what others do, the production answer, how it squares with GOALS.md, and whether it follows the
tree's existing practice.

### Decision 1 — the server model, and where it lives

**Recommended:** thread-per-core with fibers (§3), **in std**, as part of the isolate itself rather than a separate
object. The first `spawn fiber` on an isolate makes it a scheduler. The `scope` that holds the spawn runs that
scheduler on its own stack (fiber 0) until its fiber children end. The Poller, the timer heap and the ready queue
are isolate-local runtime state, created on first use.

- **Packaging.** Go, Erlang and Java put the scheduler in the runtime. Rust puts it in crates, which costs an
  ecosystem split per executor. Python's `asyncio` froze a wrong design in its stdlib, and Zig deleted
  `std.event.Loop` once.
- **Why std is safe here.** kama's surface is the language's own `spawn` and `scope` plus the blocking std APIs it
  already has. There is no executor API to freeze, which is what made `asyncio` a trap.
- **GOALS.** #4: one way to start a child (`spawn`) and one to bound it (`scope`), with no `Runtime` object to
  configure. #5: the spawn site says `fiber`.
- **Practice.** This matches `scope`/`spawn`: the language owns structure and std owns the seam (`std::concurrent`
  brings in the isolate seam today).

### Decision 2 — the context switch, and the stacks

| | Switch | Windows TIB / `__chkstk` / CET | Notes |
|---|---|---|---|
| ucontext | `swapcontext` | n/a | glibc makes an `rt_sigprocmask` **system call** per switch. macOS's header marks it "DEPRECATED … should not be used"; musl does not ship it; Zig 0.16 removed its bindings |
| Boost.Context | per-arch asm | x64 asm swaps StackBase, StackLimit, DeallocationStack and FiberData, and saves XMM6–15 | CET shadow stacks on Linux only |
| Windows Fiber API | `SwitchToFiber` | the OS does it all, and FLS switches with the fiber | the UCRT's `errno` lives in FLS, so it becomes per fiber |
| Go | own asm | own | growable stacks need precise frame maps, which C cannot give |

**Recommended:**
- **Own asm per architecture on POSIX:** x86-64 SysV, arm64 AAPCS64 (with x18 left alone on Apple), riscv64 and
  i686. Each is about 30 instructions, saving only callee-saved registers. It is original code, as the AFD backend
  is: Boost.Context's license wants its notice in every source distribution, and `kama_runtime.h` ships as source.
- **The OS Fiber API on Windows.** It is the platform's own answer: TIB fields, `__chkstk` probes, CET and FLS are
  all correct by construction, and MSVC has no x64 inline asm anyway.
- **An architecture with no switch is refused** with a clear diagnostic at the `spawn fiber` site, as kama refuses
  an unmodelled arch for a comptime `sizeof`.
- **CET (Linux x86-64), BTI and PAC (arm64)** each get a `__CET__` / `__ARM_FEATURE_BTI_DEFAULT` arm in the asm, so
  a hardened distro's default `-fcf-protection` still links.

**Stacks:**
- Each stack is an `mmap` reservation with one PROT_NONE guard page below it, committed only as touched
  (`MAP_NORESERVE` on Linux). The default is **256 KiB**. `spawn fiber(stack: n)` states another size, exactly as
  `spawn(stack: n)` does, and it is multiplied ×3 under ASan, whose frames are larger.
- A guard-page hit runs on a `sigaltstack` handler, which prints "fiber stack overflow (256 KiB; `spawn
  fiber(stack: …)` to raise it)" and aborts, where today an isolate overflow is a bare segfault. The same handler
  can serve isolates.

**The cost a reader should see:**
- **Mappings.** A guard page costs two kernel mappings per fiber. Linux's `vm.max_map_count` defaults to 65,530,
  so a process holds about **32,000** guarded fibers unless the operator raises it, as Elasticsearch's install docs
  require.
- **Address space.** On 32-bit, 10,000 × 256 KiB does not fit.

The alternative, a stack-limit check emitted in every kama prologue (Go's), costs no mappings but leaves C frames
unprotected, and its cost is unmeasured. Recommended as a follow-on measurement, not the default.

**The bytes of a switch** (§6): a hand-written arm64 switch saves 20 registers into a 160-byte frame and costs about
21 ns for a round trip on both macOS and Linux. `swapcontext` costs 715 ns for the same round trip on macOS and 497 ns
on Linux, about 25–35 times as much, because each switch is a system call that sets the signal mask.

**GOALS.** #3: allocation is predictable, and a stack is reserved where `spawn` says. #5: the size is stated.
**Practice:** platform deltas live in per-platform implementations behind one interface.

### Decision 3 — exclusivity with fibers: no park inside a lasting access

The brief said "make exclusivity per fiber". That is inverted. Fibers on one isolate share `Shared<T>` values on
purpose, which is the point of having no `Sendable` bound. A separate access list per fiber would hide fiber B's
write from fiber A's open `borrow` window, which is exactly the bug the check exists to catch. And sharing one list
between fibers breaks `kama_access_end`'s LIFO pop as soon as two of them interleave.

| | What happens when a task holds a borrow across a suspension and another task writes |
|---|---|
| Rust | `RefCell` across `.await` panics at run time (`BorrowMutError`); `clippy::await_holding_refcell_ref` and `await_holding_lock` are lints |
| Swift | actor reentrancy: state may change across an `await`, unchecked |
| Go, Java | a data race; the race detector may see it |

**Recommended: a compile-time rule.** Inside a lasting access to `Shared` storage, no call may reach a park. That
covers a `borrow`/`foreach`/`match` window whose host is reached through a `Shared`, and a call whose `ref` receiver
or argument points into one.
- **"Reaches a park"** is the reachability walk `--no-heap` already runs (`checkNoHeapTransitive`; indirect calls at `rejectNoHeapIndirect`), with
  a different leaf. The leaf is the std seam marked `@parks`: socket I/O, channel `send`/`recv`, `join`, `sleep`,
  and offloaded file I/O and DNS. Taking a lock (`Locked<T>`) is not a park: it blocks the thread only for a critical
  section, which the lock-window rule keeps from waiting on anything.
- **A contract call** gets an edge to every implementer, as `--no-heap` has done since `0.9.659`.
- **A `fnptr` or bound-function call** counts as "may park" unless its signature promises otherwise, as `@noheap
  fnptr` does.
- **A foreign call does not park.** It blocks, which is decision 8's subject.
- **Only code a fiber entry reaches is held to the rule**, so a program with no `spawn fiber` loses nothing.

What this buys:
- **No conflict across a park is possible.** That is the iterator-invalidation guarantee kama already gives,
  extended to suspension, and it is static, as kama's view and window rules are.
- **The context switch saves no runtime state at all.** At every park the parking fiber has no open access and no
  `@onPanic` region (decision 15 forbids one across a park), so `kama_accesses` and `kama_recover_top` need no swap.
  A debug build asserts both are empty in the park primitive, the static rule's runtime depth, as the mods counter
  backs iterator invalidation.
- **The fd-lifetime bug of decision 13 cannot be written:** a parked fiber always owns, or exclusively borrows, the
  socket it waits on.
- **The same walk is the lock window's** (`checkLockWindows`), whose second leaf is another lock's acquisition.

**The price:** a broadcast written as `foreach (ref Sub s in subs) { s.stream.write(…) }` over a `Shared` list is
refused, and the diagnostic says to take the handles out first. That is the pattern Rust users reach with a lint,
made a rule. The rest of the swap set is exact:
- `kama__panicking_here` is the thread's own and is not swapped.
- `errno`, `GetLastError` and `WSAGetLastError` are read by std before any park (the seams already read them right
  after the call).
- Module `static`s stay per isolate. A `static` takes a compile-time initializer (SPEC *Comptime*), so it holds no heap
  value, and sharing one among fibers is like sharing a JavaScript global.

**GOALS.** #3a–3e: safety by construction without lifetimes, by a walk over a graph the compiler already builds.
**Practice:** `--no-heap`'s engine with a second predicate. That is the first concrete instance of KR-79 (declared
reachability properties), which names "no blocking" as one of its predicates.

### Decision 4 — wasm

The brief's premise, that a wasm fiber means Asyncify, no longer holds.

| Mechanism | State on 2026-10-22 |
|---|---|
| JSPI (JavaScript Promise Integration) | **Standardized** (phase 5). Unflagged in Chrome 137 (May 2025), Firefox 153 (July 2026), Safari 27 (September 2026), Node 25, and backported to Node 24.20 LTS |
| Wasm stack switching (WasmFX) | phase 3. Off by default in V8 and Wasmtime (x86-64 Linux only); no engine enables it |
| emscripten `fiber.h` | still requires `-sASYNCIFY=1` (PR #27638 to support JSPI is open). `-sJSPI` is no longer experimental (6.0.8) |
| Asyncify | rewrites every function that may suspend: the colouring cost SPEC rejected |

**Recommended:** fibers on wasm over JSPI, as the second implementation phase after native.
- **Each fiber is one call into a `WebAssembly.promising` export**, which V8 and the others run on its own stack.
- **A park is a call to a `WebAssembly.Suspending` import** that returns a Promise, resolved by the browser event
  the fiber waits on: a WebSocket message, a timer, a channel send.
- **The browser's event loop is the scheduler**, as SPEC already says it is the selector.
- **The same blocking-shaped std surface then works on wasm:** `ReliableStream.read` parks, in place of the app
  loop's polling.
- **The floor:** a program that spawns fibers on wasm needs a JSPI engine (the versions above). A program that does
  not is unaffected.
- **Not emscripten's `fiber.h`**, which needs Asyncify. kama drives JSPI through its own JS library, as
  `kama_net_web.js` already does for networking.
- **SPEC's *Why not green threads* is rewritten** with these facts.

**GOALS.** #10 and the platform rule: a platform gets the same primary interface rather than the common subset.
**Practice:** wasm networking is already a kama JS library (`lib/std/net/web/kama_net_web.js`).

### Decision 5 — one scheduler for jobs and I/O, or two

| | I/O tasks | CPU work |
|---|---|---|
| Go | one M:N scheduler with stealing for both | |
| Java | virtual threads on a ForkJoinPool | parallel streams on the common pool, a second instance |
| tokio | its runtime | `spawn_blocking`, and rayon for CPU |
| Seastar | the reactor | the same reactor; long work yields by a task quota |
| Eio | per-domain fibers | `Executor_pool` |

**Recommended: two.**
- **Fibers never migrate.** They hold non-`Sendable` state, so per-isolate fiber schedulers cannot steal from each
  other by construction.
- **CPU fan-out is a separate job pool** sized by `cpuCount()`. Its unit is a `Sendable` job, so it can steal.
- **A fiber that wants CPU work submits a job and parks on its result**, a channel the job completes, so a long
  computation never stalls its core's other fibers.
- **The cost, stated:** I/O isolates plus a job pool can run up to twice as many threads as cores. Uneven
  long-lived connections stay where they were accepted, which is Seastar's and Eio's accepted cost.
- **Jobs must not park,** by the decision-3 walk: a job entry that reaches `@parks` is refused. That is head-of-line
  blocking ruled out at compile time, where a fixed pool's 1/K stall would otherwise be.

`parallel_for` stays the data-parallel layer. The job pool is the task-parallel one, and its API is its own commit
in the implementation map.

### Decision 6 — how the scheduler registers a socket (measured)

**This is an internal choice per backend, not an API decision.** The public `Poller` stays level-triggered, as
ruled on 2026-10-21. The scheduler registers through an internal path, and every arm obeys one rule: park only after
WouldBlock, treat a wake as a hint, retry the I/O.
- **AFD (Windows) is natively one-shot.** mio emulates edges there by re-arming when the I/O call reports
  WouldBlock, which is the same one IOCTL per park, so there is nothing to choose.
- **`poll(2)`** rebuilds its set on every wait.

That leaves epoll and kqueue, measured in §6 with `bench/poller/`.

**Recommended, from the measurement in §6:**
- **epoll: edge-triggered.** Register `EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET` once, when the socket meets the
  scheduler, as Go and mio do. A park then costs no system call. One-shot costs 8–13% (an `epoll_ctl` per park), and
  level costs 5% at 10,000 connections, in the kernel's second poll of every reported socket.
- **kqueue: `EV_CLEAR` on READ once, WRITE only while a writer is parked, and every change batched into the next
  wait.** Two differences cleared the noise:
  - a re-arm made by its own `kevent()` costs 6% a park;
  - `EV_CLEAR` on both filters, Go's and mio's, loses 3% at 10,000 connections to one unwanted WRITE report per
    request.

  READ-only edge ties lazy level and beats one-shot (`EV_DISPATCH`) by about 2%.
- **AFD: one-shot,** re-armed at park after WouldBlock, which costs one IOCTL per park, as mio does.
- **`poll(2)`:** rebuild per wait.

The public `Poller` keeps applying its own `add` at once; that is one call per socket, not per park.

### Decision 7 — how a call knows it is on a fiber

| | |
|---|---|
| Loom, Go, Eio | **transparent:** the same blocking API parks when called on a virtual thread or fiber |
| Zig 0.16 | **explicit:** every I/O function takes an `Io`; the threaded or evented implementation is chosen by what is passed |
| Rust, Swift, C# | colour: `async fn` |

**Recommended: transparent.**
- **One mechanism.** An isolate running a scheduler holds an isolate-local scheduler pointer. A std socket used
  there is made non-blocking internally, and its `read` loops on WouldBlock → park → retry.
- **One way to write I/O (#4).** `TcpStream.read` has one meaning, "returns when there is data", on a thread or a
  fiber. Library code is written once.
- **Zig's choice would break std.** An explicit `Io` threads a parameter through every I/O signature in std, a
  source break for every program, to state something the spawn site already states.
- **Explicitness (#5) lives where the behaviour differs:**
  - the spawn site says `fiber`;
  - a socket the program itself set non-blocking keeps answering `WouldBlock`, for a program that drives its own
    `Poller`;
  - the park points are named in std by `@parks`, so `grep @parks` lists every one.

### Decision 8 — blocking that is not a socket

- **Channels become fiber-aware.**
  - A waiter list replaces the one waker slot: a fiber, or a thread's condvar.
  - A send-side wait covers a full bounded channel, and rendezvous follows.
  - A send from another isolate pushes the woken fiber onto its scheduler's remote queue and wakes that scheduler's
    Poller, so the `Waker` (`0.9.719`) becomes the scheduler's own.
  - Two fibers on one isolate may then talk over a channel, which deadlocks the core today.
- **Joining parks.** `scope`'s join, an `Isolate` drop and `Fiber.join()` park the joining fiber on a waker. They
  never call `pthread_join` on a scheduler thread.
- **Regular files and DNS go to an offload pool.** `read` of a regular file never reports WouldBlock on any OS, and
  `getaddrinfo` blocks for seconds. Both run on a small per-process pool while the fiber parks; libuv and tokio do
  the same. This is where KR-7 (a non-blocking `File`) and KR-202 (a DNS lookup with a deadline) land.
- **A foreign C call that blocks stalls its core.** That is stated plainly, as Java states pinning in native
  frames. The answer is an explicit `std::concurrent::offload(…)` that runs it on the pool, as tokio's
  `spawn_blocking` does. It is its own commit.

### Decision 9 — the spawn spelling, and failure

**Spelling.** One way to start a child: `spawn`, which kama already has. A fiber says so.

| Option | Example |
|---|---|
| **a contextual word after `spawn` (recommended)** | `spawn fiber handle(conn: give c);` · `Fiber<Reply> f = spawn fiber(stack: 64 * 1024) fetch(req: give r);` |
| a clause | `spawn(here) handle(conn: give c);` |
| a std function over a `fnptr` | `fibers.start(entry: handle, bundle: give c)`, blocked by KR-107 (a `fnptr` is not a type argument) |

- **The entry and the bundle.** The entry is the isolate's: a top-level `fn` and exactly one bundle. The bundle may
  be any movable value, because it never leaves the isolate, or a `ref` the scope outlives.
- **`fiber` is contextual,** meaningful only after `spawn`, so it reserves nothing. It is greppable as `spawn fiber`.

**Failure.**
- An isolate entry returns `void` because "there is no channel to hand a result back over". A fiber shares the
  thread, so that reason is gone.
- **Recommended:** a fiber entry may return a value, the handle form is `Fiber<R>`, and `join()` parks and yields
  `R`. A bare spawn in a scope takes a `void` entry; a discarded `Result` is already refused.
- **A failed child does not cancel its siblings by default.** In an accept loop, one bad connection must not stop
  the others. Group cancellation is decision 11.

**A panic still aborts the process.** There is no per-fiber fault isolation, as with Go's unrecovered panic and
Rust's `panic=abort`. Supervision belongs to the process level: a restart under systemd, or a supervising parent
process. `@onPanic` cannot wrap general code.

### Decision 10 — an unbounded set of children

A bare `spawn` must be a direct statement of its `scope` (`tests/xfail/spawn_in_nested_block`), so a scope's children
can be counted by reading it. SPEC justifies that by "roughly one isolate per core, not one per work item", which
is the opposite of the fiber case.

| | |
|---|---|
| Trio | a nursery object, passed to whoever may spawn into it |
| Swift | `withTaskGroup { group.addTask … }` |
| Kotlin | `coroutineScope { launch … }` |
| Go | `errgroup`, or nothing |

**Recommended:**
- **`spawn fiber` may appear anywhere inside its `scope`'s body**, loops included. The scope keeps a runtime list of
  live fiber children, and a child that ends unlinks itself and returns its stack to a per-isolate free list, so
  an accept loop's memory is bounded by its live connections.
- **The closing brace parks until the list is empty.**
- **A `ref` bundle must still root in a place the scope outlives**, so never in a loop body's local, which the
  existing place check extends to cover.
- **Isolate `spawn` keeps its counted rule,** because the reason for it still holds for isolates.

The scope stays the one structured construct, so kama needs no nursery object.

### Decision 11 — cancellation and deadlines

- **A per-scheduler timer heap.** "Timers need nothing new" was wrong: one `Poller.wait(timeoutMs)` serves one
  sleeper, not ten thousand.
  - Each scheduler keeps a min-heap of deadlines, and the wait's timeout is the earliest one.
  - Linux uses `epoll_pwait2`, nanosecond resolution, since 5.11, with millisecond `epoll_wait` before.
  - On Windows, `GetQueuedCompletionStatusEx` resolves to the ~16 ms system timer. Go fixed exactly this in 2024
    (commit cf52e70: a high-resolution waitable timer associated with the IOCP through `NtAssociateWaitCompletionPacket`,
    1,258 µs → 251 µs for a `Sleep(…)` benchmark). kama does the same.
- **Deadlines are per operation.** `std::concurrent::sleep(d)`, and a deadline on socket reads and writes (std has
  only `setUserTimeout` today). An expired one wakes the fiber with `Err(TimedOut)`.
- **Cancellation is cooperative and structured.** Cancelling a scope (an explicit call, or the first child `Err`
  when the scope asks for that) marks its fibers. Each one's current and every later park returns
  `Err(IoError{Cancelled})` until it ends. `IoErrorKind` is `@nonExhaustive`, so `Cancelled` is additive.
- **A fiber in a pure CPU loop is not interrupted.** It is cooperative, as in Trio and Kotlin.

### Decision 12 — fairness and preemption

**Recommended: cooperative, with three fairness rules.**
- **I/O is polled every N runs even while fibers are ready.** Go checks its global queue every 61 scheduler ticks
  so that two goroutines respawning each other cannot starve it; tokio's coop budget is 128 per task. N starts at
  61 and is measured.
- **`std::concurrent::yield()`** for a long loop.
- **No preemption:** Go's signal-based asynchronous preemption needs frame maps C does not have.

A watchdog that reports a fiber running too long cannot print a warning, because kama has no stderr warning channel
(a standing ruling). It is a trace event, or nothing.

### Decision 13 — descriptor lifetime

With decision 3, the case that bites Go, "fiber B closes a socket fiber A is parked on, and its number is reused",
cannot be written: a socket reachable only through a `Shared` cannot be read across a park. What remains is
implementation, and it is listed so that it is not rediscovered:
- **epoll watches the open file description, not the number.** A descriptor `kama_desc_dup` made keeps a closed
  one's registration alive, so the close path always runs `EPOLL_CTL_DEL` first.
- **A token names a registration record** whose free is deferred past the current batch of events, so a stale event
  in the same wait never reaches reused memory.
- **A socket `give`n to another isolate is deregistered first.** The `Sendable` hand-off goes through std, which
  can do it.
- **Windows completion I/O needs `WSA_FLAG_OVERLAPPED`.** Every socket `kama__socket` makes has it, but two paths do
  not: a duplicated descriptor (`kama_desc_dup`) and one received from another process. Those need the flag before
  step 8 of the map does overlapped I/O on them.

### Decision 14 — tooling: sanitizers and the debugger

- **ASan:** `__sanitizer_start_switch_fiber` before each switch and `__sanitizer_finish_switch_fiber` after, with
  NULL on a fiber's final switch "so that the fake stack is destroyed".
- **TSan:** `__tsan_create_fiber`, `__tsan_switch_to_fiber` and `__tsan_destroy_fiber`.
- **Valgrind:** `VALGRIND_STACK_REGISTER`.
- Without these, `./dev test san` and the TSan runs fail or lie, so they are in the first runtime commit, not a
  follow-on.
- **The debugger.** A parked fiber is invisible to lldb, because only threads are. IDE debugging is a stated goal,
  so `kama_lldb.py` gains an lldb OS plugin that lists each parked fiber as a thread from its saved registers, and
  the entry trampoline zeroes the frame pointer and link register so a backtrace ends cleanly.

### Decision 15 — MCU, `@noheap`, `@onPanic`

- **Parking allocates nothing.** The waiter record lives on the fiber's own stack.
- **Spawning allocates a stack.** Under `--no-heap` it draws from a static pool sized at build time.
- **No Poller on bare metal.** A wake comes from an interrupt handler through an ISR-safe ready queue. That is a
  later row beside KR-19's MCU packaging; the runtime interface keeps room for it (the Cortex-M switch is about ten
  instructions).
- **No park inside an `@onPanic` region.** The same walk as decision 3 enforces it: a `longjmp` across a fiber
  switch would land on another stack.

## 6. Measurements

### Decision 6: one park, by registration

The harness is `bench/poller/` (`run.sh`, `summarize.py`). It is a single-threaded C server that runs the park
protocol under each arm, with no fibers: try the read, get WouldBlock, park, wake, retry. Three client threads drive
it over loopback TCP. It counts every system call the server thread makes and times the server thread's CPU
(`CLOCK_THREAD_CPUTIME_ID`).

The protocol:
- 15 rounds of fresh processes, with the arm order rotated each round.
- The reference arm runs twice. Its gap to itself is the noise a real difference must clear.
- A difference counts only when it beats the reference's own second run, in ≥ 12 of 15 paired rounds.

**Workloads:**
- **pingpong:** one 64-byte request per round trip, so a task parks once per request.
- **pipeline:** 16 requests written at once and a yield after each, so a socket stays readable while its task is
  not parked.

`read` and `write` calls are the same in every arm and are left out.

**kqueue** (macOS 26.4, Apple M4, 15 rounds). The reference is `clear`, and `clear#2` is its noise run.
"Syscalls" are waits plus registration calls per request. Changes are kevent change records; a batched one rides
in the next wait's call.

| workload | arm | CPU ns/req, min · median | syscalls/req | changes/req | unwanted reports/req | vs `clear`, paired median | slower in |
|---|---|---|---|---|---|---|---|
| pingpong, 100 | `clear` (`EV_CLEAR`, READ and WRITE once) | 2,874 · 3,043 | 0.015 | 0 | 1.000 | — | — |
| | `clear#2` | 2,891 · 3,007 | 0.015 | 0 | 1.000 | 0.996 | 7/15 |
| | `lt` (level; disable on an unwanted report, enable at park) | 2,884 · 3,038 | 0.016 | 0 | 0 | 0.991 | 5/15 |
| | `dispatch` (`EV_DISPATCH`, re-enabled in the next wait) | 2,874 · 3,087 | 0.015 | 1.000 | 0 | 1.014 | 10/15 |
| | `dispatch-now` (re-enabled by its own `kevent()`) | 3,084 · 3,212 | **1.013** | 1.000 | 0 | **1.059** | **12/15** |
| pingpong, 10,000 | `clear` | 3,343 · 3,511 | 0.605 | 0 | 1.022 | — | — |
| | `clear#2` | 3,311 · 3,544 | 0.595 | 0 | 1.022 | 1.005 | 8/15 |
| | `lt` | 3,152 · 3,298 | 0.421 | 0 | 0 | **0.968** | **1/15** |
| | `dispatch` | 3,241 · 3,383 | 0.406 | 0.990 | 0 | 0.959 | 4/15 |
| | `dispatch-now` | 3,398 · 3,549 | 1.377 | 0.990 | 0 | 1.023 | 10/15 |
| pipeline, 100 | `clear` | 1,784 · 1,918 | 0.010 | 0 | 1.045 | — | — |
| | `clear#2` | 1,800 · 1,872 | 0.010 | 0 | 1.047 | 0.964 | 5/15 |
| | `lt` | 1,763 · 1,935 | 0.010 | 0.001 | 0 | 1.006 | 8/15 |
| | `dispatch` | 1,759 · 1,853 | 0.010 | 0 | 0 | 0.963 | 4/15 |
| | `dispatch-now` | 1,725 · 1,843 | 0.010 | 0 | 0 | 0.974 | 7/15 |
| pipeline, 10,000 | `clear` | 2,147 · 2,285 | 0.037 | 0 | 0.338 | — | — |
| | `clear#2` | 2,060 · 2,360 | 0.038 | 0 | 0.330 | 1.036 | 10/15 |
| | `lt` | 2,117 · 2,336 | 0.014 | 0.123 | 0.061 | 0.991 | 7/15 |
| | `dispatch` | 2,147 · 2,319 | 0.014 | 0.061 | 0 | 1.002 | 9/15 |
| | `dispatch-now` | 2,094 · 2,338 | 0.075 | 0.061 | 0 | 0.999 | 7/15 |

What the kqueue numbers say:
- **Only two differences clear the noise.** `dispatch-now` costs 6% in ping-pong (12 of 15 rounds): one `kevent()` per
  park, about 170 ns. And `lt` beats `clear` by 3% at 10,000 connections (faster in 14 of 15 rounds).
- **Batching is what matters, not the trigger mode.** A re-arm that rides the next wait costs nothing measurable.
  Today's `Poller.add` applies each change with its own `kevent()` (`kama__kq_change`), which is exactly the
  `dispatch-now` arm.
- **`EV_CLEAR` on both filters,** Go's and mio's registration, pays one unwanted WRITE report per request, because
  each write's acknowledgement makes the socket writable again. That is what costs at 10,000 connections.
  `EV_CLEAR` on READ alone would have `lt`'s zero bookkeeping in ping-pong, so it could tie `lt`, never beat it.
- **The brief's premise is false here.** "A socket with no fiber parked on it would be reported on every wait" holds
  only for a scheduler that leaves it enabled. Disabling it on the first unwanted report costs one batched change,
  and in ping-pong none ever happens, because a woken task reads before the next wait.

**epoll** (Linux 7.0.9, aarch64, in the podman VM on the same M4; 15 rounds). The reference is `et`, and `et#2` is
its noise run. epoll has no change list, so a registration is always its own `epoll_ctl`.

| workload | arm | CPU ns/req, min · median | syscalls/req | `epoll_ctl`/req | unwanted reports/req | vs `et`, paired median | slower in |
|---|---|---|---|---|---|---|---|
| pingpong, 100 | `et` (`EPOLLET`, IN and OUT once) | 3,625 · 3,745 | 0.011 | 0 | 0 | — | — |
| | `et#2` | 3,662 · 3,831 | 0.011 | 0 | 0 | 1.029 | 9/15 |
| | `lt` (level; disarm on an unwanted report, re-arm at park) | 3,472 · 3,812 | 0.013 | 0 | 0 | 1.008 | 8/15 |
| | `oneshot` (`EPOLLONESHOT`, a MOD per park) | 3,894 · 4,082 | **1.010** | 0.999 | 0 | **1.084** | **14/15** |
| pingpong, 10,000 | `et` | 4,463 · 4,726 | 0.001 | 0 | 0 | — | — |
| | `et#2` | 4,398 · 4,608 | 0.001 | 0 | 0 | 0.982 | 4/15 |
| | `lt` | 4,706 · 4,984 | 0.001 | 0 | 0 | **1.048** | **15/15** |
| | `oneshot` | 5,017 · 5,261 | 0.983 | 0.982 | 0 | **1.128** | **15/15** |
| pipeline, 100 | `et` | 3,136 · 3,365 | 0.010 | 0 | 0.062 | — | — |
| | `et#2` | 3,235 · 3,347 | 0.010 | 0 | 0.062 | 0.995 | 7/15 |
| | `lt` | 3,159 · 3,309 | 0.011 | 0.001 | 0 | 0.984 | 5/15 |
| | `oneshot` | 3,167 · 3,375 | 0.010 | 0 | 0 | 1.011 | 8/15 |
| pipeline, 10,000 | `et` | 4,106 · 4,245 | 0 | 0 | 0.057 | — | — |
| | `et#2` | 4,091 · 4,224 | 0 | 0 | 0.057 | 0.995 | 6/15 |
| | `lt` | 4,121 · 4,347 | 0.031 | 0.030 | 0.016 | 1.022 | 11/15 |
| | `oneshot` | 4,119 · 4,451 | 0.015 | 0.015 | 0.009 | 1.043 | 11/15 |

What the epoll numbers say:
- **`EPOLLONESHOT` pays for its `epoll_ctl(MOD)` on every park:** 8% at 100 connections and 13% at 10,000 (14 and 15
  of 15 rounds), about 340–530 ns a request.
- **Level costs 5% at 10,000 connections (15 of 15) with the same system calls as edge.** The cost is inside the
  kernel. After it reports a level-triggered descriptor, epoll puts it back on the ready list ("Level-triggered:
  re-queue so the next epoll_wait() rechecks availability", `fs/eventpoll.c`), and so polls every reported socket a
  second time. Edge-triggered skips that.
- **Pipelined load shows no difference above the noise.** A busy server parks rarely.

So on epoll, edge-triggered wins, the registration Go and mio chose. On kqueue the trigger mode mattered less than
batching, and the arm that would combine both, `EV_CLEAR` on READ only, is measured next.

**kqueue, `EV_CLEAR` on READ only.** The first run's best arms had a cost each: `lt` disarms after an unwanted report
and the kernel checks a level knote again, and `clear` reports every write's acknowledgement. `clear-read` registers
READ edge-triggered once and adds WRITE only while a writer is parked. A second run (15 rounds, macOS, while other
work loaded the machine, load average 4–14) put it beside the two that tied. The reference is `clear-read`, and its
noise run reached 10–11 of 15 this time, so the bar is higher here.

| workload | arm | CPU ns/req, min · median | changes/req | unwanted reports/req | vs `clear-read`, paired median | slower in |
|---|---|---|---|---|---|---|
| pingpong, 100 | `clear-read` | 2,683 · 2,852 | 0 | 0 | — | — |
| | `clear-read#2` | 2,820 · 2,977 | 0 | 0 | 1.049 | 10/15 |
| | `lt` | 2,815 · 2,935 | 0 | 0 | 1.038 | 11/15 |
| | `dispatch` | 2,723 · 3,054 | 1.000 | 0 | 1.026 | **13/15** |
| pingpong, 10,000 | `clear-read` | 2,970 · 3,053 | 0 | 0 | — | — |
| | `clear-read#2` | 2,909 · 3,101 | 0 | 0 | 1.007 | 11/15 |
| | `lt` | 2,931 · 3,073 | 0 | 0 | 0.997 | 7/15 |
| | `dispatch` | 3,045 · 3,101 | 0.990 | 0 | 1.018 | **12/15** |
| pipeline, 10,000 | `clear-read` | 1,921 · 2,077 | 0 | 0.006 | — | — |
| | `clear-read#2` | 1,957 · 2,095 | 0 | 0.007 | 1.016 | 9/15 |
| | `lt` | 1,931 · 2,087 | 0.122 | 0.061 | 1.025 | 10/15 |
| | `dispatch` | 2,047 · 2,073 | 0.061 | 0.004 | 1.004 | 9/15 |

`clear-read` and `lt` tie, and `dispatch`, which pays a change record per park, trails them by about 2%. So on kqueue
edge-triggered READ is the cheapest arm or tied for it, with no bookkeeping at all on a park, which is the same rule
as epoll's.

**What decides decision 6.** On both backends a park should cost nothing: register a socket's readability
edge-triggered once, and batch every other change into the next wait. That is Go's and mio's registration, except on
kqueue's WRITE filter, which is registered only while a writer is parked.

**The counters were checked against the kernel's own count.** Each epoll arm ran once (ping-pong, 100 connections,
20,000 requests) under `strace -f -c`, with `VERIFY=1` printing every counter: setup, server and client threads.

| arm | `epoll_ctl`, strace = harness | `epoll_pwait`, strace = harness | `read`, strace / harness |
|---|---|---|---|
| `et` | 200 = 100 + 0 + 100 | 20,031 = 202 + 19,829 | 59,998 / 59,997 |
| `oneshot` | 20,088 = 100 + 19,888 + 100 | 20,168 = 200 + 19,968 | 59,989 / 59,988 |
| `lt` | 200 = 100 + 0 + 100 | 20,261 = 294 + 19,967 | 59,983 / 59,982 |

The registration and wait counts match exactly (glibc on arm64 issues `epoll_wait` as `epoll_pwait`). `read` is one
higher in every arm, the dynamic loader's own read. `write` is four higher, the program's output lines. macOS has no
`strace` short of disabling SIP, so the kqueue counters are trusted because only the backend section of the file
differs between the two builds.

### Decision 2: one switch there and back

A throwaway probe ping-ponged 20 million round trips between a thread's stack and a 256 KiB `mmap` stack, interleaving
the two arms within each of five rounds and reporting the minimum. The asm arm is about 30 instructions: it saves
x19–x30 and d8–d15, swaps `sp`, restores, and returns.

| | macOS 26.4, M4 | Linux 7.0.9 arm64 (VM on the same M4) |
|---|---|---|
| hand-written switch, round trip | 20.1 ns | 21.7 ns |
| `swapcontext`, round trip | 714 ns | 497 ns |

## 7. What the brief got wrong

Corrected here and in ROADMAP_DETAIL §6:
- **"Exclusivity per fiber"** was inverted (decision 3).
- **"Timers need nothing new"** was wrong (decision 11).
- **"Each isolate accepts on its own listener"** holds only on Linux. macOS's `handOff` is not a balancer and
  Windows has nothing, so there an acceptor isolate hands sockets out over channels.
- **"One-shot costs one system call per park"** is false on kqueue, where the re-arm rides the next wait's
  changelist ("all changes contained in the changelist are applied before any pending events are read").
- **"Go and tokio pay no system call per park"** holds on epoll and kqueue only. mio re-arms AFD per park, and Go
  uses overlapped I/O on Windows.
- **"Fibers on wasm need Asyncify"** is out of date (decision 4).
- **SPEC's "no language change"** was wrong. The surface change is small (`spawn fiber`, decision 9), but the
  `scope` lowering, the park rule and the `Fiber<R>` handle are compiler work.
- **The brief's spawn spelling** ("a `Job` contract or a bound function pointer") contradicted SPEC's
  fn-plus-one-bundle rule. Decision 9 keeps the rule.

## 8. Implementation map

Each step is one commit with its fixtures (native, san and TSan; wasm where noted) and a VERSION bump. Sizes are
read off this document.

| # | Commit | Files | Size |
|---|---|---|---|
| 1 | The switch and stacks, with sanitizer annotations and the overflow handler, tested from C | `include/kama_fiber.h` (new), `kama_runtime.h` | M |
| 2 | The scheduler: ready queue, park/unpark, the timer heap, internal registration per decision 6, a remote queue on the Waker | `kama_fiber.h`, `kama_os.h` | L |
| 3 | `spawn fiber`, the `scope` list of live fibers, `Fiber<R>` and `join`, and `ref` bundles rooted outside the loop | `src/kama.y`, `src/kama.cemit.cpp`, `docs/grammar.bnf`, `tree-sitter-kama/` (+ the Zed and Helix pins) | L |
| 4 | The park rule. `@parks`, std's 64 marks and the lock-window walk shipped with `Locked<T>` (`0.9.731`–`0.9.733`: `checkLockWindows`, region markers, kept `@noheap` slot edges, blind-call facts); this step adds the leaf rule for a window over a `Shared`, a call access and `@onPanic`, the job-entry rule, and the debug assert | `src/kama.cemit.cpp` | S |
| 5 | Sockets park on a fiber: TCP, UDP, Unix, accept and connect, with deadlines, deregistration on `give`, and DEL before close | `lib/std/net/`, `kama_os.h` | L |
| 6 | Channels fiber-aware (waiter lists, send-side), and joins park | `include/kama_channel.h`, `lib/std/concurrent/` | M |
| 7 | The offload pool: regular files, DNS, `offload(…)` (with KR-7, KR-202) | `lib/std/fs/`, `lib/std/net/`, `kama_os.h` | M |
| 8 | Windows: the high-resolution timer, and completion I/O where readiness cannot serve (`TransmitFile`, KR-229) | `kama_os.h` | M |
| 9 | The job pool (decision 5) | `lib/std/concurrent/` | M |
| 10 | wasm over JSPI (decision 4) | `src/kama.driver.cpp`, `lib/std/net/web/`, `kama_fiber.h` | L |
| 11 | lldb OS plugin; SPEC *Concurrency* gains *Fibers*; *Why not green threads* rewritten; this file deleted | `include/kama_lldb.py`, `docs/` | M |

## Sources

Verified 2026-10-22:
- **JSPI:** [proposals](https://github.com/WebAssembly/proposals/blob/main/README.md),
  [Chrome 137](https://developer.chrome.com/release-notes/137),
  [Firefox 153](https://developer.mozilla.org/en-US/docs/Mozilla/Firefox/Releases/153),
  [Safari 27](https://developer.apple.com/documentation/safari-release-notes/safari-27-release-notes),
  Node [#60014](https://github.com/nodejs/node/pull/60014) and [#59941](https://github.com/nodejs/node/pull/59941)
- **Stack switching:** [V8 feature flags](https://github.com/v8/v8/blob/main/src/flags/feature-flags.h),
  [Wasmtime](https://github.com/bytecodealliance/wasmtime/blob/main/docs/stability-wasm-proposals.md)
- **emscripten:** [fiber.h](https://github.com/emscripten-core/emscripten/blob/main/site/source/docs/api_reference/fiber.h.rst),
  [#27638](https://github.com/emscripten-core/emscripten/pull/27638),
  [ChangeLog](https://github.com/emscripten-core/emscripten/blob/main/ChangeLog.md)
- **Zig:** [0.16.0 release notes](https://ziglang.org/download/0.16.0/release-notes.html)
- **Eio:** [README](https://github.com/ocaml-multicore/eio/blob/main/README.md),
  [executor_pool](https://github.com/ocaml-multicore/eio/blob/main/lib_eio/executor_pool.mli)
- **Java:** [JEP 444](https://openjdk.org/jeps/444), [JEP 491](https://openjdk.org/jeps/491)
- **Go:** [netpoll_epoll](https://github.com/golang/go/blob/master/src/runtime/netpoll_epoll.go),
  [netpoll_kqueue](https://github.com/golang/go/blob/master/src/runtime/netpoll_kqueue.go),
  [stack.go](https://github.com/golang/go/blob/master/src/runtime/stack.go),
  [proc.go](https://github.com/golang/go/blob/master/src/runtime/proc.go),
  [cf52e70](https://github.com/golang/go/commit/cf52e709977d331a70df9463cf9e307024b6779f)
- **mio, tokio:** [epoll.rs](https://github.com/tokio-rs/mio/blob/master/src/sys/unix/selector/epoll.rs),
  [kqueue.rs](https://github.com/tokio-rs/mio/blob/master/src/sys/unix/selector/kqueue.rs),
  [afd.rs](https://github.com/tokio-rs/mio/blob/master/src/sys/windows/afd.rs),
  [coop](https://github.com/tokio-rs/tokio/blob/master/tokio/src/task/coop/mod.rs)
- **Seastar:** [tutorial](https://github.com/scylladb/seastar/blob/master/doc/tutorial.md)
- **Boost.Context:** [x64 ms_pe asm](https://github.com/boostorg/context/blob/develop/src/asm/jump_x86_64_ms_pe_masm.asm),
  [sysv elf asm](https://github.com/boostorg/context/blob/develop/src/asm/jump_x86_64_sysv_elf_gas.S)
- **Windows:** [CreateFiberEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiberex),
  [Fibers](https://learn.microsoft.com/en-us/windows/win32/procthread/fibers),
  [GetTickCount64](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount64)
- **ucontext:** [Apple ucontext.h](https://github.com/apple-oss-distributions/libplatform/blob/main/include/ucontext.h),
  [glibc swapcontext](https://github.com/bminor/glibc/blob/master/sysdeps/unix/sysv/linux/x86_64/swapcontext.S)
- **Sanitizers:** [common_interface_defs.h](https://github.com/llvm/llvm-project/blob/main/compiler-rt/include/sanitizer/common_interface_defs.h),
  [tsan_interface.h](https://github.com/llvm/llvm-project/blob/main/compiler-rt/include/sanitizer/tsan_interface.h)
- **Linux:** [fs/eventpoll.c](https://github.com/torvalds/linux/blob/master/fs/eventpoll.c),
  [vm.max_map_count](https://github.com/torvalds/linux/blob/master/Documentation/admin-guide/sysctl/vm.rst),
  [epoll_pwait2](https://man7.org/linux/man-pages/man2/epoll_wait.2.html)
- **kqueue:** [FreeBSD kqueue(2)](https://man.freebsd.org/cgi/man.cgi?query=kqueue&sektion=2),
  [xnu event.h](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/event.h)
