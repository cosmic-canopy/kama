---
name: kama
description: Ask the kama compiler for verified facts about a kama codebase — find a symbol by name, read a real signature, check what a name resolves to, and verify a change actually compiles. Use when working on .kama files, when you need a function's exact signature or parameter names, when a kama build fails, or when you are about to guess at kama syntax.
license: MIT
---

# Working with kama, using the compiler as the source of truth

`AGENTS.md` covers the rules. This is the cookbook: what to actually run, in what order, when you
have a concrete question about a kama codebase.

The premise is that **you should not guess about this codebase.** `kama query` answers from what the
compiler resolved — the same index the language server uses — so an answer is checked rather than
plausible. It costs one process and no editor.

## The one-line orientation

```sh
kama query <any-file-in-the-package> --search "" --project
```

An empty needle lists every symbol the package declares, with kind, name and location. This is the
fastest way to learn an unfamiliar kama project, and it is the whole-package outline that
`--symbols` (one file) cannot give you.

## Recipes

### "What is this function's signature?"

Never reconstruct one from call sites — kama has mandatory named parameters, so a wrong parameter
name is a compile error, and call sites do not show defaults or return types.

```sh
kama query src/app.kama --search encodeTo --project   # 1. locate it -> path:LINE:COL
kama query src/util.kama --type 42:9                  # 2. what it is
kama query src/util.kama --sighelp 60:24              # 3. the signature, at a call site
```

`--complete` is often faster than all three: its `detail` column is the full signature with
parameter names, for every candidate in scope at once.

```console
$ kama query src/app.kama --complete 24:5
trigger=bare recv= callee= prefix=in active=-1 filled=
function	envOr	fn string envOr(name: string, dflt: string)
```

### "What methods does this value have?"

Put the cursor just after a `.` on the receiver and ask for completions. `trigger=dot recv=<Type>`
in the header confirms the compiler agreed about the receiver — if `recv` is empty, it did not
resolve the receiver and the list below is not what you think it is.

### "Where is this used, and is it safe to change?"

```sh
kama query src/util.kama --refs 42:9 --project
```

`--project` is the difference between "uses in this file" and "uses in the package". Without it you
will confidently miss the callers that matter.

### "Is my change correct?"

```sh
kama build src/app.kama      # the type check. Do this.
kama run                     # build the manifest entry and run it
```

`kama check` is faster and weaker: it resolves names and matches named arguments, but an expression
type error such as `int32 x = "oops";` passes it. A green `check` means "names resolve", not "this
compiles". When a build fails, `kama query <file> --diagnostics` gives you the analysis errors
structured; type errors come from the C compiler and are reported against the `.kama` file and line.

### "Did the compiler even see my symbol?"

```sh
kama query src/app.kama --coverage
```

One line per identifier in the source, in order, each tagged `decl:<kind>`, `ref:<kind>`,
`unresolved`, or `-`. An unexpected `unresolved` is usually a missing import.

## Scripting it

Add `--json` to any mode for one envelope — `{"schema":1,"mode":…,"file":…,"results":[…]}` — with
`results` always an array and `[]` for a miss, so you need no per-mode parser.

```sh
kama query src/app.kama --search Widget --project --json | jq -r '.results[] | "\(.uri):\(.line) \(.kind) \(.name)"'
```

## Two things that will bite you

**Coordinates are 1-based LINE and 0-based COLUMN.** Not both 1-based, not both 0-based. If a query
returns `no type` where you expected a hit, try one column left before concluding anything.

**Each invocation re-analyzes the prelude and every imported `std::` module** — a fixed ~0.05–0.35 s
per process depending on imports, whether or not you use a symbol from them. Answering a question off
that analysis costs well under a millisecond, so the analysis *is* the cost of a query.

So ask several questions in one go rather than shelling out per identifier in a loop — modes are
repeatable and combinable, answered in argv order from one analysis:

```sh
kama query src/app.kama --def 24:9 --type 24:9 --refs 24:9   # ~0.23s; as three processes, ~0.68s
```

Each answer is preceded by a `## <question>` line; under `--json` you get one `"mode":"batch"`
envelope whose records each carry an `ask` echo. One question on its own is unchanged.

## When to stop using this

For a bulk textual rename, or for anything in a non-kama file, ordinary search is the right tool.
`kama query` earns its cost when the question is *semantic* — what does this name resolve to, what
is the real signature, who actually calls this — and that is where a grep quietly gets it wrong.
