# The analysis gap — findings ⑩ and ⑪, and the test-infra holes (in-flight design)

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE for the next session.** [ROADMAP.md](../ROADMAP.md) rows 1–7 are this campaign, in order.
The design is settled: read *Decisions taken* and *Milestones* below, then start at **milestone 0**
(clear `_localCTypes`, its own commit) and **milestone 1** (the first diagnostic). The traps table is
not background — every row in it is a thing that will bite the implementation directly.

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
| **OPEN — this document** | ⑩ (no expression type-checking), ⑪ (silent narrowing), the test-infra holes |

## What the probes established

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

- `tests/trap/` is **skipped** under `KAMA_SAN`, `KAMA_WASM`, and on Windows/MSYS2 (`run_tests.sh:553`) —
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
| **0** | **Clear `_localCTypes` with its siblings.** Own commit, before anything reads it — see the trap table. | `cemit.cpp:15426`, `:15959`, `:16066`, `:16184` | ~10 LOC |
| **1** | **The first diagnostic.** A `TKind` classifier + `declTypeKind` + `rejectInitKindMismatch`, modelled on `rejectNullInit` (`:438`), whose own comment already names this gap. Call it from `rejectNullInit`'s two existing sites plus the primitive-local path. **Kind boundaries only** — reject `Str`↔numeric/`Bool`/`Char`/`Class`, `Bool`↔numeric, `Class`/`Contract`/`Enum`/`Sig`↔primitive; allow every integral↔integral, float↔integral, char↔integral. | `:438`, `:800`, `:3134`, `:3283` | ~250 LOC, 9 fixtures, **0** corpus migration |
| **2** | **⑪-const** — reject a provably out-of-range constant narrowing cast. Also fixes `constValue`'s `CastNode` arm, which treats a cast as value-preserving, so `InlineArray<T, cast<int8>(300)>` sizes at **300** while the runtime cast gives **44**. | `:2093`, `constValue:5933`/`:5949` | ~80 LOC |
| **3** | Extend the kind rule to **return**, **argument**, **variant payload**, **match arm**. `emitReorderedCall` is the single named-argument matcher for all 29 call forms, and its primitive by-value path falls straight through unchecked at `:12100`. | `:3761`, `:11677`, `:14692`, `:14215`; `emitOwnedValueInto:13897` covers return+arm together | ~200 LOC |
| **4** | **Duplicate-diagnostic dedupe** by `(ASTNode*, message)` in `unsupported()` — required before any check inside a generic type's member body, which is emitted once per instantiation. | `:178` | ~30 LOC |
| **5** | **`typeOfExpr` proper** — the total function with a distinguished `Unknown`, composed from the twelve existing partial resolvers. The resolution logic (name lookup, generic-instance binding, `This`, auto-deref, contract receivers) is already written inside `callReturnTypeRaw` and `exprClass`. | new | ~500 LOC |
| **5a** | **MEASURE** — a warn-only `--strict-numeric` mode over the corpus, with a taxonomy (literal-typed · `usize`-width · genuine widening · genuine narrowing). **Run it before AND after 5b**; the delta is how much of the migration contextual literal typing absorbs. | rides M5 | ~60 LOC |
| **5b** | **Contextual literal typing** (D2a). Generalize `pendingWideLits` (`kama.y:2020`) from one parked magnitude to any — it was built to defer exactly 2^31 for the unary-minus rule, which is the same mechanism. Move the "does not fit `int32`" diagnostic out of `makeUnsuffixedInt` (`kama.y:2007`) into the checker, since the parser cannot know the destination; `compilation_unit` already reports unclaimed parked literals. **Fixtures:** `int64 a = 4294967295;` must now *compile* (today a hard parse error demanding `4294967295i64`); `int8 s = 300;` must be rejected at the literal; `tests/int_literal_min.kama` must still hold. | `kama.y:1998`–`2031` + M5's target-type channel | ~180 LOC |
| **6** | **Strict numeric conversion** (D2) — the source-breaking rule **plus its corpus migration in ONE commit**. 5b must land first, or the migration carries thousands of literal suffixes 5b would have made unnecessary. | | ~200 LOC + migration sized by 5a |
| **7** | **⑪-runtime** (D1) — trap, plus `try cast<T>`. `try` is contextual and today parses only before `new`; extend to `cast`. Reuses `try new`'s `Optional<T>` static-result path. Fixtures: `tests/cast_try_ok.kama`, `tests/trap/cast_narrow_runtime`, `tests/xfail/cast_try_bad_type` (mirror `xfail/try_new_bad_type`). | `:2093`, `src/kama.y` | ~120 LOC |
| **8** | **⑩b-cheap** — a concrete-only template-body walk as a new pass after `checkDeclaredTypes` (`:19591`), reusing `collectBindings` (`kama.query.cpp:1441`), skipping any expression that mentions a type parameter. Catches unresolved names + concrete type errors in uninstantiated templates. | new pass | ~150 LOC |
| **9** | **⑩b-full** — opaque type parameters answering `findMethod` from declared bounds (`MethodInfo::whenParams`/`whenBounds`). Bounded quantification; **wants its own design doc.** | | ~600–900 LOC |

**0–4 are the spine and are independently shippable. 6 and 7 must precede the 1.0 tag (both are
source-visible). 8–9 are a parallel track and gate nothing.**

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
| ⚠️ **`analyze()` IS `emit()`** — `cemit.cpp:46` emits into a throwaway sink (`_analysisSink`, or one `ostringstream` for every stream multi-file). **So an emitter-path rule is in `kama check` for free**, and the agreement leg is satisfied by construction. An earlier draft of this brief said the opposite and would have sent the work into a standalone pass for no reason | the constraint that actually bites is different: the emit walk **skips template bodies** (`:20105`) and runs an instance body **once per instantiation** |
| ⚠️ **`exprClass` returns `""` for a primitive AND for "unknown"**, and ~40 callers depend on `""` meaning "not a class, take the raw-C path" | a checker built on it is either silent on every primitive (the exact bug) or fires on every unresolved name (unlandable). The new function needs a **distinguished `Unknown` that never diagnoses** — this is the single most important design constraint |
| ⚠️ **`_localCTypes` is never cleared between function bodies** — `emitFunction:15426`, `emitDtorDefinition:15959`, `emitMethodOrCtorBody:16066`/`:16184` all clear the sibling maps and not this one; the only clear is `:2727`, inside `parallel_for` worker synthesis | `lvalueCType:17311` and `receiverScalarCType:18599` consult it **before** the field lookup, so a stale entry shadows a same-named field of a different type in a later function. Latent today; a landmine the moment a checker reads it. **Milestone 0, its own commit** |
| **`cType` is not injective** — `char` → `uint32_t`, `usize` → `size_t` | key the checker on `primKey` (`:1009`), which exists for exactly this, and add its missing `usize`/`isize` arms |
| **Deliberate unknowns that must not fire** | array literals, bare variant ctors and value-producing `match` take their type from context (`_matchTargetCType`/`_variantTargetType`); `borrow` aliases have no written type; a primitive `match` payload binding has no type record at all; `foreach` bindings never reach `_localCTypes`; intrinsics register string args with `className == ""` on purpose; `extern fn` types are opaque |
| **`This` needs `_thisType` on the stack** or `cType` errors; **`Base` resolves only after `linkBases()`**; **`sig` forward declarations** are why `checkDeclaredTypes` runs last | a resolver that skips the `ScopedStr _ts(_thisType, …)` dance `callReturnTypeRaw:17372` does will false-positive on every `This`-returning method |
| ⚠️ **`tools/check-agents.sh:152`–`162` ASSERTS this gap still exists** — it fails if `kama check` starts catching expression type errors | **milestone 1 will fail it by design.** Delete the assertion and the caveat from `usage()` (`kama.driver.cpp:5183`–`5184`), `docs/agents.md` and `agents/AGENTS.md` in that same commit |
| **An `xfail/init_type_mismatch` would pass the xfail leg TODAY** — `kama build` already rejects it, via clang | what proves kama caught it is the `.msg` carrying kama's own wording, plus the agreement leg (`run_tests.sh:640`–`:651`), which fails today |
| the prelude is compiled INTO the binary | `./dev build` after any `prelude/global.kama` edit |
| a breaking rule and its corpus migration must land in ONE commit | `run_tests.sh` fails any fixture whose stderr matches `/warning/i`, and `unsupported()` prints `warning:` |

## Verification

- `./dev matrix > /tmp/m.log 2>&1; tail -5 /tmp/m.log`, then grep the **same** file. Once per milestone.
- Every fixture must be rejected by **`kama check`** as well as `kama build`.
- The probes in §1–§3 are the acceptance test: each must stop compiling (or start being diagnosed by kama
  rather than by clang), and the resulting diagnostics belong in the commit message.
