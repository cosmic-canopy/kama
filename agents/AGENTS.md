# Working on a kama project

Instructions for an AI coding agent. `kama` is a C-family language (C#-like syntax, no garbage
collector, RAII, mandatory named parameters) that compiles to portable C.

Every tool that reads `AGENTS.md` picks this up automatically. For the ones that don't, run
`kama agents list`.

## Ask the compiler instead of guessing

`kama query` answers from what the compiler actually **resolved**, not from what a grep matched.
Prefer it to searching the tree — it is one process, one file, no editor and no language server.

```sh
kama query <file> --search Widget --project   # find a symbol BY NAME, across the package
kama query <file> --symbols                   # outline of one file
kama query <file> --complete L:C              # candidates + FULL signatures with parameter names
kama query <file> --type L:C                  # what is this, exactly
kama query <file> --def L:C                   # where is it declared
kama query <file> --refs L:C --project        # every use, across the package
kama query <file> --diagnostics               # analysis diagnostics, structured
```

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

`kama check` runs name resolution, named-argument matching, and ownership/move analysis. It is
**not** a full type check: an expression type mismatch such as `int32 x = "oops";` is caught by the
C compiler during `kama build`, so `check` reports OK. Treat a green `check` as "names resolve",
never as "this compiles".

## Rules an LLM trained on C#, Rust, TypeScript or Go will get wrong

These are the ones that actually cost time. kama is deliberately stricter; the strictness is the
feature.

- **Every call uses named arguments.** `add(a: 1, b: 2)`, never `add(1, 2)`. There are no positional
  calls, which is why kama needs no function overloading.
- **Bind intermediates to a local.** Inference reads *named locals*, not arbitrary nested
  expressions. `"${a.length()}"` is a lexical error and `showIt(x: Leaf.make(n: 7))` cannot infer —
  give the intermediate a name first.
- **`match`, never `switch`.** `switch` does not exist. `match` is exhaustive and produces a value.
- **No `null`, no exceptions.** Absence is `Optional<T>`, failure is `Result<T, E>`; `== null` on a
  safe type is a compile error. Constructors cannot fail — a fallible one is a `static` factory
  returning `Result`. `null` exists only for `Ptr<T>` at the FFI boundary.
- **A `type resource`'s fields are always private.** Expose behavior, not state. (A `type value`
  owns nothing, so its fields may be public.)
- **`.` constructs, `::` resolves scope.** `Box.make(v: 10)` builds; `Plain::tag()` is a static.
  On a generic static the turbofish is mandatory: `Box::<int32>::tag()`.
- **A `string` is UTF-8 bytes.** `length()` counts bytes and `s[i]` is a `uint8`. Iterate bytes with
  `foreach (uint8 b in s)` and codepoints with `foreach (char c in s.chars())` — a `foreach` binding
  must have the type the collection actually yields, so `foreach (char c in s)` is rejected. Casing
  and whitespace are ASCII-only by design.
- **`@generate` requires every field to be marked** `@field` or `@skip`. An unmarked field is an
  error, so adding one can never silently start serializing it.
- **Integer overflow traps** in debug rather than wrapping; `std::num`'s `wrapping*` are the opt-in.
- **A namespace must match the file's path** under the source root: `namespace acme::geo;` lives in
  `<src>/acme/geo.kama` or `<src>/acme/geo/`. Get this wrong and the import fails with
  `cannot resolve module`, which reads like a missing dependency and is not one.
- **`export { A, B };` is its own declaration**, near the top of the file — not a modifier you put in
  front of `type`. Without it a namespaced type is invisible to importers even though it compiles.
- **One way to do a thing.** Before adding a helper, `--search` for an existing one.

## Conventions

Types are `PascalCase`, methods and functions `lowerCamel`, and contracts take no `I` prefix. The
`string` primitive is lowercase, like `int32`. Follow the surrounding file over any of this.

## Where the truth is

In descending order of authority. Prefer running the compiler over reading any of them.

- `kama query` and `kama build` — what the compiler resolved. Always current, by construction.
- The language reference and full spec: <https://kama-lang.org>
- `kama.json` — this project's manifest: dependencies, build flags, targets, toolchain pin.
- `<https://kama-lang.org/llms.txt>` — the machine-readable index of all of the above.

If a doc and the compiler disagree, the compiler is right and the doc is a bug worth reporting.
