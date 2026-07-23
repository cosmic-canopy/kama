# Concurrency — specification (design pass converged 2026-07-22)

**Status: FINAL design (M0 converged).** The design-refinement pass is done; this document is now the **spec**
(surface + lowering + runtime ABI) that the implementation milestones (M1–M6 below) build to — the same bar the
construction-model and streams campaigns met before code. As each milestone ships, `SPEC.md` / `GOALS.md` /
`KEYWORDS.md` / `grammar.bnf` are updated to match.

The *direction* was settled in [ROADMAP.md §6](../ROADMAP.md) (shared-nothing by construction: isolates +
ownership-transferring channels). This pass turned its seven open questions into concrete decisions.

## The one idea

Earn data-race freedom the way kama earns null-safety: make the hazard **unrepresentable**, not checked. No
borrow checker over shared memory — **remove the shared mutable state.** Where Rust proves exclusivity over
shared memory (borrow checker, lifetimes, `Send`/`Sync`, `async` coloring, `Pin`), kama removes the shared
mutable state so there is nothing to prove. The honest trade: less flexible for the last few percent of
shared-mutation performance; far simpler to reason about, and **portable native↔wasm from one source** —
which threaded C++/Rust are not.

## Model — three levels, one ownership model, three sharing seams

Coarse → fine:

- **`isolate`** — a real OS thread (native) / Web Worker (wasm). Shared-nothing: its own stack, heap, and module
  statics. *Few* of them — think ~one per core, or a handful of long-lived service isolates. Isolates
  communicate **only** through channels and the atomic/shared-region seam. An isolate **may block** on a
  channel; that is honest and cheap precisely because isolates are coarse.
- **`channel<T>`** — a typed pipe between isolates. Crossing it reuses the existing ownership model: send a
  `value` → **copy**; send a `resource` → **`give`** (move, zero-copy on native; the use-after-send error
  already falls out of move-tracking). Blocking `recv()` parks the receiving isolate. Bounded capacity is a
  construction parameter; capacity `0` is a rendezvous (unbuffered) channel. **One** channel type — favor one
  way.
- **`job` / `parallel_for`** — the fine-grained CPU-work layer, scheduled onto a pool of isolates. A job is a
  plain function + moved args and **never blocks mid-stack** — it runs and returns. A `future<T>` is the
  one-shot form and *is* a one-slot channel. This is the data-parallel (rayon / games task-graph) half of the
  model; the pool + work-stealing scheduler is a **library** on top of the isolate ABI, not language.

**Why this and not green threads / async-await.** The "proceed until ready" ergonomic has two implementations:
stackful green threads (Go/Erlang — needs a userspace stack-switching scheduler, impossible on wasm without
Asyncify, which *is* the coloring cost we reject) or `async`/`await` (function coloring, `Pin` — kama's
least-kama feature). Kama takes neither into the **language**: isolates are real threads (blocking is honest
when few) and massive parallelism comes from the never-blocking job layer. The high-connection-server ergonomic
that green threads exist for is recovered *above* the language by a native scheduler library — see
[§ Web/server workloads](#webserver-workloads).

### The three sharing seams (greppable, like `unsafe {}`)

| Seam | Meaning | Native | wasm | MCU |
|---|---|---|---|---|
| module `static` | **per-isolate** state ("each thread its own module") | `_Thread_local` (own copy per isolate) | automatic (separate Worker instance) | plain C `static`, **zero cost** (one core = one isolate) |
| `hardware` qualifier | `volatile` MMIO + single-core ISR↔loop flag | `volatile T*` | n/a | the MCU register/ISR seam — **not** cross-isolate ([ROADMAP §5](../ROADMAP.md)) |
| `Atomic<T>` / shared-region | the **only** cross-isolate mutable sharing | `<stdatomic.h>` / `_Atomic` | SharedArrayBuffer + Atomics | atomics if multi-core |

The load-bearing rule: **a module `static` is per-isolate by construction; cross-isolate mutable sharing
requires `Atomic<T>` / a shared-region.** A `static` therefore cannot be seen by another isolate, so it cannot
race; to share you must reach for the greppable atomic seam. This *unifies* with the MCU story
([MCU_READINESS.md](../MCU_READINESS.md) Tier-0): the module-statics feature MCU needs is the same feature, and
because its default semantics are per-isolate it is automatically race-free the day it runs on a multicore
native/wasm target. On a single-core MCU there is exactly one isolate, so a `static` is an ordinary zero-cost C
static. This campaign **pins the semantic**; the MCU milestone **builds** statics to it.

## The seven questions — decided

### 1. Portable spawn substrate (the hard one)

Pin a `kama_isolate_*` C ABI in the runtime header; the native (pthreads) and wasm (JS-worker-bridge) backends
live behind it, and the emitter targets the ABI, never a platform directly.

**An isolate entry is a top-level function + a moved-in argument bundle** — *not* a closure over the enclosing
environment. This is what makes shared-nothing hold *by construction*: there is no captured mutable state to
share, so there is nothing to race over. The argument bundle is itself a sendable payload (§2), transferred by
the same `give`/`copy` rule a channel uses.

- **Native** = pthreads. Address space is physically shared, so `give` across an isolate is a **pointer handoff
  (zero-copy)**. The compiler forbids exploiting the shared address space outside the seams, so the *semantics*
  are shared-nothing even though the memory is physically shared.
- **wasm** = a separate-instance Web Worker (its own module instance + linear memory). `give` is a postMessage
  **transferable** (zero-copy, browser-enforced no-use-after-transfer). wasm cannot spawn its own worker — the
  JS host must — so `kama_isolate_spawn` on wasm posts a spawn request to the host bridge; this is hidden behind
  the ABI so **one source** expresses "spawn an isolate" on both targets.

The same source works because the language semantics (shared-nothing) are a subset both substrates satisfy.

```
// runtime ABI sketch (kama_runtime.h) — native pthreads first (M2), wasm bridge behind it (M5)
typedef void (*kama_isolate_entry)(void* arg_bundle);   // top-level fn, moved-in bundle
kama_isolate_t  kama_isolate_spawn(kama_isolate_entry, void* arg_bundle);
void            kama_isolate_join(kama_isolate_t);
```

### 2. Channel type + sendability = structural, compiler-computed

"Sendable" is **not** a hand-written contract or marker. It is computed structurally by the compiler from the
ownership kind + field types (the transitive closure), exactly as the escape check works today — favor
simplicity, and it must be transitively correct (a lesson from Rust's auto-`Send`).

A type `T` is **sendable** iff one of:
- `T` is a `value` whose fields are all sendable (deep bitwise/`copy` transfer; **no smuggled non-owning refs**);
- `T` is a `resource` (transferred by `give` / move);
- `T` is an immutable-`Shared<U>` (the read-across-isolates primitive, §7a).

**Rejected** (compile error naming the offending field, mirroring the escape check): `view` (a borrow), `Ptr`
(raw), a bare `contract` value (a borrow), and any type transitively containing a non-sendable field.

```
type value Channel<T> where T: Sendable { ... }     // Sendable is a compiler predicate, not a user contract
fn send(ref Channel<T> ch, give T item);            // resource -> give; value -> copy at the call
fn Optional<T> recv(ref Channel<T> ch);             // blocking; None once the channel is closed + drained
```

Send consuming via `give` hooks straight into the existing move machinery (`moveOnlySource` extracts the named
local, `ownsByValue` gates the hand-off, `markMoved` transitions it to `Moved`; later use of the sent local is
already a compile error). Blocking first (M3); bounded capacity param, `0` = rendezvous.

### 3. Per-isolate state & the prelude

Kama has **no mutable module-level/global state today**, so isolates share immutable code + prelude and own
their own data trivially — no per-isolate re-collection machinery is required. The single forward rule is the
per-isolate-`static` semantic pinned above (built by the MCU milestone). Immutable prelude/module code is
shared; per-isolate data is `_Thread_local` on native and automatic on wasm.

### 4. Structured-concurrency scope = RAII for tasks

A concurrency `scope` owns its child task handles; the scope's **drop joins** all children (Rust
`std::thread::scope` + Swift/Trio structured concurrency). Deterministic task lifetimes, no orphans — the
concurrency form of the no-leak guarantee, composed straight onto the existing drop/RAII order.

Because join happens at scope exit, a child task **may safely borrow from the enclosing scope** (it is
guaranteed alive until join) — this is exactly what makes disjoint-slice `parallel_for` (§7b) sound without a
borrow checker. The rule: a task must not outlive a borrow it holds; the existing escape check enforces that a
borrow captured by a task does not escape the joining scope.

```
scope {                              // structured: joins all children at the closing brace
    parallel_for (e in entities) {   // splits entities into disjoint sub-Views, one per pool isolate
        e.physics.step(dt);          // each isolate mutates only its slice — no locks, cannot race
    }
}                                    // barrier: every slice joined before we continue
render(entities);                    // safe — the borrow is reclaimed at join
```

### 5. Scheduler / "proceed until ready"

Coarse isolates *may* block on channels (honest, cheap when few). Fine-grained parallelism uses the job layer,
which never blocks mid-stack → **no stack-switching runtime, no coloring** in the language. The pool +
work-stealing **job system is a library** on top of the isolate ABI (per §6), not language surface. The
"don't-waste-threads on thousands of idle connections" case is served by the existing non-blocking `Poller` on
one isolate (and, above the language, a native fiber scheduler — see [§ Web/server](#webserver-workloads)),
never by green threads in the language.

### 6. `Atomic<T>` seam

A new intrinsic type — the minimal shared-region opt-in, the concurrency analog of `unsafe {}`/`Ptr` (opt-in,
greppable, **atomics-only**).

- **Native** → `<stdatomic.h>` / `_Atomic T`; load / store / CAS / `fetch_add` etc.
- **wasm** → JS Atomics over a SharedArrayBuffer cell.
- Default memory ordering is **seq-cst** (favor simplicity); explicit weaker orderings are an expert opt-in
  parameter. Scoped to atomics only — general shared mutable memory stays out of the safe surface.

### 7. The two safe-sharing primitives

Both recover what shared-nothing otherwise costs, without reintroducing shared mutability.

**(a) immutable-`Shared` read-across-isolates.** A `Shared<T>` over a **deeply immutable** `T` is
sendable/shareable — immutable data is race-free even when shared (cheap read-only sharing of big assets). The
control-block refcount becomes atomic when a `Shared` is shared across isolates. On wasm the immutable payload
lives in the shared region (below) so workers read it zero-copy.

**(b) scoped disjoint-slice `parallel_for`.** A scope lends each task a **non-overlapping** mutable sub-`View`
of one buffer and reclaims all of them at join; safe **by disjointness** (`split_at_mut` / rayon). Composes with
the `view`/escape model — the sub-views are borrows that cannot escape the parallel scope.

**wasm unification.** All three seams (`Atomic<T>`, immutable-`Shared`, disjoint slices) route through the
**single SharedArrayBuffer region** on wasm; on native they are just the normal shared address space. The
shared-region seam is the wasm unification point — one mechanism, three safe uses.

## Web/server workloads

A kama server is a **native binary** ([WEB_FRAMEWORK_READINESS.md](../WEB_FRAMEWORK_READINESS.md)); wasm is for
**clients** (a browser cannot bind a listening socket). So the wasm-portability constraint that keeps green
threads out of the *language surface* does not restrict a server scheduler.

- Everything the web-framework gap analysis blocks on is either a **library** (HTTP/1.1 parser, router,
  middleware via `type contract HttpHandler`, JSON, body codecs, TLS-via-FFI) or a **primitive this campaign
  ships** (`std::time` M1, `isolate` M2, `channel` M3, atop the already-shipped non-blocking `Poller`).
- The Node-defining async event loop / scheduler is a **library** on those primitives (§6: "the job system is a
  library"; web doc step 2: single-thread loop + scheduler over `Poller` first, multi-core isolates later).
- **Straight-line handler code without coloring is preserved.** Blocking-shaped `recv()`/I/O is the portable
  surface (maps to a Worker as a real block). The "multiplex thousands of connections on a few threads"
  ergonomic — block one handler, run another — is a **native scheduler-library** concern: it can back the *same*
  blocking surface with fibers (native stack-switching, like Go's netpoller) with **no language change, no
  function coloring, zero wasm impact**. Green threads are therefore an implementation detail of a native
  library, never a language construct — which is *why* deferring them from the language is correct, not a
  limitation. The primitives here suffice for either a fiber-backed or an explicit-task scheduler.

## Guardrails (north stars — verify every step)

- **No-GC / RAII / deterministic destruction** hold across isolates and task scopes (structured concurrency IS
  the RAII extension). Every commit **ASan/UBSan-clean**; add a **`KAMA_TSAN`** sweep once M2 lands threads.
- **No-null, no shared mutable state in the safe surface** — sharing confined to the greppable
  `Atomic<T>`/shared-region seam, exactly as raw pointers are confined to `unsafe {}`.
- **Portable native↔wasm from one source** — the differentiator; the spawn/channel lowering must not fork the
  user-facing surface. **If a construct can't map to Web Workers, it doesn't ship** (green-thread multiplexing
  therefore lives in a native library, not the language).
- **One way / favor simplicity** ([GOALS.md](../../GOALS.md)) — no function coloring, no mandatory-mutex model;
  the cooperative isolates + jobs model is the one concurrency story.

## Precondition (cheap, do first)

- **`std::time`** — a monotonic clock + `Duration`/`Instant` (native `clock_gettime` /
  `QueryPerformanceCounter`; wasm `emscripten_get_now`). Small, independently useful, unblocks timeouts +
  scheduling + any scheduler benchmark. First code commit of the campaign.

## Milestones

- **M0 Design pass** — this spec. ✅ *(converged 2026-07-22)*
- **M1 `std::time`** — the precondition module (`lib/std/time/`, `namespace std::time;`), native + wasm, fixtures.
- **M2 Runtime isolate seam** — `kama_isolate_*` ABI; native pthreads; spawn one isolate running a top-level fn +
  moved arg bundle and join it. `isolate` surface + emitter lowering. No channels yet. Add `KAMA_TSAN`. ✅ *(landed 2026-07-23 — see below)*
- **M3 Channels** — `channel<T>`; blocking send/recv reusing `give`/`copy` + move-tracking; structural
  sendability check with field-naming errors; bounded + rendezvous. ✅ *(landed 2026-07-23 — see below)*
- **M4 Structured-concurrency scope** — `scope` joining children at drop (RAII-for-tasks); the
  borrow-outlives-task rule via the escape check.
- **M5 wasm parity** — Worker + postMessage bridge behind the M2 ABI; `give`→transferable; the native↔wasm
  portability proof (one fixture passing identically on both).
- **M6 `Atomic<T>` + the two safe-sharing primitives** — the narrow shared seams (native + wasm/SharedArrayBuffer);
  immutable-`Shared` cross-isolate; disjoint-slice `parallel_for`. The job-system library lands on top.

## M2 — landed (2026-07-22)

The native isolate seam shipped: spawn a top-level fn on a fresh OS thread with a **moved-in** argument
bundle, and join it. Shared-nothing by construction (bare top-level entry + moved arg), TSan-proven. No
channels (M3), no wasm (M5), no `Atomic<T>` (M6).

**Surface** (`import std::concurrent;` for the fused form; `import std::concurrent::{Isolate};` for the handle):

```
fn void worker(Payload p) { ... }              // entry: a top-level fn (no env capture → shared-nothing)
isolate worker(p: give payload);               // fused: spawn on a new OS thread, then join
Isolate h = isolate worker(p: give payload);   // handle form: spawn now; the RAII handle owns the join
h.join();                                       // explicit join; ~Isolate() also joins (drop = join)
```

The `give`n bundle must be a move-only `resource` VALUE and the entry must return `void` (M2 has no channel
to return over). Post-spawn use of the moved source is a compile error (reuses the `give` move seam — no new
tracking). An `isolate { block }` capture form is intentionally not offered (it would close over the env).

**Where it lives:**
- Runtime ABI — `kama_isolate.h` (repo root; `#include <pthread.h>`, `static inline` `kama_isolate_spawn`/
  `_join` + heap-boxed `_spawn_boxed`/`_join_boxed` for the handle). Deliberately NOT in `kama_runtime.h`
  (keeps threads off the freestanding/MCU path; `pthread_t` ≠ `void*`).
- Stdlib — `lib/std/concurrent/concurrent.kama` (`namespace std::concurrent`): `extern "kama_isolate.h";`
  + `resource Isolate { Ptr handle; join(); ~Isolate(); fromRaw(Ptr) }` (drop = join, idempotent join).
- Compiler — `isolate` keyword (`kama.l`); `%token ISOLATE` + `isolate_statement` (fused) and a
  `variable_initializer` alt (handle) in `kama.y`; `IsolateNode : ExpressionStatementNode` (`kama.ast.h`);
  `emitStatement`/`emitExpression` branches → `isolatePrep`/`emitIsolate`/`emitIsolateExpr` (`kama.cemit.cpp`),
  which emit a per-entry file-scope trampoline (deduped) + heap-move + spawn/join. `needsIsolate` gates
  `-lpthread` (native) in `kama.driver.cpp`.
- Tests — `KAMA_TSAN=1` sweep in `run_tests.sh`; fixtures `tests/isolate_basic`, `isolate_two_disjoint`
  (TSan-clean shared-nothing proof), `isolate_handle` + `isolate_raii_join` (handle + drop-join),
  `tests/xfail/isolate_use_after_move`. Green on native + `KAMA_TSAN` + `KAMA_SAN`.

## M3 — landed (2026-07-23)

The channel seam shipped: a typed, blocking `channel<T>` for moving values **between** isolates — the first
sanctioned cross-isolate shared object. **All thread-safety lives in C** (`kama_channel.h`): one
`pthread_mutex` + two condvars serialize send/recv/endpoint-drop, so **no atomics are needed** (the mutex also
serializes liveness + free — the last endpoint to drop frees, race-free). The kama side is a thin `Ptr`-handle
library, exactly like the M2 `Isolate` handle. Bounded ring **and** rendezvous (cap 0); SPSC (multi-producer
`clone()` is a follow-up). TSan- + ASan-proven.

**Surface** (`import std::concurrent::{Channel, Sender, Receiver};` — the sender moves into a spawned isolate):

```
fn void producer(Sender<int32> tx) { tx.send(item: give x); }   // ~Sender() closes → recv returns None
Channel<int32> ch = Channel::bounded(capacity: 4);   // capacity 0 == rendezvous (synchronous hand-off)
Sender<int32>   tx = ch.sender();
Receiver<int32> rx = ch.receiver();
Isolate h = isolate producer(tx: give tx);           // HANDLE form (fused would join → deadlock a producer)
Optional<int32> v = rx.recv();                        // blocks; None once drained AND all senders dropped
```

`send(item:)` → `bool` (false if the receiver is gone); `recv()` → `Optional<T>` (None = closed + drained).
Both block. `Channel<T>` is a thin factory; the queue's lifetime is governed solely by the two endpoint
liveness flags (last-dropper-frees), so `~Channel()` is a no-op — extract both endpoints before discarding it.

**send/recv are PURE LIBRARY — no emitter intrinsics** (the kickoff expected intrinsics; two discoveries
removed the need). `sizeof(T)` works in a generic body, so the factory is library. `addr(of:)` + `memset`
let `send` be library too: the caller's `give` already suppresses the caller-side drop via normal
move-tracking, and inside `send`, after the C memcpy relocates the bytes into the queue, the moved-in source
is `memset` to zero so its method-end destructor is a no-op (INVARIANT #0: a resource's zero value is
drop-safe). `recv` builds `Optional::Some(value: give dst)`, whose `give` suppresses `dst`'s drop. Bitwise
relocation → the receiver holds the sole owner (verified for a heap-owning `T` in `channel_move_heap`).

**Sendability is a structural compiler gate** (no user marker): a `T` may cross only if it does not
transitively reach a **non-atomic shared refcount** (`Shared<X>`/`Weak<X>` in its field/base/variant/
collection-element graph). `Owned<X>` is fine (unique — the move transfers it whole). `computeReachesSharedWeak()`
is the Shared|Weak-only sibling of `computeReachesPointer` (same fixpoint, `Owned` excluded);
`checkChannelSendability()` rejects an offending `channel<T>`, naming the culprit field.

**Where it lives:**
- Runtime ABI — `kama_channel.h` (repo root; `#include <pthread.h>`, one `kama_channel_t` struct +
  `static inline` `kama_channel_new`/`_send`/`_recv`/`_drop_sender`/`_drop_receiver`, all bodies under `mu`;
  cap-0 is a synchronous-handoff branch in send/recv). Like `kama_isolate.h`, kept OFF `kama_runtime.h`.
- Stdlib — `lib/std/concurrent/channel.kama` (`namespace std::concurrent`): generic `resource`
  `Channel<T>` / `Sender<T>` / `Receiver<T>` (each a `Ptr handle`); `send`/`recv` are ordinary kama methods.
- Compiler — `kama.cemit.cpp`: `computeReachesSharedWeak()` + `checkChannelSendability()` (beside
  `computeReachesPointer`), the channel-family template capture (`_channelTmpl`/`_senderTmpl`/`_receiverTmpl`,
  gated on `std__concurrent`). `kama.driver.cpp`: the isolate `-lpthread` hint broadened to `needsPthread`
  (either `kama_isolate.h` or `kama_channel.h`). **No new AST/grammar/keyword** — channels are pure library.
- Tests — `channel_bounded` (cross-isolate hand-off, =45), `channel_backpressure` (cap-1, in-order),
  `channel_move_heap` (heap-owning `T`, free-once), `channel_send_owned` (`Owned<X>` IS sendable),
  `channel_rendezvous` (cap-0 lockstep), `channel_close` (drop wakes a blocked recv → None),
  `tests/xfail/channel_send_shared` (the sendability reject). Green on native + `KAMA_TSAN` + `KAMA_SAN`;
  skipped on wasm (native-only until M5).

**Known limitation:** items left **undelivered** in an abandoned channel (both endpoints dropped with the
queue non-empty) have their bytes reclaimed by the C buffer free, but their kama-level destructors do **not**
run — so a heap-owning `T` left buffered leaks. Value types never leak. The contract is *drain your channel*
(the fixtures do). A drain-on-`~Receiver` (needs a non-blocking `try_recv`) is the clean fix — a follow-up.

## Deferred (per §6)

Co-equal general shared-memory ("hybrid") threading; an M:N green-thread runtime / `async`/`await` in the
language. Reopen only if a concrete case the seam + poller + a native scheduler library cannot express appears.
