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

1. **Language-completeness residual (1.0 blocker — deep emitter work).** One fundamental (non-library)
   gap in the move-tracking / ownership-lowering core; it reproduces with plain resources/collections and
   has a clean workaround, so a "language-complete" 1.0 closes it but it doesn't block the stdlib/engine
   work. (Reserved later-track keyword `volatile`/`hardware` stays deferred — it hard-errors, never
   miscompiles.)
   - **Target-typed rvalue as a `match` subject** *(one residual, clean "bind to a local first" workaround)*.
     Target-typed inline construction already works in every other by-value position (initializer, `return`,
     `operator[]` store, value-producing `match` arm, class-typed lvalue store, call-argument, variant
     payload, string-rvalue indexing, inline `new`) — see [SPEC.md](SPEC.md). **Still open:** a value-producing
     construct as a `match` **SUBJECT** (`match (Optional::Some(x)) { … }`). The instance must be inferred from
     the payload during the *discovery* pass (so its struct is emitted), but that pass has no local-variable
     types — only a literal payload could infer, which isn't worth a partial feature; the full fix needs
     discovery-time local typing or lazy generic-struct emission. Documented RULES (not gaps): an inline
     `new`/value **borrowed** by a `ref`/`out` or contract parameter (an rvalue has no lvalue to reseat), and
     an inline construct in a `do/while` condition (ISO-C + `continue` semantics).
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

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The one
remaining §1 language-completeness residual is the value-producing `match` **subject** inference; what
remains here is genuinely later-track or opt-in.

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
- **Aggregate initializer for `type extern value` (FFI ergonomics).** Surfaced writing the WebGPU bindings
  ([`examples/webgpu/`](examples/webgpu/)): a field-wise call on a POD extern struct — `WGPUColor(r: 1.0,
  g: 0.5)` — silently zero-inits (the field args are dropped) instead of setting the named fields, so C-API
  descriptor code must fall back to `T x = T(); x.field = …;`. Give extern-value types a real by-name
  aggregate initializer (`WGPUColor(r: 1.0, …)` → designated init). Verbose-but-correct today; a pure
  ergonomic win for the many-field descriptor structs a C graphics/OS API is built from.
- **Transitive import of a type's public-API types (module ergonomics).** Surfaced writing `BitSet`
  ([`lib/std/collections/bit_set.kama`](../lib/std/collections/bit_set.kama)): to `foreach` over a
  collection's iterator, the iterator type must be imported *by name alongside the container*
  (`import std::collections::{BitSet, BitSetIter}`) because resolving `bs.setBits()`'s type needs `BitSetIter`
  in the consumer's scope. Generic-iterator collections dodge this — their iterator monomorphizes to a
  globally-unique name (`SlotMapValueIter_int32`) that resolves with no import — so **BitSet is the only
  collection that pays it** (its iterator is non-generic). Fix = importing a type also makes the types named in
  its public method signatures resolvable (import brings the API surface, not just the symbol). Non-blocking
  and isolated; a pure ergonomic/consistency win.
- **BUG — move-tracker: moved-state leaks across same-named variables in sibling scopes.** Surfaced writing
  `SortedMap` (M6): `give`-consuming a resource variable named `k` in one scope leaves the tracker believing a
  **different** `k` (a fresh binding — e.g. an `int32 k` in a later sibling `{}` block, or a `foreach (string
  k …)` loop var vs a subsequent `int32 k`) is still moved, so its first use is a false "use of `k` after it
  was moved". Minimal repro: `S k = S(); S d = give k; { int32 k = 5; return k + k; }` → false positive. The
  moved-set is keyed by variable **name**, not by the scoped binding; it must be scoped to the declaration and
  reset when a name is re-declared. Workaround: rename the later variable. A correctness-of-diagnostics bug
  (rejects valid code), not a miscompile.
- **BUG — generic `DynamicArray<V>.operator[]` mis-lowers to a raw pointer subscript inside a nested
  argument.** Surfaced writing `SortedMap` (M6): `return Optional::Some(value: this.clone(v: this.d[i]))` where
  `d` is a generic `DynamicArray<V>` field emits `this.d[i]` as a **raw pointer index** (→ "raw pointer access
  requires an `unsafe {}` block", and under `unsafe` a C "subscripted value is not an array" on the struct)
  instead of resolving `operator[]`. It only bites when the indexing is nested inside another call's argument
  in a return (`Some(value: f(this.d[i]))`); binding to a local first (`V x = this.d[i]; … Some(value: x)`)
  resolves `operator[]` correctly. The nested-in-argument position skips user-`operator[]` resolution for a
  generically-typed element and falls back to raw `Ptr` subscripting.
- **Minor niceties (post-1.0):** an opt-in `Equatable` derive (auto `==` for `value` types) and
  post-increment returning the old value in expression position (`i++` works as a statement today). Two small
  ergonomic gaps surfaced writing `SortedMap` (M6), both with clean idioms today: (a) an rvalue passed to a
  `ref` parameter emits `&(rvalue)` (invalid C) — bind to a local first (or auto-hoist a temp, as some other
  positions already do); (b) `give` into a raw `Ptr<T>` deref (`p[0] = give x`) is rejected for an owning `T`
  ("not a bare sub-expression") — a `ref T` out-parameter (`out = give x`) works and is the idiom the B-tree
  uses for its pair moves.
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

## 4. Reflection + serialization — remaining follow-ups (1.x)

**Serialization is feature-complete and shipped** (by-value + object-graph + polymorphic contracts, json
backend) — the compiler-intrinsic lowering, its user surface (`@generate`/`@field`/`@skip`, the
`Serialize`/`Deserialize` contracts, `encode`/`decode`, hand-written override), the `reachesPointer` mode
gate, and the `DeError` set all live in [SPEC.md](SPEC.md) "Serialization". What remains is additive
library + hardening work:

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T,N>` read; a bare `encode`/`decode` of an
  intrinsic/enum value; generic enums. (A `const` field is a separate general language gap — doesn't parse today.)
- **Enforce the poly-edge rule (hardening)** — a `Shared`/`Weak`/`Owned<Contract>` graph edge currently *assumes*
  every implementor is `@generate(Serialize, Deserialize)`; a non-`@generate` implementor is silently absent from
  the dispatch tables (its `.vtbl` → no writer). Turn this into a **compile error** at the edge (or require the
  element contract to refine `Serialize`/`Deserialize`). Small, additive; no wire/behavior change.
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

- **Reflection + declarative serialization** — see §4; back ends follow as modules. Rides on the shipped
  `std::fs`/`std::io` for asset + scene load.
- **Container / data-structure reach.** `DynamicArray`/`FixedArray`/`string`/`InlineArray` + `Map<K,V>`/`Set<K>` are shipped
  (see [SPEC.md](SPEC.md)). **Ownership-consistency pass done:** every container hands ownership back on removal
  (`DynamicArray.remove(index:) -> T` / `pop() -> Optional<T>`, `Map.remove(key:) -> Optional<V>`, matching
  `Deque.popFront`/`popBack`), and `Map` gained an in-place value borrow (`getRef(key:) -> ref V`) plus value
  iteration (`values()`/`valuesMut()`) — closing the "non-`Copyable` map value is write-only" hole. **Entry-wise
  iteration done:** `foreach (Entry<K,V> e in m.entries())` yields each key-value pair by copy (`e.key()` /
  `e.value()`, both `Copyable`) — this required building the `Entry<K,V>`-through-`Optional` monomorphization
  support in the emitter (contract-instance args now deep-substitute + absolutize nested generics; a
  `fn ref string` borrow is no longer hoisted into a dropped temp). No `entriesMut()` — a key is never mutated
  in place; use `valuesMut()` / `getRef`. **`Deque<T>` (ring buffer)** and **`PriorityQueue<T: Comparable>`
  (binary heap)** are shipped — the latter min-heap by default (`minHeap()`), `maxHeap()` to invert; over a
  `DynamicArray` (which gained an O(1) `swap(i:,j:)` primitive) for A* / event scheduling. **`SlotMap<V>`
  (generational slot map)** is shipped — `insert` returns a stable `Handle`, and `get`/`getRef`/`remove` reject
  a stale handle (one whose slot was removed/reused) via a per-slot odd-while-occupied generation, so a
  dangling handle is a clean `None`/panic, not a use-after-free (*the* ECS/asset-registry structure).
  **`SortedMap<K: Comparable, V>` / `SortedSet<K>` (B-tree, min-degree 6) shipped** — ordered iteration +
  `first`/`last`/`floor`/`ceil`/`range` beyond the hash map's surface, plus `getRef` in-place borrow, deep
  `copy`, and JSON serde (ascending-key order); nodes back their keys/values/child-boxes with `DynamicArray`
  (reusing its move-out / shift / RAII) and a child is an `Owned<BTreeNode>` box, so splits/borrows/merges/
  predecessor-swaps relocate move-only keys+values ASan-clean. It **needed a language feature**, now shipped:
  a `fn ref T` may return the result of a place-returning method call (`recv.getRef(...)`) when the receiver
  roots at `this` — the escape check traces the root through the call, as `operator[]` already does — so a
  recursive `getRef` forwards an in-place borrow up through the `Owned`-boxed tree. **Slice/span `View<T>`
  — DONE** (M7): a first-class **`type view`** kind (a non-owning, stack-only borrow = C# `ref struct`) on the
  value/resource/contract ownership axis; the stdlib `View<T>` + `arr.view()`/`arr.slice(from:,count:)` give a
  zero-copy subrange, index + mutate-through + `foreach`, `const View<T>` for read-only. Escape-checked exactly
  like a contract value (never a field, collection element, or `enum` payload; returnable only when it borrows
  `this`/a `ref` param — structural, no lifetime tracking) so it can't dangle; a view owns nothing (no `~dtor`,
  no owning fields, private fields). Users/the engine can author their own (`type view StridedView<T>`/`Grid2D`).
  *Known limitation:* a view over a `DynamicArray` is invalidated by a resize (`add`/`reserve`) — same contract
  as a C++ `span`/iterator; not enforced (no lifetime tracking). Honest caveat: general **linked lists** are mostly a cache
  anti-pattern in data-oriented engines (the useful form is an intrusive free-list / LRU); raw **BSTs** are
  subsumed by the sorted map; **spatial trees** (quadtree/octree/BVH/k-d) are engine-specific, not stdlib.
- **Collections revisit — uniform preallocation, pluggable hasher, custom allocator.** The containers grew
  piecemeal; give them a consistent set of parametric knobs (all with defaults, so today's API is unchanged):
  1. **Preallocation everywhere.** `DynamicArray`/`FixedArray` have `reserve(n:)`, but **`Map`/`Set` do not** — they start
     at cap 0 and grow from 8, rehashing every entry ~log2(N) times on a bulk insert. This is a *measured*
     cost: on the `map` bench, at an EQUAL hash, kama (grow-from-8) is ~8.6 ms vs C (preallocated `cap`) ~5.7 ms
     — the whole remaining delta after hash. Add `Map`/`Set` `reserve(n:)` + a capacity ctor `Map(capacity:)`
     so a known-size build skips the rehash storm (and the bench can preallocate for a fair compare).
  2. **Pluggable hasher (quality/speed as a user choice).** Today `Map`/`Set` bake in the strong splitmix64
     `Hashable` finalizer (two dependent 64-bit muls — the entire `map`-vs-C gap once hash is equalized). Let
     the user pick, à la Rust's `BuildHasher`: `Map<K, V, H>` with a default strong hasher, swappable to a
     fast one (single Fibonacci multiply / `h ^ (h>>>16)` xorshift) for trusted-key hot loops — strong stays
     the default (DoS-resistant), fast is opt-in. (Cheap independent win regardless: `Map`/`Set` cap is always
     a power of two, so `hash % cap` in `slotOf`/`put`/`grow` → `hash & (cap-1)` drops the `udiv`, ~11%.)
  3. **Custom allocator.** The containers hardcode `malloc`/`realloc`/`calloc`/`free`. Thread an **allocator**
     parameter (arena/pool/stack/bump for hot loops; a *fallible* allocator for the no-heap embedded target).
     Bigger surface than it looks — it has to reach element construction/relocation and RAII drop — so it
     rides with the embedded target rather than 1.0. `Map<K, V, H, A>` / `DynamicArray<T, A>` with defaults is the
     likely shape.
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
  - **Compiler (small — DONE; see [SPEC.md](SPEC.md)):** a `kama build --shared` mode emitting a
    `.so`/`.dylib`/`.dll` (`-fPIC -shared -fvisibility=hidden`; `KAMA_EXPORT` decorates each `expose`d
    symbol — `dllexport` on Windows), and the **`expose`** keyword giving reload entry points **C-ABI
    linkage**. That is the
    *same* kama→host boundary the **wasm exports** and the **scripting host** (§7) also use — so hot-reload
    added **no new language surface**, it consumed planned surface. One boundary, three consumers. *(A
    Windows copy-before-load, so the on-disk `.dll` can be rebuilt while loaded, is a library concern.)*
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
