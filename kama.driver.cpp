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
#endif

#include "kama.parser.hpp"
#include "kama.lexer.hpp"
#include "kama.context.h"
#include "kama.cemit.h"
#include "kama.prelude.h"   // KAMA_PRELUDE_SRC + KAMA_PRELUDE_MODULES (embedded built-in kama)

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
// cwd: $KAMA_HOME, else <exeDir>/../include (bin/kama -> ../include), else
// <exeDir> (repo root layout), else ".".
std::string resolveRuntimeDir(const char* argv0)
{
    if (const char* home = getenv("KAMA_HOME")) return home;
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
    if (fileExists(exeDir + "/../include/kama_runtime.h")) return exeDir + "/../include";
    if (fileExists(exeDir + "/kama_runtime.h"))            return exeDir;
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

// The stdlib root, resolved from the binary like resolveRuntimeDir: $KAMA_HOME/lib,
// else <exeDir>/../lib/kama (installed, bin/kama -> ../lib/kama), else <exeDir>/lib
// (repo/dev layout), else "lib". The stdlib ships INSIDE the install; `std::*` resolves here.
std::string resolveStdlibDir(const char* argv0)
{
    if (const char* home = getenv("KAMA_HOME")) return std::string(home) + "/lib";
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
    if (dirExists(exeDir + "/../lib/kama")) return exeDir + "/../lib/kama";
    if (dirExists(exeDir + "/lib"))          return exeDir + "/lib";
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

// `@compileFor(FLAG)` conditional compilation (the "structure" axis): the active build-flag set
// (built-ins derived from `--target`/`--release`, plus `--define`), the declared-flag universe (from
// `kama.json`, Stage 2), and whether strict flag-name validation is on. File-scope like `g_noHeap`,
// populated in main, threaded to each CEmitter via `setBuildFlags`.
static std::set<std::string> g_activeFlags;
static std::set<std::string> g_declaredFlags;
static bool g_strictFlags = false;

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
    std::string version;    // optional metadata (resolver's single-version-per-major check)
};

// Two dependency specs name "the same package" iff every identity-bearing field matches. With no SemVer
// yet, this IS the whole conflict test: identical -> dedup in the resolver, divergent -> hard error.
static bool sameSpec(const DepSpec& a, const DepSpec& b)
{
    return a.path == b.path && a.git == b.git && a.url == b.url
        && a.rev == b.rev && a.integrity == b.integrity;
}

struct ManifestReader {
    const std::string& s;
    size_t i = 0;
    std::string err;
    std::set<std::string>& declared;
    std::set<std::string>& defaults;
    std::map<std::string, DepSpec>* deps = nullptr;      // set to capture `dependencies` (else it's skipped)
    std::map<std::string, DepSpec>* devDeps = nullptr;   // set to capture `dev-dependencies` (else skipped)
    std::string* mainOut = nullptr;                       // set to capture the `main` entry field (else skipped)
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
                // unknown keys ignored (forward-compat)
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}' in a dependency body");
            }
            if (target) (*target)[name] = spec;
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `dependencies`");
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
            else if (key == "main" && mainOut) { if (!str(*mainOut)) return false; }   // entry `.kama` (read by `kama run`)
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
                             std::map<std::string, DepSpec>* devDeps = nullptr)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.deps = &deps;
    r.devDeps = devDeps;   // optional: also capture `dev-dependencies` (M2.2)
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

// One resolved package in `kama.lock`. The lock is what the build's view is materialized from — the
// reproducibility record: (source, pinned identity, integrity, transitive deps).
struct LockEntry {
    std::string source;                     // "path" | "git" | "url"
    std::string path, git, url, rev, commit, integrity;
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
        if (!e.rev.empty())       out << ", \"rev\": \""       << jsonEscape(e.rev)       << "\"";
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
            else if (k == "rev")       { if (!str(e.rev))       return false; }
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
    emitter.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);   // `@compileFor` conditional compilation
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
    emitter.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);   // `@compileFor` conditional compilation
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

// `kama update [--version vX.Y.Z]` — self-update by re-running the canonical installer,
// which re-detects a C compiler (slim vs bundled zig) so the install flavor stays consistent.
int cmdUpdate(const std::string& pinned)
{
#ifdef _WIN32
    std::string env = pinned.empty() ? "" : "$env:KAMA_VERSION='" + pinned + "'; ";
    std::string cmd = "powershell -NoProfile -Command \"" + env +
                      "irm https://kama-lang.org/install.ps1 | iex\"";
#else
    std::string env = pinned.empty() ? "" : "KAMA_VERSION=" + pinned + " ";
    std::string cmd = env + "curl -fsSL https://kama-lang.org/install.sh | sh";
#endif
    return runCmd(cmd);   // the installer prints old->new; verify with `kama --version`
}

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

// The shared content-addressed store root. `~/.kama/store` (user-mutable, install-independent) with a
// `KAMA_STORE` override for isolation (the guard test points it at a tmp dir so it never touches the
// real store — and NOT KAMA_HOME, which selects the read-only install root/stdlib).
std::string storeDir()
{
    if (const char* s = getenv("KAMA_STORE")) return s;
#ifdef _WIN32
    if (const char* u = getenv("USERPROFILE")) return std::string(u) + "/.kama/store";
#else
    if (const char* h = getenv("HOME")) return std::string(h) + "/.kama/store";
#endif
    return ".kama/store";   // degenerate fallback (no HOME): project-local, still functional
}

// M2.1: fetch a `git`/`url` dependency into the content-addressed store, verify sha256 integrity, and
// fill `out` (lock entry) + `storePath` (target for the view symlink). Staging is atomic — nothing
// enters `store/<name>-<hash>` until the tree hash is known, so a killed fetch leaves no half entry.
static bool fetchToStore(const std::string& name, const DepSpec& d,
                         LockEntry& out, std::string& storePath, std::string& err)
{
    std::string store = storeDir();
    if (!makeDirs(store)) { err = "cannot create store at " + store; return false; }
    std::string staging = store + "/.tmp-" + std::to_string((long)getpid()) + "-" + name;
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

    std::string finalDir = store + "/" + name + "-" + hash.substr(sizeof("sha256-") - 1);
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
            (!spec.url.empty() && L.source == "url" && L.url == spec.url &&
                (spec.integrity.empty() || spec.integrity == L.integrity));
        if (unchanged) {
            std::string key = L.treeHash.empty() ? L.integrity : L.treeHash;   // the store dir hash
            std::string finalDir;
            if (key.size() > sizeof("sha256-") - 1)
                finalDir = storeDir() + "/" + name + "-" + key.substr(sizeof("sha256-") - 1);
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
    if (!loadManifestDeps(manifest, deps, err, &devDeps)) {
        fprintf(stderr, "kama: %s: %s\n", manifest.c_str(), err.c_str()); return 2;
    }

    // Rebuild both views from scratch so they can never drift from the manifest/lock.
    std::string viewDir    = base + "/.kama/deps";
    std::string devViewDir = base + "/.kama/dev-deps";
    runCmd(rmRfCmd(viewDir));
    runCmd(rmRfCmd(devViewDir));
    if (!makeDirs(viewDir)) { fprintf(stderr, "kama install: cannot create %s\n", viewDir.c_str()); return 1; }

    struct Req { std::string name; DepSpec spec; std::string requestor; bool dev; };
    std::map<std::string, DepSpec> chosen;        // name -> the one resolved spec (conflict guard)
    std::map<std::string, std::string> chosenBy;  // name -> first requestor (conflict diagnostics)
    std::map<std::string, LockEntry> lock;        // output
    bool madeDevDir = false;

    auto drain = [&](std::deque<Req>& q) -> int {
        while (!q.empty()) {
            Req r = q.front(); q.pop_front();
            auto ci = chosen.find(r.name);
            if (ci != chosen.end()) {
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
            chosen[r.name] = r.spec; chosenBy[r.name] = r.requestor;

            LockEntry e; std::string storePath, ferr;
            if (!resolveOne(r.name, r.spec, base, oldLock, e, storePath, ferr)) {
                fprintf(stderr, "kama install: %s\n", ferr.c_str()); return 1;
            }
            e.dev = r.dev;
            if (r.dev && !madeDevDir) {
                if (!makeDirs(devViewDir)) { fprintf(stderr, "kama install: cannot create %s\n", devViewDir.c_str()); return 1; }
                madeDevDir = true;
            }
            if (!linkDir(storePath, (r.dev ? devViewDir : viewDir) + "/" + r.name)) {
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
    if (int rc = drain(prodQ)) return rc;   // phase 1 (prod) drains fully before phase 2 → prod wins on shared names
    if (int rc = drain(devQ))  return rc;   // phase 2 (dev)

    if (!writeLockFile(base + "/kama.lock", lock)) {
        fprintf(stderr, "kama install: cannot write %s/kama.lock\n", base.c_str()); return 1;
    }
    fprintf(stderr, "kama: installed %zu package(s) into %s\n", lock.size(), base.c_str());
    return 0;
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
    add("path", d.path); add("git", d.git); add("url", d.url); add("rev", d.rev); add("integrity", d.integrity);
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
        "  kama pkg install [<dir>]            resolve `kama.json` (dev-)dependencies into .kama/{deps,dev-deps} + kama.lock\n"
        "  kama pkg add   [--dev] <name> (--git U [--rev R] | --url U [--integrity H] | --path P)\n"
        "  kama pkg remove <name>\n"
        "  kama pkg update [<pkg>]             re-resolve pins (advance a branch pin) and rewrite the lock\n"
        "  kama update    [--version vX.Y.Z]   self-update the toolchain via the installer\n"
        "  kama --version\n");
}

void pkgUsage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama pkg install [<dir>]            resolve dependencies into .kama/{deps,dev-deps} + kama.lock\n"
        "  kama pkg add   [--dev] <name> (--git U [--rev R] | --url U [--integrity H] | --path P)\n"
        "  kama pkg remove <name>\n"
        "  kama pkg update [<pkg>]             re-resolve pins and rewrite the lock\n");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("kama %s\n", KAMA_VERSION);
        return 0;
    }
    if (argc < 2) { usage(); return 2; }

    std::string subcommand = argv[1];

    if (subcommand == "update") {
        std::string pinned;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--version" && i + 1 < argc) pinned = argv[++i];
            else { fprintf(stderr, "kama update: unexpected arg '%s'\n", a.c_str()); return 2; }
        }
        return cmdUpdate(pinned);
    }

    if (subcommand == "pkg") {
        if (argc < 3) { pkgUsage(); return 2; }
        std::string verb = argv[2];
        if (verb == "install") {
            std::string dir;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg install: unexpected option '%s'\n", a.c_str()); return 2; }
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
            bool dev = false; std::string name; DepSpec d; std::string rev, integ;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if      (a == "--dev")                     dev = true;
                else if (a == "--git" && i + 1 < argc)     d.git = argv[++i];
                else if (a == "--url" && i + 1 < argc)     d.url = argv[++i];
                else if (a == "--path" && i + 1 < argc)    d.path = argv[++i];
                else if (a == "--rev" && i + 1 < argc)     rev = argv[++i];
                else if (a == "--integrity" && i + 1 < argc) integ = argv[++i];
                else if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg add: unknown option '%s'\n", a.c_str()); return 2; }
                else if (name.empty()) name = a;
                else { fprintf(stderr, "kama pkg add: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            if (name.empty()) { fprintf(stderr, "kama pkg add: missing <name>\n"); return 2; }
            int nsrc = (!d.git.empty()) + (!d.url.empty()) + (!d.path.empty());
            if (nsrc != 1) { fprintf(stderr, "kama pkg add: exactly one of --git/--url/--path is required\n"); return 2; }
            if (!rev.empty()   && d.git.empty()) { fprintf(stderr, "kama pkg add: --rev is only valid with --git\n"); return 2; }
            if (!integ.empty() && d.url.empty()) { fprintf(stderr, "kama pkg add: --integrity is only valid with --url\n"); return 2; }
            d.rev = rev; d.integrity = integ;
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
            g_declaredFlags = declared;
            g_strictFlags   = true;
            for (auto& d : defaults) g_activeFlags.insert(d);

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
    // cwd ($KAMA_HOME, else <exe>/../include, else <exe>, else ".").
    std::string runtimeDir = resolveRuntimeDir(argv[0]);

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
