# The contract model — conformance, hidden kinds, and what follows

*In-flight campaign doc. **Delete this file when the last campaign below ships**, once SPEC carries the
record — see the maintenance table at the top of [ROADMAP.md](../ROADMAP.md).*

> ### ►► What this is
>
> A design review held during M2a (2026-08-04) that started as "is retro-impl dangerous?" and ended with
> a coherent model for how *every* kind declares conformance. It schedules **four campaigns**, each its
> own session, in the order given. Nothing here is built yet.
>
> Read the *Corrections* section before re-deriving anything — three plausible-sounding claims were
> checked against the tree and turned out to be false.

## Why this exists

M2a needed `int32` to satisfy a `FromStr` contract and `float64` to satisfy a `Real` contract. Both went
through **retro-impl** (`implements C for T` at top level), which is the only mechanism kama has for
giving an existing type a contract. That prompted the question of whether retro-impl is a footgun — a
module reaching into a type it does not own.

Investigating it surfaced something better: retro-impl exists to paper over **two kinds that have no
spelling**, and giving them one removes the mechanism rather than fencing it.

## The core finding — two hidden kinds

[GOALS.md](../../GOALS.md) #3c states the rule: *every declaration is `type <kind> Name`*, and lists
`type value`, `type resource`, `type view`, `type contract`. Two kinds break it:

| kind | how it is spelled | consequence |
|---|---|---|
| `enum` | its **own grammar production** (`modifiers_opt ENUM type_decl_head enum_underlying_opt enum_body`) — not part of `type`, and **no `class_base_opt`** | an enum cannot declare conformance inline, so `implements Error for MyError` is *mandatory*, not stylistic |
| `intrinsic` | **no kama spelling at all** — `TypeKind::Intrinsic` exists only inside the compiler | a primitive cannot declare conformance either, so the prelude retro-implements onto it 68 times |

Everything retro-impl is used for traces back to one of those two gaps. Give both a spelling and the
mechanism has no remaining job.

## The design

```kama
type enum MyError implements Error { NotFound, Denied }

type intrinsic <int8, int16, int32, int64, uint8, uint16, uint32, uint64>
    implements Comparable {
    public fn Ordering compareTo(ref This other) { … }      // one impl replaces eight
}

type intrinsic string implements Equatable { }              // the NATIVE `equals` satisfies it
```

**The set form is load-bearing, and it is the type-list bound that failed elsewhere.** A type list cannot
serve `sqrt` — that needs a *different C function* per width (`sqrtf` vs `sqrt`) and kama has no in-body
type branching by design. It works here because these bodies are **genuinely identical** across the set:
they use raw `<` / `==`, which stay raw C operators for all-primitive operands and never recurse. The
prelude's 68 near-identical impls collapse to roughly 8–10.

**A contract is a SCOPE.** An intrinsic is decorated with methods *within the scope of a contract*, so a
contract-supplied method is not part of the intrinsic's own API:

- `(3).compareTo(other: 4)` — **rejected**; not the type's API
- `3` passed where a `Comparable` is expected — **fine**
- `sort(items: v)` — **fine**; bound dispatch
- `a < b` — **unaffected**; all-primitive comparisons stay raw C operators
- `string.equals` / `string.length` — **unaffected**; those are the type's *native* API

Disambiguation, when two contracts supply the same method name for one type, uses `::` — which already
means scope resolution in kama, and the contract *is* the scope:

```kama
Ordering o = Comparable::compareTo(self: x, other: y);   // static, zero cost
```

Same shape as Rust's `<i32 as Ord>::cmp`, reached from kama's own syntax rules rather than borrowed.

**The declaration IS the anchor.** Nothing widens a primitive to a contract value today (`Comparable c =
3;` appears nowhere in `lib/`, `prelude/` or `tests/`) and the reason is now clear: a `__as_<Contract>`
vtable is emitted **from a declaration site**, and an intrinsic has none. That same gap forced the enum
"Model C" promotion — a plain enum is rebuilt into a tagged-union `ClassInfo` on the fly so it can carry
a method and a vtable. Give both kinds a spelling and the machinery has somewhere to attach.

**The orphan rule falls out of ownership** instead of being bolted on. Nobody owns `int32` but the
prelude, so `type intrinsic int32 implements Real` is legal for `std::math` (which owns `Real`) and
rejected for a third party owning neither side. That closes the real hazard: two dependencies that have
never heard of each other both claiming the same (contract, type) pair and breaking a build neither
their user nor either author can fix.

### Four pieces of machinery retire together

1. **retro-impl** — no remaining job
2. **the nominal-recording special case** — `string`'s `Equatable` is currently "recorded from its
   built-in `equals` via `registerCollection`" *only because* an intrinsic could not say `implements`
3. **the enum tagged-union promotion** ("Model C") — a full type gets a real `ClassInfo` from the start
4. **the missing primitive→contract path** — now expressible for the first time

### Measured cost: one test file

Every stdlib call already goes through a bounded type parameter — `sort`, `priority_queue`,
`sorted_map`, `map`, `dynamic_array`, `fixed_array`, `deque` all call `a.compareTo(other: b)` or
`item.equals(other: …)` on a `ref T` / `ref K`. Only [tests/comparable.kama:22-36](../../tests/comparable.kama#L22)
calls `compareTo` on **concrete** locals — 9 of its 11 assertions — and rewriting those through a bound
is a better test anyway, since it exercises the supported surface.

## Corrections — do not re-derive these

Three claims that sounded right and are **false**:

- **"Retro-impl coherence is not enforced."** It is. `applyRetroactive` rejects a duplicate impl
  (``"`float64` already implements `Real`"``), a method clobbering an existing one, and an incomplete
  impl. What looks like a hole — `_retroConformances` being a `std::set` that silently dedups — is only
  the *pre-scan*, whose own comment says the real coherence check happens later.
- **"The contract `for` clause is stale now that `view` exists."** It is not. The clause expresses the
  *ownership* axis, and a view owns nothing, so grouping views with values is accurate. Moreover **there
  is no view-only contract possible**: contracts express *capabilities*, whereas a view's distinguishing
  property is a *restriction* (it may not be stored). Anything a view can do method-wise, a value can —
  so `for view` would have nothing to express.
- **"Cross-TU monomorphization divergence blocks specialization."** That hazard is real for
  separately-compiled languages (C++ templates, Rust crates) and does **not** arise here: kama runs one
  `CEmitter` per build and `collectProgram` receives prelude + built-ins + every user unit, so
  monomorphization decisions are made with whole-program visibility.

## The campaigns, in order

```
contract model  ->  full specialization  ->  const generics  ->  view-escape check
```

### 1 · Contract model

Build the design above. Open questions:

1. **Multi-method contracts** need a completeness check over the set form.
2. **Boxing cost** — `Owned<Error>` allocates; confirm the intrinsic path does not silently make that
   common.
3. **Guard rails** — a `type intrinsic` block must not declare fields; one block per (contract,
   intrinsic). The existing duplicate/clobber checks already have the right shape.
4. **Migration** — the prelude's 68 impls, `lib/std/io`'s one, and ~26 in `tests/`.

### 2 · Full generic specialization

**Universal, not an intrinsic workaround.** Once conformance is settled this stops being a substitute for
it and becomes what it is: polymorphism for generic *functions*, across all types.

- **Full, not partial** — a specialization's type arguments must be **fully concrete**: no type
  parameters, no bounds. Any two are then either identical (a duplicate → clean compile error, the same
  shape as the existing duplicate-operator rejection) or disjoint. No specificity lattice to define and
  nothing to prove — this is the sound half of C++'s explicit specialization. `View<int32>` qualifies;
  `View<T>` does not.
- Specializing on a **contract bound** is where overlap returns; defer unless a total order proves
  definable.
- Confirm the whole-program property (see *Corrections*) also holds on the `kama check` / LSP paths.

### 3 · Const generics → `Fixed<intBits, fracBits>`

Blocked today by three things, two of which are compiler bugs in their own right:

1. `ClassDeclarationNode::constParams` is **populated by the grammar and never read** by the emitter —
   const params on *types* are parse-only plumbing.
2. A const param **cannot be a runtime value**: `emitExpression`'s identifier branch never consults
   `_constSubst`, so `this.raw >> F` emits an undeclared C identifier and dies in the C compiler **with
   no kama diagnostic**. Silent bad codegen — worth fixing regardless of `Fixed`.
3. No **type-level selection** to map `I+F` onto a backing width, and no "next wider type" for the
   multiply. This is the genuine design question.

(`@generate(of)` is also rejected on generic types, which `Fixed16_16` uses — but that is fixable in
kama source by hand-writing a ctor.) `Fixed16_16` keeps its name meanwhile precisely to reserve `Fixed`.

### 4 · Derived view-escape check

Nothing verifies that a `view` can satisfy the contract it implements — every `isBorrow` use is at escape
sites, destructibility or isolate prep, none at the `implements` site. Reject at that site, over the
methods **actually injected** (the marker-contract pattern puts the factory in the impl rather than the
contract, so reading the contract alone would miss it): a static/ctor factory returning `This`, and
methods handing back `Owned<This>`/`Shared<This>`. `View.slice() -> This` stays legal because it borrows
the receiver.

Purely additive — no syntax, no contract re-declarations. Necessarily **partial**: boxing-idiom contracts
like `Error` have perfectly borrow-safe signatures (`fn string message()`) and stay caught later, at the
boxing site.
