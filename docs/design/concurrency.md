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
  moved arg bundle and join it. `isolate` surface + emitter lowering. No channels yet. Add `KAMA_TSAN`. ✅ *(landed 2026-07-22 — see below)*
- **M3 Channels** — `channel<T>`; blocking send/recv reusing `give`/`copy` + move-tracking; structural
  sendability check with field-naming errors; bounded + rendezvous. *(kickoff prepped 2026-07-22 — see below)*
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

## M3 — implementation kickoff (verified hooks 2026-07-22 — don't re-explore)

A running start for the next session. M2 (the isolate seam) is shipped + green. M3 adds **channels**: a typed,
blocking `channel<T>` for moving values between isolates. *(Remove this section once M3 lands, as this doc's
"M2 — landed" note replaced the M2 kickoff.)*

**The core tension (read first).** M2 is shared-nothing by construction. A channel is the **first sanctioned
cross-isolate shared mutable state** (the spec's greppable sharing seam, §6/Q6–Q7). ONE heap object (the queue)
is co-owned by endpoints living on different threads. kama's `Shared<T>` refcount is **plain, non-atomic**
(`prelude/std/memory/shared.kama:11` — `type value Ctrl { usize strong; usize weak; }`, `strong++`/`strong--`
at :65/:86), so a cross-isolate `Shared<Queue>` would **race the refcount**. **Decision: put ALL thread-safety
in C** — a dedicated `kama_channel.h` owning the queue + `pthread_mutex` + two condvars + endpoint-liveness flags,
mirroring how `kama_isolate.h` owns the threads. Because every state transition (send, recv, endpoint-drop) runs
under the one mutex, **no atomics are needed** — the mutex serializes liveness + free too (the second endpoint to
drop frees, race-free). The kama side stays a thin `Ptr`-handle library, exactly like `Isolate`.

**Recommended surface** — a `Channel<T>` factory producing a **move-only** `Sender<T>` / `Receiver<T>` pair (SPSC
for bring-up; multi-producer `sender.clone()` is a follow-up). The sender moves into a spawned isolate's bundle;
the receiver stays (or vice-versa):

```
import std::concurrent::{Channel, Sender, Receiver};

fn void producer(Sender<int32> tx) {           // a `resource` bundle (Ptr handle) — moves into the isolate
    int32 i = 0;
    while (i < 10) { tx.send(give i); i = i + 1; }
}                                               // ~Sender() marks the channel sender-closed → wakes recv

fn int main() {
    Channel<int32> ch = Channel::bounded(capacity: 4);   // capacity 0 == rendezvous (M3.3)
    Sender<int32>   tx = ch.sender();
    Receiver<int32> rx = ch.receiver();
    isolate producer(tx: give tx);              // reuse the M2 bundle move (Sender is a move-only resource)
    int32 sum = 0;
    Optional<int32> v = rx.recv();              // blocks; None once drained AND all senders dropped
    while (v != Optional::None) { sum = sum + v.unwrap(); v = rx.recv(); }
    return sum;                                 // 45
}
```

`recv()` → `Optional<T>` (None = closed+drained). `send(give x)` → `bool` (false if the receiver is gone). Both
BLOCK. `send`/`recv` move a T **by memcpy relocation** (the same bitwise move the isolate bundle uses): send
copies T's bytes into a queue slot and **marks the source moved** (no double-drop — the receiver's `recv` produces
the sole owning copy). This is why the milestone says "reusing `give`/`copy` + move-tracking" — see decisions below.

**Design decisions (recommend / flag):**
1. **`send`/`recv` are emitter-lowered intrinsics on the endpoint types, NOT pure library methods.** They must
   splice into the move seam (memcpy + `markMoved(source)` for send; produce a fresh owned T for recv) so the
   compiler suppresses the moved value's destructor — the SAME mechanism `isolatePrep` uses (`kama.cemit.cpp`
   :6036 `emitExpression(src)` + `moveOnlySource`/`markMoved` :6050). Model the dispatch on smart-ptr method
   interception (`emitSmartPtrCall`, `isSmartPtrClass` :5623). *(Alternative considered: a `mem::forget(give T)`
   intrinsic keeping send pure-library — more general but more surface; the endpoints already need emitter
   awareness for the memcpy+sizeof, so intrinsic `send`/`recv` is less total code.)*
2. **Structural sendability gate.** T may cross only if it does not transitively reach a **non-atomic shared
   refcount** — i.e. no `Shared<X>`/`Weak<X>` in T's field/variant/base graph (`Owned<X>` is fine: unique
   ownership, the move transfers it whole; plain values, collections of sendable, resources of sendable fields
   are all fine). Build this as a fixpoint field-walk cloned from **`computeReachesPointer()`** (`kama.cemit.cpp`
   :6452–6514) — restricted to `CollKind::Shared|Weak` — with the field-naming error the milestone asks for:
   "cannot send `T` over a channel — its field `x: Shared<Y>` shares a non-atomic refcount across isolates; use
   `Owned<Y>` (unique) or send `Y` by value." `computeDestructible()` (:6373–6443) is the walk template (base +
   `ci.fields` + `ci.variants`, resolving generics under `_typeSubst`).
3. **Endpoints are move-only `resource`s holding a `Ptr` handle** (the `Isolate` pattern,
   `lib/std/concurrent/concurrent.kama:23`). `~Sender()`/`~Receiver()` call `kama_channel_drop_sender/receiver`
   (mark that side closed, wake the other, free when both closed). A resource holding a `Ptr` is sendable +
   drop-safe (INVARIANT #0: null handle ⇒ drop is a no-op).
4. **Bounded first, rendezvous second.** Bounded (`capacity ≥ 1`) is a ring buffer; rendezvous (`capacity 0`) is a
   direct hand-off needing a slightly different handshake — land it in M3.3, not bring-up.
5. **recv/send return via the prelude `Optional<T>` / `bool`** (no new error type for M3). A `recv`-with-timeout
   rides `std::time::Duration` (`lib/std/time/time.kama`) + `pthread_cond_timedwait` — defer to a follow-up.

**Verified hook points** (file:line — `kama_isolate.h` + the M2 isolate seam are the copy-me analog):

| Layer | File:line | What to do |
|---|---|---|
| Runtime ABI | **new `kama_channel.h`** (mirror `kama_isolate.h` — `#include <pthread.h>` + `"kama_runtime.h"`; no sync primitives exist in the tree yet, grep-confirmed) | `typedef struct { pthread_mutex_t mu; pthread_cond_t notEmpty, notFull; uint8_t* buf; size_t elemSize, cap, count, head, tail; int senderLive, receiverLive; } kama_channel_t;` + `static inline` `kama_channel_new(elemSize, cap)` / `_send(ch, elem)→int` / `_recv(ch, out)→int` / `_drop_sender`/`_drop_receiver` (all under `mu`; last dropper frees) |
| Stdlib | `lib/std/concurrent/` (new `channel.kama`; the `concurrent.kama` `Isolate` pattern :23) | `extern "kama_channel.h";` + `extern fn Ptr kama_channel_new(usize elemSize, usize cap);` etc.; generic `resource Channel<T>` / `Sender<T>` / `Receiver<T>` (each a `Ptr handle`) with `~` calling the drop FFI; `export`. Generic-type syntax: `type resource Sender<T> { Ptr handle; … }` (see `prelude/std/memory/owned.kama:13`, `dynamic_array.kama:39`) |
| Emitter — send/recv | `kama.cemit.cpp` — intercept like `emitSmartPtrCall` (:5985) / method dispatch; reuse the move seam `isolatePrep` uses (:6036/:6050) | lower `tx.send(give x)` → `kama_channel_send(handle, &x_tmp)` (memcpy the bytes; `markMoved` the source) returning bool; `rx.recv()` → an `Optional<T>` built from a `T out; kama_channel_recv(handle, &out)` (the fresh owned value). `sizeof(T)`/`cType(T)` under `_typeSubst` give the element size (`kama.cemit.cpp` :1080 sizeof, :234 cType) |
| Emitter — sendability | `kama.cemit.cpp` new pass beside `computeReachesPointer` :6452 / `computeDestructible` :6373 | the field-walk gate of decision #2; run it where a `channel<T>`/`Sender<T>`/`Receiver<T>` is instantiated (the generic-type registration path, `registerGenericTypeInst` :4180) so the error fires at the use site |
| Driver link | `kama.driver.cpp` — the `needsIsolate`/`-lpthread` machinery (:620 bool, :345/:387 `externsHeader`, :787 link gate) | add `needsChannel` set from `externsHeader("kama_channel.h")`, threaded through the three transpile fns like `externsIsolate`; gate `-lpthread` on `(needsIsolate \|\| needsChannel) && !wasm` (channels imply pthreads) |
| Tests | `run_tests.sh` — the M2 `KAMA_TSAN` sweep + the `std::concurrent` wasm-skip already added | new fixtures below run under the existing `KAMA_TSAN=1`; the wasm-skip (`grep std::concurrent`) already covers `channel.kama` importers |

**Draft M3 fixtures:** (1) **bounded producer→consumer**: spawn a producer isolate that sends N ints, `recv` them
in main until None, assert the sum — **TSan-clean** under `KAMA_TSAN=1` (the cross-isolate hand-off proof);
(2) **backpressure**: `capacity 1`, producer sends more than fits so `send` blocks until the consumer drains —
assert all values arrive in order; (3) **close semantics**: producer drops its `Sender` early; `recv` returns
`Optional::None` after draining (not a hang/crash); (4) **xfail sendability**: `channel<BadT>` where
`BadT { Shared<X> s; }` → the field-naming sendability error (decision #2); (5) **rendezvous** (M3.3): `capacity 0`
hand-off. All native + `KAMA_TSAN` + `KAMA_SAN` green; skipped on the wasm leg (native-only until M5).

**Suggested sub-stages:** M3.1 bounded channel ABI + endpoints + emitter send/recv (fixtures 1–2) → M3.2 the
structural sendability gate (fixture 4) → M3.3 rendezvous + close-drain polish (fixtures 3, 5).

**Risks / unknowns:** (a) **destructor suppression on `send`** — the moved value must not be double-dropped;
confirm `markMoved` on the send source fully suppresses the caller-side drop as it does for the isolate bundle
(it should — same seam). (b) **recv of a T that owns heap** (e.g. `channel<DynamicArray<int32>>`) — the memcpy
relocates the `{ptr,len,cap}` struct; verify the sender's source is not dropped and the receiver's copy is the
sole owner (bitwise move is correct, but write a fixture). (c) **rendezvous handshake** (cap 0) is the fiddly bit
— a bounded ring with `cap≥1` is the safe bring-up; don't start with rendezvous.

## Deferred (per §6)

Co-equal general shared-memory ("hybrid") threading; an M:N green-thread runtime / `async`/`await` in the
language. Reopen only if a concrete case the seam + poller + a native scheduler library cannot express appears.
