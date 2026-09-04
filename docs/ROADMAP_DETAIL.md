# kama roadmap — detail

The reasoning behind every item in [ROADMAP.md](ROADMAP.md). That file is the **order**; this one is the
**why**, one section per topic. Nothing here is history — when an item ships, its record moves to the
permanent doc (see the table below) and its section is deleted from here.

> ### ⚠️ Maintaining these two files — read before editing either
>
> **[ROADMAP.md](ROADMAP.md) is the short ordered list and must stay short.** It carries one row per item:
> a one-line summary and a link into this file. No reasoning belongs there, ever — that is what sent the
> single-file version to 1,279 lines and made "what is next" unreadable. `tools/check-roadmap.sh` holds
> down the line ceiling, that every row resolves to a section here, and that no section here is orphaned.
>
> **When an item ships, DELETE it from both files.** A roadmap that also logs completions stops being
> readable as a plan, and the single-file version drifted that way twice.
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

<a id="s1"></a>

## 1. Remaining before 1.0

What the language *is* lives in [SPEC.md](SPEC.md); the engine/MCU capability matrices in
[ENGINE_READINESS.md](ENGINE_READINESS.md) / [MCU_READINESS.md](MCU_READINESS.md); the history in the git log.

**The language surface is feature-complete.** Anything that would *break* source has to land before the
tag or wait for 2.0.

**The docs/naming reconcile — CLOSED `0.9.98`, and the row was wrong about its own subject.** It was
scheduled as a NAMING pass (PascalCase types, lowerCamel methods, no `I`-prefix on contracts, lowercase
`string`). Measured across `lib/` and `prelude/`, every one of those conventions **already held** — no
`I`-prefixed contract, no non-PascalCase type declaration (the only lowercase ones are
`prelude/builtin.kama`'s primitives, which the convention wants lowercase), no non-lowerCamel public
method in `lib/std`. There was nothing to reconcile.

What was actually broken was **doc code that the compiler refuses**, and it was worse than stale: `SPEC.md`
says in prose *"Bare `int` is not a kama type"* and then used `int` as a type in seventeen of its own code
blocks. Those snippets were legal until the module campaign (`0.9.80`) made every C keyword a reserved
word; nothing noticed, because nothing reads the docs' code. Measured and fixed: **29 × bare `int`**
(SPEC.md), **10 × bare `float`** (TYPE_MODEL.md), and **8 ×** the `p::{X}` module-import spelling the same
campaign deleted (the form that parses is brace-first, `import { p::X };`) — all eight of those in prose
rather than in blocks. **13 fenced blocks were rejected by the lexer before; zero after.**

`tools/check-doc-spelling.sh` holds it down, deriving its flag set from `kama.l`'s own two tables (the C
words kama reserves, minus the ones kama uses) rather than listing them — hardcoding is how the docs
rotted in the first place. ⚠️ **It is a LEXICAL check on purpose.** Compiling every fenced block was the
first design and is wrong: 81 of 139 blocks do not parse standalone, and almost none of those is a defect
— they are deliberate fragments. That guard would mean annotating ~110 blocks with opt-out markers and
would not have caught one of the 47 real defects. A reserved word is refused wherever it appears, fragment
or not, which is why text is the right substrate here. The same extraction, aimed at negative claims
instead, is what the "~42 negative doc claims have no xfail link" row wants.

The **manifest** key set is reconciled: the entry field is `entry`, not `main` (npm's `main` names a
library's entry point for importers — the opposite end of the word), and `out` names the build-output
root. Both are read by `kama seed`, which is what would otherwise have propagated a wrong name into every
project created after it.

**A value-producing `match` is typed at CHECK time — shipped `0.9.37`.** ⚠️ **The row that tracked this was
right about the blind spot and wrong about what it cost, and the correction is the part worth keeping.**

The row said the cause was pass ordering: `emitMatchSwitch` binds an arm's payload during emission while
`rejectInitKindMismatch` runs earlier in `checkDeclaredTypes`. That path is **unreachable** — the only
shape `checkDeclaredTypes` could see is a `match` in a field initializer, and that is refused outright
(*"a value-producing `match` here needs a statement slot"*). Twenty probes across every ordinary position
— local init, assignment, return, free/method/collection argument, operand, ternary branch, nested match,
block arm, `ref` param, interpolation, `cast<T>(match …)`, `isize`/`usize` vs `int64`/`uint64`, and this
section's own `p.v` member-access example — were **already rejected**, because the destination is threaded
into `_matchTargetCType` and the rule fires on the ARM, one level down.

The real defect was **stale target inheritance**, and it was a silent miscompile:

```kama
fn int64 f(int8 p) { return cast<int64>(p); }
int64 y = f(p: true ? match (e) { case A(v: v): v; … } : match (e) { … });   // built clean, returned 44
```

Both gates that thread a destination (call argument, assignment) tested the node itself for being a
`MatchNode`, so the same match **wrapped in a ternary** slipped past — and a non-threading position left
`_matchTargetCType` alone rather than clearing it, so the match adopted the enclosing `int64`. The arm
check then passed, and an `int64` 300 reached an `int8` parameter as 44. The kind rule went the same way
(a number into a `bool p` was accepted). Two locks had to fail at once: the target was inherited, **and**
the argument-level check that should have caught it was silent because `typeOfExpr` answered `""` for a
`match` — which is the blind spot the row named.

Both are closed. `needsTargetType` looks through a ternary; non-threading positions now **clear** the
target so a miss fails closed on the existing "must appear in a typed position" error; `typeOfExpr` binds
each arm's payload types (via `matchSubjectClassQuiet` + `bindInstSubst` + `bindArmPayloadTypes`, shared
with the emitter) while classifying that arm. Two neighbours fell out and are fixed with it: the target
was also stale across an arm's **non-final statements**, and an **empty block arm** reached neither
arm-block check, emitting C that read the result temp uninitialized — caught only by clang, never by kama.

⚠️ Do not restore the **literal** fallback in `typeOfExpr`'s pass 2. A literal is contextually typed (D2a),
so it can only echo the destination (catching nothing) or contradict it (a false positive; `isize written =
match (wr) { case Ok(value: w): w; case Err(error: e): 0; }` was rejected as an `int32`). The fix is to
bind the payload types, not to guess — the `anyValueArm` gate stays exactly as it is.

**The classifier's blind spots, surveyed 2026-08-18** — measured with `kama check --strict-numeric` over
`tests/*.kama` **one file at a time** (checking them together makes one program and every `main` collides),
counting distinct `file:line` in the `unknown-src` + `op-unknown` buckets. The list in `typeOfExpr`'s
comment had drifted from the code in two places, both found by probe rather than by reading.

*Re-measured after the value-producing-`match` work (2026-08-18, `0.9.37`): **369 → 165** distinct blind
lines, and the `match`-shaped ones **220 → 16**. The 16 that remain were the generic-body family (closed `0.9.43`)
plus arms whose values are literal ternaries, not a residue of the match typing itself. **The corpus needed
no migration at all** — 1234 fixtures green with zero edits — which is itself the evidence that the arm
rule had been enforcing the same constraint all along, and therefore that this milestone's payoff was the
miscompile above and an honest instrument, never 220 silent numeric rules.*

| source | status | size |
|---|---|---|
| `foreach` binding | **closed** `0.9.31` | −69 lines |
| `match`-arm payload binding | **closed** `0.9.32` | −64 lines |
| `borrow` alias | **was already closed** — the comment outlived the code; the borrow site records both the C type and the type node | 0 |
| mixed arithmetic | **not a gap** — milestone 6 makes mixed operands an *error*, so there is no type to invent | 0 |
| value-producing `match` at check time | **closed** `0.9.37` | −204 lines |
| type parameter | **closed** `0.9.40`–`0.9.43` — an opaque type parameter gives `T` a type that promises what its bounds promise, so an expression typed by `T` resolves at the declaration | ~24 |
| comptime parameter | open — the residue of that work: a `comptime N: int32` stands for a VALUE, and a probe has none to invent without deciding the template's own `comptime assert`. Concentrated in `lib/std/num/fixed.kama` | ~19 |
| intrinsic / `extern fn` with no recorded return type | open, small — the `string.length()` class the isize campaign fixed one instance of | ~5 |
| a user **operator overload**'s result | open, small — **the table's missing row**, found 2026-08-18 by probe while writing the conformance fixtures: `(n + 3)` is `?` even though `operator+` declares `-> int32`. The operator-heavy files (`math/vec`, `quat`, `num/fixed`) are blind mostly for the *generic* reason above, so this is its own small bucket, not their cause | ~5 |

**What the binding milestone corrected about its own brief.** The migration was recorded here as 19
fixtures, measured by writing `_localCTypes[nm]` at the two `foreach` sites. The landed fix routes through
`_localTypeNodes` instead — read only by `neverNullType` and the numeric rules, where `_localCTypes` is
also read by `lvalueCType`, the compound-assignment path and the ownership/RAII paths — and breaks **six**.
`fs_roundtrip`, `net_tcp_options` and the `proc_*` family never fail through the classifier; those 13 were
the ownership paths reacting to a map they had no business seeing a binding in. The distinction matters
beyond the count: a `foreach (string s in …)` binding is a borrow on the indexed path and an owned value on
the iterator path, so teaching `_localCTypes` about bindings perturbs `ownsByValue` LHS detection.

**The gap showed in the emitted CODE**, now guarded by `tools/check-binding-widen.sh`: an unanswered source
falls back to `KAMA_NARROW`, so a *widening* `cast<int64>(v)` out of a binding carried a check that could
never fire. ⚠️ **It is not a release-speed claim, and this was measured rather than assumed** (2026-08-18):
at `-O2`/`-O3` clang inlines `kama_narrow_chk_s`, proves an `int32` always fits an `int64` and deletes it —
the same loop compiled with and without the check timed **identically** (0.11 s both, same clang, same
flags, a one-token diff in the C). The cost is a debug build and `--keep-c` readability, which is what the
README's "drops into an existing C codebase" rests on. **The "~19% of a 2M-iteration loop" figure recorded
during the cast trap does not reproduce for this shape** — do not repeat it without re-measuring what it
was actually about. Neither the exit code nor a timing can see this, which is why the guard reads the C.

**The Zed grammar pin follows the GRAMMAR — done, and no longer a scheduled row.** `editor/zed/extension.toml`
pins a *commit* and Zed fetches that rev, so the pin — not the working tree — is what Zed users get. It had
drifted 16 grammar changes and 19 days behind, which is why `slot` and named match patterns stopped
highlighting with nothing to say so.

This used to be scheduled as *"repoint it at the 1.0 tag"*, on the reasoning that a content guard could not
exist against a moving SHA — it would fail the very commit that changes the grammar. Two things were wrong
with that:

- **The trigger is the grammar, not the release.** Tying it to `VERSION` would fire on all 346 commits of
  that drift window when only 16 touched the grammar, and a pin that always moves signals nothing by moving.
- **The chicken-and-egg is one commit deep, not fatal.** The target is *computable* —
  `git log -1 --format=%H -- tree-sitter-kama` — and, decisively, **stable under its own fix**: the pin lives
  in `editor/zed/`, outside the grammar directory, so correcting it never moves the answer. So the guard
  converges in one step, and a grammar change simply lands as TWO commits (the change, then the pin), the
  same shape the `VERSION` rule already has.

Also wrong was a worry about pinning an unpushed commit: the pin travels in the same push as the grammar it
names, so no *published* state ever points at something the remote cannot serve.

`tools/check-editors.sh` §2d now asserts the pin IS the last grammar commit and prints the exact fix; §2c
still proves the rev resolves and carries a grammar at all. A tag pin was considered and rejected — a tag
only moves at releases, so it would be stale by this rule for the whole window between them.

Everything else here is library or toolchain work that does **not** gate the tag:

1. **`std::process` — async/Poller-driven *live* child-stream reads.** `run()` captures a finished child's
   output today; streaming a running child's stdout as it arrives is the piece left.
2. **Standard-library follow-ups — the M2 PARITY CAMPAIGN**, briefed in
   [design/stdlib-parity.md](design/stdlib-parity.md) (**M2a shipped**; delete that file when M2c ships).
   The bar is **Rust-`std` parity**: the only no-GC peer, and the only one whose stdlib also stops before
   regex/TLS/HTTP/crypto — which is the right line now that kama has a package manager.
   ⚠️ **So TLS, regex, HTTP and crypto are DECLARED NON-GOALS for `std`, not unscheduled work**, and this
   is the sentence that says so. Recorded emphatically because the first consumer's queue lists TLS with
   the status "ROADMAP" and is waiting for it: `wss://` is not coming to `std`, and the answer for a
   secure socket is a package or terminating TLS at a reverse proxy. A non-goal that reads like a
   backlog item gets re-triaged forever. No new language
   surface; pure library/codegen. Split M2a (parse · sort · math completion · `char` classification) /
   M2b (fs + path · io handles + `lines()` · sleep + wall clock · DNS) / M2c (`std::random` ·
   `std::encoding`). The items below are that campaign's contents:
   - **`std::net`** — DNS/`getaddrinfo` (numeric hosts only today). *(UDP and ephemeral-port `getsockname`
     ship — `lib/std/net/udp.kama`; IPv6 and multicast are separate, tracked in §2.)*
   - **`std::math` has no INTEGER `min`/`max`/`clamp`.** `minf`/`maxf`/`clampf` ship and are `float64`
     only (`lib/std/math/scalar.kama`), so `min(cpuCount(), xs.length())` — the obvious thing to write for a
     `parallel_for (…, workers:)` count — has no function behind it and needs a local plus an `if`. Found
     2026-08-29 while spelling that clause. A generic over the existing `Comparable<T>` contract is the
     obvious shape, which would also cover `string` and user types; the alternative is a per-width family
     matching `minf`'s style. Small, and it is the kind of hole that only shows up when someone reaches
     for it.
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
   - **Windows suite wall-clock — the residual now that Windows itself is closed.** ~906 s there vs
     ~75 s in the container. `kama build -j` parallelizes on Windows (1.65x on 17 TUs), but
     `run_tests.sh` pins `KAMA_BUILD_JOBS=1` and fans out per fixture, so that win does not reach the
     suite. The per-fixture cost is the C compile plus Windows process startup, not — as previously
     recorded here — a connect/accept timeout: `net_addr_ctor` opens no socket at all and cost the same
     40 s as `net_refused`. Defender exclusion on the runner temp dir is the cheapest untried lever.
     Platform record: [platforms/windows.md](platforms/windows.md).
3. **MCU toolchain packaging — polish.** The turnkey Cortex-M path ships and is QEMU-proven
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

<a id="s2"></a>

## 2. Deferred language bits (tracked)

Policy: **no known limitation stays untracked** — each is scheduled or a declared non-goal. The
language-completeness residual is **closed**; what remains here is genuinely later-track or opt-in.

- **Layout control does not reach an `enum`.** `@align(N)`/`@packed` ship on a type with a struct
  (`type value`/`type resource`) and are REFUSED on both enum shapes, with a diagnostic that says so — this
  is a tracked deferral, not an oversight, and it is deliberately not a half-answer. A payload-less enum has
  no struct at all: it lowers to an integer (`typedef uint8_t E;` when pinned), and what it can already say
  about its layout is its tag width, `type enum E : IntType`. A **tagged** enum is the real gap, and the
  reason it waits is that `emitVariantStruct` emits an outer struct wrapping a per-variant payload
  `struct` and a `union` — so `__attribute__((packed))` on the outer one does **not** reach the payloads,
  and "packed except where it matters" is worse than refused. Settling it means deciding whether `packed`
  propagates inward and pinning that with a fixture that reads real `sizeof`s, which is a different piece
  of work from the passthrough that shipped. `@align(N)` alone would reach a tagged enum today, but
  shipping align-yes/packed-no is a worse rule than one line that covers both. Nothing needs it: the
  motivating cases (a vertex buffer, an `std140` block, an MMIO register block, a wire struct) are all
  `type value`. Reopen when a real wire-format union appears.

- **`Fixed<B> comptime(int32 F)` does not implement `Real`.** A contract requires *every* method, so conformance
  means writing 21 fixed-point functions including `sin`/`cos`/`atan2`/`exp`/`log`/`cbrt` in Q-format —
  CORDIC and polynomial-approximation work, a numerical-methods project rather than a library chore. It is
  the obvious first customer of the exported `Real` contract (`lib/std/math/scalar.kama`).

- **Per-target primitive availability — considered, deliberately NOT built.** *If a target genuinely cannot
  supply a primitive, reject its uses with a kama diagnostic rather than a C-level assert.* The reasoning, so
  it is not re-derived: (1) it would not replace `kama_runtime.h`'s asserts, which answer "did the C compiler
  deliver my premise **on this build**, with this `--cc` and these flags" — the only mechanism that catches a
  wrong flag or a changed toolchain default, since kama accepts arbitrary triples and any `--cc`; a kama-side
  availability table is a second assumption about the toolchain, not a verification of it. (2) The motivating
  case is not real: AVR *has* `float64`, behind `-mdouble=64`. (3) The cost is a stdlib and source-language
  fork — `float64` appears in 8 `lib/std` files including `fmt/fmt.kama`, both serializers, `math/scalar`,
  `time` and `num/endian`, and a bare `1.0` **is** a `float64`, so banning the type makes `1.0` an error.
  (4) Rust, Zig and Go all guarantee a 64-bit IEEE float on every supported target; C guarantees `double`
  exists while permitting 32 bits — the exact hole the assert plugs. No mainstream language bans a float type
  per target. If a real case ever appears, the mechanism is the existing `@compileFor` decl-level prune plus a
  tag-type boundary, not a use-site predicate on primitives.

- **`fnptr` cannot take type parameters** — the only declaration form in kama that cannot
  (`type value X<T>`, `type contract C<T>`, `enum Result<T,E>` and `fn f<T>` all can). So a generic
  callback signature has no name: `fnptr Ordering Compare<T>(ref T a, ref T b);` does not parse
  ([kama.y](../src/kama.y), the `FNPTR` rule has no type-param slot). **Deliberately deferred, not overlooked**
  — for the case it would serve, a generic **contract** is the better tool anyway: it monomorphizes to a
  direct inlinable call where an `fnptr` is an indirect one, and a comparator object can carry state,
  which matters because kama has no capturing closures. `std::collections`' `Order<T>` is the worked
  example. Additive and non-breaking, so it costs nothing to wait. *(This used to defer itself "alongside
  the full specialization campaign", on the argument that a concrete-args specialization would cover the
  per-type-body case an `fnptr` gets reached for. That campaign is now a non-goal — see the entry below —
  and it changes nothing here: the generic-contract answer never depended on it.)*

- **Full generic specialization — a DECLARED NON-GOAL.** It was scheduled as campaign 2 of the contract
  model ("polymorphism for generic functions, across all types": a second body for a generic function,
  selected by fully-concrete type arguments, so any two are identical or disjoint). Scoping it against
  the tree killed it, on three counts:
  - **It re-opens the hole M6 closed, in a worse form.** Retro-impl (`implements C for T`) was deleted
    because it let a module reach into a type it does not own. A specialization lets a module reach into
    a *function* it does not own and change what that function does for a given type — program-wide,
    invisible at every call site, from any package that can see the generic. Retro-impl at least added a
    *named method* you could see on the type; a specialization silently replaces a body. Whole-program
    coherence catches a *duplicate* specialization and does nothing about a single one, which is the
    dangerous case.
  - **What is left over is contract design, which is the language's answer already.** `type contract`
    plus `type intrinsic` covers primitives, `string`, and every type you own — that is exactly what
    `std::math`'s `Real` is, and [scalar.kama](../lib/std/math/scalar.kama) says so in prose. The
    genuine remainder is a per-type body for a user type you do **not** own, and unlocking that is the
    thing we do not want.
  - **Nothing depends on it.** Comptime parameters' three blockers are all comptime-parameters-on-types issues;
    the view-escape check is independent; no site in `lib/`, `prelude/`, `tests/`, `examples/` or
    `bench/` needs it.

  Reopen only if a concrete case appears that a contract genuinely cannot express. Three real defects
  came out of scoping it and have shipped: a duplicate function declaration was silent (for a generic
  template the last body simply won), a function type parameter's `= Default` was parsed and dropped,
  and a type parameter shadowing a visible type said nothing. All three are pinned by `tests/xfail/`.

- **The safety/unsafe boundary — SWEPT, and the seam has since been MOVED (findings 1, 2, 9 closed).**
  The intended guarantee is that danger is isolated behind `unsafe`: nothing in safe kama should be able
  to produce UB, and a bug inside an `unsafe fn` is library-author territory. Every claim below was
  produced by compiling and running a probe under ASan/UBSan, never by reading — repros in the commit
  that lands this.

  **The root cause, which subsumed most of the individual holes — now CLOSED by the unsafe seam
  (findings 1, 2 and 9).** GOALS §3a said the safe surface never sees a raw pointer and that `unsafe { }`
  + `UnsafePtr<T>` guard the FFI boundary. In fact **only the dereference operator `p[i]` was gated**.
  Safe kama could freely *produce* a raw pointer (`addr(of:)`, a public `dataPtr()`, `cast<UnsafePtr<T>>`
  of an integer), *store* it in a field, and **call arbitrary C with it** — `extern fn` calls were not
  gated at all. Both programs below compiled; both are now rejected at compile time, by four independent
  rules in the first case. What the seam IS now lives in [SPEC.md](SPEC.md) and [GOALS.md](GOALS.md);
  what is left below is findings 3–8, 10 and 11.

  ```kama
  extern "<stdlib.h>";
  extern fn UnsafePtr malloc(usize n);
  extern fn void free(UnsafePtr p);
  fn int32 main() {
      UnsafePtr q = malloc(n: cast<usize>(64));
      free(p: q); free(p: q);        // ASan: attempting double-free. NO `unsafe` in this program.
      return 0;
  }
  ```
  and, with the only `unsafe` being an idiomatic "unsafe core, safe API" accessor a library author would
  reasonably write:
  ```kama
  type value Cell { UnsafePtr<int32> p;
      public ctor make(UnsafePtr<int32> p) { this.p = p; }
      public unsafe fn int32 get() { return this.p[0]; }
  }
  Cell c = Cell.make(p: addr(of: seed));
  { int32 tmp = 1234; c = Cell.make(p: addr(of: tmp)); }
  return c.get();                     // ASan: stack-use-after-scope
  ```
  No `View` is involved in the second — the whole `type view` escape apparatus that GOALS §3c says makes
  a borrow unable to dangle is simply bypassed by a plain `type value` with a raw field.

  **The distinction that decides each remedy:** *could a library author have prevented it with the tools
  kama gives them?* If yes it is an stdlib defect — fix the library, add a fixture, the language design
  is fine. If no it is a language hole and needs a compiler rule before the tag.

  | # | finding | class | size |
  |---|---|---|---|
  | ~~1~~ | ~~`extern fn` calls are ungated — double-free, zero `unsafe`~~ — **CLOSED**: calling an `extern fn` needs an `unsafe fn`, no scalar exemption | language | — |
  | ~~2~~ | ~~`addr(of:)` + an `UnsafePtr` field = general dangling-pointer factory~~ — **CLOSED**: `UnsafePtr` is contained; producing, holding and propagating one all need an `unsafe fn` | language | — |
  | 3 | ~~**`View::<T>.make` is a `public ctor`**~~ — **RESTATED, not closed**: the ctor is private and minting needs a `@viewable` grant, so safe kama cannot forge one. The length is still *trusted* — a type that owns the memory can hand out a truthful pointer with a false length | both | — |
  | ~~4~~ | ~~**view reseat** — nested block, loop body, and through a `ref View<T>` parameter~~ — **CLOSED**: a view may not be a `ref`/`out` parameter at all (fn, method and contract member), and a view local must root in a window, so there is no intra-function form left either | language | — |
  | ~~5~~ | ~~**resize invalidation** — `View` over a `DynamicArray` that grows~~ — **CLOSED**: the window rule forces the mint into a `borrow`, and the window freezes its host, so the `add()` that reallocs is rejected | language | — |
  | ~~6~~ | ~~**aliasing** — `bad(d: ref d, v: d.view())`, then `d.reserve(...)`~~ — **CLOSED**: a view argument roots through its receiver and is compared against every mutable argument *and* the receiver, which is where the sibling form `b.eat(v: b.view())` was hiding | language | — |
  | ~~7~~ | ~~**`reserve()` reallocs without bumping `mods`**~~ — **CLOSED, twice.** The counter now bumps in `growTo`, where the buffer actually moves, so every caller is covered at once. And `foreach` became a WINDOW, so growing the container mid-loop is a **compile error**: the mistake is unnameable rather than trapped, and the four repros moved `tests/trap/` → `tests/xfail/`. The counter stays as depth for the `unsafe`/FFI paths no static rule sees | stdlib | — |
  | ~~8~~ | ~~**the compile-time foreach-invalidation guard is dead code**~~ — **RETIRED**: the guard is gone rather than revived; the borrow window is the mechanism that replaces it | language | — |
  | ~~9~~ | ~~one `unsafe { }` disables definite assignment for the whole function~~ — **CLOSED by construction**: `unsafe` IS the function now, so relaxing locals is correct, and `out` params stopped being relaxed at all | language | — |
  | 10 | **an uninstantiated generic body gets no analysis at all** — unsafe gate, escape check, moves, definite assignment all deferred to instantiation. A package author ships `check`-green code and consumers get the errors | language, not UB | M |
  | ~~11~~ | ~~narrowing `cast<int8>(300)` → 44, silently~~ — **CLOSED**: a CONSTANT that does not fit is rejected at compile time, and a RUNTIME one now traps in every build, with `truncate<T>` (low bits) and `try cast<T>` (`Optional<T>`) as the two escapes. Record in [SPEC.md](SPEC.md) | wart | — |

  **Confirmed defended, by probe not assumption:** every arithmetic class (div0, mod0, `INT_MIN/-1`,
  shift width, float-cast, signed overflow — trapped in *every* build, `-fwrapv` in release); bounds on
  array/list/string/substring/view-index/view-slice/negative-index; dangling place-returns; use-after-move
  in a loop; the `export` rule under nesting; `parallel_for`'s write-capture rule, which sees a write made
  through a captured raw pointer; and `unsafe` does **not** leak into a generic body.

  **`type view` was the unfinished design under 3–6, and it SHIPPED** — a view answers *which* container
  it windows (the mint: a private ctor plus a `@viewable` grant) and *how long* the window stays open (the
  `borrow` scope). See [SPEC.md](SPEC.md#slices--spans--viewt-). Finding ③ is restated rather than closed,
  in the Working order above; ⑧'s dead guard is retired.

  What the survey behind it found, kept because it is the cost basis and not a recap: of 59
  `.view()`/`.slice()` sites only **11** are outside `tests/`, there are **zero** long-lived views in
  `lib/`/`prelude/`/`examples/`/`bench/`, and `examples/webgpu` holds no kama `View` at all. Of Rust's four
  borrow abilities, three cost kama nothing it uses; the fourth — storing a borrow in a struct — is
  *already* forbidden and worked around with borrowed raw-pointer fields: **37** `UnsafePtr` fields across
  the **18** `type view` declarations, of which 11 are `modsp` mutation counters and **26** are borrowed
  data pointers. That is finding ②'s shape, and it is why containment had to key on the type.

  **The remedy shipped: `unsafe fn`.** `unsafe` now marks the BODY (C#'s meaning, not Rust's), calling one
  is unrestricted, `extern` is gated at the CALL, and any expression, declaration or binding whose TYPE is
  or contains `UnsafePtr` may only occur inside one. Findings 1, 2 and 9 are closed; the record is in
  [SPEC.md](SPEC.md#what-requires-an-unsafe-fn--the-decision-table) and [GOALS.md](GOALS.md) §3a.

- **The unsafe seam — no `null` in safe kama.** Its own campaign, agreed while the `slot` work was in
  flight (which is where its customers came from: eight buffer-realloc sites now carry `= null` field
  initializers). **Two thirds shipped**: the raw pointer is spelled `UnsafePtr<T>`, and `null` is now
  rejected for every safe type in the STORE direction (declaration, field default, assignment) as it always
  was for `== null` — which is what "no `null` in safe kama" was actually asking for.

  What is left: a compiler-emitted **debug null trap** at the two `_inUnsafe` deref gates
  (`src/kama.cemit.cpp`, the raw index read and store), and `Optional`-returning **FFI wrappers** in
  `lib/std/ptr/`, its natural home.

  **⚠️ Measured by the boundary sweep: there is no memory-safety justification left for going further,
  and specifically not for the `Optional<UnsafePtr<T>>` proposal.** Three things came back from probing:

  - **Nothing null-shaped reaches a binary.** Dereferencing an `UnsafePtr` already requires `unsafe`, so
    null-deref UB is already inside the region the guarantee concedes. The type-keyed rule leaves **six**
    positions uncovered — call argument, `return null`, `Optional::Some(value: null)`, a module static, a
    field lvalue, an element lvalue — and all six pass `kama check`, but **clang rejects every one**
    (`error: incompatible pointer to integer conversion … from 'void *'`). They are therefore an instance
    of the tracked *"`kama check` does not type-check expressions"* gap, landing on the LSP and on an AI
    agent verifying its work — not on a shipped program.
  - **`Optional` has no unwrap, and one cannot be added today.** `match` is the only way to open it —
    there is no `?` operator and **no method on `Optional`**, because a generic enum is a
    monomorphization template and rejects members (`src/kama.cemit.cpp:10267`). `unwrapPtr` is monomorphic
    for exactly that reason, and the compiler emits calls to it directly into C at **nine** sites, so its
    signature is a hard dependency of the `new`/`try new` lowering. Pervasive absence without an
    ergonomic unwrap means a `match` at every one of ~30 stdlib sites.
  - **The niche optimization is not a layout tweak.** `Optional<T>` is tag-then-union
    (`emitVariantStruct`, `:14472`) with no layout special-casing anywhere; the tagged shape is
    constructed and destructured *literally, by field name*, at ~25 emission sites. (One genuine upside if
    it were ever done: `Some` is tag 0, which today forces an explicit reset of zero-initialized
    `Optional` fields at `:15387` — `NULL == None` would delete that.)

  **So: `UnsafePtr` containment, above, is the cheaper and more direct route to the same goal.** Every
  genuine `null` in the tree (75/75) targets an `UnsafePtr`; if `UnsafePtr` can only be handled inside
  `unsafe`, `null` is confined by construction — no `Optional`, no niche opt, no unwrap ergonomic, no
  token deletion, no `tree-sitter` change. What remains worth doing on its own schedule is the honest FFI
  surface: only **8** extern declarations return a genuinely nullable pointer (`malloc` ×3,
  `kama_poller_create`, `kama_diropen`, `kama_channel_new`, `kama_argv_new`, `kama_envp_build`) —
  `fopen`/`dlopen`/`getenv`/`realloc`/`mmap` are not declared at all, since kama routes them through
  `kama_*` seams that already return `bool` + an out-param or a `Result`.

  **The `unsafe UnsafePtr<T> p = null;` field modifier and the token-level `null` ban are dropped**, and the
  reason is worth keeping: both existed to force raw-pointer declarations to be greppable, and the rename
  already did that by the type's own name. The `null` rule that shipped is keyed on the **declared type**
  instead of on lexical context, which is strictly better — it needs no new grammar and no lexical region
  around a declaration, and its blast radius across `lib/` and `prelude/` was
  **zero**, because every real `null` in the tree already targets an `UnsafePtr`. Reopen only if a case
  appears that the type-keyed rule cannot express.
  - **It no longer owns iterator laundering — that shipped on its own.** The two were folded together on
    the argument that the honest fix *was* the seam, because making the iterators `type view` "is not a
    local fix": `Iterable<T>.iterator()` returns a *contract value*, so the iterator would box (heap
    ownership of a borrow), rejecting `View<T>`'s conformance and taking `sort` with it. **That premise was
    wrong.** `foreach` never dispatches through the contract — `emitForeachIterator` resolves
    `iterator()`/`iterMut()` structurally and emits direct monomorphized calls, and `sort` takes a `View<T>`
    directly, not an `Iterable`. Nothing boxes on that path, so all 17 borrowing iterators became `type view`
    with **zero compiler changes**. What is left for the seam is what it was always really about: saying that
    a raw pointer is raw.

- **`kama check` does not type-check expressions — so it reports OK on code that will not build.**
  `int32 x = "oops";` passes `check` and exits 0; only `kama build` rejects it, via the C compiler
  (correctly located, since the emitted C carries `#line`). What `check` *does* catch is name
  resolution, unknown functions/methods/types, named-argument mismatches, and ownership/move and
  serde analysis — a useful fast subset, but not the verdict its name suggests. It also owns the last
  residual of the `export { … }` rule: every position that *names* a type is checked, but an expression
  that never names one (`v.iterator().next()`, reaching a non-exported iterator through inference) is not a
  declaration at all, and catching it means knowing an expression's type. This matters most to
  the two consumers that surface `check`-class diagnostics and nothing else: an **AI agent** told to
  verify its work, and the **LSP**, which shows a clean buffer for a file that will fail to compile.
  Documented rather than hidden ([agents.md](agents.md), `usage()`, `agents/AGENTS.md`) and pinned by
  `tools/check-agents.sh`, which asserts the caveat still holds — so closing this gap will *fail* that
  guard and force the claim out of all three. Real expression type checking in the front end is a
  campaign, not a fix.

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
  `@generate(Formattable)`, and tagged strings all ship (SPEC). Still open, additive, no current need: combining a
  base marker with width/flags (`${n:08x}`), a custom fill character, center-align (`^`); a `@generate(Formattable)`
  on a **generic**/**variant**/**enum** type; a `${x:?}`-routed `@generate(Debug)` (spec hook already exists);
  per-derive `@skip(Formattable)` / `@skip(Serializable)` for redaction (today `@skip` is one shared boolean —
  parameterize `FieldInfo::serSkip` to a per-derive set when a concrete case appears); and tagged-string
  *type-preserved params* (Model B — each hole keeping its static type into the params list, `html` returning
  a distinct `SafeHtml`). Regex is a separate campaign. `string + <number>` stays a compile error by design.
- **Full `expose` (2.0).** The minimal `expose fn` free-function C-ABI boundary ships today (SPEC + §8
  hot-reload); the **full `expose`** — richer wasm module exports + the scripting host interface — stays 2.0 (§7).

  What is actually missing on the wasm half, measured 2026-08-23 rather than assumed. An `expose`d
  function **is** a real wasm export (`WebAssembly.Module.exports()` lists it), so an embedder that
  instantiates the `.wasm` directly can already call it — and never runs `main`. What does **not** work is
  reaching it off the generated JS as `Module._add`: that needs `-sEXPORTED_FUNCTIONS`/`-sMODULARIZE`,
  and kama emits a PROGRAM (shebang, runs `main`, exits), not a library. `tests/expose_basic.kama` claimed
  the `Module._add` form worked; it never did, and its comment now says so.

  ⚠️ **This row owns a constraint from `0.9.63`.** Every wasm build now sets `-sEXIT_RUNTIME=1`, because
  without it node's graceful teardown deadlocks against V8's background threads (see the comment at the
  flag in `kama.driver.cpp`, and `run_tests.sh`'s watchdog history). A *module* artifact must keep its
  runtime alive after `main`, so shipping this work means making that flag conditional again — on the
  artifact **kind** (program vs module), never on which library the program happens to use, which is the
  keying that was wrong before.
- **Derive follow-ons.** `@generate(Equatable, Hashable)` ships for plain types (SPEC § *Derives*). Still
  open, additive: the same derives on a **generic** or **variant** type (the same v1 boundary
  `@generate(Formattable)` draws — all of them now error rather than half-deriving; `Serializable`/`Deserializable`
  were the two kinds with no arm, so they were *accepted in silence* and died in the C compiler on a
  missing `_F<file>__Box_int32__as_Serialize` vtable — guarded by `tests/xfail/generate_serialize_generic`),
  and on a payload-less **enum**, which
  has no struct to walk and today declares `implements` on its own `type enum` line instead. A `Copyable` derive is a
  **non-goal**: a value/view copies by kind, and a resource's `copy` ctor is an ownership decision no field
  walk can make (a memberwise copy of a raw handle double-frees).
- **A generic FREE function cannot call a generic free function with its own type parameter.**
  `fn T outer<T>(T v) { return ident(x: v); }` reports "cannot infer generic type parameter 'T' —
  argument 'x' is not a literal or a locally-typed value". `collectGenericInsts` walks each body ONCE,
  verbatim, with `_typeSubst` empty, so the argument's declared type reads as a bare name and
  `inferGenericInst` rejects it *in that pre-pass* — before the per-instantiation re-walk that would
  resolve it. The same fixpoint already answers this for a generic call inside a generic **TYPE**'s
  member (`registerInstGenerics`); the free-fn-inside-free-fn case never got the matching treatment.
  Fix = let the pre-pass DEFER an unresolvable argument instead of diagnosing it, and diagnose only what
  is still unbound after the fixpoint settles. Clean diagnostic, not silent, so it is a limitation rather
  than a hazard — but it blocks the ordinary "thin generic wrapper" shape. Found while checking whether
  a comptime param could be passed to a generic call; it fails identically for a type param, so it
  is the general gap, not a comptime one.

- **A generic `enum` cannot declare members or contracts.** `type enum Tag comptime(int32 N) { A; public fn
  int32 bump() { return N; } }` is rejected — "a generic enum is a monomorphization template, so each
  instance would need its own conformance". Clean diagnostic and a real limitation: it is why
  `EnumDeclarationNode`'s const-param data still has no reader after the comptime-parameters campaign, since a
  const param can only be READ inside a body and a generic enum has none. Whoever lifts this should add
  the const-param fixture that could not be written (`tests/constgen_value_type.kama` records the gap).

- ~~**`INT32_MIN` has no direct spelling.**~~ **Fixed.** `-2147483648` folds the negation into the
  literal at parse time (Rust's rule): the magnitude is one past INT32_MAX so the literal alone is
  rejected, but under a unary minus it fits exactly. It previously did not work at all and for an
  unrelated reason — the emitter wrote `--2147483648`, which the C compiler reads as a pre-decrement.
  Guarded by `tests/int_literal_min.kama`.

  ~~**Same family: a SUFFIXED literal is not range-checked against its own suffix.**~~ **Fixed** as
  milestone 5b-C, the same rule at every width: a magnitude one past the maximum parks and the `MINUS`
  claims it, anything larger is rejected at the literal. The brief filed this as a latent hole because the
  corpus is clean — 885 suffixed literals, none out of range — and that was half right. `300i8` really was
  only latent. But the NEGATIVE boundary was a live bug: `-128i8` narrowed to -128 at parse time, the
  minus then negated an already-negative node, and the emitter wrote `(--128)`, which C reads as a
  pre-decrement. INT8_MIN/INT16_MIN/INT32_MIN/INT64_MIN had no suffixed spelling at all, and `kama check`
  passed the file — only clang objected. Guarded by `tests/int_literal_suffix_min.kama` and three
  `tests/xfail/int_literal_suffix_*` fixtures.

- **Unresolved type names — one residual: GENERIC ARGUMENTS.** Declared type names are now checked
  (`checkDeclaredTypes`, a single-visit walk at the tail of `collectProgram`), so a misspelled or unimported
  type in a parameter, return, field or variant payload is a kama-level error instead of a C-level
  `undeclared identifier` against generated code. What is still unchecked is a type name *inside* a generic
  argument — `DynamicArray<Bogos>` — because `checkTypeResolves` early-returns on `type->genericArg`.
  Recursing into `genericArgs` (as `addTypeRef` does in kama.query.cpp) is the natural phase 2, but it
  widens the surface onto comptime size expressions, defaulted allocator args and bounds, so it wants
  its own sweep. Guarded today by `tests/xfail/unknown_type_{local,param,return,field,method_param,
  variant_payload}` + `unimported_type_param`, and by `tests/decl_type_check_guards.kama` for the three
  shapes the pass must NOT reject (a generic free fn's own params, `This`, a `sig` used before its file).
- **`std::net` — IPv6 and UDP multicast.** `IpAddr` has a `V4` arm only ([`lib/std/net/addr.kama`]), left
  deliberately as an `enum` so a `V6(...)` arm adds without reshaping `SocketAddr` or any call site.
  Multicast join/leave (`IP_ADD_MEMBERSHIP`) is likewise unbuilt — broadcast covers LAN discovery today.
  Both are ordinary socket-option work on the shipped seam.
- **Contract conformance now IS checked when a value is bound to a contract** (0.9.105) — recorded here
  only because the shape of the miss is worth not repeating. The argument hand-off skipped the KIND rule
  for a contract destination, correctly (a contract admits every kind by design, which is what makes
  `hashVia(h: 22)` over a primitive a shipped feature) — but nothing was put in its place, so the one
  position that skipped the kind check checked nothing at all. ⚠️ And the diagnostic that DID exist, in
  local-declaration position, was the emission cascade's fall-through `else`: it fired only for a value
  `exprClass` cannot type, so it caught a plain enum and let every non-conforming CLASS through to the
  branch above it, which emitted `Plain__as_Hashable` for clang to discover did not exist. **A check that
  only sees what its neighbours could not classify is not a check** — and it read as one for as long as
  nobody wrote the class case down. Pinned by three `tests/xfail/contract_*_nonconforming.kama`.
- **Class-to-class mismatches are a DIAGNOSTICS defect, not a soundness one.** `D d = c;`, `return c;`
  where `D` is declared, and `take(d: c)` all fail — but as a *C-level* message about mangled names, on a
  kama line. C never assigns between two struct types, so clang refuses them and nothing wrong-typed
  reaches a running program. Deliberately scoped OUT of the type-identity rule (0.9.103) for that reason.
  ⚠️ **`return this;` in a fallible ctor** (instead of `return Result::Ok(value: this);`) is the same
  defect reached from the ctor path: it emits `__ret_0 = self;`, a `G*` into a `Result<G,Err>`, caught
  only by clang.
- **`UnsafePtr<A>` into `UnsafePtr<B>` is unchecked by kama.** Probed 2026-08-28: `UnsafePtr<int32>` into
  `UnsafePtr<int64>` compiles with a clang *warning* and runs, which is a genuine width hole (eight bytes
  read from a four-byte allocation). It sits inside `unsafe fn`, which is the sanctioned trusted region,
  and the two C spellings differ so clang does see it — hence tracked here rather than in the identity
  rule, which covers only types that SHARE a spelling.
- **A field's DEFAULT INITIALIZER is not walked by either discovery pass.** `collectGenericInsts` and
  `collectCollections` both read a field's declared *type* and never its initializer expression, so
  `public int32 v = ident(x: 7);` — a generic call as a field default — is never discovered. It used to
  emit the template's own mangled C name and let clang refuse the result; since `0.9.100` it is a kama
  diagnostic naming the callee, pinned by `tests/xfail/generic_call_unresolved_instance.kama`. The
  workaround is the turbofish or a call from a body. Fixing it means walking the initializer in both
  passes and settling the substitution context for a generic type's fields, which is why it is tracked
  here rather than folded into the walk-parity work. ⚠️ **`tools/check-scan-parity.sh` cannot see this
  one** — it holds the two walks at parity on AST *node kinds*, and this is an asymmetry in which
  *declarations* get walked at all, which is the same for both.
- **Windows long-path support is deferred.** Surfaces only on a deep working directory. ⚠️ This entry
  used to say "the temp-path builder"; there is no such builder, and grepping `MAX_PATH` turns up two
  *different* ceilings that want separate fixes:
  - **Runtime, in shipped code** — `kama_diropen` (`include/kama_os.h:117`) builds its `<path>\*` search
    pattern in a `char[MAX_PATH]` and returns `ENOMEM` past it, so a **user's** program fails to iterate
    a deep directory. The fix is the `\\?\` prefix + `FindFirstFileW` (the `A` variants cannot exceed
    `MAX_PATH` at all), which means going wide through that whole seam.
  - **Compiler-side** — `PATH_MAX` is `_MAX_PATH` (`src/kama.driver.cpp:59`), and `absolutePath`'s
    `GetFinalPathNameByHandleA` treats an over-long result as a miss and falls back (`:176`, which says
    so). Lower stakes: it degrades to the unresolved spelling rather than failing.
- **Explicit SIMD — SHIPPED 2026-08-31**, all three stages. The record of what the surface IS lives in
  [SPEC.md](SPEC.md) (*Explicit SIMD*); this entry keeps only the MEASUREMENTS behind it, because each one
  cost real time to obtain and every one of them contradicted an assumption someone held first.

  - ⚠️ **`std::math` already auto-vectorizes on native, and beats hand-written SIMD.** `fadd.4s` loops,
    and clang de-interleaving AoS to SoA so `dot` and `Mat4.transform` run four at a time. A hand-written
    explicit-SIMD cross product measured **65% SLOWER** than the scalar source for exactly that reason.
    So `Simd` is **not** "SIMD for a language that had none", rebuilding `std::math` on it is rejected on
    measurement, and the docs say so where an author would reach for it.
  - ⚠️ **`ext_vector_type` is a silent miscompile under gcc** — ignored with a warning, leaving a
    ONE-LANE scalar. `vector_size` is the only portable spelling; `__builtin_shufflevector` works on
    both, so only the type ever needed a seam.
  - ⚠️ **gcc has no `__builtin_elementwise_*`**, and treats the name as an *implicit function
    declaration* — a warning, the same shape as above. Elementwise ops are written as per-lane loops,
    which both backends fold to the branchless vector form.
  - ⚠️ **`-fno-math-errno` is what lets a per-lane libm loop vectorize.** With it, `sqrtf` per lane folds
    to `fsqrt v0.4s` on gcc and clang; without it neither folds. macOS defaults to it and Linux does not,
    which is how a host reading came to disagree with the container's. `sqrt`/`floor`/`ceil` are NOT in
    `kama_runtime.h` for a different reason — it is freestanding, and they need libm.
  - ⚠️ **UBSan does not instrument vector arithmetic.** Same build, same flags: a scalar `int32 MAX + 1`
    trapped and the identical addition in a lane wrapped. The overflow check for signed lanes is emitted
    by the compiler because nothing else supplies it.
  - ⚠️ **A mask must carry `T`.** A comparison's lane width follows its OPERAND's — f32x4 gives 4-byte
    lanes, i16x8 gives 2-byte — identically on gcc 13.3 and clang 18, so a bare `Mask#(N)` has no C type.
  - ⚠️ **Probe design, which went wrong three times across two sessions.** A SIMD claim is invisible to
    exit codes, so the instrument is everything: never measure at an ABI boundary (AAPCS64 passes
    `struct{float x,y,z,w}` in four separate registers), never let the kernel be constant-foldable or
    dead, keep every lane of the result live or the compiler deletes the others, and remember that Apple
    writes `fadd.4s v0, v0, v1` where GNU writes `fadd v0.4s`. The rules live in the headers of
    [tests/support/simd_probe.kama](../tests/support/simd_probe.kama) and
    [simd_type_probe.kama](../tests/support/simd_type_probe.kama), beside the probes they constrain.
  - ⚠️ **A guard's negative control depends on what it measures.** `check-simd-native.sh` can use `-O0`,
    because it measures AUTO-vectorization. `check-simd-type.sh` cannot: an explicit vector type emits
    vector instructions at every optimization level, so its control is a scalar-only program the pattern
    must not match.

  Not built, and each is a decision rather than an omission: **widths above 128 bits** wait for the
  CPU-tuning knob (§9) — no AOT language ships wider lanes without a build flag, so this is parity, not a
  gap; **per-ISA intrinsics** are a declared non-goal, being the half every surveyed language keeps
  `unsafe` or experimental; and **`sqrt`/`floor`/`ceil` on a lane batch** want the `kama_math.h` seam that
  `lib/std/math/scalar.kama` uses and an intrinsic cannot reach.

  ⚠️ **On the 1.0 tag:** the row is done, so the question it raised is closed — nothing here was
  source-breaking, and the surface is additive.

- **UBSan's `function` check is disabled suite-wide, for a REASON — not an oversight** (`run_tests.sh:60`).
  It is a false-positive suppression, not a masked bug: kama's dispatch stores every slot as
  `Ret (*)(void* self, …)` and calls the concrete `Ret C__m(C* self, …)` through it. That type-erased
  `self` is ABI-identical — it is how essentially all C OO dispatch works — but the `function` sub-check
  enforces exact function-pointer type identity, so it would flag *every* contract call. Every other UBSan
  check (integer overflow, null, bounds, alignment, …) and all of ASan stay on. The residual risk is narrow
  and real: a genuine slot/signature mismatch is not caught *by UBSan* (the emitter builds both sides, so
  one usually fails to compile). If that coverage is ever wanted back, the route is emitting a per-slot
  typed thunk — `static Ret C__m__thunk(void* self, …)` that casts and calls — which makes the pointer
  types exact and lets the check be re-enabled — but that buys nothing (the emitter generates both sides
  of a slot from one declaration, so a real mismatch fails to compile) and adds an indirection to every
  dynamic call, which is the wrong trade for the embedded and hot-path targets. **DECIDED 2026-08-12: the
  exemption is permanent, and [SPEC.md](SPEC.md) now says so** in the numeric-safety/sanitizer section,
  including the guidance that a user building under `-fsanitize=undefined` should pass
  `-fno-sanitize=function`. Closed — kept here only so it is not re-diagnosed as a hole a third time.
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts with "one way to do
  a thing," and **named parameters** already cover the disambiguation overloading is usually reached for.
  **Operators are the sanctioned exception** — a type may carry several `operator*` distinguished by operand
  type (`mat*vec`, `mat*mat`), matching C++/C#/Rust. Reopen only if a concrete case shows named params can't
  express it.

- **The release tier's signed-overflow rule was toolchain-dependent — FIXED 0.9.125.** Kept here because
  the shape recurs: a flag interaction that two of three toolchains agree on reads as settled. The driver
  passed `-fsanitize=signed-integer-overflow` in BOTH tiers, keeping it in release only for
  `INT_MIN / -1` (which `-fwrapv` does not define), on the assumption that `-fwrapv` suppresses it for
  the ordinary ops. ⚠️ **Measured: that holds on Ubuntu clang 18.1.3 and gcc 13.3, and NOT on Apple clang
  21.** So a `--release` `int32 MAX + 1` trapped on macOS and wrapped on Linux from one source — and
  worse, every signed add carried `adds; b.vs; brk` and every multiply `smull; cmp; b.ne; brk`, against a
  bare `add`/`mul` on Linux. The performance invariant at the top of ROADMAP.md was false on macOS.
  Fixed by making the sanitizer debug-only and emitting the `TYPE_MIN / -1` check
  (`kama_sdiv_i32`/`_i64`, tested on the OPERANDS because `INT64_MIN / -1` overflows a result check).
  ⚠️ It went unnoticed because nothing could see it: `tests/trap/` builds debug only, and
  `tests/trap/intmin_div.kama` even STATED the release behaviour in its own comment without ever testing
  it. `tools/check-release-arith.sh` now asserts the semantics *and* the zero cost in a real release
  build.

- **Found by the first external project on kama** (a game port, 2026-08-31). Six of its reports are now
  fixed: a multi-module library not being consumable as a dependency, the output being named after the
  alphabetically first source file, absolute dependency symlinks, three stale claims in the WebGPU
  example, and — in `0.9.128` — the two recorded below. They are kept together because their provenance
  is the point: every one was found by someone *using* the language rather than by the corpus, and not
  one of them had a fixture that could have caught it. ⚠️ **Two of the six turned out to be bigger than
  their report** (the `comptime` export was a whole unwired subsystem; the frame loop was a seam gap, not
  an example typo), which is the argument for probing a user's report rather than patching its sentence.

  - **A module-scope `comptime` constant could not be exported — FIXED 0.9.128.** Kept as a record
    because probing it found something much larger than the report, and the shape recurs. KB-3 was
    filed as "naming a `comptime` in `export { }` is rejected", and the plan was to decide between
    fixing the doc and fixing the compiler. ⚠️ **Neither resolution was available: the probe found the
    subsystem had never been wired at all, and returned FIVE different answers for one declaration.**
    Same file: works. Sibling file in the same module: `kama check` says OK, `kama build` emits invalid
    C. Another module, qualified: same — check OK, clang fails. `import`: "does not export". `export`:
    "no such top-level declaration".
    - **The cause.** `declFileOf` and the export/import membership tests consulted seven registries,
      none of which hold a module-scope declaration — so the file rung could not see a module variable
      and waved through every cross-file reference. And a `comptime` lowered to `static const` in its
      declaring unit's own `.c`, so no other translation unit had anything to link against. Both halves
      were invisible to `kama check`, which folds the program into one unit.
    - **The fix.** A module `comptime` is now emitted into the shared header (`static const` — one
      addressable copy per TU, which is right for an immutable value and violates no ODR), joins the
      export/import membership sets, and resolves through `resolveModuleVar` — the same alias/qualifier
      search `resolveFuncImpl` does — at all three of its use sites, so an imported or module-qualified
      constant can also size an `InlineArray` and be addressed. A mutable module `static` is now
      **rejected at the export list with its own sentence** and gets the file rung too, which turns the
      remaining silent miscompile into a diagnostic.
    - **One nuance left open, unprobed.** Only an *exported* constant moves to the header, so a private
      `comptime fn` table still lives in one unit — but an exported one is `static const` in every TU
      that includes the header. An ordinary constant is dead-stripped where unused; a `@section`-placed
      one on an MCU may not be. No fixture exercises that combination and no user has hit it. If one
      does, the answer is a single definition with `extern` declarations, not a retreat from the header.
    - ⚠️ **The lesson, which is the reason this entry stays.** The report named the narrowest visible
      symptom. Had it been taken at face value — add the name to one membership set — the export would
      have been accepted and the program would still have failed in the C compiler, and the fixture
      proving the fix would have been a single file, which is exactly the shape that could not catch
      any of this. **A cross-file claim needs a cross-file fixture** (`tests/mod_export_const.d/`).
  - **`examples/webgpu/triangle.kama` had an unbounded-allocation frame loop — FIXED 0.9.128.** The
    example now reads `surfTex.status`, treats `Occluded`/`Timeout` as skip-the-frame, reconfigures only
    once per `Outdated`/`Lost` invalidation, stops on a 600-frame failure streak, and **sleeps on every
    path that returns without presenting** — through a new `kama_gpu_sleep_ms` on the `std::gpu` seam
    (native sleep, web no-op, because the browser owns the rAF loop). The seam function is deliberately
    an unconditional sleep rather than `glfwWaitEventsTimeout`: a throttle that can return early on an
    event is not a throttle, and this function exists because a loop lost its only throttle. The record
    of what went wrong, kept because the shape is general: when
    `wgpuSurfaceGetCurrentTexture` yields no texture the example reconfigures the swapchain and returns
    **without presenting** — and vsync, its only throttle, applies only to a presented frame. So the path
    runs at unbounded rate, allocating a swapchain per iteration. ⚠️ The trigger is trivial and permanent:
    `Occluded` (a wgpu-native extension the example never consults, not one of `webgpu.h`'s six statuses)
    is returned with a NULL texture whenever the window is not visible — another window in front is
    enough. The reporter reached 15.6 GB resident and took a machine down through the kernel watchdog,
    twice. ⚠️ **The language half is the larger point, and it promoted a row** — see the safe `std::gpu`
    wrapper in [§8](#s8). Nothing in kama could have caught this: the allocation is inside wgpu-native,
    reached through `UnsafePtr` handles carrying no RAII, so there is no kama object, no destructor and
    nothing for `@noheap` to see. ⚠️ **But be precise about which half would have caught it** — the
    reporter's framing, and this entry's first draft, both said RAII, and that is wrong. RAII would not
    have helped: the leak is wgpu-native's swapchain, triggered by ignoring an untyped `status` int. What
    prevents *this* bug is a **typed acquire result** whose error enum names `Occluded`. RAII prevents
    the *other* leak in the same file — five hand-written `wgpu*Release` calls per frame, all on the
    happy path, which any later early `return` would leak. Two halves, two different bugs, and a wrapper
    wants both.
  - **Build settings did not propagate from a dependency — FIXED `0.9.165`-`0.9.168`.** See the
    as-shipped record below (*The build-settings campaign*).
  - **The `std::gpu` seam is half-built.** `kama_gpu_pump` calls `glfwPollEvents()` and **discards the
    queue** (the web pump is `{ return 1; }`), so there is no keyboard, mouse, wheel, pointer-lock,
    resize, focus or gamepad on either target; and the surface is configured at a hardcoded 512×512 with
    no way to learn the drawable size. A size accessor is a few lines. Whether the input *library* above
    that seam is kama's or an engine's is the more arguable half, but a window seam that polls events and
    throws them away is not finished. **Now split across two rows** — the size accessor and the handle
    RAII ride the safe-wrapper row, and input is its own (`std::input`, a peer of `std::gpu`). ⚠️ The
    arguable half has an answer now: the first engine built on kama **derives its own window** and
    duplicates ~60 lines of surface-derivation out of `kama_gpu.c` to get keyboard and mouse, which is
    the outcome a seam that throws its events away forces on everybody.

- **The first consumer's second audit (2026-09-01), triaged against this tree.** Everything below was
  REPRODUCED here before being scheduled — their report names the symptom, and three times running the
  shape underneath it has been different. Their doc is `../friendly-fire-department/docs/KAMA_GAPS.md`;
  they renumber it between audits, so find an entry by its text, never by a remembered KG number. They
  pin `0.9.132` and have verified `@noheap` transitivity by behaviour in their own tree.

  - **A contract member returning a type declared BESIDE the contract — FIXED `0.9.134`.** ⚠️ **It was
    FIVE sites doing the same partial swap, not one bug.** A contract's member signatures are rendered
    under the CONTRACT's own name-resolution scope — its imports, not the implementing unit's — and five
    places did that by hand, each moving three of `NsCtx`'s four fields and leaving `unitPath` pointing at
    the other file. That is not a cosmetic omission: `checkReach` decides the per-file import rung by
    comparing exactly that path against the file being walked, *precisely so it can tell "these imports
    belong to a different file"*. A stale path made the two agree, so `Thing` was judged against the
    contract's own imports — and a file does not import what it declares, so the implementer was told to
    `import { Thing };` a name its line 1 already imported. Only a member returning a USER type could
    reach it, which is what made it look like a type-system bug.
    - ⚠️ **Four of the five were found by fixing the first four.** The repro moved its diagnostic from
      `impl.kama` to `main.kama` twice as each site was closed — `contractSigOf`, `emitClassInterfaceVtables`,
      the refinement-thunk loop, `emitInterfaceTypes` (which had no reseat at all) and the
      contract-value return-type path. Each time the answer was to instrument `checkReach` and read
      `nsUnit` against `refFile`, never to guess the next one. They are one `ScopedContractNs` now, which
      also carries the diagnostic file — so a sixth site cannot get it wrong.
    - ⚠️ **The misattributed diagnostic was the same bug, not a second one.** It named one file's path
      with another's LINE (`impl.kama:9`, where line 9 of that file is a closing brace and line 9 of the
      contract's file is the member) — because the scope had moved and the diagnostic file had not. It
      cost the reporter the bisection; it cost one line to fix.
  - **A module `comptime` cannot be interpolated — FIXED `0.9.134`, and it was neither of those things.**
    Reported as comptime-specific and interpolation-specific; it is a MODULE-SCOPE name failing as a
    method RECEIVER. Interpolation only lowers `${X}` to a method call on the value, so a module `static`
    failed identically — the half nobody reported — and so did a direct `X.toString()`. One missing
    lookup: `receiverScalarCType` and `receiverTypeNode` consulted locals and bound `comptime` parameters
    but never `_moduleStatics`, though a module name resolves the way a module function does
    (`resolveModuleVar`). A module-scope name now behaves identically to a local at that site, verified by
    the two producing the same diagnostic for the same mistake.
    ⚠️ **The line number fixed itself.** "at line 1, column 0" was not a separate defect: resolution
    failed, so the error came from a node carrying the constant's declaration line. What was worth fixing
    on its own is the message, which was four words with no subject — it now names the receiver, which is
    the general rule that stops the next one costing forty minutes.
  - **A `fnptr` in a field or a module `static` cannot be CALLED — FIXED `0.9.134`.** ⚠️ Filed as four
    failures; two did not reproduce (binding into both places already worked), so it was one defect with
    two arms: `emitInvocation` recognised only a bare LOCAL of signature type, so a `static` call fell
    through to "call to unknown function", and a field call reached METHOD dispatch and was reported as a
    missing method. Both arms added; the field arm is checked last, after every real method has failed to
    resolve, so a field can never shadow a method. The callback registry — install now, dispatch later,
    re-bind in between — now has a spelling ([tests/fnptr_stored.kama](../tests/fnptr_stored.kama)), and
    SPEC's `fnptr` section says so.

  - **No way to give an `extern fn` or an `expose fn` a symbol name different from its kama name.**
    `@linkName("…")` — the peer of Rust's `#[link_name]` / `#[export_name]`. Rowed 2026-09-01 while
    closing the reserved-word-field row, because working that one out showed the two are **different
    problems** and Rust keeps them separate on purpose:

    | problem | Rust | kama |
    |---|---|---|
    | "my grammar cannot SPELL this name" | `r#type` | contextual keywords (`type`, `copy`, `give`, `base`, `default`, `truncate`) |
    | "this SYMBOL is named something else" | `#[link_name]` / `#[export_name]` | **nothing — this row** |

    A struct field is the first problem, and it is closed: the field name is resolved at compile time and
    emitted as text, so there is no symbol involved. This row is the second. Today an `extern fn` must be
    spelled exactly as C names it (SPEC: *"an `extern` keeps a literal name"*), so a C symbol colliding
    with a kama KEYWORD — not merely with another identifier, which the wrapper convention already
    handles — has no binding at all; and `expose fn` emits under its bare kama name with no way to choose
    the exported symbol.

    ⚠️ **Do not name it `@cname`.** kama's only backend is C emission today, so the name would be accurate
    and would age badly: the 2.0 dual-mode arc puts a bytecode VM behind the same source, and a VM has no
    C names. Name it for the concept (`@linkName`, `@symbol`), not for one backend.

    Not urgent: no consumer is blocked on it, and no collision of this kind has been hit. It is rowed so
    the distinction is not re-derived — the reserved-word-field work reached for `@cname` twice before the
    measurement showed a contextual keyword was both smaller and categorically the right tool.


  - **The real-time cluster stopped being an argument and became a number.** Because kama cannot run on
    the CoreAudio thread, their synth runs on the frame loop and feeds the device through a ring — so
    stall tolerance IS latency, and the two cannot be traded. A macOS session logged **326 underruns** at
    a 170 ms queue; deepening it to 683 ms fixed occlusion outright and still left **~290 during window
    drags**, because `glfwPollEvents` blocks inside a nested run loop for the whole drag while the
    producer never runs. The audio thread keeps running throughout — that is *how* the underruns were
    counted. Their conclusion, and it looks right: moving the synth into the device callback "fixes the
    case completely, and nothing else does", which needs the foreign-thread row to be safe at all, the
    `@noheap` transitivity that shipped for the guarantee to be real, and the panic-policy row before it
    can ship — a bounds miss in a mixer currently aborts the process from a thread nobody can see.
  - **Build settings — SHIPPED `0.9.165` through `0.9.168`.** Their two complaints and one more this
    campaign found, with the reasoning that is worth not re-deriving:
    - **A project tier had to come first.** `cflags`/`ldflags` existed only under `select.TARGET.<NAME>`,
      and a target is matched BY NAME — so `--target aarch64-linux-gnu`, an anonymous triple, matched no
      entry and got none of them. Decisively: a DEPENDENCY cannot know how its consumer spells the
      target, so propagation alone would have been half-dead on arrival.
    - **Per-manifest resolution, then concatenation.** Each manifest resolves under its own precedence
      and only then are the lists joined, because `link`-REPLACES applied globally would let a dependency
      delete the consumer's `-lm` — adding a dependency could break your link.
    - **Read from the `.kama/deps` VIEW, not from `kama.lock`**: the view is flat and already transitive,
      and `kama.local.json` `overrides` repoint it without touching the lock, so a lock-driven walk could
      read settings from a dependency other than the one being compiled.
    - **The negative half is the load-bearing one.** A rule that propagated everything would pass every
      positive test. A dependency cannot reach the consumer's `cc`/`ar`/`sysroot`/`runtime`/`subsystem`,
      cannot impose `no-heap` (it changes what compiles, program-wide), cannot demand a `webgpu` SDK
      download, and cannot contribute a RELATIVE `-I` (it would resolve against the consumer's working
      directory and silently find the consumer's own `include/`).
    - **`emSettings` is an object, and the value's SHAPE decides its kind** — an array unions, a scalar
      is last-wins with the project winning. An array of raw `-sFOO=1` strings would have been `cflags`
      with extra steps, and the merge is the entire point: emcc is last-wins, so kama's own
      `-sEXPORTED_RUNTIME_METHODS` (emitted after the project's `cflags`) silently beat any project that
      set the same key — while simply moving the project later would have dropped the two names the
      stdlib's JS glue needs. ⚠️ Two dependencies disagreeing on a scalar must be recorded and reported
      AFTER the project merges: reporting during the walk refuses a manifest that had already settled it.
    - ⚠️ **`OUTPUT=OBJECT` with more than one translation unit was UNREACHABLE**, found writing the
      guard. The arity check sat in the single-invocation arm and counted `cFiles`, but two inputs mean
      `nJobs > 1`, so such a build took the per-TU path, compiled each input, and "joined" them with a
      command still carrying `-c`. No diagnostic, and an artifact nobody can use. Reachable before this
      campaign, via `needsGpu`.
    - ⚠️ **The reproducible-float fixture would have been VACUOUS.** It reproduces only under
      `--release`: in debug, `a * b + c` is emitted as `KAMA_ADD(KAMA_MUL(a, b), c)` and the
      overflow-checking macros already break the expression clang would have contracted. `run_tests.sh`
      builds every fixture in debug, so the planned `tests/*.d` fixture would have passed identically
      with and without the key. The oracle lives in `tools/check-buildsettings.sh` instead, and detects
      its own vacuity on a host with no FMA.
    - **The window/framework sub-complaint is answered by the project tier, not by hoisting kama's
      list.** A project with its OWN window seam now writes its own `link` + per-target `ldflags`, which
      is the right outcome: kama's hardcoded GLFW/framework list exists for a program that externs
      `kama_gpu.h`, and a project that does not is not entitled to track it.
    - What is left is the C++ half of `csources`, which is its own row.

  - **Declared NOT ours, and they agree** — the audio backend, WebGPU binding breadth, their RFC6455
    framing, module statics being per-isolate (correct behaviour), and a PATH entry that is a directory
    breaking `make` in the emscripten image. Their `ENGINE_TODO.md` holds those.

- **The docs taught a `@compileFor` spelling that silently deleted code — FIXED 2026-09-01.** Kept as a
  record because it is the house rule's own failure mode, caught by a user rather than by us.
  [SPEC.md](SPEC.md) and [KEYWORDS.md](KEYWORDS.md) both taught `@compileFor(NATIVE)` / `(WASM)` /
  `(EMBEDDED)` / `(WINDOWS)` — **four sites, one more than was reported** — and not one of those is a
  flag: a built-in target NAME deliberately does not become one, because gating on it would gate on how
  the build was *spelled*. A manifest build rejects the name; a **loose** build reads no manifest and
  treats an undeclared flag as inactive, so the documented spelling compiles clean and the declaration
  is **gone**, with the only symptom a missing symbol somewhere else — or nothing at all if both sides
  were gated. ⚠️ **`tools/check-compilefor.sh` §4b already proved the COMPILER rejects such a name,
  which is precisely why the docs drifting was invisible**; the guard now greps the docs too (§7, and
  it was arming-tested against an unfixed doc before being believed). Same commit dropped
  `a.byteLen()`, documented twice beside `dataPtr()` and — verified across every commit in the repo —
  **never implemented at all**; write `cast<usize>(a.length()) * sizeof(T)`.

- **The audio-seam cluster — found by the first external project, 2026-09-01.** Ten gaps hit designing
  one audio device seam, recorded together because they are one story and because every citation was
  re-read against this tree before it was filed. The stake in their words: their engine says *"the
  platform split is one mechanism throughout — a `type contract` with `@compileFor`-gated
  implementations"*, and **that is not true today and cannot be made true** — the first two below are
  exactly why, and their shipped platform seam had to push its split down into C `#ifdef`s instead.
  ⚠️ **Every claim here was verified against the compiler, and two of their three "small wins" are
  small while the third is not** — sizing a user's report is our job, not theirs.

  - **THE PLATFORM SEAM (`@compileFor` on `extern`; attributes on members; `InlineArray` bridges) — SHIPPED `0.9.131`.** `@compileFor` gates `extern "<h>";`, `extern fn` and `fnptr`;
    `@noheap` marks a method, `ctor`, destructor or operator; `InlineArray<T>#(N)` has `dataPtr()` and
    `view()`. Record in [SPEC.md](SPEC.md) (*Conditional compilation*, *No-heap subset*, the container
    section). Three findings worth keeping, none of them in the row as written:
    - ⚠️ **The row was a GRAMMAR-SHAPE problem, not eleven missing features.** `attribute_list` had
      exactly two arms — a twin of the `fn … block` free function and a twin of `field_declaration` —
      so every other form simply had no attributed twin. Hoisting the prefix to one arm each
      (`attribute_list plain_function_declaration`, `attribute_list plain_class_member`) gave all
      eleven forms attributes at once and DELETED both duplicated twins. Bison still reports exactly
      the one dangling-`else` conflict `%expect 1` accounts for.
    - ⚠️ **The `InlineArray` bridges were mis-sized here as "S … a plain consistency hole rather than a design question".**
      `dataPtr()` was one line; `view()` was not. It needed the `View<T>` instance force-registered, C
      emitted from the emitter (a runtime macro cannot name the program-specific `View_<T>`), the
      `Viewable<View<T>>` grant on the synthetic ClassInfo, and — the part no reading predicted — a
      PASS rather than a line in `registerFixed`, because whether `std::collections::View` existed yet
      depended on the user's unrelated imports. It also exposed `mintReturnTypeNode` having no route to
      an intrinsic receiver, so a `borrow` alias over an `InlineArray` came out untyped.
    - ⚠️ **The trap that would have made the `extern` gate a silent no-op:** `CEmitter::emit` (the single-TU
      `transpile` path) ran `emitIncludes` BEFORE `collectProgram`, i.e. before `pruneInactiveDecls`,
      while `emitProgram` had the order right. Reproduced before fixing; `check-compilefor.sh` now
      asserts it on that same path, and the guard was confirmed to FAIL against the unfixed compiler.

  - **`@noheap` IS TRANSITIVE — SHIPPED `0.9.132`.** A `@noheap` body may not call anything that
    allocates, at any depth; the diagnostic names the chain. The rule is one sentence: **infer where the
    compiler can see the callee, require a declaration where it cannot.** So an ordinary un-annotated
    helper is fine exactly when it is allocation-free (nothing in `lib/std` needed marking, and nothing
    was marked), while a `fnptr`, a bound function pointer, a contract member and a `virtual` slot are
    blind seams that must carry `@noheap` on the DECLARATION — checked against every implementation, the
    way `const fn` already is. Record in [SPEC.md](SPEC.md) *No-heap subset*. Four findings worth keeping:
    - ⚠️ **The detector is the gate, not a walker.** Allocation facts are recorded by `rejectIfNoHeap`
      itself and call edges by `emitReorderedCall` — the one function every resolved call funnels
      through — so detection cannot drift from the gate, because it IS the gate. A separate AST pass was
      the obvious design and is wrong twice: two of the seven allocation sites (boxing a primitive / an
      error into an owning contract handle) are TYPE-directed and invisible to any walker over source,
      and a second walker obliged to know every allocating node kind is exactly how `scanExprForGenerics`
      drifted by three node kinds and began failing open.
    - ⚠️ **The leaf decides everything, and `GlobalAllocator` is it.** Every chain ends at an `extern fn`,
      so propagation alone would have proven only "reaches no `new`" — and a container does not allocate
      with `new`, it goes through its `A: Allocator`. Naming that one leaf is what makes `list.add(x)` in
      an audio callback an error. It needs no annotation to stay precise: `A` is a type parameter, so
      `DynamicArray<T, BumpAllocator>` is a different monomorph reaching a different `allocate`, and the
      arena idiom real-time code actually uses stays legal for free. `deallocate` counts too — `free` can
      block on the allocator lock exactly as `malloc` can, which is what makes merely OWNING a container
      in the region a defect.
    - ⚠️ **The destructor edge is recorded from OWNERSHIP, not from the call.** RAII is what runs at the
      end of a real-time scope, and a `T__dtor(&x)` is emitted from ~20 places (scope cleanup, condition
      temps, assignment drops, match subjects, unwind paths). An edge duplicated across twenty sites fails
      open the moment one is missed, so the edge is taken in `recordDestructibleLocal` instead: declaring
      a destructible local IS the fact, and where the emitter chooses to run the destructor is a lowering
      detail the proof does not model. `emitDtorDefinition` also never set `_currentFunc` — harmless for
      the friend-accessor match it was written for, a silent mis-attribution for anything keyed on it.
    - ⚠️ **An intrinsic is invisible to the walk, and `string` is the one that mints.** A `string` method
      has no AST body, so it contributes neither an edge nor a fact — `"${a}${b}"` was rejected in a
      no-heap region while `a + b` was accepted, the same allocation with two answers. Derived from the
      RETURN TYPE (an intrinsic string method returning a new owned value allocates) rather than a list of
      names, so a method added later is covered the day it is added. ⚠️ Scoped to `String` deliberately:
      the smart-pointer intrinsics also return an owning value — `Shared.downgrade` hands back a `Weak<T>`
      — and allocate NOTHING, so a bare `ownsByValue(return)` rule would have rejected them.
    - ⚠️ **`T__copy(&(x))` was spelled at TWELVE sites; it is one `copyCall` now.** A deep copy of an owning
      type allocates by definition, and `foreach (string s in xs)` allocates once per element with nothing
      in the body that looks like an allocation (the iterator deep-copies to yield by value; `foreach (ref
      string …)` borrows and is legal). ⚠️ The refactor was proved codegen-neutral by diffing the emitted
      `.c` of every fixture across it — 680 files, zero differences — rather than by reading it.
    - ⚠️ **A prelude body can fail a `--no-heap` build the author never wrote.** Gating the string copy
      program-wide broke a program that sorts three integers, because `Template.part`/`hole` in the PRELUDE
      do `return copy this._parts[at]` and every prelude body is emitted. The stdlib's escape for exactly
      this (`@compileFor(!NOHEAP)`, as on `sort`) **cannot be spelled on a member**. So the copy fact is
      recorded for the analysis but rejected only under the ATTRIBUTE — a second concrete instance of why
      the flag half below is not a one-line change.
    - ⚠️ **A dead special case, caught by writing the fixture.** Smart-pointer drops were given a
      hand-written fact on the assumption their destructors are runtime C. They are not: `Owned<T>__dtor`
      is EMITTED and calls `GlobalAllocator.deallocate`, so the ordinary edge already reached the leaf and
      gave the better diagnostic. The real bug was elsewhere — a by-value smart-ptr PARAMETER is pushed
      onto the root scope through `_pendingParamDtors` and never reached `recordDestructibleLocal`, so
      `@noheap fn consume(Owned<Node> o)` compiled while a `DynamicArray` local one line away did not.
    - ⚠️ **The claim that motivated the whole row was invisible to the claim guard.**
      `check-doc-claims.sh` matched "IS a compile error" and the SPEC sentence said "MAKE every … a
      compile error", so the strongest promise in the section carried no fixture and went unpinned long
      enough to become false. The pattern is widened and the claim is marked. A claim regex that
      recognises one grammatical voice has a blind spot the size of the other.

    **THE FLAG HALF SHIPPED `0.9.147`, and the design question it was parked on had a wrong premise.**
    "A user-code/library distinction the emitter does not have" was true of `diagFile()`, which is what
    had been looked at, and false of the emitter: `declFile` is recorded at COLLECT time and carries the
    `<`-sentinel `checkReach` already reads. ⚠️ **Neither half of the test works alone, and each covers
    exactly the other's blind spot** — the sentinel says nothing about `lib/std`, which is parsed off disk
    under its real path, while the module-path test (collision-proof, since `std` and `core` are reserved
    segments) calls the PRELUDE user code, because the prelude has an empty namespace scope and
    `GlobalAllocator::allocate` renders bare. ⚠️ And `declFileOf` answers "" for a mangled
    `Class__method`, so a member must be judged by its OWNER's file or every method seeds nothing.
    The diagnostic anchors on the innermost user body — the frame holding the call the author can change —
    which also kept the message count at one per defect instead of one per stack frame.

  - **SHIPPED `0.9.160`/`0.9.161` — no `-fsanitize` flag remains, on any target or tier.** The consumer's
    finding was that kama's unconditional `-fsanitize=integer-divide-by-zero,shift-exponent,
    float-cast-overflow` (with `-fsanitize-trap`, so no sanitizer runtime) made emscripten refuse
    `-sWASM_WORKERS`, and with it AudioWorklet — no audio thread in the browser at all — and that the
    refusal was over-broad for trap-only mode. The answer taken was the second of the three they offered,
    on every target rather than wasm alone: the four faults are now the compiler's own checks in the
    emitted C (`KAMA_DIV`/`MOD`/`SHL`/`SHR`, `kama_f2i_chk`, and in debug `KAMA_ADD`/`SUB`/`MUL`/`NEG`
    plus the place operators for `+=`/`++`), which cost the branch the sanitizer already cost, print a
    message where `ud2` printed nothing, run the panic hook, and are what an `@onPanic` region can recover
    from. Release codegen parity is asserted by `tools/check-release-arith.sh`. (Wasm Workers also need
    SharedArrayBuffer and therefore COOP/COEP headers — a hosting constraint, not kama's.)
- **The defects the first external project's queue turned up, and what each one cost to find.** All
  reproduced against the shipped compiler before anything was written; two were fixed in `0.9.148` and
  `0.9.149`, one in `0.9.150`, and the residue below is what is left. The two newest (their KB-12 and
  KB-13, triaged 2026-09-04 against `0.9.164`) are the first bullets.

  - **The raw seam and the owning `static` — triaged 2026-09-04 against `0.9.169`, from their KB-14 and
    KB-15.** Both reproduce exactly (KB-15 at 527 MB vs 2.5 MB). They are ONE seam, and the decisive
    measurement is that it is load-bearing: a local `UnsafePtr<T>` element is deliberately untyped to
    ownership (`ptrLocalElemType` is kept out of `exprClass`, and its comment says why) so that
    `nd[i] = od[i]` in `DynamicArray.growTo` stays a bitwise relocate. Widening `exprClass` to local
    pointer elements FIXES KB-14 (the repro returns 14) and BREAKS 45 fixtures, every one failing with
    `cannot give out of a field/element` inside `dynamic_array.kama` — the same diagnostic the consumer hit
    trying `give slot[0]`. A third symptom found here: `drop(value: slot[0])` compiles and emits a literal
    `(void)0;`. ⚠️ **GOALS §3a/§3e answer this without a design doc**: a raw pointer is the FFI seam and
    "never general-purpose escape"; to persist or share, you OWN it. So `p[i] = v` keeps C semantics, and
    the work is the DIAGNOSTICS (refuse the silent `drop`; name the two spellings at the method call) plus
    removing the reason anyone owns through a raw pointer at all — which is the next bullet.
  - **A module `static` cannot own a destructible resource, and it was untracked.** `static World g =
    World.make();` is refused — "no destructible resources yet"; the gate's comment: "v1 has no static-dtor
    seam". This is WHY the first consumer callocs a `World`: on the web `main`'s frame is unwound while
    the rAF callback lives, so the one owner that outlives a frame is a static. The shape the goals give:
    `static Optional<Owned<World>> g;` — absence in the type (§3b), `match` forces the dead case exactly as
    `Weak.tryUpgrade` does — and the C callback receives a pointer borrowed from the owner and turns it into
    a `ref World` PARAMETER at one `unsafe fn`, which is measured working and dropping today (their own
    `place(b: ref slot[0])` is that shape). Three pieces: the dtor seam at BOTH teardown sites (the
    synthesized `main` after `kama_main`, and the isolate trampoline — statics are `KAMA_ISOLATE_LOCAL`),
    the static-initializer rule (`isConstInitExpr`) accepting a payload-less variant literal so the static
    can start as `None`, and the reassignment bug below, because assigning the static IS a generic-resource
    reassignment.
  - **A GENERIC resource reassigned from a fresh rvalue never drops the old value — found here, ours.**
    `slot = fresh();` on `Owned<T>`, `Shared<T>` or a user `Box<T>`: 503 MB over 2,000 iterations; the
    non-generic twin 1.8 MB; `slot = give t;` from a local 1.9 MB (so `tests/give_assign.kama`'s own shape
    is fine — and its comment's claim "b's old is freed" is true of that shape only, and unobservable by
    its exit code either way). Root: the fresh-rvalue reassignment branch in the assignment emitter is
    gated `_genericTypeInstOf.find(lty) == end()` on the claim that "the value-producing path further down
    already drops the old value" — that path is the `isIntrinsicColl` COLLECTION branch, so a generic
    resource matches neither and lands in a plain store. Fixture instrument: a dtor counter in a module
    `static` (`tests/out_arg_drops.kama`'s idiom) returning the count — exit-code observable on every leg,
    which an RSS oracle is not. ⚠️ And reducing it found a second divergence: a generic type's dtor naming
    that static directly is emitted BEFORE the static's declaration (`use of undeclared identifier` from
    clang, `kama check` clean); route through a helper `fn` for the fixture, and row it.
  - **`reproducible-float` does not propagate from a dependency — a defect in `0.9.169`.** Found by
    re-reading the consumer's `sim`/`tests` case against the shipped key: their raw `-ffp-contract=off`
    cflag now propagates, the first-class key that replaces it does not. It was grouped with
    `no-heap`/`webgpu`, and it is not alike — it cannot refuse code or demand an SDK; it can only turn
    contraction OFF, and it states a requirement of the DEPENDENCY's own arithmetic, which the consumer
    compiles.
  - **KG-15 in their doc is stale**: `Mat4 * Vec4` is caught by `kama check` today ("the right-hand
    operand expects a `Mat4`, so it cannot be given a `Vec4`"), and check and build agree.

  - **A statement-form `match` over a width-pinned enum whose arms all `return` fails
    `-Werror,-Wreturn-type` (their KB-12).** Thirteen lines: `type enum Tri : uint8 { A, B, C }` and a
    `fn int32 pick(Tri t)` whose `match` returns in every arm. `kama check` says OK and clang says
    "non-void function does not return a value in all control paths" — a check/build divergence, the
    class the harness's analysis-agreement phase exists to catch. ⚠️ **kama is not missing the analysis:**
    the same function with an `if` and no `else` is refused by kama's own "can reach the end of its body
    without returning a value" check, so the return-path walk correctly treats an exhaustive `match` as
    divergent. The C disagrees because `emitMatchDefaultArm` closes the `switch` with `default: break;`
    unless the match is VALUE-producing on a PINNED tag — the arm that already panics instead, added when
    `-Werror=uninitialized` hit the same CFG edge (a pinned enum lowers to `typedef uint8_t`, so clang sees
    256 values and three cases). Its comment declares the statement form correct "byte-for-byte", and it
    is, unless every arm diverges: then the same dead edge is a `-Wreturn-type` error. Measured siblings:
    the unpinned `type enum Tri { A, B, C }` builds and returns 3; the same match followed by `return 0;`
    builds. The fix is the panic arm in both forms on a pinned tag — the argument in that comment (dead on
    a safe path, a diagnosed abort for a raw integer from an `extern` or a deserializer) does not depend
    on whether the match produces a value. **Diff a fix against its sibling**: the value form was fixed
    for exactly this shape and the statement form was declared fine by construction.

  - **A file must import a type it never names (their KB-13).** Twelve lines, two files in one module:
    `decl.kama` exports `type enum Kind` and a `type value Holder { public int32 n; public Kind k; … }`;
    `main.kama` imports `Holder` and `makeHolder` only and declares `Holder h = makeHolder();`. Result:
    "`Kind` is declared in `src/decl.kama` and this file does not import it", against a file that never
    spells `Kind`. Located with a backtrace, not read: `checkDefiniteAssignment` classifies a local's
    fields as owning or not by calling `cType(f.type)` on each field of the class, `cType` resolves the
    field's type through `resolveUserName` with the FIELD's own identifier node as the site, and
    `checkReach` judges that node — text from the declaring file — against `refFilePath()`, the consumer
    being emitted. Measured shapes: a `type value` and a `type resource` holder both fire, an enum field
    and a `type value` field both fire, a resource holder fires **three** times (so at least two more
    read sites of the same shape exist); a discarded `makeHolder();` and an inline `makeHolder().n` do
    not, and a primitive-only holder does not. ⚠️ **This is the fixed `InlineArray`-size bug's family
    exactly** — a member's declared type resolved wherever it is READ — and that bug's record above says
    which fix is right: bake the answer at collect time, where the declaring file's scope is installed;
    the read-site scope swap is the measured wrong one (30 fixtures, 17 agreement pairs). "Is this field
    owning" is a fact about the class, not about the reader. **The position is a second wrong-file
    mechanism**: `site->line` is the field's line in `decl.kama` (7 in their repro, 5 with a value field)
    stamped with `main.kama`'s path — a 6-line file blamed at `7:0`. `run_tests.sh`'s
    `diag_position_faults` would flag it, in a `.d/` fixture that exercises it. Their cost statement is
    the one to keep: adding a typed field to a widely-held `type value` is a breaking change to every
    file that merely holds one, invisible from the type's own definition.

  - **A diagnostic can name the USER's file at a line that does not exist in it.** `diagFile()` prefers
    `_collectingUnitPath`, then `_emitDeclFile`, then the file being compiled — and for a prelude or
    stdlib body emitted in the HEADER pass the first two are empty, so the error is stamped with the
    user's path and the library's line number. An 8-line repro was blamed at line 69. ⚠️ **It has been
    worked around twice already rather than fixed**: the `GlobalAllocator` leaf stores no position at all
    (`AllocSite{…, 0, ""}`) and `CEmitter::line()` is deliberately NARROWER than `diagFile()` for the same
    reason, with a comment saying it must not be widened to it. `run_tests.sh`'s `diag_position_faults`
    would catch the class, but only where a fixture drives it, and none does.

  - **`exprClass` is unreliable inside a generic instantiation.** Adding the class-identity rule
    (`0.9.148`) made this visible: run inside a generic body it fires on the stdlib's own correct code,
    because in `Owned<T, A>.adoptIn` the assignment `this.alloc = allocator` has the FIELD answering the
    substituted `BumpAllocator` and the `A allocator` PARAMETER still answering the default
    `GlobalAllocator`. Same family as the generic-scan drift. The rule is gated on `_typeSubst.empty()`
    until this is fixed, so `Mat4 m = someVec4;` is caught in every position EXCEPT inside a generic body.

  - **No way to ask for reproducible floating point — SHIPPED `0.9.169`** as the manifest key
    `reproducible-float`. Measured here rather than taken from the report: **13** of 64 random `a*b + c`
    triples differ on aarch64-macos, 0 with `-ffp-contract=off` — and ONLY in a release build, which is
    the finding worth keeping (see the vacuous-fixture note above).

  - **An `InlineArray` field's SIZE had to be imported with the type — FIXED `0.9.150` (their KB-11).**
    Indexing `b.cells[0]` across a package boundary needed `geom::N` in the reader's import block; the
    same shape inside one package always compiled. ⚠️ **The diagnostic named the wrong problem
    entirely** — "raw pointer access requires an `unsafe fn`" for a missing-import bug, pointing the
    reader at `unsafe`. The mechanism is a SILENT SKIP: a member's declared type is resolved wherever it
    is READ, and `registerFixed` returns quietly when the size will not fold, so the field stopped being
    a container. The LAYOUT was never wrong — a consumer calling a method that indexes the field
    internally built and returned the right value. Fixed by BAKING the size at collect time, where the
    declaring file's scope is still installed. ⚠️ **The read-site fix was tried first and is wrong:**
    installing the owner's scope in `cTypeInInstance` for a non-generic class broke **30 fixtures and 17
    analysis-agreement pairs** — the five-site NsCtx partial-swap hazard, and the same seam as the
    `exprClass` row.

  **⚠️ What their queue is worth reading for.** KG-15 was filed as "ergonomic friction" and was a
  check/build divergence reaching four positions, not one; KB-10 was filed as an `InlineArray` problem and
  was `isConcreteTypeArg` not recognising a raw pointer, which broke EVERY generic inference over a
  pointer element; KB-11 was filed as an import problem and was a silent skip plus a size resolved
  in the wrong scope; KB-13 was filed as an import-rule problem and is the KB-11 read-site resolution
  again, at three or more sites. **All four were bigger than the report, in the same direction: a
  consumer describes the shape they hit, not the rule that is wrong.** Re-derive the rule before sizing
  the fix. KB-12 is the exception that proves it from the other side: filed as "emit a terminator after
  the match", it is the statement-form half of a fix that already exists, and the right change is the
  sibling's, not the one suggested.

  - **This bucket is EMPTY**, and what emptied it is worth keeping, because both items were parked on a
    dependency rather than on a judgement. The bucket said they waited on `@noheap` transitivity and the
    foreign-thread entry; transitivity shipped, which discharged half of that immediately.
    **`@noheap` on a `fnptr` type** then went from nice-to-have to needed by the very campaign that
    unblocked it — a `fnptr` is a blind seam the proof cannot cross, so the call became a HARD ERROR in a
    no-heap region with nothing an author could write to permit it — and it **SHIPPED `0.9.146`**:
    `@noheap` on the signature, every bind checked against it, the call then provable
    ([SPEC.md](SPEC.md) *No-heap subset*). ⚠️ **It was not the grammar change everyone assumed**, which is
    the reusable part: `attribute_list plain_function_declaration` already covered the `fnptr` arm and
    tree-sitter already had `optional(attribute_list)` on it, so the attribute PARSED and was rejected
    semantically — no bison edit, no regenerated `grammar.bnf`, no Zed pin. ⚠️ **And it exposed that the
    bind was checked in ONE position out of six:** `emitFnPtrBind` is the only caller of `sigMatches` and
    a local declaration is its only caller, so an assignment, an argument, a return, a field and a module
    `static` bound through `emitExpression`'s bare-name arm, which checked nothing — a shape-mismatched
    function bound there compiled, and the call passed the wrong argument count. **A per-region panic
    policy** was promoted by measurement rather than by argument — see the consumer's underrun numbers
    above — and **shipped in `0.9.162` as `@onPanic(recover: <literal>)`** (SPEC *Recoverable regions*):
    `setjmp`/`longjmp`, gated on `@noheap` plus a transitive no-destructible-local walk, the hook not
    run for a recovered panic. ⚠️ `__builtin_setjmp` is not supported on arm64 macOS, so `<setjmp.h>` is
    included only by a program that declares a region. ⚠️ **A "not scheduled" bucket whose reason is a
    dependency needs re-reading every time that dependency ships**, or it silently becomes a list of
    things nobody will look at again.

- **A METHOD and a CTOR cannot take type or `comptime` parameters** — only a free function can. The
  `type_params_opt` slot appears in exactly three grammar rules ([kama.y](../src/kama.y), the
  `function_declaration` arms); `method_declaration`'s four `FN` arms and both `CTOR` arms have none, so
  `fn T widen<T>()` inside a type is a *parse error*, not a diagnosed restriction. A generic **type**'s
  members are unaffected — they monomorphize per the enclosing type's parameters, which is the common case.
  What is out of reach is a member introducing a parameter the receiver does not have.

  **The idiom is a free function with bounds**, and the stdlib uses it throughout:
  `fn void sortWith<T, C: Order<T>>(View<T> items, ref C by)`
  ([lib/std/collections/sort.kama](../lib/std/collections/sort.kama)) is a free function precisely because
  `C` is a second parameter `View<T>` cannot introduce. The method form *is* expressible — take
  `ref Order<T> by`, a contract borrow — at the cost of dynamic dispatch where the free function
  monomorphizes to a direct inlinable call. So this costs **ergonomics, not capability or performance**,
  which is the same profile as the `fnptr` entry above and the same reason it waits. Note also that the
  turbofish's absence on a method is *not* an extra restriction: `3c9b441` removed the one receiver
  turbofish (`r.deserialize::<T>()`, sugar for a `__kamaDeserialize<T>` free trampoline), and with no
  generic methods a method turbofish has nothing to name. Reopen if a real API cannot be spelled either way.

- **A `comptime` parameter's type is an integer, `bool` or `char`** ([kama.y](../src/kama.y),
  `comptime_param_type`) — no compile-time float, array or struct parameter. This is where a
  user-writable "this argument must be compile-time constant" would come from. Compile-time values now
  live in their own list — `comptime(int32 N)` at a definition, `#(4)` at a use — so widening the set
  further is purely additive; see [SPEC.md](SPEC.md) *Generics*.
  ⚠️ **Rust has shipped comptime parameters since 2021 and still restricts them to integers, `bool` and `char`**,
  because a composite value in a parameter list has to be encoded into a mangled symbol name. That is the
  constraint, not an oversight to fix. The compiler can still *require* a constant argument for its own
  intrinsics — `Simd`'s `shuffle(pattern:)` does — the same by-name knowledge it has of `InlineArray`'s
  `get`/`set`/`length`.

- **A non-constant turbofish argument reports the wrong thing.** `shifted(x: 2)#(runtime)`, where
  `shifted` *is* a generic function, says *"turbofish type arguments are only valid on a generic function"*
  — which is false and points away from the real problem, that `runtime` is not a compile-time constant.
  A bad diagnostic rather than a hazard (the program does not build), but "the error names the wrong cause"
  is exactly what kama's diagnostics exist to prevent. Wants an `xfail` fixture in the same commit as the fix.

<a id="s3"></a>

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

<a id="s4"></a>

## 4. Reflection + serialization — remaining follow-ups (1.x)

Serialization ships today (by-value + object-graph + polymorphic contracts) with **two backends — `json` (text)
and `binary` (KBIN)** — see [SPEC.md](SPEC.md) "Serialization". What remains is additive library + hardening:

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T>#(N)` read; a bare `encode`/`decode` of an
  intrinsic/enum value; generic enums. (A `const` field is a separate general language gap — doesn't parse today.)
- **Binary backend follow-on (deferred).** `@bits(n)` bit-packing (tighter integers/bools), field-name
  interning, and a schema-locked *positional* mode (needs an emitter change; trades forward-compat for max
  compactness). Delta/snapshot replication stays ENGINE-level (above serde); generic byte compression is an
  io-adapter layer (§1 transform adapters), not a serde concern.
- **More back ends (library, no compiler change)** — YAML; **XML**/**HTML**. Each is a `Serializer`/`Deserializer`
  impl + `encode`/`decode`. `std::encoding::base64` is a separate small module.
- **A back end's ENTRY POINTS are a convention, not a contract** — the defect the line above quietly
  describes. `Serializer`/`Deserializer`/`Serializable`/`Deserializable` are real contracts
  ([prelude/global.kama:207](../prelude/global.kama)), but `encode`/`decode`/`decodeFrom` are **bare free
  functions**, duplicated per back end (`json.kama:198,538,546`, `binary.kama:253,263,270`) with nothing
  checking that a back end supplies them or that their signatures agree. "A drop-in twin of the JSON back
  end" is true only by discipline. Wants a `Format` (or `Codec`) contract carrying the three, so a back end
  is a checked implementation. It is also the source of the **one** name collision in the flattened-stdlib
  measurement (`encode`, json vs binary) — it surfaced while measuring a flattened stdlib for the module campaign. Take it with the std-lib cleanup pass, not before.
- **`@deprecated` attribute (language, adjacent)** — a declaration marker (rides the `@`-attribute infra)
  emitting a use-site warning. Its own small task.
- **Optional/default *function/constructor* parameters (language, adjacent)** — the "options struct with
  optionals" ctor pattern. A deliberate non-goal for now: named static factories + named params cover it.
  (Distinct from **default *type* parameters**, which shipped.)

<a id="s5"></a>

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
  | **AVR (Harvard) family** *(deferred — Cortex-M/RISC-V first)* | Four AVR-specific pieces: (1) ISR — `@interrupt("VECTOR")` → the `ISR(VECTOR)` macro (`<avr/interrupt.h>`), not the parameterless `__attribute__((interrupt))`; (2) Harvard `PROGMEM` — flash const data needs `PROGMEM` + `pgm_read_*` accessors (a flash pointer can't be plain-deref'd), so `@section` alone doesn't cover it; (3) toolchain — `avr-gcc`-only (clang/zig don't target AVR cleanly); (4) **`-mdouble=64` in the target's `cflags`** — avr-gcc still defaults to a 32-bit `double`, which `kama_runtime.h`'s `_Static_assert` rejects. It is the ONLY target in kama's spectrum that fails those asserts, and the assert is doing its job: without it, `bitcast<uint64>(d)` would pun an 8-byte union member against a 4-byte one and `kama_f64_bits` would `memcpy` 8 bytes out of a 4-byte `double`. A config line, not a language gap. |

  **Why kama fits:** no-GC + RAII → deterministic, no hidden pauses; allocation is explicit in the emitted C
  (greppable no-heap audit); trap lowering is dependency-free; `InlineArray<T>#(N)`, sized ints, and `unsafe`/`UnsafePtr`
  FFI already exist. **North star: blink an LED** (the embedded "first triangle"). **Start Cortex-M, not AVR**
  (`zig cc`/clang do `thumbv*-none-eabi` cleanly; pico-sdk is tidy; AVR pain comes later).

### Compile-time evaluation & platform-specific compilation — residuals

The const-eval ladder and decl-level conditional compilation are done ([SPEC.md](SPEC.md)). What is left:

- **Host-endianness flag → `htole`/`htobe` (small).** `std::num` `byteswap*` (pure value swaps) + `bitcast`
  ship, but *host-order* serialization helpers need a compile-time endianness fact pure kama arithmetic can't
  observe — a natural `@compileFor`-style built-in flag (`LITTLE_ENDIAN`/`BIG_ENDIAN`). Every current target is
  little-endian, so this is deferred until a big-endian target appears.
- **`comptime fn` nice-to-haves (deferred).** Named-arg reorder *inside* a comptime fn body; a **local**
  `comptime T X = f();` initialized by a comptime-fn call (module + type-associated const forms ship); a
  per-fn `@steps(…)` budget override; dual-use fallback emission. (`sizeof` inside a comptime fn body ships
  with M6 — `alignof` does not, and that is now a rule rather than a gap: see §2's layout entry.)
- **Platform tag-type compilation.** The `@compileFor`-gated contract-impl seam is the sanctioned platform-variance
  mechanism (per-platform `type` impls behind a platform-agnostic `contract`, exactly one survives) — NOT
  in-function branching / `#ifdef`. Extending it as new targets land is forward library/driver work.

<a id="s6"></a>

## 6. Concurrency — what is left above the primitives

The language primitives are done ([SPEC.md](SPEC.md#concurrency-)). The higher-level **job system and
event-loop scheduler are libraries** on them (Go/Erlang-style block-on-channel, deliberately **not**
`async/await` function-colouring) — see the engine track (§8) and
[WEB_FRAMEWORK_READINESS.md](WEB_FRAMEWORK_READINESS.md).

**The pool the job system is built from can now be sized to the machine** — `cpuCount()` plus
`parallel_spawn` shipped 2026-08-29 ([SPEC.md](SPEC.md#concurrency-)), so the library can construct itself
at the right width instead of hardcoding one. What remains for that row is scheduling, not spawning: a
fixed pool where one job blocks ties up 1/K of capacity no matter how K was chosen, and **head-of-line
blocking is the job system's problem to solve.**

- **Deferred (reopen only on a concrete case) — general shared-memory ("hybrid").** Co-equal shared-memory
  threading is *not* planned; it reintroduces the hazard the model removes. Capability is retained (via the
  `Atomic<T>` seam + immutable-`Shared` + disjoint `parallel_for`); only some ergonomics move behind the seam.

<a id="s7"></a>

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

<a id="s8"></a>

## 8. Engine track (product north star)

A portable lightweight **WebGPU** game engine — a product built *on* kama, **not** part of the language. Tiers:
**math types** (shipped) → buffers/bindings → first triangle (shipped, browser + native, `examples/webgpu`) →
scene/material. Depends on the 1.x systems (file I/O for assets, serialization for scenes). The kama-scoped
remainder is a thin safe `std::gpu` binding wrapper over the shipped `kama_gpu.h` seam; the engine *spine*
(buffer/pipeline/binding libraries, renderer) is the engine product. See
[ENGINE_READINESS.md](ENGINE_READINESS.md).

- **Safe `std::gpu` binding wrapper — no longer "optional polish".** `lib/std/gpu` holds a C seam and *no
  kama at all*. The wrapper is the missing kama file, shaped like `std::net`'s `type resource TcpStream
  { isize fd; }`: `Device`, `Surface`, `Buffer`, `Texture`, `TextureView`, `RenderPipeline`, `BindGroup`,
  `CommandEncoder`, each an `UnsafePtr` handle whose destructor calls the matching `wgpu*Release`. Plus a
  `Result<SurfaceTexture, SurfaceError>` acquire whose error enum **names `Occluded`**, and the two seam
  holes below (drawable size; the discarded event queue).
  - **Why it was promoted.** The first external project's KB-5 — an unbounded-allocation frame loop that
    reached 15.6 GB and took a machine down twice. ⚠️ **Be precise about which half would have caught it:**
    RAII would *not* have. That allocation is inside wgpu-native, triggered by ignoring an untyped status
    int; what prevents it is the **typed acquire result**. RAII prevents the *other* leak in the same file
    — `examples/webgpu/triangle.kama` hand-writes **five** `wgpu*Release` calls per frame, all on the happy
    path, so any early `return` added later leaks them. Two different bugs, two different halves.
  - **Scope line.** Exactly the handles the seam and the triangle already touch, and nothing above them.
    Buffers/bindings/pipelines *as an engine renderer* stay out; the same things *as released handles* are
    in. Bindings *breadth* is the engine's job, not the language's.

- **Dev-loop hot-reload — a *library* on two small compiler primitives that already ship.**
  - **Compiler primitives (ship — see SPEC *Exposing to a host*):** `kama build --shared` (`.so`/`.dylib`/`.dll`)
    and the `expose` keyword's C-ABI linkage are the *same* kama→host boundary the wasm exports and the scripting
    host (§7) use — so hot-reload needs **no new language surface**. One boundary, three consumers.
  - **Library:** the `dlopen`/`dlsym`/`dlclose` + file-watch + function-pointer rebind loop — pure FFI over
    `unsafe`/`UnsafePtr`, zero compiler changes. This is the bulk of the feature. (A Windows copy-before-load, so the
    on-disk `.dll` can be rebuilt while loaded, is a library concern.)
  - **Engine:** the *data-in-host, code-in-module* architecture (world state lives in the platform-layer arena,
    passed *into* the reloaded module) so a reload doesn't wipe the world. Prior art: Handmade Hero, Our
    Machinery, Unreal Live++, Godot GDExtension, Bevy `hot_lib_reloader`.
  - **Scope — desktop dev only.** dlopen is absent/forbidden on ship targets (no wasm dlopen; banned on iOS;
    Android/Quest only via a pushed `.so`). Cross-platform *shipping* scripting is the §7 VM, not this. This buys
    fast native iteration on Linux/Mac/Windows — enough to justify the two tiny primitives.

<a id="s9"></a>

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
- **Compile-time / suite-time.** Measured 2026-08-09/10 on a 10-core M-series host. The whole record is
  below; there is no separate design doc any more.

  **Shipped, in order of when the evidence justified it:**

  | | change | measured |
  |---|---|---|
  | 1 | `./dev matrix` stopped running the guard block twice (`KAMA_SKIP_CHECKS`) | −61 s from `matrix` |
  | 2 | one parallel guard runner, `tools/run-checks.sh`, glob-enrolled | guards 61 s → 38 s |
  | 2½ | `make -j` | cold compiler build 8 s → 3 s |
  | 3.1 | `kama query` takes an ordered question list — one analysis, N answers | `check-query` 20.9 s → 6.0 s |
  | 3.2 | `kama check --each` — N programs in one process, sharing prelude + import closure | agreement 22 s → 10 s |
  | **5** | **`kama build -j` — the C compiles run concurrently** | **user builds ~2× (below)** |
  | 6 | the harness itself: fifo-semaphore gate, per-run seam sweeps, multi-file leg fanned out | −30 core-s, −9 s wall |
  | **7** | **`-O2` on the compiler itself — it had never been optimized** | **front end 8.2× (below)** |

  `./dev test` **184 s → 122 s → 104.6 s**; guard block **61 s → 14 s**.

  ### Lever 7 — the compiler was built at `-O0`, and had been forever

  **The single largest lever in this section, found last, by taking a measurement the closure-pruning
  brief demanded before any code was written.** `Makefile`'s `CXXFLAGS` carried **no `-O` flag at all** —
  not in the Makefile, not in `tools/`, not in `dev`, not in either CI workflow. Every kama binary ever
  built ran unoptimized, **including the ones `release.yml` shipped**.

  It hid because the `-O` flags anyone looks at are the ones `kama build --release` hands the C compiler
  for the *user's* program (`-O3` native / `-Oz` wasm, `kama.driver.cpp`). That is a different codebase one
  level down, and "we do optimized builds" was true the whole time — about the other one.

  Measured (`OPT ?= -O2`, `examples/httpd`, same host). **Taken when httpd was 32 units; closure pruning
  has since made it 10, so these absolute numbers no longer reproduce** — the ratios are the claim:

  | | `-O0` | `-O2` |
  |---|---|---|
  | `kama check` front end | 338 ms | **41 ms** (8.2×) |
  | ├ closure-parse (32 units) | 174 ms | 13.7 ms (12.7×) |
  | ├ prelude-parse | 28 ms | 2.2 ms |
  | └ analyze | 135 ms | 25.1 ms (5.4×) |
  | `kama build -j 10` | 0.67 s | **0.37 s** (1.8×) |
  | `kama check --each tests/*.kama` (597 files) | 25.3 s | **4.65 s** (5.4×) |
  | `./dev test` wall | 122.0 s | **104.6 s** (1.17×) |
  | `./dev test` **CPU** | 391 s | **197 s** (**1.99×**) |

  **The suite halves in CPU but drops only 17 % in wall clock**, because it is already parallel and its
  remainder is external `clang`, running the fixture binaries, and bash — exactly what lever 6's
  decomposition predicted. CI, on 2-4-core runners, is CPU-bound and should see much more of the 2×.

  Correctness, in the order the evidence was taken — **the optimized compiler emits byte-identical C**:
  every `.c` and the `.gen.h` for `examples/httpd` compare equal, and `kama check --each` over all 597
  fixtures produces byte-identical diagnostics. `-Werror` is clean at `-O2`. A one-off **ASan+UBSan build
  of the compiler itself at `-O2`** ran the whole suite green (974 passed, 26 checks, zero sanitizer
  reports) — worth knowing because `./dev test san` sanitizes the *emitted programs*, never the compiler,
  so nothing else in the gate covers UB in the compiler's own C++.

  `-O1`/`-O2`/`-O3` landed within noise of each other (41.4 / 41.1 / 42.2 ms) and `-O3` built *and* ran
  slower, so there is nothing above `-O2` to chase. Kept overridable (`make OPT=-O0`) for compiler
  debugging and held by [`tools/check-opt.sh`](../tools/check-opt.sh). The from-scratch compiler build
  costs more now (8 s → 23.5 s serial, 3 s → 12.7 s at `-j10`), which is why both CI workflows build with
  `-j4`.

  ⚠️ **Everything measured before 2026-08-10 was measured against an unoptimized compiler.** The rows above
  are re-baselined; anything quoted elsewhere from that period is not. **`benchmarks/RESULTS.md`'s runtime
  columns were unaffected** — the bench builds `--release`, so the *emitted* code was always `-O3`; only its
  compile-time column measured the compiler. Re-run in full 2026-08-10: **kama 1227 ms → 908 ms** for the
  9 bench binaries, i.e. **3.38× C per binary → 2.33×**, while every other language in the cohort drifted
  5-19 % *slower* in the same run. The runtime rows moved 3-5 % together, which is that run's ambient
  noise, not a change — kama stays at C parity on compute. Since kama's figure is transpile-to-C **plus**
  clang, it can never beat C; 2.33× means the front end now costs about a third again what clang costs on
  the same code.

  ⚠️ **The LSP per-keystroke floor below (~85 ms, 86 % `analyze`) is a pre-`-O2` number and has NOT been
  re-measured.** `analyze` alone got 5.4× faster, so the floor certainly moved; the split is not published
  here again until someone takes the measurement rather than deriving it.

  **Lever 6 is where the suite's remaining time actually was, and it is not the compiler.** Decomposing
  the 82 s fixture phase: building all 597 standalone is 33.8 s, running the built binaries is 24.6 s, and
  ~22 s is bash. Cutting at that got −30 **core**-seconds but only −9 s of wall, because the old polling
  gate's latency overlapped with the other nine workers — a prediction of ~12 s that was simply wrong.
  What remains of the 22 s is spread across one `mkdir` and one warning-`grep` per fixture, the subshell
  each job needs, and the result writes. Nothing left there is worth a commit.

  **Lever 5 — `-j`, and it is the only one users feel.** A C compiler handed N sources in ONE invocation
  compiles them **serially**, and a program importing anything from `std` was 16-32 TUs at the time (a
  directory-module import then pulled in every file in the directory — closure pruning has since cut that,
  see below), so `kama build` used one core for ~70 % of its wall time.
  Now each TU is its own `-c` job and the objects are linked. Measured on `examples/httpd`, **then 32 TUs
  and now 10, so these absolute numbers no longer reproduce** — the ratio is what the lever claims:
  C phase 0.93 s → 0.27 s + 0.02 s link (**3.2×**), whole build **1.27 s → 0.67 s (1.9×)**. Degrades
  gracefully — ~1.3× on a 2-core machine — and gains exactly nothing for an import-free program (1 TU).
  Guarded by [`tools/check-build-jobs.sh`](../tools/check-build-jobs.sh); the harness pins
  `KAMA_BUILD_JOBS=1` because its own pool is already core-wide, which also keeps the suite covering the
  single-invocation path. `-j 1` is that path byte for byte.

  ⚠️ **`-j` does NOT move the bench's compile-time column, and should not be expected to.** That column
  builds `--release`, which folds the whole program into one unity TU — there is nothing to split.
  Measured over the 9 bench workloads: `--release` is **1.71 s at both `-j 1` and `-j 10`**, while the
  same nine in **debug** go **2.88 s → 2.04 s (1.41×)**. The win is a dev-loop win, and the bench measures
  release artifacts. If a number for the edit-compile loop is ever wanted, it needs its own debug row —
  don't "fix" the release one.

  **What is left is a product feature, not a suite lever — and two premises died proving it.**

  - **`zig cc` already does incremental rebuilds, so lever 4 (kama's own object cache) is mostly moot for
    bundled installs.** Measured, 32 TUs: one invocation is 4.13 s cold, **0.07 s warm, 0.11 s after
    editing one file** — it has a content-addressed per-TU object cache and re-compiles only what changed.
    That is strictly better than the cache §4 of the old brief designed (no cold-CI penalty, no `gen.h`
    invalidation problem). Per-TU `zig cc` cannot use it (bounded by zig's ~0.18 s process startup: 0.59 s
    cold *and* warm), which is why `-j` clamps to 1 for zig — parallelizing there would be 7× better cold
    and **5× worse in the edit-rebuild loop**. A slim install (the installer's choice whenever a system C
    compiler exists) uses clang, which has no cache, and gets the full `-j` win.
  - **An on-disk front-end cache does NOT fix the LSP's per-keystroke floor**, which is what the old brief
    claimed was its main justification. The LSP already caches parses in-process
    (`kama.driver.cpp` `g_parseCacheMap`); its steady state was **~85 ms/keystroke, 86 % of it
    `CEmitter::analyze`** over the whole closure plus prelude — ⚠️ **a pre-lever-7 number, now stale and
    not re-measured.** Serializing the AST and re-running collect leaves that untouched either way, which
    is the point that still stands. Whether the floor needs *incremental or cached analysis* is now an
    open question rather than a settled one — see §10.
  - **Declined: `kama build --each`** (batch the fixture builds in one process). Priced at ~9-10 s off a
    129 s suite for a 377-line refactor plus a harness restructure, and it gives users nothing. Not worth
    it; recorded so it is not re-derived.

  **Deliberately NOT taken: folding a build to a single TU.** The biggest raw number (24 TUs 0.52 s → 1 TU
  0.08 s, same binary) but it changes what kama *emits*, and multi-TU emission is load-bearing — the
  logging campaign fixed a whole multi-TU `static` hazard class (`adfffce`) that a single-TU suite would
  stop exercising. (`--release` native already folds to one unity TU, for cross-module inlining.)

- **Closure pruning — SHIPPED 2026-08-10.** A directory-module import used to compile the whole
  directory: `import { std::collections::DynamicArray };` pulled in all 14 files of `lib/std/collections`,
  because `resolveModuleFiles` falls back to a flat listing and the `{…}` names control *visibility*, not
  what gets compiled. `examples/httpd` named four imports and got 32 TUs, 20 of which contributed no live
  symbol. An import now resolves to the files defining the named symbols plus their transitive
  intra-directory closure. Resolution rule: [SPEC.md](SPEC.md#module-resolution). Guard:
  `tools/check-closure-pruning.sh`. Escape hatch: `KAMA_NO_PRUNE=1`, plus `KAMA_PRUNE_TRACE=1|2` for
  per-import decisions and the reference that pulled in each kept file.

  | examples/httpd | before (32 TU) | after (10 TU) | saving |
  |---|---|---|---|
  | `kama build -j 10` | 390 ms | **168 ms** | **57 %** |
  | `kama build -j 1` | 1011 ms | 355 ms | 65 % |
  | `kama check` front end | 43.9 ms | 29.2 ms | 33 % |
  | └ `analyze` | 26.5 ms | 14.2 ms | 46 % |
  | └ `closure-parse` | 14.9 ms | 12.8 ms | 14 % |

  Well past the ≥ 25 % rule fixed before the measurement, and past the 40.5 % the design brief predicted
  from a 32 → 17 hand-prune. The derived closure reaches **10**, and even the parse got cheaper: a module
  no kept file imports is never indexed at all. That also repaid the 1.1 ms the identifier set cost to
  collect, so the front end is now below its pre-campaign baseline on every phase.

  **It changes module semantics**, which is why it landed pre-1.0: a compile error in an unused sibling
  file no longer fails the build, and neither does a conformance that was arriving only because the whole
  directory loaded.

  **Five claims did not survive being run.** Three were caught by the Step-1 measurement and two by the
  implementation; each is recorded because each was asserted rather than measured.

  1. ~~"resolution-level pruning reaches most of the 20 dead units"~~ — the brief settled on 17 units
     remaining, reasoning that a named-but-dead generic TU can never be dropped. True, but it under-counted
     what the closure drops elsewhere: the answer is **10**.
  2. ~~"the intra-directory import graph is sparse, so `import` edges suffice"~~ — the *import* graph is
     sparse but it is not the closure. `priority_queue.kama` has **no `import` at all** and declares
     `DynamicArray<T, A> data;`: an unqualified name resolves against the file's own module scope
     program-wide, so the files of one module reference each other implicitly. The closure follows every
     identifier spelling instead, which is a superset of the references and cannot under-compute.
  3. ~~"parsing is the cheap part, so indexing the directory is free"~~ — at `-O0` parse was 54 % of the
     front end and the objection was real; lever 7 dissolved it. Indexing by **full parse** was the right
     call, and a lightweight declaration scanner would have been wrong for a second reason the brief did
     not have: `view.kama` declares `type view View<T>`, so the type-kind word is a bare identifier rather
     than a closed `value|resource|contract` set. A regex would have missed it.
  4. ~~"a nameless declaration is inert"~~ — two kinds are not, and both are invisible to any closure.
     `type intrinsic <int32> implements Parseable` registers a conformance for a *primitive* under no name.
     Worse, `spawn` and `parallel_for` require `extern "kama_isolate.h";` from
     `lib/std/concurrent/concurrent.kama` while naming nothing in it — and the demand is program-wide, so
     the `spawn` need not even be in the file that did the import. Both providers are marked unprunable.
  5. ~~"an `extern fn` is a declaration like any other"~~ — this one cost more than half the win.
     `extern fn` declares a C symbol, not a module definition, so several files legitimately repeat it:
     three of collections' fourteen each declare their own `extern fn memset`. A reference to `memset`
     from `fixed_array.kama` was dragging in `map.kama` and `bit_set.kama`, and `hasher.kama` behind
     `map`. A name a file declares *itself* is satisfied there and pulls in no sibling — which took httpd
     from 19 units to 10.

  **Residual, not scheduled.** Five dead-but-kept TUs need whole-program reachability rather than
  resolution-level pruning (`dynamic_array`, `fixed_array`, `view`, `ptr`, `stream` are near-empty because
  a generic materializes at its instantiation site, yet their symbols are genuinely imported). They are the
  cheapest population — one definition each — so the remaining prize is small. A bare `import a::b;` also
  loads the whole module by design: nothing pins a file, and seeding from the importing file's tokens would
  be unsound, since a type reached only through inference is never spelled.

<a id="s10"></a>

## 10. Tooling / distribution (deferred)

- **tree-sitter accepts 78 of kama's 80 reserved words as a binding name; the compiler accepts 2.**
  Measured 2026-09-02 across the full keyword table, in both type positions:

  ```kama
  Thing else = Thing.make();   // tree-sitter: a clean declaration. kama: parse error.
  isize break = 1;             // same, after a builtin type
  ```

  The two grammars reserve differently *by construction*, which is why this is a gap and not a typo.
  `kama.l` consults one table at **every** identifier, so a reserved spelling is refused everywhere.
  tree-sitter has `word: $.identifier` and extracts keywords **contextually** — a keyword literal is
  only recognised in states where the grammar expects it, and a binding site expects `$.identifier`,
  so every reserved word lexes as a name there. The two that agree, `slot` and `type`, agree because
  the *compiler* accepts them: they are the contextual pair.

  What it costs: every editor on this grammar (Zed, Helix, nvim-treesitter) renders
  `Thing else = …` as a valid declaration with `else` coloured as a variable, and the compiler then
  rejects it — the editor is confidently wrong exactly where a beginner is most likely to be.

  ⚠️ **`tools/check-treesitter.sh` cannot see this class, and one fixture hides it.** Oracle 5 asks the
  compiler only where the two disagree *about files already in the manifest*; detecting "kama raises a
  parse error but tree-sitter is clean" for the whole corpus needs a compiler run per file (~43 s), which
  the guard deliberately avoids. And `tests/xfail/reserved_word_as_name.kama` is in
  `test/parse-errors.txt` and passes — but its ERROR node is at the **use** site
  (`return cast<int32>(base);`), not the declaration. Delete that second line and the entry fails.
  Measured; it is the reason this was found at all.

  **Unprobed, and that is the first task, not the fix:** tree-sitter's keyword extraction has no "reserved everywhere" switch, so the
  fix is either an external scanner (`src/scanner.c` — the grammar has none today, and the `>>` note
  in its header records not needing one as a virtue) or a negative lookahead over 78 spellings baked
  into the `identifier` token. Which of those is tolerable is the question to answer first.


- **AI/agent tooling — SHIPPED.** `kama query --search NAME` / `--diagnostics` / `--json`, the
  `kama agents` command, and the `AGENTS.md` it writes into a project. Record: [agents.md](agents.md);
  guarded by `tools/check-agents.sh`. Residuals, none blocking:
  - **No name-based entry beyond `--search`.** There is no call hierarchy (`callers-of`) and no type
    hierarchy (`implementors-of`), though the contract rename group already holds the data.
  - **No stdin / unsaved-buffer mode.** Every query reads the file from disk, so an agent cannot ask
    about an edit it has not written out. The LSP can; the CLI deliberately cannot.
  - **One full `analyze()` per invocation** — the §9 front-end cache is the fix, and a batch mode the
    cheaper rung.

- **No `scripts` table in `kama.json`.** *(Decided; not scheduled — recorded so it stops being
  re-proposed.)* npm needed one because npm had no build system. kama has `kama build` plus `select`,
  `flags` and `TARGET` entries, so a named configuration is already declarative and portable, which a
  table of shell strings is not. It would also become a second, per-project, undocumented build system
  that every consumer has to read to learn what `test` means. If tasks are ever wanted, the 2.0
  scripting runtime (§7) is the vehicle — a task written in kama, not a shell string — and that is a
  reason to spend the design budget there rather than here.

- **`kama fmt` — a native formatter, not an external tool.** *(Unscheduled.)* The language should print
  itself: one canonical form, applied by the toolchain, so a project never argues about style and a diff
  never carries noise that is not a change. Driven by the project's `kama.json` (the same manifest that
  already carries the flag universe and the toolchain pin), with a small, deliberately non-negotiable set
  of knobs — indent width, line width, brace style — rather than a style language. `kama fmt --check` is
  one more `tools/check-*.sh`, glob-enrolled automatically.

  **This entry used to be scheduled with mandatory braces, and used to argue tree-sitter was the cheap
  substrate. Both were wrong; the corrections were measured, and are recorded here so they are not
  re-derived.**

  - **It was never the migration tool.** The brace rule's entire migration was **2 sites in 1 file** —
    2,116 bodies were already braced and there were zero same-line bare bodies. Braces shipped alone.
  - **Do NOT link the vendored tree-sitter parser.** `tree-sitter-kama/src/parser.c` is a generated
    *table* — the only `ts_*` symbols it defines are `ts_lex`/`ts_lex_keywords`. The parsing engine is
    tree-sitter's **runtime library**, which this repo does not vendor (three headers only; `node_modules/`
    is gitignored). Linking it means vendoring a third-party C runtime into `src/` permanently *and*
    making the editor grammar load-bearing for the compiler. The old "the CST is already trustworthy"
    argument is true and still does not reach that conclusion.
  - **Use the compiler's own front end.** Comments die in exactly three lexer rules (`kama.l`: the
    `{reserved_preprocessor}` and `{single_line_comment}` actions, and the `IN_COMMENT` start state).
    Retaining them is a `vector<Trivia>` on `LexerInstanceData` — the same struct that already carries
    `identTokens`, an `unordered_set<std::string>` taking *one insert per identifier token on a ~14 ms
    parse*. At ~8k comments corpus-wide the cost is below measurement noise, and a `bool keepTrivia`
    gates it to the fmt path anyway. This is also the only route that keeps one parser, and it serves the
    LSP.
  - **Byte offsets are not needed** — attaching a comment requires only a *monotonic* position, and
    `(line, col)` already is one. (An earlier brief claimed the opposite and would have sent the work
    through a much larger lexer change.)
  - **The cost is the printer, not the parser.** 72 AST node types; the C emitter needed ~530 dispatch
    arms over them. `CEmitter::unparseExpr` (`kama.cemit.cpp`) is a partial precedent — it renders
    expressions back to kama text for `assert` messages — but it covers ~20 node types, is deliberately
    lossy (`return ""` on anything unhandled), and drops literal spelling (`'a'` → `97`), so it is proof
    of shape, not a foundation. Retaining raw literal spellings is a prerequisite either way.
  - **Policy calls to make against a working printer, not in the abstract.** The corpus already agrees on
    4-space indent (zero tabs), operator/comma spacing, no paren padding, no trailing commas, and ≤1
    consecutive blank line — a formatter enforcing only those is a near no-op. What it would *change* is
    the contested part: **6,090** one-line `{ … }` blocks, **1,586** one-line `fn` bodies, **3,486**
    hand-aligned lines, a de-facto line width of **110** (picking 100 rewraps ~14.5% of the corpus), and
    brace style — `lib/`, `prelude/` and `examples/` are 100% K&R while `tests/`, `bench/` and, most
    visibly, both `seed/` templates carry Allman.
  - **Free leverage:** `tools/check-treesitter.sh` already maintains a 1,000+ file corpus both parsers
    agree on — a ready-made test set for the two properties that matter, **idempotence**
    (`fmt(fmt(x)) == fmt(x)`) and **semantic preservation** (reparse, or compare emitted C).

- **C symbol naming — folded into the module-system campaign, and SHIPPED in it** (`0.9.81`–`0.9.84`).
  Both defects were downstream of a model with two ways to name a thing, which is why they were folded in
  rather than fixed where they showed. A kama identifier that is a C keyword is now REFUSED at the lexer —
  kama reserves the whole C11 + C23 set — rather than emitted raw for clang to choke on; and a generated
  `.c` is named by its module while a file-private symbol is named by its file, with units emitted in a
  canonical order, so `--keep-c` is reproducible. The campaign's design doc was deleted when it closed, per this
  file's own maintenance rule; the record is the git log, SPEC's *Modules* section, and `docs/packages.md`.

- **Devirtualize a contract-value call in DEBUG builds.** `Comparable<int32> c = l; c.compareTo(other: r);`
  costs nothing at `-O2` — clang folds the `static const` vtable pointer, devirtualizes, inlines the thunk,
  and reduces the whole call to six instructions with no call at all. At `-O0` the indirection survives, so
  a debug build pays for a fat pointer whose target the compiler knew when it wrote it one line earlier.
  kama already does the analogous analysis for inheritance — `buildVtables` keeps a whole-program override
  index so a never-overridden slot lowers to a direct call ([kama.cemit.cpp](../src/kama.cemit.cpp), guarded by
  `tools/check-ecs-zero-dispatch.sh`). The contract-value case needs less: a local "this fat pointer's
  `vtbl` was assigned a known constant and never reassigned" check. Low priority — release builds are
  already optimal, and this only buys debug-build speed.

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
  - **The fixed prelude-ANALYSIS floor per keystroke** — see *Cache the toolchain front end* in §9, which
    is the same defect measured on the build path and is where the fix belongs. M5 removed the prelude
    *parse* from every keystroke; analyzing it again on every buffer change is what remains, and it is a
    floor no file can get under. ⚠️ **Re-measure before acting on it.** The ~85 ms/keystroke figure this
    was sized against predates §9 lever 7, and `analyze` alone got 5.4× faster when the compiler started
    being built optimized; **closure pruning then took `analyze` a further 46 % on httpd** (26.5 → 14.2 ms)
    by shrinking what a keystroke has to analyze at all. Two large cuts have landed under this number
    since it was taken, so whether the floor is still worth a campaign is an open question, not a settled
    one — and note the *prelude* share is the part neither cut touches.
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
  without vendoring — the point at which cross-package conformance coherence (SPEC § *`type intrinsic`*) becomes load-bearing.
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
