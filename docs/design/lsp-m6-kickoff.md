# LSP M6 — editor clients + the campaign's loose ends (cold-start brief)

**Status: NOT STARTED — the ACTIVE next milestone** now that M5 has shipped (2026-07-28,
`dfdbb9a`…`6d295e1`). Read [lsp.md](lsp.md) for campaign context, then this. Sized `S/M` in
[lsp.md](lsp.md):222 — the client work genuinely is small, because every editor reads the same server.

M6 is the last LSP milestone. It has three parts, and only the first is what the milestone is named for:

1. **Editor clients** — thin configs pointing at `kama lsp`, plus `docs/editors.md`.
2. **The syntax-highlighting audit** — already scoped in detail at [lsp.md](lsp.md):229-240. Do not
   re-derive it; that section has the motivating evidence (the VS Code grammar disagreed with the
   compiler four ways on numeric literals alone) and the compiler-as-oracle method.
3. **Three deferred items** carried from M4 and M5, written up below so they are not lost.

Everything below was verified against the tree at `6d295e1`.

---

## Part 3 — the deferred items

### 3a. Index argument labels (carried from M4, unblocked by M4.9)

Renaming a parameter should rewrite its call sites. It cannot today, because argument labels are not in
the reference index — and every kama argument is named, so this is not a niche gesture.

- **Seam:** `emitReorderedCall` ([kama.cemit.cpp](../kama.cemit.cpp):8264) — the single named-argument
  matcher for every call form, so one edit covers them all — plus `recordRef` with a `field:`-style key
  naming the callee's parameter.
- **Why it waited:** M4.9 made the label spans correct. Before that this was a data-loss risk, since
  rename *replaces* the range it is handed; a span that ran past the label would have eaten code. It is
  now an ordinary feature.
- **Test the span before the feature.** `prepareRename` on a label must return the label's range and
  nothing more; the M3.4/M4.9 assertions in `tools/check-lsp.sh` are the pattern to copy.

### 3b. Promote the undeclared-import warning to a diagnostic (carried from M4)

`loadProgramUnits` ([kama.driver.cpp](../kama.driver.cpp):560-583) prints "package X imports Y but does
not declare it" to stderr, warn-once per process — so in an editor it lands in the log channel nobody
reads rather than the Problems pane. It should be a `publishDiagnostics` entry against the offending
`kama.json`.

Two things to get right, both already learned the hard way in the workspace-deps campaign (`a99436d`):

- **Never blame a package the user cannot fix** — the gate already clears the owner when it resolves
  under the content-addressed store prefix. Keep that.
- **It is warn-once per process** (a `static std::set` at :484, added precisely so `kama lsp` would not
  repeat it per keystroke). A diagnostic must be *republished* every analysis or it vanishes on the next
  keystroke, so the warn-once set and the diagnostic path need different lifetimes.

### 3c. The LSP never calls `setBuildFlags` (found during M5, pre-existing)

`lspAnalyze` and `lspAnalyzeWorkspace` do not call `setBuildFlags`, while `kama check`
([kama.driver.cpp](../kama.driver.cpp):4065) and `kama query` (:4130) do. So `_activeFlags` is empty in
the server, and `pruneInactiveDecls` drops every `@compileFor`-gated declaration that a real build would
keep — the editor sees a different program from the compiler.

Unrelated to M5 and deliberately not fixed there (M5 touched that pass's *reuse* invariant, not its
inputs, and mixing the two would have muddied a bisect). It needs a decision, not just a patch: **which
flag set should an editor analyze under?** The build's default is the obvious answer; a per-workspace
override in `kama.json` is the likely follow-on. Note the M5 parse cache's soundness argument assumes a
**fixed** flag set per process — if flags ever become switchable at runtime, `lspEvictParsedFile("")`
must be called on every change. That is written at `parseFile`; keep it true.

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
