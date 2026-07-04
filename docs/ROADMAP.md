# cstar roadmap

The forward plan. The **language feature set is complete**. History lives in the git
log; this file is only about what's next.

## The shape

- **1.0 — language complete.** Every *language* feature has landed: OO + RAII + the
  value/ownership model, generics, sum types + pattern matching, operator overloading. After
  1.0 the language surface is stable — you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection +
  serialization, file I/O, networking, an embedded/MCU target. Mostly library + codegen, little
  new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via
  a shared IR feeding C, direct-wasm, and a bytecode VM — the `cstar` binary self-contained.
- **Concurrency — shared-nothing by construction** (1.x/2.0 direction): data-race freedom by
  removing shared mutable state, not by a borrow checker. 1.0 ships a single-threaded core.
- **Engine track** (the product north star): a portable lightweight **WebGPU** game engine,
  woven through 1.x. Its Tier-0 math types are already unblocked by operator overloading.

## Remaining before 1.0

1. **Documentation & spec reconciliation** *(in progress)* — bring every doc current with the
   shipped language and settle the naming/case conventions (PascalCase types, lowerCamel
   methods, no `I`-prefix on contracts, lowercase `string`). 1.0 is the API-stability point,
   so naming is fixed here.
2. **Tag 1.0** — nothing else is deferred; the language is complete.

## Tracked limitations & non-goals

Policy: **no known limitation stays untracked** — each is either scheduled or a declared
non-goal.

- **`export` keyword** — reserved, hard-errors today; **tracked to 2.0** (the cstar→host
  boundary: WASM module exports for the browser engine + the scripting host interface). The
  engine's wasm build may pull a minimal `export` earlier.
- **`volatile` keyword** — reserved, hard-errors today; **tracked to 1.x → Embedded/MCU**
  (emit C `volatile` for ISR↔loop flags / MMIO registers).
- **`contract` refining a `contract`** (`type contract A : B`) parses, but deep multi-level
  contract inheritance is not yet lowered — revisit if a real case needs it.
- **By-value collection params/returns** — `give`-ing a collection into a variant payload
  works; general by-value collection params/returns (and deep-`copy` of a whole container into
  a variant) are **tracked to post-1.0** (same move-the-struct mechanism; not blocking).
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts
  with "one way to do a thing," and **named parameters** already cover the disambiguation
  overloading is usually reached for. **Operators are the sanctioned exception** — a type may
  carry several `operator*` distinguished by operand type (`mat*vec`, `mat*mat`, `v*s`, `s*v`),
  matching C++/C#/Rust. Reopen only if a concrete example shows named params can't express it.

## 1.x — systems & runtime (post-1.0)

Built ON the finished language; mostly library + codegen, not new syntax. These are also the
substrate the engine needs (asset I/O, scene serialization, networking).

- **Reflection + declarative serialization** — opt-in compile-time attributes/reflection →
  multi-format serialization (binary / json / yaml), little-endian canonical; scenegraph +
  serializer selection.
- **File I/O** — safe file APIs; gates serialization and engine asset loading.
- **Networking** — native UDP/TCP sockets vs browser **WebRTC DataChannels** (unreliable) /
  **WebSockets** (reliable), via FFI (the browser has no raw sockets — a real wasm nuance).
- **Embedded / MCU target** — globals/statics for ISR flags, `volatile` *emit*, ISR attributes,
  no-heap mode, avr/arm toolchains.
- **Native dispatch devirtualization** — on the `dispatch` bench cstar matches C on a real
  vtable indirect call, but Rust is ~5× faster because it devirtualizes/inlines the monomorphic
  case. A devirtualization / speculative-inlining pass, or `final`-method static-call lowering,
  would close the one real native gap.

## Concurrency — shared-nothing by construction (design direction)

The intended concurrency model. **1.0 ships a single-threaded core**; this is the 1.x/2.0
direction, not a shipped feature. It earns data-race freedom the way cstar earns null-safety — by
making the hazard *unrepresentable*, not by checking it. Where Rust proves exclusivity over shared
memory with a borrow checker, cstar **removes the shared mutable state**.

- **Model — isolates + ownership-transferring channels.** An *isolate* is a shared-nothing unit of
  execution (≈ an OS worker natively, a Web Worker on wasm). Crossing a channel reuses the existing
  ownership model: send a `value` → **copy**; send a `resource` → **`give`** (move, zero-copy;
  use-after-send is already a compile error via move tracking); genuinely shared hot-path data → a
  narrow **`Atomic<T>` / shared-region** seam — the concurrency analog of `unsafe { }`/`Ptr` at the
  FFI boundary (opt-in, greppable, atomics-only).
- **Maps 1:1 onto wasm.** isolate → Web Worker; `give` across a channel → postMessage
  *transferable* (zero-copy, browser-enforced no-use-after-transfer); shared-region →
  SharedArrayBuffer + Atomics. Concurrency stays portable native↔browser from one source — which
  threaded C++/Rust do not.
- **Isolate vs job — two levels.** An *isolate* is the unit of *isolation* (few — roughly one per
  core / one Web Worker); a *task/job* is the unit of *work* scheduled onto isolates (many). The
  engine's job system / scheduler is a library on top, not language.
- **Structured concurrency = RAII for tasks.** A concurrency scope joins its child tasks at scope
  exit — deterministic task lifetimes, no orphans. The concurrency version of the no-leak
  guarantee; on-brand with RAII.
- **"Proceed until ready" without coloring.** The do-other-work-until-a-result-is-ready ergonomic
  is cheap tasks that block on a channel while a scheduler runs other ready work (the Go/Erlang
  model) — **not** Rust-style stackless `async/await`. Function coloring / `Pin` /
  self-referential state machines would be cstar's least-cstar feature, against "one way / favor
  simplicity."
- **Lock-free default, locks as expert opt-in.** The default path has no shared state → no locks.
  Atomics power the expert lock-free structures, built once in the engine/stdlib (as Rust's
  std/crossbeam build theirs over `unsafe`). No mandatory mutex-everywhere model.
- **Recommended language surface.** `isolate`/`task`, an ownership-transferring channel (reusing
  `give`/`copy`), `Atomic<T>`, a structured-concurrency scope, and two targeted *safe* sharing
  primitives that recover what shared-nothing otherwise costs:
  - **immutable `Shared` read-across-isolates** — immutable data is race-free even when shared
    (recovers cheap read-only sharing of big assets);
  - **scoped disjoint-slice parallel-for** — a scope lends each task a non-overlapping mutable
    slice of one buffer and reclaims it at join; safe by disjointness (the `rayon`/`split_at_mut`
    pattern). Recovers data-parallel mutation without general shared memory.
- **Deferred — general shared-memory ("hybrid").** Co-equal shared-memory threading is *not*
  planned; it reintroduces the hazard the model removes. Capability is retained (via the seam + the
  two primitives above); only some ergonomics move behind the seam. Reopen only if a concrete case
  the seam can't express appears — the same discipline as the overloading non-goal.
- **Positioning.** This turns concurrency from "the last gap vs Rust" into a *different, simpler,
  more portable* safe-concurrency model. Honest trade: Rust's shared-memory-with-static-exclusivity
  is more flexible for max-perf shared mutation; cstar's shared-nothing is far easier to reason
  about and portable to wasm. Prior art: **Dart isolates** (closest — shared-nothing, native+web),
  **Erlang/Elixir** actors, **Web Workers** + SharedArrayBuffer, **structured concurrency**
  (Swift/Kotlin/Trio); **Pony** for the type-level ceiling.

## 2.0 — dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the same cstar syntax usable both compiled and as a
scripting language with a full REPL. The guiding constraint: **the `cstar` binary is the only tool
you need.** External C toolchains stay *optional* — used for the portable-C release path, never
required to write, run, or iterate on cstar.

This rests on a **polymorphic emitter**: one front end lowered to a **shared IR**, then rendered
by several backends.

```
   Frontend  (parser → type checker → ownership/move analysis)
                          │
                          ▼
                     shared IR          (monomorphized, drops inserted,
                          │              vtables + match/operators desugared)
          ┌───────────────┼────────────────┐
          ▼               ▼                 ▼
          C              WASM            Bytecode
          │               │                 │
     TinyCC / clang    browser /          native VM
     (JIT + release)   Wasmtime         (REPL, self-contained)
```

Every backend shares the same front end, so the safety analysis (ownership, move tracking,
exhaustiveness) is proven **once**, before the IR.

- **C backend — the portability moat (kept, always).** cstar → readable portable C → any C
  toolchain. `clang`/`emcc` for release; a bundled **TinyCC** for near-instant in-process JIT
  (`cstar run foo.cstar` and the REPL are **JIT-compiled, not tree-walked**). C stays a
  first-class target — "runs anywhere C runs" is the whole moat, and these new backends are
  *additive*, never a replacement.
- **WASM backend — the self-contained web path.** Direct cstar → wasm (no `emcc`), run in the
  browser or under Wasmtime. This is the web scripting/engine substrate; C→emcc remains the
  maximal-compatibility option.
- **Bytecode + VM backend — the self-contained native REPL.** A cstar-owned VM gives a true
  interactive REPL with zero external tooling — the strongest read of "the binary is the only tool
  you need."

**The IR is the crux, and the real work.** Today there is no IR: the C emitter writes C text
directly and *bakes in* monomorphization, RAII drop insertion, vtable layout, and match/operator
desugaring (`cstar.cemit.*`, ~150 methods). The refactor pulls that **semantic lowering up into
the shared IR**, leaving each backend a comparatively dumb renderer. Design constraints:

- **Keep the IR high-level and structured** (retain `if`/`while`/`for` and named locals), *not*
  SSA/basic-blocks — so the C backend can still emit the readable, `#line`-mapped C that is a
  headline feature. A low-level IR would forfeit that.
- **Move the runtime into cstar.** Collections/smart-pointers/`string` live as hand-tuned C in
  `cstar_runtime.h` today; a wasm or VM backend can't `#include` it. Reimplementing those
  intrinsics as **cstar generic library types** (unsafe core, safe API — already flagged in
  GOALS #1 for self-hosting) makes them flow through the shared IR and monomorphize into *any*
  backend. Multi-backend and the cstar-stdlib/self-hosting goal are the **same project**: do it
  once, all three backends inherit it.
- **Contain semantic drift.** A VM is a second execution semantics — the main risk. Having the C
  backend and the VM consume the *same lowered IR* reduces drift from "two languages" to "two
  renderers of one IR." Build the IR first; then a VM is a legitimate, low-drift option.

**Speed ladder** (fastest last): tree-walk < bytecode VM < TinyCC-JIT < AOT C→clang. If raw
scripting speed dominates, JIT wins; if zero-install and interactivity dominate, the VM /
direct-wasm win. The "binary is the only tool" constraint tilts the *default* iteration experience
toward the VM + direct-wasm, with the C/JIT path there when you want native speed or C
compatibility.

- **Why it beats other scripting languages:** Python/Ruby/Lua are bytecode interpreters; cstar
  scales from a self-contained VM up to JIT/AOT-native — the same source, at or near native speed.
- *Licensing note:* TinyCC is LGPL; if a bundled JIT ships, confirm the linking terms against the
  MIT/permissive goal (GOALS #8). The VM / direct-wasm paths sidestep this entirely.

## Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked) →
buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for
assets, serialization for scenes). See [ENGINE_READINESS.md](ENGINE_READINESS.md).

## Performance (from the benchmark suite — [benchmarks/RESULTS.md](benchmarks/RESULTS.md))

cstar is at **C/C++ parity** on native compute and wins decisively on footprint (~2 MB RSS,
~66 KB binary) and the no-GC `alloc` workload. `cstar→wasm` (optimized) **beats hand-written JS
on fib/pi/collatz/fnptr (up to ~4.5×)** and ties on dispatch/alloc.

- **Native dispatch devirtualization** (above, under 1.x) is the one real native gap.
- **WASM tiering.** Measure at the optimizing tier (`node --no-liftoff` — what a real long-running
  app gets); the bench forces TurboFan for the wasm track so the numbers reflect steady-state, not
  V8's short-lived baseline (Liftoff) compiler.
- **Bench methodology (don't re-chase).** Short workloads are skewed by parallel load — run the
  bench with nothing else competing. The `/work` bind mount adds only ~0.3–1.7 ms (negligible);
  a named volume / tmpfs speeds up *builds* but not the measured numbers.

## Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to releases; Marketplace
  publishing is deferred (pair with an eventual rename).
- **Brand rename** — "C*"/cstar is crowded; revisit before publish. Current name kept for now.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
