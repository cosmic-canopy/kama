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

### B. Exactly ONE level — a deriving type must be `final` ✅ SHIPPED

`extends` implies a leaf. `virtual`/`abstract` become legal only on a **root**. The author must still
*write* `final` rather than have it inferred — and the reason is the user's, worth stating in their terms
(2026-08-02): **`final` is an early, visible, INTENTIONAL trigger.** You meet the hard limit while writing
the type that reaches it, instead of being surprised by an error later, the first time you try to extend
that type once more. Inference would move the discovery to the wrong place.

**As shipped, the rule generalizes to the limit rather than hard-coding 1.** `KAMA_INHERIT_DEPTH` counts
`extends` edges from a root; a class *at* the limit must be `final`, and a class *below* it may still be
`virtual`/`abstract`. So raising the constant to 2 moves the `final` requirement down to the second
derived level and legalizes exactly one middle layer — verified:

| | depth 1 | depth 2 |
| --- | --- | --- |
| `Widget` (root, 0 hops) | `virtual` | `virtual` |
| `Control extends Widget` (1 hop) | must be `final` | may be `virtual` — the middle layer |
| `Button extends Control` (2 hops) | rejected: too deep | must be `final` |

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
design later proves depth is needed, lifting the cap to 2 breaks no existing code — a `final` leaf stays a
legal `final` leaf, it simply stops being required to be one at that level.

*(kama already brakes depth harder than any mainstream language: a type must declare `virtual`/`abstract`
to be extended AT ALL, so every level is an explicit opt-in. C++/Java/C# all default to extensible. The cap
is an additional brake on top of that, not the only one.)*

### C. A switch, so kama can be built without inheritance ✅ SHIPPED

**A build-time `#if` on the compiler, exactly as briefed.**

```sh
make                      # inheritance in
make KAMA_INHERITANCE=0   # a compiler built without it
```

⚠️ **This was got wrong once and corrected (user, 2026-08-02).** A runtime knob shipped first —
`--inherit-depth=N` / kama.json `"inheritDepth"`, with 0 meaning off — and it served *neither* purpose of
this decision. A flag cannot produce a size delta, because every byte is still in the binary; and it is
not an extraction point, because nothing about it tells you what to delete. The knob was reverted whole:
there is **no** way for a program or a project to turn inheritance off, and there should not be, because
neither purpose is about end users. `KAMA_INHERIT_DEPTH` became a compile-time constant beside the switch.

Scope is the **emitter only**. The grammar still parses `extends`/`virtual`/`base` in an inheritance-free
build and the emitter answers with a real diagnostic — better than the syntax error that gating the
grammar would give, and worth almost nothing in bytes, since the parser tables are generated either way.
Removing the grammar later is 5 tokens, 3 productions and 3 AST node types. One choke point,
`CEmitter::rejectInheritance`, mirroring `rejectIfNoHeap`; everything downstream is then unreachable,
which is what lets the rest be compiled out entirely.

`tools/check-no-inheritance.sh` builds the variant and exercises it. That guard is the point, not a
nicety: **an untested build variant rots within weeks, and a rotted extraction point is worse than none,
because it looks like an option and isn't.** It asserts the variant builds warning-clean, rejects all four
surfaces with its own diagnostic (not a parse error), and still *runs* a contract/generic program to the
right answer — compiling is not enough, since the gate sits beside the contract vtable machinery.

#### Measured cost (2026-08-02)

Same tree, same compiler, same flags — only `KAMA_INHERITANCE` differs:

| | with | without | delta |
| --- | ---: | ---: | ---: |
| **`.text`** (machine code) | 3,844,930 | 3,810,682 | **34,248 B** |
| stripped binary | 3,937,264 | 3,871,728 | 65,536 B |
| unstripped (`-g`) | 17,129,704 | 16,978,128 | 151,576 B |

**`.text` is the honest number: ~34 KB, about 0.9% of the compiler.** The stripped delta is larger only
because of section padding, and the `-g` delta is mostly debug info for the removed code.

And in **emitted programs: 0 bytes.** A program that uses no inheritance never emitted vtable machinery in
the first place, so removing the feature cannot shrink it — contracts keep their own vtables regardless,
because a fat pointer needs one.

**So size is not the argument either way.** 34 KB in a 3.8 MB compiler, and nothing at all in user
programs. The real cost of inheritance is the six holes above and the design surface they came from. What
the switch is genuinely for is purpose 2 — a mechanical, tested extraction point — and the experiment
below.

#### The corpus as "pure kama" (2026-08-02)

Every single-file fixture, checked with a real `KAMA_INHERITANCE=0` compiler:

| | |
| --- | ---: |
| compile with inheritance **compiled out** | **549** |
| require inheritance | **15** |

And the 15 are *exactly* the 15 that exist to test inheritance — `devirt_final_class`,
`devirt_final_method`, `devirt_no_override`, `inherit_abstract_base`, `inherit_dtor`, `inherit_field`,
`inherit_private_name_reuse`, `new_ret_upcast`, `poly_in_collection`, `upcast_new_base`,
`upcast_shared_base`, `virtual_ref`, `virtual_this`, `vtable_default_ctor`, `vtable_depth2`.
**Zero collateral.** Nothing in the prelude, the stdlib, or any other feature's fixtures reaches for it,
which is the same fact the 0-`extends`/340-`implements` table at the top of this file reports, now
confirmed by a compiler that physically cannot compile inheritance rather than by grep.

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
| **C** the switch | ✅ shipped — `make KAMA_INHERITANCE=0`, one gate (`rejectInheritance`), guard `tools/check-no-inheritance.sh`, cost measured above. ⚠️ A runtime flag shipped first and was reverted whole: it served neither purpose. |
| **B** depth cap + `final` | ✅ shipped — both walked in `linkBases` against the compile-time `KAMA_INHERIT_DEPTH` (1 = a root plus ONE derived level; raising it moves the `final` requirement down to the new deepest level); `tests/vtable_depth3` restructured to `vtable_depth2`; rejections pinned by `tests/xfail/inherit_depth_exceeded` + `tests/xfail/deriving_type_not_final`. |
| **A** no public widening | ✅ shipped — `checkDerivedPublicSurface`, ctors exempt, plus the `implements` half (contracts belong on the root). Pinned by `tests/xfail/derived_widens_public` (the `Exposer` leak) + `tests/xfail/derived_implements_contract`. |
| **2** `base.` bypasses `canAccess` | ✅ shipped — one `canAccess` on the base-method path; `_currentClass` is the derived type there, so the existing rule is exactly right. Pinned by `tests/xfail/base_bypasses_private`. |
| **3** shadowing | ✅ shipped — in `buildVtables`, which already walks the chain. Private-name reuse stays legal and is pinned by `tests/inherit_private_name_reuse`. |
| **5** base diagnostics | ✅ shipped — "no base at all" / "no such member" / "outside a method" are three messages now, not the fragment `"base access"`. Pinned by `tests/xfail/base_without_base_class`. |
| **6** `: base(...)` orphaned | ✅ shipped — the production is KEPT (it is the only reason a class-named ctor parses, hence the guided message) and given its own answer. Pinned by `tests/xfail/base_ctor_delegation`. |
| **1** base ctor never runs | ☐ — blocked on the slot-scope campaign (D5, the implicit `this`). |

### ⚠️ Hole 1 is wider than recorded: a base's FIELD DEFAULTS do not reach a derived instance either

Found while writing `tests/inherit_private_name_reuse` (2026-08-02). The brief describes hole 1 as "a
derived type never runs its base's constructor", which is true but understates it — even a field
*initializer* on the base is skipped:

```kama
type virtual resource Base { int32 n = 10;  public fn int32 rank() { return this.n; } … }
type final resource Leaf extends Base { … }

Base.make().rank()   // 10
Leaf.make().rank()   // 0   — the base's own field default never applied
```

So the base sub-object is zero-filled outright, not merely un-constructed. The fix is unchanged (install
a base VALUE built by the base's own ctor), and this makes the case for it stronger: a base author cannot
establish an invariant even by writing the default at the field, which is the one mechanism SPEC otherwise
offers as the escape hatch from "a ctor must assign every field".

### The two `final`s are different things, and the cap affects only one of them

⚠️ *An earlier revision of this file called `final` "near-vacuous" without saying WHICH `final`, which
reads as a claim about the class marker. It was never that. Corrected 2026-08-02.*

| spelling | what it does | under the cap |
| --- | --- | --- |
| `type final resource X` — a final **class** | seals the type: nothing may extend it | **required** on a deriving type. This is the design's visible limit marker (decision B) and the cap is the whole reason it exists. |
| `protected final override fn f()` — a final **method** | seals one virtual slot: no subclass may re-override *that method* | **dormant at depth 1, load-bearing at depth 2+** |

**Why the method modifier is dormant at depth 1, and why that is not a reason to remove it.** Sealing a
slot only matters when someone below you could re-override it, and at depth 1 nobody can:

- on a **leaf**, the class is already `final`, so nothing derives from it;
- on a **root**, a plain non-virtual method is already un-redefinable, because shadowing is now an error.

Devirtualization tells the same story. `emitDispatch` short-circuits left to right on
`sc.isFinalClass || mi->isFinal || !overriddenAnywhere`; trigger 2 was isolable only with a **non-final
class that derives**, which the cap makes unrepresentable, so trigger 1 or 3 always fires first.

But at **depth 2** the middle layer exists and the modifier is the only thing that does its job —
verified by building a `KAMA_INHERIT_DEPTH=2` compiler against a fully legal `Base <- virtual Mid <-
final Leaf` chain: `final override fn h()` on `Mid` is the **sole** cause of rejection, and deleting just
that one keyword makes the same chain compile.

**So `final fn` stays.** It is dormant rather than dead, and it wakes up the moment the cap rises — which
this design explicitly anticipates. Removing a feature because a *starting-point* restriction currently
hides it would be exactly backwards. `tests/devirt_final_method` and `tests/xfail/override_final` keep it
covered at depth 1 (the fixture pins that it parses, seals, and dispatches directly, even though trigger 1
is what fires).

## Sequencing

**Decision items A and B are source-breaking**, as are holes 1 and 3, so they land before the 1.0 tag or
wait for 2.0. Hole 2 is breaking only for code exploiting the hole. Holes 5 and 6 are diagnostics.

Remaining: *[the slot-scope campaign]* → **1**.

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
