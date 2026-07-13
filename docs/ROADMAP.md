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
   - ~~**General destroy-temporaries pass.**~~ DONE. Owned rvalues the compiler drops: operator /
     string-receiver / `if`-cond / `foreach` / string-`ref`, `while`/`for` **condition** temps (per-iteration
     `dropCondTemps`), string-**index** rvalue receivers, an owned rvalue method-call **receiver** — including
     through a **method chain** (`builder.make().use()`), unified on `isPlaceReturn` so a `fn ref T` place
     return (a borrow) is never dropped (this also closed a latent double-free introduced when free functions
     gained `ref T` returns) — and an owned rvalue receiver of **`.chars()`/`.split()`** (materialized once
     into a loop-scoped temp, dropped after the loop). The by-value **owned arg** is a move into the callee's
     owning slot (freed once). ASan/UBSan-clean across the suite.
   - **Target-typed rvalue in a non-local-init position** *(ergonomic; surfaced building the containers)*. An
     inline construction that needs its type from context now works in a `Type x = …` initializer, a `return`,
     an `operator[]` place-store, a value-producing `match` arm, a **class-typed lvalue store** (`this.m =
     Map()`, done), and a **call-argument** (`f(o: Optional::Some(…))` / `f(x: match(…))`, done — the call path
     threads the param type), and **indexing a string rvalue receiver** (`"abc"[0]` / `s.concat(x)[0]`, done —
     the receiver is materialized), and an inline **`new` in a non-local by-value position** — a call-argument
     (`f(a: new Sq(…))`), a `Shared/Owned<T>` **return** (`fn Shared<Shape> f() { return new Sq(…) }`, over a
     concrete OR contract element), and a **variant payload** (`Optional::Some(value: new Sq(…))`), all DONE.
     `tryHoistInlineNew` now mirrors the (ASan-clean) local-init boxing — the library-`adopt` path for a
     concrete element and the fat-box `{obj,vtbl[,ctrl]}` path for a contract element — into a hoisted temp
     whose ownership transfers to the consumer (callee param / `__ret`), so no new lifetime analysis. (The
     memory's earlier "`ParamSig.className` drops the element type" / "under-developed contract dispatch"
     blockers were mis-diagnoses: the real `Owned`/`Shared` are library-monomorphized, so the param carries the
     full instance type and named-local contract dispatch already works.) Also DONE: an inline **stack ctor
     into a by-value contract parameter** (`useShape(a: Square(3))` — materialized + fat-pointer borrow,
     caller-dropped). **Still open** (one residual, clean "bind to a local first" workaround): a
     value-producing construct as a `match` **SUBJECT** (`match (Optional::Some(x)) { … }`). The instance must
     be inferred from the payload during the *discovery* pass (so its struct is emitted), but that pass has no
     local-variable types — only a literal payload could infer, which isn't worth a partial feature; the full
     fix needs discovery-time local typing or lazy generic-struct emission. Documented RULES (not gaps): an
     inline `new`/value **borrowed** by a `ref`/`out` or contract parameter (an rvalue has no lvalue to
     reseat), and an inline construct in a `do/while` condition (ISO-C + `continue` semantics).
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
**fundamental** ownership/completeness gaps are closed (the general temp-drop pass — §1; `contract`-refining-
`contract` and multibyte char literals — both shipped); the one remaining §1 residual is the value-producing
`match` **subject** inference. What remains here is genuinely later-track or opt-in.

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
- ~~**Structural → nominal contracts for bounds.**~~ DONE. A generic bound `<T: Weighable>` is now
  **nominal** — the type must declare `implements Weighable` (`classSatisfiesBound` matches the interfaces
  list / generic-contract template, not coincidental method names), so the keyword is load-bearing
  everywhere (`foreach`/`when`/bounds) and errors pin to the declaration. Migration was zero — every bound
  fixture already declared `implements`; xfail `bound_nominal` guards the new requirement.
- ~~**`Copyable` as a formal contract.**~~ DONE (was already most of the way there). `Copyable` is the
  prelude `type contract Copyable for resource { fn This copy(); }`; a type is Copyable only by declaring
  `implements Copyable` (the `copyable` flag, set nominally) — a `value` stays bitwise-copyable. The
  nominal bound check routes `<T: Copyable>` through that same value/resource rule.

## 4. Reflection + serialization — intrinsic pivot (1.x — Phase 4)

**Design of record: [SPEC.md](SPEC.md) "Serialization" (the guiding light).** The user-facing surface
(`@generate`/`@field`/`@skip`, the `Serialize`/`Deserialize` contracts, `encode`/`decode`, hand-written
override) is locked. The **implementation is being rebuilt as a compiler intrinsic** — a lowering to C — and
this replaces the earlier "synthesize the field walk + graph driver **as kama**" approach.

**Why the pivot.** Emitting the structural machinery as kama *source* forced it to obey surface-language safety
it was never meant to (no borrowed contracts in a `List`, no contract-method dispatch on a raw `Ptr`, no
generic-instance statics, no move-out-of-`Optional`, parent-first tables, nested-`Owned` ordering) — every fix
was a hack poking `__`-holes in the smart pointers. Reflection + the ownership-graph rebuild are **core language
guarantees**; the compiler already owns type layout, the RAII model, and the triad's internals, so it emits
them directly (generalizing the `HeapShellNode` instinct to the whole layer) — correct-by-construction, and
nothing leaks. Wire formats stay library (`Serializer`/`Deserializer` contracts, swappable). Mode is gated on a
precomputed `reachesPointer(T)` flag (sibling of `destructible`): pointer-free ⇒ by-value/stack; reaches a
pointer ⇒ heap graph (`Shared<T>`).

**Phases** (each: build → `tools/cdev test` → `KAMA_SAN=1` → `KAMA_WASM=1`; own commit):

- ~~**A — Foundations.**~~ **DONE.** `reachesPointer` (memoized, beside `destructible`; inert until the
  Phase-C mode gate consumes it); prelude-ize the `Owned`/`Shared`/`Weak` triad — now **built-in kama**
  embedded into the binary (new `prelude/` dir + `tools/embed_prelude.sh` → `KAMA_PRELUDE_SRC`/
  `KAMA_PRELUDE_MODULES`), so it's always in scope with no `import std::memory` (a redundant import stays a
  no-op via an implicit `using std::memory`) **and survives a `--no-std` install** (which strips `lib/kama`).
  The whole prelude moved out of the `PRELUDE_SRC` C literal into `prelude/global.kama`. The WIP generated-kama
  graph hacks were already reverted; the committed 3a `Shared`/`Weak` graph machinery + `__`-seams stay until D.
- ~~**B — Runtime graph-context (C).**~~ **DONE.** Pure-C object-graph substrate in `kama_runtime.h`
  (`kama_gmap` open-addressing `uint64→uint64`; `kama_ser_graph` write context = addr→id dedup + worklist +
  `reserve`/`intern`; `kama_de_graph` read context = id→object `register`/`lookup`). No `std::collections`
  dependency. Inert until the C/D lowering drives it; the `std::serialization::graph` kama module it supersedes
  is deleted in D. It ports the generated-kama `SerContext`; a single id→object registry replaces the old
  per-type parallel-`List` lookups.
- ~~**C — By-value lowering (intrinsic).**~~ **DONE (structs).** A `@generate` product (value/tree) struct's
  `serialize`/`deserialize` are now emitted directly in C (`CEmitter::emitSerializeDefinition`/
  `emitDeserializeDefinition`), replacing the generated-kama `__KamaGenSer`/`__KamaGenDe`. The emitter
  registers the `Serialize`/`Deserialize` conformance itself (interfaces + a synthesized `MethodInfo`,
  `isSynthSer`/`isSynthDe`) so `encode`'s dynamic dispatch + `decode`'s static `T::deserialize` resolve; the
  body walks fields (scalars/string → `Serializer`/`Deserializer` directly; nested struct / enum / collection →
  its own `<T>__serialize`/`__deserialize`; `Optional` inlined). Gated on the type having no `serialize`
  method, so hand-written **and** driver-graph impls win. API- and wire-preserving — all `ser_*` + xfails
  green on test/ASan/wasm. **ENUM follow-up DONE too:** `@generate` enum serde (externally-tagged
  `{"tag":…[,"value":{…}]}`) now emits in C (`emitEnumSerializeDefinition`/`emitEnumDeserializeDefinition`),
  registered in `collectEnums` (conformance is nominal/`retroInterfaces` — an enum can't carry a fat-pointer
  method — bodies emitted alongside the variant dtor since `classOf` skips enums). The driver now synthesizes
  **only the graph path**; dead `buildDeserializeBody`/`injectZeroInitForDeserialize`/`hasOnConstruction`/
  `buildEnumSerialize`/`buildEnumDeserialize`/`enumDirections` removed. (`Fixed<T,N>` `@generate` fields are
  unused today — deferred.)
- ~~**D — Graph lowering (intrinsic).**~~ **DONE.** The whole object-graph write/read is now emitted directly
  in C over the pure-C `kama_ser_graph`/`kama_de_graph` context (`kama_runtime.h`), replacing the generated-kama
  `std::serialization::graph` module + the driver's `synthesizeSerialization` (both deleted). A graph node
  (root, via `reachesPointer`, OR a Shared/Weak/Owned pointee, via `computeGraphNodeTypes`' closure) emits three
  helpers — `T__serializeNode` (one id-table entry; pointer fields intern the pointee via a per-node writer
  fn-ptr, in discovery == id order so the wire is byte-identical), `T__allocShell` (pass 1: zeroed heap shell +
  scalar read), `T__wireShell` (pass 2: resolve ids → construct `Shared`/`Weak`/`Owned` over `(ptr, ctrl)`).
  The public `serialize` is the `{root,objects}` envelope (gated on `reachesPointer`); the public `deserialize`
  is the two-pass driver returning `Shared<T>` (gated on `graphDeserialize` — a node whose `Shared<T>` exists).
  `Shared`/`Weak` dedup by pointee address; cycles ride the `Weak` back-edge; a dangling id → `UnresolvedReference`;
  `Owned` is give-once via a claim gmap (2nd claim → new `DuplicateId`). Refcounts balance under ASan (each
  shell's construction-strong is dropped at driver end). The triad's `__ptrId` seam + the `HeapShellNode` AST
  node are deleted; the shells hand-construct over the prelude `Shared/Weak/Owned` C fields (`p`/`c`), needing no
  `Shared<T>` instantiation for an `Owned`-only pointee. Notably `reachesPointer`'s seed was dead since the
  Phase-A prelude pivot (concrete-element triad instances are library generics, not `isIntrinsicColl`) — now
  fixed to detect them by template key. `ser_graph_shared`/`_cycle`/`_dangling` stay byte-identical +
  `ser_graph_owned`/`_dup_owned` added; all green on test/ASan/wasm.
- **E — Polymorphic `Shared<Contract>`.** `__type`→vtbl table + guarded rebuild (`TypeMismatch`). `ser_graph_poly`.
- **F — Cleanup + docs.** Remove dead machinery; finalize SPEC/this section; `docs/grammar.bnf` (attribute
  grammar incl. `noOnConstruction`). Record that the generic-instance-static-call gap no longer blocks (the
  intrinsic routes around it natively).

- **Deserialize breadth (tail, folds into C/D)** — `Array<E>`/`Fixed<T,N>` read; a bare `encode`/`decode` of an
  intrinsic/enum value; generic enums. (A `const` field is a separate general language gap — doesn't parse today.)
- **More back ends (library, no compiler change)** — YAML; **binary** (packing + `@bits(n)` + little-endian
  canonical); **XML**/**HTML**. Each is a `Serializer`/`Deserializer` impl + `encode`/`decode`. `std::encoding::base64`
  is a separate small module.
- **`@deprecated` attribute (language, adjacent)** — a declaration marker (rides the `@`-attribute infra)
  emitting a use-site warning. Its own small task.
- **Optional/default parameters (language, adjacent)** — the "options struct with optionals" ctor pattern.

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

- **Compiler build-warning cleanup (hygiene).** The host compiler build (`tools/cdev make`) emits a handful of
  warnings; drive them to zero and consider a `-Werror` CI gate so new ones can't creep in. Current set:
  hand-written **`transpileToFile`** + **`typeToStr`** (kama.driver.cpp) and **`isValidChar`** (kama.l) are
  unused — delete or wire up; **`yynerrs`** is bison-generated (`build/kama.parser.cpp`), so suppress it on the
  generated TU (a per-file `-Wno-unused-but-set-variable`, or a bison `%define` that consumes it) rather than
  editing generated code. (Emitted-C `-Wparentheses` notes are in generated output, separate.)
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
