# Working on a kama project

Instructions for an AI coding agent. `kama` is a C-family language (C#-like syntax, no garbage
collector, RAII, mandatory named parameters) that compiles to portable C.

Every tool that reads `AGENTS.md` picks this up automatically. For the ones that don't, run
`kama agents list`.

## Ask the compiler instead of guessing

`kama query` answers from what the compiler actually **resolved**, not from what a grep matched.
Prefer it to searching the tree — it is one process, one file, no editor and no language server.

```sh
kama query <kama.json> <file> --search Widget # find a symbol BY NAME, across the package
kama query <file> --symbols                   # outline of one file
kama query <file> --complete L:C              # candidates + FULL signatures with parameter names
kama query <file> --type L:C                  # what is this, exactly
kama query <file> --def L:C                   # where is it declared
kama query <kama.json> <file> --refs L:C      # every use, across the package
kama query <file> --diagnostics               # analysis diagnostics, structured
```

**The SCOPE is an operand, not a flag.** With just a file, the query sees that file's import closure;
put `kama.json` (or `kama_workspace.json`) *before* it and the same question is asked across the whole
project — which is the only way to reach a symbol you cannot already point at.

Add `--json` to any of them for one stable envelope: `{"schema":1,"mode":…,"file":…,"results":[…]}`.
Coordinates are **1-based line, 0-based column** — do not assume both are 1-based.

**Ask everything about a file in ONE command.** Modes are repeatable and combinable, answered in the
order given from a single analysis — and that analysis is essentially the entire cost of a query, so
this is a large saving, not a tidy one:

```sh
kama query <file> --def 24:9 --type 24:9 --refs 24:9   # one analysis, three answers
```

Each answer is preceded by a `## <question>` line (under `--json`, each record carries an `ask` echo).
A lone question prints exactly what it always did.

Reach for `--search` first. It is the only mode that takes a name rather than a cursor position, so
it is usually the cheapest way in. **Never invent a function signature** — `--search` to find it,
then `--complete` or `--type` to read its real parameter names.

## Verify with `kama build`, not `kama check`

```sh
kama build <file>      # the real check: compiles, and reports type errors
kama run               # build the manifest entry and run it
kama check <file>      # FAST SUBSET — see below
```

`kama check` runs name resolution, named-argument matching, ownership/move analysis, and type checking
**by kind** — a value is a number, a `bool`, a `string`, or a type value, and crossing between two of
them (`int32 x = "oops";`) is rejected — wherever a value crosses into a destination of a stated type <!-- xfail: narrow_local, narrow_argument -->
(an initializer, an assignment, a `return`, a `match` arm, an argument, an enum payload). It also checks
**width**, at those same places: **kama has no implicit numeric conversion**, so `int8 a = big;` is an
error and wants `cast<int8>(big)`. That covers every crossing, not just narrowing — widening
(`int64 a = someInt32`), a signedness flip and int/float are all conversions and all want a `cast`.

The same rule applies **between an operator's two operands**, comparisons included: `int32 + uint8` does
not compile, and neither does `i < n` with an `int32` counter against an `isize` length — the commonest
shape to get wrong. **Declare the counter `isize` and no cast is needed** (`isize i = 0; while (i < xs.length())`);
reach for `cast<int32>(…)` only where the value genuinely leaves the size domain, such as `main`'s exit
code. A **shift** is the exception: its right operand is a count, so `x << someInt32` on an `int64` is fine.

**`isize` is the size type.** Every `length()`/`count()`, every index and every `operator[]` is an `isize`
(`ptrdiff_t`) — `string`, `View`, `Fixed` and every collection alike. `usize` (`size_t`) is reserved for
crossing into C: `sizeof`, an allocation size, an `extern fn` mirroring a `size_t`. They are the only two
platform-varying types, which is why a crossing to a fixed width (`cast<int32>(xs.length())`) is a real
narrowing on a 64-bit host. Bare `int`, `uint`, `double` and `float` are **not** kama types — write
`int32`/`isize` and `float64`/`float32`.

A **literal** is typed by its destination, so it is not a conversion and needs no cast: `int8 a = 100;`,
`float32 f = 3;` and `int8 a = 2 + 3;` are all fine, while a constant that does not fit its destination
(`int8 a = 300;`, `cast<int8>(300)`) is an error rather than 44. <!-- xfail: lit_oob_assign, cast_const_oob --> A **named** constant is not a literal —
`comptime int32 N = 5;` states a type, so `int8 x = N;` wants a cast like any other value.

**A `cast` that does not fit TRAPS at runtime** — `cast<T>` preserves the *value*, so `cast<int8>(big)`
on a 300 aborts rather than binding 44, in every build. Two escapes, and reaching for the right one is the
whole skill here:

- **`truncate<T>(x)`** keeps the low bits — the wrapping conversion (a checksum, a byte written to a wire,
  a deliberate mod-2ⁿ). Target must be narrower or equal.
- **`try cast<T>(x)`** yields `Optional<T>`, `None` where the plain cast would trap. Use it for any value
  that came from **outside the program** — a parsed document, a file, a network read — where a bad value
  is bad input, not a bug. Like `try new` it needs a declared destination:
  `Optional<int8> v = try cast<int8>(n);`.

Do NOT reach for these to silence a trap you did not expect: the trap means the value did not fit, and
the fix is usually a wider destination.

## Rules an LLM trained on C#, Rust, TypeScript or Go will get wrong

These are the ones that actually cost time. kama is deliberately stricter; the strictness is the
feature.

- **Every call uses named arguments.** `add(a: 1, b: 2)`, never `add(1, 2)`. There are no positional
  calls, which is why kama needs no function overloading.
- **Bind intermediates to a local.** An interpolation hole takes an identifier with member/index
  accessors and nothing else, so `"${a.length()}"` is a lexical error — bind the call first. And a
  GENERIC function infers from *named locals*, not from a nested call: with `fn R showIt<T>(T x)`,
  `showIt(x: Leaf.make(n: 7))` cannot infer `T`, while a `Leaf l = Leaf.make(n: 7);` one line up
  makes it work. (A non-generic call nests freely — this is about inference, not about nesting.)
- **`match`, never `switch`.** `switch` does not exist. `match` is exhaustive and produces a value.
- **No `null`, no exceptions.** Absence is `Optional<T>`, failure is `Result<T, E>`; `== null` on a
  safe type is a compile error. <!-- xfail: null_safe_compare --> A **constructor may fail** — the return type goes between `ctor` and
  the name: `public ctor Result<Buffer, SizeError> create(int32 size)`, and an infallible ctor returns
  the bare type. Do NOT write a `static fn` returning its own type; that is rejected as a disguised <!-- xfail: self_returning_static_fn -->
  constructor. `null` exists only for `UnsafePtr<T>` / `UnsafeConstPtr<T>` at the FFI boundary.
- **A `type resource`'s fields are always private.** Expose behavior, not state. (A `type value`
  owns nothing, so its fields may be public.)
- **`.` constructs, `::` resolves scope.** `Box.make(v: 10)` builds; `Plain::tag()` is a static.
  On a generic static the turbofish is mandatory: `Box::<int32>::tag()`.
- **A `string` is UTF-8 bytes.** `length()` counts bytes and `s[i]` is a `uint8`. Iterate bytes with
  `foreach (uint8 b in s)` and codepoints with `foreach (char c in s.chars())` — a `foreach` binding
  must have the type the collection actually yields, so `foreach (char c in s)` is rejected. <!-- xfail: foreach_char_over_string --> Casing
  and whitespace are ASCII-only by design.
- **`@generate` requires every field to be marked** `@field` or `@skip`. An unmarked field is an
  error, so adding one can never silently start serializing it. A `@field` carries a wire NAME and an ID
  (`@field(name: "wire")`, `@field(id: 3)`) — the name defaults to the property name, the id to the
  declaration index — and the backend keeps whichever it addresses by, so one marked type works with every
  backend. `@deprecated` on a field means read when present, never written.
- **Integer overflow traps** in debug rather than wrapping; `std::num`'s `wrapping*` are the opt-in.
- **A module is a FOLDER, and a file says nothing about which one it is in.** There is no `namespace`
  declaration — a file's module is its directory under the source root, and `kama.json`'s `modules` map
  is what gives that directory a name. A folder with no entry there is not a module: its files belong to
  the nearest listed folder above them, so nothing joins your API by accident. Import a module by its
  full name: `import { <project>::<module>::Thing };`. If that fails with `cannot resolve module`, the
  usual cause is a folder nobody listed, not a missing dependency.
- **One `import { … };` block and one `export { … };` block per file, both at the top.** A second
  `import` is a parse error, not a second directive. Every import entry names a SYMBOL —
  `import { std::collections::Map, other::Thing as T, Sibling };` — so there is no whole-module import
  and no glob. `as` renames.
- **VISIBILITY IS PER FILE, and it is symmetric.** `export { A, B };` is what lets a name LEAVE its file;
  an `import` entry is what lets one ENTER. That holds for a sibling in your own module too — it is the
  scope-less entry (`Sibling` above), which needs no path because your own module is the only candidate.
  A name you neither declare nor import is not in scope, even if the file next door is in the same folder.
- **`export { A, B };` is its own declaration**, near the top of the file — not a modifier you put in
  front of `type`. Without it the type is invisible outside this file even though it compiles.
- **Every C keyword is reserved** — `out`, `short`, `long`, `signed`, `register`, … cannot name a
  binding, because kama lowers to C. The message names the word; pick another.
- **A `ref`-returning method call cannot feed a `ref` parameter** — a call result is a temporary and
  its mutation would be lost. A **`const ref` parameter takes one fine**, so a reader's signature is the
  usual fix (`hash(of: const ref …)` fed by `pair.secretKey()`); only a MUTATING callee needs the owner
  passed by `ref` instead, or a value copied out to a local first.
- **A read-only buffer reaches C as `UnsafeConstPtr<T>`** — the read-only raw pointer, C's `T const*`.
  `dataPtr()` is `const fn` and returns one (`dataPtrMut()` is the writable half), `cstr()` is
  read-only, and `addr(of:)` on a const root hands one back. So an FFI wrapper that only reads takes
  `const ref` all the way down, and const-correctness survives the crossing.
- **Member visibility is per TYPE.** A non-`public` ctor or method is invisible even to a free function
  in the same file. Do **not** reach for `public` to fix that — a `friend` grant names the exact
  members one accessor may touch (`friend fill[blank, bytes];`), which is what keeps a raw accessor out
  of the API. (Free functions and types are per FILE — a different rule.)
- **Unwrapping a `Result`/`Optional` into a local:** a *value* payload copies out of a borrowing
  `match (r) { case Ok(value: x): x; … }`; a *resource* payload leaves a consuming
  `match (give r) { case Ok(value: x): give x; … }`. The subject must be a local — bind a call's result
  first — and a block arm that does not produce a value must `return`.
- **`print(s: …)` / `println(s: …)`** — named like every other call.
- **One way to do a thing.** Before adding a helper, `--search` for an existing one.

## Conventions

Types are `PascalCase`, methods and functions `lowerCamel`, and contracts take no `I` prefix. The
`string` primitive is lowercase, like `int32`. Follow the surrounding file over any of this.

## Where the truth is

In descending order of authority. Prefer running the compiler over reading any of them.

- `kama query` and `kama build` — what the compiler resolved. Always current, by construction.
- The language reference and full spec: <https://kama-lang.org>
- `kama.json` — this project's manifest: dependencies, build flags, targets, toolchain pin.
- `AGENTS.package.md`, when present — the package half for a library that will be published: the
  manifest floor, `tests/` as one program, vendoring a C library, the C seam, publishing.
- `<https://kama-lang.org/llms.txt>` — the machine-readable index of all of the above.

If a doc and the compiler disagree, the compiler is right and the doc is a bug worth reporting.
