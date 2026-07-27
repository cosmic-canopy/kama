#ifndef __KAMA_LSP_H__
#define __KAMA_LSP_H__

#include <string>
#include <vector>
#include <memory>
#include "kama.forward.h"
#include "kama.diagnostic.h"
#include "kama.query.h"     // SymbolInfo / Location value types (query-seam return types)

// The `kama lsp` language server (LSP campaign M1 — the walking skeleton). A JSON-RPC 2.0 server over
// stdio that reuses the M0 front-end-as-library analysis path (CEmitter::analyze + structured
// Diagnostics) to publish live, as-you-type diagnostics to any LSP editor. Opt-in subcommand — a dead
// branch unless `kama lsp` is invoked, so `kama build` pays nothing for it. Hover/def/completion are M2+
// (the query facade in kama.query.h is already built and waiting).
//
// The parse/prelude/analysis plumbing lives in kama.driver.cpp (which owns the flex/bison front end), so
// the driver exposes ONE external seam — `lspAnalyzeBuffer` — and the LSP module (kama.lsp.cpp) owns the
// editor-facing half: JSON, the stdio transport, the dispatch loop, and coordinate mapping.

// An opaque, queryable analysis handle (M2). The driver defines the body (it holds the live analyzed
// CEmitter + the owning path); the LSP module only ever holds it by shared_ptr and passes it back to the
// query seams below — so the compiler's CEmitter type never leaks into kama.lsp.cpp (preserving M1's
// layering: the LSP module owns the editor half, the driver owns the compiler half).
struct LspIndex;
using SharedLspIndex = std::shared_ptr<LspIndex>;

// Analyze one in-memory buffer through the front-end-as-library path (M2 — supersedes lspAnalyzeBuffer).
// Returns the merged parse + semantic diagnostics via `diags` (kama coords: line 1-based, column 0-based)
// AND a queryable index handle. `path` is the buffer's source path (from its file:// URI); it names the
// CompilationUnit and every diagnostic's `file`. Parse errors survive a failed parse (an as-you-type
// buffer is usually mid-edit); on a parse failure the handle is nullptr (diags still carry the errors) —
// the server keeps its last good handle so hover/def don't go dark while the buffer won't parse.
SharedLspIndex lspAnalyze(const std::string& path, const std::string& text, std::vector<Diagnostic>& diags);

// Query seams — thin wrappers over the query facade on a handle (the path re-picks the unit within the
// index). All framework-free (kama.query.h value types); a null handle yields an empty/unknown result.
std::vector<SymbolInfo> lspDocumentSymbols(const SharedLspIndex& idx, const std::string& path);
Location                lspDefinition(const SharedLspIndex& idx, const std::string& path, int line, int col);
std::string             lspHover(const SharedLspIndex& idx, const std::string& path, int line, int col);

// Run the language server over stdio; blocks until the client's `exit`. Returns the process exit code
// (0 after a clean shutdown→exit handshake, 1 if `exit` arrives without a prior `shutdown`).
int runLspServer();

#endif // __KAMA_LSP_H__
