# Inheritance — the decision, and six holes (cold-start brief)

*In-flight campaign doc. **Delete this file when the campaign ships**, once SPEC carries the record — see
the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

## Why this campaign exists

Six defects were found in one afternoon (2026-08-01/02), all by asking design questions rather than by
reading code — because in every case the code does exactly what it was written to do. The reason they
survived is visible in one table:

| surface | uses |
| --- | ---: |
| `implements` — contracts / composition | **340** |
| `extends` — inheritance | 25 |
| `extends` **inside the stdlib** (`lib/` + `prelude/`) | **0** |
| `base.<member>` | 9 |
| `override fn` | 20 |

**kama's own standard library uses no inheritance at all.** The feature is exercised only by ~25 test
fixtures, so nothing has pressed on it. That is context for prioritising, not an argument to leave it
broken: what ships must be sound, and two of these are guarantees the language claims elsewhere.

## The six

### 1. A derived type never runs its base's constructor ⚠️ *soundness of a stated guarantee*

`checkNamedCtorComplete` stops at the class boundary: it proves a ctor assigns the type's OWN fields, and
inherited fields silently take the zero-fill. Repro: `tests/pending/base_ctor_not_run.kama` — compiles,
runs, reads 0 from a field no constructor ever assigned.

Not memory-unsafe (the fill is deterministic), but **a base's invariants are unenforceable for its
subclasses**, and the construction model advertises the opposite.

**Fix — install a base VALUE, don't chain.** A factory has no `self` to chain into, but it can build the
base through the base's own ctor and install it:

```kama
public ctor make(int32 x, int32 y) {
    this.base = Base.make(x: x);      // the base's OWN ctor runs, enforcing its invariant
    this.x = x;
    this.y = y;
}
```

The rule is narrow: **`this.base` must be assigned exactly once, from a ctor call on the base type.** Not
general whole-value assignment — delegate-then-tweak is rejected by design, and base fields are private, so
there is nothing to tweak through. Wants a `Base`/`base` pair mirroring `This`/`this` so a derived author
never types the concrete base type name. Depends on the implicit `this` from
[slot-scope.md](slot-scope.md) D5.

### 2. `base.` bypasses access control ⚠️ *encapsulation hole*

`canAccess` is not run on the base path, so a derived type reaches any **private** member of its base by
choosing the other spelling. Repro: `tests/pending/base_bypasses_private.kama`.

```kama
this.secret()   // rejected: "'secret' is private in 'B'"
base.secret()   // compiles, runs, returns 42
```

**Fix: run `canAccess` on the base path.** No new restriction — apply the existing one. private rejected,
protected and public allowed.

**Do NOT restrict `base.` to protected-only.** An earlier draft proposed that; it is wrong. A derived type
may legitimately call `base.<protected>` from a **public** method (verified), so the restriction would be
on the wrong axis. And a `resource` field is always private while inheritance is resource-only, so there
are no protected FIELDS to reach — this is entirely about methods.

### 3. Shadowing is legal at any visibility ⚠️ *footgun*

A derived type may redeclare a non-virtual base method with no `override`, silently, at any visibility.
Which body runs is decided by the **static type**. Repro: `tests/pending/derived_shadows_base_method.kama`
— `d.h()` yields 2 while `base.h()` yields 1.

kama already rejects `public virtual` (*"overridable method must be declared `protected`"*) precisely
because a public override is a footgun. Silent shadowing is the same footgun with no keyword marking it;
C# at least demands `new`.

**Fix: shadowing is an error.** The only way to redefine an inherited method is `override` on a
`protected virtual` (*may* override) or `protected abstract` (*must* override). The type designer decides
what is overridable — that is what `protected` + `virtual`/`abstract` is for.

**⚠️ Explicitly still legal: reusing a name that is PRIVATE in the base.** That is not shadowing — the
base's member is invisible to the derived type, so the two names are unrelated and each type sees its own.
Verified working today (a base and a derived each with a private `n` and `helper()`, each resolving to its
own); the fix must not break it. A `friend` grant must not open a back door here either.

### 4. A polymorphic class stored BY VALUE in a generic collection does not build ⚠️ *codegen*

`DynamicArray<SomeVirtualResource>` fails at the **C** level:

```
error: use of undeclared identifier '_F4__DerivedTick__vtable'
    x.__base.__vptr = &_F4__DerivedTick__vtable;
```

The vtable IS emitted — just late. `DynamicArray<T>::takeAt` (a generic-collection method) is emitted
before the user class bodies and fills `__vptr`, and the vtable is a `static const`, so C needs a prior
declaration. A virtual/final pair with no collection is fine (`tests/devirt_final_class.kama`), so this is
specifically the **inheritance × generics** intersection — the same under-tested seam as the rest of this
brief. Repro: `tests/pending/virtual_class_in_collection.kama`.

**Fix shape:** forward-declare vtable constants, or order them before generic instances.

### 5. `base.field` and `base` with no base type report a fragment

Both produce `"base access"` — not a sentence, naming neither the member nor the type. Should say what
happened: name the member, or say the type has no base.

### 6. `: base(...)` is orphaned — parses, never read

The production exists (`constructor_initializer : COLON BASE LPAREN argument_list_opt RPAREN`) but hangs
off the class-named ctor declarator, the form `72dfdbc` made a hard error, and **no emitter code reads
`ClassConstructorInitializerNode`** — so even when that form was legal the base arguments were parsed and
silently discarded. SPEC claimed the feature worked until this was found.

**⚠️ Do NOT simply delete the production.** It is the only reason a class-named ctor *parses*, which is what
lets the emitter answer with the guided message (*"class-named constructor `X(...)` is no longer allowed —
declare a named constructor `ctor make(...)`"*). Deleting it turns that into a raw parse error. Keep it as
a diagnostic path and give `: base(...)` a message of its own — *"base-constructor delegation is not
supported; assign `this.base = Base.<ctor>(...)` instead"*.

## ✅ THE DECISION (user, 2026-08-02) — keep inheritance, weakened where it is abusable

Reached after working the alternatives all the way down; the reasoning is preserved below because the
conclusion is only defensible with it.

**Contracts stay PURE** — no default method bodies, ever. That is a deliberate choice and it has a known
price: `abstract` (a hole you MUST fill) has a clean contract equivalent, but **`virtual` (a hole you MAY
fill, defaulted otherwise) does not**. Every implementor of a contract must write every method, even to
restate a default. This is the single reason inheritance keeps a niche — the two questions are the same
question, and Rust only escapes inheritance because its traits carry default bodies.

Three additions, on top of the six fixes below:

### A. A derived type may NOT widen the public interface

It may add **fields**, add **private** helpers, and **override the protected seams the base sanctioned**
(`virtual` = may, `abstract` = must). It may not add public methods. The hierarchy's public surface is
fixed at its root, so substitutability is total rather than aspirational — and the `Exposer` leak (a
subclass re-publishing a protected seam, which compiles today) becomes impossible.

⚠️ **Constructors must be exempt** — a derived type needs its own public `ctor` (`RawChannel.open(…)`), and
construction is not part of the substitutable surface. Do not let the rule swallow them.

### B. Exactly ONE level — a deriving type must be `final`

`extends` implies a leaf. `virtual`/`abstract` become legal only on a **root**. The author must still
*write* `final` rather than have it inferred: it teaches the constraint at the point of use, and it leaves
room to lift the restriction later without changing the meaning of existing code.

⚠️ `tests/vtable_depth3.kama` is a three-level chain and becomes illegal — it must be restructured or
retired, and it is the only multi-level fixture in the tree.

**Why 1 and not 2 or 3 — the depth question was reopened and settled the same way (2026-08-02).** Real
patterns do want a middle layer: `Widget -> Control -> Button` is the archetype, where `Control` adds state
AND declares seams for its own extenders. A cap of 1 blocks that. It is still the right starting point,
because the failure modes are **asymmetric**:

- cap too strict -> the middle layer becomes **composition** -> which is the outcome this design wants
- cap too loose -> deep hierarchies appear -> the anti-pattern the whole campaign exists to prevent

Too-strict fails *toward* the goal; too-loose fails *away* from it. And a restriction is cheap to lift and
expensive to add, so 1 is the version that cannot be regretted. There is also **zero demonstrated demand**:
the only multi-level fixture in the tree tests the vtable mechanism, not a real design.

**Make the diagnostic teach the alternative**, or the cap just reads as arbitrary — something like
*"a deriving type is `final`; for a middle layer, compose the base rather than extending it."* If a real
design later proves depth is needed, lifting the cap to 2 breaks no existing code.

*(kama already brakes depth harder than any mainstream language: a type must declare `virtual`/`abstract`
to be extended AT ALL, so every level is an explicit opt-in. C++/Java/C# all default to extensible. The cap
is an additional brake on top of that, not the only one.)*

### C. A switch, so kama can be built without inheritance ✅ SHIPPED

**One knob, not two mechanisms** (user, 2026-08-02): `--inherit-depth=N` / kama.json `"inheritDepth"`,
default **1**, **0 = inheritance off entirely**. It covers both this decision and decision B, and raising
the cap to 2 later is a config change rather than a compiler change.

The originally-briefed shape — a C-preprocessor `#if` so the *compiler* could be built without the
feature — was **not** taken. It buys only the compiler-size number, and it buys it with `#ifdef`s threaded
through `kama.cemit.cpp`, the same soup rejected for platform variance. The number was measured directly
instead (below), which costs nothing to carry.

At depth 0 the gate rejects `extends`, a `virtual`/`abstract` **class**, and a `virtual`/`override`/
`abstract` **method** — all four, not just `extends`, because a `virtual class` with no subclass still
emits a vtable and would leave "no inheritance machinery in the output" false. `final` stays legal: it
seals a type, it does not extend one. One choke point, `CEmitter::rejectIfNoInherit`, mirroring
`rejectIfNoHeap`.

#### Measured cost (2026-08-02)

| what | cost |
| --- | ---: |
| emitted program that uses no inheritance — bytes attributable to the feature | **0** |
| the compiler's dedicated inheritance/vtable emitter functions | **11,121 bytes** |

**The emitted-program number is 0 and always was.** The prediction held exactly: a program that uses no
inheritance never emitted vtable machinery in the first place, so turning the feature off cannot shrink
it. `tools/check-inherit-cost.sh` now pins this — same source, same flags, only the depth differs, and
the binaries must match to the byte. ⚠️ It also asserts the program still emits **contract** vtables
(`_vtbl`), because a fat pointer needs one; without that half, the check would pass on an empty file.

**The compiler number is a lower bound**, and deliberately so — it is the summed `.text` of the functions
that exist *only* for inheritance (`linkBases` 3352, `buildVtables` 2120, `vtableSlotSig` 976,
`emitVtableInstance` 828, `emitVtableType` 628, `vptrPrefix` 456, `topoOrderClasses` 380+lambdas,
`basePathTo` 260, `isBaseOf` 252). It does **not** count the inheritance branches inlined through
`emitStruct`, `emitDispatch`, `canAccess` and the class collector, which cannot be attributed without
deleting them.

**So size is not the argument either way.** 11 KB in a 17 MB compiler, and nothing at all in user
programs — the real cost of inheritance is the six holes above and the design surface they came from,
not bytes. The knob's value is the *experiment* it enables, which is below.

#### The corpus as "pure kama" (2026-08-02)

Every single-file fixture, built with `--inherit-depth=0`:

| | |
| --- | ---: |
| compile with inheritance **off** | **549** |
| require inheritance | **13** |

And the 13 are *exactly* the 13 that exist to test inheritance — `devirt_final_class`,
`devirt_final_method`, `devirt_no_override`, `inherit_dtor`, `inherit_field`, `new_ret_upcast`,
`poly_in_collection`, `upcast_new_base`, `upcast_shared_base`, `virtual_ref`, `virtual_this`,
`vtable_default_ctor`, `vtable_depth2`. **Zero collateral.** Nothing in the prelude, the stdlib, or any
other feature's fixtures reaches for it, which is the same fact the 0-`extends`/340-`implements` table at
the top of this file reports, now confirmed by the compiler rather than by grep.

⚠️ **Read this as "the feature is unexercised", not "the feature is unnecessary".** The corpus is
kama's own code, and kama's own code is systems-level — it was never the constituency for `virtual`. The
argument for keeping inheritance is unchanged and is stated above: contracts are pure, so `virtual`
(a hole you MAY fill, defaulted otherwise) has no contract equivalent. What the experiment settles is the
*cost* question — nothing depends on it, so the restrictions in A and B can be as tight as the design
wants without breaking anything that exists.

## How the decision changes the six fixes below

| hole | status under the decision |
| --- | --- |
| 1. base ctor never runs | **still needed** — and A/B do not touch it |
| 2. `base.` bypasses `canAccess` | **still needed** |
| 3. shadowing is legal at any visibility | **largely subsumed by A** — no new public methods means no public shadowing; keep the rule for the private/protected cases |
| 4. polymorphic class by value in a collection | ✅ **SHIPPED** `0f8b2a9` — independent codegen bug |
| 5. `base.field` / no-base diagnostics | still needed |
| 6. `: base(...)` orphaned | still needed |

## Progress

| item | status |
| ---: | --- |
| **4** polymorphic type by value in a generic collection | ✅ shipped `0f8b2a9` — vtable instances gained external linkage + a header forward declaration, beside the `C__as_I` block that already needed it. Fixture `tests/poly_in_collection`. |
| **C** the switch | ✅ shipped — `--inherit-depth=N` / `"inheritDepth"`, one gate (`rejectIfNoInherit`), guard `tools/check-inherit-cost.sh`, cost measured above. |
| **B** depth cap + `final` | ✅ shipped — both walked in `linkBases`; `tests/vtable_depth3` restructured to `vtable_depth2`; rejections pinned by `tests/xfail/inherit_depth_exceeded` + `tests/xfail/deriving_type_not_final`. |
| **A** no public widening | ✅ shipped — `checkDerivedPublicSurface`, ctors exempt, plus the `implements` half (contracts belong on the root). Pinned by `tests/xfail/derived_widens_public` (the `Exposer` leak) + `tests/xfail/derived_implements_contract`. |
| **1**, **2**, **3**, **5**, **6** | ☐ |

### ⚠️ A consequence of the cap worth a decision later: `final` on a METHOD is now near-vacuous

`emitDispatch` devirtualizes on three independent triggers, short-circuiting left to right:
`sc.isFinalClass || mi->isFinal || !overriddenAnywhere`. Trigger 2 (`final` method) was isolable only
with a **non-final class that derives** — `tests/devirt_final_method` used exactly that shape. Under the
cap, a deriving type is `final`, so trigger 1 always fires first, and trigger 2 is unreachable as the
*sole* reason to devirtualize.

A root can still write `protected final virtual fn`, but nothing may then override it, so trigger 3 fires
too. So `final` on a method no longer buys any lowering the other two triggers do not already buy; what
remains is its *documentary* value (marking a slot as sealed) and the rejection in
`tests/xfail/override_final`. **Not decided here** — it is a live question of whether `final fn` should
stay in the language at depth 1, and it should be answered deliberately rather than as a side effect of
this campaign. `tests/devirt_final_method` records the situation in its own comment.

## Sequencing

**Decision items A and B are source-breaking**, as are holes 1 and 3, so they land before the 1.0 tag or
wait for 2.0. Hole 2 is breaking only for code exploiting the hole. Holes 5 and 6 are diagnostics.

Remaining order: **2**, **3** → **5**, **6** → *[the slot-scope campaign]* → **1**.

1 depends on [slot-scope.md](slot-scope.md) D5 (the implicit `this`), so that campaign runs first — it is
already rewriting every ctor body, and the two sweeps must not interleave.

## Verification

```sh
tools/cdev make && tools/cdev test
tools/cdev exec env KAMA_SAN=1 ./run_tests.sh
tools/cdev exec env KAMA_WASM=1 ./run_tests.sh
```

Baseline: **native 875 / ASan 838 / wasm 806, all 0 failed.**

The four parked repros in `tests/pending/` are the acceptance test. Three currently COMPILE and must become
rejections with a fixture asserting the message; the fourth
(`virtual_class_in_collection.kama`) currently FAILS TO BUILD and must become a passing fixture. They are
deliberately outside the suite — a fixture that passes by compiling is exactly how the first three hid.

**Widen the corpus while you are here.** 25 `extends` fixtures for a feature with six holes is the root
cause, not a coincidence. Anything added should exercise: a derived ctor initializing base state, `base.`
across all three visibilities, an `abstract` base, and a three-level chain (`tests/vtable_depth3.kama` is
the only one today) — and a `DynamicArray` of a polymorphic type, which is hole 4 and has no fixture at all.
