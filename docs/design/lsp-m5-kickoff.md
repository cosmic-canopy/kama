# LSP M5 — error recovery + incremental/perf (AS SHIPPED)

**Status: ✅ SHIPPED 2026-07-28** — commits `dfdbb9a`…`6d295e1` on `dev`. This file was the cold-start
brief; it is now the as-shipped record, the same treatment [lsp-m4-kickoff.md](lsp-m4-kickoff.md) got.
Read [lsp.md](lsp.md) first for campaign context. Next is **M6 — editor clients**,
[lsp-m6-kickoff.md](lsp-m6-kickoff.md).

M5 had two halves that shared nothing but a milestone number:

- **Error recovery** — one syntax error blanked the whole file.
- **Incremental/perf** — every keystroke re-analyzed the entire import closure.

Both shipped. Sized `L` in [lsp.md](lsp.md):210, and it stayed `L`: **no escalation to a hand-written
recursive-descent parser was needed**, which the original brief predicted and the measurements confirmed.

---

## What shipped

| # | commit | scope |
|---|---|---|
| M5.0 | `dfdbb9a` | `KAMA_TIMING` phase timing + `tools/lsp-bench.sh` |
| M5.1 | `00f8022` | Parse the embedded prelude once per process |
| M5.2 | `eb086f7` | Reuse parsed units across analyses (the import-closure cache) |
| M5.3 | `955bd0e` | Grammar: `YYINITDEPTH` + error-recovery arms at three grains |
| M5.4 | `7955ee9` | Partial units reach the LSP, with a diagnostic policy |
| M5.5 | `6d295e1` | Retire the M4.6 repair; completion is a lookup again |

### The numbers

Per keystroke, measured with `tools/lsp-bench.sh --lsp <file>` (steady state — i.e. the second and
subsequent analyses in one server process, which is every keystroke after the first):

| file | lines | units | before | after | budget |
|---|---|---|---|---|---|
| `tests/query/shapes.kama` | 29 | 1 | 40 ms | **10.3 ms** | ✅ |
| `lib/std/collections/sorted_map.kama` | 594 | 1 | 95 ms | **37.6 ms** | ✅ |
| `tests/query/complete.kama` | 172 | 14 | 172 ms | **46.3 ms** | ✅ |
| `lib/std/process/process.kama` | 363 | 17 | 237 ms | **86.0 ms** | ✅ |
| completion on an unparseable buffer | | | 229 ms | **a lookup** | ✅ |

The 100 ms budget from [lsp.md](lsp.md):253 is met everywhere, from 2.4x over it on the worst file.

---

## ⚠️ Three places the original brief was wrong

It asserted rather than measured, in exactly the way it warned the M4 brief had. Recorded here because
each one changed the plan, and because the *method* is the lesson: every claim below carries how it was
checked.

### 1. The perf thesis — "a parse cache takes 240 ms to ~20 ms"

Not reachable, and for a reason the brief could not have known without measuring: **it had no idea what
fraction of the cost was parse versus analyze**, because the compiler had zero timing instrumentation.
A parse cache removes only the parse share; every unit is still re-analyzed by a fresh `CEmitter` on
every keystroke.

M5.0 measured it. The split on `process.kama` (17 units) was closure-parse 117 ms / prelude-parse 28 ms
/ analyze 73 ms, so caching both parses reaches ~86 ms and no further. The brief's *direction* was right
and its *magnitude* was not — 86 ms, not 20 ms, and the residual is analysis that no cache in this
design can touch.

The measurement also found something the brief missed entirely: **a 3-line file cost the same as a
29-line one** (44.7 vs 45.7 ms). The fixed floor was the 772-line embedded prelude, re-parsed per
analysis — 28 ms of parse against 10 ms of analyze. That inverted the priority: the cheapest fix in the
milestone (two `static`s, M5.1) was worth more than the brief's headline item on small files.

### 2. "No parsed AST node is ever mutated by analysis" — false

The brief verified this "exhaustively" by enumerating every `->field =` assignment in `kama.cemit.cpp`.
It missed [`CEmitter::pruneInactiveDecls`](../kama.cemit.cpp) (:13971-14002), which rewrites
`unit->codeDeclarationList->swap(kept)` and `*ap = filtered` in place — and which `collectProgram` runs
over **every** unit including the prelude (:14021). The sweep missed it because the mutation is a `swap`
and a `*ap =`, not a `->field =`.

Caching is still sound, but the invariant is now stated rather than assumed: the prune is **idempotent
under a fixed build-flag set** (kept decls have their `@compileFor` stripped, so a second pass drops
nothing), and one process has one flag set for life. That clause is precisely why the closure cache is
**opt-in and `kama lsp`-only** — see `parseFile` in `kama.driver.cpp` for the full statement of it.

**Re-check this if the emitter grows another in-place AST rewrite**, and grep for `swap(` and `* … =`,
not just `->field =`.

### 3. The recovery staging was backwards

The brief ranked top-level and class-member recovery as "where most of the payoff is" and statement-level
as "optional, M5.3, add it last". That is inverted, and shipping it that way would have **regressed**
completion while appearing to improve it:

- A top-level `| error` arm discards the entire enclosing function, so on a buffer like
  `fn int32 main() { P p; p.<cursor> }` the local `p` vanishes from the index.
- Worse, it does so silently. `indexForRequest`'s gate was `countLines(text) == linesAtLastGood`; once
  recovery keeps that fresh every keystroke, the M4.6 repair stops firing. Completion would have got
  ~200x faster and materially worse, with no test failing.

Bison pops to the **nearest** state with an `error` action, so the arms must go in innermost-first:
statement, then class member, then top level as a last resort. Shipped in that order.

It also missed a hard blocker: [kama.driver.cpp:673](../kama.driver.cpp) gated `parseForQuery`'s result
on `errorCount() == 0`, so **no partial AST could reach the LSP at all** until that line changed (M5.4).

---

## Design decisions worth carrying forward

**Diagnostics on a partial parse.** Publish every parse diagnostic — many errors instead of one is the
point — and publish semantic diagnostics too, **except** when the top-level arm fired. This is the
bargain TypeScript (`getSyntacticDiagnostics`/`getSemanticDiagnostics`), clangd (recovery expressions)
and rust-analyzer all strike: publish semantics on a syntactically broken file, and rely on recovery
preserving the declaration *shell* to keep the cascade small. kama's finer arms preserve it; the
top-level arm does not, and losing a whole `type` makes every reference to it read as undeclared.
Measured on a malformed type head with two later uses: the guard is the difference between 1 diagnostic
and 3, and a file with 20 uses would get 20. Carried by `CodeGenContext::droppedTopLevelDecl`.

**The parse cache is keyed by path SPELLING, not absolute path.** A unit is *named* by the string
`parseFile` was handed and `CEmitter::unitForUri` matches names exactly, while one file is reachable by
two spellings (the URI-derived absolute path vs `dir + "/" + name` from module resolution). Serving one
for the other silently rewrites every query's URI, every diagnostic's `file`, and every go-to-definition
Location. Two spellings cost two entries — the status quo. `check-lsp.sh` ids 43/44 assert the *payload*,
not just that a response came back, because a wrong-spelling hit still answers.

**`YYINITDEPTH`, and the fix NOT to make.** `YYSTYPE` is a plain struct rather than a `%union`, so Bison
never defines `YYSTYPE_IS_TRIVIAL`, so its stack-relocation path is compiled out and the parse stack
cannot grow. Depth was capped at 200 and **40 nested `if`s reported "memory exhausted"** — a real limit
for generated code and a diagnostic that tells the user nothing true. Raised to 1200 (clears 200 nested
ifs, verified). Do **not** "fix" this by defining `YYSTYPE_IS_TRIVIAL` or providing `yyoverflow`, which
is the obvious-looking one-liner: both enable the relocation path, whose `YYCOPY` is
`__builtin_memcpy` — over a stack of `shared_ptr`s. Cost is real and was measured: buffer-parse
11.6 → 12.1 ms.

**The 10-error budget was dead code and is now live.** `isErrorLimitReached()` had zero callers
repo-wide because nothing could ever reach it — the parse stopped at error one. Recovery makes it
reachable and it needs to be: a buffer mid-refactor can otherwise emit an error per line, per keystroke,
into `publishDiagnostics`. Past the budget errors are still *counted* (so every `errorCount() > 0` gate
still fails the parse) but no longer reported.

**Retire against a criterion, not a hunch.** M5.5 added a `KAMA_LSP_NO_REPAIR` escape hatch, required
`KAMA_LSP_NO_REPAIR=1 sh tools/check-lsp.sh` to pass the *entire* harness including the two M4.6
assertions that motivated the repair, and only then deleted it. Those assertions stay as the regression
guard.

**Things deleted because nothing read them:** `Doc::version` had been written on every
didOpen/didChange since M1 and never once read. A field maintained but unread is worse than no field —
it reads like state something depends on.

---

## Traps found while building the harness

Both of these make a benchmark silently measure nothing, which is worse than a benchmark that fails:

- **`\\n` inside shell single quotes emits two backslashes**, so the "newlines" in a JSON buffer are
  literal `\n` text and the buffer's line count never moves. `check-lsp.sh` uses a single `\n` for
  exactly this reason.
- **`check-lsp.sh` is one flat shell scope.** A new fixture named `DURI` silently retargeted the
  dependency project's URI 80 lines below; the assertions still "passed" while testing the wrong buffer.

---

## Still open (carried to M6 or ROADMAP)

- **The ~10 ms fixed prelude-ANALYSIS floor.** M5.1 removed the prelude *parse*; every fresh `CEmitter`
  still re-collects and re-analyzes the prelude and the three built-in modules. That is the residual
  fixed cost of an analysis and no cache in this design touches it. Attacking it means a pre-baked
  analyzed prelude or a reusable/forkable `CEmitter` — a much larger change against the emitter's core
  invariants. ROADMAP item, not an M5 escalation. It is comfortably inside budget today.
- **`lspAnalyze`/`lspAnalyzeWorkspace` never call `setBuildFlags`**, unlike `kama check` and
  `kama query`, so `_activeFlags` is empty in the server and the LSP prunes `@compileFor` decls a build
  would keep. Pre-existing, unrelated to M5, and worth a look in M6.
- The two M4 carryovers (argument-label indexing, undeclared-import diagnostic) — see the M6 brief.

## Non-negotiables, confirmed again this milestone

- **The sanitized-compiler harness pass is a MANUAL campaign-exit step and it is not optional.**
  `make EXTRA_CXXFLAGS="-fsanitize=address,undefined"` then both `tools/check-lsp.sh` and
  `tools/check-query.sh`; `run_tests.sh` gates them off under `KAMA_SAN=1`, so nothing else covers the
  LSP C++. Run it **in the container too**: macOS ASan has no LeakSanitizer, and M5 added two
  process-lifetime caches — the container run is what actually classified them (reachable, not leaked).
- **`tools/lspref.sh` before/after on anything touching `kama.driver.cpp` or `kama.y`.** Regenerate the
  baseline first; `build/` is gitignored. M5.3 regenerated the parser and emission stayed byte-identical
  across every pre-existing fixture — that diff is the guard that an `error` arm did not change which
  production reduces on *valid* input.
- **`tools/cdev make` before `tools/cdev test`.** A grammar change is exactly where the two bisons
  diverge.
- Never call `cType` from a query path — its `unsupported()` side effect pollutes `_diagnostics`.
- Positions are kama line 1-based / column 0-based below the protocol layer; `kamaPos` and `lspRange` in
  `kama.lsp.cpp` are the only two conversion points.
