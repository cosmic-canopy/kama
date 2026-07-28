// kama driver: parse kama source and transpile to portable C, optionally
// invoking a C compiler to produce a native executable.
//
//   kama transpile <in.kama> [-o out.c] [--no-line]
//   kama build     <in.kama> [-o out] [--target native|wasm|embedded] [--webgpu]
//                              [--cc <compiler>] [--no-line] [--keep-c]
//
// native builds invoke clang; wasm builds invoke emcc (Emscripten), keying the
// output format off the -o extension (.html harness by default); embedded builds
// invoke clang `-ffreestanding -nostdlib -c` to a bare-metal object (.o).
// (LLVM is gone; the backend is kama.cemit.*.)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>               // isdigit / isspace (SemVer range parsing)
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <map>
#include <deque>
#include <algorithm>

#include <limits.h>
#include <sys/stat.h>           // stat / S_ISDIR (directory check) — POSIX + mingw-w64 UCRT
#include <dirent.h>             // opendir / readdir (module directory listing) — POSIX + mingw-w64 UCRT
#ifdef _WIN32
  // NB: do NOT include <windows.h> here — it is compiled in the same TU as kama.parser.hpp, whose token
  // enum (BOOL, CHAR, CONST, INT8, VOID, …) collides with windows.h typedefs/macros. mingw-w64's POSIX
  // dirent/stat cover everything the driver needs, so windows.h is unnecessary.
  #include <stdlib.h>           // _fullpath, _MAX_PATH
  #include <direct.h>           // _mkdir (package view materialization)
  #include <process.h>          // _getpid (staging dir name for the package store)
  #ifndef PATH_MAX
    #define PATH_MAX _MAX_PATH
  #endif
#else
  #include <unistd.h>
  #include <sys/wait.h>         // WEXITSTATUS
  #ifdef __APPLE__
    // NB: do NOT include <mach-o/dyld.h> for _NSGetExecutablePath — like windows.h above, it is compiled in
    // the same TU as kama.parser.hpp, and its `enum DYLD_BOOL { FALSE, TRUE }` collides with the token
    // enum's FALSE/TRUE. Forward-declare the one symbol selfExePath() needs instead (as kama_runtime.h
    // does); extern "C" (namespace scope) links it to the real libSystem symbol.
    extern "C" int _NSGetExecutablePath(char*, unsigned int*);
  #endif
#endif

#include "kama.parser.hpp"
#include "kama.lexer.hpp"
#include "kama.context.h"
#include "kama.cemit.h"
#include "kama.prelude.h"   // KAMA_PRELUDE_SRC + KAMA_PRELUDE_MODULES (embedded built-in kama)
#include "kama.lsp.h"       // ParseResult/parseForQuery + runLspServer (the `kama lsp` server)

#ifndef KAMA_VERSION
#define KAMA_VERSION "0.0.0-dev"
#endif

namespace {

int runCmd(const std::string& cmd);   // fwd decl (defined below) — used by linkDir on Windows

std::string absolutePath(const std::string& path)
{
    char buf[PATH_MAX];
#ifdef _WIN32
    if (_fullpath(buf, path.c_str(), PATH_MAX)) return std::string(buf);
#else
    if (realpath(path.c_str(), buf)) return std::string(buf);
#endif
    return path; // fall back to as-given (e.g. file doesn't exist yet)
}

// Split on either separator so the same code works on Windows paths.
std::string dirName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

std::string baseName(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string stripExtension(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    size_t dot   = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path;
    return path.substr(0, dot);
}

bool fileExists(const std::string& p)
{
    std::ifstream f(p.c_str());
    return f.good();
}

// Where kama_runtime.h lives, resolved so an INSTALLED binary finds it from any
// cwd: <exeDir>/../include (bin/kama -> ../include), else <exeDir> (repo root
// layout), else <exeDir>/../.. (dev tree: the Makefile builds to build/<os>-<arch>/kama),
// else ".". Always exe-relative — so a per-version toolchain at
// ~/.kama/versions/<v>/bin/kama finds *its own* runtime, never an ambient one.
std::string resolveRuntimeDir(const char* argv0)
{
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
    if (fileExists(exeDir + "/../include/kama_runtime.h")) return exeDir + "/../include";
    if (fileExists(exeDir + "/kama_runtime.h"))            return exeDir;
    if (fileExists(exeDir + "/../../kama_runtime.h"))      return exeDir + "/../..";
    return ".";
}

bool dirExists(const std::string& p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);   // POSIX + mingw-w64
}

// The `*.kama` files directly inside `dir`, sorted for deterministic emit order.
std::vector<std::string> listKamaFiles(const std::string& dir)
{
    std::vector<std::string> out;
    auto keep = [&](const std::string& name) {
        return name.size() > 5 && name.compare(name.size() - 5, 5, ".kama") == 0;
    };
    if (DIR* d = opendir(dir.c_str())) {   // POSIX + mingw-w64 (wraps FindFirstFile internally on Windows)
        while (struct dirent* e = readdir(d)) { std::string n = e->d_name; if (keep(n)) out.push_back(dir + "/" + n); }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The stdlib root, resolved from the binary like resolveRuntimeDir: <exeDir>/../lib/kama
// (installed, bin/kama -> ../lib/kama), else <exeDir>/lib (repo root), else <exeDir>/../../lib
// (dev tree: the Makefile builds to build/<os>-<arch>/kama), else "lib".
// The stdlib ships INSIDE each install; `std::*` resolves here — exe-relative, so a
// per-version toolchain uses its own stdlib, never an ambient one.
std::string resolveStdlibDir(const char* argv0)
{
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
    if (dirExists(exeDir + "/../lib/kama"))  return exeDir + "/../lib/kama";
    if (dirExists(exeDir + "/lib"))          return exeDir + "/lib";
    if (dirExists(exeDir + "/../../lib/std")) return exeDir + "/../../lib";
    return "lib";
}

// The native WebGPU SDK root: wgpu-native's prebuilt drop (include/webgpu/{webgpu,wgpu}.h + a
// lib/libwgpu_native.* under it). Fetched on demand by tools/fetch-webgpu.sh into a gitignored dir;
// $KAMA_WGPU_DIR overrides. NOT vendored (multi-MB, MPL-2.0) — the repo stays lean and MIT: we only
// link the unmodified prebuilt, so its file-level copyleft never reaches our sources.
std::string resolveWgpuDir()
{
    if (const char* d = getenv("KAMA_WGPU_DIR")) return d;
    return "third_party/wgpu";
}

// The native C compiler. A "-bundled" install ships a static zig at
// <exe>/../libexec/zig/zig[.exe]; prefer it so `kama build` works with no system
// toolchain. Otherwise fall back to system clang. (--cc overrides both, upstream.)
std::string resolveCCompiler(const char* argv0)
{
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
#ifdef _WIN32
    std::string zig = exeDir + "/../libexec/zig/zig.exe";
#else
    std::string zig = exeDir + "/../libexec/zig/zig";
#endif
    if (fileExists(zig)) return "\"" + zig + "\" cc";
    return "clang";
}

// Split a `:`-separated search-path env (KAMA_PATH) into roots.
std::vector<std::string> splitSearchPath(const char* env)
{
    std::vector<std::string> out;
    if (!env) return out;
    std::string cur;
    for (const char* p = env; ; ++p) {
        if (*p == ':' || *p == '\0') { if (!cur.empty()) out.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
    return out;
}

// Resolve module segments (["std","memory"]) to source file(s) under the first matching
// root: a file-module (<root>/std/memory.kama) or every *.kama in a directory-module
// (<root>/std/memory/). Empty result => unresolved.
std::vector<std::string> resolveModuleFiles(const std::vector<std::string>& segs,
                                            const std::vector<std::string>& roots)
{
    std::string rel;
    for (size_t i = 0; i < segs.size(); ++i) rel += (i ? "/" : "") + segs[i];
    for (auto& root : roots) {
        std::string file = root + "/" + rel + ".kama";
        if (fileExists(file)) return { file };
        std::string dir = root + "/" + rel;
        if (dirExists(dir)) { auto fs = listKamaFiles(dir); if (!fs.empty()) return fs; }
    }
    return {};
}

SharedCompilationUnit parseFile(const std::string& inputFile);   // defined below
SharedCompilationUnit parseString(const char* src, const std::string& name);   // defined below

// The project's resolved-dependency view (`<projectDir>/.kama/deps`), or "" if there is no project
// manifest or the view has not been materialized. `kama install` populates it (package-name -> store
// or path source); the build only READS it — a pure, reproducible resolution with no fetch at build
// time. Discovered like the manifest: next to the first input, else the CWD.
std::string projectDepsView(const std::vector<std::string>& inputs, const char* leaf = ".kama/deps")
{
    std::string dir;
    if (!inputs.empty()) { std::string d = dirName(inputs[0]); if (fileExists(d + "/kama.json")) dir = d; }
    if (dir.empty() && fileExists("kama.json")) dir = ".";
    if (dir.empty()) return "";
    std::string view = dir + "/" + leaf;
    return dirExists(view) ? view : "";
}


bool loadProgramUnits(const std::vector<std::string>& cliInputs, const char* argv0,
                      std::vector<SharedCompilationUnit>& units,
                      std::vector<std::string>& paths,
                      bool includeDevDeps = false)
{
    std::string stdlibDir = resolveStdlibDir(argv0);
    std::vector<std::string> extraRoots = splitSearchPath(getenv("KAMA_PATH"));
    std::string depsView = projectDepsView(cliInputs);   // resolved package roots (`kama pkg install`), or ""
    // dev-dependencies are a SEPARATE view, only on the import path under `--dev` — so production code can
    // never import a dev-dep (the phantom-dep guarantee makes this a hard resolve error), at any opt level.
    std::string devDepsView = includeDevDeps ? projectDepsView(cliInputs, ".kama/dev-deps") : "";
    // A unit's namespace as an `a::b` key (empty for a private/no-namespace file).
    auto nsKey = [](SharedCompilationUnit u) -> std::string {
        if (!u || !u->nameSpace || !u->nameSpace->name) return "";
        std::string s; auto id = u->nameSpace->name;
        if (id->qualifier) for (auto& seg : *id->qualifier) s += *seg + "::";
        if (id->value) s += *id->value;
        return s;
    };
    std::set<std::string> seen;      // resolved absolute paths already parsed
    std::set<std::string> provided;  // namespaces already in the compilation (satisfy an import w/o disk lookup)
    // The smart-pointer triad is now a built-in module (embedded, always in scope — see preludeModuleUnits),
    // so an explicit `import std::memory` is a satisfied no-op: skip the disk lookup rather than re-parse it
    // (which would double-define the triad, and would fail outright in a `--no-std` install with no lib/).
    provided.insert("std::memory");
    for (auto& in : cliInputs) {
        std::string abs = absolutePath(in);
        if (!seen.insert(abs).second) continue;
        SharedCompilationUnit u = parseFile(in);
        if (!u) return false;
        units.push_back(u); paths.push_back(abs);
        std::string k = nsKey(u); if (!k.empty()) provided.insert(k);
    }
    for (size_t i = 0; i < units.size(); ++i) {          // grows as imports are discovered (BFS)
        // Phase D: graph serde is a compiler intrinsic now — no std::serialization::graph runtime to inject.
        if (!units[i]->importDeclarationList) continue;
        std::string here = dirName(paths[i]);
        for (auto& imp : *units[i]->importDeclarationList) {
            std::vector<std::string> segs;
            if (imp->modulePath) for (auto& s : *imp->modulePath) segs.push_back(*s);
            if (segs.empty()) continue;
            std::string key;                                  // "a::b" — match against loaded namespaces
            for (size_t k = 0; k < segs.size(); ++k) key += (k ? "::" : "") + segs[k];
            if (provided.count(key)) continue;                // already in the compilation (CLI input / earlier import)
            bool reserved = (segs[0] == "std" || segs[0] == "core");
            std::vector<std::string> roots;
            if (!reserved) {
                roots.push_back(here);
                for (auto& r : extraRoots) roots.push_back(r);
                // Declared package deps resolve via the materialized view — and ONLY via it, so an
                // undeclared `import` misses the view and hits the missing-module error below (the
                // phantom-dependency guarantee falls out for free — no extra check).
                if (!depsView.empty()) roots.push_back(depsView);
                if (!devDepsView.empty()) roots.push_back(devDepsView);   // `--dev` only
            }
            roots.push_back(stdlibDir);
            auto files = resolveModuleFiles(segs, roots);
            if (files.empty()) {
                std::string name;
                for (size_t k = 0; k < segs.size(); ++k) name += (k ? "::" : "") + segs[k];
                fprintf(stderr, "kama: error: cannot resolve module '%s' (from %s)\n",
                        name.c_str(), reserved ? stdlibDir.c_str() : here.c_str());
                return false;
            }
            for (auto& f : files) {
                std::string abs = absolutePath(f);
                if (!seen.insert(abs).second) continue;
                SharedCompilationUnit mu = parseFile(f);
                if (!mu) return false;
                units.push_back(mu); paths.push_back(abs);
                std::string mk = nsKey(mu); if (!mk.empty()) provided.insert(mk);
            }
        }
    }
    return true;
}

// Parse one kama file into a CompilationUnit. Returns nullptr on failure.
SharedCompilationUnit parseFile(const std::string& inputFile)
{
    yyscan_t scanner;
    struct LexerInstanceData extra = {
        KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE,
        KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE,
        nullptr,
        std::make_shared<CodeGenContext>(std::make_shared<std::string>(inputFile)),
        nullptr
    };

    yylex_init_extra(&extra, &scanner);

    FILE* input = fopen(inputFile.c_str(), "r");
    if (!input) {
        fprintf(stderr, "kama: error: cannot open input file '%s'\n", inputFile.c_str());
        yylex_destroy(scanner);
        return nullptr;
    }
    yy_switch_to_buffer(yy_create_buffer(input, YY_BUF_SIZE, scanner), scanner);

    int rc = yyparse(scanner);
    yylex_destroy(scanner);
    fclose(input);

    if (rc != 0 || extra.codeGenContext->errorCount() > 0)
        return nullptr;
    return extra.compilationUnit;
}

// The implicit prelude (Optional/Result, the language-level contracts, the primitive retro-impls,
// Chars/Split) is embedded from prelude/global.kama into KAMA_PRELUDE_SRC (see kama.prelude.h and
// tools/embed_prelude.sh), so it ships inside bin/kama even for a --no-std install. The namespaced
// built-in triad (prelude/std/memory/*.kama) is embedded as KAMA_PRELUDE_MODULES.

// Parse an in-memory kama source string into a CompilationUnit (flex string buffer). nullptr on error.
SharedCompilationUnit parseString(const char* src, const std::string& name)
{
    yyscan_t scanner;
    struct LexerInstanceData extra = {
        KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE,
        KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE,
        nullptr,
        std::make_shared<CodeGenContext>(std::make_shared<std::string>(name)),
        nullptr
    };
    yylex_init_extra(&extra, &scanner);
    yy_scan_string(src, scanner);
    int rc = yyparse(scanner);
    yylex_destroy(scanner);
    if (rc != 0 || extra.codeGenContext->errorCount() > 0) return nullptr;
    return extra.compilationUnit;
}

// Parse a buffer for the LSP: mirrors parseString but keeps the CodeGenContext alive so parse
// diagnostics survive a failed parse (as-you-type buffers are usually mid-edit). `unit` is non-null only
// when the parse fully succeeded, so a consumer serves the last good AST on a parse error. Internal to
// this TU; the LSP reaches it through the external `lspAnalyzeBuffer` seam (defined at end of file).
struct ParseResult { SharedCompilationUnit unit; SharedCodeGenContext ctx; };
ParseResult parseForQuery(const char* src, const std::string& name)
{
    yyscan_t scanner;
    struct LexerInstanceData extra = {
        KAMA_LEXERINSTANCE_DEFAULT_LINE_ONE,
        KAMA_LEXERINSTANCE_DEFAULT_COLUMN_ONE,
        nullptr,
        std::make_shared<CodeGenContext>(std::make_shared<std::string>(name)),
        nullptr
    };
    yylex_init_extra(&extra, &scanner);
    yy_scan_string(src, scanner);
    int rc = yyparse(scanner);
    yylex_destroy(scanner);
    ParseResult r;
    r.ctx  = extra.codeGenContext;
    r.unit = (rc == 0 && extra.codeGenContext->errorCount() == 0) ? extra.compilationUnit : nullptr;
    return r;
}

SharedCompilationUnit preludeUnit() { return parseString(KAMA_PRELUDE_SRC, "<prelude>"); }

// The namespaced built-in modules (the smart-pointer triad, std::memory) — embedded like the prelude
// so they're always in scope with no `import std::memory`, in both the full and `--no-std` installs.
// Each keeps its own `namespace`/`export`; the emitter collects them under that scope + an implicit
// `using` (see CEmitter::collectProgram / ctxOf). nullptr units (a parse failure) are dropped.
std::vector<SharedCompilationUnit> preludeModuleUnits()
{
    std::vector<SharedCompilationUnit> units;
    for (int i = 0; i < KAMA_PRELUDE_MODULE_COUNT; ++i) {
        SharedCompilationUnit u = parseString(KAMA_PRELUDE_MODULES[i].src, "<prelude-module>");
        if (u) units.push_back(u);
    }
    return units;
}

// `--no-heap` (MCU step 5): reject every emitter-visible heap allocation program-wide (the no-heap
// subset). Threaded to each CEmitter via `setNoHeap`. File-scope like the other build config, set in main.
static bool g_noHeap = false;

// `--release`: strip `debugAssert(...)` at emit time (dev-only checks; `assert` stays always-on). File-scope
// like g_noHeap so the emitter-setup helpers can read it; set in main from the `--release`/`--debug` flags.
static bool g_release = false;

// `--verify` (M3.2a): enforce registry-package signatures on install — a present-but-invalid signature
// and a missing signature both become hard errors. Off by default (warn-only: a present signature is
// checked and a failure only warns), so the signing mechanism lands before the enforcement policy.
static bool g_verifySignatures = false;

// `@compileFor(FLAG)` conditional compilation (the "structure" axis): the active build-flag set
// (built-ins derived from `--target`/`--release`, plus `--define`), the declared-flag universe (from
// `kama.json`, Stage 2), and whether strict flag-name validation is on. File-scope like `g_noHeap`,
// populated in main, threaded to each CEmitter via `setBuildFlags`.
static std::set<std::string> g_activeFlags;
static std::set<std::string> g_declaredFlags;
static bool g_strictFlags = false;

// The project's baked default `KAMA_LOG` spec (from the manifest `log` section), threaded to the emitter and
// compiled into `main` so a shipped binary carries its default log filter (M5). Empty = no baked default.
static std::string g_logDefault;

// Minimal purpose-built reader for the `kama.json` project manifest. v1 needs only the declared flag
// NAMES and which carry `"default": true`; every other key (`name`/`version`/… — the future
// package-management surface) is skipped generically. Tolerant of unknown fields, strict enough to
// reject malformed JSON with a message. Deliberately NOT a general JSON library — the language's own
// JSON serialization (lib/std/serialization/json) is kama-level runtime code and cannot parse the
// compiler's own build-time config (different layer).
// One `dependencies` entry from `kama.json`. Exactly one source of {path, git, url} is set;
// `rev`/`integrity`/`version` refine it. The map key (not stored here) is the import-root segment.
struct DepSpec {
    std::string path;       // local directory (relative to the manifest) — no fetch, no store
    std::string git;        // repo URL (+ rev)
    std::string url;        // tarball URL (+ integrity)
    std::string rev;        // git tag/branch/commit
    std::string integrity;  // "sha256-<hex>" for a url dep
    std::string version;    // git-range: a SemVer range; registry dep: the range against the index
    std::string registry;   // optional explicit registry base URI (else the configured/default registry)
    std::string signature;  // registry dep (internal): the SSHSIG blob from the index (verified at fetch)
    std::string sigKey;     // registry dep (internal): the signer public key from the index
};

// The `registries` config (M3.1b): where registry deps resolve from. `default` is the base chain for
// unscoped (and unmapped-scope) names; `scopes["@acme"]` is the chain for `@acme/*`. Each value is an
// ordered list of base URIs (priority order — the first that has the package wins). `default: false`
// drops the built-in default entirely (a fully-private / air-gapped setup). The lock pins content
// identity (the integrity), not the URI, so a scope can be re-pointed to a mirror without re-resolving.
struct RegConfig {
    std::vector<std::string> defaultBases;                        // explicit `default` chain (empty => built-in)
    bool defaultSet = false;                                       // a `default` key was present
    bool defaultDisabled = false;                                  // `default: false`
    std::map<std::string, std::vector<std::string>> scopes;        // "@scope" -> ordered base chain

    // Merge a `kama.local.json` `registries` over `this` (M5.3): a local `default` (incl. `default:false`)
    // replaces the base default; a local `@scope` replaces that scope's chain (others preserved). Dev-local
    // only — the lock pins integrity not URI, so re-pointing to a same-bytes mirror re-resolves identically.
    void applyLocal(const RegConfig& local) {
        if (local.defaultSet) {
            defaultSet = true; defaultDisabled = local.defaultDisabled; defaultBases = local.defaultBases;
        }
        for (auto& sc : local.scopes) scopes[sc.first] = sc.second;
    }
};

// The `log` config (M5): the project's default log filter, baked into the binary. `level` is the global
// bareword threshold; `tags` is an ordered list of (tag -> level) overrides. A `kama.local.json` sibling
// deep-merges over the manifest's (see `applyLocal`): a set local `level` wins, and a local tag replaces the
// same-named base tag (others preserved) — so a dev can bump verbosity without discarding project defaults.
struct LogConfig {
    bool levelSet = false;
    std::string level;                                         // global threshold bareword ("" if unset)
    std::vector<std::pair<std::string, std::string>> tags;     // ordered tag -> level overrides

    void applyLocal(const LogConfig& local) {                  // `this` is the base; merge `local` on top
        if (local.levelSet) { level = local.level; levelSet = true; }
        for (auto& lt : local.tags) {
            bool found = false;
            for (auto& bt : tags) if (bt.first == lt.first) { bt.second = lt.second; found = true; break; }
            if (!found) tags.push_back(lt);
        }
    }
    std::string canonical() const {                            // -> the `KAMA_LOG` spec: global, then tag=level…
        std::string spec = level;
        for (auto& t : tags) { if (!spec.empty()) spec += ","; spec += t.first + "=" + t.second; }
        return spec;
    }
};

// A scoped package `@acme/foo` imports under its BARE last segment (`foo`) — the scope is registry
// routing only. Two scopes exposing the same bare name collide (a hard error, detected at link time).
static std::string importNameOf(const std::string& name)
{
    if (!name.empty() && name[0] == '@') { size_t s = name.rfind('/'); if (s != std::string::npos) return name.substr(s + 1); }
    return name;
}
// The scope of a package name (`@acme` for `@acme/foo`), or "" for an unscoped name.
static std::string scopeOf(const std::string& name)
{
    if (!name.empty() && name[0] == '@') { size_t s = name.find('/'); if (s != std::string::npos) return name.substr(0, s); }
    return "";
}
// A filesystem-safe label for the content-addressed store dir (`<label>-<hash>`). The hash is the real
// identity; this is only a human-readable prefix, so a scoped name's '/' is flattened to '_'.
static std::string storeLabel(const std::string& name)
{
    std::string s = name;
    for (char& c : s) if (c == '/') c = '_';
    return s;
}

// Two dependency specs name "the same package" iff every identity-bearing field matches. For a
// non-range dep this IS the whole conflict test: identical -> dedup in the resolver, divergent ->
// hard error. Range (git+version, no rev) deps take the SemVer path below instead.
static bool sameSpec(const DepSpec& a, const DepSpec& b)
{
    return a.path == b.path && a.git == b.git && a.url == b.url
        && a.rev == b.rev && a.integrity == b.integrity;
}

// ---- SemVer (M3.0) ---------------------------------------------------------------------------
// A deliberately minimal MAJOR.MINOR.PATCH engine: no pre-release/build ordering (a tag carrying a
// `-pre`/`+build` suffix simply isn't a candidate), no compound/hyphen/`||` ranges. It exists so a
// git dependency can pin a *range* (`"version": "^1.2.0"`) and resolve to the highest matching tag;
// the lock then pins the concrete version + commit, so builds stay reproducible and offline.
struct SemVer { int major = 0, minor = 0, patch = 0; };

// Parse strict "MAJOR.MINOR.PATCH" (optional leading 'v'). Rejects pre-release/build metadata, extra
// components, leading '+'/signs, and empties. Never throws; returns false on anything non-conforming.
static bool parseSemVer(const std::string& in, SemVer& out)
{
    std::string s = in;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
    if (s.find('-') != std::string::npos || s.find('+') != std::string::npos) return false;  // pre/build
    int part[3]; size_t p = 0;
    for (int k = 0; k < 3; ++k) {
        if (p >= s.size() || !isdigit((unsigned char)s[p])) return false;
        long v = 0;
        while (p < s.size() && isdigit((unsigned char)s[p])) { v = v * 10 + (s[p] - '0'); ++p;
            if (v > 1000000000L) return false; }
        part[k] = (int)v;
        if (k < 2) { if (p >= s.size() || s[p] != '.') return false; ++p; }
    }
    if (p != s.size()) return false;   // trailing junk (a 4th component, etc.)
    out.major = part[0]; out.minor = part[1]; out.patch = part[2];
    return true;
}

static int cmpSemVer(const SemVer& a, const SemVer& b)
{
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// A range normalized to an optional lower + optional upper bound, so `satisfies` is one interval
// test regardless of the surface operator. No bound at all (`*`/`x`/empty) matches anything.
struct VersionReq {
    bool hasLo = false, loInclusive = true; SemVer lo;
    bool hasHi = false, hiInclusive = false; SemVer hi;
};

// Operators: exact `1.2.3`; caret `^1.2.3`; tilde `~1.2.3`; comparators `>=` `>` `<=` `<`;
// wildcard `*`/`x`/empty. Returns false on anything else (compound ranges, hyphen ranges, `||`).
static bool parseVersionReq(const std::string& in, VersionReq& out)
{
    // trim surrounding whitespace
    size_t a = 0, b = in.size();
    while (a < b && isspace((unsigned char)in[a])) ++a;
    while (b > a && isspace((unsigned char)in[b - 1])) --b;
    std::string s = in.substr(a, b - a);
    out = VersionReq();
    if (s.empty() || s == "*" || s == "x" || s == "X") return true;   // matches anything

    auto compat = [&](const SemVer& v, char kind) {   // caret/tilde upper bound
        // caret: next incompatible release. ^1.2.3 -> <2.0.0; ^0.2.3 -> <0.3.0; ^0.0.3 -> <0.0.4.
        // tilde: next minor.            ~1.2.3 -> <1.3.0; ~0.2.3 -> <0.3.0.
        SemVer hi;
        if (kind == '^') {
            if (v.major != 0)      { hi.major = v.major + 1; hi.minor = 0; hi.patch = 0; }
            else if (v.minor != 0) { hi.major = 0; hi.minor = v.minor + 1; hi.patch = 0; }
            else                   { hi.major = 0; hi.minor = 0; hi.patch = v.patch + 1; }
        } else { // '~'
            hi.major = v.major; hi.minor = v.minor + 1; hi.patch = 0;
        }
        out.hasLo = true; out.loInclusive = true; out.lo = v;
        out.hasHi = true; out.hiInclusive = false; out.hi = hi;
    };

    if (s[0] == '^' || s[0] == '~') {
        SemVer v; if (!parseSemVer(s.substr(1), v)) return false;
        compat(v, s[0]); return true;
    }
    if (s[0] == '>' || s[0] == '<') {
        bool eq = s.size() > 1 && s[1] == '=';
        SemVer v; if (!parseSemVer(s.substr(eq ? 2 : 1), v)) return false;
        if (s[0] == '>') { out.hasLo = true; out.loInclusive = eq; out.lo = v; }
        else             { out.hasHi = true; out.hiInclusive = eq; out.hi = v; }
        return true;
    }
    // bare version => exact
    SemVer v; if (!parseSemVer(s, v)) return false;
    out.hasLo = out.hasHi = true; out.loInclusive = out.hiInclusive = true; out.lo = out.hi = v;
    return true;
}

static bool satisfies(const VersionReq& r, const SemVer& v)
{
    if (r.hasLo) { int c = cmpSemVer(v, r.lo); if (c < 0 || (c == 0 && !r.loInclusive)) return false; }
    if (r.hasHi) { int c = cmpSemVer(v, r.hi); if (c > 0 || (c == 0 && !r.hiInclusive)) return false; }
    return true;
}

// The intersection of two normalized ranges: the tighter of each bound. The authoritative
// "is it empty" check is "does any real tag satisfy the result" (done at the call site over the
// enumerated tags), so this never needs to reason about emptiness itself.
static VersionReq intersect(const VersionReq& a, const VersionReq& b)
{
    VersionReq r;
    if (a.hasLo || b.hasLo) {
        if (!a.hasLo)      { r.hasLo = true; r.lo = b.lo; r.loInclusive = b.loInclusive; }
        else if (!b.hasLo) { r.hasLo = true; r.lo = a.lo; r.loInclusive = a.loInclusive; }
        else {
            int c = cmpSemVer(a.lo, b.lo);
            if (c > 0)      { r.hasLo = true; r.lo = a.lo; r.loInclusive = a.loInclusive; }
            else if (c < 0) { r.hasLo = true; r.lo = b.lo; r.loInclusive = b.loInclusive; }
            else            { r.hasLo = true; r.lo = a.lo; r.loInclusive = a.loInclusive && b.loInclusive; }
        }
    }
    if (a.hasHi || b.hasHi) {
        if (!a.hasHi)      { r.hasHi = true; r.hi = b.hi; r.hiInclusive = b.hiInclusive; }
        else if (!b.hasHi) { r.hasHi = true; r.hi = a.hi; r.hiInclusive = a.hiInclusive; }
        else {
            int c = cmpSemVer(a.hi, b.hi);
            if (c < 0)      { r.hasHi = true; r.hi = a.hi; r.hiInclusive = a.hiInclusive; }
            else if (c > 0) { r.hasHi = true; r.hi = b.hi; r.hiInclusive = b.hiInclusive; }
            else            { r.hasHi = true; r.hi = a.hi; r.hiInclusive = a.hiInclusive && b.hiInclusive; }
        }
    }
    return r;
}

// A git dependency opts into range resolution iff it names a repo and a version range but NO fixed
// rev. (`rev` + `version` together is rejected earlier as ambiguous — pick one.)
static bool isRangeDep(const DepSpec& d)
{
    return !d.git.empty() && d.rev.empty() && !d.version.empty();
}

// A registry dependency (M3.1) opts in by naming a version range and NO explicit source (git/url/path).
// The package is resolved through a registry index (an explicit `registry` base, else the configured
// default); the chosen version's index entry yields a tarball URI + integrity, so it reduces to a url
// dep for the fetch. A name is a git-range XOR a registry dep XOR an exact pin — never a mix.
static bool isRegistryDep(const DepSpec& d)
{
    return d.git.empty() && d.url.empty() && d.path.empty() && !d.version.empty();
}
// ---------------------------------------------------------------------------------------------

struct ManifestReader {
    const std::string& s;
    size_t i = 0;
    std::string err;
    std::set<std::string>& declared;
    std::set<std::string>& defaults;
    std::map<std::string, DepSpec>* deps = nullptr;      // set to capture `dependencies` (else it's skipped)
    std::map<std::string, DepSpec>* devDeps = nullptr;   // set to capture `dev-dependencies` (else skipped)
    std::string* mainOut = nullptr;                       // set to capture the `main` entry field (else skipped)
    std::vector<std::string>* sourcesOut  = nullptr;
    std::vector<std::string>* packagesOut = nullptr;
    std::string* toolchainOut = nullptr;                  // set to capture the `toolchain` pin (else skipped)
    std::string* nameOut = nullptr;                       // set to capture the `name` (else skipped) — `kama publish`
    std::string* versionOut = nullptr;                    // set to capture the `version` (else skipped) — `kama publish`
    RegConfig* registriesOut = nullptr;                   // set to capture the `registries` config (M3.1b)
    std::map<std::string, DepSpec>* overridesOut = nullptr;// set to capture `overrides` (kama.local.json, M5.3)
    LogConfig* logOut = nullptr;                          // set to capture the `log` config (M5)
    ManifestReader(const std::string& src, std::set<std::string>& d, std::set<std::string>& df)
        : s(src), declared(d), defaults(df) {}

    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    bool str(std::string& out) {
        ws(); if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i; out.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) { char e = s[i++];
                switch (e) { case 'n':c='\n';break; case 't':c='\t';break; case 'r':c='\r';break;
                             case '"':c='"';break; case '\\':c='\\';break; case '/':c='/';break; default:c=e; } }
            out.push_back(c);
        }
        if (i >= s.size()) return fail("unterminated string");
        ++i; return true;
    }

    // A JSON array of strings (`sources`). Rejects a non-array or a non-string element rather than
    // tolerating it: this one declares what the tooling may rewrite, so a typo must not silently widen
    // or narrow the set.
    bool stringArray(std::vector<std::string>& out) {
        ws(); if (i >= s.size() || s[i] != '[') return fail("expected a JSON array");
        ++i; ws();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            std::string v; if (!str(v)) return false;
            out.push_back(v);
            ws();
            if (i < s.size() && s[i] == ',') { ++i; ws(); continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return fail("expected ',' or ']' in a string array");
        }
    }

    bool skipValue() {   // string | number | true/false/null | balanced object/array
        ws(); if (i >= s.size()) return fail("unexpected end of manifest");
        char c = s[i];
        if (c == '"') { std::string t; return str(t); }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']'; int depth = 0; bool inStr = false;
            while (i < s.size()) {
                char d = s[i++];
                if (inStr)            { if (d == '\\' && i < s.size()) ++i; else if (d == '"') inStr = false; }
                else if (d == '"')      inStr = true;
                else if (d == open)     ++depth;
                else if (d == close)  { if (--depth == 0) return true; }
            }
            return fail("unbalanced brackets in manifest");
        }
        while (i < s.size() && s[i]!=','&&s[i]!='}'&&s[i]!=']'&&s[i]!=' '&&s[i]!='\t'&&s[i]!='\n'&&s[i]!='\r') ++i;
        return true;
    }

    bool flagsObject() {   // { "NAME": { "default": true }, "OTHER": {}, ... }
        ws(); if (i >= s.size() || s[i] != '{') return fail("`flags` must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string name; if (!str(name)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a flag name");
            ++i; declared.insert(name); ws();
            if (i < s.size() && s[i] == '{') {           // inner object: look for "default": true
                ++i; ws();
                if (i < s.size() && s[i] == '}') ++i;
                else while (true) {
                    std::string k; if (!str(k)) return false;
                    ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in a flag body");
                    ++i; ws();
                    if (k == "default") {
                        if      (s.compare(i, 4, "true")  == 0) { defaults.insert(name); i += 4; }
                        else if (s.compare(i, 5, "false") == 0) { i += 5; }
                        else if (!skipValue()) return false;
                    } else if (!skipValue()) return false;
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    return fail("expected ',' or '}' in a flag body");
                }
            } else if (!skipValue()) return false;       // tolerate a non-object flag value
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `flags`");
        }
        return true;
    }

    bool depsObject(std::map<std::string, DepSpec>* target) {   // { "name": { "git":.., "rev":.., "url":.., "integrity":.., "path":.., "version":.. }, ... }
        ws(); if (i >= s.size() || s[i] != '{') return fail("`dependencies` must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string name; if (!str(name)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a dependency name");
            ++i; ws();
            if (i >= s.size() || s[i] != '{') return fail("a dependency value must be an object");
            ++i; ws();
            DepSpec spec;
            if (i < s.size() && s[i] == '}') ++i;
            else while (true) {
                std::string k; if (!str(k)) return false;
                ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in a dependency body");
                ++i; ws();
                std::string v;
                if (i < s.size() && s[i] == '"') { if (!str(v)) return false; }
                else if (!skipValue()) return false;         // tolerate a non-string value (ignored)
                if      (k == "path")      spec.path = v;
                else if (k == "git")       spec.git = v;
                else if (k == "url")       spec.url = v;
                else if (k == "rev")       spec.rev = v;
                else if (k == "integrity") spec.integrity = v;
                else if (k == "version")   spec.version = v;
                else if (k == "registry")  spec.registry = v;
                // unknown keys ignored (forward-compat)
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}' in a dependency body");
            }
            // A git dep pins EITHER an exact `rev` OR a `version` range — never both (ambiguous).
            if (!spec.git.empty() && !spec.rev.empty() && !spec.version.empty()) {
                err = "dependency '" + name + "': a git dependency takes either \"rev\" (an exact "
                      "tag/branch/commit) or \"version\" (a range), not both";
                return false;
            }
            // A name is a git-range XOR a registry dep XOR an exact pin (git/url/path). A registry dep is
            // a bare `version` with no source; an explicit `registry` base is only meaningful for it.
            {
                int nsrc = (!spec.git.empty()) + (!spec.url.empty()) + (!spec.path.empty());
                if (nsrc > 1) {
                    err = "dependency '" + name + "': give exactly one of \"git\"/\"url\"/\"path\"";
                    return false;
                }
                if (!spec.registry.empty() && nsrc != 0) {
                    err = "dependency '" + name + "': \"registry\" pins a registry dependency (a bare "
                          "\"version\"), so it cannot combine with git/url/path";
                    return false;
                }
                if (!spec.registry.empty() && spec.version.empty()) {
                    err = "dependency '" + name + "': \"registry\" requires a \"version\" range";
                    return false;
                }
            }
            if (target) (*target)[name] = spec;
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `dependencies`");
        }
        return true;
    }

    // Parse a base-URI value: one string, or an array of strings (a priority-ordered chain), into `out`.
    bool baseList(std::vector<std::string>& out) {
        ws();
        if (i < s.size() && s[i] == '"') { std::string v; if (!str(v)) return false; out.push_back(v); return true; }
        if (i < s.size() && s[i] == '[') {
            ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            while (true) {
                std::string v; if (!str(v)) return false; out.push_back(v); ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; break; }
                return fail("expected ',' or ']' in a registry base list");
            }
            return true;
        }
        return fail("a registry base must be a string or an array of strings");
    }

    bool registriesObject(RegConfig* cfg) {   // { "default": <base|[bases]|false>, "@scope": <base|[bases]>, ... }
        ws(); if (i >= s.size() || s[i] != '{') return fail("`registries` must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string key; if (!str(key)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a registry key"); ++i; ws();
            if (key == "default") {
                cfg->defaultSet = true;
                if (s.compare(i, 5, "false") == 0) { cfg->defaultDisabled = true; i += 5; }
                else if (s.compare(i, 4, "true") == 0) { i += 4; }   // tolerate `true` (== use built-in)
                else if (!baseList(cfg->defaultBases)) return false;
            } else {
                std::vector<std::string> bases;
                if (!baseList(bases)) return false;
                cfg->scopes[key] = bases;
            }
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `registries`");
        }
        return true;
    }

    // A level name in the `log` config — the same vocabulary the runtime `KAMA_LOG` grammar accepts, so a
    // typo is a clean manifest error here rather than a silently-ignored default at runtime.
    static bool validLevelName(const std::string& v) {
        return v=="off"||v=="error"||v=="warn"||v=="info"||v=="debug"||v=="trace";
    }

    // Parse the `log` config object into `*logOut`. { "level": <lvl>, "tags": { <tag>: <lvl> } }, both keys
    // optional. Level names are validated against the same vocabulary the runtime `KAMA_LOG` grammar accepts.
    // Unknown keys are tolerated (forward-compat, e.g. a future compile-strip floor).
    bool logObject() {
        ws(); if (i >= s.size() || s[i] != '{') return fail("`log` must be a JSON object");
        ++i; ws();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string k; if (!str(k)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in `log`");
            ++i; ws();
            if (k == "level") {
                std::string v; if (!str(v)) return false;
                if (!validLevelName(v)) return fail("`log.level` must be one of off/error/warn/info/debug/trace");
                if (logOut) { logOut->level = v; logOut->levelSet = true; }
            } else if (k == "tags") {
                ws(); if (i >= s.size() || s[i] != '{') return fail("`log.tags` must be a JSON object");
                ++i; ws();
                if (i < s.size() && s[i] == '}') ++i;
                else while (true) {
                    std::string tag; if (!str(tag)) return false;
                    ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in `log.tags`");
                    ++i; ws();
                    std::string v; if (!str(v)) return false;
                    if (!validLevelName(v)) return fail("a `log.tags` level must be off/error/warn/info/debug/trace");
                    if (logOut) logOut->tags.push_back({tag, v});
                    ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    return fail("expected ',' or '}' in `log.tags`");
                }
            } else if (!skipValue()) return false;   // unknown `log` keys tolerated
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `log`");
        }
        return true;
    }

    bool parse() {
        ws(); if (i >= s.size() || s[i] != '{') return fail("manifest must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string key; if (!str(key)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a key");
            ++i;
            if (key == "flags") { if (!flagsObject()) return false; }
            else if (key == "dependencies" && deps) { if (!depsObject(deps)) return false; }
            else if (key == "dev-dependencies" && devDeps) { if (!depsObject(devDeps)) return false; }
            else if (key == "registries" && registriesOut) { if (!registriesObject(registriesOut)) return false; }
            else if (key == "overrides" && overridesOut) { if (!depsObject(overridesOut)) return false; }  // kama.local.json dep path-overrides (M5.3)
            else if (key == "log" && logOut) { if (!logObject()) return false; }   // baked log default (M5)
            else if (key == "sources" && sourcesOut) { if (!stringArray(*sourcesOut)) return false; }  // LSP project scope
            else if (key == "packages" && packagesOut) { if (!stringArray(*packagesOut)) return false; } // workspace members
            else if (key == "main" && mainOut) { if (!str(*mainOut)) return false; }   // entry `.kama` (read by `kama run`)
            else if (key == "toolchain" && toolchainOut) { if (!str(*toolchainOut)) return false; }   // pin (read by the selector)
            else if (key == "name" && nameOut) { if (!str(*nameOut)) return false; }
            else if (key == "version" && versionOut) { if (!str(*versionOut)) return false; }
            else if (!skipValue()) return false;         // name / version / future package keys
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' at top level");
        }
        return true;
    }
};

// Load a `kama.json` manifest → declared flag names + defaults. Returns false + sets `err` on failure.
static bool loadManifestFlags(const std::string& path,
                              std::set<std::string>& declared,
                              std::set<std::string>& defaults,
                              std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ManifestReader r(src, declared, defaults);
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `dependencies` (name -> DepSpec). Reuses ManifestReader (unknown keys
// tolerated), so this is orthogonal to the flag load. Returns false + sets `err` on malformed JSON.
static bool loadManifestDeps(const std::string& path, std::map<std::string, DepSpec>& deps, std::string& err,
                             std::map<std::string, DepSpec>* devDeps = nullptr, RegConfig* reg = nullptr)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.deps = &deps;
    r.devDeps = devDeps;   // optional: also capture `dev-dependencies` (M2.2)
    r.registriesOut = reg; // optional: also capture `registries` config (M3.1b)
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.local.json`'s INSTALL-path override fields (M5.3): `overrides` (dep path-overrides) and a
// local `registries` config. Both are dev-local — they redirect where deps/registries resolve WITHOUT
// touching the committed `kama.json`/`kama.lock`. Reuses ManifestReader (its `log`/`flags` are read by the
// compiler driver, not here). Returns false + `err` on malformed JSON.
static bool loadManifestLocalInstall(const std::string& path, std::map<std::string, DepSpec>& overrides,
                                     RegConfig& reg, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.overridesOut = &overrides;
    r.registriesOut = &reg;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `main` entry field (the entry `.kama`, relative to the manifest). Reuses
// ManifestReader; `mainOut` is left empty if the field is absent. Returns false + sets `err` on malformed
// JSON. (M2.3 — read by `kama run` to resolve the entry when no file is passed.)
static bool loadManifestMain(const std::string& path, std::string& mainOut, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.mainOut = &mainOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `toolchain` pin (the version the selector should run for this project).
// `tcOut` is left empty if the field is absent. Returns false + sets `err` only on malformed JSON. Reused by
// the PATH selector — a cheap read of one key, done *before* any compiler runs. (M1.)
static bool loadManifestToolchain(const std::string& path, std::string& tcOut, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.toolchainOut = &tcOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `sources` — the directories/files that make up this package, relative to
// the manifest. Empty (or absent) means "not declared", and the LSP falls back to walking the whole
// manifest directory under a file cap. Declaring them is what removes the guess, and with it the cap.
// Returns false + `err` on malformed JSON. (LSP M3.5.)
static bool loadManifestSources(const std::string& path, std::vector<std::string>& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.sourcesOut = &out;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; out.clear(); return false; }
    return true;
}

// Load a `kama.json` manifest's `packages` — the member packages of a WORKSPACE root, relative to the
// manifest, each a directory holding its own kama.json. A trailing `/*` expands to every immediate
// subdirectory that has one (`"packages/*"`), so a monorepo need not edit the root manifest per package.
// Declaring this is what turns "is this a monorepo root?" from something the LSP infers from position
// into something the repository states. Returns false + `err` on malformed JSON. (LSP M3.5.)
static bool loadManifestPackages(const std::string& path, std::vector<std::string>& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.packagesOut = &out;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; out.clear(); return false; }
    return true;
}

// Load a `kama.json` (or `kama.local.json`) manifest's `log` config into `out` (left default if absent).
// Reuses ManifestReader. Returns false + `err` on malformed JSON or an invalid level name. (M5.)
static bool loadManifestLog(const std::string& path, LogConfig& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.logOut = &out;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a manifest's `name` + `version` (both empty if absent). Reused by `kama publish`. Returns false +
// `err` only on malformed JSON.
static bool loadManifestNameVersion(const std::string& path, std::string& nameOut, std::string& versionOut,
                                    std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.nameOut = &nameOut; r.versionOut = &versionOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// One resolved package in `kama.lock`. The lock is what the build's view is materialized from — the
// reproducibility record: (source, pinned identity, integrity, transitive deps).
struct LockEntry {
    std::string source;                     // "path" | "git" | "url" | "registry"
    std::string path, git, url, rev, commit, integrity;
    std::string registry;                   // registry dep: the base URI the version was resolved from
    std::string version;                    // resolved concrete SemVer for a range dep (git+version / registry); else empty
    std::string treeHash;                   // the store dir key "sha256-<treehash>" (== integrity for git; the
                                            // canonical unpacked-tree hash for url, whose integrity is the tarball)
    bool dev = false;                       // a dev-dependency (top-level view only, never propagated transitively)
    std::vector<std::string> dependencies;  // direct dep names (serialized so the build never re-reads a dep's manifest)
};

static std::string jsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s) switch (c) {
        case '"':  o += "\\\""; break;  case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;  case '\t': o += "\\t";  break;
        case '\r': o += "\\r";  break;  default:   o += c;
    }
    return o;
}

// Write `kama.lock` deterministically (sorted std::map => byte-stable => reproducible re-install).
static bool writeLockFile(const std::string& path, const std::map<std::string, LockEntry>& pkgs)
{
    std::ofstream out(path);
    if (!out) return false;
    out << "{\n  \"lockVersion\": 1,\n  \"packages\": {";
    bool first = true;
    for (auto& kv : pkgs) {
        const LockEntry& e = kv.second;
        out << (first ? "\n" : ",\n"); first = false;
        out << "    \"" << jsonEscape(kv.first) << "\": { \"source\": \"" << jsonEscape(e.source) << "\"";
        if (!e.path.empty())      out << ", \"path\": \""      << jsonEscape(e.path)      << "\"";
        if (!e.git.empty())       out << ", \"git\": \""       << jsonEscape(e.git)       << "\"";
        if (!e.url.empty())       out << ", \"url\": \""       << jsonEscape(e.url)       << "\"";
        if (!e.registry.empty())  out << ", \"registry\": \""  << jsonEscape(e.registry)  << "\"";
        if (!e.rev.empty())       out << ", \"rev\": \""       << jsonEscape(e.rev)       << "\"";
        if (!e.version.empty())   out << ", \"version\": \""   << jsonEscape(e.version)   << "\"";
        if (!e.commit.empty())    out << ", \"commit\": \""    << jsonEscape(e.commit)    << "\"";
        if (!e.integrity.empty()) out << ", \"integrity\": \"" << jsonEscape(e.integrity) << "\"";
        // treeHash == the store dir key; omit when it equals integrity (git) so those entries stay byte-
        // identical to M2.1 — it only appears for url deps (whose integrity is the tarball, not the tree).
        if (!e.treeHash.empty() && e.treeHash != e.integrity)
                                  out << ", \"treeHash\": \""  << jsonEscape(e.treeHash)  << "\"";
        if (e.dev)                out << ", \"dev\": true";
        out << ", \"dependencies\": [";
        for (size_t j = 0; j < e.dependencies.size(); ++j)
            out << (j ? ", " : "") << "\"" << jsonEscape(e.dependencies[j]) << "\"";
        out << "] }";
    }
    out << (first ? "" : "\n  ") << "}\n}\n";
    return true;
}

// Parse a `kama.lock` we wrote (writeLockFile's exact schema) back into name -> LockEntry. A dedicated
// hand-parser mirroring the writer (the lock schema differs from `kama.json` — commit/treeHash/dev/
// dependencies[] — so it does NOT share ManifestReader). Minimal + tolerant: unknown keys skipped, every
// field optional (the writer omits empties), only structural JSON errors fail. `str()` is the inverse of
// jsonEscape, so it round-trips exactly. Returns false + sets `err` on malformed JSON.
struct LockReader {
    const std::string& s; size_t i = 0; std::string err;
    LockReader(const std::string& src) : s(src) {}
    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }
    bool str(std::string& out) {
        ws(); if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i; out.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) { char e = s[i++];
                switch (e) { case 'n':c='\n';break; case 't':c='\t';break; case 'r':c='\r';break;
                             case '"':c='"';break; case '\\':c='\\';break; case '/':c='/';break; default:c=e; } }
            out.push_back(c);
        }
        if (i >= s.size()) return fail("unterminated string"); ++i; return true;
    }
    bool skipValue() {   // string | number | true/false/null | balanced object/array (== ManifestReader's)
        ws(); if (i >= s.size()) return fail("unexpected end of lock");
        char c = s[i];
        if (c == '"') { std::string t; return str(t); }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']'; int depth = 0; bool inStr = false;
            while (i < s.size()) { char d = s[i++];
                if (inStr)          { if (d == '\\' && i < s.size()) ++i; else if (d == '"') inStr = false; }
                else if (d == '"')    inStr = true;
                else if (d == open)   ++depth;
                else if (d == close){ if (--depth == 0) return true; } }
            return fail("unbalanced brackets in lock");
        }
        while (i < s.size() && s[i]!=','&&s[i]!='}'&&s[i]!=']'&&s[i]!=' '&&s[i]!='\t'&&s[i]!='\n'&&s[i]!='\r') ++i;
        return true;
    }
    bool entry(LockEntry& e) {   // { "source":.., "path":.., ..., "dev": true, "dependencies": [..] }
        ws(); if (i >= s.size() || s[i] != '{') return fail("a lock entry must be an object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string k; if (!str(k)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in a lock entry"); ++i; ws();
            if      (k == "source")    { if (!str(e.source))    return false; }
            else if (k == "path")      { if (!str(e.path))      return false; }
            else if (k == "git")       { if (!str(e.git))       return false; }
            else if (k == "url")       { if (!str(e.url))       return false; }
            else if (k == "registry")  { if (!str(e.registry))  return false; }
            else if (k == "rev")       { if (!str(e.rev))       return false; }
            else if (k == "version")   { if (!str(e.version))   return false; }
            else if (k == "commit")    { if (!str(e.commit))    return false; }
            else if (k == "integrity") { if (!str(e.integrity)) return false; }
            else if (k == "treeHash")  { if (!str(e.treeHash))  return false; }
            else if (k == "dev")       { if (s.compare(i,4,"true")==0) { e.dev = true; i += 4; }
                                         else if (s.compare(i,5,"false")==0) { i += 5; }
                                         else if (!skipValue()) return false; }
            else if (k == "dependencies") {
                ws(); if (i >= s.size() || s[i] != '[') return fail("`dependencies` must be an array");
                ++i; ws();
                if (i < s.size() && s[i] == ']') ++i;
                else while (true) {
                    std::string dep; if (!str(dep)) return false; e.dependencies.push_back(dep); ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    return fail("expected ',' or ']' in `dependencies`");
                }
            }
            else if (!skipValue()) return false;   // unknown key (forward-compat)
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in a lock entry");
        }
        return true;
    }
    bool parse(std::map<std::string, LockEntry>& pkgs) {
        ws(); if (i >= s.size() || s[i] != '{') return fail("lock must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string key; if (!str(key)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a key"); ++i;
            if (key == "packages") {
                ws(); if (i >= s.size() || s[i] != '{') return fail("`packages` must be a JSON object");
                ++i; ws();
                if (i < s.size() && s[i] == '}') ++i;
                else while (true) {
                    std::string name; if (!str(name)) return false;
                    ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a package name"); ++i;
                    LockEntry e; if (!entry(e)) return false; pkgs[name] = e; ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == '}') { ++i; break; }
                    return fail("expected ',' or '}' in `packages`");
                }
            } else if (!skipValue()) return false;   // lockVersion / future keys
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' at top level");
        }
        return true;
    }
};

// Read a kama.lock into name -> LockEntry (empty file / `{}` -> empty map + true). Caller does fileExists.
static bool parseLockFile(const std::string& path, std::map<std::string, LockEntry>& pkgs, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LockReader r(src);
    if (!r.parse(pkgs)) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// mkdir -p (POSIX + mingw): create `path` and any missing parents. Returns true if `path` is a dir after.
static bool makeDirs(const std::string& path)
{
    for (size_t i = 0; i < path.size(); ) {
        size_t slash = path.find('/', i);
        std::string part = (slash == std::string::npos) ? path : path.substr(0, slash);
        if (!part.empty() && !dirExists(part))
#ifdef _WIN32
            _mkdir(part.c_str());
#else
            mkdir(part.c_str(), 0755);
#endif
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return dirExists(path);
}

// Symlink a directory `linkPath` -> `target` (absolute). Windows: a directory junction (mklink /J).
// Replaces an existing link. This is how the per-project view points at a store/path source (pnpm
// model) — zero-copy, no duplication.
static bool linkDir(const std::string& target, const std::string& linkPath)
{
#ifdef _WIN32
    runCmd("cmd /c rmdir \"" + linkPath + "\" 2>nul");
    return runCmd("cmd /c mklink /J \"" + linkPath + "\" \"" + target + "\"") == 0;
#else
    unlink(linkPath.c_str());
    return symlink(target.c_str(), linkPath.c_str()) == 0;
#endif
}

// Emit an already-parsed unit to a single `.c` (`srcPath` drives #line). Returns 0 on success.
int transpileUnitToFile(SharedCompilationUnit unit, const std::string& srcPath,
                        const std::string& outPath, bool emitLines, bool* externsMathH = nullptr,
                        bool* externsNetWeb = nullptr, bool* externsApp = nullptr,
                        bool* externsGpu = nullptr, bool* externsIsolate = nullptr)
{
    std::ofstream out(outPath);
    if (!out) {
        fprintf(stderr, "kama: error: cannot write '%s'\n", outPath.c_str());
        return 1;
    }
    CEmitter emitter(out, srcPath, emitLines);
    emitter.setPrelude(preludeUnit());   // Optional/Result available implicitly
    emitter.setNoHeap(g_noHeap);         // `--no-heap`: reject heap allocation program-wide
    emitter.setRelease(g_release);       // `--release`: strip `debugAssert`
    emitter.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);   // `@compileFor` conditional compilation
    emitter.setLogDefault(g_logDefault);   // baked `KAMA_LOG` project default (M5), compiled into main
    for (auto& m : preludeModuleUnits()) emitter.addPreludeModule(m);   // the always-in-scope triad
    int unsupported = emitter.emit(unit);
    if (externsMathH) *externsMathH = emitter.externsHeader("<math.h>");   // -> the driver appends -lm
    if (externsNetWeb) *externsNetWeb = emitter.externsHeader("kama_net_web.h");   // -> wasm --js-library
    if (externsApp) *externsApp = emitter.externsHeader("kama_app.h");   // std::app -> wasm -sEXIT_RUNTIME=1
    if (externsGpu) *externsGpu = emitter.externsHeader("kama_gpu.h");   // std::gpu seam -> native --webgpu link
    if (externsIsolate) *externsIsolate = emitter.externsHeader("kama_isolate.h")     // std::concurrent seams ->
                                       || emitter.externsHeader("kama_channel.h");    // native -lpthread (isolate OR channel)
    out.close();
    if (unsupported > 0) {
        fprintf(stderr, "kama: %d unlowered construct(s) — see the warnings above.\n", unsupported);
        return 1;   // a construct kama couldn't lower (incl. a safety-gate violation) is a hard error
    }
    return 0;
}

// Emit a multi-file program from already-parsed `units` (parallel to `sourcePaths`): one
// shared header (`headerPath`, included as `headerName`) + one `.c` per unit (`cPaths`).
// Returns 0 on success.
int emitProgramUnits(const std::vector<SharedCompilationUnit>& units,
                     const std::vector<std::string>& sourcePaths,
                     const std::string& headerPath, const std::string& headerName,
                     const std::vector<std::string>& cPaths, bool emitLines,
                     bool* externsMathH = nullptr,   // link hint: did the program `extern "<math.h>";`?
                     bool* externsNetWeb = nullptr,  // link hint: did it `extern "kama_net_web.h";`?
                     bool* externsApp = nullptr,     // link hint: did it `extern "kama_app.h";`? (std::app)
                     bool* externsGpu = nullptr,     // link hint: did it `extern "kama_gpu.h";`? (WebGPU seam)
                     bool* externsIsolate = nullptr) // link hint: did it `extern "kama_isolate.h";`? (isolate seam)
{
    std::ofstream header(headerPath);
    if (!header) { fprintf(stderr, "kama: error: cannot write '%s'\n", headerPath.c_str()); return 1; }

    std::vector<std::unique_ptr<std::ofstream>> moduleFiles;
    std::vector<std::ostream*> moduleStreams;
    for (auto& cp : cPaths) {
        auto f = std::unique_ptr<std::ofstream>(new std::ofstream(cp));
        if (!*f) { fprintf(stderr, "kama: error: cannot write '%s'\n", cp.c_str()); return 1; }
        moduleStreams.push_back(f.get());
        moduleFiles.push_back(std::move(f));
    }

    CEmitter emitter(header, "", emitLines);
    emitter.setPrelude(preludeUnit());   // Optional/Result available implicitly
    emitter.setNoHeap(g_noHeap);         // `--no-heap`: reject heap allocation program-wide
    emitter.setRelease(g_release);       // `--release`: strip `debugAssert`
    emitter.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);   // `@compileFor` conditional compilation
    emitter.setLogDefault(g_logDefault);   // baked `KAMA_LOG` project default (M5), compiled into main
    for (auto& m : preludeModuleUnits()) emitter.addPreludeModule(m);   // the always-in-scope triad
    int unsupported = emitter.emitProgram(units, headerName, header, moduleStreams, sourcePaths);
    if (externsMathH) *externsMathH = emitter.externsHeader("<math.h>");   // -> the driver appends -lm
    if (externsNetWeb) *externsNetWeb = emitter.externsHeader("kama_net_web.h");   // -> wasm --js-library
    if (externsApp) *externsApp = emitter.externsHeader("kama_app.h");   // std::app -> wasm -sEXIT_RUNTIME=1
    if (externsGpu) *externsGpu = emitter.externsHeader("kama_gpu.h");   // std::gpu seam -> native --webgpu link
    if (externsIsolate) *externsIsolate = emitter.externsHeader("kama_isolate.h")     // std::concurrent seams ->
                                       || emitter.externsHeader("kama_channel.h");    // native -lpthread (isolate OR channel)
    header.close();
    for (auto& f : moduleFiles) f->close();

    if (unsupported > 0) {
        fprintf(stderr, "kama: %d unlowered construct(s) — see the warnings above.\n", unsupported);
        return 1;
    }
    return 0;
}

// Transpile a multi-unit program (a file plus every module it `import`s) into ONE self-contained .c.
// Emits the normal shared-header + per-unit layout to temp siblings, then folds it into a single file:
// the header verbatim (its include guard + runtime includes stay), then each unit's body with its
// `#include "<name>.gen.h"` line dropped (the header is already inlined). Keeps `transpile`'s one-file
// contract while supporting the module system (a single translation unit for downstream tools).
int transpileProgramToSingleFile(const std::vector<SharedCompilationUnit>& units,
                                 const std::vector<std::string>& unitPaths,
                                 const std::string& outPath, bool emitLines,
                                 bool* externsMathH = nullptr,   // link hints (see emitProgramUnits) — a
                                 bool* externsNetWeb = nullptr,   // `build` caller needs these to append the
                                 bool* externsApp = nullptr,      // right link flags (-lm, --js-library, …);
                                 bool* externsGpu = nullptr,      // the `transpile` command passes none.
                                 bool* externsIsolate = nullptr)  // isolate seam -> native -lpthread
{
    std::string dir = dirName(outPath);
    std::string stem = stripExtension(baseName(outPath));
    std::string headerName = stem + ".gen.h";
    std::string headerPath = dir + "/" + headerName;
    std::vector<std::string> cPaths;
    for (size_t i = 0; i < units.size(); ++i)
        cPaths.push_back(dir + "/" + stem + "__u" + std::to_string(i) + ".c.tmp");
    if (emitProgramUnits(units, unitPaths, headerPath, headerName, cPaths, emitLines,
                         externsMathH, externsNetWeb, externsApp, externsGpu, externsIsolate) != 0) return 1;

    std::ofstream out(outPath);
    if (!out) { fprintf(stderr, "kama: error: cannot write '%s'\n", outPath.c_str()); return 1; }
    { std::ifstream h(headerPath); out << h.rdbuf(); }
    out << "\n";
    std::string incLine = "#include \"" + headerName + "\"";
    for (auto& cp : cPaths) {
        std::ifstream u(cp);
        std::string line;
        while (std::getline(u, line)) {
            if (line.find(incLine) != std::string::npos) continue;   // header already inlined above
            out << line << "\n";
        }
    }
    out.close();
    remove(headerPath.c_str());
    for (auto& cp : cPaths) remove(cp.c_str());
    return 0;
}

int runCmd(const std::string& cmd)
{
    int rc = system(cmd.c_str());
    if (rc == -1) return 1;
#if defined(_WIN32)
    return rc;                 // Windows: system() returns the child's exit code directly
#else
    return WEXITSTATUS(rc);    // POSIX: extract it from the wait status (<sys/wait.h>)
#endif
}

// Fetch `version` ("" = latest) into the versioned store via the canonical installer, which re-detects a C
// compiler (slim vs bundled zig) so the flavor stays consistent. `makeDefault` tells the installer to also
// refresh the PATH selector + the global default to that version (the installer always sets the default on a
// first-ever install regardless). Used by `kama update` (makeDefault) and `kama toolchain install` (not).
int runInstaller(const std::string& version, bool makeDefault)
{
#ifdef _WIN32
    std::string env;
    if (!version.empty()) env += "$env:KAMA_VERSION='" + version + "'; ";
    if (makeDefault)      env += "$env:KAMA_SET_DEFAULT='1'; ";
    std::string cmd = "powershell -NoProfile -Command \"" + env + "irm https://kama-lang.org/install.ps1 | iex\"";
#else
    std::string env;
    if (!version.empty()) env += "KAMA_VERSION=" + version + " ";
    if (makeDefault)      env += "KAMA_SET_DEFAULT=1 ";
    std::string cmd = env + "curl -fsSL https://kama-lang.org/install.sh | sh";
#endif
    return runCmd(cmd);   // the installer prints old->new; verify with `kama --version`
}

// `kama update [--version vX.Y.Z]` — install the latest (or a pinned version) and make it the global default.
int cmdUpdate(const std::string& pinned) { return runInstaller(pinned, /*makeDefault=*/true); }

// Run `cmd` and capture its stdout, trimmed of trailing newlines; *exitCode (if given) receives the
// child's exit status. "" if the process can't be spawned. This is the one place the driver needs a
// subprocess's OUTPUT (a hasher's hex digest, a git commit), not just its status like runCmd.
std::string runCmdCapture(const std::string& cmd, int* exitCode = nullptr)
{
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) { if (exitCode) *exitCode = -1; return ""; }
    std::string out; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
#ifdef _WIN32
    int rc = _pclose(p);
#else
    int st = pclose(p); int rc = (st == -1) ? -1 : WEXITSTATUS(st);
#endif
    if (exitCode) *exitCode = rc;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// Cross-platform recursive delete of `p` (staging cleanup / dedup). Same shell-out as cmdInstall's
// view rebuild.
static std::string rmRfCmd(const std::string& p)
{
#ifdef _WIN32
    return "cmd /c rmdir /s /q \"" + p + "\" 2>nul";
#else
    return "rm -rf \"" + p + "\"";
#endif
}

// A full git object name: exactly 40 hex chars. Distinguishes a pinned commit sha (fetch the object
// directly — `--depth 1 --branch` can't ride a sha) from a tag/branch rev. Short shas are NOT accepted:
// the lock always stores the full 40-char commit, and a shorter manifest rev harmlessly rides the branch
// path and fails loudly as an unknown ref — the right nudge to pin fully.
static bool isSha1Hex(const std::string& s)
{
    if (s.size() != 40) return false;
    for (char c : s) if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return false;
    return true;
}

// Pull the digest out of a hasher's output: sha256sum/shasum print "<64hex>  <file>"; certutil prints
// a header line, then the digest (older versions space-separate the bytes), then a status line. Scan
// for the first run of >=64 hex chars (spaces within a line don't break the run), take exactly 64.
static std::string firstSha256Hex(const std::string& s)
{
    auto isHex = [](char c){ return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
    std::string run;
    for (char c : s) {
        if (isHex(c)) { run += c; if (run.size() == 64) return run; }
        else if (c == ' ' || c == '\t') continue;   // certutil may split the digest with spaces
        else run.clear();
    }
    return "";
}

// sha256 of one file's bytes, as "sha256-<hex>". Shells out to the platform hasher — no vendored crypto
// or HTTP anywhere in the package manager: the same subprocess model as git/curl/tar, and the hasher
// CLI ships on every target next to them. "" if no hasher is available (caller hard-errors).
std::string sha256Of(const std::string& path)
{
    std::string q = "\"" + path + "\"";
    std::vector<std::string> cmds;
#ifdef _WIN32
    cmds.push_back("certutil -hashfile " + q + " SHA256");
#else
    cmds.push_back("sha256sum " + q + " 2>/dev/null");    // Linux / container
    cmds.push_back("shasum -a 256 " + q + " 2>/dev/null"); // macOS
#endif
    for (auto& c : cmds) {
        int rc = 0; std::string hex = firstSha256Hex(runCmdCapture(c, &rc));
        if (rc == 0 && hex.size() == 64) return "sha256-" + hex;
    }
    return "";
}

// ---- signing (M3.2a): SSHSIG via ssh-keygen -Y ------------------------------------------------
// A fixed SSHSIG namespace (a signature is bound to it, so a kama signature can't be replayed elsewhere).
static const char* kSigNamespace = "kama-registry";

// Is `ssh-keygen` on PATH? Signing/verification skip gracefully when it's absent (like git/curl/sha256).
static bool hasSshKeygen()
{
    int rc = 0; runCmdCapture("command -v ssh-keygen 2>/dev/null", &rc); return rc == 0;
}

// Sign `file` with the SSH private key `keyPath` (`ssh-keygen -Y sign` writes `<file>.sig`). On success
// fills `sigOut` (the armored SSHSIG) + `pubKeyOut` (the signer public key line, from `<keyPath>.pub`).
static bool sshSign(const std::string& file, const std::string& keyPath,
                    std::string& sigOut, std::string& pubKeyOut, std::string& err)
{
    std::string sigfile = file + ".sig";
    runCmd(rmRfCmd(sigfile));
    int rc = runCmd("ssh-keygen -Y sign -f \"" + keyPath + "\" -n " + kSigNamespace + " \"" + file + "\" >/dev/null 2>&1");
    if (rc != 0) { err = "ssh-keygen -Y sign failed (is the key '" + keyPath + "' an SSH private key?)"; return false; }
    { std::ifstream f(sigfile, std::ios::binary); sigOut.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
    { std::ifstream f(keyPath + ".pub", std::ios::binary); pubKeyOut.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
    runCmd(rmRfCmd(sigfile));
    // trim a trailing newline on the public key line (keeps the index tidy)
    while (!pubKeyOut.empty() && (pubKeyOut.back() == '\n' || pubKeyOut.back() == '\r')) pubKeyOut.pop_back();
    if (sigOut.empty()) { err = "ssh-keygen produced no signature"; return false; }
    return true;
}

// Verify that `signature` is a valid SSHSIG over `file`'s bytes (`ssh-keygen -Y check-novalidate` — a
// cryptographic check against the signature's embedded key; a configured trust set is a later M3.2 slice).
static bool sshVerify(const std::string& file, const std::string& signature, const std::string& stagingDir)
{
    std::string sigTmp = stagingDir + ".sig";
    { std::ofstream o(sigTmp, std::ios::binary); if (!o) return false; o << signature; }
    int rc = runCmd("ssh-keygen -Y check-novalidate -n " + std::string(kSigNamespace) +
                    " -s \"" + sigTmp + "\" < \"" + file + "\" >/dev/null 2>&1");
    runCmd(rmRfCmd(sigTmp));
    return rc == 0;
}
// -----------------------------------------------------------------------------------------------

// Collect every file under `root` as paths RELATIVE to root (recursively), for a deterministic tree hash.
static void collectFilesRel(const std::string& root, const std::string& rel, std::vector<std::string>& out)
{
    std::string dir = rel.empty() ? root : root + "/" + rel;
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string childRel = rel.empty() ? n : rel + "/" + n;
            if (dirExists(root + "/" + childRel)) collectFilesRel(root, childRel, out);
            else out.push_back(childRel);
        }
        closedir(d);
    }
}

// The store identity: sha256 of the canonical unpacked source TREE (not the raw clone/tarball — git
// adds .git/, tarballs vary in wrapper/compression). Sorted files, each hashed as `relpath\0` + bytes.
// Built in C++ (robust to newlines-in-filenames, unlike a find|cat pipe) into a temp, then hashed once.
std::string treeHashOf(const std::string& dir)
{
    std::vector<std::string> files;
    collectFilesRel(dir, "", files);
    std::sort(files.begin(), files.end());
    std::string tmp = dir + ".hashinput";
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return "";
    for (auto& rel : files) {
        out.write(rel.data(), rel.size()); out.put('\0');
        std::ifstream f(dir + "/" + rel, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        out.write(bytes.data(), bytes.size());
    }
    out.close();
    std::string h = sha256Of(tmp);
    remove(tmp.c_str());
    return h;
}

// The kama home root: `~/.kama` (`%USERPROFILE%\.kama` on Windows). Holds the versioned toolchains
// (`versions/<v>/`), the PATH selector (`bin/kama`), the global-default record (`default`), and the shared
// package store (`store/`). NOT overridable at runtime — a per-version toolchain must resolve its own
// support dirs exe-relative, never via an ambient env var. (`KAMA_HOME` is only the *installer's* prefix.)
std::string kamaHome()
{
#ifdef _WIN32
    if (const char* u = getenv("USERPROFILE")) return std::string(u) + "/.kama";
#else
    if (const char* h = getenv("HOME")) return std::string(h) + "/.kama";
#endif
    return ".kama";   // degenerate fallback (no HOME): project-local, still functional
}

// The shared content-addressed store root. `~/.kama/store` (user-mutable, install-independent) with a
// `KAMA_STORE` override for isolation (the guard test points it at a tmp dir so it never touches the
// real store — and NOT KAMA_HOME, which is only the installer prefix).
std::string storeDir()
{
    if (const char* s = getenv("KAMA_STORE")) return s;
    return kamaHome() + "/store";
}

// M1 toolchain-management paths, all under kamaHome(). `versions/<v>/` is one self-contained install;
// `bin/kama` is the PATH selector (a copy of the default version's binary); `default` records the global
// default version (one line). These are shared across every installed toolchain.
std::string versionsDir()        { return kamaHome() + "/versions"; }
std::string defaultVersionFile() { return kamaHome() + "/default"; }
std::string selectorPath()       { return kamaHome() + "/bin/kama"; }
std::string versionDir(const std::string& v) { return versionsDir() + "/" + v; }
std::string versionBin(const std::string& v) { return versionDir(v) + "/bin/kama"; }

// M2.1: fetch a `git`/`url` dependency into the content-addressed store, verify sha256 integrity, and
// fill `out` (lock entry) + `storePath` (target for the view symlink). Staging is atomic — nothing
// enters `store/<name>-<hash>` until the tree hash is known, so a killed fetch leaves no half entry.
static bool fetchToStore(const std::string& name, const DepSpec& d,
                         LockEntry& out, std::string& storePath, std::string& err)
{
    std::string store = storeDir();
    if (!makeDirs(store)) { err = "cannot create store at " + store; return false; }
    std::string staging = store + "/.tmp-" + std::to_string((long)getpid()) + "-" + storeLabel(name);
    runCmd(rmRfCmd(staging));   // clear any stale staging from a prior crash

    std::string commit, lockIntegrity;
    if (!d.git.empty()) {
        if (isSha1Hex(d.rev)) {
            // Raw commit sha: --depth 1 --branch can't ride a sha, so init + fetch the object directly.
            // This is also the mechanism the lock-honoring re-fetch uses (by the recorded `commit`), so a
            // branch-pinned dep reproduces its exact tree offline.
            if (runCmd("git init -q \"" + staging + "\"") != 0 ||
                runCmd("git -C \"" + staging + "\" remote add origin \"" + d.git + "\"") != 0 ||
                runCmd("git -C \"" + staging + "\" fetch -q --depth 1 origin " + d.rev) != 0 ||
                runCmd("git -C \"" + staging + "\" checkout -q FETCH_HEAD") != 0) {
                err = "git fetch of pinned commit " + d.rev + " failed for '" + name + "' (" + d.git + ")";
                runCmd(rmRfCmd(staging)); return false;
            }
            commit = d.rev;   // we asked for exactly this object; FETCH_HEAD is it (no rev-parse needed)
        } else {
            // tag/branch pin via a shallow clone.
            std::string branch = d.rev.empty() ? "" : (" --branch \"" + d.rev + "\"");
            if (runCmd("git clone --depth 1" + branch + " \"" + d.git + "\" \"" + staging + "\" 2>/dev/null") != 0) {
                err = "git clone failed for '" + name + "' (" + d.git + ")"; runCmd(rmRfCmd(staging)); return false;
            }
            commit = runCmdCapture("git -C \"" + staging + "\" rev-parse HEAD");
        }
        runCmd(rmRfCmd(staging + "/.git"));   // exclude VCS metadata from the canonical tree
    } else {
        std::string tgz = staging + ".tgz";
        if (runCmd("curl -fsSL \"" + d.url + "\" -o \"" + tgz + "\"") != 0) {
            err = "download failed for '" + name + "' (" + d.url + ")"; runCmd(rmRfCmd(tgz)); return false;
        }
        std::string tarballHash = sha256Of(tgz);   // the artifact identity a re-download can re-verify
        if (tarballHash.empty()) { err = "cannot hash tarball for '" + name + "'"; runCmd(rmRfCmd(tgz)); return false; }
        if (!d.integrity.empty() && d.integrity != tarballHash) {
            err = "integrity mismatch for '" + name + "': expected " + d.integrity + ", got " + tarballHash;
            runCmd(rmRfCmd(tgz)); return false;   // hard fail — nothing enters the store
        }
        // M3.2a: verify the tarball signature while the tgz still exists (SSHSIG is over its bytes).
        // Enforced under `--verify`; otherwise warn-only. Skips gracefully if ssh-keygen is absent.
        if (!d.signature.empty()) {
            if (!hasSshKeygen()) {
                if (g_verifySignatures) { err = "cannot verify '" + name + "': ssh-keygen not available"; runCmd(rmRfCmd(tgz)); return false; }
            } else if (!sshVerify(tgz, d.signature, staging)) {
                if (g_verifySignatures) {
                    err = "signature verification failed for '" + name + "' (" + d.url + ")";
                    runCmd(rmRfCmd(tgz)); return false;
                }
                fprintf(stderr, "kama: warning: signature check failed for '%s' — continuing (verification "
                        "is not enforced; pass --verify to require it)\n", name.c_str());
            }
        } else if (g_verifySignatures) {
            err = "'" + name + "' is unsigned but --verify requires a signature";
            runCmd(rmRfCmd(tgz)); return false;
        }
        lockIntegrity = tarballHash;   // trust-on-first-use when the manifest omits `integrity`
        if (!makeDirs(staging)) { err = "cannot stage '" + name + "'"; runCmd(rmRfCmd(tgz)); return false; }
        if (runCmd("tar -xzf \"" + tgz + "\" -C \"" + staging + "\" --strip-components=1") != 0) {
            err = "cannot unpack '" + name + "'"; runCmd(rmRfCmd(staging)); runCmd(rmRfCmd(tgz)); return false;
        }
        runCmd(rmRfCmd(tgz));
    }

    std::string hash = treeHashOf(staging);   // "sha256-<hex>" — the store dir name + dedup key
    if (hash.empty()) { err = "cannot hash '" + name + "' (is sha256sum/shasum available?)"; runCmd(rmRfCmd(staging)); return false; }
    if (!d.git.empty()) lockIntegrity = hash;   // git has no artifact; the tree hash IS its integrity

    std::string finalDir = store + "/" + storeLabel(name) + "-" + hash.substr(sizeof("sha256-") - 1);
    if (dirExists(finalDir)) runCmd(rmRfCmd(staging));   // dedup: identical content already stored
    else if (rename(staging.c_str(), finalDir.c_str()) != 0) {
        err = "cannot finalize store entry for '" + name + "'"; runCmd(rmRfCmd(staging)); return false;
    }

    storePath = finalDir;
    out.source = d.git.empty() ? "url" : "git";
    if (d.git.empty()) out.url = d.url;
    else { out.git = d.git; out.rev = d.rev; out.commit = commit; }
    out.integrity = lockIntegrity;
    out.treeHash  = hash;   // the store dir key; == integrity for git, differs for url (tarball integrity)
    return true;
}

// Resolve ONE dependency node → `out` (lock entry) + `storePath` (view-link target). Honors `oldLock`
// (cargo model): a git/url dep whose manifest spec is unchanged reuses the pinned identity — a warm store
// links with ZERO fetch (offline), a cold store re-fetches by the recorded `commit`/`integrity` and asserts
// the tree hash still matches. A new/changed spec resolves fresh via fetchToStore. path deps always relink
// the local dir. The caller sets out.dev / out.dependencies.
static bool resolveOne(const std::string& name, const DepSpec& spec, const std::string& base,
                       const std::map<std::string, LockEntry>& oldLock,
                       LockEntry& out, std::string& storePath, std::string& err)
{
    if (!spec.path.empty()) {
        std::string target = absolutePath(base + "/" + spec.path);
        if (!dirExists(target)) { err = "path dependency '" + name + "' not found at " + target; return false; }
        out = LockEntry(); out.source = "path"; out.path = spec.path; storePath = target;
        return true;
    }
    auto it = oldLock.find(name);
    if (it != oldLock.end()) {
        const LockEntry& L = it->second;
        bool unchanged =
            (!spec.git.empty() && L.source == "git" && L.git == spec.git && L.rev == spec.rev) ||
            // a registry dep reaches here already pinned to its resolved tarball URI (`spec.url`), so it
            // reuses the url identity — its "registry" source is preserved by `out = L` below.
            (!spec.url.empty() && (L.source == "url" || L.source == "registry") && L.url == spec.url &&
                (spec.integrity.empty() || spec.integrity == L.integrity));
        if (unchanged) {
            std::string key = L.treeHash.empty() ? L.integrity : L.treeHash;   // the store dir hash
            std::string finalDir;
            if (key.size() > sizeof("sha256-") - 1)
                finalDir = storeDir() + "/" + storeLabel(name) + "-" + key.substr(sizeof("sha256-") - 1);
            if (!finalDir.empty() && dirExists(finalDir)) {   // offline hit — link the pinned tree, no fetch
                out = L; storePath = finalDir; return true;
            }
            // cold store — re-fetch by the pinned identity (git: the recorded commit sha via the sha path).
            DepSpec pinned;
            if (L.source == "git") { pinned.git = L.git; pinned.rev = L.commit; }
            else                   { pinned.url = L.url; pinned.integrity = L.integrity; }
            LockEntry fetched;
            if (!fetchToStore(name, pinned, fetched, storePath, err)) return false;
            if (!key.empty() && fetched.treeHash != key) {
                err = "lock integrity drift for '" + name + "': lock has " + key +
                      ", re-fetch produced " + fetched.treeHash; return false;
            }
            out = L; return true;   // keep the lock's recorded fields verbatim
        }
    }
    return fetchToStore(name, spec, out, storePath, err);   // new dep or changed spec — fresh
}

static std::string semVerStr(const SemVer& v)
{
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

// Enumerate a git repo's SemVer tags (network-free against file:// repos). `git ls-remote --tags <url>`
// prints "<sha>\trefs/tags/<tag>"; an annotated tag ALSO emits a "<tag>^{}" deref line — strip the
// "^{}" and dedupe so a tag isn't counted twice (the classic silent double-candidate bug). Non-SemVer
// and pre-release tags are silently skipped (not candidates). Returns false + `err` on command failure
// or zero parseable version tags.
static bool gitVersionTags(const std::string& gitUrl,
                           std::vector<std::pair<SemVer, std::string>>& out, std::string& err)
{
    int rc = 0;
    std::string res = runCmdCapture("git ls-remote --tags \"" + gitUrl + "\" 2>/dev/null", &rc);
    if (rc != 0) { err = "cannot list tags of git repo '" + gitUrl + "'"; return false; }
    std::set<std::string> seen;
    std::istringstream ss(res);
    std::string line;
    const std::string prefix = "refs/tags/";
    while (std::getline(ss, line)) {
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string ref = line.substr(tab + 1);
        if (ref.compare(0, prefix.size(), prefix) != 0) continue;
        std::string tag = ref.substr(prefix.size());
        if (tag.size() >= 3 && tag.compare(tag.size() - 3, 3, "^{}") == 0)
            tag = tag.substr(0, tag.size() - 3);          // annotated-tag deref
        if (!seen.insert(tag).second) continue;           // dedupe
        SemVer v;
        if (parseSemVer(tag, v)) out.push_back({v, tag});
    }
    if (out.empty()) { err = "git repo '" + gitUrl + "' has no SemVer version tags"; return false; }
    return true;
}

// The highest tag satisfying `req`. Returns false when none matches (the caller turns that into the
// right message: a disjoint conflict, or "no tag satisfies the range").
static bool selectHighestTag(const std::vector<std::pair<SemVer, std::string>>& tags,
                             const VersionReq& req, SemVer& vOut, std::string& tagOut)
{
    bool found = false;
    for (auto& t : tags) {
        if (!satisfies(req, t.first)) continue;
        if (!found || cmpSemVer(t.first, vOut) > 0) { vOut = t.first; tagOut = t.second; found = true; }
    }
    return found;
}

// ---- registry (M3.1) -------------------------------------------------------------------------
// The built-in default registry base URI. A compile constant (overridable per-project by a
// `registries.default`, M3.1b). Empty until M3.3 wires a live host — so an unconfigured, unscoped
// registry dep errors clearly rather than silently reaching a dead URL.
static const char* kDefaultRegistry = "";

// Join a registry base and a relative path into one URI. An absolute `rel` (has a scheme, or is an
// absolute path) is returned unchanged, so an index may point its tarballs at a different host.
static std::string joinUri(const std::string& base, const std::string& rel)
{
    if (rel.find("://") != std::string::npos || (!rel.empty() && rel[0] == '/')) return rel;
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b + "/" + rel;
}

// One published version in a package's registry index. `dependencies` are recorded in the index (for
// protocol conformance + other clients) but the resolver reads the fetched tarball's own manifest, so
// they are parsed-and-ignored here. `signature`/`key` are M3.2a (absent in an unsigned registry).
struct IndexEntry {
    std::string version, integrity, tarball, signature, key;
};

// Parse a `<base>/<name>/index.json` document (writeIndex's schema) into version entries. A dedicated
// hand-parser mirroring LockReader: minimal + tolerant (unknown keys skipped, every field optional).
struct IndexReader {
    const std::string& s; size_t i = 0; std::string err;
    IndexReader(const std::string& src) : s(src) {}
    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; }
    bool fail(const char* m) { if (err.empty()) err = m; return false; }
    bool str(std::string& out) {
        ws(); if (i >= s.size() || s[i] != '"') return fail("expected a string");
        ++i; out.clear();
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c == '\\' && i < s.size()) { char e = s[i++];
                switch (e) { case 'n':c='\n';break; case 't':c='\t';break; case 'r':c='\r';break;
                             case '"':c='"';break; case '\\':c='\\';break; case '/':c='/';break; default:c=e; } }
            out.push_back(c);
        }
        if (i >= s.size()) return fail("unterminated string"); ++i; return true;
    }
    bool skipValue() {
        ws(); if (i >= s.size()) return fail("unexpected end of index");
        char c = s[i];
        if (c == '"') { std::string t; return str(t); }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']'; int depth = 0; bool inStr = false;
            while (i < s.size()) { char d = s[i++];
                if (inStr)          { if (d == '\\' && i < s.size()) ++i; else if (d == '"') inStr = false; }
                else if (d == '"')    inStr = true;
                else if (d == open)   ++depth;
                else if (d == close){ if (--depth == 0) return true; } }
            return fail("unbalanced brackets in index");
        }
        while (i < s.size() && s[i]!=','&&s[i]!='}'&&s[i]!=']'&&s[i]!=' '&&s[i]!='\t'&&s[i]!='\n'&&s[i]!='\r') ++i;
        return true;
    }
    bool version(IndexEntry& e) {
        ws(); if (i >= s.size() || s[i] != '{') return fail("an index version must be an object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string k; if (!str(k)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in an index version"); ++i; ws();
            if      (k == "version")   { if (!str(e.version))   return false; }
            else if (k == "integrity") { if (!str(e.integrity)) return false; }
            else if (k == "tarball")   { if (!str(e.tarball))   return false; }
            else if (k == "signature") { if (!str(e.signature)) return false; }
            else if (k == "key")       { if (!str(e.key))       return false; }
            else if (!skipValue()) return false;   // dependencies / unknown (forward-compat)
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in an index version");
        }
        return true;
    }
    bool parse(std::vector<IndexEntry>& out) {
        ws(); if (i >= s.size() || s[i] != '{') return fail("index must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string key; if (!str(key)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a key"); ++i;
            if (key == "versions") {
                ws(); if (i >= s.size() || s[i] != '[') return fail("`versions` must be an array");
                ++i; ws();
                if (i < s.size() && s[i] == ']') ++i;
                else while (true) {
                    IndexEntry e; if (!version(e)) return false; out.push_back(e); ws();
                    if (i < s.size() && s[i] == ',') { ++i; continue; }
                    if (i < s.size() && s[i] == ']') { ++i; break; }
                    return fail("expected ',' or ']' in `versions`");
                }
            } else if (!skipValue()) return false;   // name / future keys
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' at top level");
        }
        return true;
    }
};

// Fetch + parse a package's registry index (network-free against a file:// base; curl speaks both).
// Returns false + `err` on fetch/parse failure or an empty index.
static bool fetchRegistryIndex(const std::string& base, const std::string& name,
                               std::vector<IndexEntry>& out, std::string& err)
{
    std::string url = joinUri(base, name + "/index.json");
    int rc = 0;
    std::string body = runCmdCapture("curl -fsSL \"" + url + "\" 2>/dev/null", &rc);
    if (rc != 0 || body.empty()) { err = "cannot fetch registry index for '" + name + "' from " + url; return false; }
    IndexReader r(body);
    if (!r.parse(out)) { err = "malformed registry index for '" + name + "' (" + url + "): " + r.err; return false; }
    if (out.empty())   { err = "registry index for '" + name + "' (" + url + ") lists no versions"; return false; }
    return true;
}

// The highest index version satisfying `req`. Returns false when none matches.
static bool selectHighestIndex(const std::vector<IndexEntry>& entries, const VersionReq& req,
                               SemVer& vOut, IndexEntry& entryOut)
{
    bool found = false;
    for (auto& e : entries) {
        SemVer v; if (!parseSemVer(e.version, v)) continue;   // skip non-SemVer / pre-release entries
        if (!satisfies(req, v)) continue;
        if (!found || cmpSemVer(v, vOut) > 0) { vOut = v; entryOut = e; found = true; }
    }
    return found;
}
// ---------------------------------------------------------------------------------------------

// The resolver core: a two-phase transitive BFS from the ROOT manifest, honoring `oldLock` per node. Phase 1
// (prod) seeds `dependencies` and follows each fetched package's own `dependencies`; phase 2 (dev) seeds the
// root's `dev-dependencies` (a package needed in prod is never demoted to dev — prod wins) and follows their
// prod deps only — dev is strictly non-transitive. Rebuilds `.kama/deps` (prod) + `.kama/dev-deps` (dev)
// from scratch and writes a deterministic kama.lock. Shared by `pkg install` (oldLock = the current lock)
// and `pkg update` (oldLock = the current lock minus the pins being refreshed). No arbitrary code ever runs.
static int resolveProject(const std::string& base, const std::map<std::string, LockEntry>& oldLock)
{
    std::string manifest = base + "/kama.json";
    std::map<std::string, DepSpec> deps, devDeps; std::string err;
    RegConfig regCfg;
    if (!loadManifestDeps(manifest, deps, err, &devDeps, &regCfg)) {
        fprintf(stderr, "kama: %s: %s\n", manifest.c_str(), err.c_str()); return 2;
    }

    // `kama.local.json` (M5.3): a gitignored, dev-local sibling that layers INSTALL-path overrides over
    // `kama.json` — a local `registries` config (merged now, so resolution routes through it) and dep
    // `overrides` (applied AFTER the canonical lock is written, below). Local-only by construction: the
    // registry merge is lock-safe because the lock pins integrity not URI, and the dep overrides never
    // touch the lock at all — so CI (which has no `kama.local.json`) reproduces the identical build.
    std::map<std::string, DepSpec> overrides;
    std::string localManifest = base + "/kama.local.json";
    if (std::ifstream(localManifest).good()) {
        RegConfig localReg; std::string lerr;
        if (!loadManifestLocalInstall(localManifest, overrides, localReg, lerr)) {
            fprintf(stderr, "kama: %s: %s\n", localManifest.c_str(), lerr.c_str()); return 2;
        }
        regCfg.applyLocal(localReg);
    }

    std::string viewDir    = base + "/.kama/deps";
    std::string devViewDir = base + "/.kama/dev-deps";

    // Range deps (git+version) select the highest matching tag. Because the BFS resolves each node on
    // first sight, a *later*, tighter requestor of the same name can invalidate an already-fetched tag.
    // We handle that by RESTARTING resolution with the now-known constraint pre-seeded — bounded, since
    // each restart only ever tightens a name's range (so its chosen version monotonically decreases).
    // Tag enumeration is memoized across attempts (`tagCache`); a warm `oldLock` avoids it entirely.
    std::map<std::string, VersionReq> seeded;                                  // constraints carried across restarts
    std::map<std::string, std::vector<std::pair<SemVer, std::string>>> tagCache;   // git url -> parseable version tags
    std::map<std::string, std::vector<IndexEntry>> regCache;                       // "base\nname" -> registry index versions
    const int RESTART = -1;
    const int MAX_ATTEMPTS = 256;   // defensive: names + versions are finite, so this is never reached

    for (int attempt = 0; ; ++attempt) {
        if (attempt >= MAX_ATTEMPTS) {
            fprintf(stderr, "kama install: version resolution did not converge (too many range conflicts)\n");
            return 1;
        }
        // Rebuild both views from scratch so they can never drift from the manifest/lock.
        runCmd(rmRfCmd(viewDir));
        runCmd(rmRfCmd(devViewDir));
        if (!makeDirs(viewDir)) { fprintf(stderr, "kama install: cannot create %s\n", viewDir.c_str()); return 1; }

        struct Req { std::string name; DepSpec spec; std::string requestor; bool dev; };
        std::map<std::string, DepSpec> chosen;        // name -> the one resolved (tag-pinned) spec
        std::map<std::string, std::string> chosenBy;  // name -> first requestor (conflict diagnostics)
        std::map<std::string, std::string> chosenVer; // range deps: name -> resolved concrete "x.y.z"
        std::map<std::string, VersionReq>  accReq;    // range deps: name -> intersected range so far
        std::map<std::string, std::string> reqStr;    // range deps: name -> first requestor's range text
        std::map<std::string, std::string> regBaseOf; // registry deps: name -> resolved registry base URI
        std::map<std::string, std::string> importUsedBy; // import (bare) name -> the scoped name that claimed it
        std::map<std::string, LockEntry> lock;        // output
        bool madeDevDir = false;

        // Enumerate a git repo's version tags once, memoized across attempts.
        auto ensureTags = [&](const std::string& gitUrl,
                              std::vector<std::pair<SemVer, std::string>>*& tags, std::string& terr) -> bool {
            auto it = tagCache.find(gitUrl);
            if (it == tagCache.end()) {
                std::vector<std::pair<SemVer, std::string>> t;
                if (!gitVersionTags(gitUrl, t, terr)) return false;
                it = tagCache.emplace(gitUrl, std::move(t)).first;
            }
            tags = &it->second; return true;
        };

        // The priority-ordered registry base chain a registry dep resolves through: an explicit
        // `registry` pin wins outright; else the `@scope` chain (for a scoped name), else the `default`
        // chain, else the built-in default. `default: false` drops the built-in (air-gapped). Empty =>
        // no registry configured (a hard error at the call site). The lock pins the integrity, not the
        // URI, so a scope can be re-pointed to a mirror in this config without changing what's fetched.
        auto registryBasesFor = [&](const std::string& name, const DepSpec& spec) -> std::vector<std::string> {
            if (!spec.registry.empty()) return { spec.registry };
            std::string sc = scopeOf(name);
            if (!sc.empty()) { auto it = regCfg.scopes.find(sc); if (it != regCfg.scopes.end()) return it->second; }
            if (regCfg.defaultDisabled) return {};
            if (!regCfg.defaultBases.empty()) return regCfg.defaultBases;
            if (kDefaultRegistry[0]) return { std::string(kDefaultRegistry) };
            return {};
        };

        // Fetch + parse a registry package's index once, memoized across attempts (by base+name).
        auto ensureIndex = [&](const std::string& base, const std::string& name,
                               std::vector<IndexEntry>*& idx, std::string& terr) -> bool {
            std::string key = base + "\n" + name;
            auto it = regCache.find(key);
            if (it == regCache.end()) {
                std::vector<IndexEntry> v;
                if (!fetchRegistryIndex(base, name, v, terr)) return false;
                it = regCache.emplace(key, std::move(v)).first;
            }
            idx = &it->second; return true;
        };

        // Select the highest version of `name` satisfying `req` from its source (git tags OR a registry
        // index). On success fills `sv` and one of {tag} (git) / {url,integrity,regBase} (registry). A
        // false return with `matched=false` means "no version satisfies"; with `terr` set means a hard
        // error (unreachable source / no configured registry).
        struct VerPick { std::string tag, url, integrity, regBase, signature, sigKey; };
        auto selectVersion = [&](const std::string& name, const DepSpec& spec, const VersionReq& req,
                                 bool& matched, SemVer& sv, VerPick& pick, std::string& terr) -> bool {
            matched = false;
            if (isRegistryDep(spec)) {
                std::vector<std::string> bases = registryBasesFor(name, spec);
                if (bases.empty()) {
                    terr = "no registry configured for '" + name + "' — set \"registry\" on the "
                           "dependency or a \"registries\" default"; return false;
                }
                // Priority order: the first base that HAS a satisfying version wins (a higher-priority
                // private registry shadows a lower-priority public one). A base whose index is missing or
                // lacks a satisfying version is skipped (not a hard error).
                for (const std::string& base : bases) {
                    std::vector<IndexEntry>* idx = nullptr; std::string ferr;
                    if (!ensureIndex(base, name, idx, ferr)) continue;
                    IndexEntry e;
                    if (selectHighestIndex(*idx, req, sv, e)) {
                        matched = true;
                        pick.url = joinUri(base, e.tarball); pick.integrity = e.integrity; pick.regBase = base;
                        pick.signature = e.signature; pick.sigKey = e.key;
                        break;
                    }
                }
                return true;
            }
            std::vector<std::pair<SemVer, std::string>>* tags = nullptr;
            if (!ensureTags(spec.git, tags, terr)) return false;
            matched = selectHighestTag(*tags, req, sv, pick.tag);
            return true;
        };

        auto drain = [&](std::deque<Req>& q) -> int {
            while (!q.empty()) {
                Req r = q.front(); q.pop_front();
                bool incomingRange = isRangeDep(r.spec) || isRegistryDep(r.spec);   // a versioned dep (git range or registry)

                auto ci = chosen.find(r.name);
                if (ci != chosen.end()) {
                    bool chosenRange = accReq.count(r.name) > 0;
                    if (chosenRange != incomingRange) {   // one is an exact/url/path pin, the other a range
                        fprintf(stderr, "kama install: dependency conflict on '%s': %s and %s mix an exact pin "
                                "and a version range — use the same form\n",
                                r.name.c_str(), chosenBy[r.name].c_str(), r.requestor.c_str());
                        return 1;
                    }
                    if (chosenRange) {
                        // Intersect the two ranges and re-select the highest tag satisfying both.
                        VersionReq req;
                        if (!parseVersionReq(r.spec.version, req)) {
                            fprintf(stderr, "kama install: dependency '%s' (required by %s) has an invalid "
                                    "version range \"%s\"\n", r.name.c_str(), r.requestor.c_str(), r.spec.version.c_str());
                            return 1;
                        }
                        VersionReq merged = intersect(accReq[r.name], req);
                        SemVer sv; VerPick pick; bool matched = false; std::string terr;
                        if (!selectVersion(r.name, r.spec, merged, matched, sv, pick, terr)) {
                            fprintf(stderr, "kama install: %s\n", terr.c_str()); return 1;
                        }
                        if (!matched) {
                            fprintf(stderr, "kama install: dependency conflict on '%s': %s requires \"%s\" and %s "
                                    "requires \"%s\" — no published version satisfies both\n",
                                    r.name.c_str(), chosenBy[r.name].c_str(), reqStr[r.name].c_str(),
                                    r.requestor.c_str(), r.spec.version.c_str());
                            return 1;
                        }
                        accReq[r.name] = merged;
                        if (semVerStr(sv) != chosenVer[r.name]) {   // chosen version no longer highest — restart
                            seeded[r.name] = merged;
                            return RESTART;
                        }
                        continue;   // chosen version still satisfies the tighter range → dedup
                    }
                    if (!sameSpec(ci->second, r.spec)) {
                        fprintf(stderr, "kama install: dependency conflict on '%s': %s and %s require different "
                                "sources (no version reconciliation yet — pin both to the same git/rev or url)\n",
                                r.name.c_str(), chosenBy[r.name].c_str(), r.requestor.c_str());
                        return 1;
                    }
                    continue;   // dedup (diamond / prod-wins-over-dev)
                }
                if (!r.spec.path.empty() && r.requestor != "<root manifest>") {
                    fprintf(stderr, "kama install: path dependency '%s' (required by %s) is only allowed at the "
                            "top level — a fetched package cannot reference a local path reproducibly\n",
                            r.name.c_str(), r.requestor.c_str());
                    return 1;
                }

                // First encounter of a versioned dep (git range or registry): fold in any seeded (restart)
                // constraint, then pin the highest satisfying version — from the lock if it still fits
                // (offline, no ls-remote / no index fetch), else from the source (git tags / registry index).
                if (incomingRange) {
                    bool isReg = isRegistryDep(r.spec);
                    VersionReq req;
                    if (!parseVersionReq(r.spec.version, req)) {
                        fprintf(stderr, "kama install: dependency '%s' (required by %s) has an invalid "
                                "version range \"%s\"\n", r.name.c_str(), r.requestor.c_str(), r.spec.version.c_str());
                        return 1;
                    }
                    auto sd = seeded.find(r.name);
                    if (sd != seeded.end()) req = intersect(req, sd->second);

                    SemVer sv; VerPick pick; bool reused = false;
                    std::vector<std::string> regBases = isReg ? registryBasesFor(r.name, r.spec) : std::vector<std::string>{};
                    if (isReg && regBases.empty()) {
                        fprintf(stderr, "kama install: no registry configured for '%s' (required by %s) — set "
                                "\"registry\" on the dependency or a \"registries\" default\n",
                                r.name.c_str(), r.requestor.c_str());
                        return 1;
                    }
                    auto lk = oldLock.find(r.name);
                    if (lk != oldLock.end() && !lk->second.version.empty()) {
                        SemVer lv;
                        // Registry offline reuse requires the locked base to still be an active candidate —
                        // a re-pointed scope drops out of the chain and forces a fresh resolve (+ the
                        // confusion cross-check below).
                        bool baseStillActive = std::find(regBases.begin(), regBases.end(), lk->second.registry) != regBases.end();
                        bool sameSrc = isReg ? (lk->second.source == "registry" && baseStillActive)
                                             : (lk->second.source == "git"      && lk->second.git == r.spec.git);
                        if (sameSrc && parseSemVer(lk->second.version, lv) && satisfies(req, lv)) {
                            sv = lv; reused = true;   // honor the lock — no ls-remote / no index fetch
                            if (isReg) { pick.url = lk->second.url; pick.integrity = lk->second.integrity; pick.regBase = lk->second.registry; }
                            else       { pick.tag = lk->second.rev; }
                        }
                    }
                    if (!reused) {
                        bool matched = false; std::string terr;
                        if (!selectVersion(r.name, r.spec, req, matched, sv, pick, terr)) {
                            fprintf(stderr, "kama install: %s\n", terr.c_str()); return 1;
                        }
                        if (!matched) {
                            fprintf(stderr, "kama install: dependency '%s' (required by %s): no %s version "
                                    "satisfies \"%s\"\n", r.name.c_str(), r.requestor.c_str(),
                                    isReg ? "registry" : "git tag", r.spec.version.c_str());
                            return 1;
                        }
                        // Dependency-confusion guard: a name@version is a fixed content identity. If the
                        // lock already pinned this exact version, a freshly-resolved DIFFERENT integrity
                        // (e.g. a mirror serving different bytes under the same version) is a hard error.
                        if (isReg && lk != oldLock.end() && lk->second.source == "registry"
                                && lk->second.version == semVerStr(sv) && !lk->second.integrity.empty()
                                && lk->second.integrity != pick.integrity) {
                            fprintf(stderr, "kama install: dependency confusion on '%s'@%s: the lock pinned "
                                    "integrity %s (from %s) but %s serves %s — refusing to install different "
                                    "bytes under the same version\n",
                                    r.name.c_str(), semVerStr(sv).c_str(), lk->second.integrity.c_str(),
                                    lk->second.registry.c_str(), pick.regBase.c_str(), pick.integrity.c_str());
                            return 1;
                        }
                    }
                    accReq[r.name] = req; reqStr[r.name] = r.spec.version; chosenVer[r.name] = semVerStr(sv);
                    if (isReg) {                      // pin: resolveOne now treats it as an exact url dep
                        r.spec.url = pick.url; r.spec.integrity = pick.integrity; regBaseOf[r.name] = pick.regBase;
                        r.spec.signature = pick.signature; r.spec.sigKey = pick.sigKey;   // verified at fetch (M3.2a)
                    } else {
                        r.spec.rev = pick.tag;        // pin: resolveOne now treats it as an exact git dep (tag → commit)
                    }
                }

                chosen[r.name] = r.spec; chosenBy[r.name] = r.requestor;

                LockEntry e; std::string storePath, ferr;
                if (!resolveOne(r.name, r.spec, base, oldLock, e, storePath, ferr)) {
                    fprintf(stderr, "kama install: %s\n", ferr.c_str()); return 1;
                }
                if (incomingRange) e.version = chosenVer[r.name];   // record what the range resolved to
                if (regBaseOf.count(r.name)) {                       // a registry dep: overwrite the url source
                    e.source = "registry"; e.registry = regBaseOf[r.name];
                }
                e.dev = r.dev;
                // A scoped `@acme/foo` imports under its bare last segment (`foo`). Two distinct packages
                // resolving to the same bare name would collide on one view link — a hard error (alias one).
                std::string importName = importNameOf(r.name);
                auto iu = importUsedBy.find(importName);
                if (iu != importUsedBy.end() && iu->second != r.name) {
                    fprintf(stderr, "kama install: import-name collision on '%s': both '%s' and '%s' import as "
                            "'%s' — two scopes cannot expose the same name\n",
                            importName.c_str(), iu->second.c_str(), r.name.c_str(), importName.c_str());
                    return 1;
                }
                importUsedBy[importName] = r.name;
                if (r.dev && !madeDevDir) {
                    if (!makeDirs(devViewDir)) { fprintf(stderr, "kama install: cannot create %s\n", devViewDir.c_str()); return 1; }
                    madeDevDir = true;
                }
                if (!linkDir(storePath, (r.dev ? devViewDir : viewDir) + "/" + importName)) {
                    fprintf(stderr, "kama install: cannot link dependency '%s'\n", r.name.c_str()); return 1;
                }
                // Read THIS package's own prod deps → record (serialized so the build never re-reads) + enqueue.
                std::string childManifest = storePath + "/kama.json";
                std::vector<std::string> directNames;
                if (fileExists(childManifest)) {
                    std::map<std::string, DepSpec> childDeps; std::string cerr;
                    if (!loadManifestDeps(childManifest, childDeps, cerr)) {
                        fprintf(stderr, "kama install: %s: %s\n", childManifest.c_str(), cerr.c_str()); return 2;
                    }
                    for (auto& ck : childDeps) { directNames.push_back(ck.first);
                        q.push_back({ck.first, ck.second, r.name, r.dev}); }
                    std::sort(directNames.begin(), directNames.end());   // deterministic dependencies[] order
                }
                e.dependencies = directNames;
                lock[r.name] = e;
            }
            return 0;
        };

        std::deque<Req> prodQ, devQ;
        for (auto& kv : deps)    prodQ.push_back({kv.first, kv.second, "<root manifest>", false});
        for (auto& kv : devDeps) devQ.push_back({kv.first, kv.second, "<root manifest>", true});
        int rc = drain(prodQ);                       // phase 1 (prod) drains fully before phase 2 → prod wins
        if (rc == 0) rc = drain(devQ);               // phase 2 (dev)
        if (rc == RESTART) continue;                 // a tighter range surfaced — re-resolve with it seeded
        if (rc != 0) return rc;

        if (!writeLockFile(base + "/kama.lock", lock)) {
            fprintf(stderr, "kama install: cannot write %s/kama.lock\n", base.c_str()); return 1;
        }

        // Dep path-overrides (M5.3, patch-style): the canonical lock is now written and untouched. Redirect
        // ONLY the materialized view for each overridden dep to a local path, so this dev's build compiles
        // against local code. Because nothing here writes the lock, a committed `kama.lock` stays canonical
        // and CI (no `kama.local.json`) reproduces the published resolution exactly. The overridden dep must
        // still be a declared, canonically-resolvable dependency (a drop-in replacement); its NEW transitive
        // deps, if any, are not re-followed (v1 patch semantics — replace-style is a later milestone).
        for (auto& ov : overrides) {
            const std::string& name = ov.first;
            const DepSpec& spec = ov.second;
            if (spec.path.empty()) {
                fprintf(stderr, "kama: override '%s' in kama.local.json must specify a \"path\" "
                        "(only local path-overrides are supported)\n", name.c_str()); return 1;
            }
            bool isDev = deps.count(name) == 0 && devDeps.count(name) != 0;
            if (!deps.count(name) && !isDev) {
                fprintf(stderr, "kama: override '%s' in kama.local.json is not a dependency of this "
                        "project — declare it in kama.json first\n", name.c_str()); return 1;
            }
            std::string target = absolutePath(base + "/" + spec.path);
            if (!dirExists(target)) {
                fprintf(stderr, "kama: override '%s' target not found at %s\n", name.c_str(), target.c_str());
                return 1;
            }
            std::string link = (isDev ? devViewDir : viewDir) + "/" + importNameOf(name);
            runCmd(rmRfCmd(link));
            if (!linkDir(target, link)) {
                fprintf(stderr, "kama: cannot link override '%s'\n", name.c_str()); return 1;
            }
            fprintf(stderr, "kama: override '%s' -> %s (local, not locked)\n", name.c_str(), target.c_str());
        }

        fprintf(stderr, "kama: installed %zu package(s) into %s\n", lock.size(), base.c_str());
        return 0;
    }
}

// `kama pkg install [<dir>]`: materialize the per-project dependency view from `kama.json` into
// `<project>/.kama/{deps,dev-deps}/` and write a deterministic `kama.lock`, honoring an existing lock.
int cmdInstall(const std::string& projectDir)
{
    std::string base = projectDir.empty() ? "." : projectDir;
    std::string manifest = base + "/kama.json";
    if (!fileExists(manifest)) {
        fprintf(stderr, "kama install: no kama.json in %s\n", base.c_str());
        return 2;
    }
    std::map<std::string, LockEntry> oldLock;
    std::string lockPath = base + "/kama.lock";
    if (fileExists(lockPath)) {
        std::string lerr;
        if (!parseLockFile(lockPath, oldLock, lerr)) {
            fprintf(stderr, "kama install: %s: %s\n", lockPath.c_str(), lerr.c_str());
            return 2;
        }
    }
    return resolveProject(base, oldLock);
}

// `kama pkg update [<pkg>]`: re-resolve pins and rewrite the lock; never touches kama.json. No arg → every
// node re-resolves fresh (empty oldLock: a git branch advances to its newest commit, a url re-downloads).
// A <pkg> arg → drop only that pin (and its now-unreferenced subtree, which the manifest-driven BFS simply
// won't revisit); every other pin stays honored.
int cmdPkgUpdate(const std::string& projectDir, const std::string& onlyPkg)
{
    std::string base = projectDir.empty() ? "." : projectDir;
    std::string manifest = base + "/kama.json";
    if (!fileExists(manifest)) { fprintf(stderr, "kama pkg update: no kama.json in %s\n", base.c_str()); return 2; }

    std::map<std::string, LockEntry> oldLock;
    if (!onlyPkg.empty()) {
        std::string lockPath = base + "/kama.lock", lerr;
        if (fileExists(lockPath) && !parseLockFile(lockPath, oldLock, lerr)) {
            fprintf(stderr, "kama pkg update: %s: %s\n", lockPath.c_str(), lerr.c_str()); return 2;
        }
        std::map<std::string, DepSpec> d, dd; std::string err;   // <pkg> must be a declared (dev-)dependency
        if (!loadManifestDeps(manifest, d, err, &dd)) { fprintf(stderr, "kama: %s: %s\n", manifest.c_str(), err.c_str()); return 2; }
        if (!d.count(onlyPkg) && !dd.count(onlyPkg)) {
            fprintf(stderr, "kama pkg update: '%s' is not a dependency in %s\n", onlyPkg.c_str(), manifest.c_str());
            return 2;
        }
        oldLock.erase(onlyPkg);   // drop just this pin → it (and any now-orphaned subtree) re-resolves fresh
    }
    return resolveProject(base, oldLock);   // onlyPkg empty → empty oldLock → everything fresh
}

// ---- kama.json manifest mutation (pkg add / remove) --------------------------------------------------
// Byte-preserving splice: everything OUTSIDE the target `dependencies`/`dev-dependencies` section (name,
// version, flags, unknown keys, formatting) is kept verbatim; only that one section is re-emitted (each
// dep's own value text is preserved, just the block layout normalized — the section is machine-managed).

// Match the bracket at s[open] ('{' or '['); set `close` to its partner index. Respects strings.
static bool matchBrace(const std::string& s, size_t open, size_t& close)
{
    char oc = s[open], cc = (oc == '{') ? '}' : ']'; int depth = 0; bool inStr = false;
    for (size_t k = open; k < s.size(); ++k) { char c = s[k];
        if (inStr)          { if (c == '\\') ++k; else if (c == '"') inStr = false; }
        else if (c == '"')    inStr = true;
        else if (c == oc)     ++depth;
        else if (c == cc)   { if (--depth == 0) { close = k; return true; } } }
    return false;
}

// The indentation (leading spaces/tabs) of the line containing byte `pos`.
static std::string indentBefore(const std::string& s, size_t pos)
{
    size_t start = (pos == 0) ? 0 : s.rfind('\n', pos - 1);
    start = (start == std::string::npos) ? 0 : start + 1;
    std::string ind;
    for (size_t k = start; k < pos && (s[k] == ' ' || s[k] == '\t'); ++k) ind += s[k];
    return ind;
}

// Walk the members of the JSON object whose '{' is at `objOpen`, appending (key, rawValueText) in order.
// rawValueText is the value's exact bytes (object/array/string/number). Also, if `findKey` is non-null and
// matches, sets *keyPos/*valEnd to that member's span. Returns false only on malformed structure.
static bool walkMembers(const std::string& s, size_t objOpen,
                        std::vector<std::pair<std::string,std::string>>& out,
                        const std::string* findKey = nullptr, size_t* keyPos = nullptr, size_t* valEnd = nullptr)
{
    size_t i = objOpen + 1;
    auto ws = [&]{ while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; };
    ws(); if (i < s.size() && s[i] == '}') return true;   // empty object
    while (i < s.size()) {
        ws(); if (i >= s.size() || s[i] != '"') return false;
        size_t kp = i; std::string k; ++i;
        while (i < s.size() && s[i] != '"') { if (s[i]=='\\' && i+1<s.size()) { k += s[i+1]; i += 2; } else { k += s[i]; ++i; } }
        ++i; ws(); if (i >= s.size() || s[i] != ':') return false; ++i; ws();
        size_t vstart = i, vend;
        if (i < s.size() && (s[i] == '{' || s[i] == '[')) { size_t cl; if (!matchBrace(s, i, cl)) return false; vend = cl + 1; i = vend; }
        else if (i < s.size() && s[i] == '"') { ++i; while (i < s.size() && s[i] != '"') { if (s[i]=='\\' && i+1<s.size()) ++i; ++i; } ++i; vend = i; }
        else { while (i < s.size() && s[i]!=','&&s[i]!='}'&&s[i]!=' '&&s[i]!='\t'&&s[i]!='\n'&&s[i]!='\r') ++i; vend = i; }
        out.push_back({k, s.substr(vstart, vend - vstart)});
        if (findKey && k == *findKey) { if (keyPos) *keyPos = kp; if (valEnd) *valEnd = vend; }
        ws(); if (i < s.size() && s[i] == ',') { ++i; continue; }
        break;
    }
    return true;
}

static std::string depEntryValue(const DepSpec& d)   // the "{ ... }" value for one dependency
{
    std::string f;
    auto add = [&](const char* k, const std::string& v){ if (!v.empty()) { if (!f.empty()) f += ", ";
        f += std::string("\"") + k + "\": \"" + jsonEscape(v) + "\""; } };
    add("path", d.path); add("git", d.git); add("url", d.url); add("registry", d.registry);
    add("rev", d.rev); add("version", d.version); add("integrity", d.integrity);
    return "{ " + f + " }";
}

static std::string reemitSection(const std::vector<std::pair<std::string,std::string>>& mem, const std::string& baseInd)
{
    if (mem.empty()) return "{}";
    std::string mi = baseInd + "  ", out = "{\n";
    for (size_t k = 0; k < mem.size(); ++k)
        out += mi + "\"" + jsonEscape(mem[k].first) + "\": " + mem[k].second + (k + 1 < mem.size() ? ",\n" : "\n");
    return out + baseInd + "}";
}

// Locate the top-level object and a `section` member's object span. Returns true if the section exists.
static bool findSection(const std::string& s, const std::string& section,
                        size_t& topOpen, size_t& keyPos, size_t& objOpen, size_t& objClose)
{
    topOpen = s.find('{');
    if (topOpen == std::string::npos) return false;
    std::vector<std::pair<std::string,std::string>> top; size_t kp = 0, ve = 0;
    if (!walkMembers(s, topOpen, top, &section, &kp, &ve) || kp == 0) return false;
    keyPos = kp;
    // the value starts at the first '{' at/after the ':' following keyPos — find it from the raw value.
    size_t colon = s.find(':', keyPos); objOpen = s.find('{', colon);
    if (objOpen == std::string::npos || !matchBrace(s, objOpen, objClose)) return false;
    return true;
}

static bool manifestAddDep(const std::string& path, const std::string& name, const DepSpec& d,
                           bool dev, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
    std::string section = dev ? "dev-dependencies" : "dependencies";
    std::string val = depEntryValue(d);

    size_t topOpen, keyPos, objOpen, objClose;
    std::string out;
    if (findSection(s, section, topOpen, keyPos, objOpen, objClose)) {
        std::vector<std::pair<std::string,std::string>> mem;
        if (!walkMembers(s, objOpen, mem)) { err = "malformed `" + section + "` in " + path; return false; }
        bool replaced = false;
        for (auto& m : mem) if (m.first == name) { m.second = val; replaced = true; break; }
        if (!replaced) mem.push_back({name, val});
        std::string sec = reemitSection(mem, indentBefore(s, keyPos));
        out = s.substr(0, objOpen) + sec + s.substr(objClose + 1);
    } else {
        topOpen = s.find('{');
        if (topOpen == std::string::npos) { err = "manifest is not a JSON object"; return false; }
        size_t topClose; if (!matchBrace(s, topOpen, topClose)) { err = "malformed manifest " + path; return false; }
        // top-level member indent (first member's line indent, else two spaces)
        std::vector<std::pair<std::string,std::string>> top; walkMembers(s, topOpen, top);
        std::string topInd = "  ";
        { size_t f = s.find('"', topOpen + 1); if (f != std::string::npos && f < topClose) topInd = indentBefore(s, f); }
        std::string secText = "\"" + section + "\": " + reemitSection({{name, val}}, topInd);
        bool empty = top.empty();
        if (empty) out = s.substr(0, topOpen) + "{\n" + topInd + secText + "\n}" + s.substr(topClose + 1);
        else       out = s.substr(0, topOpen + 1) + "\n" + topInd + secText + "," + s.substr(topOpen + 1);
    }
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { err = "cannot write '" + path + "'"; return false; }
    o << out; return true;
}

// Remove `name` from whichever section holds it. Returns true even if absent (idempotent); sets *found.
static bool manifestRemoveDep(const std::string& path, const std::string& name, std::string& err, bool* found = nullptr)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
    if (found) *found = false;
    for (const char* section : {"dependencies", "dev-dependencies"}) {
        size_t topOpen, keyPos, objOpen, objClose;
        if (!findSection(s, section, topOpen, keyPos, objOpen, objClose)) continue;
        std::vector<std::pair<std::string,std::string>> mem;
        if (!walkMembers(s, objOpen, mem)) { err = std::string("malformed `") + section + "` in " + path; return false; }
        bool here = false;
        std::vector<std::pair<std::string,std::string>> kept;
        for (auto& m : mem) { if (m.first == name) here = true; else kept.push_back(m); }
        if (!here) continue;
        std::string sec = reemitSection(kept, indentBefore(s, keyPos));
        std::string out = s.substr(0, objOpen) + sec + s.substr(objClose + 1);
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write '" + path + "'"; return false; }
        o << out; if (found) *found = true; return true;
    }
    return true;   // not present anywhere — idempotent no-op
}

// `kama pkg add [--dev] <name> (--git U [--rev R] | --url U [--integrity H] | --path P)` — mutate the
// manifest, then install (lock + view update in one shot).
int cmdPkgAdd(const std::string& base, const std::string& name, const DepSpec& d, bool dev)
{
    std::string manifest = base + "/kama.json";
    if (!fileExists(manifest)) { fprintf(stderr, "kama pkg add: no kama.json in %s\n", base.c_str()); return 2; }
    std::string err;
    if (!manifestAddDep(manifest, name, d, dev, err)) { fprintf(stderr, "kama pkg add: %s\n", err.c_str()); return 1; }
    return cmdInstall(base);
}

// `kama pkg remove <name>` — drop it from the manifest (idempotent), then install.
int cmdPkgRemove(const std::string& base, const std::string& name)
{
    std::string manifest = base + "/kama.json";
    if (!fileExists(manifest)) { fprintf(stderr, "kama pkg remove: no kama.json in %s\n", base.c_str()); return 2; }
    std::string err; bool found = false;
    if (!manifestRemoveDep(manifest, name, err, &found)) { fprintf(stderr, "kama pkg remove: %s\n", err.c_str()); return 1; }
    if (!found) fprintf(stderr, "kama pkg remove: '%s' is not a dependency (nothing to do)\n", name.c_str());
    return cmdInstall(base);
}

// ---- kama publish (M3.1) ---------------------------------------------------------------------
// Map a `--registry` argument to a local directory. M3.1 publishes to a `file://`/dir registry (the
// network-free path that also mirrors an air-gapped self-host); a remote-transport publish is M3.3.
static bool registryDirFromArg(const std::string& arg, std::string& dir, std::string& err)
{
    const std::string scheme = "file://";
    if (arg.compare(0, scheme.size(), scheme) == 0) { dir = arg.substr(scheme.size()); return true; }
    if (arg.find("://") != std::string::npos) {
        err = "publishing to a remote registry (" + arg + ") is not supported yet — publish to a "
              "file:// or local-directory registry (a remote-transport publish is M3.3)";
        return false;
    }
    dir = arg; return true;   // a plain local path
}

// Splice a new version entry into `<name>/index.json` (create it if absent), preserving every existing
// entry verbatim. The new entry becomes the first element of `versions`. Immutability (refusing to
// overwrite an existing version) is enforced by the caller before this runs.
static bool appendIndexEntry(const std::string& indexPath, const std::string& name,
                             const std::string& entryJson, std::string& err)
{
    if (!fileExists(indexPath)) {
        std::ofstream o(indexPath, std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write '" + indexPath + "'"; return false; }
        o << "{\n  \"name\": \"" << jsonEscape(name) << "\",\n  \"versions\": [\n    "
          << entryJson << "\n  ]\n}\n";
        return true;
    }
    std::ifstream in(indexPath, std::ios::binary);
    if (!in) { err = "cannot open '" + indexPath + "'"; return false; }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
    size_t vk = s.find("\"versions\"");
    size_t br = (vk == std::string::npos) ? std::string::npos : s.find('[', vk);
    if (br == std::string::npos) { err = "malformed registry index '" + indexPath + "' (no versions array)"; return false; }
    size_t j = br + 1; while (j < s.size() && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
    bool hasExisting = (j < s.size() && s[j] != ']');
    std::string out = s.substr(0, br + 1) + "\n    " + entryJson + (hasExisting ? "," : "") + s.substr(br + 1);
    std::ofstream o(indexPath, std::ios::binary | std::ios::trunc);
    if (!o) { err = "cannot write '" + indexPath + "'"; return false; }
    o << out; return true;
}

// `kama publish --registry <dir-or-file-uri>`: tar the project sources, hash them, and record the new
// version in the registry's `<name>/index.json` (write-once — refuses to overwrite an existing version).
// A published registry dep reduces to a url dep on install, so the tarball IS the url-dep format.
int cmdPublish(const std::string& base, const std::string& registryArg, const std::string& keyPath)
{
    std::string manifest = base + "/kama.json";
    if (!fileExists(manifest)) { fprintf(stderr, "kama publish: no kama.json in %s\n", base.c_str()); return 2; }
    if (registryArg.empty())   { fprintf(stderr, "kama publish: --registry <dir-or-file-uri> is required\n"); return 2; }
    if (!keyPath.empty() && !hasSshKeygen()) { fprintf(stderr, "kama publish: --key needs ssh-keygen (not found on PATH)\n"); return 2; }

    std::string name, version, err;
    if (!loadManifestNameVersion(manifest, name, version, err)) {
        fprintf(stderr, "kama publish: %s: %s\n", manifest.c_str(), err.c_str()); return 2;
    }
    if (name.empty() || version.empty()) {
        fprintf(stderr, "kama publish: %s needs a \"name\" and a \"version\"\n", manifest.c_str()); return 2;
    }
    SemVer sv;
    if (!parseSemVer(version, sv)) {
        fprintf(stderr, "kama publish: version \"%s\" is not a MAJOR.MINOR.PATCH SemVer\n", version.c_str()); return 2;
    }
    std::map<std::string, DepSpec> deps;
    if (!loadManifestDeps(manifest, deps, err)) { fprintf(stderr, "kama publish: %s: %s\n", manifest.c_str(), err.c_str()); return 2; }

    std::string regDir;
    if (!registryDirFromArg(registryArg, regDir, err)) { fprintf(stderr, "kama publish: %s\n", err.c_str()); return 2; }

    std::string pkgDir = regDir + "/" + name;
    std::string indexPath = pkgDir + "/index.json";
    if (fileExists(indexPath)) {   // immutability: refuse to overwrite an already-published version
        std::ifstream f(indexPath, std::ios::binary);
        std::string idx((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<IndexEntry> existing; IndexReader ir(idx);
        if (!ir.parse(existing)) { fprintf(stderr, "kama publish: malformed %s: %s\n", indexPath.c_str(), ir.err.c_str()); return 1; }
        for (auto& e : existing) if (e.version == version) {
            fprintf(stderr, "kama publish: %s@%s is already published (versions are immutable — bump the "
                    "version)\n", name.c_str(), version.c_str());
            return 1;
        }
    }

    // Stage a wrapper dir named after the package (so install's `--strip-components=1` peels exactly one
    // level), copying the sources but excluding VCS/build/lock cruft, then gzip it. `sha256Of` is the
    // tarball integrity a consumer re-verifies.
    std::string tmp = regDir + "/.tmp-publish-" + std::to_string((long)getpid());
    std::string wrapper = importNameOf(name);   // a single path component (a scoped name has a '/')
    runCmd(rmRfCmd(tmp));
    if (!makeDirs(tmp + "/" + wrapper)) { fprintf(stderr, "kama publish: cannot stage the package\n"); runCmd(rmRfCmd(tmp)); return 1; }
    std::string copyCmd = "tar -c -C \"" + base + "\" --exclude=./.git --exclude=./.kama --exclude=./build "
                          "--exclude=./kama.lock --exclude=./kama.local.json -f - . | tar -x -C \"" + tmp + "/" + wrapper + "\" -f -";
    if (runCmd(copyCmd) != 0) { fprintf(stderr, "kama publish: cannot copy sources (is tar available?)\n"); runCmd(rmRfCmd(tmp)); return 1; }
    // The integrity hash must be REPRODUCIBLE: publishing the same sources twice (e.g. to two mirrors) has
    // to yield the same sha256, or a consumer re-pointing at a mirror trips the dependency-confusion guard.
    // Two things break that, and neither is fixable with portable tar flags (GNU's --mtime/--sort don't
    // exist on bsdtar), so normalize the inputs instead:
    //   1. the staging wrapper dir is created fresh on every publish, so ITS mtime lands in the archive —
    //      clamp every staged entry to a fixed timestamp;
    //   2. libarchive's `tar -cz` (macOS) stamps the CURRENT TIME into the gzip header — compress through
    //      `gzip -n`, which omits the name/timestamp. (GNU tar was reproducible here only by accident: it
    //      pipes to gzip via stdin, which stores 0.)
    runCmd("find \"" + tmp + "/" + wrapper + "\" -exec touch -t 198001010000 {} +");
    std::string tarball = tmp + "/pkg.tar.gz";
    if (runCmd("tar -cf - -C \"" + tmp + "\" \"" + wrapper + "\" | gzip -n > \"" + tarball + "\"") != 0) {
        fprintf(stderr, "kama publish: cannot create the tarball\n"); runCmd(rmRfCmd(tmp)); return 1;
    }
    std::string integrity = sha256Of(tarball);
    if (integrity.empty()) { fprintf(stderr, "kama publish: cannot hash the tarball (is sha256sum/shasum available?)\n"); runCmd(rmRfCmd(tmp)); return 1; }

    // M3.2a: optionally sign the tarball. The SSHSIG blob + signer public key go into the index entry.
    std::string signature, sigKey;
    if (!keyPath.empty()) {
        std::string serr;
        if (!sshSign(tarball, keyPath, signature, sigKey, serr)) { fprintf(stderr, "kama publish: %s\n", serr.c_str()); runCmd(rmRfCmd(tmp)); return 1; }
    }

    if (!makeDirs(pkgDir)) { fprintf(stderr, "kama publish: cannot create %s\n", pkgDir.c_str()); runCmd(rmRfCmd(tmp)); return 1; }
    std::string finalTarball = pkgDir + "/" + version + ".tar.gz";
    runCmd(rmRfCmd(finalTarball));
    if (rename(tarball.c_str(), finalTarball.c_str()) != 0) {
        // rename can fail across volumes; fall back to a copy.
        if (runCmd("cp \"" + tarball + "\" \"" + finalTarball + "\"") != 0) {
            fprintf(stderr, "kama publish: cannot place the tarball at %s\n", finalTarball.c_str()); runCmd(rmRfCmd(tmp)); return 1;
        }
    }
    runCmd(rmRfCmd(tmp));

    // Build the index entry. `dependencies` are recorded for protocol conformance (the resolver reads the
    // fetched manifest, so this is informational metadata) — a name → its declared version range.
    std::string depsJson;
    for (auto& kv : deps) {   // sorted map => deterministic
        if (!depsJson.empty()) depsJson += ", ";
        depsJson += "\"" + jsonEscape(kv.first) + "\": {";
        if (!kv.second.version.empty()) depsJson += " \"version\": \"" + jsonEscape(kv.second.version) + "\" ";
        depsJson += "}";
    }
    std::string entry = "{ \"version\": \"" + jsonEscape(version) + "\", \"integrity\": \"" + jsonEscape(integrity)
                      + "\", \"tarball\": \"" + jsonEscape(name + "/" + version + ".tar.gz") + "\"";
    if (!depsJson.empty()) entry += ", \"dependencies\": { " + depsJson + " }";
    if (!signature.empty()) entry += ", \"signature\": \"" + jsonEscape(signature) + "\", \"key\": \"" + jsonEscape(sigKey) + "\"";
    entry += " }";

    if (!appendIndexEntry(indexPath, name, entry, err)) { fprintf(stderr, "kama publish: %s\n", err.c_str()); return 1; }
    fprintf(stderr, "kama: published %s@%s (%s%s) to %s\n", name.c_str(), version.c_str(), integrity.c_str(),
            signature.empty() ? "" : ", signed", regDir.c_str());
    return 0;
}

void usage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama transpile <in.kama> [-o out.c] [--no-line] [--dev]\n"
        "  kama build     <in.kama>... [-o out] [--target native|wasm|embedded] [--release|--debug] [--shared]\n"
        "                             [--no-heap] [--link <lib>]... [--webgpu] [--cc <compiler>] [--no-line] [--keep-c] [--dev]\n"
        "                  (pass multiple .kama files to build a multi-file program; --dev also resolves dev-dependencies)\n"
        "  kama run       [<file>] [--release|--debug] [--dev] [--define NAME]... [--config PATH] [-- <program args>]\n"
        "                  (build the entry .kama — explicit <file>, else the manifest \"main\" — and run it; native-only)\n"
        "  kama pkg install [<dir>] [--verify] resolve `kama.json` (dev-)dependencies into .kama/{deps,dev-deps} + kama.lock\n"
        "                                      (--verify: require + check registry-package signatures)\n"
        "  kama pkg add   [--dev] <name> (--git U [--rev R | --version V] | --url U [--integrity H] | --path P |\n"
        "                                --version V [--registry BASE])   (bare --version = a registry dependency)\n"
        "  kama pkg remove <name>\n"
        "  kama pkg update [<pkg>]             re-resolve pins (advance a branch pin) and rewrite the lock\n"
        "  kama publish [<dir>] --registry <dir-or-file-uri> [--key <ssh-key>]   tarball + record (+ sign) in the index\n"
        "  kama toolchain list                 installed versions (+ the default and what the cwd resolves to)\n"
        "  kama toolchain install <v>          install version <v> into ~/.kama/versions/<v>\n"
        "  kama toolchain uninstall <v>        remove an installed version\n"
        "  kama toolchain default <v>          set the global default version\n"
        "  kama toolchain pin <v>              pin this project's toolchain in kama.json\n"
        "  kama update    [--version vX.Y.Z]   install the latest (or <v>) and make it the default\n"
        "  kama --version\n");
}

void pkgUsage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama pkg install [<dir>] [--verify] resolve dependencies into .kama/{deps,dev-deps} + kama.lock\n"
        "  kama pkg add   [--dev] <name> (--git U [--rev R | --version V] | --url U [--integrity H] | --path P |\n"
        "                                --version V [--registry BASE])\n"
        "  kama pkg remove <name>\n"
        "  kama pkg update [<pkg>]             re-resolve pins and rewrite the lock\n");
}

// ---- M1: toolchain version management (selector + `kama toolchain`) ----------------------------------

void toolchainUsage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama toolchain list                 installed versions (+ the default and what the cwd resolves to)\n"
        "  kama toolchain install <v>          install version <v> into ~/.kama/versions/<v>\n"
        "  kama toolchain uninstall <v>        remove an installed version\n"
        "  kama toolchain default <v>          set the global default version\n"
        "  kama toolchain pin <v>              pin this project's toolchain in kama.json\n");
}

// Read a file's contents, trimmed of surrounding whitespace ("" if absent/empty). Used for the one-line
// `~/.kama/default` record.
static std::string readTrimmedFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' '||s.back()=='\t')) s.pop_back();
    size_t b = 0; while (b < s.size() && (s[b]==' '||s[b]=='\t'||s[b]=='\n'||s[b]=='\r')) ++b;
    return s.substr(b);
}

// The nearest `kama.json` walking up from the cwd ("" if none). The selector reads its `toolchain` pin —
// a project rooted in any subdirectory still resolves its pin, like git/cargo find their root.
static std::string findManifestUpward()
{
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) return "";
    std::string dir = buf;
    for (;;) {
        if (fileExists(dir + "/kama.json")) return dir + "/kama.json";
        size_t slash = dir.find_last_of("/\\");
        if (slash == std::string::npos) break;
        std::string parent = dir.substr(0, slash);
        if (parent.empty()) parent = "/";     // parent of "/foo" is "/"
        if (parent == dir) break;             // reached the root — no progress
        dir = parent;
    }
    return "";
}

// The version the PATH selector should run in the cwd: dev-local override (`kama.local.json` `toolchain`,
// M5.3) → project pin (nearest kama.json `toolchain`) → `KAMA_VERSION` env → global default
// (`~/.kama/default`). "" ⇒ no preference (run this binary as-is). The local override is gitignored, so a
// dev can test against a different toolchain without touching the committed pin.
static std::string resolvePin()
{
    std::string manifest = findManifestUpward();
    if (!manifest.empty()) {
        std::string local = dirName(manifest) + "/kama.local.json";
        if (std::ifstream(local).good()) {
            std::string tc, err;
            if (loadManifestToolchain(local, tc, err) && !tc.empty()) return tc;
        }
        std::string tc, err;
        if (loadManifestToolchain(manifest, tc, err) && !tc.empty()) return tc;
    }
    if (const char* v = getenv("KAMA_VERSION")) { if (*v) return v; }
    return readTrimmedFile(defaultVersionFile());
}

// Set a top-level string member `key` = `value` in a kama.json, byte-preserving (everything else verbatim;
// inserts the member if absent). Reuses the pkg-add splice helpers. Used by `kama toolchain pin`.
static bool manifestSetTopString(const std::string& path, const std::string& key,
                                 const std::string& value, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
    size_t topOpen = s.find('{');
    if (topOpen == std::string::npos) { err = "manifest is not a JSON object"; return false; }
    std::vector<std::pair<std::string,std::string>> top; size_t kp = 0, ve = 0;
    if (!walkMembers(s, topOpen, top, &key, &kp, &ve)) { err = "malformed manifest " + path; return false; }
    std::string quoted = "\"" + jsonEscape(value) + "\"";
    std::string out;
    if (kp != 0) {                            // key present — replace just its value span
        size_t colon = s.find(':', kp);
        size_t vstart = colon + 1;
        while (vstart < s.size() && (s[vstart]==' '||s[vstart]=='\t'||s[vstart]=='\n'||s[vstart]=='\r')) ++vstart;
        out = s.substr(0, vstart) + quoted + s.substr(ve);
    } else {                                  // absent — insert as a new first member
        size_t topClose; if (!matchBrace(s, topOpen, topClose)) { err = "malformed manifest " + path; return false; }
        std::string topInd = "  ";
        { size_t f = s.find('"', topOpen + 1); if (f != std::string::npos && f < topClose) topInd = indentBefore(s, f); }
        std::string member = "\"" + key + "\": " + quoted;
        if (top.empty()) out = s.substr(0, topOpen) + "{\n" + topInd + member + "\n}" + s.substr(topClose + 1);
        else             out = s.substr(0, topOpen + 1) + "\n" + topInd + member + "," + s.substr(topOpen + 1);
    }
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { err = "cannot write '" + path + "'"; return false; }
    o << out; return true;
}

// `kama toolchain list` — installed versions, the global default, and what the cwd resolves to.
int cmdToolchainList()
{
    std::vector<std::string> versions;
    if (DIR* d = opendir(versionsDir().c_str())) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            if (dirExists(versionDir(n))) versions.push_back(n);
        }
        closedir(d);
    }
    std::sort(versions.begin(), versions.end());
    std::string def = readTrimmedFile(defaultVersionFile());
    std::string resolved = resolvePin();
    if (versions.empty()) {
        printf("no toolchains installed (install one with `kama toolchain install <v>`)\n");
    } else {
        for (const std::string& v : versions)
            printf("%s %s\n", (v == def ? "*" : " "), v.c_str());
        printf("\n* = global default (%s)\n", def.empty() ? "unset" : def.c_str());
    }
    if (!resolved.empty()) printf("this directory resolves to: %s\n", resolved.c_str());
    return 0;
}

// `kama toolchain install <v>` — fetch version <v> into ~/.kama/versions/<v> via the canonical installer
// (KAMA_VERSION=<v>). Idempotent: a no-op if already present. The installer lays down the versioned dir.
int cmdToolchainInstall(const std::string& v)
{
    if (dirExists(versionDir(v))) { printf("kama %s already installed\n", v.c_str()); return 0; }
    return runInstaller(v, /*makeDefault=*/false);   // add alongside; don't disturb the current default
}

// `kama toolchain uninstall <v>` — remove ~/.kama/versions/<v>. Refuses to remove the current default.
int cmdToolchainUninstall(const std::string& v)
{
    if (!dirExists(versionDir(v))) { fprintf(stderr, "kama toolchain uninstall: '%s' is not installed\n", v.c_str()); return 1; }
    if (readTrimmedFile(defaultVersionFile()) == v) {
        fprintf(stderr, "kama toolchain uninstall: '%s' is the default — set another default first\n", v.c_str());
        return 1;
    }
    if (runCmd(rmRfCmd(versionDir(v))) != 0) { fprintf(stderr, "kama toolchain uninstall: failed to remove '%s'\n", v.c_str()); return 1; }
    printf("removed kama %s\n", v.c_str());
    return 0;
}

// `kama toolchain default <v>` — record the global default (the version an unpinned directory resolves to).
// The PATH selector is unchanged: it always hands off to the versioned location, so it needn't match the
// default — the installer lays it down once, and `kama update` refreshes it.
int cmdToolchainDefault(const std::string& v)
{
    if (!fileExists(versionBin(v))) {
        fprintf(stderr, "kama toolchain default: '%s' is not installed — run `kama toolchain install %s`\n", v.c_str(), v.c_str());
        return 1;
    }
    std::ofstream o(defaultVersionFile(), std::ios::binary | std::ios::trunc);
    if (!o) { fprintf(stderr, "kama toolchain default: cannot write %s\n", defaultVersionFile().c_str()); return 1; }
    o << v << "\n";
    printf("default is now kama %s\n", v.c_str());
    return 0;
}

// `kama toolchain pin <v>` — write `"toolchain": "<v>"` into ./kama.json (byte-preserving).
int cmdToolchainPin(const std::string& v)
{
    std::string manifest = "./kama.json";
    if (!fileExists(manifest)) { fprintf(stderr, "kama toolchain pin: no kama.json in the current directory\n"); return 2; }
    std::string err;
    if (!manifestSetTopString(manifest, "toolchain", v, err)) { fprintf(stderr, "kama toolchain pin: %s\n", err.c_str()); return 1; }
    printf("pinned this project to kama %s\n", v.c_str());
    return 0;
}

// This process's own executable path (argv[0] is unreliable — a PATH lookup passes the bare name). The
// selector uses it to recognize itself; falls back to argv[0] where the OS query is unavailable.
static std::string selfExePath(const char* argv0)
{
#if defined(_WIN32)
    char* p = nullptr;
    if (_get_pgmptr(&p) == 0 && p) return p;   // full path of the running .exe (no windows.h needed)
#elif defined(__linux__)
    char buf[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
#elif defined(__APPLE__)
    char buf[PATH_MAX]; unsigned int sz = sizeof(buf);       // _NSGetExecutablePath declared up top (extern "C")
    if (_NSGetExecutablePath(buf, &sz) == 0) return buf;
#endif
    return argv0 ? argv0 : "kama";
}

// The PATH selector: before dispatch, resolve which version this directory wants and re-exec THAT version's
// binary. Only the selector at `~/.kama/bin/kama` selects — a per-version binary (or a dev/repo build) runs
// in place, which is both the loop-stopper (the re-exec target is a different path, so it won't select
// again) and what keeps a dev `./kama` from silently handing off to an installed toolchain. The selector is
// only ever a hand-off: it never compiles itself (it has no sibling include/lib), so it always execs the
// versioned location where the support dirs live. `toolchain`/`update` manage the install and stay put.
void maybeReExec(char** argv, const std::string& subcommand)
{
    if (getenv("KAMA_NO_SELECT")) return;                              // explicit escape hatch
    if (subcommand == "toolchain" || subcommand == "update") return;  // these manage the install in place
    if (absolutePath(selfExePath(argv[0])) != absolutePath(selectorPath())) return;   // only the selector selects
    std::string v = resolvePin();
    if (v.empty()) return;                                            // no default/pin — run in place
    std::string bin = versionBin(v);
    if (!fileExists(bin)) {
        fprintf(stderr, "kama: toolchain '%s' is not installed — run `kama toolchain install %s`\n", v.c_str(), v.c_str());
        exit(1);
    }
    argv[0] = const_cast<char*>(bin.c_str());   // so the versioned compiler resolves its runtime/stdlib from
                                                // its OWN location (exe-relative), not the selector's ~/.kama/bin
#ifdef _WIN32
    _putenv_s("KAMA_NO_SELECT", "1");
    intptr_t rc = _spawnv(_P_WAIT, bin.c_str(), argv);               // Windows can't exec-in-place; spawn + forward exit
    exit(rc < 0 ? 1 : (int)rc);
#else
    setenv("KAMA_NO_SELECT", "1", 1);
    execv(bin.c_str(), argv);                                        // replaces this process on success
    fprintf(stderr, "kama: failed to exec %s\n", bin.c_str());
    exit(1);
#endif
}

} // namespace

// The LSP's external seams into the front end (declared in kama.lsp.h). Mirror the `kama check` setup —
// a FRESH CEmitter per call (analysis is not re-runnable on one instance), the embedded prelude + built-in
// modules in scope — but drive it off an in-memory buffer and return structured diagnostics instead of
// printing. Parse diagnostics are collected even when the parse fails. Defined at GLOBAL scope (external
// linkage) so kama.lsp.cpp can call them; the anon-namespace helpers above stay internal.
//
// The opaque handle body: it keeps the analyzed CEmitter alive so the query facade (documentSymbols /
// definitionAt / typeAtPosition — instance methods reading the index analyze() built) can answer
// hover/def/outline as instant reads, not a re-analysis per cursor move.
struct LspIndex { std::shared_ptr<CEmitter> idx; std::string path; };

SharedLspIndex lspAnalyze(const std::string& path, const std::string& text,
                          std::vector<Diagnostic>& diags, const char* argv0)
{
    ParseResult pr = parseForQuery(text.c_str(), path);
    if (pr.ctx) for (const auto& d : pr.ctx->diagnostics) diags.push_back(d);
    if (!pr.unit) return nullptr;                  // parse failed — diags carry the errors; no queryable index

    // Load the whole program so cross-module names resolve (imported modules parsed from disk, exactly as
    // `kama check`/`build` do), then substitute the in-memory buffer for the open file's on-disk unit so
    // unsaved edits still analyze. Without this the single open buffer sees only the built-in prelude, so
    // every `import`ed type reads as "not exported" — a cascade of false diagnostics. Falls back to
    // single-file if the file isn't on disk yet (a fresh unsaved buffer) or a module can't be resolved:
    // best-effort diagnostics then, but hover/def/outline for the open file still work.
    std::vector<SharedCompilationUnit> units;
    std::vector<std::string> paths;
    if (loadProgramUnits({ path }, argv0, units, paths, /*includeDevDeps*/ false) && !units.empty()) {
        std::string abs = absolutePath(path);
        bool swapped = false;
        for (size_t i = 0; i < units.size(); ++i)
            if (paths[i] == abs) { units[i] = pr.unit; swapped = true; break; }
        if (!swapped) units.insert(units.begin(), pr.unit);   // defensive: keep the live buffer in the set
    } else {
        units = { pr.unit };                       // single-file fallback (unsaved/new file or unresolved import)
    }

    auto emitter = std::make_shared<CEmitter>(path);   // analysis mode: no C emitted
    emitter->setPrelude(preludeUnit());            // Optional/Result implicitly in scope
    for (auto& m : preludeModuleUnits()) emitter->addPreludeModule(m);
    emitter->analyze(units);
    // Only the OPEN file's diagnostics go back to the editor (the server publishes to one URI); imported
    // modules are analyzed for context, not surfaced. Their diagnostics carry a different `file`.
    for (const auto& d : emitter->diagnostics()) if (d.file == path) diags.push_back(d);
    auto h = std::make_shared<LspIndex>();
    h->idx = emitter;
    h->path = path;
    return h;
}

// ---- workspace indexing (M3.5) ---------------------------------------------------------------------

// Every *.kama under `root`, recursively, as absolute paths. Prunes build output, the package store, and
// dot-directories; aborts once the cap is blown (the caller reports rather than truncating silently).
// Deliberately NOT collectFilesRel (line ~1523): the prune list, extension filter and early abort are most
// of the body, and its one caller (treeHashOf) wants a complete unfiltered tree.
static void collectKamaFiles(const std::string& root, const std::string& rel,
                             std::vector<std::string>& out, size_t& seen, size_t budget)
{
    if (seen > budget) return;
    std::string dir = rel.empty() ? root : root + "/" + rel;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;          // ".", "..", .git, .kama (the package store) …
        std::string childRel = rel.empty() ? n : rel + "/" + n;
        if (dirExists(dir + "/" + n)) {
            if (n == "build") continue;                  // generated C + objects, never sources
            collectKamaFiles(root, childRel, out, seen, budget);
            if (seen > budget) break;
        } else if (n.size() > 5 && n.compare(n.size() - 5, 5, ".kama") == 0) {
            if (++seen > budget) break;
            out.push_back(dir + "/" + n);
        }
    }
    closedir(d);
}

// The cap in force for a GUESSED file set. `KAMA_LSP_MAX_FILES` overrides the default; 0 means no limit,
// for someone who knows their tree really is one program and would rather not add a manifest.
static size_t lspFileBudget()
{
    if (const char* env = getenv("KAMA_LSP_MAX_FILES")) {
        char* end = nullptr;
        long v = strtol(env, &end, 10);
        if (end && *end == '\0' && v >= 0) return v == 0 ? (size_t)-1 : (size_t)v;
    }
    return kLspMaxProjectFiles;
}

// Collect one package's sources, recursing into any sub-projects its manifest declares.
//
//   `sources`  present -> exactly those files/directories are this package's.
//   `packages` present -> each entry is a sub-project directory holding its own kama.json, walked the same
//                         way. A trailing `/*` ("packages/*") expands to every immediate subdirectory that
//                         has a manifest, so a monorepo need not edit its root manifest per package.
//   neither present    -> a leaf: the package's whole directory is its sources.
//
// A manifest with `packages` but no `sources` is a pure aggregator and contributes no files of its own —
// walking its directory would re-collect every member and defeat the precision it just declared.
// `visited` (canonical paths) breaks cycles: a manifest may legally name a directory that names it back,
// and a symlink makes a loop trivial.
static void collectPackageTree(const std::string& dir, std::vector<std::string>& out,
                               std::set<std::string>& visited)
{
    if (!visited.insert(absolutePath(dir)).second) return;

    std::vector<std::string> srcs, members;
    std::string err;
    const std::string manifest = dir + "/kama.json";
    if (fileExists(manifest)) {
        loadManifestSources(manifest, srcs, err);
        loadManifestPackages(manifest, members, err);
    }

    size_t seen = 0;
    if (!srcs.empty()) {
        for (const auto& rel : srcs) {
            // absolutePath, not a bare join: `"sources": ["."]` would otherwise yield `<dir>/./x.kama`,
            // and CEmitter::unitForUri matches unit names EXACTLY — so every query would miss.
            std::string abs = absolutePath(dir + "/" + rel);
            if (dirExists(abs))       collectKamaFiles(abs, "", out, seen, (size_t)-1);
            else if (fileExists(abs)) out.push_back(abs);
            // A listed path that does not exist is skipped: a manifest may name a directory not created
            // yet, and refusing to index everything else over that would be hostile.
        }
    } else if (members.empty()) {
        collectKamaFiles(absolutePath(dir), "", out, seen, (size_t)-1);
    }

    for (const auto& rel : members) {
        if (rel.size() > 2 && rel.compare(rel.size() - 2, 2, "/*") == 0) {
            std::string parent = dir + "/" + rel.substr(0, rel.size() - 2);
            std::vector<std::string> subs;
            if (DIR* d = opendir(parent.c_str())) {
                while (struct dirent* e = readdir(d)) {
                    std::string n = e->d_name;
                    if (n.empty() || n[0] == '.') continue;
                    if (dirExists(parent + "/" + n) && fileExists(parent + "/" + n + "/kama.json"))
                        subs.push_back(parent + "/" + n);
                }
                closedir(d);
            }
            std::sort(subs.begin(), subs.end());          // readdir order is not deterministic
            for (const auto& sub : subs) collectPackageTree(sub, out, visited);
        } else if (dirExists(dir + "/" + rel)) {
            collectPackageTree(dir + "/" + rel, out, visited);
        }
    }
}

std::string lspRealPath(const std::string& path) { return absolutePath(path); }

LspProject lspFindProject(const std::string& openFilePath, const std::string& workspaceRoot)
{
    LspProject p;
    if (openFilePath.empty()) return p;
    std::string wsRoot = workspaceRoot.empty() ? std::string() : absolutePath(workspaceRoot);
    std::string dir    = dirName(absolutePath(openFilePath));

    // Is the file inside the editor's folder? That is what licenses widening past the first manifest.
    bool underWorkspace = !wsRoot.empty() &&
                          (dir == wsRoot || dir.compare(0, wsRoot.size() + 1, wsRoot + "/") == 0);

    // Walk up collecting manifest directories, nearest first. Bounded by the editor's folder when we have
    // one; a `.kama` component stops the walk so a vendored dep never escapes into its host project.
    std::vector<std::string> manifests;
    std::string cur = dir;
    for (;;) {
        if (baseName(cur) == ".kama") break;
        if (fileExists(cur + "/kama.json")) manifests.push_back(cur);
        if (underWorkspace && cur == wsRoot) break;      // examined it, go no higher
        std::string parent = dirName(cur);
        if (parent == cur || parent == ".") break;       // filesystem root
        cur = parent;
    }

    // DECLARED beats inferred. If an ancestor manifest explicitly OWNS this file — via `packages` and/or
    // `sources`, expanded recursively — then widening to it is not a guess and needs no editor boundary to
    // license it. Prefer the outermost such manifest (the top of a nest of monorepos), and require that its
    // expansion actually CONTAINS the open file, so an ancestor that happens to declare unrelated members
    // is not mistaken for this file's owner.
    std::string self = absolutePath(openFilePath);
    for (auto it = manifests.rbegin(); it != manifests.rend() && p.root.empty(); ++it) {
        std::vector<std::string> srcs, members;
        std::string e;
        loadManifestSources(*it + "/kama.json", srcs, e);
        loadManifestPackages(*it + "/kama.json", members, e);
        if (srcs.empty() && members.empty()) continue;          // declares nothing: not an owner
        std::vector<std::string> files;
        std::set<std::string> visited;
        collectPackageTree(*it, files, visited);
        for (const auto& f : files)
            if (absolutePath(f) == self) {
                p.root            = *it;
                p.hasManifest     = true;
                p.declaredSources = true;
                p.files           = files;
                break;
            }
    }
    if (p.declaredSources) {
        std::sort(p.files.begin(), p.files.end());
        p.files.erase(std::unique(p.files.begin(), p.files.end()), p.files.end());
        p.seenCount = p.files.size();
        return p;
    }

    if (!manifests.empty()) {
        // Nothing declared ownership, so this IS an inference. Under a declared workspace the outermost
        // manifest wins (a monorepo root that never said so — over-indexing is the safe direction, since
        // under-indexing is what silently rewrites a caller we never saw). Without an editor boundary only
        // the nearest is defensible: widening could otherwise swallow a stray $HOME manifest.
        p.root        = underWorkspace ? manifests.back() : manifests.front();
        p.hasManifest = true;
    } else if (underWorkspace) {
        p.root = wsRoot;
    } else {
        return p;                                        // no project: rename keeps refusing, honestly
    }

    // Nothing above declared ownership of this file, so the file set is INFERRED: every .kama under the
    // root. That inference is what the cap bounds — see kLspMaxProjectFiles. A manifest can end it by
    // declaring `sources` (which files are mine) and/or `packages` (which sub-projects I own).
    size_t seen = 0, budget = lspFileBudget();
    p.cap = budget;
    collectKamaFiles(p.root, "", p.files, seen, budget);
    p.seenCount = seen;
    if (seen > budget) { p.tooLarge = true; p.files.clear(); }
    std::sort(p.files.begin(), p.files.end());           // deterministic unit order
    return p;
}

SharedLspIndex lspAnalyzeWorkspace(const std::vector<std::string>& files,
                                   const std::vector<std::pair<std::string, std::string>>& overlays,
                                   const char* argv0)
{
    if (files.empty()) return nullptr;

    // Parse the live buffers first. A buffer that won't parse simply keeps its on-disk copy — a mid-edit
    // file shouldn't blind the whole workspace.
    std::vector<std::pair<std::string, SharedCompilationUnit>> live;
    for (const auto& ov : overlays) {
        ParseResult pr = parseForQuery(ov.second.c_str(), ov.first);
        if (pr.unit) live.push_back({ absolutePath(ov.first), pr.unit });
    }

    // Passing every project file as a CLI input makes loadProgramUnits' BFS yield the UNION of all their
    // import closures, deduped by absolute path — which is exactly the project-wide set, plus whatever
    // std/dependency modules it needs for names to resolve.
    std::vector<SharedCompilationUnit> units;
    std::vector<std::string> paths;
    if (!loadProgramUnits(files, argv0, units, paths, /*includeDevDeps*/ false) || units.empty())
        return nullptr;

    for (const auto& lv : live) {
        bool swapped = false;
        for (size_t i = 0; i < units.size(); ++i)
            if (paths[i] == lv.first) { units[i] = lv.second; swapped = true; break; }
        if (!swapped) units.push_back(lv.second);        // an open file outside the project set
    }

    auto emitter = std::make_shared<CEmitter>(files.front());
    emitter->setPrelude(preludeUnit());
    for (auto& m : preludeModuleUnits()) emitter->addPreludeModule(m);
    emitter->analyze(units);
    auto h = std::make_shared<LspIndex>();
    h->idx  = emitter;
    h->path = files.front();
    return h;
}

std::vector<SymbolInfo> lspWorkspaceSymbols(const SharedLspIndex& idx, const std::string& query,
                                            const std::string& root)
{
    static const size_t kMaxResults = 200;   // a picker wants the first screenful, not the whole project
    if (!idx || !idx->idx || root.empty()) return {};
    std::string absRoot = absolutePath(root);
    std::vector<SymbolInfo> out;
    for (auto& s : idx->idx->workspaceSymbols(query)) {
        if (out.size() >= kMaxResults) break;
        // Real-path both sides: the stdlib resolves relative to the compiler binary, so in a dev tree its
        // raw path can still carry the project root as a literal prefix (see lspRealPath).
        std::string f = absolutePath(s.uri);
        if (f.size() <= absRoot.size() || f.compare(0, absRoot.size(), absRoot) != 0 ||
            f[absRoot.size()] != '/')
            continue;                                     // std, a dependency, or outside the project
        if (f.compare(absRoot.size(), 7, "/.kama/") == 0) continue;   // the package store
        out.push_back(s);
    }
    return out;
}

std::vector<SymbolInfo> lspDocumentSymbols(const SharedLspIndex& idx, const std::string& path)
{
    if (!idx || !idx->idx) return {};
    return idx->idx->documentSymbols(path);
}

Location lspDefinition(const SharedLspIndex& idx, const std::string& path, int line, int col)
{
    if (!idx || !idx->idx) return Location{};
    return idx->idx->definitionAt(path, line, col);
}

std::string lspHover(const SharedLspIndex& idx, const std::string& path, int line, int col)
{
    if (!idx || !idx->idx) return "";
    return idx->idx->typeAtPosition(path, line, col);
}

std::vector<Location> lspReferences(const SharedLspIndex& idx, const std::string& path,
                                    int line, int col, bool includeDecl)
{
    if (!idx || !idx->idx) return {};
    return idx->idx->referencesAt(path, line, col, includeDecl);
}

SrcRange lspPrepareRename(const SharedLspIndex& idx, const std::string& path, int line, int col)
{
    if (!idx || !idx->idx) return SrcRange{};
    return idx->idx->renameRangeAt(path, line, col);
}

int main(int argc, char** argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("kama %s\n", KAMA_VERSION);
        return 0;
    }
    if (argc < 2) { usage(); return 2; }

    std::string subcommand = argv[1];

    maybeReExec(argv, subcommand);   // PATH selector: hand off to the version this directory pins (M1)

    if (subcommand == "toolchain") {
        if (argc < 3) { toolchainUsage(); return 2; }
        std::string verb = argv[2];
        auto oneArg = [&](const char* cmd) -> std::string {
            std::string v;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama toolchain %s: unexpected option '%s'\n", cmd, a.c_str()); return "\x01"; }
                else if (v.empty()) v = a;
                else { fprintf(stderr, "kama toolchain %s: unexpected arg '%s'\n", cmd, a.c_str()); return "\x01"; }
            }
            return v;
        };
        if (verb == "list") {
            if (argc > 3) { fprintf(stderr, "kama toolchain list: takes no arguments\n"); return 2; }
            return cmdToolchainList();
        }
        std::string v = oneArg(verb.c_str());
        if (v == "\x01") return 2;                     // an option/extra-arg error was already printed
        if (v.empty()) { fprintf(stderr, "kama toolchain %s: missing <version>\n", verb.c_str()); return 2; }
        if (verb == "install")   return cmdToolchainInstall(v);
        if (verb == "uninstall") return cmdToolchainUninstall(v);
        if (verb == "default")   return cmdToolchainDefault(v);
        if (verb == "pin")       return cmdToolchainPin(v);
        fprintf(stderr, "kama toolchain: unknown command '%s'\n", verb.c_str()); toolchainUsage(); return 2;
    }

    if (subcommand == "lsp") {
        // The `kama lsp` language server (M1): a JSON-RPC 2.0 server over stdio that reuses the
        // front-end-as-library analysis path to publish live diagnostics. Takes no input file (it reads
        // buffers from the editor over the wire), so it returns here before the input/flag handling below.
        return runLspServer(argv[0]);   // argv[0] locates the stdlib for loading imported modules
    }

    if (subcommand == "update") {
        std::string pinned;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--version" && i + 1 < argc) pinned = argv[++i];
            else { fprintf(stderr, "kama update: unexpected arg '%s'\n", a.c_str()); return 2; }
        }
        return cmdUpdate(pinned);
    }

    if (subcommand == "publish") {
        std::string registry, dir, key;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--registry" && i + 1 < argc) registry = argv[++i];
            else if (a == "--key" && i + 1 < argc)      key = argv[++i];
            else if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama publish: unknown option '%s'\n", a.c_str()); return 2; }
            else if (dir.empty()) dir = a;
            else { fprintf(stderr, "kama publish: unexpected arg '%s'\n", a.c_str()); return 2; }
        }
        return cmdPublish(dir.empty() ? "." : dir, registry, key);
    }

    if (subcommand == "pkg") {
        if (argc < 3) { pkgUsage(); return 2; }
        std::string verb = argv[2];
        if (verb == "install") {
            std::string dir;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--verify") g_verifySignatures = true;   // M3.2a: enforce registry signatures
                else if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg install: unexpected option '%s'\n", a.c_str()); return 2; }
                else if (dir.empty()) dir = a;
                else { fprintf(stderr, "kama pkg install: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            return cmdInstall(dir);
        }
        if (verb == "update") {
            std::string pkg;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg update: unexpected option '%s'\n", a.c_str()); return 2; }
                else if (pkg.empty()) pkg = a;
                else { fprintf(stderr, "kama pkg update: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            return cmdPkgUpdate("", pkg);
        }
        if (verb == "add") {
            bool dev = false; std::string name; DepSpec d; std::string rev, integ, ver, registry;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if      (a == "--dev")                     dev = true;
                else if (a == "--git" && i + 1 < argc)     d.git = argv[++i];
                else if (a == "--url" && i + 1 < argc)     d.url = argv[++i];
                else if (a == "--path" && i + 1 < argc)    d.path = argv[++i];
                else if (a == "--rev" && i + 1 < argc)     rev = argv[++i];
                else if (a == "--integrity" && i + 1 < argc) integ = argv[++i];
                else if (a == "--version" && i + 1 < argc)   ver = argv[++i];
                else if (a == "--registry" && i + 1 < argc)  registry = argv[++i];
                else if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg add: unknown option '%s'\n", a.c_str()); return 2; }
                else if (name.empty()) name = a;
                else { fprintf(stderr, "kama pkg add: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            if (name.empty()) { fprintf(stderr, "kama pkg add: missing <name>\n"); return 2; }
            int nsrc = (!d.git.empty()) + (!d.url.empty()) + (!d.path.empty());
            if (nsrc > 1) { fprintf(stderr, "kama pkg add: at most one of --git/--url/--path\n"); return 2; }
            if (nsrc == 0 && ver.empty()) {
                fprintf(stderr, "kama pkg add: give a source (--git/--url/--path) or a --version (registry dependency)\n"); return 2;
            }
            if (!rev.empty()   && d.git.empty()) { fprintf(stderr, "kama pkg add: --rev is only valid with --git\n"); return 2; }
            if (!integ.empty() && d.url.empty()) { fprintf(stderr, "kama pkg add: --integrity is only valid with --url\n"); return 2; }
            if (!registry.empty() && nsrc != 0)  { fprintf(stderr, "kama pkg add: --registry is only valid for a registry dependency (no --git/--url/--path)\n"); return 2; }
            if (!ver.empty() && (!d.url.empty() || !d.path.empty())) {
                fprintf(stderr, "kama pkg add: --version applies to a git range (--git) or a registry dependency, not --url/--path\n"); return 2;
            }
            d.rev = rev; d.integrity = integ; d.version = ver; d.registry = registry;
            return cmdPkgAdd(".", name, d, dev);
        }
        if (verb == "remove") {
            std::string name;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg remove: unexpected option '%s'\n", a.c_str()); return 2; }
                else if (name.empty()) name = a;
                else { fprintf(stderr, "kama pkg remove: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            if (name.empty()) { fprintf(stderr, "kama pkg remove: missing <name>\n"); return 2; }
            return cmdPkgRemove(".", name);
        }
        fprintf(stderr, "kama pkg: unknown command '%s'\n", verb.c_str()); pkgUsage(); return 2;
    }

    std::vector<std::string> inputs;      // one or more .kama source files
    std::string output;
    std::string cc;                       // empty => pick default per target
    std::string target     = "native";    // native | wasm
    std::vector<std::string> links;        // -l libraries (FFI)
    bool        emitLines  = true;
    bool        keepC      = false;
    bool        webgpu     = false;
    bool        release    = false;        // debug by default
    bool        shared     = false;        // --shared: build a native .so/.dylib/.dll (expose entry points)
    std::vector<std::string> defines;      // --define NAME: activate a `@compileFor` flag (repeatable)
    std::vector<std::string> undefines;    // --undefine NAME: deactivate a default flag (repeatable)
    std::string configPath;                // --config PATH: explicit kama.json (else auto-discovered)
    bool        devBuild   = false;        // --dev: also put .kama/dev-deps on the import path (dev-dependencies)
    bool        querySymbols = false;      // `kama query --symbols`: dump the document outline
    std::string queryDef;                  // `kama query --def L:C`: go-to-definition at a cursor
    std::string queryType;                 // `kama query --type L:C`: hover (kind+name) at a cursor
    std::string queryRefs;                 // `kama query --refs L:C`: find-references at a cursor
    bool        queryProject = false;      // `kama query --project`: index the whole project, not one closure
    const bool  runMode    = (subcommand == "run");   // `kama run`: build to a temp binary, exec it, forward exit
    std::vector<std::string> progArgs;     // args after `--`, forwarded to the run child (run-only)

    // Options may appear in any order, before or after the input file.
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { for (++i; i < argc; ++i) progArgs.push_back(argv[i]); break; }   // rest are program args
        else if (a == "-o" && i + 1 < argc)       output = argv[++i];
        else if (a == "--cc" && i + 1 < argc)     cc = argv[++i];
        else if (a == "--link" && i + 1 < argc)   links.push_back(argv[++i]);
        else if (a == "--target" && i + 1 < argc) target = argv[++i];
        else if (a == "--no-line")                emitLines = false;
        else if (a == "--keep-c")                 keepC = true;
        else if (a == "--webgpu")                 webgpu = true;
        else if (a == "--shared")                 shared = true;
        else if (a == "--no-heap")                g_noHeap = true;   // reject heap allocation program-wide (MCU step 5)
        else if (a == "--release")                release = true;
        else if (a == "--debug")                  release = false;
        else if (a == "--define" && i + 1 < argc)   defines.push_back(argv[++i]);    // `@compileFor` flag on
        else if (a == "--undefine" && i + 1 < argc) undefines.push_back(argv[++i]);  // `@compileFor` flag off
        else if (a == "--config" && i + 1 < argc)   configPath = argv[++i];          // explicit kama.json
        else if (a == "--dev")                      devBuild = true;                 // also resolve dev-dependencies
        else if (a == "--symbols")                  querySymbols = true;             // `kama query` outline
        else if (a == "--def" && i + 1 < argc)      queryDef = argv[++i];            // `kama query` go-to-def L:C
        else if (a == "--type" && i + 1 < argc)     queryType = argv[++i];           // `kama query` hover L:C
        else if (a == "--refs" && i + 1 < argc)     queryRefs = argv[++i];           // `kama query` refs L:C
        else if (a == "--project")                  queryProject = true;             // `kama query` workspace scope
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "kama: unknown option '%s'\n", a.c_str()); usage(); return 2;
        }
        else                                      inputs.push_back(a);
    }

    // `-- <args>` are forwarded to the program `kama run` launches; they mean nothing to build/transpile.
    if (!runMode && !progArgs.empty()) {
        fprintf(stderr, "kama: `-- <args>` is only meaningful for `kama run`\n"); usage(); return 2;
    }

    // `kama run` with no file resolves the entry from the manifest `main` field (discovered via --config,
    // else kama.json in CWD). Explicit files still win. Do this before the empty-input check below.
    if (runMode && inputs.empty()) {
        std::string manifest = configPath;
        if (manifest.empty() && std::ifstream("kama.json").good()) manifest = "kama.json";
        if (manifest.empty()) {
            fprintf(stderr, "kama run: no input file and no kama.json in this directory\n"); return 2;
        }
        std::string mainRel, merr;
        if (!loadManifestMain(manifest, mainRel, merr)) {
            fprintf(stderr, "kama run: %s: %s\n", manifest.c_str(), merr.c_str()); return 2;
        }
        if (mainRel.empty()) {
            fprintf(stderr, "kama run: %s has no \"main\" entry (add \"main\": \"src/app.kama\") or pass a file\n",
                    manifest.c_str());
            return 2;
        }
        std::string mdir = dirName(manifest);
        inputs.push_back(mdir == "." ? mainRel : mdir + "/" + mainRel);
    }

    if (inputs.empty()) { fprintf(stderr, "kama: no input file\n"); usage(); return 2; }
    const std::string& input = inputs[0];   // first input drives default output naming

    if (target != "native" && target != "wasm" && target != "embedded") {
        fprintf(stderr, "kama: unknown --target '%s' (expected native|wasm|embedded)\n", target.c_str());
        return 2;
    }
    const bool wasm     = (target == "wasm");
    // --target embedded (MCU campaign step 3): a freestanding bare-metal build. It stops at a
    // `-ffreestanding -nostdlib` OBJECT (.o) — the board-specific link (crt0/startup + linker script /
    // memory map) is inherently per-chip and is the user's link step (or the turnkey step-6 toolchain),
    // exactly as every bare-metal toolchain separates compilation from the linker-script'd final image.
    const bool embedded = (target == "embedded");

    // `kama run` is native-only: it builds a hosted executable and execs it. wasm needs node/a browser;
    // embedded emits a freestanding object with no runnable entry — point those at `kama build`.
    if (runMode && (wasm || embedded)) {
        fprintf(stderr, "kama run is native-only (wasm needs node/a browser; embedded emits a freestanding "
                        "object) — use `kama build --target %s`\n", target.c_str());
        return 2;
    }

    // Build the active `@compileFor` flag set (the "structure" axis — reproducible, from the explicit
    // build invocation, never ambient env). Built-ins are auto-derived: NATIVE/WASM/EMBEDDED from
    // `--target`, DEBUG/RELEASE from `--release`. User flags come from `--define` (a `kama.json`
    // manifest layers declared defaults in on top — Stage 2). `--undefine` then removes.
    g_activeFlags.insert(embedded ? "EMBEDDED" : (wasm ? "WASM" : "NATIVE"));
    g_activeFlags.insert(release ? "RELEASE" : "DEBUG");
    g_release = release;   // `--release` also strips `debugAssert` (threaded to the emitter via setRelease)

    // Load the `kama.json` manifest if present (explicit `--config`, else auto-discovered next to the
    // input file, else CWD). It DECLARES the valid user-flag universe — enabling STRICT validation of
    // `@compileFor`/`--define` names (a typo like `WINODWS` is then rejected, not silently dropped) —
    // and may mark flags `"default": true` (active unless `--undefine`'d). No manifest -> permissive:
    // undeclared flags are simply inactive, so bare per-file builds need no config.
    {
        std::string manifest = configPath;
        if (manifest.empty()) {
            std::string dir; size_t slash = input.find_last_of('/');
            if (slash != std::string::npos) dir = input.substr(0, slash + 1);
            if      (std::ifstream(dir + "kama.json").good()) manifest = dir + "kama.json";
            else if (std::ifstream("kama.json").good())       manifest = "kama.json";
        }
        if (!manifest.empty()) {
            std::set<std::string> declared, defaults; std::string err;
            if (!loadManifestFlags(manifest, declared, defaults, err)) {
                fprintf(stderr, "kama: %s: %s\n", manifest.c_str(), err.c_str());
                return 2;
            }
            // Built-in flags (from --target/--release) are always valid and must not be redeclared.
            for (const char* r : {"NATIVE","WASM","EMBEDDED","DEBUG","RELEASE"})
                if (declared.count(r)) {
                    fprintf(stderr, "kama: %s: `%s` is a built-in flag (set by --target/--release) and "
                                    "cannot be declared in `flags`\n", manifest.c_str(), r);
                    return 2;
                }
            // Baked log default (M5): the manifest `log` section becomes the project's compiled-in KAMA_LOG
            // spec, seeded into the process env in `main` (overwrite=0, so `--log`/env still win).
            LogConfig logCfg; std::string logErr;
            if (!loadManifestLog(manifest, logCfg, logErr)) {
                fprintf(stderr, "kama: %s: %s\n", manifest.c_str(), logErr.c_str());
                return 2;
            }

            // `kama.local.json` (M5.2): a gitignored sibling of the manifest that DEEP-MERGES over it for the
            // fields the compiler reads directly here — `flags` (union: local declares/enables more) and `log`
            // (per-tag merge, local wins). Local-only by construction (never committed / never in the lockfile),
            // so it can never perturb a reproducible or CI build. Its dep `overrides` + `registries` +
            // `toolchain` local overrides (M5.3) are read on the install/selector paths (`resolveProject`,
            // `resolvePin`), not here.
            std::string mdir = dirName(manifest);
            std::string localManifest = (mdir == "." ? std::string() : mdir + "/") + "kama.local.json";
            if (std::ifstream(localManifest).good()) {
                std::set<std::string> ldeclared, ldefaults; std::string lerr;
                if (!loadManifestFlags(localManifest, ldeclared, ldefaults, lerr)) {
                    fprintf(stderr, "kama: %s: %s\n", localManifest.c_str(), lerr.c_str());
                    return 2;
                }
                for (const char* r : {"NATIVE","WASM","EMBEDDED","DEBUG","RELEASE"})
                    if (ldeclared.count(r)) {
                        fprintf(stderr, "kama: %s: `%s` is a built-in flag (set by --target/--release) and "
                                        "cannot be declared in `flags`\n", localManifest.c_str(), r);
                        return 2;
                    }
                declared.insert(ldeclared.begin(), ldeclared.end());   // union — local extends the universe
                defaults.insert(ldefaults.begin(), ldefaults.end());
                LogConfig localLog;
                if (!loadManifestLog(localManifest, localLog, lerr)) {
                    fprintf(stderr, "kama: %s: %s\n", localManifest.c_str(), lerr.c_str());
                    return 2;
                }
                logCfg.applyLocal(localLog);
            }

            g_declaredFlags = declared;
            g_strictFlags   = true;
            for (auto& d : defaults) g_activeFlags.insert(d);
            g_logDefault = logCfg.canonical();

            // Package deps (M2): if the manifest declares dependencies, the resolved view must already
            // be materialized. The build is a pure, reproducible READ of the view — it never fetches —
            // so a missing view is a user error pointing at `kama pkg install`, not a silent build. Under
            // `--dev` the dev-dependency view must exist too (else the dev build silently misses them).
            std::map<std::string, DepSpec> deps, devDeps; std::string derr;
            if (loadManifestDeps(manifest, deps, derr, &devDeps)) {
                std::string mdir = dirName(manifest);
                if (!deps.empty() && !dirExists(mdir + "/.kama/deps")) {
                    fprintf(stderr, "kama: %s declares dependencies but %s/.kama/deps is missing — run `kama pkg install`\n",
                            manifest.c_str(), mdir.c_str());
                    return 2;
                }
                if (devBuild && !devDeps.empty() && !dirExists(mdir + "/.kama/dev-deps")) {
                    fprintf(stderr, "kama: %s declares dev-dependencies but %s/.kama/dev-deps is missing — run `kama pkg install`\n",
                            manifest.c_str(), mdir.c_str());
                    return 2;
                }
            }
        }
    }

    // User flags: under strict mode (a manifest was loaded) validate names against the declared
    // universe before applying. `--define` adds; `--undefine` removes (e.g. turning off a default).
    {
        static const std::set<std::string> builtinFlags = {"NATIVE","WASM","EMBEDDED","DEBUG","RELEASE"};
        auto validate = [&](const std::string& name, const char* what) -> bool {
            if (!g_strictFlags || builtinFlags.count(name) || g_declaredFlags.count(name)) return true;
            fprintf(stderr, "kama: %s references undeclared flag `%s` (add it to the `flags` object in "
                            "kama.json)\n", what, name.c_str());
            return false;
        };
        for (auto& d : defines)   { if (!validate(d, "--define"))   return 2; g_activeFlags.insert(d); }
        for (auto& u : undefines) { if (!validate(u, "--undefine")) return 2; g_activeFlags.erase(u); }
    }

    // `--shared` is a native shared-library packaging step (desktop dev-loop hot-reload). On wasm
    // the host re-instantiates the module instead of `dlopen`ing it, so `expose` alone (KAMA_EXPORT
    // -> EMSCRIPTEN_KEEPALIVE) covers the web boundary — `--shared` is meaningless there.
    if (shared && wasm) {
        fprintf(stderr, "kama: --shared is native-only; a wasm build exports `expose`d functions "
                        "directly (no --shared needed)\n");
        return 2;
    }
    if (shared && embedded) {
        fprintf(stderr, "kama: --shared is native-only; --target embedded emits a freestanding object "
                        "you link into your firmware image\n");
        return 2;
    }

    // Release builds strip debug info and #line, optimize, and define NDEBUG.
    if (release) emitLines = false;

    // Where kama_runtime.h lives — resolved so an installed binary works from any
    // cwd (exe-relative: <exe>/../include, else <exe>, else <exe>/../.., else ".").
    std::string runtimeDir = resolveRuntimeDir(argv[0]);

    if (subcommand == "check") {
        // Semantic check only: parse + resolve + type-check + ownership/serde analysis, WITHOUT emitting C
        // or invoking a C compiler. Runs the exact `collectProgram` analysis a build runs (via the
        // analysis-mode CEmitter's `analyze()`), so it catches the analysis-phase diagnostics fast. This is
        // the front-end-as-library entry the LSP query path (and its test harness) build on.
        std::vector<SharedCompilationUnit> units;
        std::vector<std::string> unitPaths;
        if (!loadProgramUnits(inputs, argv[0], units, unitPaths, devBuild)) return 1;

        CEmitter idx(input);                 // analysis mode: no output stream
        idx.setPrelude(preludeUnit());       // Optional/Result available implicitly
        idx.setNoHeap(g_noHeap);
        idx.setRelease(g_release);
        idx.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);
        idx.setLogDefault(g_logDefault);
        for (auto& m : preludeModuleUnits()) idx.addPreludeModule(m);
        idx.analyze(units);
        const auto& diags = idx.diagnostics();
        for (const auto& d : diags) {
            const char* sev = d.severity == DiagSeverity::Error ? "error"
                            : d.severity == DiagSeverity::Warning ? "warning" : "note";
            fprintf(stderr, "%s:%d:%d: %s: %s\n",
                    d.file.c_str(), d.line, d.column, sev, d.message.c_str());
        }
        if (!diags.empty()) {
            fprintf(stderr, "kama: %s FAILED (%zu diagnostic%s)\n",
                    input.c_str(), diags.size(), diags.size() == 1 ? "" : "s");
            return 1;
        }
        fprintf(stderr, "kama: %s OK (%zu unit%s analyzed)\n",
                input.c_str(), units.size(), units.size() == 1 ? "" : "s");
        return 0;
    }

    if (subcommand == "query") {
        // Debug harness for the LSP query index (M0 T4/T5): runs analyze() and dumps the requested query
        // over the resulting index, in deterministic text. Mirrors `check`'s front-end-as-library setup;
        // the `kama lsp` server (M1) will call the same CEmitter query methods and map them to protocol JSON.
        //   kama query <file> --symbols      document outline (one `L:C kind name` line per user decl)
        //   kama query <file> --def  L:C     go-to-definition at 1-based line:col
        //   kama query <file> --type L:C     hover (kind + name) at 1-based line:col
        //   kama query <file> --refs L:C     find-references (decl + every use) at 1-based line:col
        //   kama query <file> --project      widen the unit set from <file>'s import closure to the whole
        //                                    project (M3.5 workspace indexing), so --refs sees files that
        //                                    use <file> without being imported by it
        std::vector<SharedCompilationUnit> units;
        std::vector<std::string> unitPaths;
        std::vector<std::string> queryInputs = inputs;
        if (queryProject) {
            // No editor here to declare a workspace, so lspFindProject falls back to the nearest kama.json
            // (see its contract) — which is what a CLI user in a package expects.
            LspProject proj = lspFindProject(input, "");
            if (proj.root.empty()) {
                fprintf(stderr, "kama query --project: %s is not inside a kama package (no kama.json found)\n",
                        input.c_str());
                return 1;
            }
            if (proj.tooLarge) {
                fprintf(stderr, "kama query --project: %s holds more than %zu .kama files\n",
                        proj.root.c_str(), proj.cap);
                return 1;
            }
            queryInputs = proj.files;
        }
        if (!loadProgramUnits(queryInputs, argv[0], units, unitPaths, devBuild)) return 1;
        // Every query re-picks the unit by an EXACT name match (CEmitter::unitForUri), and the project
        // enumeration yields absolute paths — so a relative `input` would match nothing. Ask by the same
        // spelling the units were parsed with. (The LSP server is immune: file:// URIs are already absolute.)
        const std::string queryUri = queryProject ? absolutePath(input) : input;

        CEmitter idx(input);
        idx.setPrelude(preludeUnit());
        idx.setNoHeap(g_noHeap);
        idx.setRelease(g_release);
        idx.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);
        idx.setLogDefault(g_logDefault);
        for (auto& m : preludeModuleUnits()) idx.addPreludeModule(m);
        idx.analyze(units);

        auto parseLC = [](const std::string& s, int& l, int& c) -> bool {
            auto colon = s.find(':');
            if (colon == std::string::npos) return false;
            l = atoi(s.substr(0, colon).c_str());
            c = atoi(s.substr(colon + 1).c_str());
            return true;
        };

        if (querySymbols) {
            for (const auto& s : idx.documentSymbols(queryUri))
                printf("%d:%d %s %s\n", s.selectionRange.line, s.selectionRange.column,
                       symKindName(s.kind), s.name.c_str());
            return 0;
        }
        if (!queryDef.empty()) {
            int l, c;
            if (!parseLC(queryDef, l, c)) { fprintf(stderr, "kama query: --def wants L:C\n"); return 2; }
            Location loc = idx.definitionAt(queryUri, l, c);
            if (loc.range.line == 0) { printf("no definition\n"); return 0; }
            printf("%s:%d:%d\n", loc.uri.c_str(), loc.range.line, loc.range.column);
            return 0;
        }
        if (!queryType.empty()) {
            int l, c;
            if (!parseLC(queryType, l, c)) { fprintf(stderr, "kama query: --type wants L:C\n"); return 2; }
            std::string t = idx.typeAtPosition(queryUri, l, c);
            printf("%s\n", t.empty() ? "no type" : t.c_str());
            return 0;
        }
        if (!queryRefs.empty()) {
            int l, c;
            if (!parseLC(queryRefs, l, c)) { fprintf(stderr, "kama query: --refs wants L:C\n"); return 2; }
            auto refs = idx.referencesAt(queryUri, l, c, /*includeDecl*/ true);
            if (refs.empty()) { printf("no references\n"); return 0; }
            for (const auto& r : refs)
                printf("%s:%d:%d\n", r.uri.c_str(), r.range.line, r.range.column);
            return 0;
        }
        fprintf(stderr, "kama query: pass --symbols, --def L:C, --type L:C, or --refs L:C\n");
        return 2;
    }

    if (subcommand == "transpile") {
        // Transpile to ONE .c. A file with no imports stays on the single-unit fast path; anything that
        // `import`s modules pulls in every transitive unit (loadProgramUnits, as `build` does) and folds
        // them into one self-contained translation unit.
        std::string outPath = output.empty() ? (stripExtension(input) + ".c") : output;
        std::vector<SharedCompilationUnit> units;
        std::vector<std::string> unitPaths;
        if (!loadProgramUnits(inputs, argv[0], units, unitPaths, devBuild)) return 1;
        int rc = (units.size() == 1)
               ? transpileUnitToFile(units[0], unitPaths[0], outPath, emitLines)
               : transpileProgramToSingleFile(units, unitPaths, outPath, emitLines);
        if (rc == 0) fprintf(stderr, "kama: wrote %s\n", outPath.c_str());
        return rc;
    }

    if (subcommand == "build" || runMode) {
        // `kama run` reuses the whole native build path, but builds into a throwaway binary it execs and
        // removes afterward (mirrors the store staging name at fetchToStore). Force the output there so any
        // stray `-o` can't leave an artifact behind.
        if (runMode) {
            const char* td = getenv("TMPDIR");
            std::string tmpDir = (td && *td) ? td : "/tmp";
            if (!tmpDir.empty() && tmpDir.back() == '/') tmpDir.pop_back();
            output = tmpDir + "/kama-run-" + std::to_string((long)getpid());
        }
        // Compiler: native uses a bundled `zig cc` if present else system clang; wasm uses
        // emcc (emcc keys output format off the -o extension). --cc / $EMCC override.
        std::string compiler = cc;
        if (compiler.empty()) {
            if (wasm) {
                const char* env = getenv("EMCC");
                compiler = env ? env : "emcc";
            } else {
                compiler = resolveCCompiler(argv[0]);   // bundled zig cc, else system clang
            }
        }

        // Default output: native -> bare exe name (or lib<name>.<so|dylib|dll> for --shared);
        // wasm -> an HTML harness (emcc also emits the .js + .wasm alongside it).
#if defined(_WIN32)
        const char* sharedExt = ".dll";
#elif defined(__APPLE__)
        const char* sharedExt = ".dylib";
#else
        const char* sharedExt = ".so";
#endif
        std::string defaultOut = wasm     ? (stripExtension(input) + ".html")
                               : embedded ? (stripExtension(input) + ".o")
                               : shared   ? (stripExtension(input) + sharedExt)
                                          : stripExtension(input);
        std::string outPath    = output.empty() ? defaultOut : output;

        // Transpile to one or more .c (multi-file emits a shared header too).
        // Generated files land in the output directory; cleaned unless --keep-c.
        std::string genDir = dirName(outPath);
        std::vector<std::string> cFiles;     // .c to compile
        std::vector<std::string> genFiles;   // generated files to remove afterwards
        std::string headerDir;

        // Parse the CLI inputs and transitively pull in every imported module. A single
        // file with no imports stays on the fast path (one .c, no shared header); anything
        // that drags in more units (multiple inputs, or `import`s) uses the multi-file path.
        std::vector<SharedCompilationUnit> units;
        std::vector<std::string> unitPaths;
        if (!loadProgramUnits(inputs, argv[0], units, unitPaths, devBuild)) return 1;

        bool needsLibm = false;   // set if the program `extern "<math.h>";`'s (std::math / libm) -> link -lm
        bool needsNetWeb = false; // set if the program `extern "kama_net_web.h";`'s (std::net::web) -> --js-library
        bool needsApp = false;    // set if the program `extern "kama_app.h";`'s (std::app) -> wasm -sEXIT_RUNTIME=1
        bool needsGpu = false;    // set if the program `extern "kama_gpu.h";`'s (WebGPU seam) -> native surface libs
        bool needsPthread = false;// set if the program uses a std::concurrent seam (`kama_isolate.h` / `kama_channel.h`) -> native -lpthread
        if (units.size() == 1) {
            std::string cPath = stripExtension(input) + ".c";
            if (transpileUnitToFile(units[0], unitPaths[0], cPath, emitLines, &needsLibm, &needsNetWeb, &needsApp, &needsGpu, &needsPthread) != 0) return 1;
            cFiles.push_back(cPath);
            genFiles.push_back(cPath);
        } else if (release && !wasm) {
            // UNITY release build: fold every unit into ONE translation unit (the same merge the
            // `transpile` command uses) instead of per-module .c files. kama has no incremental object
            // cache — a build already hands all .c to a single clang invocation — so the multi-file split
            // buys nothing and *costs* cross-module inlining: with separate TUs the C compiler can't inline
            // a stdlib call (a `std::math` Vec4 op, a collection accessor) into the user's hot loop, so
            // numeric code stays out-of-line and never auto-vectorizes. One TU lets clang inline + vectorize
            // it, landing hot math at C parity (measured ~6×→1× on the `math` bench; LTO across separate TUs
            // recovers only part of it — docs/design/simd.md § M0). Debug keeps per-module .c for faithful
            // stepping; wasm keeps its own path.
            std::string cPath = genDir + "/" + baseName(stripExtension(outPath)) + ".c";
            if (transpileProgramToSingleFile(units, unitPaths, cPath, emitLines, &needsLibm, &needsNetWeb, &needsApp, &needsGpu, &needsPthread) != 0) return 1;
            cFiles.push_back(cPath);
            genFiles.push_back(cPath);
        } else {
            std::string headerName = baseName(stripExtension(outPath)) + ".gen.h";
            std::string headerPath = genDir + "/" + headerName;
            headerDir = genDir;
            std::vector<std::string> cPaths;   // one per unit; index-suffixed so distinct dirs never collide
            for (size_t i = 0; i < units.size(); ++i)
                cPaths.push_back(genDir + "/" + stripExtension(baseName(unitPaths[i])) + "_" + std::to_string(i) + ".c");
            if (emitProgramUnits(units, unitPaths, headerPath, headerName, cPaths, emitLines, &needsLibm, &needsNetWeb, &needsApp, &needsGpu, &needsPthread) != 0) return 1;
            cFiles   = cPaths;
            genFiles = cPaths;
            genFiles.push_back(headerPath);
        }

        // Native --webgpu needs the fetched wgpu-native SDK. Fail early with an actionable message
        // rather than a raw "webgpu/webgpu.h not found" from the C compiler.
        std::string wgpuDir;
        if (!wasm && webgpu) {
            wgpuDir = resolveWgpuDir();
            if (!fileExists(wgpuDir + "/include/webgpu/webgpu.h")) {
                fprintf(stderr,
                    "kama: native --webgpu needs the wgpu-native SDK, not found at '%s'.\n"
                    "      Run tools/fetch-webgpu.sh, or set $KAMA_WGPU_DIR to a drop with include/ + lib/.\n",
                    wgpuDir.c_str());
                return 2;
            }
        }

        std::ostringstream cmd;
        // Promote two silent-UB classes to hard errors (the front end has no return-path
        // / definite-assignment analysis yet): a non-void function that falls off the end,
        // and a read of an uninitialized local. The #line directives map these back to the
        // .kama source. (Audit Step 2 — "no silent surprises".)
        cmd << compiler << " -std=c11 -Werror=return-type -Werror=uninitialized ";
        // Binding a callback-based C API (WebGPU/GLFW/SDL/…) means handing a kama `fnptr` to a C
        // callback field. At the `extern` boundary the user asserts ABI compatibility the same way a
        // C cast would — but a kama callback lowers enums to `int` and typed handles to `void*`, which
        // clang (error-by-default since v16) flags as an incompatible function-pointer type. kama can't
        // name those C types, so demote it to a warning (still visible) rather than a hard error — the
        // FFI boundary is the sanctioned unsafe seam.
        cmd << "-Wno-error=incompatible-function-pointer-types ";
        // --shared: emit a position-independent shared library. -fvisibility=hidden hides everything
        // by default; only `expose`d functions (KAMA_EXPORT -> visibility("default"),used) reach the
        // dynamic symbol table, so a host `dlopen`+`dlsym`s exactly the declared entry points. `used`
        // also keeps them past -dead_strip/--gc-sections. (native-only — rejected with --target wasm.)
        if (shared) cmd << "-fPIC -shared -fvisibility=hidden ";
        if (release) {
            // Optimized, no debug info, asserts off. Native uses -O3 (max speed — matches Rust's release
            // default); wasm uses -Oz (size — download cost dominates). -ffunction/data-sections +
            // --gc-sections let the linker drop unused (std)library code — the
            // "pay for what you use" pruning lever. Native also strips symbols.
            cmd << (wasm ? "-Oz " : "-O3 ") << "-DNDEBUG -ffunction-sections -fdata-sections ";
            if (!wasm && !embedded) {   // -Wl,*/-s are link-time; embedded stops at -c (see below)
#ifdef __APPLE__
                cmd << "-Wl,-dead_strip ";
#else
                cmd << "-Wl,--gc-sections ";
#endif
                cmd << "-s ";
            }
        } else {
            // Debug: faithful stepping + breakpoints in .kama via #line (DWARF `-g`). We intentionally do
            // NOT pass `-gsource-map` on wasm: it makes the generated JS load a `.wasm.map` at startup via a
            // chain of runtime symbols (`addRunDependency`, `UTF8ArrayToString`, …) that newer emcc (≥6) does
            // not include by default when the `.js` is run under node — only a browser provides them — which
            // crashes bare-node execution (the test harness + CI). The DWARF info still supports in-browser
            // debugging; browser-devtools .kama source-mapping is a deferred nicety if it's ever wanted back.
            cmd << "-g -O0 ";
        }
        // Numeric safety — no arithmetic UB (Rust's model). Divide-by-zero, shift-past-width, and
        // out-of-range float->int all TRAP in EVERY build (they're always bugs). Signed overflow TRAPS in
        // debug (catch the accidental-overflow bug in dev) and WRAPS (defined two's-complement, -fwrapv,
        // zero-cost) in release. `-fsanitize-trap` lowers to `__builtin_trap` — a clean abort with NO
        // sanitizer-runtime dependency. `shift-exponent` only (not `shift-base`), so `1 << 31` (setting the
        // sign bit) stays legal. Intentional signed wrap is opt-in (unsigned math, or a `wrapping*` helper).
        cmd << "-fsanitize=integer-divide-by-zero,shift-exponent,float-cast-overflow "
               "-fsanitize-trap=integer-divide-by-zero,shift-exponent,float-cast-overflow ";
        // Signed overflow: debug TRAPS it; release `-fwrapv`-WRAPS `+`/`-`/`*` (defined). `INT_MIN / -1` is
        // NOT defined by `-fwrapv`, so we ALSO keep the overflow-trap in release — it wraps the ordinary
        // ops (trap suppressed by `-fwrapv`) but still traps that one pathological division. Net: no
        // arithmetic UB in either build.
        cmd << "-fsanitize=signed-integer-overflow -fsanitize-trap=signed-integer-overflow ";
        if (release) cmd << "-fwrapv ";
        // --target embedded: a freestanding, hosted-runtime-free compile that stops at an OBJECT. No libc
        // (`-nostdlib`), no OS/hosting assumptions (`-ffreestanding`), and `-c` so no link is attempted —
        // the crt0/startup + linker script are the user's per-chip link step. `-DKAMA_TARGET_EMBEDDED`
        // selects the freestanding `main`/panic forms in the emitted C + runtime. The CPU triple
        // (`-target thumbv*-none-eabi -mcpu=...`) is deliberately NOT baked in v1 — pass it via `--cc`.
        if (embedded) cmd << "-ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED -c ";
        cmd << "-I" << runtimeDir << " -I" << dirName(absolutePath(input)) << " -I. ";
        if (!headerDir.empty()) cmd << "-I" << headerDir << " ";   // the shared generated header
        if (wasm && webgpu) cmd << "--use-port=emdawnwebgpu ";   // emscripten WebGPU port
        // Native --webgpu: find wgpu-native's webgpu.h / wgpu.h. (wasm gets its header from the port above.)
        if (!wasm && webgpu) cmd << "-I\"" << wgpuDir << "/include\" ";
        // The WebGPU platform seam (kama_gpu.h) — its include dir on BOTH targets (web = header-only
        // static-inline; native also compiles kama_gpu.c below). Only when the program externs it.
        if (needsGpu) cmd << "-I\"" << (resolveStdlibDir(argv[0]) + "/std/gpu") << "\" ";
#if defined(__APPLE__)
        // GLFW usually lives under a Homebrew prefix the bare compiler doesn't search by default.
        if (!wasm && needsGpu) cmd << "-I/opt/homebrew/include -I/usr/local/include ";
#endif
        // std::net::web (WebSocket / WebTransport) is a thin JS-glue --js-library, linked only when the
        // program actually externs its header. Both the header (compile) and the glue (link) live next to the
        // module in the stdlib. The glue moves bytes across the wasm/JS boundary via the exported heap views.
        if (needsNetWeb) {
            std::string webdir = resolveStdlibDir(argv[0]) + "/std/net/web";
            cmd << "-I\"" << webdir << "\" ";                          // find kama_net_web.h at compile
            if (wasm)
                cmd << "--js-library \"" << webdir << "/kama_net_web.js\" "
                    << "-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8 ";
        }
        // std::app's run loop keeps the wasm runtime alive (emscripten_set_main_loop); EXIT_RUNTIME lets
        // its quit() (emscripten_force_exit) shut down cleanly with a real exit code. The isolate seam needs
        // it too: under -sPROXY_TO_PTHREAD `main` runs on a worker, and EXIT_RUNTIME is what carries its
        // return value out as the process exit code (else node sees 0 regardless).
        if (wasm && (needsApp || needsPthread)) cmd << "-sEXIT_RUNTIME=1 ";
        for (auto& cf : cFiles) cmd << "\"" << cf << "\" ";
        // Native WebGPU seam: compile the surface TU (Objective-C on macOS — it attaches a CAMetalLayer
        // to the NSWindow) and link GLFW + the window-system libs. Only when the program externs
        // kama_gpu.h AND targets native (the web seam is header-only static-inline, compiled nowhere).
        if (!wasm && needsGpu) {
            std::string seam = resolveStdlibDir(argv[0]) + "/std/gpu/kama_gpu.c";
#if defined(__APPLE__)
            cmd << "-x objective-c \"" << seam << "\" -x none ";
#else
            cmd << "\"" << seam << "\" ";
#endif
        }
        // Native --webgpu: link wgpu-native, with an rpath so the .dylib/.so is found at run time (dev
        // loop — a shipped app would bundle it). The link stays out of the wasm path (emcc port covers it).
        if (!wasm && webgpu) {
            cmd << "-L\"" << wgpuDir << "/lib\" -lwgpu_native ";
#if !defined(_WIN32)
            cmd << "-Wl,-rpath,\"" << absolutePath(wgpuDir) << "/lib\" ";
#endif
        }
        // The seam's window/surface libraries (GLFW + platform frameworks). Split from wgpu-native
        // above so a windowless native build (e.g. the link-gate smoke) links only libwgpu_native.
        if (!wasm && needsGpu) {
#if defined(__APPLE__)
            cmd << "-L/opt/homebrew/lib -L/usr/local/lib -lglfw "
                   "-framework Cocoa -framework Metal -framework QuartzCore -framework IOKit "
                   "-framework CoreFoundation -framework CoreVideo -lobjc ";
#elif defined(_WIN32)
            cmd << "-lglfw3 -lgdi32 -luser32 -ld3dcompiler ";
#else
            cmd << "-lglfw -lX11 -ldl -lpthread ";
#endif
        }
        // Link-time libraries (skipped for --target embedded: it stops at `-c`, so its board link — where
        // the user supplies startup + linker script — owns library selection).
        if (!embedded) for (auto& lib : links) cmd << "-l" << lib << " ";   // FFI link flags
        // Pay-for-what-you-use: link libm only when the program pulls in <math.h> (std::math or any libm
        // FFI). Native only — wasm/emscripten bundles libm. (--gc-sections still prunes unused code.)
        if (needsLibm && !wasm && !embedded) cmd << "-lm ";
        // Pay-for-what-you-use: wire up threads only when the program uses the isolate seam (std::concurrent's
        // kama_isolate.h / kama_channel.h). Native: link libpthread (harmless on macOS — pthreads live in libc;
        // required on Linux). Wasm: emscripten pthreads = Web Workers over a shared SharedArrayBuffer, so the
        // same pthread_* C compiles unchanged (mutex/cond lower to Atomics.wait). -sPROXY_TO_PTHREAD runs
        // `main` on a dedicated worker so it may block on join/recv (Atomics.wait THROWS on the JS main
        // thread). PTHREAD_POOL_SIZE pre-warms worker slots (KAMA_PTHREAD_POOL, default 0); STRICT=0 lets the
        // pool grow on demand so a `scope` with more children than the pool never stalls — pre-warm is a pure
        // latency knob, not a correctness cap.
        if (needsPthread && !embedded) {
            if (wasm) {
                const char* pool = getenv("KAMA_PTHREAD_POOL");   // build-time override; unset => 0 (grow on demand)
                cmd << "-pthread -sPROXY_TO_PTHREAD "
                    << "-sPTHREAD_POOL_SIZE=" << (pool && *pool ? pool : "0") << " "
                    << "-sPTHREAD_POOL_SIZE_STRICT=0 ";
            } else {
                cmd << "-lpthread ";
            }
            // M6.3: parallel_for's default worker count. KAMA_PARFOR_WORKERS (build-time) pins K for
            // deterministic CI; unset => 0 => the emitted code calls kama_parfor_workers() (hw cores) at
            // runtime. Only a *count* knob — slices are disjoint + joined, so K never changes results.
            const char* pfw = getenv("KAMA_PARFOR_WORKERS");
            cmd << "-DKAMA_PARFOR_WORKERS_DEFAULT=" << (pfw && *pfw ? pfw : "0") << " ";
        }
#if defined(_WIN32)
        // std::net uses Winsock (kama_os.h). Link ws2_32 on native Windows builds; harmless (and pruned by
        // --gc-sections) for programs that don't open a socket. POSIX sockets need no extra lib.
        if (!wasm && !embedded) cmd << "-lws2_32 ";
#endif
        cmd << "-o \"" << outPath << "\"";
        int rc = runCmd(cmd.str());

        if (!keepC) for (auto& gf : genFiles) remove(gf.c_str());
        if (rc != 0) {
            fprintf(stderr, "kama: %s failed (exit %d)\n", compiler.c_str(), rc);
            return rc;
        }
        // `kama run`: exec the freshly built binary, forward its exit code, then remove the temp. `-- <args>`
        // are forwarded (quoted) — inert until argv marshaling lands, but wired at the process boundary now.
        if (runMode) {
            std::ostringstream run;
            run << "\"" << outPath << "\"";
            for (auto& pa : progArgs) run << " \"" << pa << "\"";
            int prc = runCmd(run.str());   // system() -> WEXITSTATUS: the child's exit code
            remove(outPath.c_str());
            return prc;
        }
        fprintf(stderr, "kama: built %s\n", outPath.c_str());
        return 0;
    }

    fprintf(stderr, "kama: unknown subcommand '%s'\n", subcommand.c_str());
    usage();
    return 2;
}
