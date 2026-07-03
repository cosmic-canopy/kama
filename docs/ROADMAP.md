# cstar roadmap

Living plan for where cstar is headed. The guiding shape (set 2026-06-30):

- **1.0 — language complete.** Every *language* feature lands: OO + RAII + the value/ownership
  model, **generics**, **sum types + pattern matching**, **operator overloading**. After 1.0 the
  language surface is stable — you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection +
  declarative serialization, file I/O, networking, an embedded/MCU target. Mostly library +
  codegen work, little-to-no new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via
  a TinyCC-JIT / wasm REPL.
- **Engine track** (the product north star): a portable lightweight **WebGPU** game engine, woven
  through 1.x. Its Tier-0 math types are unblocked by operator overloading in 1.0.

This file is the durable home for the plan + deferred decisions so they aren't lost. Near-term
milestones are also summarized in `CLAUDE.md` / `GOALS.md`.

## Road to 1.0 — language complete

Recommended order: **M26e → M26f → M26g → M26h → M27 → M28 → M29 (expr-position lowering) → M30 (struct-ordering + generics completeness) → M31 (operators) → Step 7 → tag.**
**Status (v0.1.65):** M27 (generics) ✅, M28 (tagged unions) ✅, M29 (expression-position lowering) ✅, **M30 (struct-ordering + generics completeness) ✅ COMPLETE** (a struct can hold a user `value` BY VALUE — `enum Event{Resize(Vec2)}`, `Box<Rock>` — via a unified topological struct order; `string` verified as a generic arg). **All the M27/M28 deferred gaps are now closed — next is M31 (operator overloading).** Rationale: close the
borrow-safety arc (M26e, done), then *complete the value model* (M26f — resource move semantics,
deep-copy, copy contract), then enable owned-interface storage (M26g — the engine needs it), then the
**type-model reframe** (M26h — the `value`/`resource`/`contract` vocabulary + access-control rules,
done *before* generics so generics is authored in it); then the type-system push (generics → sum types
— the dependency spine, since `Optional<T>` *is* a generic tagged union — and generics is the
biggest/riskiest piece, best done early with full runway); then operators as a visible engine-math
finale; then docs. `Optional<T>` (M28) is also the documented escape hatch for genuinely-conditional
ownership (see M26f).

*Numbering note:* M0–M26 are historical (done/committed). The remaining work is numbered by build
order, so generics — long reserved as "M23" but never built — becomes **M27**, and operators (a few
notes back called "M27") becomes **M29**. Each may sub-decompose (M27a/b…) like M25/M26 did. The
type-model reframe is inserted as **M26h** (the M26 ownership-model family), before M27 — so M27+ keep
their names/numbers. *(Build-order renumber, 2026-07-03: the M27/M28 deferred gaps are scheduled as two
cleanup milestones done **before** operators — **M29** expression-position lowering + **M30**
struct-ordering + generics completeness — so operators shift M29 → **M31**.)*

1. **M26e — borrow escape check** (second-class borrows). ✅ **DONE (v0.1.35).** Closed the last
   memory-safety hole; with the no-null work already shipped (M26a–d), fully delivers GOALS §3b.
   *(The `Weak` Try-pattern that was bundled here moves to M28, where `Optional` exists.)*
   - **Rule (DECIDED 2026-06-30):** **"borrow is parameter-only; storage requires ownership"** — no
     lifetime annotations (the simplicity bet). Finding: borrows are *already* second-class by
     construction (`ref`/`out` is only a param/arg modifier — no `ref` return/local/field). So M26e
     was tight: it turned the remaining escape vectors — a bare interface value (which borrows its
     object) **stored in a field, returned, or used as a collection element** — from ugly C errors /
     a silent `List<IShape>` hole into one clean, consistent cstar diagnostic guiding you to own the
     object (`Shared<IShape>`, explicit, never an implicit box). Interface *params/locals* (borrows)
     are unaffected. **No `ref` returns** (so no `ref T operator[]`); a returnable "reference" is
     always an owned smart-ptr **handle** (mutate via auto-deref), and value-container elements are
     mutated by the owner's own methods (tell-don't-ask; only *escaping* a borrow is banned). A
     future callback-lend (`grid.mutateAt(i:, with: (ref Cell c) => …)`) needs inline closures →
     post-1.0. Impl: `rejectStoredInterface` helper at emitStruct/prototype sites + registerCollection;
     xfail iface_field/iface_return/iface_collection. 110/110.

2. **M26f — complete the value model** *(in progress).* The give/copy model was only partly built
   (smart pointers, and collection `give`); this finishes it. **Sub-steps (order = safety first):**
   - **M26f-1 — class holds a collection/smart-ptr field.** ✅ **DONE (v0.1.36).** Split each
     `CSTAR_*_DEFINE` macro into `_TYPE` (struct, emitted before class bodies) + `_FUNCS`; zero-init
     the field in the ctor. Fixture `coll_field`. *(This also made the non-pod double-drop reachable
     — hence B is next.)*
   - **M26f-2 — resource values move-only + use-after-move analysis (option "B").** ✅ **DONE
     (v0.1.37).** Safety-critical (closes the double-drop M26f-1 exposed; ASan-clean, 123/123). **Resolved marker rule** ("silent default,
     scream when ambiguous" — see [TYPE_MODEL.md](TYPE_MODEL.md)): a **`resource` (destructible) value
     moves** — a bare named hand-off is a *silent move* (the source is consumed, its dtor suppressed),
     `give` is optional emphasis, and `copy` errors until the type has a copy contract (M26f-4). **No
     mandatory marker yet** (only one op is plausible); the "scream when both are plausible" branch
     switches on in M26f-4. **Move-tracking is compile-time ("B")**, not runtime drop-flags: an
     **inline three-state analysis** (per-local `NotMoved`/`MaybeMoved`/`Moved`; snapshot + merge at
     if/else joins) that rejects use-after-move, skips a definitely-moved local's dtor, and **rejects
     conditional-drop** (a value moved on some-but-not-all paths, live to scope-exit — the exact case a
     hidden drop-flag would need, ruled out by GOALS §3; **`Optional<T>` (M28) is the escape hatch**).
     Loops/switch use a **conservative rule** (reject moving a local declared *outside* the loop — no
     fixpoint); the fuller loop-fixpoint / break-continue-to-targets analysis is a forward-compatible
     upgrade if a real program needs it. Trigger = **destructibility** (the interim proxy for
     `resource`; the keyword arrives in M26h). Needs a migration sweep of existing destructible-value
     copy sites (they become moves; a reused source now surfaces a real latent double-drop).
   - **M26f-3 — collection deep-`copy`.** ✅ **DONE (v0.1.38).** `copy` on `Array`/`List`/`String` →
     a real deep copy: a `NAME__copy` runtime fn allocates a fresh buffer and copies the elements
     (the init site emits it, overwriting the shallow blit). Gated on **bitwise-copyable elements**
     (owns nothing) — a collection of resource-owning elements needs a per-element copy, deferred to
     M26f-4. Fixtures `coll_copy`/`string_copy` (independent buffers, ASan-clean) + xfail
     `copy_resource_coll`.
   - **M26f-4 — the `Copyable` contract (opt-in copy).** ✅ **DONE (v0.1.39).** A `resource`
     (move-only value) opts into copy by declaring a **public nullary `copy` returning its own type**
     — the interim spelling of the internal **`Copyable`** contract (the explicit `: Copyable` form
     needs `This`, so it lands with M26h/M27). Its presence makes the `give`/`copy` marker **mandatory**
     for that type ("scream when ambiguous" — both move and copy are now plausible): a *bare* hand-off
     is a compile error, `copy x` deep-copies via `copy()` (the source stays valid), `give x` moves.
     Handled at all four hand-off sites (init / argument / return / assignment). `copy` on a `resource`
     *without* the contract errors with guidance ("add a `copy` method to opt into `Copyable`, or `give`
     to move"). Since `copy`/`give` were hard keywords, they're now **contextual** — usable as method
     names (a `method_name` grammar rule) so a resource can literally name its opt-in method `copy`.
     Fixture `copy_resource` (all four sites + give/copy split, ASan/UBSan-clean) + xfail
     `copy_bare_ambiguous`; updated xfail `copy_value` (non-copyable → guidance). 128/128.
     *(Deferred to M26f-5: element-wise deep-copy of a collection whose elements are `Copyable`
     resources — `copy_resource_coll` stays rejected; and a latent pre-existing ordering bug —
     `computeDestructible` runs before `collectCollections`, so a class owning ONLY a collection
     field with no explicit `~dtor` isn't seen as a resource. Both orthogonal to the copy contract.)*
   - **M26f-5 — the comprehensive give/copy test matrix + two follow-ons.** ✅ **DONE (v0.1.40).**
     Three pieces: **(5a)** fixed a pre-existing ordering bug — `collectCollections` now runs BEFORE
     `computeDestructible` (which seeds `destructible = hasDtor || isCollection` and re-derives each
     collection's `elemDestructible`/`elemCopyable` from the final class destructibility), so a class
     owning ONLY a collection field (no explicit `~dtor`) is correctly a resource (move-only, its
     buffer freed — was a silent leak + shallow-copy hole). **(5b)** collection deep-`copy` of
     `Copyable`-resource elements: the `Array`/`List` `__copy` macro gained an `ELEM_COPY` param
     (bitwise `CSTAR_ELEM_MEMBERWISE` for POD, else the element's `Elem__copy`), so `copy` of a
     `List<CopyableRes>` deep-copies each element (fixture `coll_copy_resource`, ASan-clean); a
     collection of *non*-`Copyable` resource elements stays rejected (`copy_resource_coll`).
     **(5c)** the behavior matrix documented as a table in [SPEC.md](SPEC.md) (every cell → its
     fixture) + the missing `coll_give` (collection move) fixture. 130/130.
   - **The marker rule (RESOLVED):** a marker is required exactly when both *move* and *copy* are
     plausible — a **`resource` that has opted into a copy contract**, and **collections** (both ops
     real). Silent where there's one natural op: `value`/primitive → copy (`give` errors), a plain
     `resource`/`Owned` → move (a bare hand-off moves; `give` optional), `Shared`/`Weak` → retain
     (`give` = opt-in move). See [TYPE_MODEL.md](TYPE_MODEL.md).

3. **M26g — owned-interface storage** *(was M26f; before 1.0; in progress).* Smart-pointers over an
   interface element (`Shared<IShape>` / `Owned<IShape>` — a fat-pointer element, a real extension of
   the smart-ptr machinery) + interface fields / returns / collections, so polymorphism can be *stored*
   (the engine's `List<IDrawable>` scene). Bare stored `IShape` stays a clear error; ownership is
   explicit, never implicitly boxed. **Drop mechanism (DECIDED): a virtual-destructor slot in the
   interface vtable** (`void (*__dtor)(void*)`, set per impl / NULL when nothing to free) — lean
   handles, the standard C++ model, per-type static cost. Sub-steps:
   - **M26g-1 — `Owned<IShape>`.** ✅ **DONE (v0.1.41).** The handle IS the fat pointer
     `{void* obj; const I_vtbl* vtbl}`, `obj` heap-owned. `new Circle(...)` boxes a concrete impl
     (malloc → ctor → set obj/vtbl); `s.m()` dispatches through the vtbl; drop runs `vtbl->__dtor(obj)`
     then frees. Move-only (`give` nulls `.obj`). New runtime macros `CSTAR_OWNED_IFACE_{TYPE,FUNCS}`;
     `registerSmartPtr` accepts an interface element (`CollectionInfo.elemIsInterface`);
     `emitSmartPtrCall` routes an interface handle to `emitInterfaceDispatch`. Fixture `owned_iface`
     (polymorphism + a destructible impl freed via the vtbl slot + move, ASan-clean) + xfail
     `owned_iface_nonimpl`. 132/132.
   - **M26g-2 — `Shared<IShape>` + `Weak<IShape>`.** ✅ **DONE (v0.1.42).** The refcounted variants:
     the fat element `{obj, vtbl, ctrl}`; retain/release on the shared count (last strong handle drops
     the concrete via the vtbl `__dtor`); `Weak<I>` observes without holding, `upgrade() -> Shared<I>`
     (empty when expired), `valid()`/`expired()`. New macros `CSTAR_{SHARED,WEAK}_IFACE_{TYPE,FUNCS}`;
     the Shared→Weak reseat + `smartPtrInvalidate` learned the fat `.obj`/`.vtbl` layout. Fixture
     `shared_iface` (retain + weak + upgrade-alive + expired-after-death, ASan-clean).
   - **M26g-3 — storage.** ✅ **DONE (v0.1.42)** for **fields + returns** — an interface smart-ptr is
     an ordinary value type, so `Shared<IShape>` as a class field and `Owned`/`Shared<IShape>` as a
     return work directly (`rejectStoredInterface` only fires on a *bare* interface). Fixture
     `iface_ptr_store`. **Collections (`List<Shared<IDrawable>>`) — ✅ DONE in M27** (was deferred): the
     general smart-pointer-in-collection support (nested-generic element mangling + per-element RAII drop
     + foreach dispatch through the stored handle, M27b-beta-2) plus the `>>` token split (M27b-beta-4).
     Fixture `list_shared_iface`.

4. **M26h — type-model reframe** *(the vocabulary milestone; before generics; in progress).* Rename the
   type kinds to the ownership model — **`value`** (owns nothing, copies), **`resource`** (owns/identity,
   moves), **`contract`** (was `interface`) — and drop **`pod`** (a `value` picks field visibility per
   field). **Every type declaration is marked by a `type` keyword** — `type value Vec2`,
   `type resource Buffer`, `type contract Drawable` — parallel to `fn` on every function (GOALS §5,
   greppable/self-describing). The marker means the kind word appears only in a fixed position, so
   `value`/`resource`/`contract` are **never reserved** (they stay ordinary identifiers) — no
   contextual-keyword machinery, no grammar-conflict risk. `type` itself is reserved (collision-free).
   Sub-steps:
   - **M26h-1 — `type` marker + kind plumbing.** ✅ **DONE (v0.1.43).** Lexer `type` keyword; one
     `marked_type_declaration : TYPE modifiers_opt IDENTIFIER basic_identifier class_base_opt class_body`
     production (`%expect 1` unchanged — no new conflict); `ClassDeclarationNode.typeKind` + emitter
     `TypeKind {Legacy,Value,Resource,Contract}` on `ClassInfo`. `value`/`resource` route to `ClassInfo`,
     `contract` to `InterfaceInfo` (`InterfaceMethod` refactored to hold returnType+params so it builds
     from either an `interface` or a `type contract`); `isMoveOnlyValue` = **declared `resource`**
     (empty resource is move-only), legacy `class` keeps the destructibility proxy. Old `class`/`pod`/
     `interface` still work as aliases (suite stays green). Fixtures `type_kinds` (value copies +
     resource + contract dispatch + `value`/`resource`/`contract` as identifiers) + `type_resource_move`.
     136/136, ASan-clean.
   - **M26h-2 — enforce the access-control / ownership grid.** ✅ **DONE (v0.1.44).** Rules on the
     new-spelling kinds (legacy `class` stays lenient until h-3): `~dtor` ⟺ `resource`; **a `value`
     that transitively owns a resource is a compile error** (reuses the `computeDestructible` fixpoint —
     a destructible `value` = owns something → "declare `type resource`"); per-field visibility on a
     `value` (default private, `public` allowed, `protected` rejected) via a `fieldVisibility` helper;
     a `resource` field is private-only; **`protected` only on an extensible `resource`**;
     `virtual`/`abstract`/`final` reject on a `value`; a `contract` has no fields/bodies/ctor-dtor;
     a bad kind word errors. Also: an **empty `resource`** is move-only but has no dtor — tracked for
     move analysis, drop skipped (fix: record move-only-but-not-destructible locals; guard the cleanup
     dtor on destructibility). Positives `value_public_field`/`empty_resource` + 8 xfails
     (`dtor_on_value`, `value_owns_resource`, `public_field_on_resource`, `protected_on_value`,
     `virtual_on_value`, `contract_with_field`, `contract_with_body`, `bad_type_kind`). 146/146, ASan-clean.
   - **M26h-3 — migrate all fixtures, hard-cut the old keywords.** ✅ **DONE (v0.1.45).** Migrated all
     ~140 fixtures (+ the `bench` dispatch source) to `type value`/`type resource`/`type contract`
     (plain `class` → `resource` if it owns a dtor/collection/smart-ptr else `value`, the compiler as
     oracle; `pod class` → `type value` + explicit `public` fields; `virtual/abstract/final class` →
     `type … resource`; `extern class` → `type extern value`); deleted two now-obsolete legacy xfails
     (`pod_method`, `field_visibility` — a `value` may have methods and public fields). Removed
     `class`/`pod`/`interface` from the lexer, grammar, and the dead emitter `pod` code (grammar stays
     `%expect 1`; `class Foo {}` is now a parse error). Fixed rule-4 (a `final resource` override uses
     `protected` by NVI). Regenerated `docs/grammar.bnf`. 144/144, ASan-clean. *(Prose docs
     SPEC/KEYWORDS/TYPE_MODEL updated next.)* Full model in [TYPE_MODEL.md](TYPE_MODEL.md). *(Sequenced
     before M27 so generics is authored in the new vocabulary — a `contract` bound, not `interface`.)*

5. **M26i — expression & temporary ergonomics + ISO-C conformance.** ✅ **DONE (v0.1.46).** Two small
   emitter fixes that clear the last everyday papercuts before generics, plus a portability win.
   **(a) `list[i].m()`** — a method call on a collection element now works: the element is borrowed
   *in place* via a bounds-checked `NAME__at` accessor (`T*` into the buffer), so a const method reads
   it and a mutating one mutates the *stored* element — uniform with every other `x.m()`, no copy, no
   temp, works for `value` and `resource` elements. **(b) inline constructor in argument position** —
   `f(x: Counter(start: 5))` now materializes a hoisted temp (`Counter __t; Counter__ctor(&__t, 5);
   f(x: __t);`), passed by value (a `value` copies, a `resource` moves — the temp is consumed by the
   callee, dropped once; nested inline ctors compose). **(c) ISO-C11 conformance** — building (b) added
   the **temp-hoisting pass** the emitter always anticipated, which let us **retire the emitter's one
   GNU statement-expression** (`({…})` at the by-value smart-ptr hand-off). The emitter now emits
   **strictly-conforming ISO C11** across the whole suite (a `-std=c11 -pedantic-errors` gate proves
   it); a single `({…})` survives only for a by-value smart-ptr hand-off inside a *loop condition*,
   where inline per-iteration evaluation is semantically required. Performance-neutral (same machine
   code as the manual workarounds). Fixtures `elem_method`/`ctor_in_arg` (+ resource-move, nested) +
   xfails `elem_method_const` (deep-const preserved) / `ctor_in_arg_ref`. 148/148. *(Resolves the two
   ergonomics limitations; the remaining untracked limitations are dispositioned under "Tracked
   limitations & non-goals" below.)*

6. **M27 — generics** — ✅ **DONE (M27a–M27c, v0.1.47–v0.1.54)** *(the long-reserved "M23", renumbered
   to its build order).* Full user-defined generics: `Map<K,V>`, multi-param + nested (`>>` lexing). The
   foundational type-system feature — unblocks `Optional<T>`, `Map`, and every future library type.
   *(Was slated to defer to 1.1; pulled back in for a language-complete 1.0.)*
   **Built:** M27a generic functions (call-site inference); M27b-alpha/beta-1 single- & multi-param
   generic types + nested-arg mangling; M27b-beta-2 smart-pointer-in-collection (`List<Shared<I>>`);
   M27b-beta-3 generic resources (`type resource Box<T>`); M27b-beta-4 the `>>` lexer split (nested
   generics with no space); M27c-1 contract bounds (`<K: I + J>`, monomorphized → static dispatch,
   enforced); M27c-2 the `This` self-type. Monomorphization throughout; ISO-C11; ASan/UBSan clean.
   **Known follow-ups (pre-existing gaps surfaced while building M27, orthogonal — now SCHEDULED):**
   *(a) a generic instance holding a user value-type **by value** (`Box<Rock> { Rock item; }`) hits a
   struct emit-order error → **M30** (topological struct-ordering pass). (b) an inline constructor as a
   **return expression** (`return Point(...)`) isn't lowered → **M29** (expression-position lowering).*
   - **DECIDED (2026-06-30):**
     - **Monomorphization**, not erasure — one specialized copy per concrete type (elements inline,
       no boxing). **Zero runtime/memory cost**; identical layout to today's intrinsics. (Erasure
       would box every element — rejected.)
     - **Architecture = "C": one engine, pluggable bodies.** Generalize the existing intrinsic
       monomorphization machinery so a generic type's method bodies come *either* from a C runtime
       macro (the built-ins — `List`/`Owned`/… keep their hand-tuned bodies) *or* from cstar source
       (user types). Full surface unification (`List<T>` and a user `Stack<T>` declared/used
       identically), zero perf risk. Reimplementing the built-ins *in cstar* (option "B") is deferred
       to self-hosting and is a no-rework continuation — see GOALS §1.
     - **Constraints = interfaces only, inline syntax.** `class Map<K: IHashable + IComparable, V>`;
       `+` means **AND** (all listed interfaces; no disjunction — static dispatch needs the exact
       method set). An interface **bound** is compile-time only → calls monomorphize to **static
       direct calls (zero-cost)**, distinct from an interface **value** (runtime fat pointer). No
       base-class bounds (use an interface). **Generic functions** too, not just types.
     - **give/copy** composes for free — the markers dispatch on the *concrete* type at each
       monomorphized site (a `T` that resolves to `Owned` moves, a pod copies). Confirm in impl.
   - **DECIDED — the `This` keyword** (a contract's self-type). When a contract must name its *own*
     implementing type (`Map`'s key needs "K equals K"), use **`This`** — chosen over Rust/Swift's
     `Self` because it pairs with cstar's `this` value (`this : This` = value : type), follows the case
     convention (types PascalCase), and is self-documenting per GOALS §5/§7. `interface IEquatable { fn
     bool equals(other: This); }` → used as `K: IEquatable`; also enables self-returning methods (`fn
     This clone();`). Distinct from a generic-interface *param* (`ISequence<T>` = "some OTHER type") —
     `This` = "my OWN type"; both can coexist. Resolved by a substitution pass at `implements` + each
     monomorphization. Ship **named-method** contracts (`equals`/`compareTo`) in M27; they *become*
     operators at M29. *(Lands in SPEC/KEYWORDS when M27 builds — per-milestone docs.)*
   - **Tech:** `>>` token split for nested generics (`Map<K, List<V>>`, `List<Shared<T>>`).
   - **Forward-compat (M27 build notes, from M28/M29 — neither changes M27's design):** make the
     monomorphization engine general over type *declarations* (class **and** enum/variant), since
     `Optional<T>`/`Result<T,E>` (M28) are generic tagged unions and the first consumers. M29's
     operator-interfaces (generic math) will *reuse* this milestone's interface-bound + `This`
     mechanism — M27 lays the groundwork, no conflict.

7. **M28 — tagged unions + `match` + `Optional<T>` + `Result<T,E>`.** ✅ **COMPLETE (v0.1.55–v0.1.59).** Sum types
   with exhaustive pattern matching; `Optional<T>`/`Result<T,E>` as *library* tagged unions, **not**
   compiler intrinsics — one way to do a thing. The "no forgotten case" capstone. Migrates
   `Weak.upgrade() -> tryUpgrade(): Optional<Shared<T>>` to its final form (no empty-Shared sentinel).
   - **Decided:** ONE unified `enum` spans the spectrum — a plain no-payload enum stays a bare C
     integer (zero regression); any payload-carrying or generic enum is a discriminated union
     (`enum Shape { Circle(float64 radius), … }`, payloads are named). `: IntType` pins the underlying
     int / tag width. `match` is a single **value-producing** construct usable in statement *and*
     expression position; compile-time exhaustiveness, `_` wildcard, payload binding into a fresh
     arm scope. Per-variant RAII (switch-on-tag dtor drops only the active payload; move-only iff a
     payload owns a resource). Bit-flag sets (choose-any-subset) are a *separate* later feature.
   - **M28a — declaration + layout + construction + `: IntType` + per-variant RAII.** ✅ **DONE
     (v0.1.55).** A payload/generic enum is backed by a `ClassInfo` (reusing M27 monomorphization +
     M26 ownership/move analysis); emitted as `struct { Tag tag; union {…} u; }`. Construction is a
     C99 compound literal (`Shape::Circle(radius: 2.0)`); smart-ptr payloads move in via the M26d
     hand-off. Fixtures `enum_payload`, `enum_payload_raii` (SAN); xfail `enum_variant_arity`,
     `enum_payload_byvalue` (by-value user value — the documented struct-order gap, hold behind
     `Owned`/`List`), `enum_recursive` (infinite-size by-value self-reference).
   - **M28b — value-producing `match`.** ✅ **DONE (v0.1.56).** One construct, both positions: a
     value in a local-init / `return` (lifted to a temp + switch — strict ISO C11, no statement-
     expression, reusing the M26i hoist), or a `;`-terminated statement (value discarded). Compile-
     time exhaustiveness (`case _:` wildcard), payload binding into a fresh arm scope (borrow, like a
     foreach element). Fixtures `match_value`, `match_stmt`, `match_shared` (SAN); xfail
     `match_nonexhaustive`, `match_bad_variant`.
   - **M28c — `Optional<T>` library sum type.** ✅ **DONE (v0.1.57).** A minimal implicit prelude —
     `enum Optional<T> { Some(T value), None }` in cstar source, parsed and collected before user
     code in the global namespace (resolvable unqualified everywhere, like the builtin collections),
     with its templates emitting nothing unless instantiated. Generic-variant construction resolves
     the instance from the target-type context (`Optional<int32> o = Optional::Some(value: 5)`);
     cross-scope mangling is anchored by the use-site ctx per instance. Fixtures `optional_basic`,
     `optional_multi`, `optional_shared` (SAN — `Optional<Shared<T>>` drops its handle once),
     `optional_nested` (`Optional<Optional<int32>>` with a nested `match`).
   - **M28e — `Result<T,E>` prelude sum type.** ✅ **DONE (v0.1.58).** `enum Result<T, E> { Ok(T
     value), Err(E error) }` in the prelude — a two-parameter generic union that fell out of the M28a
     monomorphization + M28c prelude for free (division of labor: `Optional` = absence, `Result` =
     fallibility). Fixtures `result_basic` (via a ternary), `result_shared` (SAN — the `Ok` handle
     drops once). *(Landed before M28d; the Weak migration is the heavier remaining piece.)*
   - **M28d — `Weak.tryUpgrade()` migration.** ✅ **DONE (v0.1.59). M28 COMPLETE.** `Weak<T>.upgrade()`
     (returned an empty-Shared sentinel you had to remember to `.valid()`-check) becomes
     `tryUpgrade(): Optional<Shared<T>>` — absence in the type, consumed by `match`. The C runtime
     `__upgrade` stays an internal helper; the emitter registers `Optional<Shared<T>>` per `Weak<T>`
     and emits a `__tryUpgrade` wrapper (`Some` on a live ctrl, else `None`), for both class and
     interface Weaks. Fixtures `weak_basic`, `weak_expired`, `shared_iface` migrated to
     `tryUpgrade()` + `match` (inline `match` on a call result ✅ **done in M29a**; a local binding also works).
     Every fallible op is now compiler-checked.
   - **Deferred gaps (surfaced building M28, orthogonal — now SCHEDULED, detail in M29/M30 below):**
     *(1)* the builtin `String` as a generic type ARG (`Optional<String>` emits the bare name, not
     `cstar_string`) → **M30**. *(2)* a value-producing `match` lifted only in a local-init/`return`
     (assignment RHS rejected) + single-expression arms → **M29**. *(3)* a resource payload built only
     from a fresh rvalue / bound-local `give` (inline `new`, collection move-in, nested inline
     `Some(Some(…))` not yet lowered) → **M29**. (Also in the memory's `roadmap-deferred`.)

8. **M29 — expression-position lowering.** ✅ **COMPLETE (M29a–d, v0.1.60–v0.1.63)** *(deferred-gap cleanup, before operators).*
   Generalize the one M26i temp-hoist pass so **every** value-producing construct lowers in **any**
   expression position — strict ISO C11, no GNU statement-expression (the M26i invariant). Today
   in-place construction / value-lifting only works in the positions each feature happened to wire
   (M26i: ctor-in-argument; M28b: `match` in a local-init / `return`); this milestone lifts them one
   level higher, uniformly. **Closes:** M27-b (inline ctor as a `return`/general rvalue,
   `return Point(...)`); M28-3 (inline `new` as a variant payload arg, a collection move-in as a
   payload, nested inline `Some(Some(…))` construction); M28-bonus (inline `match` on a call result,
   `match(w.tryUpgrade()){…}`); M28-2 (value-producing `match` in an assignment RHS `x = match(…)`, +
   multi-statement/block `match` arms).
   - **Mechanism:** thread the target-type context (`_matchTargetCType`/`_variantTargetType`) to the
     assignment-RHS + nested sites, and generalize the `_hoisted`/`_hoistOK` hoist so a class-resolving
     call is recognized as an inline ctor in general expression position (extend the M26a/M26i
     in-place-construction recognition beyond decl-init/arg). A context with no statement slot (a
     loop/branch condition) still cleanly rejects — never emits `({…})`.
   - **Sub-steps:** **M29a** ✅ **DONE (v0.1.60)** — call-return typing (an `exprClass` InvocationNode
     branch + `tryUpgrade`'s recorded return type) + an owning subject temp, so **inline `match` on a
     call result** works (`match(w.tryUpgrade()){…}`, the temp dropped once after the switch; fixtures
     `match_on_call` (SAN), xfail `match_in_cond`). *(Also folded in the macOS CI fix — the `SAN_FLAGS[@]`
     empty-array `set -u` bug under bash 3.2.)* **M29b** ✅ **DONE (v0.1.61)** — inline construction in
     general expression position: `return Point(...)` (inline ctor as a general rvalue) + inline `new`
     and nested `Some(Some(…))` as variant payloads, via the `tryHoistInlineCtor`/`tryHoistInlineNew`
     helpers (materialize a preceding temp, moved into the union; fixtures `return_ctor`,
     `variant_new_payload` (SAN), `variant_nested`; xfail `variant_new_iface`). **M29c** ✅ **DONE
     (v0.1.62)** — assignment-RHS value lowering (`x = match(…)`, `o = Optional::Some(…)`) via a narrow
     `_localCTypes` map for the LHS type + a dedicated assignment branch that drops a live destructible
     LHS before the blit; plus `give`-a-collection into a variant payload (`Some(give xs)`). Fixtures
     `assign_match`, `assign_match_drop` (SAN), `variant_coll_move` (SAN); xfail `variant_coll_copy`.
     **M29d** ✅ **DONE (v0.1.63)** — block `match` arms (`case X: { … }`): multi-statement arms in
     statement position, and in value position the block's trailing expression-statement (a call /
     assignment) is the arm value. Grammar adds `CASE match_pattern COLON block` (LALR-clean, `%expect 1`
     held); the collection scanner now recurses into `match` arms (so a type used only inside an arm is
     registered). Fixtures `match_block_stmt`, `match_block_value` (SAN — a block-local `Shared` drops at
     the arm end); xfail `match_block_no_value`.

9. **M30 — struct-ordering + generics completeness.** ✅ **COMPLETE (M30a–b, v0.1.64–v0.1.65)** *(deferred-gap cleanup, before operators).*
   - **M30a — unified topological struct-ordering.** ✅ **DONE (v0.1.64).** A generic instance / tagged
     union / normal class can now hold a user `value`-type **BY VALUE** (`Box<Rock> { Rock item; }`,
     `class Holder { Rock r; }`, `enum Event { Resize(Vec2 size), … }`). New `unifiedStructOrder()` — a
     post-order DFS over ALL laid-out structs (normal classes + generic instances + unions; collections/
     extern excluded) with edges = base + by-value field/payload containment; a generic instance's field
     deps resolve under its `_typeSubst`/use-site `_nsCtx` (mirroring `computeDestructible`), so
     `Box<Rock>`→`Rock` is an edge but `Box<int32>`→nothing. The two per-kind struct-body phases in
     `emitHeaderContent` merge into one loop over this order (per-node output unchanged — only ORDER
     moves); the generic-instance forward typedef splits out to the phase-(a) forward loop. A by-value
     cycle (self / mutual) is caught as an infinite-size error (subsumes the M28a self-reject). The M28a
     by-value-user-payload reject is removed. **Engine payoff:** event/render-command unions carry
     `Vec2`/`Color` payloads by value — no more `Owned` workaround. Fixtures: `enum_payload_byvalue`
     (xfail→positive), `struct_byvalue` (`Box<Rock>` + `Holder{Rock}` + cross-kind chain),
     `variant_value_raii` (SAN — a union carrying a `resource` value drops it once); xfail
     `struct_cycle_mutual`, `enum_recursive`.
   - **M30b — builtin `string` as a generic arg.** ✅ **DONE (v0.1.65).** Was a *false alarm*: the
     lowercase `string` keyword already resolves to `cstar_string` as a generic arg (`Pair<int32, string>`
     is in SPEC). Verified + fixtured `Optional<string>`, `Result<int32, string>`, `List<string>` (all
     SAN-clean — the owned `cstar_string`s free once). The capital-`String` alias / PascalCase spelling is
     deferred to the **Step 7 naming pass** (per user). Fixtures `optional_string`, `result_string`,
     `list_string`.
   - **Retires:** the struct-order tracked-limitation + the two `roadmap-deferred` struct-order/`String`
     entries.

10. **M31 — operator overloading + full static methods.** Ergonomic `pod` math — `Vec2 + Vec2`,
   `Vec2::dot(left:, right:)` — the engine's Tier-0 dependency. The value model (M26) already
   treats `Vec2 c = a + b` as a cheap pod copy. Validated by a first Vec2/3/4 + Mat4 library.
   - **M31a — full static methods ✅ (v0.1.66).** `static fn` carries no implicit `self` (`MethodInfo.isStatic`
     → `paramListC` gets a null self-type at both the prototype and the body); `Type::method(named:)` resolves
     in `emitInvocation`'s `::` branch (the qualifier head → a class, the method must be `static`) and lowers
     to `emitReorderedCall(cName, "", ...)` (no leading self — named args reorder for free). A `this`/bare-field
     use inside a static body, and `static`+`virtual`/`override`/`abstract`, are clean errors. Fixtures
     `static_method` (`Vec2::dot`), `static_method_noself`; xfail `static_this`, `static_call_nonstatic`,
     `static_virtual`.
   - **Design Qs:** Operator-method **syntax** — operators are the *sanctioned exception* to
     named-args-only (a binary op has exactly two operands, positional by nature); how do we spell
     it (`fn Vec2 operator+(Vec2 rhs)`? a special `operator` member? free-function form?). **Which
     operators** (arithmetic, comparison `== < >`, index `[]`, unary `-`/`!`, compound `+=`?). The
     **`static` method form** (`Type::method`, no `self` — `static` is only partial today, M19).
     Should `==` tie into a structural-equality default for `pod`s? **Operators-in-interfaces** for
     generic math (`interface IArithmetic { fn This operator+(This rhs); }`) reuse M27's interface-bound
     + `This` mechanism — so a generic `T: IArithmetic` gets `+`. (Does NOT impact M27's design; M27
     ships the named-method form, M31 makes the methods operators.)

11. **Step 7 — doc/SPEC reconciliation + naming pass.** Bring SPEC/KEYWORDS/GOALS/README current
   (give/copy + by-value from M26c/d, generics, `match`/`Optional`, operators; GOALS §3a unsafe
   wording vs shipped `unsafe{}`/`Ptr`). **Fold in the repo-wide naming/case convention pass**
   (lower-camel methods, PascalCase types) — 1.0 is the API-stability point, and post-1.0 renames
   are breaking, so settle it *now*.

12. **Step 8 — tag 1.0.** Nothing deferred — the language is complete.

## Tracked limitations & non-goals

Policy: **no known limitation stays untracked** — each is either fixed, tied to a milestone, or
declared a deliberate non-goal. The current inventory (swept from SPEC/KEYWORDS/emitter, M26i):

- **`list[i].m()` / inline ctor in arg** — ✅ **fixed (M26i).**
- **`export` keyword** — reserved, hard-errors today; **tracked to 2.0** (the cstar→host boundary —
  WASM module exports for the browser engine, and the scripting host interface). The engine's wasm
  build may pull a minimal `export` earlier. *(Was previously in KEYWORDS.md only, not the roadmap.)*
- **`volatile` keyword** — reserved, hard-errors today; **tracked to 1.x → Embedded/MCU target**
  (below): emit C `volatile` for ISR↔loop flags / MMIO registers.
- **Full `static` (`Type::method`)** — ✅ **done (M31a, v0.1.66).** **`operator` overloading** — hard-error today; **tracked to M31b.**
- **Collection passed by value (params/returns)** — `give`-ing a collection into a variant payload is
  ✅ **done (M29c)** (move the struct, null the source). General by-value collection params/returns
  (and `copy`/deep-copy of a whole container into a variant) remain **tracked to M31+** (reuse the same
  move-the-struct mechanism; not blocking).
- **`List<Shared<T>>` (smart-ptr-in-collection) + nested-generic `>>`** — ✅ **fixed (M27b-beta-2/beta-4).**
- **`contract` refining a `contract`** (`type contract A : B`) parses today; deeper multi-level
  contract inheritance is **tracked to M27** (alongside interface bounds + `This`).
- **Non-goal — function / constructor overloading.** Deliberately *not* planned: it conflicts with
  GOALS "one way to do a thing," and cstar's **named parameters** already cover the disambiguation
  overloading is usually reached for. Not a limitation to fix — a design decision. *(Reopen only if a
  concrete case shows named params can't express it.)*

## 1.x — systems & runtime (post-1.0)

Built ON the finished language; mostly library + codegen, not new syntax. These are also the
substrate the engine needs (asset I/O, scene serialization, networking).

- **Reflection + declarative serialization** — opt-in compile-time attributes/reflection →
  multi-format serialization (binary / json / yaml), little-endian canonical; scenegraph +
  serializer selection. A flagship system the user has wanted since early on.
- **File I/O** — safe file APIs; gates serialization and engine asset loading.
- **Networking** — native UDP/TCP sockets vs browser **WebRTC DataChannels** (unreliable "UDP") /
  **WebSockets** (reliable), via FFI (the browser has no raw sockets — a real wasm nuance).
- **Embedded / MCU target** — globals/statics for ISR flags, `volatile` *emit* (re-reserved in
  M19 for exactly this), ISR attributes, no-heap mode, avr/arm toolchains.
- **Perf — native dispatch devirtualization.** On the `dispatch` bench cstar matches C (≈6.18 ms,
  a real vtable indirect call ×8M) but **Rust is ~5× faster (1.18 ms)** — it devirtualizes/inlines
  the monomorphic case. A devirtualization / speculative-inlining pass, or `final`-method
  static-call lowering (M25b already tracks `final`), would close it.

## 2.0 — dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the *same* cstar syntax usable both as a compiled
language and as a scripting language with a full REPL. This rests on a **multi-backend
("polymorphic") emitter**: a shared Flex/Bison/AST front end lowered to more than one target, not
just the single C backend it has today.

- **Compiled (today):** cstar → C (`cstar.cemit`) → clang (native) / emcc (wasm). The **web
  target is wasm *via C*** (cstar→C→emcc) — there is no direct cstar→wasm; C is the portable
  middle.
- **Scripting / REPL (future):** the *same* front end + C emitter, but compiled and run
  **in-memory** instead of written to disk:
  - **Native fast path — TinyCC (TCC) JIT.** TCC compiles C in ~milliseconds in-process, so
    `cstar run foo.cstar` and the REPL are **JIT-compiled, not tree-walked** — near-native speed
    with instant startup. The REPL feeds each input through the front end, accumulates
    definitions, and TCC-compiles-and-runs. This is the answer to "fast interpreter on the native
    side": it's a JIT, not an interpreter.
  - **Web — run the wasm.** Browser scripting/REPL = compile to wasm (emcc) and run it in the
    browser's wasm runtime.
  - A dedicated **bytecode VM** backend is the alternative if a more dynamic REPL is wanted; the
    design leans **JIT/VM, never a slow tree-walker**.
- **Why it can be much faster than other scripting languages:** Python/Ruby/Lua are bytecode
  interpreters; cstar-as-script runs **compiled** (TCC-JIT native, or wasm) — at or near native
  speed, orders of magnitude faster than a bytecode interpreter. A future **cstar-script track**
  in the benchmark suite would measure this head-to-head (the suite today measures cstar
  *compiled* vs the scripting langs, which already wins handily).

## Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked by M29)
→ buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for
assets, serialization for scenes). See `docs/ENGINE_READINESS.md`.

## Performance (from the benchmark suite — `docs/benchmarks/RESULTS.md`)

cstar is at **C/C++ parity** on native compute and wins decisively on footprint (2 MB RSS,
66 KB binary) and the no-GC `alloc` workload. `cstar→wasm` (optimized — see below) **beats
hand-written JS on fib/pi/collatz/fnptr (up to ~4.5×)** and ties on dispatch/alloc.

- **Native dispatch — devirtualization** (tracked above under 1.x perf): the one real native gap.
- **WASM tiering (resolved — was an artifact, not a lag).** An earlier "cstar→wasm lags JS on
  pi/dispatch" finding was wrong: it was V8 running the short-lived wasm in its **baseline
  Liftoff** compiler (never tiering up to the optimizing **TurboFan** before the process exited,
  especially under load) while JS got its auto-JIT. Measured at the optimizing tier
  (`node --no-liftoff` — what a real long-running app gets automatically), cstar→wasm beats JS on
  fib/pi/collatz/fnptr and ties dispatch/alloc (strict IEEE). The bench now forces TurboFan for
  the wasm track. (Aside: `-ffast-math` is out — it would relax FP the baselines don't get.
  Isolated/cool, optimized wasm is ~10 ms — even further ahead — but the bench measures hot, so
  the committed numbers are the conservative, reproducible ones.)
- **Bench methodology (verified, don't re-chase).** Short workloads are skewed badly by
  **parallel load** — run the bench with nothing else competing. The `/work` bind mount
  (virtiofs/9p on macOS/Windows) adds only **~0.3–1.7 ms** for native *and* wasm under a
  controlled idle measurement — negligible. A named volume / tmpfs build dir would speed up
  *builds* on Mac/Windows but does **not** affect the measured numbers.

## Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to GitHub Releases;
  Marketplace publishing is deferred (pair with the eventual rename).
- **Brand rename** — "C*"/cstar is crowded; revisit before 1.0/publish (celestial direction tied
  to Cosmic Canopy, e.g. *Canopus*/*Carina*). User prefers the current name for now.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
