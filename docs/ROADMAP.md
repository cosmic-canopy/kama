# cstar roadmap

The forward plan. The **language feature set is complete**. History lives in the git
log; this file is only about what's next.

## The shape

- **1.0 — language complete.** Every *language* feature has landed: OO + RAII + the
  value/ownership model, generics, sum types + pattern matching, operator overloading. After
  1.0 the language surface is stable — you build *with* it, not *on* it.
- **1.x — systems & runtime.** Capabilities built ON the finished language: reflection +
  serialization, file I/O, networking, an embedded/MCU target. Mostly library + codegen, little
  new syntax.
- **2.0 — dual-mode scripting** (flagship): the *same* language usable compiled OR scripted, via
  a shared IR feeding C, direct-wasm, and a bytecode VM — the `cstar` binary self-contained.
- **Concurrency — shared-nothing by construction** (1.x/2.0 direction): data-race freedom by
  removing shared mutable state, not by a borrow checker. 1.0 ships a single-threaded core.
- **Engine track** (the product north star): a portable lightweight **WebGPU** game engine,
  woven through 1.x. Its Tier-0 math types are already unblocked by operator overloading.

## Remaining before 1.0

1. **Documentation & spec reconciliation** *(in progress)* — bring every doc current with the
   shipped language and settle the naming/case conventions (PascalCase types, lowerCamel
   methods, no `I`-prefix on contracts, lowercase `string`). 1.0 is the API-stability point,
   so naming is fixed here.
2. **Tag 1.0** — nothing else is deferred; the language is complete.

## Tracked limitations & non-goals

Policy: **no known limitation stays untracked** — each is either scheduled or a declared
non-goal.

- **`export` keyword** — reserved, hard-errors today; **tracked to 2.0** (the cstar→host
  boundary: WASM module exports for the browser engine + the scripting host interface). The
  engine's wasm build may pull a minimal `export` earlier.
- **`volatile` keyword** — reserved, hard-errors today; **tracked to 1.x → Embedded/MCU**
  (emit C `volatile` for ISR↔loop flags / MMIO registers).
- **`contract` refining a `contract`** (`type contract A : B`) parses, but deep multi-level
  contract inheritance is not yet lowered — revisit if a real case needs it.
- **By-value collection params/returns** — `give`-ing a collection into a variant payload
  works; general by-value collection params/returns (and deep-`copy` of a whole container into
  a variant) are **tracked to post-1.0** (same move-the-struct mechanism; not blocking).
- **Non-goal — function / constructor overloading.** Deliberately not planned: it conflicts
  with "one way to do a thing," and **named parameters** already cover the disambiguation
  overloading is usually reached for. **Operators are the sanctioned exception** — a type may
  carry several `operator*` distinguished by operand type (`mat*vec`, `mat*mat`, `v*s`, `s*v`),
  matching C++/C#/Rust. Reopen only if a concrete example shows named params can't express it.

## Reference model, the standard library & near-term prerequisites

Three linked threads (policy: nothing untracked). The **prerequisites are now DONE** (the reference
model's one soundness hole is closed; the three fast-follows shipped); the **modular-stdlib shape**
below remains the open design question, sequenced before the collection rewrite.

### The reference model — why no borrow checker, and what it costs

cstar earns memory safety by making hazards *unrepresentable* (ownership + RAII, no null,
second-class borrows), **not** by statically checking first-class references — the same philosophy as
the no-GC and shared-nothing-concurrency positions. Concretely:

- A **borrow is second-class**: `ref`/`out` params, a `contract` value, an indexed place (`a[i]`,
  `ref a[i]`), and a place-returning `operator[]` result all borrow their object and are usable *only
  within the enclosing statement* — they cannot be stored in a local, a field, or a collection, nor
  returned further. There is **no `ref`-local / `ref`-field syntax**, so a borrow cannot outlive the
  statement that produced it. This is exactly why `operator[]` — and a future general `ref T`-returning
  method — need **no borrow checker**: the borrow is too short-lived to be invalidated, and the "I need
  a stored reference" case is served by **owning** it (`Shared<T>`/`Owned<T>`).
- **What the absence costs (honest):**
  - No first-class / stored borrows — patterns Rust writes as `&'a T` in a struct become refcounted
    ownership in cstar.
  - No static aliasing/exclusivity proof for the cases a borrow checker would cover.
  - **A known soundness gap — CLOSED (runtime, in the library).** Mutating a collection *while iterating
    it* — `foreach (ref e in v) { v.add(…); e = … }` — would be a **use-after-free** when the mutation
    reallocates. The old intrinsic `foreach` had a targeted *compile-time* guard (`fc212e7`), but the
    **library `List`** iterates through the opaque iterator protocol, so the compiler can't see the alias.
    Reinstated as a **C#-style runtime modification counter** living in `List`'s own cstar source: `add`
    bumps a `mods` field, each iterator snapshots it (via the `addr(of: place)` builtin — a `Ptr` to the
    counter) and `panic`s in `next()`/`hasNext()` if it changed, before the stale cursor is used. Safety
    in the library, cstar's stated boundary — a hand-rolled container decides whether to pay for it;
    `Array`/`Fixed` can't reallocate so they need none. Place-return escape is still enforced (a `ref T`
    result must borrow `this`/a `ref` param, never a local).
- **Position:** keep the no-borrow-checker stance (core to "favor simplicity" — no lifetime
  annotations), and *close the specific holes* the absence leaves (as done above) with targeted rules
  rather than a whole analysis — the way null-safety is enforced.

### Fast-follow prerequisites for a cstar-written standard library — DONE

Place-returning `operator[]` (shipped) lets a collection be written *in* cstar; three small features
(`2a5beec`, `fc212e7`) finished the prerequisite set, proven end-to-end by a generic heap `Vec<T>`
written entirely in cstar (`tests/opindex_vec`, ASan-clean):

- **`sizeof(T)`** ✅ — compile-time size builtin (mirrors `cast<T>`); monomorphizes, so `n * sizeof(T)`
  works in a generic `Vec<T>` over `Ptr<T>`.
- **`panic` / `assert`** ✅ — named-arg trap builtins over a new `cstar_panic` runtime helper (the
  `cstar_bounds_fail` shape); a user collection bounds-traps without FFI `abort()`.
- **General `ref T`-returning methods** ✅ — `fn ref T at(usize i)` etc. reuse the `operator[]` place
  machinery (caller derefs the call). *Remaining niceties (unscheduled):* the same for **free
  functions**; `debug_assert`-style release stripping; formatted panic messages.
- **`alignof(T)`** ✅ (`3eff4b4`) — sibling to `sizeof`, for arena/pool allocators.
- **`foreach` over user types** ✅ (`9b49ad7`) — a zero-cost **iterator protocol** (not index-based:
  containers aren't contiguous). Value: `iterator()` + `next() -> Optional<T>`; mutable: `iterMut()` +
  `hasNext()` + a place-returning `next()` (Rust's `iter()`/`iter_mut()` split). Resolved structurally,
  monomorphized to direct calls (no vtable). Proven on a generic heap `Vec<T>` and a non-contiguous
  linked list (`tests/iter_*`).

### Generic contracts (`type contract Foo<T>`) — DONE

Shipped: a **generic contract** is monomorphized-per-use exactly like a generic *type* — the template is
kept out of `_interfaces` (parked in `_genericContracts`) and each reachable `Iterator<int32>` becomes a
specialized `Iterator_int32` interface emitted under a bound `_typeSubst`. **Full value + bound parity**
with plain contracts: usable as a static bound `<I: Iterator<int32>>` (zero-cost, direct
`Concrete__next(&x)`) *and* as a dynamic fat-pointer value `Iterator<int32> it` (vtable dispatch).
Bound-checking matches by method **name** (type-parameter-independent), so it reads the template
directly — no instance needed just to constrain a type. This gives a *formal*
`Iterator<T>`/`Comparable<T>`/`Container<T>` opt-in + generic-over-iterator code. `foreach` still
resolves its iterator protocol **structurally** (the formal contract is additive). Fixtures:
`tests/gencontract_{bound,value,multi}`, `tests/xfail/gencontract_{unsat,arity}`.

- **Boundary — now closed.** A generic contract implemented by a generic class with its *own* type param
  (`class Foo<T> implements Iterator<T>`) initially wasn't monomorphized; resolved in step 1b below
  (`registerGenericTypeInst` rebuilds a generic instance's `implements` under its subst). Concrete and
  enclosing-param uses both work now.

### The standard-library shape — modular & opt-in (design question)

The roadmap already ties "**move the runtime into cstar**" (below, under 2.0) to the IR/multi-backend
work — "Multi-backend and the cstar-stdlib are the same project." Two questions to settle before
building it:

- **Is the math layer a stdlib *module* or language-level value types?** Recommendation: build the math
  types (`Vec2/3/4`, `Mat4 = Fixed<float32,16>` / `Fixed<Vec4,4>`, `Quat`) **now, as ordinary `value`
  types** — they need only operator overloading + `Fixed` (both shipped), **no** stdlib prerequisites —
  and *package* them as a module once the module mechanism exists. Unblocks the engine's Tier-0 math
  immediately and stays forward-compatible either way.
- **How does "modular / opt-in / pay-for-what-you-use" work?** The **prelude mechanism** (`PRELUDE_SRC`
  — parsed cstar collected before user code, the model `Optional`/`Result` already use) is the seed: a
  stdlib = more prelude-collected cstar modules in a `Std` namespace, `using`-imported; generic types
  already emit only when instantiated. **Caveat to solve:** plain (non-generic) functions are *not*
  reachability-pruned today, so a large module would bloat output — "pay for what you use" needs either
  dead-function elimination or explicit per-module opt-in. **A dedicated design pass is warranted**
  before the collection rewrite.
- **Contracts: structural or nominal (require `implements`)? — decided: NOMINAL / opt-in via `implements`.**
  Today `implements` is **advisory** for the two structural uses and required only for the fat-pointer
  value: `foreach` resolves an iterator by method name (`iter_*` never declare `implements`), and a bound
  `<T: Weighable>` accepts a type structurally (a `Rock` with the methods but no `implements` passes).
  That hybrid reads as *implicit*, against GOALS' *explicit-over-implicit*. **Decision (user): go nominal
  (Rust-like) — require `implements` for `foreach` and bounds too**, so the keyword is load-bearing
  everywhere and errors pin to the declaration. Touches every bound + the shipped `iter_*`/
  `generic_bound_*` fixtures, so execute it deliberately (a migration pass) here.
- **The capability-contract family** — the compiler-recognized, **opt-in-via-`implements`** contracts that
  form cstar's nominal, explicit vocabulary: **`Iterator<T>` / `IteratorMut<T>`** (**`foreach` is now
  NOMINAL** — prelude contracts `Iterator<T> for both { fn Optional<T> next(); }` + `IteratorMut<T>
  { fn bool hasNext(); fn ref T next(); }`; the library iterators (`ListIter`/`ArrayIter` → `Iterator<T>`,
  `ListIterMut`/`ArrayIterMut` → `IteratorMut<T>`) and every user iterator `implements` them; `foreach`
  verifies the declaration and rejects a structural-only match — xfail `iter_no_contract` — then emits the
  same zero-cost direct calls. Both sides are now nominal: container-side **`Iterable<T>` / `IterableMut<T>`**
  (xfail `iter_no_iterable`) advertise iterability on the type itself, and the **iterator-invalidation
  guard** is closed (C#-style `mods` counter in the library `List`, `addr(of:)` back-pointer — see the
  soundness-gap note above)), **`Deref<T>`** (DONE — auto-deref; see the sequence below),
  **`Copyable`** (below), **`Comparable<T>`** (future). All are the same species — a contract the compiler
  keys a capability off of. New ones follow the `Deref` template (prelude contract + a recognizer keyed on
  the contract name/instance).
- **`Copyable` as a formal contract (follow-up after `Deref`).** Today `Copyable` is *structural* — a
  `resource` is copyable if it has a public nullary `copy()` returning its own type (the compiler
  deep-copies a collection's copyable elements via it). Formalize as `type contract Copyable { This
  copy(); }` (non-generic, uses `This` — no generic contracts needed) and make recognition **nominal**
  (`implements Copyable`), bundled with the structural→nominal migration.
- **Smart pointers: intrinsic or library?** RAII + move-only + use-after-move are already general
  `resource` features, and the heap `Vec<T>` proved an RAII resource over `Ptr<T>`+`unsafe` works in pure
  cstar. The one missing language piece is **auto-deref** — a **`Deref` contract** so `ptr.method()`
  reaches the pointee (else the clunky `ptr.get().method()`). Auto-deref needs **no lifetime tracking and
  opens no new hole**: it returns a *place* rooted at `this` (through the owned `Ptr`), governed by the
  existing second-class-borrow rules (un-storable, root-at-`this`/ref-param, statement-scoped) — the same
  ASan-clean pattern as `Vec`'s `ref T operator[]`. **Gate auto-deref behind implementing `Deref`** (the
  *contract* is the gate — not a compiler-blessed smart-pointer list), keeping the one bit of implicit
  magic opt-in and legible. Plan: **move `Owned` to the library first** (unique = the `Vec`-of-one
  pattern; proves `Deref`), keep `Shared`/`Weak` intrinsic until their library versions (shared control
  block + refcount + `tryUpgrade`) are proven. The contract becomes the guarantee; the pointers become code.
- **Containers → library.** Make the collections library types, not compiler intrinsics (game devs roll
  their own precisely because one-size containers don't fit). This is the natural first stress-test of the
  module + `Deref` machinery. `Depth 2` (`ref T operator[]`) already unblocks writing them in cstar.

**Sequencing (decided — the self-hosting stdlib path).** Generic contracts are **done** (above). The
plan is bottom-up: build each enabling primitive, port an intrinsic to a cstar library type behind a
contract, then delete the intrinsic — shrinking the compiler core toward a real modular stdlib.

1. **`Deref` (auto-deref) — DONE.** Prelude `type contract Deref<T> { fn ref T deref(); }`; a type that
   `implements Deref<T>` forwards `ptr.method()`/`ptr.field` to the pointee (recursive → transitive). One
   central fallback at the `emitDispatch` "unknown method" point + parallels in `emitMemberAccess`/
   `exprClass`, keyed on the deref method's `ref T` return; the intrinsic `isSmartPtr` path is untouched.
   No lifetimes (place rooted at `this`, second-class-borrow rules), nominal/opt-in, inert when no
   `Deref` is in scope. Fixtures `tests/deref_{basic,heap}` (heap = a `Ptr<T>`+`unsafe`+RAII `resource`,
   the real smart-ptr shape, ASan-clean) + xfail `deref_missing`.
1b. **Generic class implements a generic contract — DONE.** `Box<T> implements Deref<T>` /
   `Own<T> implements Deref<T>` now work: `registerGenericTypeInst` rebuilds the instance's `implements`
   list under its own subst (`Deref<T>`→`Deref_Point`, register the instance); `linkBases` skips generic
   instances. This was the prerequisite for step 3 — a **generic** heap smart pointer written in cstar now
   auto-derefs (fixture `tests/deref_generic`: `Own<T>` over `Ptr<T>`+`unsafe`+RAII, ASan-clean). Suite 281
   green (native+wasm+ASan/UBSan). **Also closes the enclosing-template-param follow-up under *Generic
   contracts*.**
2. **Construction primitive (`new` → `HeapOwner`/`adopt`) + `drop` — DONE.** `new T(args)` is no longer
   hardcoded to the intrinsics: a type implementing the prelude contract
   `HeapOwner<T> { static fn This adopt(Ptr<T> raw); }` is a valid `new` target, lowered to
   `p = malloc; if(!p) panic; T__ctor(p,args); dst = Owner::adopt(p)` — **guaranteed zero-copy placement
   at every opt level** (verified in the emitted C; no optimizer reliance), with `new` still valid ONLY
   into an RAII owner (can't leak). Also added the missing OOM guard to the intrinsic `new`. New
   `drop(value: place)` builtin runs a place's dtor (no-op if non-destructible), so a library owner over
   `Ptr<T>` drops its heap pointee before `free`.
3. **Smart pointers as cstar library types — unique owner DONE (concrete + polymorphic).** A library
   `Box<T>` written in cstar: construct via `new`, auto-deref via `Deref`, move-only + use-after-move +
   RAII free from `resource`, pointee `~dtor` via `drop` — ASan/LeakSanitizer-clean (`tests/box_{basic,
   move,dtor}` + xfail). **Polymorphic ownership too — `Box<Shape>` (Rust's `Box<dyn Trait>`):** a
   `HeapOwner` instance over a *contract* element is routed through the interface-owner path (inline
   `{obj,vtbl}`, virtual dispatch, vtable `__dtor`), **intrinsic perf parity — one malloc, verified in the
   emitted C** (`tests/box_iface{,_dtor}` + xfail). One user-facing `Box<T>`; lifetime/RAII in the library,
   **type erasure in the compiler** (it can't be safe library code — the Rust boundary). RAII/move/UAM are
   general `resource` behavior, not smart-ptr magic.
3b. **Reference-counted ownership (`Rc`/`Weak`) — DONE (concrete), pure library.** `Rc<T>` = a Copyable
   `resource` over `{Ptr<T> p; Ptr<Ctrl> c}` — `copy r` retains (strong++), `~Rc` releases the object at
   `strong==0` + the ctrl at `weak==0`; `RcWeak<T>` (non-`Deref`) with `downgrade()`/`tryUpgrade() ->
   Optional<Rc<T>>` (Some while alive, None after drop). **Zero new compiler features** — retain-on-copy,
   move, use-after-move, RAII all fall out of `type resource` + Copyable. `tests/rc_{basic,move,dtor}`,
   `weak_upgrade` (ASan/LeakSan-clean) + xfails. Along the way, fixed a compiler segfault on
   **mutually-recursive generic types** (`Rc`↔`RcWeak`): `deepSubstType` now substitutes a generic arg's
   inner params so a nested arg isn't stored self-referentially in `_typeSubst` (was looping `mangleElem`).
   **Interface `Rc<Shape>` (strong) — DONE:** the `Box<Contract>` divert now picks the smart-ptr kind by
   the template's **Copyable** flag — Copyable (retain-on-copy) → refcounted `CSTAR_SHARED_IFACE`
   (`{obj,vtbl,ctrl}`), move-only → unique `OWNED_IFACE`. So `Rc<Shape>` is intrinsic-parity refcounted
   polymorphic ownership (`copy r` → `ctrl->strong++`, virtual dispatch, vtable `__dtor` at `strong==0`;
   `tests/rc_iface{,_dtor}`, LeakSan-clean). **Weak interface `RcWeak<Shape>` — DONE (parity-complete):**
   the divert discovers the weak partner (the method returning a non-self generic resource,
   `Rc.downgrade -> RcWeak<T>`), routes `RcWeak<Shape>` to the intrinsic Weak IFACE, and generates the
   library API on the erased pair — `Rc_Shape.downgrade()` (field-copy + `weak++`) and
   `RcWeak_Shape.tryUpgrade() -> Optional<Rc_Shape>` (the runtime `__upgrade` wrapped). One friction —
   names: `CollectionInfo.ifacePartner` stores the paired `Shared`↔`Weak` instance name (default =
   conventional prefix, intrinsic path unchanged; overridden to `Rc_Shape`↔`RcWeak_Shape`).
   `tests/weak_iface{,_dtor}`, LeakSan-clean. **THE FULL MATRIX IS COVERED — Owned/Shared/Weak ×
   concrete/contract — the library smart pointers reach parity with the intrinsics.**
4. **Smart-pointer purge — ✅ DONE.** `Owned`/`Shared`/`Weak` now live in **`lib/std/memory/`** (ordinary
   cstar), pulled in with `import std::memory::{…}`. The compiler's **concrete** `isSmartPtr` recognition is
   deleted (`cType`/`isCollectionType`/`registerCollection` name-matches gone); the **IFACE type-erasure**
   path stays (polymorphic `Shared<Shape>` still routes there via `registerGenericTypeInst`). `Shared`/`Weak`
   are **copy-only** (`implements Copyable, !Movable`): bare hand-off retains, `give` is an error, `Weak` is
   made with an explicit `.downgrade()` (no implicit `Weak w = s`, no default-empty `Weak`). The purge
   surfaced and fixed real cross-cutting gaps: **cross-module generic instantiation** (`absolutizeType` +
   emit generic instances under the *template* ctx, so a `std::memory::Owned<Counter>` used in another file
   mangles its arg and its module-local `Ctrl` correctly), a **`symbolAliases`-on-`ClassInfo`** hole (an
   imported generic used as a class/enum-payload *field* now resolves), method-return typing on a generic
   instance (`match(w.tryUpgrade())`), library-owner **auto-deref in `ref` position**, **ctor field-init** of
   a copy-only field (zero-init + null-guarded library dtors), and `BindableFunctionPtr` binding a library
   owner. Suite **314** green (native + wasm + ASan/UBSan/LeakSan). The `box_*`/`rc_*` fixtures stay as
   user-authored-smart-pointer coverage (the library is written the same way — no compiler privilege).
4b. **`Copyable` structural → nominal (Phase 3) — ✅ DONE.** `implements Copyable` (recognized by name,
   like `Movable`) is now the opt-in for copyability; a lone public nullary `copy()` no longer implies it
   (explicit over implicit). A class implementing `Copyable` must provide the `copy()` method (validated in
   `collectClasses`). Migrated the `rc_*`/`weak_*`/`copy_*` fixtures to add `implements Copyable`; new
   `tests/copyable_nominal` (lone `copy()` = move-only) + xfail `copyable_no_method`. Suite **316** green
   (native + wasm + ASan/UBSan/LeakSan).
5. **Module / `import` system — ✅ DONE.** Explicit, per-symbol, TypeScript/Rust-flavored `import` on
   cstar's existing `::`-namespace machinery. Four forms (`import a::b;` qualified-only · `import a::b as m;`
   whole-module alias · `import a::b::{X, Y as Z};` per-symbol/renamed); no glob. Visibility is a **top-of-file
   `export { A, B };` manifest** (module-private by default; non-exported = un-importable; declarations carry
   no visibility modifier, so `type`/`fn` syntax stays uniform; mirrors `import`); the reserved wasm `export`
   was renamed **`expose`** (three boundary vocabularies: `import`/`export` module, `public`/… member,
   `expose`/`extern` host). Modules = a namespaced file *or* a directory of same-namespace files; resolved by `::`-path under
   the importing dir, `$CSTAR_PATH`, then the **binary-anchored stdlib** (`std`/`core` reserved). Transitive,
   dedup-by-path (cycles load once), already-loaded namespaces satisfy imports without disk lookup; pruning
   stays the linker's `--gc-sections` job. **`using` retired** entirely (migrated to `import`). Suite **305**
   green (native + wasm + ASan/UBSan); fixtures `mod_import_basic`/`mod_alias`/`mod_dir_module` + xfails
   `mod_missing`/`mod_export_private`/`mod_collision`. The reachability-pruning "caveat" was a non-issue —
   `--gc-sections` already ships in release builds. **Now unblocks step 4** (the smart-pointer purge lands
   `Owned`/`Shared`/`Weak` into `lib/std/memory/` — the module's first residents).
6. **Containers as cstar library types → remove intrinsic containers** — repeat the port for the
   collections. **`List<T>` DONE** — `lib/std/collections/list.cstar` (growable, heap-owning, bounds-checked
   `operator[]`, value + mutable iterators, deep-`copy()`, dtor); `implements Copyable(bare: give) when T:
   Copyable` so `List<int32>` copies while `List<Owned<Shape>>` compiles + is fully usable (the conditional
   `copy()`/`iterator()` are emitted only when the element is Copyable). Intrinsic `List` recognition removed
   from `cType`/`isCollectionType`; ~35 fixtures + the `dispatch`/`alloc` benches migrated to `import
   std::collections::{List}` + `List()`. Resource elements borrow (`foreach (ref T …)`); the by-value
   iterator is Copyable-elements-only. **`Array<T>` (fixed-size sibling) remains** — same pattern, ~4
   fixtures, then the intrinsic `registerCollection`/`CollKind` path is fully retired.
7. **Reflection + attributes** — the opt-in reflection system (design brief below); the attribute
   language feature + serialization modules. Comes last, on top of the modular stdlib.

In parallel, buildable anytime: the **math value types** (no prereqs — operator overloading + `Fixed`
shipped); *package* them once the module mechanism exists. (Note: const generics, `Fixed<T,N>`,
place-indexing, `operator[]`, and generic contracts all landed as language features *after* the "language
complete" framing above — so where the **1.0 cut** falls, relative to these prerequisites, is itself part
of this discussion.)

### Reflection — design brief (step 7, after the smart-pointer + container ports)

Opt-in compile-time reflection driving polymorphic serialization. Requirements (from the design owner):

- **Opt-in / zero-cost when unused.** *Nobody pays a byte or a cycle if they don't reflect.* A type is
  inert unless it opts in; no global registry, no per-type metadata emitted unless requested.
- **Declarative marks on types** — an attribute syntax (e.g. `[Reflect]` / `@derive(...)` — spelling
  TBD) on a type opts it in. This is the new language surface (grammar + AST + emit).
- **Granular — opt-in reflectable members.** Per-field control (mark which fields are reflected /
  serialized / skipped), not all-or-nothing.
- **Scenegraph-capable (composition).** Reflect object graphs, not just flat structs — which typically
  needs **temporary IDs** to identify references within the chain (so a serialized graph can round-trip
  shared/back references without cycles).
- **Polymorphic by serialization output.** One reflection description, many back ends — swap the
  **serializer** to emit text / binary / YAML / JSON (little-endian canonical for binary). The serializer
  is a module; reflection is the substrate it reads.

Mostly codegen over `ClassInfo`; the only new *language* feature is the attribute mark (+ deciding how
per-member opt-in is spelled). Serializers land later as modules once the module system exists.

## 1.x — systems & runtime (post-1.0)

Built ON the finished language; mostly library + codegen, not new syntax. These are also the
substrate the engine needs (asset I/O, scene serialization, networking).

- **Reflection + declarative serialization** — the final step of the self-hosting-stdlib sequence
  (after the smart-pointer + container ports); see the **Reflection — design brief** above for the full
  requirements (opt-in/zero-cost, declarative marks, granular per-member, scenegraph via temp IDs,
  polymorphic serializer output). Serialization back ends follow as modules.
- **File I/O** — safe file APIs; gates serialization and engine asset loading.
- **Networking** — native UDP/TCP sockets vs browser **WebRTC DataChannels** (unreliable) /
  **WebSockets** (reliable), via FFI (the browser has no raw sockets — a real wasm nuance).
- **Embedded / MCU target** — globals/statics for ISR flags, `volatile` *emit*, ISR attributes,
  no-heap mode, avr/arm toolchains.
- **Native dispatch devirtualization** *(optimization, not a gap)* — on a *monomorphic* call site
  (a receiver of statically-known concrete type) Rust devirtualizes/inlines the call while clang
  does **not** devirtualize the emitted C vtable — so hand-written C is equally behind Rust there.
  It is a clang-vs-rustc optimizer gap, not a cstar defect. (On the corrected `dispatch` bench Rust
  converges to C and the ~5× vanishes; **cstar ties the C/C++/Rust cluster** and beats Go's
  interface dispatch — after fixing a redundant per-call re-read of the indexed element in the
  contract-dispatch emitter, which alone had made cstar ~2× before.) cstar can still win further
  where it *sees* the concrete type, by emitting a **direct call** instead of a vtable call — a
  laddered pass:
  - **Tier 1 — sound static devirtualization (no inlining).** Direct-call when the target is
    provable: a **concrete-value receiver**, a **`final` class/method**, or a **method with no
    overrides program-wide** (a "slot → overridden?" map built after `buildVtables()`). cstar's
    whole-program view makes the last one free, where C++ needs LTO + `-fwhole-program-vtables`;
    it runs before C emission, sidestepping the no-LTO/multi-TU limit. (`isFinalClass` /
    `MethodInfo::isFinal`, and the receiver's concrete type via `exprClass()`, already exist.)
  - **Tier 2 — intraprocedural type-flow.** Devirtualize a base-typed local with a proven concrete
    assignment. Sound, no inlining.
  - **Tier 3 — inlining-enabled / guarded devirtualization.** Either a cstar-level inliner (hard
    part: integrating callee scope-cleanup / drop order / move-state with the existing
    `emitScopeCleanup`/`emitUnwindAll`) then re-run Tier 1, or guarded/speculative inline caches
    (needing a type heuristic / PGO). A separate, larger project — pursue only if a real hot path
    (e.g. engine ECS dispatch) proves Tier 1 insufficient. Land Tier 1 first.

## Concurrency — shared-nothing by construction (design direction)

The intended concurrency model. **1.0 ships a single-threaded core**; this is the 1.x/2.0
direction, not a shipped feature. It earns data-race freedom the way cstar earns null-safety — by
making the hazard *unrepresentable*, not by checking it. Where Rust proves exclusivity over shared
memory with a borrow checker, cstar **removes the shared mutable state**.

- **Model — isolates + ownership-transferring channels.** An *isolate* is a shared-nothing unit of
  execution (≈ an OS worker natively, a Web Worker on wasm). Crossing a channel reuses the existing
  ownership model: send a `value` → **copy**; send a `resource` → **`give`** (move, zero-copy;
  use-after-send is already a compile error via move tracking); genuinely shared hot-path data → a
  narrow **`Atomic<T>` / shared-region** seam — the concurrency analog of `unsafe { }`/`Ptr` at the
  FFI boundary (opt-in, greppable, atomics-only).
- **Maps 1:1 onto wasm.** isolate → Web Worker; `give` across a channel → postMessage
  *transferable* (zero-copy, browser-enforced no-use-after-transfer); shared-region →
  SharedArrayBuffer + Atomics. Concurrency stays portable native↔browser from one source — which
  threaded C++/Rust do not.
- **Isolate vs job — two levels.** An *isolate* is the unit of *isolation* (few — roughly one per
  core / one Web Worker); a *task/job* is the unit of *work* scheduled onto isolates (many). The
  engine's job system / scheduler is a library on top, not language.
- **Structured concurrency = RAII for tasks.** A concurrency scope joins its child tasks at scope
  exit — deterministic task lifetimes, no orphans. The concurrency version of the no-leak
  guarantee; on-brand with RAII.
- **"Proceed until ready" without coloring.** The do-other-work-until-a-result-is-ready ergonomic
  is cheap tasks that block on a channel while a scheduler runs other ready work (the Go/Erlang
  model) — **not** Rust-style stackless `async/await`. Function coloring / `Pin` /
  self-referential state machines would be cstar's least-cstar feature, against "one way / favor
  simplicity."
- **Lock-free default, locks as expert opt-in.** The default path has no shared state → no locks.
  Atomics power the expert lock-free structures, built once in the engine/stdlib (as Rust's
  std/crossbeam build theirs over `unsafe`). No mandatory mutex-everywhere model.
- **Recommended language surface.** `isolate`/`task`, an ownership-transferring channel (reusing
  `give`/`copy`), `Atomic<T>`, a structured-concurrency scope, and two targeted *safe* sharing
  primitives that recover what shared-nothing otherwise costs:
  - **immutable `Shared` read-across-isolates** — immutable data is race-free even when shared
    (recovers cheap read-only sharing of big assets);
  - **scoped disjoint-slice parallel-for** — a scope lends each task a non-overlapping mutable
    slice of one buffer and reclaims it at join; safe by disjointness (the `rayon`/`split_at_mut`
    pattern). Recovers data-parallel mutation without general shared memory.
- **Deferred — general shared-memory ("hybrid").** Co-equal shared-memory threading is *not*
  planned; it reintroduces the hazard the model removes. Capability is retained (via the seam + the
  two primitives above); only some ergonomics move behind the seam. Reopen only if a concrete case
  the seam can't express appears — the same discipline as the overloading non-goal.
- **Positioning.** This turns concurrency from "the last gap vs Rust" into a *different, simpler,
  more portable* safe-concurrency model. Honest trade: Rust's shared-memory-with-static-exclusivity
  is more flexible for max-perf shared mutation; cstar's shared-nothing is far easier to reason
  about and portable to wasm. Prior art: **Dart isolates** (closest — shared-nothing, native+web),
  **Erlang/Elixir** actors, **Web Workers** + SharedArrayBuffer, **structured concurrency**
  (Swift/Kotlin/Trio); **Pony** for the type-level ceiling.

## 2.0 — dual-mode: compiled + scripting/REPL (flagship)

The end goal is **one language, two modes** — the same cstar syntax usable both compiled and as a
scripting language with a full REPL. The guiding constraint: **the `cstar` binary is the only tool
you need.** External C toolchains stay *optional* — used for the portable-C release path, never
required to write, run, or iterate on cstar.

This rests on a **polymorphic emitter**: one front end lowered to a **shared IR**, then rendered
by several backends.

```
   Frontend  (parser → type checker → ownership/move analysis)
                          │
                          ▼
                     shared IR          (monomorphized, drops inserted,
                          │              vtables + match/operators desugared)
          ┌───────────────┼────────────────┐
          ▼               ▼                 ▼
          C              WASM            Bytecode
          │               │                 │
     TinyCC / clang    browser /          native VM
     (JIT + release)   Wasmtime         (REPL, self-contained)
```

Every backend shares the same front end, so the safety analysis (ownership, move tracking,
exhaustiveness) is proven **once**, before the IR.

- **C backend — the portability moat (kept, always).** cstar → readable portable C → any C
  toolchain. `clang`/`emcc` for release; a bundled **TinyCC** for near-instant in-process JIT
  (`cstar run foo.cstar` and the REPL are **JIT-compiled, not tree-walked**). C stays a
  first-class target — "runs anywhere C runs" is the whole moat, and these new backends are
  *additive*, never a replacement.
- **WASM backend — the self-contained web path.** Direct cstar → wasm (no `emcc`), run in the
  browser or under Wasmtime. This is the web scripting/engine substrate; C→emcc remains the
  maximal-compatibility option.
- **Bytecode + VM backend — the self-contained native REPL.** A cstar-owned VM gives a true
  interactive REPL with zero external tooling — the strongest read of "the binary is the only tool
  you need."

**The IR is the crux, and the real work.** Today there is no IR: the C emitter writes C text
directly and *bakes in* monomorphization, RAII drop insertion, vtable layout, and match/operator
desugaring (`cstar.cemit.*`, ~150 methods). The refactor pulls that **semantic lowering up into
the shared IR**, leaving each backend a comparatively dumb renderer. Design constraints:

- **Keep the IR high-level and structured** (retain `if`/`while`/`for` and named locals), *not*
  SSA/basic-blocks — so the C backend can still emit the readable, `#line`-mapped C that is a
  headline feature. A low-level IR would forfeit that.
- **Move the runtime into cstar.** Collections/smart-pointers/`string` live as hand-tuned C in
  `cstar_runtime.h` today; a wasm or VM backend can't `#include` it. Reimplementing those
  intrinsics as **cstar generic library types** (unsafe core, safe API — already flagged in
  GOALS #1 for self-hosting) makes them flow through the shared IR and monomorphize into *any*
  backend. Multi-backend and the cstar-stdlib/self-hosting goal are the **same project**: do it
  once, all three backends inherit it.
- **Contain semantic drift.** A VM is a second execution semantics — the main risk. Having the C
  backend and the VM consume the *same lowered IR* reduces drift from "two languages" to "two
  renderers of one IR." Build the IR first; then a VM is a legitimate, low-drift option.

**Speed ladder** (fastest last): tree-walk < bytecode VM < TinyCC-JIT < AOT C→clang. If raw
scripting speed dominates, JIT wins; if zero-install and interactivity dominate, the VM /
direct-wasm win. The "binary is the only tool" constraint tilts the *default* iteration experience
toward the VM + direct-wasm, with the C/JIT path there when you want native speed or C
compatibility.

- **Why it beats other scripting languages:** Python/Ruby/Lua are bytecode interpreters; cstar
  scales from a self-contained VM up to JIT/AOT-native — the same source, at or near native speed.
- *Licensing note:* TinyCC is LGPL; if a bundled JIT ships, confirm the linking terms against the
  MIT/permissive goal (GOALS #8). The VM / direct-wasm paths sidestep this entirely.

## Engine track (product north star)

A portable lightweight **WebGPU** game engine. Tiers: **math types** (Tier 0 — unblocked) →
buffers/bindings → first triangle → scene/material. Depends on the 1.x systems (file I/O for
assets, serialization for scenes). See [ENGINE_READINESS.md](ENGINE_READINESS.md).

## Performance (from the benchmark suite — [benchmarks/RESULTS.md](benchmarks/RESULTS.md))

cstar is at **C/C++ parity** on native compute (fib/pi/collatz/fnptr/alloc, and now dynamic
dispatch) and wins decisively on footprint (~2 MB RSS, ~66 KB binary) and the no-GC `alloc`
workload. `cstar→wasm` (optimized) **beats hand-written JS on fib/pi/collatz/fnptr (up to ~4.5×)**
and is near-parity on `alloc`/`dispatch`.

- **Native dispatch — the ~5× "gap" was an artifact.** The old workload used monomorphic call
  sites that let rustc devirtualize the call to plain arithmetic while clang did not (hand-written C
  was equally ~5× behind). The corrected `dispatch` measures *true* polymorphic dispatch over a
  heap-owned collection: **Rust converges to C** (artifact gone), and **cstar ties the C/C++/Rust
  cluster** (~6.2 ms) and beats Go's interface dispatch. Reaching that parity took one emitter fix —
  the contract-dispatch path was re-reading the indexed element twice per call, which alone had made
  cstar ~2×; hoisting it to a single temp halved the workload. Further devirtualization (1.x, above)
  is optional upside for code with statically-known receivers, not a gap-closer here.
- **WASM tiering.** Measure at the optimizing tier (`node --no-liftoff` — what a real long-running
  app gets); the bench forces TurboFan for the wasm track so the numbers reflect steady-state, not
  V8's short-lived baseline (Liftoff) compiler.
- **Bench methodology (don't re-chase).** Short workloads are skewed by parallel load — run the
  bench with nothing else competing. The `/work` bind mount adds only ~0.3–1.7 ms (negligible);
  a named volume / tmpfs speeds up *builds* but not the measured numbers.

## Tooling / distribution (deferred)

- **VS Code Marketplace publish** — the `.vsix` is built + attached to releases; Marketplace
  publishing is deferred (pair with an eventual rename).
- **Brand rename** — "C*"/cstar is crowded; revisit before publish. Current name kept for now.
- **FreeBSD CI** — a non-blocking `vmactions/freebsd-vm` job once Windows is proven on a tag.
- **Browser-debug ergonomics** — richer wasm source maps / a no-extension flow.
