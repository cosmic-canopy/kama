# The analysis gap — findings ⑩ and ⑪, and the test-infra holes (in-flight design)

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE for the next session.** Milestones **0 through 5b have SHIPPED** — see the table below for
what each one turned out to be. The next task is **milestone 6, strict numeric conversion**
([ROADMAP.md](../ROADMAP.md) row 1), then 7. **Row 6's size is settled at 103**: 5b-A drove 5a's `literal`
bucket from 219 to zero, so what the sweep still reports is what the strict rule will actually have to
migrate. **Read *The 5a measurement* below before scoping it** — it says something the brief did not
predict, and note that milestone 6 **will fail both boundary guards by design**. The
design is settled otherwise: read *Decisions taken* and the *Milestones* table. The traps table is not
background — every row in it is a thing that bit the implementation directly, and the ones marked ✅ are
the ones that already did.

> **Read this before the ROADMAP row it replaces.** Every claim below was **probed against the built
> compiler on 2026-08-15**, with the command recorded. The row this brief supersedes described ⑩ as
> *"an uninstantiated generic body gets no analysis"*. That is true, and it is **not the root cause** —
> probing found a larger one underneath it. The view-window campaign shipped a row asserting it closed
> findings ④⑤⑥ and closed none of them, because a plan's "closes" column got copied forward as a result;
> this file shows its work so that cannot happen a third time.

## Status

| | |
|---|---|
| **Closed by the view window** | ④⑤⑥ (the window rule, the freeze, `ref`/`out` view params, view-root aliasing) and ⑦ (`foreach` is a window; `mods` bumped in `growTo`). `1655e8a`…`19fc999` |
| **Closed by the spine (M0–M4)** | ⑩ *by kind* at all five hand-off positions, and ⑪ *for constants*. `10b8f30`…`9794284` |
| **Closed by M5/5a** | ⑩ *by type* at **six** positions (assignment was the sixth, and unwired), and the row-3 migration is now measured rather than guessed. `db9e3e1`…`c0be0ad` |
| **Closed by 5b** | CONTEXTUAL LITERAL TYPING, all three halves: a constant that does not fit its destination at all six positions (A), a wide unsuffixed literal a wider destination can claim (B), and a suffixed literal against its own suffix (C). `59edf78`… |
| **OPEN — this document** | ⑩ by WIDTH (milestone 6), ⑪ at runtime (milestone 7), the uninstantiated-generic half (8–9), the test-infra holes |

## What the probes established

> ⚠️ **These probes ran on 2026-08-15, BEFORE the spine.** §1 and §3's constant half are now CLOSED —
> both programs below are rejected by `kama check`, in kama's own words, and their repros live in
> `tests/xfail/init_kind_*` and `tests/xfail/cast_const_oob*`. §2 and §3's runtime half are still open
> and still reproduce exactly as written. Kept because the *reasoning* is what milestones 5–9 build on.

### 1. ⑩ is not about generics. **kama never type-checks an initializer at all.**

This is the finding that reframes the row. The ROADMAP described ⑩ as an *uninstantiated generic* problem.
It is not — the same hole is wide open in a plain, non-generic `main`:

```kama
fn int32 main() { int32 x = "not an int"; return 0; }
```

```sh
kama check c_plain.kama          # -> "OK (1 unit analyzed)"    <-- accepted
kama build c_plain.kama -o /tmp/cp
# -> clang: error: initializing 'int32_t' with an expression of incompatible type 'kama_string'
```

**The type error is caught by clang, against generated C the author never wrote — never by kama.** So:

- `kama check` accepts it, which means **the editor shows a broken file as clean**. That is the exact
  disagreement `run_tests.sh`'s analysis-agreement leg exists to catch, and the leg is real — it simply has
  **no fixture** of this shape. (The same pattern produced the `foreach`-rvalue defect closed in `25175fa`:
  a genuine hole, guarded in principle, unfixtured in practice.)
- The diagnostic a user actually gets names a C type (`kama_string`) and a C line number.

**This is the largest single correctness item left before the tag**, and it is a *missing subsystem*, not a
missing rule: kama has no expression type-checker. Scope it as one.

### 2. A never-instantiated generic body gets **no analysis whatsoever**

```kama
fn void wrong<T>(ref DynamicArray<T> d) {
    View<T> v = d.view();          // window-rule violation
    int32 x = "not an int";        // type error
    undefinedFunction(a: 1);       // unresolved name
    d.nonexistentMethod();         // unresolved method
}
fn int32 main() { return 0; }
```

```sh
kama check g_never.kama          # -> OK
kama build g_never.kama -o /tmp/gn   # -> "kama: built /tmp/gn"     <-- FOUR errors, builds clean
```

Not just types: **name resolution, method resolution and the view rules are all deferred to
instantiation.** This is the half the ROADMAP named, and it is the one that hits package authors —
ship `check`-green, consumers get the errors — but note it is a *second* defect, not the cause of §1.

⚠️ **Do not assume the two share a fix.** §1 needs a type-checker; §2 needs the analysis passes to run over
a template body with its parameters treated as opaque. §2 without §1 still leaves the plain case open;
§1 without §2 still leaves the generic body unanalyzed.

### 3. ⑪ is two problems, and only one of them is mechanical

```kama
int8 a = cast<int8>(300);   // CONSTANT, provably out of range  -> 44, silently
int32 big = 300;
int8 b = cast<int8>(big);   // runtime value                    -> 44, silently
```

Both truncate; the program exits 88. The **constant** case is statically provable and the compiler already
has the folder (`constValue`) — rejecting it is mechanical and clearly right, and there is precedent:
`xfail/constgen_oob` rejects a constant out-of-range array index while a dynamic one traps at runtime.

The **runtime** case is a **design question and needs a decision, not an implementation**:
- leave it (C/Rust behaviour — Rust's `as` truncates silently and offers `try_into` for checked), or
- trap at runtime like the bounds check does, or
- add a checked spelling and leave `cast` as the unchecked one.

⚠️ Decide this with the user before writing code. GOALS' *one way to do a thing* cuts against adding a
second cast spelling; the bounds-check precedent cuts toward trapping.

### 4. The test-infra holes are what let all of the above hide

- `tests/trap/` is **skipped** under `KAMA_SAN`, `KAMA_WASM`, and on Windows/MSYS2 (`run_tests.sh`, the `TRAP_OK`/`WASM`/`SAN_FLAGS` gate) —
  UBSan intercepts the trap and node's abort codes differ.
- **No leg runs MSan.** Per [[dev-infra]], MSan-origins catches what ASan and wasm both miss.
- **An `xfail` fixture never links, so it never reaches ASan.** This is why the view-window campaign's
  acceptance test had to be a probe ledger rather than a green suite — ④⑤⑥ survived all three legs and 34
  guards. Any campaign about *rejection* needs the same discipline.

## Decisions taken (with the user, 2026-08-15) — these are settled, do not re-open

| # | decision |
|---|---|
| **D1** | **A runtime narrowing cast TRAPS**, and **`try cast<T>(x) -> Optional<T>`** is the fallible form. Truncation needs no new spelling — mask first (`cast<uint8>(x & 0xFF)` is provably in range and never trips). Rationale: the safe cohort (Swift, Zig, Ada) traps by default and offers *named* alternatives; nobody makes the fallible form the only form, because most narrowing casts are ones the author knows are fine. **`try` is not a new concept** — [KEYWORDS.md](../KEYWORDS.md) already defines it as the non-panicking form of an operation, yielding `Optional` (`try new`, "the ONE fallible construction entry… there is no `tryAllocate`"), so extending it to `cast` keeps the rule one sentence. Verified precedent: bounds checks are unconditional — `include/kama_runtime.h:506`/`:434`-`442`/`:537` → `kama_bounds_fail:352`, and `grep -c NDEBUG include/kama_runtime.h` is **0**, so they survive `--release`. |
| **D2** | **The end state is NO implicit numeric conversion at all** (Rust/Swift/Go), not Zig's implicit-widening. Kind-only checking lands first as the engineering path, but **both land before the 1.0 tag** — the strict rule is source-breaking and 1.0 is the API-stability point, so leaving it half-done freezes loose conversions until 2.0. |
| **D2a** | **CONTEXTUAL LITERAL TYPING — change the node, do not carve out an exception.** A literal takes its type from its **destination**; `int32` only when nothing constrains it; the fits-check runs against *that* type. The design comment at `src/kama.y:1998` forbids typing a literal by its **MAGNITUDE** ("editing a constant could silently retype the expression around it") — the C/C++/C# "first type that fits" rule. **Destination-driven typing is not that hazard, it is its opposite**: the type comes from `int8`, which the author wrote on the same line. That comment also cites Rust, but Rust *does* type `let x: i8 = 4;` contextually and falls back to `i32` only when unconstrained — kama today is stricter than the language it cites. |

**⚠️ The corpus cost of D2 is not grep-able.** Two naive regexes find 126 sites in 31k lines (33
bare-literal inits of non-`int32` locals; 93 `usize`-returning calls into sized-int locals) and miss
assignments, arguments, returns, field inits and compound ops entirely. **M5a is the measuring
instrument; do not scope M6 before it has run.**

## Milestones

| # | milestone | key sites | size |
|---|---|---|---|
| **0** ✅ | **SHIPPED `10b8f30`.** Clear `_localCTypes` with its siblings. ⚠️ The brief called this latent; it was NOT — a `string` local in one body shadowed an `int32` FIELD in a later one, and the compiler **refused to build** valid code with an ownership diagnostic about a rule the program does not touch. Fixture: `tests/local_ctype_scope.kama`. | `emitFunction`, `emitDtorDefinition`, `emitMethodOrCtorBody` ×2 | 4 lines + 1 fixture |
| **1** ✅ | **SHIPPED `9df1fa5`.** `TKind` + `kindOfCType` + `declTypeKind` + `exprKind` + `rejectInitKindMismatch`, modelled on `rejectNullInit` and wired at its two sites (`checkDeclaredTypes`' field declarator, `emitStatement`'s local declarator — the primitive-local arm needed no separate call, the local loop already covers every declarator). **FOUR families, not the brief's list:** `Unknown`/`Num`/`Bool`/`Str`/`Aggregate`, with the rule "equal, or either is Unknown". `Char` collapsed into `Num` (a `char` IS integral, so char↔integral needs no third family) and `Enum` collapsed too (a payload-less enum is emitted as an integer; a payload enum is a ClassInfo, so `isClass` covers it). | `rejectNullInit` and its two call sites | ~230 LOC, 10 fixtures, **0** corpus migration |
| **2** ✅ | **SHIPPED `84df441`.** ⑪-const. ⚠️ `InlineArray<T, cast<int8>(300)>` — the named consequence — **does not parse**; a `cast` is not grammatical in a const-generic argument, so that path was never reachable. The reachable one is `comptime`, and it went the OTHER way: `comptime int32 N = cast<int8>(300);` was *accepted* while the identical runtime spelling truncated. The rejection therefore lives in the folder as well as the emit path. Migration: `tests/num_cast.kama` asserted the truncation. | `emitExpression`'s `CastNode` arm, `constValue`'s `CastNode` arm | ~90 LOC, 3 fixtures |
| **3** ✅ | **SHIPPED `9794284`.** The kind rule at the other four hand-off positions. `emitOwnedValueInto` covers return + all four `match`-arm sites with ONE call; argument and variant payload need their own. ⚠️ **ARGUMENT position is PARTIAL** — `ParamSig` records `className` and leaves it EMPTY for a primitive, so it reaches `string`/class params and is silent on numeric ones. Completing it means giving `ParamSig` a C type for primitives: a signature change reaching every construction site, including the synthesized intrinsic ones. | `emitOwnedValueInto`, `emitReorderedCall`, `emitVariantConstruction` | ~120 LOC, 4 fixtures |
| **4** ✅ | **SHIPPED `81b7612`.** Duplicate-diagnostic dedupe. ⚠️ Keyed on **(file, line, message)**, not the brief's `(ASTNode*, message)` — `unsupported()` is handed a LINE, not a node. Guarded in `check-diag-file.sh` (a count assertion has no shape in the xfail harness). It also made it safe to diagnose from `constValue`, which milestone 2 needed. | `unsupported()` | ~15 LOC |
| **5** ✅ | **SHIPPED `db9e3e1`, `a5b118d`, `c0be0ad`.** `typeOfExpr` — the lowered C type of any expression, `""` when not certain, `""` never diagnoses; `exprKind` is one line over it. ⚠️ **The brief's `ParamSig` claim was FALSE** (see below), and a **SIXTH hand-off position existed**: plain assignment, unwired by the spine, so `x = "oops";` still failed at clang. Two extractions the plan did not foresee: `indexElemTypeRaw` (`exprClass` filtered the user `operator[]`'s `ref T` through `isClass`, so a container of primitives answered "unknown") and `classifierCType` (`cType` DIAGNOSES on `This`/`Base`). | `exprClass`, `callReturnTypeRaw`, `receiverScalarCType`, `lvalueCType`, `operatorResultClass` | ~330 LOC, 10 fixtures, **0** corpus migration |
| **5a** ✅ | **SHIPPED `c0be0ad`.** `--strict-numeric`, hidden (no `usage()`, no docs) — a **TSV on stdout**, not warnings: the soft `warning()` channel was deliberately deleted and `run_tests.sh` fails any fixture whose stderr matches `/warning/i`. ⚠️ It reports **its own blind spot as a bucket** (`unknown-src`), which is what forced the arithmetic arm — that count was **29,282** without it and **1,752** with it, so the first draft measured 0.7% of the corpus and would have read as "no migration". Taxonomy below. | rides M5 | ~120 LOC |
| **5b** | **Contextual literal typing** (D2a) — **THREE problems with different sizes; see *Milestone 5b, scoped* below.** ⚠️ The earlier sketch here was wrong twice: there is **no "M5 target-type channel"** (`typeOfExpr` answers the SOURCE type; the destination was already at every site), and `pendingWideLits` is needed for only ONE of the two halves. **Measured before starting: 5b has ZERO corpus migration** — of 5a's 219 `literal` rows only 21 have a range-checkable destination and none is out of range, none of the 105 unsigned-wide rows passes a negative, and all 885 suffixed literals fit their suffix. | 5b-A: `rejectConstCastOverflow`'s callers · 5b-B: `kama.y` + a parser→emitter channel that does not exist yet | A: small · B: ~180 LOC |
| **5b-C** ✅ | **SHIPPED.** The suffixed range check, at every width. ⚠️ **Not the latent hole this brief filed it as** — the negative boundary was a live bug emitting invalid C; see the section below. | `createIntegerLiteralNode`, `WideLit`, `emitExpression`'s `Int64Node` arm | ~45 LOC, 4 fixtures, **0** corpus migration |
| **5b-B** ✅ | **SHIPPED.** A wide unsuffixed literal is built at its natural width and marked; a hand-off claims it, `emitExpression` reports one nothing claimed. ⚠️ **The parser→emitter channel this brief scoped was not needed** — the record belongs on the NODE. `pendingWideLits` stays, for 5b-C. | `makeUnsuffixedInt`, `negateWideLit`, `governWideLiterals`, `ASTNode::wideUnsuffixed` | ~80 LOC, 5 fixtures, **0** corpus migration |
| **5b-A** ✅ | **SHIPPED.** The fits-check at all six hand-off positions, reach = whatever `constValue` folds. **5a's `literal` bucket went 219 → 0**, so row 2 is now measured at 103. ⚠️ Two things the plan did not foresee: a `uint64` fold reads as NEGATIVE, and `usize`/`uint64` deserve a sign rule even with no asserted range. | `primIntRangeC`, `constOutOfRange`, `rejectConstOutOfRange`, `noteNumericHandoff` | ~90 LOC, 10 fixtures, **0** corpus migration |
| **6** | **Strict numeric conversion** (D2) — the source-breaking rule **plus its corpus migration in ONE commit**. 5b must land first, or the migration carries thousands of literal suffixes 5b would have made unnecessary. Note the kind rule already owns the four-family half, so 6 is purely about WIDTH. | `kindOfCType`'s callers | ~200 LOC + migration sized by 5a |
| **7** | **⑪-runtime** (D1) — trap, plus `try cast<T>`. `try` is contextual and today parses only before `new`; extend to `cast`. Reuses `try new`'s `Optional<T>` static-result path. The constant half already rejects there, so the site and the range helper (`primIntRange`) exist. Fixtures: `tests/cast_try_ok.kama`, `tests/trap/cast_narrow_runtime`, `tests/xfail/cast_try_bad_type` (mirror `xfail/try_new_bad_type`). | `emitExpression`'s `CastNode` arm, `src/kama.y` | ~120 LOC |
| **8** | **⑩b-cheap** — a concrete-only template-body walk as a new pass after the `checkDeclaredTypes(units)` call in `analyze`, reusing `collectBindings` (`kama.query.cpp`), skipping any expression that mentions a type parameter. Catches unresolved names + concrete type errors in uninstantiated templates. | new pass | ~150 LOC |
| **9** | **⑩b-full** — opaque type parameters answering `findMethod` from declared bounds (`MethodInfo::whenParams`/`whenBounds`). Bounded quantification; **wants its own design doc.** | | ~600–900 LOC |

**0–5a have shipped. 6 and 7 must precede the 1.0 tag (both are source-visible). 8–9 are a parallel
track and gate nothing.**

**What 5/5a closed, so 5b starts from the truth:** kama now answers *which type*, not just which
family, at **six** hand-off positions. It still does not check WIDTH — `kama check` catches
`int32 x = "oops"` and still passes `int8 a = big`, and both guards (`check-agents.sh`,
`check-query.sh`) pin **both** ends of that boundary, so milestone 6 will fail them by design.

## The 5a measurement — what row 3's migration actually is

769 files (`tests lib prelude examples bench`, xfail excluded), deduped:

| bucket | rows | what it is |
|---|---|---|
| `literal` | **219** | a literal into a differently-typed destination. **Row 2 (contextual literal typing) absorbs every one.** Spread across all six positions: 75 argument, 63 local, 41 assignment, 40 field |
| `narrowing` | **103** | ⚠️ **all `int32_t -> uint8_t`, and all one shape.** `fn uint8 hexDigit(uint8 v) { return 48ui8 + v; }` — C promotes two `uint8` operands to `int`, so the return narrows. Concentrated in `lib/std/serialization/json/json.kama` and the `ser_*` fixtures; 68 return, 34 argument, 1 assignment |
| `widening` · `signedness` · `int-float` · `usize-width` | **0** | none, anywhere |
| `unknown-src` | 1,752 | the instrument's blind spot: generic bodies (type-parameter operands) and MIXED-type arithmetic |

**The finding the brief did not predict.** The corpus is not loose about conversions — it already
spells them (161 `cast<usize>` alone, 335 `cast<int32>`), which is why `widening` is empty. What row 3
will actually hit is **C's integer promotion of sub-`int` arithmetic surfacing at a return**, and that
is 103 well-bounded sites in one subsystem, not a corpus-wide sweep.

## Milestone 5b, scoped — START HERE

**Every claim below was probed against `0.9.18` on 2026-08-16.** `.scratch/m5b/` held the probes; the
commands are one-liners, re-run them rather than trusting this table.

| probe | today |
|---|---|
| `int64 a = 4294967295;` | **Parse error** — "does not fit `int32`, the width of an unsuffixed literal" |
| `int8 s = 300;` | **accepted** (silently truncates) |
| `f(x: 300)` into `fn void f(int8 x)` | **accepted** |
| `fn int8 g() { return 300; }` | **accepted** |
| `int32 m = -2147483648;` | accepted — `tests/int_literal_min.kama` pins it, and it must keep working |

These are **three problems** (A, B and the C rider below), and conflating them is what made the
old estimate meaningless.

### 5b-A ✅ SHIPPED — a folded constant that does not FIT its destination

The rule reaches all six positions from the **two functions** the kind rule already funnels through
(`rejectInitKindMismatch`, `rejectValueKindMismatch`) — nowhere else holds a destination type. Reach is
whatever `constValue` folds, matching milestone 2's `cast` rule, so `int8 x = cast<int8>(300)` and
`int8 x = 300` no longer give two answers. `primIntRangeC` is the same table `primIntRange` holds, reached
from a lowered C type; `primIntRange` now routes through it so the numbers live in one place.

**The `literal` bucket is now 0** — 219 → 0, leaving `103 narrowing` + `1752 unknown-src`. That is row 2's
real size. Contextual typing is implemented as a **query, not an AST mutation**: `noteNumericHandoff` drops
a literal that fits, because such a literal already *is* of the destination's type. Retyping the node would
be wrong under a second instantiation and would change `primKeyOfLiteral`'s method dispatch.

⚠️ **`constValue` folds into an int64, so a `uint64` source above INT64_MAX comes back NEGATIVE.** Read at
face value that rejects `uint64 w = 18446744073709551615ui64;`, which `tests/int_literal_wide.kama` pins as
correct. The `srcUnsignedWide` flag is that carve-out, and it also lets the rule catch the case the `cast`
rule still misses (that magnitude into an `int64` destination).

⚠️ **`usize`/`uint64` have no asserted range and still get a rule**: negative is out of range on every
possible target, and that answer needs no width. `usize n = -1;` was accepted before.

**The record below is what it was.**

`int8 s = 300;` parses fine: it is an `Int32Node`, and nothing checks it against the destination. This
needs no grammar change and no literal retyping — it is milestone 2's rule at a different site.
`rejectConstCastOverflow(target, value, lo, hi, line)` already exists and already says the right thing
for `cast<int8>(300)`; `primIntRange` gives the range from a declared type and `constValue` folds the
value. Wire that trio at the six hand-off positions the kind rule already visits.

⚠️ **Corpus impact is UNMEASURED.** The population it draws from is 5a's 219 `literal` rows, but 5a
records only that source and destination types differ, never whether the value fits — most of those 219
presumably do. Measure before scoping: the cheapest way is to run 5b-A's check in warn-only mode over
the corpus the way `--strict-numeric` already does.

### 5b-B ✅ SHIPPED — a literal too wide for `int32` whose destination is wider

⚠️ **The parser→emitter channel was not built, and was not needed.** The brief's design put the record of
"this literal has no width yet" in `CodeGenContext::pendingWideLits`, a table that dies with the parse —
hence the channel. Putting it **on the AST node** (`ASTNode::wideUnsuffixed`) removes the problem: the
emitter already holds the node. `makeUnsuffixedInt` builds a wide literal at its NATURAL width and marks
it; `governWideLiterals` claims every marked literal reachable from a hand-off's value, called from the
same two functions 5b-A hooks; `emitExpression` reports one nothing claimed. Governance is recorded in a
**set on the emitter, not on the node** — the emit walk runs once per instantiation and again for a build
after an analyze, and mutating shared AST across those passes would make the second pass silent.

⚠️ **`pendingWideLits` was NOT retired** — 5b-C still needs it. A suffixed boundary literal states its own
width, so no destination may claim it and its report belongs at end of parse, where full coverage
(including uninstantiated generics) comes free. Only the unsuffixed producer moved off it.

**Two magnitudes stay parse errors**, because no destination could rescue them: past `uint64`'s maximum,
and a negated magnitude past `int64`'s. The negation folds at the `MINUS` rule (`negateWideLit`), which is
also where `-2147483648` keeps producing an ordinary `Int32Node` — and `-9223372036854775808` now has an
unsuffixed spelling for the same reason.

**Accepted cost:** the emit walk skips uninstantiated template bodies, so an ungoverned wide literal inside
a generic nobody instantiates is no longer caught. It mentions no type parameter, so milestone 8's
concrete-only walk is positioned to reach it — an expectation, not a guarantee.

**The record below is what it was.**

`int64 a = 4294967295;` dies in `makeUnsuffixedInt` ([kama.y](../../src/kama.y)) before any destination
is known. This is the half `pendingWideLits` is for — but note what is actually parked and where:

- `pendingWideLits` holds **exactly one magnitude**, 2^31, parked so the unary-minus rule can claim it
  (`kama.y:1542`, `takeWideLit`) and make `INT32_MIN` writable. Everything larger is a hard error on
  the spot. Generalizing it to *any* magnitude is the change.
- ⚠️ **`reportPendingWideLits()` runs at `kama.y:413` — inside `compilation_unit`, at END OF PARSE.**
  So an unclaimed parked literal is a *parse* error, reported long before the emitter runs. For a
  DESTINATION to claim one, that report has to move to after resolution.
- ⚠️ **There is no parser→emitter channel for it.** `CEmitter` holds only `_synthCtx`, a context it
  makes for its own synthesized nodes — not the parse's `CodeGenContext`. Building that channel is the
  real work here, and no earlier draft of this brief accounted for it.

### 5b-C ✅ SHIPPED — a SUFFIXED literal is now range-checked against its own suffix

⚠️ **"Corpus-clean, so a latent hole and no live bug" was half wrong.** `300i8` was latent, as recorded.
The NEGATIVE boundary was not: `-128i8` emitted `(--128)`, which C reads as a pre-decrement, so
INT8_MIN/INT16_MIN/INT32_MIN/INT64_MIN had **no suffixed spelling at all** — a clang error against
generated code, with `kama check` green. The parked-magnitude mechanism `pendingWideLits` already held for
the unsuffixed 2^31 generalizes to every width unchanged (the node holds the wrapped value, `MINUS` claims
it, end-of-parse reports leftovers); only its message had to move into the entry, since the two producers
word it differently. One thing the plan did not foresee: **INT64_MIN has no C literal either**
(`-9223372036854775808LL` is a magnitude past LLONG_MAX), so `emitExpression`'s `Int64Node` arm now writes
`(-9223372036854775807LL - 1)`, which is what `<stdint.h>` writes.

**The record below is what it was.**

Filed today under ROADMAP row 30 ("remaining language limitations") and reasoned about in
[§2](../ROADMAP_DETAIL.md#s2), but it is the SAME rule as 5b-A and should ride with it rather than wait
for a campaign of its own. Verified still live on `0.9.18`:

```kama
int8 a = 300i8;            // -> 44, silently. `kama check` says OK; the program exits 44
int32 b = 2147483648i32;   // -> INT32_MIN, silently
```

`createIntegerLiteralNode` ([kama.y](../../src/kama.y)) narrows with a C cast and never compares. The
unsuffixed path checks; the suffixed one — where the author has *stated* the width — does not, which is
the wrong way round. Fix = the same range test against the suffix's width, with the negation fold
extended to cover `-128i8` / `-2147483648i32`. **Corpus-clean** (no suffixed literal anywhere exceeds
its suffix), so it is a latent hole and carries no migration. Delete the row-30 entry when it lands.

### Ordering

5b-A and 5b-C are the same rule at two sites and should land together; neither needs the grammar
channel. 5b-B is the one that does. **All three before milestone 6**, or its migration carries literal
suffixes 5b would have made unnecessary.

**Re-run 5a after 5b** — the `literal` bucket should go to zero, and what remains is row 6's real size.
The command:

```sh
find tests lib prelude examples bench -name '*.kama' -not -path '*/xfail/*' \
  | while read -r f; do ./kama check --strict-numeric "$f" 2>/dev/null; done \
  | grep '^kama-strictnum' | sort -u > /tmp/sn.tsv
cut -f6 /tmp/sn.tsv | sort | uniq -c | sort -rn
```

**Where the checker lives: in the emitter, not a standalone pre-pass.** The destination type at every
site is a *derived* value — `_localCTypes` / `_localTypeNodes` / `_currentReturnCType` / `_typeSubst` /
`_matchTargetCType` / `_thisType` are all populated as side effects of the emit walk, so a pre-pass
would be a second implementation of scope tracking, generic substitution and namespace resolution,
free to drift. The two things the emit walk cannot reach get the two separate answers above
(milestone 4 for duplicates, milestone 8 for templates).

## Test-infra (tracked separately — it gates nothing here)

An MSan leg, and `tests/trap/` running somewhere it currently does not. Listed in ROADMAP.md as its
own row rather than folded into this campaign.

## Traps — each already cost something in this repo

| trap | consequence |
|---|---|
| **A doc is not evidence.** This row's own description of ⑩ was wrong about the root cause | a brief that starts from the ROADMAP would build a generics fix and leave the plain case open |
| **A green `./dev matrix` proves little for a REJECTION campaign** — an `xfail` never links, so it never reaches ASan, and the analysis-agreement leg only covers fixtures that exist | write the `xfail` first; the acceptance test is a probe ledger |
| ⚠️ **`analyze()` IS `emit()`** — `CEmitter::analyze` emits into a throwaway sink (`_analysisSink`, or one `ostringstream` for every stream multi-file). **So an emitter-path rule is in `kama check` for free**, and the agreement leg is satisfied by construction. An earlier draft of this brief said the opposite and would have sent the work into a standalone pass for no reason | the constraint that actually bites is different: the emit walk **skips template bodies** (`emitModuleContent`'s `fn->typeParams` continue) and runs an instance body **once per instantiation** |
| ⚠️ **`exprClass` returns `""` for a primitive AND for "unknown"**, and ~40 callers depend on `""` meaning "not a class, take the raw-C path" | a checker built on it is either silent on every primitive (the exact bug) or fires on every unresolved name (unlandable). The new function needs a **distinguished `Unknown` that never diagnoses** — this is the single most important design constraint |
| ⚠️ **`_localCTypes` is never cleared between function bodies** — `emitFunction`, `emitDtorDefinition`, `emitMethodOrCtorBody` (entry and exit) all clear the sibling maps and not this one; the only clear was inside `parallel_for` worker synthesis | `lvalueCType` and `receiverScalarCType` consult it **before** the field lookup, so a stale entry shadows a same-named field of a different type in a later function. Latent today; a landmine the moment a checker reads it. **Milestone 0, its own commit** |
| **`cType` is not injective** — `char` → `uint32_t`, `usize` → `size_t` | key the checker on `primKey`, which exists for exactly this. ⚠️ Its missing `usize`/`isize` cannot be switch ARMS — neither has a `builtInVal`; `cType` handles them by string compare |
| **Deliberate unknowns that must not fire** | array literals, bare variant ctors and value-producing `match` take their type from context (`_matchTargetCType`/`_variantTargetType`); `borrow` aliases have no written type; a primitive `match` payload binding has no type record at all; `foreach` bindings never reach `_localCTypes`; intrinsics register string args with `className == ""` on purpose; `extern fn` types are opaque |
| **`This` needs `_thisType` on the stack** or `cType` errors; **`Base` resolves only after `linkBases()`**; **`sig` forward declarations** are why `checkDeclaredTypes` runs last | a resolver that skips the `ScopedStr _ts(_thisType, …)` dance `callReturnTypeRaw` does will false-positive on every `This`-returning method |
| ✅ **TWO guards asserted this gap, not one.** The brief named only `tools/check-agents.sh`; `tools/check-query.sh` carried a byte-identical assertion. Five sites in all, with `usage()`, `docs/agents.md` and `agents/AGENTS.md` | Both were flipped in milestone 1 and now pin BOTH ends of the boundary — `check` MUST catch a kind mismatch and MUST still pass a width one. `docs/agents.md` also credited the wrong guard |
| ✅ **`kama_string` is itself a registered class**, so an `isClass` test placed before the primitive names classifies every `string` as an aggregate | It rejected `string val = "";` on line 432 of the PRELUDE. Primitives are a closed set — test them first |
| ✅ **A CONTRACT is not an aggregate for a kind rule.** `Hashable h = n;` over an `int32` is a shipped feature, and so is its boxed form `Owned<Hashable> b = 20;` | Three fixtures rejected (`comparable`, `intrinsic_widen`, `intrinsic_widen_box`). A destination that admits every kind cannot discriminate on kind — contracts, `sig`s and smart-pointer boxes over a contract are all `Unknown` |
| ✅ **An xfail for a type error would pass the xfail leg on its own** — `kama build` already rejected it, via clang | What proves *kama* caught it is the `.msg` carrying kama's own wording, plus the agreement leg (`run_tests.sh:640`–`:651`). Held for all 17 fixtures the spine added: none needed an `analysis_skip` entry, so `check` does the rejecting. **A fixture must land in the SAME commit as its rule** — landing it first makes the agreement leg red |
| ✅ **`ParamSig` DOES carry a C type for a primitive.** Milestone 3 recorded the argument position as partial because "`className` is EMPTY for a primitive". It is not: `paramSigsOf` fills it with `cType(p->type)` for every DECLARED parameter, and `f(x: "str")` on `fn void f(int32 x)` has been rejected since M3 | The real gap was the **synthesized intrinsic** signatures, which name no parameter type at all. Much smaller than the "signature change reaching every construction site" the note predicted |
| ✅ **…but the type cannot go in `className`, and this cost a matrix cycle.** That field also drives `ownsByValue`, and the read-only `kama_string__*` intrinsics deliberately BORROW their string argument — the trap table's own "Deliberate unknowns" row said so, and the call site says so in a comment | Naming it there made **51 fixtures** demand a `give`/`copy` marker for `s.contains(...)`. The checker got its own field, `ParamSig::kindCType`, consulted only where `className` is empty |
| ✅ **`ParamSig::byRef` had no default initializer.** A `ParamSig` built field-by-field left it indeterminate | `InlineArray_uint8_256__get` was handed `&i` where it wanted a `size_t`. **Clean on the host leg, failed on the container** — a bug whose existence depended on stack contents. Now defaulted |
| ✅ **There were SIX hand-off positions, not five.** Plain assignment (`x = expr;`) is not an initializer and the spine never wired it | The campaign's headline defect survived one statement past the fixture pinning it: `int32 x = 0; x = "oops";` passed `kama check` and failed at clang. `tests/xfail/assign_kind_int_from_string` |
| ✅ **A measurement that hides its blind spot is worse than no measurement.** 5a's first draft dropped every hand-off whose source it could not type | It reported 219 rows, all `literal`, reading as "row 3 has no migration". Adding an `unknown-src` bucket showed **29,282** dropped — 0.7% coverage. Modelling same-type arithmetic (C's rule stated exactly, not a guess) took it to 1,752 and surfaced the 103 real narrowings |
| the prelude is compiled INTO the binary | `./dev build` after any `prelude/global.kama` edit |
| a breaking rule and its corpus migration must land in ONE commit | `run_tests.sh` fails any fixture whose stderr matches `/warning/i`, and `unsupported()` prints `warning:` |

## Verification

- `./dev matrix > /tmp/m.log 2>&1; tail -5 /tmp/m.log`, then grep the **same** file. Once per milestone.
- Every fixture must be rejected by **`kama check`** as well as `kama build`.
- The probes in §1–§3 are the acceptance test: each must stop compiling (or start being diagnosed by kama
  rather than by clang), and the resulting diagnostics belong in the commit message.
- ⚠️ **Do not trust a line number in this file.** `src/kama.cemit.cpp` is ~20k lines and moves under every
  commit; the spine alone shifted it by 257. Everything above names a SYMBOL for that reason — grep for it.
  Line numbers survive only where they point into a file this campaign does not edit.
