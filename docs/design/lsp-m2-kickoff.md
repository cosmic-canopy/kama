# LSP M2 — hover + go-to-definition + document symbols — kickoff / handoff

**Status: READY TO BUILD (cold-start brief).** M0 (query index + `%locations` spans) and M1 (server
scaffold + live diagnostics) are complete on `dev` (`4a74dfe`, `e74a0bd`; see [lsp.md](lsp.md) "Progress"
and the `next-lsp` memory). This is the self-contained handoff for **M2 — the first *interactive*
language features**: hover, go-to-definition, and the document outline, wired straight to the M0 query
facade that's already built and waiting. Read [GOALS.md](../GOALS.md) (favor simplicity, few deps, one
way to do a thing) before building.

## Goal & acceptance

In VS Code (and any LSP editor): **hover** over a decl name or type reference shows its kind+name;
**go-to-definition** (F12 / Ctrl-click) on a type reference jumps to the declaration; the **outline**
(breadcrumbs + Ctrl-Shift-O) lists every user decl with its kind and accurate name range. Diagnostics
(M1) stay live and unregressed. A `tools/check-lsp.sh` case per feature asserts the responses over
stdio. Everything reuses the M0 facade — **M2 is mostly protocol glue + one lifetime change** (below).

## The one real structural change M2 must make: keep the analyzed index alive

M1's `lspAnalyzeBuffer` (kama.driver.cpp, global scope after the anon namespace) builds a **fresh
`CEmitter` per change, collects `diagnostics()`, and throws it away**. But the query methods are
**instance methods that read the index built at the tail of `analyze()`**:

```cpp
std::vector<SymbolInfo> documentSymbols(const std::string& uri) const;    // reads _defSites/_units
Location                definitionAt(const std::string& uri, int l, int c); // NON-const: replays resolution, mutates _nsCtx (save/restored)
std::string             typeAtPosition(const std::string& uri, int l, int c); // NON-const, same
```
(all in `kama.cemit.h:452-455`, framework-free `kama.query.h` return types.)

So to answer hover/def/outline the server needs a **live, analyzed `CEmitter`** for the document — not
just its diagnostics. Two ways:

- **(A) Re-analyze per query** — a stateless seam `lspHoverAt(path, text, l, c)` that re-parses +
  re-analyzes internally, then calls the facade. Simplest, matches M1's per-keystroke analyze, keeps
  `CEmitter` fully inside the driver TU. **Downside:** re-parses the prelude on *every hover/mouse-move*
  (hover fires far more often than keystrokes). Wasteful but correct for small files.
- **(B) Cache a persistent index handle per document** — analyze once per change, hold the analyzed
  `CEmitter` behind an opaque handle, answer queries as instant reads. Also gives **last-good-index**
  behavior for free (keep the previous handle on a parse failure → hover/def keep working while the
  buffer is mid-edit and won't parse). **Downside:** slightly more plumbing (a handle type + query-seam
  wrappers).

**Lean: (B).** Hover/def fire on every cursor move; re-analyzing each time is the wrong default, and (B)
is the clean way to deliver queries-during-parse-error (which editors expect). Keep `CEmitter` **inside
the driver TU** — expose an *opaque* handle so the LSP module still only sees `kama.query.h` value types
+ `kama.lsp.h` seams (preserves the M1 layering: LSP module owns the editor half, driver owns the
compiler half). Recommended shape:

```cpp
// kama.lsp.h — opaque; the driver defines the body (holds shared_ptr<CEmitter> + the owning path).
struct LspIndex;                                   // forward-declared; server holds a std::shared_ptr<LspIndex>
using SharedLspIndex = std::shared_ptr<LspIndex>;

// Analyze a buffer and return BOTH the diagnostics (as M1) AND a queryable index handle (nullptr on a
// parse failure — diags are still filled with the parse errors). Supersedes/absorbs lspAnalyzeBuffer.
SharedLspIndex lspAnalyze(const std::string& path, const std::string& text, std::vector<Diagnostic>& diags);

// Query seams — thin wrappers over the facade on a handle (path re-passed to pick the unit within the index).
std::vector<SymbolInfo> lspDocumentSymbols(const SharedLspIndex&, const std::string& path);
Location                lspDefinition(const SharedLspIndex&, const std::string& path, int line, int col);
std::string             lspHover(const SharedLspIndex&, const std::string& path, int line, int col);
```
The driver defines `struct LspIndex { std::shared_ptr<CEmitter> idx; std::string path; }` and the seams
just forward to `idx->documentSymbols(path)` / `definitionAt` / `typeAtPosition`. `CEmitter` is safe to
hold alive (it owns its units as `SharedCompilationUnit`; analysis is single-threaded, so the
`definitionAt` `_nsCtx` save/restore is fine). Refactor M1's `Doc` to cache `SharedLspIndex lastGoodIndex`
alongside (or instead of) `lastGoodUnit`; on a clean parse replace it, on a parse failure keep it.

## What M0/M1 already give you (reuse inventory — all on `dev`)

- **The facade (M0):** `documentSymbols`/`definitionAt`/`typeAtPosition` return `SymbolInfo` /
  `Location` / `std::string` (`kama.query.h`). `SymbolInfo{name, kind (SymKind), range, selectionRange,
  container}`; `Location{uri, range}` where `uri` is the unit's path (`== *unit->name`, i.e. the `path`
  you passed) and `range` is in **kama coords**; `SrcRange{line,column,endLine,endColumn}`. `SymKind`
  enum + `symKindName()` (`kama.query.h:21-23`).
- **Worked call examples:** the `kama query` subcommand (`kama.driver.cpp:3252-3303`) already calls all
  three and prints them — copy its position parsing + facade calls. `tools/check-query.sh` pins exact
  positions against `tests/query/shapes.kama` (a stable fixture) — reuse those coords in the LSP harness.
- **M1 server (kama.lsp.cpp):** the JSON reader/writer, the transport, the dispatch loop, the per-doc
  store, and — critically — **`rangeToJson` (the ONE coord-conversion site: kama line-1/col-0 → LSP
  0/0)**. Every M2 result reuses it. Add `hover`/`definition`/`documentSymbol` cases to `dispatch()`.
- **Capabilities:** M1's `handleInitialize` advertises only `textDocumentSync: 1`. M2 flips on
  `hoverProvider: true`, `definitionProvider: true`, `documentSymbolProvider: true`.

## Coordinate convention (unchanged, still the easy-to-miss bit)

kama = **line 1-based, column 0-based**; LSP = **line 0-based, character 0-based**. Positions arrive from
the client in LSP coords → **convert to kama before calling the facade** (`kamaLine = lspLine + 1`,
`kamaCol = lspChar`), and convert results back via `rangeToJson`. The facade is entirely in kama coords.
The `kama query`/`check-query.sh` positions are raw kama coords — a useful cross-check.

## Build plan (staged; `tools/cdev make && tools/cdev test` green after each)

- **M2.1 — index handle refactor.** Introduce `LspIndex`/`SharedLspIndex` + `lspAnalyze` (absorb
  `lspAnalyzeBuffer`); cache the handle in the server's `Doc` (last-good on parse failure). Diagnostics
  path unchanged in behavior. Verify `check-lsp` still green.
- **M2.2 — document symbols.** Handle `textDocument/documentSymbol` → `lspDocumentSymbols(handle, path)`
  → `DocumentSymbol[]` (flat: each `{name, kind, range, selectionRange}`, no `children` for v1 —
  container nesting is a refinement). Map `SymKind` → LSP `SymbolKind` (Class/Value/Resource→Class 5 or
  Struct 23; Contract→Interface 11; Enum→Enum 10; EnumMember→EnumMember 22; Function/GenericFn→Function
  12; Method→Method 6; Ctor→Constructor 9; Field→Field 8; GenericType→Class 5). Flip on the capability.
- **M2.3 — go-to-definition.** Handle `textDocument/definition` → convert LSP pos → kama →
  `lspDefinition(handle, path, l, c)` → `Location{uri,range}` or `null`. Map `uri` (a path) → `file://`
  URI (add a `pathToUri` helper — the inverse of M1's `uriToPath`) and `range` via `rangeToJson`.
- **M2.4 — hover.** Handle `textDocument/hover` → `lspHover(handle, path, l, c)` → LSP `Hover`
  `{contents: {kind:"plaintext", value:"<kind> <name>"}}` (or `null` when the string is empty). A range
  is optional; skip it for v1.
- **M2.5 — harness + capabilities smoke.** Extend `tools/check-lsp.sh`: after `didOpen` of a fixture with
  known decls (reuse a `shapes.kama`-style buffer, or point at `tests/query/shapes.kama` positions),
  send `documentSymbol` / `definition` / `hover` requests and assert the framed responses (a symbol with
  its name range; a definition Location at the decl; a hover string). Assert `initialize` now advertises
  the three providers.

## Reuse map (files to touch)

- `kama.driver.cpp` — `struct LspIndex` + `lspAnalyze` + the three query seams (global scope, next to
  `lspAnalyzeBuffer`, which they absorb). ⚠️ **The driver's helpers are in an anonymous namespace
  (internal linkage) — every LSP-facing function MUST be defined at GLOBAL scope after `} // namespace`
  (line ~2882), exactly as `lspAnalyzeBuffer` is.** This is the single biggest gotcha; see the M1 memory.
- `kama.lsp.h` — the handle type + query-seam declarations.
- `kama.lsp.cpp` — `dispatch()` cases for the three requests; `handleInitialize` capability flags;
  `pathToUri`; the `SymKind`→LSP-`SymbolKind` map; result→JSON builders (reusing `rangeToJson`).
- `tools/check-lsp.sh` — new assertion cases.
- **Unchanged:** `kama.query.h`, `kama.cemit.h` (the facade already exposes everything M2 reads).

## Risks / notes

1. **Query-during-parse-error** — handled by keeping the last-good index (design (B)). Without it, hover
   on a mid-edit buffer goes dark. Worth doing in M2, not deferring.
2. **Perf** — (B) makes queries instant; analysis still runs once per change (M1's cost). Incremental is
   M5.
3. **Facade coverage gaps (from M0, expected):** fields + enum *members* have no def-site node yet (M0
   deferred them — the tables keep no per-field/member node), and body **use-sites** aren't indexed
   (that's M3 find-references). So go-to-def works on **signature/type references and decl names**, not
   yet on a variable use inside a function body or on a field access. Hover/def on those returns null for
   now — fine for M2; note it in the harness so it reads as intentional, not broken. Locals-hover is a
   later refinement.
4. **`definitionAt` is non-const** (mutates `_nsCtx`, save/restored) — that's why the handle holds a
   mutable `CEmitter`. Single-threaded dispatch makes this safe; don't share a handle across threads.

## After M2

**M3** — find-references + rename (the body use-site walk the M0 index deliberately deferred). **M4** —
completion + signature-help (delivers the `global::` floor-completion payoff). **M5** — error-recovery +
incremental/perf. **M6** — broadened to **editor clients + `docs/editors.md`** (Neovim `nvim-lspconfig`,
Vim, Emacs eglot snippets) — one server, so each editor is a few lines of client glue; the user wants all
major IDEs ASAP. Keep the JSON/transport layer editor-agnostic (it already is).
