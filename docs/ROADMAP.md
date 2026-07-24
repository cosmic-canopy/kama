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
- **Concurrency — shared-nothing by construction** (✅ **shipped** 2026-07-23, campaign complete): data-race
  freedom by removing shared mutable state, not a borrow checker — isolates + ownership-transferring channels +
  structured-concurrency `scope` + `Atomic<T>` + immutable-`Shared` + disjoint-slice `parallel_for`, native +
  wasm, TSan/ASan-proven. **Next big direction: the MCU/embedded campaign (§5/§6) — confirmed 2026-07-23;
  first step = module-level statics built to the per-isolate rule.**
- **Engine track** (product north star): a portable lightweight **WebGPU** game engine, woven through
  1.x. Its Tier-0 math types are unblocked now.

## 1. Remaining before 1.0

What the language *is* lives in [SPEC.md](SPEC.md); the engine capability matrix in
[ENGINE_READINESS.md](ENGINE_READINESS.md); the history in the git log. What remains to call the language
**complete**:

1. **Language-completeness residual ✅ CLOSED (2026-07-23).** The last two `match` subject-inference gaps
   (nested value-producing `match`/variant-producing ternary as a subject; contract-dispatched method call as
   a subject) are now fixed — see the two ✅ items below. The language surface is complete; what remains before
   the 1.0 tag is the docs/naming reconcile (§3). (The `hardware` qualifier — MCU step 2 — has since shipped;
   `volatile` is no longer a keyword.)
   - **Value-producing `match`/ternary as a `match` subject ✅ DONE (2026-07-23).** Target-typed inline
     construction works in every by-value position (initializer, `return`, `operator[]` store, value-producing
     `match` arm, class-typed lvalue store, call-argument, variant payload, string-rvalue indexing, inline
     `new`) — see [SPEC.md](SPEC.md). A bare **variant-constructor** subject (`match (Optional::Some(x)) { … }`)
     already worked via a function-level pre-scan. **Now also closed:** a *nested* value-producing `match` and
     a *variant-producing ternary* directly as a subject — the discovery pre-scan (`inferMatchSubjInst`)
     recurses through the ternary/nested-match to infer + register the tagged-union instance, and `exprClass`
     grew `MatchNode`/`TernaryExpressionNode` cases so call/var-valued branches resolve too. Fixtures:
     `tests/match_nested_subject.kama`, `tests/match_ternary_subject.kama`. Documented RULES (not gaps):
     an inline `new`/value **borrowed** by a `ref`/`out` or contract parameter (an rvalue has no lvalue to
     reseat), and an inline construct in a `do/while` condition (ISO-C + `continue` semantics).
   - **Contract-dispatched method call as a `match` subject ✅ DONE (2026-07-23).** `match (g.method())` where
     `g` is a **contract value** and `method` returns a tagged union (`Optional`/`Result`) now resolves the
     contract method's declared return type through fat-pointer dispatch (an `isInterface` branch in
     `exprClass` looks the method up via `contractMethods`, rendering the return type under the contract's own
     scope with `T` bound for a generic-contract instance). Found in streams M4. Fixtures:
     `tests/match_contract_call_subject.kama` (generic `Iterator<int32>`), `tests/match_contract_call_plain.kama`.
2. **Standard-library follow-ups (tracked; mostly post-1.0, no new language surface).** The shipped I/O +
   math subset is sufficient for 1.0; these extend the modules as pure library/codegen work:
   - **`std::net`** — UDP, DNS/`getaddrinfo`, ephemeral-port `getsockname`.
   - **`std::fs` / `std::io`** — buffered readers, richer `Metadata` (mtime/perms), path helpers, `mkdir`.
   - **`std::io` transform adapters (compression et al.)** — `Writer`/`Reader` *wrappers* that transform bytes
     in flight, composing with serde and net over the M4 substrate (Go/Rust `io`-wrapper style):
     `DeflateWriter<W>`/`InflateReader<R>` (gzip/deflate), later checksums/hashing/framing. On the **web target
     these are a near-free ride** — wrap the browser's built-in `CompressionStream`/`DecompressionStream`
     (no wasm code-size cost); on native, wrap zlib/zstd. Composes as `encodeTo(v, into: DeflateWriter(sink))`.
     Note the *transport* free rides too (WebSocket `permessage-deflate`, HTTP `Content-Encoding`) — transparent,
     no code. Generic compression also crushes the self-describing binary format's field-name redundancy, so it
     pairs naturally with the binary serde backend. (Engine-level replication — snapshots/deltas/dirty-tracking,
     reliable-vs-unreliable routing — stays above this, in the engine, not the stdlib.)
   - **`std::math` SIMD ✅ DONE (2026-07-22).** The last Tier-0 engine-readiness item. Outcome: **no SIMD
     emitter code was needed.** The measurement (permanent `math` bench, all 11 languages) showed clang already
     auto-vectorizes the math when the ops **inline**; the only thing blocking it was the build model —
     `./kama build` compiled each module as a separate TU, so `std::math` shipped as scalar out-of-line calls
     (native `math`: 5.3× C). **Fix shipped:** `--release` native builds now compile as **one unity translation
     unit** (debug keeps per-module `.c`), so the ops inline + auto-vectorize → **native math at C parity**
     (27→3 ms; dead-even with C, ahead of C++/Rust). Unity beat LTO decisively (LTO's cross-module inliner is
     far weaker than a real single TU). `Quat`'s Hamilton product is intentionally left **scalar** — measured, a
     hand-vectorized version is *slower* than the 16 pipelined scalar FMAs on ARM64. Recorded in
     [SPEC.md](SPEC.md) (*Math* + *Building & debugging*). **Deferred to 1.x (not blocking):** a concrete
     `f64`/`DVec` family alongside `float32`; rotors. (The `vector_size(16)` primitive idea is dropped — auto-vec
     behind unity builds delivers parity without it.)
   - **Windows CI** — the `windows-latest` leg now passes the full suite (the `kama_os.h` `_WIN32` branch is
     verified); promote the leg from best-effort to **required** so a Windows regression blocks a merge.
3. **Docs reconcile → tag 1.0.** 1.0 is the API-stability point; naming/case conventions are fixed here
   (PascalCase types, lowerCamel methods, no `I`-prefix on contracts, lowercase `string`).

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The §1
language-completeness residual is now **closed** (nested/ternary + contract-dispatched `match` subjects
landed 2026-07-23); what remains here is genuinely later-track or opt-in.

- **Contract refinement — two under-tested edges (tracked; clean workarounds).** `type contract Child …
  implements Parent` works for dispatch, but was only exercised with scalar-param parents until streams M4
  gave it generic-instance params. (a) A merged parent method whose param is a **generic instance**
  (`View<uint8>`) re-resolves in the *child* contract's namespace at vtable-emit, so the child's file must
  `import` that generic type or the emitted C vtable names an undefined type. (b) A concrete type implementing
  the child gets **no parent-contract conformance thunk** — pass it where the parent is expected only if it
  *also* spells `implements Parent` explicitly; and a child-contract-**value** → parent-contract-param upcast
  is unsupported (dispatch *through* the child to inherited methods works). Both have trivial workarounds (used
  in `lib/std/net/stream.kama`); fixing (a) = resolve the merged param under the parent's namespace in
  `linkContracts`, (b) = auto-emit parent thunks for refining-contract implementers.
- **Unicode module (post-1.0).** The shipped `string` core is UTF-8 bytes + `.chars()` codepoints with
  **ASCII** casing/whitespace; a later module adds Unicode-correct casing + whitespace, and an eager
  `DynamicArray<string>` collect for `split` (the lazy `Split` iterator ships today).
- **Stdlib layering — triaged, 3 LOW-prio follow-ups ([design/stdlib-layering.md](design/stdlib-layering.md)).**
  The prelude-vs-`lib`-vs-primitive split is already principled (*contracts/syntax/intrinsics in the prelude;
  backends opt-in* — `fmt`/serde/memory/concurrency all follow it), so nothing is mis-placed. Recorded, none
  blocking: (a) split/MCU-promote `Atomic` so lock-free cells need no pthread runtime (MCU track, §5/§6);
  (b) an idiomatic `std::gpu` kama module over the raw `kama_gpu.h` seam (engine track, §8); (c) confirm
  intrinsic `Array`/`List` vs library `DynamicArray` naming against GOALS "one way" (collections revisit, §5).
  Decided NOT to add a convenience-import of common containers — explicit per-symbol imports stay.
- **String interpolation `"${x}"` + formatting ✅** — shipped: the `Format` contract + `Formatter` sink +
  `toString<T>` (prelude), and `${expr}` holes (identifier + `.field`/`[index]`) lowering to a compile-time,
  statically-checked `Formatter` build (see [SPEC.md](SPEC.md) "Formatting & string interpolation"). The
  interp-in-operand papercut is closed (`93bde40`). Still open on this substrate, in DECIDED ORDER:
  1. **Format specifiers ✅ (M1–M3)** — `${expr:spec}` with a literal-analog vocabulary, grammar
     `[+|-]* [0? width] [.precision] | base`: precision `.N` on floats (`${pi:.2}`) and base `0x`/`0o`/`0b` on
     integers, where the leading `0` is echoed so `${n:x}`→`ff` and `${n:0x}`→`0xff` (a valid Kama literal);
     the letter's case controls digit case. A signed negative round-trips via width masking. **M2** adds a
     minimum field width — `${n:6}` (space-pad) / `${n:06}` (zero-pad), composing with precision on floats
     (`${pi:08.2}`) — for zero-padded columns (`${h:02}:${m:02}`). **M3** adds a `+` force-sign flag (`${n:+}`
     → `+42`) and a `-` left-align flag (`${n:-6}`). Applied via spec-aware `Formatter` fast-paths
     (`writeF64Prec`/`writeU64Radix`/`writeI64Width`/`writeU64Width`), so the `Format` contract is UNCHANGED; a
     spec on a user-type/kind-mismatched hole is a compile error. The hole AST carries a parallel `specs`
     vector (raw spec parsed at codegen — zero AST churn as the vocabulary grows). **Deferred (still parse at
     codegen, so additive):** combining a base marker with width/flags (`${n:08x}` zero-padded hex), a custom
     fill character (non-space/`0`, e.g. `*`), and center-align (`^`). Each currently errors with a clear
     "can't be combined / unsupported" diagnostic.
  2. **`@generate(Format)` ✅** — synthesizes a default field-dump `Format` impl (`Type { f1: v1, f2: v2 }`),
     mirroring `@generate(Serialize, Deserialize)`: a `genFormat` flag → a synth `format` method whose body
     (`emitFormatDefinition`) writes each non-`@skip`ped field into the caller's `Formatter` (scalars via the
     matching `writeX` fast-path, `char` via `writeChar`, a composite field through its own `__format`).
     Strings render **raw/unquoted** (uniform single-contract dispatch). Named after the CONTRACT (`Format`),
     NOT `display`/`debug` — kama has one to-string contract, no Display/Debug split. **Deferred (each a clean
     compile error today, additive later):** a field that doesn't `implements Format` (`Optional`/collection/
     enum-typed fields — they'd gain `Format` separately); `@generate(Format)` on a **generic**/**variant**/
     **enum** type itself; and, if a type ever needs BOTH a curated display and a structural dump, a
     `${x:?}`-routed `@generate(Debug)` (the spec hook already exists — purely additive, does not reopen the
     one-contract decision). **Per-derive `@skip(Derive…)` (deferred, additive):** today `@skip` is a single
     boolean shared by every derive (skip from serialization AND the Format dump). When a field needs to
     diverge — the killer case is *redaction* (persist `passwordHash` via Serialize but hide it from a `${acct}`
     log line), and the inverse (a cached/computed field: `@skip` Serialize but show in Format) — the sanctioned
     design is to parameterize it: `@skip(Format)` / `@skip(Serialize)` / `@skip(Serialize, Format)`, with bare
     `@skip` = all, using the same contract-name vocabulary as `@generate(...)`. `FieldInfo::serSkip` becomes a
     per-derive set; `@field(name:)` stays serde-only. Note the mandatory-marking interaction: `@skip(Format)`
     leaves the field IN serde, so it still needs an explicit `@field` (`@field @skip(Format)`). Build when a
     concrete need appears (YAGNI); bare `@skip` today is forward-compatible.
  3. **Tagged strings ✅** (`sql"…"`/`html"…"`/`stripIndent"…"`) — an identifier immediately before a string
     tags it; the compiler hands a tag function `fn R name(ref Template t)` the trusted literal parts and the
     rendered holes SEPARATELY (a `Template` value over borrowed arrays), so `html` escapes holes, `sql` binds
     them as out-of-band `?` params (injection-safe), `stripIndent` dedents the template only. Tags + `SqlQuery`
     live in `std::fmt`; `Template` is a prelude type. Specs compose inside a tag. Built additively on the
     `{parts, holes(+spec), tag}` AST. **Deferred (compatible future add-on, no current need):** *type-preserved
     params* — each hole keeping its static type into the params list (Model B, a per-tag hole contract) rather
     than the rendered `string` of the shipped Model A. `html` returning a distinct `SafeHtml` type is likewise
     future. Regex is a separate campaign.
  `string + <number>` stays a compile error by design — interpolation is the one way to mix values into text.
- **Full `expose` (2.0).** The minimal `expose fn` free-function C-ABI boundary ships today (see
  [SPEC.md](SPEC.md) + §5); the **full `expose`** — richer wasm module exports + the scripting host
  interface — stays **2.0** (§7).
- **`hardware` keyword** ✅ **SHIPPED (MCU step 2)** (renamed from C's `volatile`, which is no longer a
  keyword, to shed the threading-confusion legacy): emits C `volatile` for MMIO registers
  (`hardware Ptr<T>` → `volatile T*`, mirroring `const Ptr<T>`; composes with `const` → `const volatile T*`)
  and single-core ISR↔loop flags (`hardware` on a module `static` → `volatile T`/`volatile T*`).
  **Explicitly NOT a concurrency primitive** — cross-thread sharing is §6 atomics.
- **Opt-in `Equatable` derive (auto `==` for `value` types) — post-1.0 minor nicety.** Deferred into the
  construction-model campaign's broader derive story (`Equatable`/`Hashable`/`Copyable` as one consistent
  opt-in `@generate` surface, not three ad-hoc ones) — see `docs/design/construction-model.md` §8c. Kama today
  requires a hand-written `operator==` (auto structural `==` is a deliberate non-default); the derive would
  synthesize a memberwise `==` on request.
- **Force explicit field init (construction-model tightening — design campaign).** Make every ctor assign
  *every* field explicitly, with the compiler eliding redundant zero-stores — **except** types that opt into
  zero (`@generate(zero)` bags). Precedent: **Rust** (all fields required), **Swift** (definite
  initialization), **Zig** (all fields or a declared default), **C# structs**; C++ is the outlier (→ Core
  Guidelines ES.20 + clang-tidy compensate — the "always initialize" house rule many game engines already
  enforce). This *tightens* the existing keystone — `checkNamedCtorComplete` already forces owning +
  non-default-fillable fields — by removing the carve-outs. Two design questions to settle first: (1) are
  pointer-shaped fields (`Ptr`/`Owned`/collection) auto-exempt, or must they spell `= null` (Zig's
  spell-or-declare-default is the most uniform)? (2) is a bare `T x;` *outside* a ctor still allowed, or must
  every value come from a ctor (Rust/Swift: no bare uninitialized values)? Cost is a one-time stdlib sweep
  (~40 collection/allocator ctors gain explicit `len = 0` / `data = null` — exactly the *implicit* zero-init
  reliance this surfaces). **Why it matters here:** it is the language's proper answer to "drop only if live"
  — it subsumes the abandoned definite-construction-for-drop-safety attempt and, as a *side effect*, lets a
  raw-handle `resource` retire its `fd > 0` drop guard (a field must be explicitly assigned before it can be
  dropped, so no `{0}` handle ever reaches a dtor), while also catching plain uninitialized-field logic bugs.
  Relates to [construction-model](design/construction-model.md).
- **Raw-handle drop guard + the "can you own stdin?" question (fix after force-explicit-field-init).** Today
  `std::fs::File`'s dtor guards `if (fd > 0)` so a `{0}` (empty/uninitialized) `File` drops cleanly — but the
  drop-only-if-live A/B compiler work (git log) already stops the compiler dropping a `{0}` on the
  field-first-write and `match(give)` paths, so the guard now only defends the residual bare-local shapes
  (a `File f;` whole-reassigned or never assigned then dropped). Two open threads: (a) the guard is
  *empirically deletable* — the full ASan suite passes with it removed — so once force-explicit-field-init
  closes the residual it should go; (b) `fd > 0` (not `>= 0`) means a `File` **cannot own fd 0/1/2**
  (stdin/stdout/stderr) — the empty sentinel steals those. Decide whether owning a std stream in a `File` is
  even legitimate (likely use a distinct type / `Optional<File>` and never wrap fd 0), or adopt a `-1` empty
  niche (Rust `OwnedFd`) so `fd >= 0` is ownable. Same trap awaits every future raw-handle resource (sockets
  in `std::net`, GPU handles).
- **Enum-variant payload-type registration gap (bug, small).** A type used *only* as an enum variant's
  payload — where that variant is never constructed (the enum is exercised only via its other variants) —
  is not registered/emitted, so the enum's C `struct` references an undeclared type (`unknown type name
  'Shared_Probe'`). Reproduces with a plain `enum E { A, B(Shared<Probe>) }` constructed only via `A` —
  independent of Model C / `.as<>` (found alongside M5/P3). Fix: scan **every** variant's payload types at
  enum registration (like class fields via `scanTypeForCollections`), not lazily at construction.
- **Generic free-fn / static-method can't instantiate a generic type from its own type param (limitation, workaround).**
  A generic free function `fn f<W: C>(…) { Foo<W> x = Foo.make(…); … }` fails with "unknown type in constructor
  call `Foo`": the dot-on-type ctor resolver only rewrites the type name when it is itself a type *param*
  (`_genericTypeParams.count(tn)`), not when it is a generic *template* name — even though the free-fn body IS
  monomorphized with `_typeSubst[W]` bound. A **static** method on a generic type is likewise uncallable with
  explicit or inferred args (`Foo::<W>.m()` parses as a ctor; `Foo::<W>::m()` is a parse error; `Foo::m()`
  can't infer W). What **works**: constructing from the enclosing *type's* own param inside a type method (how
  `Map` does `MapKeyIter::<K>.make`), and **instance** methods on an instance the caller built at a concrete
  site. **Workaround (used by streams M2):** expose the op as an instance method —
  `JsonWriter<W> w = JsonWriter::<W>.make(sink: give s); w.encodeValue(v);` — rather than a free `encodeTo<W>`.
  Fix = teach the ctor resolver to substitute template type-args under `_typeSubst`, plus a static-generic call
  spelling. Post-1.0, additive; not a blocker (the instance-method form is clean). Surfaced during streams M2.
- **Custom-allocator default-convenience — the automatic never-null seal (construction-model, pre-1.0).**
  **✅ M8d.0 DONE:** the structural `when [A: default]` gate shipped — `empty()`/`withCapacity()` (and the PQ
  heaps, wrapper `Set`/`Sorted*` conveniences) simply *do not exist* for a non-default-fillable `A`, so
  `DynamicArray<T, BumpAllocator>.empty()` is a clean "not available for this instantiation — requires
  `when [A: default]`" compile error instead of an incomplete (zero-allocator) collection. Generic feature:
  a `default` bound in `whenConditionsHold` → `isDefaultFillable`, no nominal `Default` contract. This also
  kills the force-emit false-positive (a gated ctor is dropped from the monomorph, so it is neither emitted
  nor completeness-checked) and re-earns the protection `Map/Set::withCapacity` had as a `static fn`.
  **✅ M8d.1 DONE (the automatic seal):** the completeness gate `checkNamedCtorComplete` was INERT for every
  generic factory (a monomorph-name mismatch + a missing `give` unwrap); both fixed, so a generic ctor that
  leaves a non-default-fillable field unassigned is now a **hard compile error** (author-discipline →
  compiler-enforced). `isDefaultFillable` is now gate-accurate (scans the pruned `ci.methods`, not `ci.ctors`).
  The now-firing gate surfaced that the smart-pointer `adopt` relies on the same default-alloc pattern → gated
  `when [A: default]` (custom-`A` boxes use `adoptIn`); to keep `HeapOwner` conformance (heapOwnerTarget /
  owning-field detection) working, a contract-required `ctor` is now a compile-time guarantee, **not** a runtime
  vtbl slot. And the M8b-deferred **non-zero `default` fill** landed (a field whose `default` allocates is
  filled by calling its ctor, not zero-inited). **✅ CLOSED (M8d.2 + M8 Phase E):** the coexistence-era
  nameless primaries were removed (M8d.2) and the whole legacy construction surface is now a hard compile
  error (Phase E): a legacy class-named ctor DECL, and a nameless `Type(…)` / `new Type(…)` call on a
  named-ctor type (which silently dropped its args / left the object un-constructed), both reject and point at
  `Type.make(…)`. The vtable-only synth default ctor is gone (a polymorphic bare local sets its own `__vptr`).
  **✅ CLOSED (M8e — the final milestone):** the self-returning `static fn` reject (after migrating
  `deserialize` and `File.open` to ctors), the `E: Error` bound on `Result` (a boxed `Owned<Error>` satisfies
  it), and dropping `onConstruction` (the transitional serde exemption removed). Construction is now uniform
  and fully enforced — ready to tag 1.0.
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
- **Generic named-ctor type-arg spelling — ✅ RESOLVED (M8 Phase E Step 0): turbofish ON THE TYPE, uniform.**
  A generic named ctor's explicit type args always ride the TYPE via turbofish, in BOTH plain-call and `new`
  positions: `T::<Args>.make(…)` and `new T::<Args>.make(…)`. Chosen over bare type-position `T<Args>.make(…)`
  because keeping the `::` disambiguator makes it conflict-free in plain-expression position (a bare `T<Args>`
  is ambiguous with less-than), and the ctor's own turbofish slot stays free for a future ctor with its OWN
  generics (`T::<TypeArgs>.make::<CtorArgs>(…)`). The stdlib was swept `X.make::<A>(` → `X::<A>.make(`; the
  retired `X.make::<A>(…)` (type args on the ctor) is now a hard error redirecting to the on-type form.
  **Remaining (M8e doc pass):** reconcile the SPEC turbofish note (~1140) + the stale nameless-construction
  examples (`new T(…)` / `T(…)`, e.g. SPEC ~111/117/366/…) to the named-ctor surface — the SPEC construction
  section has been deferred to the design doc (`docs/design/construction-model.md`) throughout the campaign.

## 4. Reflection + serialization — remaining follow-ups (1.x)

Serialization ships today (by-value + object-graph + polymorphic contracts) with **two backends — `json` (text)
and `binary` (KBIN)** — see [SPEC.md](SPEC.md) "Serialization". What remains is additive library + hardening work:

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T,N>` read; a bare `encode`/`decode` of an
  intrinsic/enum value; generic enums. (A `const` field is a separate general language gap — doesn't parse today.)
- **✅ Binary backend (`std::serialization::binary`, KBIN) DONE** — a compact self-describing little-endian
  tagged format; a pure-library `Serializer`/`Deserializer` over the streams `Writer`/`Reader` substrate (zero
  compiler change), byte-oriented (`encode -> DynamicArray<uint8>`), full JSON parity incl. the object graph
  (shared/cycle/Owned/polymorphic) + forward-compat `skipValue`; triple-green. **Follow-on (deferred):**
  `@bits(n)` bit-packing (tighter integers/bools — a format add-on, in serde), field-name interning, and a
  schema-locked *positional* mode (needs an emitter change; trades forward-compat for max compactness). Delta/
  snapshot replication + reliable-vs-unreliable routing stay ENGINE-level (above serde); generic byte compression
  is an io-adapter layer (see §2 `std::io` transform adapters), not a serde concern.
- **More back ends (library, no compiler change)** — YAML; **XML**/**HTML**. Each is a `Serializer`/`Deserializer`
  impl + `encode`/`decode`. `std::encoding::base64` is a separate small module.
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
  - **Fallible allocation ✅ shipped (MCU step 5).** `allocate -> Optional<Ptr>` (`None` on OOM, never
    panics) rides the `Allocator` seam non-breakingly: `new`/collections unwrap-or-panic (prelude
    `unwrapPtr`) to keep pre-step-5 behavior, `try new -> Optional<Owned<T>>` is the non-panic construction
    entry, and direct `allocate` callers `match` on `None` (graceful arena exhaustion). Not MCU-only — the
    same seam is the game-engine frame-allocator / real-time-audio pattern.
- **Browser networking transports** — native TCP ships (`std::net`); the browser has no raw sockets, so the
  wasm path needs **WebRTC DataChannels** (unreliable) / **WebSockets** (reliable) via a host FFI shim (a
  real wasm nuance). Native UDP/DNS and the rest of the stdlib reach are the §1 follow-ups.
- **Embedded / MCU target** — globals/statics for ISR flags ✅ (step 1), `hardware`/`volatile` *emit* ✅ (step 2),
  `--target embedded` ✅ (step 3), `@interrupt`/`@section` ✅ (step 4), fallible `allocate` + `try new` +
  `@noheap`/`--no-heap` no-heap subset ✅ (step 5); avr/arm toolchains remain.
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
  | **Freestanding runtime** | ✅ **SHIPPED (step 3).** `--target embedded` compiles a value program to a **`-ffreestanding -nostdlib` object** (`.o`): the emitter emits a guarded entry (`#if KAMA_TARGET_EMBEDDED` → `int main(void){ kama_main(); for(;;){} }` — no `argc/argv`, never returns; hosted `main(argc,argv)` otherwise) and `KAMA_ISOLATE_LOCAL`/panic collapse to their freestanding forms. Triple-agnostic (pass `-target thumbv*-none-eabi` via `--cc`); the board link — crt0/startup + linker script — is the user's step (turnkey triples/scripts/HALs are the "Toolchain / build" row below). Arduino `setup()`/`loop()` is a later HAL nicety |
  | **No-heap / pluggable allocator** | ✅ **SHIPPED (step 5).** The `Allocator` seam is now **fallible** (`allocate -> Optional<Ptr>`, `None` on OOM — never panics); `new`/collections unwrap-or-panic (prelude `unwrapPtr`), **`try new -> Optional<Owned<T>>`** is the non-panic construction entry, and direct `allocate` callers `match` on `None` (graceful arena exhaustion). The **no-heap subset** is a per-region **`@noheap`** fn attribute + a whole-program **`--no-heap`** flag: every emitter-visible allocation (`new`/`try new`, `parallel_for`/`spawn` boxing, `Owned<Error>` boxing, interpolation) becomes a compile error via one gate. Target-independent — **the same seam serves embedded no-heap AND engine frame/arena pools + real-time audio** (`tests/alloc_frame_arena.kama` demos both). (Bring-your-own allocator per collection was already shipped, M10/M11.) |
  | **Globals / statics** | ✅ **SHIPPED (MCU step 1).** `static T name = const;` at module scope — deterministic zero/const init at reset, **per-isolate by construction** (`static KAMA_ISOLATE_LOCAL T name`: `_Thread_local` on native + wasm-pthreads which share one linear memory, plain `static` on a single-core `--target embedded`). v1 scope: **value / `Ptr` / `InlineArray`** only (owns nothing, no teardown seam) with **compile-time-const** initializers (omitted = zero-init); destructible-resource statics + runtime init are deferred (YAGNI — blink-LED needs ISR flags + handles + buffers, not heap). Race-free-by-construction, proven TSan-clean (a `static` cannot be seen by another isolate; sharing stays on the `Atomic<T>` seam). **`const` data in flash** (`.rodata`; **AVR** Harvard `PROGMEM`) is now covered by the **`@section(".x")`** placement attribute — ✅ **SHIPPED (step 4)**, on both statics and functions (AVR `PROGMEM` is `@section` + the AVR toolchain, later) |
  | **`hardware` qualifier** | ✅ **SHIPPED (step 2).** The renamed `volatile` (no longer a keyword) — `hardware Ptr<T>` → `volatile T*` for MMIO registers, `hardware` on a module `static` → `volatile T`/`volatile T*` for an ISR↔loop flag/handle, and `const hardware Ptr<T>` → `const volatile T*` for a read-only register; mirrors the shipped `const Ptr<T>` → `const T*`. **MMIO + single-core ISR only — NOT a concurrency primitive** (that's §6 atomics) |
  | **ISR declaration** | ✅ **SHIPPED (step 4).** `@interrupt expose fn void h()` → `__attribute__((interrupt, used))` — the Cortex-M / RISC-V / classic-ARM calling convention (enforced `void f(void)`; `expose` gives the vector table a bare symbol; `used` survives `--gc-sections`). Vendor `ISR(VECTOR)` (AVR) is the later `@interrupt("VECTOR")` step. Paired with **`@section(".x")`** placement (statics + functions → `__attribute__((section(".x")))`) for the vector table / flash / DMA RAM |
  | **Toolchain / build** | target triples (`thumbv*-none-eabi`, `avr`, …), linker scripts (`-T`), startup objects, MCU flags, and linking the vendor HAL (pico-sdk / Arduino core / esp-idf) + flashing |
  | **Panic/trap handler** | ✅ **SHIPPED (step 3).** Under `KAMA_TARGET_EMBEDDED`, bounds/panic/OOM funnel through one **overridable weak `kama_panic_handler`** (default `for(;;) __builtin_trap()`); a firmware author provides a strong symbol to blink / reset / breakpoint. (Numeric traps were already runtime-free via `__builtin_trap`.) |
  | **Inline assembly** | ✅ **SHIPPED (step 6a).** `asm("wfi");` inside `unsafe { }` → `__asm__ __volatile__("…" : : : "memory")` (always volatile + a full compiler memory barrier, so `cpsid i`/`dsb`/`dmb` order memory correctly by default). One string operand, no interpolation; `\n`-separated for multiple instructions. Native/embedded only (wasm has no register-level inline asm; the runner SKIPs it). Curated named helpers (`wfi()`, `disable_interrupts()`) are a thin follow-on **library** over this primitive; extended-asm operands + `@naked` are deferred. |

  **Why kama fits well:** no-GC + RAII → deterministic, no hidden pauses; allocation is explicit in the
  emitted C (greppable no-heap audit); trap lowering already dependency-free; `InlineArray<T,N>`, sized ints, and
  `unsafe`/`Ptr` FFI already exist. **North star: blink an LED** (the embedded "first triangle") — forces
  exactly the critical path and nothing else. **Start Cortex-M, not AVR** (`zig cc`/clang do `thumbv*-none-eabi`
  cleanly; pico-sdk is tidy; AVR's Harvard/`PROGMEM`/`avr-gcc`-only pain comes later).

### Compile-time evaluation & platform-specific compilation (const-eval campaign — its own milestone)

Motivated by both MCU (baud divisors, gamma/trig/CRC tables, `.rodata` layout) and the engine (lookup
tables, shader/permutation specialization) — see [MCU_READINESS.md](MCU_READINESS.md) and
[ENGINE_READINESS.md](ENGINE_READINESS.md) const-eval rows. A ladder, landed incrementally:

- **6b-1 — arithmetic on const-generic params** ✅ **SHIPPED.** `constValue()` folds
  binary/unary/cast trees, grammar accepts `InlineArray<T, (N+1)>` (parenthesized const size), and a
  post-discovery pass (`registerInstColls`) registers const-param-derived collection sizes before the
  collection typedefs emit. Fixture `const_generic_arith`.
- **6b-2 — named compile-time constants (`comptime`)** ✅ **SHIPPED.** A named constant at three scopes,
  one keyword: **local/block** (`comptime T N = <expr>;`, explicit — errors at the decl if it can't fold;
  a plain `const` local still folds opportunistically, C++ `const`/`constexpr`-style), **module**
  (`comptime T NAME = <expr>;`), and **type** (`comptime T NAME` read `Type::NAME`, member-visibility-
  controlled). Three orthogonal axes settled: `const` = runtime-immutable, `static` = runtime-associated,
  `comptime` = compile-time. Values fold via `constValue` (registries `_constLocalVals` / `_moduleConsts`
  / `_typeConsts`, gathered before the collection pre-pass); lowers to a real `static const` symbol
  (addressable / `@section`-able) with **cross-constant references baked to literals** → no C static-init-
  order dependency (the fiasco cannot occur; forward/cyclic refs are a clean error). Fixtures
  `comptime_local` / `comptime_module` / `comptime_type` (+ `const_local_size` for the opportunistic path).
- **6b-3 — compile-time function evaluation (`comptime fn`)** ✅ **SHIPPED.** A bounded AST interpreter
  (`kama.comptime.cpp` — a sibling of the emitter that produces *values*, not C text) RUNS a `comptime fn`
  at compile time and bakes its result into a `static const` scalar or **table** (`InlineArray_T_N X =
  {.v={…}}`) — zero runtime cost, sits in `.rodata`/flash. Subset: integer (int64 lane + width/sign, so
  narrow-int wrap happens on cast/store exactly as the emitted C) / float / bool / char + fixed
  `InlineArray<T,N>`; locals, `if`/`while`/`do`/`for`/`foreach`, `=` assignment, array-element writes
  `t[i]=…`, ternary, and calls to other comptime fns. **Both forms**: top-level `comptime fn` and
  **type-associated** `comptime fn` (read `Type::name()`, member-visibility-controlled — default private,
  a private one callable only from within its own type's comptime fns). **Comptime-only** (decided, not
  dual-use): a comptime fn is never emitted as C; a runtime-position call is a clean error pointing at the
  `comptime` constant form (relaxing to dual-use later is backward-compatible). Two safety rails: **purity**
  is structural (any node the interpreter has no eval case for — `new`/`spawn`/`unsafe`/`asm`/FFI/strings/
  pointers/mutable-static reads — is a clean "unsupported in comptime fn" diagnostic, the C++ constexpr
  model) and a **step budget** (1e6) + call-depth cap so a runaway can't hang the compiler. Evaluated in a
  post-collection `evalComptimeConsts` pass (before `registerInstColls`, so a baked scalar can size a
  const-generic array). Flagship fixture `comptime_fn_crc` (a 256-entry CRC-8 LUT baked at compile time,
  asserted bit-identical to a runtime recompute) + `tools/check-comptime.sh` transpile-grep; `comptime_fn_scalar`,
  `comptime_fn_type_assoc`; negatives `xfail/comptime_fn_{runtime_call,impure,budget,visibility}`.
  Deferred (nice-to-haves, not blocking): `sizeof`/`alignof` and named-arg reorder *inside* a comptime fn
  body; a **local** `comptime T X = f();` initialized by a comptime-fn call (module + type-associated const
  forms ship); a per-fn `@steps(…)` budget override; and dual-use fallback emission.
- **Known rough edge (backlog):** `foreach` over a `static const` fixed array (a `comptime` array constant)
  emits a C `const`-discard warning — the foreach lowering takes a non-`const` receiver pointer
  (`kama.cemit.cpp` ~2570) and the by-value + `ref` paths share it, so a blanket `const` would break
  `ref`-foreach write-back. Benign (the loop only reads) and index access (`X.get(index: i)`) is warning-free;
  fix is a const-correct foreach lowering (const receiver pointer + `const`-element `get` on the by-value path).
- **Platform-specific compilation** — the no-`#ifdef` answer, and **decided in direction: tag TYPES to
  force an abstraction boundary, do NOT add in-function branching.** A platform-agnostic `contract`
  defines the seam; per-platform concrete types implement it and carry a **`@target(...)`-style tag**;
  the toolchain selects the tagged implementation for the active target. There is deliberately **no
  `static if (arch == ...)`** and no scattered branching — that ifdef/`comptime-if` soup is explicitly
  rejected as ugly, and forcing the impl behind a type/interface keeps the boundary clean. This is
  expected to be **simpler** than a branching model (a decl-level tag + selection, not an evaluator).
  Const-eval supplies compile-time *values*; type-tagging supplies the *structure*. Inline asm (6a) is
  the raw primitive under those per-platform types. (wasm cannot do inline asm at all — another reason
  the interface seam matters.) **► Design of record: [docs/design/conditional-compilation.md](design/conditional-compilation.md)**
  — prepared 2026-07-24 (the campaign after const-eval). Key realization there: build-mode (`DEBUG`/`RELEASE`)
  and platform are **one primitive** — a decl-level keep/drop gate (`@when(FLAG)`); "platform" is that gate on
  contract impls (the tag-type seam), not a second mechanism. One attribute + one prune pass; no `#ifdef`
  reaches the emitted C.

## 6. Concurrency — shared-nothing by construction (✅ SHIPPED — campaign complete 2026-07-23)

**► Spec + implementation: [docs/design/concurrency.md](design/concurrency.md)** — the M0 design-refinement pass
converged into a **spec that is now fully implemented** (surface + lowering + `kama_isolate_*` runtime ABI +
structural sendability + the per-isolate-`static` rule); milestones **M1–M6 all landed** (see the Progress line
below and the doc's per-milestone "landed" notes). Decisions settled there: **execution = isolates +
data-parallel jobs** (no language-level green threads / `async`; a native fiber scheduler for high-connection
servers is a *library* on the primitives, since servers are native-only); **module `static` is per-isolate by
construction** (unifies with the MCU Tier-0 statics blocker — §5 / [MCU_READINESS.md](MCU_READINESS.md)); the
job system + event-loop scheduler are libraries, not language. Start there.

**► Progress (2026-07-23):** M1 (`std::time`), M2 (native isolate seam — spawn + moved bundle + join),
**M3 (channels — bounded + rendezvous, blocking send/recv, structural sendability gate)**,
**M4 (structured concurrency — `scope { }` + join-before-drop barrier + `ref`-borrow with escape &
same-root-disjointness checks; the spawn verb is now `spawn`, bare `spawn` is scope-only)**, and
**M5 (wasm parity — emscripten pthreads + `-sPROXY_TO_PTHREAD`; the seam headers compiled unchanged, all 15
`std::concurrent` fixtures now pass on the wasm leg against the same `.expect` as native)**,
**M6.1 (`Atomic<T>` — the sanctioned cross-isolate shared-MUTABLE cell; width-generic `__atomic_*` seam,
integer/`Ptr` element, C11 `_explicit` ordering)**, and **M6.2 (immutable-`Shared` cross-isolate reads — the
`immutable` type qualifier + `computeDeeplyImmutable` fixpoint; a `Shared<immutable T>` is sendable and uses a
two-flavor atomic refcount selected per-instance, ordinary `Rc` unchanged)**, and **M6.3 (disjoint-slice
`parallel_for` — splits a `View<T>`/contiguous container into K non-overlapping sub-Views, one per worker
isolate, joined at a self-joining barrier; safe by disjointness; the body is hoisted into a worker fn with
captures threaded in as `ref` params, non-atomic captured writes rejected; K = hw cores, `KAMA_PARFOR_WORKERS`
override)** have ALL landed (native TSan- + ASan-proven; concurrency green on native **and** wasm) — **the
concurrency campaign is complete**. See the design doc's per-milestone "landed" notes.

**► NEXT ACTION — the MCU/embedded campaign is CONFIRMED (re-triage concluded 2026-07-23).** The post-concurrency
re-triage across the three READINESS docs — [MCU_READINESS.md](MCU_READINESS.md) ·
[ENGINE_READINESS.md](ENGINE_READINESS.md) · [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md) — ran and
**MCU/embedded (§5) won**, then the last 1.0 language residual was cleared first (nested/ternary +
contract-dispatched `match` subjects, §1 — closed 2026-07-23). Why MCU wins the triage:
- It **was** the **only** track with real **language-surface** work queued: module-level statics ✅, the
  `hardware` (MMIO/`volatile`) qualifier ✅, ISR-entry binding ✅, a freestanding `--target embedded`
  runtime ✅, fallible `allocate -> Optional<Ptr>` + `try new` + the `@noheap`/`--no-heap` no-heap subset ✅
  (steps 1–5 all shipped 2026-07-23; only avr/arm toolchain packaging + HAL remain, which are
  build/library, not language). Engine and Web are now **library/platform** work with **no language blocker**
  (Engine: WebGPU/`std::gpu` + the job-system *library* on the shipped `parallel_for`; Web: the event-loop
  *scheduler library* over `Poller` on the shipped isolate/channel primitives + `std::time`).
- Its **#1 blocker builds onto settled, shipped ground**: module statics lower to the concurrency model's
  **per-isolate-`static`** rule (plain C `static` single-core / `_Thread_local` multicore native / automatic
  wasm), and that model is now implemented, not just designed — lowest-risk of the three.

**First concrete step (the campaign starts here):** module-level statics with deterministic zero/const init,
built to the per-isolate rule (§5 "Globals / statics" row + [MCU_READINESS.md](MCU_READINESS.md) Tier 0 /
Recommended sequence step 1). Then the `hardware` qualifier, then `--target embedded`. Recommended v1 scoping
(YAGNI): allow `value`/`Ptr`/`InlineArray` statics only — defer destructible-`resource` statics, whose at-exit /
thread-exit teardown hook is a genuinely new seam not needed for the blink-LED north star.

The concurrency model (now **shipped** — the description below is the design record it was built to). It earns
data-race freedom the way kama earns null-safety — by making the hazard
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
