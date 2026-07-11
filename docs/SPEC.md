# kama language specification (overview)

This is a semantics overview. The **grammar is authoritative** — see
[grammar.bnf](grammar.bnf) (generated from `kama.y`). Executable examples live in
[`../tests/`](../tests/) (`*.kama` with a `.expect` exit code). The design philosophy — *one way to do a
thing, explicit over implicit, no GC / RAII* — lives in [../GOALS.md](../GOALS.md); this document is the
semantics/feature reference. Status flags below: ✅ implemented, 🚧 reserved (not yet implemented).

## Model

kama compiles to **portable C** (native + WASM). No garbage collector — object lifetimes are deterministic
(RAII). Calls use **named arguments** (no positional). Every type declaration is `type value` (owns nothing,
copies), `type resource` (owns/has identity, moves, RAII-dropped), or `type contract` (an interface).

## Types ✅

| kama | C |
|---|---|
| `int8 int16 int32/int int64` | `int8_t … int64_t` |
| `uint8 uint16 uint32 uint64` | `uint8_t … uint64_t` |
| `float32` / `float64`/`double` | `float` / `double` |
| `bool` | `bool` |
| `string` | `kama_string` (borrowed view or heap-owned RAII string) |
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

```kama
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

```kama
string s = "A\u{E9}\u{20AC}";           // "Aé€" — 6 UTF-8 bytes, 3 codepoints
int n = 0;
foreach (char c in s.chars()) { n = n + 1; }   // n == 3 (codepoints, not bytes)
uint8 first = s[0];                     // 65 ('A'), a byte
```

**`char`** is a distinct primitive — a Unicode scalar value backed by `uint32` (not a numeric type, so it
can't silently mix with ints). Literals: `'a'`, `'\n'`, `'\u{1F600}'`. Equality + ordering compare
codepoints; `cast<int32>(c)` / `cast<char>(i)` convert (arithmetic on codepoints is explicit, the Rust
model). A multibyte *source* char literal is decoded to its scalar value: `'é'` == `'\u{E9}'` == 233,
`'😀'` == `'\u{1F600}'` == 128512.

**Operators & methods.** `string` carries the small, always-available ergonomic surface every language
ships — all compiler intrinsics on the primitive (no import), byte-oriented like `s[i]`:

- **`+` / `==` / `!=`** — `a + b` concatenates (a fresh heap-owned string), `a == b` / `a != b` compare
  bytes. The compiler special-cases string operands (not a user overload — `string` is a primitive); both
  operands must be `string` (`string + <number>` is a compile error — a to-string/`Display` substrate is
  future work). Chains and compose: `a + b + c`, `s.trim() == "x"`.
- **slice** — `substring(start:, end:)` copies the byte range `[start, end)` into an owned string
  (bounds-checked; a **byte** range, not codepoint-validated — use `.chars()` for codepoints).
- **search** — `find(substring:)` returns `Optional<usize>` (the first byte offset, `None` when absent —
  null-safe, no `-1` sentinel); `contains(substring:)`, `startsWith(prefix:)`, `endsWith(suffix:)` return
  `bool`; `isEmpty()`.
- **transform** (each returns a fresh owned string) — `trim()` / `trimStart()` / `trimEnd()` strip **ASCII**
  whitespace; `replace(old:, with:)` swaps all non-overlapping occurrences; `toLower()` / `toUpper()` map
  **ASCII** case (bytes ≥ 0x80 untouched — UTF-8-safe). Unicode whitespace/casing are deferred to a later
  Unicode module.
- **`split(separator:)`** — a lazy `Split` iterator (`implements Iterator<string>`, like `.chars()`),
  yielding each piece as an owned string with no collections import: `foreach (string p in s.split(separator:
  ","))`. Go `strings.Split` semantics (consecutive/trailing separators yield `""`; an empty separator
  yields the whole string once). Collect into a `List<string>` explicitly if you need random access.

```kama
string path = "/usr/local/bin";
foreach (string part in path.split(separator: "/")) { … }   // "", "usr", "local", "bin"
string greet = "Hello, " + name + "!";
if (greet.toLower().contains(substring: "hello")) { … }
match (greet.find(substring: ",")) { case Some(i): …; case None: …; }
```

## Collections & strings ✅

Built-in generics, monomorphized per element type and backed by the C runtime (unsafe internals, safe API —
the Rust-`Vec` model); **indexing is bounds-checked** (a clean trap, not UB). An indexed element `a[i]` is
a **place** (an lvalue): you can write a field through it (`a[i].x = v`), index it again
(`m[i][j] = v`), compound-assign it (`a[i] += x`), or borrow it (`ref a[i]`) — every form stays
bounds-checked. (Reading `a[i]` still yields a copy.)

```kama
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

`List` / `Array` also offer `reserve(n:)` (List — preallocate to skip incremental growth), `remove(index:)`
(List — drop the element and shift the tail), `clear()` (List), and `contains(item:)` / `indexOf(item:)`
(both — present **only when the element is `Equatable`**, i.e. `string` or a user type with `equals`).

### Hash maps & sets (`std::collections`) ✅

`Map<K, V>` (open-addressing, linear-probing, tombstoned, grows at 0.75 load) and `Set<K>` (a thin wrapper
over `Map<K, Unit>`), over a key `K: Hashable + Equatable`. These two contracts are in the **prelude**:

```kama
type contract Hashable  for both { fn uint64 hash(); }
type contract Equatable for both { fn bool equals(This other); }
```

Keys are hashed and compared **by content**, so a lookup key built any way (a concat, a fresh
construction) finds the stored entry. `string` and every **integer width** satisfy both out of the box, via
**pure-kama** `implements` blocks that ship in the **prelude** (universal — no `std::collections` import;
the retroactive-conformance mechanism, *no* compiler blessing): `string` gets FNV-1a `Hashable` + a
`Equatable` recorded nominally from its built-in `equals`; every integer gets a splitmix64 `Hashable` +
scalar `equals` (a conformance on a **primitive** — `this` is the scalar itself). Floats get `Equatable`
only (exact `==`) — intentionally not hash-keyable. A **user key** declares `implements Hashable, Equatable`
and provides the two methods. Bounds are **nominal**: the `implements` is required (a coincidental `equals`
is not enough), the same rule as `foreach`.

```kama
import std::collections::{Map, Set};

Map<string, int32> counts = Map();
counts.put(key: "a", value: 1);
counts.put(key: "a", value: 2);                        // overwrite (drops the old value)
int32 v = match (counts.get(key: "a")) { case Some(x): x; case None: 0; };   // 2
counts.remove(key: "a");   bool has = counts.contains(key: "b");   int32 n = counts.length();

Set<string> seen = Set();
seen.add(key: "x");   bool member = seen.contains(key: "x");
```

`foreach (K k in map)` / `foreach (K k in set)` iterates the keys (a by-value key iterator, present for a
`Copyable` key; it guards against a mid-iteration `put`/`remove` like the `List` iterator). `copy m`
deep-copies a whole map (independent clone) — present only when **both** key and value are `Copyable`,
gated by a **multi-condition `when [K: Copyable, V: Copyable]`**. Entry-wise iteration
(`foreach (Entry e in m.entries())`) is a tracked follow-up (a generic `Entry<K,V>` value yielded through
`Optional` doesn't monomorphize yet).

`Map` is **move-only**: it owns its keys and values (dropping them on overwrite, `remove`, `clear`, and at
end of life — ASan/UBSan-clean for owning keys *and* values, e.g. `Map<string, List<string>>`). Lookups
**borrow** the key (`ref K`), so they don't consume a key you're holding; `get(key:)` returns
`Optional<V>` with a **deep copy** of the value (present only when `V` is `Copyable`). A key that is an
inline rvalue — a `string`/number literal or a user-type ctor — is materialized into a temp automatically,
so `m.get(key: 5)` / `m.get(key: Point(1, 2))` work without binding a local first.

## Smart pointers ✅

The smart pointers are a **standard library**, not compiler intrinsics: `Owned`/`Shared`/`Weak` live in
`std::memory`, written in ordinary kama (RAII `resource`s over `Deref`/`HeapOwner`, refcounting in kama),
and are pulled in with `import std::memory::{…}`. The compiler adds only what a library can't express: the
type-erasure (fat pointer + vtable) that makes `Owned<Shape>`/`Shared<Shape>` over a **contract** work, and
`new T(args)` heap placement into any `HeapOwner<T>`.

```kama
import std::memory::{Owned, Shared, Weak};
```

`Owned<T>` — unique heap ownership (= Rust `Box` / C++ `unique_ptr`), zero overhead, **move-only**,
**auto-deref**, RAII-freed. The kama surface stays pointer-free; the raw pointer is confined to the library.
Use it for heap objects, recursive data structures, and polymorphic ownership.

```kama
Owned<Counter> c = new Counter(start: 40);          // `new` heap-boxes the ELEMENT type
c.bump();  int n = c.get();                          // auto-deref: . reaches the pointee
Owned<Counter> d = c;                                // MOVE: c is now empty (moved-from)
fn Owned<Node> make(int v) { Owned<Node> n = new Node(id: v); return n; }  // factory: moves out
```

Move-only: copying/initializing/returning an `Owned` transfers ownership and invalidates the source, so the
pointee is freed exactly once (RAII, with the pointee's destructor).

**Ownership hand-off — `give` / `copy`.** When a *named* owned value is handed off — in an initializer,
assignment, argument, or return — an explicit marker states the intent, uniformly in all four positions:
**`give`** moves (invalidates the source), **`copy`** retains (`Shared`/`Weak`) or duplicates. **Every owning
kind is movable**; what varies is whether it is *also* `Copyable` and what a *bare* hand-off defaults to.
`Owned` is move-only — a bare hand-off moves, and `copy Owned` is an error (it's unique). `Shared`/`Weak`
are `Copyable` and declare `bare: copy`, so a bare hand-off **retains** (refcount++); `copy` is the explicit
retain, and **`give` still moves the handle** — the ref transfers and the source is consumed (this is how a
`Shared` returns from a factory without a spurious retain/drop). A `value`/primitive just copies. A fresh
`new`/constructor/call result needs no marker. Smart pointers also **pass by value**: the callee *owns* the
argument and drops it at function end — `fn int use(Owned<T> p)` consumes it (`use(p: give x)`), `fn int
peek(Shared<T> s)` retains it (`peek(s: x)`, `x` stays valid).

```kama
Owned<Counter> b = give a;   // explicit move (a consumed)
Shared<Counter> t = s;       // copy/retain (default) — both valid
Shared<Counter> u = copy s;  // explicit retain (same as bare); `give s` moves the handle (s consumed)
```

*(`copy` of a collection is a deep copy — a fresh buffer, element-wise: a bitwise-copyable element is copied
memberwise, a `Copyable`-resource element is deep-copied via its own `copy()`. A resource element that is not
`Copyable` is rejected. `give` of a collection **moves** the buffer.)*

**Move-only `resource` values + the `Copyable` contract.** A **`type resource`** value (it owns something, or
has identity) is **move-only**: a bare named hand-off *moves* (the source is consumed, its destructor
suppressed), so its heap is freed exactly once — a silent copy is never emitted (that would double-free).
`give` is optional emphasis; `copy` is an error unless the type opts in. A `resource` **opts into copy**
**nominally** — `implements Copyable(bare: …)` (the prelude contract `Copyable { fn This copy(); }`) plus a
**public nullary `copy()`** method (a lone `copy()` method without the `implements` does *not* make a type
copyable). Opting in **requires declaring the bare-hand-off default**: `Copyable(bare: give)` (a bare hand-off
moves) or `Copyable(bare: copy)` (a bare hand-off deep-copies via `copy()`). A marker (**`give x`** / **`copy
x`**) always overrides the default; there is no "ambiguous — must annotate" error. Because `copy`/`give` are
markers only in expression position, they're **contextual keywords** — usable as method names, so the opt-in
method is literally named `copy`.

```kama
type resource Res implements Copyable(bare: copy) {   // a bare hand-off deep-copies
    List<int32> items;
    ~Res() { }
    public fn Res copy() { Res r = Res(v: this.items[0]); return give r; }   // the Copyable method
}
Res b = copy a;   // deep copy — a stays valid, b has its own buffer
Res c = give b;   // move — b consumed
Res d = a;        // bare — follows the declared default (here: deep-copy)
```

**Auto-deref — the `Deref<T>` contract.** The standard smart pointers forward member access to their
pointee (`ptr.method()`/`ptr.field` reach the held `T`). Any type can opt into the same **auto-deref** by
implementing the prelude contract `type contract Deref<T> { fn ref T deref(); }` — the `implements` is the
explicit gate. When a member isn't found on the wrapper itself, it resolves on the pointee `T` and is
called through `deref()` (which returns a *place* — a `T*` — into the pointee); resolution is recursive, so
deref chains. `deref()` returns a place rooted at `this`, so it's bound by the same second-class-borrow
rules as `ref T operator[]` (no lifetimes needed). This is how a smart pointer is written as an ordinary
`resource` (RAII + move-only come free) rather than a compiler intrinsic.

```kama
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

**The give/copy behavior matrix.** **Every owning kind is movable**; a bare hand-off follows the kind's
default (a `Copyable` type must declare it with `bare:`), and an explicit marker overrides. The rule is
uniform across all four hand-off positions — **initializer, assignment, argument, return** — and a *fresh*
rvalue (`new`/constructor/call result) never takes a marker.

| kind | bare hand-off | `give` | `copy` |
|---|---|---|---|
| primitive / `value` | **copy** (cheap) | copy (a value's move is a copy) | copy (redundant, allowed) |
| `Owned<T>` (unique) | **move** | move (emphasis) | ⛔ "is unique" |
| `Shared<T>` (ref-counted) | **retain** (strong++) | **move** (transfer the handle) | retain (explicit) |
| `Weak<T>` (weak ref) | **retain** (weak++) | **move** (transfer the handle) | retain (explicit) |
| collection (`Array`/`List`/`string`) | ⛔ marker required | **move** (buffer) | **deep copy** (fresh buffer) |
| plain `resource` (move-only value) | **move** | move (emphasis) | ⛔ "opt into `Copyable`" |
| `Copyable` resource (has `copy()`) | its declared `bare:` default | move | **deep copy** via `copy()` |
| collection of `Copyable` elements | ⛔ marker required | move | **deep copy** (element-wise `copy()`) |

A marker on a fresh rvalue is an error. Move tracking is compile-time: reading a moved value, moving out of a
field/element, moving inside a loop a value declared outside it, and a conditional move that is still live at
scope exit are all rejected — there is no runtime drop flag.

`Shared<T>` — ref-counted shared ownership (= C++ `shared_ptr` / Rust `Rc`). **Copyable**: each copy retains
(refcount++), each drop releases, and the pointee is destroyed when the **last** handle goes away.

```kama
Shared<Tex> a = new Tex(id: 7);
Shared<Tex> b = a;     // retain — a and b share one Tex (both valid)
b.use();  int n = a.id;
// a, b drop in RAII order; the Tex is freed exactly once, with the last handle
```

`Weak<T>` — a non-owning weak reference to a `Shared<T>`'s pointee. It does **not** keep the pointee alive, so
it **breaks reference cycles** that `Shared` alone would leak. You can't dereference a `Weak` (it may be
dead) — **upgrade** it with the checked `tryUpgrade()`, which returns an `Optional<Shared<T>>` you must
`match` on, so the dead case is impossible to ignore:

```kama
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

```kama
fn int add(int a, int b) { return a + b; }
fn int main() { return add(b: 20, a: 10); }   // named args; reordered to declared order
```
`ref`/`out` parameters pass by pointer: `fn void set(out int dst) { dst = 42; }` … `set(dst: ref x);`.

## FFI — calling C ✅

`extern fn Ret name(params);` declares a C function's call signature (name + named params for lowering); the
C **prototype comes from the header** you `extern "<header.h>";` — kama never emits a prototype for an
extern function (so there's no redeclaration conflict, and a missing include is a plain C error). Link
libraries with `--link`. The FFI boundary is the language's only "unsafe" seam (explicitly `extern`):

```kama
extern "<stdlib.h>";             // every C function comes from an explicit header
extern "<math.h>";
extern fn Ptr  malloc(usize n);     // Ptr = void* (opaque pointer/handle); usize = size_t
extern fn void free(Ptr p);
extern fn float64 sqrt(float64 x);  // libm auto-links when a program `extern "<math.h>";`s (pay-for-use)

fn int main() {
    Ptr p = malloc(n: 64);
    if (p == null) { return 1; }  // hold / null-check / compare — but no deref yet
    free(p: p);
    return cast<int>(sqrt(x: 1764.0));   // 42
}
```

**The FFI rule (one sentence): declare C types/functions by `extern`-including their header.** An `extern`
declaration is purely kama's call-signature (name + named params, so it can lower the call) — the actual C
prototype comes from the header you include with `extern "<header.h>";`. kama never emits a C prototype for
an extern function, so there are no redeclaration conflicts; and the runtime hides its own libc dependencies
(block-scope declarations), so **no** C function (not even `malloc`) is available without its header — a
missing include is a plain C error, never a silent guess.

`Ptr` is `void*`; `Ptr<T>` is `T*` — an **opaque carrier** (hold, pass to/from C, `null`-check, compare;
**no dereference** in kama outside `unsafe`). `usize`/`isize` map to `size_t`/`ptrdiff_t`. Names beginning
`kama_` are reserved (runtime-provided).

### Math (`std::math`) ✅

Engine Tier-0 linear algebra — concrete **float32** value types: `Vec2/3/4`, `Mat2/3/4`, `Quat`, plus scalar
helpers (`radians`/`degrees`/`lerp`/`clampf`/`pi()`/… over libm). `import std::math::{Vec3, Mat4, Quat, …}`.
Methods + operators (one `operator*` per type: matrices/quaternions **compose**, vector transform / rotate
are named methods — no overloading). Matrices are **column-major** with the **column-vector** convention
(`result = M * v`, GPU/WebGPU-native); `perspective`/`orthographic`/`lookAt` target **WebGPU 0..1 depth**,
right-handed. `Quat` is a unit quaternion (`fromAxisAngle`/`fromEuler`, Hamilton `*`, `rotate`, `slerp`/
`nlerp`, `toMat3`/`toMat4`). All literals are `f32`-suffixed (a bare `1.0` is float64). SIMD is a later
implementation swap behind this API (the layout is SIMD-ready).

### Numbers (`std::num`) ✅

Numeric type **limits** as zero-arg functions — `int8Min/Max` … `int64Min/Max`, `uint8Max` … `uint64Max`,
`float32Max`/`float32MinNormal`/`float32Epsilon` (signed min is `-max - 1`) — and per-width integer
**operations** `minI32/maxI32/clampI32/absI32/signI32` (+ the `I64` set), parallel to `std::math`'s float32
`minf`/`maxf`/…, and explicit **wrapping** arithmetic `wrappingAddI32`/`wrappingSubI32`/`wrappingMulI32`/
`wrappingNegI32` (+ `I64`) for intentional overflow. `import std::num::{int32Max, minI32, wrappingAddI32,
…}`. (A generic `min<T: Comparable>` waits on a `Comparable`/`Ordering` contract, which lands with the
sorted containers.)

**No undefined behavior in arithmetic** (Rust's model). Every integer operation is *defined* — never C's
UB:
- **Signed overflow** (`+`/`-`/`*`) **traps** in debug builds (catches the accidental-overflow bug during
  development) and **wraps** two's-complement in release (`-fwrapv`, zero-cost, *defined* — not UB).
  Intentional signed wrapping is opt-in: `std::num`'s `wrappingAddI32`/`wrappingSubI32`/`wrappingMulI32`/
  `wrappingNegI32` (+ the `I64` set) always wrap and never trap (computed in the unsigned type), or just use
  unsigned math directly. **Unsigned overflow always wraps** (as C already defines).
- **Divide by zero** and **`INT_MIN / -1`** **trap** (a clean abort) in every build — always bugs, never UB.
- **Shift ≥ the type width** **traps**; a **signed left shift into the sign bit** (`1 << 31`) is **defined**
  (computed in the unsigned type — a defined bit pattern), so bit-twiddling is safe.
- **Out-of-range `float → int`** **traps**; in-range truncates toward zero. Integer narrowing wraps mod 2ⁿ.

Enforced by `-fsanitize-trap` (a bare `__builtin_trap`, no sanitizer-runtime dependency) + `-fwrapv` +
the `kama_lshift` runtime shim — so a kama program can't hit arithmetic UB whether built debug or release.

```kama
import std::math::{Vec3, Mat4};
fn int main() {
    Mat4 vp = Mat4::perspective(fovyRad: 1.0472f32, aspect: 1.777f32, near: 0.1f32, far: 100.0f32)
            * Mat4::lookAt(eye: Vec3(x: 0.0f32, y: 2.0f32, z: 5.0f32),
                           center: Vec3::zero(), up: Vec3::unitY());   // method chaining
    Vec3 p = vp.transformPoint(p: Vec3(x: 1.0f32, y: 0.0f32, z: 0.0f32));
    return cast<int>(p.length());
}
```

### Standard I/O (`std::io` / `std::fs` / `std::net`) ✅

A native, single-binary I/O foundation — **library over FFI, no new language surface** beyond the prelude's
`enum Unit` (the empty `Result<Unit, E>` payload — one error convention for void-fallible ops). `std::io`
gives `IoError` + error classification; `std::fs` gives a RAII `File` (fd closed by its destructor) plus free
`readFile`/`writeFile`/`stat`/`readDir`/`remove`; `std::net` gives RAII `TcpListener`/`TcpStream` (blocking
TCP). All fallible calls return `Result<…, IoError>`, consumed by `match`.

```kama
import std::fs::{readFile, writeFile};
fn int main() {
    match writeFile(path: "out.txt", data: "hi") {
        case Ok: {}
        case Err(e): { return 1; }
    }
    return 0;
}
```

Every platform difference lives in one bundled C bindings header, **`kama_os.h`** (pulled in only when a
module `extern "kama_os.h";`s it — pay-for-what-you-use), which keeps OS aggregates (`struct stat`,
`sockaddr_in`, `dirent`) opaque behind `static inline` accessors — the standard FFI boundary (Rust `libc` /
Zig `@cImport`), forced by "an `extern` struct emits the literal C name." POSIX (Linux/macOS/iOS/Android) and
Windows (Winsock + CRT) both ship; under wasm the virtual FS works, sockets need a host proxy. Sockets link
`-lws2_32` on Windows (pay-for-use, like `-lm` for `<math.h>`). `examples/httpd/` is a ~200-line static-file
HTTP server built on these three modules.

**FFI data — all controlled, no `unsafe` needed:**

```kama
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
kama uses its fields (all public, the C layout) but never re-emits it (so no redefinition), and its name is
the literal C name. `addr(of: x)` takes the address of a real local (out-params, descriptor pointers) — a
*controlled* op, no `unsafe`. `s.cstr()` yields a C `const char*`.

### `unsafe { }` — raw pointer memory access

The **only** place kama can touch arbitrary memory through a raw pointer. Raw `Ptr<T>` index/store is a
**compile error outside** an `unsafe { }` block — so the entire dangerous surface is explicit and greppable
(`grep -rn 'unsafe {'`). Everything else (collections, smart pointers, FFI structs/handles/out-params,
`addr`) stays safe.

```kama
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
(including a safety-gate violation) is a **hard build error** — kama never emits incomplete C and claims
success.

### Writing a collection *in* kama — `sizeof`, `panic`/`assert`, place-returning methods ✅

The above pieces (a place-returning `operator[]`, `Ptr<T>` + `unsafe`, generics, RAII) let a `Vec`/matrix
be written **in the language** rather than baked into the compiler. Three builtins complete the kit:

- **`sizeof(T)`** — the compile-time byte size of a type (a `usize`); monomorphizes, so
  `malloc(n: n * sizeof(T))` works in a generic `Vec<T>`.
- **`panic(msg: string)` / `assert(cond: bool)`** — a clean **trap** (writes the message + `abort()`, not
  UB — the user-facing form of the built-in bounds trap). For a *bug that can't continue*; recoverable
  errors use `Result<T, E>`. (kama aborts on panic — no stack unwinding; ≈ Rust's `panic=abort`.)
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
The guard lives in `List`'s own kama source (not the compiler), so it's a stdlib policy: a hand-rolled
container chooses whether to pay for it. `Array`/`Fixed` are fixed-size and can't reallocate, so they
need no guard. (The iterator's back-pointer to the counter uses the `addr(of: place)` builtin — the
address of a place as a `Ptr<T>`; safe to take, `unsafe` to deref.)

### Function pointers — `fnptr` ✅

kama has no naked function pointers. **`fnptr`** declares an explicit, named function-pointer **type**
(independent of any user type) — **zero-cost** (a bare C function pointer, no wrapper). It is **non-null**
(must be bound; no `null`, no null-check at the call), and binding a free function is **signature-checked**.
(A bodiless `fn` is *not* a function pointer — a forgotten body is a clear error, never a silent type.)

```kama
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

```kama
type value Vec2 { public int32 x; public int32 y; fn int32 dot(ref Vec2 o) { return this.x*o.x + this.y*o.y; } }
fnptr int32 DotFn(ref Vec2 self, ref Vec2 o);   // receiver is an explicit first param

DotFn d = Vec2::dot;            // unbound (`::` = no instance, no binding) — zero-cost
int32 n = d(self: ref u, o: ref v);
```

**`BindableFunctionPtr<Sig>`** — a callable that *captures* a receiver so you don't pass it each call. Unlike
the zero-cost `fnptr`, it carries an object (opt-in cost) and is **RAII-managed**. It's constructed like any
other object, and **the ownership model follows the pointer type you hand in** — no separate keyword:

```kama
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
signature is one kama's `fnptr` doesn't spell identically — most commonly `const`-qualified parameters —
name the callback via a header `typedef` and **cast** to it at the edge:

```kama
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
  fields, no ctor/dtor. Types satisfy it via `implements`; it may refine another with `implements` too
  (`type contract Animated for both implements Drawable { … }` — a conformer must supply Drawable's methods
  as well, and dispatch through `Animated` reaches them).

The full model + rationale is in [TYPE_MODEL.md](TYPE_MODEL.md). The kind words `value` / `resource` /
`contract` are **contextual, not reserved** — because they appear only right after `type`, they remain
ordinary identifiers everywhere else (`int32 value = 5;`). Only `type` is a keyword.

```kama
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

```kama
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

kama has no exceptions, so **constructors are infallible** — trivial, in-place field setup that cannot fail.
Fallible resource acquisition is a **`static fn` factory returning `Result<T, E>`**: the fallible work lives
in the factory, and on failure it returns `Err` *before* the resource exists, so no half-constructed object
can escape and `match` forces the caller to handle the error.

```kama
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

```kama
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

**Owning a derived through a base handle (upcast).** A `Shared`/`Owned` over a derived class widens to one
over a base class (or a contract it satisfies) — the IS-A relationship, Liskov-style:

```kama
Shared<Circle> c = new Circle();
Shared<Shape>  s = c;          // upcast — retain (both handles share one Circle)
Owned<Circle>  u = new Circle();
Owned<Shape>   o = give u;     // upcast — move (u consumed)
int a = s.describe();          // polymorphic: describe() calls the protected virtual area() -> Circle's
```

Polymorphism flows through the base's **public surface**, which invokes the `protected virtual` hooks
(Template Method) — you never call an overridable method through the handle directly. Destruction is
**virtual**: a `virtual`/`abstract resource` (and every owning contract handle) carries a vtable `__dtor`
slot, so dropping through a base/contract handle runs the **most-derived** destructor's full chain — the
derived's owned resources are freed, never sliced, exactly once. (An upcast only ever yields an *owning*
handle or a scope-local borrow; an un-owned contract value can't be stored — see Contracts.)

## Contracts ✅

A **`contract`** is a public-only guarantee — "some type satisfying this contract." It has methods only: no
bodies, no fields, no ctor/dtor.

```kama
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

```kama
type resource Holder { Shared<Shape> shape;  public fn int64 area() { return this.shape.area(); } }
fn Owned<Shape> make(int64 s) { Owned<Shape> o = new Square(s: s); return give o; }
```

A `List<Shared<Shape>>` (the engine's scene) works — polymorphic elements stored and dropped in RAII order.

### Retroactive conformance — `implements C for T` ✅

A type can be given a contract **after the fact**, from outside its declaration — including a **primitive**
(`string`, …) or a type from another module — with a top-level `implements C for T { … }` block. The methods
lower exactly like ordinary methods on `T` (mangled `T__method`), so they dispatch with zero overhead through
a generic bound `<K: C>`; there is **no method overloading and no "extension method" call-syntax** — the block
adds real conformance, not sugar. This is how std gives primitives their behavioral contracts *in kama* (e.g.
`Hashable`/`Equatable` for `string`, so `Map<string, V>` keys hash) rather than hard-coding them in the
compiler.

```kama
type contract Hashable for both { fn uint64 hash(); }

implements Hashable for string {                    // a primitive gains a contract, in pure kama
    public fn uint64 hash() {
        uint64 h = 2166136261ui64;                  // FNV-1a
        int32 i = 0;
        while (i < cast<int32>(this.length())) { h = (h ^ cast<uint64>(this[i])) * 16777619ui64; i = i + 1; }
        return h;
    }
}
fn uint64 hashOf<K: Hashable>(K k) { return k.hash(); }   // `string` now satisfies the bound
```

**Coherence (orphan rule).** A retroactive impl is permitted only when the compilation declares **either** the
contract **or** the target type — so third parties can't give conflicting conformances. kama's whole-program
view makes this a direct duplicate check (a conflicting or duplicated impl is a compile error), which is *also*
the future package-manager guard. A retroactively-conformed contract dispatches **statically** (through
generic bounds); it does not add a fat-pointer interface vtable. A **scalar primitive** target (`int32`)
works too: its `this` is the value itself, so a method takes `T self` by value and the call is a plain
`int32_t__hash(k)` — this is how `Map<int32, V>` / `Set<int32>` get their keys. (Other primitive widths are
one-line std `implements` blocks, added on demand.)

## Static methods & operator overloading ✅

**Static methods** — a `static fn` has **no implicit `self`** and is called at the type level with named
args:

```kama
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
language (so a `Vec`/matrix can be written *in* kama). The place is a **second-class borrow** of
`self`: it is used transiently and cannot be stored (there is no `ref`-local/`ref`-field to hold it),
and a `const` receiver makes it read-only. Bounds safety is the operator's responsibility — a
`Fixed`/collection-backed body is auto-checked; a raw `Ptr<T>` body is `unsafe`. The same place-return
works for a **named method** — `public fn ref T at(usize i) { … }` — so `v.at(i) = x` too. (A `ref T`
return is supported on methods/operators; free-function `ref T` returns are not yet.)

Used in a `contract`, an operator becomes a **bound** for generic math (see below).

## Generics ✅

User-defined generics, **monomorphized** (one specialized copy per concrete type — elements inline, no
boxing; identical layout and cost to the built-in collections).

```kama
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
  ```kama
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
  ```kama
  type contract Arithmetic { This operator+(This rhs); }
  fn T sum<T: Arithmetic>(T a, T b) { return a + b; }   // `a + b` -> static Concrete__op_add(&a, b)
  ```
  The concrete type declares `implements Arithmetic` (bounds are nominal), and `a + b` in the monomorphized
  body lowers to a direct call — no vtable, no boxing.
- **Generic contracts** — a `contract` may itself be parameterized (`type contract Iterator<T>`), and is
  **monomorphized per use** just like a generic type (`Iterator<int32>` → a specialized `Iterator_int32`).
  It has **full value + bound parity** with a plain contract: usable as a static bound
  `fn sum<I: Iterator<int32>>(I it)` (zero-cost, direct calls) *and* as a dynamic fat-pointer value
  `fn drain(Iterator<int32> it)` (vtable dispatch). A type opts in with `implements Iterator<int32>`.
  ```kama
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

```kama
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

```kama
int32 area = match (sh) {                     // expression position — yields a value
    case Circle(r): cast<int32>(r * r * 3);
    case Rect(w, h): cast<int32>(w * h);
};

match (color) {                               // statement position — a plain enum works too
    case Red: fire();
    case _: hold();                           // wildcard catch-all
};
```

An arm is either a **single expression** (`case X: <expr>;`) or a **block** (`case X: { … }`). A block
arm names the value it produces with a **`:= <expr>;`** statement, which must be the block's **final**
statement (single-exit) — it accepts any expression, and reads as "bind this value out" (a `match` in a
typed position *is* an assignment from the outside, `x = match … { … := v; }`). `:=` is distinct from
`return`, which leaves the enclosing function:

```kama
string label = match (reading) {
    case Some(c): {
        string name = "mild";
        if (c < 0)  { name = "freezing"; }
        if (c > 30) { name = "hot"; }
        := name;                              // the arm's value (must be last)
    }
    case None: "unknown";
};
```

The `match` subject can be a variable, a method call, a static-method call, or a free-function call
(`match (File::open(path: p, mode: OpenMode::Read)) { … }`). Arbitrary-integer branching (not on an enum)
is done with `if` / `else if` — there is no `switch`.

## Error model — `Optional` / `Result` ✅

The prelude provides two tagged-union types, so error handling needs no exceptions and no `null`:

- **`Optional<T>`** — `Some(T)` or `None`. A value that may be absent.
- **`Result<T, E>`** — `Ok(T)` or `Err(E)`. A value or an error.
- **`Unit`** — a single-variant enum (`Unit::Unit`), the empty value. It is the payload for a fallible
  operation that succeeds with nothing to return: `Result<Unit, E>` (the analogue of Rust's `Result<(), E>`),
  so **one** error convention — always `Result` — covers valued and void operations alike, with no
  placeholder payload. Example: `fn Result<Unit, IoError> remove(ref string path)` in `std::fs`.

Both are ordinary tagged unions consumed by `match`, so the caller is *forced* to handle the empty/error
case (exhaustiveness):

```kama
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

```kama
// lib/graphics.kama          — module `graphics`
namespace graphics;
export { Texture, scale };             // the public surface, at a glance — mirrors `import`

type resource Texture { ... }          // declarations carry NO visibility modifier
fn int32 scale(int32 x) { ... }
type resource GpuHandle { ... }        // unlisted → module-private

// main.kama
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
kama→host/WASM boundary (`expose`) is a third. A file with **no** `namespace` keeps its symbols file-private
(single-file scripts need no boilerplate).

**Resolution.** `import a::b::c` maps to `a/b/c.kama` (file-module) or `a/b/c/` (directory-module: every
`*.kama` in it shares `namespace a::b::c`), searched under (1) the importing file's dir, (2) `$KAMA_PATH`,
(3) the **stdlib bundled with the compiler** (located relative to the binary like the runtime header, so
`std::*` resolves on any install regardless of cwd). `std`/`core` are reserved roots (stdlib only). Loading is
transitive and deduped by path, so import cycles load once. A namespace already in the compilation (e.g. a
file also on the command line) satisfies an import without a disk lookup. The stdlib is **optional on disk**:
no `import std::…` means the resolver never touches it, and nothing is auto-linked — a `no_std`-like floor
(only `kama_runtime.h` is mandatory; the prelude `Optional`/`Result`/`Deref`/`HeapOwner` is baked into the
compiler).

Passing several files to one build still works (`kama build a.kama b.kama -o app`); the compiler emits a
shared header (`<out>.gen.h`) + one `.c` per unit — imports just add the resolved module files to that set.

**Scope resolution uses `::`** (namespaces, qualified types, enum variants: `Color::Blue`); `.` is
**instance/value access only** (`obj.field`, `obj.method()`). The two are *syntactically* distinct, so
there's no namespace-vs-object precedence rule — a `::` head is always a type/namespace, a `.` head always a
value. `main` is the global entry point (unmangled).

## Serialization — `@`-attributes + `@generate` ✅

Opt-in, compile-time-reflected serialization: the only new *language* surface is the attribute mark; the
serializers ship as library modules. **`@generate(Serialize, Deserialize)`** on a type (per-direction opt-in)
synthesizes the two methods **as kama** and merges them into the type — **format-agnostic**, driving the
abstract prelude contracts (`Serializer` / `Serialize` / `Deserializer` / `Deserialize` / `DeError`), so a new
backend is a module with no compiler change.

- **Per-field marks are mandatory** on a `@generate`d type: each field is `@field`, `@field(name: "wire")`
  (rename), or `@skip` — an unmarked field is a **compile error** (no silent omission).
- **Coverage:** scalars, `string`, nested `@generate` types, `Optional<T>`, and `List<T>`/`Array<E>` (containers
  are indexed via `operator[]`, so a `List<resource>` works — no `Copyable`/by-value-`foreach` requirement).
- **Deserialize** uses a **bypass-constructor** model: the compiler zero-initializes the struct (an internal
  `ZeroValueNode` → `(T){0}`, no user grammar) and populates fields **in place**, returning `T` directly — this
  expresses a nested resource the all-args-constructor model couldn't.
- **`onConstruction()`** — an opt-in lifecycle hook run on *every* construction (compiler-injected at ctor-end
  **and** by the deserialize codegen after field-set, since deserialize bypasses the ctor; it may be private). A
  `@generate(Deserialize)` type must define it or opt out with **`@generate(Deserialize, noOnConstruction)`**.
- **Smart-pointer fields** (`Owned`/`Shared`/`Weak`) are **rejected** as `@field` — `@skip` them and serialize
  an id, reconnecting in `onConstruction` (the object-graph story is a forward item; see ROADMAP §4).
- **Backend:** `std::fmt` (number→string) + `std::serialization::json` (`JsonWriter`/`JsonReader`); entry points
  `json::toString(v:)` and `json::tryParse::<T>(src:)` (the latter wraps `Result<T, DeError>`).

```kama
import std::serialization::json::{toString, tryParse};

@generate(Serialize, Deserialize)
type value Widget {
    @field int32 x;
    @skip  int32 born;                        // not serialized; set by the hook
    public Widget(int32 x) { this.x = x; }
    fn void onConstruction() { this.born = 7; }   // runs at ctor-end AND after deserialize field-set
}

Widget a = Widget(x: 5);                        // born = 7 (ctor-end injection)
string j  = toString(v: a);                     // {"x":5}   (born is @skip)
Result<Widget, DeError> r = tryParse::<Widget>(src: give j);   // born = 7 again on the deserialized value
```

## Building & debugging ✅

```sh
kama build app.kama                 # native debug (-g, breakpoints in .kama via #line)
kama build app.kama --release       # optimized, stripped, NDEBUG
kama build app.kama --target wasm   # browser: .html + .js + .wasm
```

Debug builds are breakpoint-debuggable in an IDE (locals + call stack map back to `.kama`).

## Reserved keywords not yet implemented 🚧

Two keywords are **reserved but not yet implemented** — using either today is a **hard error** (never a
silent no-op), pending its future scope:

- **`volatile`** 🚧 — reserved for the embedded/MMIO scope (ISR↔loop shared flags, peripheral registers);
  implemented when kama targets embedded.
- **`expose`** 🚧 — reserved for the kama→host boundary (WASM module exports, scripting host interface),
  distinct from in-language `public`/`private` (member access) and `export` (the module public-surface
  manifest — `export { … };`, which ships today).

## Known limitations (tracked → [ROADMAP.md](ROADMAP.md) §1)

A few ownership-lowering edge cases are open at 1.0. Each **hard-errors** (never miscompiles) and has a
clean workaround:

- **Inline `new` in a non-local position** — `f(a: new Sq(…))` (a call argument) or a `Shared/Owned<I>`
  **return** (`fn Shared<Shape> g() { return new Sq(…) }`). Boxing works in a `Type x = new …` initializer;
  elsewhere bind it to a local first. (Being addressed.)
- **`ref T` return from a free function** — supported on methods/operators; a free `fn ref T f(ref …)` is
  not yet. Return an owned value, or use a method. (Being addressed.)
- **A value-producing construct as a `match` SUBJECT** — `match (Optional::Some(…)) { … }` (a bare variant
  ctor / value-producing match / variant-producing ternary as the subject) needs generic-instance inference
  the subject position doesn't provide; bind the subject to a typed local first.

(Target-typed inline construction now works in initializers, `return`, `operator[]` place-stores,
value-producing `match` arms, class-typed lvalue stores, call arguments, and string-rvalue indexing.)

## Reserved/runtime

Generated C reserves `__`-prefixed identifiers (`__base`, `__vptr`, `__ret_N`) and `Type__member` mangling.
The runtime ([../kama_runtime.h](../kama_runtime.h)) provides `kama_string` and a
`kama_trace`/`kama_trace_get` hook used by tests.
