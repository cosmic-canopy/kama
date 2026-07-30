# LSP M6 — editor clients + the campaign's loose ends (cold-start brief)

**Status: STAGE A COMPLETE (2026-07-29). NEXT = STAGE B, then C, then D — start at
[§ Stage B/C — start here](#stage-bc--start-here).**

M6 is the last LSP milestone. It has four stages; **A is shipped**:

| Stage | What | Status |
|---|---|---|
| **A** | The three deferred correctness items (`setBuildFlags`, argument labels, the import diagnostic) | ✅ **SHIPPED** — `5061875`, `d78a1fd`, `7f8dfde`, `8dc90ab`, `cbdd284` |
| **B1** | Fix the TextMate grammar (11 verified defects) + extend `check-syntax-drift.sh` | **NEXT** |
| **B2** | `textDocument/semanticTokens` (`/full` only), measured | pending |
| **C** | Editor clients for 7 more editors + `docs/editors.md` + the VS Code status-bar picker | pending |
| **D** | Campaign exit | pending |

Read [lsp.md](lsp.md) for campaign context and [build-configuration.md](build-configuration.md) for the
flag model. Session plan of record: `~/.claude/plans/let-s-continue-the-lsp-warm-wilkes.md`.

> ⚠️ **Line numbers below were re-derived against `cbdd284`** (2026-07-29, after Stage A). Stage A moved
> `kama.driver.cpp` by ~380 lines and touched `kama.lsp.cpp`, `kama.lsp.h`, `kama.query.cpp` and
> `kama.cemit.{h,cpp}`. **Re-derive again if you land anything before starting** — a stale brief is how
> this campaign has repeatedly misled the next session, and it had already happened twice before Stage A.

---

## Stage A — as shipped, and where this brief was wrong

Every prior milestone's brief was wrong somewhere load-bearing. This one was too, in three places:

1. **The override channel was wrong.** The brief specified `initializationOptions` +
   `workspace/didChangeConfiguration`. The user chose **`kama.local.json`** instead (see the struck-through
   bullet under item 3c), and it is the better answer: the editor and `kama build` agree *by construction*
   rather than by remembering to pass the same flags twice, the F5 path needs no arguments of its own, and
   every editor gets the same override with no per-client settings schema. `didChangeConfiguration` is
   consequently **not wired at all** — the trigger is `didChangeWatchedFiles` on the two manifests.
2. **A2's key design would have shipped a silent, test-invisible bug.** The brief said to `recordRef` the
   label with the param's `bindingKey`. `bindingKey` reads `_refUnit`, which at a call site is the
   **caller's** unit while the DefSite was keyed under the **declaring** unit — so every cross-unit label
   would produce a key matching nothing, be dropped, and *same-file labels would still appear to work*.
   Fixed by inverting: store the param's declaration NODE, resolve node→key after `buildDefSites`.
   `tests/query/labels/` is deliberately two units to lock this down.
3. **The A2 std-parameter hazard was mis-diagnosed** (by the plan, not the brief). It does not
   materialize — rename already refuses, because such a param has no def-site the project owns and
   `includeDeclaration` puts the declaration among the references, so both the `ownsFile` check and the
   project-less "used in another file" check fire. Verified end-to-end rather than reasoned about.

**Two things found en route that were not on anyone's list:**

- **⚠️ NOTHING INSIDE A GENERIC TYPE'S OR GENERIC FUNCTION'S BODY IS IN THE REFERENCE INDEX.** Params,
  locals, `foreach`/`match` bindings and body use-sites alike, which covers all of `lib/std`'s containers
  (`DynamicArray<T>.add(item:)` and friends). **One cause:** generic instances are emitted from
  `emitHeaderContent` (`emitGenericInst` / `emitGenericTypeInst`), which runs *before* the per-unit
  `emitModuleContent` loop that sets `_refUnit` — and `recordRef`/`recordDef` both drop everything when it
  is null. Not fixed in A2: it needs a template → declaring-unit map available during the header pass
  (`_declUnit` is built in `buildDefSites`, i.e. after emission), each instantiation re-walks the *same*
  template nodes so attribution must be to the template's own unit, and it touches the emission path where
  the campaign holds a byte-identical-output invariant. **Tracked in ROADMAP §10** under the LSP bullet.
- **A latent trap in the harness**, fixed: `check-lsp.sh` counted `"code":"Parse"` across *every*
  `recover.kama` publish in the session and demanded exactly 3, so any republish (which A1 introduces) would
  fail it for a reason unrelated to recovery. Now `head -1`, which is what it always meant.

**Also shipped, as an A1 prerequisite:** `select.TARGET` gained `"default": true` — every other select value
already had it, so no manifest could declare a default target and a single-board project retyped `--target`
forever. Precedence: built-in `HOST` < `kama.json` < `kama.local.json` < `--target`.
Guarded by 4 new cases in `tools/check-target.sh`.

**Stage A verification, all green:** 802/802 native **and** container; emission byte-identical across 537
`lspref` fixtures; both LSP harnesses clean under `-fsanitize=address,undefined` on **both** platforms
(LeakSanitizer included, which only the container has); per-keystroke 84–86 ms against the 100 ms budget,
one timing line per request.

---

## Stage B/C — start here

**Before the first edit**, regenerate the baselines (`build/` is gitignored — regenerate, never assume):

```sh
make && tools/lspref.sh > build/lspref-before.txt      # 537 lines, 0 TRANSPILE_FAILED
tools/lsp-bench.sh       > build/lsp-bench-before.txt  # cold-process phase split
tools/lsp-bench.sh --lsp > build/lsp-bench-lsp-before.txt   # steady state: 84-86 ms, 12 timing lines
```

**Seam map, verified against `cbdd284`:**

| What | Where |
|---|---|
| `initialize` capabilities block | [kama.lsp.cpp](../kama.lsp.cpp):642-663 — add `semanticTokensProvider` after :662 (`signatureHelpProvider`), before :663; the nested `completionProvider` just above is the shape to copy |
| Handler template | `handleDocumentSymbol` [kama.lsp.cpp](../kama.lsp.cpp):762 (uri → `docs.find` → seam → `Json::array()` → `sendResponse`) |
| Dispatch table | insert after [kama.lsp.cpp](../kama.lsp.cpp):1135; :1136 is `didChangeWatchedFiles`, :1145 is the -32601 fallback. Method string `textDocument/semanticTokens/full` |
| Seam declaration | [kama.lsp.h](../kama.lsp.h):196 (`lspPrepareRename`) — declare `lspSemanticTokens` beside it, before :202 |
| Seam definition | [kama.driver.cpp](../kama.driver.cpp):4521 (`lspPrepareRename`) / :4527 (`lspCompletion`) — same 2-line null-guard-and-forward |
| Facade | `documentSymbols` [kama.query.cpp](../kama.query.cpp):846; `workspaceSymbols` :869 — put `semanticTokensFor` beside them |
| Token data | `_positions` per unit, **already sorted by (line, column)** at [kama.query.cpp](../kama.query.cpp):707-713 — exactly the order delta encoding wants. `DefSite.kind` classifies every key including the `local:`/`field:`/`enum:` ones |
| Value shapes | [kama.query.h](../kama.query.h):16 `SrcRange`, :22 `SymKind` (14 values), :29 `DefSite`, :44 `PosEntry` |

**B2's four real hazards** (all re-confirmed in the current tree):

1. **`_positions` contains duplicate ranges.** Step (3)'s dedup is by `IdentifierNode*`, but decl-name
   entries are pushed with `id == nullptr` ([kama.query.cpp](../kama.query.cpp):613), so they never enter
   the `seen` set. The known collision — a type declaring a `ctor` whose implicit result type resolves
   through the class's own decl identifier — is filtered **only in `_refIndex`**
   ([:717-736](../kama.query.cpp#L717), the `selectionRange` equality test at :733-734), *not* in
   `_positions`. LSP forbids overlapping tokens, so dedupe by (line, column) in the facade.
2. **`selectionRange` can be multi-line** when `nameId` is null ([:380](../kama.query.cpp#L380) falls back
   to the whole decl node's span). Tokens cannot span lines — drop any entry where `endLine != line`.
3. **`endLine`/`endColumn` of 0 means UNKNOWN** — needs the collapse `lspRange` does, but expressed as a
   *length* rather than a range.
4. **Empty `declKey`** (builtins like `int32`, unresolved names) → emit nothing and let TextMate colour it.

**`/full` only** — no range or delta variants. **Measure with `tools/lsp-bench.sh --lsp`**: semantic tokens
fire on every edit in most clients, and this is the one M6 item that can move the budget.

**Stage C's client work now has a settled shape**, and it is smaller than the brief implies:

- The VS Code watcher **already** covers both manifests ([extension.js](../editor/vscode/extension.js), the
  `synchronize.fileEvents` array) — Stage A extended it. Every other client wires the same client-side hook.
- **The status-bar picker writes `kama.local.json`, not editor settings**, so the F5 path needs no changes
  and the same override works in every editor. What VS Code still lacks: an explicit
  `"activationEvents": ["onLanguage:kama"]` (there is none today), a `contributes.configuration` section,
  and a `kama.selectBuildConfig` command. `activate()` is where the status-bar item goes, refreshed from
  `onDidChangeActiveTextEditor`.
- **`editor/vscode/README.md` is stale** — it still says find-references, rename and completion "arrive in
  later milestones"; all three shipped in M3/M4.
- `docs/editors.md` must state **honestly which editors get colour from what**: TextMate for VS Code and
  Sublime (Sublime consumes the same `.tmLanguage` — reuse it), semantic tokens for VS Code / Neovim /
  Emacs, and Helix + Zed getting LSP features with **no** syntax colouring until M7's tree-sitter grammar.
  Register it in `README.md`'s layout bullet, `llms.txt` § Toolchain & runtime (which mentions neither the
  LSP nor any editor today), `GETTING_STARTED.md` §4 (VS-Code-only), and ROADMAP.

**⚠️ `tools/check-lsp.sh` is ONE FLAT SHELL SCOPE** — M5 lost time to a new `DURI` silently retargeting a
fixture 80 lines below with the assertions still "passing" against the wrong buffer. Currently taken:

```
UPPERCASE: BAD CFGA CFGB CFGBAD CFGC CFGDIR CFGG CFGSRC CFGTYPO DSRC DURI FRSRC FRSRC2 FRURI GOOD
           IMP IURI LSRC LURI M34 MURI NEWB NURI OSRC OURI QURI RECOV ROOT RURI SEM SHP SPAN
           SPURI SURI URI WW WWURI XDROP XURI
lowercase: cfgcli cfggtext cfglsp cfgn dep depok drop fail frok frws n out session tmp
request ids: 1-30, 32-49, 53, 54   (FREE: 31, 50-52, 55+)
```

Assertion counts to grow, not shrink: **check-lsp 108** (90 `expect` + 18 `cfgexpect`/`cfgreject` + 4
custom counters), **check-query 173**.

---

## Part 2 — the syntax-highlighting audit (Stage B1) — STILL TO DO, and re-verified

> All ten defects below were **re-checked against the current file during Stage A planning and are
> still present**, plus three more found then: (1b) the escape rule's `\u{…}` hex count is
> unbounded where the lexer is `{1,6}`, so `\u{1234567}` mis-colours as valid; (11) `#declarations`
> and `#cast` are *included after* `#keywords`, and TextMate breaks a position tie in favour of the
> earlier include — so those two rules may be entirely dead. Verify (11) in the tokenizer before
> relying on either. One thing the grammar gets RIGHT and must not be "fixed": `/*` does not nest
> in `kama.l` either, so the non-nesting block-comment rule is correct.

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

## Part 3 — the deferred items — ✅ ALL THREE SHIPPED IN STAGE A

> **Kept verbatim below for the reasoning, not as a work list.** Read the "Stage A — as shipped" section at
> the top first: it records the three places this part was wrong. In particular 3a's `recordRef` advice and
> 3c's `initializationOptions` channel were both superseded, and following them now would reintroduce the
> bugs Stage A fixed.

### 3a. Index argument labels (carried from M4, unblocked by M4.9) — ✅ SHIPPED `8dc90ab`

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

### 3b. Promote the undeclared-import warning to a diagnostic (carried from M4) — ✅ SHIPPED `cbdd284`

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

### 3c. The LSP never calls `setBuildFlags` (found during M5) — ✅ SHIPPED `5061875`+`d78a1fd`+`7f8dfde`

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
- ~~**The override channel is `initializationOptions` + `workspace/didChangeConfiguration`**, carrying
  `{ select: {GROUP: VALUE}, define: [], undefine: [] }`.~~ **WRONG — superseded (user, 2026-07-29).**
  The channel is **`kama.local.json`**, kama's existing gitignored sibling of `kama.json`, already
  deep-merged by every CLI path for `flags`/`select`/`log`. That makes the editor and a plain `kama build`
  agree *by construction* rather than by remembering to pass the same flags twice — which is the entire
  point of A1 — so the F5 path needs no arguments of its own, and every editor gets the same override with
  no per-client settings schema. `workspace/didChangeConfiguration` is consequently **not wired at all**;
  because the channel is a file, the trigger is `workspace/didChangeWatchedFiles` on the two manifests.
  Prerequisite this exposed and A1 shipped: `select.TARGET` had no `"default": true`, so no manifest could
  declare a default target.

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
