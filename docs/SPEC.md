# cstar language specification (overview)

This is a semantics overview. The **grammar is authoritative** — see
[grammar.bnf](grammar.bnf) (generated from `cstar.y`). Executable examples live in
[`../tests/`](../tests/) (`*.cstar` with a `.expect` exit code). Status flags below: ✅ implemented,
🚧 planned.

## Model

cstar compiles to **portable C**. No garbage collector — object lifetimes are deterministic (RAII).
Calls use **named arguments** (no positional). The philosophy: one way to do a thing, explicit over
implicit (see [../GOALS.md](../GOALS.md)).

## Types ✅

| cstar | C |
|---|---|
| `int8 int16 int32/int int64` | `int8_t … int64_t` |
| `uint8 uint16 uint32 uint64` | `uint8_t … uint64_t` |
| `float32` / `float64`/`double` | `float` / `double` |
| `bool` | `bool` |
| `string` | `cstar_string` (runtime; literals borrow) |
| `void` | `void` |
| user `type value`/`type resource` | `struct` (value semantics) |

No raw arrays and **no raw pointers — by design** (no `unsafe`). Collections are generic library types.

## Collections & strings ✅ (M9)

Built-in generics, monomorphized per element type and backed by the C runtime (unsafe internals, safe API
— the Rust-`Vec` model); **indexing is bounds-checked** (a clean trap, not UB).

```cstar
Array<int32> a = new Array<int32>(size: 4);   // fixed buffer, zero-initialized
a[0] = 10;  a[1] = 20;                          // bounds-checked []
int32 first = a[0];
foreach (int32 x in a) { /* ... */ }            // iterate

List<Point> ps = new List<Point>();             // growable
ps.add(item: p);   int n = ps.length();   Point q = ps[0];

string s = "ab";                                // borrowed literal (no alloc)
string t = s.concat(other: "cd");               // heap-owned, RAII-freed
bool eq = s.equals(other: t);   int len = s.length();
```

All collections own their storage and free it via RAII (with element-destructor chaining). Only the
`(collection, element-type)` pairs the program actually uses are emitted (pay-for-what-you-use).
A **method call on an element** works directly — `list[i].method()` borrows the element *in place*
(M26i), so a mutating method mutates the stored element; a `const` collection allows only const
methods on its elements. M9 limits: `add`/index take elements **by value** (no move yet) — don't
separately destruct an added source; don't return or inline-use a temp-owned string without binding
it to a local.

## Smart pointers ✅ (M10) / 🚧

`Owned<T>` — unique heap ownership (= Rust `Box` / C++ `unique_ptr`), zero overhead, **move-only**,
**auto-deref**, RAII-freed. The cstar surface stays pointer-free; the raw pointer is confined to the
runtime. Use it for heap objects, recursive data structures, and (later) polymorphic ownership.

```cstar
Owned<Counter> c = new Counter(start: 40);  // M26a: `new` heap-boxes the ELEMENT type
c.bump();  int n = c.get();                          // auto-deref: . reaches the pointee
Owned<Counter> d = c;                                // MOVE: c is now empty (moved-from)
fn Owned<Node> make(int v) { Owned<Node> n = new Node(id: v); return n; }  // factory: moves out
```

Move-only: copying/initializing/returning an `Owned` transfers ownership and invalidates the source, so
the pointee is freed exactly once (RAII, with the pointee's destructor).

**Ownership hand-off — `give` / `copy` ✅ (M26c/d).** When a *named* owned value is handed off — in an
initializer, assignment, argument, or return — an explicit marker states the intent, uniformly in all four
positions: **`give`** moves (invalidates the source), **`copy`** retains (`Shared`/`Weak`) or duplicates.
The natural op is the default, so a marker is only required where a silent copy would be surprising: `Owned`
defaults to `give` (and `copy Owned` is an error — it's unique); `Shared`/`Weak` default to `copy`/retain,
with **`give` as the opt-in move** of the handle; a `value`/primitive just copies (and `give` on one is an
error). A fresh `new`/constructor/call result needs no marker. Smart pointers also **pass by value** (M26d):
the callee *owns* the argument and drops it at function end — `fn int use(Owned<T> p)` consumes it (`use(p:
give x)`), `fn int peek(Shared<T> s)` retains it (`peek(s: x)`, `x` stays valid).
```cstar
Owned<Counter> b = give a;   // explicit move (a consumed)
Shared<Counter> t = s;       // copy/retain (default) — both valid
Shared<Counter> u = give s;  // opt-in move of the share (no retain)
```
*(`copy` of a collection is a deep copy — a fresh buffer (M26f-3), element-wise: a bitwise-copyable element is copied memberwise, a `Copyable`-resource element is deep-copied via its own `copy()` (M26f-5). A resource element that is not `Copyable` is rejected. `give` of a collection **moves** the buffer.)*

**Move-only `resource` values + the `Copyable` contract ✅ (M26f-2 / M26f-4).** A **`type resource`**
value (it owns something, or has identity) is **move-only**: a bare named hand-off *moves* (the source is
consumed, its destructor suppressed), so its heap is freed exactly once — a silent copy is never emitted
(that would double-free). `give` is optional emphasis; `copy` is an error unless the type opts in. A
`resource` **opts into copy** by declaring a **public nullary `copy` returning its own type** (the interim
spelling of the internal **`Copyable`** contract; the explicit `: Copyable` form needs `This`, so it
arrives with M27). Once copyable, the marker is **mandatory** — both move and copy are plausible, so a
*bare* hand-off is a compile error and you must write **`give x`** (move) or **`copy x`** (deep-copy via
`copy()`; the source stays valid). Because `copy`/`give` are markers only in expression position, they're
**contextual keywords** — usable as method names, so the opt-in method is literally named `copy`.
```cstar
type resource Res {
    List<int32> items;
    ~Res() { }
    public fn Res copy() { Res r = Res(v: this.items[0]); return give r; }   // opt into Copyable
}
Res b = copy a;   // deep copy — a stays valid, b has its own buffer
Res c = give b;   // move — b consumed
Res d = a;        // ERROR: a is copyable — say `give` or `copy`
```

**The give/copy behavior matrix (M26f).** A marker is required exactly when *both* move and copy are
plausible ("silent default, scream when ambiguous"); otherwise the one natural op is silent. The rule is
uniform across all four hand-off positions — **initializer, assignment, argument, return** — and a *fresh*
rvalue (`new`/constructor/call result) never takes a marker. Every cell is covered by a fixture (`tests/`,
`✗` = an `xfail/` rejection):

| kind | bare hand-off | `give` | `copy` | fixtures |
|---|---|---|---|---|
| primitive / `value` | **copy** (cheap) | ⛔ "applies to an owned value" | copy (redundant, allowed) | give_value ✗ |
| `Owned<T>` (unique) | **move** | move (emphasis) | ⛔ "is unique" | give_copy, give_param, give_return, copy_owned ✗ |
| `Shared<T>` (ref-counted) | **retain** (strong++) | opt-in move | retain | give_copy, give_param, give_return |
| `Weak<T>` | **retain** (weak++) | opt-in move | retain | weak_dtor |
| collection (`Array`/`List`/`String`) | ⛔ marker required | **move** (buffer) | **deep copy** (fresh buffer) | coll_give, coll_copy, string_copy, collection_bare ✗ |
| plain `resource` (move-only value) | **move** | move (emphasis) | ⛔ "opt into `Copyable`" | move_value, move_return, copy_value ✗ |
| `Copyable` resource (has `copy()`) | ⛔ ambiguous | move | **deep copy** via `copy()` | copy_resource, copy_bare_ambiguous ✗ |
| collection of `Copyable` elements | ⛔ marker required | move | **deep copy** (element-wise `copy()`) | coll_copy_resource, copy_resource_coll ✗ |

A marker on a fresh rvalue is an error (handoff_fresh ✗). Move tracking is compile-time: reading a moved
value (use_after_move ✗), moving out of a field/element (move_field ✗), moving inside a loop a value
declared outside it (move_in_loop ✗), and a conditional move that is still live at scope exit
(cond_move_live ✗, switch_partial_move ✗) are all rejected — there is no runtime drop flag.

`Shared<T>` — ref-counted shared ownership (= C++ `shared_ptr` / Rust `Rc`). **Copyable**: each copy
retains (refcount++), each drop releases, and the pointee is destroyed when the **last** handle goes away.

```cstar
Shared<Tex> a = new Tex(id: 7);
Shared<Tex> b = a;     // retain — a and b share one Tex (both valid)
b.use();  int n = a.id;
// a, b drop in RAII order; the Tex is freed exactly once, with the last handle
```

`Weak<T>` — a non-owning weak reference to a `Shared<T>`'s pointee. It does **not** keep the pointee
alive, so it **breaks reference cycles** that `Shared` alone would leak. You can't dereference a `Weak`
(it may be dead) — **upgrade** it with a checked `upgrade()`:

```cstar
Weak<Tex>  w = s;             // make a weak ref from a Shared (does not keep Tex alive)
Shared<Tex> up = w.upgrade();  // -> a valid Shared if the Tex is alive, else empty
if (up.valid()) { up.use(); }       // .valid() = the upgraded Shared is non-empty
bool dead = w.expired();    // true once the last Shared is gone
Weak<Tex>  e;               // default-empty (expired)
```

**No null (safe surface) — see GOALS §3b.** A value, `Owned`/`Shared`, `ref`/`out` borrow, or contract value
is always valid: there is nothing to null-check. `== null` / `!= null` on a safe type is a **compile error**
(the C habit checks the wrong thing here); `null` is only for `Ptr<T>` at the FFI boundary. A `Weak<T>`'s
liveness is obtained through `tryUpgrade(out: s) -> bool` (🚧 planned; today: `upgrade()` + `.valid()`),
whose result forces you to handle the dead case.

Passing a smart pointer: **borrow** it by passing `ref T` — the borrow names the *object* (`ref T`, storage-
agnostic; a `ref` may not name the smart pointer itself), which auto-derefs to the held object; or **transfer
by value** (M26d), where the callee owns the argument and drops it at function end (`Owned` moves in, `Shared`
retains). The pointee is a **`value`/`resource`** or (M26g) a **contract** — `Owned`/`Shared`/`Weak<IShape>`
own a concrete implementer behind a fat handle and dispatch polymorphically (see Contracts below). A smart
pointer works as a *field*, *return*, and (M27) a **collection element** — `List<Shared<IShape>>` stores and
drops each handle in RAII order and dispatches polymorphically through it. See **Generics** below.

## Functions ✅

```cstar
fn int add(int a, int b) { return a + b; }
fn int main() { return add(b: 20, a: 10); }   // named args; reordered to declared order
```
`ref`/`out` parameters pass by pointer: `fn void set(out int dst) { dst = 42; }` … `set(dst: ref x);`.

## FFI — calling C ✅ (M15)

`extern fn Ret name(params);` declares a C function's call signature (name + named params for lowering);
the C **prototype comes from the header** you `extern "<header.h>";` — cstar never emits a prototype for an
extern function (so there's no redeclaration conflict, and a missing include is a plain C error). Link
libraries with `--link`. The FFI boundary is the language's only "unsafe" seam (explicitly `extern`):

```cstar
extern "<stdlib.h>";             // every C function comes from an explicit header
extern "<math.h>";
extern fn Ptr  malloc(usize n);     // Ptr = void* (opaque pointer/handle); usize = size_t
extern fn void free(Ptr p);
extern fn float64 sqrt(float64 x);  // build with: --link m

fn int main() {
    Ptr p = malloc(n: 64);
    if (p == null) { return 1; }  // hold / null-check / compare — but no deref yet
    free(p: p);
    return cast<int>(sqrt(x: 1764.0));   // 42
}
```

**The FFI rule (one sentence): declare C types/functions by `extern`-including their header.** An `extern`
declaration is purely cstar's call-signature (name + named params, so it can lower the call) — the actual
C prototype comes from the header you include with `extern "<header.h>";`. cstar never emits a C prototype
for an extern function, so there are no redeclaration conflicts; and the runtime hides its own libc
dependencies (block-scope declarations), so **no** C function (not even `malloc`) is available without its
header — a missing include is a plain C error, never a silent guess. Fully consistent.

`Ptr` is `void*`; `Ptr<T>` is `T*` — an **opaque carrier** (hold, pass to/from C, `null`-check, compare;
**no dereference** in cstar yet). `usize`/`isize` map to `size_t`/`ptrdiff_t`. Names beginning `cstar_`
are reserved (runtime-provided).

**FFI data (M16) — all controlled, no `unsafe` needed:**

```cstar
extern "<stdlib.h>";                       // a C #include
type extern value div_t { int32 quot; int32 rem; }   // bind an external C struct (not re-emitted)
extern fn div_t div(int32 numer, int32 denom);

extern fn float64 frexp(float64 value, Ptr<int32> exp);

fn int main() {
    div_t r = div(numer: 17, denom: 5);    // r.quot=3, r.rem=2  (field access on a C struct)
    int32 e = 0;
    frexp(value: 1764.0, exp: addr(of: e));// addr(of: x) = &x  — controlled out-param
    return r.quot + r.rem + e;             // 5 + 11 = 16
}
```

`type extern value Foo { ... }` (was `extern class`) is an **external** struct provided by an included
header / linked code — cstar uses its fields (all public, the C layout) but never re-emits it (so no
redefinition), and its name is the literal C name.
`addr(of: x)` takes the address of a real local (out-params, descriptor pointers) — a *controlled* op, no
`unsafe`. `s.cstr()` yields a C `const char*`.

### `unsafe { }` — raw pointer memory access (M17)

The **only** place cstar can touch arbitrary memory through a raw pointer. Raw `Ptr<T>` index/store is a
**compile error outside** an `unsafe { }` block — so the entire dangerous surface is explicit and
greppable (`grep -rn 'unsafe {'`). Everything else (collections, smart pointers, FFI structs/handles/
out-params, `addr`) stays safe.

```cstar
Ptr<int32> p = malloc(n: 16);    // void* -> int32_t* (implicit)
unsafe {
    p[0] = 10;  p[1] = 32;       // raw store  (p[0] is *p)
    int32 v = p[0] + p[1];       // raw read
}
// p[0] = 1;                     // ERROR outside unsafe: "raw pointer access requires an `unsafe { }` block"

Array<float32> verts = ...;
Ptr<float32> data = verts.dataPtr();   // SAFE to obtain (Rust as_ptr rule); usize n = verts.byteLen();
// ... pass (data, n) to a C upload fn; dereferencing `data` still needs `unsafe`
```

`a.dataPtr()`/`a.byteLen()` bridge a collection's buffer to C (safe to call; the returned `Ptr` is valid
only while the collection is alive + unmodified, and dereferencing it requires `unsafe`). An unlowered
construct (including a safety-gate violation) is a **hard build error** — cstar never emits incomplete C
and claims success.

### Function pointers — `fnptr` (M21) ✅

cstar has no naked function pointers. **`fnptr`** declares an explicit, named function-pointer **type**
(independent of any user type) — **zero-cost** (a bare C function pointer, no wrapper). It is **non-null** (must be bound;
no `null`, no null-check at the call), and binding a free function is **signature-checked**. (A bodiless
`fn` is *not* a function pointer — a forgotten body is a clear error, never a silent type.)

```cstar
fnptr int32 Comparator(int32 a, int32 b);       // an explicit function-pointer TYPE
fn int32 cmp(int32 a, int32 b) { return a - b; }

Comparator c = cmp;                             // bind by name (positional, type-checked) — used directly
int32 r = c(a: 9, b: 2);                        // named invoke through the pointer
```

A bare **function name used as a value** is its function pointer (Rust-like), so binding and passing need no
operator — `c = cmp` and `f(cb: cmp)` just work. (This **retires the old `funcptr(of:)`** builtin.) An
`fnptr` can also be a **parameter** (`fn run(Op op, …) { op(…) }` — the core callback shape).

**Unbound method references** — `Type::method` (zero-cost). A method lowers to `Class__method(Class* self,
…)`, so it's a function pointer whose **first parameter is the receiver**; the object is passed explicitly:

```cstar
type value Vec2 { public int32 x; public int32 y; fn int32 dot(ref Vec2 o) { return this.x*o.x + this.y*o.y; } }
fnptr int32 DotFn(ref Vec2 self, ref Vec2 o);   // receiver is an explicit first param

DotFn d = Vec2::dot;            // unbound (`::` = no instance, no binding) — zero-cost
int32 n = d(self: ref u, o: ref v);
```

**`BindableFunctionPtr<Sig>`** — a callable that *captures* a receiver so you don't pass it each call. Unlike
the zero-cost `fnptr`, it carries an object (opt-in cost) and is **RAII-managed**. It's constructed like any
other object, and **the ownership model follows the pointer type you hand in** — no separate keyword:

```cstar
fnptr int32 Compare(int32 a, int32 b);   // NB: receiver is HIDDEN here (the inverse of an unbound fnptr)
type value Scaler { int32 k; public Scaler(int32 k){ this.k = k; }
               public fn int32 apply(int32 a, int32 b){ return (a - b) * this.k; } }

Owned<Scaler>  s  = new Scaler(k: 3);     // (constructed as Owned)
BindableFunctionPtr<Compare> c  = new BindableFunctionPtr<Compare>(obj: s,  method: Scaler::apply);  // MOVE-in (sole owner)
Shared<Scaler> s2 = new Scaler(k: 2);
BindableFunctionPtr<Compare> c2 = new BindableFunctionPtr<Compare>(obj: s2, method: Scaler::apply);  // RETAIN (shared owner)
BindableFunctionPtr<Compare> c3 = sub;   // free-function PROMOTION (no object) — so this type "accepts either"

int32 r = c(a: 9, b: 2);                 // -> Scaler::apply(boundObj, 9, 2) = (9-2)*3 = 21
```

An `Owned` `obj:` **moves in** (the bindable becomes the sole owner, drops it via the element dtor); a
`Shared` `obj:` is **retained** (refcount; the object lives while any owner holds it). A bare `fnptr` /
free function **promotes** in with a null object — so a `BindableFunctionPtr<Sig>` parameter accepts both
free and bound callables, while `fnptr` stays the zero-cost free-only form. It is **move-only** (it may
uniquely own its object): returning one from a factory transfers ownership; the captured object's
destructor runs **exactly once** when the bindable finally drops.

**FFI**: an `extern fn` may take an `fnptr` type as a param; passing it hands C the raw pointer. C requires
an *exact* function-pointer-type match (incompatible fn-pointer types are a hard error), so when the C
callback signature is one cstar's `fnptr` doesn't spell identically — most commonly `const`-qualified
parameters — name the callback via a header `typedef` and **cast** to it at the edge:

```cstar
extern "<stdlib.h>";
extern "cb.h";   // typedef int (*CompareFn)(const void*, const void*);
fnptr int32 Comparator(Ptr<int32> a, Ptr<int32> b);
extern fn void qsort(Ptr buf, usize nmemb, usize size, CompareFn compar);
...
Comparator c = cmp;
qsort(buf: a.dataPtr(), nmemb: 4, size: 4, compar: cast<CompareFn>(c));   // cast to the header's fn-ptr type
```

🚧 next: full user-defined generics (`Map<K,V>`, `>>`); then math types → cstar-level WebGPU bindings.

## Control flow ✅

`if/else`, `while`, `do/while`, `for`, `switch` (C#-style, no fall-through), `break`, `continue`,
`return`; full operator set (`+ - * / %`, bitwise, shifts, comparisons, `&& || !`, ternary `?:`),
assignment ops (`= += …`), `++`/`--`, casts.

## Type declarations — `value` / `resource` / `contract` ✅ (M26h)

Every type declaration is introduced by the **`type` marker** followed by a *kind* — parallel to `fn`
on every function, so declarations are greppable and self-describing:

- **`type value Name { … }`** — owns nothing, **copies** freely (a `memcpy`; no hidden shared refs).
  Sealed (no `virtual`/`abstract`/`final`), no destructor. Fields default **private**; mark a field
  `public` per field (a `value` with all-public fields is what used to be a `pod`).
- **`type resource Name { … }`** — owns something, or has identity: **move-only**, RAII-dropped.
  Fields are **private only** (ownership stays encapsulated). An empty `type resource Token { }` is a
  valid move-only identity/token. Extensible variants add a qualifier after `type`: `type virtual
  resource`, `type abstract resource`, `type final resource`.
- **`type contract Name { … }`** — a public-only guarantee (replaces the old `interface`); methods
  only, no bodies, no fields, no ctor/dtor. Types satisfy it via `implements`; it may refine another
  (`type contract Animated : Drawable { … }`).

The full model + rationale is in [TYPE_MODEL.md](TYPE_MODEL.md). The kind words `value` / `resource` /
`contract` are **contextual, not reserved** — because they appear only right after `type`, they remain
ordinary identifiers everywhere else (`int32 value = 5;`). Only `type` is a keyword.

```cstar
type value Counter {
    int value;                                       // fields are private by default (M25)
    public Counter(int start) { value = start; }     // constructor (mark `public` to call from outside)
    public fn void add(int n) { value = value + n; } // method (implicit self)
    public fn int get() { return value; }
}
Counter c = Counter(start: 40);   // stack value — a stack value DROPS `new` (M26a)
c.add(n: 2);                       // a `value` copies on hand-off
```
Fields, methods (take an implicit `self`), one constructor, field initializers (run in the ctor),
`this.field`, `obj.method(args)`. Lowers to a `struct` + `Counter__method(Counter* self, …)` functions.
Members are **private by default** (M25 — see below); `new` is reserved for the heap (`Owned`/`Shared`
element construction), so a stack value uses `Counter(start: 40)`, not `new Counter(...)`. A stack
constructor may also be written **inline in a call argument** — `f(x: Counter(start: 5))` (M26i) — it
materializes a temporary passed by value (a `value` copies, a `resource` moves); use a local for a
`ref`/`out` parameter.

A type that owns a heap resource (a collection, an `Owned`/`Shared`/`Weak`, or another `resource`) is
declared **`type resource`** and is move-only:

```cstar
type resource Buffer {
    List<byte> data;                                 // owns heap → resource; fields stay private
    public Buffer(int n) { … }
    public fn int32 size() { return this.data.length(); }
}
```
A `value` that transitively owns a resource is a **compile error** ("declare `type resource`"), and a
`~dtor` is allowed only on a `resource` (`~dtor` ⟺ `resource` — a `value` owns nothing to free).

## RAII / destructors ✅

A `~Type()` destructor runs deterministically at scope exit, in reverse construction order, on every
path (block end, early `return`, `break`/`continue`). Destructible fields are destroyed in reverse
declaration order. No GC; allocation/deallocation is predictable.

## Inheritance & virtual dispatch ✅ (M6, M25b, M26h)

Extensible hierarchies are a **`resource`** concern (an embedded vtable breaks a `value`'s free copy).
The extensible base opts in with a qualifier after `type`:

```cstar
type virtual resource Shape {                          // `type virtual resource` opts in to extension
    public fn int describe() { return this.area(); }   // public surface
    protected virtual fn int area() { return 0; }      // overridable hooks are written `protected`
}
type final resource Circle extends Shape {             // `type final resource` = sealed leaf
    protected override fn int area() { return 42; }
}
```
Single inheritance (`extends`), base embedded by value (upcast is offset-0), base ctor via `: base(...)`,
`base.m()` for non-virtual upcalls. `virtual`/`override` methods dispatch through a vtable. **Inheritance
is opt-in and one-way:** only a `type virtual resource`/`type abstract resource` may be `extends`-ed (a
`value`, a plain `resource`, and a `type final resource` are sealed); an overridable method is written
`protected` (never public/private — public polymorphism is a `contract`'s job); `type final resource`/`final`
method seal a leaf/slot. `virtual`/`abstract`/`final` and `protected` are meaningless outside an extensible
`resource` — they are errors on a `value`, a plain `resource`, or a `contract`. See `docs/KEYWORDS.md` for
the full kind table.

## Contracts ✅ (M6b, M25, M26h)

A **`contract`** (this replaces the old `interface`) is a public-only guarantee — "some type satisfying
this contract." It has methods only: no bodies, no fields, no ctor/dtor.

```cstar
type contract IShape { fn int64 area(); }              // a public guarantee (a "type placeholder")
type value Circle implements IShape {                  // a value satisfies a contract, too
    int64 r;
    public Circle(int64 r) { this.r = r; }
    public fn int64 area() { return r * r; }           // a method satisfying IShape MUST be `public`
}
fn int64 measure(IShape sh) { return sh.area(); }      // accept "any shape" — by value = zero-copy dispatch
```
A contract is represented as a fat pointer `{obj, vtbl}` (an implementation detail of type erasure — never
something you spell). Both a `value` and a `resource` may `implements` any number of contracts; a method
that satisfies a contract method **must be declared `public`** (the contract is public — a hidden
implementer would be reachable through the contract but not by name). A contract may **refine** another
(`type contract Animated : Drawable { … }`) for capability layering, without inheritance.

**Passing a contract — by value vs. `ref`/`out`** (mirrors C#'s `ref` rule exactly):
- `IShape sh` (by value) — "use it as a shape." A concrete `Circle` coerces in (IS-A); zero-copy dispatch.
  This is the common path.
- `ref IShape sh` / `out IShape sh` — "I may **reseat** your handle." Requires the argument to be an actual
  `IShape` variable (its address is passed, so the reseat sticks). Passing a **concrete type** by `ref`/`out`
  is a compile error — bind it first (`IShape s = c; measure(sh: ref s)`). Mutable references are *invariant*:
  a `Circle` variable isn't a slot that could hold an arbitrary shape, so it can't back a `ref IShape`.

**Borrow vs. storage — a contract value is second-class (M26e).** The fat pointer *borrows* its object, so a
bare contract value is fine as a **parameter or local** (the zero-copy polymorphic view above) but **cannot
be stored beyond the call that made it** — a bare `IShape` **field**, **return type**, or **collection
element** is a compile error, because the borrowed object could die and leave it dangling. To keep
polymorphism around, **own the object** with a smart pointer over the contract (below). Ownership is always
written explicitly — never an implicit box. This is the language-wide rule **"borrow is parameter-only;
storage requires ownership"** — the same reason a `ref` parameter can't be returned and a returnable
"reference" is always an owned smart-pointer handle.

**Owned contracts — `Owned`/`Shared`/`Weak<IShape>` ✅ (M26g).** A smart pointer *over a contract* owns the
concrete object behind a fat handle `{obj, vtbl}` (`Shared`/`Weak` add a `ctrl` block). `new Circle(...)`
boxes a concrete implementer into it; `p.draw()` dispatches polymorphically through the vtable; dropping the
handle runs the concrete destructor through a **virtual-destructor slot in the contract vtable**, then frees
the object. `Owned<I>` is move-only; `Shared<I>` retains/releases (`Weak<I>.upgrade() -> Shared<I>`). Because
the handle is an ordinary value type, it **stores** — as a field or a function return:
```cstar
type resource Holder { Shared<IShape> shape;  public fn int64 area() { return this.shape.area(); } }
fn Owned<IShape> make(int64 s) { Owned<IShape> o = new Square(s: s); return give o; }
```
A `List<Shared<IShape>>` (the engine's scene) works — polymorphic elements stored and dropped in RAII order.

## Static methods & operator overloading ✅ (M31)

**Static methods** (M31a) — a `static fn` has **no implicit `self`** and is called at the type level with
named args:
```cstar
type value Vec2 {
    public float64 x;  public float64 y;
    public Vec2(float64 x, float64 y) { this.x = x; this.y = y; }
    public static fn float64 dot(Vec2 left, Vec2 right) { return left.x*right.x + left.y*right.y; }
}
float64 d = Vec2::dot(left: a, right: b);
```
A `static` method has no vtable slot (so it can't be `virtual`/`override`/`abstract`) and may not touch
`this` or a bare field.

**Operator overloading** (M31b) — the sanctioned exception to named-args-only (a binary operator has
exactly two operands, positional by nature). The full overloadable set is supported: arithmetic
`+ - * / %`, comparison `== != < > <= >=`, bitwise `& | ^ << >>`, unary `- ! ~`, and `++`/`--`. **Arity
picks the form:**

| params | form | example | `a op b` lowers to |
|--------|------|---------|--------------------|
| 0 | unary on `this` | `Vec2 operator-()` | `Vec2__op_neg(&a)` |
| 1 | binary **method** (`this` is the left operand) | `Vec2 operator+(Vec2 rhs)` | `Vec2__op_add(&a, b)` |
| 2 | binary **free/static** (both explicit) | `Vec2 operator*(float64 s, Vec2 v)` | `Vec2__op_mul(a, b)` |

The free form handles the mixed-type case a method can't — a primitive on the **left** (`s * v`). Dispatch
prefers the method form on the left operand's type, else a free form on either operand's type. A binary or
unary expression whose operands are all primitives keeps the built-in C operator (zero overhead).

**Chaining & compound assignment.** Operators chain freely (`a + b + c`, `-(a + b)`, `(a + b) * s`), in
any position including a raw `if`/`while` condition: a nested rvalue is wrapped in a C99 compound-literal
array so the method form's by-pointer `this` is legal without a statement slot (and it re-evaluates
correctly each loop pass). Compound assignment lowers to the operator — `pos += vel` ≡ `pos = pos + vel`.

`==` is **explicit** — a `value` without `operator==` cannot be compared (there is no auto-generated
structural equality; an opt-in `Equatable` derive is future work). Index `operator[]` and the `true`/`false`
conversion operators are out of scope.

Used in a `contract`, an operator becomes a **bound** for generic math (see below).

## Generics ✅ (M27)

User-defined generics, **monomorphized** (one specialized copy per concrete type — elements inline, no
boxing; identical layout and cost to the built-in collections). Erasure was rejected.

```
type value Pair<A, B> { public A a; public B b; public Pair(A a, B b){ this.a = a; this.b = b; } }
fn T max<T>(T a, T b) { return a > b ? a : b; }         // generic fn — type args INFERRED from the call
Pair<int32, string> p = Pair(a: 1, b: "x");             // generic type
int32 m = max(a: 3, b: 4);                              // -> max<int32>, a static specialized C fn
List<Shared<IShape>> scene;                             // nested generics, no space (the `>>` split)
```

- **Generic functions and types**; multi-parameter (`Pair<A, B>`), nested (`Box<Pair<int, int>>`) — nested
  `>>` needs no space. `type value`/`resource` generics both work (a generic resource is move-only with a
  per-instance dtor). Function type args are inferred from the call (no explicit `<…>`).
- **Contract bounds** — `fn sort<T: IComparable>(…)`, `type value Map<K: IHashable + IComparable, V>`. `+`
  means **AND** (all listed contracts). A bound lets the body call the contract's methods on a type-param
  value; because it's monomorphized, those calls are **static direct calls** (zero cost, no vtable). Each
  concrete type argument is checked to satisfy its bounds, else a clean compile error.
- **`This`** — the self-type, inside a `type contract` or a type's own methods: `fn bool equals(This other)`,
  `fn This clone()`. Resolves to the implementing/concrete type; used as a bound (`<T: IEquatable>`), the
  dispatch is static. Chosen over `Self` to pair with the `this` value and the PascalCase-types convention.
- **Generic math (operators as bounds, M31c)** — a `contract` may declare **operators**, giving generic
  code arithmetic over any conforming type at zero cost:
  ```
  type contract IArithmetic { This operator+(This rhs); }
  fn T sum<T: IArithmetic>(T a, T b) { return a + b; }   // `a + b` -> static Concrete__op_add(&a, b)
  ```
  The concrete type's `operator+` satisfies the bound structurally, and `a + b` in the monomorphized body
  lowers to a direct call — no vtable, no boxing.

## Access control ✅ (M25, M26h)

Encapsulation is compile-time only (the emitted C is unchanged) and stricter than C#:
- **Private by default.** A member with no modifier is private; `public`/`protected`/`private` set it
  explicitly. `protected` = the owner or a subclass; external code sees `public` only.
- **Field visibility is per field on a `value`.** A `type value` marks each field `public` (externally
  accessible) or leaves it private (default, reached through accessors) — a `value` with all-public
  fields is the old "pod." A `type resource` keeps **all fields private** (ownership stays encapsulated);
  a `type contract` has no fields at all.
- **Overridable methods are written `protected`** (public polymorphism is a `contract`'s job); a type
  opts into extension as a `type virtual resource`/`type abstract resource` and seals as a plain
  `resource`/`type final resource`. `protected` and `virtual`/`abstract`/`final` are errors outside an
  extensible `resource`.
- **`~dtor` ⟺ `resource`** — a destructor is allowed only on a `resource` (a `value` owns nothing).
- **`friend`** grants are granular and owner-declared: `friend <accessor>[members];` (or `[...]` for all
  privates), where the accessor is a type, a free function, or a `Type::method` — greppable and explicit.

See `docs/KEYWORDS.md` for the full kind × visibility table.

## Enums ✅

```cstar
enum Color { Red, Green = 5, Blue }   // Red=0, Green=5, Blue=6
Color c = Color::Blue;       // enum members are scope-resolved with ::
```
Lowers to a C `enum` (members mangled `Color_Red`…). Enum values are integers — usable in `switch`,
comparisons, and `cast`.

## Modules / namespaces ✅ (M13–M14)

Pass several source files to one build; the compiler emits a shared header (`<out>.gen.h`) + one `.c` of
definitions per file, then compiles + links them:

```sh
cstar build graphics.cstar physics.cstar main.cstar -o app
```

**Private-by-default.** A file with no `namespace` keeps its top-level symbols **file-private** (invisible
to other files) — so single-file programs/scripts need no boilerplate and you can never accidentally call
another file's helper. To share across files, declare a namespace:

```cstar
// graphics.cstar
namespace Graphics;
type resource Texture { ... }   // Graphics::Texture (owns a GPU handle → resource)
fn int32 scale(int32 x) { ... } // Graphics::scale

// main.cstar
using Graphics;              // import unqualified
using Phys = Physics;        // alias
fn int main() {
    Texture t = ...;              // Graphics::Texture (via using)
    Physics::Texture p = ...;     // qualified — distinct type, no collision
    int n = Graphics::scale(x: 3);// qualified namespaced call
}
```

**Scope resolution uses `::`** (namespaces, qualified types, enum members: `Color::Blue`); `.` is
**instance/value access only** (`obj.field`, `obj.method()`). The two are now *syntactically* distinct, so
there's no namespace-vs-object precedence rule — a `::` head is always a type/namespace, a `.` head always a
value. `main` is
the global entry point (unmangled). Deferred: per-symbol `public`/`private` access modifiers (a file is
wholly public if it declares a namespace, wholly private otherwise), nested `namespace { }` blocks, and an
incremental `.o` build cache. 🚧

## Building & debugging ✅

```sh
cstar build app.cstar                 # native debug (-g, breakpoints in .cstar via #line)
cstar build app.cstar --release       # optimized, stripped, NDEBUG
cstar build app.cstar --target wasm   # browser: .html + .js + .wasm
```
Debug builds are breakpoint-debuggable in an IDE (locals + call stack map back to `.cstar`).

## Reserved/runtime

Generated C reserves `__`-prefixed identifiers (`__base`, `__vptr`, `__ret_N`) and `Type__member`
mangling. The runtime ([../cstar_runtime.h](../cstar_runtime.h)) provides `cstar_string` and a
`cstar_trace`/`cstar_trace_get` hook used by tests.
