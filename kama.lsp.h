#ifndef __KAMA_LSP_H__
#define __KAMA_LSP_H__

#include <string>
#include <vector>
#include "kama.forward.h"
#include "kama.diagnostic.h"

// The `kama lsp` language server (LSP campaign M1 — the walking skeleton). A JSON-RPC 2.0 server over
// stdio that reuses the M0 front-end-as-library analysis path (CEmitter::analyze + structured
// Diagnostics) to publish live, as-you-type diagnostics to any LSP editor. Opt-in subcommand — a dead
// branch unless `kama lsp` is invoked, so `kama build` pays nothing for it. Hover/def/completion are M2+
// (the query facade in kama.query.h is already built and waiting).
//
// The parse/prelude/analysis plumbing lives in kama.driver.cpp (which owns the flex/bison front end), so
// the driver exposes ONE external seam — `lspAnalyzeBuffer` — and the LSP module (kama.lsp.cpp) owns the
// editor-facing half: JSON, the stdio transport, the dispatch loop, and coordinate mapping.

// Analyze one in-memory buffer through the front-end-as-library path and return the merged parse +
// semantic diagnostics in kama coordinates (line 1-based, column 0-based). `path` is the buffer's source
// path (from its file:// URI); it names the CompilationUnit and every diagnostic's `file`. Parse errors
// survive a failed parse (an as-you-type buffer is usually mid-edit); the last cleanly-parsed unit is
// cached into `lastGood` (M2 queries read it so the file doesn't go dark on a parse error).
std::vector<Diagnostic> lspAnalyzeBuffer(const std::string& path, const std::string& text,
                                         SharedCompilationUnit& lastGood);

// Run the language server over stdio; blocks until the client's `exit`. Returns the process exit code
// (0 after a clean shutdown→exit handshake, 1 if `exit` arrives without a prior `shutdown`).
int runLspServer();

#endif // __KAMA_LSP_H__
