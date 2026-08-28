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
| const-generic parameter | open — the residue of that work: a `const N: int32` stands for a VALUE, and a probe has none to invent without deciding the template's own `comptime assert`. Concentrated in `lib/std/num/fixed.kama` | ~19 |
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

- **`Fixed<B, const F>` does not implement `Real`.** A contract requires *every* method, so conformance
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
  - **Nothing depends on it.** Const generics' three blockers are all const-generics-on-types issues;
    the view-escape check is independent; no site in `lib/`, `prelude/`, `tests/`, `examples/` or
    `bench/` needs it.

  Reopen only if a concrete case appears that a contract genuinely cannot express. Three real defects
  came out of scoping it and have shipped: a duplicate function declaration was silent (for a generic
  template the last body simply won), a function type parameter's `= Default` was parsed and dropped,
  and a type parameter shadowing a visible type said nothing. All three are pinned by `tests/xfail/`.

- **`--no-heap` does not gate container allocation.** The flag rejects `new`, string interpolation,
  `spawn`, `parallel_for` and error boxing (`rejectIfNoHeap`), but a `DynamicArray`/`Map`/`string` growing
  through `GlobalAllocator` reaches `malloc` unchallenged — so the flag under-delivers on what its name
  promises. M2a worked around this for the one case it introduced (`sort`/`sortWith` are
  `@compileFor(!NOHEAP)`, so they vanish from a no-heap build), but that is a spot fix, not the rule.
  The real change is to make `GlobalAllocator` growth an error under the flag, leaving containers usable
  only with an explicit arena/pool allocator — which is the MCU story anyway. Wide blast radius (every
  container use in a no-heap build), so it is its own campaign.

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

- **A negative claim in the docs has no guard unless an `xfail` fixture proves it.**
  `tests/idioms_kama_way.kama` compiles the docs' **positive** examples, which is why
  [coming-from-other-languages.md](coming-from-other-languages.md) cannot rot — but a sentence of the
  form *"X is a compile error"* is unverifiable that way, because you cannot put a rejected snippet in
  a fixture that must compile. SPEC claimed `foreach (char c in s)` "is a type error — the
  byte/codepoint distinction is enforced" and it was not enforced; the loop walked bytes and bound
  each to a `char`, yielding mojibake. Nobody noticed because all 17 `foreach`-over-a-string sites in
  the tree used one of the two *correct* spellings, so the mistake was never typed. Fixed
  (`tests/xfail/foreach_char_over_string`, `tests/xfail/foreach_elem_type_mismatch`), and a spot-check
  of 13 more negative claims found no others — but nothing *guarantees* the mapping. There are ~42
  such claims across SPEC/TYPE_MODEL/coming-from-other-languages against 557 xfail fixtures. Wants a
  guard that extracts the claims and requires each to name a fixture, which needs a machine-readable
  link between the two (a `<!-- xfail: name -->` marker beside the claim is the cheap shape).

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
  `@generate(Format)` draws — all of them now error rather than half-deriving; `Serialize`/`Deserialize`
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
  a const generic param could be passed to a generic call; it fails identically for a type param, so it
  is the general gap, not a const-generic one.

- **A generic `enum` cannot declare members or contracts.** `type enum Tag<const N: int32> { A; public fn
  int32 bump() { return N; } }` is rejected — "a generic enum is a monomorphization template, so each
  instance would need its own conformance". Clean diagnostic and a real limitation: it is why
  `EnumDeclarationNode`'s const-param data still has no reader after the const-generics campaign, since a
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
  widens the surface onto const-generic size expressions, defaulted allocator args and bounds, so it wants
  its own sweep. Guarded today by `tests/xfail/unknown_type_{local,param,return,field,method_param,
  variant_payload}` + `unimported_type_param`, and by `tests/decl_type_check_guards.kama` for the three
  shapes the pass must NOT reject (a generic free fn's own params, `This`, a `sig` used before its file).
- **`std::net` — IPv6 and UDP multicast.** `IpAddr` has a `V4` arm only ([`lib/std/net/addr.kama`]), left
  deliberately as an `enum` so a `V6(...)` arm adds without reshaping `SocketAddr` or any call site.
  Multicast join/leave (`IP_ADD_MEMBERSHIP`) is likewise unbuilt — broadcast covers LAN discovery today.
  Both are ordinary socket-option work on the shipped seam.
- **Fallible `new` is concrete-only.** `try new` / `new(allocator:)` support concrete `Owned`/`Shared`;
  the type-erased interface-element handle (`Owned<Contract>`) and the stateful-allocator form report "not
  yet supported" (`emitFallibleNewBox`). A follow-on to the MCU step-5 allocator work.
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
- **Every Windows binary kama emits is CONSOLE subsystem, including GUI programs.** Double-clicking the
  native `examples/webgpu` triangle opens TWO windows: the console Windows creates for a console-subsystem
  PE, and then the actual graphics window GLFW opens on top of it. Verified with `file` — `triangle.exe`
  and `kama.exe` both report `(console)`. A shipped GUI app is linked `-mwindows`
  (`-Wl,--subsystem,windows`), which suppresses the console; the cost is that `print`/`eprintln` then go
  nowhere unless the program attaches one, so it cannot simply be the default. It wants an explicit
  choice — a manifest field or a build flag — which is now a **solved shape rather than an open one**:
  runtime linkage took exactly that question and answered it with a `TargetSpec` field, a `kama.json`
  target key, and a CLI flag that wins over it (`runtime` / `--dynamic-runtime`, `kama.driver.cpp`;
  [targets.md](targets.md) § *Runtime linkage*). A `subsystem` key beside it is the obvious spelling.
  What is genuinely undecided is only the **default**, and unlike runtime linkage there is no
  can't-lose answer: console-by-default surprises GUI apps with a stray window, windows-by-default
  makes every `print` vanish. Pairs with the long-path item above — both are "what shape is a Windows
  application, as opposed to a Windows console tool".
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

<a id="s3"></a>

## 3. Open design questions (settle before the work they gate)

- **Should `spawn`'s disjointness check move from ROOT granularity to PLACE granularity?** The view
  model introduced `placePath()` / `placesConflict()` — a place is a base plus its chain of field
  names, and two places conflict iff one is a prefix of the other. `spawn`'s existing rule
  (`Scope::borrowedRoots`, pinned by `tests/xfail/scope_borrow_same_root.kama`) compares **roots**.
  Adopting the place test would unify the two predicates — one rule, which is what [GOALS.md](GOALS.md) §4
  asks for — and admit the disjoint-field case the ECS/engine shape wants.

  ⚠️ **PROBED 2026-08-27, and this entry's premise was wrong.** It said `w.bodies` and `w.springs` "are
  rejected as *the same root `w`*". They are not: `spawn` never reaches the root comparison, because it
  accepts only a **bare local** by `ref` — the argument must be an `IdentifierNode`
  (`kama.cemit.cpp:11970`), so a field expression is refused outright with a different message
  (*"`spawn` to `step` borrows — pass a bare local by `ref`"*). The restriction has its own stated reason,
  which the row never mentioned: *"a field/element root's lifetime we don't track"*.

  So this is **not** a predicate swap. The order is: (1) decide whether a `spawn` borrow may designate a
  place at all, which is a LIFETIME question about the field's owning root, not a disjointness one;
  (2) teach the borrow trampoline to pass `&w.bodies` rather than `&local`; only then (3) does
  `placesConflict` vs `borrowedRoots` matter. **It is a relaxation of a concurrency rule across a thread
  boundary, and it is not cheap to DO either — the `M?` was read off a mechanism that does not exist.**
  Settle the design first; do not fold it into a view commit.

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

- **Deserialize breadth** — `FixedArray<E>`/`InlineArray<T,N>` read; a bare `encode`/`decode` of an
  intrinsic/enum value; generic enums. (A `const` field is a separate general language gap — doesn't parse today.)
- **Binary backend follow-on (deferred).** `@bits(n)` bit-packing (tighter integers/bools), field-name
  interning, and a schema-locked *positional* mode (needs an emitter change; trades forward-compat for max
  compactness). Delta/snapshot replication stays ENGINE-level (above serde); generic byte compression is an
  io-adapter layer (§1 transform adapters), not a serde concern.
- **More back ends (library, no compiler change)** — YAML; **XML**/**HTML**. Each is a `Serializer`/`Deserializer`
  impl + `encode`/`decode`. `std::encoding::base64` is a separate small module.
- **A back end's ENTRY POINTS are a convention, not a contract** — the defect the line above quietly
  describes. `Serializer`/`Deserializer`/`Serialize`/`Deserialize` are real contracts
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
  (greppable no-heap audit); trap lowering is dependency-free; `InlineArray<T,N>`, sized ints, and `unsafe`/`UnsafePtr`
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
remainder is at most a thin safe `std::gpu` binding wrapper over the shipped `kama_gpu.h` seam (optional stdlib
polish); the engine *spine* (buffer/pipeline/binding libraries, renderer) is the engine product. See
[ENGINE_READINESS.md](ENGINE_READINESS.md).

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
     `type intrinsic <int32> implements FromStr` registers a conformance for a *primitive* under no name.
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
