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
| module `static` | **per-isolate** state ("each thread its own module") | `_Thread_local` (own copy per isolate) | `_Thread_local` (emscripten pthreads share one linear memory, so TLS — not a plain `static` — is what makes it per-isolate) | plain C `static`, **zero cost** (one core = one isolate) |
| `hardware` qualifier | `volatile` MMIO + single-core ISR↔loop flag | `volatile T*` | n/a | the MCU register/ISR seam — **not** cross-isolate ([ROADMAP §5](../ROADMAP.md)) |
| `Atomic<T>` / shared-region | the **only** cross-isolate mutable sharing | `<stdatomic.h>` / `_Atomic` | SharedArrayBuffer + Atomics | atomics if multi-core |

The load-bearing rule: **a module `static` is per-isolate by construction; cross-isolate mutable sharing
requires `Atomic<T>` / a shared-region.** A `static` therefore cannot be seen by another isolate, so it cannot
race; to share you must reach for the greppable atomic seam. This *unifies* with the MCU story
([MCU_READINESS.md](../MCU_READINESS.md) Tier-0): the module-statics feature MCU needs is the same feature, and
because its default semantics are per-isolate it is automatically race-free the day it runs on a multicore
native/wasm target. On a single-core MCU there is exactly one isolate, so a `static` is an ordinary zero-cost C
static. This campaign **pins the semantic**; the MCU milestone (step 1, ✅ shipped) **builds** statics to it —
`static T name = const;` lowering to `static KAMA_ISOLATE_LOCAL T name` (`_Thread_local` on native + wasm, empty
on `--target embedded`). Note the wasm detail: kama's wasm isolates are emscripten pthreads sharing **one**
linear memory (like native pthreads share an address space), so per-isolate `static`s need `_Thread_local`
there too — a plain wasm `static` would be shared and racy.

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
- **M5 wasm parity** — emscripten pthreads (Web Workers over a shared `SharedArrayBuffer`) behind the M2 ABI;
  `give` stays a pointer handoff; the native↔wasm portability proof (fixtures passing identically on both).
  *(The postMessage/transferable bridge was the early framing; model 1 was chosen — see "M5 — landed".)*
- **M6 `Atomic<T>` + the two safe-sharing primitives** — the narrow shared seams (native + wasm/SharedArrayBuffer);
  immutable-`Shared` cross-isolate; disjoint-slice `parallel_for`. The job-system library lands on top.
  ✅ *(all landed 2026-07-23 — M6.1 `Atomic<T>`, M6.2 immutable-`Shared`, M6.3 `parallel_for`; campaign complete)*

## M2 — landed (2026-07-22)

The native isolate seam shipped: spawn a top-level fn on a fresh OS thread with a **moved-in** argument
bundle, and join it. Shared-nothing by construction (bare top-level entry + moved arg), TSan-proven. No
channels (M3), no wasm (M5), no `Atomic<T>` (M6).

**Surface** (`import std::concurrent;` for the fused form; `import std::concurrent::{Isolate};` for the handle):

```
fn void worker(Payload p) { ... }              // entry: a top-level fn (no env capture → shared-nothing)
spawn worker(p: give payload);                 // fused: spawn on a new OS thread, then join
Isolate h = spawn worker(p: give payload);     // handle form: spawn now; the RAII handle owns the join
h.join();                                       // explicit join; ~Isolate() also joins (drop = join)
```

> **M4 rename:** the spawn verb is **`spawn`** (was `isolate` through M3). `Isolate` remains the RAII handle
> *type*. In M4 the bare fused `spawn f(…);` becomes **scope-only** (see the M4 section); the handle form works
> anywhere.

The `give`n bundle must be a move-only `resource` VALUE and the entry must return `void` (M2 has no channel
to return over). Post-spawn use of the moved source is a compile error (reuses the `give` move seam — no new
tracking). A `spawn { block }` capture form is intentionally not offered (it would close over the env).

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
Isolate h = spawn producer(tx: give tx);             // HANDLE form (fused would join → deadlock a producer)
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
  `tests/xfail/channel_send_shared` (the sendability reject). Green on native + `KAMA_TSAN` + `KAMA_SAN`,
  and on wasm since M5.

**Known limitation:** items left **undelivered** in an abandoned channel (both endpoints dropped with the
queue non-empty) have their bytes reclaimed by the C buffer free, but their kama-level destructors do **not**
run — so a heap-owning `T` left buffered leaks. Value types never leak. The contract is *drain your channel*
(the fixtures do). A drain-on-`~Receiver` (needs a non-blocking `try_recv`) is the clean fix — a follow-up.

## M4 — landed (2026-07-23)

Structured concurrency shipped: **`scope { … }`** — a block-bodied keyword (like `unsafe`) that owns the
isolates `spawn`ed inside it and **joins them all at the closing brace, before any local destructor runs**
(join-before-drop). That ordering is the one load-bearing codegen contribution — the M4 analog of M3's
"all-thread-safety-in-C" — and it is what makes a child that **borrows** an enclosing local sound with **no
lifetime inference**. Native pthreads (reuses the M2 `kama_isolate.h` seam — no new runtime). Native +
`KAMA_TSAN` + `KAMA_SAN` green; wasm-green since M5.

**The spawn verb is now `spawn`** (was `isolate` through M3 — `isolate` is a noun; `Isolate` remains the RAII
handle *type*). A bare `spawn f(…);` is **scope-only**: a deferred-join child of the enclosing scope; outside
a scope it is a compile error (nothing owns the join). The handle form `Isolate h = spawn f(…)` works anywhere
(join on `h`'s drop).

**Surface** (`import std::concurrent;`):

```
scope {                                       // structured: joins all children at the closing brace
    spawn writer(s: give bundleA);            // M4.1: deferred-join child, MOVED bundle (reuses the M2 trampoline)
    spawn writer(s: give bundleB);            //       runs concurrently with the first
}                                             // BARRIER: both joined here, before any local dtor

Acc lower = Acc::range(lo: 0,  hi: 50);
Acc upper = Acc::range(lo: 50, hi: 100);
scope {
    spawn accumulate(a: ref lower);           // M4.2: `ref` BORROW of a caller-owned local — no move, no box
    spawn accumulate(a: ref upper);           //       distinct root -> statically disjoint, no race
}                                             // both joined; lower/upper safe to read
```

The entry is a bare top-level `void` fn taking **exactly one param** — its SHAPE selects the mode: a by-value
move-only `resource` (`give`, M4.1) or a `ref T` borrow of a caller-owned bundle (M4.2). A `void` return
because there is no result channel of its own — results come back through an M3 channel or the borrowed
bundle's fields.

**The barrier lives in `emitScopeCleanup`** — the single cleanup site reached by BOTH the fall-through path
and the return/break/continue unwinds — so every child is joined before any local drops on ALL exit paths (an
early `return` out of a scope joins first; the borrowed-pointer lifetime is sound for exactly this reason). A
`scope` carries an `isTaskScope` flag + a `taskChildren` handle list; a bare `spawn` registers its
`kama_isolate_t` into the innermost task scope instead of joining now.

**The borrow (M4.2) is a second trampoline shape + two static guards.** The move trampoline heap-boxes and
frees; the borrow trampoline passes `&local` straight through as the `ref T` (`T*`) param — no box, no free,
no move (the caller keeps the local, dropped after the join). Two structural checks keep it race-free with no
lifetime inference, so the M4 guarantee is *lifetime* AND, for what it admits, *exclusivity*:
- **Escape check** — the borrowed root must outlive the barrier: declared in the task scope or an outer scope
  (or a parameter). A local of a block nested in the scope drops before the join → rejected. `findScopeDeclaring`
  over `declaredNames`, mirroring the view-return root-trace.
- **Same-root disjointness** — no two children of one scope may borrow the same root local (they would race on
  that cell). Distinct roots are statically disjoint; splitting one buffer by disjoint index-ranges (unprovable
  here) is M6's `splitAt`. Borrowing is rejected in the handle form (it may outlive the scope).

**Where it lives:**
- Front end — `scope` keyword (`kama.l`); `scope_statement : SCOPE block` (`kama.y`, mirrors
  `unsafe_statement`); `ScopeNode` (`kama.ast.h`, statement-only; forward-declared in `kama.forward.h`). The
  `isolate`→`spawn` rename touched the keyword table, the `SPAWN` token, and `spawn_statement`.
- Compiler — `kama.cemit.cpp`: `emitScope` (modeled on `emitBlockScoped`), the child-join loop at the top of
  `emitScopeCleanup`, `innermostTaskScope`/`innermostTaskScopeIndex`/`findScopeDeclaring`, and the give/borrow
  branch in `isolatePrep` (the borrow trampoline + escape + same-root checks). No new runtime header (reuses
  `kama_isolate.h`); `Scope` gains `isTaskScope` + `taskChildren` + `borrowedRoots`.
- Tests — `scope_join_barrier` (=42), `scope_many_children` (=15), `scope_nested` (=42), `scope_borrow_disjoint`
  (=86); `xfail/spawn_outside_scope`, `xfail/scope_borrow_escape`, `xfail/scope_borrow_same_root`. Green on
  native + `KAMA_TSAN` + `KAMA_SAN`, and on wasm since M5.

**Known limitations** (both deferred — not blocking; tackle only when a concrete need appears):
- A bare `spawn` must be a DIRECT statement of the `scope` body (its handle is declared in the scope's C block
  so the barrier can name it) — spawning inside a nested block or a `while`/`for` loop within the scope is not
  yet supported. Fixing it needs the scope to hoist a dynamic handle list (a small vector joined at the brace).
  **Dynamic N-way fan-out (spawn K workers where K is runtime) is really M6's `parallel_for`** — data-parallel
  over a collection with disjoint slices — so spawn-in-loop may be subsumed rather than built directly.
- The entry takes exactly ONE bundle param; multi-arg entries (a `ref` borrow plus value params, matching the
  `accumulate(sink: ref …, lo:, hi:)` ideal) would need a per-entry arg-bundle struct. The single bundle param
  carries the child's inputs today, so this is pure ergonomics.

## M5 — landed (2026-07-23)

Wasm parity shipped: the **same** isolate seam (isolates + channels + `scope`) now runs on the **wasm**
target, with **zero runtime-C changes** — `kama_isolate.h` / `kama_channel.h` compiled unchanged under
emscripten. All 15 `std::concurrent` fixtures that were wasm-skipped now pass on the wasm leg against the
**same `.expect`** used natively (channels, isolates, scopes), plus the negative xfail tests still reject.

**Threading model = emscripten pthreads (model 1 of the two considered).** Web Workers over a shared
`SharedArrayBuffer` linear memory; `pthread_create`/`_join`, `pthread_mutex`, `pthread_cond` lower to
`Atomics.wait`. `give` stays a **pointer handoff** (memory physically shared, exactly like native pthreads).
Kama's shared-nothing guarantee is **structural** (bare-fn entry + moved bundle), not address-space
separation — so a shared linear memory doesn't weaken it, and native semantics are matched 1:1. The
alternative (Web Worker + `postMessage(transferable)`, separate address spaces) was rejected: it serializes
every bundle instead of handing off a pointer, needs a whole divergent runtime, and doesn't match native.
The entire host-threading dependency stays **quarantined** in the two seam headers, so a future switch (e.g.
WASI-threads) is contained to their wasm arm + driver flags — the language, emitter, and fixtures don't move.

**The load-bearing gotcha (solved): the JS main thread may not block.** `pthread_join` and a blocking
`channel recv()` lower to `Atomics.wait`, which THROWS on the main thread. Fixed with **`-sPROXY_TO_PTHREAD`**
— emscripten runs kama's `main()` on a dedicated worker, so it blocks on join/recv freely — plus
**`-sEXIT_RUNTIME=1`** to carry `main`'s return value out as the process exit code (else node sees 0).

**What shipped** (all in `kama.driver.cpp`, guarded by the existing `needsPthread` gate —
`externsHeader("kama_isolate.h")||("kama_channel.h")`, `kama.cemit.cpp:349`):
- Wasm arm of the pthread block: `-pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=<n> -sPTHREAD_POOL_SIZE_STRICT=0`
  (native arm still `-lpthread`).
- `-sEXIT_RUNTIME=1` extended from `std::app`-only to `needsApp || needsPthread`.
- `run_tests.sh`: the `std::concurrent` wasm-skip block removed — fixtures build to `.js` and run under `node`.

**Pool sizing = pre-warm 0, grow on demand.** `-sPTHREAD_POOL_SIZE_STRICT=0` lets the worker pool grow past
the pre-warm, so correctness never depends on the pool size — a `scope` with more children than the pool
never stalls; pre-warm is a pure startup-latency optimization. The pre-warm count reads from a
**`KAMA_PTHREAD_POOL` env var at build time** (default `0`; same idiom as the driver's `EMCC`/`KAMA_HOME`
reads), so it's tunable per build with no compiler recompile. (Tuning the pre-warm of an *already-built*
`.wasm` per run would need emscripten's JS-expression form of `-sPTHREAD_POOL_SIZE` — deferred, unneeded.)

**Known caveat (serving, not codegen): browser needs cross-origin isolation.** `SharedArrayBuffer` is enabled
by default under **node** (the test harness — node v22, no flags needed), but a **browser** page serving a
Kama concurrency binary must send `COOP: same-origin` + `COEP: require-corp` headers to unlock it. That's a
deployment concern, out of scope here; noted for whoever ships a threaded Kama app to the web.

## M6.1 — landed (2026-07-23)

`Atomic<T>` shipped — the ONE sanctioned cross-isolate shared-**mutable** cell (the concurrency analog of
`unsafe {}`/`Ptr`). Pure-kama surface (`lib/std/concurrent/atomic.kama`, a move-only `resource` over one
inline `T`) over a new freestanding `kama_atomic.h` width-generic op layer that dispatches on `sizeof(T)` to
`__atomic_*_n` builtins — lock-free inline on native AND emscripten from one source. Element restricted to an
integer primitive / `usize`/`isize` / `Ptr`. Ordering = C11's `_explicit` idiom (`load()` + `loadExplicit(order:)`),
seq-cst by default. Exempted from the M4.2 same-root borrow rule (several children may `ref`-borrow one cell).
Fixtures: `atomic_counter`/`atomic_cas`/`atomic_flag`/`atomic_ordering` (+ `xfail/atomic_nonscalar`), all
green native + wasm + TSan.

## M6.2 — landed (2026-07-23)

immutable-`Shared` cross-isolate read sharing shipped — the first of the two *safe-sharing* primitives (§7a).
A `Shared<T>`/`Weak<T>` over a **deeply-immutable** `T` is now sendable/shareable across isolates: immutable
data is race-free even when shared, so several isolates can hold and read the same asset zero-copy.

- **`immutable` type qualifier** (`type immutable value|resource T`) — a greppable modifier (kama.l/kama.y),
  verified by a compiler fixpoint `computeDeeplyImmutable()` (the AND/greatest-fixpoint dual of
  `reachesPointer`): a qualified type is deeply immutable iff every field/base/variant-payload is a primitive,
  `string`, `enum`, or another deeply-immutable type — no raw `Ptr`, `Owned`/`Shared`/`Weak`, or mutable
  collection. A qualified type with a mutable member is a compile error **naming the member**. Distinct from a
  `const` binding (which permits a mutable alias, so cannot license cross-isolate sharing).
- **Sendability** — a `Shared`/`Weak` over a deeply-immutable element no longer sets `reachesSharedWeak`, so
  the channel-sendability gate and cross-scope `ref`-borrow accept it automatically.
- **Two-flavor atomic refcount** — the control-block strong/weak ops route through a freestanding
  `kama_ctrl.h` seam selected **per Shared/Weak instance** at emit time (`__kama_ctrl_atomic()` → 0/1 from a
  `useAtomicRefcount` flag): an ordinary `Rc` keeps the non-atomic ops (zero overhead); a `Shared<immutable T>`
  gets Arc-correct atomics (relaxed retains, release + acquire fence on the last drop, a CAS `tryUpgrade`).
  Immutable graphs are acyclic by construction, so the atomic drop needs no cycle dance. `__atomic_*` lowers
  on native AND emscripten from one source. *(Scope: the concrete-element **library** path — the primary
  target — is done; the intrinsic fat-pointer path for `Shared<immutable Contract>` is a follow-up.)*

Fixtures: `shared_immutable_send` (4 isolates hammer clone/drop on one `Shared<immutable Leaf>`),
`shared_immutable_parallel_read` (3 isolates read a nested immutable asset zero-copy),
`xfail/immutable_mutable_field`; the mutable-payload `xfail/channel_send_shared` still rejects. All green
native + wasm + TSan (0 data races) + ASan.

## M6.3 — landed (2026-07-23)

disjoint-slice `parallel_for` shipped — the third and final safe-sharing primitive (§7b), which **closes the
concurrency campaign**. `parallel_for (ref T e in coll) { … }` splits `coll` into K non-overlapping
sub-`View`s (one per worker isolate), runs the body over each in place through a `ref T e` binding, and joins
them ALL at its own closing brace. **Safe by disjointness**: two workers never touch the same element, so no
lock and no data race — the data-parallel (rayon / OpenMP) half of the model, with no borrow checker.

- **Self-joining barrier.** The parallel_for owns its own **dynamic** K-way fan-out and join (unlike a
  `scope`'s static child list) — the closing brace is the barrier, so a following statement sees every slice
  reclaimed. Standalone-usable (no enclosing `scope` needed) and composes inside a `scope` (it joins at its
  own brace, before the scope's). This is exactly the "dynamic N-way fan-out is really M6's parallel_for"
  note the M4 section anticipated.
- **Input = a `View<T>`, or any contiguous container exposing `.view()`** (`DynamicArray`/`FixedArray`
  auto-viewed). A non-contiguous collection (a `Map`, …) has no `.view()` and is rejected — for free, no
  special-casing. K disjoint slices are K calls to the existing `View.slice(from,count)` (zero-copy); no new
  `split_at` primitive was needed.
- **Worker count K = hardware cores by default** (`kama_parfor_workers()` → `sysconf`/`emscripten_num_logical_cores`),
  capped at the collection length, overridable at build time via a `KAMA_PARFOR_WORKERS` env → driver
  `-DKAMA_PARFOR_WORKERS_DEFAULT` (the M5 `KAMA_PTHREAD_POOL` idiom). Disjoint slices + a join barrier make K
  a pure speed knob — never a correctness one (tests pin it for determinism).
- **The body is HOISTED into a synthesized worker fn.** Kama has no closures (a `spawn` entry is a bare
  capture-free fn), so the emitter runs a **free-variable analysis** of the body and threads each captured
  enclosing local into the worker as a `ref` param — reusing the M4.2 `_refParams` deref lowering verbatim
  (the "per-entry arg-bundle struct" the M4 note anticipated). The loop lowering itself is the existing
  `foreach (ref …)` iterator path over the sub-`View`, reused via a synthesized `ForEachNode` — zero
  duplication. A per-site arg struct + trampoline crosses the isolate ABI's single `void*`.
- **Write-through-capture gate (the sole new static rule).** Writes through the disjoint element `e` are
  always fine; a captured local may be **read** freely (immutable reads don't race, §7a) but **written** only
  if it is an `Atomic<T>` (the sanctioned shared-mutable seam — reusing the M4.2 atomic exemption).
  A write to a non-atomic capture (assignment, `++`, a non-`const` method call, or a `ref`/`out` arg) is
  rejected, naming the culprit. Accessing `this`/a field is rejected (a worker runs with no receiver —
  shared-nothing, like a `spawn` entry).

Where it lives: `parallel_for` keyword (`kama.l`); `parallel_for_statement` (`kama.y`, mirrors
`foreach_statement` but `ref` is mandatory + a `block` body); `ParallelForNode` (`kama.ast.h`/`kama.forward.h`).
`CEmitter::emitParallelFor` + the free-variable walker (`kama.cemit.cpp`); `kama_parfor_workers()`
(`kama_isolate.h`, native + wasm arms); the driver `-D` (`kama.driver.cpp`). No new runtime beyond the one
helper (reuses `kama_isolate.h`'s spawn/join).

Fixtures: `parfor_double` (=90, auto-view + in-place doubling), `parfor_view_direct` (=40, direct `View`),
`parfor_capture_read` (=36, read capture — the walker's self-check), `parfor_atomic` (=100, `Atomic` reduction
— the write-exemption), `parfor_in_scope` (=112, composition inside a `scope`);
`xfail/parfor_write_capture` (non-atomic write), `xfail/parfor_noncontiguous` (a `Map`), `xfail/parfor_this`.
All green native + wasm + TSan (0 data races) + ASan.

## Deferred (per §6)

Co-equal general shared-memory ("hybrid") threading; an M:N green-thread runtime / `async`/`await` in the
language. Reopen only if a concrete case the seam + poller + a native scheduler library cannot express appears.

**Immutable-`Shared` over a CONTRACT element (the M6.2 intrinsic fat-pointer path).** M6.2 shipped the atomic
refcount for the concrete-element *library* path only. Sharing a *polymorphic* immutable object across isolates
— `Shared<immutable Contract>`, the analog of Rust's `Arc<dyn Trait + Sync>` — is deliberately **deferred as
YAGNI**: in kama it is largely absorbed by two already-working paths — a *closed* polymorphic set is an
immutable sum type (`type immutable value Expr { case … }`, a concrete type on the shipped path), and a
single-implementation object is just its concrete immutable type. The intrinsic path is genuinely needed only
for *open* polymorphism (plugin/extensible interfaces) shared immutably across isolates, which has no consumer
today. **Reopen when a concrete open-polymorphism case appears.** Design when wanted: an `immutable contract`
(all implementers compiler-enforced deeply immutable) + routing the `KAMA_SHARED/WEAK_FUNCS` macros and the
emitter-direct `ctrl->strong++` sites through the existing `kama_ctrl.h` seam (the `CollectionInfo`
`useAtomicRefcount` flag is already set for it). Cheap interim option: reject `immutable` on a contract with a
clear "not yet supported" message rather than silently ignoring it.
