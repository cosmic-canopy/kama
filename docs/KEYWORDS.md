# cstar keyword audit (through M26h)

> **Keywords added since the original M19 audit**: `fn` (M20 — every function/method declaration), `fnptr`
> (M21 — an explicit function-pointer type), the `::` scope-resolution operator (M20b), `give`/`copy`
> (M26c — ownership hand-off markers), and **`type`** (M26h — the type-declaration marker on every
> `type value`/`type resource`/`type contract`). All implemented; see their rows below.
>
> **M26h — the ownership reframe (SHIPPED).** `class`, `pod class`, and `interface` **no longer exist**.
> Every type declaration now begins with the `type` marker followed by a *kind*: **`type value`** (owns
> nothing, copies), **`type resource`** (owns/identity, move-only, RAII-dropped), or **`type contract`**
> (a public-only guarantee — replaces `interface`). The kind words `value`/`resource`/`contract` are
> **contextual, not reserved** — they mean a kind only right after `type`, and stay ordinary identifiers
> everywhere else (`int32 value = 5;`). See the [type-model doc](TYPE_MODEL.md) and the rows below.

Status of the reserved keywords (64 in the lexer), established by exercising each through
`cstar transpile`/`build` (the emitter dispatches on AST nodes, not tokens, so this is empirical). Legend:

- ✅ **works** — parses, lowers, behaves correctly
- ⚠️ **partial** — works in some forms, missing others
- 🪦 **dead** — parses but is silently dropped (a no-op)
- 🔒 **unenforced** — parses, but its *guarantee* is not checked
- ❌ **unsupported** — reserved; using it is a hard error
- 🐞 **bug** — see notes

## Status table

| Keyword(s) | Status | Notes |
|---|---|---|
| `bool` `int8/16/32/64` `uint8/16/32/64` `float32` `float64` `void` `string` | ✅ | core types |
| `int` | ✅ | alias → `int32` |
| `double` | ✅ | alias → `float64` (C `double`) |
| `type` | ✅ | **M26h: the type-declaration marker** — every type is `type <kind> Name { … }` (parallel to `fn`). The kind is `value`/`resource`/`contract` (+ the `virtual`/`abstract`/`final` qualifiers after `type`). Greppable (`grep '^type '`). Replaces the removed `class`/`pod class`/`interface` keywords |
| `value` `resource` `contract` | ✅ | **M26h — CONTEXTUAL, not reserved.** They name a kind **only right after `type`**; everywhere else they are ordinary identifiers (`int32 value = 5;`, a field/method named `resource`). `type value` = owns nothing, copies, sealed, fields private-or-`public` per field, no dtor. `type resource` = owns/identity, move-only, RAII-dropped, fields private-only. `type contract` = public-only guarantee (was `interface`): methods only, no bodies/fields/ctor/dtor; satisfied via `implements`; may refine another (`type contract A : B`) |
| `enum` `namespace` | ✅ | |
| `operator` | ⚠️ | reserved and parses (`operator+` etc.), but operator-overload **lowering is deferred to M29** — using it is a hard error today |
| `extends` `implements` `this` `base` `ref` `out` | ✅ | (`base(...)`/`base.m()` need **named** args) |
| `new` | ✅ | **M26a: the heap operator.** `Owned<Box> p = new Box(...)` boxes the **element type** on the heap; a stack value **drops `new`** (`Box b = Box(...)`). `new` into a plain value type is an error. (Collections keep `new Array<T>(...)` — the `Array<T>(...)` call form doesn't parse.) |
| `virtual` `override` | ✅ | **M25b/M26h**: an overridable method is written `protected` (never public/private), and its type must opt in as a `type virtual resource`/`type abstract resource` (extension is a `resource` concern — a `value` is sealed); `override` only re-seats a real base slot. (Bug 1 — ctor-less polymorphic type — fixed.) |
| `give` `copy` | ⚠️ | **M26c/d hand-off markers.** `give` = move (invalidates the source); `copy` = retain (`Shared`/`Weak`) or duplicate. Ride a **named** value (a fresh `new`/ctor/call result needs none) and work uniformly in **initializer, assignment, argument, and return** positions. Defaults by kind: `Owned` → give (copy is an error), `Shared`/`Weak` → copy, `value`/primitive → copy (give is an error), a plain `resource` (move-only) → move (bare hand-off moves; `copy` is an error until it opts into `Copyable`). **A collection requires the marker: `give` = move (buffer), `copy` = deep copy (M26f-3/5)** — element-wise: a bitwise-copyable element copies memberwise, a `Copyable`-resource element deep-copies via its own `copy()`; a non-`Copyable` resource element is rejected. **`Copyable` resource (M26f-4):** a `resource` opts into copy by declaring a **public nullary `copy` returning its own type**; then the marker is **mandatory** ("scream when ambiguous") — a bare hand-off is an error, `copy x` deep-copies via `copy()`, `give x` moves. Since `copy`/`give` are only markers in expression position, they're **contextual** — usable as method names (so the opt-in method is literally `copy`). Full behavior matrix (every cell → fixture) in [SPEC.md](SPEC.md) |
| `if` `else` `switch` `case` `default` `do` `while` `for` `foreach` `in` `break` `continue` `return` | ✅ | |
| `using` `true` `false` `null` `cast` `unsafe` `extern` | ✅ | |
| `static` | ⚠️ | a static method is callable via an **instance** (`c.foo()`), but type-level `C::foo()` is `unsupported`, and `static` adds no real static semantics (the method still takes `self`) |
| `volatile` | ❌ | **reserved** for the embedded/MMIO scope (ISR↔loop shared flags, peripheral registers) — using it is a clear error, not a silent no-op; implemented when cstar targets embedded |
| `export` | ❌ | **reserved** for the cstar→host boundary (WASM module exports, scripting host interface) — clear error on use, not a silent no-op |
| `public` `private` `protected` | ✅ | **M25a/b + M26h enforced.** Members are **private by default**; `public`/`protected` set it explicitly. `protected` = owner-or-subclass and is meaningful **only inside an extensible `resource`** (`type virtual`/`abstract resource`) — an error on a `value`, a plain `resource`, or a `contract`. **Field visibility is per field on a `type value`** (`public` field = the old "pod"); a `type resource` keeps fields private; a `type contract` has no fields |
| `friend` | ✅ | **M25c enforced** — granular, owner-granted grants: `friend <accessor>[members];` (or `[...]` = all privates). The accessor is a **type**, a **free function**, or a **`Type::method`**; it may then reach the named private members. Granting an unknown or public member is an error. (The old `friend(list)` member *modifier* is retired — one way.) |
| `abstract` | ✅ | **M25a/b + M26h**: `type abstract resource` is non-instantiable (`new` rejected; also when a subclass leaves an inherited pure method un-overridden); methods are `protected abstract`; the type must declare ≥1 overridable member. A qualifier after `type`, on a `resource` only |
| `final` | ✅ | **M25b + M26h enforced**: `type final resource` is a sealed leaf (cannot be `extends`-ed); `final` on a method seals its slot (no subclass `override`); only a `type virtual`/`abstract resource` may be extended at all (a `value` and a plain `resource` are already sealed) |
| `const` | ✅ | **const-correctness complete (M24a–e).** A `const` binding is deeply immutable (no reassign, no write *through* it, no `++`/`--`); `const fn` is non-mutating and the only kind callable on a const receiver; params take `const T` / `const ref T` (read-only borrow); a `const` field is write-once (constructor only); `const Ptr<T>` lowers to `const T*` for const-correct FFI |

## Bugs found

**Bug 1 — polymorphic type without an explicit constructor crashes (SIGBUS).** *(Fixed; recorded in the
pre-M26h `class` vocabulary — a `type virtual resource` today.)*
`class D extends B { override int32 f() {…} }` with `new D()` (no `D` ctor) segfaults on the first virtual
call. Root cause: `__vptr` is assigned **only inside constructor bodies** ([cstar.cemit.cpp:2077-2079](../cstar.cemit.cpp#L2077)),
and `new` skips the ctor when `hasCtor == false` ([cstar.cemit.cpp:609-615](../cstar.cemit.cpp#L609)), so the
vtable pointer is left uninitialized. Adding any constructor to `D` fixes it. Proper fix: synthesize a
default constructor for any polymorphic type with a vtable (sets `__vptr`, chains the base ctor). Not a keyword issue;
tracked as a focused codegen follow-up.

## Dispositions

| Item | Decision | Status |
|---|---|---|
| `const` | **M24** const-correctness — deeply-immutable bindings, `const fn`, `const T`/`const ref T` params, write-once fields, `const Ptr<T>`→`const T*` FFI. | ✅ done (M24a–e) |
| `volatile` | **reserved** — genuinely needed for the **embedded/Arduino** scope the user works in (ISR↔main-loop shared flags and memory-mapped peripheral registers, where `volatile` is the *correct* tool on a single-core MCU — not the multicore-atomics misconception). Using it today is a clear error; implement properly (emit C `volatile`) when cstar targets embedded — alongside the bigger embedded needs (globals/statics, ISR attributes, no-heap mode, MCU toolchains). | ✅ done (reserved + errors on use) |
| `export` | **kept reserved** — it has a real future role at the cstar→host boundary (WASM module exports for the browser engine; the scripting host interface), which is distinct from in-language `public`/`private`. Using it today is a **clear hard error**, never a silent no-op. | ✅ done (errors on use) |
| `public` `private` `protected` `abstract` `final` | the **Access-Control milestone (M25)** — enforced. Private-by-default; the type *kind* drives data exposure + extensibility; overridable methods are written `protected`; sealing via a `value`/plain `resource`/`type final resource` + `final` methods. (M26h reframed the kinds to `type value`/`type resource`/`type contract`; the old `pod` = a `value` with `public` fields.) | ✅ done (M25a + M25b, M26h) |
| `friend` | **granular friend grants** — owner names the exact accessor (a type / free function / `Type::method`) + the exact private members (`[a, b]` or `[...]`). | ✅ done (M25c) |
| `static` | full static (type-level `C::foo()`, no implicit `self`) deferred to **M29** (with operator overloading + full static methods). Callable via an instance today. | deferred (M29) |
| **Bug 1** (vtable init) | **fixed** — `buildVtables` now synthesizes a default constructor for any polymorphic type lacking one (sets `__vptr`, chains the base ctor); a synth ctor that can't supply required base args is a clear error. Regression: `tests/vtable_default_ctor`. | ✅ done |

No keyword is left as a *silent* no-op: `volatile` and `export` are clear errors pending their embedded /
WASM-host implementations, the access-control words are enforced (M25), `const` is shipped (M24), the
`give`/`copy` hand-off markers are shipped (M26c/d), and the vtable crash is fixed. Still deferred (a hard
error on use): `operator` overloading and full `static` methods (both M29).
