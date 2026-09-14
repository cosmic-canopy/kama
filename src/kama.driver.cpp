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
#ifdef _WIN32
  // NB: do NOT include <windows.h> here — it is compiled in the same TU as kama.parser.hpp, whose token
  // enum (BOOL, CHAR, CONST, INT8, VOID, …) collides with windows.h typedefs/macros. What needs the real
  // header lives on the other side of kama.winpath.h, in a TU of its own: the `\\?\` long-path spelling,
  // the directory listing, and the `-j` pool's spawn. ⚠️ NOT <dirent.h>: mingw-w64's opendir on a `\\?\`
  // path past MAX_PATH returns a valid DIR* and then lists the CURRENT WORKING DIRECTORY (measured —
  // docs/platforms/windows.md), which for module discovery means silently compiling the wrong files.
  // listDir below goes through FindFirstFileW instead.
  #include "kama.winpath.h"
  #include <stdlib.h>           // _MAX_PATH
  #include <direct.h>           // _mkdir (package view materialization)
  #include <process.h>          // _getpid (staging dir name for the package store)
  #include <io.h>               // _fileno / _setmode — binary stdio, and `kama seed`'s terminal check
  #include <fcntl.h>            // _O_BINARY (a CRT header: it does not reach <windows.h> either)
  // Two Win32 entry points DECLARED rather than included, the same way _NSGetExecutablePath is on macOS
  // below and for the same reason: <windows.h> cannot come into this TU (see the note above), and the CRT
  // has no equivalent. GetConsoleMode is the only honest "is stdin a terminal" test here — _isatty answers
  // "is this fd a CHARACTER DEVICE", and the NUL device is one, so `kama seed </dev/null` read as
  // interactive. HANDLE is void*, DWORD is unsigned long; __stdcall is a no-op on x64/ARM64 and correct on x86.
  extern "C" void* __stdcall GetStdHandle(unsigned long);
  extern "C" int   __stdcall GetConsoleMode(void*, unsigned long*);
  // Three more, for absolutePath's reparse-point resolution — see the note there. GetFullPathName
  // canonicalizes text and never touches the disk, so it cannot see a junction; only a handle to the
  // opened object can be asked where it actually landed. The `A` spellings are UTF-8 under the manifest
  // src/kama.manifest embeds (the process ANSI code page), which is what lets this TU stay narrow.
  extern "C" void*         __stdcall CreateFileA(const char*, unsigned long, unsigned long, void*,
                                                 unsigned long, unsigned long, void*);
  extern "C" unsigned long __stdcall GetFinalPathNameByHandleA(void*, char*, unsigned long, unsigned long);
  extern "C" int           __stdcall CloseHandle(void*);
  #ifndef PATH_MAX
    #define PATH_MAX _MAX_PATH
  #endif
#else
  #include <dirent.h>           // opendir / readdir — listDir's POSIX arm
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
#include "kama.seed.h"      // KAMA_SEED_* project templates, embedded — the `kama seed` command

#ifndef KAMA_VERSION
#define KAMA_VERSION "0.0.0-dev"
#endif

// The null device, spelled the way the SHELL runCmd hands a command to will read it. runCmd goes through
// system(), which is `cmd /c` on Windows, and cmd.exe has no `/dev/null` — it takes it for a path, cannot
// find a `\dev` directory, and prints
//     The system cannot find the path specified.
// The redirection failing means the COMMAND never runs, and the caller then reports its own operation as
// having failed. That is why `kama install` said "git clone failed" for every package dependency on
// Windows: the clone was fine, `2>/dev/null` was not.
#if defined(_WIN32)
  #define KAMA_DEVNULL "NUL"
  #define KAMA_TAR_LOCAL "--force-local "   // see the tar call in fetchToStore
  // "is this program on PATH", for the same shell. `command` is a POSIX shell BUILTIN and cmd.exe has no
  // such thing — the probe did not report "no ssh-keygen", it reported "'command' is not recognized",
  // which the caller read as absent. So `kama publish --key` refused to sign on Windows on machines that
  // had ssh-keygen installed all along.
  #define KAMA_WHICH "where "
  // What the OS calls an executable. See selectorPath()/versionBin().
  #define KAMA_EXE_SUFFIX ".exe"
#else
  #define KAMA_DEVNULL "/dev/null"
  #define KAMA_TAR_LOCAL ""
  #define KAMA_WHICH "command -v "
  #define KAMA_EXE_SUFFIX ""
#endif

namespace {

int runCmd(const std::string& cmd);   // fwd decl (defined below) — used by linkDir on Windows

// The directory for throwaway files, asked the way each platform actually answers.
//
// `TMPDIR` is the POSIX spelling and was once the only one consulted here, with `/tmp` as the fallback.
// Nothing on Windows sets TMPDIR — not the OS, not msys2 — so `kama run` fell through to the literal
// `/tmp`, which a NATIVE tool reads as `\tmp` on the current drive. On the CI runner that drive is D:,
// `D:\tmp` does not exist, and the link died with "cannot open output file". On a dev box `C:\tmp` often
// DOES exist, which is worse: it appeared to work while filling a directory nobody owns. Windows sets
// `TMP`/`TEMP` for every session, and msys2 rewrites both to Win32 paths when it spawns a native child,
// so those are the question to ask. (Not GetTempPathA: <windows.h> is banned in this TU — see the
// include block at the top.) Normalized to '/' for the reason absolutePath() normalizes.
std::string tempDir()
{
    const char* td = getenv("TMPDIR");
#ifdef _WIN32
    if (!(td && *td)) td = getenv("TMP");
    if (!(td && *td)) td = getenv("TEMP");
#endif
    std::string d = (td && *td) ? td : "/tmp";
    for (char& c : d) if (c == '\\') c = '/';
    if (!d.empty() && d.back() == '/') d.pop_back();
    return d;
}

// The spelling a path gets at the moment it is handed to the OS — and ONLY then. Every path this driver
// stores, joins, prints or compares is the `/`-joined UTF-8 spelling absolutePath mints; `osp` is applied
// to the argument of the call (`fopen(osp(p).c_str(), …)`, `std::ifstream in(osp(p))`) and its result is
// never kept. On POSIX it is the identity. On Windows it is kama_win_ospath: under 248 characters the
// path passes through untouched, past that it becomes the absolute, backslash, `\\?\`-prefixed spelling
// the narrow CRT accepts beyond MAX_PATH (kama.winpath.h has the measurements). Encoding needs nothing
// here — the manifest makes the narrow CRT UTF-8 process-wide.
#ifdef _WIN32
static std::string osp(const std::string& p) { return kama_win_ospath(p); }
#else
static const std::string& osp(const std::string& p) { return p; }
#endif

// A path exactly as the SHELL handed it over. msys2 converts a long POSIX argument into the verbatim
// `//?/C:/…` spelling when it spawns a native child, and `cygpath -m` answers the same for one (both
// measured). Left alone, that prefix survives the driver's `/`-joins as `/?/C:/…` once a lexical join
// collapses the doubled slash, and then names nothing. Stripped here at the operand, the path is an
// ordinary long one: osp() re-derives the prefix at the OS edge when the length calls for it, and
// absolutePath strips whatever Win32 answers in. Identity everywhere else and for everything shorter.
static std::string cliPath(const std::string& p)
{
#ifdef _WIN32
    if (p.size() >= 4 && ((p[0] == '/' && p[1] == '/' && p[2] == '?' && p[3] == '/') ||
                          (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\'))) {
        std::string q = p.substr(4);
        if (q.size() >= 4 && (q[0] == 'U' || q[0] == 'u') && (q[1] == 'N' || q[1] == 'n') && (q[2] == 'C' || q[2] == 'c')
            && (q[3] == '/' || q[3] == '\\'))
            q = "//" + q.substr(4);
        return q;
    }
#endif
    return p;
}

// A path for a TOOL's command line — the C compiler's linker and archiver, and cmd.exe's redirections in
// the `-j` pool. On Windows the first two are GNU ld and ar, which ANSI-decode their argv and cannot open
// a non-ASCII path in either direction, and the third stops at MAX_PATH (all measured; clang's own
// compile step has neither limit), so a path with any byte past 0x7F or 248+ characters is spelled by
// its 8.3 alias instead. Everything else passes through byte-for-byte. Never stored: the driver's own
// spelling stays what it was, and this is applied only where the object list, `-o` and the per-job
// `>out 2>err` are written into a command line.
#ifdef _WIN32
static std::string toolPath(const std::string& p) { return kama_win_shortpath(p); }
#else
static const std::string& toolPath(const std::string& p) { return p; }
#endif

std::string absolutePath(const std::string& path)
{
#ifdef _WIN32
    // ⚠️ Win32 answers in BACKSLASHES, and everything else in this driver builds paths by joining with
    // '/' (`dir + "/" + name`, the project enumeration, the module resolver). Mixing the two produces
    // `D:\a\proj/app.kama` from one code path and `D:\a\proj\app.kama` from another for the SAME file — and
    // several comparisons here are exact string equality (CEmitter::unitForUri re-picks a unit by name), so
    // they silently match nothing. That is what made every `kama query --project` answer "no references" on
    // Windows while the same queries passed on Linux and macOS. Normalize to '/' at the one place absolute
    // paths are minted: Win32 and the CRT accept forward slashes everywhere, as do gcc/clang command lines.
    // The fallback below is normalized too: a caller that handed us a backslash spelling of a file that
    // does not exist yet must not be the one path that escapes the convention.
    // ⚠️ And RESOLVE REPARSE POINTS, because resolving links is what the POSIX branch below DOES — this
    // function is realpath(), and the rest of the driver leans on that. `joinPathLexical` exists purely to
    // opt OUT of it; `owningPackageDir` opts IN, and that is load-bearing: a fetched package is reached
    // through the `.kama/deps/<name>` link, and the check that a package's manifest is not the user's to
    // edit is "did I land inside the store?". GetFullPathName is pure text manipulation and never touches the
    // disk, so on Windows — where linkDir materializes the view with `mklink /J`, a JUNCTION — that walk
    // stopped at the view entry, the store test failed, and `kama build` told the user to go edit a
    // kama.json inside the content-addressed store, whose tree hash the edit would invalidate.
    //
    // Only a handle knows. FILE_FLAG_BACKUP_SEMANTICS is what makes CreateFile open a DIRECTORY at all,
    // and access 0 asks for metadata only, so this neither locks the file nor needs rights to read it.
    // A path that does not exist yet cannot be opened — a `-o` output, say — so that falls through to
    // GetFullPathName, mirroring realpath's own failure mode (POSIX falls back to the path as given).
    std::string s;
    void* h = CreateFileA(osp(path).c_str(), 0, 0x7 /* FILE_SHARE_READ|WRITE|DELETE */, nullptr,
                          3 /* OPEN_EXISTING */, 0x02000000 /* FILE_FLAG_BACKUP_SEMANTICS */, nullptr);
    if (h != (void*)-1) {          // INVALID_HANDLE_VALUE
        // 32767 is the NT path ceiling, so a `\\?\` answer always fits and there is no MAX_PATH here.
        char fin[32768];
        // 0 == FILE_NAME_NORMALIZED | VOLUME_NAME_DOS: the long-name spelling on a drive letter, not the
        // \\?\Volume{GUID} form. A return >= the buffer means "needed this much" — a miss, fall through.
        unsigned long n = GetFinalPathNameByHandleA(h, fin, (unsigned long)sizeof fin, 0);
        CloseHandle(h);
        if (n > 0 && n < (unsigned long)sizeof fin) s.assign(fin, n);
    }
    if (s.empty()) { s = kama_win_fullpath(path); if (s.empty()) s = path; }
    // GetFinalPathNameByHandle always answers in the \\?\ extended form. Strip it: everything downstream
    // (and every C compiler command line) wants the ordinary spelling, and a UNC path comes back as
    // \\?\UNC\server\share, whose ordinary spelling is \\server\share. The forward-slash spelling is the
    // one msys2 hands a native child for a long POSIX argument (`//?/C:/…`, measured) — same treatment,
    // or the operand it names "does not exist" to every narrow call that follows.
    if      (s.rfind("\\\\?\\UNC\\", 0) == 0 || s.rfind("//?/UNC/", 0) == 0) s = "\\\\" + s.substr(8);
    else if (s.rfind("\\\\?\\",      0) == 0 || s.rfind("//?/",     0) == 0) s = s.substr(4);
    for (char& c : s) if (c == '\\') c = '/';
    return s;
#else
    char buf[PATH_MAX];
    if (realpath(path.c_str(), buf)) return std::string(buf);
    return path; // fall back to as-given (e.g. file doesn't exist yet)
#endif
}

// The entries of `dir`, without `.` and `..`, in the filesystem's order — every caller that needs
// determinism sorts. Empty when the directory cannot be opened, which no caller distinguishes from empty.
// POSIX: dirent. Windows: FindFirstFileW through kama.winpath — see the include block for why not dirent.
static std::vector<std::string> listDir(const std::string& dir)
{
    std::vector<std::string> out;
#ifdef _WIN32
    kama_win_listdir(dir, out);
#else
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            const char* n = e->d_name;
            if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
            out.push_back(n);
        }
        closedir(d);
    }
#endif
    return out;
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
    std::ifstream f(osp(p).c_str());
    return f.good();
}

// Where kama_runtime.h lives, resolved so an INSTALLED binary finds it from any
// cwd: <exeDir>/../include (bin/kama -> ../include), else <exeDir> (a flat tree
// with the binary beside its headers), else <exeDir>/../../include (dev tree: the
// Makefile builds to out/<os>-<arch>/kama and the runtime headers live in include/),
// else ".". Always exe-relative — so a per-version toolchain at
// ~/.kama/versions/<v>/bin/kama finds *its own* runtime, never an ambient one.
//
// ⚠️ The dev-tree branch is NOT covered by the first one: from out/<platform>/kama,
// <exeDir>/../include resolves to out/include, which does not exist. Both are needed.
// Failure here is silent — the fallback is "." and the error surfaces much later as a
// C compile that cannot find kama_runtime.h — so tools/check-runtime-dir.sh pins it.
std::string resolveRuntimeDir(const char* argv0)
{
    std::string exeDir = dirName(absolutePath(argv0 ? argv0 : "kama"));
    if (fileExists(exeDir + "/../include/kama_runtime.h"))    return exeDir + "/../include";
    if (fileExists(exeDir + "/kama_runtime.h"))               return exeDir;
    if (fileExists(exeDir + "/include/kama_runtime.h"))       return exeDir + "/include";
    if (fileExists(exeDir + "/../../include/kama_runtime.h")) return exeDir + "/../../include";
    return ".";
}

bool dirExists(const std::string& p)
{
    struct stat st;
    return stat(osp(p).c_str(), &st) == 0 && S_ISDIR(st.st_mode);   // POSIX + mingw-w64
}

// The `*.kama` files directly inside `dir`, sorted for deterministic emit order.
std::vector<std::string> listKamaFiles(const std::string& dir)
{
    std::vector<std::string> out;
    auto keep = [&](const std::string& name) {
        return name.size() > 5 && name.compare(name.size() - 5, 5, ".kama") == 0;
    };
    for (const std::string& n : listDir(dir)) if (keep(n)) out.push_back(dir + "/" + n);
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
    for (const std::string& n : listDir(dir)) {
        if (n.empty() || n[0] == '.') continue;          // .git, .kama (the package store) …
        std::string childRel = rel.empty() ? n : rel + "/" + n;
        if (dirExists(dir + "/" + n)) {
            if (n == "out" || n == "build") continue;    // generated C + objects, never sources
                                                         // ("out" is the current convention; "build" predates it)
            collectKamaFiles(root, childRel, out, seen, budget);
            if (seen > budget) break;
        } else if (n.size() > 5 && n.compare(n.size() - 5, 5, ".kama") == 0) {
            if (++seen > budget) break;
            out.push_back(dir + "/" + n);
        }
    }
}

// The first `kama.json` at or below `dir`, or "" if there is none. Projects do not nest: a manifest
// inside another project's source root would be a second project living inside the first, and every
// question about it — whose module is that file in, whose symbol prefix does it get — has two answers.
//
// This needs no exemptions precisely because `source` must be a real subdirectory: the project's own
// manifest, its `.kama/deps` view, its `out/` and any vendored dependency beside `src/` are all
// structurally outside the tree being walked. Skips the same dot-directories and generated roots
// collectKamaFiles does, for the same reasons.
static std::string nestedManifestUnder(const std::string& dir)
{
    std::string found;
    for (const std::string& n : listDir(dir)) {
        if (n.empty() || n[0] == '.') continue;
        const std::string child = dir + "/" + n;
        if (dirExists(child)) {
            if (n == "out" || n == "build") continue;
            found = nestedManifestUnder(child);
        } else if (n == "kama.json") {
            found = child;
        }
        if (!found.empty()) break;
    }
    return found;
}

// The stdlib root, resolved from the binary like resolveRuntimeDir: <exeDir>/../lib/kama
// (installed, bin/kama -> ../lib/kama), else <exeDir>/lib (repo root), else <exeDir>/../../lib
// (dev tree: the Makefile builds to out/<os>-<arch>/kama), else "lib".
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

// This process's own argv[0], for the exe-relative resolvers above. Set once at the top of main, beside
// the other build configuration, because `configureEmitter` — the ONE place every CEmitter is set up —
// is handed an emitter and nothing else, and the answer is a process constant either way.
static const char* g_argv0 = nullptr;

// The text the BINARY compiled for a compiler-owned unit, or null when it carries none. `<prelude>` and
// the triad are embedded (tools/embed_prelude.sh); `<builtin>` is a documentation file read off disk, so
// it IS its own source and has nothing to be checked against.
static const char* embeddedSourceOf(const std::string& unitName)
{
    if (unitName == "<prelude>") return KAMA_PRELUDE_SRC;
    for (int i = 0; i < KAMA_PRELUDE_MODULE_COUNT; ++i)
        if (unitName == KAMA_PRELUDE_MODULES[i].name) return KAMA_PRELUDE_MODULES[i].src;
    return nullptr;
}

// Does the file on disk still say what the binary compiled? Compared with `\r` dropped, so a CRLF checkout
// matches rather than mismatching spuriously — and spurious MISMATCH is the safe direction anyway: it
// costs a path, while a spurious match is the whole fault this exists to prevent.
static bool fileMatchesEmbedded(const std::string& path, const char* embedded)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) return false;
    std::string disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string src(embedded);
    disk.erase(std::remove(disk.begin(), disk.end(), '\r'), disk.end());
    src.erase(std::remove(src.begin(), src.end(), '\r'), src.end());
    return disk == src;
}

// Where a compiler-owned source ACTUALLY lives on disk, given the synthetic name it carries inside the
// binary (`<prelude>`, `<prelude>/std/memory/owned.kama`). Empty when there is no such file — or when the
// file is no longer the one that was compiled.
//
// ⚠️ A PATH HERE IS A CLAIM ABOUT THE BINARY'S OWN SOURCE, so it is checked against it. The prelude and
// the triad are compiled from text EMBEDDED at build time, while this resolves a file on disk beside the
// install — two things that drift the moment either moves, and a stale binary against an edited prelude
// is the everyday case in this repo. Measured before the check: insert five lines at the top of a copy's
// `prelude/global.kama` without rebuilding, and go-to-definition on `Optional` still answered line 8,
// where line 8 had become a comment and `Optional` had moved to 13. Every language that precompiles its
// standard library makes this mismatch DETECTABLE — Rust's `/rustc/<hash>/…` resolves only against the
// matching `rust-src`, the JVM reports "source does not match the bytecode" — and the ones that compile
// it from disk (Go, Zig, C) cannot have the problem at all. kama needs neither a hash nor a version pin,
// because the text it compiled is right here: compare, and hand back nothing on a mismatch. Degrade,
// never lie — every caller already treats "" as "no file on disk (a --no-std install)".
//
// ⚠️ The synthetic name is NOT replaced by this, and must not be. The leading `<` is a sentinel four
// passes read: `checkReach` exempts compiler-owned declarations from the export/import rungs on it,
// `CEmitter::line` suppresses a `#line` into a file that may not exist, `setPackageResolver` skips the
// filesystem walk for such a unit, and `moduleOfUnit` returns "" for `<prelude>` specifically — a real
// path there would derive a module name and re-mangle every prelude symbol. So the path travels
// ALONGSIDE the name, for the query layer only, and go-to-definition is the one thing that reads it.
//
// The triad resolves in both trees, since `lib/std/memory/*.kama` ships either way. The global prelude
// resolves beside the stdlib in an install and one level up in this repo, where `lib/` IS the stdlib
// root and `prelude/` is its sibling.
static std::string builtinSourcePath(const std::string& unitName)
{
    // Normalized, because this becomes a `file://` URI an editor opens and shows in a tab. The resolvers
    // are exe-relative and compose `..` freely — a dev tree yields
    // `out/Darwin-arm64/../../lib/../prelude/global.kama`, which opens fine and reads as a bug.
    const std::string root = resolveStdlibDir(g_argv0);
    const char* embedded = embeddedSourceOf(unitName);
    // The file must exist AND still be what was compiled. `<builtin>` carries no embedded text (it is a
    // documentation file, and its own source of truth), so existence is all there is to ask of it.
    auto usable = [&](const std::string& p) {
        return fileExists(p) && (!embedded || fileMatchesEmbedded(p, embedded));
    };
    const std::string pfx = "<prelude>/";
    if (unitName.compare(0, pfx.size(), pfx) == 0) {
        const std::string p = root + "/" + unitName.substr(pfx.size());
        return usable(p) ? lspRealPath(p) : std::string();
    }
    if (unitName == "<prelude>" || unitName == "<builtin>") {
        const std::string base = unitName == "<builtin>" ? "/prelude/builtin.kama" : "/prelude/global.kama";
        for (const std::string& p : { root + base, root + "/.." + base })
            if (usable(p)) return lspRealPath(p);
    }
    return std::string();
}

// ---- the built-in documentation file (prelude/builtin.kama) -----------------------------------------
//
// `int32`, `string`, `isize` and the `string` intrinsics are registered in C++ — they are reserved words,
// not declarations, so unlike the prelude there is no source anywhere to point at. The shape Go and Rust
// both settled on for this is a documentation-only file the tooling aims at (`builtin.go`,
// `primitive_docs.rs`), and this is the scan that finds a name's line in it.
//
// ⚠️ THE FILE'S LAYOUT IS THIS FUNCTION'S CONTRACT: one declaration per line, a type as
// `type <kind> <Name>`, a method as `fn <ret> <name>(` indented inside its type. Reformatting the file
// moves where go-to-definition lands, which is why the file says so at the top.
//
// It reads the FILE rather than carrying a table, deliberately. A hand-written table here plus the file
// would be a THIRD statement of the same truth on top of the two that already exist; this way the
// compiler knows only "the registered names" and "the file", and tools/check-builtin-doc.sh holds those
// two together in both directions.
static const std::map<std::string, SrcRange>& builtinDocIndex()
{
    static std::map<std::string, SrcRange> idx;
    static bool built = false;
    if (built) return idx;
    built = true;
    const std::string path = builtinSourcePath("<builtin>");
    if (path.empty()) return idx;                       // no such file (a --no-std install): no locations
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) return idx;
    auto isIdent = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    std::string line, curType;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        const size_t cmt = line.find("//");
        if (cmt != std::string::npos) line.erase(cmt);   // a trailing comment is not a declaration
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;

        // `type <kind> <Name>` at column 0 — a top-level type, and the enclosing scope for what follows.
        if (i == 0 && line.compare(0, 5, "type ") == 0) {
            size_t k = line.find_first_not_of(" ", 5);                 // the kind word
            if (k == std::string::npos) continue;
            size_t ke = k; while (ke < line.size() && isIdent(line[ke])) ++ke;
            size_t n = line.find_first_not_of(" ", ke);                // the NAME
            if (n == std::string::npos || !isIdent(line[n])) continue;
            size_t ne = n; while (ne < line.size() && isIdent(line[ne])) ++ne;
            curType = line.substr(n, ne - n);
            idx[curType] = SrcRange{ lineNo, (int)n, lineNo, (int)ne };
            continue;
        }
        // `… fn <ret> <name>(` inside the type above. The name is the identifier before the paren, which
        // is the one spelling a return type cannot be confused with however it is written.
        if (curType.empty() || line.find("fn ") == std::string::npos) continue;
        const size_t lp = line.find('(');
        if (lp == std::string::npos || lp == 0) continue;
        size_t ne = lp; while (ne > 0 && isIdent(line[ne - 1])) --ne;
        if (ne == lp) continue;                                        // `(` not preceded by a name
        idx[curType + "." + line.substr(ne, lp - ne)] = SrcRange{ lineNo, (int)ne, lineNo, (int)lp };
    }
    return idx;
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

static bool loadManifestSource(const std::string& path, std::string& out, std::string& err);

// The source root `manifest` declares, cached by path.
//
// ⚠️ THE CONTRACT, and the one distinction the `"src"` default must not destroy:
//
//     ""            -> there is no kama.json at `manifest` (or it will not parse). NOT a package root;
//                      the caller falls back to listing the directory.
//     anything else -> the source root relative to the manifest — `source` as declared, or "src" when
//                      the key is absent. A manifest ALWAYS has one.
//
// The default is therefore applied HERE and only here, because this is the only reader that knows
// whether the file exists. Defaulting inside loadManifestSource would return "src" for a directory with
// no manifest at all, and since that is the dominant case — every ordinary directory-module — `import
// a::b` would start resolving `a/b/src/*.kama` and silently lose `a/b/*.kama`.
//
// A manifest that will not PARSE is deliberately fused with "no manifest": the editor must not stop
// resolving modules over a JSON typo somewhere up the tree. The cost is that a broken manifest silently
// downgrades a package to a plain directory-module, which is the lesser of the two failures.
//
// Why the declaration is cached but never the expanded file list: `kama lsp` is long-lived, and a newly
// added `.kama` file has to become visible without restarting the server. Expanding costs a readdir,
// which is what the flat listing this replaced cost anyway.
static std::map<std::string, std::string>& manifestSourceCache()
{
    static std::map<std::string, std::string> cache;
    return cache;
}

const std::string& manifestSourceCached(const std::string& manifest)
{
    std::map<std::string, std::string>& cache = manifestSourceCache();
    auto it = cache.find(manifest);
    if (it != cache.end()) return it->second;
    std::string src, err;
    if (!fileExists(manifest) || !loadManifestSource(manifest, src, err)) src.clear();
    else if (src.empty()) src = "src";                  // the manifest exists and named none
    return cache.emplace(manifest, std::move(src)).first->second;
}

// The source files of the package rooted at `dir`. Returns false when `dir` is NOT a package root, which
// is the caller's signal to fall back to the flat listing; true with an EMPTY `out` means it is one and
// its source directory simply is not there yet.
//
// This is what lets a dependency use the `src/` layout docs/packages.md teaches: an installed
// `.kama/deps/geo/` holds only `kama.json`, its sources one level down, so a non-recursive listing of it
// finds nothing and the package could not be imported AT ALL.
bool packageSourceFiles(const std::string& dir, std::vector<std::string>& out)
{
    const std::string& rel = manifestSourceCached(dir + "/kama.json");
    if (rel.empty()) return false;                      // not a package root
    // Lexical, not absolutePath: absolutePath is realpath(), which resolves the `.kama/deps/<name>`
    // symlink away — and what this returns becomes the UNIT NAME, which CEmitter::unitForUri matches by
    // exact string equality. Resolve it and go-to-definition lands in the content-addressed store rather
    // than in the deps view the user can see. (It also collapses any `..` in `dir` itself, which the
    // stdlib root carries: <exeDir>/../../lib/std/…)
    std::string sub = joinPathLexical(dir, rel);
    if (dirExists(sub)) { size_t seen = 0; collectKamaFiles(sub, "", out, seen, (size_t)-1); }
    // A source root that does not exist yields no files rather than an error: a manifest may name a
    // directory not created yet, and the build path reports that separately (resolveBuildConfig).
    std::sort(out.begin(), out.end());   // readdir order is not deterministic; emit order must be
    return true;
}

struct DepSpec;   // defined with the package resolver, far below; only the pointer is needed here
static bool loadWorkspace(const std::string& path, std::vector<std::pair<std::string, bool>>& out,
                          std::map<std::string, DepSpec>* depsOut, std::string& err);

// The basename of the workspace file, in one place: several walks and the LSP's watcher all name it.
static const char* const kWorkspaceFile = "kama_workspace.json";

// Expand one `projects` entry against `dir`: a plain member directory, or a trailing "/*" meaning every
// immediate subdirectory that has a manifest. Sorted — readdir order is not deterministic.
//
// A member is a directory holding a `kama.json`, in BOTH forms. The plain form used to check only that
// the directory existed, which let a manifest-less directory be named, expand, and then contribute
// nothing — declared and silently empty, the shape collectPackageTree's missing `else` arm also guards.
std::vector<std::string> expandProjectsEntry(const std::string& dir, const std::string& rel)
{
    std::vector<std::string> out;
    if (rel.size() > 2 && rel.compare(rel.size() - 2, 2, "/*") == 0) {
        std::string parent = dir + "/" + rel.substr(0, rel.size() - 2);
        for (const std::string& n : listDir(parent)) {
            if (n.empty() || n[0] == '.') continue;
            if (dirExists(parent + "/" + n) && fileExists(parent + "/" + n + "/kama.json"))
                out.push_back(parent + "/" + n);
        }
        std::sort(out.begin(), out.end());
    } else if (fileExists(dir + "/" + rel + "/kama.json")) {
        out.push_back(dir + "/" + rel);
    }
    return out;
}

// Every member directory of the workspace file at `wsDir`, canonical. Returns false + `err` on a
// malformed file, a mandatory member that is not there, or a mandatory glob that matched nothing.
//
// NOT recursive, and it has no cycle break, because neither is representable any more: a workspace does
// not nest and a project does not nest, so there is exactly one of these files per repository and depth
// is spelled with a deeper glob ("group/libs/*") rather than a second file. That is what took "which
// workspace owns me?" from a tree walk to a lookup.
bool expandWorkspace(const std::string& wsDir, std::set<std::string>& members, std::string& err)
{
    const std::string wsFile = wsDir + "/" + kWorkspaceFile;
    std::vector<std::pair<std::string, bool>> entries;
    if (!loadWorkspace(wsFile, entries, nullptr, err)) { err = wsFile + ": " + err; return false; }

    // A workspace root is not a project. Members live beside this file, not under it, so "projects do
    // not nest" has nothing to say about them — but a root that is ALSO a project would be a project
    // composing projects, which is the one shape the whole split exists to remove.
    if (fileExists(wsDir + "/kama.json")) {
        err = wsFile + ": there is also a kama.json here — a workspace root is not a project. Move the "
              "project into a directory of its own and list it";
        return false;
    }

    for (const auto& e : entries) {
        const std::string& rel = e.first;
        const bool optional    = e.second;
        std::vector<std::string> hits = expandProjectsEntry(wsDir, rel);
        if (hits.empty() && !optional) {
            const bool glob = rel.size() > 2 && rel.compare(rel.size() - 2, 2, "/*") == 0;
            err = wsFile + ": " + (glob
                ? "`" + rel + "` matched no project — say \"optional\": true if it may match nothing"
                : "no project at `" + rel + "` (no " + rel + "/kama.json) — it says \"optional\": false");
            return false;
        }
        for (const auto& h : hits) members.insert(absolutePath(h));
    }
    return true;
}

// The directory of the workspace file at or above `dir`, or "" if there is none. A `.kama` component
// stops the walk, so a vendored dependency can never reach out into its host repository and call itself
// a member.
std::string workspaceRootFor(const std::string& dir)
{
    for (std::string cur = absolutePath(dir);;) {
        if (baseName(cur) == ".kama") return "";
        if (fileExists(cur + "/" + kWorkspaceFile)) return cur;
        std::string parent = dirName(cur);
        if (parent == cur || parent == ".") return "";   // filesystem root
        cur = parent;
    }
}

// The workspace that OWNS `projectDir`: the members of the nearest ancestor workspace file that actually
// lists it. Falls back to `{projectDir}` when nothing claims it — no workspace, so a path dep stays
// top-level only. `err` (optional) carries a malformed or unsatisfiable workspace file; callers that
// must keep answering on a broken tree pass nullptr and get the fallback.
//
// CONTAINMENT is still the whole rule, as it is for lspFindProject, so the build and the editor agree on
// what one workspace is.
std::set<std::string> workspaceMembers(const std::string& projectDir, std::string* err = nullptr)
{
    const std::string self  = absolutePath(projectDir);
    const std::string wsDir = workspaceRootFor(self);
    if (wsDir.empty()) return { self };

    std::set<std::string> members;
    std::string e;
    if (!expandWorkspace(wsDir, members, e)) { if (err) *err = e; return { self }; }
    if (!members.count(self)) return { self };
    return members;
}

// Resolve a module NAME (["std","collections"]) to the files that make it up, searching `roots` in
// order. A module name is `<project>::<chain>`, so this is the exact inverse of moduleIdForFile and is
// defined beside it, below, where the manifest's module tree exists. Empty result => unresolved.
//
// ⚠️ It no longer LOOKS for anything. Until 2d it tried `<root>/std/collections.kama` (a file-module,
// §2b.9) and then `<root>/std/collections/` (any directory at all), which is how a loose build reached
// across the filesystem for a name nobody had passed it — the behavior §2i removes. What resolves now is
// a module some project DECLARES, and a loose build's own files are not searched for because they are
// already in hand: the operands are the compilation.
std::vector<std::string> resolveModuleFiles(const std::vector<std::string>& segs,
                                            const std::vector<std::string>& roots,
                                            std::string* matchedRoot = nullptr);

// Why a module did not resolve, said in terms of the rule that refused it. Defined beside the resolver,
// declared here because loadProgramUnits is where the failure surfaces.
static void reportUnresolvedModule(const std::string& name, const std::vector<std::string>& segs,
                                   const std::string& fromFile, const std::string& buildManifestDir,
                                   const std::string& stdlibDir, bool reserved);

SharedCompilationUnit parseFile(const std::string& inputFile);   // defined below
SharedCompilationUnit parseString(const char* src, const std::string& name);   // defined below

// The name an `import` must write to reach this unit: the module its PATH puts it in, and nothing else.
// A file used to be able to answer this with a `namespace` declaration, which is precisely the defect
// §1a found — the resolver and the emitter could disagree, so a file was COMPILED into one scope and
// IMPORTED as another. There is one source of truth now and CEmitter::ctxOf reads the same one. Empty
// means nothing can name it. Defined below, where the derivation it calls exists.
static std::string moduleKeyOf(const SharedCompilationUnit& u, const std::string& path);

// ---- closure pruning: a directory import should not compile the directory ---------------------------
// `import std::collections::{DynamicArray};` resolves to EVERY .kama in lib/std/collections/ — the `{…}`
// list controls visibility, not compilation. examples/httpd names four imports and compiles 32 units, 20
// of which contribute no live symbol. So: resolve to the files that DEFINE the named symbols, plus their
// transitive intra-directory closure.
//
// The closure cannot be computed from `import` edges. Unqualified names resolve against the file's own
// namespace program-wide (CEmitter::resolveFuncImpl), and every file of a directory module shares one
// namespace — so siblings reference each other with no import at all. lib/std/collections/priority_queue
// .kama has no `import` whatsoever and declares `DynamicArray<T, A> data;`. An import-edge closure
// under-computes and emits calls to functions that were never compiled. What it follows instead is
// CompilationUnit::identTokens — every identifier spelling in the file, a superset of its references.
//
// Intra-module import edges need no separate handling: `import std::collections::{View};` puts the token
// `View` in the importing file's identTokens, so an import edge is just one more reference. Cross-module
// edges remain the BFS in loadProgramUnits.
bool pruneTraceOn(int level = 1);   // defined below

struct ModuleIndex {
    std::vector<std::string>                    files;   // as resolved — the SPELLING parseFile was handed
    std::vector<SharedCompilationUnit>          units;   // parallel to files
    std::map<std::string, std::vector<size_t>>  byName;  // top-level name -> the files declaring it
    bool complete    = false;  // false if a file failed to parse: fall back, let the load path report it
};

// Index cache for ONE loadProgramUnits call, keyed by the joined file list. Deliberately not process-wide:
// a static would go stale in `kama lsp` when a file changes on disk (the parse cache validates mtime/size,
// a file-list key cannot), and the win does not need it. Within a call it earns its keep — per-symbol
// `provided` re-enters the same module for a different symbol, and this is what keeps that from re-parsing.
typedef std::map<std::string, ModuleIndex> ModuleIndexCache;

const ModuleIndex& moduleIndexFor(const std::vector<std::string>& files, ModuleIndexCache& cache)
{
    std::string key;
    for (auto& f : files) { key += f; key += '\n'; }
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    ModuleIndex ix;
    ix.files = files;
    ix.complete = true;
    for (size_t i = 0; i < files.size(); ++i) {
        // Parsed in resolution order, and STOPPING at the first failure — the load path below re-reads
        // these same units rather than calling parseFile again, so a broken file is reported exactly
        // once, and the files after it stay unparsed exactly as they are today.
        SharedCompilationUnit u = parseFile(files[i]);
        ix.units.push_back(u);
        if (!u) { ix.complete = false; break; }
        for (auto& n : u->topLevelNames) ix.byName[n].push_back(i);
    }
    return cache.emplace(key, std::move(ix)).first->second;
}

// The subset of `files` needed to satisfy `symbols`, or EMPTY meaning "no pruning — load them all".
//
// Every bail-out returns empty rather than guessing, so anything this does not fully understand keeps
// today's behavior byte for byte: the missing-symbol error, the `@compileFor` "not available in this
// build configuration" diagnostic, and the export-privacy check all still fire from the full module.
std::vector<std::string> closureOfModule(const std::vector<std::string>& files,
                                         const std::vector<std::string>& symbols,
                                         ModuleIndexCache& cache,
                                         const char** whyNot = nullptr)
{
    auto bail = [&](const char* why) { if (whyNot) *whyNot = why; return std::vector<std::string>(); };

    // A bare `import a::b;` or `import a::b as m;` names nothing, so nothing pins a file. Do NOT try to
    // seed it from the importing file's own tokens: a type reached only through inference is never
    // spelled anywhere, and that unsoundness is not present in the symbol-list case, where the named
    // symbol pins one file and the reference closure covers the rest.
    if (symbols.empty())  return bail("bare import");
    if (files.size() < 2) return bail("single file");

    const ModuleIndex& ix = moduleIndexFor(files, cache);
    if (!ix.complete)    return bail("parse failed");
    // There used to be a `mixed namespaces` bail here, on the premise that the reference closure needs
    // every file in `files` to share one scope — true, and it was a real hazard while a package's `source`
    // root was walked recursively and a file's scope came from a declaration it could put anything in.
    // Neither holds now: `files` comes from resolveModuleFiles, which builds the set by asking
    // moduleIdForFile about each candidate and keeping the ones whose module IS the one being imported.
    // The premise is guaranteed by construction, so the check was dead code — MEASURED, not reasoned:
    // built with a tripwire on that branch and ran the whole matrix on all three legs, and it never fired.

    std::vector<size_t>      work;
    std::vector<bool>        keep(ix.files.size(), false);
    std::vector<std::string> why(ix.files.size());   // trace level 2 only
    auto add = [&](size_t i, const std::string& reason) {
        if (keep[i]) return;
        keep[i] = true; work.push_back(i);
        if (pruneTraceOn(2)) why[i] = reason;
    };

    for (auto& s : symbols) {
        auto it = ix.byName.find(s);
        // The index did not find a symbol this import names. Either it is genuinely undeclared, or it was
        // dropped by `@compileFor` — both want the full module, so the existing tailored diagnostic fires.
        if (it == ix.byName.end()) return bail("unknown symbol");
        for (size_t i : it->second) add(i, "imported: " + s);
    }
    for (size_t i = 0; i < ix.units.size(); ++i)
        if (ix.units[i] && ix.units[i]->unprunable)
            add(i, "unprunable");   // nameless decl, so unreachable by any reference

    // Fixpoint over references. A visited set, not recursion: `set` -> `Map` and `sort` -> `View` already
    // make this a graph, and the reference closure makes a cycle far likelier than the import edges do.
    while (!work.empty()) {
        size_t i = work.back(); work.pop_back();
        if (!ix.units[i]) continue;
        for (auto& tok : ix.units[i]->identTokens) {
            // A name this file declares ITSELF is satisfied here; it pulls in no sibling. That is a
            // no-op for ordinary declarations — two files of one namespace cannot both define `View` —
            // but `extern fn` is the exception that matters: it declares a C symbol, not a module
            // definition, so several files legitimately repeat it. Three of lib/std/collections' files
            // each declare their own `extern fn memset`, and without this a reference to `memset` from
            // fixed_array.kama drags in map.kama and bit_set.kama (and then hasher.kama behind map),
            // which is three of the seven units httpd was keeping.
            if (ix.units[i]->topLevelNames.count(tok)) continue;
            auto it = ix.byName.find(tok);
            if (it == ix.byName.end()) continue;
            for (size_t j : it->second) add(j, baseName(ix.files[i]) + " references " + tok);
        }
    }

    std::vector<std::string> out;
    for (size_t i = 0; i < ix.files.size(); ++i) if (keep[i]) out.push_back(ix.files[i]);
    if (pruneTraceOn(2))
        for (size_t i = 0; i < ix.files.size(); ++i)
            if (keep[i]) fprintf(stderr, "kama-prune:     keep %-22s %s\n",
                                 baseName(ix.files[i]).c_str(), why[i].c_str());
    if (out.empty())            return bail("empty closure");   // a resolver bug looks like this
    if (out.size() == files.size()) { if (whyNot) *whyNot = "nothing to drop"; }
    return out;
}

// The unit for `file` if some module index in this call already parsed it, else a fresh parse. Indexing a
// module parses every file in it, so without this the kept files would be parsed twice — and a file with
// a syntax error would report it twice, which the fixture suite compares byte for byte.
SharedCompilationUnit indexedUnit(ModuleIndexCache& cache, const std::string& file)
{
    for (auto& kv : cache) {
        const ModuleIndex& ix = kv.second;
        for (size_t i = 0; i < ix.units.size() && i < ix.files.size(); ++i)
            if (ix.files[i] == file && ix.units[i]) return ix.units[i];
    }
    return parseFile(file);
}

// `KAMA_PRUNE_TRACE=1` reports what the closure decided, per import, on stderr. The campaign's validation
// gate: the closure is computed and traced before it is allowed to change what gets loaded.
// Level 2 additionally names every kept file and the reference that pulled it in — which is how you answer
// "why is this still 19 units and not 17", the only question this campaign ever gets asked.
bool pruneTraceOn(int level)
{
    static int lvl = [] { const char* e = getenv("KAMA_PRUNE_TRACE"); return (e && *e) ? atoi(e) : 0; }();
    return lvl >= level;
}

// `KAMA_NO_PRUNE=1` restores pre-campaign resolution: a directory import loads the directory. It exists so
// the whole suite can be A/B'd on ONE binary, and the A/B is a proof rather than a hope — with pruning off
// every `provided` entry is `whole`, so the unit set is identical to the pre-campaign one by construction.
bool pruningOff()
{
    static bool off = [] { const char* e = getenv("KAMA_NO_PRUNE"); return e && *e; }();
    return off;
}

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

std::string owningPackageDir(const std::string& fromDir);   // defined below, beside the manifest loaders

// ---- file -> module identity (§2b/§2e) -------------------------------------------------------------
//
// What phase 2 replaces the `namespace` declaration WITH: a file's module is derived from where it sits,
// not from what it says. The derivation itself is `moduleIdForFile`, defined below because it needs the
// manifest reader; the TYPE lives up here because `configureEmitter` hands the answer to the emitter and
// sits above that reader.
struct ModuleId {
    bool        inProject = false;   // false: a LOOSE file — no manifest names it (§2i)
    std::string project;             // the project's IMPORT name (`@acme/geo` imports as `geo`)
    std::string module;              // composed module name; "" means the project root

    // What an `import` writes, and what §2e.25 mangles into the C symbol. A loose file has no project
    // name to carry, so its folder chain IS its module — and a loose file in the root has no module at
    // all, which is what makes it unimportable (§2e.27).
    std::string full() const {
        if (!inProject) return module;
        return module.empty() ? project : project + "::" + module;
    }
};
static ModuleId moduleIdForFile(const std::string& absPath);
bool kamaModuleVisibleTo(const std::string& importer, const std::string& imported);   // §2c, defined below

// module full name ("std::collections", or a bare project name for a root module) -> the `kama.json` that
// declares it. Filled as units are attributed, consumed by the visibility rung.
static std::map<std::string, std::string>& moduleManifestIndex()
{
    static std::map<std::string, std::string> m;
    return m;
}

// owningPackageDir answers with an ABSOLUTE path, and that answer reaches user-facing output — the `out`
// root is built from it, so every "kama: built …" line would carry a full path for an ordinary build run
// from inside its own project. Spell it "." when it IS the current directory, which is what the shallow
// check used to return there. Purely cosmetic, and purely about not making the common case noisier.
static std::string relativizeToCwd(const std::string& dir)
{
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) && absolutePath(dir) == std::string(buf)) return ".";
    return dir;
}

// THE project-discovery rule. There is one question here — "which project owns this invocation?" — and
// it used to be answered in four places with three different rules: the dependency view walked nowhere,
// the build-config path (which decides the `out` root) repeated that same non-walk verbatim, the
// toolchain selector walked but only from the CWD, and owningPackageDir walked properly but was reserved
// for a different question. The rules disagreed, so `kama build proj/src/app.kama` from outside proj/
// could take its dependencies from one project, its output root from another, and its toolchain version
// from a third. Everything routes through here now; owningPackageDir stays the walk primitive.
//
// The rule: **walk up from the first input's directory, else from the CWD**, stopping at a `.kama`
// component so a vendored dependency is never owned by its host project.
//
//     kama build proj/src/app.kama          # from proj/'s PARENT -> proj/
//     kama run                              # no input -> walk up from the CWD
//
// Two things this settles that the old spellings got wrong:
//
//   * The walk goes BEFORE the CWD fallback. A file belongs to the project it LIVES in, not to wherever
//     the shell happens to be standing — building projB/src/app.kama while sitting in projA uses projB's
//     manifest rather than silently borrowing projA's.
//   * With no inputs the CWD is WALKED, not just probed. `projectManifestDir` used to test `./kama.json`
//     and give up, so `kama run` from a subdirectory of its own project found nothing while the toolchain
//     selector, walking, found the project — the two disagreeing about the same directory.
//
// NOTE this is whoever is COMPILING, which for a monorepo is not necessarily the package a given source
// file belongs to. That distinction is the whole of the per-package import check below, and is why
// owningPackageDir remains a separate entry point rather than being folded in here: it answers
// "who owns THIS file?" per unit, while this answers "who is driving?" once per invocation.
// ⚠️ THE CLI NO LONGER WALKS. `main` installs the answer from the operand (§2g.32) before any of this
// runs, and then this whole walk is dead for a command line: a project operand sets it to that manifest's
// directory, and a LOOSE build sets it EMPTY on purpose — no manifest, no deps view, no `out` root.
//
// A tri-state, and the third state is the point: `set` distinguishes "the CLI says there is no project"
// from "nobody has said anything yet". Without it a loose build would fall through to the walk and pick
// up whatever manifest sits above the file, which is exactly the silent behavior the operand rule exists
// to remove.
//
// The walk below therefore survives for ONE caller: `kama lsp`, which is handed a buffer and a rootUri
// and never a command line, so discovery there is inherent rather than convenient (§2g.35).
static bool        g_cliProjectSet = false;
static std::string g_cliProjectDir;
void setCliProjectDir(const std::string& dir) { g_cliProjectSet = true; g_cliProjectDir = dir; }

std::string projectManifestDir(const std::vector<std::string>& inputs)
{
    if (g_cliProjectSet) return g_cliProjectDir;
    // The shallow hit first, so the common in-project case keeps returning the RELATIVE path it always
    // did (owningPackageDir returns an absolute one, and this string reaches user-facing diagnostics).
    std::string start = inputs.empty() ? std::string(".") : dirName(inputs[0]);
    if (fileExists(start + "/kama.json")) return start;
    std::string owner = owningPackageDir(start);
    return owner.empty() ? std::string() : relativizeToCwd(owner);
}

// The same answer as a manifest PATH, for the callers that want the file rather than its directory.
std::string projectManifestPath(const std::vector<std::string>& inputs)
{
    std::string dir = projectManifestDir(inputs);
    return dir.empty() ? std::string() : dir + "/kama.json";
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

// ---- the loose ROOT: a module is a folder, manifest or no manifest (§2i) ---------------------------
//
// A loose build has no manifest by construction — the operand's basename is the mode (§2g), and mode 1
// means "just these files". But a module is a FOLDER, so the tree the operands span is what names them:
// resolve every operand, take the deepest common ancestor of their DIRECTORIES, and a file's module is
// its own directory relative to that root, `/` written `::`.
//
// Order-independent, because it reads a SET and not a sequence — which is the invariant §1b actually
// wants. It IS set-dependent: adding a file from a sibling tree raises the root and renames modules.
// That is accepted rather than mitigated (§2i.41) — a different file set is a different program in
// loose mode, and unrelated trees yielding long module names is the signal that the build wants a
// `kama.json`.
//
// ⚠️ For an OPERAND this overrides a `kama.json` sitting above it, because in loose mode no manifest
// participates in anything: not the flag universe, not the dependency view, not the `out` root, and so
// not identity either. A project's files compiled loosely are therefore NOT that project's modules —
// they carry the folder names their operand set derives, with no project name in front. That is why
// `kama check src/app.kama src/thing/t.kama` cannot resolve `probe::thing`: the operand set names that
// file `thing`. The answer is to name the manifest, and the diagnostic says so.
//
// ⚠️⚠️ And it overrides it for an OPERAND ONLY — not for everything under the root, which is what this
// first did and what two guards caught within the hour. A loose build's stdlib may perfectly well sit
// inside the tree its operands span (`check-runtime-dir` stages a payload at `$tmp/payload/lib/kama` and
// compiles `$tmp/s.kama`; `check-manifest` vendors a path dep under the project it builds), and those
// files are a DEPENDENCY, not a source — §2i says so in as many words. Named by the root they would have
// become `payload::lib::kama::std::collections`. A dependency keeps the identity its own project gives
// it; what a loose build refuses to read is a manifest over the files it was HANDED.
static bool        g_looseBuild   = false;   // the CLI named .kama operands (§2g mode 1)
static std::string g_looseRoot;              // "" — no operands, or no common ancestor at all
static std::set<std::string> g_looseOperands; // resolved, so a manifest is overridden for these only

void setLooseBuild(bool loose) { g_looseBuild = loose; }

// `b` is `a` itself or sits under it. A path COMPONENT test, never a string prefix: `/a/bc` is not
// under `/a/b`. (The same trap deepestModuleFor guards against for relative paths.)
static bool underPath(const std::string& a, const std::string& b)
{
    if (a.empty()) return false;
    if (b == a) return true;
    if (b.size() <= a.size() || b.compare(0, a.size(), a) != 0) return false;
    return b[a.size()] == '/' || a == "/";
}

// Establish the root for ONE program's operand set. Called per program, not per process: `kama check
// --each` runs N independent programs in one process, and each one's root is its own operands' — which
// is what makes `--each` mean N loose builds rather than one wide one.
static void setLooseRoot(const std::vector<std::string>& operands)
{
    g_looseRoot.clear();
    g_looseOperands.clear();
    std::vector<std::string> dirs;
    for (const auto& o : operands) {
        if (o.empty() || o[0] == '<') continue;          // synthetic units have no path to span
        const std::string abs = absolutePath(o);
        g_looseOperands.insert(abs);
        dirs.push_back(dirName(abs));
    }
    if (dirs.empty()) return;
    std::string root = dirs[0];
    for (size_t i = 1; i < dirs.size() && !root.empty(); ++i) {
        while (!underPath(root, dirs[i])) {
            const std::string parent = dirName(root);
            if (parent == root || parent == ".") { root.clear(); break; }   // no common ancestor at all
            root = parent;
        }
    }
    g_looseRoot = root;
}

// The module an OPERAND sits in, derived from the loose root — "" for a file in the root itself (§2e.27
// makes those unimportable), for anything outside the root, and for a folder whose name is not a legal
// kama identifier. That last one is not a rejection: nothing could ever write `import my-lib::{ … }`,
// so such a folder is not a module and its files stay file-private, exactly as they are today.
static std::string looseModuleFor(const std::string& absPath)
{
    if (g_looseRoot.empty()) return std::string();
    const std::string dir = dirName(absPath);
    if (!underPath(g_looseRoot, dir) || dir == g_looseRoot) return std::string();
    const std::string rel = dir.substr(g_looseRoot.size() + 1);
    std::string mod;
    for (size_t i = 0, s = 0; ; ++i) {
        if (i != rel.size() && rel[i] != '/') continue;
        const std::string seg = rel.substr(s, i - s);
        if (!kamaIsIdentifier(seg)) return std::string();
        mod += (mod.empty() ? "" : "::") + seg;
        if (i == rel.size()) break;
        s = i + 1;
    }
    return mod;
}

// Declared up beside loadProgramUnits, which is what needs it first. The unit itself says nothing about
// where it lives, so it is unused — kept in the signature because every call site has one in hand and a
// future rung (a synthetic unit's stated module, §2f.30) would read it.
static std::string moduleKeyOf(const SharedCompilationUnit&, const std::string& path)
{
    if (path.empty() || path[0] == '<') return std::string();   // synthetic: no path to derive from
    return moduleIdForFile(path).full();
}

// Defined once DepSpec exists, beside the manifest loaders it wraps.
const std::set<std::string>& declaredImportNames(const std::string& packageDir);
// A package's OWN import name — what its files spell when they reach across its own modules. Empty if
// the manifest has no `name` or cannot be read.
const std::string& ownImportName(const std::string& packageDir);
std::string storeDir();   // the content-addressed package store (~/.kama/store)

// The directory of the nearest `kama.json` at or above `fromDir` — the package that OWNS a file. A
// `.kama` component stops the walk, so a vendored dependency is never owned by its host project. "" if
// no manifest is above it at all (a scratch file, which nothing can be said about).
std::string owningPackageDir(const std::string& fromDir)
{
    // POSITIVE results only. The cache is a process-wide `static` with no invalidation, and `kama lsp`
    // is a long-lived process that re-analyzes on every keystroke — so a cached "there is no project
    // here" would survive the user creating the kama.json that makes one. That is not hypothetical: the
    // LSP registers a `**/kama.json` watcher precisely because manifests appear mid-session, and
    // `kama seed` is a normal way to make one appear. A miss costs a walk to the root, which is a
    // handful of stat calls — nothing next to the parse it precedes.
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
    if (found.empty()) return found;
    return cache.emplace(fromDir, found).first->second;
}



// THE FILE GATE, consulted at each of the three points a unit is admitted to a compilation below.
// Defined further down, beside the build-flag globals it reads; declared here because loadProgramUnits
// is the only caller and sits above them.
enum class FileGate { Active, Inactive, Malformed };
static FileGate fileGateOf(const SharedCompilationUnit& unit, const std::string& path);
static std::string renderFileGate(const SharedAttributeList& gate);
static std::string activeTargetLabel();

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
                      std::vector<Diagnostic>* diagsOut = nullptr,
                      bool prune = true)
{
    // The operand set names this program's modules when no manifest does (§2i). Established HERE, per
    // program rather than per process: `kama check --each` runs N independent programs in one process and
    // each one's root is its own operands' — which is what makes `--each` mean N loose builds and not one
    // wide one. It is also why this is not read off `main`'s argv, which knows nothing of `--each`.
    setLooseRoot(cliInputs);
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
    ModuleIndexCache moduleIndex;    // per call, so it can never serve a unit staler than this analysis
    std::set<std::string> seen;      // resolved absolute paths already parsed
    // Namespaces already in the compilation, satisfying an import without a disk lookup. TWO states,
    // because once a module can be loaded in PART, "is this module here?" stops being the question:
    //   whole   — every file of it is loaded; satisfies any symbol list, and a bare import.
    //   partial — only the files some earlier import needed. Satisfies a symbol list only if it declares
    //             every name asked for; otherwise the import is re-resolved and the closure pulls more.
    // Without the partial state a second import of the same module with DIFFERENT symbols would be
    // skipped and its files never loaded. lib/std/net/udp.kama is the live case: it imports
    // `std::net::{SocketAddr, RecvFrom}` from INSIDE namespace std::net.
    std::set<std::string> providedWhole;
    std::map<std::string, std::set<std::string>> provided;
    // A module the COMPILER already carries is already provided, so an explicit `import` of it is a
    // satisfied no-op. Read off the embedded set rather than naming a module, which is the difference
    // between a rule and an exception: this says nothing about `std::memory` in particular, and a second
    // embedded module would need no edit here.
    //
    // WHOLE, necessarily — a partial entry would send an unsatisfied symbol back to the resolver, and in
    // a `--no-std` install (bin/kama only, no lib/ at all) there is nothing on disk to satisfy it.
    //
    // ⚠️ This used to read `providedWhole.insert("std::memory")` and was justified by "there is no
    // lib/std/memory on disk". That is now false — the triad lives at lib/std/memory/ and is embedded as
    // well — so the justification was re-derived rather than the string edited. PROBED before rewriting
    // it: with this skip removed entirely the build still succeeds, because the embedded units are
    // collect-only and `Owned<T>` is generic, so it emits at each instantiation and the disk copy's
    // translation unit comes out three lines long and empty. The skip is kept because handing the C
    // compiler an empty file for a module we already have is work with no result, not because dropping
    // it breaks.
    for (int pi = 0; pi < KAMA_PRELUDE_MODULE_COUNT; ++pi)
        if (const char* m = KAMA_PRELUDE_MODULES[pi].module) if (*m) providedWhole.insert(m);
    // A CLI input is loaded entire — but it is one FILE, and its namespace may have other files. So it
    // contributes PARTIAL, exactly like a pruned module below: the names it actually declares. Marking it
    // `whole` claimed the whole module was present, and a file that imports a SIBLING from inside its own
    // namespace then skipped the resolve and never loaded it — `import std::collections::{View}` inside
    // `namespace std::collections` reported "module does not export `View`", plus the cascade from every
    // type that then failed to resolve. Harmless for a build reached through a consumer (the module
    // resolves as a foreign import and loads whole), so what it actually broke was every command whose
    // input IS a member file: `kama check`, and therefore the language server on 8 of the 47 stdlib files
    // and on any multi-file module a user writes.
    //
    // A CLI input is loaded entire, so it contributes `whole` — and when the input is one FILE of a
    // multi-file module, the loop below MAKES that true by loading the module's other files. It has to:
    // "files of one directory share a namespace, so a sibling is reachable unqualified with no `import`
    // at all" (SPEC, Modules). Those references are not import edges, so nothing pins the
    // siblings and there is no import for resolution to hang off — which is exactly the case SPEC answers
    // with "anything the resolver does not fully understand loads the WHOLE module".
    //
    // Without it, analysing one member file saw only that file: `lib/std/math/mat.kama` names `Vec2`/`Vec4`
    // from its sibling `vec.kama` and reported 202 unknown-type errors, and a file that self-imports a
    // sibling reported "module does not export X". Builds were never affected — reached through a consumer
    // the module resolves as a foreign import and loads whole — so what this fixes is every command whose
    // INPUT is a member file: `kama check`, and therefore the language server.
    //
    // SAME DIRECTORY AND SAME MODULE, which is narrower than it looks and deliberately so. Directory,
    // because that is what a module is; the repo has four module NAMES living in more than one directory
    // (`shapes`, `geo`, `Graphics`, `lib` — unrelated fixture modules that merely share a name) and
    // merging those would be wrong. Module, because a directory need not be one: a file the derivation
    // gives no module (a loose operand in the build's own root) is its own compilation, so nothing is
    // pulled in beside it.
    //
    // 2d widened this in one visible way: a project's entry file now HAS a module (its root), where a
    // file declaring no namespace used to have none and skip the scan entirely. So an editor session on
    // `src/app.kama` now loads its folder's other files, which is what a module means. Cost is bounded and paid only where it buys something — worst
    // case in this repo is std::collections' 14 files, measured at ~10 ms, one-time.
    // `kama lsp` pays it once per session rather than per keystroke: the M5.2 parse cache is keyed on
    // path+mtime+size, and a sibling does not change while you type in another file.
    //
    // ⚠️ AND IT DOES NOT RUN FOR A LOOSE BUILD (§2i.40). "A loose build does no filesystem searching — you
    // pass every source file" is the rule, and this scan was the last place it was not true: a file
    // sharing a directory with an operand was compiled into the program without ever being named. 2d
    // deleted the `here` import search on that argument and left this one for 2e to decide; the decision
    // is that nothing sneaks in. A build that wants more than it names has a `kama.json` for saying so —
    // and that is not a workaround, it is the model: §2i.42 makes listing those same folders a NO-OP, so
    // the manifest is what turns an operand list into a project rather than a second way to compile.
    //
    // `kama lsp` is NOT a loose build and keeps the scan: `setLooseBuild` is called once, on the
    // build/check/run operand path, and the server never goes through it. So an open buffer inside a
    // project still analyses against its module's other files, which is the asymmetry §2g.35 designed —
    // the CLI takes its operand at its word, the editor walks. An open file with no `kama.json` above it
    // degrades to single-file, which is what a file with no module has always got.
    size_t gatedOut = 0;                 // inputs the file gate excluded — see the report below the loop
    for (size_t ci = 0; ci < cliInputs.size(); ++ci) {
        std::string abs = absolutePath(cliInputs[ci]);
        if (!seen.insert(abs).second) continue;
        SharedCompilationUnit u = parseFile(cliInputs[ci]);
        if (!u) return false;
        // ADMISSION POINT 1 of 3 — the file gate on an operand. A file the CLI NAMED is a direct
        // request, so a gate that excludes it is a hard ERROR rather than a quiet nothing: `kama build
        // audio_native.kama --target wasm32-…` has to say why it produced no binary. A file the build
        // COLLECTED (a manifest's source root, the LSP's workspace) is skipped silently instead — that
        // is what collection is for. `g_looseBuild` is exactly that distinction, already recorded:
        // it is set iff the operands themselves are the compilation (§2i.40).
        switch (fileGateOf(u, cliInputs[ci])) {
            case FileGate::Malformed: return false;
            case FileGate::Inactive:
                if (g_looseBuild) {
                    // Pointed at the GATE's own line, not at the file as a whole: the gate is the thing
                    // to read, and an editor given a bare filename has nowhere to put the squiggle.
                    const int gl = (*u->fileGate)[0] ? (*u->fileGate)[0]->line : 1;
                    fprintf(stderr, "kama: error: %s:%d: `file %s;` excludes this file from a %s build,"
                                    " so there is nothing to compile.\n",
                            cliInputs[ci].c_str(), gl, renderFileGate(u->fileGate).c_str(),
                            activeTargetLabel().c_str());
                    return false;
                }
                ++gatedOut;
                continue;
            case FileGate::Active: break;
        }
        units.push_back(u); paths.push_back(abs);
        std::string k = moduleKeyOf(u, abs);
        if (k.empty()) continue;                 // in no module: file-private, it IS its own module
        providedWhole.insert(k);
        if (g_looseBuild) continue;              // §2i.40 — the operands ARE the compilation
        for (auto& sib : listKamaFiles(dirName(cliInputs[ci]))) {
            std::string sabs = absolutePath(sib);
            if (seen.count(sabs)) continue;      // the input itself, or another input, or already loaded
            // A sibling that does not parse is left to whoever asks for it directly: it may be unrelated
            // to this module (a different namespace), and failing the analysis of the file the user DID
            // ask about, because of a file they did not, would trade one broken command for two.
            SharedCompilationUnit su = parseFile(sib);
            if (!su || moduleKeyOf(su, sabs) != k) continue;
            // ADMISSION POINT 2 of 3. The module check comes FIRST deliberately: a malformed gate is an
            // error only in a file this compilation actually wants, and a sibling in another module is
            // not one — the same line this scan already draws for a sibling that does not parse.
            switch (fileGateOf(su, sib)) {
                case FileGate::Malformed: return false;
                case FileGate::Inactive:  continue;
                case FileGate::Active:    break;
            }
            seen.insert(sabs);
            units.push_back(su); paths.push_back(sabs);
        }
    }
    // EVERY input gated out. Left alone this surfaces as `clang: error: no input files` — a message
    // about the wrong tool, for a build that did exactly what its own source told it to. It is reachable
    // the moment a project's entry file carries a gate, so it gets kama's own sentence.
    if (units.empty() && gatedOut > 0) {
        fprintf(stderr, "kama: error: every source file is excluded from a %s build by its own"
                        " `file @compileFor(...)` gate — there is nothing to compile.\n",
                activeTargetLabel().c_str());
        return false;
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
            // The names this import needs, and whether what is already loaded covers them. A bare import
            // (no symbol list) names nothing and is satisfied only by a `whole` module.
            std::vector<std::string> wanted;
            if (imp->symbols)
                for (auto& sym : *imp->symbols)
                    if (sym && sym->identifier && sym->identifier->value)
                        wanted.push_back(*sym->identifier->value);   // the module-side name, never the alias
            if (providedWhole.count(key)) continue;           // already in the compilation, entire
            if (!wanted.empty()) {
                auto pit = provided.find(key);
                if (pit != provided.end()) {
                    bool covered = true;
                    for (auto& w : wanted) if (!pit->second.count(w)) { covered = false; break; }
                    if (covered) continue;                    // every name asked for is already loaded
                }
            }
            bool reserved = (segs[0] == "std" || segs[0] == "core");
            std::vector<std::string> roots;
            if (!reserved) {
                // The project this invocation is FOR, and the §2i cutover in one line: it is the file's
                // own directory that used to sit here, so `import a::b` reached for `<dir>/a/b/` on disk
                // and a loose build pulled in files nobody had named. A project's modules now come from
                // the project, which for a loose build is nothing at all — `projectManifestDir` is empty
                // by the operand rule (§2g.32), and the operands are already the compilation.
                //
                // `kama lsp` is the caller that still WALKS to find this, because an editor is handed a
                // buffer and never a command line; that asymmetry is §2g.35 and it is deliberate.
                if (!buildManifestDir.empty()) roots.push_back(buildManifestDir);
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
                reportUnresolvedModule(key, segs, paths[i], buildManifestDir, stdlibDir, reserved);
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
                // ⚠️ A package importing ITSELF is not a free-ride. A library whose own files reach
                // across its own modules (`lib/src/b/b.kama` importing `lib::a::av`) spells its own
                // package name — and no `dependencies` entry ever contains that, so the rule below would
                // demand a declaration that cannot be written. It only fired when the library was
                // CONSUMED, because that is when its sources arrive through the dependency view rather
                // than through the file's own directory; standalone, `via` is the local path and the
                // whole block is skipped. So a multi-module library built, shipped, and then failed the
                // first time anyone depended on it, with a diagnostic whose premise ("this package will
                // not build on its own") the previous command had just disproved. The suggested remedy
                // was a package depending on itself.
                const bool selfImport = !owner.empty() && ownImportName(owner) == segs[0];
                if (!owner.empty() && !selfImport && !declaredImportNames(owner).count(segs[0])) {
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
            // The files this import actually needs. Empty means "no pruning" — either a bail-out inside
            // the closure, the escape hatch, or a caller that opted out.
            const char* whyNot = nullptr;
            std::vector<std::string> keep = (prune && !pruningOff())
                                          ? closureOfModule(files, wanted, moduleIndex, &whyNot)
                                          : std::vector<std::string>();
            if (pruneTraceOn()) {
                fprintf(stderr, "kama-prune: %s %zu/%zu%s%s\n", key.c_str(),
                        keep.empty() ? files.size() : keep.size(), files.size(),
                        whyNot ? " — " : "", whyNot ? whyNot : "");
                if (whyNot && strcmp(whyNot, "unknown symbol") == 0)
                    for (auto& s : wanted)
                        fprintf(stderr, "kama-prune: UNFOUND %s::%s\n", key.c_str(), s.c_str());
            }
            const std::vector<std::string>& toLoad = keep.empty() ? files : keep;
            const bool pruned = !keep.empty() && keep.size() < files.size();

            // ADMISSION POINT 3 of 3. A module whose files are ALL gated out resolves to a real
            // directory holding real sources and still contributes nothing, so it has to be reported
            // HERE. Left to fall through, the symptom arrives much later and much worse — an
            // unresolved name in the importer, pointing at code that is not the problem.
            size_t admitted = 0;
            for (auto& f : toLoad) {
                std::string abs = absolutePath(f);
                bool fresh = seen.insert(abs).second;
                // From the index when it has already parsed this file, so indexing never doubles the
                // parse — and so a file that fails to parse is reported once, by whichever got there first.
                SharedCompilationUnit mu = indexedUnit(moduleIndex, f);
                if (!mu) return false;
                switch (fileGateOf(mu, f)) {
                    case FileGate::Malformed: return false;
                    // Stays in `seen`: under a fixed flag set the answer cannot change, so marking it
                    // processed is what keeps a later import of the same module from re-reading it.
                    case FileGate::Inactive:  continue;
                    case FileGate::Active:    break;
                }
                ++admitted;
                if (fresh) { units.push_back(mu); paths.push_back(abs); }
                std::string mk = moduleKeyOf(mu, abs);
                if (mk.empty()) continue;
                // What this module now provides. A pruned module contributes only the names it actually
                // declares, so a later import asking for one that is absent re-resolves and the closure
                // pulls the rest in. Recorded for every kept file, including one an earlier import
                // already loaded — the record is about the namespace, not about who loaded the file.
                if (pruned) for (auto& n : mu->topLevelNames) provided[mk].insert(n);
                else        providedWhole.insert(mk);
            }
            if (admitted == 0) {
                fprintf(stderr, "kama: error: %s:%d: imports module '%s', but every file in it is"
                                " excluded from a %s build by its own `file @compileFor(...)` gate.\n",
                        paths[i].c_str(), (imp ? imp->line : 1), key.c_str(), activeTargetLabel().c_str());
                fprintf(stderr, "kama: note: a platform split needs BOTH sides — give the module a file"
                                " gated for this target too, or gate the `import` site's declaration.\n");
                return false;
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
    if (g_parseCache && stat(osp(inputFile).c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
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

    FILE* input = fopen(osp(inputFile).c_str(), "r");
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
            SharedCompilationUnit u = parseString(KAMA_PRELUDE_MODULES[i].src,
                                                  KAMA_PRELUDE_MODULES[i].name);
            if (u) v.push_back(u);
        }
        return v;
    }();
    return units;
}

// `--no-heap` (MCU step 5): reject every emitter-visible heap allocation program-wide (the no-heap
// subset). Threaded to each CEmitter via `setNoHeap`. File-scope like the other build config, set in main.
static bool g_noHeap = false;

// `--strict-numeric` (M5a): TALLY every numeric hand-off whose source and destination types differ,
// as a TSV on stdout. Rejects nothing. Hidden — deliberately absent from `usage()` and the docs,
// because it exists to SIZE the strict-conversion rule and is deleted when that rule lands.
static bool g_strictNumeric = false;

// `--probe-templates`: TSV on stdout, one row per generic template NOBODY instantiates — what
// `checkUninstantiatedTemplates` walked, how many errors it raised, how many diagnostics it had to
// DEFER because a type was unknown rather than wrong, and — one column per `CEmitter::DeferKind` — WHY
// each deferral happened, which is what says whether a compiler change or a source change closes it.
// Hidden, for the same reason `--strict-numeric` is. It sized the opaque-type-parameter campaign and is
// KEPT rather than deleted — see `setProbeReport` for why: the walk still has a named residual, and reach
// is the kind of thing that regresses without any fixture noticing.
static bool g_probeTemplates = false;

// `--release`: strip `debugAssert(...)` at emit time (dev-only checks; `assert` stays always-on). File-scope
// like g_noHeap so the emitter-setup helpers can read it; set in main from the `--release`/`--debug` flags.
static bool g_release = false;
static bool g_outputShared = false;   // OUTPUT=SHARED — the emitter defines the runtime slots without a `main`

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

// ---- the FILE GATE: `file @compileFor(!ARCH_WASM32);` ------------------------------------------
//
// A unit's first line gates the WHOLE file, the way `@compileFor` gates one declaration. It is read
// HERE, in the driver, and deliberately NOT by the emitter that handles declaration gates: a gated-out
// file must never reach analysis at all, because its declarations may name types, externs and headers
// that do not exist on this target — which is the entire reason the feature exists. A package build
// compiles every `.kama` under its source root whatever the import graph, so before this there was no
// way for a native-only file to sit in a project that also builds for wasm.
//
// The unit IS still parsed — that is how the gate is read — so a syntax error in a gated-out file is an
// error on every target. That is a property worth keeping, not a cost: the alternative design (compile
// only import-reachable files) was rejected partly because it silently stops checking a file nobody
// imports.
//
// The predicate itself is `kamaCompileForActive` (kama.cemit.cpp), shared with the declaration gate so
// the two can never come to disagree about what `!FLAG` or `A, B` means.

// The gate as the author wrote it, so a diagnostic can quote the line instead of describing it.
static std::string renderFileGate(const SharedAttributeList& gate)
{
    if (!gate) return std::string();
    std::string out;
    for (auto& at : *gate) {
        if (!at || !at->name) continue;
        if (!out.empty()) out += " ";
        out += "@" + *at->name;
        if (!at->args || at->args->empty()) continue;
        out += "(";
        bool first = true;
        for (auto& arg : *at->args) {
            if (!arg) continue;
            if (!first) out += ", ";
            first = false;
            if (arg->name && arg->name->value && !arg->expression) { out += *arg->name->value; continue; }
            if (arg->expression) {
                auto* su = dynamic_cast<SimpleUnaryExpressionNode*>(arg->expression.get());
                if (su && su->token == EXCLAMATION && su->expression)
                    if (auto* id = dynamic_cast<IdentifierNode*>(su->expression.get()))
                        if (id->value) { out += "!" + *id->value; continue; }
            }
            out += "…";
        }
        out += ")";
    }
    return out;
}

static FileGate fileGateOf(const SharedCompilationUnit& unit, const std::string& path)
{
    if (!unit || !unit->fileGate || unit->fileGate->empty()) return FileGate::Active;

    // ⚠️ `@compileFor` and NOTHING else, and it is a hard error rather than a silent no-op — the same
    // rule, for the same reason, that a bodyless `extern` states: `@noheap` gates allocation in a BODY
    // and `@interrupt`/`@section` attach to EMITTED CODE, and a file is neither. Enforced here rather
    // than in the grammar so the rejection is a sentence, not a token name.
    bool malformed = false;
    for (auto& at : *unit->fileGate) {
        if (!at) continue;
        if (!at->name || *at->name != "compileFor") {
            fprintf(stderr, "kama: error: %s:%d: a file gate takes `@compileFor(...)` and nothing else"
                            " — `@%s` has no meaning on a whole file (it gates a body or emitted code,"
                            " and a file is neither).\n",
                    path.c_str(), at->line, (at->name ? at->name->c_str() : "?"));
            malformed = true;
        }
    }
    if (malformed) return FileGate::Malformed;

    const int line = (*unit->fileGate)[0] ? (*unit->fileGate)[0]->line : 1;
    bool bad = false;
    const bool active = kamaCompileForActive(unit->fileGate, g_activeFlags, g_declaredFlags, g_strictFlags,
                                             [&](const std::string& m) {
                                                 fprintf(stderr, "kama: error: %s:%d: %s\n",
                                                         path.c_str(), line, m.c_str());
                                                 bad = true;
                                             });
    if (bad) return FileGate::Malformed;
    return active ? FileGate::Active : FileGate::Inactive;
}

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
    // How the language's OWN runtime is linked: "" (unset -> the target's default) | "static" | "dynamic".
    // A toolchain property, which is why it lives here beside cc/ar/sysroot rather than in a select group —
    // Rust's model. Only Windows has a default worth overriding today (see the -lpthread arm of the link
    // tail); elsewhere libc IS the system, so there is no non-system runtime to make a choice about.
    std::string runtime;
    // Which Windows SUBSYSTEM the PE declares: "" (unset -> "console") | "console" | "windows".
    // A GUI program linked console-subsystem gets a stray console window Windows opens for it, on top of
    // the window it actually asked for. Same shape as `runtime` directly above and for the same reason —
    // a permanent property of the artifact, chosen per target, not a per-invocation switch. Only Windows
    // has a subsystem field at all; elsewhere this is an accepted no-op so one build script carries it.
    std::string subsystem;
    std::vector<std::string> cflags, ldflags;
    // Native libraries this artifact links (`-l<name>`), from the project's own `link` key or a target's
    // override of it. Distinct from `ldflags`, which is raw linker text: `link` is the portable half, so
    // a project needing `-lm` everywhere says it once instead of per target.
    std::vector<std::string> link;
    bool                     linkSet = false;   // this target OVERRODE `link` (vs. inheriting the project's)
    // Two PROJECT properties a target may override, same shape as `link` and for the same reason: both
    // are permanent facts about the artifact rather than per-invocation choices, and both have a real
    // per-target exception — `no-heap` on EMBEDDED but not on HOST is the ordinary case, and a WASM
    // build gets WebGPU from the browser rather than from the wgpu-native SDK.
    //
    // `no-heap` is the one that most needed a home: it fails SILENTLY when forgotten. The build simply
    // succeeds with allocation allowed, so a bare-metal target quietly gains a heap nobody asked for.
    bool webgpu = false,  webgpuSet = false;
    bool noHeap = false,  noHeapSet = false;
    // "this program's floating point must be bit-reproducible across targets" — named for the GOAL,
    // not for the clang flag it happens to emit today (the rule ROADMAP row 20 writes down for
    // `@linkName`: kama's only backend is C now, and a 2.0 bytecode VM has no `-ffp-contract`).
    bool reproFloat = false, reproFloatSet = false;
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
    // SIMD128 — "this target has 128-bit vector lanes", derived from the triple exactly as the rest are.
    //
    // ⚠️ It is NOT "the target can compile a `Simd<T, N>`". Every target can: `vector_size` degrades to
    // correct scalar code, which is the whole reason kama exposes a type rather than per-ISA intrinsics.
    // This flag answers the different question a library actually asks — *are the lanes real?* — so that
    // an author can pick a different ALGORITHM (a `@compileFor(SIMD128)` conformance beside a
    // `@compileFor(!SIMD128)` one) rather than hoping the fallback is fast enough.
    //
    // True where 128-bit vectors are in the target's BASELINE, needing no extra flag: SSE2 is mandatory
    // in the x86-64 ABI, NEON is mandatory in AArch64, and wasm gets it because the driver passes
    // `-msimd128` on every wasm build. Anything else — a 32-bit ARM whose NEON is optional, an MCU — is
    // false, which is the honest answer rather than an optimistic one.
    //
    // Gate on THIS, never on a target name: the flag is a fact about capability, and a name is not
    // (tests/xfail/simd128_target_name).
    if (t.arch == "x86_64" || t.arch == "aarch64" || t.arch == "wasm32" || t.arch == "wasm64")
        f.insert("SIMD128");
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

// How this build spells the target it is building for — the triple, because a file gate names the
// DERIVED `ARCH_`/`OS_`/`ABI_` flags, and those are read off the triple rather than off the name.
// (A built-in catalog name deliberately never becomes a flag; see derivedTargetFlags.)
static std::string activeTargetLabel()
{
    if (g_target.arch.empty()) return "this build";
    const std::string triple = g_target.arch + "-" + g_target.os + "-" + g_target.abi;
    if (!g_target.name.empty() && g_target.name != triple) return g_target.name + " (" + triple + ")";
    return triple;
}

static std::map<std::string, TargetSpec> g_manifestTargets;
// The project-level `link` list. Seeded into g_target after target resolution, unless the selected
// target overrode it (TargetSpec::linkSet).
static bool g_manifestWebgpu = false, g_manifestNoHeap = false, g_manifestReproFloat = false;

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
        if (!u.runtime.empty()) out.runtime = u.runtime;
        if (!u.subsystem.empty()) out.subsystem = u.subsystem;
        out.cflags.insert(out.cflags.end(),  u.cflags.begin(),  u.cflags.end());
        out.ldflags.insert(out.ldflags.end(), u.ldflags.begin(), u.ldflags.end());
        if (u.linkSet)   { out.link   = u.link;   out.linkSet   = true; }   // replace, per the note in the reader
        if (u.webgpuSet) { out.webgpu = u.webgpu; out.webgpuSet = true; }
        if (u.noHeapSet) { out.noHeap = u.noHeap; out.noHeapSet = true; }
        if (u.reproFloatSet) { out.reproFloat = u.reproFloat; out.reproFloatSet = true; }
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
    // ...and where that source lives on disk, so go-to-definition on `Optional` opens the declaration
    // instead of landing nowhere. The unit keeps its synthetic `<prelude>` NAME — see builtinSourcePath.
    e.setPrelude(preludeUnit(), builtinSourcePath("<prelude>"));   // Optional/Result available implicitly
    // ...and a place for the names that have no source at all — see builtinDocIndex.
    e.setBuiltinDoc(builtinSourcePath("<builtin>"), builtinDocIndex());
    e.setNoHeap(g_noHeap);             // `--no-heap`: reject heap allocation program-wide
    e.setStrictNumeric(g_strictNumeric);   // `--strict-numeric` (M5a): measure numeric hand-offs
    e.setProbeReport(g_probeTemplates);    // `--probe-templates`: measure the uninstantiated-template walk
    e.setRelease(g_release);           // `--release`: strip `debugAssert`
    e.setSharedModule(g_outputShared); // `OUTPUT=SHARED`: define the runtime slots in a module with no `main`
    e.setBuildFlags(g_activeFlags, g_declaredFlags, g_strictFlags);   // `@compileFor` conditional compilation
    e.setLogDefault(g_logDefault);     // baked `KAMA_LOG` project default (M5), compiled into main
    e.setForeignRoots({ absolutePath(resolveStdlibDir(g_argv0)), absolutePath(storeDir()) });   // KR-38 attribution
    // Which PACKAGE owns a given source file. The emitter needs this only to name both sides when two
    // packages claim the same conformance, so it is a callback rather than a precomputed per-unit table:
    // the walk is filesystem work the emitter has no business doing, and it runs at most once per error.
    // A synthetic unit (`<prelude>`, `<prelude>/std/…`) has no path — `dirName` would hand back "." and
    // the walk would climb into whatever project happens to be the working directory, attributing the
    // prelude's conformances to the user.
    e.setPackageResolver([](const std::string& unitPath) -> std::string {
        if (unitPath.empty() || unitPath[0] == '<') return std::string();
        std::string dir = owningPackageDir(dirName(unitPath));
        return dir.empty() ? std::string() : dir + "/kama.json";
    });
    // Which MODULE owns a given source file — the file's identity, and what the emitter mangles its
    // declarations with (SPEC.md § Modules). Same callback shape and the same reason: walking to
    // the owning manifest and reading its module tree is filesystem work, cached driver-side.
    //
    // ⚠️ The synthetic arm is not a "return nothing" guard the way setPackageResolver's is. The
    // smart-pointer triad reaches the emitter as `<prelude>/std/memory/*.kama` — units with no path at all,
    // which no path→module derivation can reach — and it DOES go through ctxOf. Its module is therefore
    // stated rather than derived, in KamaPreludeModule::module (src/kama.prelude.h).
    //
    // What that header warns of — drop this and `std__memory__Owned` becomes `_F<file>__Owned` — was a
    // PREDICTION about phase 2e through 2c and 2d, and false at the time: a compiler built with this arm
    // returning "" stayed entirely green, because lib/std/memory/*.kama still declared `namespace
    // std::memory` and ctxOf's declaration rung caught them. 2e deleted both, so it is now TRUE, and
    // measured again rather than assumed: with this arm returning "" the triad loses its identity and a
    // one-line `new int32()` program no longer builds — `Owned` stops satisfying its own bounds. This is
    // the ONLY thing naming the embedded modules now.
    e.setModuleResolver([](const std::string& unitPath) -> std::string {
        if (unitPath.empty()) return std::string();
        if (unitPath[0] == '<') {
            for (int i = 0; i < KAMA_PRELUDE_MODULE_COUNT; ++i)
                if (KAMA_PRELUDE_MODULES[i].name && unitPath == KAMA_PRELUDE_MODULES[i].name)
                    return KAMA_PRELUDE_MODULES[i].module ? KAMA_PRELUDE_MODULES[i].module : "";
            return std::string();          // `<prelude>` itself: the floor, scoped by collectProgram
        }
        return moduleIdForFile(unitPath).full();      // "" for a loose file — no kama.json above it
    });
    // The visibility rung (§2c). The driver answers the PREDICATE rather than exporting `ModuleVis`, for
    // the same reason it answers `setModuleResolver`: this is manifest work, and `ModuleNode`/`ModuleVis`
    // are static to this file and appear in no header. Both arguments are full module names as
    // `ModuleId::full()` writes them; "" means a file in no module.
    e.setModuleVisible([](const std::string& importer, const std::string& imported) -> bool {
        return kamaModuleVisibleTo(importer, imported);
    });
    // The always-in-scope triad, each with the `lib/std/memory/*.kama` it was embedded from — a file
    // that ships in every install, unlike the global prelude.
    for (auto& m : preludeModuleUnits())
        e.addPreludeModule(m, builtinSourcePath(m && m->name ? *m->name : std::string()));
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

// One node of `kama.json`'s nested `modules` map (SPEC.md § Modules).
//
// A module is a FOLDER that the manifest lists; folders without an entry stay invisible, so nothing
// joins the API by accident. The map MIRRORS the folder tree — each key is one path segment, a folder
// directly inside its parent — and **a module's name is the chain of keys read down to it**. That is the
// invariant the whole section rests on: a sibling entry cannot change a module's name, because the name
// is the path you literally read, not something inferred from which other entries happen to exist. (A
// flat map with `/` keys would infer it, so adding an unrelated `"serialization"` entry would silently
// rename `serialization/json`'s public API.)
//
// `"."` is the project root — the files under `source` that are in no module. It is the root's real
// path rather than a reserved word, so no folder can collide with it and it needs no escape rule.
enum class ModuleVis {
    List,        // ["a", "b"] — its own files plus the modules NAMED a and b. At least one entry.
    Children,    // its own files plus every module nested under it, at any depth
    Internal,    // its own files plus every module in this project
    Public,      // all of the above plus dependent projects
};

struct ModuleNode {
    std::string key;                        // the folder segment as written ("." for the project root)
    std::string name;                       // `name` override for this one segment, or "" — never `::`-joined
    ModuleVis   vis = ModuleVis::Public;
    std::vector<std::string> visibleTo;     // the `List` form, in file order (empty unless vis == List)
    std::vector<ModuleNode>  children;      // this node's own `modules`

    // Filled by validateModules, not by the parser: the composed identity. Kept beside the node so every
    // consumer reads one answer rather than re-deriving the chain and risking a different join.
    std::string modName;                    // "collections::detail"; "" for the root node
    std::string relPath;                    // "collections/detail" under `source`; "" for the root node

    // The segment this node contributes to its module name — `name` when overridden, else the key.
    const std::string& segment() const { return name.empty() ? key : name; }
    bool isRoot() const { return key == "."; }
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

// The compiler's own MAJOR.MINOR.PATCH. `KAMA_VERSION` is `0.9.204+g<sha>` in a development build (the
// Makefile appends the sha) and `parseSemVer` rejects `+`/`-` by design, so cut there first. False for a
// build that reports no version at all (`0.0.0-dev`, the no-Makefile fallback).
static bool compilerSemVer(SemVer& out)
{
    std::string v = KAMA_VERSION;
    size_t cut = v.find_first_of("+-");
    if (cut != std::string::npos) v = v.substr(0, cut);
    return parseSemVer(v, out);
}

// The manifest's `kama` key: the RANGE of compilers a package's source needs, checked wherever the package
// is built — `kama pkg install` for the root and every fetched dependency, `kama build` for the root and
// every dependency in the view (docs/packages.md § What compiler a package needs). `who` names the
// package. Empty means the key is absent; an unparsable range never reaches here (the reader refuses it).
static bool kamaReqSatisfied(const std::string& req, const std::string& who, std::string& err)
{
    if (req.empty()) return true;
    VersionReq vr; SemVer me;
    if (!parseVersionReq(req, vr) || !compilerSemVer(me)) return true;
    if (satisfies(vr, me)) return true;
    err = who + " needs kama " + req + " (`\"kama\"` in its manifest); this is kama " KAMA_VERSION
        + std::string(" — `kama update`, or pin a newer compiler with `kama toolchain`");
    return false;
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

// One `-s<KEY>=<value>` emscripten setting. The JSON SHAPE decides which of the two kinds it is, which
// is the whole reason `emSettings` is an object rather than an array of raw `-sFOO=1` strings: emcc is
// last-wins on a repeated `-s`, so kama's own `-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8` silently
// beat any project that set the same key through `cflags`. A merge needs comparable keys, and a raw
// string array is just `cflags` with extra steps.
//
// A JSON ARRAY is a LIST setting and UNIONS (kama's values, then dependencies', then the project's);
// anything else is a SCALAR and is last-wins, with the project winning over kama and over every
// dependency. So the project can say `"EXPORTED_RUNTIME_METHODS": ["ccall"]` and get its own name
// AND the two the stdlib's JS glue needs.
struct EmValue {
    bool isList = false;
    std::vector<std::string> list;   // isList
    std::string scalar;              // !isList — already rendered (`true` -> "1", a number verbatim)
};

// Everything ONE manifest contributes to the C command line, with its own project/target rules already
// applied. Joined across manifests by the dependency walk in resolveBuildConfig, where the ordering and
// merge rules are written out.
struct BuildSettings {
    std::string owner;        // "" for the root project, else the dependency's import name
    std::vector<std::string> cflags, ldflags, link;
    std::vector<std::string> csources, jsLibraries;   // as written, relative to `manifestPath`
    std::vector<std::string> cincludes;               // include DIRECTORIES, likewise relative
    std::vector<std::pair<std::string, EmValue>> emSettings;
    // The one boolean that travels. `no-heap` and `webgpu` are whole-artifact decisions a dependency
    // must not make for its consumer (one changes what compiles, the other demands an SDK) and are read
    // from the root only. This one can only turn contraction OFF, and it states a requirement of the
    // dependency's OWN arithmetic — which the consumer compiles. Shipped in 0.9.169 grouped with the
    // other two, and the first consumer's raw `-ffp-contract=off` cflag (which propagates) would have
    // silently lost its guarantee on migrating to the key.
    bool reproFloat = false;
    std::string kamaReq;      // the `kama` compiler-version range this manifest declares ("" = none)
};

// One resolved C source: who declared it, and where it actually is. `owner` is what keeps two packages
// shipping `shim.c` from writing the same object file (and racing for it under `-j`).
struct CSourceRef { std::string owner, path; };
static std::vector<CSourceRef> g_csources;
// `cincludes`, resolved the same way: a package's include DIRECTORIES. A raw relative `-I` in a
// dependency's cflags is refused (it would resolve against the consumer's cwd), and a header beside a
// `.c` is found through that entry's own directory — this is the structured route for the case in
// between: a library whose headers live in their own `include/` tree, which is most of them.
static std::vector<CSourceRef> g_cincludes;
// The emscripten pair, resolved and merged across every manifest in the build. Both are inert on a
// non-wasm target; the SHAPE is still validated everywhere, because the same manifest builds both.
static std::vector<CSourceRef> g_jsLibraries;
// Ordered, so the command line is stable and a conflict names its settings in manifest order. `owner`
// on each entry is the manifest that last set it — a scalar conflict between two dependencies is
// refused, and the message needs both names.
struct EmSetting {
    std::string key, owner;
    EmValue value;
    // Two DEPENDENCIES setting one scalar differently. Recorded rather than reported on the spot,
    // because the project merges LAST and stating the setting itself is exactly how a consumer settles
    // it — reporting during the dependency walk would refuse a manifest that had already answered.
    std::string conflictOwner, conflictValue;
};
static std::vector<EmSetting> g_emSettings;

// ...and everything else the ROOT manifest contributes to the C command line, as written. `link` is
// seeded into g_target after target resolution unless the target overrode it; `cflags`/`ldflags` PREPEND
// onto whatever the target carries, because the target tier appends (project-then-target, so the more
// specific list gets the last word); the path-valued lists are resolved against the manifest's own
// directory once the build configuration is settled.
static BuildSettings g_rootSettings;

struct ManifestReader {
    const std::string& s;
    size_t i = 0;
    std::string err;
    std::set<std::string>& declared;
    std::set<std::string>& defaults;
    std::map<std::string, DepSpec>* deps = nullptr;      // set to capture `dependencies` (else it's skipped)
    std::map<std::string, DepSpec>* devDeps = nullptr;   // set to capture `dev-dependencies` (else skipped)
    std::string* entryOut = nullptr;                      // set to capture the `entry` field (else skipped)
    std::string* kindOut = nullptr;                       // set to capture `kind` ("library"|"executable")
    std::string* sourceOut = nullptr;                      // set to capture `source`, the one source root
    std::vector<std::string>* linkOut = nullptr;           // set to capture the project-level `link`
    // The project-level twins of the per-target `cflags`/`ldflags`. They exist for the reason `link`
    // sits on the project too — a setting that is true of the ARTIFACT, not of one target — and,
    // decisively, because a DEPENDENCY cannot know how its consumer spells the target: `select.TARGET`
    // is matched by NAME (resolveTarget), and `--target aarch64-linux-gnu` resolves as an anonymous
    // triple matching no manifest entry at all. A dep with only per-target settings would contribute
    // nothing to such a build, which would leave dependency propagation half-dead on arrival.
    std::vector<std::string>* cflagsOut = nullptr;          // set to capture the project-level `cflags`
    std::vector<std::string>* ldflagsOut = nullptr;         // set to capture the project-level `ldflags`
    std::vector<std::string>* csourcesOut = nullptr;        // set to capture `csources` (the project's own C)
    std::vector<std::string>* jsLibrariesOut = nullptr;      // set to capture `jsLibraries` (emscripten --js-library)
    std::vector<std::string>* cincludesOut = nullptr;        // set to capture `cincludes` (include directories)
    // A VECTOR, not a map: the file's own order is the order a conflict names its settings in, and the
    // order they reach the command line.
    std::vector<std::pair<std::string, EmValue>>* emSettingsOut = nullptr;
    std::vector<ModuleNode>* modulesOut = nullptr;         // set to capture the nested `modules` map (§2b)
    bool* webgpuOut = nullptr;                            // set to capture the project-level `webgpu`
    bool* noHeapOut = nullptr;                            // set to capture the project-level `no-heap`
    bool* reproFloatOut = nullptr;                        // set to capture `reproducible-float`
    // `kama_workspace.json` mode: a DIFFERENT file with a different key set, read by the same parser
    // rather than a second one. `projects` there is a MAP whose every entry states `optional`, and the
    // only other legal key is `dependencies` — so the mode is a flag, not a sink, because it changes
    // which keys are recognized at all.
    bool workspaceFile = false;
    // A vector, not a map: the file's own order is the order a validation error names members in.
    std::vector<std::pair<std::string, bool>>* wsProjectsOut = nullptr;
    std::string* outDirOut = nullptr;                     // set to capture the `out` build-output dir (else skipped)
    std::string* toolchainOut = nullptr;                  // set to capture the `toolchain` pin (else skipped)
    std::string* kamaReqOut = nullptr;                    // set to capture the `kama` compiler-version RANGE (validated either way)
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
    bool fail(const std::string& m) { if (err.empty()) err = m; return false; }

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

    // A JSON array of strings (`projects`). Rejects a non-array or a non-string element rather than
    // tolerating it: this one declares what the tooling may rewrite, so a typo must not silently widen
    // or narrow the set.
    // `key` names the offending key in the message. It is optional only because the module reader's
    // `visibleTo` predates it; every other caller passes one, because "expected a JSON array" against a
    // manifest with eight array-valued keys tells the reader nothing about which one they got wrong.
    bool stringArray(std::vector<std::string>& out, const char* key = nullptr) {
        const std::string where = key ? std::string(" for `") + key + "`" : std::string();
        ws(); if (i >= s.size() || s[i] != '[') return fail("expected a JSON array of strings" + where);
        ++i; ws();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            std::string v;
            if (!str(v)) { err.clear(); return fail("expected a string in the array" + where); }
            out.push_back(v);
            ws();
            if (i < s.size() && s[i] == ',') { ++i; ws(); continue; }
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            return fail("expected ',' or ']' in the array" + where);
        }
    }

    // One `csources` entry. `.c` and nothing else, for now, and the refusals say why rather than
    // leaving the author to find out from the C compiler.
    //
    // C++ is refused BY NAME because it is a real gap and not an oversight: kama hands every input the
    // same flag prefix (`-std=c11` and the C-only warning promotions), so a C++ TU would need its own,
    // and linking one needs the target's C++ runtime library, which varies per target. `.m`/`.mm` are
    // refused for a third reason — an Objective-C source is inherently one-platform, and `csources` is
    // deliberately project-level with no per-target tier to exclude it from a wasm build.
    // The path rules every file-naming manifest key shares: inside the package, and relative to the
    // manifest that names it — so the project stays relocatable and a published package cannot name a
    // directory nobody else has.
    bool validRelPath(const std::string& p, const char* key) {
        const std::string k = std::string("`") + key + "`";
        if (p.empty()) return fail(k + " has an empty path");
        if (p[0] == '/' || (p.size() > 1 && p[1] == ':'))
            return fail(k + " path \"" + p + "\" is absolute; it must be relative to the manifest "
                        "that declares it, or the project stops being relocatable (and a published "
                        "package would name a directory nobody else has)");
        if (p.find("..") != std::string::npos)
            return fail(k + " path \"" + p + "\" escapes the project with `..`; a package's own files "
                        "belong inside it");
        return true;
    }

    bool validCSource(const std::string& p) {
        if (!validRelPath(p, "csources")) return false;
        const size_t dot = p.rfind('.');
        const std::string ext = dot == std::string::npos ? std::string() : p.substr(dot);
        if (ext == ".c") return true;
        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++" || ext == ".mm")
            return fail("`csources` compiles C only today, and \"" + p + "\" is C++. Two things are "
                        "missing: every input shares one flag prefix (`-std=c11` plus the C-only warning "
                        "promotions), and a C++ link needs the target's C++ runtime library. Build it "
                        "outside kama and name the objects through `ldflags` for now");
        if (ext == ".m")
            return fail("`csources` compiles C only today, and \"" + p + "\" is Objective-C — which is "
                        "one platform's language, while `csources` is project-wide with no per-target "
                        "tier to exclude it from your other targets. Guard the C with `#ifdef __APPLE__` "
                        "instead");
        return fail("`csources` names C source files (`.c`), and \"" + p + "\" is not one");
    }

    // `emSettings`: { "<KEY>": <array of strings | string | number | boolean> }. The value's SHAPE is
    // the declaration of what it means (see EmValue), so there is no table of known emscripten keys to
    // keep in step with emscripten — a project states which kind it is, and kama's own settings declare
    // themselves the same way.
    bool emSettingsObject(std::vector<std::pair<std::string, EmValue>>& out) {
        ws(); if (i >= s.size() || s[i] != '{') return fail("`emSettings` must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string k; if (!str(k)) return false;
            if (k.empty()) return fail("`emSettings` has an empty setting name");
            for (const auto& kv : out)
                if (kv.first == k) return fail("`emSettings` names `" + k + "` twice");
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after an `emSettings` name");
            ++i; ws();
            EmValue v;
            if (i < s.size() && s[i] == '[') {
                v.isList = true;
                if (!stringArray(v.list, "emSettings")) return false;
            } else if (i < s.size() && s[i] == '"') {
                if (!str(v.scalar)) return false;
            } else if (s.compare(i, 4, "true") == 0)  { v.scalar = "1"; i += 4; }
            else if (s.compare(i, 5, "false") == 0)   { v.scalar = "0"; i += 5; }
            else {
                // A bare number, captured verbatim so `4` and `4.0` reach emcc as written.
                size_t start = i;
                while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i]=='-' || s[i]=='+' ||
                                        s[i]=='.' || s[i]=='e' || s[i]=='E')) ++i;
                if (i == start)
                    return fail("`emSettings.\"" + k + "\"` must be an array of strings, a string, a "
                                "number or a boolean");
                v.scalar = s.substr(start, i - start);
            }
            out.push_back({ k, v });
            ws();
            if (i < s.size() && s[i] == ',') { ++i; ws(); continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `emSettings`");
        }
        return true;
    }

    // A JSON boolean. Its own reader because `optional` is REQUIRED and closed: `"optional": "no"` must
    // be refused by name rather than read through skipValue as though it had said something.
    bool boolean(bool& out) {
        ws();
        if (s.compare(i, 4, "true")  == 0) { i += 4; out = true;  return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; out = false; return true; }
        return fail("expected `true` or `false`");
    }

    // `kama_workspace.json`'s `projects`: { "<path-or-glob>": { "optional": <bool> }, ... }.
    //
    // A MAP rather than the array `kama.json` used to carry, because every entry now states something:
    // whether the workspace is still well-formed when that member is not checked out. There is no bare
    // `{}` and no default — a project is the smallest shippable unit, so a partial checkout is routine
    // (submodules, role-scoped trees), and which members may be absent is the first thing a reader of
    // this file wants to know. `optional` is required on a GLOB too, where it asks whether matching
    // nothing is allowed: `libz/*` expanding to zero directories is the same class of typo as a missing
    // path, and silence would swallow it.
    bool wsProjectsObject() {
        ws(); if (i >= s.size() || s[i] != '{') return fail("`projects` must be a JSON object");
        ++i; ws(); if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            std::string path; if (!str(path)) return false;
            ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' after a project path");
            ++i; ws();
            if (i >= s.size() || s[i] != '{')
                return fail("the value for `" + path + "` must be an object stating \"optional\"");
            ++i; ws();
            bool optional = false, sawOptional = false;
            if (i < s.size() && s[i] == '}') ++i;
            else while (true) {
                std::string k; if (!str(k)) return false;
                ws(); if (i >= s.size() || s[i] != ':') return fail("expected ':' in a project entry");
                ++i;
                // Closed, like the top level: an unknown key here is a swallowed decision about a member.
                if (k != "optional") return fail("unknown key `" + k + "` in the entry for `" + path + "`");
                if (!boolean(optional)) return false;
                sawOptional = true;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}' in the entry for `" + path + "`");
            }
            if (!sawOptional)
                return fail("`" + path + "` does not say whether it is \"optional\" — every workspace "
                            "entry states it, because a partial checkout is routine");
            if (wsProjectsOut) wsProjectsOut->push_back({ path, optional });
            ws();
            if (i < s.size() && s[i] == ',') { ++i; ws(); continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' in `projects`");
        }
        return true;
    }

    // One module node's `visibility` (§2c.17). Four forms, one widening ladder: a LIST of the modules
    // that may import this one, then `children` (my subtree), `internal` (this project), `public` (the
    // world). Each keyword rung exists because it names a set a list cannot track — `children` and
    // `internal` both grow as modules are added.
    //
    // The empty list is refused for a sharper reason than "it is degenerate": a module nobody can import
    // is unreachable, so it can only be dead code — no call path from `main` enters it. With `[]` gone,
    // the manifest ALONE proves there is no unreachable module in the project, with no call-graph
    // analysis. (`"children"` on a leaf is the same argument and is caught in validateModules, which is
    // the first place that knows whether a node has children.)
    bool visibilityValue(ModuleNode& n, const std::string& where) {
        ws();
        if (i < s.size() && s[i] == '[') {
            if (!stringArray(n.visibleTo, "visibleTo")) return false;
            if (n.visibleTo.empty())
                return fail("`visibility` for `" + where + "` is an empty list, so nothing could ever "
                            "import it — an unimportable module can only be dead code. Name at least one "
                            "module, or use \"internal\" or \"public\"");
            n.vis = ModuleVis::List;
            return true;
        }
        std::string v;
        if (!str(v)) return false;
        if      (v == "children") n.vis = ModuleVis::Children;
        else if (v == "internal") n.vis = ModuleVis::Internal;
        else if (v == "public")   n.vis = ModuleVis::Public;
        else return fail("`visibility` for `" + where + "` must be \"public\", \"internal\", \"children\", "
                         "or a list of the modules that may import it, not \"" + v + "\"");
        return true;
    }

    // `kama.json`'s nested `modules` map (§2b): { "<segment>": { "visibility": …, "name": …,
    //                                                            "modules": { … } }, … }
    //
    // The FIRST self-recursive reader in this file, and deliberately so: every other nested object here
    // (`select`, `flags`, `projects`, `log`) has a fixed depth someone wrote out by hand, but a module
    // tree is as deep as a source tree. `depth` is a backstop against a pathological or hand-edited file
    // spinning the parser, not a design limit — eight levels is deeper than any real `src/`.
    //
    // `where` is the key path read so far, so an error names the node it is about rather than leaving the
    // reader to count braces. Modelled on wsProjectsObject above: a map of closed-key objects, per-entry
    // errors that name the entry, and a required field enforced by a `saw` flag.
    bool modulesObject(std::vector<ModuleNode>& out, const std::string& where, int depth) {
        if (depth > 8)
            return fail("`modules` nests more than 8 deep at `" + where + "` — a module tree mirrors a "
                        "folder tree, so this is almost certainly a malformed manifest");
        ws(); if (i >= s.size() || s[i] != '{')
            return fail(where.empty() ? std::string("`modules` must be a JSON object")
                                      : "`modules` for `" + where + "` must be a JSON object");
        // An EMPTY map is refused, at every depth. A `modules` that lists nothing decides nothing, which
        // is exactly what omitting the key used to mean — and since the key is required (below, in
        // resolveBuildConfig), accepting `{}` would leave the requirement satisfiable by a decorative
        // key. That is the rot 1c hit with `entry`. Same argument as §2c's ban on an empty `visibility`
        // list: a node that declares no audience and a map that declares no module are both dead text.
        ++i; ws(); if (i < s.size() && s[i] == '}')
            return fail(where.empty()
                ? std::string("`modules` is empty — every project lists at least its root module, `\".\"` "
                              "(the files directly under `source`)")
                : "`modules` for `" + where + "` is empty — either drop the key or list the folders inside "
                  "that module");
        while (true) {
            ModuleNode n;
            if (!str(n.key)) return false;
            const std::string here = where.empty() ? n.key : where + "." + n.key;
            ws(); if (i >= s.size() || s[i] != ':')
                return fail("expected ':' after the module key `" + here + "`");
            ++i; ws();
            // Every entry is an OBJECT, never a bare value: a shorthand would drop the key that says what
            // the value MEANS, and the moment a second key is wanted the format would have to support
            // both spellings forever. The object is the extension point.
            if (i >= s.size() || s[i] != '{')
                return fail("the value for the module `" + here + "` must be an object stating "
                            "\"visibility\"");
            ++i; ws();
            bool sawVisibility = false;
            if (i < s.size() && s[i] == '}') ++i;
            else while (true) {
                std::string k; if (!str(k)) return false;
                ws(); if (i >= s.size() || s[i] != ':')
                    return fail("expected ':' in the module `" + here + "`");
                ++i;
                // Closed, like the top level: an unknown key inside a module entry is a swallowed
                // visibility decision, which is the whole reason unknown keys became an error in 1a.
                if (k == "visibility") { if (!visibilityValue(n, here)) return false; sawVisibility = true; }
                else if (k == "name") {
                    if (!str(n.name)) return false;
                    // A `::`-joined `name` would smuggle hierarchy past the nesting — the one thing the
                    // nested map exists to prevent, since the name would stop being the chain of keys.
                    if (n.name.find("::") != std::string::npos)
                        return fail("`name` for the module `" + here + "` is \"" + n.name + "\", but a "
                                    "`name` overrides ONE segment — nest the map instead of joining with "
                                    "`::`");
                    // The root's identity IS the project name (§2b.12), so there is nothing here to
                    // override — and a `name` on it would be a second, hidden spelling of the project.
                    if (n.key == ".")
                        return fail("`\".\"` is the project root and takes no `name` — its identity is the "
                                    "project's own `name`");
                }
                else if (k == "modules") { if (!modulesObject(n.children, here, depth + 1)) return false; }
                else return fail("unknown key `" + k + "` in the module `" + here + "`");
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}' in the module `" + here + "`");
            }
            // Required on EVERY node, including one whose folder holds no `.kama` files today. Making it
            // conditional on file presence would mean ADDING A SOURCE FILE INVALIDATES THE MANIFEST —
            // the wrong coupling entirely. A folder that groups today can hold code tomorrow, and its
            // answer should already be written down.
            if (!sawVisibility)
                return fail("the module `" + here + "` does not state a `visibility` — every module "
                            "declares its audience, including one whose folder holds no `.kama` files yet");
            // The SEGMENT — `name` when overridden, else the key — becomes a segment of the module's
            // name, so it has to be spellable in an `import`. Checked here rather than at the key,
            // because a folder named `my-lib` is not an error in itself: the `name` override is exactly
            // the escape for it (§2b.10), and keys may arrive before or after it inside the node.
            if (!n.isRoot() && !kamaIsIdentifier(n.segment()))
                return fail(n.name.empty()
                    ? "`" + n.key + "` is not a legal kama identifier, so it cannot be a segment of a "
                      "module name — give that node a `name` that is"
                    : "`name` for the module `" + here + "` is \"" + n.name + "\", which is not a legal "
                      "kama identifier");
            out.push_back(std::move(n));
            ws();
            if (i < s.size() && s[i] == ',') { ++i; ws(); continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail(where.empty() ? std::string("expected ',' or '}' in `modules`")
                                      : "expected ',' or '}' in `modules` for `" + where + "`");
        }
        return true;
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
                // How the language's own runtime links. VALIDATED rather than tolerated, unlike the
                // unknown keys below: a typo here ("shared", "dyn") would otherwise read as "not dynamic"
                // and silently give you the default you were trying to change.
                else if (k == "runtime")  {
                    if (!str(t.runtime)) return false;
                    if (t.runtime != "static" && t.runtime != "dynamic")
                        return fail("a target's `runtime` must be \"static\" or \"dynamic\"");
                }
                // The PE subsystem. VALIDATED for the same reason `runtime` is: "gui" or "win32" would
                // otherwise read as "not windows" and hand back the console default being changed —
                // and the symptom (a stray console window) looks like the feature never shipped.
                else if (k == "subsystem") {
                    if (!str(t.subsystem)) return false;
                    if (t.subsystem != "console" && t.subsystem != "windows")
                        return fail("a target's `subsystem` must be \"console\" or \"windows\"");
                }
                else if (k == "cflags")  { if (!stringArray(t.cflags,  "cflags"))  return false; }
                else if (k == "ldflags") { if (!stringArray(t.ldflags, "ldflags")) return false; }
                // A target OVERRIDES the project's `link` rather than adding to it — that is what
                // "overridable" means, and it is the only way to say "not on this target". Note this is
                // the opposite of `cflags`/`ldflags` below, which APPEND onto the built-in they merge
                // over; the two have always differed and the difference is deliberate.
                else if (k == "link")    { if (!stringArray(t.link, "link")) return false; t.linkSet = true; }
                else if (k == "webgpu")  { if (!boolean(t.webgpu)) return false; t.webgpuSet = true; }
                else if (k == "no-heap") { if (!boolean(t.noHeap)) return false; t.noHeapSet = true; }
                else if (k == "reproducible-float") {
                    if (!boolean(t.reproFloat)) return false; t.reproFloatSet = true;
                }
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
            // RECOGNITION is unconditional; only STORAGE is sink-guarded. Every caller sets one or two
            // sinks and leaves the rest null, so folding the two together — `key == "sources" && sourcesOut`
            // — sent a known key the caller did not ask for down the same path as a typo. That is why
            // rejecting unknown keys could not simply be bolted onto the final `else`: it would have
            // rejected `name` whenever the reader was loading `sources`. Split, every one of the eleven
            // loadManifest* wrappers validates the WHOLE file for free.
            //
            // `kama_workspace.json` is a different file, so it gets its own closed key set here rather
            // than a shared one with per-key mode checks. It holds `projects` and `dependencies` and
            // nothing else — no `name`, no `version`, no `toolchain`: a workspace is not a project, has
            // nothing to be a library or executable OF, and §2a's extractability invariant (a project
            // never reads its workspace file for anything affecting compilation) rules out a pin here.
            if (workspaceFile) {
                if (key == "projects") { if (!wsProjectsObject()) return false; }
                else if (key == "dependencies") { if (deps) { if (!depsObject(deps)) return false; } else if (!skipValue()) return false; }
                // `modules` gets its own refusal rather than falling into the generic one below: a
                // workspace has no `source` and no root module, so there is nothing for a module map
                // to be relative TO — and putting one here would make a project's identity depend on a
                // file §2a's extractability invariant says it must never read.
                else if (key == "modules")
                    return fail("`modules` belongs in a project's kama.json, not in kama_workspace.json — "
                                "a workspace has no `source` and no root module for a module to hang "
                                "off, and a project must build identically whether or not this file is here");
                else return fail("`" + key + "` does not belong in kama_workspace.json, which holds only "
                                 "`projects` and `dependencies` — a workspace is not a project");
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; break; }
                return fail("expected ',' or '}' at top level");
            }
            if (key == "flags") { if (!flagsObject()) return false; }
            else if (key == "select") { if (!selectObject()) return false; }   // build-config groups
            else if (key == "dependencies") { if (deps) { if (!depsObject(deps)) return false; } else if (!skipValue()) return false; }
            else if (key == "dev-dependencies") { if (devDeps) { if (!depsObject(devDeps)) return false; } else if (!skipValue()) return false; }
            else if (key == "registries") { if (registriesOut) { if (!registriesObject(registriesOut)) return false; } else if (!skipValue()) return false; }
            else if (key == "overrides") { if (overridesOut) { if (!depsObject(overridesOut)) return false; } else if (!skipValue()) return false; }  // kama.local.json dep path-overrides (M5.3)
            else if (key == "log") { if (logOut) { if (!logObject()) return false; } else if (!skipValue()) return false; }   // baked log default (M5)
            // One source root, not a list. A list would let `src/shapes/` and `gen/shapes/` silently be
            // one module, and there is nowhere in the model to say which of them a name came from.
            else if (key == "source") { if (sourceOut) { if (!str(*sourceOut)) return false; } else if (!skipValue()) return false; }
            else if (key == "link") { if (linkOut) { if (!stringArray(*linkOut, "link")) return false; } else if (!skipValue()) return false; }
            // The project tier of `cflags`/`ldflags`. A target's list APPENDS onto these (the same
            // direction a target appends onto the built-in it merges over), so the order on the command
            // line is project-then-target and the more specific one gets the last word.
            else if (key == "cflags") { if (cflagsOut) { if (!stringArray(*cflagsOut, "cflags")) return false; } else if (!skipValue()) return false; }
            else if (key == "ldflags") { if (ldflagsOut) { if (!stringArray(*ldflagsOut, "ldflags")) return false; } else if (!skipValue()) return false; }
            // The project's own C sources, compiled alongside the C kama emits. PARSED and VALIDATED
            // unconditionally, like `modules` and unlike the sink-guarded keys above: the value set is
            // closed (a `.c` path, relative to this manifest), and a swallowed entry would be a
            // translation unit silently missing from the link — which surfaces as an undefined symbol
            // with nothing pointing back at the manifest that named it.
            else if (key == "csources") {
                std::vector<std::string> scratch;
                std::vector<std::string>& into = csourcesOut ? *csourcesOut : scratch;
                if (!stringArray(into, "csources")) return false;
                for (const std::string& p : into) if (!validCSource(p)) return false;
            }
            // The emscripten pair. VALIDATED on every target, not just wasm — the same manifest builds
            // both, and a typo caught only under `--target wasm` is a typo found by whoever ships to the
            // web rather than by whoever wrote it. APPLIED only on wasm (the `subsystem` precedent:
            // an accepted no-op, so one build script carries it).
            else if (key == "jsLibraries") {
                std::vector<std::string> scratch;
                std::vector<std::string>& into = jsLibrariesOut ? *jsLibrariesOut : scratch;
                if (!stringArray(into, "jsLibraries")) return false;
                for (const std::string& p : into) if (!validRelPath(p, "jsLibraries")) return false;
            }
            // Include directories, under the path rules every file-naming key shares (inside the
            // package, relative to the manifest that names them). Whether each is actually a directory
            // is checked where the command line is built, which is the one place that can name the
            // declaring package too.
            else if (key == "cincludes") {
                std::vector<std::string> scratch;
                std::vector<std::string>& into = cincludesOut ? *cincludesOut : scratch;
                if (!stringArray(into, "cincludes")) return false;
                for (const std::string& p : into) if (!validRelPath(p, "cincludes")) return false;
            }
            else if (key == "emSettings") {
                std::vector<std::pair<std::string, EmValue>> scratch;
                if (!emSettingsObject(emSettingsOut ? *emSettingsOut : scratch)) return false;
            }
            // The module map (§2b). RECOGNIZED unconditionally and parsed even when nothing captures it,
            // unlike the sink-guarded keys above: its errors are the point. A swallowed `modules` would be
            // a swallowed visibility decision, which is precisely what unknown-keys-are-an-error exists to
            // stop, and skipValue() cannot see inside a nested object.
            else if (key == "modules") {
                std::vector<ModuleNode> scratch;
                if (!modulesObject(modulesOut ? *modulesOut : scratch, "", 1)) return false;
            }
            // Booleans are VALIDATED here rather than only where they are consumed, for the same reason
            // `kind` is: the value set is closed, so `"no-heap": "yes"` must be refused by name rather
            // than read as some truthiness nobody wrote down.
            else if (key == "webgpu")  { bool b = false; if (!boolean(b)) return false; if (webgpuOut) *webgpuOut = b; }
            else if (key == "no-heap") { bool b = false; if (!boolean(b)) return false; if (noHeapOut) *noHeapOut = b; }
            // Same shape and the same reason as the two above: a closed value set, validated in the
            // reader so `"reproducible-float": "yes"` is refused by name rather than read as some
            // truthiness nobody wrote down.
            else if (key == "reproducible-float") { bool b = false; if (!boolean(b)) return false; if (reproFloatOut) *reproFloatOut = b; }
            // Moved out, not dropped. A monorepo root carrying `projects` was a manifest that looked like
            // a project while being an aggregator with no `kind`, no `source`, no namespace and no
            // artifact — the one shape every other rule here needed an exemption for. The workspace is a
            // scope ABOVE the project now, with its own file, and a project does not nest.
            else if (key == "projects")
                return fail("`projects` now lives in kama_workspace.json at the monorepo root — a "
                            "workspace is not a project. Move it there, one entry per member, each "
                            "stating \"optional\"");
            else if (key == "entry") { if (entryOut) { if (!str(*entryOut)) return false; } else if (!skipValue()) return false; }   // entry `.kama` (read by `kama run`)
            // Validated HERE rather than only where it is consumed, because the value set is closed and a
            // caller that does not read `kind` would otherwise let `"kind": "libary"` through unexamined.
            else if (key == "kind") {
                std::string k;
                if (!str(k)) return false;
                if (k != "library" && k != "executable")
                    return fail("`kind` must be \"library\" or \"executable\", not \"" + k + "\"");
                if (kindOut) *kindOut = k;
            }
            else if (key == "out") { if (outDirOut) { if (!str(*outDirOut)) return false; } else if (!skipValue()) return false; }   // build output root (default "out")
            // The pre-1.0 spelling of `entry`. Rejected HERE rather than skipped-and-remembered, so every
            // command says so and not just `kama run` — npm's `main` names a library's entry point for
            // importers, kama's names the `kama run` target, and silently ignoring it meant a manifest that
            // visibly declared an entry behaved as though it had none.
            else if (key == "main")
                return fail("`main` is now `entry` — npm's `main` names a library's entry point for "
                            "importers, kama's names the `kama run` target. Rename the key");
            else if (key == "toolchain") { if (toolchainOut) { if (!str(*toolchainOut)) return false; } else if (!skipValue()) return false; }   // pin (read by the selector)
            // `toolchain` pins which compiler RUNS for a project; `kama` is the range a PACKAGE's source
            // needs, checked for the root and every dependency at install and at build (kamaReqSatisfied).
            // Validated HERE like `kind`: the value set is closed (a version range), and a caller that
            // does not capture it would otherwise let `"kama": "latest"` through unexamined.
            else if (key == "kama") {
                std::string rq;
                if (!str(rq)) return false;
                VersionReq vr;
                if (!parseVersionReq(rq, vr))
                    return fail("`kama` must be a compiler version range — `>=0.9.200`, `^1.2.0`, `~1.2.0`, an "
                                "exact version, or `*` — not \"" + rq + "\"");
                if (kamaReqOut) *kamaReqOut = rq;
            }
            // A project's `name` is its ROOT NAMESPACE (§2a.2), so it must be spellable in an `import`.
            // Validated HERE rather than only where it is consumed, for the reason `kind` and the two
            // booleans are: the value set is closed, and a caller that does not read `name` would
            // otherwise let a manifest through that no importer could ever name.
            //
            // `kama seed` has refused a hyphenated name since 1a, with the reasoning written out at
            // seedValidName — `import my-lib::{…}` is a parse error, so the name is a dead end and
            // refusing it beats shipping one. This is the same rule reaching the manifest, which is
            // where a project that was not seeded gets read.
            //
            // The SCOPE is exempt: `@my-org/geo` imports under its bare last segment, so the hyphen in
            // an npm-style scope is registry routing and never reaches a namespace. importNameOf is the
            // same function the dependency view names its directories with.
            else if (key == "name") {
                std::string nm;
                if (!str(nm)) return false;
                const std::string ident = importNameOf(nm);
                if (!kamaIsIdentifier(ident)) {
                    std::string hint = ident;
                    for (char& c : hint) if (c == '-') c = '_';
                    return fail("`name` is \"" + nm + "\", but a project's name is its root module and `"
                                + ident + "` is not a legal kama identifier — nothing could write `import "
                                + ident + "::{ … }`"
                                + (kamaIsIdentifier(hint) ? ". Try \"" + hint + "\"" : ""));
                }
                // `global` is the always-in-scope floor (§2f.29), reserved by exactly this rule rather
                // than by separate machinery — which is the point: there is no third kind of scope, only
                // a project name nobody else may claim. Refused HERE, not only at `kama seed`, because a
                // project that was not seeded is read here and nowhere else.
                //
                // ⚠️ `std` and `core` are reserved too, and they are NOT refused here — measured, not
                // reasoned: adding them refuses `lib/kama.json`, whose `name` IS "std", and the whole
                // standard library stops resolving. Nothing legitimate is ever named `global` (the
                // prelude is embedded and has no manifest at all, §2f.29 as corrected), which is exactly
                // what makes it the one of the three this rung can hold down. seedValidName refuses all
                // three, where "am I creating a new project?" is the question being asked.
                if (ident == "global")
                    return fail("`name` is \"" + nm + "\", which is reserved: `global` names the "
                                "always-in-scope floor, whose symbols are visible unqualified in every "
                                "file, so a project claiming it would collide with all of them");
                if (nameOut) *nameOut = nm;
            }
            else if (key == "version") { if (versionOut) { if (!str(*versionOut)) return false; } else if (!skipValue()) return false; }
            // `license`: an SPDX identifier ("MIT"), written by `kama seed --license` and read by nobody in
            // the compiler — it is registry metadata, the way npm's and cargo's are. Held to a STRING so a
            // typo'd shape is caught here like every other key; the identifier itself is not validated,
            // because the SPDX list is not a table kama should carry.
            else if (key == "license") { std::string lic; if (!str(lic)) return false; }
            else return fail("unknown key `" + key + "`");
            ws();
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            if (i < s.size() && s[i] == '}') { ++i; break; }
            return fail("expected ',' or '}' at top level");
        }
        return true;
    }
};

// ---- the module map's cross-cutting checks (§2b) ----------------------------------------------------
//
// Separate from the parser because each of these needs the WHOLE map, not one entry: a composed name is
// only unique against every other composed name, `"children"` is only wrong once you know a node has no
// children, and a `visibility` list can only be checked against the set of modules that exist. The
// parser stays local and these run once after it, which is also why they can fill in `modName`/`relPath`
// — every consumer then reads one answer instead of re-deriving the key chain.

// Walk the tree in file order, composing each node's name and path from the chain of keys read down to
// it. `out` collects every composed name so the second pass can resolve visibility lists.
static bool composeModules(std::vector<ModuleNode>& nodes, const std::string& nameSoFar,
                           const std::string& pathSoFar, const std::string& projectName,
                           std::map<std::string, ModuleNode*>& byName, std::string& err)
{
    for (auto& n : nodes) {
        if (n.isRoot()) {
            // `"."` is the project root — the files under `source` in no module — so it contributes no
            // segment and no path component. It is only meaningful at the top level.
            if (!nameSoFar.empty()) {
                err = "`\".\"` names the PROJECT root, so it belongs at the top of `modules`, not inside `"
                      + nameSoFar + "`";
                return false;
            }
            n.modName.clear();
            n.relPath.clear();
        } else {
            n.modName = nameSoFar.empty() ? n.segment() : nameSoFar + "::" + n.segment();
            n.relPath = pathSoFar.empty() ? n.key       : pathSoFar + "/" + n.key;
        }

        // Two paths colliding on one identity. Reachable through a `name` override or a literal duplicate
        // key, and it has to be caught: two modules answering to one name means an `import` picks one of
        // them by parse order, which is exactly the class of silent wrong answer §2b exists to remove.
        const std::string shown = n.modName.empty() ? std::string("\".\"") : "`" + n.modName + "`";
        auto it = byName.find(n.modName);
        if (it != byName.end()) {
            err = "two modules compose to the same name " + shown + " — `" + it->second->relPath
                + "` and `" + n.relPath + "`. A module's name is the chain of keys read down to it, so "
                  "give one of them a `name`";
            return false;
        }
        // `global::X` is the floor, always in scope with no import (§2f). A module answering to that name
        // could never be reached by the spelling it claims.
        if (n.modName == "global") {
            err = "a module may not be named `global` — that names the always-in-scope floor, so nothing "
                  "could ever reach this module by the name it claims";
            return false;
        }
        // `geo::X` would name both the root of project `geo` and its module `geo`, and nothing in the
        // spelling says which. The root's identity IS the project name (§2b.12), so the module yields.
        if (!projectName.empty() && n.modName == projectName) {
            err = "the module `" + n.relPath + "` composes to `" + projectName + "`, which is this "
                  "project's own name — `" + projectName + "::X` would name both its root and this "
                  "module. Give it a `name`";
            return false;
        }
        // An empty subtree is an empty audience, so `"children"` on a leaf grants access to nobody —
        // the same argument that bans the empty list, and the other half of what lets the manifest alone
        // prove there is no unreachable module.
        if (n.vis == ModuleVis::Children && n.children.empty()) {
            err = "`visibility` for the module " + shown + " is \"children\", but it has no nested "
                  "modules — an empty subtree is an empty audience, so nothing could import it";
            return false;
        }
        byName[n.modName] = &n;
        if (!composeModules(n.children, n.modName, n.relPath, projectName, byName, err)) return false;
    }
    return true;
}

// Every module named by a `visibility` list must exist. A typo here fails OPEN — the module quietly
// grants access to nobody it meant to — so silence would be the worst possible outcome.
static bool checkVisibilityTargets(const std::vector<ModuleNode>& nodes,
                                   const std::map<std::string, ModuleNode*>& byName, std::string& err)
{
    for (auto& n : nodes) {
        const std::string shown = n.modName.empty() ? std::string("\".\"") : "`" + n.modName + "`";
        for (auto& target : n.visibleTo) {
            if (byName.count(target)) {
                // A module's own files always see each other, so the list is ADDITIVE and never names
                // itself. Saying so anyway is a misunderstanding worth naming rather than ignoring.
                if (target == n.modName) {
                    err = "`visibility` for the module " + shown + " names itself — a module's own files "
                          "always see each other, so the list only names OTHER modules";
                    return false;
                }
                continue;
            }
            err = "`visibility` for the module " + shown + " names `" + target + "`, which is not a "
                  "module in this project — a list may only name modules declared in this `modules` map";
            return false;
        }
        if (!checkVisibilityTargets(n.children, byName, err)) return false;
    }
    return true;
}

// The whole post-parse pass. `projectName` is the manifest's `name`; pass "" only where it is genuinely
// unknown, which costs the root-collision check and nothing else.
static bool validateModules(std::vector<ModuleNode>& mods, const std::string& projectName,
                            std::string& err)
{
    std::map<std::string, ModuleNode*> byName;
    if (!composeModules(mods, "", "", projectName, byName, err)) return false;
    return checkVisibilityTargets(mods, byName, err);
}

// ---- file -> module identity (§2b/§2e), continued from the ModuleId declaration above --------------
//
// The parsed module map per manifest. Rides the same lifetime as manifestSourceCache and is cleared by
// the same eviction: `kama lsp` treats a manifest edit as "the program being analyzed changed", so a map
// cached anywhere else would serve stale modules across exactly that edit.
struct ManifestModules {
    std::string             projectName;   // already run through importNameOf
    std::vector<ModuleNode> mods;
    bool                    ok = false;    // false: unreadable or rejected — attribute nothing
};

static std::map<std::string, ManifestModules>& manifestModulesCache()
{
    static std::map<std::string, ManifestModules> cache;
    return cache;
}

static const ManifestModules& manifestModulesCached(const std::string& manifest)
{
    auto& cache = manifestModulesCache();
    auto it = cache.find(manifest);
    if (it != cache.end()) return it->second;
    ManifestModules m;
    std::string err, rawName;
    if (fileExists(manifest)) {
        std::ifstream in(osp(manifest), std::ios::binary);
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::set<std::string> declared, defaults;
        ManifestReader r(src, declared, defaults);
        r.modulesOut = &m.mods;
        r.nameOut    = &rawName;
        // A manifest that does not parse is not an error HERE — this runs on the resolution and editor
        // paths, which stay lenient about a broken manifest somewhere up the tree. resolveBuildConfig is
        // where a build says so out loud.
        if (r.parse()) {
            m.projectName = importNameOf(rawName);
            m.ok = validateModules(m.mods, m.projectName, err);
        }
        if (!m.ok) m.mods.clear();
    }
    return cache.emplace(manifest, std::move(m)).first->second;
}

// The deepest listed node whose folder is an ancestor of (or is) `relDir`, or nullptr for the project
// root. NEAREST ANCESTOR is the whole rule: a folder with no entry is not a module, but its files are
// not homeless either — they belong to the closest folder above them that IS one. That is what keeps
// "list a folder" an API decision rather than a compilation one.
static const ModuleNode* deepestModuleFor(const std::vector<ModuleNode>& nodes, const std::string& relDir)
{
    const ModuleNode* best = nullptr;
    for (auto& n : nodes) {
        if (n.isRoot() || n.relPath.empty()) continue;
        // A path-COMPONENT prefix, never a string prefix: `net` must not claim `network/x.kama`.
        if (relDir != n.relPath &&
            !(relDir.size() > n.relPath.size() && relDir.compare(0, n.relPath.size(), n.relPath) == 0
              && relDir[n.relPath.size()] == '/'))
            continue;
        best = &n;
        if (const ModuleNode* deeper = deepestModuleFor(n.children, relDir)) best = deeper;
        break;   // siblings are disjoint paths, so at most one can match
    }
    return best;
}

// The module `absPath` belongs to. `inProject` false means no kama.json above it, or one whose `source`
// root does not contain it (a vendored project's own files answer to THEIR manifest, not this one), or
// a LOOSE build, where no manifest participates at all and the operand set names the modules.
static ModuleId moduleIdForFile(const std::string& absPath)
{
    ModuleId id;
    const std::string abs = absolutePath(absPath);
    // §2i, and the reason it is tested FIRST: for a file this build was HANDED, a `kama.json` above it is
    // not this build's manifest — it is not any build's manifest, because a loose build has none. An
    // operand is named by its folder. Everything else (the stdlib, a dependency, a module pulled in to
    // satisfy an import) is reached rather than named, and keeps the identity its OWN project gives it,
    // which is what makes `std::collections` still `std::collections` in a loose build.
    if (g_looseBuild && g_looseOperands.count(abs)) { id.module = looseModuleFor(abs); return id; }

    const std::string dir = owningPackageDir(dirName(absPath));
    if (dir.empty()) return id;
    const std::string manifest = dir + "/kama.json";
    const std::string& srcRel = manifestSourceCached(manifest);
    if (srcRel.empty()) return id;                       // unreadable: not a project root as far as we know

    const std::string srcRoot = absolutePath(joinPathLexical(dir, srcRel));
    if (abs.size() <= srcRoot.size() || abs.compare(0, srcRoot.size(), srcRoot) != 0
        || abs[srcRoot.size()] != '/')
        return id;                                        // outside `source` — the manifest does not own it

    const ManifestModules& mm = manifestModulesCached(manifest);
    id.inProject = true;
    id.project   = mm.projectName;

    std::string relDir = dirName(abs.substr(srcRoot.size() + 1));
    if (relDir == "." ) relDir.clear();
    if (!relDir.empty())
        if (const ModuleNode* n = deepestModuleFor(mm.mods, relDir)) id.module = n->modName;
    // Remember which manifest owns this module name. There is no name->manifest index in the driver —
    // `manifestModulesCached` is keyed by manifest PATH — and building one by searching would mean
    // guessing which roots to search. Every unit in the compilation passes through here, so recording it
    // on the way past is both complete and free. Read by `kamaModuleVisibleTo`.
    moduleManifestIndex()[id.full()] = manifest;
    return id;
}

// The stem of the `.c` a unit emits to, derived from the unit's IDENTITY rather than its position in the
// argument list (§2e.26). The old form was `<basename>_<index>`, and the index made `--keep-c` output
// depend on the order the operands were typed: the same three-file program emitted `main_0.c x_1.c x_2.c`
// one way and `x_0.c x_1.c main_2.c` the other. The `_N` was not decoration — it was guarding the case
// where two source files share a basename — so it can only go once something path-derived replaces it.
//
// A module IS a folder, so basenames inside one are unique by construction, and `ModuleId::full()` carries
// the project name — `geo::shapes` and `app::shapes` cannot collide. That leaves exactly one ambiguous
// population: files with no module at all, i.e. the loose ROOT (§2e.27), whose symbols are unimportable
// anyway. Two of those sharing a basename is the collision the `_N` used to absorb, and the caller reports
// it by name instead.
static std::string cStemForUnit(const std::string& unitPath)
{
    // Non-identifier characters cannot appear in a C symbol and would be silently dropped by some tools,
    // so fold them to `_`. That can map two distinct names onto one stem (`my-prog` and `my_prog`), which
    // is exactly what the caller's duplicate check is for.
    auto sanitize = [](const std::string& s) {
        std::string r;
        for (char c : s) r += (isalnum((unsigned char)c) || c == '_') ? c : '_';
        return r;
    };
    const std::string stem = sanitize(stripExtension(baseName(unitPath)));
    if (unitPath.empty() || unitPath[0] == '<') return stem;   // synthetic: no path to derive from
    std::string mod = moduleIdForFile(unitPath).full();
    if (mod.empty()) return stem;                              // the loose root — basename is all there is
    for (size_t p; (p = mod.find("::")) != std::string::npos; ) mod.replace(p, 2, "__");
    return mod + "__" + stem;
}

// ---- module name -> files: the inverse of the above (§2b) ------------------------------------------

// The node whose composed name IS `want` ("net::web"), anywhere in the tree.
static const ModuleNode* nodeByModName(const std::vector<ModuleNode>& nodes, const std::string& want)
{
    for (auto& n : nodes) {
        if (!n.isRoot() && n.modName == want) return &n;
        if (const ModuleNode* d = nodeByModName(n.children, want)) return d;
    }
    return nullptr;
}

// THE VISIBILITY RUNG (§2c). `visibility` governs reach BEYOND a module and nothing else — the file rung
// (`export` out, `import` in) has already had its say by the time this is asked.
//
// Both arguments are FULL module names as `ModuleId::full()` writes them: `project::a::b`, or a bare
// project name for a root module, or a bare folder chain for a LOOSE module, or "" for a file in no
// module at all.
//
// ⚠️ NO NODE MEANS ALLOW, and it is the common case rather than a fallback: a loose module has no manifest
// by design (§2i), the prelude is embedded, and `<prelude>/…` units are synthetic. Denying those would
// break every single-file build in the corpus.
//
// ⚠️ A LOOSE IMPORTER IS IN NO PROJECT, so `internal`/`children`/a list can never grant it — only `public`
// can. That is not a special case either: it falls out of "same project" being false. In practice a loose
// build resolves only the stdlib and $KAMA_PATH, and `lib/kama.json` is `public` throughout.
static std::string projectOfModule(const std::string& full)
{
    const size_t c = full.find("::");
    return c == std::string::npos ? full : full.substr(0, c);
}
static std::string relativeModuleName(const std::string& full)
{
    const size_t c = full.find("::");
    return c == std::string::npos ? std::string() : full.substr(c + 2);
}

bool kamaModuleVisibleTo(const std::string& importer, const std::string& imported)
{
    if (imported.empty() || importer == imported) return true;   // its own files always see each other

    auto it = moduleManifestIndex().find(imported);
    if (it == moduleManifestIndex().end()) return true;          // no manifest declares it — see above
    const ManifestModules& mm = manifestModulesCached(it->second);
    if (!mm.ok) return true;                                     // unreadable: attribute nothing

    const std::string rel = relativeModuleName(imported);
    const ModuleNode* node = nullptr;
    if (rel.empty()) {                                           // the project ROOT module, `"."`
        for (auto& n : mm.mods) if (n.isRoot()) { node = &n; break; }
    } else {
        node = nodeByModName(mm.mods, rel);
    }
    if (!node) return true;                                      // not declared: nothing to enforce

    if (node->vis == ModuleVis::Public) return true;
    // Every remaining form is bounded by the project, so a different project — or none — cannot qualify.
    // A LOOSE importer is caught here too rather than by a case of its own: its "project" is its own
    // folder chain, which cannot equal the imported module's project.
    if (projectOfModule(importer) != projectOfModule(imported)) return false;
    switch (node->vis) {
        case ModuleVis::Internal: return true;
        case ModuleVis::Children: {
            // Nested UNDER it, at any depth. `rel` is "" for a root node, which `composeModules` already
            // refuses `children` on, so an empty prefix cannot match everything by accident.
            const std::string mine = relativeModuleName(importer);
            return rel.size() < mine.size() && mine.compare(0, rel.size(), rel) == 0
                && mine.compare(rel.size(), 2, "::") == 0;
        }
        case ModuleVis::List: {
            const std::string mine = relativeModuleName(importer);
            for (auto& t : node->visibleTo) if (t == mine) return true;
            return false;
        }
        default: return true;
    }
}

// The files of the module `segs` inside the project rooted at `projDir`, or empty if that project is not
// the one being named or does not have such a module.
//
// The file set is derived rather than listed, by asking moduleIdForFile about every candidate: RECURSIVE
// under the module's folder, because an unlisted subfolder's files belong to the nearest listed ancestor
// (deepestModuleFor's rule), and minus anything a deeper LISTED module claims for itself. Reusing the
// derivation is the point — a second implementation of "which module owns this file?" is exactly the
// disagreement between two spellings that this campaign exists to remove.
static std::vector<std::string> moduleFilesInProject(const std::string& projDir,
                                                     const std::vector<std::string>& segs)
{
    const std::string manifest = projDir + "/kama.json";
    if (!fileExists(manifest)) return {};
    const ManifestModules& mm = manifestModulesCached(manifest);
    if (mm.projectName.empty() || mm.projectName != segs[0]) return {};   // a different project's name
    const std::string& srcRel = manifestSourceCached(manifest);
    if (srcRel.empty()) return {};

    // LEXICAL, never realpath: this becomes the UNIT NAME, and resolving `.kama/deps/<name>` away would
    // put go-to-definition in the content-addressed store instead of the view the user can see. Same
    // reason packageSourceFiles spells it this way.
    std::string dir = joinPathLexical(projDir, srcRel);
    std::string want = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) want += "::" + segs[i];
    if (segs.size() > 1) {
        const ModuleNode* n = nodeByModName(mm.mods, want.substr(segs[0].size() + 2));
        if (!n) return {};                                 // no such module in this project's map
        dir += "/" + n->relPath;
    }
    if (!dirExists(dir)) return {};

    std::vector<std::string> all, out;
    size_t seen = 0;
    collectKamaFiles(dir, "", all, seen, (size_t)-1);
    for (auto& f : all) if (moduleIdForFile(f).full() == want) out.push_back(f);
    std::sort(out.begin(), out.end());                     // readdir order is not deterministic
    return out;
}

std::vector<std::string> resolveModuleFiles(const std::vector<std::string>& segs,
                                            const std::vector<std::string>& roots,
                                            std::string* matchedRoot)
{
    if (matchedRoot) matchedRoot->clear();
    if (segs.empty()) return {};
    for (const auto& root : roots) {
        // A root is either a project itself (the stdlib, or the project being built) or a directory OF
        // projects (a dependency view, a $KAMA_PATH entry), so both spellings are tried. Which one it was
        // is not worth distinguishing: a project answers only to its own `name` either way.
        for (const std::string& p : { root, root + "/" + segs[0] }) {
            std::vector<std::string> files = moduleFilesInProject(p, segs);
            if (!files.empty()) { if (matchedRoot) *matchedRoot = root; return files; }
        }
    }
    return {};
}

// A module that does not resolve used to be one line — the name, and the directory the search had
// started from. There is no search now, so "from <dir>" named a place nothing had looked; and the three
// ways to arrive here want three different answers. §2i's own words, in the order a user meets them.
static void reportUnresolvedModule(const std::string& name, const std::vector<std::string>& segs,
                                   const std::string& fromFile, const std::string& buildManifestDir,
                                   const std::string& stdlibDir, bool reserved)
{
    fprintf(stderr, "kama: error: cannot resolve module '%s' (imported by %s)\n",
            name.c_str(), fromFile.c_str());
    if (reserved) {
        fprintf(stderr, "kama: note: `std` and `core` resolve only from the standard library, which this "
                        "compiler reads at %s — an install ships `kama.json` beside `std/`, and without "
                        "it nothing there can be imported\n", stdlibDir.c_str());
        return;
    }
    // A project build: the manifest is in hand, so the answer is which of its two lists is missing an
    // entry. Its own name means a folder nobody listed; anything else means a dependency nobody declared.
    if (!buildManifestDir.empty()) {
        const std::string manifest = buildManifestDir + "/kama.json";
        const ManifestModules& mm = manifestModulesCached(manifest);
        if (!mm.projectName.empty() && mm.projectName == segs[0])
            fprintf(stderr, "kama: note: `%s` is this project, but %s lists no module `%s` — a folder is a "
                            "module only once it is listed under \"modules\"\n",
                    segs[0].c_str(), manifest.c_str(), name.c_str());
        else
            fprintf(stderr, "kama: note: %s declares no dependency named `%s` — add it under "
                            "\"dependencies\" and run `kama pkg install`\n", manifest.c_str(), segs[0].c_str());
        return;
    }
    // A loose build, where the operands ARE the compilation (§2i). Nothing was looked for, so the useful
    // thing to say is that — and, when the file turns out to sit in a project, that naming the manifest
    // is the whole fix. The walk is affordable here because this path ends the build.
    fprintf(stderr, "kama: note: this build names no `kama.json`, so its modules are the files on the "
                    "command line and their folders — and none of them is in `%s`. Name that module's "
                    "sources too, or build a project\n", name.c_str());
    const std::string owner = owningPackageDir(dirName(fromFile));
    if (!owner.empty())
        fprintf(stderr, "kama: note: %s belongs to the project %s/kama.json — `kama build %s/kama.json` "
                        "compiles it with the modules that manifest declares\n",
                fromFile.c_str(), owner.c_str(), owner.c_str());
}

// Load a `kama.json` manifest → the `select.TARGET` catalog. Reuses ManifestReader (unknown keys
// tolerated), so this is orthogonal to the flag load.
static bool loadManifestTargets(const std::string& path, std::map<std::string, TargetSpec>& out,
                                std::map<std::string, SelectGroup>& groupsOut, std::string& err,
                                std::string* defaultTargetOut = nullptr)
{
    std::ifstream in(osp(path), std::ios::binary);
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
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ManifestReader r(src, declared, defaults);
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `dependencies` (name -> DepSpec). Reuses ManifestReader (unknown keys
// tolerated), so this is orthogonal to the flag load. Returns false + sets `err` on malformed JSON.
static bool loadManifestDeps(const std::string& path, std::map<std::string, DepSpec>& deps, std::string& err,
                             std::map<std::string, DepSpec>* devDeps = nullptr, RegConfig* reg = nullptr,
                             std::string* kamaReq = nullptr)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.deps = &deps;
    r.devDeps = devDeps;   // optional: also capture `dev-dependencies` (M2.2)
    r.registriesOut = reg; // optional: also capture `registries` config (M3.1b)
    r.kamaReqOut = kamaReq; // optional: also capture the `kama` compiler range (install checks it)
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
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.overridesOut = &overrides;
    r.registriesOut = &reg;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `entry` field (the entry `.kama`, relative to the manifest). Reuses
// ManifestReader; `entryOut` is left empty if the field is absent. Returns false + sets `err` on malformed
// JSON — which now includes a manifest still spelling the key `main`, rejected by the reader itself.
// (M2.3 — read by `kama run` when no file is passed.)
static bool loadManifestEntry(const std::string& path, std::string& entryOut, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.entryOut = &entryOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `out` build-output root, relative to the manifest. Left empty if absent,
// which the caller reads as the default "out". Returns false + sets `err` only on malformed JSON.
static bool loadManifestOutDir(const std::string& path, std::string& outDir, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.outDirOut = &outDir;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's project-level `link` — the native libraries this artifact links, as bare
// names (`["m"]` -> `-lm`). Left empty if absent. Returns false + `err` on malformed JSON.
static bool loadManifestArtifactFlags(const std::string& path, bool& webgpu, bool& noHeap,
                                      bool& reproFloat, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.webgpuOut = &webgpu; r.noHeapOut = &noHeap; r.reproFloatOut = &reproFloat;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// The project tier of the three flag lists that reach the C compiler: `link`, `cflags`, `ldflags`.
// ONE wrapper rather than three, because they are read together everywhere — the root manifest here and
// (with the target tier merged in) every dependency's, through loadBuildSettings.
static bool loadManifestProjectFlags(const std::string& path, BuildSettings& out, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.linkOut = &out.link; r.cflagsOut = &out.cflags; r.ldflagsOut = &out.ldflags;
    r.csourcesOut = &out.csources; r.jsLibrariesOut = &out.jsLibraries; r.cincludesOut = &out.cincludes;
    r.emSettingsOut = &out.emSettings;
    r.reproFloatOut = &out.reproFloat;
    r.kamaReqOut = &out.kamaReq;
    if (!r.parse()) {
        err = r.err.empty() ? "malformed JSON" : r.err;
        out = BuildSettings();
        return false;
    }
    return true;
}

// Load a `kama.json` manifest's `kind` — "library" or "executable", the two things a project can be.
// REQUIRED of a project manifest, so an empty result on success means the key is absent and the caller
// must reject it; the value itself is validated by the reader. Returns false + `err` on malformed JSON.
static bool loadManifestKind(const std::string& path, std::string& kindOut, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.kindOut = &kindOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `toolchain` pin (the version the selector should run for this project).
// `tcOut` is left empty if the field is absent. Returns false + sets `err` only on malformed JSON. Reused by
// the PATH selector — a cheap read of one key, done *before* any compiler runs. (M1.)
static bool loadManifestToolchain(const std::string& path, std::string& tcOut, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.toolchainOut = &tcOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// Load a `kama.json` manifest's `source` — THE one directory holding this project's `.kama` files,
// relative to the manifest. `out` is left empty when the key is absent, which means the default `"src"`;
// applying that default is manifestSourceCached's job, because only a caller that knows the manifest
// EXISTS may apply it (see the contract there). Returns false + `err` on malformed JSON or a source root
// that is not a plain subdirectory name.
//
// `source` must name a real SUBDIRECTORY. `"."` is rejected rather than merely discouraged: it would put
// the manifest itself, `.kama/deps`, `out/` and any vendored project INSIDE the source root, and the rule
// that no `kama.json` may live under `source` would then need an exemption for each of them. A
// subdirectory keeps all four structurally outside it and the rule exemption-free.
static bool loadManifestSource(const std::string& path, std::string& out, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.sourceOut = &out;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; out.clear(); return false; }
    if (out.empty()) return true;                       // absent: the caller applies the default

    const std::string bad =
        out == "." || out == ".."                          ? "`source` must name a subdirectory, not \"" + out + "\""
      : out[0] == '/' || (out.size() > 1 && out[1] == ':') ? "`source` must be relative to the manifest, not an absolute path"
      : out.find("..") != std::string::npos               ? "`source` may not reach outside the project with `..`"
      : std::string();
    if (!bad.empty()) { err = bad + " (the default is \"src\")"; out.clear(); return false; }
    return true;
}

// Load a `kama.json` → its module map (§2b), composed and checked. An absent `modules` yields an empty
// vector and is not an error TODAY: `visibility` is form-checked only until the visibility phase, so a
// map that decides nothing yet is not worth a corpus-wide edit to require. It becomes required in the
// same change that deletes the `namespace` declaration, which is when the map turns load-bearing — the
// only way left to name a module.
//
// `name` is read here rather than by the caller because one of the checks is about it (a module may not
// compose to the project's own name), and reading it separately would mean two parses of one file.
static bool loadManifestModules(const std::string& path, std::vector<ModuleNode>& out, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    std::string projectName;
    ManifestReader r(src, declared, defaults);
    r.modulesOut = &out;
    r.nameOut    = &projectName;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; out.clear(); return false; }
    // A scoped package imports under its bare last segment, so THAT is the root module a module could
    // collide with — `@acme/geo` is imported as `geo`.
    if (!validateModules(out, importNameOf(projectName), err)) { out.clear(); return false; }
    return true;
}

// Load a `kama_workspace.json` — the members this monorepo composes, relative to the file, each a
// directory holding its own kama.json, and each stating whether it may be absent. A trailing `/*` expands
// to every immediate subdirectory that has a manifest (`"libs/*"`), so a workspace need not be edited per
// project. Declaring this is what turns "is this a monorepo root?" from something the LSP infers from
// position into something the repository states.
//
// NOTE the key name: `packages` was rejected because kama.lock already uses that word for resolved
// DEPENDENCIES — packages are what you consume, projects are what you compose. (LSP M3.5.)
//
// `dependencies` here are BUILD-TIME tooling, built for the host rather than the target being
// cross-compiled to (§2d.23). Parsed and validated so the file's schema is whole and a typo is caught;
// nothing consumes them yet.
static bool loadWorkspace(const std::string& path, std::vector<std::pair<std::string, bool>>& out,
                          std::map<std::string, DepSpec>* depsOut, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.workspaceFile = true;
    r.wsProjectsOut = &out;
    r.deps          = depsOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; out.clear(); return false; }
    return true;
}

// Load a `kama.json` (or `kama.local.json`) manifest's `log` config into `out` (left default if absent).
// Reuses ManifestReader. Returns false + `err` on malformed JSON or an invalid level name. (M5.)
static bool loadManifestLog(const std::string& path, LogConfig& out, std::string& err)
{
    std::ifstream in(osp(path), std::ios::binary);
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
    std::ifstream in(osp(path), std::ios::binary);
    if (!in) { err = "cannot open '" + path + "'"; return false; }
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> declared, defaults;   // unused here
    ManifestReader r(src, declared, defaults);
    r.nameOut = &nameOut; r.versionOut = &versionOut;
    if (!r.parse()) { err = r.err.empty() ? "malformed JSON" : r.err; return false; }
    return true;
}

// A package's own import name, cached like `declaredImportNames` and read from the same manifest. Used
// by the undeclared-import rule to tell an intra-package import from a free-ride: a file reaching across
// its OWN package's modules spells its own package name, which no `dependencies` entry will ever contain.
const std::string& ownImportName(const std::string& packageDir)
{
    static std::map<std::string, std::string> cache;
    auto it = cache.find(packageDir);
    if (it != cache.end()) return it->second;
    std::string name, version, err;
    if (loadManifestNameVersion(packageDir + "/kama.json", name, version, err) && !name.empty())
        name = importNameOf(name);
    else
        name.clear();
    return cache.emplace(packageDir, std::move(name)).first->second;
}


// ---- what ONE manifest contributes to the C command line --------------------------------------------
//
// A dependency's `cflags`/`ldflags`/`link` used to be read NOWHERE: at build time a dep's kama.json was
// consulted for its `source`, its `name` and its `dependencies`, and for nothing else. So every consumer
// had to repeat the block, and drift between the copies was silent — the first external project repeats
// it in two packages today.
//
// The rule is per-manifest resolution, then concatenation. Each manifest resolves its own settings under
// today's precedence (project tier, then the selected target's; a target's `link` REPLACES that
// manifest's own project `link`), and only then are manifests joined. Resolving globally instead would
// let a DEPENDENCY's `select.TARGET.<T>.link` delete the consumer's `-lm` — i.e. adding a dependency
// could break your link, which is not what "not that one, here" was ever meant to say.

// Resolve one manifest's contribution for target `targetName`. `targetName` is the RESOLVED target's
// name (g_target.name), which is empty for an anonymous triple — a dep then contributes its project tier
// only, and cannot contribute a per-target list at all. That is the honest answer rather than a
// surprising one: a target is matched by name, and a dependency does not know how you spell yours.
static bool loadBuildSettings(const std::string& manifestPath, const std::string& targetName,
                              BuildSettings& out, std::string& err)
{
    if (!loadManifestProjectFlags(manifestPath, out, err)) return false;
    if (targetName.empty()) return true;
    std::map<std::string, TargetSpec>  targets;
    std::map<std::string, SelectGroup> groups;     // ignored: a dep declares no flag universe of ours
    if (!loadManifestTargets(manifestPath, targets, groups, err)) return false;
    auto t = targets.find(targetName);
    if (t == targets.end()) return true;
    out.cflags.insert(out.cflags.end(),   t->second.cflags.begin(),  t->second.cflags.end());
    out.ldflags.insert(out.ldflags.end(), t->second.ldflags.begin(), t->second.ldflags.end());
    if (t->second.linkSet) out.link = t->second.link;   // replaces THIS manifest's own list, nobody else's
    if (t->second.reproFloatSet) out.reproFloat = t->second.reproFloat;   // same: this manifest's own tier
    return true;
}

// A dependency's relative `-I`/`-L`/`-isystem` resolves against the CONSUMER's working directory, not
// against the dependency — so it either fails loudly or, worse, silently finds the consumer's own
// `include/`. Refused rather than propagated: the supported route for a path is a structured key kama
// resolves against the declaring manifest. The ROOT's own relative paths are untouched — those are the
// consumer's business and have always been CWD-relative.
static bool depFlagPathIsPortable(const std::vector<std::string>& flags, const std::string& owner,
                                  const std::string& manifestPath, std::string& err)
{
    static const char* kPathFlags[] = { "-I", "-L", "-isystem", "-iquote", "-idirafter" };
    for (size_t i = 0; i < flags.size(); ++i) {
        const std::string& f = flags[i];
        for (const char* pf : kPathFlags) {
            const size_t n = strlen(pf);
            if (f.compare(0, n, pf) != 0) continue;
            // Either `-Ifoo` (joined) or `-I foo` / `-isystem foo` (the next element).
            std::string arg = f.size() > n ? f.substr(n) : (i + 1 < flags.size() ? flags[i + 1] : std::string());
            while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
            if (arg.empty() || arg[0] == '/' || (arg.size() > 1 && arg[1] == ':')) break;   // absolute: their call
            err = "dependency `" + owner + "` declares a relative path in its build flags (`" + f + "`),\n"
                  "      which would resolve against YOUR working directory rather than against\n"
                  "      " + manifestPath + ". Make it absolute.";
            return false;
        }
    }
    return true;
}

// Merge one manifest's `emSettings` over what is already there. `owner` is "" for the root project.
//
// A LIST unions (kama's values are already in place, then dependencies', then the project's), so a
// project asking for `EXPORTED_RUNTIME_METHODS: ["ccall"]` keeps the two names the stdlib's JS glue
// needs instead of silently replacing them — which is exactly what happened when the only way to say
// this was a raw `-s` in `cflags` and emcc took the last one.
//
// A SCALAR is last-wins, and the merge order makes that "the project wins". Two DEPENDENCIES setting
// one scalar differently is refused by name: picking the alphabetically-later package would be a silent
// answer to a question only the consumer can settle — and stating it in the root settles it.
static bool mergeEmSettings(const std::vector<std::pair<std::string, EmValue>>& in,
                            const std::string& owner, std::string& err)
{
    for (const auto& kv : in) {
        EmSetting* have = nullptr;
        for (auto& e : g_emSettings) if (e.key == kv.first) { have = &e; break; }
        if (!have) { g_emSettings.push_back({ kv.first, owner, kv.second }); continue; }
        if (have->value.isList != kv.second.isList) {
            err = "emSettings `" + kv.first + "` is a list in one manifest and a single value in "
                  "another (" + (have->owner.empty() ? std::string("this project")
                                                     : "dependency `" + have->owner + "`") + " vs " +
                  (owner.empty() ? std::string("this project") : "dependency `" + owner + "`") +
                  ") — one shape or the other, not both";
            return false;
        }
        if (kv.second.isList) {
            for (const std::string& v : kv.second.list) {
                bool dup = false;
                for (const std::string& x : have->value.list) if (x == v) { dup = true; break; }
                if (!dup) have->value.list.push_back(v);
            }
            continue;
        }
        // A scalar. The project wins outright and clears any conflict two dependencies could not settle
        // between themselves; two dependencies disagreeing is remembered for emScalarConflict() to
        // report once the project has had its say.
        if (owner.empty()) {
            have->value = kv.second; have->owner = owner;
            have->conflictOwner.clear(); have->conflictValue.clear();
            continue;
        }
        if (!have->owner.empty() && have->value.scalar != kv.second.scalar && have->conflictOwner.empty()) {
            have->conflictOwner = owner; have->conflictValue = kv.second.scalar;
        }
    }
    return true;
}

// The scalar disagreements no project value settled. Reported after every manifest has merged, naming
// both packages and the setting — picking the alphabetically-later one would be a silent answer to a
// question only the consumer can settle.
static bool emScalarConflict(std::string& err)
{
    for (const auto& e : g_emSettings) {
        if (e.conflictOwner.empty()) continue;
        err = "dependency `" + e.owner + "` sets emSettings `" + e.key + "` to \"" + e.value.scalar +
              "\" and dependency `" + e.conflictOwner + "` sets it to \"" + e.conflictValue +
              "\" — set `" + e.key + "` in this project's own `emSettings` to say which";
        return false;
    }
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
    bool                     dev = false;              // --dev: also walk `.kama/dev-deps` for build settings
    // Whether a DEPENDENCY's malformed manifest is fatal. True for every command that produces an
    // artifact; false for `kama query` and `kama lsp`, the same split `strictImports` draws — refusing
    // to answer an editor over a broken manifest in some package is punishing the wrong thing.
    bool                     strictDeps = false;
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
    g_rootSettings = BuildSettings();
    g_csources.clear(); g_jsLibraries.clear(); g_cincludes.clear(); g_emSettings.clear();
    g_manifestWebgpu = false; g_manifestNoHeap = false; g_manifestReproFloat = false;
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

        // `kind` is REQUIRED. A project is a library or an executable, and nothing inferred it before —
        // the only thing that ever read `entry`'s presence was `kama run`, and it read it as an entry
        // point, not as a kind. Enforced here because this is the one place a manifest is validated as
        // THIS project's manifest rather than mined for one key.
        //
        // Deliberately NOT enforced on a dependency's manifest yet: the rule that reads a dependency's
        // kind is "only a library can be imported", which does not exist until the visibility phase.
        // Enforcing it here early would be enforcement with no rule behind it.
        std::string kind;
        if (!loadManifestKind(manifest, kind, err)) { err = manifest + ": " + err; return false; }
        if (kind.empty()) {
            err = manifest + ": no `kind` — every project states whether it is a \"library\" or an "
                  "\"executable\" (add \"kind\": \"executable\")";
            return false;
        }

        // `source` is validated here rather than only in its loader because all three of the loader's
        // other callers discard `err` on purpose — they run on the resolution and editor paths, which
        // must stay lenient about a manifest somewhere up the tree. This is the build path, where a
        // source root that is misspelled or simply absent should be said out loud rather than quietly
        // resolving to nothing. (The editor still gets it: lspResolveConfig falls back to permissive host
        // defaults and reports the error to the client, so a broken manifest does not stop analysis.)
        std::string srcRel;
        if (!loadManifestSource(manifest, srcRel, err)) { err = manifest + ": " + err; return false; }
        if (srcRel.empty()) srcRel = "src";
        const std::string srcDir = joinPathLexical(dirName(manifest), srcRel);
        if (!dirExists(srcDir)) {
            err = manifest + ": `source` is \"" + srcRel + "\", but " + srcDir + " does not exist";
            return false;
        }
        const std::string nested = nestedManifestUnder(srcDir);
        if (!nested.empty()) {
            err = manifest + ": " + nested + " is inside this project's `source` root — projects do not "
                  "nest. Move it beside \"" + srcRel + "\" rather than under it";
            return false;
        }

        // The module map (§2b). Its LOCAL errors already fire for every command, because ManifestReader
        // recognizes `modules` unconditionally — but the cross-cutting ones (composed names unique, a
        // `visibility` list naming a module that exists, `"children"` on a leaf) need the whole map, so
        // they run here, on the build path, for the same reason `source` is re-checked here: this is the
        // one place a manifest is validated as THIS project's rather than mined for one key. The
        // resolution and editor paths stay lenient about a manifest somewhere up the tree.
        //
        // And the map is REQUIRED, for the same reason `kind` is and in the same place. It was optional
        // through 2a-2d because `visibility` was only form-checked and a file's identity could still come
        // from a `namespace` it declared. Neither is true any more: the map is the ONLY way left to name a
        // module, so a project without one is a project whose folders have no API — a thing to say out
        // loud here rather than leave to be discovered as "cannot resolve module" at the first import.
        // (An empty map is refused in modulesObject, so `mods.empty()` here means the key is ABSENT.)
        // Not enforced on a DEPENDENCY's manifest, matching `kind`: the rule that would read one is
        // visibility's, and that phase has not landed.
        std::vector<ModuleNode> mods;
        if (!loadManifestModules(manifest, mods, err)) { err = manifest + ": " + err; return false; }
        if (mods.empty()) {
            err = manifest + ": no `modules` — every project states its module map, and its root module is "
                  "`\".\"`, the files directly under `source` (add "
                  "\"modules\": { \".\": { \"visibility\": \"internal\" } })";
            return false;
        }

        // The project's own `link`, which a `select.TARGET` entry may then override wholesale. Read here
        // rather than folded into loadManifestTargets because it is a PROJECT property, not a target one
        // — the target key exists only to say "not this one" / "something else here".
        if (!loadManifestProjectFlags(manifest, g_rootSettings, err)) {
            err = manifest + ": " + err; return false;
        }
        if (!kamaReqSatisfied(g_rootSettings.kamaReq, "this project", err)) { err = manifest + ": " + err; return false; }
        // Same shape and the same place as `link`: project properties a target may then override.
        if (!loadManifestArtifactFlags(manifest, g_manifestWebgpu, g_manifestNoHeap,
                                      g_manifestReproFloat, err)) {
            err = manifest + ": " + err; return false;
        }

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
        if (std::ifstream(osp(localManifest)).good()) {
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
                if (!l.runtime.empty()) t.runtime = l.runtime;
                if (!l.subsystem.empty()) t.subsystem = l.subsystem;
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
    // The project's `link` applies to every target that did not override it.
    if (!g_target.linkSet)   g_target.link   = g_rootSettings.link;
    // ...and the project's compile/link flags go BEFORE the target's, which resolveTarget has already
    // appended onto the built-in's. One insert at the front, not an append: "project, then target" is
    // the whole rule, and it is the same direction `select.TARGET` appends in.
    g_target.cflags.insert(g_target.cflags.begin(), g_rootSettings.cflags.begin(), g_rootSettings.cflags.end());
    g_target.ldflags.insert(g_target.ldflags.begin(), g_rootSettings.ldflags.begin(), g_rootSettings.ldflags.end());
    if (!g_target.webgpuSet) g_target.webgpu = g_manifestWebgpu;
    if (!g_target.noHeapSet) g_target.noHeap = g_manifestNoHeap;
    if (!g_target.reproFloatSet) g_target.reproFloat = g_manifestReproFloat;

    // ---- what the DEPENDENCIES contribute ----------------------------------------------------------
    //
    // Enumerated from the `.kama/deps` VIEW, not from kama.lock, for two reasons. The view is flat and
    // already transitive, so one readdir gets the whole production closure with no BFS to re-derive.
    // And the view is what is actually COMPILED: `kama.local.json` `overrides` repoint it without
    // touching the lock, so a lock-driven walk could read a dependency's settings while building a
    // different copy of that dependency.
    //
    // Order is deps (alphabetical by import name), then the root, then the CLI — one sentence for every
    // key. `-l` ordering does not matter here the way it does for archives: every dependency's kama code
    // is already inside this program's own objects, and `link` names SYSTEM libraries.
    if (!req.manifest.empty()) {
        const std::string projDir = dirName(req.manifest);
        std::vector<std::string> views;
        views.push_back(projDir + "/.kama/deps");
        if (req.dev) views.push_back(projDir + "/.kama/dev-deps");
        std::vector<std::string> depCflags, depLdflags, depLink;
        for (const std::string& view : views) {
            std::vector<std::string> names = listDir(view);
            std::sort(names.begin(), names.end());   // deterministic: the view is a set, not a sequence
            for (const std::string& n : names) {
                // ⚠️ LEXICAL, never absolutePath — that is realpath(), and a dependency reaches its
                // package THROUGH this symlink. Resolving it away would respell the dep's files as
                // first-party paths and print a store path the user cannot see.
                const std::string depManifest = joinPathLexical(view, n) + "/kama.json";
                if (!fileExists(depManifest)) continue;
                BuildSettings bs; bs.owner = n;
                std::string derr;
                if (!loadBuildSettings(depManifest, g_target.name, bs, derr)) {
                    // Lenient for the editor, fatal for a build. Reach costs nothing: `kama pkg install`
                    // already hard-fails on a child manifest it cannot read, so a tree that installs at
                    // all has readable dependency manifests — this only changes WHEN it is reported.
                    if (req.strictDeps) { err = depManifest + ": " + derr; return false; }
                    continue;
                }
                if (!depFlagPathIsPortable(bs.cflags, n, depManifest, derr) ||
                    !depFlagPathIsPortable(bs.ldflags, n, depManifest, derr)) {
                    if (req.strictDeps) { err = derr; return false; }
                    continue;
                }
                // The dependency's `kama` range, at BUILD time: install already refused a package this
                // compiler cannot build, so this catches the view a newer manifest has outgrown (a `path`
                // dependency edited in place, a downgraded compiler). Same leniency split as above.
                if (!kamaReqSatisfied(bs.kamaReq, "`" + n + "`", derr)) {
                    if (req.strictDeps) { err = derr; return false; }
                    continue;
                }
                depCflags.insert(depCflags.end(),   bs.cflags.begin(),  bs.cflags.end());
                depLdflags.insert(depLdflags.end(), bs.ldflags.begin(), bs.ldflags.end());
                depLink.insert(depLink.end(),       bs.link.begin(),    bs.link.end());
                // An OR, one way: a dependency that needs reproducible arithmetic gets it, and no
                // dependency's `false` can take it from a consumer (or a sibling) that asked. The
                // consumer compiles the dependency's code, so the requirement is the consumer's too.
                if (bs.reproFloat) g_target.reproFloat = true;
                // A dependency wrapping a C library ships the shim that binds it, and the consumer
                // compiles it — which is the whole reason `csources` propagates. Resolved against the
                // DECLARING manifest, lexically (see the symlink note above).
                for (const std::string& cs : bs.csources)
                    g_csources.push_back({ n, joinPathLexical(dirName(depManifest), cs) });
                for (const std::string& js : bs.jsLibraries)
                    g_jsLibraries.push_back({ n, joinPathLexical(dirName(depManifest), js) });
                for (const std::string& inc : bs.cincludes)
                    g_cincludes.push_back({ n, joinPathLexical(dirName(depManifest), inc) });
                if (!mergeEmSettings(bs.emSettings, n, derr)) {
                    if (req.strictDeps) { err = derr; return false; }
                    continue;
                }
            }
        }
        g_target.cflags.insert(g_target.cflags.begin(),   depCflags.begin(),  depCflags.end());
        g_target.ldflags.insert(g_target.ldflags.begin(), depLdflags.begin(), depLdflags.end());
        // `link` is a NAME list, so a repeat is pure noise and two dependencies both wanting `m` is the
        // ordinary case. `cflags`/`ldflags` are raw text, where a repeat can be load-bearing
        // (`-Xlinker -foo`) and where last-wins is the semantic — those are left exactly as written.
        depLink.insert(depLink.end(), g_target.link.begin(), g_target.link.end());
        std::vector<std::string> merged; std::set<std::string> seen;
        for (const std::string& l : depLink) if (seen.insert(l).second) merged.push_back(l);
        g_target.link = merged;
    }
    // The project's own C comes after its dependencies', matching the flag order: a dependency
    // contributes first, the project last.
    if (!req.manifest.empty()) {
        for (const std::string& cs : g_rootSettings.csources)
            g_csources.push_back({ "", joinPathLexical(dirName(req.manifest), cs) });
        for (const std::string& js : g_rootSettings.jsLibraries)
            g_jsLibraries.push_back({ "", joinPathLexical(dirName(req.manifest), js) });
        for (const std::string& inc : g_rootSettings.cincludes)
            g_cincludes.push_back({ "", joinPathLexical(dirName(req.manifest), inc) });
        // The project merges LAST, which is what makes "the project wins" true of a scalar — including
        // over a conflict two dependencies could not settle between themselves.
        std::string merr;
        if (!mergeEmSettings(g_rootSettings.emSettings, "", merr)) { err = merr; return false; }
        if (!emScalarConflict(merr)) { err = merr; return false; }
    }
    // The CLI can only turn this ON; the manifest is the way to say it for every build, and a target is
    // the way to say "not this one". `--no-heap` therefore ORs in rather than overriding.
    if (g_target.noHeap) g_noHeap = true;

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
    std::ofstream out(osp(path));
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
    std::ifstream in(osp(path), std::ios::binary);
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
            _mkdir(osp(part).c_str());
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
//
// ⚠️ A target INSIDE the user's own tree is linked RELATIVELY; only the machine-global store stays
// absolute. The distinction is what makes a resolved tree relocatable: an absolute
// `game/.kama/deps/engine -> /Users/me/proj/engine` dangles the moment the same repo is mounted
// anywhere else — a container at /work, CI with a different checkout root, a second worktree — and the
// resulting error ("declares no dependency named `engine`") blames the manifest, which is correct.
// A relative `../../../engine` is right under every mount point simultaneously, so one resolved tree
// serves host and container at once. Re-running `pkg install` per environment is not an alternative:
// each run overwrites the other's links, so the two fight. (Reported from a real port —
// friendly-fire-department KAMA_GAPS KB-6, which had to post-process the links to work around it.)
//
// The store is deliberately exempt. `~/.kama/store/<pkg>-<hash>` is content-addressed and machine-global,
// not part of any repo, so a relative path to it would be both longer and more fragile — and it does not
// travel with the tree anyway.
static bool linkDir(const std::string& target, const std::string& linkPath)
{
#ifdef _WIN32
    // A junction stores an absolute path by definition, so there is nothing to relativize here.
    //
    // ⚠️ This was two `cmd /c` shell-outs (`rmdir`, then `mklink /J`) until 0.9.303, and cmd is
    // MAX_PATH-bound: a path dependency under a 265-character project failed with `The system cannot
    // find the path specified.` / `kama install: cannot link dependency`, while the byte-identical
    // project at a short path linked fine. `osp()` could not help — its `\\?\` prefix applies at the
    // Win32 file-call edge and means nothing on a command line handed to a shell. The junction is now
    // made with FSCTL_SET_REPARSE_POINT, which takes the verbatim spelling and spawns no process.
    return kama_win_make_junction(target, linkPath);
#else
    std::string linkTarget = target;
    const std::string store = storeDir();
    if (store.empty() || target.compare(0, store.size(), store) != 0) {
        // ⚠️ Resolve the link's PARENT, not the link. `absolutePath` is realpath(), and the link does not
        // exist yet — resolving it yields nothing, `dirName("")` is "", and the relative path comes back
        // unusable, leaving the absolute target silently in place. The parent is `.kama/deps`, which the
        // installer has already created.
        std::string rel = relativePath(absolutePath(dirName(linkPath)), target);
        if (!rel.empty()) linkTarget = rel;
    }
    unlink(linkPath.c_str());
    return symlink(linkTarget.c_str(), linkPath.c_str()) == 0;
#endif
}

// Emit an already-parsed unit to a single `.c` (`srcPath` drives #line). Returns 0 on success.
int transpileUnitToFile(SharedCompilationUnit unit, const std::string& srcPath,
                        const std::string& outPath, bool emitLines, bool* externsMathH = nullptr,
                        bool* externsNetWeb = nullptr, bool* externsApp = nullptr,
                        bool* externsGpu = nullptr, bool* externsIsolate = nullptr)
{
    std::ofstream out(osp(outPath));
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
    // Native: link libpthread for any use of the seams (harmless when unneeded). Wasm: a threaded runtime is a
    // hosting model, so only for a program that actually creates a thread (KB-26 — see noteThreadSpawn).
    if (externsIsolate) *externsIsolate = g_target.isWasm() ? emitter.spawnsThreads()
                                       : (emitter.externsHeader("kama_isolate.h") || emitter.externsHeader("kama_channel.h"));
    out.close();
    for (const auto& d : emitter.diagnostics()) renderDiagnostic(stderr, d);
    if (unsupported > 0) {
        fprintf(stderr, "kama: %d unlowered construct(s) — see the errors above.\n", unsupported);
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
    std::ofstream header(osp(headerPath));
    if (!header) { fprintf(stderr, "kama: error: cannot write '%s'\n", headerPath.c_str()); return 1; }

    std::vector<std::unique_ptr<std::ofstream>> moduleFiles;
    std::vector<std::ostream*> moduleStreams;
    for (auto& cp : cPaths) {
        auto f = std::unique_ptr<std::ofstream>(new std::ofstream(osp(cp)));
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
    // Native: link libpthread for any use of the seams (harmless when unneeded). Wasm: a threaded runtime is a
    // hosting model, so only for a program that actually creates a thread (KB-26 — see noteThreadSpawn).
    if (externsIsolate) *externsIsolate = g_target.isWasm() ? emitter.spawnsThreads()
                                       : (emitter.externsHeader("kama_isolate.h") || emitter.externsHeader("kama_channel.h"));
    header.close();
    for (auto& f : moduleFiles) f->close();

    for (const auto& d : emitter.diagnostics()) renderDiagnostic(stderr, d);
    if (unsupported > 0) {
        fprintf(stderr, "kama: %d unlowered construct(s) — see the errors above.\n", unsupported);
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

    std::ofstream out(osp(outPath));
    if (!out) { fprintf(stderr, "kama: error: cannot write '%s'\n", outPath.c_str()); return 1; }
    { std::ifstream h(osp(headerPath)); out << h.rdbuf(); }
    out << "\n";
    std::string incLine = "#include \"" + headerName + "\"";
    for (auto& cp : cPaths) {
        std::ifstream u(osp(cp));
        std::string line;
        while (std::getline(u, line)) {
            if (line.find(incLine) != std::string::npos) continue;   // header already inlined above
            out << line << "\n";
        }
    }
    out.close();
    remove(osp(headerPath).c_str());
    for (auto& cp : cPaths) remove(osp(cp).c_str());
    return 0;
}

int runCmd(const std::string& cmd)
{
#if defined(_WIN32)
    // system() on Windows is `cmd /c <string>`, and cmd.exe has a quoting rule that bites exactly one
    // shape of command: when the string STARTS with a double quote, cmd strips the first and last quote
    // on the whole line and runs what is left as one token. Every command built here that leads with a
    // quoted program path therefore arrives mangled —
    //     "C:/…/kama-run-6644.exe" "alpha" "beta" "gamma"
    //  -> 'C:/…/kama-run-6644.exe" "alpha" "beta" "gamma' is not recognized as an internal or external
    // which is what `kama run -- args` did on Windows once it had a temp path valid enough to reach here.
    // The C compiler invocations never hit it because they lead with a bare `clang`, not a quoted path.
    //
    // The documented workaround is an extra outer pair: cmd strips those, and the inner quoting — which
    // was correct all along — survives. Applied only when the command leads with a quote, so every other
    // command keeps the exact string it has always had.
    if (!cmd.empty() && cmd[0] == '"') {
        int wrapped = system(("\"" + cmd + "\"").c_str());
        return wrapped == -1 ? 1 : wrapped;
    }
#endif
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
    // The Windows pool. This used to run the wave SERIALLY and `-j` was clamped to 1 above, on the
    // grounds that there is no posix_spawn/waitpid here — so a 16-TU program compiled one TU at a time
    // no matter what the user asked for, on the platform where process startup is most expensive. That
    // is the single largest lever on Windows build time.
    //
    // CreateProcessW does it, behind kama.winpath.h (this TU cannot include <windows.h>: its token macros
    // collide with kama.parser.hpp's enum), and the COMMAND LINE IS PASSED VERBATIM. That matters twice:
    //
    //  * The command is a SHELL STRING carrying its own quotes (`"…/zig" cc`, `-o "out dir/x.o"`), and the
    //    CRT's _spawn family builds a child's command line by quoting each argv entry and escaping inner
    //    quotes with backslashes — rules cmd.exe does not use, so `cmd /c "clang \"a b.c\""` reached clang
    //    with the backslashes intact. CreateProcess has no such layer; cmd reads the line as written, which
    //    keeps the POSIX branch's contract that a "compiler" is a shell string, not a tokenized argv.
    //  * ⚠️ It used to be a generated .bat per job for exactly that reason, and that is the wrong vehicle:
    //    cmd.exe parses a batch FILE in the CONSOLE code page (437 by default — measured, and 437 is also
    //    what a console-less kama.exe hands it), not in the process code page the manifest sets. So a
    //    non-ASCII path in the .bat was mojibake and the wave "could not find the path" while the same
    //    text on cmd's command line, which arrives as UTF-16, was fine. No file, no code page.
    //
    // The same leading-quote rule runCmd applies to system() applies here: cmd strips the outer pair of a
    // `/c` string that begins with `"`, so one is wrapped around it.
    if (jobs < 1) jobs = 1;
    if (jobs == 1) {
        for (size_t i = 0; i < cmds.size(); ++i) {
            rcs[i] = runCmd(cmds[i]);
            if (rcs[i] != 0) return rcs[i];
        }
        return 0;
    }

    // Which cmd.exe, and found how. %COMSPEC% is the right answer when it is set — but an msys2 shell
    // does NOT export it, so this cannot rely on it. (Asking cmd.exe to print %COMSPEC% says it is set;
    // that is cmd defining the variable for itself, not evidence about kama's environment.) Fall back to
    // %SystemRoot%, and finally to the bare name, which CreateProcess resolves through PATH when the
    // program is the first token of the line.
    std::string comspec;
    if (const char* cs = getenv("COMSPEC")) { if (*cs) comspec = cs; }
    if (comspec.empty()) {
        if (const char* sr = getenv("SystemRoot")) { if (*sr) comspec = std::string(sr) + "\\System32\\cmd.exe"; }
    }
    if (comspec.empty()) comspec = "cmd.exe";

    struct Live { intptr_t h; size_t idx; };
    std::vector<Live> live;          // in LAUNCH order; reaped from the front
    size_t next = 0;
    bool stop = false;
    int firstFail = 0, firstFailIdx = -1;

    auto reapFront = [&]() {
        Live l = live.front();
        live.erase(live.begin());
        // One named child at a time, so the pool reaps in launch order. It still keeps `jobs` compiles
        // in flight, which is where the win is; the only cost is that a slot behind an unusually slow
        // job opens later than it could.
        int status = kama_win_wait(l.h);
        rcs[l.idx] = status;
        if (status != 0 && (firstFailIdx < 0 || (int)l.idx < firstFailIdx)) {
            firstFail = status; firstFailIdx = (int)l.idx;
            stop = true;   // stop LAUNCHING; drain what is already running (see the note above)
        }
    };

    while (next < cmds.size() || !live.empty()) {
        while (!stop && next < cmds.size() && (int)live.size() < jobs) {
            const std::string& c = cmds[next];
            std::string line = "\"" + comspec + "\" /c " + (!c.empty() && c[0] == '"' ? "\"" + c + "\"" : c);
            intptr_t h = kama_win_spawn_shell(line);
            if (h == -1) { rcs[next] = 1;
                           if (firstFailIdx < 0) { firstFail = 1; firstFailIdx = (int)next; }
                           stop = true; ++next; continue; }
            live.push_back(Live{h, next});
            ++next;
        }
        if (live.empty()) break;
        reapFront();
    }
    return firstFail;
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
    if (FILE* f = fopen(osp(path).c_str(), "rb")) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, to);
        fclose(f);
    }
    remove(osp(path).c_str());
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
    cmds.push_back("sha256sum " + q + " 2>" KAMA_DEVNULL);    // Linux / container
    cmds.push_back("shasum -a 256 " + q + " 2>" KAMA_DEVNULL); // macOS
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
    int rc = 0; runCmdCapture(KAMA_WHICH "ssh-keygen 2>" KAMA_DEVNULL, &rc); return rc == 0;
}

// Sign `file` with the SSH private key `keyPath` (`ssh-keygen -Y sign` writes `<file>.sig`). On success
// fills `sigOut` (the armored SSHSIG) + `pubKeyOut` (the signer public key line, from `<keyPath>.pub`).
static bool sshSign(const std::string& file, const std::string& keyPath,
                    std::string& sigOut, std::string& pubKeyOut, std::string& err)
{
    std::string sigfile = file + ".sig";
    runCmd(rmRfCmd(sigfile));
    int rc = runCmd("ssh-keygen -Y sign -f \"" + keyPath + "\" -n " + kSigNamespace + " \"" + file + "\" >" KAMA_DEVNULL " 2>&1");
    if (rc != 0) { err = "ssh-keygen -Y sign failed (is the key '" + keyPath + "' an SSH private key?)"; return false; }
    { std::ifstream f(osp(sigfile), std::ios::binary); sigOut.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
    { std::ifstream f(osp(keyPath + ".pub"), std::ios::binary); pubKeyOut.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
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
    { std::ofstream o(osp(sigTmp), std::ios::binary); if (!o) return false; o << signature; }
    int rc = runCmd("ssh-keygen -Y check-novalidate -n " + std::string(kSigNamespace) +
                    " -s \"" + sigTmp + "\" < \"" + file + "\" >" KAMA_DEVNULL " 2>&1");
    runCmd(rmRfCmd(sigTmp));
    return rc == 0;
}
// -----------------------------------------------------------------------------------------------

// Collect every file under `root` as paths RELATIVE to root (recursively), for a deterministic tree hash.
static void collectFilesRel(const std::string& root, const std::string& rel, std::vector<std::string>& out)
{
    std::string dir = rel.empty() ? root : root + "/" + rel;
    for (const std::string& n : listDir(dir)) {
        std::string childRel = rel.empty() ? n : rel + "/" + n;
        if (dirExists(root + "/" + childRel)) collectFilesRel(root, childRel, out);
        else out.push_back(childRel);
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
    std::ofstream out(osp(tmp), std::ios::binary);
    if (!out) return "";
    for (auto& rel : files) {
        out.write(rel.data(), rel.size()); out.put('\0');
        std::ifstream f(osp(dir + "/" + rel), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        out.write(bytes.data(), bytes.size());
    }
    out.close();
    std::string h = sha256Of(tmp);
    remove(osp(tmp).c_str());
    return h;
}

// The kama home root: `~/.kama` (`%USERPROFILE%\.kama` on Windows). Holds the versioned toolchains
// (`versions/<v>/`), the PATH selector (`bin/kama`), the global-default record (`default`), and the shared
// package store (`store/`). NOT overridable at runtime — a per-version toolchain must resolve its own
// support dirs exe-relative, never via an ambient env var. (`KAMA_HOME` is only the *installer's* prefix.)
std::string kamaHome()
{
    // HOME first on BOTH platforms, %USERPROFILE% only as the Windows fallback — the same order Git for
    // Windows uses, and for the same two reasons. In an msys2/Cygwin shell `~` IS $HOME, so consulting
    // only USERPROFILE made `~/.kama` in the shell and kamaHome() name different directories. And that
    // shell does not always export USERPROFILE at all, in which case this fell through to the degenerate
    // project-local ".kama" — silently putting the toolchain root inside whatever tree you happened to be
    // building. It is also what lets a test isolate itself by setting HOME, which is how
    // tools/check-toolchain.sh keeps its stub versions out of the developer's real ~/.kama.
    if (const char* h = getenv("HOME")) return std::string(h) + "/.kama";
#ifdef _WIN32
    if (const char* u = getenv("USERPROFILE")) return std::string(u) + "/.kama";
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
// ⚠️ The `.exe` is load-bearing on Windows, and its absence disabled the whole versioned-toolchain
// feature there rather than breaking it visibly. maybeReExec decides "am I the selector?" by comparing
// selfExePath() — which the OS answers `…\bin\kama.exe` — against selectorPath(). Spelled without the
// suffix the two could never be equal, so the selector always concluded it was not the selector and ran
// in place: no pin was ever honoured on Windows. versionBin has the same problem one step later, where
// an installed toolchain is `kama.exe` and fileExists() was asked about `kama`.
std::string selectorPath()       { return kamaHome() + "/bin/kama" KAMA_EXE_SUFFIX; }
std::string versionDir(const std::string& v) { return versionsDir() + "/" + v; }
std::string versionBin(const std::string& v) { return versionDir(v) + "/bin/kama" KAMA_EXE_SUFFIX; }

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
            if (runCmd("git clone --depth 1" + branch + " \"" + d.git + "\" \"" + staging + "\" 2>" KAMA_DEVNULL) != 0) {
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
        // --force-local on Windows: GNU tar reads a `:` in a file argument as a REMOTE HOST separator, so an
        // ordinary absolute path becomes a network fetch —
        //     tar (child): Cannot connect to C: resolve failed
        // and every url-tarball dependency failed to unpack. The flag says "that colon is part of a path".
        // POSIX paths have no colon to misread, and the flag is GNU-only, so it stays on this side.
        if (runCmd("tar " KAMA_TAR_LOCAL "-xzf \"" + tgz + "\" -C \"" + staging + "\" --strip-components=1") != 0) {
            err = "cannot unpack '" + name + "'"; runCmd(rmRfCmd(staging)); runCmd(rmRfCmd(tgz)); return false;
        }
        runCmd(rmRfCmd(tgz));
    }

    std::string hash = treeHashOf(staging);   // "sha256-<hex>" — the store dir name + dedup key
    if (hash.empty()) { err = "cannot hash '" + name + "' (is sha256sum/shasum available?)"; runCmd(rmRfCmd(staging)); return false; }
    if (!d.git.empty()) lockIntegrity = hash;   // git has no artifact; the tree hash IS its integrity

    std::string finalDir = store + "/" + storeLabel(name) + "-" + hash.substr(sizeof("sha256-") - 1);
    if (dirExists(finalDir)) runCmd(rmRfCmd(staging));   // dedup: identical content already stored
    else if (rename(osp(staging).c_str(), osp(finalDir).c_str()) != 0) {
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
    std::string res = runCmdCapture("git ls-remote --tags \"" + gitUrl + "\" 2>" KAMA_DEVNULL, &rc);
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
    std::string body = runCmdCapture("curl -fsSL \"" + url + "\" 2>" KAMA_DEVNULL, &rc);
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
    std::string rootReq;
    if (!loadManifestDeps(manifest, deps, err, &devDeps, &regCfg, &rootReq)) {
        fprintf(stderr, "kama: %s: %s\n", manifest.c_str(), err.c_str()); return 2;
    }
    // The root's own `kama` range, before anything is fetched: a project this compiler cannot build
    // should not get a materialized view it then cannot use.
    if (!kamaReqSatisfied(rootReq, "this project", err)) { fprintf(stderr, "kama: %s: %s\n", manifest.c_str(), err.c_str()); return 2; }

    // `kama.local.json` (M5.3): a gitignored, dev-local sibling that layers INSTALL-path overrides over
    // `kama.json` — a local `registries` config (merged now, so resolution routes through it) and dep
    // `overrides` (applied AFTER the canonical lock is written, below). Local-only by construction: the
    // registry merge is lock-safe because the lock pins integrity not URI, and the dep overrides never
    // touch the lock at all — so CI (which has no `kama.local.json`) reproduces the identical build.
    std::map<std::string, DepSpec> overrides;
    std::string localManifest = base + "/kama.local.json";
    if (std::ifstream(osp(localManifest)).good()) {
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
    //
    // This is where a broken workspace file is SAID OUT LOUD. The resolution and editor paths pass no
    // `err` and quietly get "no workspace", because they must keep answering on a tree mid-edit; install
    // is the command that acts on the whole thing, so a member that is not there is its problem.
    std::string wsErr;
    const std::set<std::string> wsMembers = workspaceMembers(base, &wsErr);
    if (!wsErr.empty()) { fprintf(stderr, "kama install: %s\n", wsErr.c_str()); return 2; }

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
                // not carried with it. Between two members of one kama_workspace.json it is exactly as
                // reproducible as the workspace itself, which is what makes a member able to declare the
                // siblings it imports, and so able to be lifted out and still build.
                if (!r.spec.path.empty() && r.requestor != "<root manifest>" &&
                    !(wsMembers.count(absolutePath(r.requestorDir)) && wsMembers.count(r.spec.pathAbs))) {
                    fprintf(stderr, "kama install: path dependency '%s' (required by %s) is only allowed at the "
                            "top level, or between two members of one kama_workspace.json — a fetched "
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
                // Read THIS package's own prod deps → record (serialized so the build never re-reads) + enqueue.
                // BEFORE the view link below: the manifest also carries the package's `kama` range, and a
                // package this compiler refuses must leave no link behind for a later build to trip over.
                std::string childManifest = storePath + "/kama.json";
                std::vector<std::string> directNames;
                if (fileExists(childManifest)) {
                    std::map<std::string, DepSpec> childDeps; std::string cerr, childReq;
                    if (!loadManifestDeps(childManifest, childDeps, cerr, nullptr, nullptr, &childReq)) {
                        fprintf(stderr, "kama install: %s: %s\n", childManifest.c_str(), cerr.c_str()); return 2;
                    }
                    // The fetched package's `kama` range against THIS compiler. Refused here, once per
                    // package, so a tree that installs is a tree this compiler can build — the build-time
                    // check (resolveBuildConfig) is for a view a newer manifest has since outgrown.
                    if (!kamaReqSatisfied(childReq, "`" + r.name + "`", cerr)) {
                        fprintf(stderr, "kama install: %s\n", cerr.c_str()); return 2;
                    }
                    for (auto& ck : childDeps) { directNames.push_back(ck.first);
                        // storePath is where THIS package's manifest lives — the dir its own path specs
                        // are relative to. For a path dep that is the local package dir itself.
                        q.push_back({ck.first, ck.second, r.name, storePath, r.dev}); }
                    std::sort(directNames.begin(), directNames.end());   // deterministic dependencies[] order
                }
                if (!linkDir(storePath, (r.dev ? devViewDir : viewDir) + "/" + importName)) {
                    fprintf(stderr, "kama install: cannot link dependency '%s'\n", r.name.c_str()); return 1;
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
    std::ifstream in(osp(path), std::ios::binary);
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
    std::ofstream o(osp(path), std::ios::binary | std::ios::trunc);
    if (!o) { err = "cannot write '" + path + "'"; return false; }
    o << out; return true;
}

// Remove `name` from whichever section holds it. Returns true even if absent (idempotent); sets *found.
static bool manifestRemoveDep(const std::string& path, const std::string& name, std::string& err, bool* found = nullptr)
{
    std::ifstream in(osp(path), std::ios::binary);
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
        std::ofstream o(osp(path), std::ios::binary | std::ios::trunc);
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
        std::ofstream o(osp(indexPath), std::ios::binary | std::ios::trunc);
        if (!o) { err = "cannot write '" + indexPath + "'"; return false; }
        o << "{\n  \"name\": \"" << jsonEscape(name) << "\",\n  \"versions\": [\n    "
          << entryJson << "\n  ]\n}\n";
        return true;
    }
    std::ifstream in(osp(indexPath), std::ios::binary);
    if (!in) { err = "cannot open '" + indexPath + "'"; return false; }
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
    size_t vk = s.find("\"versions\"");
    size_t br = (vk == std::string::npos) ? std::string::npos : s.find('[', vk);
    if (br == std::string::npos) { err = "malformed registry index '" + indexPath + "' (no versions array)"; return false; }
    size_t j = br + 1; while (j < s.size() && (s[j]==' '||s[j]=='\t'||s[j]=='\n'||s[j]=='\r')) ++j;
    bool hasExisting = (j < s.size() && s[j] != ']');
    std::string out = s.substr(0, br + 1) + "\n    " + entryJson + (hasExisting ? "," : "") + s.substr(br + 1);
    std::ofstream o(osp(indexPath), std::ios::binary | std::ios::trunc);
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
        std::ifstream f(osp(indexPath), std::ios::binary);
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
    // `./out` is the build-output root a project actually uses (docs/targets.md); `./build` predates
    // that convention and stays excluded so an older layout is not suddenly published. Excluding the
    // output is not tidiness — out/<triple>/ differs per publishing machine, so shipping it would
    // break the REPRODUCIBLE integrity hash the block immediately below depends on.
    std::string copyCmd = "tar -c -C \"" + base + "\" --exclude=./.git --exclude=./.kama "
                          "--exclude=./out --exclude=./build "
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
    if (rename(osp(tarball).c_str(), osp(finalTarball).c_str()) != 0) {
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
    // Omitted when there is none, so its presence MEANS "there is a name here a tool can act on" rather
    // than needing a truth test against "". See Diagnostic::subject.
    if (!d.subject.empty()) j.set("subject", d.subject);
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

// Embedded text starts with the newline that follows the raw-string delimiter (`R"KAMAGENTS(` and
// `R"KAMASEED(` alike). Drop it so a written file does not open with a blank line.
static const char* embeddedBody(const char* s) { return (s && *s == '\n') ? s + 1 : s; }

void agentsUsage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama agents install <kama.json> [--claude] [--tool <name>]... [--all-tools] [--skill] [--force]\n"
        "                                      write AGENTS.md (+ pointers) beside the manifest; a library\n"
        "                                      (`\"kind\": \"library\"`) also gets AGENTS.package.md\n"
        "  kama agents print [--skill|--package] write the guidance to stdout instead\n"
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
//
// `who` is the message prefix ("kama agents" / "kama seed"). Two commands write files into a user's tree
// and they must refuse identically — a second no-clobber rule is a second set of ways to get it wrong.
static bool embeddedWrite(const char* who, const std::string& dir, const std::string& rel,
                          const std::string& body, bool force, int& written)
{
    std::string path = dir.empty() || dir == "." ? rel : dir + "/" + rel;
    if (fileExists(path) && !force) {
        fprintf(stderr, "%s: %s exists — pass --force to overwrite\n", who, path.c_str());
        return false;
    }
    std::string parent = dirName(path);
    if (!parent.empty() && parent != path && !dirExists(parent) && !makeDirs(parent)) {
        fprintf(stderr, "%s: cannot create %s\n", who, parent.c_str());
        return false;
    }
    std::ofstream out(osp(path), std::ios::binary);
    if (!out) { fprintf(stderr, "%s: cannot write %s\n", who, path.c_str()); return false; }
    out << body;
    if (!out) { fprintf(stderr, "%s: failed writing %s\n", who, path.c_str()); return false; }
    printf("%s: wrote %s\n", who, path.c_str());
    ++written;
    return true;
}

// `kama agents install`, callable: `kama seed` offers the same guidance at the end of a fresh project and
// must reach it IN-PROCESS. Re-dispatching would re-enter maybeReExec, which could hand the agent files to
// a different toolchain than the one that just wrote the manifest beside them.
//
// Every tool name is resolved BEFORE anything is written, and then every destination is checked for a
// collision before the first write — a typo used to be caught halfway through, after AGENTS.md had already
// landed, leaving a half-installed project behind an exit 2. The name check has been here a while; the
// collision pre-flight has not, and an existing CLAUDE.md could still strand a freshly written AGENTS.md.
//
// `kind` is the project's manifest kind ("library" / "executable" / ""). A library also gets the package
// half, AGENTS.package.md, beside AGENTS.md: the guidance only a package that will be published needs
// (agents/PACKAGE.md — the manifest floor, `tests/` as one program, vendoring a C library, the C seam,
// publishing). It is keyed on the MANIFEST rather than a flag because `--force` rewrites every file this
// command owns, and a flag a re-install forgets would silently drop the addendum; `kama agents install
// <kama.json>` already names the manifest, so the kind is one read away, and `kama seed` knows it.
static int cmdAgentsInstall(const std::string& dir, const std::vector<std::string>& tools,
                            bool skill, bool force, const std::string& kind)
{
    std::vector<const KamaAgentStub*> chosen;
    for (const auto& t : tools) {
        const KamaAgentStub* found = nullptr;
        for (int i = 0; i < KAMA_AGENT_STUB_COUNT; ++i)
            if (t == KAMA_AGENT_STUBS[i].name) { found = &KAMA_AGENT_STUBS[i]; break; }
        if (!found) {
            fprintf(stderr, "kama agents: unknown tool '%s' — `kama agents list` shows them all\n", t.c_str());
            return 2;
        }
        chosen.push_back(found);
    }

    std::vector<std::pair<std::string, const char*>> files;
    files.push_back({ "AGENTS.md", KAMA_AGENTS_MD });
    if (kind == "library") files.push_back({ "AGENTS.package.md", KAMA_AGENTS_PACKAGE });
    for (const auto* s : chosen) files.push_back({ s->dest, s->src });
    if (skill) files.push_back({ ".claude/skills/kama/SKILL.md", KAMA_AGENTS_SKILL });

    if (!force) {
        int clash = 0;
        for (const auto& f : files) {
            std::string path = dir.empty() || dir == "." ? f.first : dir + "/" + f.first;
            if (fileExists(path)) { fprintf(stderr, "kama agents: %s exists\n", path.c_str()); ++clash; }
        }
        if (clash) {
            fprintf(stderr, "kama agents: nothing written — pass --force to overwrite, or use "
                            "`kama agents print` and merge by hand\n");
            return 1;
        }
    }

    int written = 0;
    for (const auto& f : files)
        if (!embeddedWrite("kama agents", dir, f.first, embeddedBody(f.second), force, written)) return 1;
    printf("kama agents: %d file%s written. AGENTS.md holds the content; the rest point at it.\n",
           written, written == 1 ? "" : "s");
    return 0;
}

// ------------------------------------------------------------------------------------------------
// `kama seed` — turn a directory into a kama project.
//
// The manifest is the point. Everything a project needs to be findable, importable and buildable lives
// in kama.json, and until now the only way to learn its shape was to read docs/packages.md and type it
// out. It also closes a trap permanently: a manifest with no `out` scatters build artifacts through the
// source tree. (The other one it used to close — a library with src/ and no `sources`, silently
// unimportable — closed for everyone when `source` gained its "src" default.)
//
// Named `seed`, not `init`: kama has a toolchain and a package store it could plausibly be
// initializing, so `init` names the wrong thing about half the time.
//
// This is the driver's ONE interactive code path — nothing else here reads stdin. It prompts only when
// stdin is a terminal; a pipe, a file or a CI runner gets the defaults, exactly as `--yes` would.
// User docs: docs/packages.md.

enum class SeedKind { Executable, Library, Monorepo };

struct SeedOpts {
    std::string name, version, kind, members;
    bool nameGiven = false, versionGiven = false, kindGiven = false, membersGiven = false;
    bool agents = false, agentsGiven = false;
    std::vector<std::string> tools;
    bool allTools = false, skill = false, yes = false, force = false;
    std::string license;                 // `--license mit`: the one body seed can write today
    bool licenseGiven = false;
};

void seedUsage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama seed [<dir>] [--kind executable|library|monorepo] [--name <n>] [--version <v>]\n"
        "            [--members <a,b,c>]        the members of a monorepo (required for that kind)\n"
        "            [--agents|--no-agents] [--claude] [--tool <name>]... [--all-tools] [--skill]\n"
        "            [--license mit]            write a LICENSE and record it in kama.json\n"
        "            [--yes|-y] [--force]\n"
        "\n"
        "  Interactive when stdin is a terminal; a pipe or a script behaves as --yes.\n");
}

// Is stdin a terminal? The one place that asks, because `kama seed` is the one command that prompts.
// <unistd.h> is already included above; Windows needs <io.h>, which like <direct.h>/<process.h> is a CRT
// header and does not reach <windows.h> (the prohibition at the top of this file is about windows.h).
//
// ⚠️ NOT _isatty on Windows. _isatty answers "is this fd a character device", and NUL is a character
// device — so `kama seed </dev/null`, which is how every script, every CI job and tools/check-seed.sh
// runs it, looked INTERACTIVE and prompted. GetConsoleMode succeeds only on a real console handle, which
// is the question being asked. STD_INPUT_HANDLE is (DWORD)-10.
static bool stdinIsTerminal()
{
#ifdef _WIN32
    unsigned long mode;
    return GetConsoleMode(GetStdHandle((unsigned long)-10), &mode) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

// Print `question [hint]: ` and return what was typed, trimmed — EMPTY for a bare Enter, and empty for
// EOF too. Returning empty rather than substituting a default is what keeps the yes/no case honest: the
// hint shown for a no-default question is "y/N", and folding that string in as the answer would make
// Enter read as 'y'. (It did, once. The prompt said [y/N] and wrote the file anyway.)
//
// EOF returning empty is the load-bearing half: a prompt that spins on end-of-stream hangs forever, and
// a seeding tool that can hang a CI job is worse than one that never prompts at all. Callers only reach
// here when stdin IS a terminal and --yes was absent, so EOF means the terminal went away.
static std::string seedReadLine(const char* question, const std::string& hint)
{
    printf("  %s [%s]: ", question, hint.c_str());
    fflush(stdout);
    char buf[256];
    if (!fgets(buf, sizeof buf, stdin)) { printf("\n"); return std::string(); }
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

// Ask, showing the default and taking it on a bare Enter.
static std::string seedAsk(const char* question, const std::string& dflt)
{
    std::string a = seedReadLine(question, dflt);
    return a.empty() ? dflt : a;
}

static bool seedAskYesNo(const char* question, bool dflt)
{
    std::string a = seedReadLine(question, dflt ? "Y/n" : "y/N");
    if (a.empty()) return dflt;
    return a[0] == 'y' || a[0] == 'Y';
}

// Replace every occurrence of the two placeholders. Both are legal kama identifiers, which is what lets
// seed/*.kama compile as-is and join the tree-sitter corpus (see kama.seed.h).
static std::string seedSubst(const char* tmpl, const std::string& name, const std::string& ident)
{
    std::string s = embeddedBody(tmpl);
    struct { const char* tok; const std::string& val; } subs[] = {
        { "KAMA_SEED_NAME", name }, { "KAMA_SEED_IDENT", ident },
    };
    for (const auto& sub : subs) {
        const size_t n = strlen(sub.tok);
        for (size_t p = s.find(sub.tok); p != std::string::npos; p = s.find(sub.tok, p + sub.val.size()))
            s.replace(p, n, sub.val);
    }
    return s;
}

// A default name from the target directory: lowercased, with anything outside [a-z0-9_-] folded to '_'.
// Only the DEFAULT is sanitized — an explicit --name or a typed answer is never rewritten, because
// silently renaming somebody's package is the implicit behavior this language argues against.
static std::string seedDefaultName(const std::string& dir)
{
    std::string b = baseName(absolutePath(dir));
    std::string out;
    for (char c : b) {
        if (c >= 'A' && c <= 'Z') out += (char)(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out += c;
        else out += '_';
    }
    if (out.empty() || !((out[0] >= 'a' && out[0] <= 'z'))) out = "app";
    return out;
}

// The package-name rule, written down here for the first time — nothing validated a name before, and
// `kama seed` is the right place because the name it stamps into kama.json is the name every consumer
// then has to spell.
//
//   <name> ::= [ "@" <seg> "/" ] <seg>        <seg> ::= [a-z] [a-z0-9_-]*
//
// Lowercase because the name is also a path component (.kama/deps/<importName>, the store label), and a
// case-insensitive filesystem cannot tell `Geo` from `geo`.
//
// A PROJECT of either kind is held to more: its name is its ROOT NAMESPACE, so it must also be a legal
// kama IDENTIFIER. `import my-lib::{ … }` is a parse error, so a hyphenated library could never be
// imported at all — better to refuse the name than to ship the dead end — and an executable's own
// symbols are qualified by the same name, so it is not the softer case it looks like. (The root-namespace
// rule itself lands with module identity; seed applies it early so it never creates a project that rule
// would reject. `mustBeImportable` stays parameterised for the monorepo ROOT, which is an aggregator with
// no namespace of its own and becomes a workspace file rather than a project.)
static bool seedValidName(const std::string& name, bool mustBeImportable, std::string& err)
{
    const std::string ident = importNameOf(name);
    const std::string scope = scopeOf(name);
    auto seg = [](const std::string& s) {
        if (s.empty() || !(s[0] >= 'a' && s[0] <= 'z')) return false;
        for (char c : s)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        return true;
    };
    if (!scope.empty() && !seg(scope.substr(1))) {
        err = "'" + name + "' has a bad scope — a scope is @ then [a-z][a-z0-9_-]*"; return false;
    }
    if (name[0] == '@' && scope.empty()) { err = "'" + name + "' is missing the /name after its scope"; return false; }
    if (!seg(ident)) {
        err = "'" + name + "' is not a valid package name — lowercase, starting with a letter, then "
              "letters, digits, '_' or '-' (a name is a directory name in the package store, and a "
              "case-insensitive filesystem cannot tell 'Geo' from 'geo')";
        return false;
    }
    if (!mustBeImportable) return true;

    if (ident.find('-') != std::string::npos) {
        std::string suggest = ident;
        for (char& c : suggest) if (c == '-') c = '_';
        err = "a project's name is its root module, and '" + ident + "' is not a legal kama identifier "
              "— try '" + suggest + "'";
        return false;
    }
    if (kamaIsKeyword(ident.c_str())) {
        err = "'" + ident + "' is a kama keyword, so it cannot be a project's root module";
        return false;
    }
    if (ident == "std" || ident == "core") {
        err = "'" + ident + "' is reserved — an import rooted there resolves to the standard library, so "
              "nothing could ever import this package";
        return false;
    }
    // The floor (§2f.29). Reserved by the ordinary project-name uniqueness rule rather than by separate
    // machinery, which is what keeps `global` from being a third kind of scope: it is a name, taken.
    if (ident == "global") {
        err = "'global' is reserved — it names the always-in-scope floor, whose symbols are visible "
              "unqualified in every file, so a project claiming it could not be imported without "
              "colliding with all of them";
        return false;
    }
    return true;
}

static bool seedParseKind(const std::string& s, SeedKind& out)
{
    if (s == "executable" || s == "exe") { out = SeedKind::Executable; return true; }
    if (s == "library"    || s == "lib") { out = SeedKind::Library;    return true; }
    if (s == "monorepo")                 { out = SeedKind::Monorepo;   return true; }
    return false;
}

// Split "a, b ,c" on commas, trimming each. Empty entries are dropped so a trailing comma is harmless.
static std::vector<std::string> seedSplitMembers(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            size_t b = 0, e = cur.size();
            while (b < e && (cur[b] == ' ' || cur[b] == '\t')) ++b;
            while (e > b && (cur[e-1] == ' ' || cur[e-1] == '\t')) --e;
            if (e > b) out.push_back(cur.substr(b, e - b));
            cur.clear();
        } else cur += s[i];
    }
    return out;
}

// The manifest text, generated directly. NOT via the byte-preserving splice mutators (manifestAddDep,
// manifestSetTopString) — those exist to edit a file a HUMAN owns and every one of them requires the
// file to already exist; a seed is the one case with no prior bytes to preserve. Nor via kama.json.h's
// Json, which serializes compact, on one line, which is strictly worse for a file whose first reader is
// a person.
//
// `source` is NOT emitted. It used to be, under the name `sources`, because a library with src/ and no
// such key was silently unimportable — it built for its author and failed for every consumer. `source`
// now defaults to exactly "src", which is the layout seed writes, so emitting it would be emitting the
// default. One way to do a thing.
static std::string seedManifest(SeedKind kind, const std::string& name, const std::string& version,
                                const std::string& license)
{
    std::string m = "{\n";
    m += "  \"name\": \""    + jsonEscape(name)    + "\",\n";
    m += "  \"version\": \"" + jsonEscape(version) + "\"";
    // The SPDX identifier, only when `--license` wrote a body to match: a `license` key with no LICENSE
    // file beside it would be a claim the tree does not back.
    if (!license.empty()) m += ",\n  \"license\": \"" + jsonEscape(license) + "\"";
    if (kind == SeedKind::Executable)
        m += ",\n  \"kind\": \"executable\",\n  \"entry\": \"src/app.kama\"";
    else {
        m += ",\n  \"kind\": \"library\"";
        // The compiler range a library's source needs, seeded as the floor it is written against — the
        // compiler running `seed`. Raised by hand when the source starts to need more, never lowered by
        // guesswork. Omitted when this build reports no version to seed (`0.0.0-dev`).
        SemVer me;
        if (compilerSemVer(me))
            m += ",\n  \"kama\": \">=" + std::to_string(me.major) + "." + std::to_string(me.minor) + "."
               + std::to_string(me.patch) + "\"";
    }
    // The module map, with the one node every project has: `"."`, the files directly under `source`.
    // Seeded rather than left out, even though a one-file project could omit it, because it is the
    // example — the shape someone copies when they add their first folder, and the place the answer to
    // "who may import this?" is written. A library's root IS its published surface, so `public`; an
    // executable has no dependents to distinguish it from `internal`, so it says the narrower thing.
    m += ",\n  \"modules\": {\n    \".\": { \"visibility\": \"";
    m += (kind == SeedKind::Library ? "public" : "internal");
    m += "\" }\n  }";
    return m + "\n}\n";
}

// The MIT body — the text this repo's own LICENSE carries, with the two blanks filled. Generated here
// like seedManifest rather than embedded from `seed/`: the embedding is four named roles wired through
// tools/embed_seed.sh, the Makefile and kama.seed.h, and a fifth for twenty fixed lines is not worth
// the wiring. `--license` accepts `mit` alone today; a second body is a second function beside this
// one, and the refusal at the flag names the list.
static std::string seedLicense(const std::string& holder, const std::string& year)
{
    return "MIT License\n"
           "\n"
           "Copyright (c) " + year + " " + holder + "\n"
           "\n"
           "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
           "of this software and associated documentation files (the \"Software\"), to deal\n"
           "in the Software without restriction, including without limitation the rights\n"
           "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
           "copies of the Software, and to permit persons to whom the Software is\n"
           "furnished to do so, subject to the following conditions:\n"
           "\n"
           "The above copyright notice and this permission notice shall be included in all\n"
           "copies or substantial portions of the Software.\n"
           "\n"
           "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
           "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
           "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
           "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
           "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
           "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
           "SOFTWARE.\n";
}

// `kama_workspace.json`. NO name and NO version, and that is the model rather than an omission: a
// PROJECT is the smallest sharable unit, so it has both; a workspace is only a collection organizing a
// workflow, with no sources, no namespace and no artifact to name or to version.
//
// An explicit member list rather than a "libs/*" glob — kama does not get to invent a directory name for
// somebody else's repository, and the glob is available to anyone who wants it. Every entry says
// `optional` because there is no default: which members may be missing from a checkout is the first
// thing a reader of this file asks.
static std::string seedWorkspace(const std::vector<std::string>& members)
{
    std::string m = "{\n  \"projects\": {\n";
    for (size_t i = 0; i < members.size(); ++i)
        m += "    \"" + jsonEscape(members[i]) + "\": { \"optional\": false }"
           + (i + 1 < members.size() ? ",\n" : "\n");
    return m + "  }\n}\n";
}

int cmdSeed(const std::string& dirArg, const SeedOpts& o)
{
    const std::string dir    = dirArg.empty() ? "." : dirArg;
    const std::string prefix = (dir == ".") ? std::string() : dir + "/";

    // (1) REFUSE FIRST, before a single prompt. Seeding over a real project is destructive in a way
    // --force must not be able to authorize: a manifest carries dependencies, a toolchain pin and a flag
    // universe that nothing here could reconstruct.
    if (fileExists(prefix + "kama.json")) {
        fprintf(stderr, "kama seed: %skama.json exists — this is already a kama project\n", prefix.c_str());
        return 1;
    }
    if (fileExists(prefix + kWorkspaceFile)) {
        fprintf(stderr, "kama seed: %s%s exists — this is already a kama workspace\n",
                prefix.c_str(), kWorkspaceFile);
        return 1;
    }

    // (2) ANSWERS. A terminal gets a prompt for whatever no flag already settled; anything else behaves
    // as --yes. Both conditions matter: --yes wins over a terminal, and a non-terminal needs no --yes.
    //
    // KIND is asked FIRST, because it decides whether the other two questions exist at all: a workspace
    // has no name and no version to ask for.
    const bool ask = !o.yes && stdinIsTerminal();
    std::string kindStr = o.kindGiven ? o.kind : "executable";
    if (ask && !o.kindGiven)    kindStr = seedAsk("kind (executable/library/monorepo)", kindStr);

    // (3) VALIDATE EVERYTHING, still having written nothing.
    SeedKind kind;
    if (!seedParseKind(kindStr, kind)) {
        fprintf(stderr, "kama seed: unknown kind '%s' (expected executable, library or monorepo)\n",
                kindStr.c_str());
        return 2;
    }

    // A workspace has neither, so neither flag may be given rather than being silently dropped — the same
    // reason `--members` on a library is an error. `name` still gets a value here because .gitignore and
    // README.md are written for a DIRECTORY, not for a manifest entry.
    if (kind == SeedKind::Monorepo && (o.nameGiven || o.versionGiven)) {
        fprintf(stderr, "kama seed: --%s does not apply to --kind monorepo — a workspace is not a "
                        "project: it has no name and no version, only the projects it composes\n",
                o.nameGiven ? "name" : "version");
        return 2;
    }
    std::string name = o.nameGiven ? o.name : seedDefaultName(dir);
    if (ask && kind != SeedKind::Monorepo && !o.nameGiven)    name    = seedAsk("package name", name);
    std::string version = o.versionGiven ? o.version : "0.1.0";
    if (ask && kind != SeedKind::Monorepo && !o.versionGiven) version = seedAsk("version", version);

    std::string membersStr = o.members;
    if (kind == SeedKind::Monorepo && ask && !o.membersGiven)
        membersStr = seedAsk("members (comma-separated)", membersStr);
    std::vector<std::string> members = seedSplitMembers(membersStr);
    if (kind == SeedKind::Monorepo && members.empty()) {
        fprintf(stderr, "kama seed: --kind monorepo needs --members <a,b,c> — a monorepo root is a list of "
                        "the projects it composes, and kama does not get to invent their names\n");
        return 2;
    }
    if (kind != SeedKind::Monorepo && !members.empty()) {
        fprintf(stderr, "kama seed: --members applies to --kind monorepo only\n");
        return 2;
    }

    // Only a project has a name and a version to check. A workspace's `name` here never leaves the
    // README, so holding it to the importable rule would be refusing a directory name over a manifest
    // entry that does not exist.
    std::string verr;
    if (kind != SeedKind::Monorepo) {
        if (!seedValidName(name, true, verr)) {
            fprintf(stderr, "kama seed: %s\n", verr.c_str()); return 2;
        }
        SemVer sv;
        if (!parseSemVer(version, sv)) {
            fprintf(stderr, "kama seed: '%s' is not a MAJOR.MINOR.PATCH version\n", version.c_str());
            return 2;
        }
    }
    // A member becomes a library, so it is held to the importable rule — and it is checked HERE, with
    // every other answer, so a bad third member costs nothing rather than half a monorepo.
    std::set<std::string> seenMember;
    for (const auto& m : members) {
        if (!seedValidName(m, true, verr)) {
            fprintf(stderr, "kama seed: member %s\n", verr.c_str()); return 2;
        }
        if (!seenMember.insert(m).second) {
            fprintf(stderr, "kama seed: member '%s' is listed twice\n", m.c_str()); return 2;
        }
    }

    bool agents = o.agents;
    if (ask && !o.agentsGiven) agents = seedAskYesNo("write AGENTS.md so AI agents know this project?", false);

    // `--license`: a flag, not a question — a license is a decision the author brings, and a default
    // would be kama choosing one for somebody else's code. `mit` is the one body seed can write, so any
    // other value is refused BY NAME with the way through, rather than recording an SPDX id the tree does
    // not back. Case-insensitive on the way in; the SPDX spelling ("MIT") on the way out.
    std::string license;
    if (o.licenseGiven) {
        std::string l = o.license;
        for (char& c : l) c = (char)tolower((unsigned char)c);
        if (l != "mit") {
            fprintf(stderr, "kama seed: --license can write `mit` only; for '%s' add the LICENSE file "
                            "yourself and set \"license\" in kama.json\n", o.license.c_str());
            return 2;
        }
        license = "MIT";
    }

    // (4) THE WHOLE FILE LIST, built up front and pre-flighted for collisions, so a seed lands entirely
    // or not at all. embeddedWrite checks per file as it writes, which stops halfway — fine for one file,
    // wrong for a monorepo whose fourth member collides after three have landed.
    const std::string ident = importNameOf(name);
    std::vector<std::pair<std::string, std::string>> files;
    if (kind == SeedKind::Executable) {
        files.push_back({ "kama.json", seedManifest(kind, name, version, license) });
        files.push_back({ "src/app.kama", seedSubst(KAMA_SEED_APP, name, ident) });
    } else if (kind == SeedKind::Library) {
        files.push_back({ "kama.json", seedManifest(kind, name, version, license) });
        files.push_back({ "src/" + ident + ".kama", seedSubst(KAMA_SEED_LIB, name, ident) });
    } else {
        // One LICENSE at the root of the workspace, and every member's manifest says which one — the
        // members are the sharable units, and each is what a registry reads.
        files.push_back({ kWorkspaceFile, seedWorkspace(members) });
        for (const auto& m : members) {
            const std::string mi = importNameOf(m);
            files.push_back({ m + "/kama.json", seedManifest(SeedKind::Library, m, version, license) });
            files.push_back({ m + "/src/" + mi + ".kama", seedSubst(KAMA_SEED_LIB, m, mi) });
        }
    }
    files.push_back({ ".gitignore", seedSubst(KAMA_SEED_GITIGNORE, name, ident) });
    files.push_back({ "README.md",  seedSubst(KAMA_SEED_README,    name, ident) });
    if (!license.empty()) {
        // The holder is git's `user.name` when there is one — the name the commits will carry — and the
        // project's name otherwise, which is at least true. stderr goes to the null device: without git
        // the shell's own "not found" would land in the middle of seed's output.
        int rc = 0;
        std::string holder = runCmdCapture(std::string("git config --get user.name 2>") + KAMA_DEVNULL, &rc);
        if (rc != 0 || holder.empty()) holder = name;
        const time_t now = time(nullptr);
        char year[8] = "";
        if (const struct tm* t = localtime(&now)) strftime(year, sizeof year, "%Y", t);
        files.push_back({ "LICENSE", seedLicense(holder, year) });
    }

    if (!o.force) {
        int clash = 0;
        for (const auto& f : files)
            if (fileExists(prefix + f.first)) {
                fprintf(stderr, "kama seed: %s%s exists\n", prefix.c_str(), f.first.c_str()); ++clash;
            }
        if (clash) { fprintf(stderr, "kama seed: nothing written — pass --force to overwrite\n"); return 1; }
    }

    // (5) WRITE.
    int written = 0;
    for (const auto& f : files)
        if (!embeddedWrite("kama seed", dir, f.first, f.second, o.force, written)) return 1;

    printf("kama seed: %d file%s written.\n", written, written == 1 ? "" : "s");

    // (6) The agent guidance is just the shipped command, called in process (see cmdAgentsInstall). It
    // prints its own summary, so seed's goes above rather than after it.
    if (agents) {
        std::vector<std::string> tools = o.tools;
        if (o.allTools) {
            tools.clear();
            for (int i = 0; i < KAMA_AGENT_STUB_COUNT; ++i) tools.push_back(KAMA_AGENT_STUBS[i].name);
        }
        int rc = cmdAgentsInstall(dir, tools, o.skill, o.force, kind == SeedKind::Library ? "library" : "executable");
        if (rc) return rc;
    }

    // (7) The next step, which differs by kind — a library has nothing to run, and a monorepo root has
    // nothing of its own to build.
    const std::string cd = (dir == ".") ? "" : "cd " + dir + " && ";
    if (kind == SeedKind::Executable)   printf("  next: %skama run\n", cd.c_str());
    else if (kind == SeedKind::Library) printf("  next: %skama check src/%s.kama\n", cd.c_str(), ident.c_str());
    else printf("  next: %skama check %s/src/%s.kama\n", cd.c_str(), members[0].c_str(),
                importNameOf(members[0]).c_str());
    return 0;
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
        "                              (`no-heap`, `link` and `webgpu` are also kama.json keys, per-target overridable)\n"
        "                             [-j|--jobs <n>]   concurrent C compiles (default: core count)\n"
        "                             [--dynamic-runtime]  link the runtime as a DLL instead of statically.\n"
        "                              Windows only in effect (elsewhere libc IS the system, so there is no\n"
        "                              non-system runtime to choose about); a target's `runtime` key in\n"
        "                              kama.json says the same thing per-project. See docs/targets.md.\n"
        "                             [--subsystem console|windows]  the Windows PE subsystem (default\n"
        "                              console). `windows` suppresses the console a GUI program would\n"
        "                              otherwise be given, and reattaches the parent's console so `print`\n"
        "                              still works from a terminal. Windows only in effect; a target's\n"
        "                              `subsystem` key in kama.json says the same thing per-project.\n"
        "                  (pass multiple .kama files to build a multi-file program; --dev also resolves dev-dependencies)\n"
        "  kama run       [<file>] [--release|--debug] [--dev] [--define NAME]... [--config PATH] [-- <program args>]\n"
        "                  (build the entry .kama — explicit <file>, else the manifest \"entry\" — and run it; native-only)\n"
        "  kama check     <in.kama>... [--each] [--json]   analyze without emitting C or invoking a C compiler\n"
        "                  (name resolution, named arguments, ownership/move and serde analysis, and type\n"
        "                   checking by KIND — a `string` cannot initialize an `int32` — and by WIDTH:\n"
        "                   there is no implicit numeric conversion, so `int8 a = big` is an error and\n"
        "                   wants `cast<int8>(big)`. A literal is typed by its destination, so\n"
        "                   `int8 a = 100` is not a conversion at all.\n"
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
        "  kama seed      [<dir>] [--kind executable|library|monorepo]   turn a directory into a kama project\n"
        "                  ([--name N] [--version V] [--members a,b,c] [--agents|--claude|--all-tools|--skill]\n"
        "                   [--license mit] [--yes] [--force]; interactive when stdin is a terminal, else it\n"
        "                   takes the defaults)\n"
        "  kama agents install <kama.json>     write AGENTS.md so an AI agent knows this project + `kama query`\n"
        "                  ([--claude] [--tool <name>]... [--all-tools] [--skill] [--force];\n"
        "                   `kama agents list` shows the tools, `kama agents print` writes to stdout)\n"
        "  kama pkg install <kama.json|kama_workspace.json> [--verify]\n"
        "                                    resolve (dev-)dependencies into .kama/{deps,dev-deps} + kama.lock;\n"
        "                                    a workspace resolves every member, each on its own\n"
        "                                      (--verify: require + check registry-package signatures)\n"
        "  kama pkg add   [--dev] <kama.json> <name> (--git U [--rev R | --version V] | --url U [--integrity H] | --path P |\n"
        "                                --version V [--registry BASE])   (bare --version = a registry dependency)\n"
        "  kama pkg remove <kama.json> <name>\n"
        "  kama pkg update <kama.json> [<pkg>] re-resolve pins (advance a branch pin) and rewrite the lock\n"
        "  kama publish <kama.json> --registry <dir-or-file-uri> [--key <ssh-key>]   tarball + record (+ sign) in the index\n"
        "  kama toolchain list                 installed versions (+ the default and what the cwd resolves to)\n"
        "  kama toolchain install <v>          install version <v> into ~/.kama/versions/<v>\n"
        "  kama toolchain uninstall <v>        remove an installed version\n"
        "  kama toolchain default <v>          set the global default version\n"
        "  kama toolchain pin <v> <kama.json>  pin that project's toolchain in its kama.json\n"
        "  kama update    [--version vX.Y.Z]   install the latest (or <v>) and make it the default\n"
        "  kama --version\n");
}

void pkgUsage()
{
    fprintf(stderr,
        "usage:\n"
        "  kama pkg install <kama.json|kama_workspace.json> [--verify]   resolve dependencies + write kama.lock\n"
        "  kama pkg add   [--dev] <kama.json> <name> (--git U [--rev R | --version V] | --url U [--integrity H] | --path P |\n"
        "                                --version V [--registry BASE])\n"
        "  kama pkg remove <kama.json> <name>\n"
        "  kama pkg update <kama.json> [<pkg>] re-resolve pins and rewrite the lock\n");
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
        "  kama toolchain pin <v> <kama.json>  pin that project's toolchain in its kama.json\n");
}

// Read a file's contents, trimmed of surrounding whitespace ("" if absent/empty). Used for the one-line
// `~/.kama/default` record.
static std::string readTrimmedFile(const std::string& path)
{
    std::ifstream f(osp(path), std::ios::binary);
    if (!f) return "";
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' '||s.back()=='\t')) s.pop_back();
    size_t b = 0; while (b < s.size() && (s[b]==' '||s[b]=='\t'||s[b]=='\n'||s[b]=='\r')) ++b;
    return s.substr(b);
}

// The input file the SELECTOR should resolve a pin for, picked out of raw argv. The selector runs before
// argument parsing — it has to, since its whole job is deciding which binary does the parsing — so it
// cannot ask the option table which token is an input. Hence a deliberately narrow rule: an existing file
// whose name ends in `.kama`.
//
// Narrow in the safe direction. `-o out`, `--cc "zig cc"` and `--target WASM` cannot match, and no one
// names a build OUTPUT `.kama`. If nothing matches we return "" and the caller walks up from the CWD,
// which is exactly what the selector did before it knew about inputs — so the worst case is the old
// behavior, never a worse one.
// The operand the selector should take its pin from. It runs BEFORE argument parsing — it must, since its
// job is choosing which binary does the parsing — so this is a scan of raw argv rather than a parse.
//
// ⚠️ It looks for a MANIFEST, and no longer for a `.kama` file, and that closes a silent bug rather than
// merely simplifying (§2g.37). The old rule matched "an existing file whose name ends in .kama" and then
// WALKED UP from it; hand it `kama build ../legacy/kama.json` and nothing matched, so the pin resolved
// from the current directory and `../legacy` got built by whatever compiler the CWD pins, saying nothing.
//
// There is no walk left either. The operand IS the project, so the pin is READ from the named file:
//
//     …/kama.json            -> that file's `toolchain` (+ its sibling kama.local.json)
//     …/kama_workspace.json  -> NONE: run in place, and re-exec per member (see cmdWorkspaceFanOut)
//     loose files, or none   -> KAMA_VERSION, else the global default
//
// A loose build therefore inherits no project's pin, which follows from a loose file not being a project
// (§2g.33) and is a deliberate behavior change.
//
// `wsOut` is set when the operand names a workspace, which is what tells maybeReExec to run in place.
static std::string selectorManifest(char** argv, bool* wsOut)
{
    *wsOut = false;
    for (int i = 2; argv[i]; ++i) {
        std::string a = argv[i];
        if (!a.empty() && a[0] == '-') continue;
        const std::string base = baseName(a);
        if (base == kWorkspaceFile && fileExists(a)) { *wsOut = true; return ""; }
        if (base == "kama.json" && fileExists(a)) return a;
    }
    return "";
}

// The version the PATH selector should run in the cwd: dev-local override (`kama.local.json` `toolchain`,
// M5.3) → project pin (nearest kama.json `toolchain`) → `KAMA_VERSION` env → global default
// (`~/.kama/default`). "" ⇒ no preference (run this binary as-is). The local override is gitignored, so a
// dev can test against a different toolchain without touching the committed pin.
// `manifest` empty means "no project named": fall through to the environment and the global default.
// (`kama toolchain list` passes nothing and wants the CWD's answer, so it keeps the walk — see its call.)
static std::string resolvePin(const std::string& manifest)
{
    if (!manifest.empty()) {
        std::string local = dirName(manifest) + "/kama.local.json";
        if (std::ifstream(osp(local)).good()) {
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
    std::ifstream in(osp(path), std::ios::binary);
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
    std::ofstream o(osp(path), std::ios::binary | std::ios::trunc);
    if (!o) { err = "cannot write '" + path + "'"; return false; }
    o << out; return true;
}

// `kama toolchain list` — installed versions, the global default, and what the cwd resolves to.
int cmdToolchainList()
{
    std::vector<std::string> versions;
    for (const std::string& n : listDir(versionsDir()))
        if (dirExists(versionDir(n))) versions.push_back(n);
    std::sort(versions.begin(), versions.end());
    std::string def = readTrimmedFile(defaultVersionFile());
    // The one caller that still WALKS: `kama toolchain list` reports what this DIRECTORY resolves to, and
    // there is no operand in the question.
    std::string resolved = resolvePin(projectManifestPath({}));
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
    std::ofstream o(osp(defaultVersionFile()), std::ios::binary | std::ios::trunc);
    if (!o) { fprintf(stderr, "kama toolchain default: cannot write %s\n", defaultVersionFile().c_str()); return 1; }
    o << v << "\n";
    printf("default is now kama %s\n", v.c_str());
    return 0;
}

// `kama toolchain pin <v>` — write `"toolchain": "<v>"` into ./kama.json (byte-preserving).
// `manifest` is the project to pin, NAMED. It used to be whatever `./kama.json` happened to be, which
// made the current directory decide which file got rewritten.
int cmdToolchainPin(const std::string& v, const std::string& manifest)
{
    if (!fileExists(manifest)) { fprintf(stderr, "kama toolchain pin: %s does not exist\n", manifest.c_str()); return 2; }
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
    bool workspace = false;
    std::string manifest = selectorManifest(argv, &workspace);
    // ⚠️ RUN IN PLACE FOR A WORKSPACE, and this is load-bearing rather than an optimization. The export
    // below is the loop-stopper, and a child inherits it — so if this process selected a version for
    // ITSELF, every member the fan-out spawns would skip selection and be built by that one compiler
    // instead of its own pin. Never exec'ing here is what lets each member start clean. (A user who
    // exports KAMA_NO_SELECT themselves is using the documented escape hatch, and it reaching members is
    // then correct.)
    if (workspace) return;
    std::string v = resolvePin(manifest);
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

// The project a PROJECT-ACTING command acts on, from its manifest operand (§2g.35). These commands
// mutate or publish a project — `pkg install/add/remove/update`, `publish`, `agents install`,
// `toolchain pin` — and every one of them used to default to the current directory. That is the same
// implicit gesture the operand rule removes from the build commands, and it is worse here: the CWD
// decided which manifest got REWRITTEN.
//
// Returns the project directory, or "" having printed the error. `*workspace` is set when the operand
// names a workspace; only the commands that can act on all of them at once accept that.
// `example` spells the whole invocation for the hint, because the manifest is not always the last
// operand — `toolchain pin` takes a version first.
static std::string projectOperandDir(const std::string& cmd, const std::string& operand, bool* workspace,
                                     const std::string& example = "")
{
    *workspace = false;
    const std::string hint = example.empty() ? "kama " + cmd + " kama.json" : example;
    if (operand.empty()) {
        fprintf(stderr, "kama %s: name the project to act on — `%s`\n", cmd.c_str(), hint.c_str());
        return "";
    }
    const std::string base = baseName(operand);
    if (base == kWorkspaceFile) {
        if (!fileExists(operand)) { fprintf(stderr, "kama %s: %s does not exist\n", cmd.c_str(), operand.c_str()); return ""; }
        *workspace = true;
        return dirName(operand);
    }
    if (base != "kama.json") {
        // Naming a DIRECTORY used to be the spelling, so say what to change rather than "unexpected arg".
        fprintf(stderr, "kama %s: name the project's manifest, not %s — `kama %s %s%skama.json`\n",
                cmd.c_str(), operand.c_str(), cmd.c_str(), operand.c_str(),
                operand.empty() || operand.back() == '/' ? "" : "/");
        return "";
    }
    if (!fileExists(operand)) { fprintf(stderr, "kama %s: %s does not exist\n", cmd.c_str(), operand.c_str()); return ""; }
    return dirName(operand);
}

// A member's path SPELLED THE WAY THE CALLER SPELLED THE WORKSPACE. expandWorkspace answers with absolute
// paths, because membership is a set comparison — but these strings go into a command the user is meant to
// be able to copy, and `kama run /private/tmp/…/apps/cli/kama.json` is not what they typed. So the
// absolute workspace root is swapped back for the operand's own spelling.
static std::string memberAsSpelled(const std::string& wsFileAsSpelled, const std::string& memberAbs)
{
    const std::string spelledDir = dirName(wsFileAsSpelled);
    const std::string absDir     = absolutePath(spelledDir);
    if (memberAbs.size() > absDir.size() + 1 && memberAbs.compare(0, absDir.size(), absDir) == 0
        && memberAbs[absDir.size()] == '/') {
        const std::string tail = memberAbs.substr(absDir.size() + 1);
        return spelledDir == "." ? tail : spelledDir + "/" + tail;
    }
    return relativizeToCwd(memberAbs);   // not under the root (a symlink): the absolute path is the honest answer
}

// `kama build|check kama_workspace.json` — run the same command once per member, each on its own project
// (§2g.36). A workspace is never one big program: members are separate projects that must each build
// standalone, and compiling them together would give one artifact where the workspace declares N.
//
// Driven as a CHILD PROCESS per member rather than a loop inside this one, and that is load-bearing three
// times over:
//   * Every member re-runs the toolchain selector and so gets ITS OWN pin (§2g.38). A loop here would
//     build every member with whatever compiler this process happens to be.
//   * Global emitter state (`g_target`, `g_activeFlags`, the flag universe) is installed per invocation
//     and is not re-entrant; a second member in-process would inherit the first one's configuration.
//   * A member that fails to build stops the run with its own exit code, unedited.
// Recursion terminates because a member's operand is always a `kama.json`, never a workspace.
static int cmdWorkspaceFanOut(char** argv, int argc, const std::string& subcommand,
                              const std::string& wsFile)
{
    const std::string wsDir = dirName(wsFile);
    std::set<std::string> members;
    std::string err;
    if (!expandWorkspace(wsDir, members, err)) { fprintf(stderr, "kama: %s\n", err.c_str()); return 2; }
    if (members.empty()) {
        fprintf(stderr, "kama: %s lists no projects\n", wsFile.c_str()); return 2;
    }

    // Everything the caller passed EXCEPT the operand: flags apply to every member, since they are this
    // invocation's choices (`--release`, `--target`) rather than any one project's.
    std::vector<std::string> flags;
    for (int i = 2; i < argc; ++i)
        if (argv[i] != wsFile) flags.push_back(argv[i]);

    const std::string self = selfExePath(argv[0]);
    int n = 0;
    for (const auto& m : members) {
        const std::string rel = memberAsSpelled(wsFile, m);
        printf("kama %s: %s\n", subcommand.c_str(), rel.c_str());
        fflush(stdout);
        std::ostringstream cmd;
        cmd << "\"" << self << "\" " << subcommand << " \"" << rel << "/kama.json\"";
        for (const auto& f : flags) cmd << " \"" << f << "\"";
        int rc = runCmd(cmd.str());   // same quoting convention as `kama run`'s child (runCmd handles cmd.exe)
        // ⚠️ KAMA_NO_SELECT is deliberately NOT set around this. maybeReExec exports it just before it
        // execs, as its loop-stopper, and a child inherits it — so if this process had selected for
        // itself, every member would silently skip selection and build with THIS compiler rather than
        // its own pin. A workspace operand runs in place precisely so that each member starts clean.
        if (rc != 0) {
            fprintf(stderr, "kama %s: %s failed\n", subcommand.c_str(), rel.c_str());
            return rc;
        }
        ++n;
    }
    printf("kama %s: %d project(s) in %s\n", subcommand.c_str(), n, wsFile.c_str());
    return 0;
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

// Collect one project's sources — its `source` root, walked recursively, which is exactly its files.
//
// No recursion into anything, and no cycle break, because a project no longer composes projects: the
// workspace above it does, and that file cannot nest either. collectWorkspaceFiles is the union.
//
//   no kama.json here -> NOT a project: contributes nothing at all. There is deliberately no `else` arm
//                        walking the directory wholesale — that is how a member naming a manifest-less
//                        directory used to swallow the whole directory as though it had been declared.
//                        (With `source` always present for a real manifest it could never fire for one
//                        anyway, and expandProjectsEntry now refuses to expand that shape at all.)
static void collectPackageTree(const std::string& dir, std::vector<std::string>& out)
{
    const std::string& srcRel = manifestSourceCached(dir + "/kama.json");
    if (srcRel.empty()) return;
    // absolutePath, not the lexical join packageSourceFiles uses: what THIS returns is handed to
    // lspAnalyzeWorkspace as CLI inputs and compared against URI-derived absolute paths, so it must be
    // canonical. The two callers want opposite things from the same data — see packageSourceFiles.
    std::string abs = absolutePath(dir + "/" + srcRel);
    // A source root that does not exist is skipped: a manifest may name a directory not created yet, and
    // refusing to index everything else over that would be hostile.
    if (!dirExists(abs)) return;
    size_t seen = 0;
    collectKamaFiles(abs, "", out, seen, (size_t)-1);
}

std::string lspRealPath(const std::string& path) { return absolutePath(path); }

// Defined HERE rather than beside manifestSourceCache: most of this file sits inside the anonymous
// namespace opened at the top, which would give this internal linkage and leave kama.lsp.o with an
// undefined symbol. The cache accessor stays where it is — an anonymous-namespace name is visible
// through the rest of the translation unit, so reaching back to it from out here is fine.
void lspEvictManifestCache() { manifestSourceCache().clear(); manifestModulesCache().clear(); }

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
    return base == "kama.json" || base == "kama.local.json" || base == kWorkspaceFile;
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

    // Walk up collecting manifest directories, nearest first, and noting the workspace file if one is
    // passed on the way. Bounded by the editor's folder when we have one; a `.kama` component stops the
    // walk so a vendored dep never escapes into its host project.
    std::vector<std::string> manifests;
    std::string wsDir;
    std::string cur = dir;
    for (;;) {
        if (baseName(cur) == ".kama") break;
        if (fileExists(cur + "/kama.json")) manifests.push_back(cur);
        if (wsDir.empty() && fileExists(cur + "/" + kWorkspaceFile)) wsDir = cur;
        if (underWorkspace && cur == wsRoot) break;      // examined it, go no higher
        std::string parent = dirName(cur);
        if (parent == cur || parent == ".") break;       // filesystem root
        cur = parent;
    }

    // DECLARED beats inferred. If an ancestor manifest's `source` root explicitly OWNS this file, then
    // widening to it is not a guess and needs no editor boundary to license it. NEAREST wins, because a
    // project no longer composes projects — a manifest further up cannot own a nearer project's file.
    //
    // CONTAINMENT IS THE WHOLE RULE. There used to be a `srcs.empty() && subs.empty()` guard here reading
    // "declares nothing: not an owner", and it was load-bearing in a way its comment did not say: it was
    // the only thing keeping collectPackageTree's whole-directory arm from running for a manifest that
    // declared nothing, trivially "containing" the open file, and being crowned a DECLARED owner with an
    // uncapped file set — the guessed set the cap exists to bound, laundered as a declaration. Now that a
    // manifest always has a source root, the guard would be dead anyway; deleting the arm is what makes
    // containment sound on its own, so the two changes belong together.
    std::string self = absolutePath(openFilePath);
    for (auto it = manifests.begin(); it != manifests.end() && p.root.empty(); ++it) {
        std::vector<std::string> files;
        collectPackageTree(*it, files);
        for (const auto& f : files)
            if (absolutePath(f) == self) {
                p.root        = *it;
                p.hasManifest = true;
                p.files       = files;
                break;
            }
    }
    if (!p.root.empty()) {
        // A workspace that LISTS this project widens the scope to every member, which is what makes a
        // rename in libs/core reach libs/ui. Listing is the whole test: a workspace overhead does not get
        // to claim a project it never named, and a member is skipped silently when it is not checked out.
        if (!wsDir.empty()) {
            std::set<std::string> members;
            std::string werr;
            if (expandWorkspace(wsDir, members, werr) && members.count(absolutePath(p.root))) {
                p.root  = wsDir;
                p.files.clear();
                for (const auto& m : members) collectPackageTree(m, p.files);
            }
        }
        std::sort(p.files.begin(), p.files.end());
        p.files.erase(std::unique(p.files.begin(), p.files.end()), p.files.end());
        p.seenCount = p.files.size();
        return p;
    }

    if (!manifests.empty()) {
        // Nothing declared ownership, so this IS an inference. Under the editor's folder the outermost
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
    // root. That inference is what the cap bounds — see kLspMaxProjectFiles. A kama.json whose `source`
    // root CONTAINS this file ends it, and a kama_workspace.json listing that project widens it.
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
    //
    // The ONE caller that opts out of closure pruning. This index backs find-references, rename and
    // workspace symbols, and pruning would shrink it: a rename would silently skip files it did not load.
    // The project's own files are CLI inputs and so are never pruned either way, so what is at stake is
    // dependencies — for the read-only stdlib that would be tolerable, but a workspace sibling package is
    // code the user can legitimately edit. Per-keystroke analysis (lspAnalyze) still prunes, which is
    // where the latency actually is.
    std::vector<SharedCompilationUnit> units;
    std::vector<std::string> paths;
    if (!loadProgramUnits(files, argv0, units, paths, /*includeDevDeps*/ false, /*strictImports*/ false,
                          /*diagsOut*/ nullptr, /*prune*/ false) || units.empty())
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

LspImportInsertion lspImportInsertion(const SharedLspIndex& idx, const std::string& path)
{
    LspImportInsertion ins;
    if (!idx || !idx->idx) return ins;
    ins.at = idx->idx->importInsertionAt(path, ins.hasBlock);
    return ins;
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
//
// The file's own DIRECTORY is not one of them any more, and its PROJECT is — the same swap §2i made in
// the loader. The editor is the one caller that still walks to find that project, because it is handed a
// buffer and never a command line (§2g.35), which is why this reads projectManifestDir rather than being
// told: for `kama lsp` the CLI tri-state is unset, so the walk happens.
static std::vector<std::string> lspImportRoots(const std::string& fromPath, bool reserved, const char* argv0)
{
    std::vector<std::string> roots;
    if (!reserved) {
        std::string proj = projectManifestDir({ fromPath });
        if (!proj.empty()) roots.push_back(proj);
        for (auto& r : splitSearchPath(getenv("KAMA_PATH"))) roots.push_back(r);
        std::string dv = projectDepsView({ fromPath });
        if (!dv.empty()) roots.push_back(dv);
    }
    roots.push_back(resolveStdlibDir(argv0));
    return roots;
}

// The module chain below the project: segments 1.. joined, "" when only the project is named.
static std::string prefixChain(const std::vector<std::string>& segs)
{
    std::string c;
    for (size_t i = 1; i < segs.size(); ++i) c += (i > 1 ? "::" : "") + segs[i];
    return c;
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

// Every composed module name in a tree, flattened.
static void collectModNames(const std::vector<ModuleNode>& nodes, std::vector<std::string>& out)
{
    for (auto& n : nodes) {
        if (!n.isRoot()) out.push_back(n.modName);
        collectModNames(n.children, out);
    }
}

// What can follow `import <prefix>` — the next segment, offered from the same MANIFESTS resolution reads.
//
// ⚠️ It used to read DIRECTORIES: every subdirectory of every import root, plus every `.kama` file
// basename, because a file was a module (§2b.9) and a directory-module was whatever happened to be on
// disk. Both are gone, so a completion offering them would offer names that no longer resolve — the worst
// kind of completion. A module is something a project DECLARES now, so this asks the projects.
std::vector<std::string> lspImportModules(const std::string& fromPath, const std::string& prefix,
                                          const char* argv0)
{
    std::string rel;
    std::vector<std::string> segs = lspSplitModulePath(prefix, rel);
    std::set<std::string> out;                       // sorted + deduped across roots

    // The top level names PROJECTS: the stdlib, the file's own project, and every dependency it declares.
    if (segs.empty()) {
        out.insert("std");
        const std::string proj = projectManifestDir({ fromPath });
        if (!proj.empty()) {
            const ManifestModules& mm = manifestModulesCached(proj + "/kama.json");
            if (!mm.projectName.empty()) out.insert(mm.projectName);
            for (const auto& d : declaredImportNames(proj)) out.insert(d);
        }
        return { out.begin(), out.end() };
    }

    // Below it, the modules of the project the first segment names.
    const std::string chain = prefixChain(segs);   // "" when only the project is named
    for (const auto& root : lspImportRoots(fromPath, segs[0] == "std" || segs[0] == "core", argv0)) {
        for (const std::string& p : { root, root + "/" + segs[0] }) {
            const std::string manifest = p + "/kama.json";
            if (!fileExists(manifest)) continue;
            const ManifestModules& mm = manifestModulesCached(manifest);
            if (mm.projectName != segs[0]) continue;
            std::vector<std::string> names;
            collectModNames(mm.mods, names);
            for (const auto& m : names) {
                if (chain.empty()) {
                    if (m.find("::") == std::string::npos) out.insert(m);
                } else if (m.size() > chain.size() + 2 && m.compare(0, chain.size(), chain) == 0
                           && m.compare(chain.size(), 2, "::") == 0) {
                    const std::string rest = m.substr(chain.size() + 2);
                    if (rest.find("::") == std::string::npos) out.insert(rest);
                }
            }
        }
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

// Every composed module name reachable from `fromPath`, breadth-first through lspImportModules — the top
// level (the stdlib, this file's own project, its declared dependencies), then each of those projects'
// modules, and so on. A project ROOT is a module too (`geo`, declared `"."`), so a name is BOTH a result
// and a prefix to descend from.
//
// Bounded on both axes rather than trusted to terminate: `modules` is a declared map that a hand-written
// manifest can nest arbitrarily, and this walk is on an interactive path. Blowing either bound narrows
// what auto-import can offer; it can never wedge the server.
static std::vector<std::string> lspAllModuleNames(const std::string& fromPath, const char* argv0)
{
    static const size_t kMaxModules = 256;
    static const int    kMaxDepth   = 6;
    std::vector<std::string> out;
    std::set<std::string>    seen;
    std::vector<std::string> frontier{ std::string() };      // "" = the top level
    for (int depth = 0; depth < kMaxDepth && !frontier.empty() && out.size() < kMaxModules; ++depth) {
        std::vector<std::string> next;
        for (const auto& prefix : frontier) {
            for (const auto& seg : lspImportModules(fromPath, prefix, argv0)) {
                const std::string full = prefix.empty() ? seg : prefix + "::" + seg;
                if (!seen.insert(full).second) continue;
                out.push_back(full);
                next.push_back(full);
                if (out.size() >= kMaxModules) break;
            }
            if (out.size() >= kMaxModules) break;
        }
        frontier.swap(next);
    }
    return out;
}

// Every `import` spelling that would bring `symbol` into scope from `fromPath` — what the auto-import
// quick fix offers, in the order it offers them.
//
// Two rungs, and the ORDER is the module campaign's own shape rather than a preference. A SIBLING in this
// file's own module comes first and is spelled bare (`import { Helper };`): that is the case the campaign
// created — a name that needed no import before now needs one — and it is the diagnostic the phase-3b
// message already names the fix for. Everything else is a module away and is spelled qualified
// (`import { std::collections::DynamicArray };`).
//
// Answers from the FILESYSTEM and the module resolver, never from the query index, for the same reason
// lspImportSymbols does: a symbol the file is about to import is by definition one it does not import,
// so it is in no index. That costs a parse of each candidate module's files — per GESTURE (a lightbulb),
// never per keystroke, and the server holds the parse cache open, so a second lightbulb on the same file
// is free.
std::vector<std::string> lspImportCandidates(const std::string& fromPath, const std::string& symbol,
                                             const char* argv0)
{
    static const size_t kMaxCandidates = 16;
    if (symbol.empty()) return {};
    std::vector<std::string> out;
    std::set<std::string>    seen;
    auto offer = [&](const std::string& spelling) {
        if (out.size() < kMaxCandidates && seen.insert(spelling).second) out.push_back(spelling);
    };

    // (1) A sibling of this file's own module. Same-module membership is asked of moduleIdForFile rather
    // than assumed from the directory: that is the one derivation of "which module owns this file?", and
    // a second one here would be exactly the disagreement between two spellings the campaign removed.
    // (Directory-local only, so a module spread over an unlisted subfolder is under-offered rather than
    // mis-offered — rung 2 still reaches it, qualified.)
    const std::string mine = moduleIdForFile(fromPath).full();
    const std::string self = absolutePath(fromPath);
    for (const auto& f : listKamaFiles(dirName(absolutePath(fromPath)))) {
        if (absolutePath(f) == self) continue;                    // its own declarations need no import
        if (moduleIdForFile(f).full() != mine) continue;
        SharedCompilationUnit u = parseFile(f);
        if (!u || !u->exportList) continue;                       // unexported: an import cannot accept it
        for (auto& n : *u->exportList)
            if (n && *n == symbol) { offer(symbol); break; }
    }

    // (2) Every other reachable module, by its `export` manifest.
    for (const auto& mod : lspAllModuleNames(fromPath, argv0)) {
        if (mod == mine) continue;                                // rung 1 owns this one, spelled bare
        for (const auto& sym : lspImportSymbols(fromPath, mod, argv0))
            if (sym == symbol) { offer(mod + "::" + symbol); break; }
        if (out.size() >= kMaxCandidates) break;
    }
    return out;
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
#ifdef _WIN32
    // Binary stdio, before a single byte moves. The CRT opens fd 0/1/2 in TEXT mode, where every '\n' on
    // the way out becomes "\r\n" and every "\r\n" on the way in loses its '\r'. kama's standard streams
    // carry BYTES, not a document:
    //   * `kama agents print` must reproduce the embedded file exactly — check-agents diffs it, and the
    //     CR per line read as "the binary's AGENTS.md differs from agents/AGENTS.md";
    //   * `kama lsp` frames JSON-RPC with a Content-Length counted in bytes, and writes the header's
    //     \r\n itself. Text mode turns those into \r\r\n and makes every byte count a lie, which is the
    //     kind of corruption that shows up as an editor mysteriously losing half its replies.
    // Same decision, same reasoning, as kama_args_init() in kama_runtime.h does for compiled programs.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("kama %s\n", KAMA_VERSION);
        return 0;
    }
    if (argc < 2) { usage(); return 2; }

    std::string subcommand = argv[1];
    g_argv0 = argv[0];               // the exe-relative resolvers' anchor (runtime headers, stdlib, prelude)

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
        // `pin` is the one verb here that touches a PROJECT rather than the installation, so it is the one
        // that names a manifest: `kama toolchain pin <version> <kama.json>`. The rest manage ~/.kama.
        if (verb == "pin") {
            std::string v, manifest;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama toolchain pin: unexpected option '%s'\n", a.c_str()); return 2; }
                else if (v.empty()) v = a;
                else if (manifest.empty()) manifest = a;
                else { fprintf(stderr, "kama toolchain pin: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            if (v.empty()) { fprintf(stderr, "kama toolchain pin: missing <version>\n"); return 2; }
            bool ws = false;
            std::string dir = projectOperandDir("toolchain pin", manifest, &ws,
                                                "kama toolchain pin " + v + " kama.json");
            if (dir.empty()) return 2;
            if (ws) {
                // §2a's extractability invariant: a project never reads its workspace file for anything
                // that affects compilation, and which compiler runs plainly is that. Each member pins
                // itself, which is also what makes a workspace build re-exec per member.
                fprintf(stderr, "kama toolchain pin: a workspace carries no `toolchain` — each project "
                                "pins itself. Name a member's kama.json\n");
                return 2;
            }
            return cmdToolchainPin(v, manifest);
        }
        std::string v = oneArg(verb.c_str());
        if (v == "\x01") return 2;                     // an option/extra-arg error was already printed
        if (v.empty()) { fprintf(stderr, "kama toolchain %s: missing <version>\n", verb.c_str()); return 2; }
        if (verb == "install")   return cmdToolchainInstall(v);
        if (verb == "uninstall") return cmdToolchainUninstall(v);
        if (verb == "default")   return cmdToolchainDefault(v);
        fprintf(stderr, "kama toolchain: unknown command '%s'\n", verb.c_str()); toolchainUsage(); return 2;
    }

    if (subcommand == "lsp") {
        // The `kama lsp` language server (M1): a JSON-RPC 2.0 server over stdio that reuses the
        // front-end-as-library analysis path to publish live diagnostics. Takes no input file (it reads
        // buffers from the editor over the wire), so it returns here before the input/flag handling below.
        return runLspServer(argv[0]);   // argv[0] locates the stdlib for loading imported modules
    }

    if (subcommand == "seed") {
        // An early-return command: it touches no .kama source, so it returns before the shared
        // input/flag handling (same reason as `agents` and `lsp`). Deliberately NOT in maybeReExec's
        // run-in-place list either: the templates and the manifest shape are version-specific, so a seed
        // run under a pinned toolchain should produce THAT toolchain's files.
        //
        // ⚠️ It names no manifest, so with the selector reading the operand (§2g.37) a seed now resolves
        // KAMA_VERSION or the global default — never an ancestor's pin. The monorepo case this comment
        // used to describe went away with the aggregator manifest: a workspace root has no `kama.json`
        // and may not carry a `toolchain`, because a project never reads its workspace file for anything
        // affecting compilation. Each project pins itself, which is the whole point.
        SeedOpts o;
        std::string dir;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--name"    && i + 1 < argc) { o.name    = argv[++i]; o.nameGiven    = true; }
            else if (a == "--version" && i + 1 < argc) { o.version = argv[++i]; o.versionGiven = true; }
            else if (a == "--kind"    && i + 1 < argc) { o.kind    = argv[++i]; o.kindGiven    = true; }
            else if (a == "--members" && i + 1 < argc) { o.members = argv[++i]; o.membersGiven = true; }
            else if (a == "--agents")                  { o.agents = true;  o.agentsGiven = true; }
            else if (a == "--no-agents")               { o.agents = false; o.agentsGiven = true; }
            // Every tool selector implies --agents: asking for CLAUDE.md and not getting AGENTS.md,
            // which is the file it points AT, would be a pointer to nothing.
            else if (a == "--claude")   { o.tools.push_back("claude"); o.agents = true; o.agentsGiven = true; }
            else if (a == "--tool" && i + 1 < argc) { o.tools.push_back(argv[++i]); o.agents = true; o.agentsGiven = true; }
            else if (a == "--all-tools") { o.allTools = true; o.agents = true; o.agentsGiven = true; }
            else if (a == "--skill")     { o.skill = true;    o.agents = true; o.agentsGiven = true; }
            else if (a == "--license" && i + 1 < argc) { o.license = argv[++i]; o.licenseGiven = true; }
            else if (a == "--yes" || a == "-y")        o.yes = true;
            else if (a == "--force")                   o.force = true;
            else if (!a.empty() && a[0] == '-') {
                fprintf(stderr, "kama seed: unknown option '%s'\n", a.c_str()); seedUsage(); return 2;
            }
            else if (dir.empty())                      dir = a;
            else { fprintf(stderr, "kama seed: unexpected arg '%s'\n", a.c_str()); return 2; }
        }
        return cmdSeed(dir, o);
    }

    if (subcommand == "agents") {
        // An early-return command: it touches no .kama source, so it returns before the shared
        // input/flag handling. It is deliberately NOT in maybeReExec's run-in-place list — the
        // guidance is version-specific, so a pinned project should get its pinned toolchain's copy.
        std::string verb = argc > 2 && argv[2][0] != '-' ? argv[2] : "";
        bool wantClaude = false, allTools = false, skill = false, force = false, package = false;
        std::string dir;
        std::vector<std::string> tools;
        for (int i = verb.empty() ? 2 : 3; i < argc; ++i) {
            std::string a = argv[i];
            if      (a == "--claude")                 wantClaude = true;
            else if (a == "--all-tools")              allTools = true;
            else if (a == "--skill")                  skill = true;
            else if (a == "--package")                package = true;
            else if (a == "--force")                  force = true;
            else if (a == "--tool" && i + 1 < argc)   tools.push_back(argv[++i]);
            else if (!a.empty() && a[0] == '-') {
                fprintf(stderr, "kama agents: unknown option '%s'\n", a.c_str()); agentsUsage(); return 2;
            }
            else if (dir.empty())                     dir = a;
            else { fprintf(stderr, "kama agents: unexpected arg '%s'\n", a.c_str()); return 2; }
        }
        if (verb.empty() || verb == "list") {
            if (verb.empty() && (wantClaude || allTools || skill || package || force || !tools.empty() || !dir.empty())) {
                fprintf(stderr, "kama agents: say `install` or `print`\n"); agentsUsage(); return 2;
            }
            return verb == "list" ? cmdAgentsList() : (agentsUsage(), 2);
        }
        if (verb == "print") {
            // The primitive the whole feature rests on: stdout, so it composes with anything —
            // pasting into a file this command has never heard of, or diffing against one.
            printf("%s", embeddedBody(package ? KAMA_AGENTS_PACKAGE : skill ? KAMA_AGENTS_SKILL : KAMA_AGENTS_MD));
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
        // `install` WRITES into a project (AGENTS.md and the tool stubs beside it), so it names the one
        // it writes into. `list` and `print` take no project and never reach here. Seeding calls
        // cmdAgentsInstall directly with the directory it just created, which is not a CWD default.
        bool agWs = false;
        std::string agDir = projectOperandDir("agents install", dir, &agWs);
        if (agDir.empty()) return 2;
        if (agWs) {
            fprintf(stderr, "kama agents install: guidance is written per project — name a member's kama.json\n");
            return 2;
        }
        if (package) { fprintf(stderr, "kama agents install: --package is a `print` option — install writes the package half by the manifest's kind\n"); return 2; }
        // The operand is the manifest itself (projectOperandDir insisted on that), so its kind is one read
        // away — and a library gets AGENTS.package.md by that fact, never by a flag (see cmdAgentsInstall).
        // A manifest with no `kind` is treated as an executable here rather than refused: this command
        // writes guidance, it does not police the manifest — `kama build` does that.
        std::string kind, kerr;
        if (!loadManifestKind(dir, kind, kerr)) { fprintf(stderr, "kama agents install: %s\n", kerr.c_str()); return 2; }
        return cmdAgentsInstall(agDir, tools, skill, force, kind);
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
        bool pubWs = false;
        std::string pubDir = projectOperandDir("publish", dir, &pubWs);
        if (pubDir.empty()) return 2;
        if (pubWs) {
            // A workspace is not a publishable unit: its members are separate packages with separate
            // names and versions, and publishing "it" would have to mean publishing N of them in an
            // order nothing here knows. Name the one, exactly as `run` does.
            std::set<std::string> members; std::string werr;
            expandWorkspace(pubDir, members, werr);
            fprintf(stderr, "kama publish: %s is a workspace of %zu project(s) — publish one at a time:\n",
                    dir.c_str(), members.size());
            for (const auto& m : members)
                fprintf(stderr, "    kama publish %s/kama.json\n", memberAsSpelled(dir, m).c_str());
            return 2;
        }
        return cmdPublish(pubDir, registry, key);
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
            bool insWs = false;
            std::string insDir = projectOperandDir("pkg install", dir, &insWs);
            if (insDir.empty()) return 2;
            if (!insWs) return cmdInstall(insDir);
            // Fanned out, not merged: every member resolves against ITS OWN manifest and gets its own
            // lock and its own `.kama/deps`. That is what keeps a member extractable — a shared
            // resolution would silently make each one depend on the others being present.
            std::set<std::string> members; std::string werr;
            if (!expandWorkspace(insDir, members, werr)) { fprintf(stderr, "kama: %s\n", werr.c_str()); return 2; }
            for (const auto& m : members) {
                const std::string rel = memberAsSpelled(dir, m);
                printf("kama pkg install: %s\n", rel.c_str());
                fflush(stdout);
                int rc = cmdInstall(rel);
                if (rc != 0) return rc;
            }
            printf("kama pkg install: %zu project(s) in %s\n", members.size(), dir.c_str());
            return 0;
        }
        // The MANIFEST comes first and the package name second, for update/add/remove alike: the manifest
        // is the scope and the name is what is being done to it — the same order `kama query <manifest>
        // <file>` uses, and the same reason.
        if (verb == "update") {
            std::string manifest, pkg;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg update: unexpected option '%s'\n", a.c_str()); return 2; }
                else if (manifest.empty()) manifest = a;
                else if (pkg.empty()) pkg = a;
                else { fprintf(stderr, "kama pkg update: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            bool ws = false;
            std::string dir = projectOperandDir("pkg update", manifest, &ws);
            if (dir.empty()) return 2;
            if (ws) { fprintf(stderr, "kama pkg update: rewrites one project's lock — name a member's kama.json\n"); return 2; }
            return cmdPkgUpdate(dir, pkg);
        }
        if (verb == "add") {
            bool dev = false; std::string manifest, name; DepSpec d; std::string rev, integ, ver, registry;
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
                else if (manifest.empty()) manifest = a;
                else if (name.empty()) name = a;
                else { fprintf(stderr, "kama pkg add: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            bool ws = false;
            std::string addDir = projectOperandDir("pkg add", manifest, &ws, "kama pkg add kama.json <name> …");
            if (addDir.empty()) return 2;
            if (ws) { fprintf(stderr, "kama pkg add: edits one project's manifest — name a member's kama.json\n"); return 2; }
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
            return cmdPkgAdd(addDir, name, d, dev);
        }
        if (verb == "remove") {
            std::string manifest, name;
            for (int i = 3; i < argc; ++i) {
                std::string a = argv[i];
                if (!a.empty() && a[0] == '-') { fprintf(stderr, "kama pkg remove: unexpected option '%s'\n", a.c_str()); return 2; }
                else if (manifest.empty()) manifest = a;
                else if (name.empty()) name = a;
                else { fprintf(stderr, "kama pkg remove: unexpected arg '%s'\n", a.c_str()); return 2; }
            }
            bool ws = false;
            std::string dir = projectOperandDir("pkg remove", manifest, &ws, "kama pkg remove kama.json <name>");
            if (dir.empty()) return 2;
            if (ws) { fprintf(stderr, "kama pkg remove: edits one project's manifest — name a member's kama.json\n"); return 2; }
            if (name.empty()) { fprintf(stderr, "kama pkg remove: missing <name>\n"); return 2; }
            return cmdPkgRemove(dir, name);
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
    bool dynamicRuntime    = false;        // --dynamic-runtime: link the runtime as a DLL (Windows opt-in)
    std::string cliSubsystem;              // --subsystem console|windows: the PE subsystem (Windows opt-in)
    std::vector<std::string> defines;      // --define NAME: activate a `@compileFor` flag (repeatable)
    std::vector<std::string> selects;      // --select GROUP=VALUE: pick a single-select group (repeatable)
    std::vector<std::string> undefines;    // --undefine NAME: deactivate a default flag (repeatable)
    // THE OPERAND IS THE MODE (§2g). One rule, checked below before anything else runs: an operand whose
    // BASENAME is `kama.json` names a project, `kama_workspace.json` a workspace, and anything else is a
    // loose source file. Operands are N `.kama` files XOR exactly one manifest, so "these files, and also
    // that project" is not rejected so much as unspellable.
    //
    // This replaces DISCOVERY. `kama build src/app.kama` inside a project used to walk up, find the
    // manifest and silently apply it, so the three modes were mixed by default and there was no spelling
    // that meant "just these files". `--config` and `--project` are gone with it: the operand IS the
    // manifest, and the operand IS the scope.
    std::string manifestOperand;           // the one manifest operand, if any (project or workspace)
    bool        workspaceMode = false;     // ...and whether its basename was kama_workspace.json
    bool        devBuild   = false;        // --dev: also put .kama/dev-deps on the import path (dev-dependencies)
    bool        eachMode   = false;        // `kama check --each`: every input is its own program, one process
    int         buildJobs  = 0;            // -j/--jobs: concurrent C compiles; 0 => resolve from env/cores
    // Every `kama query` mode, in the order the caller asked. One list rather than a scalar per mode, so
    // a single analysis can answer N questions (see `Question` above).
    std::vector<Question> questions;
    // `kama query <manifest> <file>`: the manifest widens the unit set from <file>'s import closure to
    // every file the manifest owns. Set from the operand below — this used to be `--project`.
    bool        jsonOut = false;           // --json: structured output for `query` and `check`
    const bool  runMode    = (subcommand == "run");   // `kama run`: build to a temp binary, exec it, forward exit
    std::vector<std::string> progArgs;     // args after `--`, forwarded to the run child (run-only)

    // Options may appear in any order, before or after the input file.
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { for (++i; i < argc; ++i) progArgs.push_back(argv[i]); break; }   // rest are program args
        else if ((a == "-o" || a == "--output") && i + 1 < argc) output = cliPath(argv[++i]);
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
        // Opt IN to a DLL runtime. A no-op — not an error — on a target with no non-system runtime to
        // choose about, so one cross-platform build script can carry it. Orthogonal to `--shared`, which
        // picks the OUTPUT kind rather than how the runtime is linked.
        else if (a == "--dynamic-runtime")        dynamicRuntime = true;
        // Which Windows subsystem the PE declares. A VALUED flag rather than a boolean `--gui`, so the CLI
        // and the `subsystem` manifest key speak one vocabulary. Validated here for the same reason the
        // manifest reader validates it: a typo must not quietly hand back the console default.
        else if (a == "--subsystem" && i + 1 < argc) {
            cliSubsystem = argv[++i];
            if (cliSubsystem != "console" && cliSubsystem != "windows") {
                fprintf(stderr, "kama: --subsystem must be \"console\" or \"windows\"\n");
                return 2;
            }
        }
        else if (a == "--no-heap")                g_noHeap = true;   // reject heap allocation program-wide (MCU step 5)
        else if (a == "--strict-numeric")         g_strictNumeric = true;   // M5a: measure, don't reject (hidden)
        else if (a == "--probe-templates")        g_probeTemplates = true;  // size the template probe (hidden)
        else if (a == "--release")              { release = true;  releaseExplicit = true; }
        else if (a == "--debug")                { release = false; releaseExplicit = true; }
        else if (a == "--select" && i + 1 < argc)   selects.push_back(argv[++i]);    // GROUP=VALUE
        else if (a == "--define" && i + 1 < argc)   defines.push_back(argv[++i]);    // `@compileFor` flag on
        else if (a == "--undefine" && i + 1 < argc) undefines.push_back(argv[++i]);  // `@compileFor` flag off
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
        else if (a == "--json")                     jsonOut = true;                  // structured output
        else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "kama: unknown option '%s'\n", a.c_str()); usage(); return 2;
        }
        else                                      inputs.push_back(cliPath(a));
    }

    // `-- <args>` are forwarded to the program `kama run` launches; they mean nothing to build/transpile.
    if (!runMode && !progArgs.empty()) {
        fprintf(stderr, "kama: `-- <args>` is only meaningful for `kama run`\n"); usage(); return 2;
    }

    // ---- classify the operands (§2g.32) -------------------------------------------------------------
    // Naming the file is what matters, not qualifying it: `kama build kama.json` and `kama build
    // ./libs/core/kama.json` are the same gesture, so this matches on the BASENAME and no `./` is ever
    // required. `inputs` is left holding exactly the loose `.kama` files.
    {
        std::vector<std::string> loose;
        for (const auto& a : inputs) {
            const std::string base = baseName(a);
            if (base != "kama.json" && base != kWorkspaceFile) { loose.push_back(a); continue; }
            if (!manifestOperand.empty()) {
                fprintf(stderr, "kama: two manifests named (%s and %s) — one names one compilation\n",
                        manifestOperand.c_str(), a.c_str());
                return 2;
            }
            manifestOperand = a;
            workspaceMode   = (base == kWorkspaceFile);
        }
        // ⚠️ `query` is the exception, and it is a different SHAPE of command rather than a carve-out
        // (§2g.35). A unit-set command's operands ARE the compilation, so a manifest and loose files are
        // two answers to one question. `query` is TARGET-ADDRESSED: the manifest sets the SCOPE to search
        // and the `.kama` file is what is being asked about, so the two compose — which is how a query
        // gains something `--project` could never express, a scope wider than the file's own project.
        if (!manifestOperand.empty() && !loose.empty() && subcommand != "query") {
            // Deliberately not a rule to remember: there is no argument list that means "these files, and
            // also that project", because a project's file set is the project's to state.
            fprintf(stderr, "kama: %s names a whole %s, so it cannot be combined with source files "
                            "(%s) — name one or the other\n",
                    manifestOperand.c_str(), workspaceMode ? "workspace" : "project", loose[0].c_str());
            return 2;
        }
        inputs.swap(loose);
    }

    // A unit-set command needs one form or the other, and says so rather than acting on the current
    // directory. `kama run` used to mean "read ./kama.json", which is the same implicit gesture the
    // operand rule removes everywhere else — so it goes too.
    if (manifestOperand.empty() && inputs.empty()) {
        fprintf(stderr, "kama %s: name what to %s — one or more .kama files, `kama.json` for a project, "
                        "or `kama_workspace.json` for every project in a workspace\n",
                subcommand.c_str(), subcommand.c_str());
        return 2;
    }
    if (!manifestOperand.empty() && !fileExists(manifestOperand)) {
        fprintf(stderr, "kama: %s does not exist\n", manifestOperand.c_str()); return 2;
    }
    // Install the driving project for the whole invocation — the deps view, the `out` root, the build
    // configuration and the toolchain all read it from here rather than each walking their own way.
    // Empty for a loose build and for a workspace (whose members each install their own, per re-exec).
    setCliProjectDir(workspaceMode || manifestOperand.empty() ? std::string()
                                                              : dirName(manifestOperand));
    // …and whether this is a LOOSE build, which is the same question asked of the other half of §2g: with
    // no manifest operand, no manifest participates in the compilation at all — including in what names
    // its modules (§2i). A workspace operand is not loose; it fans out and each member installs its own.
    setLooseBuild(manifestOperand.empty());

    // `--each` reinterprets the input list: N programs rather than one program's N files. Only `check` has
    // a meaning for that — `build --each` would need N outputs, which is a different (unbuilt) feature, and
    // silently building only the first input is the failure mode worth ruling out.
    if (eachMode && subcommand != "check") {
        fprintf(stderr, "kama: --each is only meaningful for `kama check` (it checks each input as its own "
                        "program)\n"); usage(); return 2;
    }

    // ---- a workspace operand fans out; it never means "one big program" (§2g.36) --------------------
    // Except for `query`, which is target-addressed rather than unit-set: there the workspace is a SCOPE
    // to search, answering one question about one file — and it is the scope `--project` could never
    // reach, since that could only ever widen to the file's own project. Handled further down.
    if (workspaceMode && subcommand != "query") {
        if (runMode || subcommand == "transpile") {
            // Running a workspace would mean supervising N processes — restart policy, log multiplexing,
            // shutdown order — which is a process supervisor, not a compiler. And running ONE member is
            // already spellable with no new concept, so the error names them rather than guessing.
            std::set<std::string> members;
            std::string werr;
            expandWorkspace(dirName(manifestOperand), members, werr);
            fprintf(stderr, "kama %s: %s is a workspace of %zu project(s) — name the one to %s:\n",
                    subcommand.c_str(), manifestOperand.c_str(), members.size(), subcommand.c_str());
            for (const auto& m : members)
                fprintf(stderr, "    kama %s %s/kama.json\n", subcommand.c_str(),
                        memberAsSpelled(manifestOperand, m).c_str());
            return 2;
        }
        return cmdWorkspaceFanOut(argv, argc, subcommand, manifestOperand);
    }


    // Resolve the build configuration: the manifest's declared flag universe + `select` groups, the
    // target triple and its derived flags, BUILD_TYPE, and the user `--define` set. All of it lives in
    // `resolveBuildConfig` rather than inline here, because `kama lsp` returns ~185 lines above this
    // point and so could never reach it — which is exactly why the server used to analyze with an empty
    // `@compileFor` set (M6 A1). DISCOVERY stays here: the CLI looks next to the input file then in CWD,
    // while the editor walks up from the open buffer bounded by its workspace folder.
    BuildConfigRequest bcReq;
    // The operand, and nothing else. A LOOSE build therefore inherits no project's configuration — not
    // its flag universe, not its target catalog, not its `out` root — which is what makes mode 1 mean
    // "just these files" (§2g.33) rather than "these files plus whatever manifest happens to be above
    // them". Discovery is gone from the CLI entirely; the editor still walks up, because an editor is
    // handed a buffer and a rootUri and never a command line.
    // A WORKSPACE operand names no project's configuration — it is not a project manifest and reading it
    // as one would fail on its very first key. Only `query` reaches here with one (build/check fanned out
    // above, and each member then resolves its OWN config), so this is the workspace-scope query: it
    // analyzes under permissive host defaults. That members may declare DIFFERENT configurations is a
    // known limit of any whole-workspace index, recorded at kama.lsp.cpp:436 for the editor's copy.
    bcReq.manifest = workspaceMode ? std::string() : manifestOperand;
    bcReq.target          = target;
    bcReq.targetExplicit  = targetExplicit;
    bcReq.selects         = selects;
    bcReq.defines         = defines;
    bcReq.undefines       = undefines;
    bcReq.release         = release;
    bcReq.releaseExplicit = releaseExplicit;
    bcReq.dev             = devBuild;
    // A dependency's malformed manifest is fatal for anything that produces an artifact, and merely
    // skipped for `kama query` — the same split `strictImports` draws, for the same reason: refusing to
    // answer a question about a file the user is looking at, over a manifest in some package, punishes
    // the wrong thing. (`kama lsp` never reaches here; lspResolveBuildConfig leaves both false.)
    bcReq.strictDeps      = subcommand != "query";

    BuildConfigResult bcfg;
    {
        std::string cerr;
        if (!resolveBuildConfig(bcReq, bcfg, cerr)) { fprintf(stderr, "kama: %s\n", cerr.c_str()); return 2; }
    }
    release = bcfg.release;
    // The manifest's `webgpu`, resolved through the same project -> target precedence as `link`.
    // `g_noHeap` was folded in inside resolveBuildConfig, because the NOHEAP `@compileFor` flag is
    // installed there and a value applied after that would be invisible to conditional compilation.
    if (g_target.webgpu) webgpu = true;

    // ---- a project operand names the project's OWN source files ------------------------------------
    // AFTER resolveBuildConfig, deliberately: that is where every manifest diagnostic lives — a missing
    // `kind`, a `source` that is "." or absent, a nested kama.json — and each of them says far more than
    // "this directory holds no .kama files" would. Validate the manifest, then use it.
    //
    // Not the entry's import closure, either: building ONE file of a project already pulls in every file
    // under its `source` root, because they are one package. So this is the same unit set the old
    // spelling produced, named by the manifest rather than by whichever file the caller happened to pick.
    // Not for `query`, whose manifest operand sets the SCOPE while the `.kama` operand stays the thing
    // being asked about; it widens its own unit set further down.
    // The project's own name, for the DEFAULT output path. Empty for a bare file operand.
    std::string manifestOutStem;
    if (!manifestOperand.empty() && subcommand != "query") {
        std::vector<std::string> srcs;
        packageSourceFiles(dirName(manifestOperand), srcs);   // false is unreachable: the operand IS the manifest
        if (srcs.empty()) {
            fprintf(stderr, "kama: %s: `source` is \"%s\", which holds no .kama files\n",
                    manifestOperand.c_str(), manifestSourceCached(manifestOperand).c_str());
            return 2;
        }
        inputs = srcs;
        // ⚠️ ...but the project's own NAME, not `inputs[0]`, is what the output is called. `srcs` is a
        // sorted glob, so the default output used to take the stem of the ALPHABETICALLY FIRST source
        // file: a project named `tests` with `entry: src/main.kama` built a binary called `engine_test`,
        // and adding a file that sorts earlier silently RENAMED the shipped executable. Libraries had it
        // too — a package `lib` with modules `a`/`b` produced `liba.a`.
        //
        // The manifest already answers this. `name` first (what the project calls itself, `@scope/`
        // stripped by importNameOf so it is filename-safe), then `entry`'s stem for a manifest with no
        // name, and only then the old behaviour — which still applies to a bare file operand, where
        // there is no manifest and the input IS the answer.
        manifestOutStem = manifestModulesCached(manifestOperand).projectName;
        if (manifestOutStem.empty()) {
            std::string entryRel, eerr;
            if (loadManifestEntry(manifestOperand, entryRel, eerr) && !entryRel.empty())
                manifestOutStem = stripExtension(baseName(entryRel));
        }
    }

    if (inputs.empty()) { fprintf(stderr, "kama: no input file\n"); usage(); return 2; }
    const std::string& input = inputs[0];   // first input drives COMPILATION order

    // The CLI wins over the manifest's `runtime`, exactly as --target wins over a `"default": true`.
    // Set after resolveBuildConfig because that is what assigns g_target.
    if (dynamicRuntime) g_target.runtime = "dynamic";
    if (!cliSubsystem.empty()) g_target.subsystem = cliSubsystem;

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

    // The project's stated kind, once, for the OUTPUT default and the entry checks below. Empty for a
    // loose build, which has no project and therefore no kind — the reason EXE stays the default there.
    std::string projectKind;
    if (!manifestOperand.empty()) { std::string kerr; loadManifestKind(manifestOperand, projectKind, kerr); }

    // Resolve the OUTPUT axis. `--shared` is sugar for `--select OUTPUT=SHARED`; the default depends on
    // the target, since a bare-metal build has no entry point to link and stops at an object — and now
    // also on the KIND, since a library has no `main` to link either. `kama build libs/core/kama.json`
    // producing an archive is what makes `kama build kama_workspace.json` mean something for a workspace
    // of libraries and one app: before this it built the app and then failed on the first library.
    std::string outputKind = g_activeFlags.count("SHARED")  ? "SHARED"
                           : g_activeFlags.count("STATIC")  ? "STATIC"
                           : g_activeFlags.count("OBJECT")  ? "OBJECT"
                           : g_activeFlags.count("EXE")     ? "EXE"
                           : embedded                       ? "OBJECT"
                           : projectKind == "library"       ? "STATIC"
                           : "EXE";
    if (shared) outputKind = "SHARED";
    // Asking for an EXECUTABLE of a library cannot work — it has no `main` — and until `kind` existed
    // nothing could say so: the build ran to completion and the LINKER reported `Undefined symbols: _main`,
    // naming a C symbol for a kama mistake. Only reachable now by asking for it explicitly, since the
    // default above already picked STATIC. `entry`'s consumer is the same sentence from the other side:
    // an executable project states which file holds its entry point, and that is checked before a
    // compile rather than discovered at the link.
    //
    // `check` and `transpile` produce no linked artifact, so the OUTPUT axis says nothing about them and a
    // library is a perfectly ordinary thing for either to be pointed at.
    if (!manifestOperand.empty() && outputKind == "EXE" && (subcommand == "build" || runMode)) {
        if (projectKind == "library") {
            fprintf(stderr, "kama %s: %s is a library, so it has no entry point to link — drop "
                            "`OUTPUT=EXE` and it builds as an archive, or name an executable project\n",
                    subcommand.c_str(), manifestOperand.c_str());
            return 2;
        }
        std::string entryRel, eerr;
        loadManifestEntry(manifestOperand, entryRel, eerr);
        if (entryRel.empty()) {
            fprintf(stderr, "kama %s: %s declares \"kind\": \"executable\" but no \"entry\" — add "
                            "\"entry\": \"src/app.kama\" naming the file that holds `main`\n",
                    subcommand.c_str(), manifestOperand.c_str());
            return 2;
        }
        const std::string entryAbs = joinPathLexical(dirName(manifestOperand), entryRel);
        if (!fileExists(entryAbs)) {
            fprintf(stderr, "kama %s: %s names \"entry\": \"%s\", but %s does not exist\n",
                    subcommand.c_str(), manifestOperand.c_str(), entryRel.c_str(), entryAbs.c_str());
            return 2;
        }
    }
    const bool outObject = (outputKind == "OBJECT");
    const bool outStatic = (outputKind == "STATIC");
    const bool outShared = (outputKind == "SHARED");
    g_outputShared = outShared;
    // Anything that is not a finished executable stops at `-c`; only EXE and SHARED reach the linker.
    const bool stopsAtObject = outObject || outStatic;

    // ---- where generated files go --------------------------------------------------------------------
    // A PROJECT (something with a kama.json) collects its build output under one root — `out` by default,
    // the manifest's `"out"` if it says otherwise — so a `.gitignore` needs one line instead of chasing
    // artifacts around the source tree. A loose `.kama` file with no manifest keeps landing beside itself:
    // `kama build hello.kama` -> `./hello` is the documented first experience, and one file is not a
    // project. Explicit `-o` wins over both.
    //
    // Scoped by TRIPLE and by BUILD TYPE, because both vary independently and a collision between them is
    // silent — you get yesterday's binary and no diagnostic. That is the stale-binary trap this repo
    // already learned the expensive way with out/<os>-<arch>/, and cargo splits on the same two axes.
    std::string projectOutDir;
    if (!bcReq.manifest.empty()) {
        std::string rel, oerr;
        loadManifestOutDir(bcReq.manifest, rel, oerr);       // malformed JSON already reported upstream
        if (rel.empty()) rel = "out";
        const std::string mdir = dirName(bcReq.manifest);
        projectOutDir = joinPathLexical(mdir.empty() ? "." : mdir, rel)
                      + "/" + g_target.triple() + "/" + (release ? "release" : "debug");
    }

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
            for (const auto& d : diags) renderDiagnostic(stderr, d);
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
        const bool queryProject = !manifestOperand.empty();
        if (queryProject) {
            // The operand says which scope to search, so there is nothing to discover: a `kama.json`
            // widens to that project's files, and a `kama_workspace.json` to every member's — the scope
            // `--project` could never reach, since it could only ever widen to the file's OWN project.
            if (workspaceMode) {
                std::set<std::string> members;
                std::string werr;
                if (!expandWorkspace(dirName(manifestOperand), members, werr)) {
                    fprintf(stderr, "kama query: %s\n", werr.c_str()); return 1;
                }
                queryInputs.clear();
                for (const auto& m : members) collectPackageTree(m, queryInputs);
            } else {
                queryInputs.clear();
                packageSourceFiles(dirName(manifestOperand), queryInputs);
            }
            if (queryInputs.empty()) {
                fprintf(stderr, "kama query: %s owns no .kama files\n", manifestOperand.c_str());
                return 1;
            }
            for (auto& f : queryInputs) f = absolutePath(f);
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
            std::ifstream in(osp(input), std::ios::binary);
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
                    for (const auto& d : ds) renderDiagnostic(stdout, d);
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
                        // Modules first, then symbols — the same answer the language server gives, and
                        // for the same reason: inside an import block `std::|` may be growing into a
                        // deeper module or already naming the one whose symbols are wanted, and the text
                        // cannot say which. Keep the two emitters in step; this one is what check-query
                        // asserts against and the LSP one is what an editor sees.
                        for (const auto& m : lspImportModules(input, cc.receiver, argv[0]))
                            rows.push_back({"module", m, ""});
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
        // Same rule as `build`: inside a project the default lands under projectOutDir, outside one it
        // lands beside the input. `kama transpile` exists to hand a .c to somebody else's toolchain, so the
        // path matters to a human — but that is an argument for putting it somewhere findable and
        // gitignored, not for scattering it through the sources.
        const std::string tStem = projectOutDir.empty()
                                ? stripExtension(input)
                                : projectOutDir + "/" + baseName(stripExtension(input));
        std::string outPath = output.empty() ? (tStem + ".c") : output;
        const std::string tDir = dirName(outPath);
        if (!dirExists(tDir) && !makeDirs(tDir)) {
            fprintf(stderr, "kama: cannot create output directory %s\n", tDir.c_str()); return 1;
        }
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
            output = tempDir() + "/kama-run-" + std::to_string((long)getpid());
            // ...under the name the LINKER will actually produce. clang/gcc append `.exe` when the `-o`
            // name carries no extension, so the `remove()` at the end of runMode was unlinking a path
            // that never existed and every `kama run` leaked its binary into the temp directory.
#ifdef _WIN32
            output += ".exe";
#endif
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
                return runCmd("zig version >" KAMA_DEVNULL " 2>&1") == 0;
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
        // wasm -> an HTML harness (emcc also emits the .js + .wasm alongside it). Inside a project the
        // stem moves to projectOutDir (see above); outside one it stays beside the input, as it always has.
        const char* sharedExt = g_target.sharedLibExt();
        // `manifestOutStem` is the project's own name when one was given; it falls back to the input's
        // stem for a bare file operand, which is where that behaviour is actually right.
        const std::string outBase = manifestOutStem.empty() ? baseName(stripExtension(input))
                                                            : manifestOutStem;
        const std::string stem = projectOutDir.empty()
                               ? (manifestOutStem.empty() ? stripExtension(input)
                                                          : dirName(stripExtension(input)) + "/" + outBase)
                               : projectOutDir + "/" + outBase;
        std::string defaultOut = wasm      ? (stem + ".html")
                               : outStatic ? (dirName(stem) + "/lib" + baseName(stem) + ".a")
                               : outObject ? (stem + ".o")
                               : outShared ? (stem + sharedExt)
                                           : stem;
        std::string outPath    = output.empty() ? defaultOut : output;

        // Transpile to one or more .c (multi-file emits a shared header too).
        // Generated files land in the output directory; cleaned unless --keep-c.
        std::string genDir = dirName(outPath);
        // The output directory may not exist yet — `out/<triple>/<type>/` never does on a first build, and
        // `-o build/app` is a shape people already write. Create it before anything tries to open a file
        // inside it, or the failure surfaces as an unexplained transpile error.
        if (!dirExists(genDir) && !makeDirs(genDir)) {
            fprintf(stderr, "kama: cannot create output directory %s\n", genDir.c_str()); return 1;
        }
        std::vector<std::string> cFiles;     // .c to compile
        std::vector<std::string> genFiles;   // generated files to remove afterwards
        // RAII, not a call at the bottom, because the bottom is only reached on the SUCCESS path.
        // Every `return` between here and there — a failed transpile, a missing SDK, a C compiler
        // that rejected the output — used to leave the generated .c behind, next to the user's
        // source for a manifest-less build. That is how 271 stale .c files (~72 KB each) came to sit
        // in tests/xfail/: the fixtures there are *meant* to fail, so they took the leaking path
        // every time. A destructor cannot be forgotten by the next early return added here.
        struct GenCleanup {
            const std::vector<std::string>& files;
            const bool& keep;
            ~GenCleanup() { if (!keep) for (auto& f : files) remove(osp(f).c_str()); }
        } genCleanup{genFiles, keepC};
        std::string headerDir;

        // Parse the CLI inputs and transitively pull in every imported module. A single
        // file with no imports stays on the fast path (one .c, no shared header); anything
        // that drags in more units (multiple inputs, or `import`s) uses the multi-file path.
        std::vector<SharedCompilationUnit> units;
        std::vector<std::string> unitPaths;
        if (!loadProgramUnits(inputs, argv[0], units, unitPaths, devBuild)) return 1;

        // Emit in a CANONICAL order, not the order the units happened to load in. `loadProgramUnits` walks
        // the import closure breadth-first from the operands, so permuting the argument list permutes the
        // vector — and the shared header declares each unit in that order, which put the same three
        // declarations in two different sequences across two builds of one program. Naming the generated
        // files by module (§2e.26) fixed WHAT is emitted; this is what fixes the order it is emitted in,
        // and both are needed before `--keep-c` output can be diffed at all.
        //
        // Sorted by the same stem the `.c` filenames use, so the file list and the header agree. Ordering
        // is free to be anything stable: the emitter pre-registers every type's mangled name before
        // resolution precisely so nothing depends on file order. It does have one visible consequence —
        // a diagnostic that names "the first declaration" of a duplicate now picks the same one every
        // time, where before it picked whichever unit loaded first.
        //
        // Before the branch below, not inside it, because the unity release path folds every unit into ONE
        // translation unit and so is order-dependent in exactly the same way.
        if (units.size() > 1) {
            std::vector<size_t> order(units.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::vector<std::string> stems(units.size());
            for (size_t i = 0; i < units.size(); ++i) stems[i] = cStemForUnit(unitPaths[i]);
            std::stable_sort(order.begin(), order.end(),
                             [&](size_t a, size_t b) { return stems[a] < stems[b]; });
            std::vector<SharedCompilationUnit> su; su.reserve(units.size());
            std::vector<std::string> sp; sp.reserve(unitPaths.size());
            for (size_t i : order) { su.push_back(units[i]); sp.push_back(unitPaths[i]); }
            units.swap(su); unitPaths.swap(sp);
        }

        bool needsLibm = false;   // set if the program `extern "<math.h>";`'s (std::math / libm) -> link -lm
        bool needsNetWeb = false; // set if the program `extern "kama_net_web.h";`'s (std::net::web) -> --js-library
        bool needsApp = false;    // set if the program `extern "kama_app.h";`'s (std::app) -> wasm -sEXIT_RUNTIME=1
        bool needsGpu = false;    // set if the program `extern "kama_gpu.h";`'s (WebGPU seam) -> native surface libs
        bool needsPthread = false;// set if the program uses a std::concurrent seam (`kama_isolate.h` / `kama_channel.h`) -> native -lpthread
        if (units.size() == 1) {
            // genDir, not the input's directory. The multi-file paths below already did this; this one
            // did not, which is why a bare `kama build x.kama` used to drop an `x.c` beside the source
            // (and leave it there whenever the build died before the --keep-c cleanup).
            std::string cPath = genDir + "/" + baseName(stripExtension(input)) + ".c";
            // Registered BEFORE the call that writes it. A failed transpile has already emitted a
            // (partial) .c by the time it reports the error, so registering afterwards — as this
            // did — leaves exactly the file the failure path was supposed to clean up.
            genFiles.push_back(cPath);
            if (transpileUnitToFile(units[0], unitPaths[0], cPath, emitLines, &needsLibm, &needsNetWeb, &needsApp, &needsGpu, &needsPthread) != 0) return 1;
            cFiles.push_back(cPath);
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
            genFiles.push_back(cPath);       // before the call that writes it — see the single-unit note
            if (transpileProgramToSingleFile(units, unitPaths, cPath, emitLines, &needsLibm, &needsNetWeb, &needsApp, &needsGpu, &needsPthread) != 0) return 1;
            cFiles.push_back(cPath);
        } else {
            std::string headerName = baseName(stripExtension(outPath)) + ".gen.h";
            std::string headerPath = genDir + "/" + headerName;
            headerDir = genDir;
            // One .c per unit, named from the unit's MODULE rather than its position in the argument list
            // (§2e.26) — `lib/std/collections/vec.kama` -> `std__collections__vec.c`. A file with no
            // module (the loose root, §2e.27) keeps its bare basename, and two of those sharing one is the
            // single case the old `_<index>` suffix was absorbing, so it is reported rather than hidden.
            std::vector<std::string> cPaths;
            std::map<std::string, std::string> stemOwner;      // stem -> the unit that claimed it
            for (size_t i = 0; i < units.size(); ++i) {
                const std::string stem = cStemForUnit(unitPaths[i]);
                auto claimed = stemOwner.emplace(stem, unitPaths[i]);
                if (!claimed.second) {
                    fprintf(stderr,
                        "kama: '%s' and '%s' would both generate '%s.c'.\n"
                        "      A generated file is named from its unit's module and file name, so two\n"
                        "      units cannot share both. Rename one, or move it into another module.\n",
                        claimed.first->second.c_str(), unitPaths[i].c_str(), stem.c_str());
                    return 1;
                }
                cPaths.push_back(genDir + "/" + stem + ".c");
            }
            genFiles = cPaths;               // before the call that writes them — see the single-unit note
            genFiles.push_back(headerPath);
            if (emitProgramUnits(units, unitPaths, headerPath, headerName, cPaths, emitLines, &needsLibm, &needsNetWeb, &needsApp, &needsGpu, &needsPthread) != 0) return 1;
            cFiles   = cPaths;
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
        //
        // The third is the C-level BACKSTOP for `UnsafeConstPtr<T>` (`T const*`): the emitter refuses a
        // const→mutable pointer conversion at every sink it knows (kama.cemit.cpp rejectConstPtrWiden),
        // and this catches whatever shape it does not — clang's `-discards-qualifiers` is a sub-flag of
        // this group. Spelled the way BOTH compilers accept: gcc hard-errors on an unknown `-Werror=`
        // option, and a cross toolchain here can be gcc. On gcc the qualifier case is a separate
        // `-Wdiscarded-qualifiers` this does not promote, so there the kama-side check carries it alone.
        cmd << compiler << crossFlags << " -std=c11 -Werror=return-type -Werror=uninitialized"
                                         " -Werror=incompatible-pointer-types ";
        // `reproducible-float`: forbid the C compiler contracting `a*b + c` into a single fused
        // multiply-add. clang's default is `on`, which contracts within one expression — so the same
        // source gives different bits on a target WITH an FMA (arm64) than on one without (wasm32 MVP,
        // baseline x86-64 SSE2), and a program whose correctness IS bit-reproducibility diverges browser
        // from native. Measured on this repo's own fixture: 13 of 64 random triples differ on
        // aarch64-macos in a release build, 0 with this flag.
        //
        // ⚠️ Emitted here, in the fixed base, so a target's raw `cflags` can still override it with
        // `-ffp-contract=fast` — raw flags are the escape hatch everywhere else and this is no different.
        // Sent on EVERY target including wasm: it is a no-op where there is no FMA, and a command line
        // that forked per target would need its own guard to say why.
        //
        // ⚠️ It reaches `kama transpile` output not at all — the flag lives on the command line, not in
        // the C. A `#pragma STDC FP_CONTRACT` would travel, but GCC does not implement it.
        if (g_target.reproFloat) cmd << "-ffp-contract=off ";
        // `-fno-math-errno`: kama never reads `errno` after a libm call (a domain error is a NaN, and
        // `std::io::lastError` is for OS calls), so the side effect only costs — with it live, a per-lane
        // `sqrtf` loop over a `Simd<float32>#(4)` cannot fold to `fsqrt.4s` and a scalar `sqrt` cannot
        // inline to the instruction, on gcc and clang alike (measured, ROADMAP_DETAIL §2). macOS defaults
        // to this and Linux does not, which is how one host read "vectorizes" and another "does not".
        // Results are bit-identical either way. Same placement rule as the flag above: fixed base, so a
        // raw `cflags` can put `-fmath-errno` back.
        cmd << "-fno-math-errno ";
        // The target's own toolchain settings from kama.json (a sysroot and any extra compile flags).
        if (!g_target.sysroot.empty()) cmd << "--sysroot=\"" << g_target.sysroot << "\" ";
        // ⚠️ The span the user's `cflags` occupy is RECORDED, because the per-TU path reuses this whole
        // prefix for the LINK — deliberately, so a `--cc "clang -fsanitize=…"` reaches both — and a
        // compile flag on a link line is not merely redundant, it can be fatal. `-x <lang>` is a sticky
        // clang MODE flag: on the link command it applies to the `.o` inputs, so clang lexes Mach-O bytes
        // as source and emits a wall of `null character ignored` before `too many errors emitted`.
        // Reported by the first engine consumer (KB-16) against a dependency whose `cflags` carried
        // `-x objective-c`: its own build was fine and every CONSUMER's link died, naming an object file
        // and nothing else. `ldflags` is the tier that reaches a link.
        //
        // Recorded as a SPAN rather than held back and appended later, because position is load-bearing
        // here: a `-I` in `cflags` sits ahead of kama's own include paths and is expected to shadow them,
        // and the `-ffp-contract=off` above is emitted before this point precisely so a raw flag can
        // override it. Appending at the end would silently reverse both. The compile command is therefore
        // byte-for-byte what it always was; only the link-only command has this span cut out.
        const size_t cflagsPos = (size_t)cmd.tellp();
        for (const auto& f : g_target.cflags) cmd << f << " ";
        const size_t cflagsEnd = (size_t)cmd.tellp();
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
        // wasm SIMD (v128) is OPT-IN in emscripten, and without this flag NOTHING on the wasm target
        // vectorizes — not the auto-vectorized std::math path, not an explicit vector type. Measured
        // 2026-08-30: a `--release --target wasm` build held ZERO v128 instructions, and the SAME
        // generated C through `emcc -msimd128` held 18. That is not a language gap; kama had simply
        // never passed the flag, while SPEC.md claimed the ops vectorized "to SSE/NEON/wasm128".
        //
        // BOTH TIERS on purpose. Debug and release stay on one instruction set, so there is no
        // "vectorizes only in release" class of bug report — and -Oz still vectorizes (measured: 14
        // v128 ops at -Oz vs 38 at -O3), so the size tier loses nothing by it.
        //
        // No baseline to gate on: wasm SIMD is in every current browser and in node >= 16 (this repo's
        // container runs node 22). See docs/targets.md, *Platform notes*.
        // Guarded by tools/check-simd-wasm.sh, which greps a real .wasm — and which runs ONLY on the
        // container wasm leg, never in `./dev check`.
        if (wasm) cmd << "-msimd128 ";
        std::string linkGc;                  // the section-GC LINK flags — appended to the link line only
        if (release) {
            // Optimized, no debug info, asserts off. Native uses -O3 (max speed — matches Rust's release
            // default); wasm uses -Oz (size — download cost dominates). -ffunction/data-sections +
            // --gc-sections let the linker drop unused (std)library code — the
            // "pay for what you use" pruning lever. Native also strips symbols.
            cmd << (wasm ? "-Oz " : "-O3 ") << "-DNDEBUG -ffunction-sections -fdata-sections ";
            if (!wasm && !stopsAtObject) {   // -Wl,* is link-time; OBJECT/STATIC stop at -c (see below)
                // ld64 spells section GC differently from GNU ld/lld, and treats `-s` as obsolete (it
                // warns on every release link), so the strip flag is for the GNU-style linkers only.
                //
                // Into `linkGc`, NOT `cmd`: `cmd` is the prefix of every per-TU `-c` job too, and clang
                // says "-Wl,-dead_strip: 'linker' input unused" once per compile-only job — 121 lines
                // for a package vendoring libsodium (ROADMAP row 3). The link line appends it below.
                if (g_target.isMacOS()) linkGc = "-Wl,-dead_strip ";
                else                    linkGc = "-Wl,--gc-sections -s ";
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
        // out-of-range float->int are faults in EVERY build (they're always bugs) — and they are kama's
        // OWN checks in the emitted C (KAMA_DIV/KAMA_MOD/KAMA_SHL/KAMA_SHR, kama_f2i_chk in
        // kama_runtime.h), not sanitizer flags. Until 0.9.160 they were
        // `-fsanitize=integer-divide-by-zero,shift-exponent,float-cast-overflow` with `-fsanitize-trap`:
        // the same compare-and-branch the helpers emit, but lowered to a bare `__builtin_trap` that printed
        // nothing, ran no panic hook, and could not be recovered from inside an `@onPanic` region. The
        // helpers cost the branch the sanitizer already cost (release parity is measured by
        // tools/check-release-arith.sh); and with no `-fsanitize` in the release line at all, emscripten
        // no longer refuses `-sWASM_WORKERS` (an AudioWorklet needs it — ROADMAP §2).
        // Signed overflow: debug TRAPS it, release `-fwrapv`-WRAPS `+`/`-`/`*` (defined, zero-cost).
        //
        // ⚠️ The sanitizer is DEBUG-ONLY, and it used to be passed in both tiers. The reasoning for that
        // was sound and the outcome was not: `-fwrapv` does not define `INT_MIN / -1`, so the sanitizer
        // was kept in release to catch that one case — on the assumption that `-fwrapv` would suppress it
        // for the ordinary ops. **Measured 2026-08-31: that assumption holds on Ubuntu clang 18.1.3 and
        // gcc 13.3, and NOT on Apple clang 21.** So a macOS release build traps where a Linux one wraps,
        // from one source and one set of flags — and pays `adds; b.vs; brk` on every signed add and
        // `smull; cmp; b.ne; brk` on every multiply, against a bare `add`/`mul` on Linux. That is a
        // compare and a branch on arithmetic this project's headline invariant calls "at C parity".
        //
        // So the release tier drops it, `-fwrapv` alone defines `+ - *` on every toolchain, and the one
        // case it was buying is checked explicitly instead — `kama_sdiv_i32`/`_i64` in kama_runtime.h,
        // emitted for a signed division at every width, in every build. One predictable branch per
        // division (already a 20-40 cycle instruction) buys back the whole release tier.
        //
        // ⚠️ And since 0.9.161 the DEBUG trap is kama's own as well (KAMA_ADD/SUB/MUL/NEG and the place
        // operators in kama_runtime.h, `__builtin_*_overflow` under !NDEBUG), so no `-fsanitize` flag
        // remains in either tier — a debug overflow prints which operation overflowed, runs the panic
        // hook and is recoverable inside an `@onPanic` region, and emscripten no longer refuses
        // `-sWASM_WORKERS` for a debug wasm build either. `-fwrapv` now goes to BOTH tiers: it costs
        // nothing, and it is what makes the runtime header's own C arithmetic defined once the sanitizer
        // is gone.
        cmd << "-fwrapv ";
        // --target embedded: a freestanding, hosted-runtime-free compile that stops at an OBJECT. No libc
        // (`-nostdlib`), no OS/hosting assumptions (`-ffreestanding`), and `-c` so no link is attempted —
        // the crt0/startup + linker script are the user's per-chip link step. `-DKAMA_TARGET_EMBEDDED`
        // selects the freestanding `main`/panic forms in the emitted C + runtime.
        //
        // The board's TRIPLE is now the TARGET axis (`--target thumbv7em-none-eabihf`), and its CPU model
        // goes in that target's `cflags` (`"cflags": ["-mcpu=cortex-m4"]`) — not through `--cc`, which is
        // what this said before the build-configuration campaign. kama itself passes NO -march/-mcpu/
        // -mtune anywhere, so every build targets the architecture's generic baseline unless a target
        // spec says otherwise. A first-class CPU-tuning knob is a recorded follow-on (ROADMAP_DETAIL §10).
        if (embedded)      cmd << "-ffreestanding -nostdlib -DKAMA_TARGET_EMBEDDED ";   // no OS: os=none
        if (stopsAtObject) cmd << "-c ";                                                // no link step
        // ⚠️ QUOTED, like every other -I below. These three were bare until 0.9.302, so a space
        // anywhere in them tore the command line and the tail became a bare input operand —
        // `clang: error: no such file or directory: 'project'` for a project under `…/my project/`.
        // Worse, it also hit runtimeDir, which is the INSTALL PREFIX: with kama installed to
        // `~/.kama` (the documented path) a Windows account named `John Smith` got
        // `no such file or directory: 'Smith/.kama/include'` and could not build hello-world at all.
        // Not Windows-only — `sh -c` word-splits identically. Measured: ROADMAP_DETAIL §2.
        cmd << "-I\"" << runtimeDir << "\" -I\"" << dirName(absolutePath(input)) << "\" -I. ";
        if (!headerDir.empty()) cmd << "-I\"" << headerDir << "\" ";   // the shared generated header
        // Each `csources` entry's own directory, so a header BESIDE the .c is findable — from the .c
        // itself, and from the kama file that `extern "shim.h";`s it. Deduped, and AFTER the project's
        // own dirs above so a first-party header still shadows a dependency's.
        {
            std::set<std::string> seenDirs;
            for (const CSourceRef& cs : g_csources) {
                std::string d = dirName(cs.path);
                if (!d.empty() && seenDirs.insert(d).second) cmd << "-I\"" << d << "\" ";
            }
            // `cincludes` — a package's own include tree, resolved against its manifest. After the
            // csources dirs, so the shadowing order above still holds. A directory that is not there is
            // named by kama (with the package that declared it) rather than silently skipped by the C
            // compiler, which is how a typo'd `include` would otherwise surface: as a missing header,
            // three steps later, in a file the consumer does not own.
            for (const CSourceRef& inc : g_cincludes) {
                if (!dirExists(inc.path)) {
                    fprintf(stderr,
                        "kama: `cincludes` names '%s', which is not a directory%s%s.\n",
                        inc.path.c_str(),
                        inc.owner.empty() ? "" : " (declared by dependency `",
                        inc.owner.empty() ? "" : (inc.owner + "`)").c_str());
                    return 2;
                }
                if (seenDirs.insert(inc.path).second) cmd << "-I\"" << inc.path << "\" ";
            }
        }
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
        // ⚠️ The COMPILE half only. `--js-library` and `-sEXPORTED_RUNTIME_METHODS` used to be emitted
        // here too; they are emscripten LINK settings and now live in the merged block in the link tail,
        // where a project can add to them instead of silently losing to them.
        if (needsNetWeb)
            cmd << "-I\"" << (resolveStdlibDir(argv[0]) + "/std/net/web") << "\" ";   // kama_net_web.h
        // EVERY wasm build, not just the two that used to need it. A kama program is a batch program:
        // `main` returns an exit code and the process is done. EXIT_RUNTIME is what makes emscripten act
        // on that — call `exit(status)` and shut the runtime down — instead of returning from `main` and
        // leaving the runtime alive for node to wind down on its own.
        //
        // It was already required for two cases and both still hold: std::app's run loop keeps the
        // runtime alive (emscripten_set_main_loop) and its quit() (emscripten_force_exit) needs this to
        // shut down with a real exit code; and under -sPROXY_TO_PTHREAD `main` runs on a worker, where
        // this is what carries its return value out as the process exit code (else node sees 0 regardless).
        //
        // The third reason, and the one that made this unconditional — `tests/fs_raii.kama` hung the
        // wasm leg four times (2026-08-13/15/17/23) and it was NOT a kama bug. Without EXIT_RUNTIME the
        // program returns from `main` and node proceeds through its full graceful teardown, which calls
        // node::NodePlatform::DrainTasks — and that DEADLOCKS against V8's own background threads:
        //
        //     main thread   DrainTasks            -> waiting for background tasks to finish
        //     background    AwaitCollectionBackground -> waiting for the main thread to run a GC
        //
        // A closed cycle, with no kama frame in it. Measured rather than reasoned (2026-08-23): the
        // hang reproduces at ~11-13% under 16-way parallel load on a 6-CPU container, every instance
        // parked in exactly that pair. `--no-concurrent-recompilation` takes it to 0/200 while
        // `--no-concurrent-marking` changes nothing, which identifies the background thread as a
        // concurrent TurboFan compile job — those hold a LocalHeap, allocate on it, and can request the
        // GC that the cycle turns on. fs_raii is the fixture most exposed because its 5,000-iteration
        // loop is exactly what triggers optimization, right before it exits.
        //
        // With EXIT_RUNTIME the runtime is torn down as soon as `main` returns: 0/300 against 40/300 for
        // the same program built without it, same load, same session.
        //
        // ⚠️ CORRECTED 2026-08-25. This paragraph used to say "the main thread calls process.exit() and
        // never enters that teardown path at all", and that is FALSE — read emscripten's emitted node
        // `quit_` rather than reasoning about what EXIT_RUNTIME ought to mean:
        //
        //     quit_ = (status, toThrow) => { process.exitCode = status; throw toThrow; };
        //
        // It sets the exit CODE and throws. It never calls `process.exit()`, so node unwinds and performs
        // its full graceful teardown, DrainTasks included. EXIT_RUNTIME shortens the window in which a
        // concurrent compile job is still in flight at that moment; it does not remove the path. Which is
        // why the hang recurred at `0.9.73` and again during a `0.9.82` matrix run, where a live stack
        // finally caught it parked in exactly the pair above — the SAME cycle, not a second one.
        //
        // The remaining exposure is real but not kama's to fix, and it is NOT wasm-specific: it is node's
        // shutdown racing V8's, and any emscripten program can reach it. `run_tests.sh` runs node with
        // `--no-concurrent-recompilation`, which is the lever the bisect actually identified.
        //
        // ⚠️ Do NOT try to close it here by forcing a hard exit (a `--pre-js` setting `Module.onExit` to
        // call `process.exit`). Measured: `process.exit()` drops pending stdout, and against a slow reader
        // 300,000 piped lines arrived as 309. That trades a rare teardown deadlock for silent output
        // corruption in every user's pipeline, which is the worse bug by a wide margin.
        //
        // Checked for fallout on the wasm SURFACE, since that is what this could cost: the module's
        // exports are a strict SUPERSET afterwards (`__funcs_on_exit` and `strerror` arrive with the
        // atexit machinery, nothing leaves), an `expose`d function is still a real wasm export, and an
        // embedder that instantiates the .wasm directly never runs `main` so none of this reaches it.
        //
        // ⚠️ It DOES constrain one thing that does not exist yet. When kama grows a module/embedding
        // artifact — the richer wasm exports and scripting-host interface of ROADMAP §7 — a module must
        // keep its runtime alive after `main`, so this has to become conditional again. Key it on the
        // artifact KIND (program vs module), which is the honest axis, and not on which library the
        // program happens to use, which is what it was keyed on before and why it was wrong here.
        // (`-sEXIT_RUNTIME=1` is emitted with the other emscripten settings in the link tail — it is a
        // LINK setting, and gathering them in one place is what lets a project merge with them.)
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
        // The project's own C, and its dependencies' — `csources`. Each is its own translation unit,
        // exactly like the seam above, and for the same reason its object goes in genDir: `dirname(-o)`
        // is the one directory a build may write to, and a user's source tree is not it. The OWNER
        // prefix is what keeps two packages that both ship `shim.c` from writing the same object — a
        // silent overwrite in a single invocation, and a RACE under `-j`.
        {
            std::map<std::string, std::string> objClaimed;   // object path -> the source that claimed it
            for (const CSourceRef& cs : g_csources) {
                if (!fileExists(cs.path)) {
                    fprintf(stderr,
                        "kama: `csources` names '%s', which does not exist%s%s.\n",
                        cs.path.c_str(),
                        cs.owner.empty() ? "" : " (declared by dependency `",
                        cs.owner.empty() ? "" : (cs.owner + "`)").c_str());
                    return 2;
                }
                std::string obj = genDir + "/csrc__" + (cs.owner.empty() ? std::string("self") : cs.owner)
                                + "__" + stripExtension(baseName(cs.path)) + ".o";
                auto claimed = objClaimed.emplace(obj, cs.path);
                if (!claimed.second) {
                    fprintf(stderr,
                        "kama: '%s' and '%s' would both compile to '%s'.\n"
                        "      Two `csources` entries in one package cannot share a file name.\n",
                        claimed.first->second.c_str(), cs.path.c_str(), obj.c_str());
                    return 2;
                }
                // `-std=gnu11`, overriding the `-std=c11` the base prefix set: kama's OWN C is written to
                // ISO C11, but a `csources` entry is somebody else's C, and the C the world writes is GNU
                // C — every compiler defaults to it, and a vendored library reaches for its extensions.
                // Measured: libsodium's `randombytes.c` uses emscripten's `EM_ASM`, which refuses to
                // compile under `-std=c*` ("use -std=gnu* modes instead"), so the first package's wasm
                // leg failed on the one file that reads the browser's entropy. Placed on the input, not
                // in the prefix, so it reaches exactly the foreign translation units.
                ccInputs.push_back({ "-std=gnu11 \"" + cs.path + "\" ", obj });
            }
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
        // The manifest's `link` first, then `--link` from the command line — same form, and a one-off on
        // the CLI should be able to come after what the project always needs.
        if (!stopsAtObject) {
            for (const auto& lib : g_target.link) link << "-l" << lib << " ";   // kama.json `link`
            for (const auto& lib : links)         link << "-l" << lib << " ";   // --link (FFI)
        }
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
                link << "-pthread ";   // the three `-s` settings go in the merged emscripten block below
            } else if (g_target.isWindows() && g_target.runtime != "dynamic") {
                // mingw-w64 installs BOTH libpthread.a and libpthread.dll.a, and the linker prefers the
                // IMPORT LIBRARY — so a bare `-lpthread` binds libwinpthread-1.dll out of the msys2 tree
                // and the program dies at process start with STATUS_DLL_NOT_FOUND (0xC0000135) anywhere
                // that DLL is absent. That is *every* machine including the one that built it, unless the
                // launcher happens to be an msys2 shell. Measured, not reasoned: tests/isolate_basic.kama
                // built here imported libwinpthread-1.dll and exited 0xC0000135 from PowerShell.
                //
                // `-Bstatic` around this ONE library, not a blanket `-static`, because the rule is "link
                // non-system runtime statically, system components dynamically" — and the libraries a user
                // names (`--link`, `--webgpu`) are their choice, not ours. Go and Rust draw the same line.
                // A blanket -static would also silently re-bind -lglfw3 and break --webgpu outright:
                // libwgpu_native.a needs -lntdll/-luserenv/-lbcrypt, which this tail never emits.
                //
                // -Bdynamic restores the linker default so nothing downstream (-lws2_32, the target's own
                // ldflags) changes meaning. Opt back into the DLL with --dynamic-runtime or a target's
                // "runtime": "dynamic" — see docs/targets.md.
                link << "-Wl,-Bstatic -lpthread -Wl,-Bdynamic ";
            } else {
                link << "-lpthread ";
            }
            // (KAMA_PARFOR_WORKERS lived here — a build-time pin for parallel_for's worker count, back
            // when that count had no spelling in the source. `workers:` is mandatory now, so the override
            // had nothing left to override: a knob that silently loses to every call site is worse than
            // no knob. Pin a build by writing the number, or read it from a const.)
        }
        // std::net uses Winsock (kama_os.h). Link ws2_32 when the TARGET is Windows; harmless (and pruned
        // by --gc-sections) for programs that don't open a socket. POSIX sockets need no extra lib.
        // Keying this on the host was the sharpest example of the cross-compilation blocker: a Windows
        // build produced on Linux silently omitted the socket library.
        if (!wasm && !stopsAtObject && g_target.isWindows()) link << "-lws2_32 ";
        // std::random's entropy seam (kama_random.h) is BCryptGenRandom on Windows, which lives in
        // bcrypt.dll — same rule, same pruning: keyed on the TARGET, dropped by --gc-sections when unused.
        if (!wasm && !stopsAtObject && g_target.isWindows()) link << "-lbcrypt ";
        // args() on Windows re-reads the command line wide through shell32's CommandLineToArgvW
        // (kama_args_init, kama_runtime.h). mingw-w64 links shell32 by default; zig's cross line is its
        // own, so say it — same rule, same pruning.
        if (!wasm && !stopsAtObject && g_target.isWindows()) link << "-lshell32 ";
        // The PE SUBSYSTEM. Console is the default and stays byte-for-byte what it always was, so every
        // console tool, the CI legs and `kama` itself are untouched; a GUI program opts IN and stops
        // getting the stray console window Windows opens for a console-subsystem PE.
        //
        // Keyed on the TARGET, not on the host and not on `needsPthread` — a GUI program need not spawn.
        // Everywhere else this is an accepted no-op (no other object format HAS a subsystem field), which
        // is the same stance `--dynamic-runtime` takes so one build script can carry the flag.
        //
        // TWO flags, and they must go to different phases. The linker gets --subsystem; the C compiler
        // gets a -D so kama_args_init() knows to reattach a console (see kama_runtime.h). The -D belongs
        // in `cmd` and NOT in the link tail: a per-TU `-c` job takes only the compile flags, so a
        // link-tail -D is silently lost in any multi-TU build.
        if (!wasm && !stopsAtObject && g_target.isWindows() && g_target.subsystem == "windows") {
            link << "-Wl,--subsystem,windows ";
            cmd  << "-DKAMA_SUBSYSTEM_WINDOWS=1 ";
        }
        // ---- The emscripten settings, in ONE place. Every `-s<KEY>=<value>` is an emcc LINK setting;
        // they used to be scattered through the COMPILE flags above only because a wasm build is never
        // split into per-TU compiles, so nobody noticed. Gathering them is what makes the merge below
        // possible at all: emcc is LAST-WINS on a repeated `-s`, so kama's own
        // `-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8` — emitted after the project's `cflags` —
        // silently beat any project that set the same key, which is the defect this closes.
        //
        // kama's own settings go in first and declare their own kind (list or scalar). The manifest's
        // merge over them: a LIST unions, a SCALAR is last-wins with the project winning. So a project
        // adds `ccall` to the exported methods rather than replacing the two the stdlib's glue needs.
        if (wasm && !stopsAtObject) {
            std::vector<EmSetting> em;
            auto setScalar = [&](const std::string& k, const std::string& v) {
                for (auto& e : em) if (e.key == k) { e.value.scalar = v; return; }
                EmValue val; val.scalar = v; em.push_back({ k, "", val });
            };
            auto addList = [&](const std::string& k, const std::string& v) {
                for (auto& e : em) if (e.key == k) { e.value.list.push_back(v); return; }
                EmValue val; val.isList = true; val.list.push_back(v); em.push_back({ k, "", val });
            };
            // A kama program is a batch program: `main` returns an exit code and the process is done.
            // (The reasoning, and the node teardown deadlock that made it unconditional, is above.)
            setScalar("EXIT_RUNTIME", "1");
            if (needsNetWeb) { addList("EXPORTED_RUNTIME_METHODS", "UTF8ToString");
                               addList("EXPORTED_RUNTIME_METHODS", "HEAPU8"); }
            if (needsPthread) {
                // PROXY_TO_PTHREAD runs `main` on a dedicated worker so it may block on join/recv
                // (Atomics.wait THROWS on the JS main thread). The pool is a pre-warm knob, not a cap.
                setScalar("PROXY_TO_PTHREAD", "1");
                setScalar("PTHREAD_POOL_SIZE", "0");
                setScalar("PTHREAD_POOL_SIZE_STRICT", "0");
            }
            std::string merr;
            for (const auto& e : g_emSettings) {
                EmSetting* have = nullptr;
                for (auto& x : em) if (x.key == e.key) { have = &x; break; }
                if (!have) { em.push_back(e); continue; }
                if (have->value.isList != e.value.isList) {
                    fprintf(stderr, "kama: emSettings `%s` is a %s here and a %s in kama's own settings "
                                    "for this build — one shape or the other, not both\n",
                            e.key.c_str(), e.value.isList ? "list" : "single value",
                            have->value.isList ? "list" : "single value");
                    return 2;
                }
                if (e.value.isList) {
                    for (const std::string& v : e.value.list) {
                        bool dup = false;
                        for (const std::string& x : have->value.list) if (x == v) { dup = true; break; }
                        if (!dup) have->value.list.push_back(v);
                    }
                } else {
                    have->value.scalar = e.value.scalar;   // the manifest wins over kama's default
                }
            }
            // ⚠️ AFTER the merge, so the documented env-wins precedence survives: an environment
            // override that a manifest key could beat would be a knob that silently loses.
            if (const char* pool = getenv("KAMA_PTHREAD_POOL"))
                if (*pool && needsPthread) setScalar("PTHREAD_POOL_SIZE", pool);
            for (const auto& e : em) {
                link << "-s" << e.key << "=";
                if (e.value.isList) {
                    for (size_t i = 0; i < e.value.list.size(); ++i)
                        link << (i ? "," : "") << e.value.list[i];
                } else {
                    link << e.value.scalar;
                }
                link << " ";
            }
            // A plain append list — emcc accumulates these, so there is no collision to resolve. Deduped
            // by resolved path: the root and a dependency naming the same file would otherwise define
            // the same JS symbols twice, which emcc refuses.
            if (needsNetWeb)
                link << "--js-library \"" << (resolveStdlibDir(argv[0]) + "/std/net/web/kama_net_web.js")
                     << "\" ";
            std::set<std::string> seenJs;
            for (const CSourceRef& js : g_jsLibraries)
                if (seenJs.insert(js.path).second) link << "--js-library \"" << js.path << "\" ";
        }
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
        //  * wasm: emcc's Python startup makes per-TU spawning far costlier than clang's.
        //    ⚠️ This used to give a second reason — that emcc's link settings (`--js-library`,
        //    `-sEXPORTED_RUNTIME_METHODS`, `-sEXIT_RUNTIME`) sat in the COMPILE flags, so a per-TU
        //    `emcc -c` would warn on every TU. That is no longer true: every `-s` moved to the link
        //    tail when `emSettings` gave them one place to be merged in. The startup cost stands on its
        //    own, so the clamp is unchanged — but the stale reason is removed rather than left to be
        //    believed. (`--use-port=emdawnwebgpu` genuinely is a compile flag: it supplies headers.)
        //  * `zig cc`: measured, and it is a SIGN FLIP rather than a smaller win. zig has its own
        //    content-addressed object cache, so one invocation over 32 TUs is 4.13s cold but 0.07s warm
        //    and 0.11s after editing one file — it already does incremental rebuilds. Per-TU zig cannot
        //    use that cache (it is bounded by zig's ~0.18s process startup: 0.59s cold AND warm), so
        //    parallelizing would be 7x better cold and 5x WORSE in the edit-rebuild loop, which is the
        //    loop that matters. A bundled install is exactly where `zig cc` comes from.
        //
        // ⚠️ Windows is deliberately NOT on this list, and this line is where that gets checked. A
        // bullet here used to say "Windows: no posix_spawn/waitpid pool", which had already stopped
        // being true when runCmdsParallel grew its `_spawnlp(_P_NOWAIT)`/`_cwait` arm — see that
        // function, whose own comment calls the pool "the single largest lever on Windows build time".
        // The comment outlived the code by describing a clamp the condition below never applied.
        if (ccInputs.size() < 2 || wasm || isZig(compiler)) nJobs = 1;

        // Compile every input on its own and join the results, rather than handing them all to one
        // invocation. STATIC has always done this (an archive has no other shape); `-j` widens it to
        // executables and shared libraries, where the join is a link rather than an `ar`.
        const bool perTU = outStatic || nJobs > 1;

        // OUTPUT=OBJECT is ONE translation unit by definition, and this is where that is enforced —
        // before the per-TU/single-invocation split rather than inside the single-invocation arm.
        //
        // ⚠️ It used to live in the else-branch below and count `cFiles`, which made it unreachable for
        // exactly the builds it was written for: two inputs mean `nJobs > 1`, so a multi-unit
        // OUTPUT=OBJECT went down the per-TU path instead, compiled each input, and then "joined" them
        // with a command still carrying `-c`. No diagnostic, and an artifact nobody can use. Counted
        // over `ccInputs` too, so the native gpu seam and a `csources` entry count as the translation
        // units they are.
        if (outObject && ccInputs.size() > 1) {
            fprintf(stderr, "kama: OUTPUT=OBJECT builds a single translation unit, but this build has "
                            "%zu — use OUTPUT=STATIC to get one archive instead\n", ccInputs.size());
            return 2;
        }

        // ⚠️ A non-ASCII OUTPUT NAME (`kama build 日本語.kama` defaults to one) needs one more step than a
        // non-ASCII directory. toolPath borrows a path's 8.3 alias, and a file that does not exist yet has
        // none — so GNU ld would be asked to create `???.exe` and refuse. Link under an ASCII stand-in in
        // the SAME directory (its alias covers the path, and the move into place stays on one volume),
        // then rename. Everywhere else `linkOut` IS `outPath`.
        std::string linkOut = outPath;
#if defined(_WIN32)
        for (unsigned char c : baseName(outPath)) if (c >= 0x80) {
            linkOut = dirName(outPath) + "/kama-out-" + std::to_string((long)getpid())
                    + outPath.substr(stripExtension(outPath).size());       // keep the extension
            break;
        }
#endif
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
                const std::string obj = toolPath(in.obj);   // the object AND cmd's two redirections (see toolPath)
                cmds.push_back(base + dashC + in.tok + "-o \"" + obj + "\""
                               + " >\"" + obj + ".out\" 2>\"" + obj + ".err\"");
            }
            std::vector<int> rcs;
            rc = runCmdsParallel(cmds, nJobs, rcs);
            // Replay in INPUT order, whatever order they finished in. Logs are not build artifacts, so
            // they go regardless of --keep-c.
            for (size_t i = 0; i < cmds.size(); ++i) {
                if (rcs[i] < 0) { remove(osp(objs[i] + ".out").c_str()); remove(osp(objs[i] + ".err").c_str()); continue; }
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
                    ar << (g_target.ar.empty() ? std::string("ar") : g_target.ar) << " rcs \"" << toolPath(linkOut) << "\"";
                    for (auto& o : objs) ar << " \"" << toolPath(o) << "\"";
                    rc = runCmd(ar.str());
                    if (rc != 0) fprintf(stderr, "kama: ar failed (exit %d)\n", rc);
                } else {
                    // Link the objects with the SAME flag prefix the compiles used, not a bare compiler
                    // name: `--cc "clang -fsanitize=address,undefined"` (the sanitizer leg) needs those
                    // flags on the link too, or the runtime is never pulled in.
                    // The user's `cflags` span comes OUT here, and only here: this command compiles
                    // nothing, it consumes objects. See where the span is recorded for what a compile
                    // flag on a link line does (KB-16).
                    std::ostringstream ld;
                    ld << base.substr(0, cflagsPos) << base.substr(cflagsEnd) << linkGc;
                    // toolPath: the objects and the output are what GNU ld opens by name (see toolPath).
                    for (auto& o : objs) ld << "\"" << toolPath(o) << "\" ";
                    ld << link.str() << "-o \"" << toolPath(linkOut) << "\"";
                    rc = runCmd(ld.str());
                }
            }
        } else {
            for (auto& in : ccInputs) cmd << in.tok;
            cmd << linkGc << link.str() << "-o \"" << toolPath(linkOut) << "\"";
            rc = runCmd(cmd.str());
        }

        if (rc != 0) {                       // genCleanup removes the generated files on the way out
            fprintf(stderr, "kama: %s failed (exit %d)\n", compiler.c_str(), rc);
            if (linkOut != outPath) remove(osp(linkOut).c_str());
            return rc;
        }
        if (linkOut != outPath) {
            remove(osp(outPath).c_str());    // a previous build's output — rename does not replace
            if (rename(osp(linkOut).c_str(), osp(outPath).c_str()) != 0) {
                fprintf(stderr, "kama: cannot move the output into place as %s\n", outPath.c_str());
                remove(osp(linkOut).c_str());
                return 1;
            }
        }
        // `kama run`: exec the freshly built binary, forward its exit code, then remove the temp. `-- <args>`
        // are forwarded (quoted) — inert until argv marshaling lands, but wired at the process boundary now.
        if (runMode) {
            std::ostringstream run;
            run << "\"" << outPath << "\"";
            for (auto& pa : progArgs) run << " \"" << pa << "\"";
            int prc = runCmd(run.str());   // system() -> WEXITSTATUS: the child's exit code
#if defined(_WIN32)
            // Windows holds the image section of an executable open a little past process exit, so the
            // remove() below fails outright the instant the child returns — measured at ~774 spins before
            // the handle dropped. That is why every `kama run` on Windows left its binary behind (~110 KB
            // a time) even once the path was right. Bounded spin rather than a sleep: the wait is
            // sub-millisecond, and <windows.h> — where Sleep lives — cannot be included in this TU.
            // If it somehow never clears, let the temp file go: the run already has the child's exit code,
            // and failing `kama run` over a leftover file in the temp directory would be the worse trade.
            for (int i = 0; i < 20000; ++i) if (remove(osp(outPath).c_str()) == 0) break;
#else
            remove(osp(outPath).c_str());
#endif
            return prc;
        }
        fprintf(stderr, "kama: built %s\n", outPath.c_str());
        return 0;
    }

    fprintf(stderr, "kama: unknown subcommand '%s'\n", subcommand.c_str());
    usage();
    return 2;
}
