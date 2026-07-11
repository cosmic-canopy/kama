# kama type model — `value` / `resource` / `contract`

Every type declaration is introduced by a `type` marker (`type value` / `type resource` / `type contract`);
the vocabulary + access-control rules below are enforced by the compiler. This doc is the durable rationale
— see also [GOALS.md §3c](../GOALS.md).

## The `type` marker

Every type declaration begins with the reserved keyword **`type`**, followed by a *kind* — exactly
parallel to `fn` on every function. This makes declarations greppable and self-describing (`grep -n
'^type '`). The kind words `value` / `resource` / `contract` (and the qualifiers `virtual` /
`abstract` / `final`) appear *only* right after `type`, so they are **contextual, not reserved** — they
stay ordinary identifiers everywhere else (`int32 value = 5;`, a field or method named `resource`, etc.).
Only `type` itself is a keyword.

```kama
type value Name    { … }   // owns nothing — copies
type resource Name { … }   // owns / has identity — moves, RAII-dropped
type contract Name { … }   // a public-only guarantee (an interface)
```

## Why reframe

`class` / `struct` / `pod` (and value-vs-reference) are C/C++ legacy framings that encode the *wrong*
axis. The axis a no-GC / RAII language actually turns on is: **does this type own a resource?** Rust
(`Copy` vs move), Hylo/Val (value semantics), Swift (`~Copyable`), and Mojo are all converging here.
kama makes ownership the **declared nature** of a type, so the designer picks the right lever at
*design* time — a "type designer" language that retrains humans and LLMs to think ownership-first.

## The three kinds

| kind | owns? | hand-off default | polymorphism |
|---|---|---|---|
| **`value`** | nothing (raw data; may still encapsulate) | **copy** | contracts only (external / erased) |
| **`resource`** | something, or identity | **move** | contracts *and* internal vtable |
| **`contract`** | — (a public-only guarantee, no state) | — | *is* the polymorphism / substitutability lever |

These are the *nature* nouns. `virtual` / `abstract` / `final` are **qualifiers** (below), not kinds.

### `value` — owns nothing, copied

A `value` is defined by its bits: copying it is a `memcpy`, and it owns nothing to free. It is the
stricter cousin of a "value type" — where a C# `struct` can smuggle a heap reference (copying it
shares that object), a kama `value` owns **nothing**, so its copy has no hidden shared ownership.

```kama
type value Vec2 {
    public float x;         // fields choose visibility per field
    public float y;
    public fn float length() { return sqrt(this.x*this.x + this.y*this.y); }
}

type value Rect {
    float x; float y; float w; float h;    // private (default) — guards its own invariant
    public fn bool contains(Vec2 p) { ... }
}                                          // still copies freely — it owns nothing
```

- A "plain-old-data" type is just a `value` whose fields are all `public`. Encapsulation (public vs
  private fields) is a per-field choice, not a separate kind; `memcpy` semantics hold either way.
- **Checked intent:** a `value` that (transitively) owns a resource is a **compile error** ("declare
  `resource`"). Like `override` — derivable, but a checked assertion that catches a design/field
  disagreement, and it closes a latent hole (a `value` holding an `Owned` → double-free).
- A `value` is **sealed** and has **no destructor** — declaring `~dtor` on a value is an error whose
  message *is* the lesson: "a value owns nothing — a `~dtor` makes it a `resource`."

### `resource` — owns something (or has identity), moved

A `resource` is moved by default and RAII-dropped. It becomes destructible by declaring a `~dtor`
**or** by owning a resource member (transitively) — you rarely hand-write a dtor; you compose owning
members (`Owned`/`Shared`/`Weak`/collections).

```kama
type resource Buffer {
    List<byte> data;                       // owned → Buffer is a resource; fields stay private
    public fn int32 size() { return this.data.length(); }
}

type resource Token { }   // owns nothing, but move-only by *identity* — a capability / linear token
```

- `resource` fields are **private-only** — ownership (owned handles, invariants) stays encapsulated;
  expose behavior through methods.
- An **empty `resource`** (`Token`) is valid: "move" is decoupled from "has-a-dtor." It's the linear
  / capability / witness pattern. (A genuinely-unused one is caught by the general dead-code lint, not
  a special rule.)
- A non-owning member does **not** make you a resource: a raw `Ptr<T>` (unsafe borrow) or a borrowed
  `contract` value confers no ownership → still a `value`.

### `contract` — a public-only guarantee

"Interface" is overloaded (the *public surface of any type* vs *the abstract type*). A **`contract`**
is the abstract thing: a public-only guarantee a type promises to satisfy. A type's public members
are just "its API."

```kama
type contract Drawable { fn void draw(); }
type contract Animated : Drawable { fn void step(float dt); }   // refinement: requires Drawable + more
```

- All methods are **public** (a contract *is* public) — no visibility modifiers, no fields, no bodies
  (no default methods, v1), no ctor/dtor.
- **Explicit** satisfaction only (a type declares it satisfies a contract) — never structural/implicit.
- **Granularity:** keep contracts small; an API requires the **narrowest** one it needs. Contracts
  **refine** each other (capability layering) *without* class inheritance.

## Polymorphism: substitutability, not reuse

Using polymorphism/inheritance **for DRY is the anti-pattern.** The goal of subtyping is
**substitutability** ("is-a", Liskov — swap an implementation behind a guarantee); DRY is a *side
effect*. Inheritance is overused because it **bundles** two goals. kama unbundles them:

- **reuse / DRY** → **generics** (monomorphized, write-once-stamped-per-type, zero cost for values)
  and composition; shared *implementation up an owned hierarchy* → `virtual`/`abstract resource`.
- **substitutability / swap** → **contracts**.
- (**ownership** → `value` / `resource`, orthogonal to both.)

A contract alone gives no reuse (it's a pure guarantee); reuse comes from a **generic** — optionally
*bounded* by a contract (`fn sort<T: Comparable>(...)`). Together, generics + contracts give `value`
types everything inheritance did — reuse *and* is-a — without inheritance's coupling.

### Two dispatch mechanisms (differ by where the vtable lives)

- **Inheritance** (`virtual`/`abstract`): the vtable pointer is **embedded in the object**;
  polymorphic instances are used only through owned handles (`Owned`/`Shared<Base>`) and need a
  **virtual destructor**. Resource-world by construction — a `value` can't be `virtual` (an embedded
  vtable breaks free copy / invites slicing).
- **Contract** (external / erased): the value's layout is **unchanged**. Dispatch is either
  - a generic **bound** (`T: Drawable`) → **monomorphized**: direct, inlinable calls, **no vtable, no
    dtor, zero runtime cost** (type set fixed at compile time; cost = code size + compile time); or
  - a runtime **contract value** → a **fat pointer** (data + witness vtable, 2 words) + one indirect
    call, no inlining — buys runtime swappability.

Both `value` and `resource` satisfy contracts. Storing a contract over a **resource** you keep needs
an owning handle (`Shared<Drawable>`); over a **value** it's a **second-class borrow** (can't
escape/store). **Ownership introduces the destructor — polymorphism does not** (except the virtual
dtor for owned hierarchies).

## The lever cheat-sheet

- Owns something / needs identity? → **`resource`**. Else → **`value`**.
- Reuse an algorithm across types? → a **generic** (zero-cost; bound it with a contract if it needs
  behavior). Don't inherit for reuse.
- Need to swap implementations? → a **contract** (works on `value` too, and cheaper — *don't* reach
  for `resource` to get polymorphism; that's the contract's job).
  - interchangeable at **compile time** → contract as a **bound** (monomorphized, zero runtime cost).
  - interchangeable at **runtime** (plugins / heterogeneous / DI) → a **contract value** (indirect).
- Default: **no contract** (concrete-first; add abstraction only when a 2nd impl / real decoupling
  appears — YAGNI / GOALS §4).

## Access control + extensibility

Default visibility everywhere: **private**. Full per-member matrix (default in **bold**;
protected† = only inside a `virtual`/`abstract resource`):

| member | `value` | `resource` | `contract` |
|---|---|---|---|
| field | **private**, public | **private** only | — (no fields) |
| method (non-virtual) | **private**, public | **private**, public, protected† | public-only, no body |
| operator | **private**, public | **private**, public | public-only (if required) |
| static method | **private**, public | **private**, public, protected† | — |
| constructor | private, **public** | private, **public**, protected | — |
| destructor | ⛔ (→ resource) | ✅ 0..1 (RAII-called) | ⛔ |
| virtual / abstract | ⛔ | ✅ **protected-only** | ⛔ (it *is* the abstraction) |
| final | ⛔ (already sealed) | ✅ (seal an override / a subclass branch) | ⛔ |

Seven rules make the grid memorable:

1. Default = **private** everywhere.
2. **`protected` ⟺ an extensible `resource`** (`virtual`/`abstract`). It's meaningless without a
   subclass, so it's an error on a `value`, a sealed `resource`, or a `contract`.
3. **overridable ⟹ protected.** `virtual`/`abstract` methods are **protected-only** — private can't
   be meaningfully overridden, and public-overridable is bad design. The public polymorphic face is a
   **contract** (or a public non-virtual method). This bakes in **NVI** (Non-Virtual Interface).
4. **public fields ⟺ `value`**; a `resource` keeps fields **private** (ownership encapsulated).
5. **`~dtor` ⟺ `resource`** (forbidden on a `value` — it owns nothing).
6. **`virtual`/`abstract`/`final` ⟺ `resource`** (values are sealed → use contracts; a contract
   already *is* the abstraction).
7. **`contract`** = all-public methods, no fields, no bodies, no ctor/dtor; may refine other
   contracts. `friend` grants apply as elsewhere.

### Extensibility qualifiers

- plain `resource` = **sealed** (the default).
- **`virtual resource`** = an extensible base (vtable + shared implementation); subclasses `override`.
- **`abstract resource`** = extensible + non-instantiable; an `abstract` method (no body) forces the
  enclosing type to be `abstract`.
- **`final`** = seal a `virtual` method (no further `override`) or a subclass branch. Redundant on a
  plain resource (already sealed).

### The NVI consequence

Because public-virtual is banned, a `contract` method that must vary per subclass is satisfied by a
**public non-virtual** method that delegates to a **protected virtual/abstract** customization point:

```kama
type abstract resource Polygon : Shape {
    public fn float area() { return this.computeArea(); }   // public, non-virtual: the stable face
    protected abstract fn float computeArea();              // the protected customization point
}
```

Callers use the public/contract face; subclasses override the protected virtual. Rare in practice —
most polymorphism is contracts + monomorphized generics; virtual inheritance is only for shared-impl.

## Hand-off marker rule — every kind is movable; the default is declared, a marker overrides

There is no `!Movable` and no "ambiguous → must annotate": **every owning kind is movable**, each kind
has a natural *bare* hand-off, and a `give`/`copy` marker overrides it. A hand-off is a *named* value
handed off in an initializer, assignment, argument, or return; a fresh `new`/ctor/call result never
takes a marker.

- **`value`** → **copy** (a value's "move" *is* a copy; the source stays valid).
- **`resource` without a copy contract** (move-only) → **move** on a bare hand-off (the source is
  consumed); `give` is optional emphasis; `copy` is an error — nothing to copy with — until it opts in.
- **`resource` with a copy contract** → it **must declare its bare default** at opt-in:
  `implements Copyable(bare: give)` (bare **moves**) or `Copyable(bare: copy)` (bare **deep-copies** via
  its public nullary `copy()`). A bare `implements Copyable` *without* `(bare: …)` is a compile error.
  `give x` moves, `copy x` deep-copies — a marker always overrides the declared default.
- **`Shared`/`Weak`** (shared ownership, `implements Copyable(bare: copy)`) → a bare hand-off **retains**
  (refcount++); `copy` is the explicit retain; **`give` moves the handle** — the ref transfers and the
  source is consumed (how a `Shared` returns from a factory without a spurious retain/drop).

A bare hand-off is **never a silent copy of a resource** (the double-drop hole is closed in every case)
and **never a silent move of a `Shared`** you meant to share (a `Shared`'s bare default is retain). This
is compile-time move tracking with **zero runtime overhead by construction** — a value moved on
some-but-not-all paths that is still live at scope exit is *rejected*, not tracked with a runtime
drop-flag (`Optional<T>` is the explicit escape hatch for genuinely-conditional ownership). The full
give/copy behavior matrix (every cell backed by a fixture) is in [SPEC.md](SPEC.md).
