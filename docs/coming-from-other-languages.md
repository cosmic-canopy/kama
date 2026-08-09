# Coming from another language

Where kama's instincts differ from C#, Rust, Python or TypeScript — and *why*. Almost everything here is
kama being **deliberately stricter**, and the strictness is the feature; the exceptions are marked as gaps
and tracked in [ROADMAP.md](ROADMAP.md).

Every "kama way" snippet below is compiled by [`tests/idioms_kama_way.kama`](../tests/idioms_kama_way.kama),
so this page cannot rot silently.

## The one theme worth internalising: bind it to a local

kama's inference works from **bound locals**, not from arbitrary nested expressions. Two rules follow from
that, and hitting either means the same fix — give the intermediate a name:

| You write | kama says | Do this |
| --- | --- | --- |
| `"len=${a.length()}"` | lexical error in the hole | `int32 n = a.length(); "len=${n}"` |
| `showIt(x: Leaf.make(n: 7))` | *"cannot infer generic type parameter"* | `Leaf lf = Leaf.make(n: 7); showIt(x: lf)` |

The naming is not busywork: an interpolation hole stays statically checked rather than becoming a
mini-language (the boundary Rust's `format!` also draws), and a named intermediate is what generic
inference reads.

A `match` **subject** used to be a third row here, and is not one any more: `match (classify(x: 1))` works,
for every call shape and for a plain (payload-less) enum as well as a tagged union. The forms that still
want a bound local are a *nested* value-producing `match` and a *variant-producing ternary* — see
[SPEC.md](SPEC.md) § *Known limitations*.

## A `resource`'s fields are always private

```kama
type resource Counter {
    int32 n;                                  // no `public` — a resource guards its invariant
    public ctor make(int32 n) { this.n = n; }
    public fn int32 value() { return this.n; }
}
```

A `type resource` owns something or has identity ([TYPE_MODEL.md](TYPE_MODEL.md)). A public field would let
a caller reach past the very invariant the type exists to hold, so kama rejects it outright rather than
trusting convention. Expose **behavior**, not state. (A `type value` owns nothing, so its fields may be
public.)

## `out` fills, `ref` borrows

```kama
fn void split(int32 a, int32 b, out int32 q, out int32 r) { q = a / b; r = a % b; }

slot int32 q; slot int32 r;
split(a: 17, b: 5, q: out q, r: out r);       // 3, 2
```

Reaching for `ref` on a `slot` is rejected, correctly: `ref` is a read-write borrow of a value that is
*already live*, and a `slot` has nothing in it yet. `out` is the promise that the callee **assigns on every
path** — which is what lets the caller's definite-assignment analysis count the slot as filled afterwards.
The `out` at the call site is mandatory because both lower to `T*`, and without it no reader could tell a
borrow from a fill.

## A free function's bounds go in its type-parameter list

```kama
fn int32 showIt<W: Shown>(W x) { return x.show(); }
```

Not `fn showIt<W>(W x) when [W: Shown]`. A `when [...]` clause expresses **conditional conformance** on a
generic *type's* method — "this method exists only when `T` is `Copyable`". A free function has no
"sometimes": its bounds are absolute, so they belong in the declaration.

## `@generate` requires every field to be marked

```kama
@generate(Serialize)
type value Point {
    @field int32 x;
    @skip  int32 cachedHash;
    …
}
```

Serde in most languages defaults to "all fields". kama makes you say, so that **adding a field can never
silently start serializing it** — the failure mode where a cache, a token or a password joins your wire
format because someone added a member. Explicit over implicit ([GOALS.md](GOALS.md) #5).

## Named arguments are not friction

```kama
println(s: "hello");
FixedArray.make(size: 4);
split(a: 17, b: 5, q: out q, r: out r);
```

There are no positional calls. This is the headline design decision, not a formality: it is why kama needs
no function overloading, and why a call site reads without jumping to the declaration.

## `.` constructs, `::` resolves scope

```kama
Box.make(v: 10)            // ctor, T inferred from the argument
Box::<int32>.make(v: 10)   // ctor, T explicit — dot after the turbofish
Plain::tag()               // static function on a type
Box::<int32>::tag()        // static function on a GENERIC type — turbofish is mandatory
pick::<int32>(a: 1, b: 2)  // generic FREE function with explicit type args
```

A dot after a type always means construction; `::` always means scope resolution. The two never blur, and
that holds on generic types too. The turbofish is **mandatory** for a generic static: a ctor can infer its
instance from its arguments, but a static has no receiver and its parameters need not mention `T`, so
there is nothing to infer from.

## Other things that surprise

- **`match` is exhaustive and value-producing; there is no `switch`.** Every arm must be covered, and the
  whole construct yields a value.
- **No `null` in the safe surface.** Absence is `Optional<T>`, failure is `Result<T, E>`; `== null` on a
  safe type is a compile error. `null` exists only for `Ptr<T>` at the FFI boundary.
- **No exceptions.** A constructor cannot fail — fallible acquisition is a `static`/`ctor` factory
  returning `Result`.
- **A `string` is UTF-8 bytes.** `length()` is bytes, `s[i]` is a `uint8`, and `.chars()` is the explicit
  opt-in for codepoints; `foreach (char c in s)` is a deliberate type error. Casing and whitespace are
  **ASCII-only** by design (as in Zig) — Unicode-correct casing is a package, not `std`.
- **`substring` traps on an offset that splits a character**, so it can never return an ill-formed
  `string`. Offsets from `find`/`split` are boundary-aligned by construction and cannot trap; for one you
  computed yourself (a byte budget), use `truncate(maxBytes:)`, or snap it with `floorCharBoundary(at:)`
  and slice as usual. Both are total. Coming from Go, where byte slicing silently yields invalid UTF-8,
  this is the difference worth knowing.
- **Integer overflow traps** in debug rather than wrapping; `std::num`'s `wrapping*` helpers are the opt-in.

## If a diagnostic misleads you

It shouldn't. The messages that named a *plausible* cause instead of the actual one have been fixed — the
`match` subject case, and the dot-on-type message that reported an instance method as "a static function"
and then advised a spelling that produced a second, contradictory error. A rejection now says what the
name you wrote actually **is** (a static, a method, a field, or nothing) and gives a spelling that
compiles.

If a message still disagrees with what you observe, trust the observation and file it — that is a bug now,
not a known gap.
