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
// than file count. It bounds a GUESS. With no kama.json owning the file, "the project" is inferred as
// every .kama under a root, and when that inference is wrong — kama itself holds 870, 813 of them
// independent tests/ fixtures with their own `main` and colliding type names — they get analyzed as ONE
// program, the symbol tables collide, and rename would confidently rewrite the wrong file. So raising this
// number is not the fix for a large project; a kama.json whose `source` root CONTAINS the file is (which
// removes the cap entirely). Note the containment: a file beside `src/` rather than inside it is owned by
// nobody and stays on this capped path, which is deliberate.
// `KAMA_LSP_MAX_FILES` overrides it for a tree the user knows is one program; 0 means unlimited.
const size_t kLspMaxProjectFiles = 500;

// The project a file belongs to, plus every `.kama` source in it. `root` is empty when the file belongs to
// no project we're willing to index — the honest answer, and the server keeps refusing rename.
struct LspProject {
    std::string              root;               // project root directory ("" = no project)
    std::vector<std::string> files;              // every *.kama under root, absolute, sorted
    bool                     hasManifest = false;   // root was found via kama.json (vs. the editor's folder)
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
//   which files are in it          ->  `source`   (the one directory holding a project's files)
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

// The driver's path -> parsed-unit cache (M5.2). Every analysis re-reads and re-parses the whole
// transitive import closure from disk, and in a server that closure is stable while you type — measured
// at ~118 ms of a 203 ms per-keystroke analysis on a file importing std::process.
//
// Enabled for the lifetime of the server ONLY. A build parses each file once, so it would gain nothing,
// and a cached unit is rewritten in place by CEmitter::pruneInactiveDecls, which makes reuse sound only
// under a fixed build-flag set — see parseFile in kama.driver.cpp for the full invariant.
//
// `lspEvictParsedFile("")` drops everything, which is what workspace/didChangeWatchedFiles does: the
// notification may name a directory, and over-evicting costs one re-parse while under-evicting serves a
// stale AST.
void lspSetParseCache(bool on);
void lspEvictParsedFile(const std::string& path);

// Drop the cached `source` root of every manifest. Separate from the parse cache because it caches a
// DIFFERENT kind of answer, and a heavier one: not a key's value but "is this directory a package root
// at all", which decides whether module resolution falls back to a flat listing. A long-lived server
// that watched a kama.json appear, change or vanish would otherwise keep answering from the shape the
// tree had at startup. Call it wherever lspEvictParsedFile("") is called for a manifest change.
void lspEvictManifestCache();

// ---- build configuration (M6 A1) -------------------------------------------------------------------
//
// Which program is the editor looking at? Until M6 the answer was "not the one you are building":
// `lspAnalyze` never called `setBuildFlags`, so `_activeFlags` was empty, and `pruneInactiveDecls`
// dropped every `@compileFor(X)` declaration a real build keeps while keeping every `@compileFor(!X)`
// one. Any project using conditional compilation saw phantom "undeclared" errors.
//
// The configuration an editor analyzes under is DEFINED as what a plain `kama build` in that project
// does: the resolved TARGET's derived flags, BUILD_TYPE=DEBUG, and the manifest's `default: true` flags.
//
// THERE IS NO EDITOR-SPECIFIC OVERRIDE CHANNEL, BY DESIGN. `kama.local.json` is the one channel — a
// gitignored sibling of `kama.json` that every CLI path already deep-merges — so the editor and
// `kama build` cannot disagree, the F5 debug path needs no extra arguments to stay in step, and a
// Neovim user overrides configuration exactly the way a VS Code user does. That is also why
// `workspace/didChangeConfiguration` is deliberately NOT wired: the server reads no client settings, so
// handling it would be either dead code that reads as though configuration flowed through it, or a
// `workspace/configuration` pull the server has no machinery for.
// One single-select axis, as an editor's configuration picker needs it: what could be chosen, and what
// was. TARGET is one of these too — the picker treats every group identically, which is the whole point of
// the build-configuration campaign's one-primitive model.
struct LspSelectGroup {
    std::string name;                    // "TARGET", "BUILD_TYPE", "OUTPUT", or a project's own
    std::vector<std::string> values;     // declaration order — `SelectGroup::values` was written for this
    std::string selected;                // the winning value ("" = the group has no default and none won)
};

struct LspBuildConfig {
    std::string manifest;        // the kama.json that was read ("" = none: permissive host defaults)
    std::string localManifest;   // the kama.local.json merged over it ("" = none)
    std::string targetName;      // e.g. "HOST"
    std::string targetTriple;    // e.g. "aarch64-macos-none"
    std::string buildType;       // e.g. "DEBUG"
    std::vector<std::string> activeFlags;   // the full `@compileFor` set, sorted
    bool        strict = false;  // a manifest declared the flag universe, so typos are errors
    // What the editor could switch TO, alongside what is in force. Carried here rather than left for a
    // client to work out from kama.json: the catalog is the built-in target list plus the built-in groups
    // plus whatever the manifest declared, and a client re-deriving that union would drift from the
    // compiler the first time either side gained a value (M6 C1).
    std::vector<LspSelectGroup> groups;
};

// Resolve and INSTALL the configuration this process analyzes under. Discovery mirrors the CLI's, in the
// direction an editor needs: walk UP from `hintPath`'s directory looking for a kama.json, never above
// `workspaceRoot`, stopping at any `.kama` component so a vendored dependency keeps its own manifest.
// `hintPath` empty installs permissive host defaults without touching the filesystem.
//
// The NEAREST manifest wins, not the outermost — deliberately different from lspFindProject's ownership
// walk. `main` discovers the input file's own kama.json first, so nearest is what makes "the editor
// agrees with `kama build <this file>`" literally true; pinning a monorepo's root manifest instead would
// put a member package's own flag names outside the declared universe and manufacture "undeclared flag"
// errors on correct code.
//
// PER PROCESS, and that is load-bearing: the M5 parse cache holds units that `pruneInactiveDecls`
// rewrote IN PLACE, so reuse is sound only under a fixed flag set (see parseFile in kama.driver.cpp).
// Any re-resolve MUST be followed by `lspEvictParsedFile("")` and a re-analysis of every open document.
//
// Returns false + `err` on a malformed manifest / unknown target / undeclared flag. On failure the
// configuration is left PERMISSIVE rather than half-applied, so the server keeps answering.
bool lspResolveBuildConfig(const std::string& hintPath, const std::string& workspaceRoot,
                           LspBuildConfig& out, std::string& err);

// Is `path` a file lspResolveBuildConfig reads? The server asks so workspace/didChangeWatchedFiles can
// tell a CONFIGURATION change from a source change — the manifest file names stay knowledge of the
// driver, which owns the manifest format.
bool lspIsManifestPath(const std::string& path);

// Was this index built from a buffer that did NOT fully parse (M5.4)? Since M5.3 the grammar recovers,
// so a mid-edit buffer still yields a live, current index off the parts that survived — which is what
// makes completion on a broken file work off a fresh index rather than a stale one. The server asks in
// order to decide whether the M4.6 line-blanking repair is still worth running.
bool lspIndexIsPartial(const SharedLspIndex& idx);

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
// Every DECLARATION a rename at the cursor would rewrite: the symbol itself plus its rename group — a
// contract method and its implementations are one name (M6 B3c). The rename path must check ownership on
// ALL of them, since a group can straddle the project boundary (an impl here, the contract in std). A
// Location with an empty uri is a def-site the index owns no file for, i.e. never project-owned.
std::vector<Location>   lspRenameDeclarations(const SharedLspIndex& idx, const std::string& path,
                                              int line, int col);
// prepareRename (M3): the identifier range at the cursor if it names a renameable user symbol, else a
// zero range (SrcRange::line == 0) — the server turns that into a null result so the editor greys out F2.
SrcRange                lspPrepareRename(const SharedLspIndex& idx, const std::string& path, int line, int col);
// semanticTokens/full (M6 B2): every resolver-classified position in one file, ascending and
// non-overlapping, in kama coordinates. The server owns the legend indices and the delta encoding — this
// stays framework-free. A read off the cached index, so it costs nothing beyond the analysis that already
// ran for diagnostics.
std::vector<SemanticToken> lspSemanticTokens(const SharedLspIndex& idx, const std::string& path);

// completion + signature help (M4). `ctx` is the LEXICAL context the server recovered from the LIVE buffer
// (completionContextAt), deliberately NOT a bare cursor position: at completion time the buffer does not
// parse, so the receiver the user just typed exists in no AST. Both use the PER-DOCUMENT index only —
// completion fires on every keystroke and must never trigger a workspace rebuild. The list is capped.
std::vector<CompletionItem> lspCompletion(const SharedLspIndex& idx, const std::string& path,
                                          const CompletionContext& ctx);
SignatureHelp               lspSignatureHelp(const SharedLspIndex& idx, const std::string& path,
                                             const CompletionContext& ctx);

// Import-path completion (M4.7). Both answer from the FILESYSTEM and the module resolver, not from the
// index: a module the user is about to import is by definition not loaded yet, so the query index cannot
// know it exists. They mirror loadProgramUnits' root order exactly (the file's own directory, KAMA_PATH,
// the resolved dependency view, then the stdlib — with `std`/`core` reserved to the stdlib), so what
// completes is what would actually resolve.
//   lspImportModules: the next path segment after `prefix` ("" = the top level).
//   lspImportSymbols: the names a module's `export { … }` manifest publishes.
std::vector<std::string> lspImportModules(const std::string& fromPath, const std::string& prefix,
                                          const char* argv0);
std::vector<std::string> lspImportSymbols(const std::string& fromPath, const std::string& modulePath,
                                          const char* argv0);

// ---- auto-import (the `codeActionProvider` quick fix) -----------------------------------------------
//
// Every `import` spelling that would bring `symbol` into scope from `fromPath`, best first: a SIBLING in
// this file's own module spelled bare (`Helper`), then each other module that exports the name, spelled
// qualified (`std::collections::DynamicArray`). Empty when nothing exports it.
//
// Answers from the module resolver and the filesystem, like the two above and for the same reason — the
// module a quick fix is about to import is one the file does not import, so no index holds it. Costs a
// parse of each candidate module's files; a per-GESTURE cost, never per keystroke, and the server keeps
// the parse cache open across them. Capped.
std::vector<std::string> lspImportCandidates(const std::string& fromPath, const std::string& symbol,
                                             const char* argv0);

// Where a new import goes in `path`, which is one place because kama has exactly ONE `import { … };`
// block per file and it is always at the top — two blocks do not parse, and neither does one below a
// declaration. So a quick fix inserts into one known position instead of choosing among directives and
// guessing an ordering convention.
//
// `hasBlock` true  -> `at` is the FIRST entry's start; insert `"<spelling>, "` there (inserting at the
//                     head means never having to find the closing brace).
// `hasBlock` false -> `at` is column 0 of the line a whole new block goes on; insert
//                     `"import { <spelling> };\n"`.
// `at.line == 0`   -> no answer (the index holds no unit for this path).
struct LspImportInsertion {
    SrcRange at;
    bool     hasBlock = false;
};
LspImportInsertion lspImportInsertion(const SharedLspIndex& idx, const std::string& path);

// Run the language server over stdio; blocks until the client's `exit`. `argv0` is the compiler's own path
// (for resolving the stdlib when loading imported modules). Returns the process exit code (0 after a clean
// shutdown→exit handshake, 1 if `exit` arrives without a prior `shutdown`).
int runLspServer(const char* argv0);

#endif // __KAMA_LSP_H__
