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

### C. Gate inheritance behind a compiler `#if`, so kama can be built without it

Two purposes: **(1)** run the whole corpus as "pure kama" with no inheritance at all, to see what actually
breaks; **(2)** measure binary size — both the kama compiler itself and the programs it emits.

⚠️ **Set the expectation before measuring, or the numbers will mislead.** A program that uses no
inheritance *already* emits zero vtable machinery (measured: 11 vtable structs/instances → 0, 7 `__vptr`
→ 0 between the two TEMP examples), and **contracts keep their own vtables** because a fat pointer needs
one. So the emitted-program delta for non-inheritance programs should be ≈0, and the real signal is the
**compiler's** size and the emitter paths that disappear. Measure both, but predict them separately.

## How the decision changes the six fixes below

| hole | status under the decision |
| --- | --- |
| 1. base ctor never runs | **still needed** — and A/B do not touch it |
| 2. `base.` bypasses `canAccess` | **still needed** |
| 3. shadowing is legal at any visibility | **largely subsumed by A** — no new public methods means no public shadowing; keep the rule for the private/protected cases |
| 4. polymorphic class by value in a collection | **still needed** — independent codegen bug |
| 5. `base.field` / no-base diagnostics | still needed |
| 6. `: base(...)` orphaned | still needed |

## Sequencing

**Decision items A and B are source-breaking**, as are holes 1 and 3, so they land before the 1.0 tag or
wait for 2.0. Hole 2 is breaking only for code exploiting the hole. **Hole 4 is a codegen fix with no
surface change** — it can land immediately and independently, and it is the only one that blocks working
code today. Holes 5 and 6 are diagnostics. **Decision item C (the `#if` gate) is additive** and is worth
doing EARLY: building without inheritance is the cheapest way to find out what the corpus actually depends
on, before any of the breaking work starts.

Suggested order: **4** (unblocks working code) → **C** (measure and learn) → **B**, **A** (the restrictions,
warn-first) → **1**, **2**, **3** → **5**, **6**.

1 depends on [slot-scope.md](slot-scope.md) D5 (the implicit `this`), so run that campaign first or fold
them together — it is already rewriting every ctor body.

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
