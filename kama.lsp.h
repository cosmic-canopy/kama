#ifndef __KAMA_LSP_H__
#define __KAMA_LSP_H__

#include <string>
#include <vector>
#include <memory>
#include <utility>
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
// CompilationUnit and every diagnostic's `file`. Imported modules are loaded from disk (resolved relative
// to `argv0`'s stdlib, like `kama check`) so cross-module names resolve — the live buffer is substituted
// for the open file's on-disk copy so unsaved edits still analyze; only the open file's diagnostics are
// returned. Parse errors survive a failed parse (an as-you-type buffer is usually mid-edit); on a parse
// failure the handle is nullptr (diags still carry the errors) — the server keeps its last good handle so
// hover/def don't go dark while the buffer won't parse.
SharedLspIndex lspAnalyze(const std::string& path, const std::string& text,
                          std::vector<Diagnostic>& diags, const char* argv0);

// ---- workspace indexing (M3.5) ---------------------------------------------------------------------
//
// lspAnalyze's unit set is REACHABILITY FROM THE OPEN FILE: the file plus its transitive imports. A file
// that imports THIS symbol without being imported back is invisible to it, which is why M3.3 had to refuse
// cross-file rename rather than half-rewrite. A workspace index replaces that with a PROJECT-WIDE unit set,
// so find-references sees every user of a symbol and rename can safely rewrite them all.
//
// The enumeration cap — and note what it is actually for. It is NOT a resource limit: 500 trivial files
// analyze in ~0.3s, faster than 43 real stdlib files, because cost tracks content and import depth rather
// than file count. It bounds a GUESS. Absent a `sources` declaration, "the project" is inferred as every
// .kama under a root, and when that inference is wrong — cstar itself holds 870, 813 of them independent
// tests/ fixtures with their own `main` and colliding type names — they get analyzed as ONE program, the
// symbol tables collide, and rename would confidently rewrite the wrong file. So raising this number is
// not the fix for a large project; declaring `sources` in kama.json is (which removes the cap entirely).
// `KAMA_LSP_MAX_FILES` overrides it for a tree the user knows is one program; 0 means unlimited.
const size_t kLspMaxProjectFiles = 500;

// The project a file belongs to, plus every `.kama` source in it. `root` is empty when the file belongs to
// no project we're willing to index — the honest answer, and the server keeps refusing rename.
struct LspProject {
    std::string              root;               // project root directory ("" = no project)
    std::vector<std::string> files;              // every *.kama under root, absolute, sorted
    bool                     hasManifest = false;   // root was found via kama.json (vs. the editor's folder)
    bool                     declaredSources = false;  // kama.json listed `sources` — no guessing, no cap
    bool                     tooLarge    = false;   // enumeration blew the cap; `files` is empty
    size_t                   seenCount   = 0;       // how many were seen before the cap (for the message)
    size_t                   cap         = 0;       // the cap actually in force (KAMA_LSP_MAX_FILES honored)
};

// Resolve the project owning `openFilePath`. `workspaceRoot` is the editor's folder (from initialize's
// rootUri / workspaceFolders), or "" if the client sent none.
//
// Walk UP from the file recording every directory holding a kama.json (never above `workspaceRoot`, which
// it may examine; stopping at any `.kama` component so a vendored dependency keeps its own manifest).
// Finding a manifest that way is not a guess — it is discovery of a declared fact, the same walk
// cargo/npm/tsc do. Two things ARE guesses, and each has a manifest key that settles it:
//
//   which manifest owns this file  ->  `projects` (a manifest naming its sub-projects, recursively)
//   which files are in it          ->  `sources`  (a package naming its own files)
//
// DECLARED first: if an ancestor manifest's expanded tree actually CONTAINS this file, that manifest is
// the project — outermost such wins (the top of a nest of monorepos), and no editor boundary is needed to
// license it, because nothing is being inferred. The file set is exactly what was declared, uncapped.
//
// INFERRED otherwise: under `workspaceRoot` the outermost manifest wins (over-indexing is the safe
// direction — under-indexing is what silently rewrites a caller we never saw); outside it, only the
// nearest is defensible, since widening could otherwise swallow a stray kama.json in $HOME. With no
// manifest at all the root falls back to `workspaceRoot`. In every inferred case the file set is every
// .kama under the root, bounded by the cap above.
LspProject lspFindProject(const std::string& openFilePath, const std::string& workspaceRoot);

// realpath(): symlinks and `..` collapsed. The LSP layer needs it because module resolution hands back
// paths RELATIVE to the compiler binary — the stdlib arrives as `<exeDir>/../../lib/std/…`, which in a
// dev tree still carries the project root as a literal string prefix. A rename deciding "is this
// definition inside the project?" by string prefix would then treat a std symbol as the project's own and
// happily rewrite it. Normalize both sides before comparing. Falls back to the input if it doesn't exist.
std::string lspRealPath(const std::string& path);

// Analyze a whole project: `files` (every project source) plus their transitive imports, with `overlays`
// — (path, live buffer text) for each open document — substituted for their on-disk copies so unsaved
// edits are reflected. Diagnostics are deliberately NOT returned: the per-document index still owns
// diagnostics/hover/go-to-def, and this index exists only for the queries that need reverse reachability.
SharedLspIndex lspAnalyzeWorkspace(const std::vector<std::string>& files,
                                   const std::vector<std::pair<std::string, std::string>>& overlays,
                                   const char* argv0);

// workspace/symbol: project-wide symbol search. `query` is a case-insensitive substring ("" matches all);
// only symbols declared in one of `files` are returned, so std, dependencies and anything else the index
// happens to hold stay out of the picker. Capped.
std::vector<SymbolInfo> lspWorkspaceSymbols(const SharedLspIndex& idx, const std::string& query,
                                            const std::vector<std::string>& files);

// Query seams — thin wrappers over the query facade on a handle (the path re-picks the unit within the
// index). All framework-free (kama.query.h value types); a null handle yields an empty/unknown result.
std::vector<SymbolInfo> lspDocumentSymbols(const SharedLspIndex& idx, const std::string& path);
Location                lspDefinition(const SharedLspIndex& idx, const std::string& path, int line, int col);
std::string             lspHover(const SharedLspIndex& idx, const std::string& path, int line, int col);
// find-references (M3). Results may name OTHER files: the index spans the open file's transitive imports,
// so each Location carries its own path — map it through pathToUri rather than assuming the open document.
std::vector<Location>   lspReferences(const SharedLspIndex& idx, const std::string& path,
                                      int line, int col, bool includeDecl);
// prepareRename (M3): the identifier range at the cursor if it names a renameable user symbol, else a
// zero range (SrcRange::line == 0) — the server turns that into a null result so the editor greys out F2.
SrcRange                lspPrepareRename(const SharedLspIndex& idx, const std::string& path, int line, int col);

// Run the language server over stdio; blocks until the client's `exit`. `argv0` is the compiler's own path
// (for resolving the stdlib when loading imported modules). Returns the process exit code (0 after a clean
// shutdown→exit handshake, 1 if `exit` arrives without a prior `shutdown`).
int runLspServer(const char* argv0);

#endif // __KAMA_LSP_H__
