# LSP M1 — walking skeleton (server scaffold + live diagnostics) — kickoff / handoff

**Status: READY TO BUILD (cold-start brief).** M0 is complete (`ffa483c`; see [lsp.md](lsp.md) "Progress"
and the `next-lsp` memory). This is the self-contained handoff for M1 — the first demoable checkpoint:
**live as-you-type diagnostics in VS Code (and any LSP editor), driven by the C++ front end.** Read
[GOALS.md](../GOALS.md) (favor simplicity, few deps, one way to do a thing) before building.

## Goal & acceptance

Open a `.kama` file in VS Code → **diagnostics appear/clear live as you type**, produced by a `kama lsp`
server process that reuses the M0 analysis path. `kama build` stays unregressed. A `tools/check-lsp.sh`
harness drives the server over stdio with scripted JSON-RPC and asserts the responses. No hover/def/
completion yet — those are M2+ (the M0 query facade is already built and waiting).

**Why diagnostics-first:** biggest perceived win earliest, and it exercises the whole pipeline (transport
→ lifecycle → document sync → analysis → publish) end-to-end, de-risking M2–M6.

## What M0 already gives you (reuse inventory — all on `dev`)

- **Analysis entry:** `CEmitter idx(sourcePath); idx.setPrelude(preludeUnit()); idx.setNoHeap(...);
  idx.setRelease(...); idx.setBuildFlags(...); idx.setLogDefault(...); for (m : preludeModuleUnits())
  idx.addPreludeModule(m); idx.analyze(units);` — runs the full front end with **zero C emitted**. See the
  `kama check` subcommand (`kama.driver.cpp:3212`) — **copy its setup verbatim**; the `kama query` block
  right after it (`:3247`) is a second worked example that also reads the query facade.
- **Semantic diagnostics:** `idx.diagnostics()` → `const std::vector<Diagnostic>&` (structured
  `Diagnostic{line,col,endLine,endCol,severity,code,message,file}`, `kama.diagnostic.h`). Also
  `idx.diagnosticsFor(uri)` (filter by file).
- **Query facade (for M2, not M1):** `documentSymbols(uri)`, `definitionAt(uri,l,c)`,
  `typeAtPosition(uri,l,c)` — `kama.query.h` value types, framework-free, ready to map to JSON.
- **Parsing:** `parseFile(path)` / `parseString(src, name)` (`kama.driver.cpp:298/334`) →
  `SharedCompilationUnit`, **`nullptr` on any parse error** (see the gap below). `loadProgramUnits(...)`
  (`:220`) resolves + parses a file and all its transitive imports.
- **Editor:** `editor/vscode/` — `extension.js` (F5→build+CodeLLDB today, ~70 lines), `package.json`
  (declares the `kama` language + grammar + the debug command). Add a **language client** here; keep F5.
- **Coordinate convention (IMPORTANT):** kama positions are **line 1-based, column 0-based** (lexer:
  `KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE=1`, `..._COLUMN_ONE=0`). **LSP is line 0-based AND character
  0-based.** So the server must map `lspLine = kamaLine - 1`, `lspChar = kamaCol` (columns already 0-based —
  verify against the `check-query.sh` positions, which are raw kama coords). Do the conversion in ONE place
  in the server layer; keep the C++ facade in kama coords.

## The one real gap M1 must fill: parse diagnostics survive failure

`parseFile`/`parseString` build a local `CodeGenContext` whose `.diagnostics` vector accumulates parse
errors (via `handleError`, `kama.context.h`), but on `rc != 0 || errorCount() > 0` they **return `nullptr`
and destroy the context** (`kama.driver.cpp:323-324, 348`). For as-you-type editing (the buffer is usually
mid-edit and invalid) the server must still show the syntax error and not go dark.

**Add a parse entry that returns diagnostics even on failure**, e.g.:
```cpp
// Parse a buffer for the LSP: always returns the CodeGenContext (so parse diagnostics are readable) and
// the CompilationUnit when it parsed (nullptr if not). Mirrors parseString but keeps the context alive.
struct ParseResult { SharedCompilationUnit unit; SharedCodeGenContext ctx; };
ParseResult parseForQuery(const char* src, const std::string& name);
```
Then in the server's document handler: parse the buffer → collect `ctx->diagnostics` (parse errors) →
if `unit` is non-null, `analyze({unit})` and append `idx.diagnostics()` (semantic) → publish the merged
set. **Last-good-AST fallback:** cache the last `unit` that parsed for each open doc; on a parse failure,
publish the parse diagnostic(s) but keep serving the last good unit for anything else (M2 queries) so the
file doesn't go dark. (M1 only needs the parse-diag half; the cache is cheap to add now.)

## Decisions to settle FIRST

1. **JSON handling (the settle-first call).** LSP is JSON-RPC 2.0 — nested request/response objects both
   directions, with string escaping. The repo has NO general JSON lib; `ManifestReader`
   (`kama.driver.cpp:630`) is a *tolerant one-way* kama.json reader, not a full parser, and the kama-level
   `lib/std/serialization/json` is runtime code (not usable in the C++ driver). *Lean:* **a small,
   self-contained JSON reader+writer in the LSP module** (~200-300 lines: parse object/array/string/number/
   bool/null; serialize the same) — matches kama's few-deps ethos and needs only the subset LSP uses.
   Alternative: vendor a single-header lib (e.g. a minimal `json.hpp`) if hand-rolling feels heavy. Decide
   before writing the transport.
2. **Where the server lives.** *Settled (M0):* a **`kama lsp` subcommand** in `kama.driver.cpp` (like
   `kama check`/`query`/`pkg`), not a separate binary — opt-in, zero cost to `kama build` (a dead branch
   unless invoked). New TU `kama.lsp.cpp` (+ `kama.lsp.h`) for the server loop, added to the Makefile
   OBJECTS next to `kama.query.o`. Escape hatch if bulk ever matters: split to a `kama-lsp` binary later.
3. **Document sync granularity.** *Lean:* **full-document sync** for v1 (`TextDocumentSyncKind.Full` in
   the `initialize` capabilities) — the client sends the whole buffer on each change; kama files are small,
   whole-file reparse is milliseconds. Incremental sync is M5.

## Build plan (staged; `make && ./run_tests.sh` green after each)

- **M1.1 — JSON + transport.** The chosen JSON reader/writer, and a stdio loop reading LSP framing
  (`Content-Length: N\r\n\r\n<N bytes>`), dispatching by `method`, writing framed responses. Pure I/O; unit-
  testable without the compiler.
- **M1.2 — Lifecycle.** `initialize` (respond with `capabilities`: `textDocumentSync: 1` (Full),
  everything else off for M1) / `initialized` (notification, no-op) / `shutdown` / `exit`. Track the
  initialized/shutdown state; reject requests before initialize.
- **M1.3 — Document store + sync.** In-memory `map<uri, {text, version, lastGoodUnit}>`. Handle
  `textDocument/didOpen`, `didChange` (replace full text), `didClose` (drop). URI↔path: parse `file://`
  URIs to a filesystem path for `sourcePath`/import resolution; keep the URI for `publishDiagnostics`.
- **M1.4 — Diagnostics pipeline.** On open/change: `parseForQuery(text)` → merge parse diagnostics
  (`ctx->diagnostics`) + (if parsed) `analyze` semantic diagnostics → convert kama coords → LSP ranges →
  send `textDocument/publishDiagnostics {uri, diagnostics[]}`. On a clean buffer, publish an empty array
  (clears old squiggles). Map `DiagSeverity` → LSP severity (Error=1, Warning=2, Information=3, Hint=4).
  **Single-file first**; a buffer that `import`s can reuse `loadProgramUnits` against the on-disk deps +
  the in-memory main buffer (a later refinement — v1 can analyze the open file alone).
- **M1.5 — VS Code language client.** Add `vscode-languageclient` to `editor/vscode/package.json` deps;
  in `extension.js`, spawn `kama lsp` over stdio (reuse `findKama()`), register a `LanguageClient` for the
  `kama` language, `start()` it in `activate`, `stop()` in `deactivate`. Keep the F5 debug command intact.
  (`npm install` + `vsce package` notes in `editor/vscode/README.md`.)
- **M1.6 — Harness.** `tools/check-lsp.sh`: pipe a scripted JSON-RPC session (initialize → didOpen a
  fixture with a known error → expect a `publishDiagnostics` with that range → didChange to fix it → expect
  an empty diagnostics array → shutdown/exit) into `kama lsp` over stdio; assert on the framed responses.
  Mirror `tools/check-query.sh` style; wire into `run_tests.sh` (native leg, `KAMA_SAN`/`KAMA_WASM`-gated
  off like the others). Fixtures under `tests/lsp/` (a dir the main loop doesn't scan).

## Reuse map (files to touch)

- `kama.driver.cpp` — new `if (subcommand == "lsp")` dispatch near `:3247`; the new `parseForQuery`
  near `parseString` (`:334`); copy the `check` setup (`:3212`).
- `kama.lsp.{h,cpp}` (new) — JSON reader/writer, transport loop, lifecycle, doc store, diagnostics
  mapping. Add `kama.lsp.o` to the Makefile OBJECTS + `kama.lsp.h` to HEADERS.
- `kama.query.h` / `kama.cemit.h` — already expose everything M1 reads (`diagnostics()`,
  `diagnosticsFor`). Nothing to add for M1 (M2 uses the query facade).
- `editor/vscode/{package.json,extension.js}` — language client.
- `tools/check-lsp.sh` + `tests/lsp/` (new).

## Risks

1. **JSON correctness** (escaping, UTF-8, nested params) — keep the reader small but correct; the harness
   catches regressions. 2. **Incremental perf** — full reparse per keystroke; measure, but kama files are
   small (M5 hardens). 3. **`analyze()` re-entrancy** — each change makes a FRESH `CEmitter` (analysis is
   not designed to be re-run on one instance); construct-per-analysis, cheap. 4. **Import resolution for an
   unsaved buffer** — v1 analyzes the open file alone; multi-file live analysis is a refinement.

## After M1

**M2** — hover + go-to-definition + document symbols, wired straight to the M0 facade
(`definitionAt`/`typeAtPosition`/`documentSymbols`) + capabilities flipped on in `initialize`. Then **M6**
broadened to **editor clients + `docs/editors.md`** (Neovim `nvim-lspconfig`, Vim, **Emacs eglot**
snippets) — the user wants all major IDEs ASAP, and one server means each is a few lines of client glue.
Keep the JSON/transport layer editor-agnostic (it already is).
