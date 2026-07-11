# kama roadmap

The forward plan — near-term to long-term, read in sequence. The language's **history** lives in the git
log; what the language **is** lives in [SPEC.md](SPEC.md). This file is only *what's next*.

## The shape

- **1.0 — language complete.** Strings, the **std I/O** foundation (`std::io`/`fs`/`net`), and the **math
  layer** (`std::math` — Vec/Mat/Quat) are **shipped**; the remaining gate is a small set of
  language-completeness residuals + the docs-reconcile/naming pass, after which the language surface is
  stable: you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection + serialization, a
  shared-lib/`expose` build, an embedded/MCU target, and deeper stdlib reach (extending the shipped I/O +
  math). Mostly library + codegen, little new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via a shared
  IR feeding C, direct-wasm, and a bytecode VM — the `kama` binary self-contained.
- **Concurrency — shared-nothing by construction** (1.x/2.0 direction): data-race freedom by removing
  shared mutable state, not a borrow checker. 1.0 ships a single-threaded core.
- **Engine track** (product north star): a portable lightweight **WebGPU** game engine, woven through
  1.x. Its Tier-0 math types are unblocked now.

## 1. Remaining before 1.0

Strings, the std I/O foundation (`std::io`/`fs`/`net` + the `examples/httpd` proof), and the math layer
(`std::math` — Vec/Mat/Quat) are **shipped**. What the language *is* lives in [SPEC.md](SPEC.md); the
engine capability matrix in [ENGINE_READINESS.md](ENGINE_READINESS.md); the history in the git log. What
remains to call the language **complete**:

1. **Language-completeness residuals (1.0 blockers — deep emitter work).** The fundamental (non-library)
   gaps still open, all in the move-tracking / ownership-lowering core; each reproduces with plain
   resources/collections and has a clean workaround, so a "language-complete" 1.0 closes them but they don't
   block the stdlib/engine work. (Reserved later-track keywords `expose`/`volatile` stay deferred — they
   hard-error, never miscompile.)
   - **General destroy-temporaries pass.** Owned rvalues the compiler doesn't specifically drop.
     Operator/receiver/`if`-cond/`foreach`/string-`ref` **and now `while`/`for` condition temps** all drop
     (the loop-and-a-half gained a per-iteration drop slot — `dropCondTemps`; the `_loopCond` gate is gone).
     Only residual: a non-string-intrinsic **by-value arg** whose owned rvalue the callee doesn't consume —
     today masked (by-value is a shallow struct copy the owning callee frees once), so latent rather than a
     live leak; fold into a general end-of-full-expression drop pass if it ever surfaces.
   - **Target-typed rvalue in a non-local-init position** *(ergonomic; surfaced building the containers)*. An
     inline construction that needs its type from context works in a `Type x = …` initializer, a `return`, an
     `operator[]` place-store, and a value-producing `match` arm, but not yet in every position. Still open: a
     **bare generic ctor into a field** (`this.m = Map()` — infer the field's type args; bind to a typed local
     first); an **inline variant construction or value-producing `match` in a call-argument position**
     (`f(o: Optional::Some(…))` / `f(x: match(…))` — "scope-qualified call resolves to no known function"; bind
     to a local first); and **indexing a `string`/rvalue receiver** (`"abc"[0]` — a string *literal* method
     call already materializes its receiver, but indexing it doesn't; bind to a local first). The fix is to
     propagate the target type / materialize the receiver in these positions like the initializer + return +
     place-store + arm paths already do.
   - ~~**Remaining primitive `Hashable`/`Equatable` widths.**~~ DONE. All integer widths
     (`int8/16/32/64`, `uint8/16/32/64`) now have prelude `Hashable` (splitmix64) + `Equatable` (scalar),
     so every integer is a universal `Map`/`Set` key; floats (`float32/64`) get `Equatable` (exact `==`) but
     intentionally NOT `Hashable` (float hash keys are a footgun — NaN/±0.0 — and there is no bit-reinterpret
     cast). Conformances are `static inline`, so unused widths cost nothing in generated C.
2. **Standard-library follow-ups (tracked; mostly post-1.0, no new language surface).** The shipped I/O +
   math subset is sufficient for 1.0; these extend the modules as pure library/codegen work:
   - **`std::net`** — UDP, DNS/`getaddrinfo`, ephemeral-port `getsockname`.
   - **`std::fs` / `std::io`** — buffered readers, richer `Metadata` (mtime/perms), path helpers, `mkdir`.
   - **`std::math`** — a **SIMD backend** (portable C vector extensions, `vector_size(16)` → SSE2/NEON/
     wasm128) as a pure implementation swap behind the unchanged, SIMD-ready-layout API (1.x / engine era);
     and a concrete `f64` (`DVec`) family alongside the `float32` one. Rotors deferred.
   - **Windows CI** — the `windows-latest` leg now passes the full suite (the `kama_os.h` `_WIN32` branch is
     verified); promote the leg from best-effort to **required** so a Windows regression blocks a merge.
3. **Docs reconcile → tag 1.0.** 1.0 is the API-stability point; naming/case conventions are fixed here
   (PascalCase types, lowerCamel methods, no `I`-prefix on contracts, lowercase `string`).

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The
**fundamental** gaps that a "language complete" 1.0 must close (the general temp-drop pass, remaining owning
collection-element cases, `contract`-refining-`contract`, multibyte char literals) are §1's residuals. What
remains here is genuinely later-track or opt-in.

- **Unicode module (post-1.0).** The shipped `string` core is UTF-8 bytes + `.chars()` codepoints with
  **ASCII** casing/whitespace; a later module adds Unicode-correct casing + whitespace, and an eager
  `List<string>` collect for `split` (the lazy `Split` iterator ships today).
- **String interpolation `"${x}"` + formatting** — needs a general to-string / `Display`-like mechanism
  (also covers `string + <number>`); sequences with reflection (its to-string substrate).
- **`expose` keyword** — reserved, hard-errors today (the kama→host boundary; distinct from `export`, the
  module public-surface manifest, which ships). **Split by scope:** a **minimal `expose`** (C-ABI
  linkage for `--shared` reload entry points) is **pulled forward to 1.x** (§5, engine dev-loop); the
  **full `expose`** (wasm module exports + the scripting host interface) stays **2.0** (§7). The keyword
  slot is reserved at 1.0 either way, so activating it in 1.x follows the same reserved-then-lit pattern
  as `volatile`.
- **`volatile` keyword** — reserved → **1.x embedded** (emit C `volatile` for ISR↔loop flags / MMIO).
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
  stdlib grows. (`std::math` / `std::io` already ship as directory modules under this mechanism — the open
  question is whether pruning scales, not whether the packaging shape works.)
- **Structural → nominal contracts for bounds.** `foreach` is now nominal (an iterator must `implements
  Iterator`/`IteratorMut`, a container `Iterable`/`IterableMut`), but a generic bound `<T: Weighable>`
  still accepts a type **structurally**. Decision (user): go fully nominal — require `implements` for
  bounds too, so the keyword is load-bearing everywhere and errors pin to the declaration. A migration
  pass over the bound fixtures.
- **`Copyable` as a formal contract.** Today it's recognized nominally by name (`implements Copyable`);
  formalize as `type contract Copyable { This copy(); }` — bundle with the structural→nominal migration.

## 4. Reflection + attributes / serialization (1.x — Phase 4)

Opt-in compile-time reflection driving polymorphic serialization — the final self-hosting-stdlib step. The
only new *language* surface is the attribute mark; serializers land as modules. **The shipped Phase 4a surface
— `@generate`/`@field`/`@skip`, the `Serialize`/`Deserialize`/`DeError` prelude contracts, bypass-ctor
deserialize, `onConstruction`, and the `std::serialization::json` backend (JSON round-trip) — is documented in
[SPEC.md](SPEC.md) "Serialization".** What remains:

- **Deserialize breadth (tail)** — `Array<E>` read, and a `const` field (write-once in the ctor, so the
  in-place field-set can't set it — a `@field const` doesn't yet parse; give it a clear diagnostic).
- **General user `enum` serialize** — accept `@generate` on `enum` declarations + variant codegen.
- **Graph serialization (scene graphs).** One blessed `@generate` mode: by-value fields serialize inline (as
  today), **pointer fields (`Owned`/`Shared`/`Weak`) serialize as ids** into a side table, with a two-pass id
  fixup on read and `onConstruction` deferred to graph-complete (the `awakeFromNib` / `IDeserializationCallback`
  point); a dangling required id → `DeError::UnresolvedReference` (the .NET `ObjectManager` analog). Builds on
  the shipped never-null `Owned`/`Shared` invariant (definite-assignment in ctors — the fixup lands pointers
  transiently null, then re-establishes non-null before `onConstruction`). **Next:** `SerContext`/`DeContext` +
  a `SerializedReference` registry (Shared→retain, Weak→downgrade, Owned→transfer-once); JSON inline + id-table.
- **More back ends (modules, no compiler change)** — YAML; **binary** (packing options + `@bits(n)` bit-packing
  + little-endian canonical); **XML** + **HTML** (XML → `<field>value</field>`, HTML a render/pretty write
  view). Each is a `Serializer`/`Deserializer` impl + `toString`/`tryParse`-style entries.
- **Rename `toString`** — too generic / clashes with other-language conventions; proposed `json::encode`
  (paired with `json::tryParse`), name to confirm.
- **`@deprecated` attribute (language, adjacent)** — a declaration marker (rides the existing `@`-attribute
  infra) that emits a use-site warning, optionally with a message/replacement hint. Its own small task.
- **Optional/default parameters (language, adjacent)** — enables the "options struct with optionals" ctor
  pattern (no default params today), an alternative to constructor overloading.
- **Docs** — `docs/grammar.bnf` (attribute grammar incl. the `noOnConstruction` flag).

**String interpolation `"${x}"` rides on the same `std::fmt` to-string substrate**, so it sequences here.

## 5. 1.x — systems & runtime (post-1.0)

Capabilities built on the finished language — the substrate the engine needs (asset I/O, scene
serialization, networking).

- **Shared-lib build + minimal `expose` (pulled forward from 2.0 — engine-unblocking).** `kama build
  --shared` → `.so`/`.dylib`/`.dll` and a **minimal `expose`** (C-ABI linkage for entry points) — the two
  small compiler primitives under the engine's desktop **dev-loop hot-reload** (§8). Both are independent
  of the 2.0 IR refactor, so they land here to make engine iteration fast *early* rather than waiting on
  the VM. The reload loop itself is a library (`dlopen`/watch/rebind over `unsafe`/`Ptr`), not roadmap
  work. Deliberately excludes the full 2.0 `expose` (wasm module exports + scripting host, §7).
- **Reflection + declarative serialization** — see the brief above; back ends follow as modules. Rides on
  the shipped `std::fs`/`std::io` for asset + scene load.
- **Container / data-structure reach.** Now **shipped**: `List`/`Array`/`string`/`Fixed`, plus **`Map<K,V>`**
  (open-addressing/tombstoned, `K: Hashable + Equatable`, owning keys+values, ASan-clean; deep `copy` +
  key iteration) and **`Set<K>`** (= `Map<K, Unit>`); `List`/`Array` gained
  `reserve`/`remove`/`clear`/`contains`/`indexOf`. The gating uses **multi-condition `when [K: Copyable,
  V: Copyable]`** (`Map.copy` needs both). **Tracked:** entry-wise iteration `foreach (Entry e in
  m.entries())` — a generic `Entry<K,V>` value yielded through `Optional` doesn't monomorphize yet. Still
  ahead:
  **slice/span `View<T>`** (a non-owning subrange view — the highest-value next; hand a buffer to a system or
  a GPU upload with no copy and no ownership transfer), **priority queue / binary heap** (A* pathfinding,
  event/timer scheduling), **deque / ring buffer** (job & event queues, audio), **slot map / generational
  arena** (stable handles with generation counters — *the* ECS/asset-registry structure, catches
  use-after-free), and a **sorted / tree map** (ordered iteration + range queries; needs
  `Comparable`/`Ordering`). Honest caveat: general **linked lists** are mostly a cache anti-pattern in
  data-oriented engines (the useful form is an intrusive free-list / LRU); raw **BSTs** are subsumed by the
  sorted map; **spatial trees** (quadtree/octree/BVH/k-d) are engine-specific, not stdlib.
- **Custom allocators.** The collections hardcode `malloc`/`realloc`/`calloc`/`free`. Thread an **allocator**
  parameter through the generic containers (arena/pool/stack/bump allocators for hot loops; a *fallible*
  allocator for the no-heap embedded target). Bigger surface than it looks — the allocator has to reach
  element construction/relocation and RAII drop — so it rides with the embedded target rather than 1.0.
  `Map<K, V, A>` / `List<T, A>` with an allocator default is the likely shape (a default keeps today's API).
- **Browser networking transports** — native TCP ships (`std::net`); the browser has no raw sockets, so the
  wasm path needs **WebRTC DataChannels** (unreliable) / **WebSockets** (reliable) via a host FFI shim (a
  real wasm nuance). Native UDP/DNS and the rest of the stdlib reach are the §1 follow-ups.
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

**One language, two modes** — the *same static kama* (identical syntax, semantics, ownership rules; dynamic
only in *execution*, never in typing) usable both compiled and as a scripting language with a full REPL. The
target is a REPL that **replaces the Python/Ruby/Lua REPL** for fast iteration and compile-→-run-on-demand,
at or near native speed. Guiding constraint: **the `kama` binary is the only tool you need** — external C
toolchains stay *optional* (the portable-C release path), never required to write, run, or iterate.

**Two tiers, chosen by what dominates:**

| Tier | Path | Optimized for |
|---|---|---|
| **Release / AOT** | `kama → C → clang`/`emcc` | maximum runtime speed, the portability moat |
| **Iteration / scripting / REPL** | direct-wasm (+ `wasm-opt`), then a bytecode VM | compile speed, zero external toolchain, interactivity |

The release tier ships today and is untouched; the new work is the *iteration* tier, and it is **additive** —
never a replacement for C.

```
   Frontend  (parser → type checker → ownership/move analysis)   ── safety proven ONCE
                          │
                   semantic lowering   (monomorphize, insert drops,
                          │             desugar vtables / match / operators)
          ┌───────────────┼────────────────────┐
          ▼               ▼                      ▼
    C (clang/emcc)   direct WASM            Bytecode VM
    RELEASE — max    + wasm-opt             REPL, self-contained
    speed, moat      ITERATION / web        (IR extracted here, if ever)
```

Every backend shares the same front end, so the safety analysis (ownership, move tracking, exhaustiveness)
is proven **once**, before lowering.

**Sequencing — polymorphic emitter first, a shared IR only when the VM forces it.** The emitter
(`kama.cemit.*`, ~150 methods) doesn't merely translate syntax — it *bakes in* the semantic lowering
(monomorphization, RAII drop insertion, vtable layout, match/operator desugaring). A "shared IR" is just
that lowering **factored out** into a data structure the backends consume — so *polymorphic emitter* and
*shared IR* are the same idea at two points on a spectrum, not opposed choices. The pragmatic path:

1. **Refactor the emitter to an abstract interface**, C as the first implementation, the shared lowering in
   the base. Low risk.
2. **Add a direct-wasm backend** as a sibling — same lowering, different rendering. Drops the `emcc`
   dependency for self-contained web/scripting builds and proves the seam. (C→emcc still produces the
   maximal-compatibility release wasm.)
3. **Extract an explicit IR only when the VM needs it** — a bytecode VM is a genuinely different execution
   model (stack/register machine), so re-deriving the lowering a *third* time is where a shared lowered form
   actually pays off. Build the IR then; the VM becomes a low-drift renderer of the *same* lowered form the
   C backend uses (containing the "second execution semantics" drift risk).

**Design constraints (hold across the whole spectrum):**
- **Keep the lowered form high-level and structured** (retain `if`/`while`/`for` and named locals), *not*
  SSA/basic-blocks — so the C backend still emits the readable, `#line`-mapped C that is a headline feature.
- **Move the runtime into kama.** Collections/smart-pointers/`string` live as hand-tuned C in
  `kama_runtime.h` (a growing share already ported to kama library types); a wasm or VM backend can't
  `#include` it. Finishing the port makes multi-backend and the kama-stdlib/self-hosting goal the **same
  project** — do it once, all backends inherit it.

**Direct-wasm optimization — lean on Binaryen, don't write an optimizer.** A naive direct `kama → wasm`
backend emits ~`-O0`/`-O1`-quality code (no inlining, redundant locals). The fix is **`wasm-opt`**
(Binaryen), a standalone optimizer that runs on *any* wasm regardless of producer — the AssemblyScript
model. `kama → wasm → wasm-opt -O3` recovers most of the gap (inlining, DCE, local coalescing, precompute).
It is **not** equal to `C→emcc -O3`: LLVM's mid-level IR optimizer (alias analysis, loop transforms) and its
SIMD **autovectorization** stay ahead — so heavy numeric loops still favor the release tier, while typical
logic/scripting is near-parity. The honest trade: direct-wasm buys **compile speed + zero dependency + a
REPL** (which emcc structurally cannot give), at **near-native**, not emcc-`-O3`, runtime.

**Speed ladder** (fastest last): tree-walk < bytecode VM < direct-wasm/`wasm-opt` < AOT C→clang. The "binary
is the only tool" constraint tilts the *default* iteration toward the VM + direct-wasm; C/emcc is the release
path.

- **Why it beats other scripting languages:** Python/Ruby/Lua are bytecode interpreters; kama scales from a
  self-contained VM up to AOT-native — the *same source*, at or near native speed.
- **Licensing.** **Binaryen (`wasm-opt`) is Apache-2.0** — clean against the MIT/permissive goal (GOALS #8),
  so the direct-wasm path is unencumbered. A bundled **TinyCC**-JIT (a near-instant native `kama run`) is a
  possible *optional* alternative but is **LGPL** — confirm the linking terms before shipping it; the VM /
  direct-wasm paths sidestep it entirely.

## 8. Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked) →
buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for assets,
serialization for scenes). See [ENGINE_READINESS.md](ENGINE_READINESS.md).

- **Dev-loop hot-reload — a *library* on two small compiler primitives.** Live-reload of gameplay code
  (edit → rebuild → swap without restarting) splits cleanly by layer, and *most of it is not the
  compiler's job* — which answers "language or engine feature?": mostly library, on a thin compiler base.
  - **Compiler (small — scheduled 1.x, §5):** a `kama build --shared` mode emitting a
    `.so`/`.dylib`/`.dll` (`-fPIC -shared`; on Windows the `dllexport` decoration + copy-before-load), and
    reuse of the reserved **`expose`** keyword (§2) to give reload entry points **C-ABI linkage**. That is
    the *same* kama→host boundary the **wasm exports** and the **scripting host** (§7) already need — so
    hot-reload adds **no new language surface**, it consumes planned surface. One boundary, three consumers.
  - **Library:** the `dlopen`/`dlsym`/`dlclose` + file-watch + function-pointer rebind loop — pure FFI over
    `unsafe`/`Ptr`, **zero compiler changes**. This is the bulk of the feature and it lives in a module.
  - **Engine:** the *data-in-host, code-in-module* architecture (world state lives in the platform-layer
    arena, passed *into* the reloaded module) so a reload doesn't wipe the world. Prior art: Handmade Hero,
    Our Machinery, Unreal Live Coding (Live++), Godot GDExtension, Bevy `hot_lib_reloader`.
  - **Scope — desktop dev only.** dlopen is absent/forbidden on the *ship* targets: no `dlopen` in wasm
    (host re-instantiates a module instead), **banned on iOS** (no loading non-bundled native code, no
    JIT), Android/Quest only via a **pushed** `.so` (no on-device compile). Cross-platform *shipping*
    scripting is the §7 **VM**, not this. This path buys fast native iteration on Linux/Mac/Windows —
    nothing more, and that is enough to justify the two tiny primitives.

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
- **Bench cohort — add Zig.** The bench covers the no-GC AOT peers (C/C++/Rust/Go) but not **Zig** —
  kama's closest *language* rival (no-GC, AOT, and, as `zig cc`, already kama's bundled backend). Add a
  `zig` track: port the 4 workloads to `.zig`, add the toolchain to `bench/Dockerfile` + `build.sh` (or
  reuse the pinned zig from the release pipeline). Expect it to **cluster with C/Rust on raw compute**
  (all LLVM at `-O3`) — the signal is in the *compile-time / binary-size / RSS* columns and cohort
  completeness, not the perf ranking. Low-value on the perf axis; worth it for "kama vs its actual peers"
  being visibly complete.

## 10. Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to releases; Marketplace publishing is
  deferred.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job (Windows is now proven; FreeBSD is the next
  platform to cover).
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
- **Package manager (ecosystem foundation).** A first-class dependency manager + registry so libraries
  distribute without vendoring — the point at which the **orphan rule** (§3, retroactive conformance) stops
  being a nicety and becomes load-bearing (separately-compiled packages can no longer be globally
  dedup-checked at once). Gates a real third-party ecosystem.
- **Longer-term — a "node.js-class" application framework in kama.** A fast, low-overhead server/app
  framework (HTTP already dogfooded via `examples/httpd`), aiming to beat the Node/Deno overhead profile on
  the no-GC/AOT (or VM-scripted) runtime — the flagship *application* of the language + package manager +
  scripting tiers together. Aspirational, post-ecosystem.
