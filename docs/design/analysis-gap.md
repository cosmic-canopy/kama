# The analysis gap — what is LEFT of findings ⑩ and ⑪, and the test-infra holes

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record — see
the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**→ START HERE.** This file used to carry the whole strict-numeric campaign, milestones 0 through 6.
**All of it has shipped**, at `0.9.28`, and the record lives in [SPEC.md](../SPEC.md) (*Arithmetic on two
values of one type* and *There is no implicit numeric conversion*), [docs/agents.md](../agents.md) and the
git log. What remains here is the two rows that still point at this file:

- **ROADMAP row 1 — uninstantiated generic bodies get no analysis** (§1 below). **The generic-FUNCTION
  half shipped 2026-08-18 at `0.9.38`.** What is left is generic TYPES and the type-parameter-dependent
  half of a body — both now MEASURED rather than estimated; see §1.
- **ROADMAP row 17 — test-infra holes** (§2 below). Unchanged and still open.

## 1. A never-instantiated generic body gets no analysis

The original repro, and what each line does today:

```kama
fn void wrong<T>(ref DynamicArray<T> d) {
    View<T> v = d.view();          // still missed  (receiver's type needs `T` bound)
    int32 x = "not an int";        // NOW CAUGHT
    undefinedFunction(a: 1);       // NOW CAUGHT
    d.nonexistentMethod();         // still missed  (same reason as line 1)
}
fn int32 main() { return 0; }
```

Before `0.9.38`, `kama check` said OK and `kama build` succeeded on all four.

⚠️ **The root cause was structural, and is now fixed at the root.** `analyze()` IS `emit()`, and the
emit walk **skipped template bodies** (`emitModuleContent`'s `fn->typeParams` continue), so every rule
in the emitter was invisible there by construction. `checkUninstantiatedTemplates` stops the skipping:
it re-emits every uninstantiated generic FUNCTION into a throwaway sink with the type params left
symbolic — `emitGenericInst` minus `bindInstParams` — from both `emit()` and `emitProgram()`, so
`build`, `check` and the LSP still agree. The whole rule set now applies inside a template body, and
keeps applying as rules are added.

### What the measurement says — `--probe-templates`, over `tests/ lib/ prelude/ examples/ bench/`

| | |
|---|---|
| distinct uninstantiated templates walked | **52** |
| of those, fully seen (nothing deferred) | **2** |
| of those, with at least one blind spot | **50** |
| method call sites the walk RESOLVED | **5** |
| method call sites it had to DEFER | **90** |
| **method-resolution reach** | **5 %** |

**Read that number precisely: it is method-call resolution, not total coverage.** The pass fully
checks everything that needs no binding for `T` — declared types (a missing import inside a template
body is caught), literal and numeric rules, unresolved free-function names, and method calls on a
CONCRETE receiver. What it cannot do is resolve a receiver whose type mentions `T`, and in real
generic code almost every receiver does. That is why 50 of 52 templates have a blind spot: a generic
function is, definitionally, mostly about its type parameter.

⚠️ **Two premises this work started with were wrong. Both were measured, not argued.**

1. **"An unbound `T` will go quiet on its own"** — because `cType` hands an unresolved name straight
   back, landing it in the `exprClass == ""` bucket ~40 callers read as "not a class". FALSE for four
   rules, which fire on an *unknown* type rather than a *wrong* one: `method call on unresolved
   receiver`, the turbofish and scope-qualified-call rejections, and `cannot tell which X to
   construct`. Unguarded they rejected **81 of 640 corpus fixtures, every one valid generic code**.
   They now route through `deferUnknownWhileProbing`, which is also what produces the tally above —
   the blind spot is counted, never silently swallowed.

2. **"The cheap pass is ~150 LOC of hand-written walk re-implementing four rules."** It is ~60 lines
   that re-use the emitter, and re-implementing anything would have been the wrong shape: a second
   copy of name/type/method resolution goes stale the next time a rule lands. But the *reach* that
   estimate implied was far too generous — see the 5 % above.

### What is LEFT, and what it costs

**(a) Generic TYPES and generic enums get no analysis at all** — the same hole in the other table
(`_genericTypes`), and the half a package author is most likely to ship: a library's public generic
type whose methods no fixture in that library instantiates. Probed 2026-08-18:

```kama
type value Holder<T> {
    T item;
    public ctor make(T x) { this.item = give x; }
    public fn void broken() { int32 x = "not an int"; undefinedFunction(a: 1); }
}
```
→ `kama check` says OK.

⚠️ **The obvious fix does not work, and this is the load-bearing finding for whoever picks this up.**
Calling `emitClassDefinitions` on the template's `ClassInfo` — the exact analogue of what works for a
function — **fails 628 of 641 corpus fixtures across six rule families.** The reason is that a
template `ClassInfo` in `_genericTypes` is a *shape awaiting specialization*, not a class: its
`when [A: default]` member gates are unevaluated and its `ctors` map is not the one an instance gets.
`registerGenericTypeInst` is what turns the shape into a class, and it needs real type arguments to do
it. Identity substitution (binding each param to itself) is **not** the shortcut: that is the exact
input that once made `mangleElem` recurse on its own result until the compiler died, which is why
`explicitGenericInst` defers an unbound argument instead.

So the type half is not a loop away — it needs a shape built with *distinct synthetic* arguments,
which is (b).

**(b) Opaque type parameters** — a synthetic type per parameter answering `findMethod`/`cType` from
the declared bounds (`MethodInfo::whenParams`/`whenBounds`, `_genericTypeBounds`). Bounded
quantification. This is where the other 95 % of method sites live, AND it is the same machinery (a)
needs to build a probe shape, so the two are one project rather than two. **Wants its own design doc.**
The earlier ~600–900 LOC estimate is untested; the 5 % reach number is what should size it.

**Recovered by (a)+(b), not by what shipped.** Milestone 5b-B accepted a known cost: an ungoverned
wide literal inside a generic nobody instantiates was not caught. One that mentions no type parameter
is now reached; one that does still is not.

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
