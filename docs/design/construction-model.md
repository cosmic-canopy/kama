# Construction Model — DESIGN OF RECORD

> **Status: FINAL design (converged 2026-07-17).** This is the agreed model for Kama's construction
> campaign. It supersedes the earlier DRAFT explored in the 2026-07-16/17 sessions. Implementation is
> sequenced as milestones M1–M8 (plan: `~/.claude/plans/let-s-start-the-construction-glistening-bentley.md`).
> As each feature ships, `SPEC.md` / `GOALS.md` §3d / `KEYWORDS.md` / `grammar.bnf` are updated to match.
> The "Explored and dropped" section at the end records ideas we deliberately rejected — do not resurrect
> them without revisiting that reasoning.

## 1. Motivation

Kama has two fixed stances: **no exceptions** and **no undefined behavior**. A consequence is that
**construction is the one place a caller cannot react to failure** unless the author provides a named
`static fn` factory returning `Result` — today that relies on **convention, not enforcement** (GOALS §3d).

This campaign **codifies and enforces best practices around object construction**. It makes construction
uniform (one kind of thing — the named constructor), makes failure a first-class value, and — the crux —
makes **"a returned object is always fully initialized, never null"** a **compile-time-enforced** guarantee.

## 2. The model

**A constructor is a named factory function that returns a fully-initialized object, or an error.** There
is exactly one kind of constructor; *everything*, including deserialization, is one.

- **Named, factory-shaped.** Called `Type.name(...)` (dot-on-type). Returns **`T`** (infallible) or
  **`Result<T, E>`** (fallible). A constructor is *not* the object — a fallible one returns `Err` and **no
  object**, so no half-built / null-bearing value can escape. Fallible vs infallible is the author's declared
  choice, encoded in the return type. Keyword: **`ctor`** (no `fn`, no `static` — implicitly type-associated).
- **Call-site greppability (hard requirement).** Construction reads distinctly from a static call:
  `Type.name(...)` is construction (dot-on-type), `Type::staticFn()` is a static fn, `Enum::Variant(...)`
  is an enum variant (both keep `::`). There is **no** nameless `Type(...)` call shape.
- **Deserialization is just a constructor.** *(Shipped as P4 — this is how the "`decode` is a ctor" idea
  landed; there is no `decode` keyword.)* A deserializable type is produced by one fallible ctor
  **`deserialize`** that receives a `Deserializer` and returns **`Result<This, Owned<Error>>`** — the *same
  shape as every fallible ctor*. The write mirror is a fallible **method** `serialize` →
  `Result<Unit, Owned<Error>>` (the one principled asymmetry: read constructs a `T`, write projects `self`).
  The error is the **boxed polymorphic `Owned<Error>`** (see §8): a concrete error enum (`DeError`/`SerError`)
  boxes into a fat `Owned<Error>`, recovered via `error.as<DeError>()`. Synthesized by default (enumerate the
  serialized fields via the `Deserializer` — the proven per-field read); overridable by hand (a user
  `ctor deserialize` reading fields via `r.deserialize::<T>()` + a flat `match`), which can chain to another
  ctor so shared logic reaches the deserialize path with no separate hook.
- **DRY = visible chaining.** A ctor reuses another by **calling it** (`return Color.fromHsv(...)`). Shared
  construction logic lives in the ctor others chain to (a private "base" ctor, by convention). `decode`
  reuses the same way — it chains to that base ctor (after reading fields), so shared logic reaches the
  deserialize path with **no separate hook**. There is **no `init`, no `final`, no `designated`, and no
  mandatory funnel** — reuse is an ordinary, greppable call.
- **`of` / `zero` are opt-in, bag-only conveniences.** A transparent `value` (all fields public) may opt in
  to a synthesized memberwise `of(...)` and/or a zero-init `zero()`. These are sugar for data bags — **not**
  a general designated/memberwise (a memberwise seam breaks on complex types, which is exactly why `decode`
  enumerates fields). Complex types construct through named ctors that set fields directly. A single private
  field removes `of` eligibility.

## 3. The two enforced guarantees (the point of the campaign)

1. **Correct return type.** Infallible ctor → `T`; fallible ctor → `Result<T, E>` (never `Optional` — a
   failure carries *why*), where `E` satisfies one **base `Error`**. *(Shipped: `E` is the boxed polymorphic
   `Owned<Error>` — §8, Model C. A concrete error enum derives `Error` and boxes in; the uniform `E: Error`
   bound on `Result` is deferred to M8, since ~17 `Result<_, int32>` sites still exist.)* `deserialize`
   returns `Result<This, Owned<Error>>` too.
2. **Complete initialization — compile-time.** Every member is assigned (to zero or a value) and every
   pointer is non-null **before the object is returned**. This is **definite-assignment analysis** over all
   members (the generalization of today's `checkCtorNeverNull`, which already proves owning pointers get
   set), checked at **compile time** for infallible ctors. It is **delegation-aware**: a ctor whose
   terminating move is `return OtherCtor(...)` is *complete by delegation* — the callee guarantees full init,
   so the caller isn't required to assign fields itself. This is what makes chaining clean without any
   "final" concept. Trivial pass-through (`this.p = p`) is provably complete. A fallible ctor returns `Err`
   before the object exists, so there is nothing more to check at runtime.

## 4. Nothing is constructible by default

There is **no implicit default/zero constructor for any type — including `value` types.** A type with no
ctor and no `of`/`zero` opt-in is unconstructable, and *using* it is a compile error. The diagnostic is
**context-aware** (accurate, never dangles an unavailable option):

- **always**: "type `X` has no constructor — define one (`ctor make(...) { … }`)";
- **only when `X` is a transparent value (all fields public)**: append "…or opt into `of` / `zero`";
- a value with a private field gets an optional one-line hint ("`of`/`zero` are available for value types
  whose fields are all public");
- a `resource` gets no `of`/`zero` mention.

The diagnostic is the teachable moment — the moment a designer meets the construction model. (This also
removes today's vtable-only synthesized default ctor: a polymorphic type must declare a ctor.)

## 5. `new` composes with construction

`new` = heap (→ an owning handle). `new Type.make(...)` → `Owned<T>` for an infallible ctor;
`new File.open(...)` → **`Result<Owned<File>, E>`** for a fallible ctor (`new` boxes `Ok`, propagates
`Err`). Fallible-`new` is the resource-acquisition idiom and is in scope (sequenced after infallible-`new`).

## 6. Full enforcement (after migration)

- The **class-named ctor *declaration*** (`public Rect(w, h) {}`) becomes an error → declare a named
  `ctor make(...)`. The nameless `Type(...)` call form is gone entirely; all construction is `Type.name(...)`.
- A `static fn` returning the enclosing type becomes an error → use a `ctor`. (Genuine static utilities that
  return *other* types — `Vec3::dot` → `float` — stay `static fn`, called with `::`.)
- A `resource` with owning (`Owned`/`Shared`) fields must construct through a ctor — definite-assignment
  guarantees the never-null seal (closing a latent hole where a ctorless owning-field resource null-inits the
  field).

## 7. Carve-outs

- **Enum variants stay `::`** (`Optional::Some(...)`).
- **Contracts may require a `ctor`** *(M8)*. A construction-shaped contract declares a `ctor` requirement
  (`type contract HeapOwner<T> for resource { ctor adopt(Ptr<T> raw); }`); conformance matches it by name
  to a `ctor` (or, during coexistence, a self-returning `static fn`) impl. This is the one construction
  capability a contract carries — a contract still holds no state and has no instance construction of its
  own. (Storing a raw `Ptr` needs no `unsafe`; only dereferencing does.) A contract's ctor requirement may
  be infallible (`ctor make(...)`) or fallible (`ctor Result<This, E> open(...)`), same as any ctor.
  **Generic code bounded by such a contract may construct through the type parameter** —
  `fn T fresh<T: Something>(int32 s) { return T.something(someval: s); }` lowers, per monomorphization, to
  the concrete implementer's ctor. This is why there is **no privileged `Default` contract**: "default
  construction" is simply a contract that requires a zero-arg ctor; a designer defines whatever
  construction contract they need and bounds on it.
- **extern-`value` FFI stays outside this model** (the C-POD aggregate-init path is unchanged).
- **Collections — the four-ctor matrix** *(M8c)*. Every growable collection (`DynamicArray`, `Deque`,
  `Map`, `Set`, `SlotMap`, `BitSet`) offers `empty()` / `withCapacity(n)` (default `GlobalAllocator`) and
  `withAllocator(a)` / `withCapacityAndAllocator(a, n)` (caller-owned `A`) — all `ctor`s (the M7.2
  `Map/Set::withCapacity` `static fn` exception is gone). `PriorityQueue` carries the capacity on its
  backing array (`minHeap/maxHeapWithCapacity`); B-tree `SortedMap`/`SortedSet` (no backing buffer) and the
  always-sized `FixedArray` keep their distinct shapes. The canonical zero-arg build is marked `default`.
  The default-allocator conveniences are gated **`when [A: default]`** *(M8d.0)* — a structural bound (the
  arg bound to `A` must itself have a `default` ctor, checked via `isDefaultFillable`; **no nominal `Default`
  contract**). So `DynamicArray<T, BumpAllocator>.empty()` *does not exist* (a clean "not available for this
  instantiation — requires `when [A: default]`" error, not a zero-allocator collection); a custom `A` uses
  `withAllocator`/`withCapacityAndAllocator`. This re-earns the compile-time protection the old `static fn`
  `Map/Set::withCapacity` had, and — because a gated ctor is dropped from the monomorph — removes the
  force-emit false-positive that blocked the completeness gate.
  **Remaining (→ M8d.1, the automatic seal):** the gate is currently *author-declared*. The completeness
  guarantee is only truly automatic once `checkCtorNeverNull`/`checkNamedCtorComplete` are widened to reject
  **any** ctor that leaves a non-default-fillable value field (e.g. `alloc: A`) unassigned — turning a
  *forgotten* `when [A: default]` (or the coexistence nameless primary, which still zero-inits `alloc`) into
  a hard compile error rather than a silent incomplete build. That is a pre-1.0 requirement (ROADMAP §2):
  never-null must be **compiler-enforced on the null field**, not left to author discipline.
- The **Equatable / Hashable / Copyable derive story** is designed alongside `of`/`zero` (one opt-in derive
  surface) but shipped separately (see §9 and ROADMAP §2). Kama today rejects auto structural `==`
  (`tests/xfail/operator_eq_missing.kama`), so a derive is a stance change designed with this family.

## 8. Base `Error` (companion dependency) — *shipped as Model C*

Because fallible ctors (incl. `deserialize`) return `Result<T, E>` and a ctor can **chain** to another, the
error must compose: there is **one base `Error`** that concrete errors (`DeError`, `SerError`, `IoError`, …)
derive. **Shipped (P1–P4) as the uniform polymorphic `Owned<Error>`:** `Error` is a base *contract*; a
concrete error stays an ergonomic Kama `enum` (matchable, payload-carrying) but gains the ability to be
**boxed** into a fat `Owned<Error>` handle and dynamically dispatched (`.message()`), and recovered with
`error.as<DeError>()` (a vtbl-pointer downcast → `Optional<DeError>`). A chained callee's `Owned<Error>` flows
into the caller's `Owned<Error>` by identity (already boxed). The remaining tail — a uniform `E: Error` **bound
on `Result`** + migrating the ~17 `Result<_, int32>` sites + dropping `onConstruction` — is deferred to **M8**.

## 9. Interaction with prior deferrals

- **Shadowing ban / param↔field discipline (from hardening Session A).** The strict "a field-shadowing param
  may only be read in the RHS of its own field's assignment" rule was deferred here. It is **not** adopted as
  a separate rule: with all construction in named ctors and complete-initialization enforced, a ctor freely
  computes field values from its inputs (`this.area = w * h` is fine); the guarantee we enforce is
  *completeness*, not *how* each field is computed.
- **Opt-in `Equatable`/`Hashable`/`Copyable` derive (from hardening Phase E).** Folded into this campaign's
  broader derive surface conceptually (same "opt-in, field-walked" shape as `of`/`zero`), but shipped as its
  own follow-up, not on this campaign's critical path.

## 10. Explored and deliberately dropped (do not resurrect without revisiting)

The design converged through a long dialogue; several richer mechanisms were considered and rejected as
over-engineering. Recorded here so they are not re-proposed:

- **A nameless `Type(...)` call shape** and a **`primary` keyword** to bless one ctor as nameless-callable —
  dropped: it reintroduces overloading-by-parameter-name pressure and a second way to spell construction,
  against "one way to do a thing." All construction is named `Type.name(...)`.
- **A pure-field-setter "primary"** (a ctor restricted to `this.f = f`) — dropped: too inflexible (real
  constructors compute fields, take transient inputs like a file path that isn't stored).
- **A compiler-synthesized memberwise as the *general* designated ctor** — dropped: a memberwise seam breaks
  on complex types (this is *why* `decode` enumerates fields). `of` is a bag-only convenience, not a general
  mechanism.
- **A separate `init` / `onConstruction` hook** (input-blind, auto-run on every path) — dropped: it doesn't
  prevent logic scattering ("ceremony for no gain"), and once `decode` is *just a constructor* that can
  chain, shared logic reaches every path via ordinary chaining without a hidden hook.
- **A mandatory "final" / "designated" constructor** every path must funnel through — dropped: the
  completeness guarantee comes from *definite-assignment*, not from a mandatory funnel; delegation-aware
  completeness makes chaining clean without one.

## 11. Implementation

Milestones M1–M8 with `file:line` touch-points, fixtures, and per-milestone triple-green (native / SAN /
WASM) acceptance live in the plan: `~/.claude/plans/let-s-start-the-construction-glistening-bentley.md`.
Sugar-first coexistence — old and new spellings both compile until M8 removes the legacy surface and turns on
enforcement — so the tree stays green at every commit across the ~60-factory / ~340-call-site stdlib
migration.

**Status.** M1–M6 done (the `ctor` keyword, dot-on-type calls, complete-init enforcement, `new` composition,
single-layer fallible serde, `@generate(of, zero)`). **M7 (stdlib migration) done for the non-generic families
(math, net) and the generic collections + smart pointers:** self-returning `static fn` factories are `ctor`s,
transparent values carry `@generate(of)`, and each container has a named `empty()` — all callable dot-on-type
or via the coexistence `::` bridge, triple-green. **M7 also required three emitter enablers (M7.0):** a generic
`ctor`'s body/return-type now specialize per instance, and a dot-on-type ctor call resolves a generic receiver
both by LHS inference and via an explicit turbofish `Type.ctor::<T>(...)`. Deferred to **M8** (coupled with the
enforcement flip): the resource instance-ctors that build in place (`Arena`, `TcpStream(fd)`, `JsonWriter`/
`JsonReader`, FixedArray's sized primary), removing the nameless `Type(...)` primaries, the ~340-site call
sweep to `Type.name(...)`, and the error-model tail (§8).
