# kama for AI agents

kama ships the thing only kama can provide: **its own facts, verified**. `kama query` answers from
what the compiler resolved, so an agent gets a checked answer instead of a plausible one — and it
needs no editor, no language-server handshake and no second process.

This page is the reference. The short version an agent actually reads is `AGENTS.md`, which
`kama agents install` writes into your project.

## Install it into a project

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
kama query <file> <mode>... [--project] [--json]
```

**Coordinates are 1-based LINE and 0-based COLUMN.** The two halves differ, so do not assume. On
line 6 of a file where `Point` begins at the 12th character, the column is `11`.

**Ask everything you want about a file in one invocation.** Modes are repeatable and freely
combinable, answered in the order given, from a single analysis — see [Cost](#cost) for why that
matters far more than it looks.

`--project` widens the scope from the file's import closure to every `.kama` the nearest `kama.json`
claims, and switches paths to absolute. Without it, only the named file is in scope.

### `--search NAME` — find a symbol by name

The one mode that takes a **name** rather than a cursor, which usually makes it the way in. Matching
is case-insensitive on a substring.

```console
$ kama query src/app.kama --search Widget --project
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
$ kama query src/app.kama --search Widget --project --json
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

## `kama check` is not a full type check

This is the sharpest edge in the toolchain for an agent, so it is stated plainly:

| | catches |
|---|---|
| `kama check` | name resolution, unknown functions/methods/types, named-argument mismatches, ownership/move analysis, serde marks |
| `kama build` | all of the above, **plus type errors** |

`int32 x = "oops";` makes `kama check` print `OK` and exit 0. Expression type checking is delegated
to the C compiler, which `kama build` invokes; the error is reported against the `.kama` file and
line, because the emitted C carries `#line`. **So verify with `kama build`.**

This is a known gap, tracked in [ROADMAP.md](ROADMAP.md) §2, and `tools/check-query.sh` asserts the
caveat still holds — so the day the front end gains real type checking, the guard fails and forces
this page to be corrected rather than letting it rot.

## Cost

Every invocation re-parses and re-analyzes the prelude and every imported `std::` module, so there
is a fixed floor per process — roughly 0.05 s for a file with no imports, 0.33 s for one importing
`std::collections` plus `std::fmt` and `std::math`, whether or not a symbol from them is used.

**Answering a question off the built index costs 0.03–1.33 ms against that ~210 ms floor**, so the
cost of a query is essentially the cost of *starting* one. Ask everything about a file in a single
invocation rather than shelling out per identifier: three questions in one process is ~0.23 s, the
same three as separate processes is ~0.68 s, and the gap widens linearly with every question you add.
(The floor itself is tracked in [ROADMAP.md](ROADMAP.md) §9 — the fix is a cached front end.)

## Why not the LSP?

`kama lsp` is a full JSON-RPC 2.0 language server ([editors.md](editors.md)) and the right choice
for an editor. For an agent it is the wrong shape: a stdio handshake, a lifecycle to manage, and a
long-lived process. `kama query` is one process, as many questions as you have, structured output —
and it is answered by the same index the server uses, so the two cannot disagree.
