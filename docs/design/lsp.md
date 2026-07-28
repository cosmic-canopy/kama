# Language Server (LSP) — campaign kickoff / handoff

**Status: M2 COMPLETE (2026-07-27, dev).** The **confirmed next-highest post-1.0 priority** (user, 2026-07-26;
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
- **NEXT: M3.5 — workspace indexing** (a project-wide unit set + `workspace/didChangeWatchedFiles`), which
  makes cross-file rename safe and lifts the M3.3 guard. Cold-start brief:
  [lsp-m3-kickoff.md](lsp-m3-kickoff.md) — which also carries the **campaign-exit checklist** of the
  remaining mid-action identifier productions whose spans are still whole-production.

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
   (a large sub-project — rewrites 1,643 lines of grammar, but ROADMAP flags it as the "right" long-term move
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
- **M5 — Robustness: error recovery + incremental/perf. `L` (or `XL` if RDP, decision 2).** Makes it feel good
  on broken/large files. Can be folded in earlier if decision 2 picks RDP up front.
- **M6 — Editor/IDE matrix + packaging + tests. `S/M`.** One `kama lsp` server, thin clients — wire up
  every editor with a generic LSP client and document each in `docs/editors.md`. `tools/check-lsp.sh`-style
  harness driving the server over stdio with fixture requests/responses.
  - **Target matrix (each = a few lines of config pointing at `kama lsp`):** **VS Code** (✅ done — the
    `editor/vscode/` client), **Neovim** (`nvim-lspconfig`), **Vim** (coc.nvim), **Emacs** (eglot /
    lsp-mode), **Sublime Text** (LSP package), **Helix**, **Zed**, **Kate**. Feature parity is automatic —
    all read the same server, so each gains hover/def/outline/refs/rename/completion as the server does.
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
