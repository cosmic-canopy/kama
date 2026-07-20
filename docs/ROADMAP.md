# kama roadmap

The forward plan — near-term to long-term, read in sequence. The language's **history** lives in the git
log; what the language **is** lives in [SPEC.md](SPEC.md). This file is only *what's next*.

## The shape

- **1.0 — language complete.** The core language, the std I/O foundation (`std::io`/`fs`/`net`), and the
  math layer (`std::math`) are in place (see [SPEC.md](SPEC.md)); the remaining gate is a small set of
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

What the language *is* lives in [SPEC.md](SPEC.md); the engine capability matrix in
[ENGINE_READINESS.md](ENGINE_READINESS.md); the history in the git log. What remains to call the language
**complete**:

1. **Language-completeness residual (1.0 blocker — deep emitter work).** One fundamental (non-library)
   gap in the move-tracking / ownership-lowering core; it reproduces with plain resources/collections and
   has a clean workaround, so a "language-complete" 1.0 closes it but it doesn't block the stdlib/engine
   work. (Reserved later-track keyword `volatile`/`hardware` stays deferred — it hard-errors, never
   miscompiles.)
   - **Value-producing `match`/ternary as a `match` subject** *(rare residual, clean "bind to a local first"
     workaround)*. Target-typed inline construction works in every by-value position (initializer, `return`,
     `operator[]` store, value-producing `match` arm, class-typed lvalue store, call-argument, variant payload,
     string-rvalue indexing, inline `new`) — see [SPEC.md](SPEC.md). A bare **variant-constructor** subject
     (`match (Optional::Some(x)) { … }`, x a param/local/literal) now works too: a function-level pre-scan
     supplies the discovery pass the local types, so the instance (`Optional<T>`) is inferred + registered
     there and reused at emit. **Still open** (much rarer): a *nested* value-producing `match` or a
     *variant-producing ternary* directly as a subject — bind to a typed local. Documented RULES (not gaps):
     an inline `new`/value **borrowed** by a `ref`/`out` or contract parameter (an rvalue has no lvalue to
     reseat), and an inline construct in a `do/while` condition (ISO-C + `continue` semantics).
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

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The only
§1 language-completeness residual left is a *nested* value-producing `match`/*variant-producing ternary*
directly as a `match` subject (the common bare-variant-ctor subject now works); what remains here is
genuinely later-track or opt-in.

- **Unicode module (post-1.0).** The shipped `string` core is UTF-8 bytes + `.chars()` codepoints with
  **ASCII** casing/whitespace; a later module adds Unicode-correct casing + whitespace, and an eager
  `DynamicArray<string>` collect for `split` (the lazy `Split` iterator ships today).
- **String interpolation `"${x}"` + formatting** — needs a general to-string / `Display`-like mechanism
  (also covers `string + <number>`); sequences with reflection (its to-string substrate).
- **Full `expose` (2.0).** The minimal `expose fn` free-function C-ABI boundary ships today (see
  [SPEC.md](SPEC.md) + §5); the **full `expose`** — richer wasm module exports + the scripting host
  interface — stays **2.0** (§7).
- **`hardware` keyword** (renamed from the reserved `volatile`, to shed the C threading-confusion legacy) —
  reserved → **1.x embedded** (§5 embedded milestone): emits C `volatile` for MMIO registers
  (`hardware Ptr<T>` → `volatile T*`, mirroring `const Ptr<T>`) and single-core ISR↔loop flags.
  **Explicitly NOT a concurrency primitive** — cross-thread sharing is §6 atomics. Reserved-then-lit like
  before; the token stays a hard error until the embedded target ships.
- **Opt-in `Equatable` derive (auto `==` for `value` types) — post-1.0 minor nicety.** Deferred into the
  construction-model campaign's broader derive story (`Equatable`/`Hashable`/`Copyable` as one consistent
  opt-in `@generate` surface, not three ad-hoc ones) — see `docs/design/construction-model.md` §8c. Kama today
  requires a hand-written `operator==` (auto structural `==` is a deliberate non-default); the derive would
  synthesize a memberwise `==` on request.
- **Enum-variant payload-type registration gap (bug, small).** A type used *only* as an enum variant's
  payload — where that variant is never constructed (the enum is exercised only via its other variants) —
  is not registered/emitted, so the enum's C `struct` references an undeclared type (`unknown type name
  'Shared_Probe'`). Reproduces with a plain `enum E { A, B(Shared<Probe>) }` constructed only via `A` —
  independent of Model C / `.as<>` (found alongside M5/P3). Fix: scan **every** variant's payload types at
  enum registration (like class fields via `scanTypeForCollections`), not lazily at construction.
- **Custom-allocator default-convenience enforcement (construction-model, pre-1.0 safety).** The M8c
  collections four-ctor matrix ships `empty()`/`withCapacity(n)` (default `GlobalAllocator`) as `ctor`s
  alongside `withAllocator`/`withCapacityAndAllocator` (custom `A`). Calling a *default*-allocator
  convenience on a *custom*-`A` type (`DynamicArray<T, BumpAllocator>.empty()`) currently compiles into an
  incomplete (zero-allocator) collection — low-reachability (contradictory intent) but a real hole in
  "nothing incomplete is constructible." The M8b completeness gate **cannot** reject it: the gate runs at
  ctor-definition emit and the compiler emits *every* ctor of a monomorph, so a custom-`A` monomorph built
  via `withAllocator` force-emits (and would falsely reject) its uncalled `empty()`/`withCapacity()`. The
  true fix is a **structural `when [A: default]` gate** so those two ctors simply *do not exist* for a
  non-default-fillable `A` (or lazy per-ctor monomorph emission). Must land before tag 1.0. Also re-earns
  the compile-time protection `Map/Set::withCapacity` had as a `static fn` (design doc §7 carve-out) before
  M8c unified them into `ctor`s.
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
- **Design spike — a safe wrapper for the raw-`Ptr` in/out dance (`Slot<T>` / `MaybeUninit`).** Container
  authors move owned values across the safe↔unsafe boundary by hand (see the `give`-into-a-raw-slot rule in
  [SPEC.md](SPEC.md) `unsafe { }`): `give` bridges a tracked value *INTO* a raw slot (source consumed —
  the one marker that reaches into `unsafe`), but there is **no symmetric way OUT** — you can't `give`
  out of a raw element (`moveOnlySource` rejects it), so reading back is a manual "zero-init a local, bitwise
  copy, take responsibility" dance (see `Deque.takeAt`, and the round-trip in `tests/give_ptr_local.kama`).
  This asymmetry is the sharp, easy-to-misuse part of the raw layer — deliberately gated behind `unsafe` and
  confined to a few stdlib containers (the Rust-`Vec`-internals bet), but a candidate for a small safe
  abstraction: a typed `Slot<T>` (kama's `MaybeUninit`) with `write(give x)` / `take() -> T` intrinsics so
  container authors stop hand-rolling both directions. Spike: is the wrapper worth the surface, or does the
  handful of container sites not justify it? Non-blocking; pure ergonomics for stdlib authors, not users.

## 4. Reflection + serialization — remaining follow-ups (1.x)

Serialization ships today (by-value + object-graph + polymorphic contracts, json backend) — see
[SPEC.md](SPEC.md) "Serialization". What remains is additive library + hardening work:

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T,N>` read; a bare `encode`/`decode` of an
  intrinsic/enum value; generic enums. (A `const` field is a separate general language gap — doesn't parse today.)
- **More back ends (library, no compiler change)** — YAML; **binary** (packing + `@bits(n)` + little-endian
  canonical); **XML**/**HTML**. Each is a `Serializer`/`Deserializer` impl + `encode`/`decode`. `std::encoding::base64`
  is a separate small module.
- **`@deprecated` attribute (language, adjacent)** — a declaration marker (rides the `@`-attribute infra)
  emitting a use-site warning. Its own small task.
- **Optional/default *function/constructor* parameters (language, adjacent)** — the "options struct with
  optionals" ctor pattern. Kept a deliberate non-goal for now: named static factories + named params cover
  it. (Distinct from **default *type* parameters**, which shipped — see below.)

**String interpolation `"${x}"` rides on the same `std::fmt` to-string substrate**, so it sequences here.

## 5. 1.x — systems & runtime (post-1.0)

Capabilities built on the finished language — the substrate the engine needs (asset I/O, scene
serialization, networking).

- **Reflection + declarative serialization** — see §4; back ends follow as modules. Rides on the shipped
  `std::fs`/`std::io` for asset + scene load.
- **Container / data-structure reach.** The core containers ship — `DynamicArray`/`FixedArray`/`InlineArray`/
  `string`, `Map`/`Set`, `Deque`, `PriorityQueue`, `SlotMap`, `BitSet`, `SortedMap`/`SortedSet`, and the
  `View<T>` slice/span (see [SPEC.md](SPEC.md) *Collections & strings*). Honest caveat on what is **not**
  planned as stdlib: general **linked lists** are mostly a cache anti-pattern in data-oriented engines (the
  useful form is an intrusive free-list / LRU); raw **BSTs** are subsumed by the sorted map; **spatial trees**
  (quadtree/octree/BVH/k-d) are engine-specific.
- **Collections revisit — remaining knobs & optimizations.** The parametric knobs themselves ship —
  preallocation (`reserve`/`withCapacity`), a pluggable `Hasher`, and a custom `Allocator` type parameter on
  every container and box, all defaulted so the plain API is unchanged (see [SPEC.md](SPEC.md) *Collections &
  strings* / *Custom allocators* / *Allocator-aware new*). What remains is a security follow-on plus memory
  optimizations:
  - **HashDoS-resistant keyed hashing (deferred).** `DefaultHasher` is deterministic/*unseeded* — a strong
    avalanche and the right default for trusted keys (Java `HashMap` / C++ `unordered_map` posture), but NOT
    resistant to attacker-chosen keys. A seed at the `finish` stage can't fix this: it would defend integer
    keys but leave string keys (unseeded FNV-1a content hash) fully exposed — two strings colliding under FNV
    collide in every map regardless of the seed. Real resistance needs a **seeded, keyed hash over the key
    bytes** (SipHash-class): the seed must enter the per-byte content accumulation, i.e. a keyed-hash protocol
    + OS entropy + per-map seed storage. It **rides the pluggable `Hasher` seam non-breakingly** (no existing
    `Map<K,V>` changes), so it's a clean future milestone — do NOT ship a finish-stage `SeededHasher`
    (misleading safety for the case that matters).
  - **Zero-size-field elision — deferred optimization.** The default `Owned<T>` carries a `GlobalAllocator
    alloc` field (mirroring the collections), which pads the handle (the runtime call inlines to a bare
    `free`, but the field is real). A general "drop any empty-struct field + synthesize a throwaway receiver
    for method calls on it" pass would reclaim it on `Owned` *and* every collection at once — its own tested
    change (touch-sites: struct decl, field read/assign, copy, serialize).
  - **Store-once allocator / thin smart-ptr handles — deferred optimization (Rust `Arc<T,A>` model).** The
    whole smart-pointer family carries `A` **per handle** (a small copyable value handle over externally-owned
    arena state — copying it duplicates pointers, not real allocator state). The memory-optimal alternative
    stores the allocator **once** in a monomorphized control block and keeps handles thin (pointers), so
    `copy()` just bumps a count. Not taken because (a) `Owned` has no control block, so it can't unify; (b) it
    would reopen the shipped concrete smart-pointer path for consistency; (c) the savings are small precisely
    *because* the allocator handle is already lightweight — and the common default-`GlobalAllocator` handle
    fatness is already covered by zero-size-field elision above. Revisit as a whole-family refactor gated on
    profiling, bundled with that elision pass.
  - **Fallible allocation — deferred with the embedded milestone.** `allocate -> Optional<Ptr>` (vs today's
    panic-on-OOM) is what a no-heap embedded target needs; it colors the mutating APIs with failure
    propagation. It rides the same `Allocator` seam non-breakingly, and is one of the two remaining gates
    (with globals/statics) for a true no-heap embedded build.
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

### Embedded / bare-metal MCU (Pi Pico · Arduino · ESP32) — design pinned, build with the milestone

"Pi/Arduino support" is **two targets**, and the split is the whole story:
- **Raspberry Pi (Linux — Pi 3/4/5, Zero):** a full ARM app processor running Linux — MMU, OS, heap,
  filesystem. **Kama already targets this** (portable C11 → `zig cc`/clang cross-compile to
  `aarch64-linux`). Unlocking it is ~a cross-compile triple + **GPIO/I²C/SPI bindings**, and those are
  ordinary C FFI over `libgpiod` / `/dev/mem` — a *library*, not compiler work. Low effort.
- **Bare-metal MCU (Arduino AVR, Cortex-M: Pi Pico/RP2040 · Arduino Zero/Nano 33, ESP32):** *freestanding*
  — no OS, no filesystem, KB of RAM, often no heap, a startup file + linker script instead of hosted libc.
  ⚠️ The **Pi Pico is an MCU, not a Linux Pi** — so "Pi" spans both buckets. This is the real milestone:

  | Piece | What's needed |
  |---|---|
  | **Freestanding runtime** | `--target embedded` (`-ffreestanding -nostdlib`); `kama_runtime.h` stops assuming hosted libc; entry contract (`main()`+loop, or Arduino `setup()`/`loop()`) — no `argc/argv` shim, `main` never returns |
  | **No-heap / pluggable allocator** | the big one — `Owned`/`Shared`/`DynamicArray`/`string` are malloc-backed. Either a no-heap subset (`value` + `InlineArray<T,N>` + `Ptr` + stack) **or** bring-your-own allocator so those ride a static arena/pool. **Ties directly to the planned "allocator passed to every collection" work** — the same seam serves embedded no-heap and engine arena pools |
  | **Globals / statics** | MCU code lives on module-level state (peripheral handles, ISR flags, flash tables); new language surface with deterministic zero/const init. Also **`const` data in flash** (`.rodata`; on **AVR** the Harvard `PROGMEM` wart) |
  | **`hardware` qualifier** | the renamed `volatile` — `hardware Ptr<T>` → `volatile T*` for MMIO registers, and `hardware` on an ISR↔loop global; mirrors the shipped `const Ptr<T>` → `const T*`. **MMIO + single-core ISR only — NOT a concurrency primitive** (that's §6 atomics) |
  | **ISR declaration** | bind a function to an interrupt vector with the right calling convention (`__attribute__((interrupt))` / vendor `ISR()` macro) |
  | **Toolchain / build** | target triples (`thumbv*-none-eabi`, `avr`, …), linker scripts (`-T`), startup objects, MCU flags, and linking the vendor HAL (pico-sdk / Arduino core / esp-idf) + flashing |
  | **Panic/trap handler** | make a bounds/overflow trap configurable (halt / reset / blink). The trap lowering is **already runtime-free** (`__builtin_trap`) — works freestanding today ✓ |

  **Why kama fits well:** no-GC + RAII → deterministic, no hidden pauses; allocation is explicit in the
  emitted C (greppable no-heap audit); trap lowering already dependency-free; `InlineArray<T,N>`, sized ints, and
  `unsafe`/`Ptr` FFI already exist. **North star: blink an LED** (the embedded "first triangle") — forces
  exactly the critical path and nothing else. **Start Cortex-M, not AVR** (`zig cc`/clang do `thumbv*-none-eabi`
  cleanly; pico-sdk is tidy; AVR's Harvard/`PROGMEM`/`avr-gcc`-only pain comes later).

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
  - **Compiler primitives (already ship — see [SPEC.md](SPEC.md) *Exposing to a host*):** the `kama build
    --shared` `.so`/`.dylib`/`.dll` mode and the `expose` keyword's C-ABI linkage are the *same* kama→host
    boundary the **wasm exports** and the **scripting host** (§7) use — so hot-reload needs **no new language
    surface**, it consumes planned surface. One boundary, three consumers. So the remaining hot-reload work is
    all library/engine: *(a Windows copy-before-load, so the on-disk `.dll` can be rebuilt while loaded, is a
    library concern.)*
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
- **Serialization benchmark track.** Serialization is a headline feature — add a round-trip workload, but
  scoped honestly: it measures *library maturity + reflection-vs-compile-time strategy*, a different axis
  than the compute kernels. Only **6 of 11** bench languages have **stdlib** JSON (kama, Go, C#, Python, JS,
  TS); Rust/Java/C++/C/Lua need third-party libs (a Dockerfile rebuild + an "idiomatic per-language lib"
  caveat, shifting it from a language compare to a library compare). **v1:** a by-value **tree round-trip**
  (encode+decode a fixed nested struct + list, N iters, checksum→exit) across the stdlib-JSON six — a clean
  contrast of *intrinsic (kama)* vs *runtime-reflection (Go/C#)* vs *interpreted (Python/JS)*. **Document,
  don't race, the object graph:** kama's shared/`Weak`/`Owned` graph serde has no equivalent in other JSON
  libs (they serialize trees, not ownership graphs), so it's a capability note in RESULTS.md, not a
  head-to-head number. Defer the external-lib languages (Rust-serde, Jackson) to a later labelled section.
- **`map` is not apples-to-apples — root-caused (2026-07-13), fix = equalize the workload.** Native `map`
  (~8.7 ms) is the one workload off C parity (~3.0 ms), because unlike the compute kernels (identical
  algorithms) each language's `map` uses its **idiomatic native map**: kama's stdlib `Map` (grow-from-8,
  splitmix64), C hand-rolled open-addressing (preallocated, single-mul hash), Go/C# preallocated stdlib maps,
  Rust `HashMap` (SipHash), C++ `unordered_map` (chaining). So it measures *map design*, not codegen. Two
  confounds, both measured (100k×10, `-O3`):
  1. **Hash strength.** kama's splitmix64 (two dependent 64-bit muls) vs C's single Fibonacci multiply. At
     an EQUAL hash both do the same work: give C splitmix64 and it goes 2.9 → **5.7 ms**; kama with a single
     multiply goes 8.6 → **3.3 ms ≈ C's 2.9 ms**.
  2. **Preallocation.** At equal (splitmix) hash, C-preallocated 5.7 ms vs kama-grow-from-8 8.6 ms — the rest
     is kama rehashing ~15× during the insert because `Map` can't preallocate (see §5 collections revisit).
     With equal hash **and** equal prealloc, kama ≈ C (the Map machinery — probe, `Optional`, `cloneVal` — is
     already at parity; identity-hash kama 2.58 ms is *faster* than C).
  **Fix for the bench (all languages near parity):** make `map` an equal-workload kernel like fib/pi — the
  same hand-rolled open-addressing int→int map with one shared hash + fixed preallocation in every language —
  OR, once `Map` gains `reserve`/a pluggable hasher (§5), pin those in the kama version and match the hash in
  the hand-rolled references. Either way the goal is: same algorithm, same hash, same prealloc → the delta is
  pure codegen. (Aside: the apparent 3.4→8.7 ms "regression" vs the 2026-07-09 baseline was a **correctness
  fix**, not a slowdown — a wide-`uint64` literal-truncation bug had clamped all three splitmix constants to
  `int64::MAX`, which the compiler lowered to a cheap `(x<<63)-x` shift-subtract; fixing the literals restored
  the real multiplies.)

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
