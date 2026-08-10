// kama driver: parse kama source and transpile to portable C, optionally
// invoking a C compiler to produce a native executable.
//
//   kama transpile <in.kama> [-o out.c] [--no-line]
//   kama build     <in.kama> [-o out] [--target HOST|MACOS|WINDOWS|LINUX|WASM|EMBEDDED|<triple>] [--webgpu]
//                              [--cc <compiler>] [--no-line] [--keep-c] [-j <n>]
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
#include <chrono>               // steady_clock — KAMA_TIMING phase timing (LSP M5.0)
#include <ctime>                // time() — the parse cache's "still being written?" guard (LSP M5.2).
                                // Explicit: macOS libc++ pulls it in transitively, the container's
                                // libstdc++ does not — the same divergence that broke the M4 build.

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
  #include <signal.h>           // sigaction — the `-j` pool ignores SIGINT once per wave, not per job
  #include <errno.h>            // EINTR — waitpid restart
  // NB: NOT <spawn.h>, though posix_spawn is the tidier API. Like <windows.h> and <mach-o/dyld.h>
  // above, it is compiled in the same TU as kama.parser.hpp and drags in a header whose TRUE/FALSE
  // collide with the token enum's. runCmdsParallel uses fork/exec, which needs nothing new.
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
#include "kama.json.h"      // Json + serialize, for `--json` output (shared with the LSP's framing)
#include "kama.agents.h"    // KAMA_AGENTS_MD + stubs, embedded — the `kama agents` command

#ifndef KAMA_VERSION
#define KAMA_VERSION "0.0.0-dev"
#endif

namespace {

int runCmd(const std::string& cmd);   // fwd decl (defined below) — used by linkDir on Windows

std::string absolutePath(const std::string& path)
{
    char buf[PATH_MAX];
#ifdef _WIN32
    // ⚠️ `_fullpath` returns BACKSLASHES, and everything else in this driver builds paths by joining with
    // '/' (`dir + "/" + name`, the project enumeration, the module resolver). Mixing the two produces
    // `D:\a\proj/app.kama` from one code path and `D:\a\proj\app.kama` from another for the SAME file — and
    // several comparisons here are exact string equality (CEmitter::unitForUri re-picks a unit by name), so
    // they silently match nothing. That is what made every `kama query --project` answer "no references" on
    // Windows while the same queries passed on Linux and macOS. Normalize to '/' at the one place absolute
    // paths are minted: Win32 and the CRT accept forward slashes everywhere, as do gcc/clang command lines.
    // The fallback below is normalized too: a caller that handed us a backslash spelling of a file that
    // does not exist yet must not be the one path that escapes the convention.
    std::string s = _fullpath(buf, path.c_str(), PATH_MAX) ? std::string(buf) : path;
    for (char& c : s) if (c == '\\') c = '/';
    return s;
#else
    if (realpath(path.c_str(), buf)) return std::string(buf);
    return path; // fall back to as-given (e.g. file doesn't exist yet)
#endif
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

// Split a path into its non-empty components, on either separator so one code path serves Windows.
// The leading "/" of a POSIX absolute path drops out; that is harmless because relativePath only ever
// compares two paths that were canonicalized the same way.
std::vector<std::string> splitPathComponents(const std::string& p)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : p) {
        if (c == '/' || c == '\\') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Join a relative `rel` onto `dir` LEXICALLY — collapsing "." and ".." as text, never touching the
// filesystem. Deliberately NOT absolutePath(): that is realpath(), which resolves symlinks, and a path
// dependency reaches its package *through* the `.kama/deps/<name>` symlink. Canonicalizing there would
// respell a dependency's files as first-party paths and defeat every "is this file mine?" test
// downstream (the LSP's ownership check, rename's refusal to touch a dependency).
std::string joinPathLexical(const std::string& dir, const std::string& rel)
{
    std::string joined = rel.empty() ? dir : dir + "/" + rel;
    bool absolute = !joined.empty() && (joined[0] == '/' || joined[0] == '\\');
    std::vector<std::string> parts;
    for (const auto& c : splitPathComponents(joined)) {
        if (c == ".") continue;
        if (c == ".." && !parts.empty() && parts.back() != "..") { parts.pop_back(); continue; }
        parts.push_back(c);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) out += "/"; out += parts[i]; }
    if (absolute) out = "/" + out;
    return out.empty() ? "." : out;
}

// `target` spelled relative to `fromDir` — both already absolute. Pure string work over canonical
// forms: drop the shared prefix, then one `..` per component of `fromDir` that is left over. Returns
// `target` unchanged when the two share no root at all (different Windows drives). Emits forward
// slashes, which is what a manifest and a lockfile spell on every platform.
std::string relativePath(const std::string& fromDir, const std::string& target)
{
    std::vector<std::string> f = splitPathComponents(fromDir), t = splitPathComponents(target);
    size_t i = 0;
    while (i < f.size() && i < t.size() && f[i] == t[i]) ++i;
    if (i == 0) return target;
    std::string out;
    for (size_t k = i; k < f.size(); ++k) out += "../";
    for (size_t k = i; k < t.size(); ++k) { out += t[k]; if (k + 1 < t.size()) out += "/"; }
    if (out.empty()) return ".";
    if (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
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

// Every *.kama under `root`, recursively, as absolute paths. Prunes build output, the package store, and
// dot-directories; aborts once the cap is blown (the caller reports rather than truncating silently).
// Deliberately NOT collectFilesRel: the prune list, extension filter and early abort are most of the
// body, and its one caller (treeHashOf) wants a complete unfiltered tree. Order is readdir's — a caller
// that needs determinism sorts (module resolution does; the LSP's set queries don't care).
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

static bool loadManifestSources(const std::string& path, std::vector<std::string>& out, std::string& err);

// The source files of the package rooted at `dir`, per its manifest's `sources` — or {} when `dir` is not
// a package root or declares none, in which case the caller falls back to the flat listing.
//
// This is what lets a dependency use the `src/` layout docs/packages.md teaches for applications: an
// installed `.kama/deps/geo/` holds only `kama.json`, its sources one level down, so a non-recursive
// listing of it finds nothing and the package cannot be imported AT ALL. `sources` is precisely the
// declaration of where a package's files are, so module resolution consults it — which is also what
// makes the key earn its keep beyond the LSP.
//
// The `sources` a manifest declares, cached by path — this runs for every import of every build and
// loadManifestSources re-reads the file on every call. Only the DECLARATION is cached, never the expanded
// file list: `kama lsp` is long-lived, and a newly added `.kama` file has to become visible without
// restarting the server. Expanding costs a readdir, which is exactly what the flat listing it replaced
// cost anyway.
const std::vector<std::string>& manifestSourcesCached(const std::string& manifest)
{
    static std::map<std::string, std::vector<std::string>> cache;
    auto it = cache.find(manifest);
    if (it != cache.end()) return it->second;
    std::vector<std::string> srcs;
    std::string err;
    if (!fileExists(manifest) || !loadManifestSources(manifest, srcs, err)) srcs.clear();
    return cache.emplace(manifest, std::move(srcs)).first->second;
}

std::vector<std::string> packageSourceFiles(const std::string& dir)
{
    std::vector<std::string> out;
    for (const auto& rel : manifestSourcesCached(dir + "/kama.json")) {
        // Lexical, not absolutePath: `"sources": ["."]` must not yield `<dir>/./x.kama` (unit names are
        // matched EXACTLY downstream), but neither may the `.kama/deps` symlink be resolved away.
        std::string sub = joinPathLexical(dir, rel);
        if (dirExists(sub))       { size_t seen = 0; collectKamaFiles(sub, "", out, seen, (size_t)-1); }
        else if (fileExists(sub)) out.push_back(sub);
        // A listed path that does not exist is skipped, as in collectPackageTree.
    }
    std::sort(out.begin(), out.end());   // readdir order is not deterministic; emit order must be
    return out;
}

static bool loadManifestProjects(const std::string& path, std::vector<std::string>& out, std::string& err);

// Expand one `projects` entry against `dir`: a plain sub-project directory, or a trailing "/*" meaning
// every immediate subdirectory that has a manifest. Sorted — readdir order is not deterministic.
std::vector<std::string> expandProjectsEntry(const std::string& dir, const std::string& rel)
{
    std::vector<std::string> out;
    if (rel.size() > 2 && rel.compare(rel.size() - 2, 2, "/*") == 0) {
        std::string parent = dir + "/" + rel.substr(0, rel.size() - 2);
        if (DIR* d = opendir(parent.c_str())) {
            while (struct dirent* e = readdir(d)) {
                std::string n = e->d_name;
                if (n.empty() || n[0] == '.') continue;
                if (dirExists(parent + "/" + n) && fileExists(parent + "/" + n + "/kama.json"))
                    out.push_back(parent + "/" + n);
            }
            closedir(d);
        }
        std::sort(out.begin(), out.end());
    } else if (dirExists(dir + "/" + rel)) {
        out.push_back(dir + "/" + rel);
    }
    return out;
}

// Every package directory in the `projects` tree rooted at `dir`, canonical, INCLUDING `dir` itself.
// The output set doubles as the cycle break (a manifest may legally name a directory that names it
// back, and a symlink makes a loop trivial) — the same guard collectPackageTree keeps for files.
void collectProjectDirs(const std::string& dir, std::set<std::string>& out)
{
    if (!out.insert(absolutePath(dir)).second) return;
    const std::string manifest = dir + "/kama.json";
    if (!fileExists(manifest)) return;
    std::vector<std::string> subs;
    std::string err;
    loadManifestProjects(manifest, subs, err);
    for (const auto& rel : subs)
        for (const auto& sub : expandProjectsEntry(dir, rel)) collectProjectDirs(sub, out);
}

// The workspace that OWNS `projectDir`: the package directories of the outermost ancestor manifest whose
// `projects` tree, expanded recursively, actually contains `projectDir`. Falls back to `{projectDir}`
// when no ancestor claims it — no workspace, so a path dep stays top-level only.
//
// Mirrors lspFindProject's rule (declared beats inferred, outermost wins, and it must CONTAIN me) so the
// build and the editor agree on what one workspace is. A `.kama` component stops the walk, so a vendored
// dependency can never reach out into its host project and call itself a member.
std::set<std::string> workspaceMembers(const std::string& projectDir)
{
    const std::string self = absolutePath(projectDir);
    std::vector<std::string> ancestors;
    for (std::string cur = self;;) {
        if (baseName(cur) == ".kama") break;
        if (fileExists(cur + "/kama.json")) ancestors.push_back(cur);
        std::string parent = dirName(cur);
        if (parent == cur || parent == ".") break;      // filesystem root
        cur = parent;
    }
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        std::set<std::string> members;
        collectProjectDirs(*it, members);
        if (members.size() > 1 && members.count(self)) return members;
    }
    return { self };
}

// Resolve module segments (["std","memory"]) to source file(s) under the first matching
// root: a file-module (<root>/std/memory.kama) or every *.kama in a directory-module
// (<root>/std/memory/). A directory that is a PACKAGE root contributes the files its manifest
// declares, wherever they live under it, rather than the flat listing. Empty result => unresolved.
std::vector<std::string> resolveModuleFiles(const std::vector<std::string>& segs,
                                            const std::vector<std::string>& roots,
                                            std::string* matchedRoot = nullptr)
{
    std::string rel;
    for (size_t i = 0; i < segs.size(); ++i) rel += (i ? "/" : "") + segs[i];
    for (auto& root : roots) {
        if (matchedRoot) *matchedRoot = root;
        std::string file = root + "/" + rel + ".kama";
        if (fileExists(file)) return { file };
        std::string dir = root + "/" + rel;
        if (dirExists(dir)) {
            auto fs = packageSourceFiles(dir);        // declared: wherever the package says its files are
            if (fs.empty()) fs = listKamaFiles(dir);  // undeclared: the plain directory-module listing
            if (!fs.empty()) return fs;
        }
    }
    if (matchedRoot) matchedRoot->clear();
    return {};
}

SharedCompilationUnit parseFile(const std::string& inputFile);   // defined below
SharedCompilationUnit parseString(const char* src, const std::string& name);   // defined below

// ---- phase timing (LSP M5.0) -----------------------------------------------------------------------
// `KAMA_TIMING=1` prints one `kama-timing:` line per analysis to STDERR — never stdout, which `kama lsp`
// owns for JSON-RPC framing. `KAMA_TIMING=2` adds a line per parsed/cached unit. Off by default and free
// when off (one getenv, then a bool test).
//
// This exists because the M5 cold-start brief sized the whole incremental-perf plan on an UNMEASURED
// split: it assumed a parse cache would take a 240 ms analysis to ~20 ms, but the per-unit cost is parse
// AND analyze, and only the parse share is cacheable. Timing before caching is what makes the staging
// decisions data rather than guesses — and it makes the brief's table reproducible from a checkout,
// which it was not.
bool timingOn(int level = 1)
{
    static int lvl = [] { const char* e = getenv("KAMA_TIMING"); return (e && *e) ? atoi(e) : 0; }();
    return lvl >= level;
}

struct Timing {
    double bufferParse = 0;    // parseForQuery — the live editor buffer
    double closureParse = 0;   // parseFile — the transitive import closure, off disk
    double preludeParse = 0;   // parseString — the embedded prelude + built-in modules
    double analyze = 0;        // CEmitter::analyze over every unit
    double query = 0;          // answering the request off the built index
    long   parsedUnits = 0;
    long   cachedUnits = 0;
};
Timing& timing() { static Timing t; return t; }

// RAII accumulator. Records nothing and costs a bool test when timing is off.
struct Stopwatch {
    double* slot;
    std::chrono::steady_clock::time_point t0;
    explicit Stopwatch(double* s) : slot(timingOn() ? s : nullptr)
    {
        if (slot) t0 = std::chrono::steady_clock::now();
    }
    ~Stopwatch()
    {
        if (slot)
            *slot += std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count();
    }
};

void timingDump(const char* what, const std::string& subject);

// RAII dump, for a scope with many early returns (the `kama query` dispatch). Declare it BEFORE the
// Stopwatch it should report, so reverse destruction order lets the watch stop first.
struct TimingScope {
    const char* what;
    std::string subject;
    TimingScope(const char* w, std::string s) : what(w), subject(std::move(s)) {}
    ~TimingScope() { timingDump(what, subject); }
};

// Print the accumulated phases and reset. `what` names the entry point so the TWO analyses one editor
// request can trigger (the didChange analysis, then M4.6's repair) are distinguishable in the log.
void timingDump(const char* what, const std::string& subject)
{
    if (!timingOn()) return;
    Timing& t = timing();
    double total = t.bufferParse + t.closureParse + t.preludeParse + t.analyze + t.query;
    fprintf(stderr,
            "kama-timing: %s %s buffer-parse=%.2f closure-parse=%.2f closure-units=%ld/%ld "
            "prelude-parse=%.2f analyze=%.2f query=%.2f total=%.2f\n",
            what, subject.c_str(), t.bufferParse, t.closureParse, t.parsedUnits, t.cachedUnits,
            t.preludeParse, t.analyze, t.query, total);
    t = Timing();
}

// The directory of the manifest DRIVING this build: next to the first input, else the CWD. "" if none.
// Note this is whoever is compiling, which for a monorepo is not necessarily the package a given source
// file belongs to — that distinction is the whole of the per-package import check below.
std::string projectManifestDir(const std::vector<std::string>& inputs)
{
    if (!inputs.empty()) { std::string d = dirName(inputs[0]); if (fileExists(d + "/kama.json")) return d; }
    if (fileExists("kama.json")) return ".";
    return "";
}

// The project's resolved-dependency view (`<projectDir>/.kama/deps`), or "" if there is no project
// manifest or the view has not been materialized. `kama install` populates it (package-name -> store
// or path source); the build only READS it — a pure, reproducible resolution with no fetch at build
// time. Discovered like the manifest: next to the first input, else the CWD.
std::string projectDepsView(const std::vector<std::string>& inputs, const char* leaf = ".kama/deps")
{
    std::string dir = projectManifestDir(inputs);
    if (dir.empty()) return "";
    std::string view = dir + "/" + leaf;
    return dirExists(view) ? view : "";
}

// Defined once DepSpec exists, beside the manifest loaders it wraps.
const std::set<std::string>& declaredImportNames(const std::string& packageDir);
std::string storeDir();   // the content-addressed package store (~/.kama/store)

// The directory of the nearest `kama.json` at or above `fromDir` — the package that OWNS a file. A
// `.kama` component stops the walk, so a vendored dependency is never owned by its host project. "" if
// no manifest is above it at all (a scratch file, which nothing can be said about).
std::string owningPackageDir(const std::string& fromDir)
{
    static std::map<std::string, std::string> cache;
    auto it = cache.find(fromDir);
    if (it != cache.end()) return it->second;
    std::string found;
    for (std::string cur = absolutePath(fromDir);;) {
        if (baseName(cur) == ".kama") break;
        if (fileExists(cur + "/kama.json")) { found = cur; break; }
        std::string parent = dirName(cur);
        if (parent == cur || parent == ".") break;
        cur = parent;
    }
    return cache.emplace(fromDir, found).first->second;
}



// `strictImports` decides what an import satisfied by the dependency view but undeclared by the importing
// file's OWN package does: fail the build (every command that produces something) or merely report it.
// The LSP and the `query` CLI that mirrors it pass false — refusing to analyze would strip an editor of
// cross-module hover, definitions and diagnostics over a *manifest* problem, when the code is fine and
// resolves.
//
// `diagsOut` (M6 A3) collects that same finding as a structured Diagnostic on the `import` statement, so in
// an editor it reaches the Problems pane instead of a log channel nobody reads. Non-null only from the LSP
// path; the CLI's five call sites are unchanged. NOTE the two have deliberately DIFFERENT lifetimes — the
// stderr message is warn-once per process (a server would otherwise repeat it forever), while a diagnostic
// must be re-pushed on every analysis or it vanishes on the next keystroke.
bool loadProgramUnits(const std::vector<std::string>& cliInputs, const char* argv0,
                      std::vector<SharedCompilationUnit>& units,
                      std::vector<std::string>& paths,
                      bool includeDevDeps = false,
                      bool strictImports = true,
                      std::vector<Diagnostic>* diagsOut = nullptr)
{
    std::string stdlibDir = resolveStdlibDir(argv0);
    std::vector<std::string> extraRoots = splitSearchPath(getenv("KAMA_PATH"));
    std::string depsView = projectDepsView(cliInputs);   // resolved package roots (`kama pkg install`), or ""
    // dev-dependencies are a SEPARATE view, only on the import path under `--dev` — so production code can
    // never import a dev-dep (the phantom-dep guarantee makes this a hard resolve error), at any opt level.
    std::string devDepsView = includeDevDeps ? projectDepsView(cliInputs, ".kama/dev-deps") : "";
    std::string buildManifestDir = projectManifestDir(cliInputs);
    // "/" would prefix-match every path and silently disable the check, so an unresolvable store root
    // (KAMA_STORE set to empty) suppresses nothing rather than everything.
    std::string storeRoot = absolutePath(storeDir()) + "/";
    if (storeRoot.size() <= 1) storeRoot = "\n";        // matches no path
    // "<manifest>\n<module>", reported once per package+module. PROCESS-wide, not per call: `kama lsp`
    // re-analyzes on every keystroke, and a per-call set would repeat the same message into the editor's
    // output channel forever. A build calls this once, so nothing changes there.
    static std::set<std::string> warnedFreeRide;
    bool undeclaredImport = false;   // strict mode: report them ALL, then fail, rather than stop at one
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
            std::string via;
            auto files = resolveModuleFiles(segs, roots, &via);
            if (files.empty()) {
                std::string name;
                for (size_t k = 0; k < segs.size(); ++k) name += (k ? "::" : "") + segs[k];
                fprintf(stderr, "kama: error: cannot resolve module '%s' (from %s)\n",
                        name.c_str(), reserved ? stdlibDir.c_str() : here.c_str());
                return false;
            }
            // A sub-project must be EXTRACTABLE — liftable out of its monorepo and still buildable — and
            // that requires it to declare every dependency it imports. The undeclared-import check above
            // is satisfied by whoever is COMPILING, so a sibling can free-ride on what only the top-level
            // app declared: it builds where it sits, fails standalone, and nothing says so.
            //
            // So: an import satisfied by the dependency view must be declared by the importing file's OWN
            // package. Anything reached through the file's own directory, $KAMA_PATH or the stdlib is
            // intra-package and needs no declaration, so a single-package project never meets this rule.
            //
            // A hard error under `strictImports`, which is every command that produces something. It
            // shipped as a warning and was promoted once the remedy was shown to be ALWAYS appliable
            // wherever it fires: the module has to be in the dependency view for this to trigger at all,
            // which means someone declared it, so the sibling's own declaration either dedups against
            // that one or is a legal workspace-internal path dep. A guarantee nobody is forced to honor
            // is not a guarantee — and build-output warnings are scrolled past.
            //
            // Only for a package the user can actually FIX. A fetched package's sources live in the
            // content-addressed store, where its manifest is not the user's to edit — and editing it
            // would break the tree hash that names its store entry. Its author's missing declaration is
            // its author's bug; here it would be a diagnostic instructing a destructive action.
            if (!reserved && !via.empty() && (via == depsView || via == devDepsView)) {
                std::string owner = owningPackageDir(here);
                if (!owner.empty() && owner.compare(0, storeRoot.size(), storeRoot) == 0) owner.clear();
                if (!owner.empty() && !declaredImportNames(owner).count(segs[0])) {
                    std::string ownerManifest = owner + "/kama.json";
                    undeclaredImport = true;
                    // M6 A3: pushed EVERY call, outside the warn-once gate below — a squiggle has to be
                    // republished on each analysis or it disappears on the next keystroke. Positioned on the
                    // `import` statement (its module-path segments carry no spans of their own), in the file
                    // that does the importing, so the caller can filter it to the open buffer the way it
                    // filters every other diagnostic. The message still names the kama.json to fix.
                    if (diagsOut) {
                        Diagnostic d;
                        d.line      = imp->line;
                        d.column    = imp->column;
                        d.endLine   = imp->endLine;
                        d.endColumn = imp->endColumn;
                        d.severity  = DiagSeverity::Warning;
                        d.code      = "undeclared-import";
                        d.file      = paths[i];
                        d.message   = "module '" + segs[0] + "' is not declared by this package (" +
                                      ownerManifest + "), so it will not build on its own — add it under "
                                      "\"dependencies\"";
                        diagsOut->push_back(d);
                    }
                    if (warnedFreeRide.insert(ownerManifest + "\n" + segs[0]).second) {
                        // Suggest the concrete line. The dependency's own package root is the nearest
                        // manifest above its sources, which for a workspace sibling is a path away.
                        std::string depPkg = owningPackageDir(dirName(absolutePath(files[0])));
                        fprintf(stderr, "kama: %s: %s imports module '%s', but its own package (%s) "
                                "does not declare it — only %s does, so this package will not build on "
                                "its own.\n",
                                strictImports ? "error" : "warning",
                                paths[i].c_str(), segs[0].c_str(), ownerManifest.c_str(),
                                (buildManifestDir.empty() ? "the build" : (buildManifestDir + "/kama.json")).c_str());
                        if (!depPkg.empty())
                            fprintf(stderr, "kama: note: add to %s: \"dependencies\": { \"%s\": "
                                    "{ \"path\": \"%s\" } }\n",
                                    ownerManifest.c_str(), segs[0].c_str(),
                                    relativePath(owner, depPkg).c_str());
                    }
                }
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
    return !(strictImports && undeclaredImport);   // reported every one above, then fail once
}

// ---- parse cache (M5.2) -----------------------------------------------------------------------
// loadProgramUnits re-reads and re-parses the ENTIRE transitive import closure on every call. A build
// calls it once; `kama lsp` calls it per keystroke, and 16 of the 17 units in a std-importing file's
// closure cannot possibly have changed while you type. Measured at ~118 ms of a 203 ms analysis.
//
// OPT-IN, enabled only by `kama lsp`, for two reasons that are not caution:
//   * build/check/transpile parse each file exactly once, so a cache is pure overhead there;
//   * a cached unit is DESTRUCTIVELY REWRITTEN by CEmitter::pruneInactiveDecls (kama.cemit.cpp:17129)
//     — `@compileFor`-inactive decls are dropped from codeDeclarationList in place. Reuse is therefore
//     sound only while the build-flag set is fixed, which one `kama lsp` process guarantees and a
//     future multi-configuration caller would not. Opt-in makes that a decision, not an accident.
//
// The key is the path SPELLING, not the absolute path. A unit is NAMED by the string parseFile was
// handed, and CEmitter::unitForUri matches unit names EXACTLY — while one file is reachable here by
// two spellings (the URI-derived absolute path the LSP passes as an input, vs `dir + "/" + name` built
// by module resolution). Serving one spelling's unit for the other would silently rewrite every query's
// URI, every diagnostic's `file`, and every go-to-definition Location. Two spellings cost two entries,
// which is exactly what happens today.
struct CachedUnit { SharedCompilationUnit unit; std::string abs; time_t mtime; off_t size; };
const size_t kParseCacheMax = 2000;   // a runaway backstop, far above any real workspace — see below
bool g_parseCache = false;
std::map<std::string, CachedUnit> g_parseCacheMap;

// Parse one kama file into a CompilationUnit. Returns nullptr on failure.
SharedCompilationUnit parseFile(const std::string& inputFile)
{
    // parseFile is reached only for on-disk files — the import closure and lspImportSymbols; the live
    // editor buffer goes through parseForQuery. So this slot is exactly "import-closure parse".
    Stopwatch sw(&timing().closureParse);

    // Validity is (st_mtime, st_size). st_mtime is second-granular on some filesystems, so a file
    // rewritten twice within one second at the same size would go unseen; that is closed from both
    // ends — a file whose mtime is within a second of now is never cached (it may still be being
    // written), and the server evicts on workspace/didChangeWatchedFiles. Deliberately NOT
    // st_mtimespec/st_mtim: the former is macOS-only, the latter needs _POSIX_C_SOURCE >= 200809 and
    // is absent from mingw-w64, and the two invalidations above make sub-second resolution moot.
    struct stat st;
    bool cacheable = false;
    if (g_parseCache && stat(inputFile.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        cacheable = (st.st_mtime + 1 < time(nullptr));
        if (cacheable) {
            auto it = g_parseCacheMap.find(inputFile);
            if (it != g_parseCacheMap.end() && it->second.mtime == st.st_mtime
                                            && it->second.size  == st.st_size) {
                ++timing().cachedUnits;
                return it->second.unit;
            }
        }
    }
    ++timing().parsedUnits;

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
        return nullptr;   // a FAILED parse is never cached: the file is about to be fixed, and a null
                          // entry would need the same mtime check that would have re-parsed it anyway
    if (cacheable) {
        // Wholesale clear rather than an LRU: this can only trip on a tree far larger than the
        // workspace indexer will index at all, and recovery costs one cold analysis.
        if (g_parseCacheMap.size() >= kParseCacheMax) g_parseCacheMap.clear();
        g_parseCacheMap[inputFile] = CachedUnit{ extra.compilationUnit, absolutePath(inputFile),
                                                 st.st_mtime, st.st_size };
    }
    return extra.compilationUnit;
}

// The implicit prelude (Optional/Result, the language-level contracts, the primitive conformances,
// Chars/Split) is embedded from prelude/global.kama into KAMA_PRELUDE_SRC (see kama.prelude.h and
// tools/embed_prelude.sh), so it ships inside bin/kama even for a --no-std install. The namespaced
// built-in triad (prelude/std/memory/*.kama) is embedded as KAMA_PRELUDE_MODULES.

// Parse an in-memory kama source string into a CompilationUnit (flex string buffer). nullptr on error.
SharedCompilationUnit parseString(const char* src, const std::string& name)
{
    // Its only callers are preludeUnit / preludeModuleUnits below, so this slot is exactly
    // "prelude parse" with no extra plumbing.
    Stopwatch sw(&timing().preludeParse);

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
// diagnostics survive a failed parse (as-you-type buffers are usually mid-edit). Internal to this TU;
// the LSP reaches it through the external `lspAnalyzeBuffer` seam (defined at end of file).
//
// `unit` is non-null whenever the start rule reduced — which since M5.3 includes a PARTIAL unit whose
// broken declarations the recovery arms discarded. `partial` says which of those two it is, and
// `droppedTopLevelDecl` says how badly: only the top-level arm loses a whole type/fn, and only that
// makes semantic diagnostics worth suppressing (see lspAnalyze).
struct ParseResult {
    SharedCompilationUnit unit;
    SharedCodeGenContext  ctx;
    bool partial = false;
    int  droppedTopLevelDecl = 0;
};
ParseResult parseForQuery(const char* src, const std::string& name)
{
    Stopwatch sw(&timing().bufferParse);

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
    r.ctx = extra.codeGenContext;
    // Self-gating (M5.4): `compilationUnit` is assigned only by the `compilation_unit` action, which
    // only runs if the start rule reduced — so this is null when the parse truly aborted and non-null
    // (possibly partial) otherwise. No rc/errorCount bookkeeping needed. parseFile and parseString keep
    // their strict gates, so kama build/check/transpile see no partial units at all.
    r.unit    = extra.compilationUnit;
    r.partial = (rc != 0 || extra.codeGenContext->errorCount() > 0);
    r.droppedTopLevelDecl = extra.codeGenContext->droppedTopLevelDecl;
    return r;
}

// Parsed ONCE per process (M5.1). The prelude source is a compile-time constant, so its AST is one
// too: a build calls this once and cannot tell the difference, while `kama lsp` called it on every
// keystroke — 772 lines of embedded kama re-parsed per analysis, measured at ~28 ms and the entire
// fixed cost of analyzing a 3-line file.
//
// Sharing one prelude AST between two live CEmitters is safe, and it is not obvious. Everything the
// emitter records ABOUT a prelude unit is a per-emitter member keyed by node pointer (_unitCtx,
// _preludeEnums, ClassInfo, the `unit == _preludeUnit` identity tests), so two emitters never see each
// other's state. The one pass that writes THROUGH to the AST is CEmitter::pruneInactiveDecls, which
// rewrites codeDeclarationList and each kept decl's attribute list in place and runs over the prelude
// too (kama.cemit.cpp:17129 / :17182) — safe here twice over: the prelude carries no `@compileFor`, so
// the prune drops nothing, and the pass is idempotent under a fixed build-flag set, which one process
// always has. Re-check both claims if the emitter grows another in-place AST rewrite.
SharedCompilationUnit preludeUnit()
{
    static SharedCompilationUnit u = parseString(KAMA_PRELUDE_SRC, "<prelude>");
    return u;
}

// The namespaced built-in modules (the smart-pointer triad, std::memory) — embedded like the prelude
// so they're always in scope with no `import std::memory`, in both the full and `--no-std` installs.
// Each keeps its own `namespace`/`export`; the emitter collects them under that scope + an implicit
// `using` (see CEmitter::collectProgram / ctxOf). nullptr units (a parse failure) are dropped.
// Parsed once per process for the same reasons as preludeUnit above — read its comment before
// touching either.
const std::vector<SharedCompilationUnit>& preludeModuleUnits()
{
    static const std::vector<SharedCompilationUnit> units = [] {
        std::vector<SharedCompilationUnit> v;
        for (int i = 0; i < KAMA_PRELUDE_MODULE_COUNT; ++i) {
            SharedCompilationUnit u = parseString(KAMA_PRELUDE_MODULES[i].src, "<prelude-module>");
            if (u) v.push_back(u);
        }
        return v;
    }();
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

// ---- build configuration: target triples ------------------------------------------------------
// A target is an `<arch>-<os>-<abi>` triple — Zig's 3-part form, because GNU's `vendor` field is
// vestigial (`unknown`/`pc`); a 4-part spelling is accepted and its vendor dropped, so pasting a Rust
// or clang triple works. Selecting a target inserts its NAME plus its derived component flags into the
// active `@compileFor` set, which is why `@compileFor(OS_LINUX)` needs no language machinery:
// membership is all `compileForActive` ever tested. Docs: docs/targets.md, docs/SPEC.md.
struct TargetSpec {
    std::string name;                            // catalog/user name ("WINDOWS", "RPI"); the triple if anonymous
    std::string arch, os, abi;
    std::string cc, ar, sysroot;                 // toolchain override (empty -> the default resolution)
    std::vector<std::string> cflags, ldflags;
    std::string triple() const { return arch + "-" + os + "-" + abi; }
    bool hosted()      const { return os != "none"; }         // has an OS and a libc
    bool isWasm()      const { return arch == "wasm32" || arch == "wasm64"; }
    // Compile/link decisions ask the TARGET these, never the host. Before this campaign they were
    // `#ifdef __APPLE__` / `_WIN32` evaluated when the COMPILER was built — correct only while host and
    // target were always the same machine, and the one hard blocker to cross-compiling.
    bool isMacOS()     const { return os == "macos"; }
    bool isWindows()   const { return os == "windows"; }
    const char* sharedLibExt() const {
        return isWindows() ? ".dll" : isMacOS() ? ".dylib" : ".so";
    }
};

// The one legitimate host `#ifdef` in the build path: deciding what `HOST` *means*. Everything else
// keys off the SELECTED target's flags (see targetLinkFlags), which is what makes cross-compiling
// possible at all.
static const TargetSpec& hostTarget()
{
    static const TargetSpec h = [] {
        TargetSpec t;
        t.name = "HOST";
#if   defined(__aarch64__) || defined(_M_ARM64)
        t.arch = "aarch64";
#elif defined(__x86_64__)  || defined(_M_X64)
        t.arch = "x86_64";
#elif defined(__riscv) && (__riscv_xlen == 64)
        t.arch = "riscv64";
#elif defined(__arm__)     || defined(_M_ARM)
        t.arch = "arm";
#elif defined(__i386__)    || defined(_M_IX86)
        t.arch = "i686";
#else
        t.arch = "unknown";
#endif
#if   defined(_WIN32)
        t.os = "windows";
#elif defined(__APPLE__)
        t.os = "macos";
#elif defined(__linux__)
        t.os = "linux";
#elif defined(__FreeBSD__)
        t.os = "freebsd";
#else
        t.os = "unknown";
#endif
#if   defined(_MSC_VER)
        t.abi = "msvc";
#elif defined(__APPLE__)
        t.abi = "none";                          // Darwin has no separate ABI field in practice
#else
        t.abi = "gnu";                           // glibc/mingw; a musl host spells its triple explicitly
#endif
        return t;
    }();
    return h;
}

// Split `<arch>-<os>[-<abi>]` or `<arch>-<vendor>-<os>-<abi>`. A value containing '-' is a triple; a
// bare token is a NAME to look up in the catalog. That one rule is the whole disambiguation.
static bool parseTriple(const std::string& s, TargetSpec& out, std::string& err)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dash = s.find('-', start);
        parts.push_back(s.substr(start, dash == std::string::npos ? std::string::npos : dash - start));
        if (dash == std::string::npos) break;
        start = dash + 1;
    }
    for (const auto& p : parts)
        if (p.empty()) { err = "empty component in target triple '" + s + "'"; return false; }
    if (parts.size() == 2)      { out.arch = parts[0]; out.os = parts[1]; out.abi = "none"; }
    else if (parts.size() == 3) { out.arch = parts[0]; out.os = parts[1]; out.abi = parts[2]; }
    else if (parts.size() == 4) { out.arch = parts[0]; out.os = parts[2]; out.abi = parts[3]; }  // drop vendor
    else { err = "target triple '" + s + "' must be <arch>-<os>[-<abi>] (2 to 4 components)"; return false; }
    out.name = s;
    return true;
}

// Derived `@compileFor` flags for a target: one flag per triple component, the single synthesized
// convenience `HOSTED` ("do I have an OS and a libc" is the most common gate in a systems language, and
// `!OS_NONE` reads badly), and — for a target the PROJECT declared — its name.
//
// A BUILT-IN catalog name deliberately does not become a flag. `MACOS`/`EMBEDDED` are shortcuts for a
// triple family, so gating on one would gate on how the build was spelled rather than on a fact about
// the target: `@compileFor(EMBEDDED)` would silently stop applying the moment a real board triple
// (`xtensa-none-elf`) was used instead of the shortcut. `@compileFor(OS_NONE)` is the fact, and it holds
// for both. A user-declared name IS a fact about a target the project defined, so it does become a flag.
static std::set<std::string> derivedTargetFlags(const TargetSpec& t, bool nameIsBuiltin)
{
    auto up = [](std::string v) {
        for (char& c : v) c = (char)toupper((unsigned char)c);
        return v;
    };
    std::set<std::string> f;
    if (!nameIsBuiltin && !t.name.empty() && t.name.find('-') == std::string::npos) f.insert(t.name);
    f.insert("ARCH_" + up(t.arch));
    f.insert("OS_"   + up(t.os));
    f.insert("ABI_"  + up(t.abi));
    if (t.hosted()) f.insert("HOSTED");
    return f;
}

static const std::map<std::string, TargetSpec>& builtinTargets();   // defined just below

// A single-select group: pick exactly one value, and its name (plus anything it inherits) joins the
// active flag set. `TARGET` and `BUILD_TYPE` are the built-in instances; a project declares its own the
// same way. There is no user-declared MULTI group on purpose — `flags` already IS the one multi-select
// bag, and a second grouping mechanism would only buy presentation.
struct SelectGroup {
    std::vector<std::string> values;                  // declaration order (error messages, IDE dropdowns)
    std::map<std::string, std::string> inherits;      // value -> base value, e.g. FAST -> RELEASE
    std::string dflt;                                 // selected when the build does not say
    bool has(const std::string& v) const {
        for (const auto& x : values) if (x == v) return true;
        return false;
    }
};

// The resolved build target, and the manifest-declared target catalog it was resolved against. File-scope
// like the other build config: set in main, read by the compile/link path (targetLinkFlags) and by the
// emitter setup. Defaults to the host so a bare `kama build` needs no configuration at all.
static TargetSpec g_target;
static std::map<std::string, TargetSpec> g_manifestTargets;

// Every single-select group except TARGET (whose values are triples and carry a toolchain, so it has its
// own catalog). Seeded with the built-in BUILD_TYPE, then extended by the manifest.
static std::map<std::string, SelectGroup> g_selectGroups;

// The built-in BUILD_TYPE group. It must stay built-in — unlike a user group, its values drive real
// behavior on both sides: the compiler strips `debugAssert` and bakes a log level, and the driver picks
// -O3/-Oz + -DNDEBUG + section GC + strip. A user value inherits all of that from whichever it names.
static void seedBuiltinSelectGroups()
{
    SelectGroup bt;
    bt.values = { "DEBUG", "RELEASE" };
    bt.dflt   = "DEBUG";
    g_selectGroups["BUILD_TYPE"] = bt;

    // What artifact to produce. Before this campaign the kind was smeared across a `--shared` boolean and
    // a bare-metal target implying object-only; every other toolchain makes it its own axis (MSBuild
    // OutputType, CMake add_library(STATIC|SHARED|MODULE), Cargo crate-type). No default here: it depends
    // on the resolved target (bare metal defaults to OBJECT), so main picks it once the target is known.
    SelectGroup out;
    out.values = { "EXE", "SHARED", "STATIC", "OBJECT" };
    g_selectGroups["OUTPUT"] = out;
}

// Walk a value's `inherits` chain, innermost first, into `out`. Cycles are broken by the visited set —
// a manifest can legally (if uselessly) say A inherits B inherits A, and that must not hang the build.
static void collectInherited(const SelectGroup& g, const std::string& value, std::set<std::string>& out)
{
    std::string v = value;
    while (!v.empty() && out.insert(v).second) {
        auto it = g.inherits.find(v);
        if (it == g.inherits.end()) break;
        v = it->second;
    }
}

// Resolve a `--target` value. Order matters: a manifest entry MERGES onto a built-in of the same name
// (so a project can attach a cross toolchain to `LINUX` without redefining it), a bare name otherwise
// comes from the catalog, and anything containing '-' is parsed as an anonymous triple — the
// `zig cc -target` gesture, usable with no config file at all.
static bool resolveTarget(const std::string& selRaw,
                          const std::map<std::string, TargetSpec>& manifestTargets,
                          TargetSpec& out, std::string& err)
{
    // Names are case-insensitive (`--target wasm` and `--target WASM` are the same thing); triples are
    // lowercase by convention and pass through untouched, since they contain '-' and skip this path.
    std::string sel = selRaw;
    if (sel.find('-') == std::string::npos)
        for (char& c : sel) c = (char)toupper((unsigned char)c);

    bool known = false;
    auto b = builtinTargets().find(sel);
    if (b != builtinTargets().end()) { out = b->second; known = true; }

    auto m = manifestTargets.find(sel);
    if (m != manifestTargets.end()) {
        const TargetSpec& u = m->second;
        if (!known) out = TargetSpec();
        out.name = sel;
        if (!u.arch.empty()) { out.arch = u.arch; out.os = u.os; out.abi = u.abi; }   // a declared triple wins whole
        if (!u.cc.empty())      out.cc      = u.cc;
        if (!u.ar.empty())      out.ar      = u.ar;
        if (!u.sysroot.empty()) out.sysroot = u.sysroot;
        out.cflags.insert(out.cflags.end(),  u.cflags.begin(),  u.cflags.end());
        out.ldflags.insert(out.ldflags.end(), u.ldflags.begin(), u.ldflags.end());
        if (out.arch.empty() || out.os.empty()) {
            err = "target '" + sel + "' declares no `triple` and is not a built-in";
            return false;
        }
        return true;
    }
    if (known) return true;

    if (sel.find('-') != std::string::npos) return parseTriple(sel, out, err);   // anonymous triple

    std::string names;
    for (const auto& kv : builtinTargets()) names += (names.empty() ? "" : "|") + kv.first;
    for (const auto& kv : manifestTargets)  names += "|" + kv.first;
    err = "unknown target '" + sel + "' (expected " + names + ", or an <arch>-<os>-<abi> triple)";
    return false;
}

// The built-in catalog: sane defaults for the standard targets. A named shortcut does NOT pin an arch —
// it resolves arch to the host's — so `--target LINUX` on an arm Mac means aarch64. A build that needs
// precision spells the triple. Names are conveniences; triples are exact.
static const std::map<std::string, TargetSpec>& builtinTargets()
{
    static const std::map<std::string, TargetSpec> cat = [] {
        const TargetSpec& h = hostTarget();
        auto mk = [&](const char* name, const char* arch, const char* os, const char* abi) {
            TargetSpec t;
            t.name = name;
            t.arch = (arch && *arch) ? arch : h.arch;      // empty -> follow the host
            t.os = os; t.abi = abi;
            return t;
        };
        std::map<std::string, TargetSpec> m;
        m["HOST"]         = h;
        m["MACOS"]        = mk("MACOS",        "", "macos",      "none");
        m["WINDOWS"]      = mk("WINDOWS",      "", "windows",    "gnu");
        m["LINUX"]        = mk("LINUX",        "", "linux",      "gnu");
        m["WASM"]         = mk("WASM",         "wasm32", "emscripten", "none");
        // Bare metal on the host's own arch. `os=none` is the whole of it: freestanding, no libc, object
        // output. A real board spells its own triple (`xtensa-none-elf`) and gets the same `OS_NONE`.
        m["EMBEDDED"]     = mk("EMBEDDED",     "", "none",       "none");
        return m;
    }();
    return cat;
}

// The project's baked default `KAMA_LOG` spec (from the manifest `log` section), threaded to the emitter and
// compiled into `main` so a shipped binary carries its default log filter (M5). Empty = no baked default.
static std::string g_logDefault;

// Every CEmitter in this process is configured HERE, in one place. The build configuration lives in the
// file-scope globals above, and there are six construction sites — two of which (the LSP's `lspAnalyze`
// and `lspAnalyzeWorkspace`) called only `setPrelude` + `addPreludeModule`. With `_activeFlags` empty,
// `compileForActive` answers false for every `@compileFor(X)`, so `pruneInactiveDecls` DROPPED every
// gated declaration a real build keeps and KEPT every negated one — the editor described a program
// nobody was building (LSP M6 A1). Collapsing the block makes a missing setter impossible rather than
// merely unlikely: the next construction site cannot forget what it never spells out.
static void configureEmitter(CEmitter& e)
{
    e.setPrelude(preludeUnit());       // Optional/Result available implicitly
    e.setNoHeap(g_noHeap);             // `--no-heap`: reject heap allocation program-wide
    e.setRelease(g_release);           // `--release`: strip `debugAssert`
    e.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);   // `@compileFor` conditional compilation
    e.setLogDefault(g_logDefault);     // baked `KAMA_LOG` project default (M5), compiled into main
    // Which PACKAGE owns a given source file. The emitter needs this only to name both sides when two
    // packages claim the same conformance, so it is a callback rather than a precomputed per-unit table:
    // the walk is filesystem work the emitter has no business doing, and it runs at most once per error.
    // A synthetic unit (`<prelude>`, `<prelude-module>`) has no path — `dirName` would hand back "." and
    // the walk would climb into whatever project happens to be the working directory, attributing the
    // prelude's conformances to the user.
    e.setPackageResolver([](const std::string& unitPath) -> std::string {
        if (unitPath.empty() || unitPath[0] == '<') return std::string();
        std::string dir = owningPackageDir(dirName(unitPath));
        return dir.empty() ? std::string() : dir + "/kama.json";
    });
    for (auto& m : preludeModuleUnits()) e.addPreludeModule(m);       // the always-in-scope triad
}

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
    // Resolver-internal, never read from or written to a manifest: `path` canonicalized against the dir
    // of the manifest that DECLARED it. A path spec is meaningless without its declaring manifest —
    // `../config` from a sibling and `../../libs/config` from the app name the same directory — so
    // every comparison and every materialization goes through this, never through the spelling.
    std::string pathAbs;
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
    // Paths compare CANONICALIZED, never as spellings — see DepSpec::pathAbs. (The raw `path` is only a
    // fallback for a spec the resolver never canonicalized; inside the resolver `pathAbs` is always set.)
    const std::string& ap = a.pathAbs.empty() ? a.path : a.pathAbs;
    const std::string& bp = b.pathAbs.empty() ? b.path : b.pathAbs;
    return ap == bp && a.git == b.git && a.url == b.url
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
    std::vector<std::string>* projectsOut = nullptr;
    std::string* toolchainOut = nullptr;                  // set to capture the `toolchain` pin (else skipped)
    std::string* nameOut = nullptr;                       // set to capture the `name` (else skipped) — `kama publish`
    std::string* versionOut = nullptr;                    // set to capture the `version` (else skipped) — `kama publish`
    RegConfig* registriesOut = nullptr;                   // set to capture the `registries` config (M3.1b)
    std::map<std::string, DepSpec>* overridesOut = nullptr;// set to capture `overrides` (kama.local.json, M5.3)
    LogConfig* logOut = nullptr;                          // set to capture the `log` config (M5)
    std::map<std::string, TargetSpec>* targetsOut = nullptr;  // set to capture `select.TARGET` entries
    std::map<std::string, SelectGroup>* groupsOut = nullptr;  // set to capture the other `select` groups
    // The `select.TARGET` value carrying `"default": true`. TARGET needs its own sink because it is
    // deliberately NOT a `g_selectGroups` entry (its values are triples with toolchains, not bare names),
    // so it has no `SelectGroup::dflt` to write into.
    std::string* defaultTargetOut = nullptr;
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

    // `select`: { "<GROUP>": { "<VALUE>": { ... }, ... }, ... }. Only the `TARGET` group is consumed
    // here; other groups are skipped (tolerated, not validated) so this reader stays the one place that
    // knows the manifest's shape. A TARGET value is a triple plus an optional toolchain:
    //   "RPI": { "triple": "aarch64-linux-gnu", "cc": "...", "ar": "...", "sysroot": "...",
    //            "cflags": [...], "ldflags": [...] }
    bool selectObject() {
        ws(); if (i >= s.size() || s[i] != '{') return fail("`select` must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string group; if (!str(group)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a select group name");
            ++i; ws();
            if      (group == "TARGET" && targetsOut) { if (!targetGroup()) return false; }
            else if (groupsOut)                       { if (!valueGroup(group)) return false; }
            else if (!skipValue()) return false;
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `select`");
        }
        return true;
    }

    // An ordinary single-select group: { "<VALUE>": { "inherits": "<BASE>", "default": true }, ... }.
    // Same shape as `flags` and `select.TARGET` — one manifest idiom, not three.
    bool valueGroup(const std::string& group) {
        ws(); if (i >= s.size() || s[i] != '{') return fail("a `select` group must be a JSON object");
        ++i; ws();
        SelectGroup& g = (*groupsOut)[group];
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string name; if (!str(name)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a select value name");
            ++i; ws();
            if (i >= s.size() || s[i] != '{') return fail("a `select` value must be an object");
            if (!g.has(name)) g.values.push_back(name);
            ++i; ws();
            if (i < s.size() && s[i] == '}') ++i;
            else while (true) {
                std::string k; if (!str(k)) return false;
                ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in a select value body");
                ++i; ws();
                if (k == "inherits") { std::string base; if (!str(base)) return false; g.inherits[name] = base; }
                else if (k == "default") {
                    if      (s.compare(i, 4, "true")  == 0) { g.dflt = name; i += 4; }
                    else if (s.compare(i, 5, "false") == 0) { i += 5; }
                    else if (!skipValue()) return false;
                } else if (!skipValue()) return false;   // future per-value keys tolerated
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}' in a select value body");
            }
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in a `select` group");
        }
        return true;
    }

    bool targetGroup() {
        ws(); if (i >= s.size() || s[i] != '{') return fail("`select.TARGET` must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string name; if (!str(name)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a target name");
            ++i; ws();
            if (i >= s.size() || s[i] != '{') return fail("a `select.TARGET` value must be an object");
            TargetSpec t; t.name = name;
            std::string triple;
            ++i; ws();
            if (i < s.size() && s[i] == '}') ++i;
            else while (true) {
                std::string k; if (!str(k)) return false;
                ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in a target body");
                ++i; ws();
                if      (k == "triple")  { if (!str(triple))      return false; }
                else if (k == "cc")      { if (!str(t.cc))        return false; }
                else if (k == "ar")      { if (!str(t.ar))        return false; }
                else if (k == "sysroot") { if (!str(t.sysroot))   return false; }
                else if (k == "cflags")  { if (!stringArray(t.cflags))  return false; }
                else if (k == "ldflags") { if (!stringArray(t.ldflags)) return false; }
                // Same spelling every other select value uses (see valueGroup) — a project that only ever
                // builds for one target declares it once instead of retyping `--target`, and the editor
                // can then analyze for it too. `--target` still wins.
                else if (k == "default") {
                    if      (s.compare(i, 4, "true")  == 0) { if (defaultTargetOut) *defaultTargetOut = name; i += 4; }
                    else if (s.compare(i, 5, "false") == 0) { i += 5; }
                    else if (!skipValue()) return false;
                }
                else if (!skipValue()) return false;   // future target keys tolerated
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}' in a target body");
            }
            if (!triple.empty()) {
                std::string terr; TargetSpec parsed;
                if (!parseTriple(triple, parsed, terr)) return fail(terr.c_str());
                t.arch = parsed.arch; t.os = parsed.os; t.abi = parsed.abi;
            }
            (*targetsOut)[name] = t;
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `select.TARGET`");
        }
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
            else if (key == "select") { if (!selectObject()) return false; }   // build-config groups
            else if (key == "dependencies" && deps) { if (!depsObject(deps)) return false; }
            else if (key == "dev-dependencies" && devDeps) { if (!depsObject(devDeps)) return false; }
            else if (key == "registries" && registriesOut) { if (!registriesObject(registriesOut)) return false; }
            else if (key == "overrides" && overridesOut) { if (!depsObject(overridesOut)) return false; }  // kama.local.json dep path-overrides (M5.3)
            else if (key == "log" && logOut) { if (!logObject()) return false; }   // baked log default (M5)
            else if (key == "sources" && sourcesOut) { if (!stringArray(*sourcesOut)) return false; }  // LSP project scope
            else if (key == "projects" && projectsOut) { if (!stringArray(*projectsOut)) return false; } // sub-projects
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

// Load a `kama.json` manifest → the `select.TARGET` catalog. Reuses ManifestReader (unknown keys
// tolerated), so this is orthogonal to the flag load.
static bool loadManifestTargets(const std::string& path, std::map<std::string, TargetSpec>& out,
                                std::map<std::string, SelectGroup>& groupsOut, std::string& err,
                                std::string* defaultTargetOut = nullptr)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> ignoredDeclared, ignoredDefaults;
    ManifestReader r(src, ignoredDeclared, ignoredDefaults);
    r.targetsOut = &out;
    r.groupsOut  = &groupsOut;
    r.defaultTargetOut = defaultTargetOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Names the build configuration owns: a user `flags` entry may not claim one, and `--define` of one is
// always "valid" (it just cannot be declared). Covers the two build types, the synthesized `HOSTED`, the
// three derived triple-component namespaces, and every built-in target name.
static bool isReservedFlagName(const std::string& n)
{
    return kamaIsBuildConfigFlag(n)          // DEBUG/RELEASE/HOSTED + the OS_/ARCH_/ABI_ namespaces
        || builtinTargets().count(n) != 0;   // and every built-in TARGET name, so `--target X` stays unambiguous
}

// Reject a `flags` object that redeclares a build-configuration name. Fills `err` and returns false —
// the caller decides whether that becomes stderr (CLI) or a `window/showMessage` (the LSP, M6 A1).
static bool reservedFlagCheck(const std::string& manifest, const std::set<std::string>& declared,
                              std::string& err)
{
    for (const auto& d : declared)
        if (isReservedFlagName(d)) {
            err = manifest + ": `" + d + "` is a build-configuration name (set by --target/--release) "
                  "and cannot be declared in `flags`";
            return false;
        }
    return true;
}

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

// The module names a package's manifest declares, as they are IMPORTED (`@acme/foo` imports as `foo`).
// Cached: the per-package import check consults it for every import of every build. Declared up beside
// loadProgramUnits, defined here where DepSpec exists.
const std::set<std::string>& declaredImportNames(const std::string& packageDir)
{
    static std::map<std::string, std::set<std::string>> cache;
    auto it = cache.find(packageDir);
    if (it != cache.end()) return it->second;
    std::set<std::string> names;
    std::map<std::string, DepSpec> deps, devDeps;
    std::string err;
    if (loadManifestDeps(packageDir + "/kama.json", deps, err, &devDeps)) {
        for (const auto& kv : deps)    names.insert(importNameOf(kv.first));
        for (const auto& kv : devDeps) names.insert(importNameOf(kv.first));
    }
    return cache.emplace(packageDir, std::move(names)).first->second;
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

// Load a `kama.json` manifest's `projects` — the sub-projects this manifest composes, relative to it,
// each a directory holding its own kama.json. A trailing `/*` expands to every immediate subdirectory that
// has one (`"packages/*"`), so a monorepo need not edit its root manifest per project. Declaring this is
// what turns "is this a monorepo root?" from something the LSP infers from position into something the
// repository states. NOTE the name: `packages` was rejected because kama.lock already uses that key for
// resolved DEPENDENCIES — packages are what you consume, projects are what you compose. (LSP M3.5.)
static bool loadManifestProjects(const std::string& path, std::vector<std::string>& out, std::string& err)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.projectsOut = &out;
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

// ---- build configuration resolution ----------------------------------------------------------------
// The manifest -> target -> flag-set sequence, lifted out of `main`. `kama lsp` returns ~185 lines
// BEFORE this used to run inline, and that is precisely why the server's `@compileFor` set was empty:
// not that the LSP forgot to call this, but that this was not callable. One copy of the precedence
// rules, because two would drift (LSP M6 A1).
//
// `manifest` is already DISCOVERED by the caller, deliberately: the CLI looks next to the input then in
// CWD, while an editor walks UP from the open file bounded by the workspace folder. Those are different
// policies, and a `bool walkUp` parameter would smuggle an editor concern in here. Empty manifest =
// permissive mode (no declared universe, no strict validation, host target, BUILD_TYPE=DEBUG).
//
// NOT included: the `.kama/deps` view-existence check. That is a BUILD precondition — a build reading a
// missing view is wrong, but an editor in a freshly cloned repo still owes you diagnostics on what it
// can see — so it stays in `main`, keyed off the manifest this echoes back.
struct BuildConfigRequest {
    std::string              manifest;                 // discovered kama.json ("" = permissive)
    std::string              target = "HOST";          // --target: a catalog/manifest NAME or a triple
    bool                     targetExplicit = false;   // was --target passed? (a manifest default must lose to it)
    std::vector<std::string> selects, defines, undefines;
    bool                     release = false;          // --release/--debug value
    bool                     releaseExplicit = false;  // was either passed? (sugar must lose to --select)
};
struct BuildConfigResult {
    std::string manifest;        // echoed back — the caller's dep-view check keys off it
    std::string localManifest;   // the kama.local.json merged over it ("" = none)
    std::string buildType;       // the winning BUILD_TYPE value (the editor reports it)
    bool        release = false; // resolved BUILD_TYPE after `inherits` — main's `release` local
    // Which value won in EVERY single-select group, not just BUILD_TYPE (TARGET excepted — it has its own
    // catalog). Exported so the editor's build-config picker reports who won rather than re-deriving it:
    // the precedence ladder below is subtle enough (built-in default < manifest default < --release sugar
    // < --select) that a second implementation in a client would drift.
    std::map<std::string, std::string> selected;
};

// Resolve `req` into the eight file-scope configuration globals. Returns false + `err` (no printing) on
// a malformed manifest, an unknown target, a bad --select or an undeclared flag.
static bool resolveBuildConfig(const BuildConfigRequest& req, BuildConfigResult& out, std::string& err)
{
    // RESET first. Four of these accumulate (`insert` / `operator[]`), which never mattered while `main`
    // was the only caller and called once. The LSP re-resolves on every manifest save, and without this
    // the second resolve would be the UNION of both configurations — flags that should have gone away
    // would linger, which is the one failure mode a config switch must not have.
    g_activeFlags.clear();
    g_declaredFlags.clear();
    g_selectGroups.clear();
    g_manifestTargets.clear();
    g_strictFlags = false;
    g_logDefault.clear();
    g_target  = TargetSpec();
    g_release = false;

    bool        release = req.release;
    std::string selTarget = req.target;

    // Built-in groups first: a project may EXTEND BUILD_TYPE/OUTPUT, so they must exist before the
    // manifest is read. `seedBuiltinSelectGroups` assigns rather than merges, so it is re-entrant.
    seedBuiltinSelectGroups();

    if (!req.manifest.empty()) {
        const std::string& manifest = req.manifest;
        std::set<std::string> declared, defaults;
        std::string dfltTarget;                       // `select.TARGET` value carrying `"default": true`
        if (!loadManifestFlags(manifest, declared, defaults, err)) { err = manifest + ": " + err; return false; }
        if (!reservedFlagCheck(manifest, declared, err)) return false;
        if (!loadManifestTargets(manifest, g_manifestTargets, g_selectGroups, err, &dfltTarget)) {
            err = manifest + ": " + err;
            return false;
        }
        // Baked log default (M5): the manifest `log` section becomes the project's compiled-in KAMA_LOG
        // spec, seeded into the process env in `main` (overwrite=0, so `--log`/env still win).
        LogConfig logCfg;
        if (!loadManifestLog(manifest, logCfg, err)) { err = manifest + ": " + err; return false; }

        // `kama.local.json` (M5.2): a gitignored sibling of the manifest that DEEP-MERGES over it for the
        // fields the compiler reads directly here — `flags` (union: local declares/enables more), `select`
        // (local targets win) and `log` (per-tag merge, local wins). Local-only by construction (never
        // committed / never in the lockfile), so it can never perturb a reproducible or CI build. Its dep
        // `overrides` + `registries` + `toolchain` local overrides (M5.3) are read on the install/selector
        // paths (`resolveProject`, `resolvePin`), not here.
        //
        // It is ALSO the LSP's build-configuration override channel (M6 A1): the VS Code status-bar
        // picker writes this file, so the editor and a plain `kama build` cannot disagree — one
        // mechanism, honoured by every editor and by the CLI, and gitignored so CI never sees it.
        std::string mdir = dirName(manifest);
        std::string localManifest = (mdir == "." ? std::string() : mdir + "/") + "kama.local.json";
        if (std::ifstream(localManifest).good()) {
            out.localManifest = localManifest;
            std::set<std::string> ldeclared, ldefaults;
            if (!loadManifestFlags(localManifest, ldeclared, ldefaults, err)) {
                err = localManifest + ": " + err;
                return false;
            }
            if (!reservedFlagCheck(localManifest, ldeclared, err)) return false;
            declared.insert(ldeclared.begin(), ldeclared.end());   // union — local extends the universe
            defaults.insert(ldefaults.begin(), ldefaults.end());
            std::map<std::string, TargetSpec> ltargets;
            std::map<std::string, SelectGroup> lgroups;
            std::string ldfltTarget;
            if (!loadManifestTargets(localManifest, ltargets, lgroups, err, &ldfltTarget)) {
                err = localManifest + ": " + err;
                return false;
            }
            // ⚠️ FIELD-BY-FIELD, not whole-value. `kama.local.json` is documented everywhere as a
            // DEEP merge (docs/packages.md), and a whole-value replace broke that for targets alone: an
            // override naming a target to say one thing about it — which is exactly what the editor's
            // build-config picker writes, `{"RPI": {"default": true}}` — silently erased the triple and
            // toolchain kama.json had declared for it, and the build then failed with "target 'RPI'
            // declares no `triple`". Local wins per FIELD it actually sets.
            for (auto& kv : ltargets) {
                auto ex = g_manifestTargets.find(kv.first);
                if (ex == g_manifestTargets.end()) { g_manifestTargets[kv.first] = kv.second; continue; }
                TargetSpec& t = ex->second;
                const TargetSpec& l = kv.second;
                if (!l.arch.empty())    t.arch    = l.arch;
                if (!l.os.empty())      t.os      = l.os;
                if (!l.abi.empty())     t.abi     = l.abi;
                if (!l.cc.empty())      t.cc      = l.cc;
                if (!l.ar.empty())      t.ar      = l.ar;
                if (!l.sysroot.empty()) t.sysroot = l.sysroot;
                if (!l.cflags.empty())  t.cflags  = l.cflags;
                if (!l.ldflags.empty()) t.ldflags = l.ldflags;
            }
            for (auto& kv : lgroups) {                                            // local extends/wins
                SelectGroup& g = g_selectGroups[kv.first];
                for (const auto& v : kv.second.values) if (!g.has(v)) g.values.push_back(v);
                for (const auto& iv : kv.second.inherits) g.inherits[iv.first] = iv.second;
                if (!kv.second.dflt.empty()) g.dflt = kv.second.dflt;
            }
            if (!ldfltTarget.empty()) dfltTarget = ldfltTarget;
            LogConfig localLog;
            if (!loadManifestLog(localManifest, localLog, err)) { err = localManifest + ": " + err; return false; }
            logCfg.applyLocal(localLog);
        }

        g_declaredFlags = declared;
        // A declared select VALUE is a legitimate `@compileFor` name too — it enters the active set
        // when its group selects it — so strict validation must accept it without the project also
        // listing it under `flags`. (Built-in target names stay out: see kamaIsBuildConfigFlag.)
        for (const auto& kv : g_selectGroups)
            for (const auto& v : kv.second.values) g_declaredFlags.insert(v);
        for (const auto& kv : g_manifestTargets) g_declaredFlags.insert(kv.first);
        g_strictFlags   = true;
        for (auto& d : defaults) g_activeFlags.insert(d);
        g_logDefault = logCfg.canonical();

        // A project that only ever builds for one target declares it once instead of retyping --target
        // (and the editor then analyzes for it too). An explicit --target still wins.
        if (!req.targetExplicit && !dfltTarget.empty()) selTarget = dfltTarget;
    }
    out.manifest = req.manifest;

    // Resolve `--target` to a triple, against the built-in catalog plus whatever the manifest declared.
    // An unknown NAME is an error, but anything containing '-' is an anonymous triple, so a one-off
    // cross build needs no config file.
    if (!resolveTarget(selTarget, g_manifestTargets, g_target, err)) return false;

    // Build the active `@compileFor` flag set (the "structure" axis — reproducible, from the explicit
    // build invocation, never ambient env). The selected target contributes its NAME plus one flag per
    // triple component (ARCH_/OS_/ABI_) and `HOSTED`; `--release` contributes DEBUG/RELEASE. User flags
    // come from `--define`, with the manifest's `"default": true` ones layered in above.
    for (const auto& f : derivedTargetFlags(g_target, builtinTargets().count(g_target.name) != 0))
        g_activeFlags.insert(f);

    // `--no-heap` is a build-configuration fact like `--release`, so it also contributes a flag. That lets
    // the STDLIB opt a declaration out of a no-heap build (`@compileFor(!NOHEAP)` on the allocating
    // `sort`), which is the only way today to make "this needs the heap" a compile error rather than a
    // silent malloc: the `rejectIfNoHeap` gate covers `new`/interpolation/`spawn`, not container growth.
    if (g_noHeap) g_activeFlags.insert("NOHEAP");

    // Single-select groups. `--release`/`--debug` are sugar for `--select BUILD_TYPE=…`, and lose to an
    // explicit `--select` on the same group so there is exactly one answer to "who won".
    {
        std::map<std::string, std::string> explicitSel;   // from --select only
        for (const auto& sel : req.selects) {
            size_t eq = sel.find('=');
            if (eq == std::string::npos || eq == 0 || eq + 1 == sel.size()) {
                err = "--select expects GROUP=VALUE (got '" + sel + "')";
                return false;
            }
            std::string group = sel.substr(0, eq), value = sel.substr(eq + 1);
            if (group == "TARGET") { err = "use --target for the TARGET group"; return false; }
            auto g = g_selectGroups.find(group);
            if (g == g_selectGroups.end()) {
                std::string known;
                for (const auto& kv : g_selectGroups) known += (known.empty() ? "" : ", ") + kv.first;
                err = "--select names no such group '" + group + "' (declare it under `select` in "
                      "kama.json; known: TARGET, " + known + ")";
                return false;
            }
            if (!g->second.has(value)) {
                std::string vals;
                for (const auto& v : g->second.values) vals += (vals.empty() ? "" : "|") + v;
                err = "'" + value + "' is not a value of select group " + group + " (expected " + vals + ")";
                return false;
            }
            // One value per group is the whole point of a single-select axis: `--define WINDOWS --define
            // LINUX` used to be silently accepted, and that ambiguity is what this replaces.
            auto prev = explicitSel.find(group);
            if (prev != explicitSel.end() && prev->second != value) {
                err = "select group " + group + " given two values ('" + prev->second + "' and '" + value +
                      "') — it is single-select";
                return false;
            }
            explicitSel[group] = value;
        }

        // Precedence, lowest first: the group's own built-in default, the manifest's `default: true`,
        // the `--release`/`--debug` sugar, then an explicit `--select`.
        std::map<std::string, std::string> chosen;
        chosen["BUILD_TYPE"] = "DEBUG";
        for (const auto& kv : g_selectGroups)
            if (!kv.second.dflt.empty()) chosen[kv.first] = kv.second.dflt;
        if (req.releaseExplicit) chosen["BUILD_TYPE"] = release ? "RELEASE" : "DEBUG";
        for (const auto& kv : explicitSel) chosen[kv.first] = kv.second;

        for (const auto& kv : chosen) {
            auto g = g_selectGroups.find(kv.first);
            if (g == g_selectGroups.end()) continue;
            std::set<std::string> chain;                             // the value plus what it inherits
            collectInherited(g->second, kv.second, chain);
            for (const auto& f : chain) g_activeFlags.insert(f);
        }
        // A user BUILD_TYPE inherits its base's driver behavior, so `FAST: {inherits: RELEASE}` gets
        // -O3/-DNDEBUG/strip and `debugAssert` stripping without redeclaring any of it.
        release        = g_activeFlags.count("RELEASE") != 0;
        out.buildType  = chosen["BUILD_TYPE"];
        out.selected   = chosen;
    }
    g_release = release;   // release also strips `debugAssert` (threaded to the emitter via setRelease)
    out.release = release;

    // User flags: under strict mode (a manifest was loaded) validate names against the declared
    // universe before applying. `--define` adds; `--undefine` removes (e.g. turning off a default).
    for (const auto& d : req.defines) {
        if (g_strictFlags && !isReservedFlagName(d) && !g_declaredFlags.count(d)) {
            err = "--define references undeclared flag `" + d + "` (add it to the `flags` object in kama.json)";
            return false;
        }
        g_activeFlags.insert(d);
    }
    for (const auto& u : req.undefines) {
        if (g_strictFlags && !isReservedFlagName(u) && !g_declaredFlags.count(u)) {
            err = "--undefine references undeclared flag `" + u + "` (add it to the `flags` object in kama.json)";
            return false;
        }
        g_activeFlags.erase(u);
    }
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
    configureEmitter(emitter);
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
    configureEmitter(emitter);
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

// Run `cmds` with at most `jobs` of them in flight — the `-j` compile pool. `rcs` is sized to
// cmds.size(): each entry is that command's exit status, or -1 if an earlier failure stopped the wave
// before it was ever launched. Returns the status of the LOWEST-INDEXED failing command (0 if all
// succeeded), so the caller's error message names the failure a serial run would have named first.
//
// On the first failure we stop LAUNCHING but let what is in flight finish. Killing children risks
// leaving a truncated .o in the user's output directory, and the cost of draining is at most one more
// compile — while the extra diagnostics are often the more useful ones.
//
// Why not a pool of threads calling runCmd(): system() sets SIGINT/SIGQUIT to SIG_IGN and blocks
// SIGCHLD for the whole PROCESS, then restores. N concurrent callers interleave that, and the first to
// finish restores the default disposition while N-1 children are still running — so Ctrl-C during a
// build lands somewhere undefined. Here the dispositions are saved once for the wave and restored once,
// which is the same protection system() gives, done once instead of N times racily. It would also be
// the first thread in an otherwise single-threaded compiler.
//
// Children exec `/bin/sh -c` rather than the compiler directly, because a kama "compiler" is a
// SHELL STRING, not an argv — `"…/zig" cc`, or `clang -fsanitize=address,undefined -g` from the test
// harness. Tokenizing that here would be a quoting-bug generator; sh already does it correctly. sh also
// preserves the 128+signal exit convention that run_tests.sh reads to tell a rejection from a crash.
int runCmdsParallel(const std::vector<std::string>& cmds, int jobs, std::vector<int>& rcs)
{
    rcs.assign(cmds.size(), -1);
    if (cmds.empty()) return 0;

#if defined(_WIN32)
    // No posix_spawn/waitpid; `-j` is clamped to 1 on Windows, so this is the only path taken there.
    (void)jobs;
    for (size_t i = 0; i < cmds.size(); ++i) {
        rcs[i] = runCmd(cmds[i]);
        if (rcs[i] != 0) return rcs[i];
    }
    return 0;
#else
    if (jobs < 1) jobs = 1;

    struct sigaction ign, oldInt, oldQuit;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT,  &ign, &oldInt);
    sigaction(SIGQUIT, &ign, &oldQuit);

    std::map<pid_t, size_t> live;   // pid -> index in cmds
    size_t next = 0;
    bool stop = false;

    while ((!stop && next < cmds.size()) || !live.empty()) {
        while (!stop && next < cmds.size() && (int)live.size() < jobs) {
            pid_t pid = fork();
            if (pid < 0) {
                rcs[next] = 127;      // could not fork
                stop = true;
                break;
            }
            if (pid == 0) {
                // Child. Restore the DEFAULT dispositions we suppressed in the parent, so a Ctrl-C at
                // the terminal (delivered to the whole foreground process group) kills the compilers
                // while kama survives to drain them, replay diagnostics and clean up. Only
                // async-signal-safe calls between fork and exec — signal() and execl() both are.
                signal(SIGINT,  SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                execl("/bin/sh", "sh", "-c", cmds[next].c_str(), (char*)nullptr);
                _exit(127);           // exec failed; 127 is the shell's own "cannot execute"
            }
            live[pid] = next++;
        }
        if (live.empty()) break;
        int st = 0;
        // waitpid(-1) is safe: every other runCmd() in the driver is synchronous, so this wave's
        // children are the only ones kama can have outstanding.
        pid_t done = waitpid(-1, &st, 0);
        if (done < 0) { if (errno == EINTR) continue; break; }
        std::map<pid_t, size_t>::iterator it = live.find(done);
        if (it == live.end()) continue;
        rcs[it->second] = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        if (rcs[it->second] != 0) stop = true;
        live.erase(it);
    }

    sigaction(SIGINT,  &oldInt,  nullptr);
    sigaction(SIGQUIT, &oldQuit, nullptr);

    for (size_t i = 0; i < cmds.size(); ++i) if (rcs[i] > 0) return rcs[i];
    return 0;
#endif
}

// Copy a captured child stream to ours verbatim, then remove the file. Per-job capture + ordered replay
// is what makes interleaving unrepresentable rather than merely unlikely: today's single invocation
// emits diagnostics in source order because the C compiler compiles the sources in order, and replaying
// in input order reproduces exactly that.
static void replayAndRemove(const std::string& path, FILE* to)
{
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, to);
        fclose(f);
    }
    remove(path.c_str());
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
// `requestorDir` is the directory of the manifest that DECLARED this dep — a path spec resolves against
// it, not against the root project (they differ the moment a workspace member declares a sibling).
static bool resolveOne(const std::string& name, const DepSpec& spec, const std::string& requestorDir,
                       const std::map<std::string, LockEntry>& oldLock,
                       LockEntry& out, std::string& storePath, std::string& err)
{
    if (!spec.path.empty()) {
        std::string target = spec.pathAbs.empty() ? absolutePath(requestorDir + "/" + spec.path) : spec.pathAbs;
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

    // The workspace this project belongs to, if any. Computed ONCE here rather than inside the attempt
    // loop below: it depends only on the manifests on disk, so it is immutable across restarts.
    const std::set<std::string> wsMembers = workspaceMembers(base);

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

        // `requestor` is a display name for diagnostics; `requestorDir` is the directory of the manifest
        // that declared this dep — what a path spec is actually relative to.
        struct Req { std::string name; DepSpec spec; std::string requestor; std::string requestorDir; bool dev; };
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
                // Canonicalize the path spec against its OWN declaring manifest before anything compares
                // or materializes it — the single point where a spelling becomes a directory.
                if (!r.spec.path.empty() && r.spec.pathAbs.empty())
                    r.spec.pathAbs = absolutePath(r.requestorDir + "/" + r.spec.path);
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
                // A path dep from a FETCHED package is not reproducible — the local directory it names is
                // not carried with it. Between two members of one declared `projects` workspace it is
                // exactly as reproducible as the workspace itself, which is what makes a sub-project able
                // to declare the siblings it imports, and so able to be lifted out and still build.
                if (!r.spec.path.empty() && r.requestor != "<root manifest>" &&
                    !(wsMembers.count(absolutePath(r.requestorDir)) && wsMembers.count(r.spec.pathAbs))) {
                    fprintf(stderr, "kama install: path dependency '%s' (required by %s) is only allowed at the "
                            "top level, or between members of one declared `projects` workspace — a fetched "
                            "package cannot reference a local path reproducibly\n",
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
                if (!resolveOne(r.name, r.spec, r.requestorDir, oldLock, e, storePath, ferr)) {
                    fprintf(stderr, "kama install: %s\n", ferr.c_str()); return 1;
                }
                // The lock lives at the ROOT, so a path it records must be root-relative to mean anything.
                // A root-declared dep already is — keep its spelling verbatim, so an existing lock stays
                // byte-identical on re-install. Only a workspace member's sibling dep gets re-based.
                if (!r.spec.path.empty() && r.requestor != "<root manifest>")
                    e.path = relativePath(absolutePath(base), storePath);
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
                        // storePath is where THIS package's manifest lives — the dir its own path specs
                        // are relative to. For a path dep that is the local package dir itself.
                        q.push_back({ck.first, ck.second, r.name, storePath, r.dev}); }
                    std::sort(directNames.begin(), directNames.end());   // deterministic dependencies[] order
                }
                e.dependencies = directNames;
                lock[r.name] = e;
            }
            return 0;
        };

        std::deque<Req> prodQ, devQ;
        for (auto& kv : deps)    prodQ.push_back({kv.first, kv.second, "<root manifest>", base, false});
        for (auto& kv : devDeps) devQ.push_back({kv.first, kv.second, "<root manifest>", base, true});
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

// ------------------------------------------------------------------------------------------------
// `--json` output for `kama query` / `kama check`.
//
// The TEXT forms are deliberately unchanged and deliberately varied — `L:C kind name`, `path:L:C`,
// `key=value`, tab-separated completion rows — because each suits its own question when a human greps
// it, and 200+ assertions pin them. What a MACHINE needs is not those four shapes harmonized, but one
// format it can parse without knowing which mode produced it. That is this.
//
// Every response is the same envelope, so a caller can dispatch on `mode` and read `results` without a
// per-mode parser, and an empty result is `[]` rather than one of the magic strings the text form uses
// ("no definition", "no type", "no references", "no signature", "no symbols", "no diagnostics"):
//
//   {"schema":1,"mode":"search","file":"src/app.kama","results":[…]}
//
// `schema` is a version, not decoration: it is the promise a consumer can pin to, so adding a field is
// safe and changing the shape of one is not. Bump it when an existing field changes meaning.
// `results` is deliberately NOT seeded here: members keep insertion order, so seeding it would push every
// mode-specific key (`query`, `context`, `ok`) after the payload. Each arm sets it last. A mode that
// forgot would emit no `results` at all, which is why check-query.sh asserts the key on every one.
Json jsonEnvelope(const char* mode, const std::string& file)
{
    Json j = Json::object();
    j.set("schema", 1);
    j.set("mode", mode);
    j.set("file", file);
    return j;
}

// One printed line, so every `--json` exit goes through the same place. Trailing newline: the output is
// a line-oriented record as far as a shell pipeline is concerned.
int jsonPrint(const Json& j) { printf("%s\n", serialize(j).c_str()); return 0; }

// A source position, in the coordinates the caller asked in: 1-based line, 0-based column (kama.query.h).
// No LSP conversion here — `kama query` is not the LSP, and silently shifting a line by one between the
// text and JSON forms of the same query would be indefensible.
Json jsonPos(int line, int column)
{
    Json j = Json::object();
    j.set("line", line);
    j.set("column", column);
    return j;
}

Json jsonSymbol(const SymbolInfo& s, bool withUri)
{
    Json j = jsonPos(s.selectionRange.line, s.selectionRange.column);
    if (withUri) j.set("uri", s.uri);
    j.set("kind", symKindName(s.kind));
    j.set("name", s.name);
    if (!s.container.empty()) j.set("container", s.container);
    return j;
}

Json jsonDiagnostic(const Diagnostic& d)
{
    Json j = Json::object();
    j.set("file", d.file);
    j.set("line", d.line);
    j.set("column", d.column);          // Diagnostic's own convention: 0 = whole line / unknown
    if (d.endLine)   j.set("endLine", d.endLine);
    if (d.endColumn) j.set("endColumn", d.endColumn);
    j.set("severity", diagSeverityName(d.severity));
    if (!d.code.empty()) j.set("code", d.code);
    j.set("message", d.message);
    return j;
}

// ------------------------------------------------------------------------------------------------
// `kama agents` — write the agent guidance into a project.
//
// The content is agents/AGENTS.md and nothing else; every other file is a POINTER to it (see
// kama.agents.h). That is not a shortcut, it is the design: `AGENTS.md` is an open cross-tool
// standard most agents read natively, so one file reaches them, and duplicating its text per tool
// would create N things to keep in sync for no gain.
//
// Everything is embedded in the binary (tools/embed_agents.sh), so this works from a `--no-std`
// install and needs no path resolution. User docs: docs/agents.md.

// The embedded text starts with the newline that follows `R"KAMAGENTS(`. Drop it so a written file
// does not open with a blank line.
static const char* agentBody(const char* s) { return (s && *s == '\n') ? s + 1 : s; }

void agentsUsage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama agents install [<dir>] [--claude] [--tool <name>]... [--all-tools] [--skill] [--force]\n"
        "                                      write AGENTS.md (+ pointers) into <dir> (default: .)\n"
        "  kama agents print [--skill]         write the guidance to stdout instead\n"
        "  kama agents list                    which tools are covered, and the file each one gets\n");
}

int cmdAgentsList()
{
    printf("AGENTS.md is read natively by most agent tools (Codex, Cursor, Windsurf, Gemini CLI,\n"
           "Zed, Aider, Warp, VS Code/Copilot, Jules, Junie, Amp, RooCode, goose, opencode, Devin,\n"
           "Kilo, Factory, Augment, ...). `kama agents install` writes it and nothing else is needed.\n\n"
           "These tools read something else, so they get a POINTER to AGENTS.md — never a copy:\n\n");
    printf("  %-12s %-34s %s\n", "--tool", "writes", "note");
    printf("  %-12s %-34s %s\n", "------", "------", "----");
    for (int i = 0; i < KAMA_AGENT_STUB_COUNT; ++i) {
        const KamaAgentStub& s = KAMA_AGENT_STUBS[i];
        const char* note = strcmp(s.name, "claude") == 0 ? "an `@AGENTS.md` import (--claude)"
                                                         : "one line: read AGENTS.md";
        printf("  %-12s %-34s %s\n", s.name, s.dest, note);
    }
    printf("\n  %-12s %-34s %s\n", "--skill", ".claude/skills/kama/SKILL.md",
           "the richer on-demand cookbook");
    printf("\n--all-tools writes every pointer above. Nothing is overwritten without --force.\n");
    return 0;
}

// Write `body` to <dir>/<rel>, creating parent directories. Refuses an existing file unless forced,
// because this writes into somebody's repository and an AGENTS.md they already wrote is the common
// case, not the exception.
static bool agentsWrite(const std::string& dir, const std::string& rel, const char* body,
                        bool force, int& written)
{
    std::string path = dir.empty() || dir == "." ? rel : dir + "/" + rel;
    if (fileExists(path) && !force) {
        fprintf(stderr, "kama agents: %s exists — pass --force to overwrite, or use "
                        "`kama agents print` and merge by hand\n", path.c_str());
        return false;
    }
    std::string parent = dirName(path);
    if (!parent.empty() && parent != path && !dirExists(parent) && !makeDirs(parent)) {
        fprintf(stderr, "kama agents: cannot create %s\n", parent.c_str());
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) { fprintf(stderr, "kama agents: cannot write %s\n", path.c_str()); return false; }
    out << agentBody(body);
    if (!out) { fprintf(stderr, "kama agents: failed writing %s\n", path.c_str()); return false; }
    printf("kama agents: wrote %s\n", path.c_str());
    ++written;
    return true;
}

void usage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama transpile <in.kama> [-o out.c] [--no-line] [--dev]\n"
        "  kama build     <in.kama>... [-o out] [--target <name-or-triple>] [--release|--debug] [--shared]\n"
        "                  (--target: HOST|MACOS|WINDOWS|LINUX|WASM|EMBEDDED, a kama.json `select.TARGET`\n"
        "                   entry, or a bare <arch>-<os>-<abi> triple such as aarch64-linux-gnu)\n"
        "                             [--no-heap] [--link <lib>]... [--webgpu] [--cc <compiler>] [--no-line] [--keep-c] [--dev]\n"
        "                             [-j|--jobs <n>]   concurrent C compiles (default: core count)\n"
        "                  (pass multiple .kama files to build a multi-file program; --dev also resolves dev-dependencies)\n"
        "  kama run       [<file>] [--release|--debug] [--dev] [--define NAME]... [--config PATH] [-- <program args>]\n"
        "                  (build the entry .kama — explicit <file>, else the manifest \"main\" — and run it; native-only)\n"
        "  kama check     <in.kama>... [--each] [--json]   analyze without emitting C or invoking a C compiler\n"
        "                  (name resolution, named arguments, ownership/move and serde analysis. NOT a full\n"
        "                   type check: an expression type mismatch is caught by `kama build`, not here.\n"
        "                   Several inputs are ONE program; --each makes each input its own program and\n"
        "                   checks them all in one process, reusing the parsed prelude and import closure —\n"
        "                   it then prints one `<exit-code> <path>` verdict line per input on stdout)\n"
        "  kama query     <file> <mode>... [--json]  ask the compiler what it resolved — the agent/editor interface\n"
        "                  (--symbols | --search NAME | --def L:C | --type L:C | --refs L:C | --complete L:C\n"
        "                   | --sighelp L:C | --diagnostics | --coverage; --project widens from <file>'s\n"
        "                   imports to the whole package. Coordinates are 1-based LINE, 0-based COLUMN.\n"
        "                   Modes are repeatable and combinable — asked together they are answered in order\n"
        "                   from ONE analysis, which is nearly the whole cost of a query.\n"
        "                   --json gives one envelope for every mode: {schema,mode,file,results})\n"
        "  kama lsp                            language server (JSON-RPC 2.0 over stdio) — see docs/editors.md\n"
        "  kama agents install [<dir>]         write AGENTS.md so an AI agent knows this project + `kama query`\n"
        "                  ([--claude] [--tool <name>]... [--all-tools] [--skill] [--force];\n"
        "                   `kama agents list` shows the tools, `kama agents print` writes to stdout)\n"
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
// `partial` (M5.4): this index was built from a buffer that did NOT fully parse — the recovery arms
// discarded the broken parts and analysis ran on what was left. It is a live, current index rather than
// a stale one, which is the whole point; callers that care (the M4.6 repair fallback) can ask.
struct LspIndex { std::shared_ptr<CEmitter> idx; std::string path; bool partial = false; };

bool lspIndexIsPartial(const SharedLspIndex& idx) { return idx && idx->partial; }

SharedLspIndex lspAnalyze(const std::string& path, const std::string& text,
                          std::vector<Diagnostic>& diags, const char* argv0)
{
    ParseResult pr = parseForQuery(text.c_str(), path);
    if (pr.ctx) for (const auto& d : pr.ctx->diagnostics) diags.push_back(d);
    if (!pr.unit) {   // the parse aborted outright (garbage at the very first token, essentially) —
        timingDump("analyze-failed", path);   // the diags carry the errors; there is no AST to index
        return nullptr;
    }

    // Load the whole program so cross-module names resolve (imported modules parsed from disk, exactly as
    // `kama check`/`build` do), then substitute the in-memory buffer for the open file's on-disk unit so
    // unsaved edits still analyze. Without this the single open buffer sees only the built-in prelude, so
    // every `import`ed type reads as "not exported" — a cascade of false diagnostics. Falls back to
    // single-file if the file isn't on disk yet (a fresh unsaved buffer) or a module can't be resolved:
    // best-effort diagnostics then, but hover/def/outline for the open file still work.
    std::vector<SharedCompilationUnit> units;
    std::vector<std::string> paths;
    // M6 A3: manifest findings (an import this package uses but never declared) come back as structured
    // diagnostics rather than only as stderr, and are pushed onto `diags` DIRECTLY — deliberately not
    // through `emitter->diagnostics()` below, which the droppedTopLevelDecl gate suppresses. This is a
    // manifest fact, not a semantic cascade, so a broken `type` elsewhere in the buffer must not hide it.
    std::vector<Diagnostic> importDiags;
    if (loadProgramUnits({ path }, argv0, units, paths, /*includeDevDeps*/ false, /*strictImports*/ false,
                         &importDiags) && !units.empty()) {
        std::string abs = absolutePath(path);
        bool swapped = false;
        for (size_t i = 0; i < paths.size(); ++i)     // paths bounds the scan; the two are parallel
            if (paths[i] == abs) { units[i] = pr.unit; swapped = true; break; }
        if (!swapped) units.insert(units.begin(), pr.unit);   // defensive: keep the live buffer in the set
    } else {
        units = { pr.unit };                       // single-file fallback (unsaved/new file or unresolved import)
    }

    // Only the OPEN file's, for the same reason the semantic filter below exists: the server publishes to
    // one URI, and a sibling package's manifest problem is not this buffer's squiggle.
    //
    // ⚠️ Compare CANONICALIZED, and hand back the buffer's own SPELLING. These diagnostics are stamped with
    // `paths[i]`, which is `absolutePath(...)` — on macOS that resolves /var -> /private/var, while `path`
    // came from the editor's file:// URI and did not. A plain `d.file == path` therefore matched nothing and
    // the squiggle silently never appeared. Same path-spelling hazard M3.5 hit with `underRoot`; here it is
    // exact equality rather than a prefix, but it is the same lesson.
    {
        std::string selfAbs = absolutePath(path);
        for (const auto& d : importDiags)
            if (d.file == selfAbs || d.file == path) {
                Diagnostic mine = d;
                mine.file = path;   // publish under the spelling the server keys documents by
                diags.push_back(mine);
            }
    }

    auto emitter = std::make_shared<CEmitter>(path);   // analysis mode: no C emitted
    configureEmitter(*emitter);   // M6 A1: the SAME configuration a build uses, not a subset of it
    { Stopwatch sw(&timing().analyze); emitter->analyze(units); }
    // Only the OPEN file's diagnostics go back to the editor (the server publishes to one URI); imported
    // modules are analyzed for context, not surfaced. Their diagnostics carry a different `file`.
    //
    // ...EXCEPT when the top-level recovery arm fired (M5.4). Losing a whole `type` or `fn` makes every
    // reference to it read as undeclared, so the file fills with squiggles describing the recovery
    // rather than the code — noise that hides the one real error above it. The finer arms lose a single
    // statement or member and cascade barely at all, so their semantics are published normally. This is
    // the same bargain TypeScript, clangd and rust-analyzer strike: publish semantics on a broken file,
    // and rely on recovery preserving the declaration SHELL to keep the cascade small. Parse
    // diagnostics always publish — reporting many errors instead of one is the point of M5.3.
    if (pr.droppedTopLevelDecl == 0)
        for (const auto& d : emitter->diagnostics()) if (d.file == path) diags.push_back(d);
    auto h = std::make_shared<LspIndex>();
    h->idx = emitter;
    h->path = path;
    h->partial = pr.partial;
    timingDump("analyze", path);
    return h;
}

// ---- workspace indexing (M3.5) ---------------------------------------------------------------------
// (collectKamaFiles lives up beside listKamaFiles — module resolution needs it too.)

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
//   `projects` present -> each entry is a sub-project directory holding its own kama.json, walked the same
//                         way. A trailing `/*` ("packages/*") expands to every immediate subdirectory that
//                         has a manifest, so a monorepo need not edit its root manifest per project.
//   neither present    -> a leaf: the project's whole directory is its sources.
//
// A manifest with `projects` but no `sources` is a pure aggregator and contributes no files of its own —
// walking its directory would re-collect every member and defeat the precision it just declared.
// `visited` (canonical paths) breaks cycles: a manifest may legally name a directory that names it back,
// and a symlink makes a loop trivial.
static void collectPackageTree(const std::string& dir, std::vector<std::string>& out,
                               std::set<std::string>& visited)
{
    if (!visited.insert(absolutePath(dir)).second) return;

    std::vector<std::string> srcs, subs2;
    std::string err;
    const std::string manifest = dir + "/kama.json";
    if (fileExists(manifest)) {
        loadManifestSources(manifest, srcs, err);
        loadManifestProjects(manifest, subs2, err);
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
    } else if (subs2.empty()) {
        collectKamaFiles(absolutePath(dir), "", out, seen, (size_t)-1);
    }

    for (const auto& rel : subs2)
        for (const auto& sub : expandProjectsEntry(dir, rel)) collectPackageTree(sub, out, visited);
}

std::string lspRealPath(const std::string& path) { return absolutePath(path); }

void lspSetParseCache(bool on) { g_parseCache = on; if (!on) g_parseCacheMap.clear(); }

void lspEvictParsedFile(const std::string& path)
{
    if (path.empty()) { g_parseCacheMap.clear(); return; }
    // Match on the ABSOLUTE path even though the map is keyed by spelling: one file can be cached under
    // two spellings and both must go. The map is small and this runs per file-change, not per keystroke.
    std::string abs = absolutePath(path);
    for (auto it = g_parseCacheMap.begin(); it != g_parseCacheMap.end();)
        it = (it->second.abs == abs) ? g_parseCacheMap.erase(it) : std::next(it);
}

// ---- build configuration, from the editor's side (M6 A1) --------------------------------------------

bool lspIsManifestPath(const std::string& path)
{
    std::string base = baseName(path);
    return base == "kama.json" || base == "kama.local.json";
}

// The single-select axes an editor may switch between, read off the globals a resolve just installed.
// TARGET leads because it is the one a user changes most; the rest follow in map order.
//
// TARGET's catalog is the union of the built-in names and the manifest's own — the same union
// `resolveTarget` resolves against, so the picker can only ever offer something a build would accept.
// Anonymous triples are deliberately absent: they are a one-off CLI gesture (`--target aarch64-linux-gnu`
// with no declaration), and there is no finite list of them to put in a menu.
static void lspCollectSelectGroups(const BuildConfigResult& res, std::vector<LspSelectGroup>& out)
{
    LspSelectGroup t;
    t.name = "TARGET";
    for (const auto& kv : builtinTargets())  t.values.push_back(kv.first);
    for (const auto& kv : g_manifestTargets)
        if (!builtinTargets().count(kv.first)) t.values.push_back(kv.first);
    t.selected = g_target.name;
    out.push_back(std::move(t));

    for (const auto& kv : g_selectGroups) {
        LspSelectGroup g;
        g.name   = kv.first;
        g.values = kv.second.values;
        auto sel = res.selected.find(kv.first);
        if (sel != res.selected.end()) g.selected = sel->second;
        out.push_back(std::move(g));
    }
}

bool lspResolveBuildConfig(const std::string& hintPath, const std::string& workspaceRoot,
                           LspBuildConfig& out, std::string& err)
{
    out = LspBuildConfig();

    // Find the NEAREST kama.json at or above the open file. Deliberately NOT lspFindProject, which also
    // enumerates every .kama under the root — that is the workspace-index cost and has no business on a
    // path that runs before the first analysis. Only the walk SHAPE is shared: a `.kama` component stops
    // it so a vendored dependency keeps its own manifest, and the editor's folder bounds it so a stray
    // kama.json in $HOME can never be picked up.
    std::string manifest;
    if (!hintPath.empty()) {
        std::string wsRoot = workspaceRoot.empty() ? std::string() : absolutePath(workspaceRoot);
        std::string cur    = dirName(absolutePath(hintPath));
        bool underWorkspace = !wsRoot.empty() &&
                              (cur == wsRoot || cur.compare(0, wsRoot.size() + 1, wsRoot + "/") == 0);
        for (;;) {
            if (baseName(cur) == ".kama") break;
            if (fileExists(cur + "/kama.json")) { manifest = cur + "/kama.json"; break; }
            if (underWorkspace && cur == wsRoot) break;      // examined it, go no higher
            std::string parent = dirName(cur);
            if (parent == cur || parent == ".") break;       // filesystem root
            cur = parent;
        }
    }

    BuildConfigRequest req;
    req.manifest = manifest;   // no --target/--select/--define: the editor's configuration comes from files

    BuildConfigResult res;
    if (!resolveBuildConfig(req, res, err)) {
        // Never leave a HALF-APPLIED configuration: a partially-populated flag set would analyze a program
        // that is neither what the manifest asked for nor a sane default. Fall back to permissive host
        // defaults (which cannot fail — no manifest is read) and report the error to the client.
        BuildConfigRequest bare;
        std::string ignored;
        BuildConfigResult bres;
        resolveBuildConfig(bare, bres, ignored);
        out.targetName   = g_target.name;
        out.targetTriple = g_target.triple();
        out.buildType    = bres.buildType;
        for (const auto& f : g_activeFlags) out.activeFlags.push_back(f);
        lspCollectSelectGroups(bres, out.groups);   // a broken manifest still gets a working picker
        return false;
    }

    out.manifest      = res.manifest;
    out.localManifest = res.localManifest;
    out.targetName    = g_target.name;
    out.targetTriple  = g_target.triple();
    out.buildType     = res.buildType;
    out.strict        = g_strictFlags;
    for (const auto& f : g_activeFlags) out.activeFlags.push_back(f);   // a std::set: already sorted
    lspCollectSelectGroups(res, out.groups);
    return true;
}

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

    // DECLARED beats inferred. If an ancestor manifest explicitly OWNS this file — via `projects` and/or
    // `sources`, expanded recursively — then widening to it is not a guess and needs no editor boundary to
    // license it. Prefer the outermost such manifest (the top of a nest of monorepos), and require that its
    // expansion actually CONTAINS the open file, so an ancestor that happens to declare unrelated members
    // is not mistaken for this file's owner.
    std::string self = absolutePath(openFilePath);
    for (auto it = manifests.rbegin(); it != manifests.rend() && p.root.empty(); ++it) {
        std::vector<std::string> srcs, subs;
        std::string e;
        loadManifestSources(*it + "/kama.json", srcs, e);
        loadManifestProjects(*it + "/kama.json", subs, e);
        if (srcs.empty() && subs.empty()) continue;             // declares nothing: not an owner
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
    // declaring `sources` (which files are mine) and/or `projects` (which sub-projects I compose).
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
    if (!loadProgramUnits(files, argv0, units, paths, /*includeDevDeps*/ false, /*strictImports*/ false) || units.empty())
        return nullptr;

    // `units` and `paths` are PARALLEL, so the scan must be bounded by `paths` and the two must grow
    // together. Bounding by units.size() and pushing only a unit read past the end of `paths` on the next
    // overlay — a real out-of-bounds read whenever two buffers are open and one sits outside the project
    // set. It went unseen because a plain build just reads adjacent memory; ASan aborts the server.
    for (const auto& lv : live) {
        bool swapped = false;
        for (size_t i = 0; i < paths.size(); ++i)
            if (paths[i] == lv.first) { units[i] = lv.second; swapped = true; break; }
        if (!swapped) { units.push_back(lv.second); paths.push_back(lv.first); }   // open, outside the project
    }

    auto emitter = std::make_shared<CEmitter>(files.front());
    configureEmitter(*emitter);   // M6 A1: refs/rename must span the same decls the build compiles
    { Stopwatch sw(&timing().analyze); emitter->analyze(units); }
    auto h = std::make_shared<LspIndex>();
    h->idx  = emitter;
    h->path = files.front();
    timingDump("workspace", files.front());
    return h;
}

std::vector<SymbolInfo> lspWorkspaceSymbols(const SharedLspIndex& idx, const std::string& query,
                                            const std::vector<std::string>& files)
{
    static const size_t kMaxResults = 200;   // a picker wants the first screenful, not the whole project
    if (!idx || !idx->idx || files.empty()) return {};
    std::set<std::string> own;
    for (const auto& f : files) own.insert(absolutePath(f));
    std::vector<SymbolInfo> out;
    for (auto& s : idx->idx->workspaceSymbols(query)) {
        if (out.size() >= kMaxResults) break;
        if (!own.count(absolutePath(s.uri))) continue;   // std, a dependency, or otherwise not ours
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

std::vector<Location> lspRenameDeclarations(const SharedLspIndex& idx, const std::string& path,
                                            int line, int col)
{
    if (!idx || !idx->idx) return {};
    return idx->idx->declarationsAt(path, line, col);
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

std::vector<SemanticToken> lspSemanticTokens(const SharedLspIndex& idx, const std::string& path)
{
    if (!idx || !idx->idx) return {};
    return idx->idx->semanticTokensFor(path);
}

std::vector<CompletionItem> lspCompletion(const SharedLspIndex& idx, const std::string& path,
                                          const CompletionContext& ctx)
{
    // Capped here rather than in the facade, for the same reason lspWorkspaceSymbols is: how much to send
    // an editor is the seam's policy, not the index's. A bare position in a stdlib-heavy file legitimately
    // has a couple of hundred names in scope.
    static const size_t kMaxItems = 200;
    if (!idx || !idx->idx) return {};
    auto items = idx->idx->completionsAt(path, ctx);
    if (items.size() > kMaxItems) items.resize(kMaxItems);
    return items;
}

SignatureHelp lspSignatureHelp(const SharedLspIndex& idx, const std::string& path,
                               const CompletionContext& ctx)
{
    if (!idx || !idx->idx) return SignatureHelp{};
    return idx->idx->signatureAt(path, ctx);
}

// The import roots visible from `fromPath`, in loadProgramUnits' order. `reserved` mirrors its rule that
// `std`/`core` resolve ONLY under the stdlib, so a local directory named `std` cannot shadow it.
static std::vector<std::string> lspImportRoots(const std::string& fromPath, bool reserved, const char* argv0)
{
    std::vector<std::string> roots;
    if (!reserved) {
        roots.push_back(dirName(absolutePath(fromPath)));
        for (auto& r : splitSearchPath(getenv("KAMA_PATH"))) roots.push_back(r);
        std::string dv = projectDepsView({ fromPath });
        if (!dv.empty()) roots.push_back(dv);
    }
    roots.push_back(resolveStdlibDir(argv0));
    return roots;
}

static std::vector<std::string> lspSplitModulePath(const std::string& path, std::string& rel)
{
    std::vector<std::string> segs;
    for (size_t i = 0; i < path.size();) {
        size_t sep = path.find("::", i);
        std::string seg = path.substr(i, sep == std::string::npos ? std::string::npos : sep - i);
        if (!seg.empty()) segs.push_back(seg);
        if (sep == std::string::npos) break;
        i = sep + 2;
    }
    rel.clear();
    for (size_t i = 0; i < segs.size(); ++i) rel += (i ? "/" : "") + segs[i];
    return segs;
}

std::vector<std::string> lspImportModules(const std::string& fromPath, const std::string& prefix,
                                          const char* argv0)
{
    std::string rel;
    std::vector<std::string> segs = lspSplitModulePath(prefix, rel);
    std::vector<std::string> roots = lspImportRoots(fromPath, !segs.empty() && (segs[0] == "std" || segs[0] == "core"), argv0);
    const std::string self = stripExtension(baseName(absolutePath(fromPath)));
    std::set<std::string> out;                       // sorted + deduped across roots
    for (auto& root : roots) {
        std::string dir = rel.empty() ? root : root + "/" + rel;
        if (!dirExists(dir)) continue;
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || n == "build") continue;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".kama") == 0) {
                std::string m = n.substr(0, n.size() - 5);
                if (!(rel.empty() && m == self)) out.insert(m);   // a file cannot import itself
            } else if (dirExists(dir + "/" + n)) {
                out.insert(n);                                    // a directory-module or a deeper level
            }
        }
        closedir(d);
    }
    return { out.begin(), out.end() };
}

std::vector<std::string> lspImportSymbols(const std::string& fromPath, const std::string& modulePath,
                                          const char* argv0)
{
    std::string rel;
    std::vector<std::string> segs = lspSplitModulePath(modulePath, rel);
    if (segs.empty()) return {};
    auto files = resolveModuleFiles(segs, lspImportRoots(fromPath, segs[0] == "std" || segs[0] == "core", argv0));
    // Parse the module rather than reading the index: the module being imported is, by definition, one the
    // open file does not import yet, so it is not in the index at all. Only reachable from inside an
    // `import …::{ }` list, so the parse is per-gesture, not per-keystroke.
    std::set<std::string> out;
    for (auto& f : files) {
        SharedCompilationUnit u = parseFile(f);
        if (!u || !u->exportList) continue;           // no `export { … }` => no public surface to offer
        for (auto& n : *u->exportList) if (n) out.insert(*n);
    }
    return { out.begin(), out.end() };
}

// ------------------------------------------------------------------------------------------------
// One question asked of a `kama query` index.
//
// Analyzing a program costs ~210ms; answering a question off the finished index costs 0.03-1.33ms — a
// thousandfold difference. Every mode used to be its own scalar (`queryDef`, `queryType`, …), which is
// precisely why one process could answer only ONE: there was nowhere to put a second, and no record of
// the order they were asked in. `--def 1:1 --type 2:2` silently answered whichever came first in the
// dispatch chain and dropped the rest. An ordered list makes N questions per analysis representable,
// and ARGV ORDER is the answer order — the only rule that makes such a command line well-defined.
enum class QMode { Symbols, Search, Diags, Def, Type, Refs, Coverage, Complete, SigHelp };
struct Question { QMode mode; std::string arg; };

// The flag that spells a mode, and whether it carries a value. `--search ""` is legal (an empty needle
// lists everything), so "takes an argument" is a property of the MODE, never of the argument's emptiness.
static const char* qFlag(QMode m)
{
    switch (m) {
        case QMode::Symbols:  return "--symbols";
        case QMode::Search:   return "--search";
        case QMode::Diags:    return "--diagnostics";
        case QMode::Def:      return "--def";
        case QMode::Type:     return "--type";
        case QMode::Refs:     return "--refs";
        case QMode::Coverage: return "--coverage";
        case QMode::Complete: return "--complete";
        case QMode::SigHelp:  return "--sighelp";
    }
    return "";
}
// Takes a CURSOR (`L:C`). Distinct from qTakesArg: --search also carries a value, but a name, not a
// position — so it is the one arg-taking mode with nothing to parse or validate as coordinates.
static bool qTakesPos(QMode m)
{
    return m == QMode::Def || m == QMode::Type || m == QMode::Refs ||
           m == QMode::Complete || m == QMode::SigHelp;
}
static bool qTakesArg(QMode m) { return m == QMode::Search || qTakesPos(m); }
// A question as the caller spelled it. One string, used for BOTH the text delimiter line and the JSON
// `ask` key, so an answer is addressed the same way whichever form you asked in.
static std::string qSpell(const Question& q)
{
    std::string s = qFlag(q.mode);
    if (qTakesArg(q.mode)) s += " " + q.arg;
    return s;
}
// Do the position modes need the file's raw text? (--coverage/--complete/--sighelp read the SOURCE, not
// the index — see their arms.) Read it once for a whole batch rather than once per question.
static bool qNeedsText(QMode m)
{
    return m == QMode::Coverage || m == QMode::Complete || m == QMode::SigHelp;
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

    if (subcommand == "agents") {
        // An early-return command: it touches no .kama source, so it returns before the shared
        // input/flag handling. It is deliberately NOT in maybeReExec's run-in-place list — the
        // guidance is version-specific, so a pinned project should get its pinned toolchain's copy.
        std::string verb = argc > 2 && argv[2][0] != '-' ? argv[2] : "";
        bool wantClaude = false, allTools = false, skill = false, force = false;
        std::string dir;
        std::vector<std::string> tools;
        for (int i = verb.empty() ? 2 : 3; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--claude")                 wantClaude = true;
            else if (a == "--all-tools")              allTools = true;
            else if (a == "--skill")                  skill = true;
            else if (a == "--force")                  force = true;
            else if (a == "--tool" && i + 1 < argc)   tools.push_back(argv[++i]);
            else if (!a.empty() && a[0] == '-') {
                fprintf(stderr, "kama agents: unknown option '%s'\n", a.c_str()); agentsUsage(); return 2;
            }
            else if (dir.empty())                     dir = a;
            else { fprintf(stderr, "kama agents: unexpected arg '%s'\n", a.c_str()); return 2; }
        }
        if (verb.empty() || verb == "list") {
            if (verb.empty() && (wantClaude || allTools || skill || force || !tools.empty() || !dir.empty())) {
                fprintf(stderr, "kama agents: say `install` or `print`\n"); agentsUsage(); return 2;
            }
            return verb == "list" ? cmdAgentsList() : (agentsUsage(), 2);
        }
        if (verb == "print") {
            // The primitive the whole feature rests on: stdout, so it composes with anything —
            // pasting into a file this command has never heard of, or diffing against one.
            printf("%s", agentBody(skill ? KAMA_AGENTS_SKILL : KAMA_AGENTS_MD));
            return 0;
        }
        if (verb != "install") {
            fprintf(stderr, "kama agents: unknown command '%s'\n", verb.c_str()); agentsUsage(); return 2;
        }
        if (wantClaude) tools.push_back("claude");
        if (allTools) {
            tools.clear();
            for (int i = 0; i < KAMA_AGENT_STUB_COUNT; ++i) tools.push_back(KAMA_AGENT_STUBS[i].name);
        }
        // Resolve every tool name BEFORE writing anything: a typo used to be caught halfway through,
        // after AGENTS.md had already landed, leaving a half-installed project behind an exit 2.
        std::vector<const KamaAgentStub*> chosen;
        for (const auto& t : tools) {
            const KamaAgentStub* found = nullptr;
            for (int i = 0; i < KAMA_AGENT_STUB_COUNT; ++i)
                if (t == KAMA_AGENT_STUBS[i].name) { found = &KAMA_AGENT_STUBS[i]; break; }
            if (!found) {
                fprintf(stderr, "kama agents: unknown tool '%s' — `kama agents list` shows them all\n",
                        t.c_str());
                return 2;
            }
            chosen.push_back(found);
        }
        int written = 0;
        if (!agentsWrite(dir, "AGENTS.md", KAMA_AGENTS_MD, force, written)) return 1;
        for (const auto* s : chosen)
            if (!agentsWrite(dir, s->dest, s->src, force, written)) return 1;
        if (skill && !agentsWrite(dir, ".claude/skills/kama/SKILL.md", KAMA_AGENTS_SKILL, force, written))
            return 1;
        printf("kama agents: %d file%s written. AGENTS.md holds the content; the rest point at it.\n",
               written, written == 1 ? "" : "s");
        return 0;
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
    std::string target     = "HOST";      // a catalog/manifest NAME, or an <arch>-<os>-<abi> triple
    bool targetExplicit    = false;        // was --target passed? (a manifest `"default": true` must lose to it)
    std::vector<std::string> links;        // -l libraries (FFI)
    bool        emitLines  = true;
    bool        keepC      = false;
    bool        webgpu     = false;
    bool        release    = false;        // debug by default
    bool releaseExplicit   = false;        // was --release/--debug passed? (sugar must not beat a manifest default)
    bool        shared     = false;        // --shared: build a native .so/.dylib/.dll (expose entry points)
    std::vector<std::string> defines;      // --define NAME: activate a `@compileFor` flag (repeatable)
    std::vector<std::string> selects;      // --select GROUP=VALUE: pick a single-select group (repeatable)
    std::vector<std::string> undefines;    // --undefine NAME: deactivate a default flag (repeatable)
    std::string configPath;                // --config PATH: explicit kama.json (else auto-discovered)
    bool        devBuild   = false;        // --dev: also put .kama/dev-deps on the import path (dev-dependencies)
    bool        eachMode   = false;        // `kama check --each`: every input is its own program, one process
    int         buildJobs  = 0;            // -j/--jobs: concurrent C compiles; 0 => resolve from env/cores
    // Every `kama query` mode, in the order the caller asked. One list rather than a scalar per mode, so
    // a single analysis can answer N questions (see `Question` above).
    std::vector<Question> questions;
    bool        queryProject = false;      // `kama query --project`: index the whole project, not one closure
    bool        jsonOut = false;           // --json: structured output for `query` and `check`
    const bool  runMode    = (subcommand == "run");   // `kama run`: build to a temp binary, exec it, forward exit
    std::vector<std::string> progArgs;     // args after `--`, forwarded to the run child (run-only)

    // Options may appear in any order, before or after the input file.
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { for (++i; i < argc; ++i) progArgs.push_back(argv[i]); break; }   // rest are program args
        else if ((a == "-o" || a == "--output") && i + 1 < argc) output = argv[++i];
        // How many C compiles may run at once. Every option in this CLI has a long form; a short form
        // exists only where the convention is universal enough that its absence would surprise (`-o`,
        // `-v`, `-j`). `0`/negative/non-numeric is a mistake worth naming, not rounding to 1.
        else if ((a == "-j" || a == "--jobs") && i + 1 < argc) {
            char* end = nullptr;
            long v = strtol(argv[++i], &end, 10);
            if (!end || *end || v < 1 || v > 1024) {
                fprintf(stderr, "kama: %s expects a positive job count (got '%s')\n", a.c_str(), argv[i]);
                usage(); return 2;
            }
            buildJobs = (int)v;
        }
        else if (a == "--cc" && i + 1 < argc)     cc = argv[++i];
        else if (a == "--link" && i + 1 < argc)   links.push_back(argv[++i]);
        else if (a == "--target" && i + 1 < argc) { target = argv[++i]; targetExplicit = true; }
        else if (a == "--no-line")                emitLines = false;
        else if (a == "--keep-c")                 keepC = true;
        else if (a == "--webgpu")                 webgpu = true;
        else if (a == "--shared")                 shared = true;
        else if (a == "--no-heap")                g_noHeap = true;   // reject heap allocation program-wide (MCU step 5)
        else if (a == "--release")              { release = true;  releaseExplicit = true; }
        else if (a == "--debug")                { release = false; releaseExplicit = true; }
        else if (a == "--select" && i + 1 < argc)   selects.push_back(argv[++i]);    // GROUP=VALUE
        else if (a == "--define" && i + 1 < argc)   defines.push_back(argv[++i]);    // `@compileFor` flag on
        else if (a == "--undefine" && i + 1 < argc) undefines.push_back(argv[++i]);  // `@compileFor` flag off
        else if (a == "--config" && i + 1 < argc)   configPath = argv[++i];          // explicit kama.json
        else if (a == "--dev")                      devBuild = true;                 // also resolve dev-dependencies
        else if (a == "--each")                     eachMode = true;                 // check: one program per input
        // `kama query` modes. Appended in ARGV ORDER, and repeatable: `--def 1:1 --def 9:9` is two
        // questions, not last-wins. (Before this was a list, the second silently vanished.)
        else if (a == "--symbols")                  questions.push_back({QMode::Symbols,  ""});
        else if (a == "--coverage")                 questions.push_back({QMode::Coverage, ""});
        else if (a == "--diagnostics")              questions.push_back({QMode::Diags,    ""});
        else if (a == "--def" && i + 1 < argc)      questions.push_back({QMode::Def,      argv[++i]});
        else if (a == "--type" && i + 1 < argc)     questions.push_back({QMode::Type,     argv[++i]});
        else if (a == "--refs" && i + 1 < argc)     questions.push_back({QMode::Refs,     argv[++i]});
        else if (a == "--complete" && i + 1 < argc) questions.push_back({QMode::Complete, argv[++i]});
        else if (a == "--sighelp" && i + 1 < argc)  questions.push_back({QMode::SigHelp,  argv[++i]});
        else if (a == "--search" && i + 1 < argc)   questions.push_back({QMode::Search,   argv[++i]});
        else if (a == "--project")                  queryProject = true;             // `kama query` workspace scope
        else if (a == "--json")                     jsonOut = true;                  // structured output
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "kama: unknown option '%s'\n", a.c_str()); usage(); return 2;
        }
        else                                      inputs.push_back(a);
    }

    // `-- <args>` are forwarded to the program `kama run` launches; they mean nothing to build/transpile.
    if (!runMode && !progArgs.empty()) {
        fprintf(stderr, "kama: `-- <args>` is only meaningful for `kama run`\n"); usage(); return 2;
    }

    // `--each` reinterprets the input list: N programs rather than one program's N files. Only `check` has
    // a meaning for that — `build --each` would need N outputs, which is a different (unbuilt) feature, and
    // silently building only the first input is the failure mode worth ruling out.
    if (eachMode && subcommand != "check") {
        fprintf(stderr, "kama: --each is only meaningful for `kama check` (it checks each input as its own "
                        "program)\n"); usage(); return 2;
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

    // Resolve the build configuration: the manifest's declared flag universe + `select` groups, the
    // target triple and its derived flags, BUILD_TYPE, and the user `--define` set. All of it lives in
    // `resolveBuildConfig` rather than inline here, because `kama lsp` returns ~185 lines above this
    // point and so could never reach it — which is exactly why the server used to analyze with an empty
    // `@compileFor` set (M6 A1). DISCOVERY stays here: the CLI looks next to the input file then in CWD,
    // while the editor walks up from the open buffer bounded by its workspace folder.
    BuildConfigRequest bcReq;
    bcReq.manifest = configPath;
    if (bcReq.manifest.empty()) {
        std::string dir; size_t slash = input.find_last_of('/');
        if (slash != std::string::npos) dir = input.substr(0, slash + 1);
        if      (std::ifstream(dir + "kama.json").good()) bcReq.manifest = dir + "kama.json";
        else if (std::ifstream("kama.json").good())       bcReq.manifest = "kama.json";
    }
    bcReq.target          = target;
    bcReq.targetExplicit  = targetExplicit;
    bcReq.selects         = selects;
    bcReq.defines         = defines;
    bcReq.undefines       = undefines;
    bcReq.release         = release;
    bcReq.releaseExplicit = releaseExplicit;

    BuildConfigResult bcfg;
    {
        std::string cerr;
        if (!resolveBuildConfig(bcReq, bcfg, cerr)) { fprintf(stderr, "kama: %s\n", cerr.c_str()); return 2; }
    }
    release = bcfg.release;

    // Package deps (M2): if the manifest declares dependencies, the resolved view must already be
    // materialized. The build is a pure, reproducible READ of the view — it never fetches — so a missing
    // view is a user error pointing at `kama pkg install`, not a silent build. Under `--dev` the
    // dev-dependency view must exist too (else the dev build silently misses them).
    //
    // A BUILD precondition, deliberately NOT part of `resolveBuildConfig`: an editor in a freshly cloned
    // repo still owes you diagnostics on what it can see, so it must not inherit this hard failure.
    if (!bcfg.manifest.empty()) {
        std::string mdir = dirName(bcfg.manifest);
        std::map<std::string, DepSpec> deps, devDeps; std::string derr;
        if (loadManifestDeps(bcfg.manifest, deps, derr, &devDeps)) {
            if (!deps.empty() && !dirExists(mdir + "/.kama/deps")) {
                fprintf(stderr, "kama: %s declares dependencies but %s/.kama/deps is missing — run `kama pkg install`\n",
                        bcfg.manifest.c_str(), mdir.c_str());
                return 2;
            }
            if (devBuild && !devDeps.empty() && !dirExists(mdir + "/.kama/dev-deps")) {
                fprintf(stderr, "kama: %s declares dev-dependencies but %s/.kama/dev-deps is missing — run `kama pkg install`\n",
                        bcfg.manifest.c_str(), mdir.c_str());
                return 2;
            }
        }
    }

    // Everything downstream keys off the RESOLVED target rather than the spelling the user typed:
    //   wasm     — the wasm backend (emcc, an .html/.js/.wasm harness)
    //   embedded — bare metal: no OS, hence freestanding (`-ffreestanding -nostdlib`) and stopping at an
    //              OBJECT (.o). The board-specific link (crt0/startup + linker script / memory map) is
    //              inherently per-chip and stays the user's step, exactly as every bare-metal toolchain
    //              separates compilation from the linker-script'd final image.
    const bool wasm     = g_target.isWasm();
    const bool embedded = !g_target.hosted();

    // `kama run` builds an executable and execs it, so it needs a hosted target it can actually run:
    // wasm needs node/a browser, and bare metal emits a freestanding object with no runnable entry.
    if (runMode && (wasm || embedded)) {
        fprintf(stderr, "kama run is native-only (wasm needs node/a browser; a bare-metal target emits a "
                        "freestanding object) — use `kama build --target %s`\n", target.c_str());
        return 2;
    }

    // Resolve the OUTPUT axis. `--shared` is sugar for `--select OUTPUT=SHARED`; the default depends on
    // the target, since a bare-metal build has no entry point to link and stops at an object.
    std::string outputKind = g_activeFlags.count("SHARED")  ? "SHARED"
                           : g_activeFlags.count("STATIC")  ? "STATIC"
                           : g_activeFlags.count("OBJECT")  ? "OBJECT"
                           : g_activeFlags.count("EXE")     ? "EXE"
                           : embedded ? "OBJECT" : "EXE";
    if (shared) outputKind = "SHARED";
    const bool outObject = (outputKind == "OBJECT");
    const bool outStatic = (outputKind == "STATIC");
    const bool outShared = (outputKind == "SHARED");
    // Anything that is not a finished executable stops at `-c`; only EXE and SHARED reach the linker.
    const bool stopsAtObject = outObject || outStatic;

    // On wasm the host re-instantiates the module rather than `dlopen`ing it, so `expose` alone
    // (KAMA_EXPORT -> EMSCRIPTEN_KEEPALIVE) covers the web boundary — a shared library is meaningless.
    if (outShared && wasm) {
        fprintf(stderr, "kama: OUTPUT=SHARED is native-only; a wasm build exports `expose`d functions "
                        "directly (no --shared needed)\n");
        return 2;
    }
    if ((outShared || outputKind == "EXE") && embedded) {
        fprintf(stderr, "kama: OUTPUT=%s needs an OS; a bare-metal target (%s) emits an object or a static "
                        "library you link into your firmware image\n",
                outputKind.c_str(), g_target.triple().c_str());
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
        //
        // One program per process by default. `--each` instead treats every input as its OWN program and
        // runs them all here — see the loop below.
        auto checkOne = [&](const std::string& src) -> int {
            std::vector<SharedCompilationUnit> units;
            std::vector<std::string> unitPaths;
            const std::vector<std::string> roots = eachMode ? std::vector<std::string>{ src } : inputs;
            if (!loadProgramUnits(roots, argv[0], units, unitPaths, devBuild)) return 1;

            CEmitter idx(src);                   // analysis mode: no output stream
            configureEmitter(idx);
            { Stopwatch sw(&timing().analyze); idx.analyze(units); }
            timingDump("check", src);
            const auto& diags = idx.diagnostics();
            // Only an ERROR fails the check. A warning is advice — reporting it is the point, but failing
            // on it would mean a deprecation notice breaks every `kama check` in the tree.
            size_t errs = 0;
            for (const auto& d : diags) if (d.severity == DiagSeverity::Error) ++errs;
            if (jsonOut) {
                // Everything on STDOUT and nothing on stderr, so a caller can read one stream. `ok` is the
                // verdict the exit code carries, restated so a consumer that captured only stdout still has
                // it. The exit code is unchanged — a wrapper script must keep working when --json is added.
                Json j = jsonEnvelope("check", src);
                j.set("ok", errs == 0);
                j.set("units", (int)units.size());
                Json rs = Json::array();
                for (const auto& d : diags) rs.push(jsonDiagnostic(d));
                j.set("results", rs);
                jsonPrint(j);
                return errs ? 1 : 0;
            }
            for (const auto& d : diags)
                fprintf(stderr, "%s:%d:%d: %s: %s\n",
                        d.file.c_str(), d.line, d.column, diagSeverityName(d.severity), d.message.c_str());
            if (errs) {
                fprintf(stderr, "kama: %s FAILED (%zu error%s)\n",
                        src.c_str(), errs, errs == 1 ? "" : "s");
                return 1;
            }
            fprintf(stderr, "kama: %s OK (%zu unit%s analyzed%s)\n",
                    src.c_str(), units.size(), units.size() == 1 ? "" : "s",
                    diags.empty() ? "" : ", with warnings");
            return 0;
        };

        if (!eachMode) return checkOne(input);

        // `--each`: N independent programs, one process. The point is the FRONT END — the embedded prelude
        // is already parsed once per process, and the parse cache (the same one `kama lsp` uses) makes the
        // shared `std::` import closure parsed once for the whole batch instead of once per program.
        // Measured over the suite's agreement corpus: 909 programs re-parse the prelude 909 times and the
        // ~106 distinct closure units 4,736 times.
        //
        // Sound because each program still gets its OWN CEmitter — every table the emitter builds is a
        // per-emitter member keyed by node pointer, so two programs never see each other's analysis (the
        // argument written out in full at `preludeUnit()` above, which has shared one prelude AST between
        // emitters since M5.1). The one pass that writes THROUGH to a shared AST is
        // CEmitter::pruneInactiveDecls, which is idempotent under a fixed build-flag set — and one process
        // has exactly one, since the flags come from argv and the manifest, not from the input.
        //
        // Verdicts go to STDOUT, one `<rc> <path>` line per input, FLUSHED as each program finishes.
        // Non-`--json` `check` writes nothing to stdout, so this is a free channel — and flushing per file
        // is what makes a crash attributable: the caller re-runs whatever has no verdict line, solo, and
        // gets the signal exactly as it would have without batching. Under `--json` the per-file envelope
        // already carries `ok` and the file, so the stream stays one envelope per line.
        int worst = 0;
        lspSetParseCache(true);
        for (const auto& src : inputs) {
            int rc = checkOne(src);
            if (rc) worst = 1;
            if (!jsonOut) { printf("%d %s\n", rc, src.c_str()); fflush(stdout); }
        }
        return worst;
    }

    if (subcommand == "query") {
        // Debug harness for the LSP query index (M0 T4/T5): runs analyze() and dumps the requested query
        // over the resulting index, in deterministic text. Mirrors `check`'s front-end-as-library setup;
        // the `kama lsp` server (M1) will call the same CEmitter query methods and map them to protocol JSON.
        // Coordinates in and out are 1-based LINE, 0-based COLUMN (kama.query.h).
        //   kama query <file> --symbols      document outline (one `L:C kind name` line per user decl)
        //   kama query <file> --search NAME  find symbols whose name CONTAINS NAME, case-insensitively —
        //                                    the by-name entry point, for a caller that has no cursor. One
        //                                    `path:L:C kind name` line each; an empty NAME lists every
        //                                    symbol in scope. Scope is the file, or the package under
        //                                    --project.
        //   kama query <file> --diagnostics  this file's analysis diagnostics, as `kama check` spells them
        //                                    but on stdout and without a pass/fail exit
        //   kama query <file> --def  L:C     go-to-definition at 1-based line:col
        //   kama query <file> --type L:C     hover (kind + name) at 1-based line:col
        //   kama query <file> --refs L:C     find-references (decl + every use) at 1-based line:col
        //   kama query <file> --complete L:C completion candidates at line:col — a `trigger=… recv=…` header
        //                                    (the LEXICAL context, from the file's raw text) then one
        //                                    `kind<TAB>label<TAB>detail` line per candidate
        //   kama query <file> --sighelp L:C  signature help at line:col — `sig=<label> active=<N>`
        //   kama query <file> --coverage     the reference index's COVERAGE ORACLE: one
        //                                    `L:C <name> <status>` line per identifier the SOURCE spells,
        //                                    so a spelling the index never learned about shows up as `-`
        //                                    instead of waiting for someone to think of it (M6 B3)
        //   kama query <file> --project      widen the unit set from <file>'s import closure to the whole
        //                                    project (M3.5 workspace indexing), so --refs sees files that
        //                                    use <file> without being imported by it
        //
        // MANY QUESTIONS, ONE ANALYSIS. Every mode above is repeatable and freely combinable, answered in
        // the order given: `kama query f.kama --def 12:5 --type 12:5 --refs 12:5` analyzes once and prints
        // three answers. Analysis is ~210ms; an answer off the finished index is 0.03-1.33ms, so asking N
        // questions in one process is ~N× cheaper than N processes. With more than one question each answer
        // is preceded by a `## <flag> <arg>` line (and under --json each record carries an `ask` key), because
        // the text form's magic empties — "no definition", "no type", "no references", "no signature",
        // "no symbols", "no diagnostics" — each sit alone on a line and would otherwise be unattributable.
        // A SINGLE question prints exactly what it always did, byte for byte: no delimiter, no `ask`.
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
        if (!loadProgramUnits(queryInputs, argv[0], units, unitPaths, devBuild, /*strictImports*/ false)) return 1;
        // Every query re-picks the unit by an EXACT name match (CEmitter::unitForUri), and the project
        // enumeration yields absolute paths — so a relative `input` would match nothing. Ask by the same
        // spelling the units were parsed with. (The LSP server is immune: file:// URIs are already absolute.)
        const std::string queryUri = queryProject ? absolutePath(input) : input;

        CEmitter idx(input);
        configureEmitter(idx);
        { Stopwatch sw(&timing().analyze); idx.analyze(units); }

        // `kama query <f> --symbols` is a faithful proxy for one lspAnalyze, so timing it here is what
        // makes the LSP's per-keystroke cost measurable without a JSON-RPC session. RAII pair, dump
        // declared first so it fires last — and it now covers a whole batch, which is the point: the
        // `analyze=` figure is paid once no matter how many questions the `query=` figure covers.
        TimingScope tdump("query", input);
        Stopwatch qw(&timing().query);

        auto parseLC = [](const std::string& s, int& l, int& c) -> bool {
            auto colon = s.find(':');
            if (colon == std::string::npos) return false;
            l = atoi(s.substr(0, colon).c_str());
            c = atoi(s.substr(colon + 1).c_str());
            return true;
        };

        if (questions.empty()) {
            fprintf(stderr, "kama query: pass --symbols, --search NAME, --def L:C, --type L:C, --refs L:C, "
                            "--complete L:C, --sighelp L:C, --diagnostics, or --coverage\n"
                            "            (L:C is 1-based line, 0-based column; add --project to widen the "
                            "scope to the whole package; any of them may be combined or repeated, and are "
                            "answered in order from one analysis)\n");
            return 2;
        }

        // Validate EVERY coordinate before answering ANY question. A typo in the fifth one must not leave
        // four answers already on stdout — a partial batch is worse than no batch, because a caller that
        // checks the exit code has already consumed output that looks complete. Same message, same exit 2
        // as when each arm checked its own.
        for (const auto& q : questions) {
            int l, c;
            if (qTakesPos(q.mode) && !parseLC(q.arg, l, c)) {
                fprintf(stderr, "kama query: %s wants L:C\n", qFlag(q.mode));
                return 2;
            }
        }

        // --coverage/--complete/--sighelp answer from the file's RAW TEXT rather than the index (see their
        // arms for why). Slurped once for the whole batch instead of once per question.
        std::string fileText;
        bool needText = false;
        for (const auto& q : questions) needText = needText || qNeedsText(q.mode);
        if (needText) {
            std::ifstream in(input, std::ios::binary);
            if (!in) { fprintf(stderr, "kama query: cannot read %s\n", input.c_str()); return 1; }
            fileText.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        const bool multi = questions.size() > 1;

        // Answer one question off the built index. Returns the `--json` record; in text mode it prints and
        // the return value goes unused. Both forms stay in the same arm on purpose — they are two spellings
        // of one answer, and separating them is how they drift apart.
        auto answerOne = [&](const Question& q) -> Json {
            // `ask` echoes the question, so a record in a batch says which one it answers. Omitted for a
            // lone question, whose output must stay byte-identical to what this command has always printed.
            auto env = [&](const char* mode) {
                Json j = jsonEnvelope(mode, queryUri);
                if (multi) j.set("ask", qSpell(q));
                return j;
            };
            int l = 0, c = 0;
            if (qTakesPos(q.mode)) parseLC(q.arg, l, c);   // already validated above

            switch (q.mode) {
                case QMode::Symbols: {
                    auto syms = idx.documentSymbols(queryUri);
                    if (jsonOut) {
                        Json j = env("symbols"), rs = Json::array();
                        for (const auto& s : syms) rs.push(jsonSymbol(s, /*withUri*/ false));
                        j.set("results", rs);
                        return j;
                    }
                    for (const auto& s : syms)
                        printf("%d:%d %s %s\n", s.selectionRange.line, s.selectionRange.column,
                               symKindName(s.kind), s.name.c_str());
                    return Json::object();
                }
                case QMode::Search: {
                    // Find a symbol by NAME rather than by cursor. Every other mode wants an L:C, which suits an
                    // editor (it has a caret) and not a caller that only knows what something is called — which
                    // otherwise has to run --symbols, parse it, and come back. Scope is the files asked about: the
                    // named file alone, or the whole package under --project. Same rule as the LSP's workspace
                    // picker (lspWorkspaceSymbols), and the reason the filter is here rather than in the facade is
                    // the same: deciding whether a path is ours needs real-path resolution the index has no
                    // business owning. No result cap — an editor wants a screenful, a script wants all of them.
                    std::set<std::string> own;
                    for (const auto& f : queryInputs) own.insert(absolutePath(f));
                    std::vector<SymbolInfo> hits;
                    for (const auto& s : idx.workspaceSymbols(q.arg))
                        if (own.count(absolutePath(s.uri))) hits.push_back(s);   // else std, a dep, or not ours
                    if (jsonOut) {
                        Json j = env("search"), rs = Json::array();
                        j.set("query", q.arg);
                        for (const auto& s : hits) rs.push(jsonSymbol(s, /*withUri*/ true));
                        j.set("results", rs);
                        return j;
                    }
                    for (const auto& s : hits)
                        printf("%s:%d:%d %s %s\n", s.uri.c_str(), s.selectionRange.line, s.selectionRange.column,
                               symKindName(s.kind), s.name.c_str());
                    if (hits.empty()) printf("no symbols\n");
                    return Json::object();
                }
                case QMode::Diags: {
                    // The same analysis diagnostics `kama check` prints, addressed per file and reusing check's
                    // spelling — but on STDOUT, like every other query mode, where check puts them on stderr; and
                    // exit 0 either way, because `query` reports and `check` judges. NOTE the shared blind spot,
                    // which this mode does not change: analysis resolves names and checks named arguments, but an
                    // expression TYPE mismatch produces no diagnostic here at all — `kama build` catches it, via
                    // the C compiler. See the `check` arm above. Positions follow Diagnostic's own convention
                    // (kama.diagnostic.h), NOT SrcRange's: column 0 means "whole line / unknown".
                    auto ds = idx.diagnosticsFor(queryUri);
                    if (jsonOut) {
                        Json j = env("diagnostics"), rs = Json::array();
                        for (const auto& d : ds) rs.push(jsonDiagnostic(d));
                        j.set("results", rs);
                        return j;
                    }
                    if (ds.empty()) { printf("no diagnostics\n"); return Json::object(); }
                    for (const auto& d : ds)
                        printf("%s:%d:%d: %s: %s\n", d.file.c_str(), d.line, d.column,
                               diagSeverityName(d.severity), d.message.c_str());
                    return Json::object();
                }
                case QMode::Def: {
                    Location loc = idx.definitionAt(queryUri, l, c);
                    if (jsonOut) {
                        Json j = env("def"), rs = Json::array();
                        if (loc.range.line != 0) {
                            Json e = jsonPos(loc.range.line, loc.range.column);
                            e.set("uri", loc.uri);
                            rs.push(e);
                        }
                        j.set("results", rs);       // 0 or 1 element — an array, so "not found" needs no special case
                        return j;
                    }
                    if (loc.range.line == 0) { printf("no definition\n"); return Json::object(); }
                    printf("%s:%d:%d\n", loc.uri.c_str(), loc.range.line, loc.range.column);
                    return Json::object();
                }
                case QMode::Type: {
                    std::string t = idx.typeAtPosition(queryUri, l, c);
                    if (jsonOut) {
                        // The facade hands back one rendered string ("value Point", "method Box.get"). It is NOT
                        // split into kind + name here: that would be this layer guessing at a boundary the facade
                        // did not draw, and a name can contain a space it would get wrong.
                        Json j = env("type"), rs = Json::array();
                        if (!t.empty()) { Json e = Json::object(); e.set("text", t); rs.push(e); }
                        j.set("results", rs);
                        return j;
                    }
                    printf("%s\n", t.empty() ? "no type" : t.c_str());
                    return Json::object();
                }
                case QMode::Refs: {
                    auto refs = idx.referencesAt(queryUri, l, c, /*includeDecl*/ true);
                    if (jsonOut) {
                        Json j = env("refs"), rs = Json::array();
                        for (const auto& r : refs) {
                            Json e = jsonPos(r.range.line, r.range.column);
                            e.set("uri", r.uri);
                            rs.push(e);
                        }
                        j.set("results", rs);
                        return j;
                    }
                    if (refs.empty()) { printf("no references\n"); return Json::object(); }
                    for (const auto& r : refs)
                        printf("%s:%d:%d\n", r.uri.c_str(), r.range.line, r.range.column);
                    return Json::object();
                }
                case QMode::Coverage: {
                    // The identifiers come from the file's RAW TEXT, never from the index — the whole point is to
                    // ask something the index cannot answer about itself. Deterministic source order, one line each.
                    if (jsonOut) {
                        Json j = env("coverage"), rs = Json::array();
                        for (const auto& id : sourceIdentifiers(fileText)) {
                            Json e = jsonPos(id.line, id.column);
                            e.set("name", id.name);
                            e.set("status", idx.coverageAt(queryUri, id.line, id.column));
                            rs.push(e);
                        }
                        j.set("results", rs);
                        return j;
                    }
                    for (const auto& id : sourceIdentifiers(fileText))
                        printf("%d:%d %s %s\n", id.line, id.column, id.name.c_str(),
                               idx.coverageAt(queryUri, id.line, id.column).c_str());
                    return Json::object();
                }
                case QMode::Complete: {
                    // The lexical context comes from the file's RAW TEXT, never from the index — see
                    // completionContextAt. Using the text (rather than the parsed unit) is also what lets this
                    // harness exercise cursor positions mid-token, which is where an editor actually asks.
                    CompletionContext cc = completionContextAt(fileText, l, c);
                    // The import triggers answer from the module resolver, not the index — a module the file does
                    // not import yet is by definition absent from it.
                    struct Row { const char* kind; std::string label, detail; };
                    std::vector<Row> rows;
                    if (cc.trigger == CompletionTrigger::ImportPath) {
                        for (const auto& m : lspImportModules(input, cc.receiver, argv[0]))
                            rows.push_back({"module", m, ""});
                    } else if (cc.trigger == CompletionTrigger::ImportSymbol) {
                        for (const auto& sym : lspImportSymbols(input, cc.receiver, argv[0]))
                            rows.push_back({"type", sym, cc.receiver});
                    } else {
                        for (const auto& it : idx.completionsAt(queryUri, cc))
                            rows.push_back({completionKindName(it.kind), it.label, it.detail});
                    }
                    if (jsonOut) {
                        // The text form's `key=value` header becomes a `context` object: it is not a result, it is
                        // what the cursor was found to be IN, and a caller checking `trigger` should not have to
                        // parse a line above the rows. `filled` stays a list rather than the text form's CSV.
                        Json j = env("complete");
                        Json ctx = Json::object();
                        ctx.set("trigger", completionTriggerName(cc.trigger));
                        ctx.set("receiver", cc.receiver);
                        ctx.set("callee", cc.callee);
                        ctx.set("prefix", cc.prefix);
                        ctx.set("activeParam", cc.activeParam);
                        Json fl = Json::array();
                        for (const auto& f : cc.filled) fl.push(f);
                        ctx.set("filled", fl);
                        j.set("context", ctx);
                        Json rs = Json::array();
                        for (const auto& r : rows) {
                            Json e = Json::object();
                            e.set("kind", r.kind);
                            e.set("label", r.label);
                            e.set("detail", r.detail);
                            rs.push(e);
                        }
                        j.set("results", rs);
                        return j;
                    }
                    std::string filled;
                    for (size_t i = 0; i < cc.filled.size(); ++i) filled += (i ? "," : "") + cc.filled[i];
                    printf("trigger=%s recv=%s callee=%s prefix=%s active=%d filled=%s\n",
                           completionTriggerName(cc.trigger), cc.receiver.c_str(), cc.callee.c_str(),
                           cc.prefix.c_str(), cc.activeParam, filled.c_str());
                    for (const auto& r : rows)
                        printf("%s\t%s\t%s\n", r.kind, r.label.c_str(), r.detail.c_str());
                    return Json::object();
                }
                case QMode::SigHelp: {
                    SignatureHelp h = idx.signatureAt(queryUri, completionContextAt(fileText, l, c));
                    if (jsonOut) {
                        Json j = env("sighelp"), rs = Json::array();
                        if (!h.label.empty()) {
                            Json e = Json::object();
                            e.set("label", h.label);
                            e.set("activeParam", h.activeParam);
                            rs.push(e);
                        }
                        j.set("results", rs);
                        return j;
                    }
                    if (h.label.empty()) { printf("no signature\n"); return Json::object(); }
                    printf("sig=%s active=%d\n", h.label.c_str(), h.activeParam);
                    return Json::object();
                }
            }
            return Json::object();   // unreachable: every QMode is handled above
        };

        Json batch = Json::array();
        for (const auto& q : questions) {
            // The delimiter earns its place only in a batch: it is what makes "no definition" attributable
            // to the question that produced it. One question prints what it always did.
            if (!jsonOut && multi) printf("## %s\n", qSpell(q).c_str());
            Json a = answerOne(q);
            if (jsonOut) {
                if (!multi) return jsonPrint(a);   // byte-identical to the single-question envelope
                batch.push(a);
            }
        }
        if (jsonOut) {
            // A batch is the same envelope one level up: `results` holds the per-question records, each the
            // exact shape that question answers with on its own, so a consumer needs no second parser.
            // `schema` stays 1 — a new `mode` value is an addition, not a change of meaning to a field.
            Json j = jsonEnvelope("batch", queryUri);
            j.set("results", batch);
            return jsonPrint(j);
        }
        return 0;
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
        // ---- Compiler selection + cross-compilation -------------------------------------------
        // kama emits ISO C and shells out, so reaching another platform is "invoke the right C
        // compiler", not "write a backend". Order of operations, and it matters:
        //   1. pick a compiler (explicit > the target's own > emcc for wasm > the host default)
        //   2. decide whether this is really a CROSS build
        //   3. if so and nobody chose a compiler, substitute one that can actually do it
        //   4. hand the triple to the compiler, if it is the kind that takes one

        // (1) Most specific first. `ccIsExplicit` means the user named a toolchain — either on the
        // command line or on the target in kama.json — which is them telling us they have one.
        std::string compiler = cc;
        bool ccIsExplicit = !compiler.empty();
        if (compiler.empty() && !g_target.cc.empty()) { compiler = g_target.cc; ccIsExplicit = true; }
        if (compiler.empty()) {
            if (wasm) {
                const char* env = getenv("EMCC");
                compiler = env ? env : "emcc";
            } else {
                compiler = resolveCCompiler(argv[0]);   // bundled `zig cc` if the install shipped one, else clang
            }
        }

        // (2) "Crossing" means the host compiler genuinely CANNOT do the job — not merely that the
        // triple string differs. A different ARCH always needs a cross compiler. A different OS needs one
        // only for a HOSTED target, where a foreign libc and linker are involved; a freestanding build on
        // the host's own arch (`-ffreestanding -nostdlib -c`, stopping at an object) is something the
        // host clang does perfectly well — which is what has always made bare-metal builds
        // triple-agnostic, and `--no-heap --target embedded` regressed when this was got wrong.
        const bool crossing = !wasm
            && (g_target.arch != hostTarget().arch
                || (g_target.hosted() && g_target.os != hostTarget().os));

        // Does this compiler take the triple as a FLAG? Two shapes, needing opposite treatment:
        //   * multi-target driver — `clang`, `zig cc` — one binary for every target, `-target <triple>`.
        //   * per-target binary — `aarch64-linux-gnu-gcc`, a vendor ARM gcc, an NDK wrapper — the triple
        //     is already in its NAME, and passing `-target` would confuse or break it.
        auto takesTargetFlag = [](const std::string& c) {
            return c.find("clang") != std::string::npos
                || (c.find("zig") != std::string::npos && c.find(" cc") != std::string::npos);
        };
        auto isZig = [](const std::string& c) { return c.find("zig") != std::string::npos; };

        // (3) Nobody chose a toolchain and this is a cross build. The host default was resolved for the
        // HOST: plain clang takes `-target` but has no libc for anywhere else, so using it would produce
        // a confusing missing-header error rather than a binary. zig is the one widely-available compiler
        // that bundles every target's libc (musl / glibc stubs / mingw-w64 / wasi-libc), so it is the
        // only thing we can substitute unprompted and expect to WORK. Probe only when crossing.
        if (crossing && !ccIsExplicit && !isZig(compiler)) {
            static const bool zigOnPath = [] {
#ifdef _WIN32
                return runCmd("zig version >NUL 2>&1") == 0;
#else
                return runCmd("zig version >/dev/null 2>&1") == 0;
#endif
            }();
            if (zigOnPath) compiler = "zig cc";
        }

        // Still nothing that can reach the target: refuse with the routes, rather than letting clang emit
        // a link error that never mentions targets.
        if (crossing && !ccIsExplicit && !isZig(compiler)) {
            fprintf(stderr,
                    "kama: cannot build for %s (%s) — the default C compiler has no libc for it.\n"
                    "  Any of:\n"
                    "    install zig                       (bundles every target's libc; picked up automatically)\n"
                    "    a `cc` on this target in kama.json  (your own cross toolchain, shared with the team;\n"
                    "                                         `sysroot`/`cflags`/`ldflags` go there too)\n"
                    "    --cc \"clang\" + a sysroot         (if you already have the target's headers/libs)\n"
                    "    kama transpile --target %-14s (emit C and build it with someone else's toolchain)\n",
                    g_target.name.c_str(), g_target.triple().c_str(), g_target.name.c_str());
            return 2;
        }

        // (4) Our triple is already Zig's 3-part <arch>-<os>-<abi> form (chosen in part for this), so it
        // passes straight through to either driver.
        std::string crossFlags;   // appended right after the compiler name
        if (crossing && takesTargetFlag(compiler)) crossFlags = " -target " + g_target.triple();

        // Default output: native -> bare exe name (or lib<name>.<so|dylib|dll> for --shared);
        // wasm -> an HTML harness (emcc also emits the .js + .wasm alongside it).
        const char* sharedExt = g_target.sharedLibExt();
        std::string defaultOut = wasm      ? (stripExtension(input) + ".html")
                               : outStatic ? (dirName(input) + "/lib" + baseName(stripExtension(input)) + ".a")
                               : outObject ? (stripExtension(input) + ".o")
                               : outShared ? (stripExtension(input) + sharedExt)
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
            // recovers only part of it). Debug keeps per-module .c for faithful
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
        cmd << compiler << crossFlags << " -std=c11 -Werror=return-type -Werror=uninitialized ";
        // The target's own toolchain settings from kama.json (a sysroot and any extra compile flags).
        if (!g_target.sysroot.empty()) cmd << "--sysroot=\"" << g_target.sysroot << "\" ";
        for (const auto& f : g_target.cflags) cmd << f << " ";
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
        if (outShared) cmd << "-fPIC -shared -fvisibility=hidden ";
        if (release) {
            // Optimized, no debug info, asserts off. Native uses -O3 (max speed — matches Rust's release
            // default); wasm uses -Oz (size — download cost dominates). -ffunction/data-sections +
            // --gc-sections let the linker drop unused (std)library code — the
            // "pay for what you use" pruning lever. Native also strips symbols.
            cmd << (wasm ? "-Oz " : "-O3 ") << "-DNDEBUG -ffunction-sections -fdata-sections ";
            if (!wasm && !stopsAtObject) {   // -Wl,* is link-time; OBJECT/STATIC stop at -c (see below)
                // ld64 spells section GC differently from GNU ld/lld, and treats `-s` as obsolete (it
                // warns on every release link), so the strip flag is for the GNU-style linkers only.
                if (g_target.isMacOS()) cmd << "-Wl,-dead_strip ";
                else                    cmd << "-Wl,--gc-sections -s ";
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
        // selects the freestanding `main`/panic forms in the emitted C + runtime.
        //
        // The board's TRIPLE is now the TARGET axis (`--target thumbv7em-none-eabihf`), and its CPU model
        // goes in that target's `cflags` (`"cflags": ["-mcpu=cortex-m4"]`) — not through `--cc`, which is
        // what this said before the build-configuration campaign. kama itself passes NO -march/-mcpu/
        // -mtune anywhere, so every build targets the architecture's generic baseline unless a target
        // spec says otherwise. A first-class CPU-tuning knob is a recorded follow-on (ROADMAP §10).
        if (embedded)      cmd << "-ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED ";   // no OS: os=none
        if (stopsAtObject) cmd << "-c ";                                                // no link step
        cmd << "-I" << runtimeDir << " -I" << dirName(absolutePath(input)) << " -I. ";
        if (!headerDir.empty()) cmd << "-I" << headerDir << " ";   // the shared generated header
        if (wasm && webgpu) cmd << "--use-port=emdawnwebgpu ";   // emscripten WebGPU port
        // Native --webgpu: find wgpu-native's webgpu.h / wgpu.h. (wasm gets its header from the port above.)
        if (!wasm && webgpu) cmd << "-I\"" << wgpuDir << "/include\" ";
        // The WebGPU platform seam (kama_gpu.h) — its include dir on BOTH targets (web = header-only
        // static-inline; native also compiles kama_gpu.c below). Only when the program externs it.
        if (needsGpu) cmd << "-I\"" << (resolveStdlibDir(argv[0]) + "/std/gpu") << "\" ";
        // GLFW usually lives under a Homebrew prefix the bare compiler doesn't search by default. A
        // nonexistent -I is harmless, so this is safe to key on the target rather than the host.
        if (!wasm && needsGpu && g_target.isMacOS()) cmd << "-I/opt/homebrew/include -I/usr/local/include ";
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
        // ---- The command is three pieces, not one: the compile flags above (`cmd`), the INPUTS
        // (`ccInputs`), and the link tail (`link`). A single invocation is exactly
        // `cmd + inputs + link + -o out`, byte for byte — reassembly is an identity, not a
        // re-derivation, which is what makes the split safe to verify by diffing commands. Splitting
        // it is also what lets N inputs compile as N independent `-c` jobs joined by one link (`-j`).
        //
        // `tok` is the input spelled exactly as it appeared on the old single command line, trailing
        // space included; `obj` is where a `-c` compile of it writes.
        struct CcInput { std::string tok, obj; };
        std::vector<CcInput> ccInputs;
        for (auto& cf : cFiles)
            ccInputs.push_back({ "\"" + cf + "\" ", stripExtension(cf) + ".o" });
        // Native WebGPU seam: the surface TU (Objective-C on macOS — it attaches a CAMetalLayer to the
        // NSWindow). Only when the program externs kama_gpu.h AND targets native (the web seam is
        // header-only static-inline, compiled nowhere).
        //
        // It is an INPUT, not a tail flag, and that is also a fix: it used to be appended to `cmd`
        // unconditionally, so a STATIC build — which compiles each TU on its own — compiled the seam
        // into *every* archive member. Its object goes in genDir; the `stripExtension(cf) + ".o"` rule
        // used for module TUs would write it into the stdlib INSTALL directory.
        if (!wasm && needsGpu) {
            std::string seam = resolveStdlibDir(argv[0]) + "/std/gpu/kama_gpu.c";
            ccInputs.push_back({ g_target.isMacOS() ? "-x objective-c \"" + seam + "\" -x none "
                                                    : "\"" + seam + "\" ",
                                 genDir + "/kama_gpu.o" });
        }

        // ---- The link tail. Every flag from here down is link-time, which is exactly why the sources
        // can move to the end: a compile-only build (`stopsAtObject`) suppresses all of it.
        std::ostringstream link;
        // Native --webgpu: link wgpu-native, with an rpath so the .dylib/.so is found at run time (dev
        // loop — a shipped app would bundle it). The link stays out of the wasm path (emcc port covers it).
        if (!wasm && webgpu && !stopsAtObject) {
            link << "-L\"" << wgpuDir << "/lib\" -lwgpu_native ";
            if (!g_target.isWindows())   // no rpath concept in PE/COFF
                link << "-Wl,-rpath,\"" << absolutePath(wgpuDir) << "/lib\" ";
        }
        // The seam's window/surface libraries (GLFW + platform frameworks). Split from wgpu-native
        // above so a windowless native build (e.g. the link-gate smoke) links only libwgpu_native.
        if (!wasm && needsGpu && !stopsAtObject) {
            if (g_target.isMacOS())
                link << "-L/opt/homebrew/lib -L/usr/local/lib -lglfw "
                        "-framework Cocoa -framework Metal -framework QuartzCore -framework IOKit "
                        "-framework CoreFoundation -framework CoreVideo -lobjc ";
            else if (g_target.isWindows())
                link << "-lglfw3 -lgdi32 -luser32 -ld3dcompiler ";
            else
                link << "-lglfw -lX11 -ldl -lpthread ";
        }
        // Link-time libraries (skipped for --target embedded: it stops at `-c`, so its board link — where
        // the user supplies startup + linker script — owns library selection).
        if (!stopsAtObject) for (auto& lib : links) link << "-l" << lib << " ";   // FFI link flags
        // Pay-for-what-you-use: link libm only when the program pulls in <math.h> (std::math or any libm
        // FFI). Native only — wasm/emscripten bundles libm. (--gc-sections still prunes unused code.)
        if (needsLibm && !wasm && !stopsAtObject) link << "-lm ";
        // Pay-for-what-you-use: wire up threads only when the program uses the isolate seam (std::concurrent's
        // kama_isolate.h / kama_channel.h). Native: link libpthread (harmless on macOS — pthreads live in libc;
        // required on Linux). Wasm: emscripten pthreads = Web Workers over a shared SharedArrayBuffer, so the
        // same pthread_* C compiles unchanged (mutex/cond lower to Atomics.wait). -sPROXY_TO_PTHREAD runs
        // `main` on a dedicated worker so it may block on join/recv (Atomics.wait THROWS on the JS main
        // thread). PTHREAD_POOL_SIZE pre-warms worker slots (KAMA_PTHREAD_POOL, default 0); STRICT=0 lets the
        // pool grow on demand so a `scope` with more children than the pool never stalls — pre-warm is a pure
        // latency knob, not a correctness cap.
        if (needsPthread && !stopsAtObject) {
            if (wasm) {
                const char* pool = getenv("KAMA_PTHREAD_POOL");   // build-time override; unset => 0 (grow on demand)
                link << "-pthread -sPROXY_TO_PTHREAD "
                     << "-sPTHREAD_POOL_SIZE=" << (pool && *pool ? pool : "0") << " "
                     << "-sPTHREAD_POOL_SIZE_STRICT=0 ";
            } else {
                link << "-lpthread ";
            }
            // M6.3: parallel_for's default worker count. KAMA_PARFOR_WORKERS (build-time) pins K for
            // deterministic CI; unset => 0 => the emitted code calls kama_parfor_workers() (hw cores) at
            // runtime. Only a *count* knob — slices are disjoint + joined, so K never changes results.
            //
            // This is a COMPILE flag that used to sit in the link tail (after the sources). Harmless
            // there for one invocation — `-D` is position-independent — but a per-TU `-c` job takes only
            // the compile flags, so it belongs in `cmd` or the TUs would silently lose the default.
            const char* pfw = getenv("KAMA_PARFOR_WORKERS");
            cmd << "-DKAMA_PARFOR_WORKERS_DEFAULT=" << (pfw && *pfw ? pfw : "0") << " ";
        }
        // std::net uses Winsock (kama_os.h). Link ws2_32 when the TARGET is Windows; harmless (and pruned
        // by --gc-sections) for programs that don't open a socket. POSIX sockets need no extra lib.
        // Keying this on the host was the sharpest example of the cross-compilation blocker: a Windows
        // build produced on Linux silently omitted the socket library.
        if (!wasm && !stopsAtObject && g_target.isWindows()) link << "-lws2_32 ";
        // The target's own link flags from kama.json, last so they can override anything above.
        if (!stopsAtObject) for (const auto& f : g_target.ldflags) link << f << " ";

        // ---- How many C compiles may run at once.
        //
        // A C compiler handed N sources in ONE invocation compiles them SERIALLY, so a 32-TU program
        // (anything importing `std` — a directory-module import pulls in every file in the directory)
        // uses one core for ~70% of `kama build`'s wall time. Compiling each TU as its own `-c` job and
        // linking the objects is measurably ~3.2x on the C phase and ~2x on the whole build (httpd, 32
        // TUs, 10-core M-series: 0.93s one-invocation vs 0.27s at -j10 + 0.02s link).
        //
        // Resolution: -j/--jobs > $KAMA_BUILD_JOBS > core count. (NOT $KAMA_JOBS — that name is already
        // the test harness's own fixture-pool width.)
        int nJobs = buildJobs;
        if (nJobs < 1) {
            if (const char* e = getenv("KAMA_BUILD_JOBS")) nJobs = atoi(e);
#if !defined(_WIN32)
            if (nJobs < 1) nJobs = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
            if (nJobs < 1) nJobs = 4;
        }
        // Clamped to 1 — i.e. today's single invocation, byte for byte — when splitting cannot pay:
        //
        //  * nothing to split (one input: an import-free program, or a --release native unity build).
        //  * Windows: no posix_spawn/waitpid pool.
        //  * wasm: emcc's link settings (--use-port, --js-library, -sEXPORTED_RUNTIME_METHODS,
        //    -sEXIT_RUNTIME) are emitted into the COMPILE flags above, so a per-TU `emcc -c` would warn
        //    on every TU; emcc's Python startup also makes per-TU spawning far costlier than clang's.
        //  * `zig cc`: measured, and it is a SIGN FLIP rather than a smaller win. zig has its own
        //    content-addressed object cache, so one invocation over 32 TUs is 4.13s cold but 0.07s warm
        //    and 0.11s after editing one file — it already does incremental rebuilds. Per-TU zig cannot
        //    use that cache (it is bounded by zig's ~0.18s process startup: 0.59s cold AND warm), so
        //    parallelizing would be 7x better cold and 5x WORSE in the edit-rebuild loop, which is the
        //    loop that matters. A bundled install is exactly where `zig cc` comes from.
        if (ccInputs.size() < 2 || wasm || isZig(compiler)) nJobs = 1;
#if defined(_WIN32)
        nJobs = 1;
#endif

        // Compile every input on its own and join the results, rather than handing them all to one
        // invocation. STATIC has always done this (an archive has no other shape); `-j` widens it to
        // executables and shared libraries, where the join is a link rather than an `ar`.
        const bool perTU = outStatic || nJobs > 1;

        int rc;
        if (perTU) {
            std::string base = cmd.str();
            std::vector<std::string> objs, cmds;
            // A compile-only job needs `-c`, which the flags already carry for OBJECT/STATIC.
            const std::string dashC = stopsAtObject ? "" : "-c ";
            for (auto& in : ccInputs) {
                objs.push_back(in.obj);
                genFiles.push_back(in.obj);
                // Capture each job's streams separately — not `2>&1` — because stream identity matters:
                // `--cc echo` (the measurement instrument) writes to stdout while a compiler writes to
                // stderr. They live beside the object, in the directory that already takes generated files.
                cmds.push_back(base + dashC + in.tok + "-o \"" + in.obj + "\""
                               + " >\"" + in.obj + ".out\" 2>\"" + in.obj + ".err\"");
            }
            std::vector<int> rcs;
            rc = runCmdsParallel(cmds, nJobs, rcs);
            // Replay in INPUT order, whatever order they finished in. Logs are not build artifacts, so
            // they go regardless of --keep-c.
            for (size_t i = 0; i < cmds.size(); ++i) {
                if (rcs[i] < 0) { remove((objs[i] + ".out").c_str()); remove((objs[i] + ".err").c_str()); continue; }
                replayAndRemove(objs[i] + ".out", stdout);
                replayAndRemove(objs[i] + ".err", stderr);
            }
            // The join runs only on a clean wave, so it can never see an object a stopped wave skipped.
            if (rc == 0) {
                if (outStatic) {
                    // A static library is compile-each-TU then archive. `ar` comes from the target spec when
                    // the project declared one, so a cross build archives with the matching binutils rather
                    // than the host's (an ar from another toolchain writes an index the target linker
                    // cannot read).
                    std::ostringstream ar;
                    ar << (g_target.ar.empty() ? std::string("ar") : g_target.ar) << " rcs \"" << outPath << "\"";
                    for (auto& o : objs) ar << " \"" << o << "\"";
                    rc = runCmd(ar.str());
                    if (rc != 0) fprintf(stderr, "kama: ar failed (exit %d)\n", rc);
                } else {
                    // Link the objects with the SAME flag prefix the compiles used, not a bare compiler
                    // name: `--cc "clang -fsanitize=address,undefined"` (the sanitizer leg) needs those
                    // flags on the link too, or the runtime is never pulled in.
                    std::ostringstream ld;
                    ld << base;
                    for (auto& o : objs) ld << "\"" << o << "\" ";
                    ld << link.str() << "-o \"" << outPath << "\"";
                    rc = runCmd(ld.str());
                }
            }
        } else {
            if (outObject && cFiles.size() > 1) {
                fprintf(stderr, "kama: OUTPUT=OBJECT builds a single translation unit, but this program has "
                                "%zu — use OUTPUT=STATIC to get one archive instead\n", cFiles.size());
                return 2;
            }
            for (auto& in : ccInputs) cmd << in.tok;
            cmd << link.str() << "-o \"" << outPath << "\"";
            rc = runCmd(cmd.str());
        }

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
