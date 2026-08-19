# The analysis gap — what is LEFT of findings ⑩ and ⑪, and the test-infra holes

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** This file used to carry the whole strict-numeric campaign, milestones 0 through 6.
**All of it has shipped**, at `0.9.28`, and the record lives in [SPEC.md](../SPEC.md) (*Arithmetic on two
values of one type* and *There is no implicit numeric conversion*), [docs/agents.md](../agents.md) and the
git log. What remains here is the two rows that still point at this file:

- **ROADMAP row 1 — uninstantiated generic bodies get no analysis** (§1 below). Still open, and
  **re-probed 2026-08-18 on `0.9.37`: unchanged — `check` is silent and `build` succeeds.**
- **ROADMAP row 17 — test-infra holes** (§2 below). Unchanged and still open.

## 1. A never-instantiated generic body gets **no analysis whatsoever**

Probed 2026-08-15 and still reproducing:

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
instantiation.** This is the half that hits package authors — ship `check`-green, consumers get the
errors.

⚠️ **The root cause is structural and is not going to be fixed by a rule.** `analyze()` IS `emit()`
(`CEmitter::analyze` emits into a throwaway sink), and the emit walk **skips template bodies** —
`emitModuleContent`'s `fn->typeParams` continue. So every rule in the emitter is invisible there by
construction, including the two milestone 6 just added.

Two sizes, and they are genuinely different projects:

| | |
|---|---|
| **cheap** | a concrete-only template-body walk as a new pass after the `checkDeclaredTypes(units)` call in `analyze`, reusing `collectBindings` (`kama.query.cpp`), skipping any expression that mentions a type parameter. Catches unresolved names + concrete type errors. ~150 LOC |
| **full** | opaque type parameters answering `findMethod` from declared bounds (`MethodInfo::whenParams`/`whenBounds`). Bounded quantification; **wants its own design doc.** ~600–900 LOC |

**One thing the cheap pass would also recover.** Milestone 5b-B accepted a known cost: the emit walk
skips uninstantiated template bodies, so an ungoverned wide literal inside a generic nobody instantiates
is not caught. It mentions no type parameter, so the concrete-only walk is positioned to reach it — an
expectation, not a guarantee.

## 2. Test-infra holes

- `tests/trap/` is **skipped** under `KAMA_SAN`, `KAMA_WASM`, and on Windows/MSYS2 (`run_tests.sh`, the
  `TRAP_OK`/`WASM`/`SAN_FLAGS` gate) — UBSan intercepts the trap and node's abort codes differ.
- **No leg runs MSan.** MSan-origins catches what ASan and wasm both miss.
- **An `xfail` fixture never links, so it never reaches ASan.** This is why a campaign about *rejection*
  cannot take a green suite as its acceptance test: the view-window campaign's findings ④⑤⑥ survived all
  three legs and 34 guards. The discipline that works is a probe ledger walking every branch of the rule
  including the ones that must stay SILENT — milestone 6 used a 48-branch one and it caught two bugs a
  green matrix did not (a `case true:` fixture that was a parse error reading as a pass, and a literal
  exemption that rejected `int8 a = 2 + 3`).

## What shipped, so it is not re-derived

Kept only as a pointer, because each cost a cycle to learn and the code no longer shows why:

- **`exprClass` returns `""` for a primitive AND for "unknown"**, and ~40 callers depend on `""` meaning
  "not a class, take the raw-C path". Every classifier built for this campaign (`typeOfExpr`,
  `callReturnTypeRaw`, `indexElemTypeRaw`, `moduleStaticCTypeRaw`) exists because of that conflation.
  **If a rule is silent on something, suspect this filter before suspecting the rule** — it hid a
  primitive three separate times.
- **A green `./dev matrix` proves little for a REJECTION rule.** Write the `xfail` first, and check the
  fixture matches the rule's own WORDING — an error is not evidence that *your* error fired.
- **`cType` is not injective** (`char` → `uint32_t`, `usize` → `size_t`), so `char + uint32` is
  indistinguishable from `uint32 + uint32` to a rule keyed on the lowered type. Pinned as permissive in
  `tests/op_exempt.kama` rather than left to be rediscovered. Separating them needs `primKey`, which
  wants a type NODE that `typeOfExpr` deliberately does not produce.
- **`--strict-numeric`** — the hidden measuring instrument (a TSV on stdout, no `usage()`, no docs) is
  still in the tree. It sized every decision in milestone 6 and is the tool for sizing the next one; its
  lesson is that **a measurement hiding its blind spot is worse than no measurement** (`op-unknown` and
  `unknown-src` are reported for that reason, and lumping comparisons into `arith-subint` once
  overstated a change by 300x).
