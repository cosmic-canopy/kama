// kama.winpath.h — the driver's Win32 edge, kept in its own translation unit.
//
// `kama.driver.cpp` cannot include <windows.h>: it is compiled in the same TU as kama.parser.hpp, whose
// token enum (BOOL, CHAR, CONST, INT8, VOID, …) collides with the Win32 typedefs and macros. For years
// that was answered by hand-declaring the two or three Win32 entry points the driver needed. The things
// below cannot be hand-declared away — FindFirstFile wants WIN32_FIND_DATA, CreateProcess wants
// STARTUPINFO — so they live here, behind narrow std::string signatures, in the one file under src/ that
// includes <windows.h>. Everything the driver stores or compares stays the `/`-joined UTF-8 spelling
// absolutePath mints; what crosses into here is handed to the OS and never comes back into a comparison.
//
// The two path facts these rest on are measured in docs/platforms/windows.md § "Things that are true on
// Windows and nowhere else": a `\\?\` prefix lifts MAX_PATH for the NARROW CRT with LongPathsEnabled=0,
// and mingw's opendir on such a path silently lists the WRONG directory (the cwd), which is why the
// driver's directory listing is here and not in <dirent.h>.
#pragma once
#ifdef _WIN32
#include <string>
#include <vector>
#include <cstdint>

// The spelling to hand the OS for `p`, which is a UTF-8 path in either separator. Same policy as
// kama__wpath in include/kama_os.h: under 248 characters and not already verbatim, `p` comes back
// untouched, so the common case is exactly what it was. Otherwise it is made absolute and normalized by
// GetFullPathNameW (`/` → `\`, `.`/`..` collapsed — which Win32 already does lexically) and given the
// `\\?\` prefix (`\\?\UNC\` for a share). A verbatim prefix arriving in EITHER spelling — `\\?\` or the
// `//?/` msys2 produces when it converts a long POSIX argument for a native child (measured) — is
// stripped and re-derived, because the kernel does not recognize the forward-slash one and does no
// normalization inside the backslash one.
std::string kama_win_ospath(const std::string& p);

// GetFullPathNameW over `p`, back as UTF-8 with backslashes and NO prefix — absolutePath's fallback for
// a path that does not exist yet (a `-o` target), where `_fullpath`'s MAX_PATH buffer used to be. Empty
// on failure.
std::string kama_win_fullpath(const std::string& p);

// The entries of directory `dir` (any separator, any length), without `.` and `..`, in the order the
// filesystem hands them back. False iff the directory could not be opened. FindFirstFileW, never dirent.
bool kama_win_listdir(const std::string& dir, std::vector<std::string>& names);

// A short, all-ASCII spelling of `p` — its 8.3 alias — for a tool that can read neither a Unicode nor a
// long one. GNU ld and ar (binutils, what mingw-w64 clang links with) ANSI-decode their argv, so any
// non-ASCII path they are given, input OR output, is `???` and "Invalid argument"; and cmd.exe, which
// carries the `-j` pool's `>out 2>err` redirections, stops at MAX_PATH (both measured: clang's own
// compile step, being LLVM, is fine in every direction and at any length — the linker, the archiver and
// the shell are the narrow programs). A path that is ASCII and under 248 characters comes back
// byte-for-byte. Otherwise the longest EXISTING prefix is shortened (an alias exists only for a name
// that does; a `-o` target's directory exists, its file does not yet) and the rest is appended
// unchanged, with `/` separators like the rest of the driver. `p` itself comes back when the volume
// keeps no short names (8dot3name is on by default on the system volume, where a user's profile and
// therefore the common non-ASCII case lives), so that rare case fails exactly as it did before rather
// than differently.
std::string kama_win_shortpath(const std::string& p);

// Create (replacing any existing link) a directory JUNCTION at `linkPath` pointing at `target`, both
// absolute. False on failure, leaving nothing behind. Uses the filesystem call (FSCTL_SET_REPARSE_POINT),
// so it honours `\\?\` and works past MAX_PATH — unlike the `cmd /c mklink /J` it replaced, which could
// not link a dependency under a 265-character project at all. See the comment on the definition for why
// neither a directory symlink nor an 8.3 alias is the answer here.
bool kama_win_make_junction(const std::string& target, const std::string& linkPath);

// Start `line` as a child process via CreateProcessW with the command line passed VERBATIM — no argv
// re-quoting layer — and return its process handle (for kama_win_wait), or -1. This is what the `-j`
// pool uses in place of a generated .bat: cmd.exe parses a batch FILE in the console code page (437 by
// default, measured), so a non-ASCII path in one is mojibake whatever the process code page says, while
// the same text on cmd's COMMAND LINE arrives as UTF-16 and is read correctly.
intptr_t kama_win_spawn_shell(const std::string& line);

// Wait for a process started by kama_win_spawn_shell, close the handle, return its exit code (1 if the
// wait or the query fails).
int kama_win_wait(intptr_t h);
#endif
