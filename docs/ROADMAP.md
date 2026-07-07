# kama roadmap

The forward plan — near-term to long-term, read in sequence. The language's **history** lives in the git
log; what the language **is** lives in [SPEC.md](SPEC.md). This file is only *what's next*.

## The shape

- **1.0 — language complete.** Strings + math are the last core pieces; after them the language surface is
  stable — you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection + serialization,
  file I/O, networking, an embedded/MCU target. Mostly library + codegen, little new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via a shared
  IR feeding C, direct-wasm, and a bytecode VM — the `kama` binary self-contained.
- **Concurrency — shared-nothing by construction** (1.x/2.0 direction): data-race freedom by removing
  shared mutable state, not a borrow checker. 1.0 ships a single-threaded core.
- **Engine track** (product north star): a portable lightweight **WebGPU** game engine, woven through
  1.x. Its Tier-0 math types are unblocked now.

## 1. Remaining before 1.0

1. **Strings — Phase 3 (ergonomics).** `+` / `==` operators (compiler special-cases string operands →
   `concat`/`equals`; `string` is a primitive, so not a user overload), an owned `substring(start:, end:)`
   (byte-range copy — safe without lifetimes; a borrowed view would need lifetime tracking), and richer
   methods (`find`, `contains`, `startsWith`, `endsWith`, `isEmpty`). The core is done — see SPEC §Strings
   (`char`, byte `s[i]`, `.chars()`).
2. **Math layer.** `Vec2/3/4`, `Mat4` (`Fixed<float32,16>` or `Fixed<Vec4,4>`), `Quat` as ordinary `value`
   types — **no prerequisites** (operator overloading + `Fixed` shipped). Unblocks the engine's Tier-0 math.
   Buildable as value types now; package as a stdlib module once the packaging question (§3) is settled —
   forward-compatible either way.
3. **Docs reconcile → tag 1.0.** 1.0 is the API-stability point; naming/case conventions are fixed here
   (PascalCase types, lowerCamel methods, no `I`-prefix on contracts, lowercase `string`).

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal.

- **Multibyte source char literals** — `'é'` (a multibyte UTF-8 char between quotes) isn't lexed yet
  (single-byte + escapes + `'\u{…}'` only); write `'\u{E9}'`. A lexer pass to match + decode.
- **String interpolation `"${x}"` + formatting** — needs a general to-string / `Display`-like mechanism
  (also covers number→string); sequences with reflection (its to-string substrate).
- **`export` keyword** — reserved, hard-errors today → **2.0** (the kama→host boundary: wasm module
  exports + the scripting host interface). The engine's wasm build may pull a minimal `export` earlier.
- **`volatile` keyword** — reserved → **1.x embedded** (emit C `volatile` for ISR↔loop flags / MMIO).
- **`contract` refining a `contract`** (`type contract A : B`) — parses, but deep multi-level contract
  inheritance isn't lowered yet; revisit if a real case needs it.
- **By-value collection params/returns** — `give`-ing a collection into a variant works; general by-value
  collection params/returns (and deep-`copy` of a whole container into a variant) → **post-1.0** (same
  move-the-struct mechanism; not blocking).
- **Minor niceties (post-1.0):** an opt-in `Equatable` derive (auto `==` for `value` types) and
  post-increment returning the old value in expression position (`i++` works as a statement today).
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts with "one way
  to do a thing," and **named parameters** already cover the disambiguation overloading is usually reached
  for. **Operators are the sanctioned exception** — a type may carry several `operator*` distinguished by
  operand type (`mat*vec`, `mat*mat`, `v*s`, `s*v`), matching C++/C#/Rust. Reopen only if a concrete case
  shows named params can't express it.

## 3. Open design questions (settle before the work they gate)

- **Modular / opt-in stdlib — how does "pay for what you use" work?** The **prelude mechanism**
  (`PRELUDE_SRC` — parsed kama collected before user code, the model `Optional`/`Result`/`Chars` use) is
  the seed: a stdlib = more prelude-collected kama modules in a `Std` namespace. Generic types already
  emit only when instantiated, and `--gc-sections` prunes unused functions in release. Open: whether that
  pruning suffices, or explicit per-module opt-in / dead-function elimination is warranted before a large
  stdlib. A design pass before the container/math packaging.
- **Structural → nominal contracts for bounds.** `foreach` is now nominal (an iterator must `implements
  Iterator`/`IteratorMut`, a container `Iterable`/`IterableMut`), but a generic bound `<T: Weighable>`
  still accepts a type **structurally**. Decision (user): go fully nominal — require `implements` for
  bounds too, so the keyword is load-bearing everywhere and errors pin to the declaration. A migration
  pass over the bound fixtures.
- **`Copyable` as a formal contract.** Today it's recognized nominally by name (`implements Copyable`);
  formalize as `type contract Copyable { This copy(); }` — bundle with the structural→nominal migration.
- **Math: language-level value types vs a stdlib module.** Recommendation: build as `value` types now (no
  prereqs), and *package* as a module once the modular-stdlib question above is settled.

## 4. Reflection + attributes (1.x — design brief)

Opt-in compile-time reflection driving polymorphic serialization — the final self-hosting-stdlib step. The
only new *language* surface is the attribute mark; serializers land as modules.

- **Opt-in / zero-cost when unused.** *Nobody pays a byte or a cycle if they don't reflect.* A type is
  inert unless it opts in; no global registry, no per-type metadata emitted unless requested.
- **Declarative marks on types** — an attribute syntax (`[Reflect]` / `@derive(...)` — spelling TBD) opts a
  type in. This is the new language surface (grammar + AST + emit).
- **Granular — per-field opt-in** (mark which fields reflect / serialize / skip), not all-or-nothing.
- **Scenegraph-capable (composition)** — reflect object graphs, not just flat structs, which needs
  **temporary IDs** so a serialized graph round-trips shared/back references without cycles.
- **Polymorphic by serializer** — one reflection description, many back ends (text / binary / JSON / YAML;
  little-endian canonical for binary). The serializer is a module; reflection is the substrate it reads.

Mostly codegen over `ClassInfo`. **String interpolation rides on the same to-string substrate**, so it
sequences here.

## 5. 1.x — systems & runtime (post-1.0)

Capabilities built on the finished language — the substrate the engine needs (asset I/O, scene
serialization, networking).

- **Reflection + declarative serialization** — see the brief above; back ends follow as modules.
- **File I/O** — safe file APIs; gates serialization and engine asset loading.
- **Networking** — native UDP/TCP sockets vs browser **WebRTC DataChannels** (unreliable) /
  **WebSockets** (reliable), via FFI (the browser has no raw sockets — a real wasm nuance).
- **Embedded / MCU target** — globals/statics for ISR flags, `volatile` *emit*, ISR attributes, no-heap
  mode, avr/arm toolchains.
- **Native dispatch devirtualization** *(optimization, not a gap).* On a *monomorphic* call site clang
  does not devirtualize the emitted C vtable while rustc does — a clang-vs-rustc optimizer gap (hand-written
  C is equally behind), not a kama defect. kama can still win where it *sees* the concrete type by emitting
  a **direct call** instead of a vtable call — a laddered pass:
  - **Tier 1 — sound static devirtualization (no inlining).** Direct-call when the target is provable: a
    **concrete-value receiver**, a **`final` class/method**, or a **method with no overrides
    program-wide** (a slot→overridden map after `buildVtables()`). kama's whole-program view makes the
    last one free where C++ needs LTO + `-fwhole-program-vtables`. (`isFinalClass` / `MethodInfo::isFinal`
    / `exprClass()` already exist.) Land this first.
  - **Tier 2 — intraprocedural type-flow.** Devirtualize a base-typed local with a proven concrete
    assignment. Sound, no inlining.
  - **Tier 3 — inlining-enabled / guarded devirtualization.** A kama-level inliner (hard part: integrating
    callee scope-cleanup / drop order / move-state with `emitScopeCleanup`/`emitUnwindAll`) then re-run
    Tier 1, or guarded/speculative inline caches. A separate, larger project — pursue only if a real hot
    path (engine ECS dispatch) proves Tier 1 insufficient.

## 6. Concurrency — shared-nothing by construction (design direction)

The intended concurrency model. **1.0 ships a single-threaded core**; this is the 1.x/2.0 direction, not a
shipped feature. It earns data-race freedom the way kama earns null-safety — by making the hazard
*unrepresentable*, not by checking it. Where Rust proves exclusivity over shared memory with a borrow
checker, kama **removes the shared mutable state**.

- **Model — isolates + ownership-transferring channels.** An *isolate* is a shared-nothing unit of
  execution (≈ an OS worker natively, a Web Worker on wasm). Crossing a channel reuses the existing
  ownership model: send a `value` → **copy**; send a `resource` → **`give`** (move, zero-copy;
  use-after-send is already a compile error via move tracking); genuinely shared hot-path data → a narrow
  **`Atomic<T>` / shared-region** seam — the concurrency analog of `unsafe { }`/`Ptr` at the FFI boundary
  (opt-in, greppable, atomics-only).
- **Maps 1:1 onto wasm.** isolate → Web Worker; `give` across a channel → postMessage *transferable*
  (zero-copy, browser-enforced no-use-after-transfer); shared-region → SharedArrayBuffer + Atomics.
  Concurrency stays portable native↔browser from one source — which threaded C++/Rust do not.
- **Isolate vs job — two levels.** An *isolate* is the unit of *isolation* (few — ~one per core / Web
  Worker); a *task/job* is the unit of *work* scheduled onto isolates (many). The engine's job system is a
  library on top, not language.
- **Structured concurrency = RAII for tasks.** A concurrency scope joins its child tasks at scope exit —
  deterministic task lifetimes, no orphans. The concurrency version of the no-leak guarantee.
- **"Proceed until ready" without coloring.** The do-other-work-until-a-result-is-ready ergonomic is cheap
  tasks that block on a channel while a scheduler runs other ready work (the Go/Erlang model) — **not**
  Rust-style stackless `async/await`. Function coloring / `Pin` / self-referential state machines would be
  kama's least-kama feature, against "one way / favor simplicity."
- **Lock-free default, locks as expert opt-in.** The default path has no shared state → no locks. Atomics
  power expert lock-free structures, built once in the engine/stdlib (as Rust's std/crossbeam do over
  `unsafe`). No mandatory mutex-everywhere model.
- **Recommended language surface.** `isolate`/`task`, an ownership-transferring channel (reusing
  `give`/`copy`), `Atomic<T>`, a structured-concurrency scope, and two targeted *safe* sharing primitives
  that recover what shared-nothing otherwise costs:
  - **immutable `Shared` read-across-isolates** — immutable data is race-free even when shared (cheap
    read-only sharing of big assets);
  - **scoped disjoint-slice parallel-for** — a scope lends each task a non-overlapping mutable slice of one
    buffer and reclaims it at join; safe by disjointness (the `rayon`/`split_at_mut` pattern).
- **Deferred — general shared-memory ("hybrid").** Co-equal shared-memory threading is *not* planned; it
  reintroduces the hazard the model removes. Capability is retained (via the seam + the two primitives);
  only some ergonomics move behind the seam. Reopen only if a concrete case the seam can't express appears.
- **Positioning.** A *different, simpler, more portable* safe-concurrency model. Honest trade: Rust's
  shared-memory-with-static-exclusivity is more flexible for max-perf shared mutation; kama's
  shared-nothing is far easier to reason about and portable to wasm. Prior art: **Dart isolates** (closest),
  **Erlang/Elixir** actors, **Web Workers** + SharedArrayBuffer, **structured concurrency**
  (Swift/Kotlin/Trio); **Pony** for the type-level ceiling.

## 7. 2.0 — dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the same kama syntax usable both compiled and as a scripting
language with a full REPL. The guiding constraint: **the `kama` binary is the only tool you need.**
External C toolchains stay *optional* — used for the portable-C release path, never required to write, run,
or iterate.

This rests on a **polymorphic emitter**: one front end lowered to a **shared IR**, then rendered by several
backends.

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

Every backend shares the same front end, so the safety analysis (ownership, move tracking, exhaustiveness)
is proven **once**, before the IR.

- **C backend — the portability moat (kept, always).** kama → readable portable C → any C toolchain.
  `clang`/`emcc` for release; a bundled **TinyCC** for near-instant in-process JIT (`kama run foo.kama`
  and the REPL are **JIT-compiled, not tree-walked**). "Runs anywhere C runs" is the whole moat; the new
  backends are *additive*, never a replacement.
- **WASM backend — the self-contained web path.** Direct kama → wasm (no `emcc`), run in the browser or
  under Wasmtime. The web scripting/engine substrate; C→emcc remains the maximal-compatibility option.
- **Bytecode + VM backend — the self-contained native REPL.** A kama-owned VM gives a true interactive
  REPL with zero external tooling.

**The IR is the crux, and the real work.** Today there is no IR: the C emitter writes C text directly and
*bakes in* monomorphization, RAII drop insertion, vtable layout, and match/operator desugaring
(`kama.cemit.*`, ~150 methods). The refactor pulls that **semantic lowering up into the shared IR**,
leaving each backend a comparatively dumb renderer. Design constraints:

- **Keep the IR high-level and structured** (retain `if`/`while`/`for` and named locals), *not*
  SSA/basic-blocks — so the C backend can still emit the readable, `#line`-mapped C that is a headline
  feature.
- **Move the runtime into kama.** Collections/smart-pointers/`string` live as hand-tuned C in
  `kama_runtime.h` today (with a growing share already ported to kama library types); a wasm or VM
  backend can't `#include` it. Finishing the port so those flow through the shared IR and monomorphize into
  *any* backend makes multi-backend and the kama-stdlib/self-hosting goal the **same project**: do it once,
  all three backends inherit it.
- **Contain semantic drift.** A VM is a second execution semantics — the main risk. Having the C backend
  and the VM consume the *same lowered IR* reduces drift from "two languages" to "two renderers of one IR."
  Build the IR first; then a VM is a legitimate, low-drift option.

**Speed ladder** (fastest last): tree-walk < bytecode VM < TinyCC-JIT < AOT C→clang. If raw scripting speed
dominates, JIT wins; if zero-install + interactivity dominate, the VM / direct-wasm win. The "binary is the
only tool" constraint tilts the *default* iteration toward the VM + direct-wasm, with C/JIT for native speed
or C compatibility.

- **Why it beats other scripting languages:** Python/Ruby/Lua are bytecode interpreters; kama scales from a
  self-contained VM up to JIT/AOT-native — the same source, at or near native speed.
- *Licensing note:* TinyCC is LGPL; if a bundled JIT ships, confirm the linking terms against the
  MIT/permissive goal (GOALS #8). The VM / direct-wasm paths sidestep this entirely.

## 8. Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked) →
buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for assets,
serialization for scenes). See [ENGINE_READINESS.md](ENGINE_READINESS.md).

## 9. Performance

Current standing (full detail in [benchmarks/RESULTS.md](benchmarks/RESULTS.md)): kama is at **C/C++
parity** on native compute (fib/pi/collatz/fnptr/alloc **and** dynamic dispatch — all LLVM-AOT languages
compiled at `-O3`), and wins decisively on footprint (~2 MB RSS, ~66 KB binary) and the no-GC `alloc`
workload. `kama→wasm` (optimized) **beats hand-written JS on fib/pi/collatz/fnptr (up to ~4.5×)** and is
near-parity on `alloc`/`dispatch`.

- **WASM tiering.** Measure at the optimizing tier (`node --no-liftoff` — what a real long-running app
  gets); the bench forces TurboFan for the wasm track so numbers reflect steady-state, not V8's short-lived
  Liftoff baseline.
- **Bench methodology (don't re-chase).** Short workloads skew under parallel load — run with nothing else
  competing. The `/work` bind mount adds only ~0.3–1.7 ms (negligible). Keep all LLVM-AOT languages at the
  same `-O` level (`-O3`), or the optimization level, not the language, dominates a tiny kernel.

## 10. Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to releases; Marketplace publishing is
  deferred.
- **Brand rename → Kama** — done: the mechanical rename (binary, `.kama` file extension, internal symbols,
  docs) landed in one commit. Remaining external steps: rename the GitHub repo to `cosmic-canopy/kama` so the
  flipped URLs resolve, and stand up `kama-lang.org`.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
