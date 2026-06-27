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
| user `class` | `struct` (value semantics) |

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
M9 limits: `add`/index take elements **by value** (no move yet) — don't separately destruct an added
source; don't return or inline-use a temp-owned string without binding it to a local.

## Smart pointers ✅ (M10) / 🚧

`Owned<T>` — unique heap ownership (= Rust `Box` / C++ `unique_ptr`), zero overhead, **move-only**,
**auto-deref**, RAII-freed. The cstar surface stays pointer-free; the raw pointer is confined to the
runtime. Use it for heap objects, recursive data structures, and (later) polymorphic ownership.

```cstar
Owned<Counter> c = new Owned<Counter>(start: 40);  // heap-allocate + run Counter's ctor
c.bump();  int n = c.get();                          // auto-deref: . reaches the pointee
Owned<Counter> d = c;                                // MOVE: c is now empty (moved-from)
Owned<Node> make(int v) { Owned<Node> n = new Owned<Node>(id: v); return n; }  // factory: moves out
```

Move-only: copying/initializing/returning an `Owned` transfers ownership and invalidates the source, so
the pointee is freed exactly once (RAII, with the pointee's destructor).

`Shared<T>` — ref-counted shared ownership (= C++ `shared_ptr` / Rust `Rc`). **Copyable**: each copy
retains (refcount++), each drop releases, and the pointee is destroyed when the **last** handle goes away.

```cstar
Shared<Tex> a = new Shared<Tex>(id: 7);
Shared<Tex> b = a;     // retain — a and b share one Tex (both valid)
b.use();  int n = a.id;
// a, b drop in RAII order; the Tex is freed exactly once, with the last handle
```

`Weak<T>` — a non-owning weak reference to a `Shared<T>`'s pointee. It does **not** keep the pointee
alive, so it **breaks reference cycles** that `Shared` alone would leak. You can't dereference a `Weak`
(it may be dead) — **upgrade** it with a checked `lock()`:

```cstar
Weak<Tex>  w = s;          // make a weak ref from a Shared (does not keep Tex alive)
Shared<Tex> up = w.lock();  // upgrade -> a valid Shared if the Tex is alive, else empty
if (up.valid()) { up.use(); }       // .valid() = the upgraded Shared is non-empty
bool dead = w.expired();    // true once the last Shared is gone
Weak<Tex>  e;               // default-empty (expired)
```

Smart-pointer limits (whole family): pointee must be a **class** type; **borrow** by passing `ref
Owned<T>`/`ref Shared<T>`/`ref Weak<T>` (passing a smart pointer by value is rejected); `Owned`
**use-after-move** is a runtime null-trap (no borrow checker yet); polymorphic `Smart<Base> = Derived` is
deferred; storing a smart pointer in a collection or a bitwise-copied class field is deferred (aggregate
copy doesn't retain/move). `Map<K,V>` + multi-param/nested generics are 🚧.

## Functions ✅

```cstar
int add(int a, int b) { return a + b; }
int main() { return add(b: 20, a: 10); }   // named args; reordered to declared order
```
`ref`/`out` parameters pass by pointer: `void set(out int dst) { dst = 42; }` … `set(dst: ref x);`.
`extern Ret name(params);` declares a function provided by C (no body emitted) — the FFI seam. ✅

## Control flow ✅

`if/else`, `while`, `do/while`, `for`, `switch` (C#-style, no fall-through), `break`, `continue`,
`return`; full operator set (`+ - * / %`, bitwise, shifts, comparisons, `&& || !`, ternary `?:`),
assignment ops (`= += …`), `++`/`--`, casts.

## Classes ✅

```cstar
class Counter {
    int value;
    Counter(int start) { value = start; }   // constructor
    void add(int n) { value = value + n; }   // method (implicit self)
    int get() { return value; }
}
Counter c = new Counter(start: 40);   // constructs in place
c.add(n: 2);
```
Fields, methods (take an implicit `self`), one constructor, field initializers (run in the ctor),
`this.field`, `obj.method(args)`. Lowers to a `struct` + `Counter__method(Counter* self, …)` functions.

## RAII / destructors ✅

A `~Type()` destructor runs deterministically at scope exit, in reverse construction order, on every
path (block end, early `return`, `break`/`continue`). Destructible fields are destroyed in reverse
declaration order. No GC; allocation/deallocation is predictable.

## Inheritance & virtual dispatch ✅ (M6)

```cstar
class Shape { int describe() { return this.area(); }  virtual int area() { return 0; } }
class Circle extends Shape { override int area() { return 42; } }
```
Single inheritance (`extends`), base embedded by value (upcast is offset-0), base ctor via `: base(...)`,
`base.m()` for non-virtual upcalls. `virtual`/`override` methods dispatch through a vtable. `interface`/
`implements` are 🚧 M6b.

## Enums ✅

```cstar
enum Color { Red, Green = 5, Blue }   // Red=0, Green=5, Blue=6
Color c = Color.Blue;
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
class Texture { ... }        // Graphics.Texture
int32 scale(int32 x) { ... } // Graphics.scale

// main.cstar
using Graphics;              // import unqualified
using Phys = Physics;        // alias
int main() {
    Texture t = ...;             // Graphics.Texture (via using)
    Physics.Texture p = ...;     // qualified — distinct type, no collision
    int n = Graphics.scale(x: 3);// qualified namespaced call
}
```

`namespace a.b;` (dotted) is allowed. Object/field names shadow a namespace in `A.B` resolution. `main` is
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
