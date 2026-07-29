# LSP M6 — editor clients + the campaign's loose ends (cold-start brief)

**Status: NOT STARTED — the ACTIVE next milestone.** M5 shipped 2026-07-28 (`dfdbb9a`…`6d295e1`), and
the **build-configuration campaign** shipped 2026-07-29 (`fdc9a75`…`fbe69f5`) *between* M5 and this —
sequenced first precisely because item 3c below needed a flag model to point the editor at. Read
[lsp.md](lsp.md) for campaign context and [build-configuration.md](build-configuration.md) for the flag
model, then this. Sized `S/M` in [lsp.md](lsp.md):222 — the client work genuinely is small, because
every editor reads the same server.

> ⚠️ **Line numbers re-verified against `fbe69f5`** (2026-07-29). The build-configuration campaign moved
> `kama.driver.cpp` by ~700 lines, so every driver reference below was re-derived; `kama.cemit.cpp` and
> `kama.query.cpp` were not meaningfully touched and their seams are unchanged. **Re-derive again if you
> land anything before starting** — a stale brief is how this campaign has repeatedly misled the next
> session, and it has now happened twice.

M6 is the last LSP milestone. It has three parts, and only the first is what the milestone is named for:

1. **Editor clients** — thin configs pointing at `kama lsp`, plus `docs/editors.md`.
2. **The syntax-highlighting audit** — already scoped in detail at [lsp.md](lsp.md):262-273. Do not
   re-derive it; that section has the motivating evidence (the VS Code grammar disagreed with the
   compiler four ways on numeric literals alone) and the compiler-as-oracle method.
3. **Three deferred items** carried from M4 and M5, written up below so they are not lost.

---

## Part 2 — the syntax-highlighting audit, already done once

The grammar-vs-lexer comparison has been run (2026-07-29) and found **ten** concrete disagreements
beyond the four numeric ones `139fb10` fixed. Method for each: write the literal, run `kama check`,
compare to the regex — compiler as oracle, not eyeballing. The work list, in
`editor/vscode/syntaxes/kama.tmLanguage.json`:

| # | Defect | What the lexer actually accepts |
|---|---|---|
| 1 | escape rule `\\(u\{…\}\|.)` colours `\e`, `\x41`, `\q` as valid | only `\' \" \\ \0 \a \b \f \n \r \t \v \$` and `\u{1..6 hex}` |
| 2 | verbatim `@"…"` has no `""` sub-rule | `""` is the escaped quote, so `@"say ""hi"""` mis-colours the rest of the line |
| 3 | char rule is a quoted *span*, so `'abc'` and `''` pass | one byte / one simple escape / `'\u{1-6}'` / one 2–4 byte UTF-8 scalar |
| 4 | `using` is highlighted | retired in favour of `import` (`docs/KEYWORDS.md`:38) |
| 5 | `type\s+(value\|resource\|view\|contract)` misses modifiers | `type final resource Box`, `type abstract resource Shape`, `type extern value div_t` are all in-tree |
| 6 | no attribute rule at all | `@generate`, `@compileFor(...)`, `@noheap`, `@skip`, `@builtin`, `@vertex`… — and `compileFor` currently mis-scopes as a *function* because of its trailing `(` |
| 7 | no operator/punctuation rules | `::`, `:=`, `...`, `?`, compound assigns |
| 8 | no turbofish rule | `::<T>`, used in `tests/io_streams.kama`:44 |
| 9 | no rule for the ignored preprocessor line | the lexer silently drops `^[ \t]*#.*` (`#region`) |
| 10 | `asm("…")` has no embedded-asm scope | and would colour `asm("${x}")` valid, which does not parse |

Also a **blind spot in the guard itself**: `tools/check-syntax-drift.sh` extracts keywords with
`grep -oE '\{"[a-z_]+"'`, which excludes every digit-bearing keyword (`int8`…`uint64`, `float32/64`).
Nothing is missing today, but the guard cannot see it if something goes missing tomorrow.

**Semantic tokens are the second layer, and the user has approved both** (2026-07-28): every production
server ships a grammar *and* `textDocument/semanticTokens` — TypeScript, rust-analyzer, clangd, gopls,
ZLS. The grammar colours instantly, offline, and for closed files in peek/diff views; semantic tokens
overlay what a regex cannot compute (defect #5 is unfixable in a regex — only the resolver knows `Box`
is a type). The data is already there: `_positions[unit]` carries `SrcRange` + `declKey`, `_defSites`
carries `SymKind`, and the `local:`/`field:`/`enum:` prefixes disambiguate the rest, so this is a read
off the cached `lastGoodIndex`, not a re-analysis. Ship `/full` only — no range or delta variants — and
**measure it with `tools/lsp-bench.sh --lsp`**, because semantic tokens fire on every edit in most
clients and are the one M6 item that can move the 100 ms budget.

**Tree-sitter is explicitly M7, not this milestone** (user, 2026-07-28). It unlocks Neovim / Helix / Zed
highlighting and GitHub linguist, and a Zed extension cannot exist without it — but it is a third
grammar to keep in sync with `kama.l`/`kama.y`, so it gets its own campaign. `docs/editors.md` must
therefore say **honestly** which editors get colour from what: TextMate for VS Code and Sublime
(Sublime consumes the same `.tmLanguage` file — reuse it, don't rewrite it), semantic tokens for VS
Code / Neovim / Emacs, and Helix + Zed getting LSP features with no syntax colouring until M7.

---

## Part 3 — the deferred items

### 3a. Index argument labels (carried from M4, unblocked by M4.9)

Renaming a parameter should rewrite its call sites. It cannot today, because argument labels are not in
the reference index — and every kama argument is named, so this is not a niche gesture.

- **Seam:** `emitReorderedCall` ([kama.cemit.cpp](../kama.cemit.cpp):8264 — unmoved) — the single named-argument
  matcher for every call form, so one edit covers them all — plus `recordRef` with a `field:`-style key
  naming the callee's parameter.
- **Why it waited:** M4.9 made the label spans correct. Before that this was a data-loss risk, since
  rename *replaces* the range it is handed; a span that ran past the label would have eaten code. It is
  now an ordinary feature.
- **Test the span before the feature.** `prepareRename` on a label must return the label's range and
  nothing more; the M3.4/M4.9 assertions in `tools/check-lsp.sh` are the pattern to copy.

### 3b. Promote the undeclared-import warning to a diagnostic (carried from M4)

`loadProgramUnits` ([kama.driver.cpp](../kama.driver.cpp):537, the check at :633-651) prints "package X imports Y but does
not declare it" to stderr, warn-once per process — so in an editor it lands in the log channel nobody
reads rather than the Problems pane. It should be a `publishDiagnostics` entry against the offending
`kama.json`.

Two things to get right, both already learned the hard way in the workspace-deps campaign (`a99436d`):

- **Never blame a package the user cannot fix** — the gate already clears the owner when it resolves
  under the content-addressed store prefix. Keep that.
- **It is warn-once per process** (`static std::set<std::string> warnedFreeRide`, :557 — unmoved, added precisely
  so `kama lsp` would not repeat it per keystroke). A diagnostic must be *republished* every analysis or
  it vanishes on the next keystroke, so the warn-once set and the diagnostic path need different
  lifetimes.

### 3c. The LSP never calls `setBuildFlags` (found during M5; the DECISION is now made)

`lspAnalyze` ([kama.driver.cpp](../kama.driver.cpp):3878, emitter setup at :3906-3908) and
`lspAnalyzeWorkspace` (:4083) still do not call `setBuildFlags`, while `kama check` (:4757), `kama
query` (:4823) and both build paths (:2190, :2237) do. So `_activeFlags` is empty in the server and
`pruneInactiveDecls` drops every `@compileFor`-gated declaration a real build would keep — the editor
sees a different program from the compiler.

**`kama query` at :4753-4759 is the reference implementation.** It does five calls; the LSP does two.
The divergence *is* the bug:

```cpp
CEmitter idx(input);
idx.setPrelude(preludeUnit());
idx.setNoHeap(g_noHeap);
idx.setRelease(g_release);
idx.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);
idx.setLogDefault(g_logDefault);
for (auto& m : preludeModuleUnits()) idx.addPreludeModule(m);
```

**What the build-configuration campaign settled.** The question used to be "which flag set should an
editor analyze under?" — it now has a definite answer, and the machinery to produce it:

- **The default is what a plain `kama build` in that project does**: the resolved `TARGET`'s derived
  flags (`ARCH_*`/`OS_*`/`ABI_*`/`HOSTED`), `BUILD_TYPE=DEBUG`, `OUTPUT`, plus the manifest's
  `default: true` user flags. `derivedTargetFlags` / `resolveTarget` / `seedBuiltinSelectGroups` /
  `loadManifestTargets` are all in the driver already; the LSP needs to *call* them.
- **The globals it must populate are `g_activeFlags` / `g_declaredFlags` / `g_strictFlags` / `g_target`
  / `g_selectGroups` / `g_release` / `g_logDefault`.** ⚠️ **`kama lsp` returns at :4310-4315, roughly
  450 lines BEFORE the block that populates any of them** — that early return is the whole reason they
  are empty. Either factor the manifest-load + target-resolve + flag-derive sequence (:4480-4700ish)
  into a function both paths call, or resolve inside `runLspServer` before the loop. The former is
  better: two copies of the precedence rules will drift.
- **The override channel is `initializationOptions` + `workspace/didChangeConfiguration`**, carrying
  `{ select: {GROUP: VALUE}, define: [], undefine: [] }` — the shape `--select`/`--define` already
  validate, so reuse that validation rather than writing a second one.

⚠️ **The M5 parse cache assumes a FIXED flag set per process** (`kama.driver.cpp`, the `g_parseCache`
comment): `pruneInactiveDecls` destructively rewrites cached units in place. So on a configuration
change you MUST call `lspEvictParsedFile("")` and re-analyze every open document.
`workspace/didChangeWatchedFiles` ([kama.lsp.cpp](../kama.lsp.cpp):1026) already does exactly that
eviction — reuse it, and the invariant stays true by making the flush the explicit contract.

**Worth doing while here:** the VS Code client should expose the groups as a **status-bar item with a
quick-pick** — one dropdown per single-select group, checkboxes for `flags`. That is the Visual Studio
gesture, it is the thing rust-analyzer and gopls notably *don't* ship, and the group model now makes it
a mechanical read. The F5 debug path (`editor/vscode/extension.js` `build()`) must pass the same
selection, or debugging builds a different program from the one the editor is showing.

---

## What M5 leaves you (start here, it is all measurable now)

- **`tools/lsp-bench.sh`** — the perf oracle. `--lsp [file]` drives a real stdio session and prints the
  per-keystroke phase split; the default mode uses `kama query` and needs no session. Record a baseline
  into `build/` before touching anything (`build/` is gitignored — regenerate, never assume).
- **`KAMA_TIMING=1`** — one `kama-timing:` line per analysis on **stderr** (never stdout; that is the
  JSON-RPC channel). `KAMA_TIMING=2` adds a line per parsed/cached unit.
- **Current per-keystroke cost**: 10 ms (29-line file) to 86 ms (`process.kama`, 17 units). The budget
  is 100 ms. **If M6 adds per-keystroke work, measure it** — there is now no excuse not to.

### The one perf item left, and it is a ROADMAP entry rather than an M6 task

Roughly **10 ms of every analysis is the prelude being re-ANALYZED** (M5.1 removed the re-*parse*; every
fresh `CEmitter` still re-collects the prelude and the three built-in modules). That is the residual
fixed floor. Attacking it means a pre-baked analyzed prelude or a reusable/forkable `CEmitter`, which is
a real change to the emitter's core invariants — and at 10 ms it is comfortably inside budget. Do not
start it unless something else pushes the budget.

## Non-negotiables (unchanged, and M5 confirmed all of them)

- **The sanitized-compiler run of both LSP harnesses is a MANUAL campaign-exit step.**
  `make EXTRA_CXXFLAGS="-fsanitize=address,undefined"`, then `tools/check-lsp.sh` and
  `tools/check-query.sh`. `run_tests.sh` gates them off under `KAMA_SAN=1`, so nothing else covers the
  LSP C++ — this is how M4 found a pre-existing out-of-bounds read. **Run it in the container as well:**
  macOS ASan has no LeakSanitizer, and the M5 caches are process-lifetime.
- **`tools/lspref.sh` before/after** anything touching `kama.driver.cpp` or `kama.y`.
- **`tools/cdev make` before `tools/cdev test`.**
- Never call `cType` from a query path.
- Positions are kama line 1-based / column 0-based below the protocol layer; `kamaPos`/`lspRange` in
  `kama.lsp.cpp` are the only two conversion points.
- **`tools/check-lsp.sh` is one flat shell scope.** Pick fixture variable names that are not already
  taken — M5 lost time to a new `DURI` silently retargeting a fixture 80 lines below, with the
  assertions still "passing" against the wrong buffer.
