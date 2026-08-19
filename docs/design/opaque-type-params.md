# Opaque type parameters — checking a generic at its declaration

*In-flight design doc. **Delete this file when the work ships**, once SPEC + ROADMAP carry the record —
see the maintenance table at the top of [ROADMAP_DETAIL.md](../ROADMAP_DETAIL.md).*

**ROADMAP row 1.** The generic-FUNCTION probe shipped at `0.9.38`; this is the rest of it, and it is one
project rather than two, because the generic-TYPE half needs the machinery the type-parameter half builds.

---

## 1. The problem, stated as what a user sees

A generic's body is only checked when something instantiates it. A library ships generics its own tests
never instantiate, so:

> **the package author's `kama check` is green, and every consumer gets the errors.**

Two live instances, both found in this repo while planning this work:

```kama
// tests/generic_uninst_ok.kama:27 — in the tree today, and `kama check` says OK
fn void neverCalled3<T>(ref DynamicArray<T> src) {
    DynamicArray<T> d = DynamicArray.empty();
    d.add(item: give src.get(index: 0));   // DynamicArray has NO `get`
}
```

The fixture whose entire job is to assert *"valid generic code is left alone"* calls a method that does
not exist. And `SPEC.md`'s own headline generic example:

```kama
fn T max<T>(T a, T b) { return a > b ? a : b; }   // SPEC.md:2831
```

does not compile at any instantiation, because the same SPEC says comparison is a **contract** — `>` *is*
`Comparable.compareTo` — and `T` never promised one.

### What `0.9.38` fixed, and what it could not

`checkUninstantiatedTemplates` re-emits every uninstantiated generic **function** into a throwaway sink,
with `_typeSubst` left empty so `T` stays symbolic. Because `analyze()` **is** `emit()`, that one change
made the entire rule set apply inside a template body — declared types, literals, numeric width,
unresolved free-function names — and keeps applying as rules are added.

What it cannot do is resolve a receiver whose type mentions `T`, because nothing is bound to `T`. Four
rules fire on an *unknown* type rather than a *wrong* one, and all four had to be routed through
`deferUnknownWhileProbing()` — unguarded they rejected 81 of 640 corpus fixtures, every one valid.

## 2. The measurement (`0.9.39`)

`--probe-templates` now attributes each deferral instead of only counting it, because a deferral a
*compiler* change closes and one only a *source* change closes are different projects. Over
`tests/ lib/ prelude/ examples/ bench/`:

| | |
|---|---|
| uninstantiated function templates walked | **48** |
| with at least one blind spot | **47** |
| method sites resolved | **5** |
| method sites deferred | **87** |
| method-resolution reach | **5 %** |

| bucket | count | what closes it |
|---|---|---|
| `turbofish` — `sortWith::<T, C>` forwards the enclosing params | **26** | compiler |
| `recv-param-bounded` — receiver **is** a param, and it declares a bound | **29** | compiler |
| `recv-generic` — receiver is `View<T>` / `DynamicArray<T>` | **24** | compiler |
| `dot-ctor` — `DynamicArray<T>.empty()`, no instance yet | **6** | compiler |
| `scope-qual` — `Natural<T>::compare(…)` | **2** | compiler |
| **`recv-param-unbound`** — receiver is a param with **no bound** | **0** | *source* |
| `recv-unknown` — receiver's type unrecoverable | **0** | — |

⚠️ **`recv-param-unbound` is zero, and that is the finding that sizes this campaign.** Nothing in the
corpus calls a method on a bare, unbounded type parameter. Every one of the 87 deferred sites is closable
by the compiler alone; the source-migration cost of enforcing bounds at method receivers is **nil**.

Two notes on reading it honestly:

- The `recv-unknown` bucket read 6 before the classifier learned that a receiver can be the **parameter
  itself** — `T.fromStr(s: s)`, `T.deserialize(r: r)`, which is how every bounded static in the stdlib
  is called. `receiverTypeNode` answers only for locals and fields. Six real *bounded* sites were sitting
  in "cannot tell" and overstating the residual.
- **This measures generic FUNCTIONS only.** Generic types have no instrument at all; §5 adds one, and
  the type half's reach is genuinely unknown until then. Saying otherwise would repeat the mistake this
  document exists to correct.

## 3. The mechanism — one synthetic type per parameter

An **opaque type parameter** is an ordinary `ClassInfo` in `_classes`, minted per template parameter,
whose methods come from that parameter's declared bounds. Bind it into `_typeSubst` and the probe walk
stops being special: `T` is a type, `View<T>` is an ordinary instantiation, `findMethod` and `cType`
answer, and every existing resolution path works unchanged.

That is deliberately the same move `0.9.38` made. Re-using the emitter was ~60 lines and inherited every
present *and future* rule; the design doc's estimate of a ~150-line hand-written walk re-implementing
four rules was the wrong shape, because a second copy of name/type/method resolution goes stale the next
time a rule lands. **Nothing here re-implements a rule either.**

```
  fn void f<T: Comparable<T>>(ref DynamicArray<T> d, T x) { ... }

  buildOpaqueParams("f") ─► _classes["__opq_f_T"]  kind=resource, copyable=false
                                                   interfaces=[Comparable___opq_f_T]
                                                   methods={compareTo}   (from the bound)

  _typeSubst["T"] = __opq_f_T
       │
       ├── x.compareTo(other: y)   ─► resolves: the bound provides it
       ├── x.area()                ─► ERROR at the template: no bound provides `area`
       └── DynamicArray<T>         ─► registerGenericTypeInst("DynamicArray", [__opq_f_T, GlobalAllocator])
                                       an ordinary instance; `d.length()`, `d.view()` resolve
```

### Why distinct synthetic names, and not identity substitution

`analysis-gap.md` §1 recorded the failed attempt and it is the load-bearing prior result: calling
`emitClassDefinitions` on a template's own `ClassInfo` **failed 628 of 641 fixtures across six rule
families**, because a `_genericTypes` entry is a *shape awaiting specialization*, not a class — its
`when [A: default]` gates are unevaluated and its `ctors` map is not the one an instance gets.
`registerGenericTypeInst` is what turns the shape into a class, and it needs real type arguments.

Identity substitution (binding `T` to `T`) is **not** the shortcut: that is the exact input that once
made `mangleElem` recurse on its own result until the compiler died, which is why `explicitGenericInst`
defers an unbound argument instead. A **distinct** synthetic name is what makes the specialization path
usable, and it is the reason the two halves are one project.

## 4. The two decisions

Both were put to the user with the real code in front of them, and locked 2026-08-19.

### D1 — a call the bounds do not prove is a HARD ERROR at the template

Full bounded quantification. SPEC already claims it —

> **Contract bounds** … A bound lets the body call the contract's methods on a type-param value.

— and it has never been enforced. The diagnostic names the fix:

```
error: `T` has no bound providing `compareTo` — add a bound: `<T: Comparable<T>>`
```

This is **source-breaking**, which is exactly why it lands before the 1.0 tag rather than waiting for
2.0. Measured cost in this repo: **zero method-receiver sites** (§2). The known source changes are the
two live defects in §1 — `tests/generic_uninst_ok.kama:27` and `SPEC.md:2831` — plus whatever §5's new
instrument turns up in generic *type* bodies.

### D2 — an unbounded `T` is move-only

The opaque type is a `resource` that has **not** opted into `Copyable`, so `when [T: Copyable<T>]`-gated
members are absent from probe instances:

```kama
// lib/std/collections/dynamic_array.kama:152
public fn DynamicArrayIter<T> iterator() when [T: Copyable<T>] { ... }

fn isize count<T>(ref DynamicArray<T> d) {
    foreach (x in d) { ... }        // needs iterator() -> error: needs `T: Copyable<T>`
}
```

and that is a **true positive**: `count<Owned<Shape>>` really cannot compile. The permissive alternative
(treat `T` as a `value`, so gates stay on) would let the probe pass a body a move-only argument rejects
— the pass would be *hiding* that case rather than counting it, which is the one thing
`--probe-templates` was built not to do.

`prelude/` + `lib/` carry **120** `when [...]` gates: 63 on `Copyable`, 26 on `default`, 34 on
`Serialize`/`Deserialize`, 6 on `Equatable`. That is the exposure *surface*, not the cost — a gate only
bites when an uninstantiated body reaches through it, and today only 24 `recv-generic` sites get far
enough to try. **The real number is not knowable until M2 lands**; M2 reports it before M3 flips
enforcement.

### D2b — a template's OWN defaulted parameter is bound opaquely

8 of the 38 generic type templates in `prelude/` + `lib/` declare a default (`A: Allocator =
GlobalAllocator`). The template must hold for **any** argument, not just the default, so the parameter
binds opaquely with its declared bound. A member gated `when [A: default]` legitimately does not exist
for an arbitrary `A`, and a body calling one unconditionally is a real bug — it would fail for
`DynamicArray<T, ArenaAllocator>`.

⚠️ This does **not** affect `DynamicArray<T>` written *inside* another template: that is
`DynamicArray<T, GlobalAllocator>`, whose `A` fills concretely from `_genericTypeDefaults` exactly as it
does today, so `when [A: default]` holds and `DynamicArray.empty()` still resolves. The decision only
reaches a template probing *its own* defaulted parameter, which is §5's case. If M4 measures that as
mostly false positives, the recorded fallback is to bind such a parameter to its default and say so in
the tally — but the strict reading is the starting position, because correctness is the point.

## 5. Milestones

| | what | ships |
|---|---|---|
| **M0** | bucket the blind spot — `DeferKind`, one TSV column each | ✅ `0.9.39` |
| **M1** | this document | ✅ |
| **M2** | opaque parameters for generic FUNCTIONS; reach reported, enforcement still deferred | |
| **M3** | the enforcement flip (D1) + corpus migration + `xfail` fixtures | |
| **M4** | generic TYPES and generic enums, via `registerGenericTypeInst` with synthetic args | |
| **M5** | SPEC record, delete `--probe-templates`, delete this file and ROADMAP row 1 | |

### M2 — the build, and the five things that can go wrong

`buildOpaqueParams(templateKey, typeParams, typeBounds)` mints one `ClassInfo` per parameter:
`name = "__opq_<templateKey>_<param>"`, `kind = Resource`, `copyable = false` (D2), `interfaces` = the
resolved bounds, and methods projected from `contractMethods(bound)` — `InterfaceMethod` → `MethodInfo`,
mirroring `injectImplMethods`, with `isAbstract = true` so no emission path looks for a body. Contract
**operators** project as `isOperator` entries so generic math and `Comparable` resolve. A pinned bound
(`T: Comparable<T>`, `<T is This>`) registers its generic-contract instance over the opaque name first.

Then `_typeSubst[param] = synthId(opaqueName)` in the probe, replacing the empty binding.
`_probeTypeParams` and its `isTypeParamName` special case should then be deletable — the opaque is a
genuinely registered type — and that deletion is the check that the mechanism is really working.

1. **Nothing synthetic may leak.** `analyze()` runs `buildDefSites()`/`buildPositions()` *after* the
   probe, so a `View___opq_T` left in `_classes` becomes a type in the query index. Record every key
   inserted during the probe (`_classes`, `_interfaces`, `_genericTypeInsts`, `_genericTypeInstOf`,
   `_genericTypeInstOrder`, `_collections`, `_primConformances`) and erase them at the end.
   `std::map::erase` keeps other elements' references valid, so resolved `ClassInfo::base` pointers
   survive. A `tools/check-*.sh` greps `--keep-c` output and `kama query` for `__opq_`.
2. **Do not double-diagnose a library body.** A probe instance of `View<__opq_T>` re-emits *View's*
   members, which live in `view.kama` and may already have been walked for a real instantiation. Gate
   ref-recording and diagnostics to the template being probed, so a probe reports the user's template
   and never the library it reached through.
3. **Probe-mode exemptions in `registerGenericTypeInst`.** Its `Atomic<T>` element check and its
   view-argument reject would both fire on an opaque argument. An opaque is neither a machine word nor a
   view; both defer under the probe.
4. **`checkBounds` on an opaque argument is a new rule, not a bug.** `satisfiesBound` reads the opaque's
   `interfaces`, so it passes exactly when the outer bound implies the inner one — i.e. it enforces
   **bound propagation** (a function using `Natural<T>` must itself declare `T: Comparable<T>`). Correct
   under D1, and M2 must report how many sites it costs before M3 turns it into an error.
5. **Termination.** Distinct names avoid the `mangleElem` self-recursion; add a probe-instantiation depth
   cap. `tests/generic_uninst_ok.kama` cases 3–4 already cover mutual recursion between templates.

### M4 — the type half

`checkUninstantiatedTypeTemplates()`, the exact analogue: for each `_genericTypes` entry with no
instantiation, build opaque arguments, call `registerGenericTypeInst(tmpl, opaqueArgs)` — the real
specialization path, which is what evaluates the `when` gates — and hand `_classes[mangled]` to
`emitClassDefinitions`, so ctor completeness, base install, view escape and every other member rule
applies. Generic `enum`s take the same path. Called from both `emit()` and `emitProgram()`, so
`build` == `check` == LSP, as the function half already is.

## 6. The record this owes SPEC

When this ships, SPEC's *Generics* section gains a claim it **does not currently make at all**:

> **what is checked at a generic's DECLARATION, versus at its instantiation** —

together with the bound-required-to-call rule, each negative claim linked to its `tests/xfail/` fixture.
The guards that already exist: `tests/xfail/generic_uninst_{type_error,unknown_fn,unknown_method}` and
the silent-half fixture `tests/generic_uninst_ok.kama`. A prose claim that something is rejected is
unguarded without one — the house rule, learned the hard way.
