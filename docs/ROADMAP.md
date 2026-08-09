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
> | A campaign, while it is still in flight | its `docs/design/*.md` — **deleted when the work ships**, once the rows above carry its record |
> | A capability against a target domain | [MCU_READINESS.md](MCU_READINESS.md) · [ENGINE_READINESS.md](ENGINE_READINESS.md) · [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md) |
> | User-facing behavior + workflow | [packages.md](packages.md) · [editors.md](editors.md) · [mcu.md](mcu.md) · [targets.md](targets.md) |
> | *Why* a thing happened, and when | the git log — do not re-tell it here |
>
> What may stay behind is **at most a one-line pointer**, and only where a forward item depends on it.
> A **residual** of shipped work (a gap, a follow-on, a deferred optimization) stays — as its own forward
> item, stated as what is left to do, not as a recap of what was done.

## The shape

- **1.0 — language complete.** The language surface is stable and the pre-1.0 work list is closed; **the
  last gate is the docs/naming reconcile** (§1) — you build *with* the language, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: deeper stdlib reach, more serde
  back ends, MCU toolchain packaging, engine/GPU library work. Mostly library + codegen, little new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via a shared IR
  feeding C, direct-wasm, and a bytecode VM — the `kama` binary self-contained (§7).
- **Concurrency** — the language primitives are done ([SPEC.md](SPEC.md#concurrency-)); what is left is
  libraries on top of them — the job system and the event-loop scheduler (§8, web-framework readiness).
- **Engine track** (product north star): a portable lightweight **WebGPU** game engine, woven through 1.x — a
  product built *on* kama, not part of the language (§8).

## 1. Remaining before 1.0

What the language *is* lives in [SPEC.md](SPEC.md); the engine/MCU capability matrices in
[ENGINE_READINESS.md](ENGINE_READINESS.md) / [MCU_READINESS.md](MCU_READINESS.md); the history in the git log.

**The language surface is feature-complete.** What is left before the tag is the
docs/naming reconcile — 1.0 is the API-stability point, so naming and case conventions fix there
(PascalCase types, lowerCamel methods, no `I`-prefix on contracts, lowercase `string`), and anything that
would *break* source has to land first or wait for 2.0.

**At the tag itself — repoint the Zed grammar pin.** `editor/zed/extension.toml` pins a *commit*, and Zed
installs the grammar by fetching that rev — so the pin, not the working tree, is what Zed users get. It is
currently behind (the commit predates `slot` and named match patterns, so neither highlights for them).
Bump `rev` to the release tag when 1.0.0 is cut, and add the guard that cannot exist while it is a moving
SHA: assert the tag's `tree-sitter-kama/grammar.js` + `queries/` match the tree. Doing it at the tag is what
dissolves the chicken-and-egg — a content check against a *commit* pin would fail the very commit that
changes the grammar. `tools/check-editors.sh` §2c today proves only that the rev resolves and carries a
grammar, never that it is the current one.

**Mandatory braces on every branch and loop body** — `if`/`else`/`while`/`for`/`foreach` would require
`{ }`, never a bare statement. This is the "easy to use correctly, hard to use incorrectly" argument, and
it is stronger in kama than in most languages precisely because there is no whitespace rule to fall back
on: a bare branch body is where `goto fail;`-shaped bugs live, and where a later edit silently attaches a
second statement to nothing. It is a **breaking source change**, so it lands before the tag or waits for
2.0. Design questions worth settling first: whether `else if` chains stay exempt (they are the one case
where the brace adds nothing and nests everything), and whether a single-line `if (x) { return; }` is
still allowed on one line — the rule should be about the braces, not about line breaks. Pairs naturally
with the formatter below, which can insert them mechanically for the migration.

Everything else here is library or toolchain work that does **not** gate the tag:

1. **`std::process` — async/Poller-driven *live* child-stream reads.** `run()` captures a finished child's
   output today; streaming a running child's stdout as it arrives is the piece left.
2. **The CONTRACT MODEL — four sequenced campaigns**, briefed in
   [design/contract-model.md](design/contract-model.md) (**read its *Status* section first** — campaign 1
   is part-built and several of its design points were revised once the code existed; delete the file when
   the last campaign ships). A design review during M2a found that **`enum` and `intrinsic` are hidden kinds**
   — GOALS #3c says every declaration is `type <kind> Name`, yet `enum` had its own grammar production
   with no `class_base_opt` and `intrinsic` had no kama spelling at all. Retro-impl exists only to paper
   over those two gaps; giving them spellings **retires four pieces of machinery** rather than fencing
   one. Run in order, each its own session, **before M2b**:
   1. **Contract model** — *both spellings now ship* (`type enum X implements C`, `type intrinsic <…>
      implements C`), the conformance registry is keyed by kama type rather than cType (closing the
      `char`/`uint32` collision), a duplicate claim names both packages, and **M4 has shipped**: the
      prelude and lib declare every one of their 90 primitive conformances, so `implements C for T`
      survives at three sites in `tests/` and nowhere else. **Three of the four pieces of machinery are
      gone** — retro-impl on enums, the enum tagged-union promotion, and the `string`-`Equatable` nominal
      special case. **M5 has shipped in full** (2026-08-08): a contract declares the self-type as a
      **pinned type parameter** (`type contract Comparable<T is This>`) rather than naming `This` in a
      signature — which removed an unsound vtbl cast every program in the tree was emitting; a primitive
      can be **widened** to a contract value, as a borrow or an owning box; and a contract-supplied method
      is reached **through the contract**, so a primitive does not absorb an API it does not own. What is
      LEFT: **M6** — delete retro-impl (its only consumers are `tests/impl_retro_user.kama` and
      `tests/xfail/impl_conflict.kama` ×2, which exist to keep the path exercised until then), the renames
      it unblocks, and the SPEC closeout. Per-milestone detail in the brief.
   2. **Full generic specialization** — universal, concrete-args-only (so any two are identical or
      disjoint; no specificity lattice). Polymorphism for generic *functions*.
   3. **Const generics on types** — `constParams` is parsed but never read, and a const param cannot be
      a runtime value (silent bad C today). Unblocks `Fixed16_16` → `Fixed<intBits, fracBits>`.
   4. **Derived view-escape check** — reject a `view` implementing a contract it cannot satisfy.

3. **Standard-library follow-ups — the M2 PARITY CAMPAIGN**, briefed in
   [design/stdlib-parity.md](design/stdlib-parity.md) (**M2a shipped**; delete that file when M2c ships).
   The bar is **Rust-`std` parity**: the only no-GC peer, and the only one whose stdlib also stops before
   regex/TLS/HTTP/crypto — which is the right line now that kama has a package manager. No new language
   surface; pure library/codegen. Split M2a (parse · sort · math completion · `char` classification) /
   M2b (fs + path · io handles + `lines()` · sleep + wall clock · DNS) / M2c (`std::random` ·
   `std::encoding`). The items below are that campaign's contents:
   - **`std::net`** — DNS/`getaddrinfo` (numeric hosts only today). *(UDP and ephemeral-port `getsockname`
     ship — `lib/std/net/udp.kama`; IPv6 and multicast are separate, tracked in §2.)*
   - **`std::fs` / `std::io`** — richer `Metadata` (mtime/perms), path helpers, `mkdir`/`rename`/`exists`,
     `OpenMode.Append`, stdin/stdout/stderr as `Reader`/`Writer` handles, `readLine`/`lines()`.
     *(Buffered readers/writers ship — `BufReader`/`BufWriter` in `lib/std/io/streams.kama`.)*
   - **`std::io` transform adapters (compression et al.)** — `Writer`/`Reader` *wrappers* that transform bytes
     in flight, composing with serde and net (Go/Rust `io`-wrapper style): `DeflateWriter<W>`/`InflateReader<R>`
     (gzip/deflate), later checksums/hashing/framing. On the **web target** these are a near-free ride — wrap
     the browser's built-in `CompressionStream`/`DecompressionStream` (no wasm code-size cost); on native, wrap
     zlib/zstd. Composes as `encodeTo(v, into: DeflateWriter(sink))`. The *transport* free-rides too
     (WebSocket `permessage-deflate`, HTTP `Content-Encoding`). Pairs naturally with the binary serde backend
     (crushes its field-name redundancy). (Engine-level replication — snapshots/deltas/dirty-tracking — stays
     above this, in the engine.)
   - **⚠️ Windows CI is RED, and must be green + required before the tag.** The `windows-latest` leg last
     reported **779 passed, 48 failed** (2026-08-01, MSYS2/UCRT64). It is still best-effort, so those
     failures do not block a merge — which is how a red leg stayed invisible. Two pieces:
     - **Triage the 48.** Only one was captured in the log tail, and it was a *harness* bug, not a compiler
       one: `check-target` asserted the `--shared` output name by grepping the stubbed `--cc echo` command
       line, which a natively-built Windows kama emits through `cmd.exe` — whose `echo` keeps the quotes
       that POSIX `sh` strips, so `-o "…/arith.dll"` failed an anchored `arith\.dll$` match while the build
       itself was correct. **Fixed** (`tr -d '"'` in `tools/check-target.sh`). The other 47 need the full
       CI log — the excerpt on hand stops after the first failing guard.
     - **The Windows suite takes 1496 s vs ~75 s in the container**, and every `net_*` fixture costs ~30 s
       (`net_refused` 30.7 s, `net_nonblocking_connect` 30.2 s). That is the shape of a connect/accept
       timeout being waited out rather than a test running, so it is likely one root cause across a dozen
       fixtures, not a dozen bugs.
     Promote to **required** once green, so a Windows regression blocks a merge.
4. **MCU toolchain packaging — polish.** The turnkey Cortex-M path ships and is QEMU-proven
   ([mcu.md](mcu.md)). What is left: more board presets (STM32/Pico), vendor-HAL glue, and a real-hardware
   flash pass — detail in §5 (embedded "Toolchain / build" row).

**Post-1.0 — the decided big-arc sequence (with the user, 2026-07-26):**
1. **Editor tooling (§10).** The front end is a reusable query API with real source spans, which every
   later tool rides on; the residuals are in §10.
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
language-completeness residual is **closed**; what remains here is genuinely later-track or opt-in.

- **`Fixed16_16` does not implement `Real`.** A contract requires *every* method, so conformance means
  writing 21 fixed-point functions including `sin`/`cos`/`atan2`/`exp`/`log`/`cbrt` in Q16.16 — CORDIC and
  polynomial-approximation work, a numerical-methods project rather than a library chore. It is the
  obvious first customer of the exported `Real` contract (`lib/std/math/scalar.kama`), and of a future
  generic `Fixed<intBits, fracBits>` (§1).

- **`fnptr` cannot take type parameters** — the only declaration form in kama that cannot
  (`type value X<T>`, `type contract C<T>`, `enum Result<T,E>` and `fn f<T>` all can). So a generic
  callback signature has no name: `fnptr Ordering Compare<T>(ref T a, ref T b);` does not parse
  ([kama.y](../kama.y), the `FNPTR` rule has no type-param slot). **Deliberately deferred, not overlooked**
  — for the case it would serve, a generic **contract** is the better tool anyway: it monomorphizes to a
  direct inlinable call where an `fnptr` is an indirect one, and a comparator object can carry state,
  which matters because kama has no capturing closures. `std::collections`' `Order<T>` is the worked
  example. Additive and non-breaking, so it costs nothing to wait. **Revisit alongside the full
  specialization campaign** (§1) — a concrete-args specialization covers the per-type-body case a generic
  `fnptr` would otherwise be reached for.

- **`--no-heap` does not gate container allocation.** The flag rejects `new`, string interpolation,
  `spawn`, `parallel_for` and error boxing (`rejectIfNoHeap`), but a `DynamicArray`/`Map`/`string` growing
  through `GlobalAllocator` reaches `malloc` unchallenged — so the flag under-delivers on what its name
  promises. M2a worked around this for the one case it introduced (`sort`/`sortWith` are
  `@compileFor(!NOHEAP)`, so they vanish from a no-heap build), but that is a spot fix, not the rule.
  The real change is to make `GlobalAllocator` growth an error under the flag, leaving containers usable
  only with an explicit arena/pool allocator — which is the MCU story anyway. Wide blast radius (every
  container use in a no-heap build), so it is its own campaign.

- **The unsafe seam — `Ptr<T>` -> `UnsafePtr<T>`, and no `null` in safe kama.** Its own campaign, agreed
  while the `slot` work was in flight (which is where its customers came from: eight buffer-realloc sites
  now carry `= null` field initializers). Shape: rename the raw pointer to say what it is; an
  `unsafe UnsafePtr<T> p = null;` field-declaration modifier; `Optional`-returning FFI wrappers;
  `unsafe { }` around the teardown guards; then ban the `null` token outside `unsafe`. Plus a
  compiler-emitted debug null trap at the two `_inUnsafe` deref gates. `lib/std/ptr/` is its natural home.
  **Source-breaking**, so before the tag or 2.0.

- **A lib/prelude diagnostic is attributed to the file being checked.** `kama check app.kama` reports an
  error raised inside `lib/std/…` or the prelude as `app.kama:<the LIB file's line>` — the line number is
  right for the wrong file, so it points into the middle of the user's source or past its end. Harmless for
  a single error a human reads in context, and actively misleading in an editor or any sweep over the
  corpus (enumerating rule violations across `tests/` had to filter by "is the reported line past this
  file's end?" to tell the two apart). Wants the unit to travel with the diagnostic, as `RefUnitScope`
  already does for the reference index.
- **Contract refinement — one under-tested edge (clean workaround).** `type contract Child … implements
  Parent` works for dispatch, but was exercised mainly with scalar-param parents. Remaining: a concrete type
  implementing the child gets **no parent-contract conformance thunk** — pass it where the parent is expected
  only if it *also* spells `implements Parent` — and a child-contract-**value** → parent-contract-param upcast
  is unsupported (dispatch *through* the child to inherited methods works). Trivial workaround, used in
  `lib/std/net/stream.kama`; the fix is to auto-emit parent thunks for refining-contract implementers.
  *(The generic-instance param edge is fixed — an inherited slot's signature is now rebound to its
  parent-resolved absolute spelling in `linkContracts`; fixture `tests/contract_refine_generic.d`.)*
- **Unicode module (post-1.0).** The shipped `string` core is UTF-8 bytes + `.chars()` codepoints with
  **ASCII** casing/whitespace; a later module adds Unicode-correct casing + whitespace, and an eager
  `DynamicArray<string>` collect for `split` (the lazy `Split` iterator ships today).
  **Grapheme-cluster segmentation belongs here too.** `substring` traps on a split *codepoint* and
  `floorCharBoundary`/`truncate` snap to one (SPEC § *Strings*), which guarantees valid UTF-8 but **not**
  visually intact text — a boundary cut can still split an `e` + combining accent, an emoji ZWJ sequence
  or a flag. Cluster boundaries are defined by UAX #29 and need the `Grapheme_Cluster_Break` property per
  codepoint, i.e. a data table — not a bit trick. A cheap partial version (range-checking the combining
  diacriticals) would be wrong for emoji, flags, Hangul and Indic while *looking* like a guarantee, so it
  is deliberately not shipped. Same stance as Zig, and utf8everywhere points at ICU for it; baking the
  tables into `std` would also contradict targeting MCUs under `--no-heap`.
- **Stdlib layering — 3 LOW-prio follow-ups.** The prelude-vs-`std::`-vs-primitive split is principled and
  documented in [FLOOR.md](FLOOR.md) § "What is floor, and what is an `import`"; nothing is mis-placed. What
  is left, none of it blocking:
  - **`Atomic` needs no pthread but rides in `std::concurrent`**, which does — so lock-free-without-threads
    is unreachable. Split it to a `std::concurrent::atomic` leaf, or promote it as an always-available seam.
    Only matters once the multicore-MCU track starts (§5).
  - **`std::gpu` has no kama module.** The `kama_gpu.h` seam exists but programs `extern` the WebGPU C API
    raw; an idiomatic wrapper is library work on the engine track (§8).

  Decided NOT to add a convenience-import of the common containers — explicit per-symbol imports stay.
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
- **Derive follow-ons.** `@generate(Equatable, Hashable)` ships for plain types (SPEC § *Derives*). Still
  open, additive: the same derives on a **generic** or **variant** type (the same v1 boundary
  `@generate(Format)` draws — both error rather than half-deriving), and on a payload-less **enum**, which
  has no struct to walk and today takes a retroactive `implements` instead. A `Copyable` derive is a
  **non-goal**: a value/view copies by kind, and a resource's `copy` ctor is an ownership decision no field
  walk can make (a memberwise copy of a raw handle double-frees).
- **Unresolved type names — one residual: GENERIC ARGUMENTS.** Declared type names are now checked
  (`checkDeclaredTypes`, a single-visit walk at the tail of `collectProgram`), so a misspelled or unimported
  type in a parameter, return, field or variant payload is a kama-level error instead of a C-level
  `undeclared identifier` against generated code. What is still unchecked is a type name *inside* a generic
  argument — `DynamicArray<Bogos>` — because `checkTypeResolves` early-returns on `type->genericArg`.
  Recursing into `genericArgs` (as `addTypeRef` does in kama.query.cpp) is the natural phase 2, but it
  widens the surface onto const-generic size expressions, defaulted allocator args and bounds, so it wants
  its own sweep. Guarded today by `tests/xfail/unknown_type_{local,param,return,field,method_param,
  variant_payload}` + `unimported_type_param`, and by `tests/decl_type_check_guards.kama` for the three
  shapes the pass must NOT reject (a generic free fn's own params, `This`, a `sig` used before its file).
- **`kama check <file>` on a single member of a DIRECTORY module reports false errors.** Sibling units in the
  same namespace are not loaded for a bare single-file check, so `kama check lib/std/math/quat.kama` reports
  `Vec3`/`Mat4` as unknown — they live in `vec.kama`/`mat.kama` under the same `namespace std::math`. Harmless
  for the language server (it resolves the whole program from the manifest) and for any file reached through
  an import, but it makes single-file `check` unusable as a lint over a directory module, which is how the
  stdlib is laid out. Fix = widen a bare `check`'s unit set to the target's own namespace directory.
- **`std::net` — IPv6 and UDP multicast.** `IpAddr` has a `V4` arm only ([`lib/std/net/addr.kama`]), left
  deliberately as an `enum` so a `V6(...)` arm adds without reshaping `SocketAddr` or any call site.
  Multicast join/leave (`IP_ADD_MEMBERSHIP`) is likewise unbuilt — broadcast covers LAN discovery today.
  Both are ordinary socket-option work on the shipped seam.
- **A value-producing `match` over an `enum X : IntType` does not compile (small, self-contained).** The
  explicit underlying type makes the tag a plain `uint8_t`/`int16_t`/… rather than a C `enum`, so the C
  compiler cannot prove the emitted `switch` exhaustive and rejects the uninitialized match temp
  (`kama_string __match1;` — `emitValueMatch`, [kama.cemit.cpp](../kama.cemit.cpp), the temp declared just
  before `emitMatchSwitch`). Reproduces on a bare `enum Color : uint8 { Red, Green, Blue }` with no
  contract and no `type` marker; a statement-form `match` is unaffected, and so is the same enum without
  the `: IntType`. Fix = emit a `default:` arm for a plain-integer tag (kama has already checked
  exhaustiveness), rather than zero-initializing every match temp — which would cost every match in every
  program. Found while building the contract-model campaign's enum fixtures; `tests/enum_implements.kama`
  works around it with a statement match and says so.
- **Fallible `new` is concrete-only.** `try new` / `new(allocator:)` support concrete `Owned`/`Shared`;
  the type-erased interface-element handle (`Owned<Contract>`) and the stateful-allocator form report "not
  yet supported" (`emitFallibleNewBox`). A follow-on to the MCU step-5 allocator work.
- **Windows long-path support is deferred** (`kama_os.h`): the temp-path builder assumes `MAX_PATH`-class
  lengths. Surfaces only on a deep working directory.
- **UBSan's `function` check is disabled suite-wide** (`run_tests.sh`). Vtable / contract /
  `BindableFunctionPtr` dispatch stores each slot as `Ret (*)(void* self, …)` and calls a concrete
  `Ret C__m(C* self, …)` through it — ABI-identical, and how essentially all C OO dispatch works, but the
  check enforces exact function-pointer identity. Either emit a matching-signature trampoline or document
  the exemption as permanent; silently off is the wrong end state for 1.0.
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
*(The `Slot<T>`/`MaybeUninit` spike that sat here is answered and shipped: the shape is a `slot`
DECLARATION, not a wrapper type — no new type, no `.assume_init()`, and an unassigned slot simply has no
drop emitted. See SPEC § *Uninitialized storage*.)*

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
networking). The MCU/embedded language surface and the const-eval ladder are done ([SPEC.md](SPEC.md),
[MCU_READINESS.md](MCU_READINESS.md)). Remaining forward work:

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

### Embedded / bare-metal MCU (Pi Pico · Arduino · ESP32) — toolchain packaging

The language surface is done ([MCU_READINESS.md](MCU_READINESS.md)); what remains is build and library work,
across two different targets:
- **Raspberry Pi (Linux — Pi 3/4/5, Zero):** a full ARM app processor running Linux, which kama already
  cross-compiles to. What is left is **GPIO/I²C/SPI bindings** — ordinary C FFI over `libgpiod` / `/dev/mem`.
- **Bare-metal MCU (Cortex-M: Pi Pico/RP2040 · Arduino Zero/Nano 33, ESP32; later AVR):** *freestanding* — no OS,
  KB of RAM, often no heap, a startup file + linker script instead of hosted libc:

  | Piece | What's needed |
  |---|---|
  | **Toolchain / build** | The turnkey Cortex-M path ships and is QEMU-proven ([mcu.md](mcu.md)). **Remaining:** more board presets (STM32/Pico), vendor-HAL glue (pico-sdk / esp-idf), a real-hardware flash pass, and (optional) folding the two-step link into `kama build --target <board>`. Arduino `setup()`/`loop()` is a later HAL nicety. |
  | **AVR (Harvard) family** *(deferred — Cortex-M/RISC-V first)* | Three AVR-specific pieces: (1) ISR — `@interrupt("VECTOR")` → the `ISR(VECTOR)` macro (`<avr/interrupt.h>`), not the parameterless `__attribute__((interrupt))`; (2) Harvard `PROGMEM` — flash const data needs `PROGMEM` + `pgm_read_*` accessors (a flash pointer can't be plain-deref'd), so `@section` alone doesn't cover it; (3) toolchain — `avr-gcc`-only (clang/zig don't target AVR cleanly). A bounded follow-on when demand warrants. |

  **Why kama fits:** no-GC + RAII → deterministic, no hidden pauses; allocation is explicit in the emitted C
  (greppable no-heap audit); trap lowering is dependency-free; `InlineArray<T,N>`, sized ints, and `unsafe`/`Ptr`
  FFI already exist. **North star: blink an LED** (the embedded "first triangle"). **Start Cortex-M, not AVR**
  (`zig cc`/clang do `thumbv*-none-eabi` cleanly; pico-sdk is tidy; AVR pain comes later).

### Compile-time evaluation & platform-specific compilation — residuals

The const-eval ladder and decl-level conditional compilation are done ([SPEC.md](SPEC.md)). What is left:

- **Host-endianness flag → `htole`/`htobe` (small).** `std::num` `byteswap*` (pure value swaps) + `bitcast`
  ship, but *host-order* serialization helpers need a compile-time endianness fact pure kama arithmetic can't
  observe — a natural `@compileFor`-style built-in flag (`LITTLE_ENDIAN`/`BIG_ENDIAN`). Every current target is
  little-endian, so this is deferred until a big-endian target appears.
- **`comptime fn` nice-to-haves (deferred).** `sizeof`/`alignof` and named-arg reorder *inside* a comptime fn
  body; a **local** `comptime T X = f();` initialized by a comptime-fn call (module + type-associated const
  forms ship); a per-fn `@steps(…)` budget override; dual-use fallback emission.
- **Platform tag-type compilation.** The `@compileFor`-gated contract-impl seam is the sanctioned platform-variance
  mechanism (per-platform `type` impls behind a platform-agnostic `contract`, exactly one survives) — NOT
  in-function branching / `#ifdef`. Extending it as new targets land is forward library/driver work.

## 6. Concurrency — what is left above the primitives

The language primitives are done ([SPEC.md](SPEC.md#concurrency-)). The higher-level **job system and
event-loop scheduler are libraries** on them (Go/Erlang-style block-on-channel, deliberately **not**
`async/await` function-colouring) — see the engine track (§8) and
[WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md).

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

**Toolchain packaging rides along.** The version store is already modality-aware (`versions/<kind>-<v>/`,
`kind=compiler`), so once a scripting runtime exists it becomes the second `kind` — `kama toolchain` gains
runtime versions alongside compiler versions, resolved by the same project pin. Nothing to build until the
runtime does; it is only listed here so the store's spare axis isn't forgotten.

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

Where kama currently stands is measured in [benchmarks/RESULTS.md](benchmarks/RESULTS.md) — read it there
rather than here, so there is one number to keep current. Forward work:

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
## 10. Tooling / distribution (deferred)

- **AI/agent tooling, shipped WITH the language.** Design in its own session; scheduled after the four
  contract-model campaigns. The goal is the **bare-bones native support a kama project would want** — not
  this repo's own working preferences. kama ships what only kama can provide (the language's own
  facts, verified); a user adds `ponytail` or anything else to their project themselves if they want it.

  **Not skill-only.** A skill is one delivery form, and a heavy one. The smaller and more portable form is a
  snippet a user pastes into their own `CLAUDE.md` — which is exactly how `karpathy-guidelines` is applied
  in this repo. Ship the snippet; offer the skill as the richer option, not the requirement.

  Most of the substrate already exists and is unadvertised:
  - **`kama query` is already an agent interface.** `--symbols` (outline), `--def L:C`, `--type L:C`
    (hover), `--refs L:C`, `--complete L:C`, `--coverage`. One process, one file, structured output, no
    editor and no LSP handshake — cheaper for an agent than speaking LSP over stdio, and it answers with
    what the COMPILER resolved rather than what a grep guessed. That is the whole value proposition:
    verified answers, not plausible ones.
  - **`llms.txt`** (78 lines) already points at the grammar as the source of truth, the spec for
    semantics, and the fixtures for runnable examples.

  **It can be written in kama.** A skill is markdown — both skills vendored here are a single `SKILL.md`
  with no scripts at all — so nothing forces node. Where a helper binary IS wanted, kama can compile it,
  and the language authoring its own tooling is the right dogfooding. Note the strongest version may need
  no new binary: `kama query` is the tool already.

  Two things worth settling early, because they shape everything else. **May the tooling invoke the
  compiler, or only read?** Being able to run `kama build` / `./dev fixture` closes the loop — an agent can
  check its own work — and that is the difference between a documentation aid and a working tool. And
  **what keeps it from drifting** the way docs do: the answer this repo reaches for elsewhere is a
  `tools/check-*.sh` guard, and `check-lsp.sh` already carries 150 assertions against the query surface.

  Reference: <https://github.com/DietrichGebert/ponytail>. It should pay for itself immediately — developing
  kama in this repo is exactly the workload.

- **`kama fmt` — a native formatter, not an external tool.** The language should print itself: one
  canonical form, applied by the toolchain, so a project never argues about style and a diff never carries
  noise that is not a change. Driven by the project's `kama.json` (the same manifest that already carries
  the flag universe and the toolchain pin), with a small, deliberately non-negotiable set of knobs —
  indent width, line width, brace style — rather than a style language. Baseline behaviour: normalize
  redundant whitespace, enforce the house layout, and stabilize the shapes that produce spurious diffs
  (argument lists that wrap, trailing commas, alignment). Two things make this cheaper here than
  elsewhere: the tree-sitter grammar already parses every accepted file and is checked against the
  compiler on every run (`tools/check-treesitter.sh`), so the formatter has a trustworthy CST to print
  from; and `kama fmt --check` is one more guard script. It is also the migration tool for mandatory
  braces (§1) — the rewrite is mechanical, and a formatter that can insert a brace can perform it.

- **A diagnostic raised inside an inlined prelude/lib body reports the USER's filename with the PRELUDE's
  line.** A prelude generic whose bound fails correctly reports it at the call site, then emits a cascade
  against `<user file>:<prelude line>` — a line the user's file may not even have. Two defects in
  one: the instantiation proceeds after its bound has already failed, and the diagnostic inherits the wrong
  unit. The second is the same class of bug as the `kama_panic_at` line-baking the codegen gate has to
  normalize around, so a fix likely serves both. Narrow — it needs a prelude GENERIC whose bound fails —
  but the wrong file:line is the kind of thing that sends a reader to the wrong place entirely.

- **A kama identifier that is a C keyword emits raw and breaks the C compiler.** `int32 switch = 3;` is a
  legal kama declaration and lowers to `int32_t switch = 3;`, which clang rejects with an error pointing at
  generated C the author never wrote. **25 of C11's 44 keywords are legal kama identifiers** — including
  `switch`, `float`, `long`, `short`, `signed`, `unsigned`, `union`, `struct`, `auto`, `goto`, `register`,
  `inline`, `typedef` and `volatile` (which this project deliberately DE-reserved). `switch` and `float`
  are the ones to worry about: kama spells them `match` and `float32`/`float64`, so both are free — and
  `switch` is an entirely plausible variable name in the embedded code kama targets.

  **Exposure is exactly locals, parameters, and struct FIELDS.** Types, functions and methods are already
  namespace-mangled (`_F4__Holder`, `_F4__helper`) and cannot collide; the bare three can. A field named
  `switch` breaks the struct definition, not just a statement.

  **Mangle rather than reserve.** Reserving 25 more words is worse for users than a rename the emitter does
  silently, and blanket-mangling every identifier would cost the readability of the emitted C, which is a
  real property of a transpiler. So: rename ONLY on collision (`switch` -> `switch_` or similar), leaving
  every other name exactly as written. The work is not one line — there is no single chokepoint where a
  kama name becomes a C name, and a USE must agree with its DECLARATION, so a partial fix is worse than
  none (an undeclared-identifier error instead of a keyword error). Intern the renamed form once, where the
  name enters the emitter's tables, so every downstream use reads it naturally.

- **Devirtualize a contract-value call in DEBUG builds.** `Comparable<int32> c = l; c.compareTo(other: r);`
  costs nothing at `-O2` — clang folds the `static const` vtable pointer, devirtualizes, inlines the thunk,
  and reduces the whole call to six instructions with no call at all. At `-O0` the indirection survives, so
  a debug build pays for a fat pointer whose target the compiler knew when it wrote it one line earlier.
  kama already does the analogous analysis for inheritance — `buildVtables` keeps a whole-program override
  index so a never-overridden slot lowers to a direct call ([kama.cemit.cpp](../kama.cemit.cpp), guarded by
  `tools/check-ecs-zero-dispatch.sh`). The contract-value case needs less: a local "this fat pointer's
  `vtbl` was assigned a known constant and never reassigned" check. Low priority — release builds are
  already optimal, and this only buys debug-build speed.

- **Repo layout — compiler sources under `src/`, and a gitignored scratch directory.** Two separate
  irritations with one shape. (a) The repo root mixes the compiler's own sources (`kama.l`, `kama.y`,
  `kama.cemit.*`, `kama.driver.cpp`, `kama_runtime.h`) with everything else; they belong under `src/`, with
  build output staying outside it so nothing generated lands beside a hand-written file. (b) There is
  nowhere sanctioned to **prototype in kama** — trying a language feature means writing a `.kama` somewhere,
  and the only choices today are the repo root (pollution) or a path outside the tree (invisible to the next
  session). A gitignored scratch directory (`.scratch/`, or similar) gives that work one home, so a
  throwaway `type contract Foo<T is This>` never risks being committed and never has to be re-derived.
  Neither is urgent; both get cheaper to do the sooner they happen, since every path reference is a cost.

- **Build configuration + cross-compilation — residuals.** The target/build-type/output selection model is
  done ([targets.md](targets.md), [SPEC.md](SPEC.md)). What is left:
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
- **VS Code Marketplace publish** — the `.vsix` is built and attached to releases; Marketplace publishing is
  deferred, and gates on a public release.
- **Language server (LSP) — residuals.** `kama lsp` and its eight editors are documented in
  [editors.md](editors.md). Forward work:
  - **Tree-sitter grammar + Zed extension — residuals.** `tree-sitter-kama/` is guarded by
    `tools/check-treesitter.sh`; `editor/zed/` is the Zed extension. Forward work:
    - **Flip the grammar source to the public URL** when the repo goes public — a tag `rev` +
      `https://github.com/cosmic-canopy/kama` in `editor/zed/extension.toml`, and the `git`+`subpath` form
      in the Helix snippet. Both spellings are already written out in `docs/editors.md`; this is a
      two-line change gated purely on visibility.
    - **Close the Zed loop.** The Rust component is compile-verified against `zed_extension_api` and the
      queries are checked, but `zed: install dev extension` is a GUI action and has not been run.
    - **Registry/ecosystem registrations, all gated on the repo being public:** publish to the Zed
      extension registry; nvim-treesitter `install_info` with `location = 'tree-sitter-kama'`; the GitHub
      linguist PR (`provisioning/linguist/languages.yml.snippet` still points at the TextMate grammar);
      and upstreaming the Helix `[[language]]`/`[[grammar]]` entries — the same class of work as the
      `nvim-lspconfig`/`eglot-server-programs` registrations below.
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
- **Workspace-internal dependencies — one follow-on.** Workspaces work today ([packages.md](packages.md)).
  What is left: version reconciliation on publish — `kama publish` substituting a registry version for a
  workspace path dep.
- **kama-aware debugger value formatting — polish on the working debugger.** Breakpoints/stepping are already
  kama-source-level, but inspected values render in their emitted-C form (a `string` shows as
  `kama_string {data,len,cap}`, `Optional<T>` as its tagged union, collections as C structs). Add LLDB type
  summaries / synthetic providers (CodeLLDB supports Python formatters) so `string`/`Optional`/`Result`/the
  collections/smart-pointers render as kama values. Small next to the LSP, high polish-value, builds directly
  on the shipped debug flow.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
- **Package manager (ecosystem foundation).** A first-class dependency manager + registry so libraries distribute
  without vendoring — the point at which the **orphan rule** (§3, retroactive conformance) becomes load-bearing.
  User docs (including the registry protocol a host must serve): [packages.md](packages.md). What remains is
  hosted-services and ops work:
  - **Both gated on hosted services / the repo being public + the website staged:**
    - **M3.3 — hosted deployment (pure ops, no compiler change).** Stand up the real registry host (Cloudflare
      Pages static index + GitHub Releases/R2 tarballs), wire the built-in default base URI (`kDefaultRegistry`,
      deliberately **empty** today so an unconfigured registry dep errors rather than reaching a dead URL) to the
      live URL, add publish auth (a token model — the one M3.1/M3.2a open question left for the remote), and
      extend a PUBLISHING.md release process. A dynamic Workers/KV/R2-or-Node service is an *optional* drop-in
      speaking the same M3.1 protocol.
    - **Mandatory verification + the trust model.** Signing ships but proves less than it looks like:
      `ssh-keygen -Y check-novalidate` validates the signature against *the key inside the signature*, so
      **nothing binds that key to a publisher** — and verification runs only on a cold url fetch (a warm
      store hit and every git dep are unchecked). [packages.md](packages.md) now says so plainly; content
      integrity (tree-hash store keys, pinned `integrity`, the confusion guard) is the guarantee that
      actually carries weight today.

      **Decided direction:** follow where the ecosystem landed rather than per-developer signing keys.
      Go ships no package signatures at all and leans on the `sum.golang.org` transparency log; PyPI
      *removed* PGP in 2023 (almost nobody verified) and replaced it with OIDC Trusted Publishing +
      attestations; npm did the same via sigstore provenance; crates.io ships checksums only. So:
      **(a)** near-term, an allowed-signers set — real `ssh-keygen -Y verify` against a configured trust
      set, plus closing the warm-store and git-dep gaps; **(b)** then CI/OIDC provenance recorded in a
      transparency log, at which point verification becomes mandatory. Both gate on a live registry, since
      mandatory verification is meaningless before one exists.
- **Longer-term — a "node.js-class" application framework in kama.** A fast, low-overhead server/app framework
  (HTTP already dogfooded via `examples/httpd`), aiming to beat the Node/Deno overhead profile on the no-GC/AOT
  (or VM-scripted) runtime — the flagship *application* of the language + package manager + scripting tiers
  together. See [WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md). Aspirational, post-ecosystem.
