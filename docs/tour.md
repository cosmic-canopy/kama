# A tour of kama

The whole language in one read. Each section is a few minutes; every snippet is real code that
compiles today. If you want the exhaustive reference instead, that is [SPEC.md](SPEC.md); if you
want to install the toolchain first, start with [Getting started](GETTING_STARTED.md).

kama looks like C#, manages memory like C++ without the footguns, and compiles to portable C11.
Three ideas carry most of the design: **ownership is written into the type**, **every call names
its arguments**, and **a compiled program ships no runtime** — no garbage collector, no exceptions,
no hidden allocation.

## Hello, exit code

A program is a `main` that returns an integer, and that integer is the process exit status.

```kama
fn int32 main()
{
    println(s: "hello");
    return 0;
}
```

Declarations read *type first, then name* — `fn int32 main()` is a function returning `int32`, and
`int32 x` is a variable. `int` is an alias for `int32`; the sized names (`int8` … `uint64`,
`float32`, `float64`, `bool`, `char`, `string`) are all spelled out. Comments are `//` and `/* */`.

`println` needs no import. It belongs to the [prelude floor](FLOOR.md) — the surface that is
always in scope, survives `--no-std`, and still works on a microcontroller.

## Named arguments

There are no positional calls. Every argument is named at the call site, and the names may be
given in any order.

```kama
fn int32 sub(int32 a, int32 b)
{
    return a - b;
}

fn int32 main()
{
    return sub(b: 8, a: 50);   // 42
}
```

This is the single most visible thing about kama. It costs a few keystrokes and buys a call site
that reads like the signature — argument order stops being a category of bug, and a diff that
adds a parameter cannot silently shift the meaning of existing calls.

`ref` and `out` are explicit on both sides, so a call that mutates its caller says so:

```kama
fn void bump(ref int32 n) { n = n + 1; }

fn int32 main()
{
    int32 count = 41;
    bump(n: ref count);
    return count;   // 42
}
```

## A type declares what it owns

Every type declaration begins with `type` and a **kind**. The kind is the ownership decision, and
it is the first thing you write:

| Kind | Means | Copies? |
| --- | --- | --- |
| `type value` | Owns nothing but its bytes | Copies freely |
| `type resource` | Owns something — memory, a file, a handle | Moves; runs a destructor |
| `type view` | Borrows someone else's data | Stack-only, cannot be stored | <!-- xfail: view_field -->
| `type contract` | A guarantee other types implement | — |

```kama
type value Point
{
    public int32 x;
    public int32 y;
    public ctor make(int32 x, int32 y) { Point p; p.x = x; p.y = y; return p; }
    public fn int32 sum() { return this.x + this.y; }
}
```

A `value` copies, so it may expose public fields. A `resource` owns something, so its fields are
always private — the point of the kind is that no one outside can break the invariant it protects.

Members are private by default; `public`, `protected` and `friend` open them up. A method is
`public fn <return type> name(...)`, a constructor is `ctor`, and a destructor is `~Type()` and
exists only on a `resource`.

## Construction, and construction that can fail

There are no brace literals. A type is built by calling a named constructor on it:

```kama
Point p = Point.make(x: 10, y: 20);
```

Constructors are named because *making* a thing is as meaningful as any other operation — `open`,
`empty`, `withCapacity` and `parse` all say more than an overload set would.

**Dot-on-type is the constructor spelling, and only that.** `::` is scope resolution — static
functions, enum variants, namespaces — so `Vec2.make(...)` constructs and `Vec2::dot(...)` calls a
static utility. The split is deliberate: `.make(` greps for construction and never catches anything
else. Calling a static function with a dot is a compile error that names the fix. <!-- xfail: dot_on_type_static -->

When construction can fail, the constructor itself returns a `Result` — it is still a `ctor`, just one
with a declared return type. It fails *before* the object exists, so a half-built value never escapes:

```kama
type enum BufferErr implements Error { BadSize; public fn string message() { return "bad size"; } }

type resource Buffer
{
    int32 size;

    public ctor Result<Buffer, BufferErr> open(int32 size)
    {
        if (size <= 0) { return Result::Err(error: BufferErr::BadSize); }
        Buffer b; b.size = size;
        return Result::Ok(value: give b);
    }

    public fn int32 capacity() { return this.size; }
    ~Buffer() { }
}
```

The result is a value you have to unwrap, so there is no way to hold an unopened `Buffer`:

```kama
int32 cap = match (Buffer.open(size: 8)) {
    case Ok(value: b):  b.capacity();
    case Err(error: e): 0;
};
```

`new` is how you reach the heap, and it always produces a smart pointer — `Owned<T>` for a single
owner, `Shared<T>` for reference counting, `Weak<T>` to break a cycle. There is no raw `malloc` in
the safe language and no `null` anywhere in it.

## RAII: things end where they are written

There is no garbage collector and nothing to call. A resource is destroyed at the closing brace of
the scope that owns it, in reverse order of construction:

```kama
type resource Handle
{
    int32 id;
    public ctor make(int32 id) { Handle h; h.id = id; return give h; }
    ~Handle() { int32 n = this.id; println(s: "closing ${n}"); }
}

fn int32 main()
{
    {
        Handle a = Handle.make(id: 1);
        Handle b = Handle.make(id: 2);
    }   // prints "closing 2" then "closing 1" — right here, deterministically
    println(s: "and then this");
    return 0;
}
```

Because a `resource` is move-only, handing one off is explicit: `give` moves it, and using the
source afterwards is a **compile error**, not a crash. <!-- xfail: use_after_move -->

```kama
Owned<Counter> a = new Counter.make(n: 20);
Owned<Counter> b = give a;      // moved — `a` is dead from here, and the compiler knows it

Shared<Counter> s = new Counter.make(n: 22);
Shared<Counter> t = copy s;     // a Shared retains instead of moving; `copy` says so out loud
```

That is the whole memory model. There is no borrow checker and no lifetime annotation: what you
keep, you own, and a borrow (`ref`, or a `view`) is scope-local by construction.

## No exceptions, no null

Fallible operations return a value you have to look at.

```kama
type enum Optional<T> { Some(T value), None }
type enum Result<T, E: Error> { Ok(T value), Err(E error) }
```

Both live in the prelude. `Optional<T>` replaces null; `Result<T, E>` replaces exceptions, and its
error type must implement the `Error` contract — a single `message()` method — so every failure can
describe itself.

```kama
fn int32 unwrapOr(Optional<int32> o, int32 dflt)
{
    return match (o) {
        case Some(value: v): v;
        case None:    dflt;
    };
}
```

## Enums and `match`

An enum variant can carry payload fields, which makes it a full sum type. `match` is the only way
to take one apart, it is exhaustive, and it produces a value:

```kama
type enum Shape { Circle(float64 radius), Rect(float64 w, float64 h), Empty }

fn int32 main()
{
    Shape s = Shape::Rect(w: 3.0, h: 4.0);
    float64 area = match (s) {
        case Circle(radius: r):         3.14 * r * r;
        case Rect(w: width, h: height): width * height;
        case Empty:                     0.0;
    };
    return cast<int32>(area);   // 12
}
```

Arms are `case <pattern>: <expression>;` — a semicolon, not a comma. `case _:` is the wildcard, and
a missing case is a compile error, so adding a variant tells you every place that needs to care. A <!-- xfail: match_nonexhaustive -->
block arm ends with `:= value;` to say what it produces. There is no `switch` and no fallthrough.

**A pattern names the fields it binds**, exactly as a call names its arguments: `h: height` binds the
variant's `h` field to a new local called `height`. There is no positional form. That matters most
when two fields share a type — with positions, writing `case Rect(height, width)` would compile
cleanly and silently hand you the wrong values, which is the whole bug class named arguments exist to
remove. Because the label decides, order does not: `case Rect(h: height, w: width)` means the same
thing. Binding a field that does not exist, binding one twice, or leaving one unbound are all
compile errors.

A variant is *not* a type — you cannot declare a `Rect`, and an enum cannot nest type declarations.
`Rect` is a name inside `Shape`'s scope, which is why it is reached with `::` like any other scope
member, and why supplying its payload produces a `Shape`.

## Contracts and inheritance

A `contract` is an interface. It states which kinds may implement it, which keeps a value-only
guarantee from silently requiring a heap allocation:

```kama
type contract Shape for value, resource { fn int64 area(); }

type value Circle implements Shape
{
    int64 r;
    public ctor make(int64 r) { Circle c; c.r = r; return give c; }
    public fn int64 area() { return r * r; }
}

fn int64 measure(Shape sh) { return sh.area(); }   // dynamic dispatch through a fat pointer
```

Every kind declares conformance the same way, including the two that are easy to forget are kinds. An
`enum` takes the clause inline, and a **primitive** declares its own — one block can serve a whole set of
widths, which is how the standard library gives `int32` and `string` their behavioral contracts in kama
rather than hard-coding them in the compiler:

```kama
type enum BufferErr implements Error { BadSize; public fn string message() { return "bad size"; } }

type intrinsic <int8, int16, int32, int64> implements Comparable<This> {
    public fn Ordering compareTo(ref This other) { … }     // ONE body for four widths
}
```

A contract that mentions its own implementing type declares it as a **pinned parameter** — `type contract
Comparable<T is This>` — and writes `T` in the signature. That is what lets such a contract be held as a
value and not only used as a bound; [the specification](SPEC.md) has the reasoning.

Classic single inheritance with `virtual` / `override` / `final` / `abstract` is there too, for the
cases where an implementation — not just an interface — is the thing being shared. kama keeps
traditional OOP rather than replacing it; see [the type model](TYPE_MODEL.md) for which lever to
reach for.

## Generics

Generics are monomorphised — each instantiation is a distinct concrete type in the generated C,
with no boxing and no runtime type information.

```kama
type value Pair<A, B>
{
    public A first;
    public B second;
    public ctor make(A a, B b) { Pair<A, B> r; r.first = a; r.second = b; return give r; }
}

fn T pick<T>(T a, T b, bool useA) { if (useA) { return a; } return b; }
```

Bounds are contracts, and they are nominal — a type has to say `implements Hashable`, a
coincidentally-matching method is not enough: `fn uint64 hashOf<K: Hashable>(K k)`, or
`<K: Hashable + Comparable>` for both. Where inference needs help, name the type explicitly with
`pick::<int32>(...)`.

## Collections and strings

`std::collections` carries the usual set — `DynamicArray<T>`, `FixedArray<T>`, `InlineArray<T>#(N)`,
`Map<K, V>`, `Set<K>`, `Deque<T>`, `PriorityQueue<T>`, sorted maps and sets. All are bounds-checked,
all are RAII, and all take an optional custom allocator as their last type parameter.

```kama
import { std::collections::DynamicArray };

fn int32 main()
{
    DynamicArray<int32> xs = DynamicArray.empty();
    xs.add(item: 1);
    xs.add(item: 2);

    foreach (ref int32 x in xs) { x = x * 10; }   // `ref` mutates in place; without it you get a copy

    int32 total = 0;
    foreach (int32 x in xs) { total = total + x; }
    return total;   // 30
}
```

`string` is an owned UTF-8 value with the operations you expect (`length`, `concat`, `split`,
`equals`), and interpolation is built into the literal:

```kama
int32 folds = 12;
string name = "kama";
println(s: "forging ${name} at ${folds} folds");
```

Interpolation is lowered at compile time — no reflection, no formatter lookup at runtime. Format
specifiers ride along: `${pi:.2}`, `${n:0x}`, `${n:06}`.

## Concurrency without colour

kama's concurrency is shared-nothing. Work runs in **isolates** — real OS threads natively, Web
Workers on the web — and they communicate over typed **channels** rather than shared memory. A
`value` sent over a channel is copied; a `resource` is moved with `give`, so the sender provably
cannot touch it afterwards. A type of your own crosses only if it declares `implements Sendable`, a
claim the compiler verifies over every field — so `grep Sendable` lists everything that may leave an
isolate, and nothing does so by accident. Primitives and `string` need no declaration.

```kama
import { std::concurrent::Channel, std::concurrent::Sender, std::concurrent::Receiver, std::concurrent::Isolate };

fn void producer(Sender<int32> tx)
{
    int32 i = 1;
    while (i <= 9) { tx.send(item: i); i = i + 1; }
}   // the sender closes as it drops, so the receiver sees the end of the stream

fn int32 main()
{
    Channel<int32> ch = Channel.bounded(capacity: 4);
    Sender<int32>   tx = ch.sender();
    Receiver<int32> rx = ch.receiver();

    Isolate h = spawn producer(tx: give tx);   // moved in — main cannot use `tx` afterwards

    int32 sum = 0;
    bool going = true;
    while (going) {
        match (rx.recv()) {
            case Some(value: x): { sum = sum + x; }
            case None:    { going = false; }
        };
    }
    h.join();
    return sum;   // 45
}
```

A `scope` block gives you structured concurrency — every child spawned inside it is joined at the
closing brace, so no task outlives the code that started it:

```kama
scope {
    spawn writer(s: give sa);
    spawn writer(s: give sb);
}   // both joined here, before anything below runs
```

And `parallel_for` splits a collection into disjoint slices, one per worker, with the closing brace
as the barrier:

```kama
parallel_for (ref int32 e in xs) { e = e * 2; }
```

There is no `async`, no `await`, and therefore no function colouring: an ordinary function is the
only kind of function there is.

## Work the compiler can do for you

`comptime` runs code at compile time and bakes the result into the binary — no startup cost, no
initialisation order to reason about.

```kama
comptime fn int32 fib(int32 n)
{
    int32 a = 0;
    int32 b = 1;
    for (int32 i = 0; i < n; i = i + 1) { int32 t = a + b; a = b; b = t; }
    return a;
}

comptime int32 FIB10 = fib(n: 10);   // 55, computed by the compiler, stored as a constant
```

`@compileFor(FLAG)` drops whole declarations that a build does not need, which is how one source
tree serves native, wasm and bare metal without a preprocessor. See
[conditional compilation](SPEC.md#conditional-compilation--compileforflag-) in the spec.

## Reaching C, and being reached from C

kama compiles *to* C, so interoperating with it is direct rather than a foreign-function bridge:

```kama
extern fn void kama_trace(int32 code);
```

Pointers and raw memory exist, and they are confined to an `unsafe fn` you can grep for. The
marked function is the seam: inside it you are writing C semantics with kama syntax, and outside it the
ownership rules hold. `unsafe` means what C# means by it — it marks the *body*, so calling such a
function is unrestricted and `public unsafe fn` is the ordinary shape.

## Where it runs

The same source builds three ways:

```sh
kama build app.kama -o app                  # native, debug
kama build app.kama --release               # optimised, stripped, dead code pruned
kama build app.kama --target wasm -o app.js # WebAssembly
```

Native covers Linux, macOS and Windows on x64 and arm64, plus cross-compilation to an arbitrary
`<arch>-<os>-<abi>` triple. WebAssembly is a first-class target, threads included. And
`--target embedded` produces bare-metal firmware — interrupt handlers, memory-mapped registers via
the `hardware` qualifier, inline `asm`, and a `@noheap` attribute the compiler enforces.

`kama transpile app.kama -o app.c` prints the generated C if you want to read exactly what your
program became. It is ordinary, portable C11 with your original names intact.

## Next

- [Getting started](GETTING_STARTED.md) — install, build, and set up your editor.
- [The specification](SPEC.md) — the complete reference.
- [The type model](TYPE_MODEL.md) — why `value` / `resource` / `view` / `contract`, in depth.
- [Packages](packages.md) — `kama seed` to start a project, then dependencies and publishing.
- [Editor setup](editors.md) — one language server, eight editors.
