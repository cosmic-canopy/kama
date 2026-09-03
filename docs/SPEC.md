# kama language specification (overview)

This is a semantics overview. The **grammar is authoritative** — see
[grammar.bnf](grammar.bnf) (generated from `kama.y`). Executable examples live in
[`../tests/`](../tests/) (`*.kama` with a `.expect` exit code). The design philosophy — *one way to do a
thing, explicit over implicit, no GC / RAII* — lives in [GOALS.md](GOALS.md); this document is the
semantics/feature reference. Status flags below: ✅ implemented, 🚧 reserved (not yet implemented).

## Model

kama compiles to **portable C** (native + WASM). No garbage collector — object lifetimes are deterministic
(RAII). Calls use **named arguments** (no positional). Every type declaration is `type value` (owns nothing,
copies), `type resource` (owns/has identity, moves, RAII-dropped), `type view` (a non-owning stack-only
borrow — a slice/span), or `type contract` (an interface).

## Types ✅

| kama | C |
|---|---|
| `int8 int16 int32 int64` | `int8_t … int64_t` |
| `uint8 uint16 uint32 uint64` | `uint8_t … uint64_t` |
| `isize` / `usize` | `ptrdiff_t` / `size_t` — the ONLY platform-varying types (8 bytes on x86_64/arm64, 4 on wasm32/thumbv6m) |
| `float32` / `float64` | `float` / `double` |
| `bool` | `bool` |
| `string` | `kama_string` (borrowed view or heap-owned RAII string) |
| `void` | `void` |
| user `type value`/`type resource` | `struct` (value semantics) |

No raw arrays and **no raw pointers — by design** (raw memory access is confined to `unsafe fn` at the FFI
boundary). Collections are generic library types.

### Numeric literals

| form | example | notes |
|---|---|---|
| decimal / hex / octal integer | `42`, `0xFF`, `0o17` | no digit separators — `1_000` is not a literal |
| based integer | `0b1010_2` | `0b<digits>_<base>`, base 2–32; the `_<base>` is required |
| integer suffix | `42i32`, `42ui32` | `u?i(8\|16\|32\|64)` — unsigned is **`ui`**; there is no bare `42u32` |
| float | `12.5`, `12.5e10`, `1e10`, `1.5e-3` | `digits.digits` with an optional exponent, **or** `digits` with a required one |
| float suffix | `1.5f32`, `1e10f64` | `f32` / `f64` |

**A float literal's dot needs digits on both sides** — `1.` and `.5` are rejected
(`tests/xfail/float_bare_dot.kama`); those are the error-prone spellings, `.5` reading as a stray member
access and `1.` as an unfinished expression. A **dotless exponent is fine** (`1e10`), because the exponent
marker already makes it unmistakably a float. This is exactly Swift's and Zig's rule. There is **no
normalization requirement**: `12.5e10` and `0.5e10` are both literals, not just `1.25e11`.

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
bool eq = s.equals(other: t);   isize len = s.length();
```

You never spell the borrowed-vs-owned distinction; the type carries it, and RAII frees exactly the owned
ones. There is no separate capital-`String` collection type.

**UTF-8 everywhere.** A `string` is **UTF-8 bytes**; `length()` is the **byte** length (O(1)), and literals
encode `\u{…}` escapes to UTF-8. Two ways to traverse it, kept distinct by type so bytes and characters
never blur:

- **bytes** — `s[i]` returns the i-th byte as a **`uint8`** (bounds-checked); `foreach (uint8 b in s)`
  iterates bytes. `foreach (char c in s)` is a type error — the byte/codepoint distinction is enforced. <!-- xfail: foreach_char_over_string -->
  That is one case of a general rule: a `foreach` binding must have the type the collection actually
  yields, and a mismatch is rejected rather than left to C's implicit conversions <!-- xfail: foreach_elem_type_mismatch -->
  (`tests/xfail/foreach_char_over_string`, `tests/xfail/foreach_elem_type_mismatch`).
- **codepoints** — `s.chars()` is a **UTF-8 codepoint iterator** (`implements Iterator<char>`):
  `foreach (char c in s.chars())` yields each Unicode scalar value as a **`char`**. It's a borrow, valid
  while the string is.

```kama
string s = "A\u{E9}\u{20AC}";           // "Aé€" — 6 UTF-8 bytes, 3 codepoints
int32 n = 0;
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
  operands must be `string` (`string + <number>` is a compile error — **string interpolation** is the one way <!-- xfail: string_plus_number -->
  to mix values into text; see below). An interpolation counts as a `string` operand (`name == "hi ${x}"`).
  Chains and compose: `a + b + c`, `s.trim() == "x"`.
- **slice** — `substring(start:, end:)` copies the byte range `[start, end)` into an owned string. A
  **byte** range (use `.chars()` for codepoints), and **total with respect to the UTF-8 invariant**: it
  traps on an out-of-range offset *and* on one that would split a character, so it can never hand back an
  ill-formed `string`. Offsets that come from a search (`find`, `split`) are boundary-aligned by
  construction and cannot trap; an offset computed by *arithmetic* is the case to guard.
- **boundary + budget** — `floorCharBoundary(at:)` returns the greatest character boundary `<= at`
  (unchanged when `at` already is one, clamped to `length()` past the end). **Total** — never traps, O(1).
  It is what makes an arithmetic offset safe at either end of a range:
  `s.substring(start: 0, end: s.floorCharBoundary(at: 80))`. `truncate(maxBytes:)` names the common case
  on top of it — at most `maxBytes` bytes, never splitting — for a wire field, a column limit or a log cap.
  Both guarantee **valid UTF-8, not visually intact text**: a cut at a codepoint boundary can still split a
  grapheme cluster (an `e` + combining accent, an emoji ZWJ sequence, a flag). Segmentation is defined by
  UAX #29, needs Unicode tables, and stays a package concern — see [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §2.
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
  yields the whole string once). Collect into a `DynamicArray<string>` explicitly if you need random access.

```kama
string path = "/usr/local/bin";
foreach (string part in path.split(separator: "/")) { … }   // "", "usr", "local", "bin"
string greet = "Hello, " + name + "!";
if (greet.toLower().contains(substring: "hello")) { … }
match (greet.find(substring: ",")) { case Some(value: i): …; case None: …; }
```

### Formatting & string interpolation ✅

Rendering a value as text goes through **one** contract, `Format` (prelude, so it works without an import and
survives `--no-std`) — the display twin of `Serialize`:

```kama
type contract Format for value, resource, intrinsic { fn void format(ref Formatter f); }
```

A type writes its pieces into a caller-owned **`Formatter`** sink (a growable UTF-8 buffer), so a whole nested
value materializes in **one** allocation — no O(n²) concat. `Formatter` has `writeStr` / `writeI64` /
`writeU64` / `writeF64` / `writeF32` / `writeBool` / `writeChar` and a `finish() -> string`. Every primitive
(`int8`..`uint64`, `float32/64`, `bool`, `string`) conforms. **`"${x}"` is the one way to render a value** —
it lowers to exactly this build, so there is no free wrapper beside it. `Format` is **infallible** (`void`,
no `Result`) — an in-memory write can't fail, unlike `serialize` over an I/O sink.

```kama
type value Point implements Format {
    int32 x; int32 y;
    public fn void format(ref Formatter f) {
        f.writeStr(s: "("); f.writeI64(v: cast<int64>(this.x));
        f.writeStr(s: ", "); f.writeI64(v: cast<int64>(this.y)); f.writeStr(s: ")");
    }
}
```

**Interpolation.** A `${expr}` hole in a plain string literal splices a value in — lowered at **compile time**
to a `Formatter` build (a `writeStr` per literal chunk, `expr.format(ref f)` per hole), statically type-checked,
**no runtime reflection**:

```kama
string s = "point ${p} at n=${n}, first=${who[0]}";   // p.format, n.format, who[0].format into one buffer
```

- **Holes are restricted** to an identifier with `.field` / `[index]` accessors (`${user.name}`, `${items[i]}`).
  Anything with an operator or call must be bound first (`let sum = a + b; "…${sum}"`) — logic stays out of
  string literals (Rust RFC 2795's restraint).
- **Escape** a literal `${` as `\${`; a lone `$` (not before `{`) stays literal.
- **Verbatim** strings never interpolate — `@"raw ${x}"` is literal (the raw escape hatch).
- A **`char`-typed hole** renders as its character (`${c}` → the glyph) via a `writeChar` fast-path — the
  compiler detects `char` from the hole's type node (`char` shares `uint32`'s C type, so it can't hold a
  `Format` conformance directly).
- **Format specifiers** `${expr:spec}` render a numeric hole in a chosen form — the vocabulary **mirrors
  Kama's numeric literals** rather than printf. Precision is `.N` (`${pi:.2}` → `3.14`) — no printf type-letter,
  since the hole's type is already known; it requires a **float** hole. Base reuses the literal prefixes
  `0x`/`0o`/`0b`, and *the leading `0` you type is echoed*, so `${n:x}` → `ff` (bare) while `${n:0x}` → `0xff`
  (prefixed — itself a valid Kama literal); the letter's case controls digit case (`${n:0X}` → `0XFF`). Base
  requires an **integer** hole and shows the unsigned bit pattern of its declared width, so a signed negative
  round-trips (`${x:0x}` on `-1i8` → `0xff`). A spec on a user-type hole, or a kind mismatch (`.N` on an int,
  `0x` on a float), is a compile error — the `Format` contract stays spec-less (specs are `Formatter` <!-- xfail: interp_spec_user_type, interp_spec_kind_mismatch -->
  fast-paths). A **minimum field width** right-aligns: `${n:6}` space-pads, `${n:06}` zero-pads (the sign
  stays ahead of the zeros), and on a float it composes with a precision (`${pi:8.2}`, `${pi:08.2}`) — ideal
  for zero-padded columns (`${h:02}:${m:02}`). A leading **`+`** forces a sign on non-negatives (`${n:+}` →
  `+42`) and **`-`** left-aligns within the width (`${n:-6}`); both compose with the width/precision. The full
  spec grammar is `[+|-]* [0? width] [.precision] | base`. Combining a base marker with width/flags, a custom
  fill character, and center-align are not yet supported (see [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §2).
- **`@generate(Format)`** synthesizes a default field-dump `Format` impl so a type renders without a
  hand-written `format` — `Type { field1: v1, field2: v2 }`, each field dispatching to its own `Format` into
  the same sink (so nesting composes, one allocation):

  ```kama
  @generate(Format) type value Stat { public int32 hp; public bool alive; }
  string s = "${Stat.of(hp: 30i32, alive: true)}";   // "Stat { hp: 30, alive: true }"
  ```

  Strings render **raw/unquoted** (uniform single-contract dispatch — no special-case). A hand-written
  `format` wins over the derive; `@skip` omits a field. Every non-skipped field must itself be a
  primitive/string or a type that `implements Format` (an `Optional`/collection/enum-typed field, or a
  generic/variant/enum carrying the attribute, is a clear compile error — see [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §2).
  It is named after the **contract** (`Format`), not `display`/`debug` — Kama has one to-string contract, no
  Display/Debug split; a `${x:?}`-routed structural `Debug` derive stays a possible additive future.
- **Tagged strings** ✅ — an identifier placed **immediately** before a string (`html"…"`, `sql"…"`,
  `stripIndent"…"`; no space) makes it *tagged*. The compiler splits the string into its trusted literal
  **parts** and its rendered **holes** (each hole run through `Format`) and hands them to a function
  `fn R name(ref Template t)`, which decides how they combine — so a tag is just a function you can define:

  ```kama
  fn string html(ref Template t) { /* literals verbatim, holes HTML-escaped — XSS-safe */ }

  string page = html"<b>${user}</b>";            // ${user} escaped, <b> kept raw
  SqlQuery  q = sql"… WHERE id = ${id}";          // holes become `?` params, out-of-band (injection-safe)
  string    s = stripIndent"…";                   // the literal template is dedented; hole values verbatim
  ```

  Because parts and holes stay **separate**, a tag treats literals (trusted) and holes (values) differently:
  `html` escapes holes but not literals, `sql` never splices a hole into the query text (values go to a
  params array), `stripIndent` dedents only the template. `Template` (a prelude type) exposes `partCount()`
  / `holeCount()` / `part(at:)` / `hole(at:)`; `stripIndent`, `html`, `sql` (+`SqlQuery`) live in `std::fmt`.
  Format specifiers compose inside a tag (`sql"…${amt:.2}"`). An unknown tag (no matching `fn` in scope) is a
  compile error. *(Type-preserved params — each hole keeping its static type into the params list rather than
  a rendered `string` — is a compatible future extension; see [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §2.)*

## Collections & strings ✅

Built-in generics, monomorphized per element type and backed by the C runtime (unsafe internals, safe API —
the Rust-`Vec` model); **indexing is bounds-checked** (a clean trap, not UB). An indexed element `a[i]` is
a **place** (an lvalue): you can write a field through it (`a[i].x = v`), index it again
(`m[i][j] = v`), compound-assign it (`a[i] += x`), or borrow it (`ref a[i]`) — every form stays
bounds-checked. (Reading `a[i]` still yields a copy.)

```kama
FixedArray<int32> a = FixedArray.make(size: 4);   // fixed buffer, zero-initialized
a[0] = 10;  a[1] = 20;                          // bounds-checked []
int32 first = a[0];
foreach (int32 x in a) { /* ... */ }            // iterate (x is a copy)
foreach (ref int32 x in a) { x = x * 2; }       // `ref`: mutate each element in place

DynamicArray<Point> ps = DynamicArray.empty();             // growable
ps.add(item: p);   isize n = ps.length();   Point q = ps[0];

string s = "ab";                                // borrowed literal (no alloc)
string t = s.concat(other: "cd");               // heap-owned, RAII-freed
```

All collections own their storage and free it via RAII (with element-destructor chaining). Only the
`(collection, element-type)` pairs the program actually uses are emitted (pay-for-what-you-use). A **method
call on an element** works directly — `list[i].method()` borrows the element *in place*, so a mutating method
mutates the stored element; a `const` collection allows only const methods on its elements. Elements enter a
collection by the ownership rules below (`give` to move, `copy` to duplicate, a `value` copies).

`DynamicArray` / `FixedArray` also offer `reserve(n:)` (DynamicArray — preallocate to skip incremental growth),
`clear()` (DynamicArray), and `contains(item:)` / `indexOf(item:)` (both — present **only when the element is
`Equatable`**, i.e. `string` or a user type with `equals`). Removal on `DynamicArray` **returns the removed
element moved out** — `remove(index:) -> T` (shifts the tail; bounds-checked, so always a valid `T`) and
`pop() -> Optional<T>` (the last element, O(1), `None` when empty). Reclaim the element by binding the return,
or **discard it to drop** (`xs.remove(index: i);` drops cleanly). This is the one consistent removal shape
across every container — a single method that hands ownership back (matching `Deque.popFront`/`popBack` and
Rust's `Vec::remove`/`pop`), never a silent drop.

### Slices / spans — `View<T>` ✅

A **`View<T>`** is a non-owning window over a contiguous run of `T` — a slice / span (zero copy, no
ownership transfer). It is a **`type view`** (the stack-only-borrow kind; see *Type declarations*): the
escape check keeps it from being stored, and **the window rule below keeps it from outliving its buffer**,
so it cannot dangle — without a borrow checker, and without lifetimes.

```kama
DynamicArray<float32> verts = DynamicArray.empty();  // … fill …
borrow verts.view() as all {                            // a WINDOW — `verts` is frozen inside it
    View<float32> mid = all.slice(from: 2, count: 4);   // a sub-range [2, 6) — a derive, no new window
    foreach (ref float32 x in mid) { x = x * 2.0; }     // mutate-through: writes back to `verts`
    isize n = mid.length();   float32 first = mid[0];   // bounds-checked index (a place)
}
uploadToGpu(window: verts.slice(from: 0, count: 3));    // pass a subrange down — no copy, no window needed
verts.add(item: 1.0);                                   // mutable again: the window has closed
```

#### The window — `borrow h.mint() as v { … }` ✅

A view borrows storage it does not own, so kama answers *how long is this view valid?* with **lexical
scope** — not a lifetime annotation, and not a programmer's promise. A **`borrow` block is that extent**,
and it is purely lexical: one C block plus one initializer per binding, no runtime cost.

```kama
borrow d.view() as v { … }        // `Viewable<View<T>>` grants `view()`
borrow m.values() as vals { … }   // `ValuesIterable<I>` grants `values()` — `Map` has no `view()`
borrow buf.span() as s { … }      // a user contract's own grant
```

Three rules, and together they are what lets the paragraph above say "cannot dangle":

1. **The host is a MINT CALL**, and the window is over the call's *receiver*. The mint is any **nullary
   member of a `@viewable` contract** the receiver's type implements — the grant names its own member, so
   nothing is hardcoded. A container alone would not say which view to open (`DynamicArray` grants three),
   and a mint takes no arguments: narrow by deriving off the alias with `.slice(…)` inside the window,
   which is free.
2. **The host place is FROZEN for the extent of the block** — no assignment, no non-`const` `ref`/`out`
   argument, no `give` out of it, no non-`const fn` receiver overlapping it, and the alias itself may not
   be reseated. Conflict is a **prefix test over places**, so a *disjoint sibling field stays fully
   mutable*: `borrow this.buf.view() as b { this.hits = this.hits + 1; … }` is legal, because two distinct
   fields cannot overlap in storage. Reads are untouched — a `const fn` call, an index, a `foreach` over
   the frozen host, and an element write (`a[0] = 7` cannot realloc, and a view is a mutate-through window
   in the first place).
3. **A view LOCAL's root must already be lifetime-bounded** — a `borrow` alias, a by-value view parameter,
   a derive off either, or a free function's result when every view handed to it was bounded. A view
   minted from a container you can still name is *not* bounded, because the next line may grow it, and is
   an error with the window as its remedy.

The rule covers **every `type view`**, not just `View<T>`: an iterator that borrows is a view. That is only
expressible because the mint is read from the grant — `Map` has no `view()`, so before that,
`MapValueIter<int32> mi = m.values();` had no window it could open at all.

**`foreach` and `parallel_for` are the same window under a different spelling.** Each holds a borrowing
iterator over its operand for the extent of the body, so the operand is frozen there by rule 2 —
`foreach (int32 x in d) { d.add(item: 9); }` is a **compile error**, not the runtime panic the growable <!-- xfail: iter_reserve_dynarray, iter_reserve_map -->
containers' mods counter used to raise. The operand roots through its receiver, so `foreach (v in
m.values())` freezes `m`. Reads stay free: a `const fn` call on the operand, an element write through a
`ref` binding (that is what `foreach (ref …)` is *for*), and any disjoint container or sibling field. The
mods counter remains as defense in depth for the `unsafe`/FFI paths that no static rule sees.

- Obtain one from a container: `DynamicArray`/`FixedArray` expose **`view()`** (whole) and
  **`slice(from:, count:)`** (bounds-checked sub-range); `View<T>` itself has `slice`, `length()`,
  `isEmpty()`, `operator[]` (a mutate-through place), and `iterator()`/`iterMut()` for `foreach`.
- A view **flows down the call stack** as a by-value parameter; **`const View<T>`** expresses read-only
  intent. It may **not** be stored in a field/collection/`enum`, and may be **returned only** when it <!-- xfail: view_field, view_collection_elem, view_optional_field -->
  borrows `this` or a `ref`/view parameter (so `arr.slice(...)` on a `ref`/`this` receiver is fine; a view
  over a *local* is rejected). To hand back data you own, copy into a `DynamicArray`. <!-- xfail: view_return_over_local, view_ctor_over_local -->
- **A view may not be passed by `ref`/`out`** — it is already a borrow, and the only thing the extra <!-- xfail: view_ref_param, view_out_param -->
  indirection adds is the power to reseat the caller's view at storage the caller never named. Pass it by
  value (it is two words) or return one. The ban covers contract members too, where a bodiless signature
  would otherwise propagate the spelling to every implementer.
- **A view argument may not root in another argument's mutable place**, nor in the receiver of a <!-- xfail: view_arg_aliases_ref, view_arg_aliases_receiver -->
  non-`const fn`: `bad(d: ref d, v: d.view())` and `b.eat(v: b.view())` hand the callee a window over
  storage it may grow. A view roots through its *receiver*, so a chained derive roots where its receiver
  roots.
- **`foreach` needs a place to iterate from.** `foreach (x in d.view())` has nowhere to put the view and
  nothing holding the buffer still — open a window and iterate the alias. An operand that *is* its own
  iterator (`"ab".chars()`, `m.valuesMut()`) is copied by value and stays legal.
- **A view is minted by the type it views, and by nothing else.** A view is a bidirectional
  relationship — it does not exist without a type to view — so a `type view`'s **constructor is private**,
  and writing `public` on it is a compile error rather than a silent downgrade (the same rule a `view` <!-- xfail: view_ctor_public -->
  *field* already obeys). Two places may call it: the **view itself**, which is what makes `View.slice`
  work; and the **type being viewed**, which declares that relationship by implementing a member of a
  contract marked **`@viewable`**. Implementing such a member lets that member's body mint the view it
  returns — the return type self-selects, so a marked contract cannot over-grant, and a member returning
  `int32` mints nothing.

  ```kama
  @viewable type contract Viewable<V> for resource, value, view { fn V view(); }

  type resource DynamicArray<T, A> implements …, Viewable<View<T>> {
      public unsafe fn View<T> view() { return View::<T>.over(at: this.data, count: this.len); }
  }
  ```

  The marked contracts in tree are `Viewable<V>`, `Iterable<T>`/`IterableMut<T>` (prelude) and
  `ValuesIterable`/`ValuesIterableMut`/`EntriesIterable` (`std::collections`). A **`@viewable` contract is a
  mint protocol, not a value**: it declares *who* may hand out a view, so boxing one would erase the very
  identity the grant is about. It emits no C type at all — no vtable, no fat pointer — and naming one as a
  local, parameter, field or return type is an error. Use it in an `implements` clause or as a generic <!-- xfail: mint_protocol_not_a_value -->
  bound. `borrow` and `parallel_for` are **nominal** on it too: a host whose method was never granted is
  rejected, though resolution stays structural, so the emitted call is still direct. `parallel_for` wants
  one specific grant — `view()` — because it needs contiguous storage; `borrow` accepts any.

  **What the mint buys is auditability and generality; the window is what buys soundness.** A view is
  minted only by a type that claims to own the memory — but that type's own `view()` can still return a
  truthful pointer with a false length, and no type system without lifetimes can tell. That residual is
  named and small: the trusted set is the `unsafe fn` bodies of the types that own the memory, and it is
  greppable at declarations. What safe kama can no longer do is hold a correctly-minted view across a
  mutation of the thing it views.

- **A borrowing iterator is itself a view.** Every collection iterator — `ViewIter`/`ViewIterMut`,
  `DynamicArrayIter`, `MapValueIter`, `BitSetIter`, the `string` iterators `Chars`/`Split`, all of them — is
  declared `type view`, so it obeys the same escape rules as the `View<T>` above: a local or a by-value
  parameter, returnable from the container's own `iterator()`/`values()` (it borrows `this`), and **never** a
  field, a collection element or an `enum` payload — **and it obeys the window rule**, so an iterator
  local is bounded exactly as a `View<T>` local is. That closes the *laundering* path, where a `View<T>`
  could not be stored in a field but its iterator, holding the same pointer, could. It costs nothing at the
  use site: `foreach` resolves `iterator()`/`iterMut()` structurally and emits direct monomorphized calls, so
  a view-kind iterator is never widened to a contract value. The mods-counter fail-fast the growable
  containers carry is unrelated and was never a substitute — it points into the container too, so reading it
  after a free *is* the use-after-free.

### Hash maps & sets (`std::collections`) ✅

`Map<K, V, H: Hasher = DefaultHasher, A: Allocator = GlobalAllocator>` (open-addressing, linear-probing,
tombstoned, grows at 0.75 load) and `Set<K, H: Hasher = DefaultHasher, A: Allocator = GlobalAllocator>` (a
thin wrapper over `Map<K, Unit, H, A>`), over a key `K: Hashable + Equatable` (the trailing `A` is the custom
allocator — see "Custom allocators" below). These two key contracts are in the **prelude**:

```kama
type contract Hashable  for value, resource, enum, intrinsic { fn uint64 hash(); }
type contract Equatable<T is This> for value, resource, enum, intrinsic { const fn bool equals(const ref T other); }
```

Keys are hashed and compared **by content**, so a lookup key built any way (a concat, a fresh
construction) finds the stored entry. `string` and every **integer width** satisfy both out of the box, via
**pure-kama** `type intrinsic` blocks that ship in the **prelude** (universal — no `std::collections`
import; *no* compiler blessing). **`hash()` returns a cheap CONTENT hash**
— identity (`cast<uint64>(this)`) for an integer, FNV-1a over the UTF-8 bytes for a `string` — and the
**avalanche/mixer is a separate, pluggable step** the `Map`/`Set` apply via their `H: Hasher` type
parameter: `slot = H::finish(k.hash()) & (cap-1)`. `H` defaults to **`DefaultHasher`** (splitmix64, strong
avalanche, DoS-*agnostic* — see below) so `Map<K, V>` is unchanged; `Map<K, V, FastHasher>` swaps in a
cheaper single-multiply mixer for trusted, well-distributed keys (the `Hasher` contract + both hashers live
in `std::collections`). `string` declares `Equatable` with an empty body, satisfied by its built-in `equals`; each
integer's is scalar (a conformance on a **primitive** — `this` is the scalar itself). Floats get `Equatable`
only (exact `==`) — intentionally not hash-keyable. A third prelude contract,
`type contract Comparable<T is This> for value, resource, intrinsic { const fn Ordering compareTo(const ref T other); }` (returning the prelude enum
`Ordering { Less, Equal, Greater }`), gives every int/float/string a total order through the same pure-kama
`type intrinsic` blocks — the bound for `PriorityQueue` and the sorted containers. A **user key** declares `implements Hashable, Equatable`
and provides the two methods. Bounds are **nominal**: the `implements` is required (a coincidental `equals`
is not enough), the same rule as `foreach`.

```kama
import { std::collections::Map, std::collections::Set };

Map<string, int32> counts = Map.empty();
counts.put(key: "a", value: 1);
counts.put(key: "a", value: 2);                        // overwrite (drops the old value)
int32 v = match (counts.get(key: "a")) { case Some(value: x): x; case None: 0; };   // 2
counts.remove(key: "a");   bool has = counts.contains(key: "b");   isize n = counts.length();

Set<string> seen = Set.empty();
seen.add(key: "x");   bool member = seen.contains(key: "x");
```

`foreach (K k in map)` / `foreach (K k in set)` iterates the keys (a by-value key iterator, present for a
`Copyable` key; it guards against a mid-iteration `put`/`remove` like the `DynamicArray` iterator).
`foreach (V v in m.values())` iterates the **values** by copy (present for a `Copyable` value), and
`foreach (ref V v in m.valuesMut())` **borrows every value in place** to mutate it — the value analogue of
`iterMut()`, and the way to walk a map whose key isn't `Copyable`. `copy m` deep-copies a whole map
(independent clone) — present only when **both** key and value are `Copyable`, gated by a **multi-condition
`when [K: Copyable, V: Copyable]`**. `foreach (Entry<K,V> e in m.entries())` iterates the **key-value pairs**
by copy (`e.key()` / `e.value()`), present only when **both** key and value are `Copyable` — the pair analogue
of `iterator()` (keys) and `values()`. `Entry<K,V>` is a named pair (the language has no tuple); a key is never
mutated in place (that would corrupt the table), so there is no `entriesMut()` — mutate values via
`valuesMut()` / `getRef`.

`Map` is **move-only**: it owns its keys and values (dropping the key + handing the value back on `remove`,
dropping both on overwrite/`clear`/end of life — ASan/UBSan-clean for owning keys *and* values, e.g.
`Map<string, DynamicArray<string>>`). Lookups **borrow** the key (`ref K`), so they don't consume a key you're
holding. Three value accessors form a consistent trio: `get(key:) -> Optional<V>` hands back a **deep copy**
(present only when `V` is `Copyable`); `getRef(key:) -> ref V` **borrows the stored value in place** (any `V` —
the accessor that makes a `Map` of move-only values like `Owned`/`Shared`/a collection first-class rather than
write-only; panics on an absent key, so guard with `contains` first, as a map lookup is *partial*);
`remove(key:) -> Optional<V>` **moves the value out** (`None` when absent — reclaim it or discard to drop).
A key that is an inline rvalue — a `string`/number literal or a user-type ctor — is materialized into a temp
automatically, so `m.get(key: 5)` / `m.get(key: Point.make(x: 1, y: 2))` work without binding a local first.

`std::collections` also carries **`Deque<T>`** (a growable ring buffer — O(1) push/pop at both ends) and
**`PriorityQueue<T: Comparable>`** (a binary heap). The queue is a **min-heap by default** (bare ctor or
`PriorityQueue.minHeap()` — smallest out first, the fit for A* / event scheduling); `PriorityQueue.maxHeap()`
inverts it. `push(item:)` and `pop() -> Optional<T>` are O(log n), `peek() -> Optional<T>` (copy, `Copyable`
element) / `peekRef() -> ref T` (borrow, panics when empty) read the root O(1). It orders via the element's
`Comparable.compareTo`, is move-only (deep-copies only for a `Copyable` element), and — since heap order isn't
meaningful — isn't iterable; drain it with `pop`. (It's backed by a `DynamicArray`, which gained an O(1)
`swap(i:, j:)` in-place element exchange.)

**`SlotMap<V>`** is a generational slot map (Rust's `slotmap`; the ECS entity / asset registry): `insert(value:)
-> Handle` hands back a stable, Copyable `Handle`, and `get(handle:) -> Optional<V>` (copy, `Copyable`) /
`getRef(handle:) -> ref V` (borrow, panics on a stale handle) / `remove(handle:) -> Optional<V>` (moves out)
all **reject a stale handle** — one whose slot was removed, or removed and reused for a different value —
returning `None` (or panicking on `getRef`) instead of aliasing the new occupant. That's a per-slot generation
counter (odd while occupied, bumped on every insert/remove), so a handle can safely outlive the value it names
and a dangling handle is caught, not a use-after-free. `contains(handle:)`, `length`/`isEmpty`, `clear`, and
`values()`/`valuesMut()` (iterate the live values, copy or borrow) round it out.

**`SortedMap<K: Comparable<K>, V>` / `SortedSet<K: Comparable<K>>`** are the **ordered** map/set — a **B-tree**
(min-degree 6; peer: Rust `BTreeMap`, C++ `std::map`) keyed by `Comparable.compareTo`, not a hash. They mirror
`Map`/`Set` (`put`/`get`(copy)/`getRef`(in-place borrow, panics absent)/`remove -> Optional<V>`/`contains`/
`length`/`isEmpty`/`clear`/deep `copy`/JSON serde), but keys stay **sorted**, so they add the ordered queries a
hash map can't answer: `first()`/`last()` (min/max key), `floor(key:)`/`ceil(key:)` (nearest ≤ / ≥), `range
(from:, to:)` (a half-open key window), and `keys()`/`values()` (resp. `SortedSet.elements()`) yielding a fresh
`DynamicArray` of copies in **ascending order** (serde likewise emits an ascending pair array). Each node backs
its keys, values, and child boxes with `DynamicArray` (reusing its move-out / shift / RAII) and holds children
as `Owned<BTreeNode>` heap boxes, so a subtree moves as one owned pointer and the splits (insert) / borrows +
merges + predecessor-swaps (remove) relocate move-only keys **and** values with ownership intact. `getRef`
forwards an in-place borrow up the tree via the escape checker's chained-ref-return rule (a `fn ref T` may
return a place-returning method call whose receiver roots at `this`). `SortedSet<K>` wraps `SortedMap<K, Unit>`,
as `Set` wraps `Map`.

### Custom allocators ✅

A collection's memory source is a trailing type parameter `A: Allocator = GlobalAllocator`. Because it
defaults (default type parameters), `DynamicArray<T>` / `Map<K,V>` / bare `BitSet` are unchanged; the
allocator is opt-in. Every container that manages its own heap buffer carries it: `DynamicArray`, `Map`,
`Set`, `Deque<T, A>`, `FixedArray<T, A>`, `BitSet<A>` (its first type parameter, so a plain `BitSet` is now
the all-defaulted instance), `SlotMap<V, A>`, and `PriorityQueue<T, A>` (which owns no buffer itself — it
threads `A` to its embedded `DynamicArray<T, A>`). A stateful allocator arrives via a named **`ctor`**:
`withAllocator(allocator:)` for the growable containers, `withAllocator(allocator:, size:)` for the eager
`FixedArray`, and `withAllocator(allocator:, maxOrder:)` for `PriorityQueue`. The ordered containers thread it
too: `SortedMap<K, V, A>` / `SortedSet<K, A>` push `A` through the B-tree — the node *contents* (inner arrays) and
every *interior* node box draw from `A` (via the placement `new(allocator:) BTreeNode` below), so `arena.reset()`
reclaims the whole tree. (One box per tree — the always-live root — stays `GlobalAllocator`, freed by RAII: a
bare `new` into a stateless-allocator box is legal for any pointee `A` and sidesteps the eager-root/`withAllocator`
ordering, whereas a placement root would require the not-yet-assigned handle. A minor, documented wart.)
An **`Allocator` is a copyable value handle** (the C++ `std::pmr::polymorphic_allocator` / Rust `&Bump` / Zig
`std.mem.Allocator` model), a two-method contract. It is **foundational**, so the `Allocator` contract and the
default `GlobalAllocator` live in the **global prelude** (beside `Comparable`/`Hashable`) — both the collections
*and* the smart pointers name them, and they survive `--no-std`. The concrete strategy types `Arena`/`BumpAllocator`
stay in `std::collections`.

```kama
type contract Allocator for value {
    fn Optional<UnsafePtr> allocate(usize bytes);  // None on OOM/exhaustion — fallible seam (never panics)
    fn void deallocate(UnsafePtr pointer, usize bytes);
}
```

The container stores `A alloc` by value and routes every buffer through `this.alloc.allocate/deallocate`;
dispatch is a **direct monomorphized call** (no vtable), so a `GlobalAllocator` (a zero-size handle straight
onto libc `malloc`/`free`) costs nothing. A **stateful** allocator is a small handle pointing into a
**caller-owned `Arena`** (one heap buffer, bump-allocated, `reset()` bulk-frees in O(1)); the arena must
**outlive** the container — a documented contract, not a borrow-checked one (a raw `UnsafePtr` isn't escape-checked
and there is no lifetime tracking). Since Kama has no constructor overloading, a stateful allocator arrives via
a **named `ctor`** (`DynamicArray.withAllocator(allocator:)`), which assigns `alloc` on the value it builds. `Allocator`/`GlobalAllocator` are prelude (global, no import); `Arena` and `BumpAllocator` ship in
`std::collections`:

```kama
import { std::collections::DynamicArray, std::collections::Map, std::collections::Arena, std::collections::BumpAllocator };

Arena arena = Arena.make(capacity: 1 << 16);                             // caller-owned; drops last
DynamicArray<int32, BumpAllocator> xs = DynamicArray.withAllocator(allocator: arena.handle());
Map<int32, int32, A: BumpAllocator> m = Map.withAllocator(allocator: arena.handle());  // named arg skips H
// ... fill/use; xs and m draw from the one arena; their deallocate is a no-op; the Arena frees the buffer.
```

**Fallible seam (✅ shipped, MCU step 5).** `allocate` returns `Optional<UnsafePtr>` — `None` on OOM/exhaustion,
never panics. Infallible `new` and the direct-`malloc` containers (`DynamicArray`, `Map`, `Set`, `Deque`,
`FixedArray`, `BitSet`, `SlotMap`, `PriorityQueue`) plus the boxed `SortedMap`/`SortedSet` B-tree keep the
pre-step-5 **panic-on-OOM** behavior (they unwrap the `Optional` via the prelude `unwrapPtr`, panicking on
`None`; `try new` takes the same pointer through `ptrOrNull`, the sibling that answers with a null pointer
so the failure can become a `None`). The ONE non-panic construction entry is **`try new`** (below); user code that calls `allocate`
directly can `match` on `None` (e.g. a bump/arena that stops at exhaustion instead of trapping — the
game-engine frame-allocator / real-time pattern, not only MCU).

#### `try new` — non-panic construction ✅ (MCU step 5)

`try new T.make(...)` yields
`Optional<Owned<T>>` — `None` when the allocation fails, instead of panicking. It is the single fallible
construction entry (`new` stays the infallible sugar that unwraps-or-panics); there is no parallel
`tryAllocate`.

**`try` accepts exactly what `new` accepts** — they share one grammar rule, so the two spellings cannot
drift: the placement form (`try new(allocator: a) T.make(…)`), the on-type turbofish
(`try new T::<A>.make(…)`), and an interface-element handle (`Optional<Owned<Contract>>`,
`Optional<Shared<Contract>>`) all work. Like `try cast`, it needs a **declared destination** to read
`Some`'s payload type from — a local, an argument, or the function's return type — which is what makes a
fallible factory the ordinary shape.

```kama
Optional<Owned<Box>> b = try new Box.make(v: 7);
match (b) { case Some(value: x): use(x); case None: /* OOM — recover, don't trap */ }

fn Optional<Owned<Mesh>> load(string path) { return try new Mesh.parse(path: path); }   // return position

// Placement: the block comes from the arena, and `None` is how an EXHAUSTED arena answers — no trap.
Optional<Owned<Shape, BumpAllocator>> s = try new(allocator: arena.handle()) Circle.make(r: 5);
```

Every allocation on this path answers `None`, including a `Shared` handle's control block. A bare
`try new` into a box whose allocator is **stateful** is refused — that block would be released through <!-- xfail: try_new_stateful_bare -->
the wrong allocator — so an arena-backed box is built with the placement form
([tests/try_new_arena.kama](../tests/try_new_arena.kama),
[tests/try_new_iface.kama](../tests/try_new_iface.kama),
[tests/xfail/try_new_stateful_bare.kama](../tests/xfail/try_new_stateful_bare.kama)).

#### No-heap subset ✅ (MCU step 5)

A per-region **`@noheap`** function attribute and a whole-program **`--no-heap`** build flag make every
emitter-visible heap allocation a **compile error** — `new`/`try new`, `parallel_for`/`spawn` argument <!-- xfail: noheap_new, noheap_try_new, noheap_spawn, noheap_error_box, noheap_interp -->
boxing, error-boxing into `Owned<Error>`, and string interpolation's `Formatter` buffer all funnel through
one gate. Target-independent (composes with `--target embedded`).

**`@noheap` is transitive**, which is what makes it a proof rather than a lint: a `@noheap` body may not <!-- xfail: noheap_transitive_new, noheap_transitive_deep, noheap_transitive_method -->
call anything that allocates, however many calls away it is, and the diagnostic names the chain
(`tick -> mix -> grow`). Nothing has to be annotated for this — where the compiler can see the callee it
**infers**, so an ordinary un-annotated helper is fine exactly when it is allocation-free
([tests/noheap_chain.kama](../tests/noheap_chain.kama)). Reachability includes **destructors**: owning a <!-- xfail: noheap_dtor_of_local -->
local whose `~T()` allocates allocates, even though the body contains no call.

The chain ends at libc, and `GlobalAllocator` is the leaf — so a container or box drawing from it is <!-- xfail: noheap_container_growth, noheap_container_local, noheap_owned_drop, noheap_flag_container -->
rejected inside a no-heap region, including merely *owning* one (dropping it frees; `free` can block on the
allocator's lock exactly as `malloc` can). **The whole-program `--no-heap` flag applies the same leaf**, so
a no-heap build cannot reach `malloc` through a container either — the flag rejects a *direct* allocation
in every body, and the leaf is what closes the indirect path a container takes. Its diagnostic anchors on
the innermost function **you** wrote — the frame holding the call you can change — and never on a prelude
or `std::` body, which would name code the author did not write and cannot edit. The same container over an **arena** is fine and needs no
annotation: `A` is a type parameter, so `DynamicArray<T, BumpAllocator>` is a different monomorph reaching
a different `allocate` ([tests/noheap_arena.kama](../tests/noheap_arena.kama)) — which is the idiom a
real-time region is expected to use.

`string` is the one **intrinsic** that mints heap, and it is immutable, so every method that "changes" one <!-- xfail: noheap_string_concat, noheap_string_method, noheap_string_copy, noheap_foreach_copy -->
returns a NEW owned string: `a + b`, `concat`, `substring`, `trim*`, `replace`, `toLower`/`toUpper`,
`truncate`, `split` and a deep `copy` are all rejected in a no-heap region. So is `foreach (string s in xs)`,
which yields each element **by value** and deep-copies to do it — `foreach (ref string s in xs)` borrows and
is legal. Reading a string is always legal (`length`, indexing, the predicates), and a string **literal** is
a borrowed view that allocates nothing, so `string tag = "voice";` belongs in a real-time body.

[tests/noheap_realtime_ok.kama](../tests/noheap_realtime_ok.kama) is the standing control for all of this:
a literal, an `InlineArray<T>#(N)` with a `borrow` window and an in-place `sortUnstable`, and a borrowed heap
object — the whole of it legal, with no annotation anywhere but the one attribute.

Where the compiler **cannot** see the callee, the target must **declare** the promise, and the call is
otherwise rejected rather than assumed harmless: a `fnptr` or bound function pointer has no knowable <!-- xfail: noheap_fnptr_call, noheap_contract_member -->
target at all, and a contract member or `virtual` method is dispatched through a slot. Marking the
**contract member** `@noheap` is what lets the proof cross it — every implementation is then checked <!-- xfail: noheap_contract_impl -->
against that promise, exactly as `const fn` already works on a contract
([tests/noheap_contract.kama](../tests/noheap_contract.kama)). A virtual call the compiler devirtualizes
(a `final` class, a `final fn`, or a method nobody overrides) is a direct call and needs no annotation.

The **`fnptr` seam has the same escape, spelled on the signature**: `@noheap fnptr int32 Op(int32 x);` <!-- test: noheap_fnptr -->
makes the promise part of the named type, so every function bound to it must itself be `@noheap` — in <!-- xfail: noheap_fnptr_bind, noheap_fnptr_bind_arg -->
**every** bind position, a local initializer, an assignment, a call argument, a field and a module
`static` alike — and a call through such a slot is then legal inside a no-heap region
([tests/noheap_fnptr.kama](../tests/noheap_fnptr.kama)). One direction, as on a contract: a `@noheap`
function may be bound to a plain signature, which constrains only itself. `BindableFunctionPtr<Sig>`
inherits it, because its `Sig` *is* an `fnptr` type. Without this a callback — the shape a real-time
system is built from — had no legal spelling in a `@noheap` region at all.

`@noheap` marks a **body**, so it goes on any declaration that has one: a free `fn`, and equally a <!-- test: noheap_method -->
**method, `ctor`, destructor or operator**. The member forms matter because a real-time entry point is
naturally a method — `synth.fill(…)` — and a destructor is what runs at the end of a real-time scope.
It is rejected where there is no body to gate: on a module `static`, and on the bodyless `extern` forms <!-- xfail: noheap_on_static, attr_on_extern -->
— an `extern "<h>";` and an `extern fn`, whose bodies are C and cannot be checked at all. A **`fnptr`** is
the exception, and by the same property rather than in spite of it: it declares a TYPE for someone else's
body to satisfy, so the promise has somewhere to land (above). `@interrupt` and `@section` remain rejected <!-- xfail: attr_on_fnptr -->
on all three, because they attach to emitted code and a `fnptr` emits only a typedef. `@interrupt` stays
free-function-only (a vector table reaches its handler by **bare** symbol, and a member's C name is
mangled and takes a receiver); `@section(".name")` is legal on a free `fn` and a member.

```kama
@noheap fn int32 tick(int32 n) { /* new / "${x}" / spawn here is a compile error */ ... }

type value Mixer {                                     // ...and the same gate on a member
    @noheap public fn int32 fill(int32 n) { ... }      // the natural spelling for an audio callback
}
```

### Allocator-aware `new` / `Owned<T, A>` / `Shared<T, A>` / `Weak<T, A>` ✅

Heap-*boxed* objects draw from an allocator too: `Owned<T, A: Allocator = GlobalAllocator>`,
`Shared<T, A>`, and `Weak<T, A>`. A bare `new T.make(args)` is unchanged (`A` defaults to `GlobalAllocator` → libc
malloc/free); a **placement** form `new(allocator: a) T.make(args)` draws the block from `a` and stores the handle in
the box, so its dtor releases through the **same** allocator — letting a boxed object live in a caller-owned
arena and be bulk-reclaimed on `reset()`:

```kama
Arena arena = Arena.make(capacity: 1 << 12);                  // drops last (outlives the box)
Owned<Node, BumpAllocator>  n = new(allocator: arena.handle()) Node.make(v: 42);
Shared<Node, BumpAllocator> s = new(allocator: arena.handle()) Node.make(v: 7);   // pointee AND ctrl from the arena
// n/s dtor deallocate() is a no-op; the objects live in the arena; the Arena frees the region.
```

The allocator must be spelled on the box type (`Owned<T, A>` / `Shared<T, A>`, explicit over implicit — a
`new(allocator: BumpAllocator)` into a box spelled `Shared<T>` is a compile error). A stateful `A` **requires** <!-- xfail: new_alloc_shared -->
the placement form — a bare `new` into a stateful-allocator box is a compile error (it would leak). For <!-- xfail: new_bare_stateful -->
`Shared`/`Weak`, **both** the pointee and the shared control block are drawn from `A`, and every handle carries
its own copyable `A` value (copied through `copy()`/`downgrade()`/`tryUpgrade()`), so whichever handle observes
`strong == 0 && weak == 0` — even a `Weak` that outlived its `Shared` — frees the ctrl through the right
allocator; `arena.reset()` reclaims a whole ref-counted graph. With allocator-aware `new`,
**`SortedMap`/`SortedSet`** (whose B-tree nodes box through `new`/`Owned`) thread `A` through their full
`SortedMap<K, V, A>` / `SortedSet<K, A>` form (interior node boxes placement-`new` from `A`; the root box stays
`GlobalAllocator`). **Interface-element** boxes (`Owned/Shared/Weak<Contract, A>`, e.g. `Shared<Shape,
BumpAllocator>`) draw from the allocator the same way: the type-erased fat handle (`{obj, vtbl[, ctrl]}`) grows a
by-value `A alloc` + pointee `objsize`, so the pointee and control block are drawn from `A` and freed through it
— completing allocator coverage for **every** box (concrete and contract-erased). Default-`GlobalAllocator`
interface boxes are byte-identical to before (they keep the plain intrinsic macros). One design limit:
object-graph serialization (`@generate` `Shared`/`Weak`/`Owned` edges) is **`GlobalAllocator`-only** — a graph
edge spelling a stateful `A` is rejected at compile time (deserialize has no allocator on the wire). <!-- xfail: graph_alloc_iface -->

## Smart pointers ✅ (triad → prelude/built-in ✅ — embedded, always in scope, no `import`)

The smart-pointer triad `Owned`/`Shared`/`Weak` is **prelude / built-in — always in scope, no `import`**.
RAII-over-GC *is* the language (every `new T.make(args)` already targets a `HeapOwner`, and the compiler
special-cases the triad throughout: `HeapOwner`/`Deref`, never-null checks, drop insertion, ctrl-block layout),
so the ownership triad is as fundamental as `int` or `UnsafePtr` and shouldn't require an import. "Built-in" means
**always-available, not rewritten in C**: they stay **kama-defined** (RAII `resource`s over `Deref`/`HeapOwner`,
refcounting in kama), loaded as part of the prelude like the primitive `Hashable`/`Equatable` conformances.

The compiler adds only what a library can't express: the type-erasure (fat pointer + vtable) that makes
`Owned<Shape>`/`Shared<Shape>` over a **contract** work, `new T.make(args)` heap placement into any `HeapOwner<T>`,
and — because it *is* their emitter — direct manipulation of their internals (ctrl blocks, shell-adopt,
ownership transfer) in the emitted C of the serialization graph lowering, so that machinery leaks **no** public
`__`-methods onto the triad.

`Owned<T>` — unique heap ownership (= Rust `Box` / C++ `unique_ptr`), zero overhead, **move-only**,
**auto-deref**, RAII-freed. The kama surface stays pointer-free; the raw pointer is confined to the library.
Use it for heap objects, recursive data structures, and polymorphic ownership.

```kama
Owned<Counter> c = new Counter.make(start: 40);     // `new` heap-boxes the ELEMENT type
c.bump();  int32 n = c.get();                        // auto-deref: . reaches the pointee
Owned<Counter> d = c;                                // MOVE: c is now empty (moved-from)
fn Owned<Node> make(int32 v) { return new Node.make(id: v); }   // inline `new` in return/arg position — factory, moves out
```

Move-only: copying/initializing/returning an `Owned` transfers ownership and invalidates the source, so the
pointee is freed exactly once (RAII, with the pointee's destructor).

**Ownership hand-off — `give` / `copy`.** When a *named* owned value is handed off — in an initializer,
assignment, argument, or return — an explicit marker states the intent, uniformly in all four positions:
**`give`** moves (invalidates the source), **`copy`** retains (`Shared`/`Weak`) or duplicates. **Every owning
kind is movable**; what varies is whether it is *also* `Copyable` and what a *bare* hand-off defaults to.
`Owned` is move-only — a bare hand-off moves, and `copy Owned` is an error (it's unique). `Shared`/`Weak` <!-- xfail: copy_owned -->
are `Copyable` and declare `bare: copy`, so a bare hand-off **retains** (refcount++); `copy` is the explicit
retain, and **`give` still moves the handle** — the ref transfers and the source is consumed (this is how a
`Shared` returns from a factory without a spurious retain/drop). A `value`/primitive just copies. A fresh
`new`/constructor/call result needs no marker. Smart pointers also **pass by value**: the callee *owns* the
argument and drops it at function end — `fn int32 use(Owned<T> p)` consumes it (`use(p: give x)`), `fn int32
peek(Shared<T> s)` retains it (`peek(s: x)`, `x` stays valid).

```kama
Owned<Counter> b = give a;   // explicit move (a consumed)
Shared<Counter> t = s;       // copy/retain (default) — both valid
Shared<Counter> u = copy s;  // explicit retain (same as bare); `give s` moves the handle (s consumed)
```

*(`copy` of a collection is a deep copy — a fresh buffer, element-wise: a bitwise-copyable element is copied
memberwise, a `Copyable`-resource element is deep-copied via its own `copy` ctor. A resource element that is not
`Copyable` is rejected. `give` of a collection **moves** the buffer.)* <!-- xfail: copy_resource_coll -->

**Move-only `resource` values + the `Copyable` contract.** A **`type resource`** value (it owns something, or
has identity) is **move-only**: a bare named hand-off *moves* (the source is consumed, its destructor
suppressed), so its heap is freed exactly once — a silent copy is never emitted (that would double-free).
`give` is optional emphasis; `copy` is an error unless the type opts in. A `resource` **opts into copy** <!-- xfail: copy_value -->
**nominally** — `implements Copyable(bare: …)` (the prelude contract `Copyable<T is This> { ctor copy(ref T source); }`)
plus a **public `copy` constructor** (a lone `copy` ctor without the `implements` does *not* make a type
copyable). It is a **`ctor`** because a copy *is* a new object — the same reason a self-returning `static fn`
is rejected as a disguised constructor; the source is *borrowed* (`ref This`), since copying never consumes <!-- xfail: self_returning_static_fn -->
it. Opting in **requires declaring the bare-hand-off default**: `Copyable(bare: give)` (a bare hand-off moves)
or `Copyable(bare: copy)` (a bare hand-off deep-copies). A marker (**`give x`** / **`copy x`**) always
overrides the default; there is no "ambiguous — must annotate" error. Because `copy`/`give` are markers only
in expression position, they're **contextual keywords** — usable as member names, so the opt-in ctor is
literally named `copy`.

```kama
type resource Res implements Copyable(bare: copy) {   // a bare hand-off deep-copies
    DynamicArray<int32> items;
    ~Res() { }
    public ctor copy(ref Res source) { return Res.make(v: source.items[0]); }     // the Copyable ctor
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
                   public ctor make(int32 x, int32 y) { Point r; r.x = x; r.y = y; return give r; } }
type value BoxP implements Deref<Point> {
    Point inner;
    public ctor make(Point p) { BoxP r; r.inner = p; return give r; }
    public fn ref Point deref() { return this.inner; }
}
BoxP b = BoxP.make(p: Point.make(x: 30, y: 12));
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
| collection (`FixedArray`/`DynamicArray`/`string`) | ⛔ marker required | **move** (buffer) | **deep copy** (fresh buffer) |
| plain `resource` (move-only value) | **move** | move (emphasis) | ⛔ "opt into `Copyable`" |
| `Copyable` resource (has a `copy` ctor) | its declared `bare:` default | move | **deep copy** via `copy` |
| collection of `Copyable` elements | ⛔ marker required | move | **deep copy** (element-wise `copy`) |

A marker on a fresh rvalue is an error. Move tracking is compile-time: reading a moved value, moving out of a <!-- xfail: handoff_fresh -->
field/element, moving inside a loop a value declared outside it, and a conditional move that is still live at
scope exit are all rejected — there is no runtime drop flag.

**Local variable shadowing is a compile error.** A local declaration may not shadow a parameter, an <!-- xfail: shadow_param, shadow_field, shadow_enclosing -->
enclosing-scope local, or an in-scope field of the enclosing type (C#-aligned; one name = one binding within
any live scope — keeps both name resolution and move tracking unambiguous). Sibling scopes may reuse a name
freely (they never coexist). A *parameter* sharing a field's name — the `this.x = x` constructor idiom — is
allowed; a static method has no `this`, so a local there can never shadow a field.

`Shared<T>` — ref-counted shared ownership (= C++ `shared_ptr` / Rust `Rc`). **Copyable**: each copy retains
(refcount++), each drop releases, and the pointee is destroyed when the **last** handle goes away.

```kama
Shared<Tex> a = new Tex.make(id: 7);
Shared<Tex> b = a;     // retain — a and b share one Tex (both valid)
b.use();  int32 n = a.id;
// a, b drop in RAII order; the Tex is freed exactly once, with the last handle
```

`Weak<T>` — a non-owning weak reference to a `Shared<T>`'s pointee. It does **not** keep the pointee alive, so
it **breaks reference cycles** that `Shared` alone would leak. You can't dereference a `Weak` (it may be
dead) — **upgrade** it with the checked `tryUpgrade()`, which returns an `Optional<Shared<T>>` you must
`match` on, so the dead case is impossible to ignore:

```kama
Weak<Tex> w = s.downgrade();                  // make a weak ref from a Shared (does not keep Tex alive)
int32 id = match (w.tryUpgrade()) {           // -> Optional<Shared<Tex>>
    case Some(value: up): up.id;                     // alive: use the upgraded Shared
    case None: -1;                            // dead: the cycle-safe path
};
```

**No null (safe surface) — see GOALS §3b.** A value, `Owned`/`Shared`, `ref`/`out` borrow, or contract value
is always valid: there is nothing to null-check. `null` is only for `UnsafePtr<T>` at the FFI boundary, and
that holds in **both directions** and for **every** other type — a safe type can neither be *compared* to
`null` (`== null` / `!= null` is a compile error; the C habit checks the wrong thing here) nor *set* to it. <!-- xfail: null_safe_compare -->
`int32 x = null;`, `Thing t = null;`, a `string` field defaulted to `null`, `x = null` and `x == null` on
any of them are all rejected; model absence with `Optional<T>`, or use a zero value. The rule reads the
**declared type**, so it covers primitives — and it does not care whether you are inside an `unsafe fn`,
which changes what may be *dereferenced*, not what may be null. An unresolved or FFI type name is left
alone, since a C typedef for a pointer is a legitimate `null` target. A `Weak<T>`'s liveness is obtained through `tryUpgrade() -> Optional<Shared<T>>`,
whose result forces you to handle the dead case.

Passing a smart pointer: **borrow** it by passing `ref T` — the borrow names the *object* (`ref T`,
storage-agnostic; a `ref` may not name the smart pointer itself), which auto-derefs to the held object; or <!-- xfail: ref_handle -->
**transfer by value**, where the callee owns the argument and drops it at function end (`Owned` moves in,
`Shared` retains). The pointee is a **`value`/`resource`** or a **contract** — `Owned`/`Shared`/`Weak<Shape>`
own a concrete implementer behind a fat handle and dispatch polymorphically (see Contracts below). A smart
pointer works as a *field*, *return*, and a **collection element** — `DynamicArray<Shared<Shape>>` stores and drops
each handle in RAII order and dispatches polymorphically through it. See **Generics** below.

## Functions ✅

```kama
fn int32 add(int32 a, int32 b) { return a + b; }
fn int32 main() { return add(b: 20, a: 10); }   // named args; reordered to declared order
```
`ref` and `out` parameters both pass by pointer, but they are **different promises**:

- **`ref T x`** — a read-write **borrow** of a value that is already live. The marker at the call site is
  optional (`f(x: ref v)` and `f(x: v)` are both fine), because nothing is riding on it.
- **`out T x`** — the callee **must assign it** on every path before returning, and may not read the <!-- xfail: out_read_before_assign -->
  incoming value. **The call site must say `out`** (`divmod(a: 17, b: 5, q: out quotient, r: out rem)`).
  The marker is mandatory because both forms lower to the same `T*`: without it neither a reader nor the
  caller's definite-assignment analysis could tell a borrow from a fill. `out` is what lets a
  [`slot`](#uninitialized-storage--slot-) be filled by a callee and counted as assigned afterwards.

```kama
fn void divmod(int32 a, int32 b, out int32 q, out int32 r) { q = a / b; r = a % b; }
slot int32 quotient; slot int32 rem;
divmod(a: 17, b: 5, q: out quotient, r: out rem);   // 3, 2
```

**`const ref T x` is a read-only borrow, and it is what a literal or a temporary may bind to.** Neither
has an address, so the compiler materialises one into a temp — which is what lets `m.get(key: 5)`,
`readFile(path: "some/path")` and `useShape(a: Square(3))` be written without binding a local first. That
temp dies at the end of the statement, so a **non-`const` `ref`/`out` cannot take one**: the callee's <!-- xfail: ref_literal_mutable, ref_string_literal_mutable, ref_temporary_nonconst -->
write would land in storage nothing can read back. The repair is whichever the callee meant — bind a
local if it really writes, or say `const ref` if it never did. Prefer `const ref` for any parameter you
only read; it is what makes the borrow usable at a call site.

"Every path" is a real flow merge, not "assigned somewhere": an `if`/`else` in which **both** arms assign
counts, a lone `if` does not, and an arm that ends in `return`/`break`/`continue` never reaches the join
and so owes nothing to it.

**A non-`void` function must return on every path.** Reaching the closing brace without a value is a
compile error in kama itself — not a C-compiler diagnostic against generated code, which the language
server could not see. A path satisfies it by RETURNING or by DIVERGING, so all of these are accepted:
an `if`/`else` where both arms return; a `match` where every arm does (a `match` is exhaustive by
construction); a tail call to `panic`; and a loop that cannot exit (`while (true)` / `for (;;)` with no
`break`). `void` functions may fall off the end. The analysis is deliberately one-sided — it reports only
what it can prove, so a construct it does not model costs a diagnostic, never a false rejection.
(Fixtures: `tests/return_paths.kama` for what must be accepted, `tests/xfail/missing_return` for what
must not.)

## FFI — calling C ✅

`extern fn Ret name(params);` declares a C function's call signature (name + named params for lowering); the
C **prototype comes from the header** you `extern "<header.h>";` — kama never emits a prototype for an
extern function (so there's no redeclaration conflict, and a missing include is a plain C error). Link
libraries with `--link`. The FFI boundary is the language's only "unsafe" seam (explicitly `extern`):

```kama
extern "<stdlib.h>";             // every C function comes from an explicit header
extern "<math.h>";
extern fn UnsafePtr  malloc(usize n);     // UnsafePtr = void* (opaque pointer/handle); usize = size_t
extern fn void free(UnsafePtr p);
extern fn float64 sqrt(float64 x);  // libm auto-links when a program `extern "<math.h>";`s (pay-for-use)

fn int32 main() {
    UnsafePtr p = malloc(n: 64);
    if (p == null) { return 1; }  // hold / null-check / compare — but no deref yet
    free(p: p);
    return cast<int32>(sqrt(x: 1764.0));   // 42
}
```

**The FFI rule (one sentence): declare C types/functions by `extern`-including their header.** An `extern`
declaration is purely kama's call-signature (name + named params, so it can lower the call) — the actual C
prototype comes from the header you include with `extern "<header.h>";`. kama never emits a C prototype for
an extern function, so there are no redeclaration conflicts; and the runtime hides its own libc dependencies
(block-scope declarations), so **no** C function (not even `malloc`) is available without its header — a
missing include is a plain C error, never a silent guess.

**An `extern` is DECLARED in every file that names it — never exported, never imported.** It keeps its
literal C spelling and is therefore not a module symbol: there is no surface for an `export` to put it on
and nothing for an `import` to bind. Repeating `extern fn UnsafePtr malloc(usize n);` in each file that
calls `malloc` is the idiom, not a smell — it is what a C header does, and a declaration is not a
definition. A file that would rather not repeat it wraps the extern in an ordinary `fn` and exports **that**;
the wrapper costs nothing, because `--release` folds the program into one translation unit and a
pass-through compiles to the same instructions as the direct call.

**Every declaration of one C symbol in a program must agree** — same return type, same parameter names,
same parameter types. They are one entry: kama emits no prototype, so nothing downstream could catch a
mismatch, and calls are lowered by *named argument*, so two declarations differing only in parameter order
would silently reorder one file's arguments (`memcpy(dst:, src:)` emitting `memcpy(src, dst, n)`). The same
holds for a `type extern value` — matching field names and types. A disagreement is an error naming both <!-- xfail: extern_disagree -->
files.

`UnsafePtr` is `void*`; `UnsafePtr<T>` is `T*` — an **opaque carrier** (hold, pass to/from C, `null`-check, compare;
**no dereference** in kama outside an `unsafe fn`). `usize`/`isize` map to `size_t`/`ptrdiff_t`. Names beginning
`kama_` are reserved (runtime-provided).

### Math (`std::math`) ✅

Engine Tier-0 linear algebra — concrete **float32** value types: `Vec2/3/4`, `Mat2/3/4`, `Quat`, plus a
full scalar surface over libm. `import { std::math::Vec3, std::math::Mat4, std::math::sqrt, std::math::sin, … };`.

**One name per scalar operation, at BOTH float widths** — the width is inferred from the argument, so
`sqrt(x: 1.0)` is a float64 call and `sqrt(x: 1.0f32)` a float32 one. That matters because a bare `1.0`
literal in kama is a **float64**, so a float32-only module made `sin(x: 1.0)` a type error for the most
obvious thing a reader would write. kama has no overloading, so the usual answers were unavailable (C
suffixes every float32 entry point, Go and Java ship one width and make you convert, C# adds a second
class `MathF`); the mechanism used instead is kama's own — a **contract with a `type intrinsic` impl per
width**, exactly how `Comparable` reaches every primitive. Each operation is one generic free function over
`Real`, and the per-width libm call lives in the impls: `sqrt cbrt sin cos tan asin acos atan exp log
log2 log10 floor ceil round trunc abs` (one argument) and `pow fmod atan2 hypot` (two).

`Real` is **exported**, so a user type can join in — `type value MyFixed implements Real<This> { … }` and every
function above works on it. Its methods carry the same names as the free functions (as Rust's
`Float::sqrt` and Swift's `squareRoot()` do), so an implementer writes `public fn MyFixed sqrt()`.
`atan2` keeps C's `(y, x)` meaning, but the arguments are **named**, so the classic mix-up cannot happen
silently. The engine helpers that have no libm counterpart stay float32 under their kama names:
`pi`/`tau`/`halfPi`/`epsilon`/`radians`/`degrees`/`lerp`/`clampf`/`minf`/`maxf`/`signf` — constants are
zero-arg functions because a zero-argument generic has nothing to infer from. The seam is `kama_math.h`:
a kama function cannot share a name with the **extern** it calls, so the binding is renamed rather than
the API.

Two limits worth knowing. A **nested generic call cannot infer** — `log(x: exp(x: 1.0))` fails because
the inner call's return type is the very `T` being resolved; bind it to a local (kama's usual "bind it to
a local" rule). And a `ref` parameter may not name a smart pointer, so a contract instantiated at <!-- xfail: ref_handle -->
`Owned<T>` — e.g. `Order<Owned<T>>` — is not expressible; sort or compare the resources themselves.
Methods + operators (one `operator*` per type: matrices/quaternions **compose**, vector transform / rotate
are named methods — no overloading). Matrices are **column-major** with the **column-vector** convention
(`result = M * v`, GPU/WebGPU-native); `perspective`/`orthographic`/`lookAt` target **WebGPU 0..1 depth**,
right-handed. `Quat` is a unit quaternion (`fromAxisAngle`/`fromEuler`, Hamilton `*`, `rotate`, `slerp`/
`nlerp`, `toMat3`/`toMat4`). All literals are `f32`-suffixed (a bare `1.0` is float64). **SIMD** needs no
explicit vector types or intrinsics: the value types have a **SIMD-ready contiguous layout** (`Vec4` = 16 B,
`Mat4` = 4×`Vec4`), and in a `--release` build the C backend **auto-vectorizes** the elementwise ops (`Vec4`
`+`/`-`/scale, `Mat4.transform`, `Mat4*Mat4`) to **NEON on aarch64, SSE on the x86-64 baseline, and v128 on
wasm** (the wasm build passes `-msimd128`; see [targets.md](targets.md) *Platform notes*) — landing
hot math **at C parity**, and often better than a hand-written vector type would: clang de-interleaves the
array to SoA registers and computes four `dot`s or four transforms at once. This relies on
the ops **inlining** into the caller, which release builds guarantee (see *Building & debugging* — release
compiles as one translation unit). Results are bit-identical to the scalar path (the exact-value semantics are
unchanged; SIMD is a pure throughput property). `Quat`'s Hamilton product is intentionally left scalar — its
shuffled ± pattern makes a hand-vectorized version *slower* than the 16 pipelined scalar FMAs on measured
hardware (ARM64), which is the same effect that makes the auto-vectorized path win generally.

⚠️ **One limit, measured.** An **arbitrary shuffle** or a **lane mask as a value** has no spelling in kama at
all, at any target: those are what an explicit SIMD surface would add, and auto-vectorization cannot produce
them from scalar source — see *Explicit SIMD* below. *(The other limit this paragraph
used to carry — that a wasm build got no vector instructions at all — was real and is fixed: the driver now
passes `-msimd128` on every wasm build. Both halves of the claim above are held down by guards that read
real machine code, `tools/check-simd-native.sh` and `tools/check-simd-wasm.sh`; it went wrong twice for
want of them.)*

### Numbers (`std::num`) ✅

Numeric type **limits** as zero-arg functions — `int8Min/Max` … `int64Min/Max`, `uint8Max` … `uint64Max`,
`float32Max`/`float32MinNormal`/`float32Epsilon` (signed min is `-max - 1`) — and per-width integer
**operations** `minI32/maxI32/clampI32/absI32/signI32` (+ the `I64` set), parallel to `std::math`'s float32
`minf`/`maxf`/…, and explicit **wrapping** arithmetic `wrappingAddI32`/`wrappingSubI32`/`wrappingMulI32`/
`wrappingNegI32` (+ `I64`) for intentional overflow. `import { std::num::int32Max, std::num::minI32,
std::num::wrappingAddI32, … };`. (A generic `min<T: Comparable>` is now expressible: the prelude defines `Comparable`/`Ordering`
— `const fn Ordering compareTo(const ref T other)` with `type intrinsic` conformances for every int/float/string — the bound for
`PriorityQueue` + the sorted containers.)

### Sorting & searching (`std::collections`) ✅

`import { std::collections::sort, std::collections::sortUnstable, std::collections::binarySearch,
std::collections::lowerBound, std::collections::isSorted, std::collections::Order, … };`.

**Free functions over a `View<T>`, not methods on each container.** One implementation therefore serves
`DynamicArray`, `FixedArray` and any **sub-range** — `sort(items: xs.slice(from: 1, count: 4))` orders a
window and leaves everything outside it untouched, which a per-container `xs.sort()` could not express.
`View<T>` gained `swap`/`reverse` to support this: a view is second-class in *escape*, not in mutability
(it already writes through its place-returning `operator[]`), and putting the raw move there keeps every
algorithm above it safe. Descending order needs no API — `sort`, then `reverse`.

**Two guarantees, deliberately both.** `sort` is **stable** and `sortUnstable` is an in-place introsort
(median-of-3 quicksort, insertion-sort cutoff, heapsort depth fallback, so the worst case stays
O(n log n)). Stability is what makes sorting by a minor key and then a major key produce the intended
answer; an in-place sort is what an MCU or an audio callback can afford. The stable form sorts an **index
permutation** and applies it with swaps, which keeps it O(n log n) *and* free of a `Copyable` bound, so
move-only elements sort stably too — the cost is the `int32` buffers, and therefore the heap.
**`sort`/`sortWith` are `@compileFor(!NOHEAP)`**, so a `--no-heap` build does not silently reach the
allocator: they simply do not exist there, and the diagnostic says so.

**Ordering comes from a contract, never a function pointer.** `Comparable` gives the natural order;
`Order<T>` supplies any other, through `sortWith`/`sortUnstableWith`/`binarySearchWith`/`lowerBoundWith`.
A comparator is an *object*, so it may carry state (a key index, a direction, a collation table) — which
is what stands in for a capturing closure, since kama has none. It is also the faster choice: a
`C: Order<T>` bound monomorphizes to a direct, inlinable call, where an `fnptr` is an indirect call the C
compiler cannot inline (the reason `qsort` trails `std::sort`). `fnptr` could not express it in any case —
a function-pointer type takes no type parameters (ROADMAP_DETAIL §2).

Because a `ref` parameter may not name a smart pointer, `Order<Owned<T>>` is not instantiable: sort a <!-- xfail: ref_handle -->
container of the resources themselves. Searching splits what Rust folds into `Result<usize, usize>` —
kama's `Result<T, E>` constrains `E` to `Error`, so `binarySearch` returns `Optional<int32>` (the **first**
index of an equal run) and `lowerBound` returns the total insertion point.

`sync::{Mutex, RwLock, Once}` has no counterpart here and that is a **stance, not a gap** — the
shared-nothing isolate model means `Atomic<T>` is the one shared-mutable seam (see *Concurrency*).

### Parsing (`std::fmt`) ✅

`import { std::fmt::parse, std::fmt::parseRadix, std::fmt::ParseError };` — the exact inverse of this module's `intStr`/`f64Str`
side (`std.fmt.parseInt` is Zig's placement too).

```kama
Result<int32, ParseError> r = parse::<int32>(s: text);
```

**A parse fails, it does not come up absent**, so the result is `Result`, not `Optional` — GOALS #3d draws
exactly that line — and `ParseError` separates `Empty` / `InvalidDigit` / `OutOfRange`, because "not a
number" and "too big for this type" want different messages. Rust, Zig and Go all keep that distinction;
only the boolean and optional shapes discard it.

**One generic spelling, no `parseI32`/`parseI64` ladder.** The mechanism is the serde one — a marker
contract (`FromStr`) plus a per-type `type intrinsic` impl supplying a fallible `ctor`, reached as
`T.fromStr(...)`.
The turbofish is required because nothing in the arguments mentions `T`. Covers `int8`…`int64`,
`uint8`…`uint64`, `float32`/`float64` and `bool` (exactly `"true"`/`"false"`). `parseRadix` adds bases
2..36 for the integer widths, case-insensitive, with **no** `0x`/`0b` prefix — the base is already an
argument. Parsing is **strict**, as in Rust: no whitespace is trimmed and a trailing byte is an error, so <!-- test: parse_errors -->
`" 7"` and `"7x"` both fail. Floats go through `strtod` behind `kama_fmt.h`, whose checked entry point
reports `ERANGE` as `OutOfRange` rather than folding it to an infinity.

### ASCII (`std::ascii`) ✅

`import { std::ascii::isDigit, std::ascii::isAlpha, std::ascii::isSpace, std::ascii::toLower, … };` — `isDigit`, `isHexDigit`, `isAlpha`,
`isAlnum`, `isSpace`, `isUpper`, `isLower`, `isPunct`, `isControl`, `isAscii`, `toLower`, `toUpper`,
`digitValue`, all over `char`.

Named `ascii` rather than `char` for two reasons: `char` is a keyword, so `std::char` cannot be a module
path; and the name states the limit in every import line instead of a footnote. This is the same boundary
Zig draws with `std.ascii`, and full Unicode character properties belong in a package (see the Unicode
stance below). Every predicate is **false** for a non-ASCII codepoint rather than guessing, and
`toLower`/`toUpper` return one unchanged — so they can never corrupt one. Free functions rather than
methods because `char` and `uint32` share a C type and the conformance registry cannot hold both.

**Fixed-point — `Fixed<B> comptime(int32 F)`.** A signed binary fixed-point `type value` in the same module, for
FPU-less targets and for exact fractional arithmetic: `+ - * /` through operator overloading (multiply and
divide widen through `int64` and re-scale), `fromInt`/`toInt`/`fromFloat`/`toFloat`, and saturating
`satAdd`/`satSub`/`satMul`. The base operators trap on overflow like every other integer op above; the
`sat*` forms clamp. Pure library, no compiler support.

Both halves of the format are parameters. `B` is the **backing integer**, bounded by the `FixedBacking<B>`
contract (`int8`/`int16`/`int32`; `int64` cannot be one, because `wide()` widens *into* an `int64` and there
is no `int128`), and `F` is the fraction count as a **comptime parameter** — so `Fixed<int32>#(16)` is
the classic Q16.16 and `Fixed<int16>#(8)` is Q8.8. The backing is *passed*, not computed from a bit count:
kama has no type-level computation, and Rust's `fixed` and C++'s `fixed_point<Rep, Exponent>` pass storage
explicitly for the same reason. Pairing a fraction with a backing too narrow to hold it (`Fixed<int8>#(16)`)
is a compile error, from one [`comptime assert`](#compile-time-assertions--comptime-assert-) in the type's <!-- xfail: fixed_bad_pairing -->
own body reading `sizeof(B)` — not a rule the compiler knows about this type. See
[MCU_READINESS.md](MCU_READINESS.md) for the no-FPU story it belongs to.

**Arithmetic on two values of one type yields that type.** `uint8 + uint8` is a `uint8`, at every width —
kama's own rule, the one Rust, Swift and Go have, and *not* C's integer promotion, which would make the
result an `int` and every sub-`int` expression a narrowing on the way back out:

```kama
fn uint8 hexDigit(uint8 v) {
    if (v < 10ui8) { return 48ui8 + v; }   // `uint8 + uint8` : uint8 — no cast, nothing to convert
    return 97ui8 + (v - 10ui8);
}
```

Two consequences worth stating, because C answers both differently:

- The result **wraps** into its own type rather than surviving at `int` width. `uint8 a = 200ui8, b =
  100ui8;` makes `a + b` equal to `44`, and it is 44 everywhere — `cast<int32>(a + b)`, `(a + b) > 250ui8`
  and `(a + b) / 2ui8` all read the wrapped value. C would carry 300 until something narrowed it, so the
  two agree only where the value is immediately stored into a `uint8`.
- A **shift** takes its type from its LEFT operand alone. The right one is a count, not a co-operand, so
  the two need not agree: `int64 x; x << someInt32` is fine, as is `(c >> 4ui8)` on a `uint8`.

C's promotion is not part of kama's surface, which is the point: a reader should not have to know it to
predict which lines need a cast. The emitted C carries an explicit narrowing so the two agree.

**There is no implicit numeric conversion.** If two numeric types differ, the conversion is written down
— the Rust/Swift/Go rule. It applies in two places:

- **Wherever a value crosses into a destination of a stated type** — a local, a field, an assignment, a
  `return`, a `match` arm, an argument, an enum payload. `int8 a = big;` is an error wanting <!-- xfail: narrow_local, narrow_return, narrow_argument -->
  `cast<int8>(big)`.
- **Between an operator's two operands.** `int32 + uint8` does not compile, and neither does <!-- xfail: op_mixed_width, op_mixed_sign, op_compare_usize -->
  `int32 < usize`. That second one is the point: C answers `-1 < 1u32` with *false*, and a rule that
  covered assignments but not comparisons would leave the sharpest edge in place.

It is every crossing, not just narrowing — widening, a signedness flip and int/float in either direction
are all conversions. What is **not** a conversion, and needs no cast:

| | |
|---|---|
| a literal, typed by its destination — or by the other operand | `int8 a = 100;` · `float32 f = 3;` · `v < 10` on a `uint8` |
| arithmetic over literals, which is still the literal | `int8 a = 2 + 3;` |
| arithmetic on one type, which yields that type | `a + b` on two `uint8`s |
| a shift, whose count is a count and not a co-operand | `x << someInt32` on an `int64` |

A **named** constant is not a literal: `comptime int32 N = 5;` states a type, so `int8 x = N;` wants a
cast. A constant that does not *fit* its destination is rejected for that instead (`int8 a = 300;`). <!-- xfail: lit_oob_local, lit_oob_constref_argument -->

**A `ref` parameter is a destination like any other.** A literal has no address, so one bound to a
`const ref` is materialised into a temp — and that temp is the parameter's storage, typed by the
parameter: `a.contains(item: 2)` on a `DynamicArray<int64>` stores an `int64`, and the same call on a
`DynamicArray<isize>` stores an `isize`. It reads as a detail of the lowering and is not one. Typing the
temp from the literal instead handed the callee a four-byte slot to read eight bytes out of, and the
container answered `false` for a value it held (`tests/constref_literal_width.kama`, fixed in `0.9.138`).

### `isize` is the size type; `usize` is the C ABI

**A length, a count and an index are an `isize`** — every collection's `length()`/`count()`, every
`operator[]`, every index parameter, `string`/`Fixed`/`View` included. `usize` is reserved for quantities
crossing into C: `sizeof`, an allocation size, an `extern fn` mirroring a `size_t`.

`isize`/`usize` are the **only** platform-varying types in the language (`ptrdiff_t`/`size_t` — 8 bytes on
x86_64/arm64, 4 on wasm32/thumbv6m), which is why they keep `size` in their names: the name is what says a
crossing to a fixed width needs a cast, and `isize → int32` is a genuine narrowing on a 64-bit host.

The size type is **signed**, which is the part that is easy to get wrong. The intuition says a length
cannot be negative, so make it unsigned — but unsigned does not *prevent* the invalid state, it makes it
*unrepresentable*, so an erroneous negative becomes an enormous positive instead of an obvious `-1`:

```kama
usize len = 0;   usize last = len - 1;    // 18446744073709551615 — silently
isize len = 0;   isize last = len - 1;    // -1, which fails `< length` and trips a bounds check
```

kama traps signed overflow in every build and lets unsigned wrap (it is defined), so `usize` would put the
most common length expression, `len - 1`, in the one arithmetic domain with no protection. The collections
already relied on signedness: `operator[]` bounds-checks `i < 0 || i >= len`, a test that cannot be written
against an unsigned index. Go's `len() -> int`, Swift's `Int`, Python's `Py_ssize_t` (PEP 353) and C++20's
`std::ssize()` all landed in the same place; the unsigned camp (C, C++, Rust, Zig) predates the lesson.

**They carry the four value contracts, and deliberately not the two wire ones.** `isize`/`usize` implement <!-- xfail: serialize_isize_field, deserialize_usize_field -->
`Format`, `Hashable`, `Equatable` and `Comparable`, so a length can be interpolated, be a `Map`/`Set` key,
be `contains`-searched and be sorted — the four a size type needs, given that every `length()`, `count()`
and index is one. They implement **neither `Serialize` nor `Deserialize`**, and they are the only
primitives that do not: a stream is read by a program that is not the one that wrote it, so a field whose
width is `ptrdiff_t` would be 8 bytes written natively and 4 read on wasm32. A platform-varying width has
no wire format. Give a serialized field a fixed width (`int64`/`uint64`) and `cast` at the boundary;
`@generate(Serialize)` over an `isize` field is an error naming the field. <!-- xfail: serialize_isize_field -->

The same line divides everything else about them: they refuse `sizeof` folding at compile time and refuse
`bitcast` (an equal-width reinterpret needs a width), while `Format`'s widening `cast<int64>` needs none.
**What decides is whether the operation needs to know the width.**

**Bare `int` is not a kama type.** It was an alias for `int32` carrying no information of its own, and a
reader coming from C or Go would expect a *platform* width from the name — the opposite of what it meant.
Write `int32` for a fixed 32-bit integer, or `isize` for a size. **`double` is gone the same way** — it
aliased `float64`, and every kama float states its width. Neither `uint` nor `float` ever existed, but both
get the same diagnostic, because a C or Go reader will try them and "unknown type" would send them hunting
for a missing import instead of a different spelling.

**The rule holds through a binding.** A `foreach` element and a `match`-arm payload are typed values like
any other, so both of these are errors wanting a cast — they are not a hole the rule quietly skips:

```kama
foreach (int64 x in xs) { int8 n = x; }                              // error, not 44
int8 n = match (big()) { case Some(value: c): c; case None: 0i8; };  // error, not 44
```

Where kama cannot be certain of a type it still says nothing rather than guessing — a type parameter, a
comptime parameter, an `extern fn` result, an intrinsic with no declared return type, and a
value-producing `match` seen before its arms are bound. Silence there is deliberate: a rule built on a
classifier that confuses "this is a primitive" with "I have no idea" is either silent on every primitive or
fires on every unresolved name.

### Two types are two types, even when they share a representation

The rule above is about *width*. This one is about **identity**: a value must already have its
destination's type, and sharing a machine representation does not make two types one. It reaches the three
kinds of type that a C backend would otherwise let blur together.

**A plain `enum` is not an integer, and not another `enum`.** Its values are named variants, so:

```kama
type enum A : uint8  { A0, A1, A2 }
type enum B : uint8  { B0, B1, B2 }

A a = A::A2;
B b = a;              // error — two distinct enums, whatever tag width they share
uint8 n = a;          // error — an enum is not its underlying integer
A back = n;           // error — an integer names no variant until it has been checked
A z = 0;              // error — an enum value is written by name: `A::A0`
A c = a + A::A1;      // error — the result would be an `A` that is no declared variant
bool q = (a < A::A1); // error — ordering is `Comparable`, which an enum may implement
```

(`tests/xfail/identity_enum_cross_enum.kama`, `identity_enum_to_int`, `identity_int_to_enum`,
`identity_enum_int_literal`, `identity_enum_arith`, `identity_enum_order`, `identity_enum_cmp_cross`.)

The two directions have different doors, because they are not symmetric. **Enum → integer is total**, so
it is an ordinary cast: `cast<int32>(Code::Bad)`. **Integer → enum is fallible by construction** — an
arbitrary integer names no variant — so it is `try cast<Color>(x)`, yielding `Optional<Color>`, and the
`None` arm is where a byte off a wire gets handled. `==`/`!=` between two values of the *same* enum is
the one operator that stays; everything else goes through `match`.

**`char` is one Unicode codepoint, not a number.** `s[i]` is a `uint8` (a byte); `.chars()` yields
codepoints. The two never cross implicitly, in either direction:

```kama
char c = 'a';   uint32 u = 65ui32;
uint32 n = c;   // error — cast<uint32>(c)
char d = u;     // error — cast<char>(u)
char e = 65;    // error — write the character: 'A'
```

That last line is where contextual literal typing stops. `float32 f = 3;` is accepted because 3 *is* a
`float32`; `65` is not a codepoint, it is an integer standing in for one.
(`tests/xfail/identity_char_to_int.kama`, `tests/xfail/identity_char_literal.kama`.)

**A `fnptr` signature type is nominal.** Two signatures with the same shape are still two types, and
assigning one signature-typed value into another is an error — including, and especially, when the shapes <!-- xfail: identity_sig_cross_sig, identity_sig_arity -->
differ, since calling through a mismatched function pointer is undefined behavior that neither the C
compiler nor the sanitizers will report here. Assigning a *function* to a signature is checked
structurally and is unaffected.
(`tests/xfail/identity_sig_cross_sig.kama`, `tests/xfail/identity_sig_arity.kama`.)

What this rule does **not** touch: a contract destination (which admits every kind by design, so
`Hashable h = someInt32;` keeps working), an inheritance upcast, `Owned`/`Shared`/`Optional` promotion,
and any hand-off whose type kama cannot resolve — the same silence the width rule keeps. Every exemption is
pinned, running, by `tests/identity_exemptions.kama`. Binding a value to a contract it does *not*
implement is a separate error (`tests/xfail/contract_arg_nonconforming.kama` and its two siblings).

**No undefined behavior in arithmetic** (Rust's model). Every integer operation is *defined* — never C's
UB:
- **Signed overflow** (`+`/`-`/`*`) **traps** in debug builds (catches the accidental-overflow bug during
  development) and **wraps** two's-complement in release (`-fwrapv`, zero-cost, *defined* — not UB).
  Intentional signed wrapping is opt-in: `std::num`'s `wrappingAddI32`/`wrappingSubI32`/`wrappingMulI32`/
  `wrappingNegI32` (+ the `I64` set) always wrap and never trap (computed in the unsigned type), or just use
  unsigned math directly. **Unsigned overflow always wraps** (as C already defines).
- **Divide by zero** and **`INT_MIN / -1`** **trap** (a clean abort) in every build — always bugs, never UB.
  Divide-by-zero comes from the sanitizer; `TYPE_MIN / -1` is **emitted by the compiler**
  (`kama_sdiv_i32`/`_i64`), because `-fwrapv` does not define it and the release tier no longer carries
  the signed-overflow sanitizer. The check is on the *operands*, since `INT64_MIN / -1` overflows the
  width a result check would use (`tests/trap/intmin_div`, `intmin_div64`).
- **Shift ≥ the type width** **traps**; a **signed left shift into the sign bit** (`1 << 31`) is **defined**
  (computed in the unsigned type — a defined bit pattern), so bit-twiddling is safe.
- **Out-of-range `float → int`** **traps**; in-range truncates toward zero.
- **A narrowing `cast<T>(x)` whose value does not fit `T`** **traps**, in every build — the integer sibling
  of the line above, and the same policy for the same reason. `cast` preserves the *value*, so a value that
  does not fit is not a conversion but a different number: `int32 big = 300; int8 a = cast<int8>(big);`
  aborts rather than binding 44. A **constant** that does not fit is rejected at compile time instead. The <!-- xfail: cast_const_oob -->
  escapes are explicit and cost nothing: **`truncate<T>(x)`** keeps the low bits, **`try cast<T>(x)`** hands
  back `Optional<T>`. A widening, a same-type cast, and one whose operand provably fits emit no check at
  all, and where the check remains its bounds are compile-time constants — so the comparison that cannot
  fail folds away.
- **The signed-overflow rule holds at EVERY width** — `int8` and `int16` behave exactly as `int32` and
  `int64` do. This used to be a documented exception, and it is worth recording why, because the cause is
  not obvious: C promotes `int8`/`int16` operands to `int`, so `100i8 + 100i8` is computed as 200 where
  `-fsanitize=signed-integer-overflow` sees nothing at all, and kama's own narrowing of the result back to
  `int8` (the *D-arith* rule above) is a **conversion**, which the overflow check does not watch either.
  The value silently became `-56` while `int32 MAX + 1` trapped. The compiler now emits the check the
  sanitizer cannot: `+ - *` on a signed sub-`int` are range-checked against the operand type, trapping in
  debug and truncating in release, and `/` is checked in **every** build because the only division that can
  overflow is `TYPE_MIN / -1`, which the rule above promises unconditionally
  (`tests/trap/arith_overflow_i8`, `arith_overflow_i16`, `arith_div_i8_min`).
  **Shifts are deliberately exempt** — `<<` takes its type from the left operand alone and a signed left
  shift into the sign bit is *defined* (above), so `3i8 << 7i8` is `-128`, not an overflow
  (`tests/arith_same_type`). **Unsigned is untouched at every width:** wrapping is defined and stays silent.

Enforced by `-fsanitize-trap` (a bare `__builtin_trap`, no sanitizer-runtime dependency) + `-fwrapv` +
the `kama_lshift` runtime shim — so a kama program can't hit arithmetic UB whether built debug or release.
⚠️ **`-fsanitize=signed-integer-overflow` is passed in the DEBUG tier only**, and that is load-bearing
rather than incidental: it is not reliably suppressed by `-fwrapv` (Apple clang does not suppress it;
Ubuntu clang and gcc do), so passing it in release made overflow trap on one platform and wrap on
another *and* cost a compare-and-branch on every signed add and multiply — against an invariant that
calls release arithmetic C-parity. The release tier therefore relies on `-fwrapv` alone, with the one
case it does not define checked explicitly. `tools/check-release-arith.sh` asserts both the semantics
and the zero cost, on a tier the fixture suite cannot build.

```kama
import { std::math::Vec3, std::math::Mat4 };
fn int32 main() {
    Mat4 vp = Mat4.perspective(fovyRad: 1.0472f32, aspect: 1.777f32, near: 0.1f32, far: 100.0f32)
            * Mat4.lookAt(eye: Vec3.of(x: 0.0f32, y: 2.0f32, z: 5.0f32),
                           center: Vec3.zero(), up: Vec3.unitY());   // method chaining
    Vec3 p = vp.transformPoint(p: Vec3.of(x: 1.0f32, y: 0.0f32, z: 0.0f32));
    return cast<int32>(p.length());
}
```

### Standard I/O (`std::io` / `std::fs` / `std::net` / `std::process`) ✅

A native, single-binary I/O foundation — **library over FFI, no new language surface** beyond the prelude's
`enum Unit` (the empty `Result<Unit, E>` payload — one error convention for void-fallible ops). `std::io`
gives `IoError` + error classification; `std::fs` gives a RAII `File` (fd closed by its destructor) plus free
`readFile`/`writeFile`/`stat`/`readDir`/`remove`; `std::net` gives RAII `TcpListener`/`TcpStream` (blocking
TCP) and `UdpSocket`. All fallible calls return `Result<…, IoError>`, consumed by `match`.

**Subprocesses (`std::process`).** A `Command` builder — argv **vector** (never a shell string, so
injection-safe by construction; `Command.shell(line:)` is the explicit `sh -c` opt-in) with `cwd`/`env`/
`envClear` and per-stream `Stdio { Inherit | Piped | Null }` — spawns an owned, move-only `type resource
Process` via `start()`, or captures via the one-shot `run() -> Output { status, stdout, stderr }` (which
drains stdout+stderr **concurrently** through `std::net::Poller`, so a child that fills both pipes can't
deadlock the parent). `Process` gives `wait()` (blocking reap → `ExitStatus { code, signal, success() }`),
`tryWait() -> Optional<ExitStatus>` (non-blocking), `kill`/`terminate`/`signal`, and the piped streams as
`std::fs::File`s (`stdout()`/`stderr()` read, `stdin()`+`closeStdin()` feed-then-EOF). Dropping a `Process`
**never blocks**: it reaps a already-exited child (no zombie) or detaches it (the OS reparents to init) —
explicit `wait()` is how you get the status. **POSIX and Windows both ship**: `process.kama` is byte-identical
across platforms, with the whole difference behind `kama_os.h` (fork/execvp/pipe/waitpid vs `CreateProcess`),
and `run()`'s two-pipe drain sits behind one `kama_capture2` seam (`poll` on POSIX, a reader thread per pipe on
Windows) so both platforms take the same code path. wasm has no process model.

**The streaming byte substrate.** `std::io` also defines two contracts that unify every byte source/sink:
`type contract Writer` (the partial-write primitive `write(View<uint8>) -> Result<usize, IoError>` + `flush`)
and `type contract Reader` (`read(View<uint8>) -> Result<usize, IoError>`, `Ok(0)` = EOF). Buffers are always
`View<uint8>` (a non-owning span — zero-copy sub-slicing, no charset assumptions: binary-native, text backends
layer UTF-8 on top). Write-all looping, `pump` (Go `io.Copy`), and `readAll` are **free helpers** over the
primitive (contracts carry no default methods); `StringWriter`/`SliceReader` are the in-memory impls and
`BufWriter<W>`/`BufReader<R>` the buffering layer (each **owns** its inner sink/source by value — kama forbids
stored borrows). `std::fs::File` implements both, and a reliable network stream is
`type contract ReliableStream implements Reader, Writer` (refinement) + `setNonBlocking` — so `TcpStream` and
the web `WsConnection` are drop-in `Reader`/`Writer`s. The upshot: the serde backends and `fmt` stream over a
file or a socket with no transport-specific code (`decodeFrom<T>(from: someReader)`), and unbounded data moves
in bounded memory. (Datagram endpoints — `UdpSocket`, WebTransport — are message-oriented, not byte streams, so
they take `View<uint8>` buffers but do **not** implement `Reader`/`Writer`.)

```kama
import { std::fs::readFile, std::fs::writeFile };
fn int32 main() {
    match writeFile(path: "out.txt", data: "hi") {
        case Ok: {}
        case Err(error: e): { return 1; }
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

**FFI data — all controlled, no `unsafe fn` needed:**

```kama
extern "<stdlib.h>";                       // a C #include
type extern value div_t { int32 quot; int32 rem; }   // bind an external C struct (not re-emitted)
extern fn div_t div(int32 numer, int32 denom);

extern fn float64 frexp(float64 value, UnsafePtr<int32> exp);

fn int32 main() {
    div_t r = div(numer: 17, denom: 5);    // r.quot=3, r.rem=2  (field access on a C struct)
    int32 e = 0;
    frexp(value: 1764.0, exp: addr(of: e));// addr(of: x) = &x  — controlled out-param
    return r.quot + r.rem + e;             // 5 + 11 = 16
}
```

`type extern value Foo { ... }` is an **external** struct provided by an included header / linked code —
kama uses its fields (all public, the C layout) but never re-emits it (so no redefinition), and its name is
the literal C name. It has no ctor; construct it either by binding a struct-returning C fn (`div(...)`
above) or by **by-name aggregate init** — `div_t r = div_t(quot: 3, rem: 2)` sets the named fields
(unset fields stay zero; an unknown field name is a compile error). `addr(of: x)` takes the address of a <!-- xfail: extern_value_unknown_field -->
real local (out-params, descriptor pointers) — a *controlled* op, no `unsafe fn` needed. `s.cstr()` yields a C
`const char*`.

### Command-line arguments + environment ✅

A program reads its own command-line arguments and environment through a small, always-in-scope **prelude
floor** surface — no import, and it **survives `--no-std`** (the tier of `Optional`/`Result`/the smart
pointers). It is *not* an importable `std::env`: arguments enter through the compiler-synthesized `main`
wrapper (which stashes `argc/argv` into a runtime global before `kama_main` runs), so a `--no-std` user
cannot reimplement them — a core, non-reimplementable capability belongs in the floor. The user's
`fn int32 main()` signature is unchanged.

```
// arguments (argv[0] is excluded — see programPath())
foreach (string a in args()) { /* each user arg, in order */ }
int32 n     = args().count();              // number of user args
string first = match (args().get(at: 0)) { case Some(value: v): copy v; case None: ""; };

// program identity — three separate accessors, not part of args()
Optional<string> inv  = programInvocation();  // argv[0] verbatim, e.g. "./myapp" (exact launch string)
Optional<string> name = programName();        // basename of argv[0], e.g. "myapp" (usage text / dispatch)
Optional<string> path = programPath();        // OS-resolved absolute exe path (find files / re-exec)

// environment — a keyed lookup, not a list
Optional<string> home = env(name: "HOME");                 // None when unset
string term = envOr(name: "TERM", dflt: "dumb");           // value, or the fallback
```

- **`args() -> Args`** yields the user arguments **excluding** argv[0]. `Args` is a single value handle that
  both `foreach`-iterates (`implements Iterator<string>`) and supports random access (`count()` +
  `get(at:) -> Optional<string>`) — one handle covers both, because the floor cannot hand back a
  `std::collections` type (collections are collected *after* the prelude). Each yielded / returned string is a
  fresh **owned** copy (`give` it onward, or read it in place).
- **`programInvocation() -> Optional<string>`** is **argv[0] verbatim** — the exact string the program was
  launched with (`./myapp`, `/usr/bin/myapp`, or a bare `myapp`), unmodified. For logging fidelity or code
  ported from Go (`os.Args[0]`) / Rust (`args().next()`). `None` only where there is no argv[0] (firmware).
- **`programName() -> Optional<string>`** is the **basename of argv[0]** — the name the program was invoked
  as (`/usr/bin/app` → `app`), what usage/error messages print and what a busybox-style multi-call binary
  dispatches on. Kept separate from `args()` (it is not a user argument). Spoofable (it is whatever the
  caller put in argv[0]); `None` only where there is no argv[0] (bare-metal firmware).
- **`programPath() -> Optional<string>`** is the **OS-resolved absolute path** to the running executable —
  reliable for locating sibling files or re-exec'ing (queried from the OS via `/proc/self/exe` /
  `_NSGetExecutablePath` / `_get_pgmptr`, *not* argv[0]). `None` where there is no such notion or no portable
  query: **wasm** (a JS/browser host), **bare-metal firmware**, or an unsupported platform (a console port
  adds its own branch). The `Optional` return is what makes the MCU/wasm "not available" honest.
- **`env(name) -> Optional<string>`** is the primitive (a keyed lookup — the environment is exposed as a
  by-name query, not an enumerable list); **`envOr(name, dflt) -> string`** is the common fallback wrapper.
- **Embedded (`--target embedded`).** There is no argv/environ on bare metal, so `kama_args_init` and the
  accessors are no-op **stubs** behind `#if KAMA_TARGET_EMBEDDED` (`args()` empty, `env()`/`programName()`
  `None`) — the surface still compiles, with **zero libc linkage** (`getenv` is a block-scope extern, elided).
- **`kama run -- <args>`** now delivers arguments end-to-end (the passthrough was inert before this landed).

### Logging (`std::log`) ✅

Leveled, tagged diagnostics — the configurable logger, **a library over a small runtime seam, no new language
surface**. The floor gives `print`/`eprint` (raw console output) and `assert`/`panic` (fatal checks);
`std::log` is the tier above: filterable, taggable, redirectable output that **keeps running** (a `warn` is a
log level, never an abort). Import it — the module is the discovery unit; it is not scattered as floor globals.

```kama
import { std::log::logInfo, std::log::logWarn, std::log::logError, std::log::logDebug, std::log::logTrace, std::log::logEnabled, std::log::setLogSink, std::log::LogLevel };

fn int32 main() {
    logInfo(tag: "boot", msg: "starting ${version()}");   // tag may be "" (untagged)
    logWarn(tag: "net", msg: "returning");
    logDebug(tag: "audio", msg: "mix ${dumpState()}");    // dumpState() runs ONLY if the record passes (v2)
    if (logEnabled(level: LogLevel::Debug, tag: "audio")) {   // logEnabled remains — for guarding a whole block
        prepareDump();
        logDebug(tag: "audio", msg: "mix ${dumpState()}");
    }
    return 0;
}
```

**Two axes, either suppresses a call.** `enum LogLevel { Error, Warn, Info, Debug, Trace }` (ordered — Error
most severe, Trace most verbose) is the **level**; a free-text string is the **tag**. A call at level `L`
prints when `L <= threshold(tag)`; the default threshold is **Info** (so Error/Warn/Info print, Debug/Trace are
suppressed until raised). Both are reconfigurable **at runtime on a shipped binary** — the QA/live-debug win
most compile-time loggers discard.

**Configuration (runtime).** One grammar, `warn,audio=debug,net=trace` — a leading bareword is the global
threshold, each `tag=level` overrides one tag (levels `error`/`warn`/`info`/`debug`/`trace`, plus `off`). Two
sources, `--log` **primary**, `KAMA_LOG` env **secondary**:

```sh
KAMA_LOG=debug ./app                 # env: global debug
./app --log warn,audio=debug         # flag: global warn, but the "audio" tag at debug
KAMA_LOG=info ./app --log=off         # the flag wins (overrides the env) → silence
```

The config source is the **process-global env**: every translation unit / isolate reads `KAMA_LOG` into its own
module-scoped static and gets a consistent answer (argv is a module-scoped static, so the `--log` flag is
bridged into `KAMA_LOG` once in `main` — see `kama_log_init_args`; only programs that `import std::log` emit
that call).

**Baked project default (`kama.json`).** A shipped binary has no `kama.json` beside it, so a project's default
filter is compiled in. The manifest gains a `log` section — a JSON object mirroring the same level vocabulary:

```json
{ "log": { "level": "warn", "tags": { "audio": "debug", "net": "trace" } } }
```

The compiler translates it to the canonical spec (`warn,audio=debug,net=trace`) and seeds it into `KAMA_LOG`
in `main` *only if the env is unset* — so the full precedence is **`--log` > `KAMA_LOG` env > baked `kama.json`
default > the built-in `info` floor**. Invalid level names are a manifest error at build time.

**Swappable sink.** The default sink writes `[LEVEL] tag: msg` to **stderr** (kept off stdout so a CLI's real
output stays clean), colored on a tty. Install your own — the filter runs upstream, so a sink only ever sees
*enabled* records:

```kama
fn void mySink(int32 level, string tag, string msg) { /* route to a file / engine console / telemetry */ }
setLogSink(s: mySink);   // set once at startup, before spawning isolates — like setPanicHandler
```

Modeled on `setPanicHandler` (a runtime-held slot), **not** a stored `Logger` object: a kama resource can't be
a module-static and an `UnsafePtr` to an interface isn't dispatchable, so the facade calls the extern
`kama_log_dispatch`, which invokes the C-held slot (or the built-in console default). On `--target embedded`
output routes through the same weak `kama_log_sink` as `print` (freestanding, no libc); config falls back to
the Info default (no argv/env on bare metal).

**Zero-cost when filtered (v2 lowering).** The compiler **recognizes** the five facade calls and lowers each to
a guard with the **message built inside** it — so a filtered-out record never assembles its (possibly
expensive) message:

```kama
logDebug(tag: "audio", msg: "mix ${dumpState()}");   // dumpState() runs ONLY if the record passes the filter
```

Two axes reach zero cost. **Level, compile-time:** under **`--release`** a `Debug`/`Trace` call is stripped
*physically* (like `debugAssert` — gone at any `-O`; `Error`/`Warn`/`Info` stay). **Level(above the floor) + tag,
runtime:** an inlined `kama_log_enabled(level, tag)` guard (the same filter, reading the process-global config)
wraps the message build. So `logEnabled` is no longer a manual necessity — it stays available, but the guard is
now automatic at every recognized call site. To make a whole subsystem *physically absent* regardless of runtime
config, gate its declarations with `@compileFor(FLAG)`. This is an **AST lowering, not a preprocessor** — typed,
hygienic, one grammar (the `"${x}"`/`assert`/`print` shape), exactly like Rust `log`/`tracing`.

### `unsafe fn` — raw pointer memory access

The **only** place kama can touch arbitrary memory through a raw pointer. Raw `UnsafePtr<T>` index/store is a
**compile error outside** an `unsafe fn` — so the entire dangerous surface is explicit and greppable, and
greppable *at the declaration* (`grep -rn 'unsafe fn'`) rather than buried in a body. Everything else
(collections, smart pointers, FFI structs/handles/out-params, `addr`) stays safe.

**`unsafe` marks the BODY, not the caller.** This is C#'s meaning of the word, not Rust's: it says *this
function does dangerous things inside*, so **calling an `unsafe fn` is unrestricted** and its visibility is
ordinary. `public unsafe fn` is the common shape, not a contradiction — the function's *signature* is the
safe boundary, so a caller needs no permission. There is no propagation and no caller obligation. (Rust's
`unsafe fn` means the opposite — *calling this is dangerous, the caller must uphold an invariant* — which is
what forces containment there. kama does not take that meaning.)

It is markable wherever a body exists: a method, a `ctor`, a destructor, an `operator`, and a free function.
It is **rejected where no body exists** — on a type, on a field, on an `abstract` method, and on a `contract` <!-- xfail: unsafe_on_type, unsafe_on_field, unsafe_on_contract_member -->
member — because there is nothing there to be unsafe. A contract member is a *conduit*: the implementation
whose signature names `UnsafePtr` must itself be an `unsafe fn`, and a caller cannot invoke the member
without holding an `UnsafePtr`. That is what keeps `A: Allocator` a perfectly safe **bound** while
`allocate`/`deallocate` stay uninvocable outside an `unsafe fn`.

```kama
unsafe fn int32 sum(UnsafePtr<int32> p) {
    p[0] = 10;  p[1] = 32;       // raw store  (p[0] is *p)
    return p[0] + p[1];          // raw read
}

fn int32 caller(UnsafePtr<int32> p) {
    return sum(p: p);            // calling an unsafe fn needs no ceremony — the signature is the boundary
    // p[0] = 1;                 // ERROR here: "raw pointer access requires an `unsafe fn`"
}

FixedArray<float32> verts = ...;
UnsafePtr<float32> data = verts.dataPtr();   // SAFE to obtain (Rust as_ptr rule)
usize n = cast<usize>(verts.length()) * sizeof(float32);   // byte count: there is no `byteLen()`
// ... pass (data, n) to a C upload fn; dereferencing `data` still needs an `unsafe fn`
```

#### What requires an `unsafe fn` — the decision table

**One rule, and it keys on the TYPE, not the spelled token: an expression, declaration, or binding whose
type IS or CONTAINS `UnsafePtr<T>` may only occur inside an `unsafe fn`.** Plus one more: **calling an
`extern fn` requires one too.**

| construct | example | verdict |
|---|---|---|
| declare an `UnsafePtr` **field** | `UnsafePtr<T> data;` | **legal** — every container and every `type extern value` depends on it |
| module **static** of `UnsafePtr` type | `static hardware UnsafePtr<uint32> gpio;` | **legal** — the MCU path depends on it |
| **read or write** such a field or static | `this.data` | `unsafe fn` only |
| a **local** of raw-pointer type | `UnsafePtr<T> p = …;` | `unsafe fn` only |
| a **signature** naming `UnsafePtr` | `fn UnsafePtr<T> dataPtr()` | legal **iff the function is `unsafe`** |
| a **contract MEMBER** naming it | `ctor adopt(UnsafePtr<T> raw)` | legal, **no marker** — bodiless; implementer and caller are each forced by their own types |
| bind one **without spelling it** | `match (a.allocate(…)) { case Some(value: p): … }` | `unsafe fn` only — the rule reads the **type** |
| a **call whose result** is raw | `kfree(p: make())` | `unsafe fn` only |
| `addr(of: x)` | `gpio = addr(of: led);` | `unsafe fn` only — it *produces* a raw pointer |
| `cast<UnsafePtr<T>>(…)` | `cast<UnsafePtr>(0x40021000)` | `unsafe fn` only; stays possible for MMIO |
| compare against `null` | `if (this.handle != null)` | `unsafe fn` only — it reads a raw-typed place |
| **declare** an `extern fn` | `extern fn int32 abs(int32)` | no marker — bodiless |
| **call** an `extern fn`, **scalar-only included** | `kama_close_socket(fd: fd)` | `unsafe fn` only |
| inline `asm(…)` | | `unsafe fn` only |
| **call** an `unsafe fn` | | unrestricted |
| `unsafe` on **`main`** | `unsafe fn int32 main()` | **rejected** — see below |

Two of these are worth their own sentence, because the obvious weaker version of each is wrong.

**The rule reads the type because a token rule leaks.** `match (a.allocate(bytes: n)) { case Some(value:
p): … }` binds an `UnsafePtr` and never spells the word — the payload's declared type is the template's
`T`. Before the type-keyed rule existed, that shape compiled into a double free with zero `unsafe` tokens
in the function.

**`extern` has no scalar exemption.** `extern fn int32 kama_close_socket(isize fd)` names no pointer and is
a double-close primitive by effect; 87 of the 188 stdlib extern declarations are pointer-free. Danger at
this boundary is a property of the callee's *effect*, which kama cannot see, not of its signature, which it
can — so a type-based carve-out would look like a rule and behave like a hole.

**`main` may not be `unsafe`.** It encloses the whole program, so the marker would put every line in <!-- xfail: unsafe_main -->
the trusted region and stop marking anything — the same shape as an `extern fn` call that needed no
marker at all. Since calling an `unsafe fn` from safe code is unrestricted (above), the fix is always
available and always better: move the raw work into a helper `unsafe fn` and call it from `main`, so
the marker names the region that actually needs it.

**Definite assignment inside an `unsafe fn`: locals relax, `out` parameters do not.** The split is
load-bearing. Relaxation exists because a raw store is invisible to the definite-assignment walker, so an
unsafe body's own initialization dance would otherwise read as a use-before-assign. Filling an `out`, by
contrast, is a contract with the *caller* — and since safe code may call an `unsafe fn` freely, relaxing it
would hand every caller a hole full of uninitialized stack. A raw fill still counts: `addr(of: dst)` marks
its target assigned, which is how a type-erased C call satisfies the rule.

`a.dataPtr()` bridges a collection's buffer to C (safe to call; the returned `UnsafePtr` is valid only
while the collection is alive + unmodified, and dereferencing it requires an `unsafe fn`). An unlowered construct
(including a safety-gate violation) is a **hard build error** — kama never emits incomplete C and claims
success.

Moving an **owned** value into a raw slot uses `give`: `buf[i] = give w;` (inside an `unsafe fn`) stores the bytes and
**consumes** `w` (its scope-drop is skipped — a use-after-move is a compile error), the one marker that <!-- xfail: use_after_move -->
carries ownership across into unsafe manual storage. An *unmarked* `slot[i] = x` is a plain bitwise store
(the untracked raw-relocate a container uses internally, e.g. moving elements between buffers). Getting a
value back *out* is manual (bitwise-copy into a local, take responsibility) — there is no `give`-out of a
raw slot; a safe `Slot<T>`/`MaybeUninit` wrapper for both directions is a tracked design spike.

That move-out direction has one name: **`std::ptr::relocate(from:, at:, into:)`**, which bitwise-moves
`from[at]` into an `out` parameter. `std::ptr` is the raw-pointer module, kept apart from `std::memory`
(the *owning* handles `Owned`/`Shared`/`Weak`) and imported explicitly. The source is left **stale** — the
bytes are still there — so the caller must vacate it (tombstone the entry, decrement the length) or the
value is dropped twice. Every container's move-out goes through it, which is what keeps `addr(of: …)` on a
[`slot`](#uninitialized-storage--slot-) out of the call site.

### Inline assembly — `asm("...")` ✅

Some operations have **no C-level equivalent**: `wfi`/`wfe` (idle-sleep), `cpsid i`/`cpsie i`
(interrupt-masked critical sections), `dsb`/`dmb`/`isb` (memory barriers), cycle-exact delays. `asm(...)`
emits them directly.

```kama
unsafe fn void idle() {
    asm("wfi");                        // -> __asm__ __volatile__("wfi" : : : "memory");
    asm("cpsid i\n\tdsb");             // multiple instructions in one \n-separated string
}
```

- **A statement** taking exactly **one string literal** (no interpolation — the text must be literal).
- **Requires an `unsafe fn`.** Inline asm is the ultimate raw operation, so it lives in the same explicit,
  greppable seam as raw-pointer access. `asm(...)` outside one is a hard build error
  (*"inline `asm(...)` must be inside an `unsafe fn`"*).
- **Always volatile + a memory clobber.** Every `asm(...)` lowers to `__asm__ __volatile__("…" : : :
  "memory")` — never optimized away or reordered, and **also a full compiler memory barrier**, so
  `cpsid i`/`dsb`/`dmb` are correct by default (without the clobber the compiler could hoist memory ops
  across them — a silent footgun). A `nop` delay with a memory clobber is harmless. There is no
  non-volatile / no-clobber form (one way, safe default).
- **Portability is yours.** The text is target-specific; like FFI, `asm(...)` breaks "runs anywhere C
  runs." It is allowed anywhere inside an `unsafe fn` (not gated to `--target embedded`).
- Curated named helpers (`wfi()`, `disable_interrupts()`, `barrier()`) are an ordinary **library** built
  on this primitive — the unsafe-core / safe-API-as-library model. Extended asm with operand constraints
  and `@naked` functions are tracked follow-ons.

### Conditional compilation — `@compileFor(FLAG)` ✅

Tag a whole **declaration**; the compiler keeps or drops it for the active build. There is **no
in-body branching** — no `static if`, no `#ifdef`/`comptime-if` soup. Build-mode (`DEBUG`/`RELEASE`)
and platform (`OS_WINDOWS`/`ARCH_WASM32`/…) are the **same primitive**: a decl-level keep/drop gate.

```kama
@compileFor(DEBUG)   fn void traceState(int32 s) { ... }   // gone entirely in a release build
@compileFor(!RELEASE) static int32 assertsRun;             // present in any non-release build
@compileFor(OS_WINDOWS, TELEMETRY) fn void ping() { ... }  // comma = AND (both flags active)
```

⚠️ **A platform flag is DERIVED FROM THE TRIPLE, and a built-in target NAME is not a flag.** The
derived set is `ARCH_<arch>`, `OS_<os>`, `ABI_<abi>`, the synthesized `HOSTED` and `SIMD128`, plus the
name of a target the *project* declared. So `NATIVE`, `WASM`, `EMBEDDED` and `WINDOWS` are **not gates
— they are undeclared flags**, and this is deliberate: `EMBEDDED` is a
shortcut for a triple family, so gating on it would gate on how the build was *spelled* and would
silently stop applying the moment a real board triple (`xtensa-none-elf`) was used instead.
`OS_NONE` is the fact, and it holds for both. ⚠️ **Getting this wrong fails silently in a loose
build**, which reads no manifest and treats an undeclared flag as simply inactive — so the
declaration is dropped and the only symptom is a missing symbol somewhere else, or nothing at all if
both sides were gated. A manifest build rejects the name outright. These four spellings were in this
document's own examples until 2026-09-01, and the first external project nearly shipped them off
these pages; `tools/check-compilefor.sh` now greps the docs for them, because the half of that guard
which proves the *compiler* rejects such a name is exactly what made the docs drifting invisible.

- **Where** — any top-level decl: `fn`, `type`, `enum`, module `static`, and the three **bodyless** forms <!-- test: compilefor_extern -->
  — `extern "<header.h>";`, `extern fn` and `fnptr`. Gating the extern pair is what lets two platform
  backends each own their own C header: a gated-out `extern "<h>";` emits **no `#include`** and
  contributes no driver link hint (`-lm`, `--js-library`), so an emscripten build never reaches for
  `<CoreAudio/CoreAudio.h>`. A bodyless form accepts `@compileFor` and **nothing else** — `@noheap` <!-- xfail: attr_on_extern -->
  gates allocation *in a body* and `@interrupt`/`@section` attach to *emitted code*, and a declaration
  with no body has neither.
  ⚠️ **A class member is NOT a gate site, and saying so is a hard error** — not a silent no-op. The <!-- xfail: compilefor_on_method -->
  prune pass walks top-level declarations only, so a member's `@compileFor` would never be evaluated
  and never stripped: the conditional would fail *open* on every build. Gate the whole `type` (which
  drops every member with it), or split the member into two types. (Per-member and per-`implements`
  gating is a later stage.)
- **Logic** — flag-set membership, a leading `!` (negation), and comma = AND. Full `&&`/`||`/parens are
  deliberately out (this is *tagging*, not an expression language).
- **Drop is literal** — a gated-out decl's symbol never exists; **no `#ifdef` reaches the emitted C**,
  the Kama compiler does the selection. A reference from kept code to a dropped decl is a normal
  unresolved-symbol error — gate both sides, or provide a same-named fallback for the complementary flag.
- **Platform via the contract seam** — a platform-agnostic `contract` + per-target `@compileFor`-gated
  `type` impls; exactly one survives per build. This is the tag-type abstraction boundary — one
  mechanism, not a second platform system.

```kama
type contract Clock for value { fn int32 tick(); }
@compileFor(!ARCH_WASM32) type value NativeClock implements Clock { ... }   // native build keeps this
@compileFor(ARCH_WASM32)  type value WasmClock   implements Clock { ... }   // wasm build keeps this
```

A flag and its negation, rather than two positive names, because that is what makes the pair both
**exhaustive and mutually exclusive** — exactly one impl survives on every target, including ones
nobody has built yet. (`HOSTED` would not serve here: emscripten has a libc, so it is hosted too.)
The working fixture is [`tests/compilefor_platform.kama`](../tests/compilefor_platform.kama). <!-- test: compilefor_platform -->

#### The file gate — `file @compileFor(FLAG);` ✅

The same primitive applied to a whole **compilation unit**, written as its **first line**: <!-- test: file_gate -->

```kama
file @compileFor(!ARCH_WASM32);     // this file is not part of a wasm build at all

import { std::io::print };
fn int32 platformValue() { ... }
```

It exists because a package build compiles **every `.kama` under its `source` root**, whatever the import
graph — which is what lets the manifest alone prove there is no unreachable module, and what left a
native-only file with no way to sit in a project that also builds for wasm. Gating declarations was not
enough: the file still had to *analyze* on every target, so an `extern` over a platform header or a type
built on one sank a build that was never going to run the code.

- **Excluded, not deactivated** — a gated-out file is never admitted to the compilation. Its declarations,
  its `export` list, its `import`s and its contribution to its module do not exist for that build. It is
  still **parsed** (that is how the gate is read), so a *syntax* error in it is an error on every <!-- xfail: file_gate_still_parsed -->
  target — deliberately, so gating a file does not quietly stop checking it.
- **First line only**, ahead of `import { … };`. The gate decides whether the file is in the build at all, <!-- xfail: file_gate_after_import -->
  so nothing may precede it.
- **`@compileFor` and nothing else** — `file @noheap;` is a hard error, not a silent no-op, for the reason <!-- xfail: file_gate_bad_attribute -->
  a bodyless `extern` gives: `@noheap` gates allocation in a *body* and `@interrupt`/`@section` attach to
  *emitted code*, and a file is neither.
- **A file the CLI names is a direct request** — if its own gate excludes it, that is an error rather than <!-- xfail: file_gate_operand -->
  a build that quietly produces nothing. A file the build *collected* (a manifest's source root, the
  language server's workspace) is skipped silently; selecting files is what collection is for.
- **Both sides, as always.** Gate a file and its complement (`FLAG` / `!FLAG`) so exactly one is admitted
  on every target. Importing a module whose files are *all* gated out is an error that says so, <!-- xfail: file_gate_module_all_gated -->
  rather than an unresolved name later in the file that did the importing.

`file` is a **contextual** keyword: one only in this position, an ordinary name everywhere else.
[`tests/file_gate.d/`](../tests/file_gate.d/) is the working example — two implementations of one
function, one gated per platform, returning the same value on the native and wasm legs.

**Flags** are reproducible — from the explicit build invocation, never ambient environment. They come
from two places: **single-select groups** (pick one value; its name becomes a flag) and the
**multi-select `flags` bag** (any number on at once).

- **`TARGET`** — the platform this runs on, selected with `--target`. Its value is a
  `<arch>-<os>-<abi>` **triple**, and each component becomes a flag: `ARCH_AARCH64`, `OS_LINUX`,
  `ABI_GNU`, plus `HOSTED` for any `os != none`. Built-ins: `HOST` (the default — this machine),
  `MACOS`, `WINDOWS`, `LINUX`, `WASM`, `EMBEDDED`. A project adds its own; anything containing `-` is
  taken as a bare triple, so `--target aarch64-linux-gnu` needs no config at all.
- **`BUILD_TYPE`** — `DEBUG` (default) / `RELEASE`, selected with `--release`/`--debug`.
- **`OUTPUT`** — `EXE` (default) / `SHARED` / `STATIC` / `OBJECT`.
- **User flags** — repeatable `--define NAME` / `--undefine NAME`, declared in `kama.json`.

Gate on the **derived** flag rather than a target name: `@compileFor(OS_NONE)` holds for `EMBEDDED`
*and* for a real board triple like `xtensa-none-elf`, whereas a built-in name describes only how the
build was spelled — which is why built-in target names are not flags at all.

**`kama.json`** (a *user-project* file, named as the build's **operand** — `kama build kama.json`)
declares the valid user-flag universe and any extra groups, and turns on **strict validation** — an
undeclared `@compileFor`/`--define` name is then rejected (typo protection). A build that names `.kama`
files instead is a *loose* build: it reads no manifest at all, so it is permissive (an undeclared flag is
simply inactive) and bare single-file builds need no config. There is no auto-discovery and no
`--config` flag — the operand says which of the two you meant, which is the rule everywhere in the CLI.

```json
{ "name": "myapp", "version": "0.1.0",
  "select": {
    "TARGET":     { "RPI":  { "triple": "aarch64-linux-gnu", "cc": "aarch64-linux-gnu-gcc" } },
    "BUILD_TYPE": { "FAST": { "inherits": "RELEASE" } },
    "CONSOLE":    { "XBOX": { "default": true }, "PS5": {} }
  },
  "flags": { "TELEMETRY": { "default": true }, "PROFILING": {} } }
```

A **single-select group** takes exactly one value — `--select CONSOLE=PS5 --select CONSOLE=XBOX` is an
error — and `inherits` pulls the base in with it, so `FAST` activates `RELEASE` and gets its
optimization/stripping behavior without redeclaring it. `kama.local.json` overrides defaults per
machine. Precedence: CLI > `kama.local.json` > `kama.json` > the built-in default. The practical
toolchain setup is in [targets.md](targets.md).

`kama.json` is the seed of the future package-management manifest (deps/versions). It is parsed by the
compiler driver (C++), not by the language's own JSON library — the compiler is not self-hosted, so its
build-time config can't run kama-level code.

### Writing a collection *in* kama — `sizeof`, `panic`/`assert`, place-returning methods ✅

The above pieces (a place-returning `operator[]`, `UnsafePtr<T>` + `unsafe`, generics, RAII) let a `Vec`/matrix
be written **in the language** rather than baked into the compiler. Three builtins complete the kit:

- **`sizeof(T)` / `alignof(T)`** — the byte size / alignment of a type (a `usize`); both monomorphize, so
  `malloc(n: n * sizeof(T))` works in a generic `Vec<T>`, and `alignof(T)` (→ C `_Alignof`) serves aligned
  DMA buffers / register-block layout asserts. **`sizeof` folds to a compile-time constant for a
  fixed-width scalar** — `int8`…`int64`, `uint8`…`uint64`, `char`, `float32`, `float64`, including through
  a bound type parameter (`sizeof(T) * 8` inside a generic) — so it can drive a `comptime` initializer or a
  comptime argument. It does **not** fold for anything whose size the target decides rather than the
  language: `usize`/`isize` (C `size_t` — 4 bytes on wasm32/thumbv6m, 8 on x86_64), `bool`, `string`, and
  user aggregates, whose layout belongs to the C compiler. **`alignof` never folds** — alignment is an ABI
  choice, not a language guarantee (1 on AVR; `_Alignof(double)` is 4 on i386). Both keep working
  everywhere a runtime value works. The premises the fold rests on (`CHAR_BIT == 8`, and `float`/`double`
  at 4/8) are `_Static_assert`ed in `kama_runtime.h`, so the C compiler verifies them for the real target
  on every build. What kama will not fold, **[`comptime assert`](#compile-time-assertions--comptime-assert-)**
  lets you assert anyway — it hands an aggregate/`alignof` predicate to the C compiler as a `_Static_assert`.
- **The three conversion verbs, told apart by what each PRESERVES.** One conversion needs one spelling, so
  the verb is chosen by intent and the compiler holds you to it:

  | verb | preserves | width rule | can fail? |
  |---|---|---|---|
  | `cast<T>(x)` | the **value** | any | **yes — traps** |
  | `truncate<T>(x)` | the **low bits** | target narrower or equal | no |
  | `bitcast<T>(x)` | **all the bits** | **same width** | no |

  **`cast<T>(x)` traps on a value that does not fit `T`**, in every build — see *No undefined behavior in
  arithmetic*. `truncate<T>(x)` is the wrapping form and is **required, not a convenience**: masking first
  cannot express it (`cast<int8>(x & 0xFF)` yields 0..255, which is itself outside `int8`, so it would trap
  in turn), and every language that traps ships a named truncating form — Swift `truncatingIfNeeded:`, Zig
  `@truncate`, C# `unchecked`. A **provably widening** `truncate` is rejected: there are no high bits to <!-- xfail: truncate_widening -->
  drop, so `cast` is what was meant. **`try cast<T>(x)`** is the fallible form, yielding `Optional<T>` —
  `None` exactly where `cast` would trap. Like `try new` it needs a **declared `Optional<T>` destination**,
  because that is where its result type comes from — a local (`Optional<int8> r = try cast<int8>(n);`), an
  argument (`f(x: try cast<int8>(n))`), or the function's return type
  (`fn Optional<int8> narrow(int32 n) { return try cast<int8>(n); }`) — and nowhere else
  ([tests/try_cast_value_position.kama](../tests/try_cast_value_position.kama),
  [tests/xfail/trycast_no_destination.kama](../tests/xfail/trycast_no_destination.kama)).
  `truncate` is a **contextual** keyword: a keyword only where a conversion can start, so
  `string`'s `truncate(maxBytes:)` — and any method of that name — still reads as a member.
- **An `enum` is not a `cast` target — `try cast<E>(x)` is the one door in.** The table above is about
  numbers, whose valid values are a **range fixed by width**; an enum's are a **set of names the author
  chose**, so an integer arriving from outside has no reason to name one. That makes the infallible verb
  dishonest at both ends: when the value is known the variant already *has* a name (`Color::Green`), and
  when it is not the conversion is fallible by construction. So `cast<Color>(n)` is a **compile error** <!-- xfail: cast_enum_rejected -->
  naming both answers, and `try cast<Color>(n)` yields `Optional<Color>` — `None` when the value names no
  variant, tested against the declared discriminants rather than a range, so a value *between* two of them
  (`{ Ok = 3, Bad = 200 }` given 4) is `None` too. The **`enum` → integer** direction is untouched:
  `cast<int32>(Color::Blue)` is total. Rust draws the line in the same place — `200u8 as Color` is
  rejected, and so is `0u8 as Color`, because validity does not enter into it; Swift's `Color(rawValue:)`
  likewise hands back an optional. The payoff is that an enum holding a value that names no variant is
  **unrepresentable in safe kama**, so `match` never meets one: the invalid byte is handled as a `None`
  **arm**, by the same construct that reads the enum. Fixtures: `tests/try_cast_enum.kama`,
  `tests/xfail/cast_enum_rejected.kama`.
- **`bitcast<T>(x)`** — a **same-width bit reinterpret** of a numeric scalar, distinct from `cast<T>` (a
  *value* conversion): `bitcast<uint32>(f)` exposes a `float32`'s IEEE-754 bits, `bitcast<float64>(u)` builds
  a double from a `uint64`. Source and target must be **equal-width numeric scalars** (`int8..int64`/
  `uint8..uint64`/`float32`/`float64`); a width mismatch, a non-scalar, or an operand whose scalar type isn't
  statically known (bind it to a local first) is a compile error. Lowers to a no-UB ISO-C11 union type-pun. <!-- xfail: bitcast_width, bitcast_nonscalar -->
  It is the safe-surface primitive for binary formats / hashing / endianness (`std::num` `byteswapF32` rides
  it); raw-memory reinterpret of composites stays behind `unsafe`/`UnsafePtr`.
- **`assert(cond:, msg:)` / `debugAssert(cond:, msg:)` / `panic(msg:)`** — a clean **trap** (writes the
  message + `file:line` to stderr, then `abort()` — not UB, the user-facing form of the built-in bounds
  trap). `msg:` is **mandatory** (empty string allowed); a failed `assert` also **auto-appends the
  condition's source text** (`assert(cond: x > 0, msg: "")` → `assertion failed: x > 0 (f.kama:12)`).
  `debugAssert` is identical but **stripped under `--release`** (dev-only checks); `assert` is always-on.
  For a premise that can be settled before the program runs, use
  **[`comptime assert`](#compile-time-assertions--comptime-assert-)** — same arguments, checked at build. For
  a *bug that can't continue*; recoverable errors use `Result<T, E>`. A custom fatal handler (for a shipped
  game/GUI with no terminal) installs via **`setPanicHandler(handler:)`** — it runs for cleanup/exhibition,
  then the runtime still terminates. (kama aborts on panic — no stack unwinding; ≈ Rust's `panic=abort`.) The
  full always-in-scope surface is catalogued in **[FLOOR.md](FLOOR.md)**.
- **`drop(value: place)`** — run a place's destructor now (a no-op for a non-destructible type); lets a
  library owner over `UnsafePtr<T>` drop its heap pointee before `free`.
- **`addr(of: place)`** — the address of a place (a field/local/element) as an `UnsafePtr<T>`. Taking an address
  is safe (an `UnsafePtr` is safe to hold); dereferencing stays `unsafe`. Lets a library type hold a live
  back-pointer to another's field (e.g. an iterator to its container's mutation counter).
- **A place-returning method** — `public fn ref T at(usize i) { … }` returns a place, exactly like
  `operator[]`, so `v.at(i) = x` works. A `ref T` result must borrow `this` or a `ref` parameter (never a
  local — it would dangle), and it's second-class (used in-place, never stored).

**`foreach` over a user type — the iterator protocol.** A user container is `foreach`-able (not just the
built-in `InlineArray`) via a small **iterator protocol** — not indexing, so it works for any shape (list, tree,
map, range). It's zero-cost: monomorphized to **direct calls** (no vtable), and the container is
**borrowed, not consumed**. The container hands out an iterator via a nullary factory method, and that
**iterator must `implements` the matching prelude contract** — `foreach` is **nominal**: a type with the
right method shape but no `implements` is rejected (explicit over implicit). <!-- xfail: iter_no_contract -->
- **value** — `foreach (T x in v)`: `v` provides `fn <Iter> iterator()` whose iterator
  `implements Iterator<T>` (`fn Optional<T> next()` — `Some` per element, `None` at the end); or `v`
  *is* the iterator (`implements Iterator<T>` + a nullary `next()`).
- **mutable** — `foreach (ref T x in v)`: `v` provides `fn <IterMut> iterMut()` whose iterator
  `implements IteratorMut<T>` (`fn bool hasNext()` + a place-returning `fn ref T next()` — Rust's
  `iter()`/`iter_mut()` split; `Optional` can't carry a place, so mutable is a parallel iterator).
- A borrowing iterator holds an `UnsafePtr` cursor (its own `unsafe` internals); the `foreach` surface is safe.

The prelude contracts (`type contract Iterator<T> for value, view { fn Optional<T> next(); }` and
`IteratorMut<T>`) are ordinary monomorphized generic contracts, so they double as a static bound —
`fn sum<I: Iterator<int32>>(I it)` (zero-cost, direct `Concrete__next`) or a dynamic fat-pointer value
`Iterator<int32> it` (vtable). `foreach` uses the same `implements`, checked nominally.

Iterator safety: growing a collection (`add`) while iterating it would be a use-after-free when the
buffer reallocates. The library `DynamicArray` **guards against this at runtime** (C#-style): a modification
counter is bumped on every structural change (`add`), each iterator snapshots it, and `next()`/
`hasNext()` `panic`s if it changed — *before* the stale cursor is dereferenced. In-place element writes
(`foreach (ref x in list) { x = … }`) don't touch the counter and are fine — that's the point of `ref`.
The guard lives in `DynamicArray`'s own kama source (not the compiler), so it's a stdlib policy: a hand-rolled
container chooses whether to pay for it. `FixedArray`/`InlineArray` are fixed-size and can't reallocate, so they
need no guard. (The iterator's back-pointer to the counter uses the `addr(of: place)` builtin — the
address of a place as an `UnsafePtr<T>`; safe to take, `unsafe` to deref.)

**Every contiguous container hands out the same two bridges, `InlineArray` included.** `dataPtr()` <!-- test: inline_array_view -->
returns an `UnsafePtr<T>` for C (safe to obtain, `unsafe` to dereference), and `view()` returns a
`View<T>` for kama — which is what reaches `borrow`, `parallel_for` and every `View<T>`-taking algorithm
in the stdlib (`sort`, `sortWith`, `binarySearch`). This matters most for `InlineArray<T>#(N)`, the one
container that is stack-allocated, fixed-size and allocation-free — so the one a `@noheap` region is
obliged to use, and until it carried these two it was the one locked out of all of the above.
`view()` is also what lets a single signature serve every size: `N` is part of an
`InlineArray<T>#(N)`'s type (and of any `contract` that mentions one), while a `View<T>` carries its
length as a value, so `fn void fill(View<float32> block)` works for a 480-frame call and a 512-frame
call alike.

⚠️ **A view over an `InlineArray` is bounded by the same rules as any other**, and needs no extra
care despite the storage being on the stack: the escape checks key on the **view**, not on where the
buffer lives. Returning one over a local is rejected, and a view local still needs a `borrow` window <!-- xfail: view_escape_inline_array, view_local_inline_array -->
(`tests/inline_array_view.kama` shows both accepted spellings). `View<T>` is a stdlib type, so a
program that never imports it has no view to mint — the diagnostic says exactly that rather than
claiming the method does not exist. `dataPtr()` is unconditional.

### Explicit SIMD — `Simd<T> comptime(int32 N)` ✅

A **lane batch**: N numbers the CPU operates on as one value. It is an intrinsic value type, monomorphized
per `(T, N)`, lowering to a C `vector_size` typedef — so `a + b` is one machine instruction, not a loop.

```kama
Simd<float32>#(4) a = [1.0f32, 2.0f32, 3.0f32, 4.0f32];   // an array literal — lanes written out
Simd<float32>#(4) k = [10.0f32; 4];                        // the fill form IS a splat
Simd<float32>#(4) c = a * k + a;                           // elementwise; C's own operators
float32          x = c.lane(index: 2);                    // a bounds-checked lane read
Simd<float32>#(4) p = c.abs();
Simd<float32>#(4) lo = c.min(rhs: k);                      // also `max(rhs:)`
InlineArray<float32>#(4) back = c.toArray();               // back to addressable memory
```

- **Integer lanes obey kama's arithmetic rules, lane by lane.** A signed `+ - *` overflow **traps in
  debug and wraps in release**, exactly as the scalar of that type does, and a `<<` into the sign bit is
  defined the same way. ⚠️ None of that comes from the toolchain: `-fsanitize=signed-integer-overflow`
  does **not** instrument vector arithmetic, so the compiler emits the check (`tests/trap/simd_lane_overflow`).
  Unsigned lanes wrap at every width, as unsigned scalars do. Bitwise `& | ^ << >>` are C's own vector
  operators (`tests/simd_int_lanes`).
- **`sizeof(T) * N` must be exactly 16**, and the element must be a **numeric primitive**. 128 bits is the
  only width every kama target has — SSE2 on the x86-64 baseline, NEON on aarch64, wasm128 — and wider
  needs a `-march`-style CPU-tuning flag that does not exist yet. Both are compile errors <!-- xfail: simd_bad_width, simd_bad_elem -->
  naming the arguments, checked where the instantiation happens.
- **Construction is the array literal**, the same spelling `InlineArray` uses: one way to write "N values
  of T". `[v; N]` splats, which is the operation every other SIMD API names separately.
- **No `foreach`, no `v[i]`, no `set`.** Taking a lane batch apart one lane at a time is what the type
  exists to avoid; `lane(index:)` is the single read and `toArray()` is the way out to memory.
- **A target with no vector unit is not an error.** The operations lower to correct scalar code — which is
  the whole reason kama exposes a *type* rather than per-CPU intrinsics.
- ⚠️ **Reach for it only for what auto-vectorization cannot express.** An ordinary loop over `Vec4`s
  already compiles to SIMD on every target kama ships, and *better*: measured, a hand-written lane batch
  was **65% slower** than the scalar source for a cross product, because the compiler de-interleaves an
  array of structs and does four at once. `Simd` is for the operations that have no scalar spelling at
  all, which are these three:

```kama
Simd<float32>#(4) rev = a.shuffle(pattern: [3, 2, 1, 0]);        // an arbitrary permutation
Simd<float32>#(4) mix = a.blend(rhs: b, pattern: [0, 5, 2, 7]);  // two vectors: 0..3 from a, 4..7 from b
Mask<float32>#(4) gt  = a.greaterThan(rhs: b);                   // a lane mask, as a VALUE
Simd<float32>#(4) pick = gt.select(ifTrue: a, ifFalse: b);
float32 total = a.reduceAdd();     // also reduceMul / reduceMin / reduceMax
```

- **A shuffle pattern must be a literal**, and that is the hardware talking: `__builtin_shufflevector` <!-- xfail: simd_shuffle_runtime -->
  needs integer constant expressions because the CPU encodes the permutation *in the instruction*. A
  constant `wzyx` is two register ops; the same permutation from a runtime value spills the vector to
  the stack and rebuilds it with four scalar loads.
- **An out-of-range lane index is an error** — `shuffle` names one vector (`0..N-1`), `blend` names two <!-- xfail: simd_shuffle_oob -->
  (`0..2N-1`, where the upper half selects `rhs`).
- **`Mask<T>#(N)` carries `T`, not just `N`**, because a comparison's lane width follows its *operand's*:
  a `float32` compare yields 4-byte lanes and an `int16` compare 2-byte ones, identically on gcc and
  clang. A bare `Mask#(4)` would have no C type. Its lanes are all-ones/all-zeros **bit patterns**, so it
  carries **no arithmetic operators** — combine with `and(rhs:)`/`or(rhs:)`/`not()`, test with <!-- xfail: simd_mask_arith -->
  `anyTrue()`/`allTrue()`. That, and `select` living on the mask so it cannot be handed a data vector,
  is why it is a distinct type rather than a `Simd<bool>#(N)`.
- **`Simd` is not `InlineArray` and not `Vec4`.** An `InlineArray` is a *container* — indexed, iterated,
  lanes meaning whatever you decide. `std::math`'s `Vec4` is *geometry* — lanes named `x/y/z/w`, meaning
  different things, laid out for a GPU vertex buffer. A `Simd`'s lanes are *interchangeable*.

**`SIMD128` — are the lanes real?** A derived `@compileFor` flag, read off the resolved target like
`ARCH_AARCH64` / `OS_LINUX` / `HOSTED`. It is **not** "this target can compile a `Simd`" — every target
can, by degrading to scalars. It says the target has 128-bit vectors *in its baseline* (mandatory SSE2 on
x86-64, mandatory NEON on AArch64, wasm with `-msimd128`), so a library can pick a different **algorithm**
rather than hoping the fallback is fast enough (`tests/simd128_flag`):

```kama
@compileFor(SIMD128)   fn int32 sum4(InlineArray<int32>#(4) a) { … Simd<int32>#(4) … }
@compileFor(!SIMD128)  fn int32 sum4(InlineArray<int32>#(4) a) { … a scalar loop … }
```

Prefer it to a target name: `SIMD128` states the capability, and a name only implies it. (Gating on a
target name is *legal* — names are ordinary flags through the `TARGET` group, and that is how platform
implementations are selected — so this is guidance about which question you are asking, not a rule the
compiler enforces.)

The codegen — that this really becomes a machine vector rather than four scalars — is asserted by
`tools/check-simd-type.sh`, because a value fixture passes just as happily against a scalar fallback.

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
op(…) }` — the core callback shape), and it can be **stored and invoked later** — in a field or a module <!-- test: fnptr_stored -->
`static` — which is the callback-registry shape: install a handler now, dispatch through it on a later
call ([tests/fnptr_stored.kama](../tests/fnptr_stored.kama)). Binding is checked in **every** one of those <!-- xfail: fnptr_bind_arg_shape -->
positions, not only in a local's initializer: a shape-mismatched bind elsewhere used to compile, and the
call through it then passed the wrong number of arguments — an indirect call through a mismatched
function-pointer type, which is undefined behavior.

A signature may carry **`@noheap`**, which makes non-allocating part of the type: `@noheap fnptr int32 <!-- test: noheap_fnptr -->
Op(int32 x);` requires every function bound to it to be `@noheap` too, and in exchange a call through the
slot is legal inside a no-heap region — the escape hatch for the blind seam. See *No-heap subset*.

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
type value Scaler { int32 k; public ctor make(int32 k){ Scaler r; r.k = k; return give r; }
               public fn int32 apply(int32 a, int32 b){ return (a - b) * this.k; } }

Owned<Scaler>  s  = new Scaler.make(k: 3);     // (constructed as Owned)
BindableFunctionPtr<Compare> c  = new BindableFunctionPtr<Compare>(obj: s,  method: Scaler::apply);  // MOVE-in (sole owner)
Shared<Scaler> s2 = new Scaler.make(k: 2);
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
fnptr int32 Comparator(UnsafePtr<int32> a, UnsafePtr<int32> b);
extern fn void qsort(UnsafePtr buf, usize nmemb, usize size, CompareFn compar);
...
Comparator c = cmp;
qsort(buf: a.dataPtr(), nmemb: 4, size: 4, compar: cast<CompareFn>(c));   // cast to the header's fn-ptr type
```

## Exposing to a host — `expose` ✅

`extern` is the *host→kama* direction (kama calls C); **`expose` is the reverse** — it gives a **free
function** a stable, host-callable entry point. `expose fn …` emits the function under its **bare,
unmangled** C name (no `Namespace__` prefix — mirroring how `extern` keeps a literal name) decorated with
`KAMA_EXPORT` for external linkage that survives dead-code elimination:

```kama
// gameplay.kama — a hot-reload module (note: no `main`)
expose fn void update(UnsafePtr<World> w, float32 dt) { /* … */ }
expose fn int32 version() { return 3; }
```

- **Native shared library:** `kama build --shared gameplay.kama -o libgameplay.so` (→ `.dylib`/`.dll` per
  platform) builds a `-fPIC -shared -fvisibility=hidden` library where **only** the `expose`d symbols are
  visible. A host `dlopen`s it and `dlsym`s `"update"` / `"version"` — the reload loop
  (`dlopen`/watch/rebind over `unsafe`/`UnsafePtr`) is an ordinary library, not compiler magic. A `--shared`
  module needs no `main`.
- **WASM:** a normal `kama build --target wasm` run exports each `expose`d function
  (`KAMA_EXPORT` → `EMSCRIPTEN_KEEPALIVE`), callable from JS as `Module._update` — no `--shared` (it is
  native-only; the web host re-instantiates the module).

**Rules** (checked at compile time — a clear error, never a silent no-op):
- **Free functions only.** `expose` is not a member/type modifier; on a method/field/type it is rejected. <!-- xfail: expose_on_method -->
- **C-ABI-safe signature.** A param or return may not be an owned-by-value type — a kama `string`, a <!-- xfail: expose_bad_abi -->
  collection (`DynamicArray`/`FixedArray`/`Map`/`Set`/…), or an `Owned`/`Shared`/`Weak` smart pointer — since RAII /
  refcount state cannot cross a raw C boundary; pass an `UnsafePtr<T>` or an `extern` struct instead.
- **No generics / no `fn ref T` place-return** (no single concrete C-ABI symbol); **bare names are unique**
  across the program (they share the C namespace — clashes with libc are yours to avoid, as with `extern`).

`expose` is distinct from `export` (module public-surface visibility) and `public`/`private` (member
access): three boundaries, three keywords. *(The full 2.0 `expose` — richer wasm module exports and the
scripting-host interface — is future work; the keyword is live today for the C-ABI boundary above.)*

## Control flow ✅

`if/else`, `while`, `do/while`, `for`, `foreach`, `break`, `continue`, `return`; the full operator set
(`+ - * / %`, bitwise, shifts, comparisons, `&& || !`, ternary `?:`), assignment ops (`= += …`), `++`/`--`,
casts. Branching on an enum is done with **`match`** (see Enums & `match` below); arbitrary-integer branching
is done with `if` / `else if`. There is no `switch` statement.

**Every branch and loop body must be braced.** `if`, `else`, `while`, `do`, `for` and `foreach` each take a
`{ … }` block — never a bare statement, and never an empty `;`:

```kama
if (n > 0) { return 1; }        // ok — and a one-line body is fine, the rule is about the braces
if (n > 0) return 1;            // ERROR: the body of `if` must be braced
if (n > 0);                     // ERROR: binds the branch to nothing
```

A bare body is where `goto fail;`-shaped bugs live: a later edit adds a second statement, it indents as
though it belongs to the branch, and it does not. kama has no whitespace rule to fall back on, so the brace
is the only thing that can carry that meaning — requiring it makes the bug unrepresentable. The rule is
about **braces, not line breaks**: `if (x) { return; }` on one line stays legal, because a braced body
cannot silently acquire a second statement.

The one exemption is **`else if`**. The `else` arm accepts a block *or* another `if`, so a chain stays flat:

```kama
if (n > 100) { return 4; } else if (n > 10) { return 3; } else { return 0; }
```

That `if` **is** the branch — it cannot grow a sibling statement the way a bare body can — and requiring
`else { if (…) { … } }` would nest every chain for no safety gain. `scope`, `parallel_for` and
`match` are unaffected: they already require a block (a `match` arm's `case P: expr;` is an expression, not
a statement body).

## Type declarations — `value` / `resource` / `view` / `contract` / `enum` ✅

Every type declaration is introduced by the **`type` marker** followed by a *kind* — parallel to `fn` on
every function, so declarations are greppable and self-describing:

- **`type value Name { … }`** — owns nothing, **copies** freely (a `memcpy`; no hidden shared refs). Sealed
  (no `virtual`/`abstract`/`final`), no destructor. Fields default **private**; mark a field `public` per
  field (a `value` with all-public fields is a plain-old-data struct).
- **`type resource Name { … }`** — owns something, or has identity: **move-only**, RAII-dropped. Fields are
  **private only** (ownership stays encapsulated). An empty `type resource Token { }` is a valid move-only
  identity/token. Extensible variants add a qualifier after `type`: `type virtual resource`, `type abstract
  resource`, `type final resource`.
- **`type view Name { … }`** — a non-owning, **stack-only borrow** (a slice/span; C# `ref struct`). It
  **copies** like a value (inline, no dtor) but owns nothing and is a **second-class borrow**: the escape
  check forbids it as a field, a collection element, or an `enum` payload, and allows it as a **return only
  when it borrows `this` or a `ref`/view parameter** (the same structural rule as a `ref T` place-return — no
  lifetime tracking). A **local** of the kind must root in a `borrow` window or a by-value view parameter,
  and its host is frozen for that window's extent, so it cannot outlive the storage it views — see *Slices /
  spans* for the window rule. A view is also never a `ref`/`out` parameter: it is already a borrow. A view may **not** declare a `~dtor` or own a resource field, and
  its fields are **private only** (its raw `UnsafePtr<T>` must not leak). A view's **conformance is checked at the
  `implements` site**: it may not implement a contract whose `ctor` **member** constructs the implementer from <!-- xfail: view_contract_ctor_infallible, view_contract_ctor_escape -->
  parameters that carry no borrow (no `UnsafePtr<T>`, no `ref`, no view) — such a constructor could only borrow one
  of its own locals, so no body could satisfy it. A member taking something borrowable is fine, and an
  *instance* method returning the self-type is always fine (it borrows the receiver, like `View.slice()`).
  The check is a signature-level pre-filter for what no body could satisfy, not a replacement for the
  body-level escape check; it is also necessarily partial, since a **marker** contract declares no members at
  all (its factory lives in the impl) and stays caught later, at the boxing site. The flagship is the stdlib
  `View<T>`; the kind is general (`type view StridedView<T>`, `Grid2D<T>`, …). See *Collections & strings*
  for `View<T>`.
- **`type contract Name for <kinds> { … }`** — a public-only guarantee (an interface); signatures only, no
  bodies, no fields, no dtor. A `ctor` **may** be required (a conformer has to supply that constructor),
  which is what lets a bound construct: `T.fromStr(s: …)`. Types satisfy it via `implements`; it may refine
  another with `implements` too (`type contract Animated for value, resource implements Drawable { … }` — a
  conformer must supply Drawable's methods as well, and dispatch through `Animated` reaches them).
  The **`for` clause is mandatory** and names which kinds may implement the contract — see below.
- **`type enum Name { … }`** — a plain set of variants or a tagged union. See *Enums & `match`* below; it
  takes the same `implements` clause as every other kind.
- **`type intrinsic <targets> implements C { … }`** — the kind a **primitive** is. It declares nothing new;
  it decorates existing built-in types with a contract's methods, one block for a whole **set** of widths.
  See *`type intrinsic`* below.

The full model + rationale is in [TYPE_MODEL.md](TYPE_MODEL.md). The kind words `value` / `resource` /
`view` / `contract` are **contextual, not reserved** — because they appear only right after `type`, they
remain ordinary identifiers everywhere else (`int32 value = 5;`). `enum` is the one kind word that IS a
reserved keyword, for the historical reason that it predates the `type` marker; that costs nothing, since
nothing else could be spelled there. Only `type` and `enum` are keywords.

**`type` may still NAME a binding** — a field, a local, a module `static`, a parameter or a method — and
be read, written and passed like any other name. It is a **contextual keyword**: it leads a declaration
only where a declaration can begin, and is an ordinary identifier everywhere else. That puts it beside
`copy`/`give`/`truncate`/`default`/`base`, and beside the kind words above.

This exists for the FFI and could not be worked around there: `webgpu.h` calls a field `type` in four
structs, and **an `extern` struct emits the literal C name**, so unlike a function — whose kama binding is
simply renamed — the field is assigned BY NAME and renaming it emits the wrong C. Omitting it is not a
lesser evil: the field then holds 0, and 0 *is* `WGPUBufferBindingType_BindingNotUsed`, so an explicitly
built bind-group entry for a uniform buffer or a sampler is **inert** rather than under-specified.

```kama
type extern value WGPUBufferBindingLayout {
    public UnsafePtr nextInChain;
    public uint32    type;              // the literal C name — no escape, no rename
    public uint32    hasDynamicOffset;
    public uint64    minBindingSize;
}
b.type = WGPUBufferBindingType_Uniform;

fn void configure(uint32 type) { … }    // and it reads as an ordinary name everywhere else
configure(type: WGPUBufferBindingType_Uniform);
```

### The contract `for` clause — which kinds may implement it ✅

A `type contract` **must** declare its implementers: `type contract C for <kinds> { … }`. The clause takes
any combination of the **five implementable kinds**, comma-separated, meaning *any of these*:

```kama
type contract Rankable for value;                                  // one kind
type contract Iterator<T> for value, view;                         // a borrowing iterator is a view,
                                                                   //   a generating one is a value
type contract Hashable for value, resource, enum, intrinsic;       // anything that can be a Map key
```

`contract` is **not** among them: a contract implementing a contract is *refinement*, a different axis,
already spelled by `implements` on the contract itself. The separator is a comma and only a comma — `+`
was rejected because it means **conjunction** in a generic bound (`<K: Hashable + Equatable>` = satisfy
all) and would mean **disjunction** here, one symbol with opposite senses.

Every kind is enforced, and each is judged as what it was declared, not as what it lowers to: a
`type view` codegens like a `value` (same layout, same copy) but implements as a **view**, so a contract
that does not name `view` rejects it. `@generate`-synthesized conformances go through the same gate — a
`@generate(Serialize) type value` needs `Serialize` to name `value`.

Widening a clause is a **non-breaking** change and narrowing one is not, so state the kinds a contract is
*for*, not merely the ones implementing it today: the clause is a design statement, and a set narrowed to
the current corpus is easily narrower than the contract's audience. `Real` names `value` alongside
`intrinsic` for exactly that reason — nothing but `float32`/`float64` implements it yet, and a
user-defined soft-float value type is what it exists for.

*(`for both` is gone. It named an arbitrary pair the moment there were more than two kinds.)*

### What an `implements` clause is checked against ✅

An `implements` clause is a **promise about signatures**, and every part of it is verified at the
declaration — not at the call sites, and not by the C compiler:

- the member **exists**, and is **`public`** (a contract is a public guarantee; a private method
  satisfying it would be reachable through the contract but not by name);
- its **return type** matches the member's, including a `ref T` place-return;
- each **parameter type** matches, and so does the **arity**, each parameter's `ref`/`out`, and each
  parameter's **label** — kama call sites are label-based, so the label is part of the call surface the
  conformance promises, not decoration;
- the **receiver** matches: a contract member may be declared `static fn` (`Hasher::finish` is
  zero-state, so `H::finish(raw)` monomorphizes to a direct call), and `static` must then agree on both
  sides. A required `ctor` is exempt — one is static by construction;
- **`const fn`** on the member is honored, and so is **`const`** on a parameter. Only in one
  direction: an implementation may be *more* const than the member asks, which widens where it can be
  called and breaks nothing.

Matching is on the **resolved** type, so an alias, an import spelling, or a generic contract's
substituted parameter (`Iterator<T>` implemented at `T = int32`) compares equal — what differs is what
the two would lower to.

This applies to every kind that can conform, including an `enum`'s conformance and a `type intrinsic`
block's, to a contract-declared **operator** (`int32 operator+(int32 rhs)` — the generic-math bound
shape), and to a conformance that dispatches only statically. A **`@viewable`** contract is the one
exception: it emits no vtable and no fat-pointer type, so its members are nominal markers rather than
slots — `Iterable<T>` declares `fn Iterator<T> iterator()` and every container correctly returns its own
concrete iterator type.

**`override` keeps the same promise, and is checked the same way.** A derived method stands in for the
base's through the base's slot, so its signature must match in every position above. It is **not**
covariance-aware: a derived return type would be a language feature with its own rules and its own
lowering, and accepting "any subtype" here would let a hierarchy promise what the vtable cannot keep. It has to be checked here: a
contract's vtable slot is filled with a **cast**, so a mismatch is invisible to the C compiler at the
declaration and surfaces — if at all — at a use site, naming mangled types the author never wrote. A
mismatched member reached *through* the contract does not fail, it silently does the wrong thing
(returning `int64` through an `int32` slot yields the low 32 bits).

```kama
type value Counter {
    int32 value;                                     // fields are private by default
    public ctor make(int32 start) { Counter r; r.value = start; return give r; }   // named ctor (`public` to call from outside)
    public fn void add(int32 n) { value = value + n; } // method (implicit self)
    public fn int32 get() { return value; }
}
Counter c = Counter.make(start: 40);   // stack value — dot-on-type construction, not `new`
c.add(n: 2);                            // a `value` copies on hand-off
```

Fields, methods (take an implicit `self`), named constructors, field initializers (run in the ctor),
`this.field`, `obj.method(args)`. Lowers to a `struct` + `Counter__method(Counter* self, …)` functions.
Members are **private by default**; `new` is reserved for the heap (`Owned`/`Shared` element construction), so
a stack value uses `Counter.make(start: 40)`, not `new Counter.make(...)`. A constructor may also be called
**inline in a call argument** — `f(x: Counter.make(start: 5))` — it materializes a temporary passed by value (a
`value` copies, a `resource` moves); use a local for a `ref`/`out` parameter.

A type that owns a heap resource (a collection, an `Owned`/`Shared`/`Weak`, or another `resource`) is
declared **`type resource`** and is move-only:

```kama
type resource Buffer {
    DynamicArray<byte> data;                                 // owns heap → resource; fields stay private
    public ctor make(int32 n) { … }
    public fn isize size() { return this.data.length(); }
}
```

A `value` that transitively owns a resource is a **compile error** ("declare `type resource`"), and a `~dtor` <!-- xfail: value_owns_resource -->
is allowed only on a `resource` (`~dtor` ⟺ `resource` — a `value` owns nothing to free).

## RAII / destructors ✅

A `~Type()` destructor runs deterministically at scope exit, in reverse construction order, on every path
(block end, early `return`, `break`/`continue`). Destructible fields are destroyed in reverse declaration
order. No GC; allocation/deallocation is predictable.

## Construction ✅

**A constructor is a named factory that returns a fully-initialized object, or an error.** There is exactly
one kind, spelled `ctor` (no `fn`, no `static` — it is implicitly type-associated), and *everything* is one,
including deserialization and copying.

```kama
type resource Buffer {
    UnsafePtr<uint8> data = null;                                  // a field default states the empty value
    int32 size;
    public ctor make(int32 size) { this.size = size; }        // the value under construction is `this`
    public ctor withCapacity(int32 n) { return Buffer.make(size: n); }   // reuse = an ordinary call
}
Buffer b = Buffer.make(size: 8);          // dot-on-type: construction
Owned<Buffer> h = new Buffer.make(size: 8);   // `new` composes — heap, an owning handle
```

- **Dot-on-type is construction, and only that.** `Type.name(…)` constructs; `Type::staticFn()` and
  `Enum::Variant(…)` keep `::`. So `.make(` greps for construction and catches nothing else. There is **no
  nameless `Type(…)` call form** for a kama type — it silently dropped its arguments, and it is a hard
  error in every position (`Type(…)`, `new Type(…)`, `try new Type(…)`, `new(allocator: a) Type(…)`, and a
  reassignment `x = Type(…)`). Two things keep that spelling because it is the *only* spelling they have:
  a `type extern value`, where `div_t(quot: 3, rem: 2)` is by-name **aggregate init** of a C struct that has
  no constructor to name, and the intrinsic `new BindableFunctionPtr<Sig>(obj:, method:)`. A generic ctor
  puts the turbofish on the **type**: `T::<Args>.make(…)`.
- **Nothing is constructible by default.** A type with no `ctor` and no `of`/`zero` opt-in cannot be built, <!-- xfail: no_ctor_value, no_ctor_new, no_ctor_resource -->
  and the diagnostic is context-aware: it offers `of`/`zero` only for a transparent `value` (all fields
  public), never for a `resource`.
- **Reuse is a visible call.** A ctor delegates by calling another (`return Buffer.make(…)`). There is no
  `init` hook, no designated/final ctor, and no mandatory funnel — shared logic lives in the ctor others
  chain to, and it is greppable.
- **A self-returning `static fn` is rejected** as a disguised constructor; so is a class-named ctor <!-- xfail: self_returning_static_fn, ctor_declares_own_type -->
  (`public Buffer(…)`). Genuine static utilities returning *other* types (`Vec3::dot` → `float`) stay
  `static fn`.
- **A contract may require a `ctor`** — `type contract HeapOwner<T> for resource { ctor adopt(UnsafePtr<T> raw); }`
  — and generic code bounded by it may construct through the type parameter, monomorphized to the concrete
  implementer. That is why there is **no privileged `Default` contract**: "default construction" is just a
  contract requiring a zero-arg ctor.

### Complete initialization — enforced ✅

**A constructor must assign every field**, checked at compile time. The returned value is complete by
delegation when the ctor's terminating move is `return Other.make(…)`, so chaining stays clean. This is
kama's answer to "a returned object is always fully initialized" — it is proven, not conventional.

Two escape hatches, both **explicit and at the declaration** rather than hidden in codegen:

- a **field initializer** — `UnsafePtr<T> data = null;`, `int32 len = 0;` — states that field's default once, and
  it runs in every ctor (and for a bare local);
- **`@generate(zero)`** blesses a whole data bag's zero state (a transparent all-public `value`).

What is exempt is not a carve-out but a guarantee the compiler supplies: an **intrinsic collection**, whose
zero representation *is* its valid empty value, and a type with a **`default` ctor**, which the compiler
calls at the fill site. (A generic field could not spell the latter anyway — there is no expression for
"the default `A`".)

```kama
type resource Ring {
    UnsafePtr<uint8> data = null; int32 len = 0;   // stated defaults — every ctor inherits them
    int32 cap;                                // no default -> every ctor must assign it
    public ctor withCapacity(int32 cap) { this.cap = cap; }
}
```

The idiom for a raw handle follows: give the field's empty value a **niche** rather than letting zero double
as "unset". `std::fs::File` declares `int32 fd = -1`, so its destructor is `if (fd >= 0)` and descriptor 0
(stdin) is an ordinary ownable handle — Rust's `OwnedFd`.

**The value under construction is `this`, and it needs no declaration.** A constructor's whole job is to
produce the type before it returns, so the storage is implied by the function itself — and since definite
assignment already proves every field is set, a declaration would add ceremony, not proof. Declaring
uninitialized storage *of the type being built* inside its own ctor is therefore an error: `this` is the
only name it has. (An *initialized* local of the same type is untouched — it is a finished value like any
other.) Falling off the end returns that value, exactly as a `void` function need spell no return;
`return give this;` is the **early-return** form.

**`self` is reserved inside a type body.** It is the emitted C name of the receiver pointer, so a local or
parameter called `self` anywhere in a `type` — method, constructor or `static fn` — is a compile error <!-- xfail: self_param -->
pointing at `this`. Outside a type body it is an ordinary identifier: a free `fn` or `fnptr` may name a
parameter `self` to spell an explicit receiver.

### Collections — the four-ctor matrix ✅

Every growable collection (`DynamicArray`, `Deque`, `Map`, `Set`, `SlotMap`, `BitSet`) offers `empty()` /
`withCapacity(n)` using the default `GlobalAllocator`, and `withAllocator(a)` / `withCapacityAndAllocator(a, n)`
for a caller-owned allocator. `PriorityQueue` carries capacity on its backing array; the B-tree
`SortedMap`/`SortedSet` and the always-sized `FixedArray` keep their own shapes. The canonical zero-arg build
is marked **`default`**, which is what makes such a field default-fillable elsewhere.

The default-allocator conveniences are gated **`when [A: default]`** — a *structural* bound (the argument
bound to `A` must itself have a `default` ctor; there is no nominal `Default` contract). So
`DynamicArray<T, BumpAllocator>.empty()` **does not exist**: you get a clean "not available for this
instantiation" error rather than a collection with a zero allocator. Use `withAllocator` for a custom `A`.

**Calling the election — `T.default()`.** The mark names *which* ctor is canonical; `T.default()` calls it
without the caller knowing the name the author chose (`empty`, `zero`, `closed`, …). It works on any type
that elected one, `value` or `resource`, and it is what makes the `when [A: default]` bound usable from
kama rather than only by the compiler's field fill:

```kama
ctor fresh() when [A: default] { this.item = A.default(); }
```

Electing a default stays the **type's** choice: a type that never marked one has no `default()`, and the
call site is a compile error naming that choice rather than a silently synthesized zero <!-- xfail: default_ctor_missing -->
(`tests/default_ctor_call.kama`, `tests/xfail/default_ctor_missing.kama`).

### Derives — `@generate(...)` ✅

One opt-in surface, on a plain (non-generic, non-variant) type. Every name is **opt-in by design**; a
hand-written member always wins over the synthesized body, and the nominal conformance is registered either way.

| Name | Synthesizes |
| --- | --- |
| `Serialize` / `Deserialize` | the reflective wire methods — see *Serialization* |
| `Format` | a field-dump `format(ref Formatter)` — `Type { f: v, … }` |
| `Equatable` | a memberwise `equals(ref This)`; also what gives the type `==` / `!=` |
| `Hashable` | a field-walked `hash()`, FNV-combined in declaration order |
| `of` | a memberwise ctor `T.of(f1:, …)` — **bag only** (a transparent `value`) |
| `zero` | a zero-init ctor `T.zero()` — bag only |

`Equatable`/`Hashable` walk each field through *its own* `equals`/`hash` — never a bitwise compare, which
would read padding and be wrong for any type whose equality is not its representation — so every field must
itself conform, and `@skip` is honored by both (which is what keeps "equal values hash equal" true). A
payload-less `enum` has no struct to walk: declare the contract on the enum and write the method.

### Deliberately not in the model

Recorded so they are not re-proposed: a nameless `Type(…)` call shape or a `primary` keyword blessing one
ctor as nameless-callable; a compiler-synthesized memberwise as the *general* designated ctor (`of` is a
bag-only convenience — a memberwise seam breaks on complex types); a separate `init`/`onConstruction` hook
(input-blind, auto-run — it does not stop logic scattering, and chaining already reaches every path); and a
mandatory "designated"/"final" ctor every path funnels through (completeness comes from definite assignment,
not from a funnel). Constructor **overloading** is a standing non-goal — named parameters cover it.

## Immutability — `const` ✅

`const` is **runtime immutability**, and it is **deep**: neither the binding nor anything reached through
it may be mutated. (The other two axes are orthogonal — `static` is runtime associated storage, `comptime`
is compile-time evaluation; see *Compile-time constants*.) It appears in exactly six positions:

| form | what it binds |
|---|---|
| `const T x = init;` | a **local** — no reassign, no write through it, no `++`/`--` |
| `const T f;` in a type body | a **field** — write-once, assignable only in a constructor |
| `const T x` / `const ref T x` parameter | a **read-only** argument; `const ref` is a read-only borrow |
| `const UnsafePtr<T> p` parameter | lowers to C `const T*`, for const-correct FFI |
| `const fn` on a method | the method does not mutate its receiver |
| `comptime(int32 N)` parameter list | a **comptime parameter** — a compile-time value, an unrelated feature |

A free function has no receiver, so `const fn` does not apply to one; nor to a `ctor`, a destructor, or an
`operator` member. The qualifier follows the modifiers: `public unsafe const fn` parses, `const public fn`
does not.

### `const fn` — a non-mutating method

Inside a `const fn` the receiver is immutable, deeply. Writing `this.f`, writing a bare field name, writing
through `this.a.b[i]`, `++`/`--` on any of those, and passing any of them to a non-const `ref`/`out`
parameter are all rejected. So is **moving** out of it — `give` leaves its source holding a moved-from
value, which is a mutation — and so is `addr(of: …)`, which would hand back a writable pointer into it.

Symmetrically, a **const receiver** — a `const` local, a `const`/`const ref` parameter, or `this` inside a
`const fn` — may call only `const fn` methods. That gate is the point of the marker: it is what lets a
caller hold a value immutably and still use it.

```kama
type value Counter {
    int32 n;
    public ctor make(int32 n) { this.n = n; }
    public const fn int32 value() { return this.n; }              // read-only
    public const fn int32 doubled() { return this.value() * 2; }  // const calling const: fine
    public fn void bump() { this.n = this.n + 1; }                // mutating
}
const Counter c = Counter.make(n: 9);
int32 v = c.value();      // fine
c.bump();                 // error: cannot call non-const method `bump` on a const receiver
```

**`const fn` is ABI-neutral.** It is a front-end rule only — the emitted C signature is identical either
way, so marking a method costs nothing and changes no generated code.

**A `const fn` may not return `ref T`.** A place returned out of a const method is a writable alias into <!-- xfail: const_place_return -->
the receiver, so `c.place() = 99` would mutate a `const` binding with no `unsafe` anywhere. The two halves
take separate names instead — `get`/`getRef`, `iterator`/`iterMut`, `peek`/`peekRef` — which is the split
the standard library already spelled and now the one the compiler enforces. (This is Rust's
`get`/`get_mut`, not C++'s const-overloading, which would need every accessor written twice. A read-only
place — C#'s `ref readonly` — would be more expressive; it is not in the language.)

Operators cannot be `const fn`, and need not be: a write through `operator[]` on a const receiver is
already rejected at the assignment, since its root is const.

### Constness in a contract

A contract member may be declared `const fn`, and an implementation must honor it — the promise is to
every caller bound by the contract, and dispatch goes through a slot, so the implementation is the only
place it can break. The same holds one level down for a `const ref` **parameter**. Only that direction is
checked: an implementation may be *more* const than its member asks, which merely widens where it can be
called.

This is the opposite call from `unsafe`, which is **rejected** on a contract member — and the reason is <!-- xfail: unsafe_on_contract_member -->
the difference between the two markers. `unsafe` describes a *body*, which a member does not have.
`const` constrains what a *caller* may pass as receiver, so it is signature-level and belongs on the
declaration.

An `override` may not drop `const` either: the caller sees only the base declaration, so a const receiver <!-- xfail: const_override_drops -->
that is legal there has to stay legal for whatever subclass sits behind the slot.

### What the standard library marks

The query surface: `length`/`isEmpty`/`capacity`/`count`, `contains`/`indexOf`/`test`/`isSubsetOf`,
`get`/`peek`/`first`/`last`/`floor`/`ceil`, `iterator` (but not `iterMut`), all of `Vec`/`Mat`/`Quat`/
`Duration`/`Instant`/`Fixed`, and the protocols — `Hashable.hash`, `Equatable.equals`,
`Comparable.compareTo`, `Error.message`, `Format.format`, `Serialize.serialize`, `Real`'s twenty-one
members. `Equatable` and `Comparable` borrow their operand `const ref`.

What it deliberately does **not** mark is as informative:

- **`view()`, `slice()`, `iterMut()`, `dataPtr()`, `getRef()`** hand out a mutable window into the
  receiver. Const on any of them would launder exactly what the rule above closes.
- **`Map`'s and `SlotMap`'s `hasNext()`** scan forward past empty slots, so asking the question moves the
  cursor. They are not queries — which is why `IteratorMut.hasNext` is not a const member either, even
  though the other implementations would satisfy it.
- **`Copyable.copy`** keeps a mutable borrow of its source: a retaining copy (`Shared`, `Weak`, a library
  `Rc`) bumps a refcount reached through it.
- **`Atomic<T>`** marks nothing. C++ would call `load()` const; kama does not, because this is the
  sanctioned shared-mutable cell and saying otherwise would be the one place const lies.
- **`fs`/`net`/`process` I/O** — `read`, `write`, `flush`, `accept`, `setNonBlocking` — change OS state
  even though no kama field moves. Only the true accessors (`rawFd`, `rawHandle`, `state`, `id`,
  `status`, `success`) are const.

## Uninitialized storage — `slot` ✅

A `slot` names the storage an **`out` parameter is about to fill** — declared externally so the reader can
see the scope the value will live in. **`slot` means only this.** A contract's requirements are its
**members**, never its "slots"; the one other place the word is load-bearing is the emitted **vtable slot**,
which is always spelled with `vtable`/`vtbl`. That is the whole of it, and it is the only kind of local a kama
program may leave without a value:

```kama
slot File f;                          // a HOLE: an `out` argument will fill it
openInto(path: p, dst: out f);        // now it is live, and drops normally from here
```

**Three rules, and they are what make a hole worth declaring:**

1. **Only an `out` argument fills a slot.** Not an assignment, not a field write, not a method call, not
   `addr(of: x)`. A value that arrives one line late is an ordinary local — `T x = …;` says so with the
   value in hand, and a branch has a stronger spelling still, since `match` and the ternary are
   value-producing and can build a `resource` (`Conn c = match (k) { case A: Conn.tcp(fd: 3); … };`).
2. **A slot with no `out` fill anywhere is an error.** A hole nothing fills is a dead declaration, not an <!-- xfail: slot_never_filled -->
   opportunity to elide a drop.
3. **The fill sits on the same unconditional path as the declaration** — a statement of the declaring
   block, or of a nested block that always runs. Not inside an `if`, a `match` arm or a loop the
   declaration is outside of. Measured *relative* to the declaration, so a slot declared **and** filled
   inside one branch is fine. The reason is that a conditionally-filled slot cannot be tested before use:
   slot validity is a compile-time fact, never a runtime check.

A `slot` is **illegal to read until it is filled**, and — the point — **no destructor is emitted where it is
provably still empty**. "Drop only if live" is therefore *proven*, not defended against at runtime. Move
state is tracked in emission order, so this is decided **per exit point**: a `return` that precedes the fill
drops nothing, while one after it drops normally. A local with no initializer and no `slot` is a compile
error, and `slot` with an initializer is one too: each thing is said exactly one way. `slot` does **not**
run the type's `default` constructor; spell `T x = T.empty();` if that is what you want.

Rule 3 is about the slot's own declaration, not about the callee: an **`out` parameter** is still proven
filled on *every* path, so the callee may fill it through an `if`/`else`, a `match`, or an early return —
that join analysis is where conditional filling legitimately lives.

Two consequences worth stating plainly. A **class-typed** slot is valid-but-empty from the declaration on,
so reading a non-owning field of one or handing it to a callee is fine; an **`Owned`/`Shared`** slot is not
— its zero value is a null pointer, so reading through it is rejected, as is reading a primitive slot, <!-- xfail: slot_read_before_assign -->
which has no field-default fill behind it.

This is **not** `Optional<T>`: a slot has no runtime tag and no drop, and it disappears entirely at
compile time. Use `Optional<T>` when emptiness is a value you carry, `slot` when it is a fact the compiler
should prove away.

## Fallible construction (no exceptions) ✅

kama has no exceptions, so a **fallible constructor returns `Result<T, E>`** (where `E: Error`) — an
infallible ctor returns the bare `T`. The fallible work lives in the ctor, and on failure it returns `Err`
*before* the object exists, so no half-constructed object can escape and `match` forces the caller to handle
the error. A fallible `new Type.ctor(...)` composes to `Result<Owned<T>, E>` — the box is allocated only on
`Ok`.

```kama
type enum SizeError implements Error { TooSmall; public const fn string message() { return "size must be positive"; } }
type resource Buffer {
    int32 size;
    private ctor make(int32 size) { Buffer r; r.size = size; return give r; }        // trivial, infallible
    public ctor Result<Buffer, SizeError> create(int32 size) {
        if (size <= 0) { return Result::Err(error: SizeError::TooSmall); }            // fail before it exists
        return Result::Ok(value: Buffer.make(size: size));                           // delegate to the base ctor
    }
    ~Buffer() { /* … */ }
}
Result<Owned<Buffer>, SizeError> b = new Buffer.create(size: 8);   // fallible `new` -> Result<Owned<T>, E>
```

A type with a *meaningful* inert state may instead start valid-but-inert and expose a `bring_up():
Result<…>` method. (This reuses named ctors + `Result` + `Owned` + RAII — no dedicated feature. See
`tests/fallible_factory`, `tests/ctor_named_fallible`, `tests/dot_on_type_new_fallible`.)

## Inheritance & virtual dispatch ✅

Extensible hierarchies are a **`resource`** concern (an embedded vtable breaks a `value`'s free copy). The
extensible base opts in with a qualifier after `type`:

```kama
type virtual(maxDepth: 1) resource Shape {             // opts in to extension, and says how deep
    int32 sides;
    public ctor make(int32 sides) { this.sides = sides; }
    public fn int32 describe() { return this.area(); }   // public surface
    protected virtual fn int32 area() { return 0; }      // overridable hooks are written `protected`
}
type final resource Circle extends Shape {             // `type final resource` = sealed leaf
    public ctor make() { this.base = Base.make(sides: 1); }   // installs its base FIRST
    protected override fn int32 area() { return 42; }
}
```

Single inheritance (`extends`), base embedded by value (upcast is offset-0), `base.m()` for non-virtual
upcalls — **subject to the same visibility rules as `this.`**, so a derived type cannot reach a `private`
base member by choosing the other spelling. `virtual`/`override` methods dispatch through a vtable. **Inheritance is
opt-in and one-way:** only a `type virtual resource`/`type abstract resource` may be `extends`-ed (a `value`,
a plain `resource`, and a `type final resource` are sealed); an overridable method is written `protected`
(never public/private — public polymorphism is a `contract`'s job); `type final resource`/`final` method seal
a leaf/slot. `virtual`/`abstract`/`final` and `protected` are meaningless outside an extensible `resource` —
they are errors on a `value`, a plain `resource`, or a `contract`. See `docs/KEYWORDS.md` for the full kind
table.

### A derived constructor installs its base ✅

A derived type's constructor **must install its base**, as its **first statement**:

```kama
public ctor make(int32 x, int32 y)
{
    this.base = Base.make(x: x);     // FIRST — the base's own ctor runs
    this.y = y;
}
```

This is not delegation. A named `ctor` is a factory with no `self` to chain into, so the base part is
built by the base's **own constructor** and then embedded whole — which is why the base's invariants hold
for every subclass, and why a base's field *initializers* reach a derived instance.

- **`Base` names the base type**, as `This` names the enclosing type, so a derived author never spells the
  concrete base name and renaming it cannot break subclasses. A real type literally named `Base` wins;
  the alias is the fallback.
- **First, and exactly once.** Until the install runs, every inherited member would read a half-built
  base. Its arguments may read this constructor's parameters but not `this.<field>` — the install is
  lowered before `this` exists.
- **A ctor call on the base type, not a general assignment.** A base part is built by its own constructor
  or not at all; base fields are private, so there is nothing to adjust afterwards.
- **`this.base` may only be assigned.** To reach an inherited member the spelling is `base.<member>`.
- A ctor that delegates wholesale (`return Other.make(…);`) owes no base — the ctor it hands off to does.
- An **`abstract`** base's constructor may be called *here and nowhere else*: the value is embedded at
  offset 0 and the derived vtable is stamped over it before anything can dispatch.

Consequently **a `virtual`/`abstract class` must declare a `ctor`** — without one it can be neither
instantiated nor installed, so it and every type below it would be unconstructible. No generator can
stand in: `@generate(zero)`/`of` require a transparent `value`, and a `value` is sealed.

### The depth budget ✅

An extensible type states **how many levels may still be added below it**, and a deriving type states
**at most one less** — or is `final`, which *is* a budget of 0 and the only spelling for it:

```kama
type virtual(maxDepth: 2) resource Root { … }
type virtual(maxDepth: 1) resource Mid extends Root { … }
type final                resource Leaf extends Mid { … }
```

The chain's length is therefore bounded by the root's budget by construction. The point is that the limit
is met where a design **opts in** to extensibility, rather than arriving as a refusal on the third type —
by which time the design has been built around an assumption the language was never going to honour.
Inheritance is deliberately restricted here (it is a footgun more often than a tool), and a budget you
must write down is how that restriction announces itself.

`maxDepth: 0` is an error — extensible yet unextendable is a contradiction; write `final`. So is a budget <!-- xfail: maxdepth_zero, maxdepth_over_ceiling -->
above the compiler's ceiling, `KAMA_INHERIT_DEPTH` (**default 2**: a root, a middle layer and a leaf,
which is what mainstream hierarchies use). Neither bound is clamped: a clamp would hide the very surprise
the annotation exists to prevent.

### Shadowing is an error ✅ <!-- xfail: derived_shadows_base_method -->

A derived type may not redeclare a method it inherits. The only way to redefine one is `override` on a <!-- xfail: derived_shadows_base_method -->
`protected virtual` (*may* override) or `protected abstract` (*must* override) — the type designer decides
what is overridable, which is what `protected` + `virtual`/`abstract` is for.

```kama
type virtual(maxDepth: 1) resource B { public fn int32 h() { return 1; } }   // no seam offered
type final resource D extends B {
    public fn int32 h() { return 2; }        // ✗ shadows B.h() — which body runs would depend
}                                            //   on the STATIC type of the receiver
```

This holds at **every** visibility, public included. It is a separate rule from *no widening* above, and
they divide the work rather than overlapping: widening is about a name the base does **not** have,
shadowing about one it **does**.

kama already rejects `public virtual` because a public override is a footgun; silent shadowing is the same
footgun with no keyword marking it at all (C# at least demands `new`).

**Reusing a name that is `private` in the base stays legal**, and is not shadowing: the base's member is
invisible to the derived type, so the two names are unrelated and each type sees its own. The rule asks the
same question access control does — *would the derived type even see this?* — so a `friend` grant opens no
back door either.

### A derived type may not widen the public interface ✅ <!-- xfail: derived_widens_public -->

The hierarchy's public surface is fixed at its **root**. A derived type may add **fields**, add
**private** helpers, and **override the protected seams the base sanctioned** (`virtual` = may,
`abstract` = must) — it may not add a public method, and it may not declare `implements`. <!-- xfail: derived_widens_public -->

```kama
type final resource Exposer extends Base {
    public ctor make() { … }                                // ✓ ctors are exempt
    protected override fn int32 secretHook() { return 2; }  // ✓ a seam the base sanctioned
    public fn int32 hook() { return this.secretHook(); }    // ✗ republishes a protected seam
}
type final resource Icon extends Widget implements Clickable { … }   // ✗ contracts belong on the root
```

Substitutability is then **total rather than aspirational**: what a base handle can do is what *any*
subclass can do, and no more. The rule exists for the second line above — a subclass republishing an
internal seam under a new public name, handing the world a hook the base deliberately kept private.

**Constructors are exempt.** A derived type needs its own public `ctor` (`RawChannel.open(…)`), and
construction is not part of the substitutable surface — you build a concrete type, then hand it out as
its base.

**`implements` is barred on a deriving type** because a contract's methods are public and need not exist
on the base, so allowing it would widen the surface through a door the rule never looked at. If a
hierarchy conforms to a contract, its root declares it and every leaf inherits the conformance.

### Depth — a declared budget ✅

See *The depth budget* above for the rule. `Widget -> Control -> Button -> …` — a chain that keeps adding
middle layers — runs out of budget and is refused at the type that asks for more than its base left: <!-- xfail: inherit_depth_exceeded -->

```
'Button' extends 'Control', which allows 1 more level(s) — so 'Button' may allow at most 0,
i.e. it must be `final`
```

The ceiling is `KAMA_INHERIT_DEPTH` in `kama.cemit.h`, a **compile-time constant of the compiler**, not a
per-project setting — it is a property of the language, not of a build. A hierarchy may ask for less than
the ceiling but never more, so a project can restrict itself further without rebuilding anything: a design
pattern that is only ever two layers says `maxDepth: 1` and the compiler holds it to that.

### Building kama without inheritance — `KAMA_INHERITANCE=0` ✅

```sh
make                      # inheritance in
make KAMA_INHERITANCE=0   # a compiler built without it
```

This is a **build-time switch on the compiler itself**, and it is not exposed to programs — there is no
flag or manifest key that turns inheritance off for a project. It exists for kama's own development, for
two reasons: to isolate what the feature costs the compiler (answerable only by building both ways and
subtracting), and to be the **extraction point** if inheritance is dropped — the `#if KAMA_INHERITANCE`
blocks are then the deletion list, already proven to compile without their contents.

Such a compiler rejects `extends`, a `virtual`/`abstract` class, and a `virtual`/`override`/`abstract`
method — all four, since a `virtual class` with no subclass still carries a vtable. `final` stays legal
(it seals a type; it does not extend one), and **contracts are untouched**: they are the intended way to
express polymorphism and keep their own vtables. The grammar still parses `extends`, so you get a real
diagnostic rather than a syntax error. `tools/check-no-inheritance.sh` builds the variant and exercises it.

**Owning a derived through a base handle (upcast).** A `Shared`/`Owned` over a derived class widens to one
over a base class (or a contract it satisfies) — the IS-A relationship, Liskov-style:

```kama
Shared<Circle> c = new Circle.make();
Shared<Shape>  s = c;          // upcast — retain (both handles share one Circle)
Owned<Circle>  u = new Circle.make();
Owned<Shape>   o = give u;     // upcast — move (u consumed)
int32 a = s.describe();          // polymorphic: describe() calls the protected virtual area() -> Circle's
```

Polymorphism flows through the base's **public surface**, which invokes the `protected virtual` hooks
(Template Method) — you never call an overridable method through the handle directly. Destruction is
**virtual**: a `virtual`/`abstract resource` (and every owning contract handle) carries a vtable `__dtor`
slot, so dropping through a base/contract handle runs the **most-derived** destructor's full chain — the
derived's owned resources are freed, never sliced, exactly once. (An upcast only ever yields an *owning*
handle or a scope-local borrow; an un-owned contract value can't be stored — see Contracts.)

## Contracts ✅

A **`contract`** is a public-only guarantee — "some type satisfying this contract." It carries signatures
only: no bodies, no fields, no dtor. Besides methods it may require a **`ctor`** or a **`static fn`**, which
is how a bound gets to *construct* rather than only to call — `ctor T fromWide(int64 v)` on
`std::num::FixedBacking` is what lets generic fixed-point arithmetic narrow back to its backing type
(`tests/contract_requires_ctor.kama`).

```kama
type contract Shape { fn int64 area(); }               // a public guarantee (a "type placeholder")
type value Circle implements Shape {                   // a value satisfies a contract, too
    int64 r;
    public ctor make(int64 r) { Circle c; c.r = r; return give c; }
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
  `Shape` variable (its address is passed, so the reseat sticks); `out` additionally requires the callee to
  assign it and the call site to say `out`. Passing a **concrete type** by `ref`/`out`
  is a compile error — bind it first (`Shape s = c; measure(sh: ref s)`). Mutable references are *invariant*: <!-- xfail: iface_ref_concrete -->
  a `Circle` variable isn't a slot that could hold an arbitrary shape, so it can't back a `ref Shape`.

**Borrow vs. storage — a contract value is second-class.** The fat pointer *borrows* its object, so a bare
contract value is fine as a **parameter or local** (the zero-copy polymorphic view above) but **cannot be <!-- xfail: iface_field, iface_return, iface_collection -->
stored beyond the call that made it** — a bare `Shape` **field**, **return type**, or **collection element**
is a compile error, because the borrowed object could die and leave it dangling. To keep polymorphism around, <!-- xfail: iface_field, iface_return, iface_collection -->
**own the object** with a smart pointer over the contract (below). Ownership is always written explicitly —
never an implicit box. This is the language-wide rule **"borrow is parameter-only; storage requires
ownership"** — the same reason a `ref` parameter can't be returned and a returnable "reference" is always an
owned smart-pointer handle.

**Owned contracts — `Owned`/`Shared`/`Weak<Shape>`.** A smart pointer *over a contract* owns the concrete
object behind a fat handle `{obj, vtbl}` (`Shared`/`Weak` add a `ctrl` block). `new Circle.make(...)` boxes a
concrete implementer into it; `p.draw()` dispatches polymorphically through the vtable; dropping the handle
runs the concrete destructor through a **virtual-destructor slot in the contract vtable**, then frees the
object. `Owned<Shape>` is move-only; `Shared<Shape>` retains/releases (`Weak<Shape>.tryUpgrade() ->
Optional<Shared<Shape>>`). Because the handle is an ordinary value type, it **stores** — as a field or a
function return:

```kama
type resource Holder { Shared<Shape> shape;  public fn int64 area() { return this.shape.area(); } }
fn Owned<Shape> make(int64 s) { Owned<Shape> o = new Square.make(s: s); return give o; }
```

A `DynamicArray<Shared<Shape>>` (the engine's scene) works — polymorphic elements stored and dropped in RAII order.

### `type intrinsic` — a primitive declares its conformances ✅

A **primitive** is a type kind like any other, and it declares conformance the same way: `type intrinsic
<targets> implements C { … }`. The `<…>` is a **set**, because one body usually serves many widths — the
prelude's per-primitive impls collapse from 64 blocks to roughly 8. Inside the block `This` is the target
being decorated, resolved per member of the set.

```kama
type contract Hashable for value, resource, enum, intrinsic { fn uint64 hash(); }

type intrinsic <string> implements Hashable {        // a primitive gains a contract, in pure kama
    public fn uint64 hash() {
        uint64 h = 2166136261ui64;                   // FNV-1a
        int32 i = 0;
        while (i < this.length()) { h = (h ^ cast<uint64>(this[i])) * 16777619ui64; i = i + 1; }
        return h;
    }
}

type intrinsic <int8, int16, int32, int64, uint8, uint16, uint32, uint64>
    implements Comparable<This> { … }                // ONE body for eight widths

fn uint64 hashOf<K: Hashable>(K k) { return k.hash(); }   // `string` now satisfies the bound
```

The set form works because the bodies are **genuinely identical** across it — they use raw `<` / `==`,
which stay raw C operators for all-primitive operands. It is not a substitute for per-type dispatch: a
type list cannot serve `sqrt`, which needs a different C function per width (`sqrtf` vs `sqrt`), and kama
has no in-body type branching by design.

A primitive gets **no `_classes` entry** — every "is this a user type?" test keys on that — so the
conformance hangs on a separate registry, and a **scalar** target's `this` is the value itself: the method
takes `T self` by value and the call is a plain `int32__hash(k)`. That is how `Map<int32, V>` /
`Set<int32>` get their keys.

**A contract is a SCOPE.** A conformance decorates a primitive *within the scope of that contract*, so a
contract-supplied method is **not part of the primitive's own API** — it is reached through the contract,
never off the bare value. Without this, any package declaring `type intrinsic <int32> implements
Weighable` would put `.weight()` on every `int32` in the program, including code that never heard of it.

```kama
int32 l = 3; int32 r = 7;
l.compareTo(other: r);                       // ERROR — `compareTo` is Comparable's, not int32's

fn Ordering cmp<T: Comparable<T>>(const ref T a, const ref T b) { return a.compareTo(other: b); }
cmp(a: l, b: r);                             // a BOUND — monomorphizes to a direct call, zero cost

Comparable<int32> c = l;
c.compareTo(other: r);                       // a CONTRACT VALUE — one indirect call
```

Those two are the only spellings, and both are real. The rule covers every type an impl block decorates;
a type that declares `implements C` in its **own body** is untouched — its methods are its own. String
interpolation is exempt: `"${x}"` is the compiler's own lowering to `Format`, not something an author
wrote.

**Widening — a primitive as a contract value.** A primitive can be bound to a contract, as a borrow or as
an owning box:

```kama
Hashable h = 3;                  // a BORROW — a fat pointer over block-scoped storage. Cannot escape:
fn void f(Hashable h) { … }      // the same escape check that governs every contract value applies.
Owned<Hashable> o = 42;          // an OWNING box — the form that can be a field, an element, a return.
```

The machinery is pay-for-what-you-use: the vtable and its deref thunks (an intrinsic's method takes `self`
by value; a vtbl slot passes `void*`) are emitted only for the pairs a program actually widens.

**Why a kind rather than a mechanism.** Before this, a primitive had no kama spelling at all, so the only
way to give it a contract was `implements C for T` — a *retroactive* block reaching into a type from
outside. Giving primitives (and enums) a spelling removed that mechanism's whole job rather than fencing
it, and the block itself is now **gone from the language**. See *The contract model* for the full argument.

**Coherence.** Two declarations of the same (contract, type) pair are a compile error, whichever kind <!-- xfail: impl_conflict, intrinsic_dup -->
declares them — a class's or enum's own `implements` list, or a `type intrinsic` block. When the two
claims come from different packages the message names **both** — kama's whole-program view makes the
conflict directly visible, so no orphan rule is needed to forbid legal-but-unusual cases in order to
prevent one the compiler can simply see.

## Static methods & operator overloading ✅

**Static methods** — a `static fn` has **no implicit `self`** and is called at the type level with named
args:

```kama
@generate(of)
type value Vec2 {
    public float64 x;  public float64 y;   // all-public transparent value → `@generate(of)` gives `Vec2.of(x:, y:)`
    public static fn float64 dot(Vec2 left, Vec2 right) { return left.x*right.x + left.y*right.y; }
}
float64 d = Vec2::dot(left: a, right: b);
```

A `static` method has no vtable slot (so it can't be `virtual`/`override`/`abstract`) and may not touch <!-- xfail: static_this -->
`this` or a bare field.

**Module statics** — `static T name = const;` at module scope declares a module-level mutable variable
(MCU step 1). It is firmware's home for state that outlives any one call: ISR↔`main` flags, peripheral
handles, ring/DMA buffers, flash tables.

```kama
static uint32 tick = 0;                 // deterministic const init at reset
static bool     data_ready;             // no initializer → zero-init
static InlineArray<uint8>#(256) rx_buf;  // a zero-initialized buffer
static UnsafePtr<Uart> uart;                  // a peripheral handle (null until assigned)

fn void on_timer() { tick = tick + 1; } // shared with `main` in the same isolate
```

- **Per-isolate by construction.** A module `static` is *not* shared global state — each isolate gets its
  own copy (lowered `static KAMA_ISOLATE_LOCAL T name`: `_Thread_local` on native and on wasm — emscripten
  pthreads share one linear memory — and a plain zero-cost `static` on a single-core `--target embedded`). So
  a `static` **cannot be seen by another isolate → cannot race**; cross-isolate mutable sharing stays on the
  greppable `Atomic<T>` / shared-region seam (see [Concurrency](#concurrency-)). This unifies the MCU need with the threading
  model: the same declaration is race-free the day it runs multicore (proven ThreadSanitizer-clean).
- **v1 scope (deliberately minimal, MCU-correct).** The type must be a **value, `UnsafePtr`, or `InlineArray`**
  (owns nothing, needs no teardown — v1 has no static-destructor seam); a destructible `resource`, `string`,
  or smart pointer is rejected. The initializer must be a **compile-time constant** (a literal, `sizeof`, or <!-- xfail: module_static_resource -->
  const arithmetic); a runtime initializer (a call / `new` / `spawn`) is rejected — **omit it to zero-init**. <!-- xfail: module_static_runtime_init -->
  These restrictions are not stopgaps: const-init is the deterministic reset-time init a bare-metal target
  wants (no static-init-order fiasco, no startup hook), and value-only keeps global data off the heap. A
  `static` is **file-private** (internal C linkage): it may not appear in an `export { … }` list, and no <!-- xfail: export_module_static -->
  other file — not even a sibling in the same module — can name it. Per-isolate storage is why: a second
  translation unit could only get its own copy, which would be a silent correctness bug rather than
  sharing. Publish a **`comptime`** if the value is constant, or a function if it is not. A `hardware`
  static (`static hardware T name`) adds the
  `volatile` qualifier for an MMIO register or single-core ISR↔loop flag — `volatile T` for a scalar,
  `volatile T*` for an `UnsafePtr<T>` handle. *(Destructible statics are a later MCU step.)*

### Compile-time constants — `comptime` ✅

A `comptime` declaration is a **named compile-time constant** (const-eval 6b-2). Three keywords name three
orthogonal axes: `const` = *runtime immutability*, `static` = *runtime associated storage*, `comptime` =
*computed at compile time* (and therefore also immutable and associated — those fall out). Unlike Rust's
`const` (which fuses immutable + compile-time), Kama keeps them separate: `const` may bind a runtime value
(`const Box b = Box.make(...)`), while `comptime` must fold before the program runs.

One keyword, three scopes:

```kama
comptime int32 CAP = 64;                 // module scope — a shared named constant
comptime int32 CAP2 = CAP + 1;           // may reference an earlier comptime (folds to 65)

type value Palette {
    public comptime int32 SIZE = 4;      // type-associated — read `Palette::SIZE`
    comptime int32 SEED = 100;           // private (default for a `value`) — internal use only
}

fn void demo() {
    comptime int32 N = 8;                            // local (function or block scope)
    InlineArray<int32>#(N) a = [0; (N)];            // drives a comptime size and fill
    InlineArray<int32>#(Palette::SIZE) b = [0; (Palette::SIZE)];
}
```

- **Where it lives sets how it's reached.** A **module** `comptime` is a module-level named constant, and
  it is the one module-scope declaration that **can** be published: name it in `export { … }` and a
  consumer may `import` it, spell it `mod::NAME`, size a type with it, or take its address — the same
  file rung a function obeys, so an unexported one stays private to its file. (Contrast the mutable <!-- xfail: export_const_unexported -->
  `static` above, which is file-private and not exportable at all.) A **type** `comptime` is read as
  **`Type::NAME`** — via `::`
  (the associated-item operator, like an enum variant `Result::Ok` or a static factory `Deque::withAllocator`);
  `.` stays reserved for constructors and instance access. A type `comptime` obeys **member visibility**
  (`public`/`private`/`protected`, default private for a `value`) — a private one is usable only inside the
  type's own code, the same rule and diagnostic as a private field. A **local** `comptime` is scoped to its
  function or block.
- **Initializer must fold** — a literal, `sizeof` of a fixed-width scalar (**not** `alignof`, and not
  `sizeof` of a `usize`/aggregate — see *Writing a collection in kama*), const arithmetic, or another
  `comptime`. A
  `comptime` whose initializer can't fold is an error **at the declaration** (a `comptime` local's message <!-- xfail: sizeof_usize_not_foldable, alignof_not_foldable -->
  points you back to `const` for a runtime-initialized immutable). A plain `const` *local* whose initializer
  happens to fold is *opportunistically* usable in a compile-time position too (mirroring C++ `const` vs
  `constexpr`: `const` works when it can, `comptime` guarantees it); at module and type scope there is no
  runtime init point, so `comptime` is the only named-constant form.
- **Lowering — real storage, baked references.** A `comptime` emits a genuine `static const T` symbol, so it
  is addressable and `@section`/flash-placeable (an MCU `.rodata` table). But a reference from *another*
  constant's initializer (`CAP2 = CAP + 1`) or a comptime size is **baked to a literal** in the emitted
  C. That sidesteps C's "initializer element is not constant" rule and, more importantly, means **there is no
  static-initialization-order dependency** — Kama has no dynamic global init to order (the C++ init-order
  fiasco cannot occur here). Constant references resolve in **declaration order**; a forward or cyclic
  reference is a clean compile error, not undefined behavior.
### Compile-time functions — `comptime fn` ✅

A **`comptime fn`** is a function the compiler RUNS at compile time to bake its result into a `static const`
scalar or **table** — a CRC / gamma / trig lookup table computed once, sitting in `.rodata`/flash with zero
runtime cost (const-eval 6b-3). It extends the `comptime` axis to *computation*: `comptime` constants name a
compile-time *value*; a `comptime fn` produces one. A comptime function is necessarily `static` (it has no
runtime `this` to read), so the bare `comptime fn` form is the whole story — no extra marker.

```kama
comptime fn InlineArray<uint8>#(256) crcTable() {           // top-level compile-time function
    InlineArray<uint8>#(256) t = [0; 256];
    for (int32 i = 0; i < 256; i = i + 1) {
        uint8 c = cast<uint8>(i);
        for (int32 k = 0; k < 8; k = k + 1)
            c = ((c & 1) != 0) ? cast<uint8>((c >> 1) ^ 0x8C) : cast<uint8>(c >> 1);
        t[i] = c;
    }
    return t;
}
comptime InlineArray<uint8>#(256) CRC = crcTable();   // baked → static const InlineArray_uint8_256 CRC = {.v={…}};

type value Palette {
    comptime fn int32 sq(int32 x) { return x * x; }             // private (default) — internal helper
    public comptime fn InlineArray<int32>#(8) squares() { … }    // read `Palette::squares()`
}
```

- **Comptime-only.** A `comptime fn` is a compile-time symbol; it is **never emitted as C**. It may be
  *called* only from a comptime context — a `comptime` constant initializer, a comptime argument, or
  another `comptime fn`. A runtime-position call is a clean error pointing at the `comptime` constant form.
  (This is a strict subset of a future dual-use / `constexpr`-style relaxation, so it can widen later without
  breaking anything.)
- **Both scopes, member visibility.** A top-level `comptime fn` is a module-level compile-time function; a
  **type-associated** one is read `Type::name()` and obeys member visibility — **default private** (scoped
  and access-restricted, like any member), callable from outside only when marked `public`. A private
  type-associated comptime fn is callable from within its own type's comptime fns.
- **The subset.** Integer (all widths — narrow-int wrap happens on cast + typed store, so a `uint8` table
  entry wraps at 256 exactly as the emitted C would), `float32`/`float64`, `bool`, `char`, and fixed
  `InlineArray<T>#(N)`. Statements: local + `const` decls, `=` assignment, fixed-array element writes
  (`t[i] = …`), `if`/`else`, `for`/`while`/`do`, `foreach` over a fixed array, `return`. Expressions:
  arithmetic / bitwise / comparison / logical (short-circuit) / ternary / cast, array index reads, and
  calls to other comptime fns.
- **Purity → reproducible builds.** A comptime fn is deterministic and effect-free: no I/O, no `new`/`spawn`,
  no FFI, no reads of mutable `static`s, no pointers/strings, and it may call **only** another `comptime fn`.
  These are enforced structurally — anything outside the subset is a clean "unsupported in comptime fn"
  diagnostic — so the same inputs always bake the same output.
- **Bounded.** A step budget (and call-depth cap) guarantees a runaway comptime fn can't hang the compiler
  (as C++ constexpr-steps / Zig `@setEvalBranchQuota`); exceeding it is a clean diagnostic naming the fn.

### Compile-time assertions — `comptime assert` ✅

A **`comptime assert(cond:, msg:)`** states a premise the build must satisfy. It takes the same arguments
as the runtime [`assert`](#writing-a-collection-in-kama--sizeof-panicassert-place-returning-methods-) —
`msg:` mandatory, the condition's source text auto-appended to the diagnostic — and only the `comptime`
marker differs, because only *when* it is checked differs. It emits no runtime code: an assertion that
holds costs nothing, and one that fails is a build error rather than a trap.

```kama
comptime assert(cond: sizeof(int32) * 8 == 32, msg: "int32 must be 32 bits");   // module scope

type value Fixed comptime(int32 F) {
    comptime assert(cond: F > 0 && F < 32, msg: "fractional bits must fit the backing");
}

comptime assert(cond: sizeof(Vertex) == 20, msg: "vertex buffer stride");       // a layout claim

fn void render() {
    comptime assert(cond: sizeof(float64) == 8, msg: "float64 is 8 bytes");     // and inside a body
}
```

- **Three scopes: module, type member, statement.** A `comptime assert` in a **generic** — type or
  function — is checked **once per instantiation**, with that instance's arguments bound, so the failure
  names the use site (`assertion failed: F > 0 && F < 32 … [with F = 40]`). That is what lets a generic
  reject a bad argument instead of miscompiling. A member assert has no visibility: `public` on one is an
  error.
- **`msg:` must be a plain string LITERAL** — stricter than the runtime `assert`, which takes any string
  expression. The reason is the second lowering below, whose message has to be a literal; one rule for
  both beats a rule that changes with the predicate.
- **One surface, two lowerings.** Which one applies is decided by the predicate, not by the author:
  - **kama answers it** when the predicate folds — literals, `comptime` constants, comptime
    parameters, `sizeof` of a fixed-width scalar, and arithmetic/comparison/logic over those. The failure
    is an ordinary kama diagnostic, so **`kama check` and the LSP report it**.
  - **The C compiler answers it** when the predicate turns on a layout fact kama deliberately does not
    model — an aggregate's `sizeof`, any `alignof`, `sizeof(usize)`. kama emits a C11 `_Static_assert`
    carrying the message, and the target's real ABI decides. This needs no layout model in kama, which is
    exactly why `sizeof` folds only for fixed-width scalars.
  - Anything else — a predicate naming a runtime value, or mixing a layout fact with one — is an error. <!-- xfail: comptime_assert_runtime_cond -->
- **⚠️ The caveat, and it is the price of the split: the layout form fails at BUILD, not at `kama check`.**
  The C compiler is what rejects it, and `kama check` runs no C compiler — so **the LSP cannot show it**.
  A scalar predicate has no such gap. Prefer the scalar form when a claim can be stated either way.
- **Not a `debugAssert`.** There is no release-stripped variant, because there is nothing to strip: a
  compile-time assertion never reaches the running program.

**MCU codegen attributes (step 4)** — `@interrupt` and `@section(".x")` are declaration attributes (the
existing `@name(args)` mechanism, extended from serialization to functions + statics). Each emits a C
`__attribute__((...))` **only** on the declaration it annotates; un-annotated code is byte-identical.

```kama
@section(".isr_vector") static hardware UnsafePtr<uint32> vtor;   // -> __attribute__((section(".isr_vector")))

@interrupt expose fn void on_systick() { … }                // -> __attribute__((interrupt, used))
```

- **`@interrupt`** binds a function to an interrupt vector: it emits `__attribute__((interrupt, used))`,
  the ISR calling convention on **Cortex-M / RISC-V / classic ARM** (`used` keeps it past `--gc-sections`).
  The handler must be `void h()` (no params, no return path) and must be **`expose`d** so the vector table
  can reference it by its bare symbol. AVR's `ISR(VECTOR)` macro form (`@interrupt("VECTOR")`) is a later step.
- **`@section(".name")`** places a module static *or* a function in a named linker section — the ISR vector
  table, a flash const table, a `.ramfunc`, or a DMA RAM bank. The board's linker script owns the addresses.

**Layout control — `@align(N)` / `@packed`.** The same passthrough mechanism, applied to a TYPE rather than
a declaration: `@align(N)` emits `__attribute__((aligned(N)))` and `@packed` emits `((packed))` on the
struct kama generates. They are the companion to layout *verification* — `sizeof`/`alignof` fold and
`comptime assert` hands the rest to the C compiler — and verification shipped first deliberately, because
asserting a layout is what makes stating one safe.

```kama
@packed  type value Reg  { public uint8 ctrl; public uint32 data; }   // sizeof 5, not 8 — an MMIO block
@align(16) type value Vec4 { public float32 x; public float32 y; public float32 z; public float32 w; }
comptime assert(cond: sizeof(Reg) == 5, msg: "the wire format is 5 bytes");
```

- **kama does not own layout, and deliberately does not start.** It emits C and the C compiler lays the
  struct out; a layout model in kama would be a second source of truth that could disagree with the real
  one per target. So these state a constraint and `sizeof`/`alignof`/`comptime assert` verify what the
  toolchain actually did.
- **`N` must be a power of two, 1 to 4096.** Not passthrough, because gcc and clang do not *refuse* a
  non-power-of-two — they round it **up**, so `@align(3)` would compile and quietly mean 4.
- **Types with a struct only** — `type value` and `type resource`. An `enum` is refused: a payload-less one <!-- xfail: layout_attr_on_enum -->
  lowers to an integer and a tagged one to a tag plus a per-variant union, which an outer `packed` would
  not reach; an enum states its layout with `type enum E : IntType` instead. A **declaration** is refused <!-- xfail: layout_attr_on_static -->
  too, so there is one way to align an object rather than two: write `@align(64) type value Buf {…}` and
  declare the static with that type. Rust makes the same call — `#[repr(align(N))]` is types-only.
- A type may carry both, and either may sit beside `@generate(...)`. Fixtures: `tests/layout_align_packed.kama`,
  `tests/xfail/align_not_power_of_two.kama`, `layout_attr_on_enum.kama`, `layout_attr_on_static.kama`.

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
constructor** is a valid operand — `v + Vec3.of(x: 1, y: 0, z: 0)` needs no separate local. It works in an
`if`/`while`/`for` condition too (the condition is wrapped / uses a loop-and-a-half so the temp materializes
and re-evaluates each pass); a `do`/`while` condition is the one place it must still be bound to a local.

**Comparison is a contract, not an operator.** The six comparison operators are the one place where the
operator is not declared on the type: `==`/`!=` lower to **`Equatable.equals`**, and `<`/`>`/`<=`/`>=`
lower to **`Comparable.compareTo`**. Declaring `operator==` (or any of the other five) is a compile error <!-- xfail: operator_cmp_declared -->
that hands back the `implements` form. This is what keeps `a == b` and a `<K: Equatable>` bound from ever
disagreeing — the split C# has, where `operator==`, `Equals`, `IEquatable<T>` and `EqualityComparer<T>` can
all give different answers. Rust is the same shape as kama here (`a == b` *is* `PartialEq::eq`).

The rule that falls out: **operators a generic bound has to name are contracts; operators that are pure
concrete-type ergonomics (`+`, `*`, `[]`) stay operator members.** So `Equatable` is the sibling of
`Comparable` it always should have been, and conforming to either also buys container eligibility — a
`Comparable` type is a `SortedMap`/`SortedSet` key and a `PriorityQueue` element; add `Hashable` and it is
a `Map`/`Set` key.

```kama
type value Cents implements Equatable, Comparable {
    public int32 v;
    public fn bool equals(ref Cents other) { return this.v == other.v; }          // `==` / `!=`
    public fn Ordering compareTo(ref Cents other) {                               // `<` `>` `<=` `>=`
        if (this.v < other.v) { return Ordering::Less; }
        if (this.v > other.v) { return Ordering::Greater; }
        return Ordering::Equal;
    }
}
```

Both contracts **borrow** their operand (`ref This`) — a comparison never consumes or copies it. `!=` is
`!equals`; `<=`/`>=` are "not Greater"/"not Less", so there is nothing separate to define. Equality stays
**explicit**: a `value` that implements neither contract cannot be compared, and there is no auto-generated <!-- xfail: operator_missing -->
structural equality — but `@generate(Equatable, Hashable)` will synthesize the memberwise walk on request
(see *Derives*). The `true`/`false` conversion operators are out of scope.

**Primitives are untouched.** An all-primitive comparison keeps the built-in C operator, so `float` `<`
keeps exact IEEE semantics at zero cost and never routes through `Comparable`. (A float is deliberately
**not** `Hashable` — NaN and ±0.0 make it a bad key — while its `Comparable` impl is a *total* order with
NaN sorting last, which is what the sorted containers need. Same split as Rust's `total_cmp` vs `PartialEq`.)

A user type may define a **place-returning index operator** — `public ref T operator[](usize i)` —
whose body returns a place (`return this.cells[i]`). It lowers to `T* C__op_index(C* self, size_t i)`,
and the caller derefs the place, so `g[i] = v`, `g[i] += 1`, `m[i][j] = v`, `m[i].field = v`, and
`ref g[i]` all work — the same place semantics as a built-in collection, now expressible in the
language (so a `Vec`/matrix can be written *in* kama). The place is a **second-class borrow** of
`self`: it is used transiently and cannot be stored (there is no `ref`-local/`ref`-field to hold it),
and a `const` receiver makes it read-only. Bounds safety is the operator's responsibility — a
`InlineArray`/collection-backed body is auto-checked; a raw `UnsafePtr<T>` body is `unsafe`. The same place-return
works for a **named method** — `public fn ref T at(usize i) { … }` — so `v.at(i) = x` too. It also
works on a **free function** and a **`static` method** — `fn ref int32 at(ref Buf b, usize i) { return
b.d[i]; }`, called as `at(b: ref b, i: 0) = 5`. Because a free/static function has no `this`, the
returned place must borrow a **`ref`/`out` parameter** (the caller-held borrow that outlives the call);
a place into a local or a by-value param is rejected (*"would dangle"*), the same escape rule as a <!-- xfail: refreturn_local, reffn_byval -->
method borrowing `this`. Generic free functions work too (monomorphized per `T`). A **`ref` of a
`contract`** is *not* returnable — a contract value already borrows its object, so own it
(`Shared<Contract>`) to hand polymorphism back.

Used in a `contract`, an operator becomes a **bound** for generic math (see below).

## Generics ✅

User-defined generics, **monomorphized** (one specialized copy per concrete type — elements inline, no
boxing; identical layout and cost to the built-in collections).

```kama
type value Pair<A, B> { public A a; public B b; public ctor make(A a, B b){ Pair<A, B> r; r.a = a; r.b = b; return give r; } }
fn T max<T: Comparable<T>>(T a, T b) { return a > b ? a : b; }   // generic fn — args INFERRED from the call
Pair<int32, string> p = Pair.make(a: 1, b: "x");       // generic type (args inferred from the LHS)
int32 m = max(a: 3, b: 4);                              // -> max<int32>, a static specialized C fn
DynamicArray<Shared<Shape>> scene;                              // nested generics, no space (the `>>` split)
```

- **Generic functions and types**; multi-parameter (`Pair<A, B>`), nested (`Box<Pair<int32, int32>>`) — nested
  `>>` needs no space. `type value`/`resource` generics both work (a generic resource is move-only with a
  per-instance dtor). Function type args are inferred from the call.
- **Turbofish — explicit type arguments.** When inference can't determine the type args — most commonly a
  **return-only generic** whose type parameter never appears in an argument — spell them explicitly with
  `f::<int32>()` (the `::` before `<` is unambiguous). Turbofish reaches a generic *function*; a generic
  *type* is still written `Box<int32>` in type position; and a generic *constructor* spells the type args on
  the type — `Box::<int32>.make(...)` / `new Box::<int32>.make(...)` (turbofish on the type, uniform).
  ```kama
  fn T zero<T>() { return cast<T>(0); }   // T appears only in the return — inference can't see it
  int32 x = zero::<int32>();              // turbofish supplies it
  int64 y = zero::<int64>();
  ```
- **Contract bounds** — `fn sort<T: Comparable>(…)`, `type value Map<K: Hashable + Comparable, V>`. `+` means
  **AND** (all listed contracts). A bound lets the body call the contract's methods on a type-param value;
  because it's monomorphized, those calls are **static direct calls** (zero cost, no vtable). Each concrete
  type argument is checked to satisfy its bounds, else a clean compile error.

  **A bound is what the body may call, and nothing else.** Reaching a member the bounds do not declare is
  an error at the DECLARATION, naming the bound that is missing rather than the method that is not there:

  ```kama
  fn T largest<T>(T a, T b) { return a.compareTo(other: b) == Ordering::Greater ? a : b; }
  // error: `T` has no bound providing `compareTo` — an unbounded type parameter promises nothing,
  //        so bound it with a contract that declares `compareTo`: `<T: …>`
  ```

  An unbounded parameter therefore promises nothing at all — it is not "anything", it is a type with no
  API — and it is treated as move-only, because an instantiation may bind it to one. A parameter whose
  bounds are all declared `for value` is a value at every instantiation, so it is copyable
  (`tests/xfail/generic_unbounded_call`, `tests/xfail/generic_wrong_bound_call`).
- **What is checked at a generic's DECLARATION, versus at its instantiation.** A generic **nobody
  instantiates is still fully analyzed** — every rule in the language applies inside its body, with each
  type parameter standing for a type that promises exactly what its bounds promise. That is what makes
  `kama check` on a library a real answer: a package's public generic, which its own tests may never
  instantiate, cannot ship green and hand its consumers the errors. It holds for a generic **function**, a
  generic **type** and a generic **`enum`** alike, and for `build`, `check` and the language server
  identically (`tests/generic_uninst_ok.kama`, `tests/xfail/generic_uninst_*`).

  What waits for the instantiation is only what a concrete type argument decides: whether that argument
  satisfies the bounds, which `when [T: …]` members exist for it, and a `comptime assert` over a const
  parameter — each reported at the use site that chose the argument, naming it.

  The one thing neither can check is a constraint the language cannot spell. `cast<T>(…)` inside a generic
  needs `T` to be a scalar, and no contract means "an integer primitive" — so a template that converts
  through its own parameter is checked at each instantiation instead. The stdlib has no such template: a
  value crossing a type-erased boundary is passed by ADDRESS and moved as `sizeof(T)` bytes, which
  converts nothing and needs no such promise. That is the idiom to reach for; `Atomic<T>` is written that
  way throughout, and its element restriction — a lock-free machine word — is enforced by the compiler at
  the use site, being likewise unspellable as a bound.
- **Comptime parameters** — a parameter may be a **value** instead of a type: `comptime(int32 N)`, in its
  own trailing list, supplied at a use site with `#(4)`. `<…>` holds types; `#(…)` holds values. Inside the declaration it reads as an ordinary value
  of its type, so a length, a shift or a scale becomes a parameter rather than part of a name:
  ```kama
  type value Fixed<B: FixedBacking<B>> comptime(int32 F) {          // storage AND fraction, both parameters
      comptime assert(cond: F > 0 && F < cast<int32>(sizeof(B)) * 8, msg: "…");
      public B raw;
      public fn int32 toInt() { return cast<int32>(this.raw.wide() / (1i64 << F)); }   // F is a value here
  }
  fn int32 shifted(int32 x) comptime(int32 S) { return x << S; }

  Fixed<int32>#(16) q = Fixed::<int32>#(16).one();   // Q16.16; `Fixed<int8>#(16)` is a compile error <!-- xfail: fixed_bad_pairing -->
  int32 y = shifted(x: 2)#(3);                     // the values are their own list, always last
  ```
  - The parameter's **type is declared** and the argument must be a compile-time constant — a literal or a
    expression, and it needs no parenthesis of its own — a negative one is simply `f()#(-1)`. An argument that does not fit its
    declared type is an error, not a wrap. <!-- xfail: constgen_oob -->
  - The **name is reserved for the whole declaration**: a parameter, field, local, `foreach` variable or
    `match` binding may not reuse it, and it cannot be assigned to. The value is resolved ahead of every <!-- xfail: constparam_shadow_match, constparam_assign -->
    runtime name, so a rebinding would be discarded rather than shadowed — the one case kama's general
    shadowing rules do not already cover.
  - Const parameters pair with **[`comptime assert`](#compile-time-assertions--comptime-assert-)**, which is
    checked once per instantiation with that instance's arguments bound: a generic states its own invariant
    over its own parameters, and a bad instantiation is rejected at the use site, naming the arguments that <!-- xfail: constgen_arity, constgen_count -->
    broke it. `sizeof` folding (above) is what lets that invariant mention a type parameter's width.
  - A generic **`enum`** may not declare members at all, so a const parameter there could never be read; <!-- xfail: enum_field -->
    the kind still accepts one for parity with the other type kinds.
- **`This`** — the self-type. Inside a type's **own** body it is that type (`fn This clone()`,
  `implements Comparable<This>`) and needs no declaration, because nothing is erased there. A **contract**
  may not name `This` in a signature; it declares the self-type as a **pinned type parameter** instead:

  ```kama
  type contract Comparable<T is This> for value, resource, intrinsic { const fn Ordering compareTo(const ref T other); }

  type value Duration implements Comparable<This> { … }     // conformance: always `This`
  fn T maxOf<T: Comparable<T>>(T a, T b) { … }              // bound: the bound's own parameter
  Comparable<int32> c = 3;                                  // use as a type: a concrete name
  ```

  The reason is erasure. `This` is a substitution, and a substitution needs something to substitute into:
  a generic bound monomorphizes and has that, a contract **value** has thrown the type away. A vtable slot
  must give `This` one concrete type, so it bound the contract while the function behind the slot had
  bound the implementing type — two bindings for one function pointer, and the cast between them was a lie
  the C compiler could not see. A pinned parameter is a real type argument that resolves identically on
  both sides, which is what lets `Comparable`, `Equatable` and `Real` be contract values at all.

  `is` is an **identity** constraint and gets its own grammar position rather than joining the `:` bound
  list, which holds contracts. At most one parameter may be pinned, it must come first, and the operand is `This` —
  `<T is Widget>` (a subtype bound) is not a thing kama has. Chosen over `Self` to pair with the `this`
  value and the PascalCase-types convention.
- **Generic math (operators as bounds)** — a `contract` may declare **operators**, giving generic code
  arithmetic over any conforming type at zero cost:
  ```kama
  type contract Arithmetic<T is This> for value { T operator+(T rhs); }
  fn T sum<T: Arithmetic<T>>(T a, T b) { return a + b; }   // `a + b` -> static Concrete__op_add(&a, b)
  ```
  The concrete type declares `implements Arithmetic<This>` (bounds are nominal), and `a + b` in the monomorphized
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
  `DynamicArray<Iterator<int32>>` or open-world runtime choice; the fat pointer can. See **Contracts**.)
- **Default type parameters + named type-arg override** — a **trailing** type parameter may carry a default
  `= DefaultType`, filled in when the use site omits it. So a library can grow parameters (a pluggable hasher,
  a custom allocator) without breaking existing call sites: `Map<string, int32>` keeps meaning
  `Map<string, int32, DefaultHasher, GlobalAllocator>`. Overriding a *later* default without spelling an
  earlier one uses Kama's **named argument model applied to type args** — name the arg to skip a default
  (`Map<int32, Entity, A: ArenaAllocator>`). Leading args stay positional; a positional arg may not follow a <!-- xfail: default_positional_after_named -->
  named one; the `:` is unambiguous at use sites (contract bounds appear only in *declarations*). Defaults +
  named overrides resolve to one canonical positional tuple **before** monomorphization, so the omitted,
  named, and fully-spelled forms all dedup to a single specialized instance.
  ```kama
  type value Wrap<T, U = int32> { public T first; public U second; /* … */ }
  Wrap<bool>          a = Wrap.make(a: true,  b: 7);     // U defaults to int32
  Wrap<bool, float64> c = Wrap.make(a: true,  b: 3.5);   // U overridden positionally
  Wrap<bool, U: int32> d = /* … */;                 // named override — same instance as `Wrap<bool>`
  ```
  Defaults are a **`type`** feature — a `type value`/`resource`/`contract` or a `type enum`. A default
  fills in an argument the *use site* omitted, and a **function's** type arguments are not written at the
  use site at all: inference reads them off the arguments, or a turbofish spells them. So there is nothing
  for a default to fill in, and `fn f<T = int32>()` is an error naming that <!-- xfail: fn_type_param_default -->
  (`tests/xfail/fn_type_param_default`).

  Default **function/constructor** parameters are a deliberate non-goal (one way to do a thing) — a
  self-documenting named `ctor` (`Map.withAllocator(allocator: …)`) covers that need instead.
- **A type parameter may not shadow a visible type.** A type parameter is a binder, so `fn area<Point>(…)` <!-- xfail: type_param_shadows_type -->
  would declare a fresh `Point` and make the real one unreachable inside that declaration — legal in Rust
  and C++, and silent in both. kama rejects it and says so, because the failure otherwise surfaces as a
  C-compiler error about the substituted type. Visibility is the declaration's own: a type it declares,
  imports, aliases or gets from the prelude all count (`tests/xfail/type_param_shadows_type`).
- **A name is declared once per module** — kama has no overloading, so a second `fn` of the same name
  is an error naming both declaration sites, for a plain function and a generic template alike. `extern` <!-- xfail: dup_fn, dup_generic_fn -->
  is exempt on both sides: re-declaring a C entry point in each module that calls it is what an `extern`
  is for (`tests/xfail/dup_fn`, `tests/xfail/dup_generic_fn`).
- **Specialization is a non-goal.** There is no way to give one generic function a second body for a
  particular concrete type argument, and there will not be — the mechanism for a per-type body is a
  `contract` (plus `type intrinsic` for a primitive), which is what `std::math`'s `Real` is. Reasoning in
  [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) § *Deferred language bits*.

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

A **`type enum`** declares either a plain (payload-less) set of variants or a **tagged union** (variants
carry payloads, and the enum may be generic):

```kama
type enum Color { Red, Green = 5, Blue }     // plain: Red=0, Green=5, Blue=6
Color c = Color::Blue;                        // variants are scope-resolved with ::

type enum Shape { Circle(float64 r), Rect(float64 w, float64 h) }   // tagged union (payloads)
```

An enum is a type kind like any other, so it **declares its contracts inline** and carries the methods that
satisfy them — the variants come first, then a `;`, then ordinary members:

```kama
type enum IoError : uint8 implements Error {
    NotFound, Denied(int32 code);

    public const fn string message() {
        return match (this) { case NotFound: "not found"; case Denied(code: c): "denied"; };
    }
}
```

The `;` separating variants from members is **mandatory**, and it is what makes the body unambiguous: a
bare `Foo` variant and a `Foo bar;` field are indistinguishable until it appears. An enum may declare
methods with or without a contract, but **not a field or a destructor** — its layout is its tag plus its
variant payloads, and it owns nothing beyond them. Declaring a method-carrying contract gives a
payload-less enum a tagged representation so it can hold the method and a dispatch vtable; that is
transparent to its by-value uses.

**`type enum E : IntType`** pins the tag to a fixed-width integer — `uint8` for a wire format or a packed
MMIO field, where the default (a compiler-chosen `enum` width) is not something you can serialize against.
It is the only part of an enum's layout kama lets you state, and it changes the lowering: ISO C cannot set
an enum's underlying type, so a pinned enum emits `typedef <IntType> E;` plus an anonymous `enum` carrying
the constants, where an unpinned one emits a real C `enum`. That is a genuine difference in what the C
compiler can prove — with a plain integer tag it cannot see a `switch` over every variant as total — and it
is why the `default:` arm of a **value-producing** `match` over a pinned enum diverges (a `kama_panic`)
rather than falling through. Every other `match` emits `default: break;` unchanged. Fixture:
`tests/enum_match_intty.kama`.

An unpinned plain enum lowers to a C `enum`; a tagged union lowers to a tag + payload union. Enum variants are
scope-resolved with `::` and constructed with named args (`Shape::Rect(w: 3.0, h: 4.0)`). A variant is
**not a type** — `Rect r` does not name anything, and an enum cannot nest type declarations — so `Rect` is a
member of `Shape`'s scope, reached with `::` like any other scope member; supplying its payload yields a
`Shape`. That is why construction's dot-on-type rule does not apply here: there is no type to dot.

**`match`** is the **one** construct for branching on an enum — payload-less enums, tagged unions, and the
`Optional`/`Result` prelude types alike. It is **value-producing** (usable in statement or expression
position), enforces **compile-time exhaustiveness**, and accepts a `_` wildcard for the catch-all case.
It is also the only construct that *reads* an enum, which is why the only way to *build* one from an
integer — `try cast<E>(x)`, above — hands back an `Optional<E>`: a value that names no variant arrives as
a `None` **arm** of the same construct, never as a trap.

**A pattern NAMES the fields it binds** — `field: local` — exactly as a call names its arguments; there is
no positional form, and kama no more exempts a one-field variant here than it exempts a one-argument call
from a label. The label is the variant's field; the identifier after it is the local it introduces, and it
may be called anything. Because the label decides, **order does not**: `case Rect(h: y, w: x)` and
`case Rect(w: x, h: y)` are the same pattern. Positional binding is how `case Rect(height, width)` compiled
clean and silently returned the wrong values — the bug class named arguments exist to remove. Three
mistakes are compile errors, each naming the fields the variant actually has: binding an **unknown** field
(`tests/xfail/match_label_unknown.kama`), binding one **twice** (`…/match_label_duplicate.kama`), and
leaving one **unbound** (`…/match_label_missing.kama`) — a pattern names every field of its variant, just as
construction supplies every one. The label is also a *reference* to the field, so hover, go-to-definition
and rename reach it. Pinned by `tests/match_named_bindings.kama`.

```kama
int32 area = match (sh) {                     // expression position — yields a value
    case Circle(radius: r):         cast<int32>(r * r * 3);
    case Rect(w: width, h: height): cast<int32>(width * height);
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
`return`, which leaves the enclosing function. An arm of a value-producing `match` must therefore either
end in `:=` or **diverge** (`return` / `break` / `continue`); in particular a block arm cannot be *empty*, <!-- xfail: match_empty_arm -->
since it would leave the match's value unset:

```kama
string label = match (reading) {
    case Some(value: c): {
        string name = "mild";
        if (c < 0)  { name = "freezing"; }
        if (c > 30) { name = "hot"; }
        := name;                              // the arm's value (must be last)
    }
    case None: "unknown";
};
```

The `match` subject can be a variable, a method call, a static-method call, a free-function call
(`match (File.open(path: p, mode: OpenMode::Read)) { … }`), or a value-producing variant constructor
(`match (Optional::Some(x)) { … }` — the concrete instance is inferred from the payload). Arbitrary-integer
branching (not on an enum) is done with `if` / `else if` — there is no `switch`.

## Error model — `Optional` / `Result` ✅

The prelude provides two tagged-union types, so error handling needs no exceptions and no `null`:

- **`Optional<T>`** — `Some(T)` or `None`. A value that may be absent.
- **`Result<T, E>`** — `Ok(T)` or `Err(E)`. A value or an error.
- **`Unit`** — a single-variant enum (`Unit::Unit`), the empty value. It is the payload for a fallible
  operation that succeeds with nothing to return: `Result<Unit, E>` (the analogue of Rust's `Result<(), E>`),
  so **one** error convention — always `Result` — covers valued and void operations alike, with no
  placeholder payload. Example: `fn Result<Unit, IoError> remove(const ref string path)` in `std::fs`.

Both are ordinary tagged unions consumed by `match`, so the caller is *forced* to handle the empty/error
case (exhaustiveness):

```kama
fn Optional<int32> find(DynamicArray<int32> xs, int32 target) { … }

int32 idx = match (find(xs: list, target: 7)) {
    case Some(value: i): i;
    case None: -1;
};
```

`Weak<T>.tryUpgrade()` returns `Optional<Shared<T>>`; a fallible `static fn` factory returns `Result<T, E>`
(see Fallible construction above).

## Modules ✅

A **module is a FOLDER**, and a file's identity is **where it sits** — never anything it declares. A
project's `kama.json` lists its modules in a nested `modules` map mirroring the source tree, and a module's
name is the chain of keys read down to it, rooted at the project's `name`
(the *Modules* section above). `import` names a module by that full name; the
compiler resolves it through the manifest, compiles the module's files, and scopes their public symbols.
There is one keyword for depending on another module — `import` (it replaced `using`).

```jsonc
// geometry/kama.json
{ "name": "geometry", "version": "0.1.0", "kind": "library",
  "modules": {
    ".":         { "visibility": "public" },   // src/*.kama          — module `geometry`
    "graphics":  { "visibility": "public" },   // src/graphics/*.kama — module `geometry::graphics`
    "internals": { "visibility": ["graphics"] }
  } }
```

```kama
// geometry/src/graphics/texture.kama   — in module `geometry::graphics`, because of WHERE IT IS
export { Texture, scale };             // the public surface, at a glance — mirrors `import`

type resource Texture { ... }          // declarations carry NO visibility modifier
fn int32 scale(int32 x) { ... }
type resource GpuHandle { ... }        // unlisted → module-private

// main.kama
import {
    geometry::graphics::Texture,      // -> bare `Texture`
    geometry::graphics::scale,
    physics::Body as PhysBody,        // `as` renames
};
fn int32 main() {
    Texture t = ...;                    // imported, bare
    phys::Body b = ...;                 // alias-qualified
    int32 n = scale(x: 3);
}
```

**There is no `namespace` declaration.** A file used to name its own scope; the declaration and the path
could then disagree, so a file could be *compiled* into one scope and *imported* as another. Deleted
outright — `namespace` is not a keyword and writing one is a syntax error.

**One `import { … };` block per file, and every entry names a SYMBOL.** `a::b::X` is symbol `X` of module
`a::b`, always — there is no whole-module import, so the entry is never ambiguous. `as` renames
(`a::b::X as Y`), and an entry with **no scope** (`X`) names a symbol of this file's own module, because
there the scope is the only candidate. Importing any symbol of a module loads that module, so a qualified
`a::b::Y` stays available afterwards. There is no glob — unqualified-everything is deliberately not offered. Fully-qualified `a::b::X` is always available once
imported; the symbol list only controls what's *also* unqualified. Two imports binding the same bare name is
a compile error — disambiguate with `as`. An `as` alias may **not** claim a name that already roots a project <!-- xfail: import_alias_claims_global -->
this file can reach, which would leave the original unspellable
(`tests/xfail/import_alias_shadows_project.kama`).

**`visibility` decides what a module reaches beyond itself**, in four widening forms — a **list** of
modules in this project, `"children"` (every module nested under it, at any depth), `"internal"` (every
module in this project) and `"public"` (plus **dependent projects**, and the only form that crosses a
project boundary). ⚠️ **Nesting determines NAME, never ACCESS**: a narrow parent does **not** confine a
`public` child, which follows Go rather than Rust's implicit downward grant. A module's own files always
see each other, so a list never names itself.

**Visibility is per FILE. A file may name only what it DECLARES or IMPORTS** — `export { … };` is the
outbound half and `import` the inbound one, and the symmetry is the rule. A top-level `type`/`fn` leaves its
file only by being named in that file's one `export` block; a listed name must be a top-level declaration of
that same file, so a directory-module's files each state their own surface. **A qualified spelling reaches no
further than an `import` would** — `a::b::X` naming a non-exported `X` is the same error, in **every**
position: a call, a construction, a static call, a field, a parameter, a return type, a local declaration.

**A sibling in the same module is imported like anything else**, and needs no path to do it, because there
is exactly one candidate: `import { DynamicArray };`, then the bare name at every use. A module's files
share a name space but not a scope, so `export` offers a name and `import` accepts it — which is what lets a
reader name the source of every symbol in a file without leaving it. This is the rung **Go** does not have
(any file of a package reaches any unexported identifier in it) and the reason **Java** needed sealed JARs
and then JPMS. What `visibility` in `kama.json` governs is reach **beyond the module** (the *Modules* rules above) — it says nothing about files.

**An exported symbol may not name an unexported type of its own file.** A project's API is derived, never <!-- xfail: export_leak_field, export_leak_return -->
written down — the `public` modules of `kama.json` crossed with its files' `export` blocks — and that
enumeration is only usable if every name in it can be spelled by whoever reads it. Rust calls the family it
rules out `private_interfaces`. Publicly reachable positions only: a `private` field's type is not part of
what the export offers.

Declarations carry **no** visibility keyword, keeping `type`/`fn` syntax uniform, and the manifest
reads as the mirror of `import`. Member access (`public`/`protected`/`private`) is a separate axis; the
kama→host/WASM boundary (`expose`) is a third. **The prelude floor and the built-in `std::memory` triad are
intrinsics** — always in scope, never imported, outside the rung entirely. A file in **no** module — a loose
file the build was handed directly, sitting in the operand set's own root — keeps its symbols file-private,
so single-file scripts need no boilerplate and cannot be imported.

**Resolution.** `import { a::b::c::X }` names symbol `X` of project `a`'s module `b::c`, and the answer comes from a manifest,
never from a search: the project being built, then its declared dependencies, then the **stdlib bundled with
the compiler** (located relative to the binary like the runtime header, so `std::*` resolves on any install
regardless of cwd). `std`, `core` and `global` are reserved roots. A module already in the compilation
satisfies an import without a disk lookup. **A build with no `kama.json` does no searching at all** — you
pass every source file, and each one's module is its directory below the operand set's deepest common
ancestor, which makes `kama build a.kama b.kama` a genuine subset of a project build rather than a second
dialect. Loading is transitive and deduped by path, so import cycles load once. The stdlib is **optional on disk**:
no `import std::…` means the resolver never touches it, and nothing is auto-linked — a `no_std`-like floor
(only `kama_runtime.h` is mandatory; the prelude `Optional`/`Result`/`Deref`/`HeapOwner` is baked into the
compiler).

**A module import compiles only what it needs.** `import { a::b::X, a::b::Y }` resolves to the files of that module
which *declare* `X` and `Y`, plus their transitive closure within it — not to every file of the module. The
closure follows **references**, not `import` edges. That was once forced: a sibling was reachable with no
`import` at all, so an import-edge closure would have under-computed. It no longer is — every sibling
reference now carries an `import { … };` — but reference-following is kept because it is a **superset** of
the import graph and cannot under-compute even when the two disagree. A name a file declares itself is
satisfied there and pulls in no sibling, which is what keeps a repeated `extern fn memset` from tying three
files together — and is why an `extern` name, which keeps its literal C spelling and so is never
scope-prefixed, sits outside the file rung.

Anything the resolver does not fully understand loads the **whole** module, so the diagnostics are
unchanged: a bare `import a::b;` (nothing pins a file — and a type reached only through inference is never
spelled, so the importing file's own text cannot be used to seed one), a symbol the directory does not
declare, and a file whose declarations are
nameless but program-wide — a `type intrinsic` conformance on a primitive, or the `extern` seam that
`spawn`/`parallel_for` require.

Two consequences, both deliberate and both pre-1.0: a defect the compiler would report in a sibling file
nothing imports no longer fails the build, and a conformance that was arriving only because the whole directory loaded must
now be reachable. `KAMA_NO_PRUNE=1` restores whole-directory loading; `KAMA_PRUNE_TRACE=1` reports each
import's decision and `=2` names the reference that retained each file.

Passing several files to one build still works (`kama build a.kama b.kama -o app`); the compiler emits a
shared header (`<out>.gen.h`) + one `.c` per unit — imports just add the resolved module files to that set.

**Scope resolution uses `::`** (modules, qualified types, enum variants: `Color::Blue`); `.` is
**instance/value access only** (`obj.field`, `obj.method()`). The two are *syntactically* distinct, so
there's no module-vs-object precedence rule — a `::` head is always a type/module, a `.` head always a
value. This is **enforced**, not merely conventional: a `::` whose head is a local, a parameter or a field
is rejected with a message naming the `.` spelling, so field access has exactly one spelling <!-- xfail: scope_op_on_value -->
(`tests/xfail/scope_op_on_value.kama`). The one deliberate crossover is **dot-on-type for constructors** —
`Vec2.make(...)` constructs, `Vec2::dot(...)` calls a `static fn` — and the split is **enforced in both
directions**, so it is a real greppability guarantee rather than a convention: a `static fn` called with a
dot is rejected (`tests/xfail/dot_on_type_not_ctor.kama`) and a `ctor` called with `::` is rejected <!-- xfail: dot_on_type_not_ctor, scope_op_on_ctor -->
(`tests/xfail/scope_op_on_ctor.kama`), each naming the other spelling. A `ctor` is static (it takes no
`self`), so it would otherwise answer to both and `grep '\.make('` would miss half the construction sites.
The rule holds through a generic type parameter too — `T.deserialize(...)` for `T: Deserialize` — and for a
`ctor` added to a primitive by a `type intrinsic` block. Every spelling is pinned by
`tests/ctor_spelling_edges.kama`.

On a **generic type** both forms take a turbofish, and the same `.`-vs-`::` split applies:

```kama
Box::<int32>.make(v: 5)     // ctor   — dot
Box::<int32>::tag()         // static — colon-colon
```

Here the turbofish is **mandatory**, unlike for a ctor: a ctor can infer its instance from its arguments,
but a static has no receiver and its parameters need not mention `T`, so there is nothing to infer from.
(`Box<int32>::tag()` cannot be the spelling — in expression position `Box < int32 >` is two comparisons,
which is why kama has a turbofish at all.) Pinned by `tests/generic_static.kama`. Relatedly, a **self-returning `static fn` is rejected as a disguised <!-- xfail: self_returning_static_fn -->
constructor** (`tests/xfail/self_returning_static_fn.kama`): if it returns the enclosing type or
`Result<This, E>`, declare it a `ctor`.

**`main` is the entry point, not a symbol.** It is reached below the visibility system — every `main`
emits as the same C symbol, and the generated C `main` calls it directly — so **calling `main` is an
error**, **`main` may not be exported**, and it must be **unique per PROJECT** rather than per module: <!-- xfail: main_exported -->
two collide where `a::helper` and `b::helper` do not. Its location is unconstrained; location simply does
not scope it, which is exactly why it is not callable.

**`global::` names the root scope explicitly** ✅ (the C# spelling). `global::X` is the same symbol as a bare
`X` — the always-in-scope [floor](FLOOR.md). It exists for the case where a local declaration shadows the
spelling you want: a file that defines its own `envOr` still reaches the floor's with
`global::envOr(name: …, dflt: …)` (`tests/global_alias.kama`). It names **only** the floor: `global` is a
reserved project name, not a path prefix, so `global::a::b::X` is an error <!-- xfail: global_absolute_path -->
(`tests/xfail/global_absolute_path.kama`).

## Concurrency ✅

Kama earns data-race freedom the way it earns null-safety: by making the hazard **unrepresentable**
rather than checked. Where Rust proves exclusivity *over* shared memory (borrow checker, lifetimes,
`Send`/`Sync`, `Pin`, `async` colouring), kama **removes the shared mutable state**, so there is
nothing to prove. There is no `async`, no `await`, and therefore no function colouring: an ordinary
function is the only kind of function.

The model has three levels, and they all reuse the ownership rules already in this document.

### Isolates — `spawn` ✅

An **isolate** is a unit of shared-nothing execution: a real OS thread natively, a Web Worker on
wasm. It has its own stack, heap and module statics, and communicates only through channels and the
`Atomic<T>` seam. Isolates are meant to be *coarse* — roughly one per core, or a handful of
long-lived service isolates — which is what makes it honest for one to block.

**There are two spawn forms, and what separates them is who owns the join.**

```kama
import { std::concurrent::Isolate };

scope { spawn worker(p: give payload); }     // the SCOPE owns the join — at its closing brace
Isolate h = spawn worker(p: give payload);   // the HANDLE owns it; starts now, joins later
h.join();                                    // explicit join; `~Isolate()` also joins (RAII)
```

A bare `spawn` **must** appear inside a `scope { }`. <!-- xfail: parallel_spawn_no_scope --> The
handle form need not, and that is its reason to exist: `Isolate` is a `resource`, so its join
travels with the handle, which may be moved into a **field**. That is how a long-lived service
isolate is written — its join is owned by an object rather than by a block, and shutdown falls out
of RAII in field order (dropping the `Sender` closes the channel, which ends the worker's `recv`
loop, which the `Isolate` drop then joins). Either way a forgotten join is impossible: there is no
detach-by-forgetting.

#### The bundle ✅

An isolate entry is an ordinary top-level `fn` that returns **`void`** and takes **exactly one**
parameter — the **bundle**. <!-- xfail: parallel_spawn_no_scope --> It is `void` because there is
no channel to hand a result back over, and one parameter because the bundle *is* the boundary:
one object, one owner, one transfer. Everything an isolate receives, it receives here.

The bundle has two forms, and the choice is not a style preference — they restrict on **opposite
axes**:

| | restricts | is free in |
| --- | --- | --- |
| `give` — a moved `resource` | **the type** — it must be a `resource` (a `value`, a primitive and a `string` are all rejected) and it must be **sendable** | **lifetime** — the child may outlive the parent's frame, which is what the handle form needs |
| `ref T` — a borrow | **the lifetime** — only from a bare `spawn` inside a `scope`, over a place that scope outlives | **the type** — any place, of any size, `ref int32` included |

So a moved bundle transfers ownership (the parent provably cannot touch it afterwards), while a
borrowed one leaves the parent owning the storage — which makes `ref` the only way an isolate can
mutate something the parent goes on using, since `join()` returns `void` and a moved bundle never
comes back.

**How state reaches an isolate — the whole surface, in four rows.**

| | who may touch it | lifetime | why it is sound |
| --- | --- | --- | --- |
| a moved `resource` | the child, exclusively | unbounded | ownership transferred |
| `ref T`, **disjoint** places | one child per place | the `scope` | no two children see the same bytes; the join orders the parent's read |
| `ref Atomic<T>`, the **same** place | every child | the `scope` | atomic operations do not race |
| `Shared<immutable T>` | every isolate | unbounded | nothing can change |

Rows 2 and 3 are what a `scope` licenses, and they are the reason it exists: its closing brace
joins **every** child before any local declared in it drops, so a borrow provably cannot dangle,
the child's writes are ordered against the parent's next read with no atomic at all, and the scope
is the domain over which "no two children overlap" is decided. Rows 1 and 4 need no scope because
they are self-sufficient — one transfers exclusivity, the other removes writing from the picture.

**Sendability applies to the bundle**, exactly as it does to a channel element: a bundle that
transitively holds a `Shared`/`Weak` over a **mutable** payload is rejected, naming the offending <!-- xfail: spawn_bundle_shared -->
field, because both isolates would release one non-atomic control block.
The same shape over a deeply-`immutable` payload is legal <!-- test: spawn_bundle_immutable --> —
that is the case whose control block switches to an atomic refcount, and it is what lets many
isolates share one large read-only asset with no copy.

### Channels ✅

A `Channel<T>` is a typed pipe. `Channel.bounded(capacity:)` sets the buffer depth; capacity `0`
is a rendezvous channel. `sender()` and `receiver()` hand out owned endpoints you move to whoever
needs them.

```kama
import { std::concurrent::Channel, std::concurrent::Sender, std::concurrent::Receiver, std::concurrent::Isolate };

Channel<int32> ch = Channel.bounded(capacity: 4);
Sender<int32>   tx = ch.sender();
Receiver<int32> rx = ch.receiver();

Isolate h = spawn producer(tx: give tx);     // the sender is moved into the isolate

Optional<int32> v = rx.recv();               // blocks; None once closed AND drained
```

`send(item:)` returns `SendResult<T> { Sent, Undelivered(T item) }` — a channel whose receivers are
all gone hands the item **back** rather than dropping it on the floor, so nothing is silently lost
and the sender decides what to do. `recv()` returns `Optional<T>`: `None` means the channel is
closed and empty, which is why dropping the last `Sender` is how a producer signals end-of-stream.

**Endpoints are counted, so a channel is many-to-many.** `sender()` and `receiver()` may each be
called any number of times; every call registers another endpoint, and a *side* closes only when its
last endpoint drops. Several `Receiver`s over one channel is a **worker pool** — each item goes to
exactly one of them <!-- test: channel_pool_recv --> — and several `Senders` is a fan-in,
<!-- test: channel_multi_send --> rendezvous included <!-- test: channel_rendezvous_multi -->. The
count moves exactly where ownership does, the same way a `Shared<T>` control block works, so the
last endpoint to drop is the one that frees the queue. There is no `clone()` on an endpoint and none
is needed: a `Channel<T>` is itself a move-only `resource`, so it can be moved *into* an isolate
whose worker then mints its own endpoint there. <!-- test: channel_mint_in_isolate -->

A side is also open **before it is ever claimed**, which is what lets that last pattern work: a
receiver may call `recv()` before the isolate holding the `Channel` has minted its `Sender`, and it
blocks rather than reading "no senders yet" as end-of-stream.

**Sendability is computed, not declared.** There is no `Send` marker to write or forget. A type is
sendable iff it is a `value` whose fields are all sendable, a `resource` (transferred by move), or a
`Shared`/`Weak` over a deeply-immutable type. What is **rejected** is a **non-atomic shared refcount** — a <!-- xfail: channel_send_shared -->
`Shared`/`Weak` over a mutable payload, or anything transitively containing one — with an error naming the
offending field, the same way the escape check reports.
A `view` and a bare `contract` value need no rule here: neither can be a field at all, so neither ever
reaches a bundle. <!-- xfail: view_field, iface_field -->
A raw **`UnsafePtr` does cross**, and that is the `unsafe` seam working as designed rather than a hole: the
bundle is built in an `unsafe ctor` and read through an `unsafe fn`, and **joining the child** — `scope`'s
closing brace, or an explicit `h.join()` — is what orders the write against the parent's read. It is the
primary isolate idiom, not an edge case. <!-- test: isolate_basic -->

### Structured concurrency — `scope` ✅

A `scope { }` block joins every child spawned inside it at its closing brace. It is RAII applied to
tasks: deterministic lifetimes, no orphans, and — because a child is guaranteed to be joined before
the scope exits — a child may safely borrow from the enclosing scope.

```kama
scope {
    spawn writer(s: give sa);     // a bare `spawn` inside a scope is a deferred-join child
    spawn writer(s: give sb);
}                                 // BARRIER: both joined here, before anything below runs
```

A bare `spawn` must be a **direct statement of its scope**; one inside a nested block is rejected. <!-- xfail: spawn_in_nested_block -->
So a scope's children are exactly the `spawn`s written in it, and can be counted by reading the
block. That matches the coarse-isolate model above: roughly one isolate per core, not one per work
item. A conditional child puts the `if` *outside* the scope <!-- test: scope_spawn_conditional -->,
and per-item work over a container is `parallel_for`.

#### What a child may borrow

A child borrows a **place** — a local, or a chain of field names on one: `spawn step(a: ref w.bodies)`
beside `spawn step(a: ref w.springs)` gives two children two disjoint fields of one world.
<!-- test: scope_borrow_disjoint_fields --> Two children may not borrow **overlapping** places, and
two places overlap exactly when one is a prefix of the other, so `ref w` beside `ref w.bodies` is
refused <!-- xfail: scope_borrow_nested_prefix --> and so is the same local twice.
<!-- xfail: scope_borrow_same_root --> An `Atomic<T>` is exempt — it is the sanctioned shared-mutable
cell, so several children may share one.

This needs no lifetime analysis, and that is the point. A field's storage is **inside** its root's
storage, so it is created and destroyed exactly with the root; bounding the root bounds the field.
The prefix test is the same `placesConflict` the `borrow` window uses, so one rule serves both.

Two shapes are refused because they are not places. An **element** (`ref xs[0]`) has a runtime index
no static rule can pin <!-- xfail: scope_borrow_element --> — splitting a buffer across workers is
`parallel_for`'s job, which supplies the proof this rule cannot. A **call result** designates no
place at all.

A place may project through an indirection only when the handle is **unique**. An `Owned<T>` is —
nothing can copy one — so distinct roots really are distinct objects and its fields are borrowable.
<!-- test: scope_borrow_owned_fields --> A `Shared`/`Weak` is not: another handle may name the same
object, so `a.bodies` and `b.springs` could be one cell under two non-prefix names, and it is
refused. <!-- xfail: scope_borrow_through_shared -->

### Data parallelism — `parallel_for` ✅

`parallel_for (ref T e in coll) { … }` splits `coll` into K non-overlapping sub-`View`s, one per
worker isolate, runs the body over each in place, and joins them all at its own closing brace. It is
safe **by disjointness** — two workers never touch the same element — so it needs no lock and no
borrow checker.

```kama
parallel_for (ref int32 e in xs, workers: cpuCount()) { e = e * 2; }   // closing brace is the barrier
```

**`workers:` is mandatory — there is no default.** kama has no optional parameters (a user `fn` cannot
declare one), so a built-in construct does not get one either. Write `workers: cpuCount()` for one slice
per core, `workers: 4` for a fixed width, or any expression: `workers: cpuCount() - 2` to leave headroom.
It is *exactly* what you asked for, capped only by `length` (more workers than elements would leave some
with no slice) — not silently reduced to the core count, since oversubscription is the caller's call.
A literal `workers: 0` or negative is a compile error. <!-- xfail: parfor_no_workers -->

Stating it is the point: how many isolates a loop splits into used to be invisible, which is what made the
chunking below surprising. Now `grep 'workers:'` finds every parallelism-width decision in a codebase.

`ref` is mandatory: disjoint *mutable* access is the entire point. The input is a `View<T>` or any
contiguous container that exposes `.view()` (`DynamicArray`, `FixedArray` are auto-viewed); a
non-contiguous container such as a `Map` has no `.view()` and is rejected. <!-- xfail: parfor_noncontiguous -->

**There are usually FEWER workers than elements** — `workers: 8` over 30 elements is 8 isolates running 4
elements each, one after another. That is the point of the construct: the work is finite and independent,
so four elements run back to back in the same total time, and 40,000 elements need not become 40,000 OS
threads.

The one thing it asks of the body: **an element may not wait on another element's progress**, since only
`workers:` of them are ever in flight. A body that blocks until *all* elements have reached some point waits forever —
the count stalls at `workers:`, and the elements queued behind those never start. Ordinary independent work is
unaffected. When you genuinely need all N running at once — a pool whose members rendezvous — that is
`parallel_spawn` below, which is also why `parallel_for` cannot build one.
<!-- test: parallel_spawn_oversubscribed -->

### Worker pools — `parallel_spawn` ✅

`parallel_spawn (ref T w in coll) { … }` starts **one long-lived isolate per element** and hands the join
to the enclosing `scope { }`. That second half is the whole point: the children run *alongside* the
statements after it.

```kama
scope {
    parallel_spawn (ref Worker w in workers) { w.run(); }   // K = workers.length()
    {
        Sender<int32> tx = ch.sender();                     // …and this runs WHILE they run
        int32 n = 0;
        while (n < 100) { tx.send(item: n); n = n + 1; }
    }                                                       // sender drops: every worker wakes and finishes
}                                                           // BARRIER: all K joined here
```
<!-- test: parallel_spawn_pool -->

This is what makes a pool sized to the machine writable — `workers` is built by an ordinary loop, so
`cpuCount()` elements means `cpuCount()` isolates. A bare `spawn` cannot: it must be a direct statement of
its scope, so K `spawn`s means K typed statements. `parallel_for` cannot either, for a different reason —
it spawns *and joins* in one statement, so the producer above would never run and the program would hang.

**Two differences from `parallel_for`, and both are load-bearing:**

- **K is `length()` exactly — never capped at the core count.** A cap does not skip the extras, it runs
  several per isolate *in turn*, and a pool worker blocks until its channel closes. A capped worker would
  therefore never start, and it would fail *silently*: the running workers drain the queue, the producer
  closes, they exit, and only then do the rest begin and immediately see the closed end.
  <!-- test: parallel_spawn_oversubscribed -->
- **No `workers:` clause** — its count IS the container's length, so a second count could only contradict
  it. To choose how many, size the container. <!-- xfail: parallel_spawn_workers -->
- **The join is the `scope`'s brace**, on every exit path, before any local drops — the same
  join-before-drop guarantee a bare `spawn` gets. So it needs an enclosing `scope`
  <!-- xfail: parallel_spawn_no_scope --> and must be a direct statement of it.
  <!-- xfail: parallel_spawn_nested_block -->

⚠️ **The coarse-isolate rule still binds, and this construct makes it easy to break.** An isolate is an OS
thread (a Web Worker on wasm), so `parallel_spawn` over a container of *work items* is the anti-pattern
named above — one isolate per item — now spelled in one line. The container should hold **workers**: a
`cpuCount()`-sized pool, one per GPU device, one per shard. Oversubscription itself is fine and sometimes
right (blocked workers cost nothing but stack), which is why there is no cap; the cost that bites is stack
reservation per thread and, on wasm, Worker exhaustion. The compiler cannot tell a worker from a job, so
this is a rule you keep, not one it checks.

### The three sharing seams ✅

Cross-isolate state is confined to three greppable seams, the same way raw memory is confined to
`unsafe fn`:

| Seam | Meaning | Native | wasm | Bare metal |
| --- | --- | --- | --- | --- |
| module `static` | **per-isolate** state — each isolate gets its own copy | `_Thread_local` | `_Thread_local` (emscripten pthreads share one linear memory, so TLS is what makes it per-isolate) | a plain C `static`, zero cost (one core = one isolate) |
| `hardware` | `volatile` MMIO and the single-core ISR↔loop flag | `volatile T*` | n/a | the register/ISR seam — *not* cross-isolate |
| `Atomic<T>` | the **only** cross-isolate mutable sharing | `_Atomic` / `<stdatomic.h>` | Atomics over a SharedArrayBuffer | atomics, if multicore |

The load-bearing rule: **a module `static` is per-isolate by construction, so it cannot be observed
by another isolate and therefore cannot race.** To share mutable state you must reach for
`Atomic<T>`, which is visible in a grep.

That is the rule about *storage*. For the rule about how state **reaches** an isolate in the first
place — the bundle, and the four ways anything crosses — see [The bundle](#the-bundle-) above; the
two answer different halves of the same question and neither is complete alone.

### `Atomic<T>` ✅

```kama
import { std::concurrent::Atomic, std::concurrent::MemoryOrder };

Atomic<int32> counter = Atomic.make(value: 0);
counter.fetchAdd(delta: 1);
int32 now = counter.load();
```

`load` / `store` / `swap` / `compareExchange` / `fetchAdd` / `fetchSub`, all sequentially consistent
by default. Each has an `…Explicit` form taking a `MemoryOrder` for the expert case. Atomics are the
whole shared-mutable surface; general shared mutable memory stays outside the safe language.

### Immutable sharing — `type immutable` ✅

The other way to share safely is to share something that cannot change. `type immutable value T` (or
`type immutable resource T`) marks a type **deeply** immutable, which the compiler verifies: every
field, base and variant payload must itself be a primitive, a `string`, an `enum`, or another deeply
immutable type. A mutable member is a compile error naming that member. <!-- xfail: immutable_mutable_field -->
**A primitive means every primitive**, the platform-varying `isize`/`usize` included — they are scalars <!-- test: immutable_size_types -->
with no interior mutability, so they are as shareable as `int64`. That half went unenforced and was
wrongly rejected until `0.9.144`: the compiler's own list said "a primitive" while its check read an
ordered span of builtin type values that `isize`/`usize` are deliberately appended *after*.

It is a **type** qualifier and nothing smaller. `immutable` on a field or a method is a hard error, not <!-- xfail: immutable_on_field, immutable_on_method -->
a silent no-op: the guarantee is deep and whole-type — one mutable field anywhere breaks it — so a
per-member spelling could promise nothing. `const` is the per-field promise (write-once, constructor
only) and `const fn` the per-method one.

A `Shared<T>` over a deeply-immutable `T` is sendable, so any number of isolates can hold and read
the same asset with no copy. Its control block switches to an atomic refcount only in that case, so
an ordinary single-isolate `Shared` pays nothing. This is distinct from a `const` binding, which
only promises *this* alias will not mutate and therefore cannot license cross-isolate sharing.

### Why not green threads or `async`/`await`

Both exist to serve "proceed until ready". Stackful green threads need a userspace stack-switching
scheduler, which on wasm means Asyncify — precisely the colouring cost being rejected. `async`/`await`
colours every function and drags in pinning. Kama takes neither into the *language*: isolates are
real threads, and blocking is honest when they are few; massive parallelism comes from the
never-blocking data-parallel layer. The "multiplex thousands of connections over a few threads"
ergonomic is a **library** concern above the language — a native scheduler can back the very same
blocking-shaped surface with fibers, with no language change and no effect on wasm.

## Serialization — `@`-attributes + `@generate` ✅ (intrinsic implementation complete — by-value + full object graph + polymorphic `Shared<Contract>`; see [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §4)

Opt-in, compile-time serialization. The **user-facing surface is just contracts + attributes**; the *wire
format* is library; **everything structural (the field walk + the object-graph machinery) is a compiler
intrinsic** — a lowering to C, not synthesized kama. This split is deliberate: reflection and the
ownership-graph rebuild are core language guarantees (the compiler already owns type layout, the RAII model,
and the smart-pointer internals), so it emits them directly and correctly rather than fighting surface-language
restrictions — and nothing leaks into the public API.

**Three layers.**
- **User-facing (opt-in):** the contracts `Serialize` / `Deserialize<T is This>`, the attributes
  `@generate(Serialize, Deserialize)` (per-direction) + `@field` / `@field(name: "wire")` / `@skip`, and one
  entry pair `encode(v:)` / `decode::<T>(src)`. A **hand-written `serialize`/`deserialize` wins** — the intrinsic
  only synthesizes for a `@generate` type that supplies none (override = implement the contract yourself).
- **Library (wire backends, swappable):** the `Serializer` / `Deserializer` contracts (`writeInt32`/`readInt32`/…,
  `beginObject`/`fieldName`/…, and the graph framing `writeRef`/`beginGraph`/…) + `DeError`. `std::serialization::json`
  (text) and `std::serialization::binary` (**KBIN** — a compact self-describing little-endian tagged format) both
  ship; yaml/xml/user backends are just new implementors — no compiler change. A type opts into serialization
  ONCE (`@generate(Serialize, Deserialize)`) and works with every backend automatically, since the generated code
  drives only the format-agnostic token contract. The **binary** backend is byte-oriented — `binary::encode`
  yields a `DynamicArray<uint8>` and `decode` takes bytes (not a `string`, since binary isn't valid UTF-8) — and
  streams over the same `Writer`/`Reader` substrate as JSON (so it flows to a file or socket for game-save /
  network payloads). It stores raw IEEE-754 bits (NaN/Inf round-trip) and is self-describing, so `skipValue`
  works and unknown fields skip cleanly (forward-compatible).
  **JSON is UTF-8 in and out.** `string`/`char` are written as raw UTF-8 bytes — JSON is a UTF-8 format
  (RFC 8259 §8.1), so escaping buys nothing — and only `"`, `\` and the control bytes are escaped. On
  READ, `\uXXXX` is decoded to UTF-8, **including surrogate pairs**: JSON inherited UTF-16 escapes from
  JavaScript, so a codepoint above the BMP arrives as a `😀` pair, which is the ordinary shape
  of JSON produced elsewhere (Python's `json.dumps` escapes *all* non-ASCII by default). The pair is
  combined at the wire edge and nothing above it ever sees a UTF-16 code unit — kama stays UTF-8
  everywhere. An **unpaired** surrogate is malformed input and is rejected, not encoded as WTF-8. <!-- test: ser_json_unicode -->
  (Fixture: `tests/ser_json_unicode`.)
- **Intrinsic (compiler):** the per-type field walk and the whole graph machinery (id table, heap shells,
  two-pass wire, ownership transfer, ordering, cycles). Zero-cost — emitted **only** for `@generate` types.

**Two modes, gated by `reachesPointer(T)`** — a precomputed per-type flag (the tighter sibling of the
`destructible` transitive-ownership walk): true iff `T` transitively reaches a `Shared`/`Weak`/`Owned` field
(recursing through owned fields and collection elements; strings/scalars/enums add nothing). You get back
exactly what you name:

| You name | `reachesPointer` | Result |
|---|---|---|
| `int32` / `MyEnum` / `MyValueType` | — / false | by value (stack) |
| tree `resource` (`User { string name }`, `DynamicArray<int32>`) | false | by value (stack) |
| `Shared<T>` (value **or** resource) | any | heap graph (one node or many) |
| bare graph type (`decode::<Node>` where `Node` reaches a pointer) | true | **compile error** → "reaches a pointer; decode as `Shared<Node>`" |

- **By-value (tree):** a `value` type (owns nothing) or a pointer-free `resource` (strings, collections, nested
  owned data — a tree, no aliasing) serializes to a bare object/array and `decode::<T>` returns it **by value**.
- **Graph (heap):** anything reaching a `Shared`/`Weak`/`Owned` is a graph — it can alias, cycle, or hold a
  `Weak` back-edge, none of which survive a by-value return — so it is **always heap**, even a single node.
  `encode` writes the id-table envelope `{"root":id,"objects":{id:{"__type":…,…}}}`; `decode::<Shared<T>>`
  rebuilds it and returns the owning root handle. A `value` type is welcome in a graph *via* `Shared` (a
  one-node heap graph); a live pointer field in a `value` type is a compile error (pointers need a graph). <!-- xfail: value_owns_resource -->

**Common rules (both modes).**
- **Per-field marks are mandatory** on a `@generate`d product: each field is `@field`, `@field(name: "wire")`,
  or `@skip` — an unmarked field is a **compile error** (no silent omission). <!-- xfail: ser_unmarked_field -->
- **Enums** serialize externally-tagged: `{"tag":"V"}` (no payload) / `{"tag":"V","value":{fields…}}` (payload);
  deserialize reads the tag, dispatches, constructs; an unknown tag → `DeError`.
- **`Map<K,V>`** serializes as an array of `{"key":…,"value":…}` pairs (a generic key can't be a JSON object key).

**Graph specifics.** `Shared`/`Weak`/`Owned` fields serialize as integer ids into the side table (`0` = null /
expired). `Shared`/`Weak` dedup by pointee identity; a `Weak` writes its id only while a strong handle exists.
`Owned` is unique-owner (a tree of nodes), reconstructed **give-once** — a duplicate owned id on the wire is a
`DeError::DuplicateId`. Cycles ride `Weak` back-edges; a dangling id → `DeError::UnresolvedReference`. A
polymorphic edge — `Shared`/`Weak`/`Owned<Contract>` — reconstructs the concrete type from each node's `__type`
tag and re-forms the fat handle with that concrete's vtable; a tag naming a type that doesn't implement the
contract → `DeError::TypeMismatch`. Every nominal implementor of a contract used as a graph edge **must** be
`@generate(Serialize, Deserialize)` — this is **compile-enforced**: a non-`@generate` implementor (which would
have no node writer and be silently dropped from the wire) is a compile error at the edge field. <!-- xfail: poly_edge_nongenerate -->
`DeError` = `{Malformed, UnexpectedEnd, TypeMismatch, MissingField, UnresolvedReference, DuplicateId}`.

The `Owned`/`Shared`/`Weak` triad is **prelude / built-in** (always in scope, no `import`) — RAII-over-GC is the
core model; see [TYPE_MODEL.md](TYPE_MODEL.md).

```kama
import { std::serialization::json::encode, std::serialization::json::decode };   // wire backend (library); the triad needs no import

// by-value (tree): a pointer-free resource round-trips on the stack
@generate(Serialize, Deserialize)
type resource User { @field(name: "user_name") string name; @field int32 age;
    public ctor make(string name, int32 age) { User r; r.name = give name; r.age = age; return give r; } }
string j = encode(v: User.make(name: "ada", age: 36));            // {"user_name":"ada","age":36}
Result<User, DeError> u = decode::<User>(src: give j);            // by value

// graph (heap): reaches a pointer -> only via Shared; cycles rebuilt through the Weak back-edge
@generate(Serialize, Deserialize)
type resource Node { @field int32 id; @field Optional<Shared<Node>> next; @field Optional<Weak<Node>> back; … }
Result<Shared<Node>, DeError> g = decode::<Shared<Node>>(src: give wire);
// decode::<Node>(...) would be a compile error: Node reaches a pointer -> decode as Shared<Node>
```

## Building & debugging ✅

```sh
kama build app.kama                          # this host, debug (-g, breakpoints in .kama via #line)
kama build app.kama --release                # optimized, stripped, NDEBUG
kama build app.kama --target wasm            # browser: .html + .js + .wasm
kama build app.kama --target EMBEDDED        # bare-metal: a -ffreestanding -nostdlib object (.o)
kama build app.kama --target aarch64-linux-gnu --cc "zig cc"   # cross-compile to any triple
kama build lib.kama --select OUTPUT=STATIC   # a static library (libapp.a)
```

Toolchain setup per platform lives in **[targets.md](targets.md)**.

**`--target`** takes a built-in name (`HOST`, `MACOS`, `WINDOWS`, `LINUX`, `WASM`, `EMBEDDED`), a target
your `kama.json` declares, or a bare `<arch>-<os>-<abi>` triple. Every compile and link flag follows the
selected target rather than the machine you are building on, so cross-compiling is a matter of having a
C compiler that can reach the target: `zig cc` does out of the box (it ships musl/mingw-w64/wasi-libc),
or declare a `cc` for the target in `kama.json`. Without one, `kama transpile --target …` always works —
emit the C and build it with someone else's toolchain.

**A bare-metal target** (any triple with `os=none`, of which `EMBEDDED` is the shortcut for this host's
arch) compiles to a `-ffreestanding -nostdlib` **object** rather than a linked executable. The synthesized entry becomes `int main(void) {
kama_main(); for(;;){} }` — no `argc/argv` (there is none), and `main` never returns (a startup/crt0 calls
it and it spins). Fatal conditions (bounds/panic/OOM) route through an overridable **weak `kama_panic_handler`**
(default `for(;;) __builtin_trap()`) — provide a strong symbol to blink/reset/breakpoint. Name the board's triple directly
(`--target thumbv7em-none-eabihf`, with a `cc` that can reach it), and link the object with
your chip's startup object + linker script (memory map) as a separate step — turnkey triples, linker scripts,
and vendor HALs are a later milestone. A module `static hardware UnsafePtr<T>` lowers to a `volatile T*` MMIO register,
and module `static`s become plain zero-cost `static`s (one core = one isolate).

Debug builds are breakpoint-debuggable in an IDE (locals + call stack map back to `.kama`), and emit **one
`.c` per module** (faithful stepping, readable generated code). A **`--release`** native build instead folds
every module into **one unity translation unit** so the C compiler can inline across module boundaries — a
`std::math` operator or a collection accessor inlines into the caller's hot loop and then auto-vectorizes,
which is what lands numeric code at C parity (kama has no incremental object cache, so a build already compiles
all modules in a single invocation — the unity fold costs nothing and only unlocks inlining). Numeric-safety
traps (`integer-divide-by-zero`, `shift-exponent`, `float-cast-overflow`, `signed-integer-overflow` → a clean
`__builtin_trap`, no sanitizer runtime) are on in **every** build; signed overflow additionally wraps
(`-fwrapv`) in release.

**One UBSan sub-check is permanently exempt: `function`.** A kama program built under
`-fsanitize=undefined` should add `-fno-sanitize=function`, as the test suite does. This is a **deliberate,
permanent exemption**, not a workaround for an unfixed defect. Contract, vtable and `fnptr` dispatch store
every slot as `Ret (*)(void* self, …)` and call the concrete `Ret C__m(C* self, …)` through it. That
type-erased `self` is ABI-identical — it is how essentially all C object dispatch works, GObject and COM
included — but the `function` sub-check enforces exact function-pointer *type identity*, so it would flag
every contract call in a correct program. Every other UBSan check (integer overflow, null, bounds,
alignment, …) and all of ASan stay on. The exemption costs no real coverage: the emitter generates both
sides of a slot from one declaration, so a genuine signature mismatch fails to compile rather than
reaching a sanitizer. Making the pointer types exact would mean emitting a cast-and-call thunk per slot,
which buys nothing and adds an indirection to every dynamic call — expressly the wrong trade for the
embedded and hot-path targets.

## Reserved keywords not yet implemented 🚧

One keyword has **reserved surface not yet implemented** — using it is a **hard error** (never a silent no-op): <!-- xfail: expose_generic, expose_bad_abi, expose_on_method -->

- **`expose`** 🚧 — the minimal free-function C-ABI symbol ships today; its **full** 2.0 surface (richer WASM
  module exports, the scripting-host interface) remains reserved, distinct from in-language `public`/`private`
  (member access) and `export` (the module public-surface manifest — `export { … };`, which ships today).

`volatile` is **not** a kama keyword: C's `volatile` is spelled `hardware` (emits C `volatile` for MMIO
registers and single-core ISR↔loop flags — see *Module-level statics* and ROADMAP_DETAIL §5). It is,
however, **reserved** — see below.

## kama's keywords

**The complete list, and the reason it is printed here**: every one that is not published is found by
walking into it. The first external project found three that way — `base`, `type`, `slot` — each costing
a build cycle to a parse error that names the token (`unexpected SLOT`) without saying that the word is
reserved. `tools/check-keyword-list.sh` holds this list identical to the lexer's table, so it cannot
drift.

```
abstract alignof as asm base bitcast bool borrow break case cast char comptime const continue
copy ctor default do else enum export expose extends extern false file final float32 float64 fn
fnptr for foreach friend give hardware if immutable implements import in int16 int32 int64 int8
isize match new null operator out override parallel_for parallel_spawn private protected public
ref return scope sizeof slot spawn static string this true truncate try type uint16 uint32
uint64 uint8 unsafe usize virtual void when while
```

**Six of them are CONTEXTUAL** — `copy`, `give`, `truncate`, `type`, `slot` and `file` may name any
binding (a field, a local, a parameter, an argument label, a member) and lead a declaration only where a
declaration can begin. Each was made contextual for the same reason: the word is one a program genuinely
wants as a name, and admitting it cost **no bison conflicts** at any name position. `type` is what lets an
FFI binding emit a C field literally called `type` without inventing a name
(`tests/extern_field_type_keyword.d/`); `slot` is the natural name for an index into a table
(`tests/contextual_slot.kama`); `file` leads the file gate below and is otherwise an ordinary name
(`tests/contextual_file.kama`) — `File file = …` is the spelling a user reaches for first, and measured,
the word is not an identifier anywhere in kama's own sources, so this arm exists purely for their code. The kind words `value` / `resource` / `view` / `intrinsic` are not
keywords at all — they lex as identifiers.

`this` and the primitive type names are keywords like any other: they cannot be redeclared.

## C's reserved words are reserved in kama

**Every C11 and C23 keyword is a reserved word in kama and cannot be used as a name** — not for a type, a <!-- xfail: int_retired, double_retired -->
function, a field, a parameter, a local, an enum case, a variant payload, a generic parameter, a `foreach`
or `match` binding, or an `expose`d/`extern` symbol. Writing one is a **lexical error** that names the
spelling.

kama compiles to C, so a name that is a C keyword emits C that does not compile: `int32 switch;` becomes <!-- xfail: int_retired, double_retired -->
`int32_t switch;`. Renaming such a name on the way out (`switch` → `k_switch`) was considered and rejected —
reserving a spelling now and relaxing it later is source-compatible, while the reverse is not.

Twenty-one of the 59 are already kama keywords, so they were never spellable. The **38** that would
otherwise lex as identifiers are:

```
alignas auto constexpr double float goto inline int long nullptr register restrict short signed
static_assert struct switch thread_local typedef typeof typeof_unqual union unsigned volatile
_Alignas _Alignof _Atomic _BitInt _Bool _Complex _Decimal128 _Decimal32 _Decimal64 _Generic
_Imaginary _Noreturn _Static_assert _Thread_local
```

Four of these are spellings a C, Go or Java reader reaches for, and their diagnostics name the replacement
rather than merely reporting that the word is reserved: `int` → `int32`/`isize`, `double` and `float` →
`float64`/`float32`. **`uint` is not a C keyword and so is not reserved** — it is rejected in type position <!-- xfail: uint_retired -->
only, with the same guidance (`uint32`/`usize`). Both halves are guarded by `tools/check-c-keywords.sh`,
which also holds the reserved table equal to the two C standards' sets.

## Known limitations (tracked → [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §1)

Everything below **hard-errors** (never miscompiles) and has a clean workaround. Two kinds:

**By-design rules** — an rvalue can't be borrowed/reseated soundly, so these stay errors, not "unbuilt":
- **An inline `new` (or owned value) borrowed by a `ref`/`out` or contract parameter** — an inline `new` is
  consumed **by value** (the callee/caller becomes the owner). To borrow it (`ref`/`out`) or reseat a
  contract handle, bind it to a local first — an rvalue has no stable lvalue to write back to. (An inline
  *stack* ctor into a **by-value** contract param does work — `f(a: Square.make(n: 3))` — since the callee only
  borrows the caller-owned temp.)
- **An inline construct in a `do/while` condition** — the temp is needed at the bottom condition, which
  `continue` must reach; a portable (statement-expression-free) ISO-C lowering can't express it. Bind to a
  local.

**Open (deferred inference)** — a rare residual; bind the subject to a typed local:
- **A value-producing `match`/ternary as a `match` SUBJECT** — a bare variant constructor subject
  (`match (Optional::Some(x)) { … }`) works: the instance (`Optional<T>`) is inferred from the payload
  during the discovery pass (a function-level pre-scan supplies the param/local types) and reused at emit.
  A **call** subject works too, for every call shape and for a plain (payload-less) enum as well as a
  tagged union — `match (classify(x: 1))`, `match (g.grade(score: 70))`, `match (Grader::always())`
  (`tests/match_call_plain_enum.kama`). The still-deferred forms are a *nested* value-producing `match` or
  a *variant-producing ternary* directly as a subject; bind those to a typed local
  (`Optional<int32> o = …; match (o) …`).

(Target-typed inline construction works in initializers, `return`, `operator[]` place-stores,
value-producing `match` arms, class-typed lvalue stores, call arguments, variant payloads, and string-rvalue
indexing. Inline `new` heap-boxes into an owning pointer — `Owned`/`Shared`, concrete OR contract element —
in every by-value position. An owned rvalue receiver is RAII-dropped through method chains
(`b.make().use()`) and for `.chars()`/`.split()` over an owned rvalue.)

## Reserved/runtime

Generated C reserves `__`-prefixed identifiers (`__base`, `__vptr`, `__ret_N`) and `Type__member` mangling.
The runtime ([../include/kama_runtime.h](../include/kama_runtime.h)) provides `kama_string` and a
`kama_trace`/`kama_trace_get` hook used by tests.
