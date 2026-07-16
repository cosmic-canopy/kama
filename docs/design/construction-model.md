# Construction Model — DESIGN DRAFT (not final)

> **Status: DRAFT.** This is a pre-1.0 language-shape proposal under active design. It is **not**
> implemented and its decisions must **not** leak into `SPEC.md` / `ROADMAP.md` / `KEYWORDS.md` / code
> comments until this document is finalized. Origin: the 2026-07-16 design session (a move-tracker
> "bug" surfaced the whole question — see [[kama-construction-model]] memory and the session plan
> `~/.claude/plans/let-s-do-an-audit-fluffy-sketch.md`). Sequenced **after** the Items 1–6 hardening
> pass.

## 1. Motivation

Kama has two fixed hard stances: **no exceptions** and **no undefined behavior**. Consequence:
**construction is the one place a caller cannot react to failure** — a plain constructor can only
succeed or `panic` (fail-fast). Today a caller can handle a *fallible* construction only if the author
was wise enough to provide a named factory returning `Result` — i.e. it relies on **convention, not
enforcement**. Separately, the constructor is the only traditional single-name function (operators are
the sanctioned multi-dispatch exception), which creates overloading pressure.

## 2. Enforceable goal (stated honestly — no over-promise)

- **NOT claimed** (unachievable): "a caller can always react to construction failure." Whether
  construction *can* fail is invariant-knowledge that lives in the type; no language rule can hand a
  caller a fallible path the author didn't model.
- **IS the goal** (enforceable): no implicit/public raw constructor; all public construction is
  **explicit + named** with a declared return type; a single **isolated shared initializer** runs on
  **every** construction path; the type's invariant **cannot be bypassed**. This removes the implicit
  panicking default, makes construction uniform/one-way, and **dissolves constructor overloading**
  (named ctors disambiguate — consistent with the existing named-params-over-overloading non-goal).

## 3. Prior art (verified)

- **Rust** has no constructors — struct literals + associated fns by convention. Its only enforcement
  is **field privacy gates the struct literal**: a private field ⇒ outsiders must use the author's
  associated fns. It does *not* bless/enforce the named-construction seam, its name, or fallibility —
  those stay convention. Kama goes one step further: **bless the seam with real syntax + enforcement.**
- **Swift** designated vs convenience initializers — the model kama adopts for DRY (§7).

## 4. Construction taxonomy — "it's all Named Construction"

Every object comes from exactly one named constructor; the kinds differ only by input + keyword:

| Kind | Input | Author writes? | Returns | Call site |
|---|---|---|---|---|
| **memberwise** | field values | compiler-synth (**transparent `value` only**) | `T` | `Vec2(x:, y:)` |
| **`ctor`** (semantic) | semantic args | yes | `T` or `Result<T,E>` | `Color.rgb(...)` |
| **`decode`** (deserialize) | `Deserializer` | compiler-synth; author may override | `Result<T, DeError>` | decode/graph path |

- **`ctor` and `decode` are implicitly static** — no `static` keyword. Construction is a blessed
  type-associated operation. `public ctor rgb(...) -> Result<Color, E>` (no `fn`, no `static`).
- **Transparency earns a free ride.** A transparent `value` (all public fields, no invariant) gets a
  **compiler-synthesized memberwise ctor**, spelled `Vec2(x:, y:)` — safe *because* transparency means
  there is no invariant to bypass (same logic as Rust "public fields ⇒ open literal"). A `resource` /
  private-field / invariant-bearing type gets **no** implicit ctor and writes explicit `ctor`(s).
- **Value types may also add named ctors** (`Rect.square(side:)`, `Circle.unit()`); memberwise + named
  **coexist**. To *suppress* the memberwise (force construction through named ctors), make a field
  **private** → the type is no longer transparent → no memberwise synthesized.

## 5. Call-site greppability (HARD REQUIREMENT)

Construction must read distinctly from a plain static-method call at the call site. Marker: **dot-on-a-
type** (unused today) for construction vs `::` for a static fn:

```
Color.rgb(r: 0.5, g: 0.2, b: 0.1)   // CONSTRUCTION (a ctor)
Color::palette()                    // a regular static fn
Vec2(x: 1.0, y: 2.0)                // memberwise ctor (transparent value)
```

OPEN: confirm dot-on-type vs a call-site keyword; wire `Type.name(...)` into the parser without
ambiguity against instance `.` (enum variants and statics use `::`, so `Type.` is free).

## 6. Fallibility protocol

Two orthogonal axes:

- **Semantic fallibility (`ctor`, opt-in, BINARY):** the only choice is fallible vs non-fallible.
  Infallible `ctor` → `T`. Fallible `ctor` → **always `Result<T, E>`** (never `Optional` — a fallible
  construction always carries *why*). The return type is the contract; a declared failure MUST be
  handled (no exceptions). This is *not* convention-reliance: we killed the implicit panicking default,
  and the author encodes real failure semantics into the signature.
- **`decode` is ALWAYS fallible** — the one non-opt-in exception. It crosses the **serialization
  boundary** (an external, untrusted byte stream that can always be malformed), so it always returns
  `Result<T, DeError>`. Falls straight out of the `Deserializer` sticky-`failed()` model
  (`prelude/global.kama:185`). Asymmetry by design: semantic construction *may* be total; deserializing
  untrusted bytes never is.
- **Allocation fallibility (OOM) — a whole-program MODE, off by default.** Hosted default = OOM panics
  (fail-fast, too late to recover). It does NOT color ctor signatures (else every heap-bearing type
  returns `Result`). The roadmapped **fallible allocation** (`allocate -> Optional<Ptr>`, ROADMAP §5)
  is the separate opt-in seam for embedded/memory-budgeted builds. Mirrors Rust (`Vec::new` aborts vs
  `try_reserve`).

## 7. Initialization + DRY

- **One `init` (all-paths).** Runs after field-setup on **every** kind — memberwise, `ctor`, and
  `decode`. Carries the "true no matter how I was built" invariant (derived caches, normalization).
  **Infallible** (invariant setup only; validation-that-can-reject lives in the fallible `ctor` before
  it). Does **not** run on copy (a copy of a valid instance is already valid). This is the
  formalization of today's `onConstruction()`.
- **Natural-only setup** lives in the `ctor` body (or a private shared helper); `decode` doesn't run
  ctor bodies, so it's automatically bypassed. Rule: **`init` = "true on every path"; `ctor` body =
  "how this path builds me."**
- **DRY via designated/convenience (Swift).** One **primary** field-setter (the memberwise for a
  transparent value; an author-designated `ctor` for an invariant type). **Convenience** ctors compute
  args and **delegate** to the primary — they never set fields directly. `ctor square(side) => Rect(w:
  side, h: side)`. Two anti-drift guarantees: field-setup lives in exactly two places (primary `ctor` +
  `decode`), and the invariant lives in exactly one place (`init`).
- **Deserialize reconciliation.** `decode` keeps the privileged from-fields path (today: zero-init
  `(T){0}` + in-place wire field-set, `kama.cemit.cpp:2958`) then runs `init`. Today's enforced
  `onConstruction()` / `@generate(Deserialize, noOnConstruction)` (`kama.cemit.cpp:2963`) becomes:
  `onConstruction` ≡ `init`; `noOnConstruction` ≡ "no cross-path invariant." Keep the explicit
  enforcement: a type where construction can be bypassed (participates in `decode`) MUST declare `init`
  or explicitly opt out.

## 8. Locked decisions (2026-07-16)

1. Unify on named construction; transparent `value` earns a synthesized memberwise ctor `Vec2(x:, y:)`.
2. Keyword **`ctor`** (named) + **`decode`** (deserialize); both implicitly static.
3. Call-site greppability via **dot-on-type** `Color.rgb(...)` vs `Color::palette()`.
4. Fallibility: `ctor` binary opt-in (`T` or `Result<T,E>`, never `Optional`); `decode` always
   `Result<T, DeError>`; OOM = panic by default.
5. One `init` (all-paths, infallible, not on copy); natural-only setup in the `ctor` body.
6. DRY via designated (primary) + convenience (delegating) ctors.
7. Value types may add named ctors; suppress memberwise via a private field.

## 8b. Interaction with the shadowing ban (hardening Session A, landed)

The hardening pass (Session A, 2026-07-16) made **local variable shadowing** a compile error — a local
may not shadow a parameter, an enclosing-scope local, or an in-scope field. Deliberately scoped to
**locals only**: a **parameter** sharing a field's name (the `this.x = x` idiom) stays **allowed with no
usage restriction**. The stricter rule once considered here — "a field-shadowing param may only be read
inside the RHS of its own field's assignment" — was **deferred into this construction model** (user
decision), because (a) this model reshapes constructors anyway, so the param/field relationship is about
to change, and (b) the strict rule would reject legitimate patterns like `this.area = w * h`. When this
model lands, revisit whether named ctors / the shared initializer need any param↔field discipline beyond
today's "allowed." A static method/factory has no `this`, so the field case is moot there — consistent
with §2's "no implicit raw ctor; construct through named seams."

## 9. Open questions (resolve before/while implementing)

- **Delegation syntax** — `ctor square(side) => Rect(...)` (expression-delegate) vs an explicit
  `this.primary(...)` call. Pin one.
- **Primary designation** — marked (`primary ctor …`) or inferred (the one all others delegate to)?
  Enforce a single primary?
- **`new` / heap composition** — how dot-on-type composes with heap boxing (`new Color.rgb(...)` →
  `Owned<Color>`?); today `new T(...)` boxes.
- **Grammar** — wire `Type.name(...)` without ambiguity vs instance `.`.
- **Interactions** — generics (a generic type's ctors/monomorphization); inheritance/derived types
  (base-primary delegation — Swift two-phase init); contracts (a `ctor` in a contract?); enums/variants
  (do `Optional::Some(...)` stay `::` or move under construction?); **extern-`value` FFI stays OUTSIDE
  this model** (separate C-POD path).
- **Confirm at finalize** — `init` not on copy; `init`/opt-out enforced only where `decode` can bypass.

## 10. Implementation surface (touch-points)

- **Grammar (`kama.y`/`kama.l`):** `ctor` + `decode` keywords; `.`-on-type call form; delegation
  syntax; `init` block.
- **Emitter (`kama.cemit.*`):** construction lowering — synthesized memberwise for transparent values;
  named-ctor dispatch via dot-call; primary/convenience delegation; `init` invocation on every path;
  the `decode` synth (reuse today's `deserialize` synth `kama.cemit.cpp:2958-2988`); reconcile the
  `onConstruction`/`noOnConstruction` enforcement (`kama.cemit.cpp:2958-2966`).
- **Prelude/stdlib migration:** all `static fn T` factories → `ctor`; `Deserializer`/`Deserialize`
  (`prelude/global.kama:189-245`) → `decode`; every construction call site across `lib/`, `prelude/`,
  `tests/`, `examples/`.
- **Not part of this campaign:** fallible allocation (ROADMAP §5, the separate allocation axis).
