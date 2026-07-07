# cstar keyword reference

Every type declaration begins with the **`type` marker** followed by a *kind*: **`type value`** (owns
nothing, copies), **`type resource`** (owns/identity, move-only, RAII-dropped), or **`type contract`** (a
public-only guarantee — an interface). The kind words `value`/`resource`/`contract` are **contextual, not
reserved** — they mean a kind only right after `type`, and stay ordinary identifiers everywhere else
(`int32 value = 5;`). See the [type-model doc](TYPE_MODEL.md) and the rows below.

Legend for the status column:

- ✅ **implemented** — parses, lowers, behaves correctly
- 🚧 **reserved** — a keyword, but using it today is a hard error (its implementation is future work)

## Status table

| Keyword(s) | Status | Notes |
|---|---|---|
| `bool` `int8/16/32/64` `uint8/16/32/64` `float32` `float64` `void` `string` | ✅ | core types. `string` is a fat value: a borrowed literal/view (no alloc) or a heap-owned RAII string (freed on drop) |
| `int` | ✅ | alias → `int32` |
| `double` | ✅ | alias → `float64` (C `double`) |
| `type` | ✅ | **the type-declaration marker** — every type is `type <kind> Name { … }` (parallel to `fn`). The kind is `value`/`resource`/`contract` (+ the `virtual`/`abstract`/`final` qualifiers after `type`). Greppable (`grep '^type '`) |
| `value` `resource` `contract` | ✅ | **contextual, not reserved.** They name a kind **only right after `type`**; everywhere else they are ordinary identifiers (`int32 value = 5;`, a field/method named `resource`). `type value` = owns nothing, copies, sealed, fields private-or-`public` per field, no dtor. `type resource` = owns/identity, move-only, RAII-dropped, fields private-only. `type contract` = public-only guarantee (an interface): methods only, no bodies/fields/ctor/dtor; satisfied via `implements`; may refine another (`type contract A : B`) |
| `enum` `namespace` | ✅ | an `enum` is a plain set or a **tagged union** (variants carry payloads; may be generic), consumed by `match` |
| `match` `case` | ✅ | **`match`** is the one construct for branching on an enum — plain enums, tagged unions, and `Optional`/`Result` alike. Value-producing (statement or expression position), **compile-time exhaustive**, with a `_` wildcard; `case` heads each arm and binds payloads (`case Some(v): …`). The subject may be a variable, method call, or free-function call. Arbitrary-integer branching uses `if`/`else if` — there is no `switch` |
| `operator` | ✅ | **operator overloading.** Full set: arithmetic `+ - * / %`, comparison `== != < > <= >=`, bitwise `& \| ^ << >>`, unary `- ! ~`, `++`/`--`. **Arity picks the form:** 0 params = unary on `this` (`Vec2 operator-()`), 1 = binary method (`Vec2 operator+(Vec2 rhs)`, `this` is the left operand), 2 = binary free/static form (`Vec2 operator*(int32 s, Vec2 v)` — enables scalar-on-the-left). Positional lowering to `Type__op_add(&lhs, rhs)`. **Type-based dispatch:** a type may carry several `operator*` distinguished by operand type — `mat*vec` + `mat*mat`, `v*s` + `s*v` — like C++/C#/Rust; only the same symbol AND operand type collide. **Chaining** (`a + b + c`, in any position incl. a condition — nested rvalues wrap in a C99 compound-literal array); **compound assignment** `pos += vel` ≡ `pos = pos + vel`. `==` is **explicit** (no auto structural equality). `[]`, `true`/`false` conversion operators out of scope. In a `contract`, an operator is a **bound** for generic math |
| `extends` `implements` `this` `base` `ref` `out` | ✅ | (`base(...)`/`base.m()` need **named** args) |
| `This` | ✅ | **the self-type** (contextual, not a lexer keyword): inside a `type contract`/type it names the implementing/concrete type — `fn bool equals(This other)`, `fn This clone()`. Resolves via monomorphization; used as a bound (`<T: Equatable>`) the dispatch is static. Distinct from the lowercase `this` **value**. Outside a type/contract it's a clean error |
| generics `<T>` / bounds `<K: A + B>` / turbofish `f::<T>()` | ✅ | **monomorphized** (zero-cost, no boxing). Generic **functions** (type args inferred; a return-only generic uses **turbofish** `f::<int32>()` — the `::` before `<` is unambiguous) and **types** (`type value Pair<A,B>`, generic `resource`), multi-param + nested (`Box<Pair<int,int>>` — the `>>` split means no space). **Contract bounds** `<K: Hashable + Comparable>` (`+` = AND) → the body may call the contracts' methods on a type-param, lowered to **static direct calls**; each concrete arg is checked to satisfy its bounds |
| `new` | ✅ | **the heap operator.** `Owned<Box> p = new Box(...)` boxes the **element type** on the heap; a stack value **drops `new`** (`Box b = Box(...)`). `new` into a plain value type is an error. (Collections keep `new Array<T>(...)`.) |
| `virtual` `override` | ✅ | an overridable method is written `protected` (never public/private), and its type must opt in as a `type virtual resource`/`type abstract resource` (extension is a `resource` concern — a `value` is sealed); `override` only re-seats a real base slot |
| `give` `copy` | ✅ | **hand-off markers.** `give` = move (invalidates the source); `copy` = retain (`Shared`/`Weak`) or duplicate. Ride a **named** value (a fresh `new`/ctor/call result needs none) and work uniformly in **initializer, assignment, argument, and return** positions. Defaults by kind: `Owned` → give (copy is an error), `Shared`/`Weak` → **copy-only** (bare hand-off retains; **`give` is an error** — shared ownership can't be moved, `implements Copyable, !Movable`), `value`/primitive → copy (give is an error), a plain `resource` (move-only) → move (bare hand-off moves; `copy` is an error until it opts into `Copyable`). **A collection requires the marker: `give` = move (buffer), `copy` = deep copy** — element-wise: a bitwise-copyable element copies memberwise, a `Copyable`-resource element deep-copies via its own `copy()`; a non-`Copyable` resource element is rejected. **`Copyable` resource:** a `resource` opts into copy **nominally** — `implements Copyable` plus a **public nullary `copy()`** method (a lone `copy()` without the `implements` does *not* opt in); then the marker is **mandatory** ("scream when ambiguous") — a bare hand-off is an error, `copy x` deep-copies via `copy()`, `give x` moves. Since `copy`/`give` are only markers in expression position, they're **contextual** — usable as method names (so the opt-in method is literally `copy`). Full behavior matrix (every cell → fixture) in [SPEC.md](SPEC.md) |
| `if` `else` `do` `while` `for` `foreach` `in` `break` `continue` `return` | ✅ | full control flow (enum branching is `match`, not `switch`) |
| `true` `false` `null` `cast` `unsafe` `extern` | ✅ | `null` is only for `Ptr<T>` at the FFI boundary; `unsafe { }` is the sole raw-pointer memory seam |
| `import` `as` `export` | ✅ | **the module system.** `import a::b;` (load; qualified-only `a::b::X`) / `import a::b as m;` (whole-module alias → `m::X`) / `import a::b::{X, Y as Z};` (per-symbol, unqualified; `as` renames). A **module** is a namespaced source file *or* a directory of same-namespace files, resolved by its `::`-path from the importing file's dir, `$CSTAR_PATH`, then the bundled stdlib (`std`/`core` are reserved roots). **`export { A, B, C };`** is a **top-of-file manifest** naming the module's public surface — **module-private by default**; a listed name must be a top-level decl in the same file, and a non-exported symbol can't be imported. Declarations themselves carry **no** visibility modifier (so `type`/`fn` syntax stays uniform); the manifest mirrors `import`. Two imports binding the same bare name is an error (use `as`). Replaces `using` (retired) |
| `static` | ✅ | **full static methods.** `static fn` has **no implicit `self`** and is called at type level: `Vec2::dot(left:, right:)`. `static` + `virtual`/`override`/`abstract` is a contradiction (no vtable slot); a `this`/bare-field reference in a static body is a clean error |
| `public` `private` `protected` | ✅ | Members are **private by default**; `public`/`protected` set it explicitly. `protected` = owner-or-subclass and is meaningful **only inside an extensible `resource`** (`type virtual`/`abstract resource`) — an error on a `value`, a plain `resource`, or a `contract`. **Field visibility is per field on a `type value`** (`public` field = a plain-old-data struct); a `type resource` keeps fields private; a `type contract` has no fields |
| `friend` | ✅ | granular, owner-granted grants: `friend <accessor>[members];` (or `[...]` = all privates). The accessor is a **type**, a **free function**, or a **`Type::method`**; it may then reach the named private members. Granting an unknown or public member is an error |
| `abstract` | ✅ | `type abstract resource` is non-instantiable (`new` rejected; also when a subclass leaves an inherited pure method un-overridden); methods are `protected abstract`; the type must declare ≥1 overridable member. A qualifier after `type`, on a `resource` only |
| `final` | ✅ | `type final resource` is a sealed leaf (cannot be `extends`-ed); `final` on a method seals its slot (no subclass `override`); only a `type virtual`/`abstract resource` may be extended at all (a `value` and a plain `resource` are already sealed) |
| `const` | ✅ | a `const` binding is deeply immutable (no reassign, no write *through* it, no `++`/`--`); `const fn` is non-mutating and the only kind callable on a const receiver; params take `const T` / `const ref T` (read-only borrow); a `const` field is write-once (constructor only); `const Ptr<T>` lowers to `const T*` for const-correct FFI |
| `fn` `fnptr` | ✅ | `fn` heads every function/method declaration. `fnptr` declares an explicit, named function-pointer **type** — zero-cost, non-null, signature-checked; a bare function name or `Type::method` binds it |
| `volatile` | 🚧 | **reserved** for the embedded/MMIO scope (ISR↔loop shared flags, peripheral registers) — using it is a clear error, not a silent no-op; implemented when cstar targets embedded |
| `expose` | 🚧 | **reserved** for the cstar→host boundary (WASM module exports, scripting host interface) — clear error on use, not a silent no-op. Formerly spelled `export`; that word is now the module-visibility keyword |

## Reserved-but-unimplemented keywords

`volatile` and `expose` are the **only** two keywords not yet implemented. Both are genuine keywords today
and using either is a **hard compile error** — never a silent no-op:

- **`volatile`** has a real future role in the **embedded/MCU** scope (ISR↔main-loop shared flags and
  memory-mapped peripheral registers, where `volatile` is the correct tool on a single-core MCU — not the
  multicore-atomics misconception). It will emit C `volatile` when cstar targets embedded, alongside the
  bigger embedded needs (globals/statics, ISR attributes, no-heap mode, MCU toolchains).
- **`expose`** has a real future role at the cstar→host boundary (WASM module exports for the browser engine;
  the scripting host interface), distinct from both in-language `public`/`private` (member access) and
  `export` (module visibility) — three separate boundaries, three words.

Every other keyword is implemented, enforced, and exercised by the fixtures in [`../tests/`](../tests/).
