# LSP M5 — error recovery + incremental/perf (cold-start brief)

**Status: NOT STARTED — the ACTIVE next milestone** now that M4 has shipped (2026-07-28, `fae5f41`…`e5c9bef`).
Read [lsp.md](lsp.md) first for campaign context, then this. Everything below was **measured or verified
against the code on 2026-07-28, on the shipped M4 server** — the M4 brief was wrong in three load-bearing
places because it asserted rather than checked, so every claim here carries its evidence and every number
is reproducible.

M5 is what makes the server feel good rather than merely work. It has two halves that share nothing but a
milestone number, and they can ship in either order:

- **Error recovery** — today one syntax error blanks the whole file.
- **Incremental/perf** — today every keystroke re-analyzes the entire import closure.

Sized `L` in [lsp.md](lsp.md):210, or `XL` if it escalates to a hand-written recursive-descent parser
(campaign decision 2, [lsp.md](lsp.md):174-181). **The measurements below argue against escalating.**

---

## Part 1 — Error recovery

### What it looks like today (measured)

A file with three independent syntax errors — a missing `;` on line 2, a typo'd keyword on line 4, a
missing `;` on line 5:

```
kama check      ->  1 error, then stops
the editor      ->  1 diagnostic, and NO semantic diagnostics at all
```

The parse aborts at the first error, so no `CompilationUnit` is produced, so the semantic layer never
runs. The user fixes one error, sees the next, fixes it, sees the next. On a file being actively typed,
the squiggle is usually just "wherever I am right now".

### What the code actually has

| fact | where |
|---|---|
| **Zero `error` productions, zero `yyerrok`, zero `%destructor`** | `kama.y` — grep returns 0 |
| `%expect 1`, `%locations`, `%define api.pure full`, `%define parse.error detailed`, `%define parse.lac full` | [kama.y:141-150](../kama.y) |
| `yyerror` takes `YYLTYPE*` and forwards to `handleError` | [kama.y:1700-1707](../kama.y) |
| `handleError` increments a counter, prints to stderr, AND appends a `Diagnostic` | [kama.context.h:51-62](../kama.context.h) |
| **A 10-error budget exists and is DEAD CODE** — `isErrorLimitReached()` has zero callers repo-wide | `CodeGenContext(…, int maxErrorCount = 10)`, [kama.context.h:34,47](../kama.context.h) |
| `yyerror` keeps only `first_line`/`first_column`, so every parse diagnostic is a **zero-width point**, never a range | [kama.y:1704-1706](../kama.y) |
| The LEXER has a separate error path that does NOT abort — `section = "Lexical"` — so multiple *lexical* errors CAN be reported | [kama.l:566-574](../kama.l), catch-all at :349 |
| `parseFile` / `parseString` destroy the `CodeGenContext` on return, so an **imported module's** parse errors reach stderr but never the editor; only `parseForQuery` keeps them | [kama.driver.cpp:596,634,651](../kama.driver.cpp) |

The 10-error row is the shape of the whole problem: the machinery for reporting *many* errors is already
there and has never once been reached, because nothing recovers to find a second one. `parse.lac full`
improves the error *message*; it does not recover. Note also that "one diagnostic" is specifically one
**parse** diagnostic — the lexer's own error path keeps scanning, so lexical errors already accumulate.

### ⚠️ A latent hard input limit, found while checking recovery readiness

**`YYSTYPE` is a plain `struct kamayystype` of 40 `shared_ptr` fields** ([kama.y:89-133](../kama.y)) —
**not a `%union`**. Chasing what that means for discarding symbols during recovery turned up something
more important, and it is not what the macro definitions suggest.

The generated parser's `YYCOPY` *is* `__builtin_memcpy` under Clang/GCC — but **that code is unreachable**.
Stack relocation is guarded by `YYSTYPE_IS_TRIVIAL`, which Bison only defines for a generated `%union`:

```c
/* build/<os>-<arch>/kama.parser.cpp:743-746 */
#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL \
             && defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))
```

Verified by preprocessing (`c++ -E … | grep -c yyvs_alloc` → **0**). Two consequences, and they point in
opposite directions:

**Good:** the classic Bison-plus-C++ hazard does not exist here. The value stack is a real
`YYSTYPE yyvsa[YYINITDEPTH]` whose 200 elements are default-constructed and destructed normally, and
`*++yyvsp = yylval` is a proper `shared_ptr` copy-assign. **Adding `error` productions is memory-safe with
no `%destructor`.** Discarded subtrees merely stay alive until `yyparse` returns; `%destructor` is a
promptness optimization, not a correctness requirement.

**Bad, and it is a real bug independent of M5:** because relocation is compiled out, the parse stack
**cannot grow**. [kama.parser.cpp:2935](../kama.y) reduces to `if (yyss + yystacksize - 1 <= yyssp)
YYNOMEM;`, so depth is hard-capped at `YYINITDEPTH 200` and `YYMAXDEPTH 10000` is irrelevant. Measured on
the shipped compiler:

| input | result |
|---|---|
| 120 nested parens `((((…1…))))` | OK |
| 200 nested parens | `Parse error: memory exhausted` |
| **30 nested `if` blocks** | OK |
| **40 nested `if` blocks** | **`Parse error: memory exhausted`** |

Nested parens are pathological; **40 nested `if`s is not** — generated code, decision tables and deeply
nested logic reach it, and the diagnostic ("memory exhausted") tells the user nothing true. This is a
one-line fix (`#define YYINITDEPTH` higher in the prologue, or provide `yyoverflow`), and it is worth
doing in M5.2 while the grammar is already open. Note the cost of the current constant: 200 × ~648 B
≈ **127 KB of C++ stack plus 8,000 `shared_ptr` ctor/dtor pairs per `yyparse` call**, which is also a
per-keystroke latency item — so raise it deliberately rather than to a huge number.

### Where to put the recovery points

The useful granularity is the *declaration*: one bad function body should not hide the next function.
Candidate productions, in value order — each is one `| error <sync-token>` arm:

1. `code_declaration` / the top-level declaration list — resync at the next top-level keyword. Recovers
   "the rest of the file" and is where most of the payoff is.
2. `class_member_declaration` — one bad method should not hide its siblings.
3. `block_statement` — resync at `;` or `}`. The finest useful grain, and the one most likely to
   introduce conflicts. Add it last and watch `%expect`.

⚠️ **`%expect 1` is exact, and the baseline is known.** Verified 2026-07-28 with
`bison -Wcounterexamples -v kama.y`: exactly **one** conflict, `shift/reduce on token ELSE` — the classic
dangling-`else`, which is what `%expect 1` accounts for. So the grammar is otherwise conflict-free, and any
NEW conflict an `error` production introduces will be visible immediately rather than buried. Re-run that
command after each recovery arm and read `kama.output`; adjust `%expect` deliberately, never reflexively.

### The recovery-quality question, answered by measurement

[lsp.md](lsp.md):174-181 frames Bison-vs-RDP as the campaign's biggest size swing, leaning "keep Bison for
v1, escalate only if latency or recovery quality demands it". **Latency does not demand it** (Part 2
shows the cost is re-parsing imports, not parsing). Recovery quality might, but a hand-written RDP is a
1,708-line rewrite plus all 483 hand-written productions and their actions (981 LALR states), and M4 shipped a completion experience that is already good
on unparseable buffers via the M4.6 repair. **Recommendation: do (1) and (2) above, measure how many
files recover usefully, and only then revisit.** The RDP remains the right long-term move for
self-hosting, which is a different justification on a different schedule.

---

## Part 2 — Incremental / perf

### The budget, and how far over it we are

[lsp.md](lsp.md):253 set the target: *"whole-file reparse+recheck per keystroke must stay sub-100 ms;
likely fine given file sizes, but measure early."* Measured now, on the shipped M4 server:

| file | lines | units in closure | didOpen | per `didChange` |
|---|---|---|---|---|
| `tests/query/shapes.kama` | 30 | 1 | 49 ms | **40 ms** |
| `lib/std/collections/dynamic_array.kama` | 287 | 1 | 65 ms | **57 ms** |
| `lib/std/collections/sorted_map.kama` | 594 | 1 | 93 ms | **95 ms** |
| `tests/query/complete.kama` | 173 | 14 | 183 ms | **172 ms** |
| `lib/std/process/process.kama` | 363 | 17 | 241 ms | **237 ms** |

Cost ≈ *own file* + *the entire import closure*, at roughly **13 ms per additional unit**. File size alone
stays inside budget even at 594 lines. **Any file that imports a std module is 2-2.5x over it** — and
13-16 of those units are imports that cannot possibly have changed while you type.

### The single worst number, and it is M4's fault

| | |
|---|---|
| server startup | 8 ms |
| `didOpen`, parseable buffer | 240 ms (one analyze) |
| `didOpen`, **unparseable** buffer | 4 ms (parse fails early, no analysis) |
| completion, parseable buffer | **1 ms** |
| completion, **unparseable** buffer | **235 ms** |

The 1 ms is the M4 architecture paying off: once an index exists, a query is a lookup. The 235 ms is
M4.6's repair (blank the cursor's line, re-analyze) running **on every completion request**, because a
buffer being typed into does not parse — which is the common case, not the edge case. M4 shipped it
knowing the cost and noting `didChange` already pays one analysis per keystroke; M5 is where it stops
being acceptable.

### The fix, and its prerequisite is now VERIFIED

A **parse cache keyed by `path -> (mtime, unit)`** removes ~93% of the per-keystroke cost, because the
import closure is re-read and re-parsed from disk on every `lspAnalyze` and never changes during an edit.

M3.5 recorded this idea with an *unverified prerequisite*: whether ASTs are written during analysis. If
they are, a cached unit cannot be reused across analyses. **Verified exhaustively on 2026-07-28: they
are not.** Method — enumerate every `->field =` assignment in `kama.cemit.cpp` (41 candidates after
excluding map/iterator writes) and classify each:

- emitted **C text** (`*_out << "…->ptr = NULL"`) — not AST at all;
- **`ClassInfo`** writes (`ci->hasVtable`, `ci->slotImpl`, [kama.cemit.cpp:7401-7449](../kama.cemit.cpp)) —
  symbol table, rebuilt per analysis;
- writes to **freshly synthesized** nodes (built from `_synthCtx`) or **explicit clones**:
  [1533](../kama.cemit.cpp), [4072](../kama.cemit.cpp), [4788](../kama.cemit.cpp),
  [4946](../kama.cemit.cpp), [5040-5070](../kama.cemit.cpp) (`clone`), [5756](../kama.cemit.cpp),
  [11987](../kama.cemit.cpp), [12005](../kama.cemit.cpp), [12053](../kama.cemit.cpp).

The one that looks like an exception is not: `fe->isRef = true` ([kama.cemit.cpp:1782](../kama.cemit.cpp))
targets a `ForEachNode` the emitter itself synthesizes to lower a `parallel_for`. **No parsed AST node is
ever mutated by analysis**, so a parsed unit is reusable. Re-run that classification if the emitter grows
a new write.

`CEmitter` itself is still not re-runnable — `lspAnalyze` makes a fresh one per call and must keep doing
so. The cache reuses **units**, not the emitter.

### Suggested staging

| # | scope |
|---|---|
| **M5.0** | The parse cache: `path -> (mtime, size, unit)` inside the driver, consulted by `loadProgramUnits`. Invalidate on `workspace/didChangeWatchedFiles` as well as mtime. Expect ~240 ms → ~20 ms on `process.kama`. |
| **M5.1** | Stop M4.6 re-analyzing per request: cache the repaired index against `(version, cursor line)`, or reuse the previous repair when the line count and the cursor line are unchanged. |
| **M5.2** | Error recovery: top-level and class-member resync productions (no `%destructor` needed — see above). Raise `YYINITDEPTH` while the grammar is open. Assert on a fixture with 3+ independent errors, and on 40 nested `if`s. |
| **M5.3** | Optional: statement-level resync, if M5.2's measurements justify the conflict risk. |

Measure after each. The numbers above are the baseline; put the new ones next to them.

---

## Carried over from M4 — deliberately deferred, now unblocked

- **Index argument labels** so renaming a parameter rewrites its call sites. M4.9 made the label spans
  correct, which is what turned this from a data-loss risk into an ordinary feature. The seam is
  `emitReorderedCall` ([kama.cemit.cpp:8264](../kama.cemit.cpp)) — the single named-argument matcher for
  every call form — plus `recordRef` with a `field:`-style key naming the callee's parameter.
- **Promote the undeclared-import warning to a diagnostic.** `loadProgramUnits` prints it to stderr,
  warn-once per process, so it lands in the editor's log channel rather than the Problems pane. It should
  be a `publishDiagnostics` entry against the offending `kama.json`. The plumbing it was waiting on now
  exists.

## Non-negotiables carried from M0–M4

- **Run the sanitized-compiler harness pass at the end of the milestone.** `make
  EXTRA_CXXFLAGS="-fsanitize=address,undefined"` then both `tools/check-lsp.sh` and
  `tools/check-query.sh`. The `KAMA_SAN=1` leg of `run_tests.sh` gates those two OFF, so nothing else
  covers the LSP C++. This is how M4 found a pre-existing out-of-bounds read in `lspAnalyzeWorkspace`
  that a plain build had been quietly surviving since the workspace-deps campaign.
- **`tools/cdev make` before `tools/cdev test`**, or the container tests a stale binary and the failures
  are meaningless. M4 also found that macOS libc++ pulls `<functional>` in transitively while the
  container toolchain does not — the cross-platform run is not optional.
- **Regenerate `build/lspref-before.txt` (`tools/lspref.sh`) BEFORE touching anything on the emit path**,
  and diff after. `build/` is gitignored, so it must be regenerated, not assumed present. M5.2 touches
  `kama.y`, which regenerates the parser — emission must stay byte-identical across all 535 fixtures.
- **Never call `cType` from a query path** — its `unsupported()` side effect pollutes `_diagnostics`.
- **Positions are kama line 1-based / column 0-based** everywhere below the protocol layer; `kamaPos` and
  `lspRange` in `kama.lsp.cpp` are the only two conversion points.
