# Language Server (LSP) — campaign kickoff / handoff

**Status: M0–M5 shipped; M6 STAGES A + B1/B2 shipped (2026-07-29); M6 B3 COMPLETE (2026-07-30, dev)** —
the three deferred correctness items are closed, the TextMate grammar agrees with the compiler and is
guarded by a real tokenizer, `textDocument/semanticTokens/full` colours by what the resolver concluded, and
**the reference index no longer loses work**: renaming a method, a contract method, an enum behind a `::`
qualifier, an exported type or a generic argument now rewrites every spelling of it, including inside a
generic body and across units. **NEXT = Stage C** (editor clients + `docs/editors.md`), which has its own
cold-start brief: [lsp-m6-c-kickoff.md](lsp-m6-c-kickoff.md). Then Stage D exit, then M7's tree-sitter
grammar. **All of it is pre-launch** (user, 2026-07-29).

**One decision from B3 that generalizes beyond the LSP:** a MODULE path is a navigation target and never a
rename target. In kama the namespace is the module path is the directory path, so renaming one is a
file-and-directory move — the same line clangd draws for `#include`, TypeScript for a module specifier and
gopls for an import path. Keeping `module:` keys out of `_defSites` is what makes rename, find-references
and semantic tokens ignore them with no extra flag.

⚠️ **The campaign's most transferable lesson is stage 0's, not B3a's.** The reference index is built by
INSTRUMENTING the emitter, so it is exactly as complete as the set of sites someone remembered to
instrument — and every test asserted a spelling somebody had thought of, so a spelling nobody thought of
failed nothing. That is not a bug you fix once. `kama query <file> --coverage` asks the other question (for
every identifier the SOURCE spells, what does the index know?) and freezes the answer in a checked-in table
per fixture, so a gap is a diff. It found nine gaps where the brief had named two. **Extend
`tests/query/coverage/` whenever a construct is added to the language.** Cold-start brief:
**[lsp-m6-kickoff.md](lsp-m6-kickoff.md)** — it carries an as-shipped record of Stage A, including the three
places its own earlier text was wrong, and a seam map for B/C re-derived after Stage A moved four files.

> **Interleaved before M6: the build-configuration campaign** (✅ shipped 2026-07-29,
> `fdc9a75`…`fbe69f5`; [build-configuration.md](build-configuration.md)). Sequenced first because M6's
> deferred item 3c — *the LSP never calls `setBuildFlags`, so the editor analyzes a different program
> than the compiler* — turned out to need a **decision** about what an editor should analyze under, and
> the flag model it would have pointed at was half-formed. Targets are now `<arch>-<os>-<abi>` triples
> whose components derive the `@compileFor` flags, `TARGET`/`BUILD_TYPE`/`OUTPUT` are user-extensible
> single-select groups declared in `kama.json`, cross-compilation works, and static libraries exist.
> M6 3c is now a wiring job with a settled answer, and the group model is what the VS Code client's
> configuration switcher will read. **M7 is now the tree-sitter grammar** (Neovim/Helix/Zed
> highlighting + a Zed extension), moved out of M6 by the user on 2026-07-28.

M5 made the server feel good rather than
merely work: error recovery (many diagnostics instead of one, and queries keep answering on a broken
buffer) and a per-keystroke cost inside the 100 ms budget on every measured file
([lsp-m5-kickoff.md](lsp-m5-kickoff.md)). Interleaved before M4: workspace-internal dependencies
([workspace-deps-kickoff.md](workspace-deps-kickoff.md)). The **confirmed next-highest post-1.0 priority** (user, 2026-07-26;
[ROADMAP.md](../ROADMAP.md) §1 post-1.0 sequence + §10). This doc is the cold-start handoff: what exists to
reuse, the decisions to settle FIRST, a milestone plan, and a size gauge. **Read [GOALS.md](../GOALS.md) and
ROADMAP §10 before designing.**

## Progress

- **M0 — front-end-as-library + query index + spans. ✅ DONE.** Commits `328a32c` (T1–T3: analyze() seam +
  structured diagnostics + span fields), `fd817dc` (T4a: def-site table), `4a74dfe` (T4b/T4c/T5: position
  index + resolution replay + query facade + **real Bison `%locations` spans**). New `kama.query.{h,cpp}`
  hold the framework-free query types + facade (`documentSymbols`/`definitionAt`/`typeAtPosition`/
  `diagnosticsFor`), built read-only at the tail of `analyze()`. Debug harness: `kama query <file>
  --symbols|--def L:C|--type L:C` + `tools/check-query.sh`. Emission byte-identical; native 792/792;
  ASan+UBSan clean. **The gating XL refactor was milder than framed** — `collectProgram()` was already a
  side-effect-free analysis pass, so M0 became a facade over it (not a class-hierarchy split). Scope: the
  index covers declaration + signature/type-reference positions; body use-sites are M3, locals-hover is M2.
- **M1 — server scaffold + live diagnostics (the walking skeleton). ✅ DONE.** New `kama lsp` subcommand:
  a JSON-RPC 2.0 server over stdio (hand-rolled JSON reader/writer + framing, no deps) that reuses the M0
  analysis path to publish live, as-you-type diagnostics. New `kama.lsp.{h,cpp}` (transport + lifecycle +
  full-document sync + coord mapping); the driver exposes ONE external seam `lspAnalyzeBuffer` (the
  parse/prelude plumbing lives in the anon-namespaced driver, so the LSP module owns only the editor-facing
  half). Gap filled: `parseForQuery` keeps the `CodeGenContext` alive so parse diagnostics survive a failed
  parse (as-you-type buffers are mid-edit). Coord map (one place): kama line 1-based/col 0-based → LSP
  0-based/0-based. VS Code language client added (`vscode-languageclient`, F5 debug kept). Harness:
  `tools/check-lsp.sh` (scripted JSON-RPC session over stdio) wired into `run_tests.sh` native leg. native
  793/793; the LSP C++ (JSON parser/transport) is ASan+UBSan-clean under adversarial input; emission
  unchanged (all changes additive). Design of record: [lsp-m1-kickoff.md](lsp-m1-kickoff.md).
- **M2 — hover + go-to-definition + document symbols (the first interactive features). ✅ DONE.** Wired
  the M0 facade (`documentSymbols`/`definitionAt`/`typeAtPosition`) to `textDocument/documentSymbol` /
  `definition` / `hover` and flipped on the three `initialize` capabilities. The one structural change:
  the driver seam `lspAnalyzeBuffer` became `lspAnalyze` (kama.lsp.h/driver.cpp), returning an **opaque
  `SharedLspIndex` handle** (holds the analyzed `CEmitter` alive) alongside diagnostics; the server's
  `Doc` caches it as `lastGoodIndex` (kept across a parse failure so hover/def stay live on a mid-edit
  buffer). Query seams `lspDocumentSymbols`/`lspDefinition`/`lspHover` forward to the facade. New
  kama.lsp.cpp helpers: `pathToUri` (inverse of `uriToPath`), `srcRangeToJson` (generalized from
  `rangeToJson` via a shared `lspRange`), `symKindToLsp` (`SymKind`→LSP `SymbolKind`). Coverage matches
  M0's index — decl names + signature/type references; body use-sites (variable uses, field access) and
  locals return null **by design** (that's M3 find-references). `tools/check-lsp.sh` extended with the
  three request types + capability + intentional-null assertions (16 checks). native 793/793; ASan+UBSan
  clean; emission unchanged. Design of record: [lsp-m2-kickoff.md](lsp-m2-kickoff.md).
  - **Follow-up (`9a7c15e`): module-aware analysis.** `lspAnalyze` originally analyzed only the open
    buffer + built-in prelude, so any `import`ed type read as *"module X does not export Y"* — a cascade
    of false diagnostics on real multi-module files. It now `loadProgramUnits()` the open file's transitive
    imports from disk (stdlib via `argv0`, threaded through `runLspServer`), substitutes the live buffer
    for the open file's on-disk unit, and returns only the open file's diagnostics. Cross-module
    go-to-definition falls out for free. `tests/query/imports.kama` fixture + 2 harness checks (18 total).
- **M3.1–M3.3 — find-references + rename (types + free/generic functions). ✅ DONE.** The body use-site
  index M0/M2 deferred, built the way the brief's option (A) proposed: **no body walker**. The real
  resolver records each use as it resolves it — `resolveUserName`/`resolveFunc` gained a defaulted trailing
  `site` identifier, and exactly **five** call sites pass it (`cType` ×3, which covers every type mention
  anywhere — locals, fields, `new T`, casts, nested generic args; `emitInvocation` ×2 for calls). Recording
  is gated on a new `_analysis` flag (set only by the analysis ctor, so `kama build` pays nothing) and on
  `_refUnit` (set in `emitModuleContent`, so each use is attributed to the file it was spelled in).
  Everything merges into the **existing** `_positions` index rather than a parallel structure: a
  resolve-fill sweep at the tail of `buildPositions()` gives every entry its `declKey`, then `_refIndex`
  (key → uses) falls out of one pass. That simplification made `definitionAt`/`typeAtPosition` **`const`
  and replay-free** (retiring the "non-const query" trap) and made **hover + go-to-definition work inside
  bodies** for free. New: facade `referencesAt`/`renameRangeAt`, driver seams `lspReferences`/
  `lspPrepareRename`, `textDocument/references` + `rename` + `prepareRename`, capabilities
  `referencesProvider` / `renameProvider:{prepareProvider}`, and `kama query --refs L:C`. Rename validates
  the new name against **the lexer's own keyword table** (`kamaIsKeyword` in `kama.l` — no duplicated
  list). Emission byte-identical across all 534 transpile fixtures; native 794/794; ASan+UBSan clean;
  `-Werror` clean.
  - **Rename is deliberately guarded to the open file.** References span every loaded unit, but the loaded
    set is only what the open file transitively imports — a file that imports *this* symbol without being
    imported back is invisible, so a cross-file rewrite could silently break an unseen caller. Rename
    refuses with an explanatory error instead of half-rewriting. **M3.5 lifts this** (see below).
- **M3.4 — locals / params / fields / enum members. ✅ DONE (`91e27a3`).** The kinds users touch most.
  Two halves. **(1) A grammar span pass, which was a data-loss fix, not polish:** `YYLLOC_DEFAULT` gives an
  identifier built mid-action its whole *production's* span, and rename REPLACES the range it is handed —
  so renaming `int32 seeded = 7` would have rewritten `seeded = 7`, and `Code::Ok` would have lost its
  qualifier. `STAMP_LOC` now narrows declarators, const declarators, enum members, parameters, the four
  member-access arms, `foreach`/`parallel_for` loop variables, `base.field`, the `Ns::Name` use production
  and the generic `Name<A, B>` arm — the last three of which shipped rename already depended on.
  **(2) Index plumbing reusing the M3.1 machinery**, not a second index: bindings have no table entry to
  key on, so `recordDef`/`_localDefs` mirror `recordRef`/`_bodyRefs` and materialize in `buildDefSites`.
  Keys are prefixed into an index-only namespace (`local:<file>:<line>:<col>:<name>`, `field:…`, `enum:…`);
  keying a binding by its **declaration site** is what makes two same-named locals in sibling scopes
  distinct symbols. `Scope` gained an analysis-only `indexDecls` vector *parallel* to `declaredNames`
  rather than changing that vector's type — `declaredNames` drives the shadowing rules and deliberately
  omits `foreach`/`match` bindings, which the index wants. `MatchArmNode` gained
  `variantId`/`bindingIds` alongside its existing strings, so a `case Ok:` arm is a real reference (without
  it, renaming an enum member produced code that no longer compiles). Locals and params are indexed but
  filtered out of `documentSymbols`; fields and enum members appear. Emission byte-identical; native
  794/794, container 794/794, ASan/UBSan 765/765; harnesses +39 assertions, including exact
  `prepareRename` ranges per kind as the data-loss guard.
- **M3.5 — workspace indexing. ✅ DONE.** The M3.3 guard is lifted: find-references and rename now span the
  whole project, not one file's import closure. **Project root = the OUTERMOST `kama.json` walking up from
  the open file** (npm/cargo *workspace* semantics, bounded by the editor's folder), so a monorepo's root
  manifest wins over a package's and renaming in one package sees the other packages' uses — nearest-wins
  would index only the one package and then silently rewrite it anyway, recreating the M3.3 bug one level
  up. With no manifest the root falls back to the editor's `rootUri`, capped at 500 `.kama` files: cstar
  itself holds 870, 813 of them independent `tests/` fixtures with their own `main` and colliding type
  names, which as one program would be slow *and* wrong. A **second, lazily-built index** (`lspAnalyzeWorkspace`,
  every open buffer passed in as an overlay) serves only references/rename/`workspace/symbol`; diagnostics,
  hover, go-to-def and the outline stay on the per-document index, so typing costs nothing extra.
  Rename refuses in three narrow cases — no project, project too large, and a definition outside the
  project (std or a dependency; `DefSite.unit == nullptr` filters only the built-in prelude, so std
  *modules* would otherwise look renameable) — and otherwise emits a multi-file `WorkspaceEdit` grouped by
  URI. **A file in no project still renames** under M3.3's original open-file rule, which is what keeps
  M3.4's local/param/field rename working in a standalone buffer. New: `workspace/symbol`,
  `workspace/didChangeWatchedFiles` (the VS Code client registers the watcher, avoiding server→client
  `client/registerCapability`), `kama query --project`, `lspRealPath`. Emission byte-identical across all
  535 fixtures; native 797/797, container 797/797, ASan/UBSan 768/768 plus a sanitized-compiler run of both
  LSP harnesses. Cost: **0.45 s** for a 43-file project rebuild vs 0.23 s for a single-file closure.
- **M4 — completion + signature help. ✅ SHIPPED 2026-07-28** (M4.0–M4.9). Design of record, with an
  "As shipped" section: [lsp-m4-kickoff.md](lsp-m4-kickoff.md). Three of the brief's load-bearing claims
  did not survive contact with the code, and each reshaped the milestone:
  - **`exprClass` / `isTypeReceiver` / `canAccess` are unusable from a query path.** The brief listed all
    three as free reuse. `exprClass` calls `cType` on six paths — forbidden, since `unsupported()` would
    land a phantom diagnostic on the file the editor is showing — and reads `_localTypes`, which is
    cleared at every function entry and after `analyze()` holds the LAST emitted function's locals.
    Substitutes: `mangleElem` (type node → mangled key, generic instances included), `findMethod`, and a
    fresh pure `visibleFrom`.
  - **The "scope extents" prerequisite was unnecessary.** kama FORBIDS shadowing, so within a callable a
    name is unique except across sibling scopes — and completion emits LABELS, which dedupe. Rename needed
    per-declaration identity; completion does not. Consequence: **zero emitter edits** in M4.0–M4.7.
  - **The "named-argument spans block signature help" prerequisite was false.** With no error productions
    in the grammar, a half-typed call has no AST anywhere, so the active parameter must come from a
    lexical scan regardless. The checklist stayed a campaign-exit requirement and shipped as M4.9.

  The brief also **omitted the highest-value context in the language**: every kama argument is named, so an
  argument slot with no label yet is a first-class completion context, ranked above import paths.

  Also fixed here: a **pre-existing out-of-bounds read** in `lspAnalyzeWorkspace` (parallel `units`/`paths`
  vectors, a `paths` index bounded by `units.size()`), present since the workspace-deps campaign and
  invisible in a plain build. The sanitized-compiler run of the LSP harnesses is what caught it — it is a
  manual step, not part of `run_tests.sh`, so **run it at the end of every LSP milestone**.
- **M5 — error recovery + incremental/perf. ✅ SHIPPED 2026-07-28** (`dfdbb9a`…`6d295e1`). As-shipped
  record: [lsp-m5-kickoff.md](lsp-m5-kickoff.md). The milestone that makes the server feel good rather
  than merely work.
  - **Recovery.** `error` arms at three grains added **innermost-first** — statement, class member, top
    level — because Bison pops to the nearest state carrying an `error` action, so a broken statement
    now costs one statement instead of the enclosing function. A buffer reports every independent error
    instead of one, and hover/outline/completion keep answering off a live *partial* index rather than a
    stale last-good one. Semantic diagnostics publish from a partial parse **except** when the top-level
    arm fired (the one case that cascades — measured at 1 real diagnostic vs 3 on a two-use file), which
    is the same bargain TypeScript, clangd and rust-analyzer strike.
  - **Perf.** Prelude parsed once per process; the import closure cached across analyses (opt-in,
    `kama lsp` only, keyed by path *spelling*). Worst measured file: **237 → 86 ms per keystroke**,
    inside the 100 ms budget at :253, and every other measured file 10–46 ms.
  - **M4.6's repair deleted.** It cost a full 229 ms re-analysis on every completion against an
    unparseable buffer; recovery keeps the index fresh, so completion is a lookup again. Retired against
    a criterion — `KAMA_LSP_NO_REPAIR=1 tools/check-lsp.sh` had to pass the whole harness *including*
    the two M4.6 assertions — not a hunch.
  - **A real compiler bug fixed en route:** 40 nested `if`s reported "memory exhausted", because
    `YYSTYPE` is a plain struct so Bison's stack-relocation path is compiled out and the parse stack
    could not grow. Raised `YYINITDEPTH`; do **not** "fix" it with `YYSTYPE_IS_TRIVIAL`/`yyoverflow`,
    which memcpy a stack of `shared_ptr`s.
  - New tool: **`tools/lsp-bench.sh`**, the reproducible perf oracle (the brief's original numbers were
    ad hoc and not reproducible from a checkout). ⚠️ The brief was wrong in three load-bearing places,
    one of which would have silently *regressed* completion — see its "As shipped" section.

## Why (from the ROADMAP)

Today the VSCode extension is **highlighting + zero-config LLDB debugging only** — no completion, hover,
go-to-definition, find-references, rename, signature help, document outline, or as-you-type diagnostics.
Without these kama "won't feel like a professional language." The LSP also does **double duty**: it forces the
front end into a **reusable query API with real source spans** — groundwork the scripting / self-hosting
tracks later reuse — and it's the prerequisite for the `global::` floor-completion idea
([logging.md](logging.md) Part E, [FLOOR.md](../FLOOR.md)). Not 1.0-blocking; load-bearing for adoption.

## What already exists (reuse inventory)

The compiler is **C++** (~23.75 K lines): Flex `kama.l` (526) + Bison `kama.y` (1,643) + `kama.ast.h` (1,073)
front end; `kama.cemit.cpp` (14,785) + `kama.cemit.h` (1,334) semantic analysis & C emission;
`kama.comptime.cpp` (655); `kama.driver.cpp` (3,509); `kama.context.h` (49) error reporting.

- **Source locations:** every `ASTNode` carries `int line` + `int column` (kama.ast.h:15-16), populated during
  parse; the emitter already writes `#line` directives (source-level debugging works). **No byte offsets / end
  positions / ranges** yet — Bison `%locations` was deliberately deferred *to this campaign*.
- **Semantic info exists but is welded to emission:** name resolution (`resolveUserName`/`resolveFunc`), a full
  type system (`ClassInfo`/`ContractInfo`/`GenericTypeInfo`), overload/operator/contract dispatch, generic
  monomorphization, per-file/class/function scopes, import aliases — all live inside `emitCodeForNode()` in
  `kama.cemit.cpp` and run **as a side effect of generating C**. There is **no query surface**: you cannot ask
  "type at position", "definition of symbol", or "symbols in file" without running (and coupling to) codegen.
- **Diagnostics:** `CodeGenContext::handleError()` prints `file:line:col: error: msg` to **stderr as plain
  text** (no structured Diagnostic{range,severity,code}); **halts after 10 errors**; **no parse error
  recovery** — a syntax error stops the parse (fatal for as-you-type editing, where the buffer is usually
  mid-edit and invalid).
- **Driver:** single-shot `kama build`/`transpile` (lex→parse→check→emit C→cc). **No watch, no incremental
  reparse, no library mode** — the compiler is a CLI, not linked as a lib.
- **Editor:** `editor/vscode/` — `extension.js` (70), `syntaxes/kama.tmLanguage.json` (152 TextMate),
  `language-configuration.json` (30). Highlighting + brackets + F5→`kama build`+CodeLLDB. **No language
  client.** `tools/check-syntax-drift.sh` keeps the grammar's keyword set in sync with `kama.l`.

## Decisions to settle FIRST (with leans)

1. **Server language / reuse strategy (settle first — shapes everything).** *Lean:* write the LSP server in
   **C++, reusing the existing front end as a library** (link the lexer/parser/AST + a refactored semantic
   pass). The alternative (a fresh server in another language, or waiting for self-hosting) throws away the
   23 K-line investment. Cost: the **front-end-as-library refactor** (M0) — the campaign's dominant work.

2. **Parser: keep Bison vs. hand-written recursive-descent (the biggest size swing).** Bison gives **no error
   recovery and no incremental reparse** — both are what make an LSP feel good on half-typed code. Options:
   (a) keep Bison, add error-recovery productions + full-file reparse on each edit (simplest; adequate for
   small/medium files); (b) replace `kama.y` with a hand-written RDP for real recovery + incremental reparse
   (a large sub-project — rewrites `kama.y`, 1,708 lines / 483 hand-written productions / 981 LALR states as of M4, but ROADMAP flags it as the "right" long-term move
   and it also benefits self-hosting). *Lean:* **(a) for v1** (Bison + recovery + whole-file reparse; files are
   small, reparse is milliseconds), **revisit (b)** only if latency/recovery quality demands it. Settle before
   M0 because it changes the M0 refactor's shape.

3. **Span strategy.** Add Bison `%locations` (or thread byte offsets through the lexer) so nodes carry
   **start+end ranges**, not just line/col. *Lean:* do this in M0 — every feature needs precise ranges.

4. **v1 feature scope.** *Lean:* v1 = **diagnostics (as-you-type) + hover + go-to-definition + document
   symbols/outline**; v2 = **completion + signature help + find-references + rename**. Diagnostics-first gives
   the biggest perceived win earliest and validates the whole pipeline.

5. **Transport / framework.** Standard JSON-RPC over stdio. *Lean:* a small hand-rolled JSON-RPC loop (kama's
   ethos: few deps) OR a minimal vendored C++ LSP framework — decide in M1.

## Milestone plan + size gauge

Sizes are T-shirt (S≈part of a session, M≈1 session, L≈2-3, XL≈several).

- **M0 — Front-end-as-library + query index + spans. `XL`. THE GATE.** Decouple semantic analysis from C
  emission: a pass that parses + resolves + type-checks and populates a **queryable index** (symbol table with
  defs/refs, type-at-node, diagnostics *collected* into structured objects, not printed) WITHOUT emitting C.
  Add source ranges (decision 3). This is the hard, load-bearing refactor of the 15 K-line `kama.cemit.*`; do
  it carefully behind the existing emit path so `kama build` stays green throughout.
- **M1 — Server scaffold + diagnostics. `L`.** JSON-RPC/stdio loop, lifecycle (`initialize`/`shutdown`),
  document sync (`didOpen`/`didChange`/`didClose`), `publishDiagnostics` off M0's collected diagnostics. First
  real feature; proves the pipeline end-to-end.
- **M2 — Hover + go-to-definition + document symbols. `M`.** Direct reads off the M0 index.
- **M3 — Find-references + rename. `M/L`.** Needs a complete reference index (every use-site → def), heavier
  than go-to-def; rename = workspace edits + safety checks.
- **M4 — Completion + signature help. `L`.** The quality-hard one: context-sensitive (members after `.`,
  names-in-scope, import paths, keywords). Delivers the `global::` floor-completion payoff.
- **M5 — Robustness: error recovery + incremental/perf. `L`. ✅ SHIPPED 2026-07-28** (`dfdbb9a`…`6d295e1`).
  As-shipped record: [lsp-m5-kickoff.md](lsp-m5-kickoff.md). **Recovery:** error arms at three grains,
  added innermost-first (statement → class member → top level), so a broken statement discards one
  statement rather than the enclosing function; a buffer now reports every independent error instead of
  one and still answers hover/def/outline/completion off a live partial index. Semantic diagnostics are
  published from a partial parse except when the top-level arm fired — the one case that cascades.
  Also fixed a real parse-depth bug: 40 nested `if`s used to report "memory exhausted". **Perf:** the
  prelude is parsed once per process and the import closure is cached across analyses, taking the worst
  measured file from 237 ms to **86 ms per keystroke** — inside the budget at :253 — and letting M4.6's
  229 ms repair be deleted, so completion is a lookup again. **Decision 2 (Bison vs RDP) resolved toward
  keeping Bison**, as predicted: latency was import re-parsing, not parsing, and recovery quality did not
  demand an RDP either. `tools/lsp-bench.sh` is the reproducible oracle for all of these numbers.
  ⚠️ The brief was wrong in three load-bearing places (the perf thesis, "analysis never mutates the AST",
  and the recovery staging, which was backwards and would have silently regressed completion) — see its
  "Three places the original brief was wrong".
- **M7 — tree-sitter grammar + Zed extension. `M/L`. PRE-LAUNCH** (user, 2026-07-29 — supersedes the
  "POST-1.0 / does not gate 1.0" note below: the whole LSP campaign now finishes before the website work
  and before 1.0). Unlocks real syntax highlighting in
  Neovim, Helix and Zed (and GitHub linguist), and a Zed extension cannot exist without it. Split out of
  M6 by the user (2026-07-28) because it is a *third* grammar to keep in sync with `kama.l`/`kama.y` and
  wants its own drift guard. Does not gate 1.0.
- **M6 — Editor/IDE matrix + packaging + tests. `S/M`. NEXT / ACTIVE.** Cold-start brief:
  [lsp-m6-kickoff.md](lsp-m6-kickoff.md), which also carries the three items M4 and M5 deliberately
  deferred (argument-label indexing, the undeclared-import diagnostic, and the LSP's missing
  `setBuildFlags` — the last now unblocked by the build-configuration campaign), the ten already-found
  TextMate grammar defects, and `textDocument/semanticTokens`. One `kama lsp` server, thin clients — wire up
  every editor with a generic LSP client and document each in `docs/editors.md`. `tools/check-lsp.sh`-style
  harness driving the server over stdio with fixture requests/responses.
  - **Target matrix (each = a few lines of config pointing at `kama lsp`):** **VS Code** (✅ done — the
    `editor/vscode/` client), **Neovim** (`nvim-lspconfig`), **Vim** (coc.nvim), **Emacs** (eglot /
    lsp-mode), **Sublime Text** (LSP package), **Helix**, **Zed**, **Kate**. Feature parity is automatic —
    all read the same server, so each gains hover/def/outline/refs/rename/completion as the server does.
  - **A1 (`setBuildFlags`) ✅ SHIPPED.** Two decisions worth not re-deriving:
    - **The build-configuration override channel is `kama.local.json`, not `initializationOptions` and not
      editor settings** (user, 2026-07-29 — this supersedes the kickoff brief). It is a gitignored sibling of
      `kama.json` that every CLI path already deep-merges, so the editor and a plain `kama build` cannot
      disagree, the F5 debug path needs no arguments of its own to stay in step, and a Neovim user overrides
      configuration exactly the way a VS Code user does. One mechanism, not two per editor.
    - **`workspace/didChangeConfiguration` is therefore deliberately NOT wired.** The server reads no client
      settings, so the notification carries nothing actionable: a handler would be either dead code that
      reads as though configuration flowed through it, or a `workspace/configuration` *pull* — a
      server→client request the server has no machinery for, and which the same reasoning already ruled out
      for watcher registration. Because the channel is a file, the change trigger is
      `workspace/didChangeWatchedFiles` on `kama.json`/`kama.local.json` instead.
    - **Known limit: one configuration per server process**, pinned from the first opened document that
      resolves a manifest. In a monorepo whose packages declare *different* flag universes, the unpinned
      packages get the pinned one's configuration. Declare the shared flag universe in the **root** manifest,
      or use one window per package. Per-project configuration needs per-configuration parse caches, since
      cached units are pruned in place — its own milestone.
  - **A2 (argument labels) ✅ SHIPPED.** `emitReorderedCall` is the single named-argument matcher for every
    call form, so one line at its per-param loop covers free functions, methods, virtual dispatch, ctors,
    bound closures and operators (29 call sites).
    - **⚠️ The key must NOT be built at the call site.** `bindingKey` reads `_refUnit`, which there is the
      *caller's* unit, while the parameter's DefSite was keyed under the *declaring* unit. A key built at
      the call site therefore mismatches on every cross-unit call — and same-file labels would still appear
      to work, which is the failure mode a test suite is least able to see. So `recordLabelRef` stores the
      parameter's **declaration node** and `buildPositions` resolves node → key *after* `buildDefSites`,
      making the answer independent of walk order. `tests/query/labels/` is a two-unit fixture precisely to
      lock this down.
    - **The std-parameter hazard does not materialize**, and it is worth knowing why: rename already refuses,
      because a std/dependency parameter has no def-site the project owns and `includeDeclaration` puts the
      declaration among the references, so both the `ownsFile` check and the project-less "used in another
      file" check fire. Verified, not assumed.
    - **Found en route: nothing inside a generic type's or generic function's body is in the reference index
      at all** — see the ROADMAP entry under this bullet's parent. Generic instances are emitted from
      `emitHeaderContent`, which runs before the per-unit loop that sets `_refUnit`, so every `recordRef`/
      `recordDef` there is dropped. It covers all of `lib/std`'s containers. Not fixed here: it needs a
      template → declaring-unit map during the header pass and touches the emission path.
  - **A3 (undeclared-import diagnostic) ✅ SHIPPED.** The split it needed already existed in the right
    place: the *detection* is per-call while only the stderr message is warn-once-per-process, so the
    `Diagnostic` is built beside the flag and the `warnedFreeRide` set is untouched. Two things learned:
    - **It must bypass the `droppedTopLevelDecl` gate.** This is a manifest fact, not a semantic cascade, so
      a broken `type` elsewhere in the buffer must not hide it — it is pushed onto `diags` directly rather
      than through `emitter->diagnostics()`.
    - **⚠️ The open-file filter has to canonicalize.** These diagnostics are stamped with `absolutePath(...)`,
      which on macOS resolves `/var` → `/private/var`, while the buffer path comes from a `file://` URI and
      does not. A plain `d.file == path` matched nothing and the squiggle silently never appeared — the same
      path-spelling hazard M3.5 hit with `underRoot`, as exact equality this time instead of a prefix.
      Compare canonicalized; publish under the spelling the server keys documents by.
    - The check only fires for an import that resolves **through a dependency view**, so the fixture has to
      declare the dep, install it, then remove the declaration while the view remains. That is a real
      editing state, and the state in which an editor most needs to speak up.
  - **B1 (the syntax-highlighting audit) ✅ SHIPPED `38fee77`.** Thirteen grammar-vs-compiler
    disagreements closed. Two decisions of record:
    - **A grep cannot guard a highlighter.** `check-syntax-drift.sh` can see that a rule EXISTS; it can
      never see that a rule FIRES — and two rules (`#declarations`, `#cast`) were present, correct and
      UNREACHABLE, because TextMate breaks a same-position tie in favour of the earlier include and
      `#keywords` matched bare `type`/`cast` first. The visible cost was a contextual kind word with no
      scope at all. `tools/check-syntax.sh` runs the real vscode-textmate engine and layers three checks:
      committed `.snap` files (the `tests/*.expect` idiom, covering every character), `kama check` agreeing
      with every fixture, and the 13 findings asserted BY NAME so a careless `-u` re-bless cannot restore
      one silently.
    - **The compiler is the oracle, and it is not ceremony.** Writing the fixtures, `kama check` rejected
      six invented spellings that would each have shipped a confidently-wrong grammar assertion — among
      them `fn name() -> T` (kama has no `->`), a bare `ctor(…)`, and `${s.length()}`.
  - **B2 (`textDocument/semanticTokens/full`) ✅ SHIPPED `cf993d2`.** The layer that corrects what a regex
    cannot compute: the grammar guesses a capitalized word is a type, and only the resolver knows whether
    `Box` is a type, a local or a field. A read off the cached index — 10 requests interleaved into the
    keystroke loop add zero timing lines and no measurable wall clock — so `/full` only, with no range or
    delta variants to justify.
    - **Legend (decision of record), and its ORDER is the wire format** since a token's type travels as an
      index into it: `class, struct, interface, enum, enumMember, function, method, property, variable,
      parameter`, modifiers `[declaration]`. Only standard LSP names, because a theme styles what it knows.
      A kama `value` maps to **struct** and a `resource` to **class** — the same distinction the two kinds
      draw. Appending is safe; reordering silently recolours every buffer in every client.
    - **The facade returns kama coordinates and a `SymKind`; the server owns legend indices and delta
      encoding**, so `kamaPos`/`lspRange` remain the only two coordinate-conversion points.
    - **It does NOT filter prelude/std targets, unlike find-references.** Not owning a symbol is a good
      reason to refuse to RENAME it and a bad reason to refuse to COLOUR it.
  - **A full syntax-highlighting audit belongs here (user, 2026-07-27).** One pass over *every* highlight
    pattern so each editor renders kama faithfully — not just the keyword list `tools/check-syntax-drift.sh`
    already guards. Motivating evidence: an ad-hoc look at the numeric rules alone found the VS Code grammar
    disagreeing with the compiler **four** ways — it highlighted `42u32` (invalid; the suffix is `u?i…`),
    missed `42ui32` entirely, accepted `1_000` and `42f32` (both invalid), and had no rule at all for based
    literals `0b1010_2`. So the editor was colouring broken code as valid and valid code as an identifier.
    Fixed in `139fb10`, with the drift guard extended to numeric suffixes. **Assume the same class of error
    in the rules not yet audited** — strings + escapes + `${}` interpolation holes, char literals
    (`'\u{E9}'`, multibyte), attributes (`@generate`, `@compileFor`), `asm(...)` blocks, contextual kind
    words, turbofish, and comment nesting. Two things worth doing while there: (a) push each rule through a
    compiler-as-oracle table like the numeric one (write the literal, ask `kama check`, compare to the
    grammar regex) rather than eyeballing; (b) consider **semantic tokens** (`textDocument/semanticTokens`)
    once the index is rich enough — the LSP can then colour a local differently from a field or a type,
    which no regex grammar can do, and every client gets it at once.
  - **⚠️ Xcode is out of scope (no supported path).** Xcode exposes **no** hook to register a third-party
    LSP server — its editor intelligence (SourceKit-LSP) is wired for Swift/C/C++/ObjC only, and Source
    Editor Extensions can do only menu-triggered text transforms (no live diagnostics/hover/def/completion).
    The most one could add is fragile, unsupported syntax highlighting via private language-spec hacks. For
    a macOS-native, full-featured kama editing experience, the answer is **Zed** (native, first-class LSP)
    or **VS Code**, not Xcode. Revisit only if Apple ever opens a generic-LSP plug-in surface.

**Overall gauge: the largest post-1.0 campaign to date** — on the order of, or larger than, the concurrency
campaign; **multiple sessions across M0-M6**, not one sitting. The cost is front-loaded in **M0** (the
library/query refactor) and back-loaded in **M4/M5** (completion quality + recovery). A **walking skeleton**
(M0→M1: real as-you-type diagnostics in VSCode) is the first satisfying checkpoint and de-risks the rest.

## Risks (ranked)

1. **M0 decoupling** — semantic analysis is entangled with emission across 14.7 K lines; teasing out a
   side-effect-free query pass without regressing `kama build` is the central risk. Mitigate: keep the emit
   path intact, build the query pass alongside, diff behavior.
2. **Error recovery (decision 2)** — Bison's no-recovery model vs. the LSP's need for partial results on
   invalid buffers. Mitigate: v1 tolerates whole-file reparse + "last good AST" fallback; escalate to RDP only
   if needed.
3. **Incremental performance** — whole-file reparse+recheck per keystroke must stay sub-100 ms; likely fine
   given file sizes, but measure early.
4. **Scope creep** — completion/rename are deep; hold them to v2 behind a working diagnostics/hover/def v1.

## Suggested order

Settle decisions 1-4 → **M0** (library + query index + spans, `kama build` stays green) → **M1** (scaffold +
diagnostics = walking skeleton in VSCode) → **M2** (hover/def/symbols) → **M6** partial (client wiring so it's
usable) → **M3** (refs/rename) → **M4** (completion/sig-help) → **M5** (recovery/incremental hardening).

## Acceptance (v1)

In VSCode over the real extension: live diagnostics as you type, hover shows types, go-to-definition jumps,
outline populates — driven by the C++ server reusing the compiler front end, with `kama build` unregressed and
a stdio-driven test harness (`tools/check-lsp.sh`).
