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

## FFI — calling C ✅ (M15)

`extern Ret name(params);` declares a C function; cstar emits its prototype and lowers calls to it. Link
libraries with `--link`. The FFI boundary is the language's only "unsafe" seam (explicitly `extern`):

```cstar
extern "<stdlib.h>";             // every C function comes from an explicit header
extern "<math.h>";
extern Ptr  malloc(usize n);     // Ptr = void* (opaque pointer/handle); usize = size_t
extern void free(Ptr p);
extern float64 sqrt(float64 x);  // build with: --link m

int main() {
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
extern class div_t { int32 quot; int32 rem; }   // bind an external C struct (not re-emitted)
extern div_t div(int32 numer, int32 denom);

extern float64 frexp(float64 value, Ptr<int32> exp);

int main() {
    div_t r = div(numer: 17, denom: 5);    // r.quot=3, r.rem=2  (field access on a C struct)
    int32 e = 0;
    frexp(value: 1764.0, exp: addr(of: e));// addr(of: x) = &x  — controlled out-param
    return r.quot + r.rem + e;             // 5 + 11 = 16
}
```

`extern class Foo { ... }` is an **external** struct provided by an included header / linked code — cstar
uses its fields but never re-emits it (so no redefinition), and its name is the literal C name.
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

### Callbacks — `funcptr(of: fn)` (M18)

Hand a cstar **free function** to a C API as a function pointer (GPU adapter/device/buffer-map callbacks,
input handlers). A *controlled* op (it names a real function) — no `unsafe`.

```cstar
extern "<webgpu.h>";
extern void wgpuBufferMapAsync(WGPUBuffer b, ..., WGPUBufferMapCallback cb, Ptr userdata);
void onMapped(int32 status, Ptr userdata) { ... }   // signature must match the C callback type
...
wgpuBufferMapAsync(b: buf, ..., cb: funcptr(of: onMapped), userdata: null);
```

`funcptr(of: fn)` lowers to the function's C name (which decays to a function pointer); the callback's
parameter type comes from the included header, and your cstar function's emitted signature must match it
(the same ABI contract as `extern` structs). Free functions only — a method would need an implicit
receiver; C callbacks pass state via a `userdata`/`Ptr` argument instead. (Note: libc callbacks declared
with `const` pointers — e.g. `qsort`'s `int(const void*, const void*)` — aren't expressible yet, since
cstar has no `const`; headers whose callback typedefs match a cstar signature, like WebGPU's, work
directly.) 🚧 next: math types + operator overloading → cstar-level WebGPU bindings.

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
