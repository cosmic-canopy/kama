# Inheritance — closing five holes (cold-start brief)

*In-flight campaign doc. **Delete this file when the campaign ships**, once SPEC carries the record — see
the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

## Why this campaign exists

Five defects were found in one afternoon (2026-08-01/02), all by asking design questions rather than by
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

## The five

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

### 4. `base.field` and `base` with no base type report a fragment

Both produce `"base access"` — not a sentence, naming neither the member nor the type. Should say what
happened: name the member, or say the type has no base.

### 5. `: base(...)` is orphaned — parses, never read

The production exists (`constructor_initializer : COLON BASE LPAREN argument_list_opt RPAREN`) but hangs
off the class-named ctor declarator, the form `72dfdbc` made a hard error, and **no emitter code reads
`ClassConstructorInitializerNode`** — so even when that form was legal the base arguments were parsed and
silently discarded. SPEC claimed the feature worked until this was found.

**⚠️ Do NOT simply delete the production.** It is the only reason a class-named ctor *parses*, which is what
lets the emitter answer with the guided message (*"class-named constructor `X(...)` is no longer allowed —
declare a named constructor `ctor make(...)`"*). Deleting it turns that into a raw parse error. Keep it as
a diagnostic path and give `: base(...)` a message of its own — *"base-constructor delegation is not
supported; assign `this.base = Base.<ctor>(...)` instead"*.

## Sequencing

1 and 3 are **source-breaking**, so they land before the 1.0 tag or wait for 2.0. 2 is breaking only for
code exploiting the hole. 4 and 5 are diagnostics and can land any time.

1 depends on [slot-scope.md](slot-scope.md) D5 (the implicit `this`), so run that campaign first or fold
them together — it is already rewriting every ctor body.

## Verification

```sh
tools/cdev make && tools/cdev test
tools/cdev exec env KAMA_SAN=1 ./run_tests.sh
tools/cdev exec env KAMA_WASM=1 ./run_tests.sh
```

Baseline: **native 873 / ASan 838 / wasm 806, all 0 failed.**

The three parked repros in `tests/pending/` are the acceptance test: each currently COMPILES, and each must
become a rejection with a fixture asserting its message. They are deliberately outside the suite — a
fixture that passes by compiling is exactly how these hid.

**Widen the corpus while you are here.** 25 `extends` fixtures for a feature with five holes is the root
cause, not a coincidence. Anything added should exercise: a derived ctor initializing base state, `base.`
across all three visibilities, an `abstract` base, and a three-level chain (`tests/vtable_depth3.kama` is
the only one today).
