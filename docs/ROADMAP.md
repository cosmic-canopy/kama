# kama roadmap

The forward plan — near-term to long-term, read in sequence. The language's **history** lives in the git
log; what the language **is** lives in [SPEC.md](SPEC.md). This file is only *what's next*.

> ### ⚠️ Maintaining this file — read before editing it
>
> **When an item ships, DELETE it from this file.** A roadmap that also logs completions stops being
> readable as a plan, and this one has drifted that way twice.
>
> Before deleting, confirm the record lives where it belongs, and **migrate it there if it does not**:
>
> | What shipped | Where its record goes |
> | --- | --- |
> | Language surface (syntax, semantics, stdlib API) | [SPEC.md](SPEC.md) |
> | A campaign (its design, decisions, and as-shipped record) | its `docs/design/*.md` |
> | A capability against a target domain | [MCU_READINESS.md](MCU_READINESS.md) · [ENGINE_READINESS.md](ENGINE_READINESS.md) · [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md) |
> | User-facing behavior + workflow | [packages.md](packages.md) · [editors.md](editors.md) · [mcu.md](mcu.md) · [targets.md](targets.md) |
> | *Why* a thing happened, and when | the git log — do not re-tell it here |
>
> What may stay behind is **at most a one-line pointer**, and only where a forward item depends on it.
> A **residual** of shipped work (a gap, a follow-on, a deferred optimization) stays — as its own forward
> item, stated as what is left to do, not as a recap of what was done.

## The shape

- **1.0 — language complete.** The core language, the std I/O foundation (`std::io`/`fs`/`net`), and the
  math layer (`std::math`) are in place (see [SPEC.md](SPEC.md)). The language surface is stable and the
  pre-1.0 work list is closed; **the last gate is the docs/naming reconcile** (§1) — you build *with* the
  language, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: deeper stdlib reach, more serde
  back ends, MCU toolchain packaging, engine/GPU library work. Mostly library + codegen, little new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via a shared IR
  feeding C, direct-wasm, and a bytecode VM — the `kama` binary self-contained (§7).
- **Concurrency** — shared-nothing by construction (isolates + channels + `scope` + `Atomic<T>` +
  immutable-`Shared` + `parallel_for`), native + wasm. **Shipped** (campaign complete); spec of record is
  [docs/design/concurrency.md](design/concurrency.md) + [SPEC.md](SPEC.md). No language work remains — the
  job-system / event-loop scheduler are libraries on the primitives (§8, web-framework readiness).
- **Engine track** (product north star): a portable lightweight **WebGPU** game engine, woven through 1.x — a
  product built *on* kama, not part of the language (§8).

## 1. Remaining before 1.0

What the language *is* lives in [SPEC.md](SPEC.md); the engine/MCU capability matrices in
[ENGINE_READINESS.md](ENGINE_READINESS.md) / [MCU_READINESS.md](MCU_READINESS.md); the history in the git log.

**The language surface is complete, and the pre-1.0 work list is closed.** Soft-float + fixed-point, the
argv/env prelude floor, diagnostics & logging, and `std::process` (POSIX + Windows) have all shipped —
records in [SPEC.md](SPEC.md) (`std::num::Q16_16`, "Command-line arguments + environment", `std::log`,
`std::process`), [design/logging.md](design/logging.md), [design/std-process.md](design/std-process.md) and
[MCU_READINESS.md](MCU_READINESS.md).

**The one remaining gate is the docs/naming reconcile → then tag 1.0.** 1.0 is the API-stability point;
naming and case conventions are fixed at the tag (PascalCase types, lowerCamel methods, no `I`-prefix on
contracts, lowercase `string`).

Everything else here is library or toolchain work that does **not** gate the tag:

1. **`std::process` — async/Poller-driven *live* child-stream reads.** `run()` captures a finished child's
   output today; streaming a running child's stdout as it arrives is the piece left.
2. **Standard-library follow-ups** (no new language surface — pure library/codegen):
   - **`std::net`** — UDP, DNS/`getaddrinfo`, ephemeral-port `getsockname`.
   - **`std::fs` / `std::io`** — buffered readers, richer `Metadata` (mtime/perms), path helpers, `mkdir`.
   - **`std::io` transform adapters (compression et al.)** — `Writer`/`Reader` *wrappers* that transform bytes
     in flight, composing with serde and net (Go/Rust `io`-wrapper style): `DeflateWriter<W>`/`InflateReader<R>`
     (gzip/deflate), later checksums/hashing/framing. On the **web target** these are a near-free ride — wrap
     the browser's built-in `CompressionStream`/`DecompressionStream` (no wasm code-size cost); on native, wrap
     zlib/zstd. Composes as `encodeTo(v, into: DeflateWriter(sink))`. The *transport* free-rides too
     (WebSocket `permessage-deflate`, HTTP `Content-Encoding`). Pairs naturally with the binary serde backend
     (crushes its field-name redundancy). (Engine-level replication — snapshots/deltas/dirty-tracking — stays
     above this, in the engine.)
   - **Windows CI** — the `windows-latest` leg passes the full suite; promote it from best-effort to
     **required** so a Windows regression blocks a merge.
3. **MCU toolchain packaging — polish.** The turnkey Cortex-M path ships and is QEMU-proven
   ([mcu.md](mcu.md)). What is left: more board presets (STM32/Pico), vendor-HAL glue, and a real-hardware
   flash pass — detail in §5 (embedded "Toolchain / build" row).

**Post-1.0 — the decided big-arc sequence (with the user, 2026-07-26):**
1. **LSP first — v1 SHIPPED (§10).** It did the double duty it was chosen for: the front end is now a
   **reusable query API with real source spans** (the `%locations` work deferred until then), which every
   later tool rides on. **M7 — the tree-sitter grammar — is what remains of the arc** (§10).
2. **Scripting / multimodal (§7) — the flagship 2.0.** The polymorphic-emitter → direct-wasm → bytecode-VM arc,
   driven by wanting a fast iteration/runtime tier for game engines + web. First concrete step: refactor the C
   emitter behind an abstract backend interface (C as the first impl), the shared lowering in the base.
3. **Self-hosting — the final-version capstone, LOWEST priority.** A maturity/dogfooding milestone, **not** an
   enabler: it rides on #1 (front-end-as-library) + #2 (runtime-into-kama), which is why §7 notes the
   runtime port makes multi-backend and self-hosting *the same project*. Do it last, when the language is stable.

**⚠️ Non-negotiable performance invariant (across all of the above).** kama is at **C parity today**, and the
**native/release tier (`kama → C → clang/emcc`) stays exactly as fast + lean — untouched.** Multimodal is
**strictly additive**: the same kama syntax *also* renders to direct-WASM and (later) a scripting VM as **separate
iteration tiers**, never a replacement for the C backend. Direct-WASM is "another hot-reload/scripting option" —
near-native and toolchain-free, but **not** as fast as native kama today (LLVM's optimizer + SIMD autovectorization
keep `C→emcc -O3` ahead on heavy numeric loops), so the **release tier remains the max-performance path for both
native and web**. Don't conflate "can emit WASM directly" with "the fast web path."

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The
language-completeness residual is **closed**; what remains here is genuinely later-track or opt-in. (Shipped
language features — `hardware`, string interpolation + format specifiers + `@generate(Format)` + tagged
strings, the construction model / named ctors / on-type turbofish, custom-allocator default-seal — have moved
to [SPEC.md](SPEC.md) / [docs/design/construction-model.md](design/construction-model.md).)

- **Contract refinement — two under-tested edges (clean workarounds).** `type contract Child … implements
  Parent` works for dispatch, but was exercised mainly with scalar-param parents. (a) A merged parent method
  whose param is a **generic instance** (`View<uint8>`) re-resolves in the *child* contract's namespace at
  vtable-emit, so the child's file must `import` that generic type or the emitted C vtable names an undefined
  type. (b) A concrete type implementing the child gets **no parent-contract conformance thunk** — pass it
  where the parent is expected only if it *also* spells `implements Parent`; and a child-contract-**value** →
  parent-contract-param upcast is unsupported (dispatch *through* the child to inherited methods works). Both
  have trivial workarounds (used in `lib/std/net/stream.kama`); fixing (a) = resolve the merged param under
  the parent's namespace in `linkContracts`, (b) = auto-emit parent thunks for refining-contract implementers.
- **Unicode module (post-1.0).** The shipped `string` core is UTF-8 bytes + `.chars()` codepoints with
  **ASCII** casing/whitespace; a later module adds Unicode-correct casing + whitespace, and an eager
  `DynamicArray<string>` collect for `split` (the lazy `Split` iterator ships today).
- **Stdlib layering — 3 LOW-prio follow-ups ([design/stdlib-layering.md](design/stdlib-layering.md)).** The
  prelude-vs-`lib`-vs-primitive split is principled (contracts/syntax/intrinsics in the prelude; backends
  opt-in), so nothing is mis-placed. Recorded, none blocking: (a) split/MCU-promote `Atomic` so lock-free cells
  need no pthread runtime (MCU track, §5); (b) an idiomatic `std::gpu` kama module over the raw `kama_gpu.h`
  seam (engine track, §8); (c) confirm intrinsic `Array`/`List` vs library `DynamicArray` naming against GOALS
  "one way" (collections revisit, §5). Decided NOT to add a convenience-import of common containers — explicit
  per-symbol imports stay.
- **Format/interpolation follow-ups (on the shipped `std::fmt` substrate).** Interpolation, format specifiers,
  `@generate(Format)`, and tagged strings all ship (SPEC). Still open, additive, no current need: combining a
  base marker with width/flags (`${n:08x}`), a custom fill character, center-align (`^`); a `@generate(Format)`
  on a **generic**/**variant**/**enum** type; a `${x:?}`-routed `@generate(Debug)` (spec hook already exists);
  per-derive `@skip(Format)` / `@skip(Serialize)` for redaction (today `@skip` is one shared boolean —
  parameterize `FieldInfo::serSkip` to a per-derive set when a concrete case appears); and tagged-string
  *type-preserved params* (Model B — each hole keeping its static type into the params list, `html` returning
  a distinct `SafeHtml`). Regex is a separate campaign. `string + <number>` stays a compile error by design.
- **Full `expose` (2.0).** The minimal `expose fn` free-function C-ABI boundary ships today (SPEC + §8
  hot-reload); the **full `expose`** — richer wasm module exports + the scripting host interface — stays 2.0 (§7).
- **Opt-in `Equatable` derive (auto `==` for `value` types) — post-1.0 minor nicety.** Deferred into the
  construction-model campaign's broader derive story (`Equatable`/`Hashable`/`Copyable` as one consistent
  `@generate` surface). Kama today requires a hand-written `operator==` (auto structural `==` is a deliberate
  non-default); the derive would synthesize a memberwise `==` on request. See
  [design/construction-model.md](design/construction-model.md) §8c.
- **Force explicit field init (construction-model tightening — design campaign).** Make every ctor assign
  *every* field explicitly, with the compiler eliding redundant zero-stores — **except** types that opt into
  zero (`@generate(zero)` bags). Precedent: Rust (all fields required), Swift (definite initialization), Zig,
  C# structs. This *tightens* the existing keystone (`checkNamedCtorComplete` already forces owning +
  non-default-fillable fields) by removing the carve-outs. Two design questions to settle first: (1) are
  pointer-shaped fields (`Ptr`/`Owned`/collection) auto-exempt, or must they spell `= null` (Zig's
  spell-or-declare-default is most uniform)? (2) is a bare `T x;` *outside* a ctor still allowed, or must every
  value come from a ctor (Rust/Swift: no bare uninitialized values)? Cost is a one-time stdlib sweep (~40
  collection/allocator ctors gain explicit `len = 0` / `data = null`). **Why it matters:** it is the language's
  proper answer to "drop only if live" — it subsumes the abandoned definite-construction-for-drop-safety
  attempt and lets a raw-handle `resource` retire its `fd > 0` drop guard. Relates to
  [design/construction-model.md](design/construction-model.md).
- **Raw-handle drop guard + the "can you own stdin?" question (fix after force-explicit-field-init).** Today
  `std::fs::File`'s dtor guards `if (fd > 0)` so a `{0}` `File` drops cleanly — but the drop-only-if-live
  compiler work already stops the compiler dropping a `{0}` on the field-first-write and `match(give)` paths,
  so the guard now only defends the residual bare-local shapes. Two threads: (a) the guard is *empirically
  deletable* (the full ASan suite passes with it removed) — once force-explicit-field-init closes the residual
  it should go; (b) `fd > 0` (not `>= 0`) means a `File` **cannot own fd 0/1/2** (stdin/stdout/stderr) — decide
  whether owning a std stream in a `File` is legitimate (likely a distinct type / `Optional<File>`), or adopt a
  `-1` empty niche (Rust `OwnedFd`) so `fd >= 0` is ownable. Same trap awaits every future raw-handle resource
  (sockets, GPU handles).
- **Unresolved type names outside local declarations are still silent (bug, small; half-fixed 2026-07-27).**
  `resolveUserName` hands an unresolved name straight back ("caller handles"), and for a long time no caller
  did — a misspelled or unimported type passed analysis and only failed later in the C compiler, as an
  `undeclared identifier` against *generated* code. `kama check` and the LSP (same analysis path) said OK, so
  the editor showed a clean file that wouldn't build. **`checkTypeResolves` now covers LOCAL declarations**
  (`unknown type X` / `type X is not imported — it lives in ns`), guarded against generic type params, which
  flow through that funnel unresolved by design and in huge volume (a probe over the fixture corpus counted
  ~30 K hits for `T`/`K`/`A`/`V` alone). **Still silent: parameter types, return types, and field types** —
  same one-line check, but those are walked at 8+ emission sites (prototype, definition, vtable, per generic
  instance), so wiring it there naively would emit duplicate diagnostics — the fix is either a single-visit
  declaration pass or a dedupe guard (file:line:message) in `checkTypeResolves`, whichever reads cleaner.
  **De-risked:** real stdlib types (`Stdio`, `DynamicArray`, `File`, `ExitStatus`) *do* reach the unresolved
  funnel, but only from SPECULATIVE `cType` calls (enum-member probing, `exprClass`) — verified they all
  resolve correctly at actual param/return/field declaration sites, so the check won't false-positive there
  and the only guard needed is the existing type-param one. Guarded by `tests/xfail/unknown_type_local` +
  two `tools/check-lsp.sh` cases.
- **Enum-variant payload-type registration gap (bug, small).** A type used *only* as an enum variant's payload
  — where that variant is never constructed — is not registered/emitted, so the enum's C `struct` references an
  undeclared type (`unknown type name 'Shared_Probe'`). Reproduces with `enum E { A, B(Shared<Probe>) }`
  constructed only via `A`. Fix: scan **every** variant's payload types at enum registration (like class fields
  via `scanTypeForCollections`), not lazily at construction.
- **Generic free-fn / static-method can't instantiate a generic type from its own type param (limitation,
  workaround).** A generic free function `fn f<W: C>(…) { Foo<W> x = Foo.make(…); … }` fails with "unknown type
  in constructor call `Foo`": the dot-on-type ctor resolver only rewrites the type name when it is itself a
  type *param*, not a generic *template* name — even though the free-fn body IS monomorphized. A **static**
  method on a generic type is likewise uncallable with explicit/inferred args. **What works:** constructing from
  the enclosing type's own param inside a type method (how `Map` does `MapKeyIter::<K>.make`), and instance
  methods on an instance built at a concrete site. **Workaround (streams M2):** expose the op as an instance
  method. Fix = teach the ctor resolver to substitute template type-args under `_typeSubst`, plus a
  static-generic call spelling. Post-1.0, additive; not a blocker.
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts with "one way to do
  a thing," and **named parameters** already cover the disambiguation overloading is usually reached for.
  **Operators are the sanctioned exception** — a type may carry several `operator*` distinguished by operand
  type (`mat*vec`, `mat*mat`), matching C++/C#/Rust. Reopen only if a concrete case shows named params can't
  express it.

## 3. Open design questions (settle before the work they gate)

- **Modular / opt-in stdlib — does "pay for what you use" pruning scale?** The **prelude mechanism**
  (`PRELUDE_SRC`) is the seed: a stdlib = more prelude-collected kama modules in a `Std` namespace. Generic
  types emit only when instantiated, and `--gc-sections` prunes unused functions in release. Open: whether that
  pruning suffices, or explicit per-module opt-in / dead-function elimination is warranted before a large stdlib
  grows. (`std::math`/`std::io` already ship as directory modules under this mechanism — the open question is
  whether pruning scales, not whether the packaging shape works.)
- **Design spike — a safe wrapper for the raw-`Ptr` in/out dance (`Slot<T>` / `MaybeUninit`).** Container
  authors move owned values across the safe↔unsafe boundary by hand: `give` bridges a tracked value *INTO* a
  raw slot (source consumed), but there is **no symmetric way OUT** — reading back is a manual "zero-init a
  local, bitwise copy, take responsibility" dance (`Deque.takeAt`; `tests/give_ptr_local.kama`). This asymmetry
  is the sharp, easy-to-misuse part of the raw layer — a candidate for a small safe abstraction: a typed
  `Slot<T>` (kama's `MaybeUninit`) with `write(give x)` / `take() -> T` intrinsics. Spike: is the wrapper worth
  the surface, or does the handful of container sites not justify it? Non-blocking; ergonomics for stdlib
  authors, not users.

## 4. Reflection + serialization — remaining follow-ups (1.x)

Serialization ships today (by-value + object-graph + polymorphic contracts) with **two backends — `json` (text)
and `binary` (KBIN)** — see [SPEC.md](SPEC.md) "Serialization". What remains is additive library + hardening:

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T,N>` read; a bare `encode`/`decode` of an
  intrinsic/enum value; generic enums. (A `const` field is a separate general language gap — doesn't parse today.)
- **Binary backend follow-on (deferred).** `@bits(n)` bit-packing (tighter integers/bools), field-name
  interning, and a schema-locked *positional* mode (needs an emitter change; trades forward-compat for max
  compactness). Delta/snapshot replication stays ENGINE-level (above serde); generic byte compression is an
  io-adapter layer (§1 transform adapters), not a serde concern.
- **More back ends (library, no compiler change)** — YAML; **XML**/**HTML**. Each is a `Serializer`/`Deserializer`
  impl + `encode`/`decode`. `std::encoding::base64` is a separate small module.
- **`@deprecated` attribute (language, adjacent)** — a declaration marker (rides the `@`-attribute infra)
  emitting a use-site warning. Its own small task.
- **Optional/default *function/constructor* parameters (language, adjacent)** — the "options struct with
  optionals" ctor pattern. A deliberate non-goal for now: named static factories + named params cover it.
  (Distinct from **default *type* parameters**, which shipped.)

## 5. 1.x — systems & runtime (post-1.0)

Capabilities built on the finished language — the substrate the engine needs (asset I/O, scene serialization,
networking). MCU/embedded language surface (statics, `hardware`, ISR/`@section`, freestanding target, fallible
alloc, inline asm) and the const-eval ladder (const generics, `comptime` constants, `comptime fn`, `@compileFor`)
have all **shipped** — see [MCU_READINESS.md](MCU_READINESS.md) / [SPEC.md](SPEC.md) /
[design/comptime-fn.md](design/comptime-fn.md) / [design/conditional-compilation.md](design/conditional-compilation.md).
Remaining forward work:

- **Reflection + declarative serialization** — see §4; back ends follow as modules. Rides on the shipped
  `std::fs`/`std::io` for asset + scene load.
- **Container / data-structure reach — honest non-goals.** The core containers ship (`DynamicArray`/`FixedArray`/
  `InlineArray`/`string`, `Map`/`Set`, `Deque`, `PriorityQueue`, `SlotMap`, `BitSet`, `SortedMap`/`SortedSet`,
  `View<T>` — see SPEC). **Not** planned as stdlib: general **linked lists** (mostly a cache anti-pattern in
  data-oriented engines — the useful form is an intrusive free-list / LRU); raw **BSTs** (subsumed by the sorted
  map); **spatial trees** (quadtree/octree/BVH/k-d — engine-specific).
- **Collections revisit — remaining knobs & optimizations.** The parametric knobs ship (preallocation,
  pluggable `Hasher`, custom `Allocator` on every container and box, all defaulted). What remains:
  - **HashDoS-resistant keyed hashing (deferred).** `DefaultHasher` is deterministic/*unseeded* — right for
    trusted keys but NOT resistant to attacker-chosen keys. A seed at the `finish` stage can't fix this (it
    would leave string keys' unseeded FNV-1a content hash exposed). Real resistance needs a **seeded, keyed hash
    over the key bytes** (SipHash-class): the seed enters the per-byte accumulation + OS entropy + per-map seed
    storage. It **rides the pluggable `Hasher` seam non-breakingly**, so it's a clean future milestone — do NOT
    ship a finish-stage `SeededHasher` (misleading safety for the case that matters).
  - **Zero-size-field elision — deferred optimization.** The default `Owned<T>` carries a `GlobalAllocator alloc`
    field (mirroring the collections), which pads the handle. A general "drop any empty-struct field + synthesize
    a throwaway receiver for method calls on it" pass would reclaim it on `Owned` *and* every collection at once.
  - **Store-once allocator / thin smart-ptr handles — deferred optimization (Rust `Arc<T,A>` model).** The
    smart-pointer family carries `A` **per handle** (a small copyable value handle). The memory-optimal
    alternative stores the allocator **once** in a monomorphized control block and keeps handles thin. Not taken
    because `Owned` has no control block (can't unify), it would reopen the shipped concrete path, and the
    savings are small (the handle is already lightweight). Revisit as a whole-family refactor gated on profiling,
    bundled with the elision pass above.
- **Browser networking transports** — native TCP ships (`std::net`); the browser has no raw sockets, so the wasm
  path needs **WebRTC DataChannels** (unreliable) / **WebSockets** (reliable) via a host FFI shim. Native
  UDP/DNS and the rest of the stdlib reach are the §1 follow-ups.
- **Native dispatch devirtualization** *(optimization, not a gap).* On a *monomorphic* call site clang does not
  devirtualize the emitted C vtable while rustc does — a clang-vs-rustc optimizer gap (hand-written C is equally
  behind), not a kama defect. kama can win where it *sees* the concrete type by emitting a **direct call** — a
  laddered pass:
  - **Tier 1 — sound static devirtualization (no inlining).** Direct-call when the target is provable: a
    concrete-value receiver, a `final` class/method, or a method with no overrides program-wide (a slot→overridden
    map after `buildVtables()`). kama's whole-program view makes the last one free where C++ needs LTO +
    `-fwhole-program-vtables`. Land this first.
  - **Tier 2 — intraprocedural type-flow.** Devirtualize a base-typed local with a proven concrete assignment.
  - **Tier 3 — inlining-enabled / guarded devirtualization.** A kama-level inliner (hard part: integrating callee
    scope-cleanup / drop order / move-state with `emitScopeCleanup`) then re-run Tier 1, or guarded inline caches.
    A separate, larger project — pursue only if a real hot path (engine ECS dispatch) proves Tier 1 insufficient.

### Embedded / bare-metal MCU (Pi Pico · Arduino · ESP32) — language surface shipped; toolchain packaging remains

"Pi/Arduino support" is **two targets**:
- **Raspberry Pi (Linux — Pi 3/4/5, Zero):** a full ARM app processor running Linux — MMU, OS, heap, filesystem.
  **Kama already targets this** (portable C11 → `zig cc`/clang cross-compile to `aarch64-linux`). Unlocking it is
  ~a cross-compile triple + **GPIO/I²C/SPI bindings** — ordinary C FFI over `libgpiod` / `/dev/mem`, a *library*.
- **Bare-metal MCU (Cortex-M: Pi Pico/RP2040 · Arduino Zero/Nano 33, ESP32; later AVR):** *freestanding* — no OS,
  KB of RAM, often no heap, a startup file + linker script instead of hosted libc. The **language surface is
  done** — the whole Tier-0/Tier-1 set (module statics, `hardware`, `@interrupt`/`@section`, `--target embedded`,
  fallible `allocate`/`try new`/`@noheap`, inline `asm`) have all shipped — see [MCU_READINESS.md](MCU_READINESS.md).
  What remains is **build/library**, not language:

  | Piece | What's needed |
  |---|---|
  | **Toolchain / build** | The turnkey Cortex-M path ships and is QEMU-proven ([mcu.md](mcu.md)). **Remaining:** more board presets (STM32/Pico), vendor-HAL glue (pico-sdk / esp-idf), a real-hardware flash pass, and (optional) folding the two-step link into `kama build --target <board>`. Arduino `setup()`/`loop()` is a later HAL nicety. |
  | **AVR (Harvard) family** *(deferred — Cortex-M/RISC-V first)* | Three AVR-specific pieces: (1) ISR — `@interrupt("VECTOR")` → the `ISR(VECTOR)` macro (`<avr/interrupt.h>`), not the parameterless `__attribute__((interrupt))`; (2) Harvard `PROGMEM` — flash const data needs `PROGMEM` + `pgm_read_*` accessors (a flash pointer can't be plain-deref'd), so `@section` alone doesn't cover it; (3) toolchain — `avr-gcc`-only (clang/zig don't target AVR cleanly). A bounded follow-on when demand warrants. |

  **Why kama fits:** no-GC + RAII → deterministic, no hidden pauses; allocation is explicit in the emitted C
  (greppable no-heap audit); trap lowering is dependency-free; `InlineArray<T,N>`, sized ints, and `unsafe`/`Ptr`
  FFI already exist. **North star: blink an LED** (the embedded "first triangle"). **Start Cortex-M, not AVR**
  (`zig cc`/clang do `thumbv*-none-eabi` cleanly; pico-sdk is tidy; AVR pain comes later).

### Compile-time evaluation & platform-specific compilation — shipped; residuals

The const-eval ladder (6b-1 const-generic arithmetic, 6b-2 named `comptime` constants, 6b-3 `comptime fn`
compile-time evaluation) and decl-level conditional compilation (`@compileFor(FLAG)` + `kama.json` flag manifest)
have all **shipped** — see [design/comptime-fn.md](design/comptime-fn.md) and
[design/conditional-compilation.md](design/conditional-compilation.md). Forward residuals:

- **Host-endianness flag → `htole`/`htobe` (small).** `std::num` `byteswap*` (pure value swaps) + `bitcast`
  ship, but *host-order* serialization helpers need a compile-time endianness fact pure kama arithmetic can't
  observe — a natural `@compileFor`-style built-in flag (`LITTLE_ENDIAN`/`BIG_ENDIAN`). Every current target is
  little-endian, so this is deferred until a big-endian target appears.
- **`comptime fn` nice-to-haves (deferred).** `sizeof`/`alignof` and named-arg reorder *inside* a comptime fn
  body; a **local** `comptime T X = f();` initialized by a comptime-fn call (module + type-associated const
  forms ship); a per-fn `@steps(…)` budget override; dual-use fallback emission.
- **Known rough edge (backlog).** `foreach` over a `static const` fixed array (a `comptime` array constant) emits
  a C `const`-discard warning — the foreach lowering takes a non-`const` receiver pointer and the by-value + `ref`
  paths share it. Benign (the loop only reads); index access is warning-free. Fix = a const-correct foreach
  lowering (const receiver pointer + `const`-element `get` on the by-value path).
- **Platform tag-type compilation.** The `@compileFor`-gated contract-impl seam is the sanctioned platform-variance
  mechanism (per-platform `type` impls behind a platform-agnostic `contract`, exactly one survives) — NOT
  in-function branching / `#ifdef`. Extending it as new targets land is forward library/driver work.

## 6. Concurrency — shared-nothing by construction (shipped)

**Shipped** — campaign complete, **no language work remains**. Spec of record:
[docs/design/concurrency.md](design/concurrency.md) + the Concurrency section of [SPEC.md](SPEC.md).

The higher-level **job system / event-loop scheduler are libraries** on these
primitives (Go/Erlang-style block-on-channel, deliberately **not** `async/await` function-coloring) — see the
engine track (§8) and [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md).

- **Deferred (reopen only on a concrete case) — general shared-memory ("hybrid").** Co-equal shared-memory
  threading is *not* planned; it reintroduces the hazard the model removes. Capability is retained (via the
  `Atomic<T>` seam + immutable-`Shared` + disjoint `parallel_for`); only some ergonomics move behind the seam.

## 7. 2.0 — dual-mode: compiled + scripting/REPL (flagship)

**Sequenced AFTER the LSP** (§1 post-1.0 order; user, 2026-07-26). **Strictly additive — it never touches the
native/release C tier, which stays at C parity.** Direct-WASM is "another hot-reload/scripting option" (near-native,
toolchain-free) — *not* as fast as native kama today, so the C→emcc release path stays the max-perf web route.

**One language, two modes** — the *same static kama* (identical syntax, semantics, ownership rules; dynamic only
in *execution*, never in typing) usable both compiled and as a scripting language with a full REPL. The target is
a REPL that **replaces the Python/Ruby/Lua REPL** for fast iteration and compile-→-run-on-demand, at or near
native speed. Guiding constraint: **the `kama` binary is the only tool you need** — external C toolchains stay
*optional* (the portable-C release path), never required to write, run, or iterate.

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

Every backend shares the same front end, so the safety analysis is proven **once**, before lowering.

**Sequencing — polymorphic emitter first, a shared IR only when the VM forces it.** The emitter
(`kama.cemit.*`) *bakes in* the semantic lowering (monomorphization, RAII drop insertion, vtable layout,
match/operator desugaring). A "shared IR" is just that lowering **factored out** into a data structure the
backends consume — so *polymorphic emitter* and *shared IR* are the same idea at two points on a spectrum. The
pragmatic path:

1. **Refactor the emitter to an abstract interface**, C as the first implementation, the shared lowering in the
   base. Low risk.
2. **Add a direct-wasm backend** as a sibling — same lowering, different rendering. Drops the `emcc` dependency
   for self-contained web/scripting builds and proves the seam. (C→emcc still produces the maximal-compatibility
   release wasm.)
3. **Extract an explicit IR only when the VM needs it** — a bytecode VM is a genuinely different execution model,
   so re-deriving the lowering a *third* time is where a shared lowered form actually pays off. The VM becomes a
   low-drift renderer of the *same* lowered form the C backend uses.

**Design constraints (hold across the whole spectrum):**
- **Keep the lowered form high-level and structured** (retain `if`/`while`/`for` and named locals), *not*
  SSA/basic-blocks — so the C backend still emits the readable, `#line`-mapped C that is a headline feature.
- **Move the runtime into kama.** Collections/smart-pointers/`string` live as hand-tuned C in `kama_runtime.h`;
  a wasm or VM backend can't `#include` it. Finishing the port makes multi-backend and the
  kama-stdlib/self-hosting goal the **same** project — do it once, all backends inherit it.

**Direct-wasm optimization — lean on Binaryen, don't write an optimizer.** A naive direct `kama → wasm` backend
emits ~`-O0`/`-O1`-quality code. The fix is **`wasm-opt`** (Binaryen), a standalone optimizer that runs on *any*
wasm regardless of producer (the AssemblyScript model). `kama → wasm → wasm-opt -O3` recovers most of the gap. It
is **not** equal to `C→emcc -O3` (LLVM's mid-level IR optimizer + SIMD autovectorization stay ahead — heavy
numeric loops still favor the release tier), but buys **compile speed + zero dependency + a REPL** at
near-native runtime.

**Speed ladder** (fastest last): tree-walk < bytecode VM < direct-wasm/`wasm-opt` < AOT C→clang.

- **Licensing.** **Binaryen (`wasm-opt`) is Apache-2.0** — clean against the MIT goal (GOALS #8). A bundled
  **TinyCC**-JIT (near-instant native `kama run`) is a possible *optional* alternative but is **LGPL** — confirm
  linking terms before shipping it; the VM / direct-wasm paths sidestep it entirely.

## 8. Engine track (product north star)

A portable lightweight **WebGPU** game engine — a product built *on* kama, **not** part of the language. Tiers:
**math types** (shipped) → buffers/bindings → first triangle (shipped, browser + native, `examples/webgpu`) →
scene/material. Depends on the 1.x systems (file I/O for assets, serialization for scenes). The kama-scoped
remainder is at most a thin safe `std::gpu` binding wrapper over the shipped `kama_gpu.h` seam (optional stdlib
polish); the engine *spine* (buffer/pipeline/binding libraries, renderer) is the engine product. See
[ENGINE_READINESS.md](ENGINE_READINESS.md).

- **Dev-loop hot-reload — a *library* on two small compiler primitives that already ship.**
  - **Compiler primitives (ship — see SPEC *Exposing to a host*):** `kama build --shared` (`.so`/`.dylib`/`.dll`)
    and the `expose` keyword's C-ABI linkage are the *same* kama→host boundary the wasm exports and the scripting
    host (§7) use — so hot-reload needs **no new language surface**. One boundary, three consumers.
  - **Library:** the `dlopen`/`dlsym`/`dlclose` + file-watch + function-pointer rebind loop — pure FFI over
    `unsafe`/`Ptr`, zero compiler changes. This is the bulk of the feature. (A Windows copy-before-load, so the
    on-disk `.dll` can be rebuilt while loaded, is a library concern.)
  - **Engine:** the *data-in-host, code-in-module* architecture (world state lives in the platform-layer arena,
    passed *into* the reloaded module) so a reload doesn't wipe the world. Prior art: Handmade Hero, Our
    Machinery, Unreal Live++, Godot GDExtension, Bevy `hot_lib_reloader`.
  - **Scope — desktop dev only.** dlopen is absent/forbidden on ship targets (no wasm dlopen; banned on iOS;
    Android/Quest only via a pushed `.so`). Cross-platform *shipping* scripting is the §7 VM, not this. This buys
    fast native iteration on Linux/Mac/Windows — enough to justify the two tiny primitives.

## 9. Performance

Current standing (full detail in [benchmarks/RESULTS.md](benchmarks/RESULTS.md)): kama is at **C/C++ parity** on
native compute (fib/pi/collatz/fnptr/alloc + dynamic dispatch, all LLVM-AOT at `-O3`), and wins decisively on
footprint (~2 MB RSS, ~66 KB binary) and the no-GC `alloc` workload. `kama→wasm` (optimized) **beats hand-written
JS on fib/pi/collatz/fnptr (up to ~4.5×)** and is near-parity on `alloc`/`dispatch`. Forward work:

- **Bench methodology (don't re-chase).** Measure wasm at the optimizing tier (`node --no-liftoff`). Short
  workloads skew under parallel load — run with nothing else competing. Keep all LLVM-AOT languages at the same
  `-O` level (`-O3`), or the optimization level dominates a tiny kernel.
- **Bench cohort — add Zig.** kama's closest *language* rival (no-GC, AOT, and, as `zig cc`, already kama's
  bundled backend). Port the workloads to `.zig`, add the toolchain. Expect it to cluster with C/Rust on raw
  compute — the signal is the *compile-time / binary-size / RSS* columns and cohort completeness, not the perf
  ranking.
- **Serialization benchmark track.** A headline feature — add a round-trip workload, scoped honestly (it measures
  *library maturity + reflection-vs-compile-time strategy*, a different axis than the compute kernels). Only 6 of
  11 bench languages have stdlib JSON. **v1:** a by-value tree round-trip across the stdlib-JSON six —
  *intrinsic (kama)* vs *runtime-reflection (Go/C#)* vs *interpreted (Python/JS)*. **Document, don't race, the
  object graph** (kama's shared/`Weak`/`Owned` graph serde has no equivalent — a capability note, not a number).
- **`map` — equalize the workload, don't re-chase.** Native `map` is off C parity because each language uses its
  *idiomatic* map (kama grow-from-8 splitmix64 vs C preallocated single-mul), so it measures *map design*, not
  codegen. Root-caused (2026-07-13): at equal hash **and** equal prealloc, kama ≈ C (the Map machinery is already
  at parity; identity-hash kama is *faster* than C). Fix for the bench: make `map` an equal-workload kernel (same
  hand-rolled open-addressing int→int map, one shared hash, fixed prealloc in every language) OR, once `Map`
  gains `reserve` (§5), pin those in the kama version and match the reference hash.

## 10. Tooling / distribution (deferred)

- **Build configuration + cross-compilation — shipped; residuals.** Targets are `<arch>-<os>-<abi>` triples
  whose components derive the `@compileFor` flags, `TARGET`/`BUILD_TYPE`/`OUTPUT` are single-select groups a
  project extends in `kama.json`, cross-compilation follows the selected target rather than the build host,
  and static/object library output exists. Record of what shipped and why:
  [design/build-configuration.md](design/build-configuration.md); user docs: [targets.md](targets.md).
  Forward work:
  - **⚠️ No CPU-tuning knob.** kama passes **no** `-march`/`-mcpu`/`-mtune` anywhere, so every build targets
    the architecture's *generic baseline*. That is the right default (portable binaries — and it is why
    `zig cc` and clang measure identical, neither tunes), but there is no first-class way to say otherwise:
    the only route today is `"cflags": ["-mcpu=…"]` on a declared `select.TARGET` entry, so **a plain
    `kama build --release` cannot tune for the host at all**. Every peer has a shorthand (Rust
    `-C target-cpu=native`, Zig `-mcpu=native`, gcc/clang `-march=native`). Likely shape: a `cpu` field on a
    target spec, plus a `native` spelling for host builds. Wants a before/after benchmark first — kama's
    emitted C is fairly generic, so the win may be small outside float/SIMD-heavy code.
  - **Per-value `BUILD_TYPE` settings** (own opt-level/LTO/strip), deliberately deferred so `kama.json` does
    not become a build-settings language; and **numeric build options surfaced as `comptime` constants**
    rather than as flag comparisons (`@compileFor` stays tagging, not logic).
- **VS Code Marketplace publish** — the `.vsix` is built + attached to releases; Marketplace publishing is
  deferred, and gates on a public release. The extension itself is complete: syntax highlighting, the
  language client, a build-configuration status bar + picker, and zero-config source-level debugging (F5
  under CodeLLDB, breakpoints mapped back to the `.kama` via the emitter's `#line` directives).
- **Language server (LSP) — v1 SHIPPED (M0–M6, closed 2026-07-30).** `kama lsp` is a JSON-RPC/stdio
  subcommand of the compiler serving diagnostics, hover, go-to-definition, outline, find-references,
  project-wide rename, workspace symbols, completion, signature help, semantic tokens and
  build-configuration awareness — **one server, eight editors**, four of them verified against a running
  editor. Record of the campaign, its decisions and its acceptance: [design/lsp.md](design/lsp.md) (with
  the per-milestone briefs beside it); user-facing setup: [editors.md](editors.md). Forward work:
  - **►► M7 — tree-sitter grammar + Zed extension. NEXT, and its own campaign.** It is the **only** thing
    blocking Helix syntax colouring (Helix colours *only* from tree-sitter) and a Zed extension **at all**
    (a Zed extension registers a *language*, which requires a grammar — there is no grammar-less LSP-only
    Zed extension). Also unlocks full Neovim/Vim colouring beyond semantic tokens, and GitHub linguist
    recognizing `.kama`. It is a **third** grammar to keep in sync with `kama.l`/`kama.y`, so it needs its
    own drift guard from day one. Three things to settle before writing a rule, all cold-start-briefed in
    [design/lsp-m6-d-kickoff.md](design/lsp-m6-d-kickoff.md):
    1. **The oracle** — write the literal, run `kama check`, compare. The compiler decides, not eyeballing;
       that method rejected six invented spellings during the TextMate audit.
    2. **The drift guard's shape** — a *pair*, because a grep can only see that a rule EXISTS while a real
       engine can see that a rule FIRES: tree-sitter's own corpus test plus agreement with `kama check`
       over the existing `tests/syntax/`.
    3. **Where the grammar lives and how it is built** — a directory in this repo or its own repository.
       Helix and Zed both fetch a grammar by git URL, and the toolchain is containerized, so a node build
       step needs a decision.
  - **The ~10 ms fixed prelude-ANALYSIS floor per keystroke.** M5 removed the prelude *parse* from every
    keystroke; analyzing it again on every buffer change is what remains, and it is a floor no file can get
    under. The fix is a pre-baked or forkable `CEmitter` — a real piece of work, not a tweak.
  - **One build configuration per server process.** It is pinned by the first opened document that resolves
    a manifest, so in a monorepo whose packages declare *different* flag universes the unpinned packages get
    the pinned one's configuration. Softened, not fixed: the status bar says which is active and
    `kama.restartServer` exists. The real fix needs **per-configuration parse caches**, because
    `pruneInactiveDecls` rewrites cached units in place.
  - **Upstream editor registration** — a `kama` entry in `nvim-lspconfig`, in Helix's built-in
    `languages.toml` and in `eglot-server-programs`. Turns six pasted lines into zero for users, but these
    are PRs to *other* projects and gate on a public release.
  - **Three editor snippets are documented but unverified** — Vim (coc.nvim), Sublime Text and Kate. Each
    needs a human at a GUI; Sublime additionally needs its LSP package installed through Package Control.
- **Workspace-internal dependencies — shipped; one follow-on.** A sub-project is extractable: path deps
  resolve against the manifest that declared them, module resolution consults a dependency's own `sources`,
  path deps are permitted between members of one declared `projects` tree, and a file's imports are checked
  against **its own** package's manifest (a hard error, lenient only in the LSP/`query` path). Record:
  [design/workspace-deps-kickoff.md](design/workspace-deps-kickoff.md); user docs:
  [packages.md](packages.md). **Forward:** version reconciliation on publish — `kama publish` substituting a
  registry version for a workspace path dep.
- **kama-aware debugger value formatting — polish on the working debugger.** Breakpoints/stepping are already
  kama-source-level, but inspected values render in their emitted-C form (a `string` shows as
  `kama_string {data,len,cap}`, `Optional<T>` as its tagged union, collections as C structs). Add LLDB type
  summaries / synthetic providers (CodeLLDB supports Python formatters) so `string`/`Optional`/`Result`/the
  collections/smart-pointers render as kama values. Small next to the LSP, high polish-value, builds directly
  on the shipped debug flow.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
- **Package manager (ecosystem foundation).** A first-class dependency manager + registry so libraries distribute
  without vendoring — the point at which the **orphan rule** (§3, retroactive conformance) becomes load-bearing.
  Design of record: [design/package-management.md](design/package-management.md); user docs:
  [packages.md](packages.md). **All self-contained compiler work has shipped; what remains is
  hosted-services/ops work:**
  - **Both gated on hosted services / the repo being public + the website staged:**
    - **M3.3 — hosted deployment (pure ops, no compiler change).** Stand up the real registry host (Cloudflare
      Pages static index + GitHub Releases/R2 tarballs), wire the built-in default base URI (`kDefaultRegistry`,
      deliberately **empty** today so an unconfigured registry dep errors rather than reaching a dead URL) to the
      live URL, add publish auth (a token model — the one M3.1/M3.2a open question left for the remote), and
      extend a PUBLISHING.md release process. A dynamic Workers/KV/R2-or-Node service is an *optional* drop-in
      speaking the same M3.1 protocol.
    - **Rest of M3.2 — mandatory verification + the trust model.** Promote signature verification from
      warn-only/`--verify`-opt-in to enforced, and pick the trust model: Go-style checksum-transparency log vs
      npm/PyPI sigstore/OIDC provenance (TOFU / a configured allowed-signers set is the near-term step; the
      transparency/provenance choice is the larger cut).
  - **M4 (multi-modal — scripting-runtime versions in the store) deferred** until the kama scripting runtime (§7)
    exists — no second modality to version until then.
- **Longer-term — a "node.js-class" application framework in kama.** A fast, low-overhead server/app framework
  (HTTP already dogfooded via `examples/httpd`), aiming to beat the Node/Deno overhead profile on the no-GC/AOT
  (or VM-scripted) runtime — the flagship *application* of the language + package manager + scripting tiers
  together. See [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md). Aspirational, post-ecosystem.
