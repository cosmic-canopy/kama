# Concurrency — campaign kickoff (design-refinement pass FIRST)

**Status:** not started. **The next big direction after 1.0.** The *direction* is settled
([ROADMAP.md §6](../ROADMAP.md) — shared-nothing by construction: isolates + ownership-transferring
channels); what's **not** settled is the *spec*. Per the agreed plan, this campaign **opens with a
design-refinement pass** — turn §6's direction into a concrete, kama-simple specification — **before** any
runtime or emitter code. Prepared as a running start for a fresh session (mirrors how the SIMD campaign was
kicked off).

## The direction (already agreed — don't relitigate, refine)

Earn data-race freedom the way kama earns null-safety: make the hazard **unrepresentable**, not checked. No
borrow checker over shared memory — **remove the shared mutable state.** The full rationale + surface sketch is
[ROADMAP.md §6](../ROADMAP.md); the load-bearing commitments:

- **Isolate = shared-nothing unit of execution** (OS worker natively; Web Worker on wasm). **Channels reuse the
  existing ownership model**: send a `value` → **copy**; send a `resource` → **`give`** (move, zero-copy; the
  use-after-send error already falls out of move-tracking); genuinely-shared hot data → a narrow **`Atomic<T>` /
  shared-region seam** (the concurrency analog of `unsafe {}`/`Ptr` — opt-in, greppable, atomics-only).
- **Maps 1:1 onto wasm** (isolate→Worker, `give`→postMessage *transferable*, shared-region→SharedArrayBuffer).
- **Structured concurrency = RAII for tasks** (a scope joins its children at exit; the no-orphan guarantee).
- **"Proceed until ready" without function coloring** — cheap tasks that block on a channel while a scheduler
  runs other ready work (Go/Erlang), **not** stackless `async/await`/`Pin` (kama's least-kama feature).
- **Lock-free default; locks as expert opt-in.** Two *safe* sharing primitives recover what shared-nothing
  costs: immutable-`Shared` read-across-isolates, and scoped disjoint-slice parallel-for (`split_at_mut`).
- **Deferred:** co-equal general shared-memory ("hybrid") threading — reopen only if the seam can't express a
  concrete case.

## Current state (verified 2026-07-22 — don't re-explore)

- **No concurrency infra of any kind exists.** No threads/isolates/atomics/channels/scheduler; the only
  `pthread` reference is `-lpthread` for native GLFW ([kama.driver.cpp](../../kama.driver.cpp) ~772), unrelated.
  **1.0 ships a single-threaded core** — this is a clean slate.
- **I/O is blocking, single event loop.** `std::net` streams are blocking by default (`setNonBlocking(true)` →
  `Err(WouldBlock)`; [lib/std/net/net.kama](../../lib/std/net/net.kama) ~34, 56–88), with a synchronous
  select/epoll `Poller::wait(timeoutMs)` ([lib/std/net/poll.kama](../../lib/std/net/poll.kama) ~52). Wasm runs
  one emscripten main loop (`emscripten_set_main_loop_arg`, [kama_app.h](../../kama_app.h) ~15–33); native is a
  plain `while(tick){}`. **No emcc `-sUSE_PTHREADS`/`-sPROXY_TO_PTHREAD`, no Worker hooks** today.
- **`std::time` does NOT exist** — the stated cheap **precondition**. Modules are directory-modules under
  `lib/std/` (`math`, `io`, `net`, `collections`, …); `std::time` slots in as `lib/std/time/` with
  `namespace std::time;` (native `clock_gettime`/`QueryPerformanceCounter`; wasm `emscripten_get_now`). Needed
  for timeouts, scheduling, and any bench of the scheduler.
- **The move/ownership seam a channel-send reuses already exists** (no new tracking needed):
  `moveOnlySource()` extracts the named local from a `give` expression; `markMoved()` transitions it to `Moved`
  in `_moveState`; `ownsByValue()` is the "hand-off needs `give`/`copy`" gate; `value`-vs-`resource` copy-vs-move
  is decided from `ClassInfo::kind` ([kama.cemit.cpp](../../kama.cemit.cpp) — `moveOnlySource`/`markMoved`/
  `ownsByValue`/`isCopyable`, ~5850–5890 + decl-init ~1776 / assign ~2134). A `chan.send(give x)` lowering hooks
  straight into these: extract cVar → validate `ownsByValue` → `markMoved` → emit the handle move; later use of
  `x` already errors via move-tracking.
- **Per-isolate module state seam** (the "per-thread module isolation" interest): the prelude/module collection
  is `preludeUnit()` + `preludeModuleUnits()` ([kama.driver.cpp](../../kama.driver.cpp) ~314–320) →
  `setPrelude()`/`addPreludeModule()` → collected in `emitProgram()`
  ([kama.cemit.cpp](../../kama.cemit.cpp) ~12081). **Open question this raises:** does kama have any *mutable
  module-level/global state* today? If so, shared-nothing requires each isolate to own its own copy — a real
  emitter concern, not just a library one.

## ► The design-refinement pass (do this FIRST — the point of the kickoff)

Settle these before writing runtime/emitter code. Each is a place §6 gives a *direction* but not a *decision*:

1. **The portable spawn substrate — the hard one.** Native = OS threads (shared address space, so `give` is a
   pointer handoff, zero-copy); wasm = Web Workers (separate address spaces, so `give` is a postMessage
   *transfer*, and **wasm can't spawn its own workers — the JS host must**). How does *one source* express
   "spawn an isolate" when the wasm side needs host cooperation? Pin the runtime seam (a `kama_isolate_*` ABI in
   the runtime header, native-threads vs JS-worker-bridge behind it) and what the emitter emits for an
   `isolate`/`task`.
2. **Channel type + sendability.** The channel type signature; how the type system admits only send-safe
   payloads (`value`→copy, `resource`→`give`; **reject** raw `Ptr`/borrows/non-owning views escaping an
   isolate). Is "sendable" a contract/marker, or purely structural from the ownership kind? Blocking vs buffered.
3. **Per-isolate state & the prelude.** Resolve the module-global-state question above; decide whether each
   isolate re-collects prelude modules or shares immutable code + per-isolate data.
4. **Structured-concurrency scope = RAII for tasks.** How a concurrency scope joins children at scope-exit
   within the existing drop/RAII model and the borrow-vs-storage rule (a task must not outlive a borrow it holds).
5. **Scheduler / "proceed until ready."** The cooperative model (tasks block on channels; scheduler runs ready
   work) without coloring — what's language vs a stdlib job-system library (§6: the job system is a library).
6. **`Atomic<T>` seam.** New type + lowering (native `stdatomic.h`/`_Atomic`; wasm Atomics + SharedArrayBuffer)
   — the minimal shared-region opt-in. Scope it to atomics-only.
7. **The two safe-sharing primitives** — immutable-`Shared` read-across-isolates; scoped disjoint-slice
   parallel-for. Confirm they compose with the ownership model without reintroducing shared mutability.

Output of the pass: a `docs/design/concurrency.md` that is a **spec** (surface + lowering + runtime ABI), and
a milestone plan — same bar the construction-model / streams campaigns met before implementation.

## Precondition (cheap, do first)

- **`std::time`** — a monotonic clock + `Duration`/`Instant` (native `clock_gettime`; wasm `emscripten_get_now`).
  Small, independently useful, and unblocks timeouts + any scheduler benchmark. Good first commit of the campaign.

## Draft milestones (refine after the design pass)

- **M0 Design pass** — settle the 7 questions above → a concrete spec. *(This kickoff's immediate successor.)*
- **M1 `std::time`** — the precondition module, native + wasm, fixtures.
- **M2 Runtime isolate seam** — `kama_isolate_*` ABI: native threads first; a single isolate that runs a task
  and joins. No channels yet.
- **M3 Channels** — ownership-transferring send/recv reusing `give`/`copy` + move-tracking; blocking first.
- **M4 Structured-concurrency scope** — RAII-for-tasks join-at-exit.
- **M5 wasm parity** — Worker + postMessage bridge; `give`→transferable; the native↔wasm portability proof.
- **M6 `Atomic<T>` + the two safe-sharing primitives** — the narrow shared seams.

## Guardrails (north stars — verify every step)

- **No-GC / RAII / deterministic destruction** hold across isolates and task scopes (structured concurrency IS
  the RAII extension). Every commit **ASan/UBSan-clean** (and TSan becomes relevant once threads exist —
  consider adding a `KAMA_TSAN` sweep alongside `KAMA_SAN`).
- **No-null, no shared mutable state in the safe surface** — sharing is confined to the greppable `Atomic<T>` /
  shared-region seam, exactly as raw pointers are confined to `unsafe {}`.
- **Portable native↔wasm from one source** — the differentiator; the spawn/channel lowering must not fork the
  user-facing surface. If a construct can't map to Web Workers, it doesn't ship.
- **One way / favor simplicity** (GOALS.md) — no function coloring, no mandatory-mutex model; the cooperative
  "proceed until ready" model is the one concurrency story.
