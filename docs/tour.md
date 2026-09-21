# A tour of kama

The whole language in one read. Each section is a few minutes; every snippet is real code that
compiles today. If you want the exhaustive reference instead, that is [SPEC.md](SPEC.md); if you
want to install the toolchain first, start with [Getting started](GETTING_STARTED.md).

kama looks like C#, manages memory like C++ without the footguns, and compiles to portable C11.
Three ideas carry most of the design: **ownership is written into the type**, **every call names
its arguments**, and **a compiled program ships no runtime** — no garbage collector, no exceptions,
no hidden allocation.

## Hello, exit code

A program is a `main` that returns an `int32`, and that integer is the process exit status.

```kama
fn int32 main()
{
    string name = "world";
    println(s: "hello, ${name}");
    return 0;
}
```

Declarations read *type first, then name* — `fn int32 main()` is a function returning `int32`, and
`string name` is a variable. Every numeric type carries its width — `int8` … `int64`, `uint8` …
`uint64`, `float32`, `float64`, and `isize` for sizes and indices — and `int` on its own is a reserved word, not an alias, so a width is
never implied. The rest are `bool`, `char` and `string`. Comments are `//` and `/* */`.

`${name}` splices a value into the text; it is lowered at compile time, and [Collections and
strings](#collections-and-strings) has the rest of it.

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
| `type enum` | One of a closed set of variants, each able to carry fields | Copies, or moves if a variant holds a resource |
| `type intrinsic` | Gives a built-in type (`int32`, `string`, …) a contract's methods | — |

The first three hold data, `contract` states a guarantee, `enum` is a sum type (see [Enums and
`match`](#enums-and-match)), and `intrinsic` is how the primitives join the same system (see
[Contracts and inheritance](#contracts-and-inheritance)).

```kama
type value Point
{
    public int32 x;
    public int32 y;
    public ctor make(int32 x, int32 y) { this.x = x; this.y = y; }
    public fn int32 sum() { return this.x + this.y; }
}
```

A `value` copies, so it may expose public fields. A `resource` owns something, so its fields are
always private — the point of the kind is that no one outside can break the invariant it protects.

Members are private by default; `public` and `protected` open them up, and a type can name exactly
who else may reach a private member with a `friend` grant (see [Modules and
visibility](#modules-and-visibility)). A method is `public fn <return type> name(...)`, a constructor is
`ctor`, and a destructor is `~Type()` and exists only on a `resource`.

A constructor builds the new value in `this`: it assigns every field (or the field declares a default,
`int32 size = 0;`), and the compiler checks that it did — a field left unset is a compile error, so there is no half-initialised object to observe. <!-- xfail: ctor_unassigned_field -->

## Construction, and construction that can fail

There are no brace literals. A type is built by calling a named constructor on it:

```kama fragment
Point p = Point.make(x: 10, y: 20);
```

Constructors are named because *making* a thing is as meaningful as any other operation — `open`,
`empty`, `withCapacity` and `parse` all say more than an overload set would.

**Dot-on-type is the constructor spelling, and only that.** `::` is scope resolution — static
functions, enum variants, modules — so `Vec2.make(...)` constructs and `Vec2::dot(...)` calls a
static utility. The split is deliberate: `.make(` greps for construction and never catches anything
else. Calling a static function with a dot is a compile error that names the fix. <!-- xfail: dot_on_type_static -->

When construction can fail, the constructor itself returns a `Result` — it is still a `ctor`, just one
with a declared return type. It fails *before* the object exists, so a half-built value never escapes:

```kama
type enum BufferErr implements Error { BadSize; public const fn string message() { return "bad size"; } }

type resource Buffer
{
    int32 size;

    public ctor Result<Buffer, BufferErr> open(int32 size)
    {
        if (size <= 0) { return Result::Err(error: BufferErr::BadSize); }
        this.size = size;
        return Result::Ok(value: this);
    }

    public fn int32 capacity() { return this.size; }
    ~Buffer() { }
}
```

The result is a value you have to unwrap, so there is no way to hold an unopened `Buffer`:

```kama fragment
int32 cap = match (Buffer.open(size: 8)) {
    case Ok(value: b):  b.capacity();
    case Err(error: e): 0;
};
```

`new` is how you reach the heap, and it always produces a smart pointer — `Owned<T>` for a single
owner, `Shared<T>` for reference counting. A `Weak<T>` comes from a `Shared` (`s.downgrade()`) to break a
cycle. There is no raw `malloc` in
the safe language and no `null` anywhere in it.

## RAII: things end where they are written

There is no garbage collector and nothing to call. A resource is destroyed at the closing brace of
the scope that owns it, in reverse order of construction:

```kama
type resource Handle
{
    int32 id;
    public ctor make(int32 id) { this.id = id; }
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
type resource Counter { int32 n; public ctor make(int32 n) { this.n = n; } ~Counter() { } }

fn int32 main()
{
    Owned<Counter> a = new Counter.make(n: 20);
    Owned<Counter> b = give a;      // moved — `a` is dead from here, and the compiler knows it

    Shared<Counter> s = new Counter.make(n: 22);
    Shared<Counter> t = copy s;     // a Shared retains instead of moving; `copy` says so out loud
    return 0;
}
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
    public ctor make(int64 r) { this.r = r; }
    public fn int64 area() { return this.r * this.r; }
}

fn int64 measure(Shape sh) { return sh.area(); }   // dynamic dispatch through a fat pointer
```

Every kind declares conformance the same way, including the two that are easy to forget are kinds. An
`enum` takes the clause inline (`BufferErr` above), and a primitive declares its own through
`type intrinsic` — one block can serve a whole set of widths, which is how the prelude gives `int32` and
`string` their behavioral contracts in kama rather than hard-coding them in the compiler. This is the
prelude's own `Hashable` for the integers:

```kama fragment
type intrinsic <int8, int16, int32, int64, uint8, uint16, uint32, uint64, isize, usize> implements Hashable {
    public const fn uint64 hash() { return cast<uint64>(this); }     // ONE body for ten widths
}
```

A contract method a caller may use on a read-only value is a `const fn`, and the implementation says so
too — see [Constness](#constness).

A contract that mentions its own implementing type declares it as a **pinned parameter** — `type contract
Comparable<T is This>` — and writes `T` in the signature. That is what lets such a contract be held as a
value and not only used as a bound; [the specification](SPEC.md) has the reasoning.

Classic single inheritance with `virtual` / `override` / `final` / `abstract` is there too, for the
cases where an implementation — not just an interface — is the thing being shared. kama keeps
traditional OOP rather than replacing it, with two rules that keep it honest: an extensible type
declares how deep its hierarchy may go, and a derived constructor installs its base explicitly.

```kama
type abstract(maxDepth: 1) resource Codec
{
    int32 tag;
    public ctor make(int32 tag) { this.tag = tag; }
    protected fn int32 getTag() { return this.tag; }
    public fn int32 encode() { return this.body() + 1; }
    protected abstract fn int32 body();                  // every subclass MUST supply it
    ~Codec() { }
}

type final resource Doubler extends Codec
{
    public ctor make(int32 seed) { this.base = Base.make(tag: seed); }
    protected override fn int32 body() { return this.getTag() * 2; }
    ~Doubler() { }
}
```

See [the type model](TYPE_MODEL.md) for which lever to reach for.

## Generics

Generics are monomorphised — each instantiation is a distinct concrete type in the generated C,
with no boxing and no runtime type information.

```kama
type value Pair<A, B>
{
    public A first;
    public B second;
    public ctor make(A a, B b) { this.first = a; this.second = b; }
}

fn T pick<T>(T a, T b, bool useA) { if (useA) { return a; } return b; }
```

Bounds are contracts, and they are nominal — a type has to say `implements Hashable`, a
coincidentally-matching method is not enough: `fn uint64 hashOf<K: Hashable>(K k)`, or
`<K: Hashable + Comparable<K>>` for both — a contract that compares a type with itself names it. Where inference needs help, name the type explicitly with
`pick::<int32>(...)`.

## Modules and visibility

A **module is a folder**. A project's `kama.json` lists its modules, a file belongs to the module its
folder is, and nothing inside a file declares otherwise. A file with no project around it is a one-file
program and needs none of this.

Visibility is per file, and it is written in two lists that mirror each other: `import { … };` says what
the file brings in, one symbol per entry, and `export { … };` says what it lets out. Anything a file does
not export stays in that file.

```kama fragment
import {
    std::collections::DynamicArray,
    geometry::graphics::Texture,
    physics::Body as PhysBody,           // `as` renames; there is no glob import
};
export { Inventory, loadLevel };         // the file's public surface, at a glance
```

Inside a type, members are private unless marked `public` (or `protected`, for a subclass). When one
other type genuinely needs a private member, the owner says so with a **`friend` grant** — and names the
members, not just the friend. Nothing else changes: no member turns public, and no file is reshaped
around the one caller.

```kama
export { Inventory };

type resource Slot
{
    friend Inventory[count, bump];       // exactly these two members, for exactly this one type
    int32 count;
    public ctor make() { this.count = 0; }
    fn void bump() { this.count = this.count + 1; }
    ~Slot() { }
}

type resource Inventory
{
    Slot potions;
    public ctor make() { this.potions = Slot.make(); }
    public fn int32 addPotion() { this.potions.bump(); return this.potions.count; }
    ~Inventory() { }
}

fn int32 main()
{
    Inventory inv = Inventory.make();
    inv.addPotion();
    return inv.addPotion();              // 2
}
```

Any other reach for `count` or `bump` is a compile error, and so is a grant naming a member that does <!-- xfail: friend_nongranted, friend_typo -->
not exist. `friend Inventory[...]` opens every member. A grant crosses generics to the *corresponding*
instance — `friend Tree[root];` in `Node<K, V>` lets `Tree<A, B>` reach `Node<A, B>`, never a sibling
instance — and a grant may name a type in another module by its full path. The rules are in the
[access-control section of the spec](SPEC.md#access-control-).

## Constness

`const` is the read-only marker, and it appears in three places:

```kama
type value Vec2 implements Equatable<This>
{
    public float64 x;
    public float64 y;
    public ctor make(float64 x, float64 y) { this.x = x; this.y = y; }

    public const fn float64 lengthSq() { return this.x * this.x + this.y * this.y; }   // reads only
    public fn void scale(float64 k) { this.x = this.x * k; this.y = this.y * k; }       // mutates
    public const fn bool equals(const ref Vec2 other) { return this.x == other.x && this.y == other.y; }
}

fn float64 measure(const ref Vec2 v)     // borrowed and read-only: no copy, no way to change it
{
    return v.lengthSq();
}

fn int32 main()
{
    const Vec2 unit = Vec2.make(x: 1.0, y: 0.0);
    Vec2 v = Vec2.make(x: 3.0, y: 4.0);
    v.scale(k: 2.0);
    return cast<int32>(measure(v: v) + measure(v: unit));    // 101
}
```

A `const` binding cannot be reassigned or mutated; a `const ref` parameter borrows without copying and
without the right to write; a `const fn` promises not to change `this`. They meet at the call: a const
receiver can only call a `const fn`, and calling anything else on one is a compile error that suggests <!-- xfail: const_ref_nonconst_call -->
the fix. That is also why the prelude's contracts — `Equatable`, `Comparable`, `Hashable`, `Formattable`,
`Error` — declare their methods `const fn`, and an implementation has to match.

## Collections and strings

`std::collections` carries the usual set — `DynamicArray<T>`, `FixedArray<T>`, `Map<K, V>`, `Set<K>`,
`Deque<T>`, `PriorityQueue<T>`, sorted maps and sets. All are bounds-checked, all are RAII, and all take
an optional custom allocator as their last type parameter. The one collection that owns no heap,
`InlineArray<T>#(N)` — `N` elements stored inline, sized at compile time — is in the prelude, needs no
import, and is a `value`.

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
fn int32 main()
{
    int32 folds = 12;
    string name = "kama";
    println(s: "forging ${name} at ${folds} folds");
    return 0;
}
```

Interpolation is lowered at compile time — no reflection, no formatter lookup at runtime. Format
specifiers ride along: `${pi:.2}`, `${n:0x}`, `${n:06}`. A hole holds a name or a field path (`${p.x}`,
`${xs[0]}`), not an arbitrary expression — bind a computed value to a local first. A type of your own
interpolates once it implements `Formattable`, or asks the compiler for it with `@generate(Formattable)`.

A `string` and every collection own heap memory, so a type holding one is a `resource`, not a `value`:
the kind table's first line is enforced, and a `value` with a `string` or collection field is a compile error that <!-- xfail: value_owns_resource -->
says which kind to use instead.

## Serialization, opt-in

Nothing is serializable by default, and there is no runtime reflection. A type opts in with one
attribute, marks every field it wants written, and the compiler generates the code — per type, as
ordinary C:

```kama
import { std::serialization::text::json::serializeJsonBuffer, std::serialization::text::json::deserializeJsonBuffer };

@generate(Serializable, Deserializable)
type resource Player
{
    @field(name: "player_name") string name;
    @field                      int32  level;
    @skip                       int32  frameCounter;            // runtime-only, never written

    public ctor make(string name, int32 level) { this.name = give name; this.level = level; this.frameCounter = 0; }
    public fn int32 levelOf() { return this.level; }
    ~Player() { }
}

fn int32 main()
{
    Result<string, Owned<Error>> written = serializeJsonBuffer(v: Player.make(name: "ada", level: 7));
    string json = match (give written) {
        case Ok(value: j):  give j;                             // {"player_name":"ada","level":7}
        case Err(error: e): "";
    };
    println(s: "wrote ${json}");

    Result<Player, Owned<Error>> back = deserializeJsonBuffer::<Player>(src: give json);
    return match (back) {
        case Ok(value: p):  p.levelOf();                        // 7
        case Err(error: e): 1;
    };
}
```

Every field is marked `@field` or `@skip` — an unmarked one is a compile error, so adding a field <!-- xfail: ser_unmarked_field -->
never silently changes a save format. `@field(id: 3)` gives a field a stable number for formats that
evolve.

The type opts in **once** and works with every back end. JSON is `std::serialization::text::json`; the
binary back ends in `std::serialization::binary::kbin` differ only by how a field is addressed — by name
(self-describing), by number (schema evolution), or by position (smallest). Each has a `…Buffer` pair for
a value in memory and a `…Stream` pair for any `Reader`/`Writer`, so a save file or a socket takes the
same call. An object graph — `Shared<T>` fields, cycles included — round-trips through the same entry
points, rebuilt with its sharing intact. A hand-written `serialize`/`deserialize` always wins over the
generated one. The *Serialization* section of [the specification](SPEC.md) has the wire formats and the
graph rules.

## The standard library, briefly

The prelude is what you get with no import: `println`, `args()`, `envOr`, `Optional`, `Result`, the smart
pointers and the core contracts — the [floor](FLOOR.md). Everything else is a `std::` module you import by
symbol. A short program touching several:

```kama
import { std::fs::File, std::fs::OpenMode, std::io::Lines, std::io::IoError,
         std::time::unixNow, std::time::Date, std::log::logInfo, std::log::logWarn,
         std::process::Command, std::process::Output };

fn int32 main()
{
    Date today = unixNow().date();                              // UTC; a `Timestamp` is the instant
    logInfo(tag: "boot", msg: "started ${today}");               // leveled; filtered by --log / KAMA_LOG

    string path = envOr(name: "NOTES", dflt: "notes.txt");
    foreach (string a in args()) { path = give a; }

    Result<File, IoError> opened = File.open(path: path, mode: OpenMode::Read);
    match (give opened) {
        case Ok(value: f): {
            int32 n = 0;
            foreach (string line in Lines::<File>.make(src: give f)) { n = n + 1; }
            println(s: "${path}: ${n} lines");
        }
        case Err(error: e): { logWarn(tag: "notes", msg: "cannot open ${path}"); }
    };

    Command git = Command.make(program: "git");                 // an argv vector, never a shell string
    git.arg(a: "status");
    Result<Output, IoError> ran = git.run();                     // stdout and stderr captured
    return match (ran) {
        case Ok(value: o):  0;
        case Err(error: e): 1;
    };
}
```

The rest follows the same shape — a `Result` where something can fail, RAII where something is held:
`std::net` (TCP, UDP, IPv4 and IPv6, multicast, WebSockets), `std::math`, `std::random`, `std::digest`,
`std::uuid`, `std::encoding` (base64, hex), `std::path`, `std::fmt` (parsing), and `std::concurrent`
below. Each has its section in [the specification](SPEC.md).

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

```kama fragment
scope {
    spawn writer(s: give sa);
    spawn writer(s: give sb);
}   // both joined here, before anything below runs
```

And `parallel_for` splits a collection into disjoint slices, one per worker, with the closing brace
as the barrier. The worker count is always written — `cpuCount()` from `std::concurrent` is the answer to
"as many as the machine has":

```kama fragment
parallel_for (ref int32 e in xs, workers: cpuCount()) { e = e * 2; }
```

When a channel is the wrong shape, there are exactly three other ways to share: `Atomic<T>` for a
counter or a flag, a `type immutable` that can be read from every isolate because nothing can write it,
and `parallel_spawn` for a fixed pool of workers over one job. The *Concurrency* section of [the
specification](SPEC.md) has all three.

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

A type can take compile-time values as well as types, in a list of their own after the type
parameters: `type value InlineArray<T> comptime(int32 N)`, written at the use site as
`InlineArray<float32>#(4)`. The same mechanism gives `Simd<float32>#(4)`, explicit SIMD that lowers to
the target's vector registers. `@generate(...)` derives the routine members — `Formattable`,
`Equatable`, `Hashable`, the serialization pair — so the compiler writes the code you would otherwise copy.

## Reaching C, and being reached from C

kama compiles *to* C, so interoperating with it is direct rather than a foreign-function bridge.
`extern` declares what C provides, and `expose` hands C a function of yours under a stable symbol —
which is how a game engine loads a hot-reloadable module with `dlsym`:

```kama
extern fn void kama_trace(int32 code);                   // provided by C

unsafe fn void trace(int32 code) { kama_trace(code: code); }

expose fn int32 version() { return 3; }                  // callable from C as `<module>_version`
```

Pointers and raw memory exist, and they are confined to an `unsafe fn` you can grep for — including
every call into C. The marked function is the seam: inside it you are writing C semantics with kama
syntax, and outside it the ownership rules hold. `unsafe` means what C# means by it — it marks the
*body*, so calling such a function is unrestricted and `public unsafe fn` is the ordinary shape.

## Where it runs

The same source builds three ways:

```sh
kama build app.kama -o app                  # native, debug
kama build app.kama --release               # optimised, stripped, dead code pruned
kama build app.kama --target wasm -o app.js # WebAssembly
```

Prebuilt toolchains ship for Linux (x64 and arm64), macOS (one universal binary) and Windows (x64),
and `--target <arch>-<os>-<abi>` cross-compiles to any triple your C toolchain reaches. WebAssembly is a first-class target, threads included. And
`--target embedded` produces bare-metal firmware — interrupt handlers, memory-mapped registers via
the `hardware` qualifier, inline `asm`, and a `@noheap` attribute the compiler enforces.

`kama transpile app.kama -o app.c` writes the generated C if you want to read exactly what your
program became. It is ordinary, portable C11. Its symbols carry a module-qualified prefix
(`k_Fapp__Pair_int32__make`) so they can never collide with C's; the debugger shows kama's names and
values, and `kama demangle` turns any symbol from a crash log back into the kama spelling.

## Tooling

One binary is the whole toolchain — compiler, build system, package manager and language server:

```sh
kama seed mygame              # a project: kama.json, src/app.kama, .gitignore, README
kama run mygame/kama.json     # build and run it
kama check mygame/kama.json   # every error, no C compiler invoked
kama pkg add mygame/kama.json physics --git https://example.com/physics.git
kama lsp                      # the language server your editor starts
kama query mygame/kama.json src/app.kama --refs 12:5    # what the compiler resolved, for scripts and agents
```

The language server gives live diagnostics, hover, go-to-definition, find-references, rename across the
project, completion, signature help, outline, workspace symbols and an auto-import fix — in VS Code,
Neovim, Vim, Emacs, Sublime Text, Helix, Kate and Zed ([editor setup](editors.md)). The VS Code extension
adds F5 debugging with kama names and values. Dependencies are git URLs, paths, archives or a registry,
pinned by a lockfile ([packages](packages.md)); `kama agents install` writes the `AGENTS.md` that tells a
coding agent to ask `kama query` instead of guessing ([AI agents](agents.md)).

## Next

- [Getting started](GETTING_STARTED.md) — install, build, and set up your editor.
- [The specification](SPEC.md) — the complete reference.
- [The type model](TYPE_MODEL.md) — the six kinds and why each exists, in depth.
- [Packages](packages.md) — `kama seed` to start a project, then dependencies and publishing.
- [Editor setup](editors.md) — one language server, eight editors.
