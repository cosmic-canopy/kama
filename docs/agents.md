# kama for AI agents

kama ships the thing only kama can provide: **its own facts, verified**. `kama query` answers from
what the compiler resolved, so an agent gets a checked answer instead of a plausible one — and it
needs no editor, no language-server handshake and no second process.

This page is the reference. The short version an agent actually reads is `AGENTS.md`, which
`kama agents install` writes into your project.

## Install it into a project

A new project can have it from the start — `kama seed --agents` (or `--claude` / `--all-tools` /
`--skill`) runs exactly the command below once the manifest is written. For an existing project:

```sh
kama agents install            # write AGENTS.md — read natively by most agent tools
kama agents install --claude   # ...and a CLAUDE.md that imports it
kama agents install --all-tools  # ...and a pointer file for every tool that reads neither
kama agents list               # which tools are covered, and what each one gets
kama agents print              # the content on stdout, to paste or pipe anywhere
```

The content lives in **`AGENTS.md`, once**. Every other file is a pointer, never a copy, so there is
one thing to edit and nothing to keep in sync. `AGENTS.md` is an
[open cross-tool standard](https://agents.md) read natively by 25+ agents.

Nothing is overwritten without `--force`. If you already have an `AGENTS.md`, use
`kama agents print` and merge by hand.

## `kama query` — the verified-facts interface

```
kama query [<kama.json>|<kama_workspace.json>] <file> <mode>... [--json]
```

**Coordinates are 1-based LINE and 0-based COLUMN.** The two halves differ, so do not assume. On
line 6 of a file where `Point` begins at the 12th character, the column is `11`.

**Ask everything you want about a file in one invocation.** Modes are repeatable and freely
combinable, answered in the order given, from a single analysis — see [Cost](#cost) for why that
matters far more than it looks.

**The SCOPE is an operand, not a flag.** A leading `kama.json` widens the scope from the file's import
closure to every `.kama` that project claims, and switches paths to absolute; `kama_workspace.json`
widens it to every project in the workspace, which no flag could express. Without one, only the named
file and its closure are in scope.

### `--search NAME` — find a symbol by name

The one mode that takes a **name** rather than a cursor, which usually makes it the way in. Matching
is case-insensitive on a substring.

```console
$ kama query kama.json src/app.kama --search Widget
/abs/src/widget.kama:12:11 value Widget
/abs/src/widget.kama:15:16 ctor Widget.of
/abs/src/widget.kama:22:10 function defaultWidget
```

An empty name lists every symbol in scope. A miss prints `no symbols`. Results never include `std`
or a dependency — only code the package owns.

### `--symbols` — one file's outline

```console
$ kama query tests/query/shapes.kama --symbols
6:11 value Point
7:17 field x
8:34 ctor Point.at
17:9 function midpoint
```

### `--complete L:C` — candidates, with real signatures

The highest-value mode for an agent, because the `detail` column is the **actual signature
including parameter names** — the thing a model most often invents. A `key=value` header describes
the cursor's lexical context, then one tab-separated `kind⇥label⇥detail` row per candidate.

```console
$ kama query tests/query/shapes.kama --complete 24:5
trigger=bare recv= callee= prefix=in active=-1 filled=
function	args	fn Args args()
function	env	fn Optional<string> env(name: string)
function	envOr	fn string envOr(name: string, dflt: string)
```

### `--type` / `--def` / `--refs` / `--sighelp`

```console
$ kama query tests/query/shapes.kama --type 6:11
value Point

$ kama query tests/query/shapes.kama --refs 6:11
tests/query/shapes.kama:6:11
tests/query/shapes.kama:12:4
tests/query/shapes.kama:13:21

$ kama query tests/query/shapes.kama --sighelp 18:26
sig=Point.at(x: int32, y: int32) -> Point active=1
```

### `--diagnostics` — structured, on stdout

The same list `kama check` prints, but on stdout and **without a pass/fail exit** — `query` reports,
`check` judges. It sees exactly what analysis sees, which is not everything; read the next section.

### `--coverage` — what the index knows about every identifier

One line per identifier the *source* spells, in source order, so a name the index never learned
shows up as `-` rather than waiting to be noticed.

```console
$ kama query tests/query/shapes.kama --coverage
4:10 shapes unresolved
6:5 value -
6:11 Point decl:value
```

### `--json`

One envelope for every mode, so a caller can dispatch on `mode` and read `results` without a
per-mode parser. `schema` is a version you can pin.

```console
$ kama query kama.json src/app.kama --search Widget --json
{"schema":1,"mode":"search","file":"/abs/src/app.kama","query":"Widget","results":[
  {"line":12,"column":11,"uri":"/abs/src/widget.kama","kind":"value","name":"Widget"}]}
```

A miss is `"results":[]` — never one of the text form's magic strings (`no definition`, `no type`,
`no references`, `no signature`, `no symbols`, `no diagnostics`). `check --json` adds `ok` and keeps
its exit code, so a wrapper testing `$?` keeps working.

The text forms are deliberately left as they are: four shapes, each suited to its own question when
a human greps it. `--json` is the one format for machines; there is no plan to harmonize the text.

### Many questions, one analysis

Every mode above is repeatable and combinable. Ask them together and they are answered from **one**
analysis, in argv order — each answer preceded by a `## <question>` line:

```console
$ kama query src/app.kama --def 24:9 --type 24:9 --refs 24:9
## --def 24:9
src/widget.kama:12:11
## --type 24:9
value Widget
## --refs 24:9
src/app.kama:24:9
src/widget.kama:12:11
```

Under `--json` the batch is the same envelope one level up, and each record carries an `ask` echo so
you can pair answers to questions without relying on order:

```console
$ kama query src/app.kama --def 24:9 --symbols --json
{"schema":1,"mode":"batch","file":"src/app.kama","results":[
  {"schema":1,"mode":"def","file":"src/app.kama","ask":"--def 24:9","results":[…]},
  {"schema":1,"mode":"symbols","file":"src/app.kama","ask":"--symbols","results":[…]}]}
```

**A single question is unchanged** — no `## ` line, no `ask`, the flat per-mode envelope — so
existing scripts keep working. A malformed `L:C` anywhere in the list is rejected before *any* answer
is printed (exit 2, empty stdout): a partial batch that exits nonzero is worse than no batch.

## `kama check` type-checks by KIND and by WIDTH

This is the sharpest edge in the toolchain for an agent, so it is stated plainly:

| | catches |
|---|---|
| `kama check` | name resolution, unknown functions/methods/types, named-argument mismatches, ownership/move analysis, serde marks, **and a type mismatch by kind** |
| `kama build` | all of the above, plus whatever the C compiler still catches |

A **kind** is one of four families: a number, a `bool`, a `string`, or a type value. Crossing between
two of them is rejected by `kama check`, in kama's own words, against your `.kama` line:

```
error: a local is declared `int32`, so it cannot be initialized with a `string` — a number was expected
```

It reaches **every place a value crosses into a destination of a stated type** — an initializer, an
assignment, a `return`, a `match` arm, a call argument, an enum payload — and the value can be any
expression: a call result, an element, a cast, a comparison, a ternary. Where kama is not certain of a
type it says nothing rather than guessing, so a green `check` is not a proof the program has no type
error; it is a proof that none of the ones kama can see are there.

**Width is checked at those same places**, because **kama has no implicit numeric conversion** — the
Rust/Swift/Go rule. `int8 a = big;` is an error and wants `cast<int8>(big)`:

```
error: a local is declared `int8`, so it cannot be initialized with a `int32` — kama has no implicit
numeric conversion. Convert it explicitly: `cast<int8>(…)`
```

It is not only narrowing. **Every** crossing is a conversion and every one wants a `cast`: widening
(`int64 a = someInt32`), a signedness flip (`uint8 a = someInt8`), int/float in both directions, and
anything involving `usize`/`isize`. If two types differ, the conversion is written down.

**`isize` is the size type** — every `length()`/`count()`/index is one, on `string`/`View`/`Fixed` and every
collection. `usize` is for the C ABI only (`sizeof`, allocation, `extern fn`). Declare loop counters `isize`
and the casts disappear. Bare `int`/`uint`/`double`/`float` are not kama types.

**And the cast itself is checked at runtime.** `cast<T>` preserves the *value*, so one that does not fit
`T` **traps** — in every build, the way an out-of-range `float → int` already did. The escapes say which
meaning was intended: **`truncate<T>(x)`** keeps the low bits (wrapping, for a checksum or a wire byte),
and **`try cast<T>(x)`** yields `Optional<T>` for a value that came from outside the program, where a bad
value is bad input rather than a bug. An agent's failure mode here is reaching for `truncate` to quiet an
unexpected trap; the trap usually means the destination is too narrow.

It also applies **between an operator's two operands**, which is where an LLM writing kama is most
likely to trip: `int32 + uint8` does not compile, and neither does `i < n` with an `int32` counter
against a `usize` length — the single commonest shape to get wrong. Write `cast<usize>(i) < n`.
Comparisons are included on purpose: C answers `-1 < 1u32` with *false*, and a rule that covered
assignments but not comparisons would leave the sharpest edge in place.

What is **not** a conversion, and needs no cast:

| | |
|---|---|
| a literal, typed by its destination | `int8 a = 100;` · `float32 f = 3;` · `uint8 b = 255;` |
| arithmetic over literals — still the literal | `int8 a = 2 + 3;` |
| arithmetic on one type, which yields that type | `uint8 c = a + b;` on two `uint8`s |
| a shift, which takes its type from the left operand | `int64 x = y << someInt32;` |

A **named** constant is not a literal: `comptime int32 N = 5;` states a type, so `int8 x = N;` wants a
cast. And a constant that does not fit its destination is rejected for that instead — `int8 a = 300;`
and `cast<int8>(300)` are errors, not 44.

Where kama is not certain of a type it still says nothing, so the same caveat applies as for kinds: a
green `check` proves none of the errors kama can see are there, not that there are none. The places it
is deliberately silent are a type parameter, a const-generic parameter, a `foreach` binding, a `borrow`
alias, an `extern fn` result, and arithmetic mixing two types.

Both halves are pinned by `tools/check-query.sh` **and** `tools/check-agents.sh`, so neither can rot.
That mechanism has now fired twice as designed: those guards once asserted that a KIND mismatch was
missed, and failed the day it started being caught; they then asserted a WIDTH mismatch was missed, and
failed the day *that* started being caught — which is what forced this section to be rewritten rather
than left stale.

## Cost

Every invocation re-parses and re-analyzes the prelude and every imported `std::` module, so there
is a fixed floor per process — roughly 0.05 s for a file with no imports, 0.33 s for one importing
`std::collections` plus `std::fmt` and `std::math`, whether or not a symbol from them is used.

**Answering a question off the built index costs 0.03–1.33 ms against that ~210 ms floor**, so the
cost of a query is essentially the cost of *starting* one. Ask everything about a file in a single
invocation rather than shelling out per identifier: three questions in one process is ~0.23 s, the
same three as separate processes is ~0.68 s, and the gap widens linearly with every question you add.
(The floor itself is tracked in [ROADMAP_DETAIL.md](ROADMAP_DETAIL.md) §9 — the fix is a cached front end.)

## Why not the LSP?

`kama lsp` is a full JSON-RPC 2.0 language server ([editors.md](editors.md)) and the right choice
for an editor. For an agent it is the wrong shape: a stdio handshake, a lifecycle to manage, and a
long-lived process. `kama query` is one process, as many questions as you have, structured output —
and it is answered by the same index the server uses, so the two cannot disagree.
