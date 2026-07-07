# cstar language specification (overview)

This is a semantics overview. The **grammar is authoritative** — see
[grammar.bnf](grammar.bnf) (generated from `cstar.y`). Executable examples live in
[`../tests/`](../tests/) (`*.cstar` with a `.expect` exit code). The design philosophy — *one way to do a
thing, explicit over implicit, no GC / RAII* — lives in [../GOALS.md](../GOALS.md); this document is the
semantics/feature reference. Status flags below: ✅ implemented, 🚧 reserved (not yet implemented).

## Model

cstar compiles to **portable C** (native + WASM). No garbage collector — object lifetimes are deterministic
(RAII). Calls use **named arguments** (no positional). Every type declaration is `type value` (owns nothing,
copies), `type resource` (owns/has identity, moves, RAII-dropped), or `type contract` (an interface).

## Types ✅

| cstar | C |
|---|---|
| `int8 int16 int32/int int64` | `int8_t … int64_t` |
| `uint8 uint16 uint32 uint64` | `uint8_t … uint64_t` |
| `float32` / `float64`/`double` | `float` / `double` |
| `bool` | `bool` |
| `string` | `cstar_string` (borrowed view or heap-owned RAII string) |
| `void` | `void` |
| user `type value`/`type resource` | `struct` (value semantics) |

No raw arrays and **no raw pointers — by design** (raw memory access is confined to `unsafe { }` at the FFI
boundary). Collections are generic library types.

### The `string` type

There is **one** string type: lowercase `string`, a builtin like `int32`/`bool`/`float64`. It lowers to a
fat value that is one of two things, chosen automatically:

- a **borrowed** literal/view — **no allocation, no free** (a string literal `"ab"`, or a view onto another
  string's storage);
- a **heap-owned** RAII string — allocated on the heap, **freed on drop** (produced by operations that must
  build a new buffer, e.g. `concat`).

```cstar
string s = "ab";                    // borrowed literal — no alloc
string t = s.concat(other: "cd");   // heap-owned, RAII-freed at scope exit
bool eq = s.equals(other: t);   int len = s.length();
```

You never spell the borrowed-vs-owned distinction; the type carries it, and RAII frees exactly the owned
ones. There is no separate capital-`String` collection type.

**UTF-8 everywhere.** A `string` is **UTF-8 bytes**; `length()` is the **byte** length (O(1)), and literals
encode `\u{…}` escapes to UTF-8. Two ways to traverse it, kept distinct by type so bytes and characters
never blur:

- **bytes** — `s[i]` returns the i-th byte as a **`uint8`** (bounds-checked); `foreach (uint8 b in s)`
  iterates bytes. `foreach (char c in s)` is a type error — the byte/codepoint distinction is enforced.
- **codepoints** — `s.chars()` is a **UTF-8 codepoint iterator** (`implements Iterator<char>`):
  `foreach (char c in s.chars())` yields each Unicode scalar value as a **`char`**. It's a borrow, valid
  while the string is.

```cstar
string s = "A\u{E9}\u{20AC}";           // "Aé€" — 6 UTF-8 bytes, 3 codepoints
int n = 0;
foreach (char c in s.chars()) { n = n + 1; }   // n == 3 (codepoints, not bytes)
uint8 first = s[0];                     // 65 ('A'), a byte
```

**`char`** is a distinct primitive — a Unicode scalar value backed by `uint32` (not a numeric type, so it
can't silently mix with ints). Literals: `'a'`, `'\n'`, `'\u{1F600}'`. Equality + ordering compare
codepoints; `cast<int32>(c)` / `cast<char>(i)` convert (arithmetic on codepoints is explicit, the Rust
model). (A multibyte *source* char literal like `'é'` isn't lexed yet — write `'\u{E9}'`.)

## Collections & strings ✅

Built-in generics, monomorphized per element type and backed by the C runtime (unsafe internals, safe API —
the Rust-`Vec` model); **indexing is bounds-checked** (a clean trap, not UB). An indexed element `a[i]` is
a **place** (an lvalue): you can write a field through it (`a[i].x = v`), index it again
(`m[i][j] = v`), compound-assign it (`a[i] += x`), or borrow it (`ref a[i]`) — every form stays
bounds-checked. (Reading `a[i]` still yields a copy.)

```cstar
Array<int32> a = new Array<int32>(size: 4);   // fixed buffer, zero-initialized
a[0] = 10;  a[1] = 20;                          // bounds-checked []
int32 first = a[0];
foreach (int32 x in a) { /* ... */ }            // iterate (x is a copy)
foreach (ref int32 x in a) { x = x * 2; }       // `ref`: mutate each element in place

List<Point> ps = new List<Point>();             // growable
ps.add(item: p);   int n = ps.length();   Point q = ps[0];

string s = "ab";                                // borrowed literal (no alloc)
string t = s.concat(other: "cd");               // heap-owned, RAII-freed
```

All collections own their storage and free it via RAII (with element-destructor chaining). Only the
`(collection, element-type)` pairs the program actually uses are emitted (pay-for-what-you-use). A **method
call on an element** works directly — `list[i].method()` borrows the element *in place*, so a mutating method
mutates the stored element; a `const` collection allows only const methods on its elements. Elements enter a
collection by the ownership rules below (`give` to move, `copy` to duplicate, a `value` copies).

## Smart pointers ✅

The smart pointers are a **standard library**, not compiler intrinsics: `Owned`/`Shared`/`Weak` live in
`std::memory`, written in ordinary cstar (RAII `resource`s over `Deref`/`HeapOwner`, refcounting in cstar),
and are pulled in with `import std::memory::{…}`. The compiler adds only what a library can't express: the
type-erasure (fat pointer + vtable) that makes `Owned<Shape>`/`Shared<Shape>` over a **contract** work, and
`new T(args)` heap placement into any `HeapOwner<T>`.

```cstar
import std::memory::{Owned, Shared, Weak};
```

`Owned<T>` — unique heap ownership (= Rust `Box` / C++ `unique_ptr`), zero overhead, **move-only**,
**auto-deref**, RAII-freed. The cstar surface stays pointer-free; the raw pointer is confined to the library.
Use it for heap objects, recursive data structures, and polymorphic ownership.

```cstar
Owned<Counter> c = new Counter(start: 40);          // `new` heap-boxes the ELEMENT type
c.bump();  int n = c.get();                          // auto-deref: . reaches the pointee
Owned<Counter> d = c;                                // MOVE: c is now empty (moved-from)
fn Owned<Node> make(int v) { Owned<Node> n = new Node(id: v); return n; }  // factory: moves out
```

Move-only: copying/initializing/returning an `Owned` transfers ownership and invalidates the source, so the
pointee is freed exactly once (RAII, with the pointee's destructor).

**Ownership hand-off — `give` / `copy`.** When a *named* owned value is handed off — in an initializer,
assignment, argument, or return — an explicit marker states the intent, uniformly in all four positions:
**`give`** moves (invalidates the source), **`copy`** retains (`Shared`/`Weak`) or duplicates. The natural op
is the default, so a marker is only required where a silent copy would be surprising: `Owned` defaults to
`give` (and `copy Owned` is an error — it's unique); `Shared`/`Weak` are **copy-only** (shared ownership —
`implements Copyable, !Movable`), so a bare hand-off **retains** and **`give` is an error** (there is no move
to make — no footgun); a `value`/primitive just copies (and `give` on one is an error). A fresh
`new`/constructor/call result needs no marker. Smart pointers also **pass by value**: the callee *owns* the
argument and drops it at function end — `fn int use(Owned<T> p)` consumes it (`use(p: give x)`), `fn int
peek(Shared<T> s)` retains it (`peek(s: x)`, `x` stays valid).

```cstar
Owned<Counter> b = give a;   // explicit move (a consumed)
Shared<Counter> t = s;       // copy/retain (default) — both valid
Shared<Counter> u = copy s;  // explicit retain (same as bare); `give s` is an error (copy-only)
```

*(`copy` of a collection is a deep copy — a fresh buffer, element-wise: a bitwise-copyable element is copied
memberwise, a `Copyable`-resource element is deep-copied via its own `copy()`. A resource element that is not
`Copyable` is rejected. `give` of a collection **moves** the buffer.)*

**Move-only `resource` values + the `Copyable` contract.** A **`type resource`** value (it owns something, or
has identity) is **move-only**: a bare named hand-off *moves* (the source is consumed, its destructor
suppressed), so its heap is freed exactly once — a silent copy is never emitted (that would double-free).
`give` is optional emphasis; `copy` is an error unless the type opts in. A `resource` **opts into copy**
**nominally** — `implements Copyable` (the prelude contract `Copyable { fn This copy(); }`) plus a **public
nullary `copy()`** method (a lone `copy()` method without the `implements` does *not* make a type copyable).
Once copyable, the marker is **mandatory** — both move and copy are plausible, so a *bare* hand-off is a
compile error and you must write **`give x`** (move) or **`copy x`** (deep-copy via `copy()`; the source
stays valid). Because `copy`/`give` are markers only in expression position, they're **contextual keywords** —
usable as method names, so the opt-in method is literally named `copy`.

```cstar
type resource Res implements Copyable {
    List<int32> items;
    ~Res() { }
    public fn Res copy() { Res r = Res(v: this.items[0]); return give r; }   // the Copyable method
}
Res b = copy a;   // deep copy — a stays valid, b has its own buffer
Res c = give b;   // move — b consumed
Res d = a;        // ERROR: a is copyable — say `give` or `copy`
```

**Auto-deref — the `Deref<T>` contract.** The standard smart pointers forward member access to their
pointee (`ptr.method()`/`ptr.field` reach the held `T`). Any type can opt into the same **auto-deref** by
implementing the prelude contract `type contract Deref<T> { fn ref T deref(); }` — the `implements` is the
explicit gate. When a member isn't found on the wrapper itself, it resolves on the pointee `T` and is
called through `deref()` (which returns a *place* — a `T*` — into the pointee); resolution is recursive, so
deref chains. `deref()` returns a place rooted at `this`, so it's bound by the same second-class-borrow
rules as `ref T operator[]` (no lifetimes needed). This is how a smart pointer is written as an ordinary
`resource` (RAII + move-only come free) rather than a compiler intrinsic.

```cstar
type value Point { public int32 x; public int32 y; public fn int32 sum() { return this.x + this.y; }
                   public Point(int32 x, int32 y) { this.x = x; this.y = y; } }
type value BoxP implements Deref<Point> {
    Point inner;
    public BoxP(Point p) { this.inner = p; }
    public fn ref Point deref() { return this.inner; }
}
BoxP b = BoxP(p: Point(x: 30, y: 12));
int32 s = b.sum();   // auto-deref -> Point__sum(BoxP__deref(&b))  (42)
int32 x = b.x;       // auto-deref -> BoxP__deref(&b)->x           (30)
```

**The give/copy behavior matrix.** A marker is required exactly when *both* move and copy are plausible
("silent default, scream when ambiguous"); otherwise the one natural op is silent. The rule is uniform across
all four hand-off positions — **initializer, assignment, argument, return** — and a *fresh* rvalue
(`new`/constructor/call result) never takes a marker.

| kind | bare hand-off | `give` | `copy` |
|---|---|---|---|
| primitive / `value` | **copy** (cheap) | ⛔ "applies to an owned value" | copy (redundant, allowed) |
| `Owned<T>` (unique) | **move** | move (emphasis) | ⛔ "is unique" |
| `Shared<T>` (ref-counted, copy-only) | **retain** (strong++) | ⛔ "copy-only" | retain (explicit) |
| `Weak<T>` (copy-only) | **retain** (weak++) | ⛔ "copy-only" | retain (explicit) |
| collection (`Array`/`List`/`string`) | ⛔ marker required | **move** (buffer) | **deep copy** (fresh buffer) |
| plain `resource` (move-only value) | **move** | move (emphasis) | ⛔ "opt into `Copyable`" |
| `Copyable` resource (has `copy()`) | ⛔ ambiguous | move | **deep copy** via `copy()` |
| collection of `Copyable` elements | ⛔ marker required | move | **deep copy** (element-wise `copy()`) |

A marker on a fresh rvalue is an error. Move tracking is compile-time: reading a moved value, moving out of a
field/element, moving inside a loop a value declared outside it, and a conditional move that is still live at
scope exit are all rejected — there is no runtime drop flag.

`Shared<T>` — ref-counted shared ownership (= C++ `shared_ptr` / Rust `Rc`). **Copyable**: each copy retains
(refcount++), each drop releases, and the pointee is destroyed when the **last** handle goes away.

```cstar
Shared<Tex> a = new Tex(id: 7);
Shared<Tex> b = a;     // retain — a and b share one Tex (both valid)
b.use();  int n = a.id;
// a, b drop in RAII order; the Tex is freed exactly once, with the last handle
```

`Weak<T>` — a non-owning weak reference to a `Shared<T>`'s pointee. It does **not** keep the pointee alive, so
it **breaks reference cycles** that `Shared` alone would leak. You can't dereference a `Weak` (it may be
dead) — **upgrade** it with the checked `tryUpgrade()`, which returns an `Optional<Shared<T>>` you must
`match` on, so the dead case is impossible to ignore:

```cstar
Weak<Tex> w = s.downgrade();                  // make a weak ref from a Shared (does not keep Tex alive)
int32 id = match (w.tryUpgrade()) {           // -> Optional<Shared<Tex>>
    case Some(up): up.id;                     // alive: use the upgraded Shared
    case None: -1;                            // dead: the cycle-safe path
};
```

**No null (safe surface) — see GOALS §3b.** A value, `Owned`/`Shared`, `ref`/`out` borrow, or contract value
is always valid: there is nothing to null-check. `== null` / `!= null` on a safe type is a **compile error**
(the C habit checks the wrong thing here); `null` is only for `Ptr<T>` at the FFI boundary. A `Weak<T>`'s
liveness is obtained through `tryUpgrade() -> Optional<Shared<T>>`, whose result forces you to handle the
dead case.

Passing a smart pointer: **borrow** it by passing `ref T` — the borrow names the *object* (`ref T`,
storage-agnostic; a `ref` may not name the smart pointer itself), which auto-derefs to the held object; or
**transfer by value**, where the callee owns the argument and drops it at function end (`Owned` moves in,
`Shared` retains). The pointee is a **`value`/`resource`** or a **contract** — `Owned`/`Shared`/`Weak<Shape>`
own a concrete implementer behind a fat handle and dispatch polymorphically (see Contracts below). A smart
pointer works as a *field*, *return*, and a **collection element** — `List<Shared<Shape>>` stores and drops
each handle in RAII order and dispatches polymorphically through it. See **Generics** below.

## Functions ✅

```cstar
fn int add(int a, int b) { return a + b; }
fn int main() { return add(b: 20, a: 10); }   // named args; reordered to declared order
```
`ref`/`out` parameters pass by pointer: `fn void set(out int dst) { dst = 42; }` … `set(dst: ref x);`.

## FFI — calling C ✅

`extern fn Ret name(params);` declares a C function's call signature (name + named params for lowering); the
C **prototype comes from the header** you `extern "<header.h>";` — cstar never emits a prototype for an
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
declaration is purely cstar's call-signature (name + named params, so it can lower the call) — the actual C
prototype comes from the header you include with `extern "<header.h>";`. cstar never emits a C prototype for
an extern function, so there are no redeclaration conflicts; and the runtime hides its own libc dependencies
(block-scope declarations), so **no** C function (not even `malloc`) is available without its header — a
missing include is a plain C error, never a silent guess.

`Ptr` is `void*`; `Ptr<T>` is `T*` — an **opaque carrier** (hold, pass to/from C, `null`-check, compare;
**no dereference** in cstar outside `unsafe`). `usize`/`isize` map to `size_t`/`ptrdiff_t`. Names beginning
`cstar_` are reserved (runtime-provided).

**FFI data — all controlled, no `unsafe` needed:**

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

`type extern value Foo { ... }` is an **external** struct provided by an included header / linked code —
cstar uses its fields (all public, the C layout) but never re-emits it (so no redefinition), and its name is
the literal C name. `addr(of: x)` takes the address of a real local (out-params, descriptor pointers) — a
*controlled* op, no `unsafe`. `s.cstr()` yields a C `const char*`.

### `unsafe { }` — raw pointer memory access

The **only** place cstar can touch arbitrary memory through a raw pointer. Raw `Ptr<T>` index/store is a
**compile error outside** an `unsafe { }` block — so the entire dangerous surface is explicit and greppable
(`grep -rn 'unsafe {'`). Everything else (collections, smart pointers, FFI structs/handles/out-params,
`addr`) stays safe.

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

`a.dataPtr()`/`a.byteLen()` bridge a collection's buffer to C (safe to call; the returned `Ptr` is valid only
while the collection is alive + unmodified, and dereferencing it requires `unsafe`). An unlowered construct
(including a safety-gate violation) is a **hard build error** — cstar never emits incomplete C and claims
success.

### Writing a collection *in* cstar — `sizeof`, `panic`/`assert`, place-returning methods ✅

The above pieces (a place-returning `operator[]`, `Ptr<T>` + `unsafe`, generics, RAII) let a `Vec`/matrix
be written **in the language** rather than baked into the compiler. Three builtins complete the kit:

- **`sizeof(T)`** — the compile-time byte size of a type (a `usize`); monomorphizes, so
  `malloc(n: n * sizeof(T))` works in a generic `Vec<T>`.
- **`panic(msg: string)` / `assert(cond: bool)`** — a clean **trap** (writes the message + `abort()`, not
  UB — the user-facing form of the built-in bounds trap). For a *bug that can't continue*; recoverable
  errors use `Result<T, E>`. (cstar aborts on panic — no stack unwinding; ≈ Rust's `panic=abort`.)
- **`drop(value: place)`** — run a place's destructor now (a no-op for a non-destructible type); lets a
  library owner over `Ptr<T>` drop its heap pointee before `free`.
- **`addr(of: place)`** — the address of a place (a field/local/element) as a `Ptr<T>`. Taking an address
  is safe (a `Ptr` is safe to hold); dereferencing stays `unsafe`. Lets a library type hold a live
  back-pointer to another's field (e.g. an iterator to its container's mutation counter).
- **A place-returning method** — `public fn ref T at(usize i) { … }` returns a place, exactly like
  `operator[]`, so `v.at(i) = x` works. A `ref T` result must borrow `this` or a `ref` parameter (never a
  local — it would dangle), and it's second-class (used in-place, never stored).

**`foreach` over a user type — the iterator protocol.** A user container is `foreach`-able (not just the
built-in `Fixed`) via a small **iterator protocol** — not indexing, so it works for any shape (list, tree,
map, range). It's zero-cost: monomorphized to **direct calls** (no vtable), and the container is
**borrowed, not consumed**. The container hands out an iterator via a nullary factory method, and that
**iterator must `implements` the matching prelude contract** — `foreach` is **nominal**: a type with the
right method shape but no `implements` is rejected (explicit over implicit).
- **value** — `foreach (T x in v)`: `v` provides `fn <Iter> iterator()` whose iterator
  `implements Iterator<T>` (`fn Optional<T> next()` — `Some` per element, `None` at the end); or `v`
  *is* the iterator (`implements Iterator<T>` + a nullary `next()`).
- **mutable** — `foreach (ref T x in v)`: `v` provides `fn <IterMut> iterMut()` whose iterator
  `implements IteratorMut<T>` (`fn bool hasNext()` + a place-returning `fn ref T next()` — Rust's
  `iter()`/`iter_mut()` split; `Optional` can't carry a place, so mutable is a parallel iterator).
- A borrowing iterator holds a `Ptr` cursor (its own `unsafe` internals); the `foreach` surface is safe.

The prelude contracts (`type contract Iterator<T> for both { fn Optional<T> next(); }` and
`IteratorMut<T>`) are ordinary monomorphized generic contracts, so they double as a static bound —
`fn sum<I: Iterator<int32>>(I it)` (zero-cost, direct `Concrete__next`) or a dynamic fat-pointer value
`Iterator<int32> it` (vtable). `foreach` uses the same `implements`, checked nominally.

Iterator safety: growing a collection (`add`) while iterating it would be a use-after-free when the
buffer reallocates. The library `List` **guards against this at runtime** (C#-style): a modification
counter is bumped on every structural change (`add`), each iterator snapshots it, and `next()`/
`hasNext()` `panic`s if it changed — *before* the stale cursor is dereferenced. In-place element writes
(`foreach (ref x in list) { x = … }`) don't touch the counter and are fine — that's the point of `ref`.
The guard lives in `List`'s own cstar source (not the compiler), so it's a stdlib policy: a hand-rolled
container chooses whether to pay for it. `Array`/`Fixed` are fixed-size and can't reallocate, so they
need no guard. (The iterator's back-pointer to the counter uses the `addr(of: place)` builtin — the
address of a place as a `Ptr<T>`; safe to take, `unsafe` to deref.)

### Function pointers — `fnptr` ✅

cstar has no naked function pointers. **`fnptr`** declares an explicit, named function-pointer **type**
(independent of any user type) — **zero-cost** (a bare C function pointer, no wrapper). It is **non-null**
(must be bound; no `null`, no null-check at the call), and binding a free function is **signature-checked**.
(A bodiless `fn` is *not* a function pointer — a forgotten body is a clear error, never a silent type.)

```cstar
fnptr int32 Comparator(int32 a, int32 b);       // an explicit function-pointer TYPE
fn int32 cmp(int32 a, int32 b) { return a - b; }

Comparator c = cmp;                             // bind by name (positional, type-checked) — used directly
int32 r = c(a: 9, b: 2);                        // named invoke through the pointer
```

A bare **function name used as a value** is its function pointer (Rust-like), so binding and passing need no
operator — `c = cmp` and `f(cb: cmp)` just work. An `fnptr` can also be a **parameter** (`fn run(Op op, …) {
op(…) }` — the core callback shape).

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
`Shared` `obj:` is **retained** (refcount; the object lives while any owner holds it). A bare `fnptr` / free
function **promotes** in with a null object — so a `BindableFunctionPtr<Sig>` parameter accepts both free and
bound callables, while `fnptr` stays the zero-cost free-only form. It is **move-only** (it may uniquely own
its object): returning one from a factory transfers ownership; the captured object's destructor runs
**exactly once** when the bindable finally drops.

**FFI**: an `extern fn` may take an `fnptr` type as a param; passing it hands C the raw pointer. C requires an
*exact* function-pointer-type match (incompatible fn-pointer types are a hard error), so when the C callback
signature is one cstar's `fnptr` doesn't spell identically — most commonly `const`-qualified parameters —
name the callback via a header `typedef` and **cast** to it at the edge:

```cstar
extern "<stdlib.h>";
extern "cb.h";   // typedef int (*CompareFn)(const void*, const void*);
fnptr int32 Comparator(Ptr<int32> a, Ptr<int32> b);
extern fn void qsort(Ptr buf, usize nmemb, usize size, CompareFn compar);
...
Comparator c = cmp;
qsort(buf: a.dataPtr(), nmemb: 4, size: 4, compar: cast<CompareFn>(c));   // cast to the header's fn-ptr type
```

## Control flow ✅

`if/else`, `while`, `do/while`, `for`, `foreach`, `break`, `continue`, `return`; the full operator set
(`+ - * / %`, bitwise, shifts, comparisons, `&& || !`, ternary `?:`), assignment ops (`= += …`), `++`/`--`,
casts. Branching on an enum is done with **`match`** (see Enums & `match` below); arbitrary-integer branching
is done with `if` / `else if`. There is no `switch` statement.

## Type declarations — `value` / `resource` / `contract` ✅

Every type declaration is introduced by the **`type` marker** followed by a *kind* — parallel to `fn` on
every function, so declarations are greppable and self-describing:

- **`type value Name { … }`** — owns nothing, **copies** freely (a `memcpy`; no hidden shared refs). Sealed
  (no `virtual`/`abstract`/`final`), no destructor. Fields default **private**; mark a field `public` per
  field (a `value` with all-public fields is a plain-old-data struct).
- **`type resource Name { … }`** — owns something, or has identity: **move-only**, RAII-dropped. Fields are
  **private only** (ownership stays encapsulated). An empty `type resource Token { }` is a valid move-only
  identity/token. Extensible variants add a qualifier after `type`: `type virtual resource`, `type abstract
  resource`, `type final resource`.
- **`type contract Name { … }`** — a public-only guarantee (an interface); methods only, no bodies, no
  fields, no ctor/dtor. Types satisfy it via `implements`; it may refine another (`type contract Animated :
  Drawable { … }`).

The full model + rationale is in [TYPE_MODEL.md](TYPE_MODEL.md). The kind words `value` / `resource` /
`contract` are **contextual, not reserved** — because they appear only right after `type`, they remain
ordinary identifiers everywhere else (`int32 value = 5;`). Only `type` is a keyword.

```cstar
type value Counter {
    int value;                                       // fields are private by default
    public Counter(int start) { value = start; }     // constructor (mark `public` to call from outside)
    public fn void add(int n) { value = value + n; } // method (implicit self)
    public fn int get() { return value; }
}
Counter c = Counter(start: 40);   // stack value — a stack value uses the plain ctor, not `new`
c.add(n: 2);                       // a `value` copies on hand-off
```

Fields, methods (take an implicit `self`), one constructor, field initializers (run in the ctor),
`this.field`, `obj.method(args)`. Lowers to a `struct` + `Counter__method(Counter* self, …)` functions.
Members are **private by default**; `new` is reserved for the heap (`Owned`/`Shared` element construction), so
a stack value uses `Counter(start: 40)`, not `new Counter(...)`. A stack constructor may also be written
**inline in a call argument** — `f(x: Counter(start: 5))` — it materializes a temporary passed by value (a
`value` copies, a `resource` moves); use a local for a `ref`/`out` parameter.

A type that owns a heap resource (a collection, an `Owned`/`Shared`/`Weak`, or another `resource`) is
declared **`type resource`** and is move-only:

```cstar
type resource Buffer {
    List<byte> data;                                 // owns heap → resource; fields stay private
    public Buffer(int n) { … }
    public fn int32 size() { return this.data.length(); }
}
```

A `value` that transitively owns a resource is a **compile error** ("declare `type resource`"), and a `~dtor`
is allowed only on a `resource` (`~dtor` ⟺ `resource` — a `value` owns nothing to free).

## RAII / destructors ✅

A `~Type()` destructor runs deterministically at scope exit, in reverse construction order, on every path
(block end, early `return`, `break`/`continue`). Destructible fields are destroyed in reverse declaration
order. No GC; allocation/deallocation is predictable.

## Fallible construction (no exceptions) ✅

cstar has no exceptions, so **constructors are infallible** — trivial, in-place field setup that cannot fail.
Fallible resource acquisition is a **`static fn` factory returning `Result<T, E>`**: the fallible work lives
in the factory, and on failure it returns `Err` *before* the resource exists, so no half-constructed object
can escape and `match` forces the caller to handle the error.

```cstar
type resource Buffer {
    int32 size;
    private Buffer(int32 size) { this.size = size; }              // trivial, infallible, private
    public static fn Result<Owned<Buffer>, int32> create(int32 size) {
        if (size <= 0) { return Result::Err(error: -1); }         // fail before the resource exists
        Owned<Buffer> b = new Buffer(size: size);
        return Result::Ok(value: give b);
    }
    ~Buffer() { /* … */ }
}
```

A type with a *meaningful* inert state may instead start valid-but-inert and expose a `bring_up():
Result<…>` method. (This reuses static methods + `Result` + `Owned` + RAII — no dedicated feature. See
`tests/fallible_factory`.)

## Inheritance & virtual dispatch ✅

Extensible hierarchies are a **`resource`** concern (an embedded vtable breaks a `value`'s free copy). The
extensible base opts in with a qualifier after `type`:

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
`base.m()` for non-virtual upcalls. `virtual`/`override` methods dispatch through a vtable. **Inheritance is
opt-in and one-way:** only a `type virtual resource`/`type abstract resource` may be `extends`-ed (a `value`,
a plain `resource`, and a `type final resource` are sealed); an overridable method is written `protected`
(never public/private — public polymorphism is a `contract`'s job); `type final resource`/`final` method seal
a leaf/slot. `virtual`/`abstract`/`final` and `protected` are meaningless outside an extensible `resource` —
they are errors on a `value`, a plain `resource`, or a `contract`. See `docs/KEYWORDS.md` for the full kind
table.

## Contracts ✅

A **`contract`** is a public-only guarantee — "some type satisfying this contract." It has methods only: no
bodies, no fields, no ctor/dtor.

```cstar
type contract Shape { fn int64 area(); }               // a public guarantee (a "type placeholder")
type value Circle implements Shape {                   // a value satisfies a contract, too
    int64 r;
    public Circle(int64 r) { this.r = r; }
    public fn int64 area() { return r * r; }           // a method satisfying Shape MUST be `public`
}
fn int64 measure(Shape sh) { return sh.area(); }       // accept "any shape" — by value = zero-copy dispatch
```

A contract is represented as a fat pointer `{obj, vtbl}` (an implementation detail of type erasure — never
something you spell). Both a `value` and a `resource` may `implements` any number of contracts; a method that
satisfies a contract method **must be declared `public`** (the contract is public — a hidden implementer
would be reachable through the contract but not by name). A contract may **refine** another (`type contract
Animated : Drawable { … }`) for capability layering, without inheritance.

**Passing a contract — by value vs. `ref`/`out`** (mirrors C#'s `ref` rule exactly):

- `Shape sh` (by value) — "use it as a shape." A concrete `Circle` coerces in (IS-A); zero-copy dispatch.
  This is the common path.
- `ref Shape sh` / `out Shape sh` — "I may **reseat** your handle." Requires the argument to be an actual
  `Shape` variable (its address is passed, so the reseat sticks). Passing a **concrete type** by `ref`/`out`
  is a compile error — bind it first (`Shape s = c; measure(sh: ref s)`). Mutable references are *invariant*:
  a `Circle` variable isn't a slot that could hold an arbitrary shape, so it can't back a `ref Shape`.

**Borrow vs. storage — a contract value is second-class.** The fat pointer *borrows* its object, so a bare
contract value is fine as a **parameter or local** (the zero-copy polymorphic view above) but **cannot be
stored beyond the call that made it** — a bare `Shape` **field**, **return type**, or **collection element**
is a compile error, because the borrowed object could die and leave it dangling. To keep polymorphism around,
**own the object** with a smart pointer over the contract (below). Ownership is always written explicitly —
never an implicit box. This is the language-wide rule **"borrow is parameter-only; storage requires
ownership"** — the same reason a `ref` parameter can't be returned and a returnable "reference" is always an
owned smart-pointer handle.

**Owned contracts — `Owned`/`Shared`/`Weak<Shape>`.** A smart pointer *over a contract* owns the concrete
object behind a fat handle `{obj, vtbl}` (`Shared`/`Weak` add a `ctrl` block). `new Circle(...)` boxes a
concrete implementer into it; `p.draw()` dispatches polymorphically through the vtable; dropping the handle
runs the concrete destructor through a **virtual-destructor slot in the contract vtable**, then frees the
object. `Owned<Shape>` is move-only; `Shared<Shape>` retains/releases (`Weak<Shape>.tryUpgrade() ->
Optional<Shared<Shape>>`). Because the handle is an ordinary value type, it **stores** — as a field or a
function return:

```cstar
type resource Holder { Shared<Shape> shape;  public fn int64 area() { return this.shape.area(); } }
fn Owned<Shape> make(int64 s) { Owned<Shape> o = new Square(s: s); return give o; }
```

A `List<Shared<Shape>>` (the engine's scene) works — polymorphic elements stored and dropped in RAII order.

## Static methods & operator overloading ✅

**Static methods** — a `static fn` has **no implicit `self`** and is called at the type level with named
args:

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

**Operator overloading** — the sanctioned exception to named-args-only (a binary operator has exactly two
operands, positional by nature). The full overloadable set is supported: arithmetic `+ - * / %`, comparison
`== != < > <= >=`, bitwise `& | ^ << >>`, unary `- ! ~`, and `++`/`--`. **Arity picks the form:**

| params | form | example | `a op b` lowers to |
|--------|------|---------|--------------------|
| 0 | unary on `this` | `Vec2 operator-()` | `Vec2__op_neg(&a)` |
| 1 | binary **method** (`this` is the left operand) | `Vec2 operator+(Vec2 rhs)` | `Vec2__op_add(&a, b)` |
| 2 | binary **free/static** (both explicit) | `Vec2 operator*(float64 s, Vec2 v)` | `Vec2__op_mul(a, b)` |

The free form handles the mixed-type case a method can't — a primitive on the **left** (`s * v`). Dispatch
prefers the method form on the left operand's type, else a free form on either operand's type. A binary or
unary expression whose operands are all primitives keeps the built-in C operator (zero overhead).

**Type-based dispatch.** A type may carry several `operator*` distinguished by **operand type** — `mat * vec`
*and* `mat * mat`, `v * s` *and* `s * v` — exactly as C++/C#/Rust allow. Resolution matches by the operand
types; a same-type / `This` / scalar right operand uses the bare name, a different user-type right operand its
own. Only two operators with the *same* symbol *and* operand type are a duplicate (a clean error).

**Chaining & compound assignment.** Operators chain freely (`a + b + c`, `-(a + b)`, `(a + b) * s`), in any
position including a raw `if`/`while` condition: a nested rvalue is wrapped in a C99 compound-literal array so
the method form's by-pointer `this` is legal without a statement slot (and it re-evaluates correctly each loop
pass). Compound assignment lowers to the operator — `pos += vel` ≡ `pos = pos + vel`. An **inline
constructor** is a valid operand — `v + Vec3(x: 1, y: 0, z: 0)` needs no separate local. It works in an
`if`/`while`/`for` condition too (the condition is wrapped / uses a loop-and-a-half so the temp materializes
and re-evaluates each pass); a `do`/`while` condition is the one place it must still be bound to a local.

`==` is **explicit** — a `value` without `operator==` cannot be compared (there is no auto-generated
structural equality). The `true`/`false` conversion operators are out of scope.

A user type may define a **place-returning index operator** — `public ref T operator[](usize i)` —
whose body returns a place (`return this.cells[i]`). It lowers to `T* C__op_index(C* self, size_t i)`,
and the caller derefs the place, so `g[i] = v`, `g[i] += 1`, `m[i][j] = v`, `m[i].field = v`, and
`ref g[i]` all work — the same place semantics as a built-in collection, now expressible in the
language (so a `Vec`/matrix can be written *in* cstar). The place is a **second-class borrow** of
`self`: it is used transiently and cannot be stored (there is no `ref`-local/`ref`-field to hold it),
and a `const` receiver makes it read-only. Bounds safety is the operator's responsibility — a
`Fixed`/collection-backed body is auto-checked; a raw `Ptr<T>` body is `unsafe`. The same place-return
works for a **named method** — `public fn ref T at(usize i) { … }` — so `v.at(i) = x` too. (A `ref T`
return is supported on methods/operators; free-function `ref T` returns are not yet.)

Used in a `contract`, an operator becomes a **bound** for generic math (see below).

## Generics ✅

User-defined generics, **monomorphized** (one specialized copy per concrete type — elements inline, no
boxing; identical layout and cost to the built-in collections).

```cstar
type value Pair<A, B> { public A a; public B b; public Pair(A a, B b){ this.a = a; this.b = b; } }
fn T max<T>(T a, T b) { return a > b ? a : b; }         // generic fn — type args INFERRED from the call
Pair<int32, string> p = Pair(a: 1, b: "x");             // generic type
int32 m = max(a: 3, b: 4);                              // -> max<int32>, a static specialized C fn
List<Shared<Shape>> scene;                              // nested generics, no space (the `>>` split)
```

- **Generic functions and types**; multi-parameter (`Pair<A, B>`), nested (`Box<Pair<int, int>>`) — nested
  `>>` needs no space. `type value`/`resource` generics both work (a generic resource is move-only with a
  per-instance dtor). Function type args are inferred from the call.
- **Turbofish — explicit type arguments.** When inference can't determine the type args — most commonly a
  **return-only generic** whose type parameter never appears in an argument — spell them explicitly with
  `f::<int32>()` (the `::` before `<` is unambiguous). Turbofish reaches only generic *functions*; a generic
  *type* is still written `Box<int32>` in type position.
  ```cstar
  fn T zero<T>() { return cast<T>(0); }   // T appears only in the return — inference can't see it
  int32 x = zero::<int32>();              // turbofish supplies it
  int64 y = zero::<int64>();
  ```
- **Contract bounds** — `fn sort<T: Comparable>(…)`, `type value Map<K: Hashable + Comparable, V>`. `+` means
  **AND** (all listed contracts). A bound lets the body call the contract's methods on a type-param value;
  because it's monomorphized, those calls are **static direct calls** (zero cost, no vtable). Each concrete
  type argument is checked to satisfy its bounds, else a clean compile error.
- **`This`** — the self-type, inside a `type contract` or a type's own methods: `fn bool equals(This other)`,
  `fn This clone()`. Resolves to the implementing/concrete type; used as a bound (`<T: Equatable>`), the
  dispatch is static. Chosen over `Self` to pair with the `this` value and the PascalCase-types convention.
- **Generic math (operators as bounds)** — a `contract` may declare **operators**, giving generic code
  arithmetic over any conforming type at zero cost:
  ```cstar
  type contract Arithmetic { This operator+(This rhs); }
  fn T sum<T: Arithmetic>(T a, T b) { return a + b; }   // `a + b` -> static Concrete__op_add(&a, b)
  ```
  The concrete type's `operator+` satisfies the bound structurally, and `a + b` in the monomorphized body
  lowers to a direct call — no vtable, no boxing.
- **Generic contracts** — a `contract` may itself be parameterized (`type contract Iterator<T>`), and is
  **monomorphized per use** just like a generic type (`Iterator<int32>` → a specialized `Iterator_int32`).
  It has **full value + bound parity** with a plain contract: usable as a static bound
  `fn sum<I: Iterator<int32>>(I it)` (zero-cost, direct calls) *and* as a dynamic fat-pointer value
  `fn drain(Iterator<int32> it)` (vtable dispatch). A type opts in with `implements Iterator<int32>`.
  ```cstar
  type contract Iterator<T> { fn Optional<T> next(); }
  type value IntRange implements Iterator<int32> { /* … fn Optional<int32> next() … */ }
  fn int32 sum<I: Iterator<int32>>(I it) { /* it.next() → static IntRange__next(&it) */ }
  ```
  (Both dispatch modes coexist by design — monomorphization can't express a heterogeneous
  `List<Iterator<int32>>` or open-world runtime choice; the fat pointer can. See **Contracts**.)

## Access control ✅

Encapsulation is compile-time only (the emitted C is unchanged) and stricter than C#:

- **Private by default.** A member with no modifier is private; `public`/`protected`/`private` set it
  explicitly. `protected` = the owner or a subclass; external code sees `public` only.
- **Field visibility is per field on a `value`.** A `type value` marks each field `public` (externally
  accessible) or leaves it private (default, reached through accessors) — a `value` with all-public fields is
  a plain-old-data struct. A `type resource` keeps **all fields private** (ownership stays encapsulated); a
  `type contract` has no fields at all.
- **Overridable methods are written `protected`** (public polymorphism is a `contract`'s job); a type opts
  into extension as a `type virtual resource`/`type abstract resource` and seals as a plain `resource`/`type
  final resource`. `protected` and `virtual`/`abstract`/`final` are errors outside an extensible `resource`.
- **`~dtor` ⟺ `resource`** — a destructor is allowed only on a `resource` (a `value` owns nothing).
- **`friend`** grants are granular and owner-declared: `friend <accessor>[members];` (or `[...]` for all
  privates), where the accessor is a type, a free function, or a `Type::method` — greppable and explicit.

See `docs/KEYWORDS.md` for the full kind × visibility table.

## Enums & `match` ✅

An `enum` declares either a plain (payload-less) set of variants or a **tagged union** (variants carry
payloads, and the enum may be generic):

```cstar
enum Color { Red, Green = 5, Blue }          // plain: Red=0, Green=5, Blue=6
Color c = Color::Blue;                        // variants are scope-resolved with ::

enum Shape { Circle(float64 r), Rect(float64 w, float64 h) }   // tagged union (payloads)
```

A plain enum lowers to a C `enum`; a tagged union lowers to a tag + payload union. Enum variants are
scope-resolved with `::` and constructed with named args (`Shape::Rect(w: 3.0, h: 4.0)`).

**`match`** is the **one** construct for branching on an enum — payload-less enums, tagged unions, and the
`Optional`/`Result` prelude types alike. It is **value-producing** (usable in statement or expression
position), enforces **compile-time exhaustiveness**, and accepts a `_` wildcard for the catch-all case.
Payload bindings are named in the arm:

```cstar
int32 area = match (sh) {                     // expression position — yields a value
    case Circle(r): cast<int32>(r * r * 3);
    case Rect(w, h): cast<int32>(w * h);
};

match (color) {                               // statement position — a plain enum works too
    case Red: fire();
    case _: hold();                           // wildcard catch-all
};
```

The `match` subject can be a variable, a method call, or a free-function call (`match (poll(x: 5)) { … }`).
Arbitrary-integer branching (not on an enum) is done with `if` / `else if` — there is no `switch`.

## Error model — `Optional` / `Result` ✅

The prelude provides two tagged-union types, so error handling needs no exceptions and no `null`:

- **`Optional<T>`** — `Some(T)` or `None`. A value that may be absent.
- **`Result<T, E>`** — `Ok(T)` or `Err(E)`. A value or an error.

Both are ordinary tagged unions consumed by `match`, so the caller is *forced* to handle the empty/error
case (exhaustiveness):

```cstar
fn Optional<int32> find(List<int32> xs, int32 target) { … }

int32 idx = match (find(xs: list, target: 7)) {
    case Some(i): i;
    case None: -1;
};
```

`Weak<T>.tryUpgrade()` returns `Optional<Shared<T>>`; a fallible `static fn` factory returns `Result<T, E>`
(see Fallible construction above).

## Modules / namespaces ✅

A **module** is a namespaced source unit — a single file *or* a directory of files that all declare the same
`namespace`. `import` names a module by its `::`-path; the compiler finds the source, compiles it, and scopes
its public symbols. There is one keyword for depending on another module — `import` (it replaced `using`).

```cstar
// lib/graphics.cstar          — module `graphics`
namespace graphics;
export { Texture, scale };             // the public surface, at a glance — mirrors `import`

type resource Texture { ... }          // declarations carry NO visibility modifier
fn int32 scale(int32 x) { ... }
type resource GpuHandle { ... }        // unlisted → module-private

// main.cstar
import graphics::{Texture, scale};     // per-symbol, unqualified
import physics as phys;                 // whole-module alias → phys::Body
import audio;                           // load only; qualified-only access audio::Mixer
fn int main() {
    Texture t = ...;                    // imported, bare
    phys::Body b = ...;                 // alias-qualified
    int n = scale(x: 3);
}
```

**Four import forms:** `import a::b;` (load; qualified-only `a::b::X`) · `import a::b as m;` (whole-module
alias → `m::X`) · `import a::b::{X, Y as Z};` (per-symbol into the bare namespace; `as` renames). There is no
glob — unqualified-everything is deliberately not offered. Fully-qualified `a::b::X` is always available once
imported; the symbol list only controls what's *also* unqualified. Two imports binding the same bare name is
a compile error — disambiguate with `as`.

**Visibility — a top-of-file `export { … };` manifest, module-private by default.** A module lists its public
surface in one block at the top; a top-level `type`/`fn` is invisible to other modules unless named there
(C#'s `internal`/`public` model), and a per-symbol import of a non-exported symbol is rejected. A listed name
must be a top-level declaration in that same file — so a directory-module's files each state their own
surface. Declarations carry **no** visibility keyword, keeping `type`/`fn` syntax uniform, and the manifest
reads as the mirror of `import`. Member access (`public`/`protected`/`private`) is a separate axis; the
cstar→host/WASM boundary (`expose`) is a third. A file with **no** `namespace` keeps its symbols file-private
(single-file scripts need no boilerplate).

**Resolution.** `import a::b::c` maps to `a/b/c.cstar` (file-module) or `a/b/c/` (directory-module: every
`*.cstar` in it shares `namespace a::b::c`), searched under (1) the importing file's dir, (2) `$CSTAR_PATH`,
(3) the **stdlib bundled with the compiler** (located relative to the binary like the runtime header, so
`std::*` resolves on any install regardless of cwd). `std`/`core` are reserved roots (stdlib only). Loading is
transitive and deduped by path, so import cycles load once. A namespace already in the compilation (e.g. a
file also on the command line) satisfies an import without a disk lookup. The stdlib is **optional on disk**:
no `import std::…` means the resolver never touches it, and nothing is auto-linked — a `no_std`-like floor
(only `cstar_runtime.h` is mandatory; the prelude `Optional`/`Result`/`Deref`/`HeapOwner` is baked into the
compiler).

Passing several files to one build still works (`cstar build a.cstar b.cstar -o app`); the compiler emits a
shared header (`<out>.gen.h`) + one `.c` per unit — imports just add the resolved module files to that set.

**Scope resolution uses `::`** (namespaces, qualified types, enum variants: `Color::Blue`); `.` is
**instance/value access only** (`obj.field`, `obj.method()`). The two are *syntactically* distinct, so
there's no namespace-vs-object precedence rule — a `::` head is always a type/namespace, a `.` head always a
value. `main` is the global entry point (unmangled).

## Building & debugging ✅

```sh
cstar build app.cstar                 # native debug (-g, breakpoints in .cstar via #line)
cstar build app.cstar --release       # optimized, stripped, NDEBUG
cstar build app.cstar --target wasm   # browser: .html + .js + .wasm
```

Debug builds are breakpoint-debuggable in an IDE (locals + call stack map back to `.cstar`).

## Reserved keywords not yet implemented 🚧

Two keywords are **reserved but not yet implemented** — using either today is a **hard error** (never a
silent no-op), pending its future scope:

- **`volatile`** 🚧 — reserved for the embedded/MMIO scope (ISR↔loop shared flags, peripheral registers);
  implemented when cstar targets embedded.
- **`export`** 🚧 — reserved for the cstar→host boundary (WASM module exports, scripting host interface),
  distinct from in-language `public`/`private`.

## Reserved/runtime

Generated C reserves `__`-prefixed identifiers (`__base`, `__vptr`, `__ret_N`) and `Type__member` mangling.
The runtime ([../cstar_runtime.h](../cstar_runtime.h)) provides `cstar_string` and a
`cstar_trace`/`cstar_trace_get` hook used by tests.
