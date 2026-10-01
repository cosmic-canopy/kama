#ifndef KAMA_OS_H
#define KAMA_OS_H

// kama OS/platform I/O bindings — the FFI boundary for `std::io` / `std::fs` / `std::net`.
//
// This header is the ONE place platform + C-preprocessor differences are absorbed, so the kama
// libraries above it stay 100% platform-agnostic. It is a *bindings layer* in the exact sense of Rust's
// `libc`, Zig's `@cImport`, or Go's `syscall`: it re-exposes the C macros (errno, htons, S_ISDIR, O_*,
// AF_INET, …) and the un-typedef'd POSIX/Winsock aggregates (`struct stat`, `sockaddr_in`, `dirent`) that
// a C-targeting language cannot name directly. Those aggregates stay OPAQUE to kama — the C compiler owns
// their platform-specific layout — and kama only ever holds `int32` file fds, `isize` socket handles, and
// scalars.
//
// Pay-for-what-you-use: this header is pulled in ONLY by a module that does `extern "kama_os.h";`, so
// non-I/O programs never `#include <sys/socket.h>`. It is included AFTER kama_runtime.h, whose
// `kama_string` / `kama_string_from_raw` it reuses.
//
// Handles: a POSIX fd is an `int`; a Windows `SOCKET` is a `UINT_PTR` (wider than int), and
// `INVALID_SOCKET` reinterpreted as `ptrdiff_t` is -1 — so a socket handle is carried as `isize`
// (ptrdiff_t) with a uniform `< 0 == error` convention on both platforms. A file fd stays `int32`.
//
// Every function is `static inline` (single-TU, unused ones are pruned) and returns the raw syscall
// result (`< 0` / a set errno = error); the kama layer maps `kama_last_error()` -> `IoError`.
//
// Windows branch: implemented — Winsock (`WSAStartup`/`SOCKET`/`WSAGetLastError`), the WIDE CRT + Win32
// (`_wopen`/`_wstat64`/`FindFirstFileW`/`_pipe`), and std::process (`CreateProcessW`/`WaitForSingleObject`/
// `TerminateProcess`). Every path and every string handed to the OS is converted UTF-8 -> UTF-16 at the call
// (kama__wide / kama__wpath below) and never through an `A` function. The POSIX branch below also serves
// iOS/Android/BSD and (via emscripten's POSIX shims) the wasm target's VFS.

// Restated here as well as in kama_runtime.h, for a TU that reaches this header FIRST: the macro only
// takes effect before glibc's <features.h> is read, and either header may be the one that gets there
// first. Setting it twice to the same value is free; setting it too late is silent, which is why the
// block in kama_runtime.h carries the argument and this one only points at it.
#if defined(__linux__) && !defined(_GNU_SOURCE) && !defined(_DEFAULT_SOURCE)
#  define _DEFAULT_SOURCE 1
#endif

#include "kama_runtime.h"

// A heap block that REMEMBERS its size, for this seam's own buffers whose size is not in hand where they are
// released — a wide path, a command line, an argv/envp vector and each string in it. One `size_t` header, and
// the block comes from and goes back to the allocation funnel with its exact layout, so a replacement allocator
// is never handed a guess. Private to this header: a buffer that becomes a `kama_string` is `kama_alloc(n, 1)`
// with `cap = n`, because the string releases it.
static inline void* kama__sized_alloc(size_t n) {
    size_t* h = (size_t*)kama_alloc(sizeof(size_t) + n, _Alignof(max_align_t));
    if (!h) return NULL;
    h[0] = sizeof(size_t) + n;
    return h + 1;
}
static inline void* kama__sized_zeroed(size_t n) {
    // The name is PARENTHESIZED, declaration and call: on macOS a TU that has already read <string.h> has
    // `memset` as a function-like fortify macro (`__builtin___memset_chk`), and a bare block-scope redeclaration
    // expands into a syntax error — every program including this header failed to compile there. A
    // parenthesized name is never macro-expanded and names the one real function either way.
    extern void* (memset)(void*, int, size_t);
    void* p = kama__sized_alloc(n); if (p) (memset)(p, 0, n); return p;
}
static inline void kama__sized_free(void* p) {
    if (!p) return;
    size_t* h = (size_t*)p - 1;
    kama_free(h, h[0], _Alignof(max_align_t));
}
static inline char* kama__sized_strdup(const char* s) {
    extern size_t strlen(const char*);
    size_t n = strlen(s) + 1;
    char* d = (char*)kama__sized_alloc(n); if (d) kama_copy(d, s, n); return d;
}

#if defined(_WIN32)

// ============================ Windows (Winsock + CRT) ============================
// Same function set and EXACT signatures as the POSIX branch below — the kama modules are identical on
// both platforms; only this header differs. Toolchain is mingw-w64 UCRT + clang (see release.yml). Winsock
// headers MUST precede <windows.h>; WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in the old winsock.h.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
// The readiness poller uses select() (see kama_poller_wait — WSAPoll mis-handles a connecting socket).
// FD_SETSIZE caps how many sockets fit one fd_set; raise it from the default 64 for the server selector.
// MUST be defined before <winsock2.h>.
#ifndef FD_SETSIZE
#  define FD_SETSIZE 1024
#endif
#include <winsock2.h>     // socket, bind, listen, accept, connect, send, recv, WSAStartup, SOCKET
#include <ws2tcpip.h>     // numeric-host helpers; getaddrinfo/freeaddrinfo (kama_resolve_host)
#include <afunix.h>       // sockaddr_un, SIO_AF_UNIX_GETPEERPID (Windows 10 1803+). Adds five macros, none lowercase.
// `if_nametoindex` (kama_interface_index) lives in iphlpapi.dll, linked for a Windows target, and is DECLARED here
// rather than through <iphlpapi.h>, whose chain (iprtrmib.h -> mprapi.h -> ... -> rpc.h/rpcndr.h) ignores
// WIN32_LEAN_AND_MEAN and defines 21 lowercase macros, `#define interface struct` and `hyper` among them. Any kama
// name spelled that way then breaks the C: `UdpSocket.joinMulticastV4(interface:)` failed to compile every program
// importing std::net on Windows (0.9.376). The prototype is netioapi.h's: NET_IFINDEX (ULONG) WINAPI (PCSTR).
unsigned long __stdcall if_nametoindex(const char* name);
#include <windows.h>      // FindFirstFileW / HANDLE / MultiByteToWideChar / GetFullPathNameW
#include <io.h>           // _wopen, _read, _write, _close, _wunlink
#include <direct.h>       // _wmkdir, _wrmdir
#include <fcntl.h>        // _O_*
#include <sys/stat.h>     // _wstat64, _S_IFDIR
#include <errno.h>        // ENOENT, ECONNREFUSED, ... (UCRT defines the POSIX supplemental codes)
#include <string.h>       // memcpy, strlen, wcslen (⚠️ NOT <wchar.h>: it defines a `stdout` macro that breaks a
                          //   kama parameter of that name in the emitted C)
#include <stdlib.h>       // malloc, free (dir cursor, wide-path buffers)

// ---- errno / last-error ----------------------------------------------------
// Windows splits errors: CRT file ops set `errno`; Winsock ops set WSAGetLastError(). The socket wrappers
// below translate the WSA code into the matching UCRT `errno` value, so kama_last_error() (== errno) is the
// single uniform source on BOTH platforms and the kama_EXXX() accessors (UCRT <errno.h>) compare correctly.
static inline int32_t kama_last_error(void)   { return (int32_t)errno; }
// A failure the seam reports, in both halves: `posix` in errno, which is what kama CLASSIFIES (the kama_EXXX()
// accessors below), and `native` — the Winsock or Win32 code, 0 when the failure is the seam's own (no memory, an
// argument it refuses) — in `_doserrno`, which is where the CRT puts the Win32 code of its own failures. So after
// ANY failure, the CRT's or the seam's, `_doserrno` holds that failure's native code and never an older one's,
// and kama_last_os_error reads it back for `IoError.rawOsError()` and its text (KPG-22).
static inline void kama__os_fail(unsigned long native, int posix) { _set_doserrno(native); errno = posix; }
static inline int32_t kama_last_os_error(void) { unsigned long d = 0; _get_doserrno(&d); return (int32_t)d; }
// The system's text for a native code, as UTF-8 in into[0..cap): its length, or 0 when the system has none. The
// trailing period and line break FormatMessage ends with are dropped — the text is quoted inside a sentence.
static inline ptrdiff_t kama_os_error_text(int32_t code, uint8_t* into, size_t cap) {
    if (!into || cap < 2 || code == 0) return 0;
    wchar_t w[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)code, 0,
                             w, (DWORD)(sizeof w / sizeof w[0]), NULL);
    while (n > 0 && (w[n - 1] == L'\r' || w[n - 1] == L'\n' || w[n - 1] == L' ' || w[n - 1] == L'.')) --n;
    if (n == 0) return 0;
    const int m = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, (char*)into, (int)(cap - 1), NULL, NULL);
    return m > 0 ? (ptrdiff_t)m : 0;
}
static inline int32_t kama_ENOENT(void)       { return (int32_t)ENOENT; }
static inline int32_t kama_EACCES(void)       { return (int32_t)EACCES; }
static inline int32_t kama_EPERM(void)        { return (int32_t)EPERM; }
static inline int32_t kama_EAGAIN(void)       { return (int32_t)EAGAIN; }
static inline int32_t kama_EINTR(void)        { return (int32_t)EINTR; }
static inline int32_t kama_ECONNREFUSED(void) { return (int32_t)ECONNREFUSED; }
static inline int32_t kama_ECONNRESET(void)   { return (int32_t)ECONNRESET; }
static inline int32_t kama_EADDRINUSE(void)   { return (int32_t)EADDRINUSE; }
static inline int32_t kama_EINPROGRESS(void)  { return (int32_t)EINPROGRESS; }
static inline int32_t kama_ETIMEDOUT(void)    { return (int32_t)ETIMEDOUT; }
static inline int32_t kama_EHOSTUNREACH(void) { return (int32_t)EHOSTUNREACH; }
static inline int32_t kama_ENETUNREACH(void)  { return (int32_t)ENETUNREACH; }
static inline int32_t kama_EMSGSIZE(void)     { return (int32_t)EMSGSIZE; }
static inline int32_t kama_EPIPE(void)        { return (int32_t)EPIPE; }
static inline int32_t kama_EINVAL(void)       { return (int32_t)EINVAL; }

// Winsock's code as the uniform errno set — one table, for the call that failed and for a connect's SO_ERROR.
static inline int kama__wsa_errno(int e) {
    switch (e) {
        case WSAEWOULDBLOCK:  return EAGAIN;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAECONNRESET:   return ECONNRESET;
        // Winsock reports a send after the peer's reset as WSAECONNRESET or WSAECONNABORTED ("aborted by the
        // software in your host machine": the local stack tore the connection down on the RST), where POSIX says
        // EPIPE or ECONNRESET — the same event, so the same IoError. A send after our own shutdown(SD_SEND) is
        // POSIX's EPIPE. Unmapped, both arrived as IoError::Other: the likeliest reading of the Windows leg's
        // failure of tests/net_write_closed_peer at 0.9.477, which that fixture now names by code if it recurs.
        case WSAECONNABORTED: return ECONNRESET;
        case WSAESHUTDOWN:    return EPIPE;
        case WSAEADDRINUSE:   return EADDRINUSE;
        case WSAEINTR:        return EINTR;
        case WSAEACCES:       return EACCES;
        case WSAEINPROGRESS:  return EINPROGRESS;
        case WSAEALREADY:     return EINPROGRESS;   // non-blocking connect already in flight
        case WSAETIMEDOUT:    return ETIMEDOUT;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAENETUNREACH:  return ENETUNREACH;
        case WSAEMSGSIZE:     return EMSGSIZE;
        case WSAEINVAL:       return EINVAL;
        default:              return e;              // carried through as IoErrorKind::Other, with its code
    }
}
static inline void kama__capture_wsa(void) { const int e = WSAGetLastError(); kama__os_fail((unsigned long)e, kama__wsa_errno(e)); }

// ---- UTF-8 <-> UTF-16, at the edge --------------------------------------------
// A kama string is UTF-8 by definition (lib/std/path/path.kama), and Windows' native string is UTF-16. This
// is utf8everywhere.org's prescription, and what Rust (`maybe_verbatim`), Go (`fixLongPath`), Zig, .NET and
// libuv all do: convert ONCE, immediately before the W call, and never touch an `A` function or the narrow
// CRT — those decode a path in the process ANSI code page (so `日本語` was mojibake before it reached the
// filesystem) and stop at MAX_PATH by construction. tools/check-path-unicode.sh and tools/check-long-path.sh
// hold both halves down.
//
// Invalid UTF-8 is refused (EINVAL) rather than silently substituted: a path that is not a kama string is a
// caller bug, and a name that quietly became `?` would be the ANSI defect wearing a new coat.
static inline wchar_t* kama__wide(const char* s, int len) {   // len -1: NUL-terminated (count INCLUDES the NUL)
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, NULL, 0);
    if (n <= 0) { kama__os_fail(GetLastError(), EINVAL); return NULL; }
    wchar_t* w = (wchar_t*)kama__sized_alloc((size_t)n * sizeof(wchar_t));   // released by kama__wfree
    if (!w) { kama__os_fail(0, ENOMEM); return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, w, n);
    return w;
}
// The read direction, as a fresh UTF-8 string from `kama_alloc(n, 1)` — `n` is `*outLen + 1` — so it may become a
// kama_string's buffer directly (cap = n). ⚠️ NTFS permits a lone surrogate in a name; it comes back as
// U+FFFD and cannot be re-opened — the same limit Rust's `to_str()` has, and not worth an OsString.
static inline char* kama__utf8(const wchar_t* w, size_t* outLen) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);   // includes the NUL
    if (n <= 0) { kama__os_fail(GetLastError(), EINVAL); return NULL; }
    char* s = (char*)kama_alloc((size_t)n, 1);
    if (!s) { kama__os_fail(0, ENOMEM); return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    if (outLen) *outLen = (size_t)n - 1;
    return s;
}
// Releasing may clobber errno; kama_proc_spawn frees its wide strings AFTER the call whose errno it returns.
// Every wide string from kama__wide is a sized block, so this is the one release for all of them.
static inline void kama__wfree(void* p) { int e = errno; kama__sized_free(p); errno = e; }

// ---- a PATH, on the CALLER'S STACK -------------------------------------------
// A path conversion ALLOCATES NOTHING. It used to mint a heap UTF-16 string per call (and a second block
// past 248 characters), which made every `std::fs` call on Windows a heap fact — and therefore on EVERY
// target, since the no-heap walk reads both arms of an `#if` so one verdict covers them all. A path call
// that allocates on no platform now allocates on none, so `--no-heap` admits `std::fs::exists`, `stat`,
// `rename`, `remove`, `createDir`, `removeDir` and `File.open` (the directory and whole-file calls stay
// heap for reasons of their own: a DIR* cursor, an owning buffer). This is Zig's `PathSpace` shape.
// Rust, Go and .NET all keep a heap fallback for a long path, which kama cannot: a reachable allocation
// is a heap fact whether or not it is taken, so a fallback would forfeit the whole point.
//
// Measured on this seam before it was written (Windows, `0.9.408`): main and every worker thread
// (`pthread_create(&t, NULL, …)` in kama_isolate.h — winpthreads inherits SizeOfStackReserve) get 2 MB,
// so one buffer is 3% of a stack and kama_rename's two are 6%. And it is not merely affordable, it is
// CHEAPER than what it replaces: 22 ns/op against 61 ns for the malloc/free pair, same run, `___chkstk_ms`
// present in the disassembly — 2.7x. Every Windows fs call gets that, not only a `--no-heap` build.
#define KAMA__WPATH_CAP 32776      /* NT limit 32767 + `\\?\UNC\` (8) + NUL */
typedef struct kama__wpathbuf { wchar_t w[KAMA__WPATH_CAP]; } kama__wpathbuf;   /* 65552 bytes */

// Past 248 characters (MAX_PATH minus the 12 CreateDirectoryW reserves for an 8.3 name — the threshold
// Rust and Go use) the path is made absolute and normalized by GetFullPathNameW (`/` -> `\`, `..`
// collapsed — which Win32 already does lexically, so nothing changes meaning) and given the `\\?\` prefix
// (`\\?\UNC\` for a share). Every W call here honours that prefix with LongPathsEnabled=0 (probed:
// docs/platforms/windows.md), so no registry setting is asked of the user.
//
// ⚠️ THE SCRATCH IS A SEPARATE FRAME ON PURPOSE. GetFullPathNameW does not document whether `lpBuffer`
// may overlap `lpFileName`, so converting in place would be a guess about undocumented behaviour. It
// writes into this helper's own buffer and the result is copied back, which also keeps the second 64 KB
// off the frame of every SHORT path — the case that is ~always taken.
KAMA_NOINLINE static wchar_t* kama__wpath_long(kama__wpathbuf* b) {
    kama__wpathbuf t;
    DWORD got = GetFullPathNameW(b->w, KAMA__WPATH_CAP - 8, t.w + 8, NULL);  // excludes the NUL on success
    if (got == 0 || got >= KAMA__WPATH_CAP - 8) { kama__os_fail(0, ENOENT); return NULL; }
    wchar_t* start; size_t len;
    if (t.w[8] == L'\\' && t.w[9] == L'\\') {          // \\srv\share\x -> \\?\UNC\srv\share\x
        memcpy(t.w + 2, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));   // lands just before the path past its `\\`
        start = t.w + 2; len = 8 + ((size_t)got - 2);
    } else {                                           // C:\x -> \\?\C:\x
        memcpy(t.w + 4, L"\\\\?\\", 4 * sizeof(wchar_t));
        start = t.w + 4; len = 4 + (size_t)got;
    }
    memcpy(b->w, start, (len + 1) * sizeof(wchar_t));   // distinct buffers, so a copy and not a move
    return b->w;
}

// UTF-8 -> UTF-16 into `b`, returning `b->w` (never a fresh block) or NULL with errno set. Invalid UTF-8
// is EINVAL, as it was; a path too long for the NT namespace is ENAMETOOLONG, which Win32 would refuse
// anyway. Both surface through IoError::Other(code), which is what POSIX already does for ENAMETOOLONG.
static inline wchar_t* kama__wpath(const char* utf8, kama__wpathbuf* b) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, b->w, KAMA__WPATH_CAP);
    if (n <= 0) { { const DWORD kama_w_ = GetLastError(); kama__os_fail(kama_w_, kama_w_ == ERROR_INSUFFICIENT_BUFFER ? ENAMETOOLONG : EINVAL); } return NULL; }
    if ((size_t)(n - 1) < 248 || (b->w[0] == L'\\' && b->w[1] == L'\\' && b->w[2] == L'?' && b->w[3] == L'\\'))
        return b->w;                                   // the common case: no second pass, no branch taken
    return kama__wpath_long(b);
}

// ---- files (wide CRT low-level I/O) -----------------------------------------
// _O_BINARY is essential: Windows text mode would translate CRLF/^Z and corrupt binary data. The `_w*` CRT
// family is preferred over raw Win32 for the file calls because it sets `errno` itself, so kama_last_error()
// stays the single error channel with no GetLastError mapping.
//
// ⚠️ Every wrapper that holds a kama__wpathbuf is KAMA_NOINLINE — see the macro's note in kama_runtime.h.
// It is a stack-overflow guard, not a tuning knob: `removeDirAll` recurses per directory level, and a
// 64 KB buffer folded into that frame would blow a 2 MB stack at depth ~32, inside `rm -rf`, at runtime.
KAMA_NOINLINE static int32_t kama__wopen(const char* path, int flags) {
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    return (int32_t)_wopen(w, flags, _S_IREAD | _S_IWRITE);
}
static inline int32_t   kama_open_read(const char* path)   { return kama__wopen(path, _O_RDONLY | _O_BINARY); }
static inline int32_t   kama_open_create(const char* path) { return kama__wopen(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY); }
static inline int32_t   kama_open_append(const char* path) { return kama__wopen(path, _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY); }
static inline ptrdiff_t kama_read(int32_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)_read((int)fd, buf, (unsigned int)n); }
static inline ptrdiff_t kama_write(int32_t fd, const uint8_t* buf, size_t n) { return (ptrdiff_t)_write((int)fd, buf, (unsigned int)n); }
static inline int32_t   kama_close_fd(int32_t fd) { return (int32_t)_close((int)fd); }
KAMA_NOINLINE static int32_t kama_unlink(const char* path) {
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    return (int32_t)_wunlink(w);
}

// ---- permissions as an access list (KR-92) --------------------------------------------------------------------
// kama's `Permissions` are the nine Unix bits on every platform; Windows keeps an access list, so the bits are
// written as one and read back from one. The mapping is Cygwin's, cut to what kama promises:
//   owner bits -> an ALLOW entry for the file's OWNER: the mapped rights, plus the rights an owner always keeps
//                 (its attributes, its access list, ownership, delete) — so an owner can always change or remove
//                 its own file, `0o000` included, as on Unix;
//   group bits -> an ALLOW entry for the file's GROUP, only when a group bit is set and the group is not the
//                 owner. ⚠️ Weak by nature: a file's group is usually the machine's "None", which every local
//                 user belongs to — documented, not pretended away;
//   other bits -> an ALLOW entry for Everyone, only when an other bit is set: an absent bit is an absent entry,
//                 and a DACL denies whatever it does not grant;
//   SYSTEM     -> full access, always. The maintainer's ruling: private is owner + SYSTEM, the smallest set that
//                 both Windows' own profile ACL and Win32-OpenSSH's key check treat as private. Administrators
//                 get no entry — they can take ownership of any file, as root reads any file;
//   DENY       -> first (canonical order), and only where a class has LESS than a broader class it is a member
//                 of: under `0o077` Everyone's grant would otherwise reach the owner. A deny names data rights
//                 only, never the control rights an owner keeps;
//   PROTECTED  -> the list inherits nothing, so a parent's inheritable entries cannot widen `0o600`.
// A plain create is untouched — it inherits its directory's list, as in every language. Only `permissions:`
// promises anything, and it is EXACT, as on POSIX: set on a new file, rewritten on an existing one, read back
// and compared. A volume that keeps no lists (FAT) fails the comparison and the call fails, rather than
// leaving a "private" file anyone can read.
//
// Everything lives in the caller's frame. `--no-heap` reads this text on every target (the no-heap scan sees
// both arms of the `#if`), so a LocalAlloc'd descriptor here would make `File.openWith` a heap fact on Linux
// too — which is why the MARTA calls (`SetNamedSecurityInfoW`, `GetNamedSecurityInfoW`, `SetEntriesInAclW`)
// are not used: they allocate, and <aclapi.h> defines `interface`. Every call below fills a caller buffer.
// The well-known SIDs the mapping names, in caller storage.
typedef struct kama__wk { BYTE world[SECURITY_MAX_SID_SIZE], auth[SECURITY_MAX_SID_SIZE],
                               users[SECURITY_MAX_SID_SIZE], system[SECURITY_MAX_SID_SIZE]; } kama__wk;
static inline int kama__wk_init(kama__wk* k)
{
    DWORD n;
    n = sizeof k->world;  if (!CreateWellKnownSid(WinWorldSid, NULL, k->world, &n)) return -1;
    n = sizeof k->auth;   if (!CreateWellKnownSid(WinAuthenticatedUserSid, NULL, k->auth, &n)) return -1;
    n = sizeof k->users;  if (!CreateWellKnownSid(WinBuiltinUsersSid, NULL, k->users, &n)) return -1;
    n = sizeof k->system; if (!CreateWellKnownSid(WinLocalSystemSid, NULL, k->system, &n)) return -1;
    return 0;
}
// A class's three bits, read back: the effective rights of the SIDs it answers for, in list order — the
// first entry to decide a right wins, allow or deny, as Windows' own access check decides.
static inline uint32_t kama__acl_class(PACL acl, PSID* sids, int nsids)
{
    DWORD allowed = 0, denied = 0;
    if (!acl) return 7u;   // no list at all: everyone may do everything
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* ace; if (!GetAce(acl, i, &ace)) continue;
        ACE_HEADER* h = (ACE_HEADER*)ace;
        if (h->AceFlags & INHERIT_ONLY_ACE) continue;
        if (h->AceType != ACCESS_ALLOWED_ACE_TYPE && h->AceType != ACCESS_DENIED_ACE_TYPE) continue;
        PSID sid = (PSID)&((ACCESS_ALLOWED_ACE*)ace)->SidStart;
        int hit = 0;
        for (int s = 0; s < nsids; ++s) if (sids[s] && EqualSid(sid, sids[s])) { hit = 1; break; }
        if (!hit) continue;
        DWORD m = ((ACCESS_ALLOWED_ACE*)ace)->Mask;
        if (m & GENERIC_ALL)     m |= FILE_ALL_ACCESS;
        if (m & GENERIC_READ)    m |= FILE_GENERIC_READ;
        if (m & GENERIC_WRITE)   m |= FILE_GENERIC_WRITE;
        if (m & GENERIC_EXECUTE) m |= FILE_GENERIC_EXECUTE;
        if (h->AceType == ACCESS_DENIED_ACE_TYPE) denied |= m & ~allowed;
        else                                      allowed |= m & ~denied;
    }
    return ((allowed & FILE_READ_DATA) ? 4u : 0u) | ((allowed & FILE_WRITE_DATA) ? 2u : 0u)
         | ((allowed & FILE_EXECUTE) ? 1u : 0u);
}
// The nine bits a security descriptor grants: the owner answers through its own entries and every-user
// groups it belongs to, the group through its entry and those, and other through the every-user groups
// (Everyone, Authenticated Users, Users) alone. SYSTEM and Administrators are no class, as root is none.
// `groupIsOwner` reports the one case the bits cannot say apart (a service running as SYSTEM).
static inline uint32_t kama__sd_mode(PSECURITY_DESCRIPTOR sd, int* groupIsOwner)
{
    PSID owner = NULL, group = NULL; PACL dacl = NULL; BOOL def, present = FALSE;
    GetSecurityDescriptorOwner(sd, &owner, &def);
    GetSecurityDescriptorGroup(sd, &group, &def);
    GetSecurityDescriptorDacl(sd, &present, &dacl, &def);
    if (!present) dacl = NULL;
    kama__wk k; if (kama__wk_init(&k) != 0) return 0u;
    const int same = owner && group && EqualSid(owner, group);
    if (groupIsOwner) *groupIsOwner = same;
    PSID os[4] = { owner, (PSID)k.world, (PSID)k.auth, (PSID)k.users };
    PSID gs[4] = { same ? NULL : group, (PSID)k.world, (PSID)k.auth, (PSID)k.users };
    PSID ws[3] = { (PSID)k.world, (PSID)k.auth, (PSID)k.users };
    return (kama__acl_class(dacl, os, 4) << 6) | (kama__acl_class(dacl, same ? os : gs, 4) << 3)
         | kama__acl_class(dacl, ws, 3);
}
// A self-relative descriptor read into the caller's buffer: 4 KB holds every list kama writes and nearly
// every other; a larger one is read again in a separate frame of its own, so the common case never pays for
// the 64 KB an ACL can reach (the `kama__wpath_long` shape). -1 with errno set on failure.
#define KAMA__SD_CAP 4096
#define KAMA__SD_INFO (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION)
typedef struct kama__sdbig { BYTE b[65536 + 1024]; } kama__sdbig;
KAMA_NOINLINE static int32_t kama__sd_mode_big(HANDLE h, const wchar_t* w, uint32_t* mode, int* groupIsOwner)
{
    kama__sdbig big; DWORD need = 0;
    BOOL ok = h ? GetKernelObjectSecurity(h, KAMA__SD_INFO, big.b, sizeof big.b, &need)
                : GetFileSecurityW(w, KAMA__SD_INFO, big.b, sizeof big.b, &need);
    if (!ok) { { const DWORD kama_w_ = GetLastError(); kama__os_fail(kama_w_, kama_w_ == ERROR_ACCESS_DENIED ? EACCES : EINVAL); } return -1; }
    *mode = kama__sd_mode(big.b, groupIsOwner);
    return 0;
}
static inline int32_t kama__sd_read_mode(HANDLE h, const wchar_t* w, uint32_t* mode, int* groupIsOwner)
{
    BYTE sd[KAMA__SD_CAP]; DWORD need = 0;
    BOOL ok = h ? GetKernelObjectSecurity(h, KAMA__SD_INFO, sd, sizeof sd, &need)
                : GetFileSecurityW(w, KAMA__SD_INFO, sd, sizeof sd, &need);
    if (!ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER) return kama__sd_mode_big(h, w, mode, groupIsOwner);
    if (!ok) { { const DWORD kama_w_ = GetLastError(); kama__os_fail(kama_w_, kama_w_ == ERROR_ACCESS_DENIED ? EACCES : EINVAL); } return -1; }
    *mode = kama__sd_mode(sd, groupIsOwner);
    return 0;
}
// The nine bits of a path: its access list, read back, with a file's DOS read-only attribute clearing every
// write bit (as the CRT's own `st_mode` does, and Cygwin) — a FILE's: Windows does not honor the attribute on
// a directory (Explorer sets it there to mark a customized folder), so it clears nothing there. A list this
// process may not read — another user's private file — falls back to what the CRT reports (read/write from
// the attribute, execute from the extension): an approximation, and documented as one.
static inline uint32_t kama__path_mode(HANDLE h, const wchar_t* w, unsigned short crtMode)
{
    const int dir = (crtMode & _S_IFDIR) != 0, readOnly = !dir && !(crtMode & _S_IWRITE);
    uint32_t mode = 0; int same = 0;
    const int e = errno;
    if (kama__sd_read_mode(h, w, &mode, &same) != 0)
        mode = ((crtMode & _S_IREAD) ? 0444u : 0u) | (readOnly ? 0u : 0222u) | ((crtMode & _S_IEXEC) ? 0111u : 0u);
    errno = e;   // the stat succeeded; an unreadable list is not its error
    if (readOnly) mode &= ~0222u;
    return mode;
}

// The kind of file a CRT `st_mode` describes, in std::fs::FileKind's order: 0 file, 1 directory, 2 symlink, 3 FIFO
// (a pipe), 4 character device (`NUL`, a console), 5 block device, 6 socket, 7 anything else. The CRT reports no
// symlink, block device or socket: a link is seen by asking for the link (kama_path_meta, follow 0).
static inline int32_t kama__file_kind(unsigned short m) {
    if (m & _S_IFDIR) return 1;
    if ((m & _S_IFMT) == _S_IFREG) return 0;
    if ((m & _S_IFMT) == _S_IFCHR) return 4;
    if ((m & _S_IFMT) == _S_IFIFO) return 3;
    return 7;
}
// `struct stat` stays opaque: one call folds every fact kama's `Metadata` carries into scalar out-params.
// mtime is NANOSECONDS from the UNIX epoch, so it is a `std::time::Timestamp` with no conversion at the
// kama end — `_stat64` carries whole seconds, which is the resolution Windows reports here. `outMode` is the
// nine permission bits the file's access list grants (kama__path_mode), not an access check for this process.
// `follow` 0 asks about a symbolic link (or a junction) ITSELF — its kind is Symlink, its time its own — where 1
// asks about what it points at, as `_wstat64` does.
KAMA_NOINLINE static int32_t kama_path_meta(const char* path, int32_t follow, uint64_t* outSize, int32_t* outKind,
                                            int64_t* outMtimeNs, uint32_t* outMode) {
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    if (!follow) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(w, &fd);
        if (f == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            kama__os_fail(e, (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES);
            return -1;
        }
        FindClose(f);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            && (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT)) {
            ULARGE_INTEGER t; t.LowPart = fd.ftLastWriteTime.dwLowDateTime; t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            *outSize = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            *outKind = 2;
            *outMtimeNs = ((int64_t)t.QuadPart - 116444736000000000ll) * 100ll;   // 100 ns ticks since 1601
            *outMode = 0777u;   // a link's own bits, as POSIX shows them: access is decided at its target
            return 0;
        }
    }
    struct _stat64 st; int r = _wstat64(w, &st);
    if (r != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outKind = kama__file_kind((unsigned short)st.st_mode);
    *outMtimeNs = (int64_t)st.st_mtime * 1000000000ll;
    *outMode = kama__path_mode(NULL, w, (unsigned short)st.st_mode);
    return 0;
}
// The same facts for an OPEN file, through its descriptor — so a check and the read that follows it are about
// one file, with no time between them in which the path can be pointed elsewhere.
static inline int32_t kama_fd_meta(int32_t fd, uint64_t* outSize, int32_t* outKind, int64_t* outMtimeNs,
                                   uint32_t* outMode) {
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outKind = kama__file_kind((unsigned short)st.st_mode);
    *outMtimeNs = (int64_t)st.st_mtime * 1000000000ll;
    const intptr_t h = _get_osfhandle(fd);
    *outMode = (*outKind == 0 || *outKind == 1) && h != -1 ? kama__path_mode((HANDLE)h, NULL, (unsigned short)st.st_mode)
                                                           : ((st.st_mode & _S_IREAD) ? 0444u : 0u) | ((st.st_mode & _S_IWRITE) ? 0222u : 0u);
    return 0;
}

// The write side of the mapping above (KR-92): the bits become an access list.
#define KAMA__OWNER_KEEPS (READ_CONTROL | WRITE_DAC | WRITE_OWNER | DELETE | FILE_READ_ATTRIBUTES \
                           | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE)
static inline DWORD kama__acl_allow(uint32_t rwx, int isDir)
{
    DWORD m = 0;
    if (rwx & 4) m |= FILE_GENERIC_READ;
    if (rwx & 2) m |= FILE_GENERIC_WRITE | (isDir ? FILE_DELETE_CHILD : 0);
    if (rwx & 1) m |= FILE_GENERIC_EXECUTE;
    return m;
}
static inline DWORD kama__acl_deny(uint32_t rwx, int isDir)
{
    DWORD m = 0;
    if (rwx & 4) m |= FILE_READ_DATA | FILE_READ_EA;
    if (rwx & 2) m |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | (isDir ? FILE_DELETE_CHILD : 0);
    if (rwx & 1) m |= FILE_EXECUTE;
    return m;
}
#define KAMA__ACL_CAP 1024   /* at most six entries of < 100 bytes each */
static inline int kama__acl_build(PACL acl, uint32_t mode, int isDir, PSID owner, PSID group, kama__wk* k)
{
    if (!InitializeAcl(acl, KAMA__ACL_CAP, ACL_REVISION)) return -1;
    const uint32_t u = (mode >> 6) & 7u, g = (mode >> 3) & 7u, o = mode & 7u;
    const int groupOwn = group && !EqualSid(owner, group);
    const uint32_t ownerDeny = (g | o) & ~u & 7u, groupDeny = o & ~g & 7u;
    if (ownerDeny && !AddAccessDeniedAceEx(acl, ACL_REVISION, 0, kama__acl_deny(ownerDeny, isDir), owner)) return -1;
    if (groupOwn && groupDeny && !AddAccessDeniedAceEx(acl, ACL_REVISION, 0, kama__acl_deny(groupDeny, isDir), group)) return -1;
    if (!AddAccessAllowedAceEx(acl, ACL_REVISION, 0, kama__acl_allow(u, isDir) | KAMA__OWNER_KEEPS, owner)) return -1;
    if (groupOwn && g && !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, kama__acl_allow(g, isDir), group)) return -1;
    if (o && !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, kama__acl_allow(o, isDir), (PSID)k->world)) return -1;
    if (!AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, (PSID)k->system)) return -1;
    return 0;
}
// Does the handle's list already grant exactly `mode`? With one SID for owner and group, the group bits echo
// the owner's, so only the bits the list can say apart are compared. 1 yes, 0 no, -1 with errno set.
static inline int kama__mode_is(HANDLE h, const wchar_t* w, uint32_t mode)
{
    uint32_t got = 0; int same = 0;
    if (kama__sd_read_mode(h, w, &got, &same) != 0) return -1;
    const uint32_t cmp = same ? 0707u : 0777u;
    return (got & cmp) == (mode & cmp);
}
// Write `mode` onto an open handle, for the file's OWN owner and group (a file this process did not create
// may belong to someone else), then read it back and compare. -1 with errno set: EACCES when the caller may
// not change the list, EPERM when the volume kept something else.
static inline int32_t kama__apply_mode(HANDLE h, uint32_t mode, int isDir)
{
    BYTE cur[512]; DWORD need = 0;
    if (!GetKernelObjectSecurity(h, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION, cur, sizeof cur, &need)) {
        { const DWORD kama_w_ = GetLastError(); kama__os_fail(kama_w_, kama_w_ == ERROR_ACCESS_DENIED ? EACCES : EINVAL); } return -1;
    }
    PSID owner = NULL, group = NULL; BOOL def;
    GetSecurityDescriptorOwner(cur, &owner, &def);
    GetSecurityDescriptorGroup(cur, &group, &def);
    if (!owner) { kama__os_fail(0, EPERM); return -1; }   // a volume with no owners keeps no lists either
    kama__wk k; if (kama__wk_init(&k) != 0) { kama__os_fail(0, EINVAL); return -1; }
    BYTE aclbuf[KAMA__ACL_CAP];
    if (kama__acl_build((PACL)aclbuf, mode, isDir, owner, group, &k) != 0) { kama__os_fail(0, EINVAL); return -1; }
    SECURITY_DESCRIPTOR sd;
    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorDacl(&sd, TRUE, (PACL)aclbuf, FALSE)
        || !SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) { kama__os_fail(GetLastError(), EINVAL); return -1; }
    // The control bit is what protects the list. SetKernelObjectSecurity accepts PROTECTED_DACL_SECURITY_INFORMATION
    // and IGNORES it — measured (KR-99, Windows 11): on a file that had inherited BA/SY/BU/AU, the flag alone
    // returned TRUE and left the list unprotected, and the bit alone protected it. So no flag is passed.
    if (!SetKernelObjectSecurity(h, DACL_SECURITY_INFORMATION, &sd)) {
        { const DWORD kama_w_ = GetLastError(); kama__os_fail(kama_w_, kama_w_ == ERROR_ACCESS_DENIED ? EACCES : EPERM); } return -1;
    }
    const int is = kama__mode_is(h, NULL, mode);
    if (is < 0) return -1;
    if (!is) { kama__os_fail(0, EPERM); return -1; }
    return 0;
}
// The process's own user and primary group, for a NEW file's owner and group entries — named in the
// descriptor, so the owner entry matches the file's owner even for an elevated administrator, whose new
// objects would otherwise belong to Administrators.
typedef struct kama__tokid { BYTE user[256]; BYTE group[256]; } kama__tokid;
static inline int kama__token_ids(kama__tokid* t, PSID* user, PSID* group)
{
    HANDLE tok; DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return -1;
    int ok = GetTokenInformation(tok, TokenUser, t->user, sizeof t->user, &n)
          && GetTokenInformation(tok, TokenPrimaryGroup, t->group, sizeof t->group, &n);
    CloseHandle(tok);
    if (!ok) return -1;
    *user = ((TOKEN_USER*)t->user)->User.Sid;
    *group = ((TOKEN_PRIMARY_GROUP*)t->group)->PrimaryGroup;
    return 0;
}
// Create or open a file for writing with exactly `mode`. A new file is born with its list (`CREATE_NEW` +
// the descriptor — never a create followed by a separate write, the race `chmod` has); an existing one is
// opened without truncating, given its list, checked, and only then emptied. A new file that fails the check
// is deleted. An existing file this process may write but not re-list (a shared file someone else owns) is
// accepted only when it already holds exactly `mode` — POSIX's answer too, where `fchmod` is skipped when
// nothing would change and refused to a non-owner otherwise. Hands back a CRT descriptor, as every other open
// here does.
KAMA_NOINLINE static int32_t kama_open_create_mode(const char* path, uint32_t mode, int32_t append)
{
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    const DWORD access = GENERIC_WRITE | READ_CONTROL | WRITE_DAC;
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    kama__tokid t; PSID user, group; kama__wk k; BYTE aclbuf[KAMA__ACL_CAP]; SECURITY_DESCRIPTOR sd;
    if (kama__token_ids(&t, &user, &group) != 0 || kama__wk_init(&k) != 0
        || kama__acl_build((PACL)aclbuf, mode, 0, user, group, &k) != 0
        || !InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorOwner(&sd, user, FALSE) || !SetSecurityDescriptorGroup(&sd, group, FALSE)
        || !SetSecurityDescriptorDacl(&sd, TRUE, (PACL)aclbuf, FALSE)
        || !SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) { kama__os_fail(GetLastError(), EINVAL); return -1; }
    SECURITY_ATTRIBUTES sa = { sizeof sa, &sd, FALSE };
    int made = 1, relist = 1;
    HANDLE h = CreateFileW(w, access, share, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS) {
        made = 0;
        h = CreateFileW(w, access, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) {
            relist = 0;
            h = CreateFileW(w, access & ~(DWORD)WRITE_DAC, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        kama__os_fail(e, (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT
              : (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) ? EACCES : EINVAL);
        return -1;
    }
    int bad = 0;
    if (made || !relist) {   // a new file: check what the volume actually kept; a shared one: that it already holds `mode`
        const int is = kama__mode_is(h, NULL, mode);
        if (is <= 0) { if (is == 0) kama__os_fail(0, made ? EPERM : EACCES); bad = -1; }
    } else {
        bad = kama__apply_mode(h, mode, 0);
    }
    if (!bad && !made && !append) {
        LARGE_INTEGER zero; zero.QuadPart = 0;
        if (!SetFilePointerEx(h, zero, NULL, FILE_BEGIN) || !SetEndOfFile(h)) { kama__os_fail(GetLastError(), EACCES); bad = -1; }
    }
    if (bad) {
        int e = errno;
        CloseHandle(h);
        if (made) DeleteFileW(w);
        errno = e;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_WRONLY | (append ? _O_APPEND : 0));
    if (fd < 0) { int e = errno; CloseHandle(h); if (made) DeleteFileW(w); errno = e; return -1; }
    return (int32_t)fd;
}
// A directory born with its list, checked by path; one that fails the check is removed again.
KAMA_NOINLINE static int32_t kama_mkdir_mode(const char* path, uint32_t mode)
{
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    kama__tokid t; PSID user, group; kama__wk k; BYTE aclbuf[KAMA__ACL_CAP]; SECURITY_DESCRIPTOR sd;
    if (kama__token_ids(&t, &user, &group) != 0 || kama__wk_init(&k) != 0
        || kama__acl_build((PACL)aclbuf, mode, 1, user, group, &k) != 0
        || !InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)
        || !SetSecurityDescriptorOwner(&sd, user, FALSE) || !SetSecurityDescriptorGroup(&sd, group, FALSE)
        || !SetSecurityDescriptorDacl(&sd, TRUE, (PACL)aclbuf, FALSE)
        || !SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) { kama__os_fail(GetLastError(), EINVAL); return -1; }
    SECURITY_ATTRIBUTES sa = { sizeof sa, &sd, FALSE };
    if (!CreateDirectoryW(w, &sa)) {
        DWORD e = GetLastError();
        kama__os_fail(e, e == ERROR_ALREADY_EXISTS ? EEXIST : e == ERROR_PATH_NOT_FOUND ? ENOENT
              : e == ERROR_ACCESS_DENIED ? EACCES : EINVAL);
        return -1;
    }
    const int is = kama__mode_is(NULL, w, mode);
    if (is <= 0) {
        const int e = is == 0 ? EPERM : errno;
        RemoveDirectoryW(w);
        errno = e;
        return -1;
    }
    return 0;
}
// `setPermissions`: the list, on a handle opened for exactly that, and — when any class gets write — a
// file's old DOS read-only attribute cleared, since it would still refuse every write the new bits grant. A
// directory keeps its attribute: Windows does not honor it there. The attribute is cleared by path, after the
// new list has granted the owner FILE_WRITE_ATTRIBUTES, so the handle asks for no more than it needs.
KAMA_NOINLINE static int32_t kama_set_permissions(const char* path, uint32_t mode)
{
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    HANDLE h = CreateFileW(w, READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        kama__os_fail(e, (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT
              : e == ERROR_ACCESS_DENIED ? EACCES : EINVAL);
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION fi;
    if (!GetFileInformationByHandle(h, &fi)) { const DWORD kama_w_ = GetLastError(); CloseHandle(h); kama__os_fail(kama_w_, EACCES); return -1; }
    const DWORD attrs = fi.dwFileAttributes;
    const int isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const int32_t rc = kama__apply_mode(h, mode, isDir);
    const int e = errno;
    CloseHandle(h);
    if (rc != 0) { errno = e; return -1; }
    if (!isDir && (mode & 0222u) && (attrs & FILE_ATTRIBUTE_READONLY)
        && !SetFileAttributesW(w, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY)) { kama__os_fail(GetLastError(), EACCES); return -1; }
    return 0;
}

// Directory creation, rename and existence. `_wmkdir` takes no mode on Windows; `MoveFileExW` with
// REPLACE_EXISTING is what makes rename overwrite as POSIX's does (plain MoveFileW fails on an existing
// destination, which would have made the same kama call behave differently per platform).
KAMA_NOINLINE static int32_t kama_mkdir(const char* path) {
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    return (int32_t)_wmkdir(w);
}
KAMA_NOINLINE static int32_t kama_rmdir(const char* path) {
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return -1;
    return (int32_t)_wrmdir(w);
}
KAMA_NOINLINE static DWORD kama__attrs(const char* path) {
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesW(w);
}
// Is this path a symlink (a reparse point here), WITHOUT following it? The distinction only matters to a
// recursive delete, which must not walk through a link and empty a directory somewhere else.
static inline int32_t kama_is_symlink(const char* path) {
    DWORD a = kama__attrs(path);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
}
// The one wrapper holding TWO buffers (128 KB — 6% of the 2 MB every thread gets, measured). The unwind
// that freed the first conversion when the second failed is gone with the allocations.
KAMA_NOINLINE static int32_t kama_rename(const char* from, const char* to) {
    kama__wpathbuf bf, bt;
    wchar_t* wf = kama__wpath(from, &bf); if (!wf) return -1;
    wchar_t* wt = kama__wpath(to,   &bt); if (!wt) return -1;
    if (!MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DWORD e = GetLastError();
        kama__os_fail(e, (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES);
        return -1;
    }
    return 0;
}
static inline int32_t kama_exists(const char* path) {
    return kama__attrs(path) == INVALID_FILE_ATTRIBUTES ? 0 : 1;
}

// ---- directory iteration (Win32 FindFirstFileW) ----------------------------
// DIR* analogue: a heap cursor holding the search handle + the pending entry (FindFirstFile already
// returns the first match). Empty string = end-of-directory; kama filters "." / ".." itself.
typedef struct kama__dir { HANDLE h; WIN32_FIND_DATAW kama_data; int pending; } kama__dir;
// The `<path>\*` search pattern is appended to the CONVERTED path, not built as a narrow string first —
// which deletes the block that form needed (and its ENOMEM bail). The buffer already reserves room: the
// prefix and NUL it is sized for are what the two extra code units come out of. `\*` after the long-path
// pass rather than before is equivalent — it used to rely on GetFullPathNameW preserving a trailing `*`,
// and now nothing has to. tools/check-long-path.sh exercises readDir at 359 characters, which proves it.
//
// This still ALLOCATES — the cursor below is a heap block, and POSIX `opendir` owns its DIR* — so
// `readDir` and `removeDirAll` remain heap facts on every target. That is honest and is asserted by
// tests/xfail/noheap_flag_fs_readdir.d.
KAMA_NOINLINE static void* kama_diropen(const char* path) {
    kama__wpathbuf b; wchar_t* w = kama__wpath(path, &b); if (!w) return NULL;
    size_t n = wcslen(w);
    if (n + 3 > KAMA__WPATH_CAP) { kama__os_fail(0, ENAMETOOLONG); return NULL; }
    w[n] = L'\\'; w[n + 1] = L'*'; w[n + 2] = L'\0';
    kama__dir* d = (kama__dir*)kama_alloc(sizeof *d, _Alignof(kama__dir));
    if (!d) { kama__os_fail(0, ENOMEM); return NULL; }
    d->h = FindFirstFileW(w, &d->kama_data);
    if (d->h == INVALID_HANDLE_VALUE) { const DWORD kama_w_ = GetLastError(); kama_free(d, sizeof *d, _Alignof(kama__dir)); kama__os_fail(kama_w_, ENOENT); return NULL; }
    d->pending = 1;
    return d;
}
static inline int32_t kama_dirclose(void* dirp) {
    kama__dir* d = (kama__dir*)dirp;
    BOOL ok = FindClose(d->h); kama_free(d, sizeof *d, _Alignof(kama__dir)); return ok ? 0 : -1;
}
static inline kama_string kama_dirnext(void* dirp) {
    kama__dir* d = (kama__dir*)dirp;
    kama_string r; r.kama_data = NULL; r.kama_len = 0; r.kama_cap = 0;                     // "" = end of directory
    if (!d->pending && !FindNextFileW(d->h, &d->kama_data)) return r;
    d->pending = 0;
    size_t len = 0;
    char* s = kama__utf8(d->kama_data.cFileName, &len);                          // kama_alloc(len + 1, 1), NUL-terminated
    if (!s) return r;
    r.kama_data = s; r.kama_len = len; r.kama_cap = len + 1;                                // same shape kama_string_from_raw builds
    return r;
}

// ---- TCP sockets (Winsock2) ------------------------------------------------
// A SOCKET is UINT_PTR; INVALID_SOCKET reinterpreted as ptrdiff_t is -1, preserving `< 0 == error`.
static inline int32_t kama_net_init(void) {
    static int done = 0;
    if (!done) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { kama__capture_wsa(); return -1; } done = 1; }
    return 0;
}
// `family` is kama's 4 or 6, never an AF_* value: those differ per OS (AF_INET6 is 23 here, 30 on macOS).
static inline ptrdiff_t kama_socket_tcp(int32_t family) {
    SOCKET s = socket(family == 6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)s;
}
static inline int32_t kama_set_reuseaddr(ptrdiff_t fd) {
    BOOL one = 1; return (int32_t)setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, (int)sizeof one);
}
static inline int32_t kama_listen(ptrdiff_t fd, int32_t backlog) {
    if (listen((SOCKET)fd, (int)backlog) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}
static inline ptrdiff_t kama_accept(ptrdiff_t fd) {
    SOCKET c = accept((SOCKET)fd, (struct sockaddr*)0, (int*)0);
    if (c == INVALID_SOCKET) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)c;
}
static inline ptrdiff_t kama_recv(ptrdiff_t fd, uint8_t* buf, size_t n) {
    int r = recv((SOCKET)fd, (char*)buf, (int)n, 0);
    if (r == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)r;
}
static inline ptrdiff_t kama_send(ptrdiff_t fd, const uint8_t* buf, size_t n) {
    int r = send((SOCKET)fd, (const char*)buf, (int)n, 0);
    if (r == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)r;
}
static inline int32_t kama_close_socket(ptrdiff_t fd) { return (int32_t)closesocket((SOCKET)fd); }

// ---- UDP datagrams (Winsock2) ----------------------------------------------
static inline ptrdiff_t kama_socket_udp(int32_t family) {
    SOCKET s = socket(family == 6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)s;
}

// ---- Unix-domain sockets (Winsock2, Windows 10 1803+) and who is on the other end -------------------------
// AF_UNIX here is stream-only: `datagram` is refused by the OS, and `UnixDatagram` is not declared for Windows.
static inline ptrdiff_t kama_socket_unix(int32_t datagram) {
    SOCKET s = socket(AF_UNIX, datagram ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)s;
}
// A Windows AF_UNIX socket carries no credentials: the peer's process id is the one fact it knows. Who that
// process runs as is its token's (kama_token_open), read when asked — so unlike Linux's SO_PEERCRED, which
// the kernel recorded at connect, it describes the process that holds that id NOW.
static inline int32_t kama_unix_peer_pid(ptrdiff_t fd, int32_t* pid) {
    ULONG p = 0; DWORD got = 0;
    if (WSAIoctl((SOCKET)fd, SIO_AF_UNIX_GETPEERPID, NULL, 0, &p, (DWORD)sizeof p, &got, NULL, NULL) != 0) {
        kama__capture_wsa(); return -1;
    }
    *pid = (int32_t)p;
    return 0;
}
// A process's access token, to read who it runs as. `pid` 0 is this process. -1 with EACCES when the token
// may not be read (a more privileged process's), ESRCH when there is no such process.
static inline ptrdiff_t kama_token_open(int32_t pid) {
    HANDLE proc = pid == 0 ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!proc) { { const DWORD kama_w_ = GetLastError(); kama__os_fail(kama_w_, kama_w_ == ERROR_ACCESS_DENIED ? EACCES : ESRCH); } return -1; }
    HANDLE tok = NULL;
    BOOL ok = OpenProcessToken(proc, TOKEN_QUERY, &tok);
    const DWORD okErr = ok ? 0 : GetLastError();   // before CloseHandle may overwrite it
    if (pid != 0) CloseHandle(proc);
    if (!ok) { kama__os_fail(okErr, EACCES); return -1; }
    return (ptrdiff_t)tok;
}
static inline void kama_token_close(ptrdiff_t tok) { CloseHandle((HANDLE)tok); }
static inline int32_t kama_getpid(void) { return (int32_t)GetCurrentProcessId(); }

// ---- descriptors (std::io::Descriptor) and passing them to another process ------------------------------------
// A `Descriptor` here is either a C-runtime descriptor (kind 0 — what a `File` holds) or a SOCKET (kind 1), and
// the two are closed, duplicated and passed by different calls.
static inline int32_t kama_desc_close(ptrdiff_t h, int32_t kind) {
    return kind == 1 ? (int32_t)closesocket((SOCKET)h) : (int32_t)_close((int)h);
}
static inline int32_t kama_desc_is_socket(ptrdiff_t h, int32_t kind) { (void)h; return kind == 1; }
static inline int32_t kama_desc_kind_fits(int32_t kind, int32_t socket) { return (kind == 1) == (socket != 0); }
static inline ptrdiff_t kama_desc_dup(ptrdiff_t h, int32_t kind) {
    if (kind == 1) {
        WSAPROTOCOL_INFOW info;
        if (WSADuplicateSocketW((SOCKET)h, GetCurrentProcessId(), &info) != 0) { kama__capture_wsa(); return -1; }
        SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0, WSA_FLAG_NO_HANDLE_INHERIT);
        if (s == INVALID_SOCKET) { kama__capture_wsa(); return -1; }
        return (ptrdiff_t)s;
    }
    return (ptrdiff_t)_dup((int)h);   // errno set by the CRT on failure
}
// Windows has no SCM_RIGHTS. The equivalent is to put each handle INTO the peer process — the socket's
// `WSADuplicateSocketW`, a file's `DuplicateHandle` — using the peer's pid (SIO_AF_UNIX_GETPEERPID), then send
// the peer what it needs to find them, ahead of the bytes: a frame of "KFD1", a count, and per descriptor its
// kind, a size, and either the WSAPROTOCOL_INFOW or the handle value as the peer sees it. Both ends are kama —
// this is a convention between them, where POSIX's is the kernel's and reaches any program. Since each handle
// already exists in the peer when this returns, the sender may close its own at once.
#define KAMA__FD_MAX 253          // Linux's SCM_MAX_FD — the most one message carries on any OS here
#define KAMA__FD_MAGIC 0x3144464Bu   // "KFD1" little-endian
static inline int32_t kama__sock_send_all(SOCKET s, const uint8_t* p, size_t n) {
    while (n > 0) {
        int r = send(s, (const char*)p, (int)n, 0);
        if (r == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}
static inline int32_t kama__sock_recv_all(SOCKET s, uint8_t* p, size_t n) {
    while (n > 0) {
        int r = recv(s, (char*)p, (int)n, 0);
        if (r == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
        if (r == 0) { kama__os_fail(0, ECONNRESET); return -1; }   // the frame was cut short
        p += r; n -= (size_t)r;
    }
    return 0;
}
static inline ptrdiff_t kama_send_fds(ptrdiff_t sock, const uint8_t* buf, size_t n, const ptrdiff_t* hs, const int32_t* kinds, size_t count) {
    if (count == 0 || count > KAMA__FD_MAX || n == 0) { kama__os_fail(0, EINVAL); return -1; }
    int32_t pid = 0;
    if (kama_unix_peer_pid(sock, &pid) != 0) return -1;
    const size_t rec = 8 + sizeof(WSAPROTOCOL_INFOW), cap = 8 + count * rec;
    uint8_t* f = (uint8_t*)kama_alloc(cap, sizeof(void*));
    if (!f) { kama__os_fail(0, ENOMEM); return -1; }
    uint32_t hdr[2] = { KAMA__FD_MAGIC, (uint32_t)count };
    memcpy(f, hdr, 8);
    size_t at = 8;
    HANDLE target = NULL;
    for (size_t i = 0; i < count; ++i) {
        uint32_t kind = kinds[i] == 1 ? 1u : 0u, size;
        if (kind == 1) {
            WSAPROTOCOL_INFOW info;
            if (WSADuplicateSocketW((SOCKET)hs[i], (DWORD)pid, &info) != 0) { kama__capture_wsa(); goto fail; }
            size = (uint32_t)sizeof info;
            memcpy(f + at + 8, &info, sizeof info);
        } else {
            if (!target && !(target = OpenProcess(PROCESS_DUP_HANDLE, FALSE, (DWORD)pid))) { kama__os_fail(GetLastError(), EACCES); goto fail; }
            HANDLE mine = (HANDLE)_get_osfhandle((int)hs[i]), theirs = NULL;
            if (mine == INVALID_HANDLE_VALUE) { kama__os_fail(0, EBADF); goto fail; }
            if (!DuplicateHandle(GetCurrentProcess(), mine, target, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS)) { kama__os_fail(GetLastError(), EACCES); goto fail; }
            uint64_t v = (uint64_t)(uintptr_t)theirs;
            size = 8;
            memcpy(f + at + 8, &v, 8);
        }
        memcpy(f + at, &kind, 4);
        memcpy(f + at + 4, &size, 4);
        at += 8 + size;
    }
    if (target) CloseHandle(target);
    int32_t ok = kama__sock_send_all((SOCKET)sock, f, at) == 0 && kama__sock_send_all((SOCKET)sock, buf, n) == 0;
    kama_free(f, cap, sizeof(void*));
    return ok ? (ptrdiff_t)n : -1;
fail:
    if (target) CloseHandle(target);
    kama_free(f, cap, sizeof(void*));
    return -1;
}
static inline ptrdiff_t kama_recv_fds(ptrdiff_t sock, uint8_t* buf, size_t n, ptrdiff_t* hs, int32_t* kinds, size_t cap, size_t* count) {
    *count = 0;
    uint32_t hdr[2];
    int first = recv((SOCKET)sock, (char*)hdr, 8, MSG_PEEK);
    if (first == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
    if (first == 0) return 0;                                     // end of stream: nothing was sent
    if (kama__sock_recv_all((SOCKET)sock, (uint8_t*)hdr, 8) != 0) return -1;
    if (hdr[0] != KAMA__FD_MAGIC || hdr[1] == 0 || hdr[1] > KAMA__FD_MAX) { kama__os_fail(0, EINVAL); return -1; }
    size_t got = 0; int bad = 0;
    for (uint32_t i = 0; i < hdr[1]; ++i) {
        uint32_t ks[2];
        if (kama__sock_recv_all((SOCKET)sock, (uint8_t*)ks, 8) != 0) { bad = 1; break; }
        if (ks[1] > sizeof(WSAPROTOCOL_INFOW)) { kama__os_fail(0, EINVAL); bad = 1; break; }
        union { WSAPROTOCOL_INFOW info; uint64_t v; } p;
        if (kama__sock_recv_all((SOCKET)sock, (uint8_t*)&p, ks[1]) != 0) { bad = 1; break; }
        ptrdiff_t h = -1;
        if (ks[0] == 1) {
            SOCKET s = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &p.info, 0, WSA_FLAG_NO_HANDLE_INHERIT);
            if (s == INVALID_SOCKET) { kama__capture_wsa(); bad = 1; break; }
            h = (ptrdiff_t)s;
        } else {
            int fd = _open_osfhandle((intptr_t)p.v, 0);
            if (fd < 0) { CloseHandle((HANDLE)(uintptr_t)p.v); kama__os_fail(0, EBADF); bad = 1; break; }
            h = (ptrdiff_t)fd;
        }
        if (got < cap) { hs[got] = h; kinds[got] = (int32_t)ks[0]; ++got; }
        else { kama_desc_close(h, (int32_t)ks[0]); bad = 1; kama__os_fail(0, EMSGSIZE); }
    }
    if (bad) { for (size_t i = 0; i < got; ++i) kama_desc_close(hs[i], kinds[i]); return -1; }
    int r = recv((SOCKET)sock, (char*)buf, (int)n, 0);
    if (r == SOCKET_ERROR) { kama__capture_wsa(); for (size_t i = 0; i < got; ++i) kama_desc_close(hs[i], kinds[i]); return -1; }
    *count = got;
    return (ptrdiff_t)r;
}
// A SID of a token — `which` 0 its user, 1 its primary group, 2+i its i-th group — copied into out[cap].
// Returns the SID's length (at most SECURITY_MAX_SID_SIZE, 68), 0 past the last group, or -1.
static inline int32_t kama_token_sid(ptrdiff_t tok, int32_t which, uint8_t* out, size_t cap) {
    TOKEN_INFORMATION_CLASS cls = which == 0 ? TokenUser : which == 1 ? TokenPrimaryGroup : TokenGroups;
    DWORD n = 0;
    (void)GetTokenInformation((HANDLE)tok, cls, NULL, 0, &n);
    if (n == 0) { kama__os_fail(GetLastError(), EACCES); return -1; }
    // Through the allocation funnel (check-alloc-funnel.sh), with the size it was asked for: `n` is rewritten
    // by the second call, so the release names `sz`. Aligned for the pointers the TOKEN_* structs hold.
    const size_t sz = (size_t)n, al = sizeof(void*);
    BYTE* buf = (BYTE*)kama_alloc(sz, al);
    if (!buf) { kama__os_fail(0, ENOMEM); return -1; }
    if (!GetTokenInformation((HANDLE)tok, cls, buf, n, &n)) { const DWORD kama_w_ = GetLastError(); kama_free(buf, sz, al); kama__os_fail(kama_w_, EACCES); return -1; }
    PSID sid;
    if (which == 0)      sid = ((TOKEN_USER*)(void*)buf)->User.Sid;
    else if (which == 1) sid = ((TOKEN_PRIMARY_GROUP*)(void*)buf)->PrimaryGroup;
    else {
        TOKEN_GROUPS* g = (TOKEN_GROUPS*)(void*)buf;
        DWORD i = (DWORD)(which - 2);
        if (i >= g->GroupCount) { kama_free(buf, sz, al); return 0; }
        sid = g->Groups[i].Sid;
    }
    DWORD len = GetLengthSid(sid);
    memcpy(out, sid, len < cap ? len : cap);
    kama_free(buf, sz, al);
    return (int32_t)len;
}
// The account a SID names: its name and its domain, each as UTF-8 into its own buffer with its length.
// -1 with ENOENT when no account has that SID.
static inline int32_t kama_sid_name(const uint8_t* sid, uint8_t* name, size_t ncap, int32_t* nlen,
                                    uint8_t* dom, size_t dcap, int32_t* dlen) {
    WCHAR wn[257], wd[257]; DWORD cn = 257, cd = 257; SID_NAME_USE use;
    if (!LookupAccountSidW(NULL, (PSID)sid, wn, &cn, wd, &cd, &use)) {
        { const DWORD kama_w_ = GetLastError(); kama__os_fail(kama_w_, kama_w_ == ERROR_NONE_MAPPED ? ENOENT : EACCES); } return -1;
    }
    *nlen = cn ? WideCharToMultiByte(CP_UTF8, 0, wn, (int)cn, (char*)name, (int)ncap, NULL, NULL) : 0;
    *dlen = cd ? WideCharToMultiByte(CP_UTF8, 0, wd, (int)cd, (char*)dom, (int)dcap, NULL, NULL) : 0;
    return 0;
}

// The profile directory Windows records for the account a SID names — `ProfileList\<SID>\ProfileImagePath`,
// its `%SystemDrive%`-style variables expanded — as UTF-8: its length, with up to `cap` bytes of it copied into out
// (the caller asks again with room when it is longer), or -1 with ENOENT when that account has no profile here.
// The SID's text is written out (`S-1-5-21-…`, what ConvertSidToStringSid prints) rather than pulling in <sddl.h>,
// as the kama side does for `UserId.sid()`.
static inline size_t kama__dec(wchar_t* at, unsigned long long v) {
    wchar_t digits[24]; size_t n = 0;
    do { digits[n++] = (wchar_t)(L'0' + (int)(v % 10)); v /= 10; } while (v);
    for (size_t i = 0; i < n; ++i) at[i] = digits[n - 1 - i];
    return n;
}
static inline ptrdiff_t kama_sid_home(const uint8_t* sid, uint8_t* out, size_t cap) {
    static const wchar_t base[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\";
    wchar_t key[sizeof base / sizeof base[0] + 16 * 12];
    size_t k = 0;
    for (; base[k]; ++k) key[k] = base[k];
    unsigned long long auth = 0;
    for (int i = 2; i < 8; ++i) auth = auth * 256u + sid[i];
    key[k++] = L'S'; key[k++] = L'-'; k += kama__dec(key + k, sid[0]); key[k++] = L'-'; k += kama__dec(key + k, auth);
    for (int i = 0; i < sid[1] && i < 15; ++i) {
        const uint8_t* p = sid + 8 + 4 * i;
        key[k++] = L'-';
        k += kama__dec(key + k, (unsigned long long)p[0] | (unsigned long long)p[1] << 8
                                | (unsigned long long)p[2] << 16 | (unsigned long long)p[3] << 24);
    }
    key[k] = 0;
    wchar_t path[1024]; DWORD bytes = (DWORD)sizeof path;
    const LSTATUS st = RegGetValueW(HKEY_LOCAL_MACHINE, key, L"ProfileImagePath", RRF_RT_REG_SZ, NULL, path, &bytes);
    if (st != ERROR_SUCCESS) {
        kama__os_fail((unsigned long)st, st == ERROR_FILE_NOT_FOUND ? ENOENT : st == ERROR_ACCESS_DENIED ? EACCES : EINVAL);
        return -1;
    }
    size_t len = 0;
    char* u = kama__utf8(path, &len);
    if (!u) return -1;
    memcpy(out, u, len < cap ? len : cap);
    kama_free(u, len + 1, 1);
    return (ptrdiff_t)len;
}

// The three pieces the shared address calls (below the platform split) need from each platform: the native
// handle type, turning a failed call's error into errno, and the same for a failed connect.
typedef SOCKET kama__sock;
static inline int32_t kama__sock_fail(void) { kama__capture_wsa(); return -1; }
// A connect's error differs from any other call's twice here. Winsock reports a non-blocking connect IN FLIGHT as
// WSAEWOULDBLOCK, where POSIX says EINPROGRESS — the one spelling every caller tests — so a connect's becomes
// EINPROGRESS (a recv's or an accept's WSAEWOULDBLOCK stays EAGAIN). And for a Unix path (`path` non-NULL),
// WSAECONNREFUSED is also Windows' answer when NOTHING is at the path, where POSIX says ENOENT: kama tells the two
// apart by looking, so a missing socket file is NotFound on every OS. Both measured on Windows 11 26200: a connect
// with room in the queue answers WSAEWOULDBLOCK, then polls writable with SO_ERROR 0; a path with no file answers
// WSAECONNREFUSED, blocking or not. A path is at most 107 bytes (kama_unix_path_max), so it fits w[] whole.
static inline int32_t kama__connect_fail(const uint8_t* path, size_t n) {
    const int e = WSAGetLastError();
    if (e == WSAECONNREFUSED && path && n > 0 && n < 128) {
        wchar_t w[128];
        const int k = MultiByteToWideChar(CP_UTF8, 0, (const char*)path, (int)n, w, 127);
        if (k > 0) {
            w[k] = 0;
            if (GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES) {
                const DWORD g = GetLastError();
                if (g == ERROR_FILE_NOT_FOUND || g == ERROR_PATH_NOT_FOUND) { kama__os_fail(g, ENOENT); return -1; }
            }
        }
    }
    kama__os_fail((unsigned long)e, e == WSAEWOULDBLOCK ? EINPROGRESS : kama__wsa_errno(e));
    return -1;
}

// ---- socket options ----------------------------------------------------------
static inline int32_t kama_set_nonblocking(ptrdiff_t fd, int32_t on) {
    u_long mode = on ? 1u : 0u;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}
static inline int32_t kama_set_broadcast(ptrdiff_t fd, int32_t on) {
    BOOL v = on ? 1 : 0;
    if (setsockopt((SOCKET)fd, SOL_SOCKET, SO_BROADCAST, (const char*)&v, (int)sizeof v) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}
static inline int32_t kama_set_nodelay(ptrdiff_t fd, int32_t on) {
    BOOL v = on ? 1 : 0;
    if (setsockopt((SOCKET)fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&v, (int)sizeof v) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}
// Resolve a non-blocking connect: 0 = connected; -1 with errno set to the pending/failed cause. SO_ERROR on
// Winsock is a WSA code, so translate it (mirror kama__capture_wsa) into the uniform errno set.
static inline int32_t kama_socket_error(ptrdiff_t fd) {
    int soerr = 0; int l = (int)sizeof soerr;
    if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &l) != 0) { kama__capture_wsa(); return -1; }
    if (soerr != 0) { kama__os_fail((unsigned long)soerr, kama__wsa_errno(soerr)); return -1; }
    return 0;
}

// ---- readiness poller (select) ---------------------------------------------
// Same shape + bit convention as the POSIX branch; a heap WSAPOLLFD[] behind an UnsafePtr stores each fd +
// requested events (WSAPOLLFD is just a convenient {SOCKET, events, revents} record here). The wait()
// itself uses select(), NOT WSAPoll: WSAPoll mis-handles a *connecting* socket — it can return a socket
// as ready with revents==0 (or never signal a refused connect), so a caller resolving a non-blocking
// connect (poll for writable, then read SO_ERROR) reads SO_ERROR while the handshake is still in flight
// and mis-reports it as connected. select() reports a connecting socket only on genuine resolution —
// writefds on success, exceptfds on failure — which is exactly the "writable == connect resolved"
// contract std::net::Poller advertises. (Bounded by FD_SETSIZE, raised above.)
typedef struct kama__poller { WSAPOLLFD* fds; int kama_len; int kama_cap; } kama__poller;
static inline void* kama_poller_create(void) {
    kama__poller* p = (kama__poller*)kama_alloc(sizeof *p, _Alignof(kama__poller));
    if (!p) { kama__os_fail(0, ENOMEM); return NULL; }
    p->fds = NULL; p->kama_len = 0; p->kama_cap = 0; return p;
}
static inline void kama_poller_add(void* ph, ptrdiff_t fd, int32_t interest) {
    kama__poller* p = (kama__poller*)ph;
    SHORT ev = 0;
    if (interest & 1) ev = (SHORT)(ev | POLLRDNORM);
    if (interest & 2) ev = (SHORT)(ev | POLLWRNORM);
    for (int i = 0; i < p->kama_len; i++) if (p->fds[i].fd == (SOCKET)fd) { p->fds[i].events = ev; p->fds[i].revents = 0; return; }
    if (p->kama_len == p->kama_cap) {
        int nc = p->kama_cap ? p->kama_cap * 2 : 8;
        WSAPOLLFD* nf = (WSAPOLLFD*)kama_alloc((size_t)nc * sizeof(WSAPOLLFD), _Alignof(WSAPOLLFD));
        if (!nf) return;
        if (p->kama_len) kama_copy(nf, p->fds, (size_t)p->kama_len * sizeof(WSAPOLLFD));
        if (p->fds) kama_free(p->fds, (size_t)p->kama_cap * sizeof(WSAPOLLFD), _Alignof(WSAPOLLFD));
        p->fds = nf; p->kama_cap = nc;
    }
    p->fds[p->kama_len].fd = (SOCKET)fd; p->fds[p->kama_len].events = ev; p->fds[p->kama_len].revents = 0; p->kama_len++;
}
static inline void kama_poller_remove(void* ph, ptrdiff_t fd) {
    kama__poller* p = (kama__poller*)ph;
    for (int i = 0; i < p->kama_len; i++) if (p->fds[i].fd == (SOCKET)fd) { p->fds[i] = p->fds[p->kama_len - 1]; p->kama_len--; return; }
}
static inline int32_t kama_poller_wait(void* ph, int32_t timeoutMs) {
    kama__poller* p = (kama__poller*)ph;
    fd_set rd, wr, ex;
    FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
    for (int i = 0; i < p->kama_len; i++) {
        p->fds[i].revents = 0;
        SOCKET s = p->fds[i].fd;
        if (p->fds[i].events & POLLRDNORM) FD_SET(s, &rd);
        if (p->fds[i].events & POLLWRNORM) FD_SET(s, &wr);
        FD_SET(s, &ex);                       // always watch for connect failure / OOB
    }
    struct timeval tv; struct timeval* ptv = NULL;
    if (timeoutMs >= 0) { tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000; ptv = &tv; }
    int r = select(0, &rd, &wr, &ex, ptv);    // Winsock ignores nfds; blocks until ready / timeout
    if (r == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
    if (r == 0) return 0;                      // timed out — no socket resolved
    int ready = 0;                             // recount per-fd (a failed connect sets both wr+ex)
    for (int i = 0; i < p->kama_len; i++) {
        SOCKET s = p->fds[i].fd;
        SHORT rev = 0;
        if (FD_ISSET(s, &rd)) rev = (SHORT)(rev | POLLRDNORM);
        if (FD_ISSET(s, &ex)) rev = (SHORT)(rev | POLLWRNORM | POLLERR);  // failed connect => resolved (writable) + error
        if (FD_ISSET(s, &wr)) rev = (SHORT)(rev | POLLWRNORM);            // success => writable
        p->fds[i].revents = rev;
        if (rev) ready++;
    }
    return (int32_t)ready;
}
static inline int32_t kama_poller_ready(void* ph, ptrdiff_t fd) {
    kama__poller* p = (kama__poller*)ph;
    for (int i = 0; i < p->kama_len; i++) if (p->fds[i].fd == (SOCKET)fd) {
        int bits = 0;
        if (p->fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) bits |= 1;
        if (p->fds[i].revents & POLLWRNORM) bits |= 2;
        return (int32_t)bits;
    }
    return 0;
}
static inline void kama_poller_free(void* ph) {
    kama__poller* p = (kama__poller*)ph;
    if (!p) return;
    if (p->fds) kama_free(p->fds, (size_t)p->kama_cap * sizeof(WSAPOLLFD), _Alignof(WSAPOLLFD));
    kama_free(p, sizeof *p, _Alignof(kama__poller));
}

// ---- process (std::process; Win32 CreateProcessW) --------------------------
// Same seam + EXACT signatures as the POSIX branch — process.kama is byte-identical across platforms. The
// argv-vector helpers (kama_argv_*) are platform-agnostic; kama_envp_build returns the SAME char*[] shape as
// POSIX (so process.kama frees it with kama_argv_free), and kama_proc_spawn converts argv[]/envp[] into the
// Windows command-line string + double-NUL env block on the fly — in UTF-8, then to UTF-16 at the call.
static inline void* kama_argv_new(int32_t n) { return kama__sized_zeroed(((size_t)n + 1) * sizeof(char*)); }
static inline void  kama_argv_set(void* v, int32_t i, const char* s) { ((char**)v)[i] = kama__sized_strdup(s ? s : ""); }
static inline void  kama_argv_free(void* v) {
    if (!v) return;
    for (char** p = (char**)v; *p; ++p) kama__sized_free(*p);
    kama__sized_free(v);
}
// Merge the inherited env (unless `clear`) with each "KEY=VALUE" override (replace-by-KEY, else append) into
// a fresh char*[] of sized blocks — identical semantics to the POSIX kama_envp_build. The inherited half is read
// from GetEnvironmentStringsW, the process's own UTF-16 block, converted to UTF-8 — NOT `_environ`, which is
// that block re-encoded through the ANSI code page. The hidden `=C:=...` drive entries are dropped, as the
// CRT drops them.
static inline void* kama_envp_build(void* overridesV, int32_t clear) {
    char** ov = (char**)overridesV;
    int nov = 0; if (ov) while (ov[nov]) nov++;
    wchar_t* blk = clear ? NULL : GetEnvironmentStringsW();
    int nbase = 0; if (blk) for (wchar_t* p = blk; *p; p += wcslen(p) + 1) nbase++;
    char** out = (char**)kama__sized_zeroed(((size_t)nbase + (size_t)nov + 1) * sizeof(char*));
    int k = 0;
    if (blk) for (wchar_t* p = blk; *p; p += wcslen(p) + 1) {
        if (*p == L'=') continue;
        size_t elen = 0;
        char* e = kama__utf8(p, &elen);                                     // kama_alloc(elen + 1, 1)
        if (!e) continue;
        const char* eq = strchr(e, '=');
        size_t klen = eq ? (size_t)(eq - e) : strlen(e);
        int overridden = 0;
        for (int j = 0; j < nov; j++) {
            const char* o = ov[j]; const char* oeq = strchr(o, '=');
            size_t olen = oeq ? (size_t)(oeq - o) : strlen(o);
            if (olen == klen && _strnicmp(o, e, klen) == 0) { overridden = 1; break; }   // Windows env is case-insensitive
        }
        if (!overridden) out[k++] = kama__sized_strdup(e);                  // the vector's strings are sized blocks
        kama_free(e, elen + 1, 1);
    }
    if (blk) FreeEnvironmentStringsW(blk);
    for (int j = 0; j < nov; j++) out[k++] = kama__sized_strdup(ov[j]);
    out[k] = NULL;
    return out;
}
// argv[] -> a single Win32 command line with MSVCRT/CommandLineToArgvW quoting (quote args with space/tab/
// newline/vtab/quote/empty; double a run of backslashes that precedes a `"` or the closing quote, and escape
// each embedded `"`). Ref: Daniel Colascione, "Everyone quotes command line arguments the wrong way."
static inline char* kama__win_cmdline(char** argv) {
    size_t total = 1;
    for (char** a = argv; *a; a++) total += 2 * strlen(*a) + 3;   // worst case: full backslash doubling + 2 quotes + space
    char* buf = (char*)kama__sized_alloc(total);                  // worst case, so it is released by its header
    if (!buf) return NULL;
    char* w = buf;
    for (char** a = argv; *a; a++) {
        if (a != argv) *w++ = ' ';
        const char* s = *a;
        int needQuote = (*s == '\0');
        for (const char* p = s; *p; p++) if (*p==' '||*p=='\t'||*p=='\n'||*p=='\v'||*p=='"') { needQuote = 1; break; }
        if (!needQuote) { for (const char* p = s; *p; p++) *w++ = *p; continue; }
        *w++ = '"';
        for (size_t i = 0; s[i]; ) {
            size_t nbs = 0;
            while (s[i] == '\\') { nbs++; i++; }
            if (s[i] == '\0')      { for (size_t k=0;k<nbs*2;k++)   *w++='\\'; break; }
            else if (s[i] == '"')  { for (size_t k=0;k<nbs*2+1;k++) *w++='\\'; *w++='"'; i++; }
            else                   { for (size_t k=0;k<nbs;k++)     *w++='\\'; *w++=s[i]; i++; }
        }
        *w++ = '"';
    }
    *w = '\0';
    return buf;
}
// char*[] "KEY=VALUE" -> a Win32 environment block (each string NUL-terminated, the whole block ending in an
// extra NUL), with its byte length in *outLen so the embedded NULs survive the UTF-16 conversion. NULL envp
// means inherit (kama_proc_spawn passes NULL straight through to CreateProcessW).
static inline char* kama__win_envblock(char** envp, size_t* outLen) {
    size_t total = 2;                                  // final "\0\0" (also the empty-block case)
    for (char** e = envp; *e; e++) total += strlen(*e) + 1;
    char* buf = (char*)kama__sized_alloc(total);
    if (!buf) return NULL;
    char* w = buf;
    for (char** e = envp; *e; e++) { size_t l = strlen(*e); memcpy(w, *e, l); w += l; *w++ = '\0'; }
    *w++ = '\0'; *w = '\0';
    *outLen = total;
    return buf;
}
// Pipe over CRT fds (so File{int32 fd} works unchanged). Both ends non-inheritable (_O_NOINHERIT) — the POSIX
// FD_CLOEXEC analog; kama_proc_spawn makes ONLY the child's chosen ends inheritable for the CreateProcess call.
static inline int32_t kama_pipe(int32_t* outRd, int32_t* outWr) {
    int fds[2];
    if (_pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) != 0) return -1;
    *outRd = (int32_t)fds[0]; *outWr = (int32_t)fds[1];
    return 0;
}
static inline int32_t kama_proc_spawn(void* argv, void* envp, const char* cwd,
                                      int32_t inFd, int32_t outFd, int32_t errFd,
                                      ptrdiff_t* outHandle, int32_t* outPid) {
    // Quote in UTF-8 first (the metacharacters are all ASCII, so the bytes >= 0x80 pass through untouched),
    // then convert the finished command line, the env block (by explicit length: it holds NULs) and the cwd.
    // The cwd gets the PLAIN conversion, not kama__wpath: CreateProcessW rejects a >MAX_PATH directory with
    // or without the `\\?\` prefix (probed: docs/platforms/windows.md), so prefixing would only change the
    // error. CreateProcessW writes into the command line, hence the heap copy.
    char* cmdline = kama__win_cmdline((char**)argv);
    if (!cmdline) { kama__os_fail(0, ENOMEM); return -1; }
    size_t envlen = 0;
    char* envblock = envp ? kama__win_envblock((char**)envp, &envlen) : NULL;
    wchar_t* wcmd = kama__wide(cmdline, -1);
    wchar_t* wenv = envblock ? kama__wide(envblock, (int)envlen) : NULL;
    wchar_t* wcwd = (cwd && cwd[0]) ? kama__wide(cwd, -1) : NULL;
    int convOk = wcmd && (!envblock || wenv) && (!(cwd && cwd[0]) || wcwd);
    kama__wfree(cmdline); kama__wfree(envblock);
    if (!convOk) { kama__wfree(wcmd); kama__wfree(wenv); kama__wfree(wcwd); return -1; }   // errno: EINVAL (bad UTF-8) or ENOMEM

    STARTUPINFOW si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    int useStd = (inFd >= 0 || outFd >= 0 || errFd >= 0);
    HANDLE hIn = INVALID_HANDLE_VALUE, hOut = INVALID_HANDLE_VALUE, hErr = INVALID_HANDLE_VALUE;
    if (useStd) {
        // A redirected stream -> the pipe/NUL fd's HANDLE; an inherited one (fd -1) -> the parent's std handle.
        hIn  = (inFd  >= 0) ? (HANDLE)_get_osfhandle((int)inFd)  : GetStdHandle(STD_INPUT_HANDLE);
        hOut = (outFd >= 0) ? (HANDLE)_get_osfhandle((int)outFd) : GetStdHandle(STD_OUTPUT_HANDLE);
        hErr = (errFd >= 0) ? (HANDLE)_get_osfhandle((int)errFd) : GetStdHandle(STD_ERROR_HANDLE);
        if (hIn  != INVALID_HANDLE_VALUE) SetHandleInformation(hIn,  HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        if (hOut != INVALID_HANDLE_VALUE) SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        if (hErr != INVALID_HANDLE_VALUE) SetHandleInformation(hErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hIn; si.hStdOutput = hOut; si.hStdError = hErr;
    }
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, useStd ? TRUE : FALSE, CREATE_UNICODE_ENVIRONMENT,
                             wenv, wcwd, &si, &pi);
    if (useStd) {   // undo inheritability on the parent's copies of the redirected ends (belt-and-suspenders)
        if (inFd  >= 0 && hIn  != INVALID_HANDLE_VALUE) SetHandleInformation(hIn,  HANDLE_FLAG_INHERIT, 0);
        if (outFd >= 0 && hOut != INVALID_HANDLE_VALUE) SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, 0);
        if (errFd >= 0 && hErr != INVALID_HANDLE_VALUE) SetHandleInformation(hErr, HANDLE_FLAG_INHERIT, 0);
    }
    kama__wfree(wcmd); kama__wfree(wenv); kama__wfree(wcwd);
    if (!ok) {
        DWORD e = GetLastError();
        kama__os_fail(e, (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES);
        return -1;
    }
    CloseHandle(pi.hThread);
    *outHandle = (ptrdiff_t)pi.hProcess;
    *outPid    = (int32_t)pi.dwProcessId;
    return 0;
}
// Wait on the process HANDLE. flags&1 (kama_WNOHANG) -> poll (timeout 0). Returns >0 (reaped; exit code in
// *outStatus + the HANDLE closed), 0 (WNOHANG: still running, HANDLE kept), -1 (error).
static inline int32_t kama_proc_wait(ptrdiff_t handle, int32_t* outStatus, int32_t flags) {
    HANDLE h = (HANDLE)handle;
    DWORD r = WaitForSingleObject(h, (flags & 1) ? 0 : INFINITE);
    if (r == WAIT_TIMEOUT) return 0;                       // still running
    if (r != WAIT_OBJECT_0) { kama__os_fail(r == WAIT_FAILED ? GetLastError() : 0, EINVAL); return -1; }
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) { kama__os_fail(GetLastError(), EINVAL); return -1; }
    CloseHandle(h);                                        // reaped: release the handle (Windows auto-cleans)
    *outStatus = (int32_t)code;
    return 1;
}
// No wait-status bit-packing on Windows: the status int IS the exit code (a child is never "signalled").
static inline int32_t kama_proc_exited(int32_t st)      { (void)st; return 1; }
static inline int32_t kama_proc_exit_code(int32_t st)   { return st; }
static inline int32_t kama_proc_signaled(int32_t st)    { (void)st; return 0; }
static inline int32_t kama_proc_term_signal(int32_t st) { (void)st; return 0; }
// No POSIX signals: any signal maps to TerminateProcess (documented in process.kama). Exit code 1.
static inline int32_t kama_kill(ptrdiff_t handle, int32_t sig) {
    (void)sig;
    if (!TerminateProcess((HANDLE)handle, 1)) { kama__os_fail(GetLastError(), EINVAL); return -1; }
    return 0;
}
// Non-blocking release on drop: reap-if-exited (harmless) then CloseHandle (detach — the child keeps running,
// the OS cleans up when it exits and no handle remains). Never blocks.
static inline void kama_proc_detach(ptrdiff_t handle) {
    HANDLE h = (HANDLE)handle;
    WaitForSingleObject(h, 0);
    CloseHandle(h);
}
static inline int32_t kama_WNOHANG(void) { return 1; }     // a private sentinel; kama_proc_wait tests flags&1
static inline int32_t kama_SIGKILL(void) { return 9; }     // ignored by kama_kill on Windows (any => Terminate)
static inline int32_t kama_SIGTERM(void) { return 15; }
static inline void    kama_sleep_ms(int32_t ms) { Sleep((DWORD)ms); }
static inline int32_t kama_open_null_read(void)  { return (int32_t)_wopen(L"NUL", _O_RDONLY | _O_BINARY); }
static inline int32_t kama_open_null_write(void) { return (int32_t)_wopen(L"NUL", _O_WRONLY | _O_BINARY); }

// Concurrent stdout+stderr drain (kama_capture2). select() is sockets-only on Windows, so drain with two
// reader threads: a thread drains stderr while this thread drains stdout, then join. Each fills its own
// funnel buffer (the caller frees it with kama_free and the capacity returned) — same contract as the POSIX version.
typedef struct kama__cap { int fd; uint8_t* buf; size_t kama_len; size_t kama_cap; int err; } kama__cap;
static inline DWORD WINAPI kama__cap_thread(LPVOID arg) {
    kama__cap* c = (kama__cap*)arg;
    for (;;) {
        if (c->kama_len == c->kama_cap) {
            size_t nc = c->kama_cap ? c->kama_cap * 2 : 65536;
            uint8_t* nb = (uint8_t*)kama_alloc(nc, 1);
            if (!nb) { c->err = 1; return 0; }
            if (c->kama_len) kama_copy(nb, c->buf, c->kama_len);
            if (c->buf) kama_free(c->buf, c->kama_cap, 1);
            c->buf = nb; c->kama_cap = nc;
        }
        int r = _read(c->fd, c->buf + c->kama_len, (unsigned int)(c->kama_cap - c->kama_len));
        if (r > 0)       c->kama_len += (size_t)r;
        else if (r == 0) break;                            // EOF
        else             { c->err = 1; return 0; }
    }
    return 0;
}
static inline int32_t kama_capture2(int32_t outFd, int32_t errFd,
                                    uint8_t** outBuf, size_t* outLen, size_t* outCap,
                                    uint8_t** errBuf, size_t* errLen, size_t* errCap) {
    kama__cap co; co.fd = (int)outFd; co.buf = NULL; co.kama_len = 0; co.kama_cap = 0; co.err = 0;
    kama__cap ce; ce.fd = (int)errFd; ce.buf = NULL; ce.kama_len = 0; ce.kama_cap = 0; ce.err = 0;
    HANDLE th = CreateThread(NULL, 0, kama__cap_thread, &ce, 0, NULL);
    if (!th) { kama__os_fail(GetLastError(), EAGAIN); return -1; }   // nothing drained yet: co.buf is still NULL
    kama__cap_thread(&co);                                 // drain stdout on this thread
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (co.err || ce.err) {
        if (co.buf) kama_free(co.buf, co.kama_cap, 1);
        if (ce.buf) kama_free(ce.buf, ce.kama_cap, 1);
        kama__os_fail(0, EIO); return -1;
    }
    *outBuf = co.buf; *outLen = co.kama_len; *outCap = co.kama_cap;
    *errBuf = ce.buf; *errLen = ce.kama_len; *errCap = ce.kama_cap;
    return 0;
}

#else

#include <errno.h>
#include <string.h>       // memset, strlen
#include <stdlib.h>       // malloc, realloc, free (poller cursor)
#include <fcntl.h>        // open, O_*
#include <sys/stat.h>     // fstat, stat, S_ISDIR
#include <dirent.h>       // opendir, readdir, closedir
#include <sys/socket.h>   // socket, bind, listen, accept, connect, setsockopt, send, recv
#include <netinet/in.h>   // sockaddr_in, htons, htonl, INADDR_ANY
#include <netinet/tcp.h>  // TCP_NODELAY
#include <arpa/inet.h>    // htons, ntohs
#include <net/if.h>       // if_nametoindex (kama_interface_index)
#include <sys/un.h>       // sockaddr_un (Unix-domain sockets)
#include <pwd.h>          // getpwuid_r (UserId.name)
#include <grp.h>          // getgrgid_r (GroupId.name)
#if defined(__APPLE__)
#include <sys/ucred.h>    // struct xucred, LOCAL_PEERCRED (a Unix socket's peer)
#endif
// getaddrinfo, freeaddrinfo, EAI_* (kama_resolve_host). ⚠️ All three are past the ISO C line glibc draws
// under `-std=c11`, which is why kama_runtime.h asks for `_DEFAULT_SOURCE` at the top of every generated
// TU — see the block there before moving this include or "simplifying" that one.
#include <netdb.h>
#include <poll.h>         // poll, struct pollfd, POLLIN/POLLOUT
#include <sys/wait.h>     // waitpid, WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG, WNOHANG  (std::process)
#include <signal.h>       // kill, SIGKILL, SIGTERM  (std::process)

// NOT <unistd.h>: on macOS its `write`/`read` carry a `__DARWIN_ALIAS_C` asm label, and a header that
// declares one of them unlabeled first makes clang error "cannot apply asm label to function after its
// first use." kama_runtime.h's `write` now carries the matching label (see kama_raw_write), so this
// header only needs to keep `read` off that collision course. Declaring the handful we need directly
// also keeps libc decls out of user code, matching kama_runtime.h's block-scoped-extern style.
extern long read(int, void*, size_t);
extern long write(int, const void*, size_t);
extern int  close(int);
extern int  unlink(const char*);
// std::process (fork/exec/pipe): hand-declared for the same reason as read/write above. These have no
// asm-label alias on macOS, so redeclaring is safe even if a socket header transitively pulls <unistd.h>.
extern int   rename(const char*, const char*);   // <stdio.h>'s, and the only thing this file wants from it
extern int   rmdir(const char*);                 // <unistd.h>'s, which this file deliberately does not pull
extern int   pipe(int[2]);
extern int   dup2(int, int);
extern int   chdir(const char*);
extern int   execvp(const char*, char* const[]);
extern int   fork(void);
extern void  _exit(int);
extern int   kill(int, int);
extern int   fcntl(int, int, ...);
extern int   ftruncate(int, off_t);              // <unistd.h>'s (KR-92: `File.openWith` empties a file only after its mode is set)
extern uid_t getuid(void);                       // <unistd.h>'s (std::process::currentUser)
extern gid_t getgid(void);
extern pid_t getpid(void);                       // (std::process::currentProcessId)
extern char** environ;

// ---- errno / last-error ----------------------------------------------------
// A stable accessor + constant accessors, so kama never hardcodes per-OS errno numbers. (The Windows
// branch will map WSAGetLastError() codes onto these same POSIX values.)
static inline int32_t kama_last_error(void)   { return (int32_t)errno; }
// The Windows branch's pair, over errno: here the OS's own code IS errno, so a seam refusal sets it and that
// is all (`native` exists for the Windows half, and is ignored).
static inline void kama__os_fail(unsigned long native, int posix) { (void)native; errno = posix; }
static inline int32_t kama_last_os_error(void) { return (int32_t)errno; }
// The OS's text for an errno value, into[0..cap): its length, or 0 when there is none. strerror_r, not strerror —
// an isolate is a thread. glibc's GNU strerror_r (under _GNU_SOURCE) RETURNS its text, maybe not in `into`;
// every other libc's (macOS, musl, glibc's XSI one) fills `into` and returns 0.
static inline ptrdiff_t kama_os_error_text(int32_t code, uint8_t* into, size_t cap) {
    if (!into || cap < 2 || code == 0) return 0;
    char* buf = (char*)into;
#if defined(__EMSCRIPTEN__)
    // The wasm build compiles strict C11, under which musl's <string.h> does not declare strerror_r — and musl's
    // strerror answers from a constant table with no shared buffer, so it is the thread-safe call there anyway.
    const char* t = strerror(code);
    if (!t) return 0;
    size_t n = strlen(t);
    if (n >= cap) n = cap - 1;
    memcpy(buf, t, n);
    return (ptrdiff_t)n;
#elif defined(__GLIBC__) && defined(_GNU_SOURCE)
    const char* t = strerror_r(code, buf, cap);
    if (!t) return 0;
    size_t n = strlen(t);
    if (n >= cap) n = cap - 1;
    if (t != buf) memcpy(buf, t, n);
    return (ptrdiff_t)n;
#else
    if (strerror_r(code, buf, cap) != 0) return 0;
    return (ptrdiff_t)strlen(buf);
#endif
}
static inline int32_t kama_ENOENT(void)       { return (int32_t)ENOENT; }
static inline int32_t kama_EACCES(void)       { return (int32_t)EACCES; }
static inline int32_t kama_EPERM(void)        { return (int32_t)EPERM; }
static inline int32_t kama_EAGAIN(void)       { return (int32_t)EAGAIN; }
static inline int32_t kama_EINTR(void)        { return (int32_t)EINTR; }
static inline int32_t kama_ECONNREFUSED(void) { return (int32_t)ECONNREFUSED; }
static inline int32_t kama_ECONNRESET(void)   { return (int32_t)ECONNRESET; }
static inline int32_t kama_EADDRINUSE(void)   { return (int32_t)EADDRINUSE; }
static inline int32_t kama_EINPROGRESS(void)  { return (int32_t)EINPROGRESS; }
static inline int32_t kama_ETIMEDOUT(void)    { return (int32_t)ETIMEDOUT; }
static inline int32_t kama_EHOSTUNREACH(void) { return (int32_t)EHOSTUNREACH; }
static inline int32_t kama_ENETUNREACH(void)  { return (int32_t)ENETUNREACH; }
static inline int32_t kama_EMSGSIZE(void)     { return (int32_t)EMSGSIZE; }
static inline int32_t kama_EPIPE(void)        { return (int32_t)EPIPE; }
static inline int32_t kama_EINVAL(void)       { return (int32_t)EINVAL; }

// ---- files -----------------------------------------------------------------
// A plain create asks for 0666 and the process umask narrows it — the default of C `fopen` (POSIX says so), Go,
// Rust, Python, Node, Zig, Java and .NET, and what `mkdir` below already did for a directory (KR-92). It was a
// hardcoded 0644, which agrees under the usual umask 022 and 077 but under 002 (a group-shared directory)
// stripped the group-write bit the user's own umask granted — overriding the one setting that says what a new
// file should allow. Only an explicit `permissions:` (kama_open_create_mode) promises a mode.
static inline int32_t   kama_open_read(const char* path)   { return (int32_t)open(path, O_RDONLY); }
static inline int32_t   kama_open_create(const char* path) { return (int32_t)open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666); }
static inline int32_t   kama_open_append(const char* path) { return (int32_t)open(path, O_WRONLY | O_CREAT | O_APPEND, 0666); }
static inline ptrdiff_t kama_read(int32_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)read((int)fd, buf, n); }
static inline ptrdiff_t kama_write(int32_t fd, const uint8_t* buf, size_t n) {
    ptrdiff_t r = (ptrdiff_t)write((int)fd, buf, n);
#if !defined(__EMSCRIPTEN__)
    if (r < 0 && (fd == 1 || fd == 2)) { extern void kama__stdio_write_failed(void); kama__stdio_write_failed(); }   // KPG-1
#endif
    return r;
}
static inline int32_t   kama_close_fd(int32_t fd) { return (int32_t)close((int)fd); }
static inline int32_t   kama_unlink(const char* path) { return (int32_t)unlink(path); }

// `struct stat` stays opaque: one call folds every fact kama's `Metadata` carries into scalar out-params.
// 0 = ok, -1 = error (errno set). mtime is NANOSECONDS from the UNIX epoch, so it IS a
// `std::time::Timestamp` at the kama end with no conversion.
//
// The sub-second field is spelled three ways across the platforms this branch serves, and each one is
// picked by the spelling it actually has rather than by the OS name: macOS/BSD's `st_mtimespec` is visible
// whenever no strict `_POSIX_C_SOURCE` hides it (nothing in `include/` sets one); POSIX.1-2008 defines
// `st_mtime` as a MACRO over `st_mtim.tv_sec` — glibc under `_DEFAULT_SOURCE` (set above), musl, and
// emscripten's musl all do — so `defined(st_mtime)` is the portable test for `st_mtim`; and a libc with
// neither gets whole seconds, which is exactly what this seam reported everywhere until 0.9.220. It used
// to stop at whole seconds on purpose, "not worth the precision nothing in `std` compares on" — the
// consumer-driven audit reversed that: a build tool compares mtimes, and a rebuild inside one second is
// the common case on a fast box. (The Windows branch stays at whole seconds: `_stat64` has no field.)
#if defined(__APPLE__)
#  define KAMA_ST_MTIME_NSEC(st) ((int64_t)(st).st_mtimespec.tv_nsec)
#elif defined(st_mtime)
#  define KAMA_ST_MTIME_NSEC(st) ((int64_t)(st).st_mtim.tv_nsec)
#else
#  define KAMA_ST_MTIME_NSEC(st) ((int64_t)0)
#endif
// The kind of file an `st_mode` describes, in std::fs::FileKind's order: 0 file, 1 directory, 2 symlink, 3 FIFO,
// 4 character device, 5 block device, 6 socket, 7 anything else.
static inline int32_t kama__file_kind(mode_t m) {
    if (S_ISREG(m)) return 0;
    if (S_ISDIR(m)) return 1;
    if (S_ISLNK(m)) return 2;
    if (S_ISFIFO(m)) return 3;
    if (S_ISCHR(m)) return 4;
    if (S_ISBLK(m)) return 5;
#if defined(S_ISSOCK)
    if (S_ISSOCK(m)) return 6;
#endif
    return 7;
}
static inline void kama__stat_facts(const struct stat* st, uint64_t* outSize, int32_t* outKind, int64_t* outMtimeNs,
                                    uint32_t* outMode) {
    *outSize = (uint64_t)st->st_size; *outKind = kama__file_kind(st->st_mode);
    *outMtimeNs = (int64_t)st->st_mtime * 1000000000ll + KAMA_ST_MTIME_NSEC(*st);
    *outMode = (uint32_t)st->st_mode & 0777u;
}
// `outMode` is the nine PERMISSION bits the file records, not an access check for this process (root ignores
// them, and an ACL can deny a file whose mode looks writable) — `access(W_OK)` would answer a different question.
// `follow` 0 is `lstat`: a symbolic link's own facts, its kind Symlink; 1 is `stat`, what it points at.
static inline int32_t kama_path_meta(const char* path, int32_t follow, uint64_t* outSize, int32_t* outKind,
                                     int64_t* outMtimeNs, uint32_t* outMode) {
    struct stat st; if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return -1;
    kama__stat_facts(&st, outSize, outKind, outMtimeNs, outMode);
    return 0;
}
// The same facts for an OPEN file (`fstat`) — so a check and the read that follows it are about one file, with no
// time between them in which the path can be pointed elsewhere.
static inline int32_t kama_fd_meta(int32_t fd, uint64_t* outSize, int32_t* outKind, int64_t* outMtimeNs,
                                   uint32_t* outMode) {
    struct stat st; if (fstat(fd, &st) != 0) return -1;
    kama__stat_facts(&st, outSize, outKind, outMtimeNs, outMode);
    return 0;
}
// Directory creation, rename and existence. 0777 is the POSIX default — the process umask narrows it,
// which is the one place a mode belongs. `rename` already replaces an existing destination here; the
// Windows branch has to ask for that explicitly to match.
static inline int32_t kama_mkdir(const char* path) { return (int32_t)mkdir(path, 0777); }
static inline int32_t kama_rmdir(const char* path) { return (int32_t)rmdir(path); }

// ---- permissions (KR-92) ------------------------------------------------------------------------------------
// A plain create asks for 0666 (above) and the process umask narrows it — the default of C `fopen`, Go, Rust,
// Python, Node, Zig, Java and .NET. `permissions:` is different, and EXACT (the maintainer's ruling): what the
// code says is what the file holds, on every platform, whether the call created the file or found it. The
// umask is the user's standing policy for DEFAULTS; an explicit request is the program's, and `install -m`,
// `mkdir -m` and `cp -p` already set exact modes. Exactness also closes the gap Go documents and leaves open:
// `O_TRUNC` keeps an existing file's mode, so a secret written through it lands in whatever the file was.
//
// The order is the point:
//   1. `O_CREAT|O_EXCL` — this call made the file, so a failure below may remove it again;
//   2. otherwise open WITHOUT `O_TRUNC` — an existing file is not emptied until its mode is right;
//   3. `fchmod` to exactly `mode` when it differs, then `fstat` again — a filesystem that cannot hold a mode
//      (FAT, some network mounts) either refuses or silently keeps its own, and the second answer is what
//      turns that into EPERM instead of a file left open;
//   4. only then `ftruncate` — a failed call never truncates a file it could not make private.
// A dangling link fails `O_EXCL` (EEXIST) and then the plain open (ENOENT): the last attempt is an ordinary
// `O_CREAT` open, which makes the link's target, and is treated as found rather than made.
static inline int32_t kama__mode_exact(int fd, uint32_t mode)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    if (((uint32_t)st.st_mode & 0777u) == mode) return 0;
    if (fchmod(fd, (mode_t)mode) != 0) return -1;
    if (fstat(fd, &st) != 0) return -1;
    if (((uint32_t)st.st_mode & 0777u) != mode) { errno = EPERM; return -1; }
    return 0;
}
static inline int32_t kama_open_create_mode(const char* path, uint32_t mode, int32_t append)
{
    const int flags = O_WRONLY | (append ? O_APPEND : 0);
    int made = 1;
    int fd = open(path, flags | O_CREAT | O_EXCL, (mode_t)mode);
    if (fd < 0 && errno == EEXIST) {
        made = 0;
        fd = open(path, flags);
        if (fd < 0 && errno == ENOENT) fd = open(path, flags | O_CREAT, (mode_t)mode);
    }
    if (fd < 0) return -1;
    if (kama__mode_exact(fd, mode) != 0 || (!append && !made && ftruncate(fd, 0) != 0)) {
        int e = errno;
        close(fd);
        if (made) unlink(path);
        errno = e;
        return -1;
    }
    return (int32_t)fd;
}
// A directory the same way: `mkdir` narrows by the umask like any create, so the mode is set exactly on a
// descriptor opened with `O_NOFOLLOW` — never by path, which a racing rename could point at something else.
// Opening a directory needs read on it, so the owner holds read until the descriptor sets the real bits: a
// bit only the owner gains, for a moment, on a directory the owner could chmod anyway. Without it a mode
// that gives the owner no read (0o300) could never be set by anyone but root.
static inline int32_t kama_mkdir_mode(const char* path, uint32_t mode)
{
    if (mkdir(path, (mode_t)(mode | 0400u)) != 0) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0 || kama__mode_exact(fd, mode) != 0) {
        int e = errno;
        if (fd >= 0) close(fd);
        rmdir(path);
        errno = e;
        return -1;
    }
    close(fd);
    return 0;
}
// `chmod` follows a link, as `setPermissions` means: the permissions of what the path names.
static inline int32_t kama_set_permissions(const char* path, uint32_t mode)
{
    struct stat st;
    if (chmod(path, (mode_t)mode) != 0 || stat(path, &st) != 0) return -1;
    if (((uint32_t)st.st_mode & 0777u) != mode) { errno = EPERM; return -1; }
    return 0;
}

// Is this path a symlink, WITHOUT following it? `lstat`, not `stat`, and the distinction is the whole
// point: a recursive delete that follows a link empties a directory somewhere else entirely. That is
// CVE-2022-21658 in Rust's `remove_dir_all` and the same bug in Go, Python and npm before it.
static inline int32_t kama_is_symlink(const char* path) {
    struct stat st; if (lstat(path, &st) != 0) return 0;
    return S_ISLNK(st.st_mode) ? 1 : 0;
}
static inline int32_t kama_rename(const char* from, const char* to) { return (int32_t)rename(from, to); }
static inline int32_t kama_exists(const char* path) {
    struct stat st; return stat(path, &st) == 0 ? 1 : 0;
}

// Directory iteration: `DIR*` stays opaque (void*); each entry's `d_name` (inline char[]) is copied into
// a kama string via the runtime's kama_string_from_raw. An empty string signals end-of-directory (kama
// filters "." / ".." itself). NULL dirp = open failed (errno set).
static inline void*       kama_diropen(const char* path)  { return (void*)opendir(path); }
static inline int32_t     kama_dirclose(void* dirp)       { return (int32_t)closedir((DIR*)dirp); }
static inline kama_string kama_dirnext(void* dirp) {
    struct dirent* e = readdir((DIR*)dirp);
    if (!e) { kama_string r; r.kama_data = NULL; r.kama_len = 0; r.kama_cap = 0; return r; }
    return kama_string_from_raw((const uint8_t*)e->d_name, 0, (int32_t)strlen(e->d_name));
}

// ---- process (std::process; POSIX fork/exec) -------------------------------
// A child's argv/envp are built HERE as copied `char*[]` (kama__sized_strdup) — owned independently of the kama `string` RAII,
// so they survive across fork even after the parent frees the source strings. kama holds only the opaque
// vector `UnsafePtr`, `int32` fds/pid, and the `int` wait-status folded to scalar accessors (like `struct stat`).
static inline void* kama_argv_new(int32_t n) {
    return kama__sized_zeroed(((size_t)n + 1) * sizeof(char*));   // n slots + NULL terminator, zeroed
}
static inline void kama_argv_set(void* v, int32_t i, const char* s) {
    ((char**)v)[i] = kama__sized_strdup(s ? s : "");       // own a copy (survives kama-string drop across fork)
}
static inline void kama_argv_free(void* v) {
    if (!v) return;
    for (char** p = (char**)v; *p; ++p) kama__sized_free(*p);
    kama__sized_free(v);
}
// Build the child's envp: start from the parent `environ` (unless `clear`), then apply each "KEY=VALUE"
// override — REPLACING an inherited entry with the same KEY (getenv semantics: a plain append wouldn't
// override, since libc returns the first match), else appending. `overrides` is a NULL-terminated char*[]
// of "KEY=VALUE". Returns a fresh char*[] of copies (free with kama_argv_free). Done in the PARENT (allocating
// is safe there); the child only assigns the result to `environ` then execs.
static inline void* kama_envp_build(void* overridesV, int32_t clear) {
    char** ov = (char**)overridesV;
    int nov = 0; if (ov) while (ov[nov]) nov++;
    int nbase = 0; if (!clear && environ) while (environ[nbase]) nbase++;
    char** out = (char**)kama__sized_zeroed(((size_t)nbase + (size_t)nov + 1) * sizeof(char*));
    int k = 0;
    for (int i = 0; i < nbase; i++) {
        const char* e = environ[i];
        const char* eq = strchr(e, '=');
        size_t klen = eq ? (size_t)(eq - e) : strlen(e);
        int overridden = 0;
        for (int j = 0; j < nov; j++) {
            const char* o = ov[j]; const char* oeq = strchr(o, '=');
            size_t olen = oeq ? (size_t)(oeq - o) : strlen(o);
            if (olen == klen && strncmp(o, e, klen) == 0) { overridden = 1; break; }
        }
        if (!overridden) out[k++] = kama__sized_strdup(e);
    }
    for (int j = 0; j < nov; j++) out[k++] = kama__sized_strdup(ov[j]);
    out[k] = NULL;
    return out;
}
// Create a pipe with BOTH ends marked FD_CLOEXEC. Critical for the fork/exec model: without it the child
// inherits a copy of EVERY parent pipe fd, and in particular a stdin pipe's WRITE end — so `closeStdin()`
// in the parent would never deliver EOF (the child keeps its own inherited write end open) and a child
// like `cat` blocks forever. CLOEXEC makes exec close every inherited end; the dup2'd 0/1/2 survive (dup2
// clears CLOEXEC on the new fd). macOS has no pipe2(), so set it via fcntl after pipe().
static inline int32_t kama_pipe(int32_t* outRd, int32_t* outWr) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    *outRd = (int32_t)fds[0]; *outWr = (int32_t)fds[1];
    return 0;
}
// fork + (optional chdir) + dup2 the three std fds + execvp. Between fork and exec the child touches ONLY
// async-signal-safe calls (chdir/dup2/close/execvp/_exit + a bare `environ =` pointer store) — NEVER malloc
// or setenv — so a custom env is a fully-built envp assigned to `environ` (built in the PARENT, where malloc
// is safe). In a multithreaded (isolate) parent only the forking thread survives in the child, and it holds
// no lock it must kama_release, so this is safe. Each std-fd arg is a real fd to dup2 (a pipe end or /dev/null),
// or -1 to leave the inherited fd untouched. Returns 0 (pid in *outPid) or -1 (fork failed).
// Two out-params carry the child identity: `outHandle` is the wait handle (`isize`/ptrdiff_t — a raw pid on
// POSIX, so handle==pid here; a `(ptrdiff_t)HANDLE` on Windows) and `outPid` is the real OS pid for `.id()`.
// Keeping them separate lets Windows wait/kill on the HANDLE while `.id()` still reports the pid.
static inline int32_t kama_proc_spawn(void* argv, void* envp, const char* cwd,
                                      int32_t inFd, int32_t outFd, int32_t errFd,
                                      ptrdiff_t* outHandle, int32_t* outPid) {
    int pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {                                        // ---- child (async-signal-safe only) ----
        // SIGPIPE at its default, which is what an exec'd program expects: an ignored signal survives exec, so a
        // `head` or `yes` spawned from here would otherwise get EPIPE where it expects to be stopped (KPG-1).
        { extern int kama__sigpipe_owned; if (kama__sigpipe_owned) (void)signal(SIGPIPE, SIG_DFL); }
        if (cwd && cwd[0]) { if (chdir(cwd) != 0) _exit(127); }
        if (inFd  >= 0) { dup2(inFd,  0); if (inFd  > 2) close(inFd);  }
        if (outFd >= 0) { dup2(outFd, 1); if (outFd > 2) close(outFd); }
        if (errFd >= 0) { dup2(errFd, 2); if (errFd > 2) close(errFd); }
        if (envp) environ = (char**)envp;
        execvp(((char**)argv)[0], (char* const*)argv);
        _exit(127);                                        // exec failed (ENOENT/EACCES/...) — 127 convention
    }
    *outHandle = (ptrdiff_t)pid; *outPid = (int32_t)pid;   // ---- parent (POSIX: handle == pid) ----
    return 0;
}
// waitpid folded to scalar; `handle` is the pid on POSIX. Returns >0 (reaped; status in *outStatus),
// 0 (WNOHANG: still running), -1 (err).
static inline int32_t kama_proc_wait(ptrdiff_t handle, int32_t* outStatus, int32_t flags) {
    int st = 0;
    int r = (int)waitpid((int)handle, &st, (int)flags);
    if (r > 0) *outStatus = (int32_t)st;
    return (int32_t)r;
}
static inline int32_t kama_proc_exited(int32_t st)      { return WIFEXITED(st)   ? 1 : 0; }
static inline int32_t kama_proc_exit_code(int32_t st)   { return (int32_t)WEXITSTATUS(st); }
static inline int32_t kama_proc_signaled(int32_t st)    { return WIFSIGNALED(st) ? 1 : 0; }
static inline int32_t kama_proc_term_signal(int32_t st) { return (int32_t)WTERMSIG(st); }
static inline int32_t kama_kill(ptrdiff_t handle, int32_t sig) { return (int32_t)kill((int)handle, (int)sig); }
// ---- detached-child reaper -------------------------------------------------
// POSIX has NO "detach" for a child. A Process dropped before its child exits leaves a ZOMBIE in this
// process's table until the PARENT exits — reparenting to init happens on parent death, not on handle
// drop. So dropping N not-yet-exited children pins N process-table slots, and once the per-user cap is
// hit `fork()` starts failing with EAGAIN (macOS kern.maxprocperuid = 2666, so a few thousand detaches
// is enough to wedge a program; Linux's default cap is high enough to hide it).
//
// So a drop that can't reap immediately PARKS the pid here, and every later drop sweeps the park with
// WNOHANG. That keeps the destructor non-blocking (the whole point of detach-on-drop) while bounding
// zombies to children that are genuinely still running. Only pids their owner has already given up on
// are ever parked, so a Process the user can still wait() on can never have its status stolen.
//
// One definition, in the entry TU (kama.cemit.cpp emits it): the accessors are `static inline` and get
// inlined into every TU, so a per-TU `static` would give each TU its own park and defeat the sweep —
// the same hazard as the panic hook / log sink / argv slots.
extern int*   kama_reap_pids;
extern size_t kama_reap_len;
extern size_t kama_reap_cap;
extern int    kama_reap_lock;   // test-and-set spinlock: two isolates can drop a Process concurrently

static inline int kama_reap_keep(int r) {                  // still ours to reap later?
    return r == 0 || (r < 0 && errno == EINTR);            // 0 = running; EINTR = ask again
}
// Sweep the park, compacting out everything reaped (or gone). Caller holds the lock.
static inline void kama_reap_sweep(void) {
    size_t w = 0;
    for (size_t i = 0; i < kama_reap_len; ++i) {
        int st = 0;
        int r = (int)waitpid(kama_reap_pids[i], &st, WNOHANG);
        if (kama_reap_keep(r)) kama_reap_pids[w++] = kama_reap_pids[i];
    }
    kama_reap_len = w;
}
// Non-blocking release on drop: reap the child if it already exited, else park it for a later sweep.
// Never blocks. (The Windows branch above closes the process HANDLE instead — no zombies there.)
static inline void kama_proc_detach(ptrdiff_t handle) {
    while (__atomic_test_and_set(&kama_reap_lock, __ATOMIC_ACQUIRE)) { }
    kama_reap_sweep();
    int st = 0;
    int r = (int)waitpid((int)handle, &st, WNOHANG);
    if (kama_reap_keep(r)) {
        if (kama_reap_len == kama_reap_cap) {
            size_t cap = kama_reap_cap ? kama_reap_cap * 2 : 16;
            int* p = (int*)kama_alloc(cap * sizeof(int), _Alignof(int));
            if (p) {
                if (kama_reap_len) kama_copy(p, kama_reap_pids, kama_reap_len * sizeof(int));
                if (kama_reap_pids) kama_free(kama_reap_pids, kama_reap_cap * sizeof(int), _Alignof(int));
                kama_reap_pids = p; kama_reap_cap = cap;
            }
        }
        if (kama_reap_len < kama_reap_cap) kama_reap_pids[kama_reap_len++] = (int)handle;
        // else: allocation failed — drop the pid rather than fail the destructor. That child stays a
        // zombie until exit (the pre-reaper behaviour), which is the right way to lose this race.
    }
    __atomic_clear(&kama_reap_lock, __ATOMIC_RELEASE);
}
// The platform null device, opened read/write. POSIX = "/dev/null" (Windows branch = "NUL").
static inline int32_t kama_open_null_read(void)  { return (int32_t)open("/dev/null", O_RDONLY); }
static inline int32_t kama_open_null_write(void) { return (int32_t)open("/dev/null", O_WRONLY); }
static inline int32_t kama_WNOHANG(void) { return (int32_t)WNOHANG; }
static inline int32_t kama_SIGKILL(void) { return (int32_t)SIGKILL; }
static inline int32_t kama_SIGTERM(void) { return (int32_t)SIGTERM; }

// Millisecond sleep (used by the cross-platform proc_* test helper; a general convenience too). POSIX uses
// the `poll(NULL, 0, ms)` idiom — no extra include beyond <poll.h>, already pulled in above.
static inline void kama_sleep_ms(int32_t ms) { poll((struct pollfd*)0, 0, (int)ms); }

// Concurrent dual-drain of two pipe read-fds to EOF, each into its own growable funnel buffer. This
// is what `run()` uses to capture a child's stdout+stderr without deadlocking (reading one to EOF then the
// other would block once the child fills the second pipe). POSIX drains via `poll` (works on pipe fds); the
// Windows branch uses two reader threads (`select` there is sockets-only). On success returns 0 and fills
// *outBuf/*outLen/*outCap and *errBuf/*errLen/*errCap — each a heap buffer the caller copies out then frees
// with kama_free(buf, cap, 1) (a NULL buffer with len 0 when a stream produced no bytes). Returns -1 on a hard poll/read error (both
// buffers freed). `poll` ignores a negative fd, so a finished stream is masked by setting its pollfd.fd to -1.
static inline int32_t kama_capture2(int32_t outFd, int32_t errFd,
                                    uint8_t** outBuf, size_t* outLen, size_t* outCap,
                                    uint8_t** errBuf, size_t* errLen, size_t* errCap) {
    int    fds[2]  = { (int)outFd, (int)errFd };
    uint8_t* buf[2] = { NULL, NULL };
    size_t len[2]  = { 0, 0 }, cap[2] = { 0, 0 };
    int    done[2] = { 0, 0 };
    struct pollfd pfd[2];
    while (!done[0] || !done[1]) {
        for (int i = 0; i < 2; i++) {
            pfd[i].fd = done[i] ? -1 : fds[i];      // negative fd => poll ignores this slot
            pfd[i].events = POLLIN; pfd[i].revents = 0;
        }
        int pr = poll(pfd, 2, -1);
        if (pr < 0) { if (errno == EINTR) continue; goto fail; }
        for (int i = 0; i < 2; i++) {
            if (done[i] || !(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (len[i] == cap[i]) {
                size_t ncap = cap[i] ? cap[i] * 2 : 65536;
                uint8_t* nb = (uint8_t*)kama_alloc(ncap, 1);
                if (!nb) goto fail;
                if (len[i]) kama_copy(nb, buf[i], len[i]);
                if (buf[i]) kama_free(buf[i], cap[i], 1);
                buf[i] = nb; cap[i] = ncap;
            }
            ptrdiff_t r = read(fds[i], buf[i] + len[i], cap[i] - len[i]);
            if (r > 0)       { len[i] += (size_t)r; }
            else if (r == 0) { done[i] = 1; }               // EOF: child closed this end
            else { if (errno == EINTR || errno == EAGAIN) continue; goto fail; }
        }
    }
    *outBuf = buf[0]; *outLen = len[0]; *outCap = cap[0];
    *errBuf = buf[1]; *errLen = len[1]; *errCap = cap[1];
    return 0;
fail:
    for (int i = 0; i < 2; i++) if (buf[i]) kama_free(buf[i], cap[i], 1);
    return -1;
}

// ---- TCP sockets -----------------------------------------------------------
// Handles are `ptrdiff_t` (isize). The address calls (bind/connect/sendto/recvfrom/getsockname) are written
// once, below the platform split. `family` is kama's 4 or 6, never an AF_* value, which differ per OS.
static inline int32_t   kama_net_init(void) { return 0; }   // POSIX: nothing to init (Windows: WSAStartup)
// A write to a peer that has closed must return EPIPE, not raise SIGPIPE (KPG-1): MSG_NOSIGNAL on every send, and on
// Apple SO_NOSIGPIPE on every socket kama makes or accepts as well. Per SOCKET, so it holds in a `--shared` library
// too, where the process-wide disposition belongs to the host (see kama__sigpipe_init).
static inline void kama__nosigpipe(ptrdiff_t fd) {
#if defined(SO_NOSIGPIPE)
    if (fd >= 0) { int one = 1; (void)setsockopt((int)fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof one); }
#else
    (void)fd;
#endif
}
#if defined(MSG_NOSIGNAL)
#define KAMA__SEND_FLAGS MSG_NOSIGNAL
#else
#define KAMA__SEND_FLAGS 0
#endif
static inline ptrdiff_t kama_socket_tcp(int32_t family) {
    ptrdiff_t fd = (ptrdiff_t)socket(family == 6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    kama__nosigpipe(fd);
    return fd;
}
static inline int32_t   kama_set_reuseaddr(ptrdiff_t fd) {
    int one = 1; return (int32_t)setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof one);
}
static inline int32_t   kama_listen(ptrdiff_t fd, int32_t backlog) { return (int32_t)listen((int)fd, (int)backlog); }
static inline ptrdiff_t kama_accept(ptrdiff_t fd) {
    ptrdiff_t c = (ptrdiff_t)accept((int)fd, (struct sockaddr*)0, (socklen_t*)0);
    kama__nosigpipe(c);
    return c;
}
static inline ptrdiff_t kama_recv(ptrdiff_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)recv((int)fd, buf, n, 0); }
static inline ptrdiff_t kama_send(ptrdiff_t fd, const uint8_t* buf, size_t n)  { return (ptrdiff_t)send((int)fd, buf, n, KAMA__SEND_FLAGS); }
static inline int32_t   kama_close_socket(ptrdiff_t fd) { return (int32_t)close((int)fd); }

// ---- Unix-domain sockets and who is on the other end ---------------------------------------------------------
// The SIGPIPE rule of the TCP sockets above holds here unchanged: SO_NOSIGPIPE at creation (Apple), MSG_NOSIGNAL
// on every send, and kama_accept already applies it to an accepted socket of any family.
static inline ptrdiff_t kama_socket_unix(int32_t datagram) {
    ptrdiff_t fd = (ptrdiff_t)socket(AF_UNIX, datagram ? SOCK_DGRAM : SOCK_STREAM, 0);
    kama__nosigpipe(fd);
    return fd;
}
// The peer of a connected Unix socket: its process, user and primary group. Linux reports what the kernel
// recorded at connect (SO_PEERCRED); macOS the peer's credentials (LOCAL_PEERCRED, whose first group is the
// effective one) and its pid (LOCAL_PEERPID).
static inline int32_t kama_unix_peer(ptrdiff_t fd, int32_t* pid, uint32_t* uid, uint32_t* gid) {
#if defined(__APPLE__)
    struct xucred xc; socklen_t l = (socklen_t)sizeof xc;
    if (getsockopt((int)fd, SOL_LOCAL, LOCAL_PEERCRED, &xc, &l) != 0) return -1;
    if (xc.cr_version != XUCRED_VERSION || xc.cr_ngroups < 1) { errno = EINVAL; return -1; }
    pid_t p = 0; socklen_t pl = (socklen_t)sizeof p;
    if (getsockopt((int)fd, SOL_LOCAL, LOCAL_PEERPID, &p, &pl) != 0) return -1;
    *pid = (int32_t)p; *uid = (uint32_t)xc.cr_uid; *gid = (uint32_t)xc.cr_groups[0];
    return 0;
#elif defined(SO_PEERCRED)
    // `struct ucred`'s layout, spelled here: glibc declares the name only under _GNU_SOURCE.
    struct { int32_t pid; uint32_t uid; uint32_t gid; } c; socklen_t l = (socklen_t)sizeof c;
    if (getsockopt((int)fd, SOL_SOCKET, SO_PEERCRED, &c, &l) != 0) return -1;
    *pid = c.pid; *uid = c.uid; *gid = c.gid;
    return 0;
#else
    (void)fd; (void)pid; (void)uid; (void)gid; errno = ENOPROTOOPT; return -1;
#endif
}
// Every group the peer belongs to, up to `cap` into out[]. Returns how many there are, which may exceed cap —
// the caller asks again with room. Linux 4.13+ (SO_PEERGROUPS); macOS lists up to 16 (LOCAL_PEERCRED).
static inline ptrdiff_t kama_unix_peer_groups(ptrdiff_t fd, uint32_t* out, size_t cap) {
#if defined(__APPLE__)
    struct xucred xc; socklen_t l = (socklen_t)sizeof xc;
    if (getsockopt((int)fd, SOL_LOCAL, LOCAL_PEERCRED, &xc, &l) != 0) return -1;
    if (xc.cr_version != XUCRED_VERSION) { errno = EINVAL; return -1; }
    for (int i = 0; i < xc.cr_ngroups && (size_t)i < cap; ++i) out[i] = (uint32_t)xc.cr_groups[i];
    return (ptrdiff_t)xc.cr_ngroups;
#elif defined(SO_PEERGROUPS)
    socklen_t l = (socklen_t)(cap * sizeof(uint32_t));
    if (getsockopt((int)fd, SOL_SOCKET, SO_PEERGROUPS, out, &l) != 0) {
        if (errno == ERANGE) return (ptrdiff_t)(l / sizeof(uint32_t));   // l now says how much room it needs
        return -1;
    }
    return (ptrdiff_t)(l / sizeof(uint32_t));
#else
    (void)fd; (void)out; (void)cap; errno = ENOPROTOOPT; return -1;
#endif
}
static inline uint32_t kama_getuid(void) { return (uint32_t)getuid(); }
static inline int32_t  kama_getpid(void) { return (int32_t)getpid(); }

// ---- descriptors (std::io::Descriptor) and passing them to another process (SCM_RIGHTS) -----------------------
// A POSIX descriptor is one small integer whatever it names, so `kind` only matters on Windows.
static inline int32_t kama_desc_close(ptrdiff_t h, int32_t kind) { (void)kind; return (int32_t)close((int)h); }
static inline int32_t kama_desc_is_socket(ptrdiff_t h, int32_t kind) {
    (void)kind; struct stat st;
    return fstat((int)h, &st) == 0 && S_ISSOCK(st.st_mode);
}
static inline int32_t kama_desc_kind_fits(int32_t kind, int32_t socket) { (void)kind; (void)socket; return 1; }
static inline ptrdiff_t kama_desc_dup(ptrdiff_t h, int32_t kind) {   // close-on-exec, like every descriptor kama makes
    (void)kind;
    return (ptrdiff_t)fcntl((int)h, F_DUPFD_CLOEXEC, 0);
}
#define KAMA__FD_MAX 253   // Linux's SCM_MAX_FD — the most one message carries
// Descriptors ride with at least one byte of data (a stream carries ancillary data only alongside some).
static inline ptrdiff_t kama_send_fds(ptrdiff_t sock, const uint8_t* buf, size_t n, const ptrdiff_t* hs, const int32_t* kinds, size_t count) {
    (void)kinds;
    if (count == 0 || count > KAMA__FD_MAX || n == 0) { errno = EINVAL; return -1; }
    struct iovec iov; iov.iov_base = (void*)(uintptr_t)buf; iov.iov_len = n;
    union { struct cmsghdr align; char b[CMSG_SPACE(sizeof(int) * KAMA__FD_MAX)]; } cb;
    memset(&cb, 0, sizeof cb);
    struct msghdr m; memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.msg_iovlen = 1;
    m.msg_control = cb.b; m.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * count);
    struct cmsghdr* c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * count);
    for (size_t i = 0; i < count; ++i) { int fd = (int)hs[i]; memcpy(CMSG_DATA(c) + i * sizeof(int), &fd, sizeof fd); }
    return (ptrdiff_t)sendmsg((int)sock, &m, KAMA__SEND_FLAGS);
}
// Up to n bytes and up to `cap` descriptors. Every received descriptor is close-on-exec (atomically where
// MSG_CMSG_CLOEXEC exists — Linux; set at once otherwise — macOS, where a fork in that instant could inherit
// one). A truncated control message (more descriptors than room, or than the kernel kept) closes every one
// that did arrive and fails with EMSGSIZE: nothing half-received is handed on, and nothing leaks.
static inline ptrdiff_t kama_recv_fds(ptrdiff_t sock, uint8_t* buf, size_t n, ptrdiff_t* hs, int32_t* kinds, size_t cap, size_t* count) {
    *count = 0;
    struct iovec iov; iov.iov_base = buf; iov.iov_len = n;
    union { struct cmsghdr align; char b[CMSG_SPACE(sizeof(int) * KAMA__FD_MAX)]; } cb;
    struct msghdr m; memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = cb.b; m.msg_controllen = (socklen_t)sizeof cb.b;
    int flags = 0;
#if defined(MSG_CMSG_CLOEXEC)
    flags |= MSG_CMSG_CLOEXEC;
#endif
    ptrdiff_t r = (ptrdiff_t)recvmsg((int)sock, &m, flags);
    if (r < 0) return -1;
    size_t got = 0; int over = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t k = ((size_t)c->cmsg_len - (size_t)CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < k; ++i) {
            int fd; memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
#if !defined(MSG_CMSG_CLOEXEC)
            (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
            if (got < cap) { hs[got] = fd; kinds[got] = 0; ++got; } else { (void)close(fd); over = 1; }
        }
    }
    if ((m.msg_flags & MSG_CTRUNC) || over) {
        for (size_t i = 0; i < got; ++i) (void)close((int)hs[i]);
        errno = EMSGSIZE; return -1;
    }
    *count = got;
    return r;
}
static inline uint32_t kama_getgid(void) { return (uint32_t)getgid(); }
// The account name of a uid (`group` 0) or gid (`group` 1), or a uid's home directory (`group` 2): its length,
// with up to `cap` bytes of it copied into out (the caller asks again with room when it is longer), or -1 with
// ENOENT when no account has that id (or, for a home directory, records none).
static inline ptrdiff_t kama_id_name(int32_t group, uint32_t id, uint8_t* out, size_t cap) {
    for (size_t sz = 4096; ; sz *= 4) {                     // through the allocation funnel, like every heap block
        char* buf = (char*)kama_alloc(sz, sizeof(void*));
        if (!buf) { errno = ENOMEM; return -1; }
        const char* name = NULL; int e;
        if (group != 1) { struct passwd pw, *r = NULL; e = getpwuid_r((uid_t)id, &pw, buf, sz, &r);
                          if (!e && r) name = group == 2 ? (pw.pw_dir && *pw.pw_dir ? pw.pw_dir : NULL) : pw.pw_name; }
        else        { struct group  gr, *r = NULL; e = getgrgid_r((gid_t)id, &gr, buf, sz, &r); if (!e && r) name = gr.gr_name; }
        if (e == ERANGE && sz < ((size_t)1 << 22)) { kama_free(buf, sz, sizeof(void*)); continue; }
        if (!name) { kama_free(buf, sz, sizeof(void*)); errno = e ? e : ENOENT; return -1; }
        size_t n = strlen(name);
        memcpy(out, name, n < cap ? n : cap);
        kama_free(buf, sz, sizeof(void*));
        return (ptrdiff_t)n;
    }
}

// ---- readiness poller (poll(2)) --------------------------------------------
// A heap `struct pollfd[]` cursor stays OPAQUE to kama (behind an `UnsafePtr`). interest bits: 1=read, 2=write.
// ready bits: 1=readable (incl. hangup/error so the caller reads EOF/err), 2=writable (a connect resolved).
typedef struct kama__poller { struct pollfd* fds; int kama_len; int kama_cap; } kama__poller;
static inline void* kama_poller_create(void) {
    kama__poller* p = (kama__poller*)kama_alloc(sizeof *p, _Alignof(kama__poller));
    if (!p) { errno = ENOMEM; return NULL; }
    p->fds = NULL; p->kama_len = 0; p->kama_cap = 0; return p;
}
static inline void kama_poller_add(void* ph, ptrdiff_t fd, int32_t interest) {
    kama__poller* p = (kama__poller*)ph;
    short ev = 0;
    if (interest & 1) ev = (short)(ev | POLLIN);
    if (interest & 2) ev = (short)(ev | POLLOUT);
    for (int i = 0; i < p->kama_len; i++) if (p->fds[i].fd == (int)fd) { p->fds[i].events = ev; p->fds[i].revents = 0; return; }
    if (p->kama_len == p->kama_cap) {
        int nc = p->kama_cap ? p->kama_cap * 2 : 8;
        struct pollfd* nf = (struct pollfd*)kama_alloc((size_t)nc * sizeof(struct pollfd), _Alignof(struct pollfd));
        if (!nf) return;
        if (p->kama_len) kama_copy(nf, p->fds, (size_t)p->kama_len * sizeof(struct pollfd));
        if (p->fds) kama_free(p->fds, (size_t)p->kama_cap * sizeof(struct pollfd), _Alignof(struct pollfd));
        p->fds = nf; p->kama_cap = nc;
    }
    p->fds[p->kama_len].fd = (int)fd; p->fds[p->kama_len].events = ev; p->fds[p->kama_len].revents = 0; p->kama_len++;
}
static inline void kama_poller_remove(void* ph, ptrdiff_t fd) {
    kama__poller* p = (kama__poller*)ph;
    for (int i = 0; i < p->kama_len; i++) if (p->fds[i].fd == (int)fd) { p->fds[i] = p->fds[p->kama_len - 1]; p->kama_len--; return; }
}
static inline int32_t kama_poller_wait(void* ph, int32_t timeoutMs) {
    kama__poller* p = (kama__poller*)ph;
    return (int32_t)poll(p->fds, (nfds_t)p->kama_len, (int)timeoutMs);   // -1 errno; 0 timeout; >0 count
}
static inline int32_t kama_poller_ready(void* ph, ptrdiff_t fd) {
    kama__poller* p = (kama__poller*)ph;
    for (int i = 0; i < p->kama_len; i++) if (p->fds[i].fd == (int)fd) {
        int bits = 0;
        if (p->fds[i].revents & (POLLIN | POLLHUP | POLLERR)) bits |= 1;
        if (p->fds[i].revents & POLLOUT) bits |= 2;
        return (int32_t)bits;
    }
    return 0;
}
static inline void kama_poller_free(void* ph) {
    kama__poller* p = (kama__poller*)ph;
    if (!p) return;
    if (p->fds) kama_free(p->fds, (size_t)p->kama_cap * sizeof(struct pollfd), _Alignof(struct pollfd));
    kama_free(p, sizeof *p, _Alignof(kama__poller));
}

// ---- UDP datagrams ---------------------------------------------------------
static inline ptrdiff_t kama_socket_udp(int32_t family) { return (ptrdiff_t)socket(family == 6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0); }

// See the Winsock twin: the native handle, and errno on failure (already set here — a connect's included: POSIX
// itself says EINPROGRESS for one in flight and ENOENT for a Unix path with nothing at it).
typedef int kama__sock;
static inline int32_t kama__sock_fail(void) { return -1; }
static inline int32_t kama__connect_fail(const uint8_t* path, size_t n) { (void)path; (void)n; return -1; }

// ---- socket options ----------------------------------------------------------
static inline int32_t kama_set_nonblocking(ptrdiff_t fd, int32_t on) {
    int fl = fcntl((int)fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (on) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return (int32_t)fcntl((int)fd, F_SETFL, fl);
}
static inline int32_t kama_set_broadcast(ptrdiff_t fd, int32_t on) {
    int v = on ? 1 : 0; return (int32_t)setsockopt((int)fd, SOL_SOCKET, SO_BROADCAST, &v, (socklen_t)sizeof v);
}
static inline int32_t kama_set_nodelay(ptrdiff_t fd, int32_t on) {
    int v = on ? 1 : 0; return (int32_t)setsockopt((int)fd, IPPROTO_TCP, TCP_NODELAY, &v, (socklen_t)sizeof v);
}
// Resolve a non-blocking connect after the socket reports writable: 0 = connected; -1 with errno set to the
// pending/failed cause (SO_ERROR). Used by TcpStream.checkConnected().
static inline int32_t kama_socket_error(ptrdiff_t fd) {
    int soerr = 0; socklen_t l = (socklen_t)sizeof soerr;
    if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &soerr, &l) != 0) return -1;   // errno already set
    if (soerr != 0) { errno = soerr; return -1; }
    return 0;
}

#endif  // !_WIN32

// ---- socket addresses (both platforms) ---------------------------------------
// Written once: `sockaddr_in`/`sockaddr_in6`/`sockaddr_storage` and the five calls that take or return one
// are the same BSD interface on Winsock and on POSIX, and the platform blocks above supply the only two
// differences (`kama__sock`, `kama__sock_fail`).
//
// An address crosses as kama holds it: a family (4 or 6), sixteen bytes in NETWORK order (V4 uses the first
// four), a host-order port, and a scope id (the `sin6_scope_id`; ignored for V4). The bytes are copied, never
// byte-swapped, so no side of the seam has an endianness. There is no host TEXT here any more: `inet_addr`
// read `010.0.0.1` as octal, and kama's strict `parseIp` is now the only numeric parser.
static inline socklen_t kama__sa_fill(struct sockaddr_storage* ss, int32_t family, const uint8_t* ip, uint16_t port, uint32_t scope) {
    memset(ss, 0, sizeof *ss);
    if (family == 6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*)(void*)ss;
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(port);
        memcpy(&a->sin6_addr, ip, 16);
        a->sin6_scope_id = scope;
        return (socklen_t)sizeof *a;
    }
    struct sockaddr_in* a = (struct sockaddr_in*)(void*)ss;
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    memcpy(&a->sin_addr, ip, 4);
    return (socklen_t)sizeof *a;
}
// The inverse. A family that is neither (never, for an inet socket) reads as family 0.
static inline void kama__sa_read(const struct sockaddr_storage* ss, int32_t* family, uint8_t* ip, uint16_t* port, uint32_t* scope) {
    memset(ip, 0, 16);
    *family = 0; *port = 0; *scope = 0;
    if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)(const void*)ss;
        *family = 6; memcpy(ip, &a->sin6_addr, 16); *port = ntohs(a->sin6_port); *scope = a->sin6_scope_id;
    } else if (ss->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)(const void*)ss;
        *family = 4; memcpy(ip, &a->sin_addr, 4); *port = ntohs(a->sin_port);
    }
}
// ⚠️ A V6 bind turns IPV6_V6ONLY OFF, so one socket on `::` serves both families (a v4 peer shows up as
// `::ffff:a.b.c.d`). Linux and macOS default it off, Windows ON, so without this the same program listened
// dual-stack on two platforms and V6-only on the third. Best effort: a stack without the option still binds.
static inline int32_t kama_bind_addr(ptrdiff_t fd, int32_t family, const uint8_t* ip, uint16_t port, uint32_t scope) {
    struct sockaddr_storage ss;
    socklen_t len = kama__sa_fill(&ss, family, ip, port, scope);
    if (family == 6) {
        int off = 0;
        (void)setsockopt((kama__sock)fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, (socklen_t)sizeof off);
    }
    if (bind((kama__sock)fd, (struct sockaddr*)&ss, len) != 0) return kama__sock_fail();
    return 0;
}
static inline int32_t kama_connect_addr(ptrdiff_t fd, int32_t family, const uint8_t* ip, uint16_t port, uint32_t scope) {
    struct sockaddr_storage ss;
    socklen_t len = kama__sa_fill(&ss, family, ip, port, scope);
    if (connect((kama__sock)fd, (struct sockaddr*)&ss, len) != 0) return kama__connect_fail(NULL, 0);
    return 0;
}
static inline ptrdiff_t kama_sendto_addr(ptrdiff_t fd, const uint8_t* buf, size_t n, int32_t family, const uint8_t* ip, uint16_t port, uint32_t scope) {
    struct sockaddr_storage ss;
    socklen_t len = kama__sa_fill(&ss, family, ip, port, scope);
    ptrdiff_t r = (ptrdiff_t)sendto((kama__sock)fd, (const char*)buf, (int)n, 0, (struct sockaddr*)&ss, len);
    if (r < 0) return kama__sock_fail();
    return r;
}
// The sender comes back through scalar out-params (never a struct), as kama_fstat_size folds `struct stat`.
static inline ptrdiff_t kama_recvfrom_addr(ptrdiff_t fd, uint8_t* buf, size_t n, int32_t* outFamily, uint8_t* outIp, uint16_t* outPort, uint32_t* outScope) {
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    socklen_t len = (socklen_t)sizeof ss;
    ptrdiff_t r = (ptrdiff_t)recvfrom((kama__sock)fd, (char*)buf, (int)n, 0, (struct sockaddr*)&ss, &len);
    if (r < 0) return kama__sock_fail();
    kama__sa_read(&ss, outFamily, outIp, outPort, outScope);
    return r;
}
static inline int32_t kama_getsockname_addr(ptrdiff_t fd, int32_t* outFamily, uint8_t* outIp, uint16_t* outPort, uint32_t* outScope) {
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    socklen_t len = (socklen_t)sizeof ss;
    if (getsockname((kama__sock)fd, (struct sockaddr*)&ss, &len) != 0) return kama__sock_fail();
    kama__sa_read(&ss, outFamily, outIp, outPort, outScope);
    return 0;
}
// The peer's address — the inverse of connect, as getsockname is of bind. An accepted stream is the one
// way a server learns who called; `TcpStream.peerAddr()` is its only reader.
static inline int32_t kama_getpeername_addr(ptrdiff_t fd, int32_t* outFamily, uint8_t* outIp, uint16_t* outPort, uint32_t* outScope) {
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    socklen_t len = (socklen_t)sizeof ss;
    if (getpeername((kama__sock)fd, (struct sockaddr*)&ss, &len) != 0) return kama__sock_fail();
    kama__sa_read(&ss, outFamily, outIp, outPort, outScope);
    return 0;
}

// ---- Unix-domain socket addresses (both platforms) ---------------------------------------------------------
// A name crosses as bytes, a length and its kind — 1 a path, 2 a Linux abstract-namespace name (the kama type
// that makes one exists only there), the same numbers kama_unix_name reports. kama has already refused an
// empty path, one holding a NUL, and one longer than kama_unix_path_max, so the fill never truncates. An
// abstract name has a NUL at sun_path[0] and follows it unterminated.
static inline int32_t kama_unix_path_max(void) { struct sockaddr_un a; return (int32_t)sizeof a.sun_path - 1; }
static inline socklen_t kama__sun_fill(struct sockaddr_un* a, const uint8_t* p, size_t n, int32_t kind) {
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path + (kind == 2 ? 1 : 0), p, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);   // a path's NUL, or abstract's lead
#if defined(__APPLE__)
    a->sun_len = (unsigned char)len;
#endif
    return len;
}
static inline int32_t kama_bind_unix(ptrdiff_t fd, const uint8_t* p, size_t n, int32_t kind) {
    struct sockaddr_un a; socklen_t len = kama__sun_fill(&a, p, n, kind);
    if (bind((kama__sock)fd, (struct sockaddr*)&a, len) != 0) return kama__sock_fail();
    return 0;
}
static inline int32_t kama_connect_unix(ptrdiff_t fd, const uint8_t* p, size_t n, int32_t kind) {
    struct sockaddr_un a; socklen_t len = kama__sun_fill(&a, p, n, kind);
    if (connect((kama__sock)fd, (struct sockaddr*)&a, len) != 0) return kama__connect_fail(kind == 1 ? p : NULL, n);
    return 0;
}
// The inverse, for what getsockname/getpeername/recvfrom hand back: *kind is 0 for an unnamed socket, 1 for a
// path, 2 for an abstract name; the name's bytes (no terminator) go into out[cap]; returns their length.
static inline ptrdiff_t kama__sun_read(const struct sockaddr_un* a, socklen_t len, int32_t* kind, uint8_t* out, size_t cap) {
    size_t base = offsetof(struct sockaddr_un, sun_path);
    size_t room = len > (socklen_t)base ? (size_t)len - base : 0;
    if (room > sizeof a->sun_path) room = sizeof a->sun_path;
    size_t n = 0, from = 0;
    *kind = 0;
    if (room > 0 && a->sun_path[0] != 0) {
        *kind = 1;
        while (n < room && a->sun_path[n] != 0) ++n;
    }
#if defined(__linux__)
    // Only Linux has the abstract namespace. Elsewhere a leading NUL is an UNNAMED socket: macOS reports an
    // unbound client as a whole zeroed sun_path, which read as an "abstract name" of 103 NULs.
    else if (room > 1) { *kind = 2; from = 1; n = room - 1; }
#endif
    memcpy(out, a->sun_path + from, n < cap ? n : cap);
    return (ptrdiff_t)n;
}
static inline ptrdiff_t kama_unix_name(ptrdiff_t fd, int32_t peer, int32_t* kind, uint8_t* out, size_t cap) {
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    socklen_t len = (socklen_t)sizeof a;
    int rc = peer ? getpeername((kama__sock)fd, (struct sockaddr*)&a, &len)
                  : getsockname((kama__sock)fd, (struct sockaddr*)&a, &len);
    if (rc != 0) return kama__sock_fail();
    return kama__sun_read(&a, len, kind, out, cap);
}

// One datagram to a named Unix socket, and one received with its sender's name (UnixDatagram — Linux, macOS).
static inline ptrdiff_t kama_sendto_unix(ptrdiff_t fd, const uint8_t* buf, size_t n, const uint8_t* p, size_t pn, int32_t kind) {
    struct sockaddr_un a; socklen_t len = kama__sun_fill(&a, p, pn, kind);
    ptrdiff_t r = (ptrdiff_t)sendto((kama__sock)fd, (const char*)buf, (int)n, 0, (struct sockaddr*)&a, len);
    if (r < 0) return kama__sock_fail();
    return r;
}
static inline ptrdiff_t kama_recvfrom_unix(ptrdiff_t fd, uint8_t* buf, size_t n, int32_t* kind, uint8_t* name, size_t cap,
                                           ptrdiff_t* nameLen) {
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    socklen_t len = (socklen_t)sizeof a;
    ptrdiff_t r = (ptrdiff_t)recvfrom((kama__sock)fd, (char*)buf, (int)n, 0, (struct sockaddr*)&a, &len);
    if (r < 0) return kama__sock_fail();
    *nameLen = kama__sun_read(&a, len, kind, name, cap);
    return r;
}

// ---- TCP keepalive, user timeout, half-close (KR-105) -------------------------------------------------
// How a client notices a peer that vanished without a FIN — a NAT that forgot the flow, a failed-over
// primary. Each knob is ONE option on every stack, and the three keepalive knobs take whole seconds on all
// of them (Linux `TCP_KEEPIDLE`; macOS spells the idle time `TCP_KEEPALIVE`; Windows 10 1709+ has all three
// under the Linux names, and `TCP_KEEPIDLE` IS its `TCP_KEEPALIVE`). The caller has already refused a
// non-positive value; a stack that lacks a knob says ENOPROTOOPT rather than silently doing nothing.
static inline int32_t kama__sockopt_int(ptrdiff_t fd, int level, int opt, int v) {
    if (setsockopt((kama__sock)fd, level, opt, (const char*)&v, (socklen_t)sizeof v) != 0) return kama__sock_fail();
    return 0;
}
static inline int32_t kama__no_sockopt(void) { errno = ENOPROTOOPT; return -1; }
static inline int32_t kama_set_keepalive(ptrdiff_t fd, int32_t on) {
    return kama__sockopt_int(fd, SOL_SOCKET, SO_KEEPALIVE, on ? 1 : 0);
}
static inline int32_t kama_set_keepalive_idle(ptrdiff_t fd, int32_t secs) {
#if defined(TCP_KEEPIDLE)
    return kama__sockopt_int(fd, IPPROTO_TCP, TCP_KEEPIDLE, secs);
#elif defined(TCP_KEEPALIVE)
    return kama__sockopt_int(fd, IPPROTO_TCP, TCP_KEEPALIVE, secs);
#else
    (void)fd; (void)secs; return kama__no_sockopt();
#endif
}
static inline int32_t kama_set_keepalive_interval(ptrdiff_t fd, int32_t secs) {
#if defined(TCP_KEEPINTVL)
    return kama__sockopt_int(fd, IPPROTO_TCP, TCP_KEEPINTVL, secs);
#else
    (void)fd; (void)secs; return kama__no_sockopt();
#endif
}
static inline int32_t kama_set_keepalive_count(ptrdiff_t fd, int32_t count) {
#if defined(TCP_KEEPCNT)
    return kama__sockopt_int(fd, IPPROTO_TCP, TCP_KEEPCNT, count);
#else
    (void)fd; (void)count; return kama__no_sockopt();
#endif
}
// How long written data may go unacknowledged before the connection is dropped. Linux measures it in
// milliseconds (`TCP_USER_TIMEOUT`); macOS (`TCP_RXT_CONNDROPTIME`) and Windows (`TCP_MAXRT`) in seconds,
// so the millisecond count is rounded UP there — a timeout never fires earlier than asked. The three stacks
// differ at the edge (Linux also bounds the keepalive probes by it), which SPEC states.
static inline int32_t kama_set_user_timeout(ptrdiff_t fd, int32_t millis) {
#if defined(TCP_USER_TIMEOUT)
    return kama__sockopt_int(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, millis);
#elif defined(TCP_RXT_CONNDROPTIME)
    return kama__sockopt_int(fd, IPPROTO_TCP, TCP_RXT_CONNDROPTIME, (int)((millis + 999) / 1000));
#elif defined(TCP_MAXRT)
    return kama__sockopt_int(fd, IPPROTO_TCP, TCP_MAXRT, (int)((millis + 999) / 1000));
#else
    (void)fd; (void)millis; return kama__no_sockopt();
#endif
}
// A half-close: 0 stops reading, 1 sends a FIN (the peer reads EOF; a later write here is BrokenPipe),
// 2 both. Winsock spells the three SD_*, which are the same three numbers.
static inline int32_t kama_shutdown(ptrdiff_t fd, int32_t how) {
#if defined(_WIN32)
    int h = how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH;
#else
    int h = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR;
#endif
    if (shutdown((kama__sock)fd, h) != 0) return kama__sock_fail();
    return 0;
}

// The family a socket was created with — the per-family options below have a V4 and a V6 spelling, and the
// socket is the only thing that knows which applies. 0 when it cannot say.
static inline int32_t kama__sock_family(ptrdiff_t fd) {
    struct sockaddr_storage ss; memset(&ss, 0, sizeof ss);
    socklen_t len = (socklen_t)sizeof ss;
    if (getsockname((kama__sock)fd, (struct sockaddr*)&ss, &len) != 0) return 0;
    return ss.ss_family == AF_INET6 ? 6 : 4;
}
// The unicast hop limit: IP_TTL on a V4 socket, IPV6_UNICAST_HOPS on a V6 one (where IP_TTL is refused).
// Both take a 4-byte integer, a DWORD on Winsock.
static inline int32_t kama_set_ttl(ptrdiff_t fd, uint32_t ttl) {
    int v = (int)ttl;
    int rc = kama__sock_family(fd) == 6
        ? setsockopt((kama__sock)fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, (const char*)&v, (socklen_t)sizeof v)
        : setsockopt((kama__sock)fd, IPPROTO_IP, IP_TTL, (const char*)&v, (socklen_t)sizeof v);
    if (rc != 0) return kama__sock_fail();
    return 0;
}

// ---- multicast -----------------------------------------------------------------
// Membership is per interface, and the two families name one differently, which is why kama has a V4 and a V6
// spelling rather than one that hides a lookup: IP_ADD_MEMBERSHIP takes the interface's ADDRESS (0.0.0.0 =
// the OS's choice), IPV6_JOIN_GROUP its INDEX (0 = the OS's choice). `join` is 1 to join, 0 to leave. The
// group and interface bytes are network order, as every address crossing here is.
#if defined(__EMSCRIPTEN__)
// emscripten's <netinet/in.h> declares no `struct ip_mreq` (everything else here it has — measured), and a wasm
// program has no raw sockets to join a group on in any case: std::net's native half is not a wasm feature.
static inline int32_t kama_multicast_v4(ptrdiff_t fd, const uint8_t* group, const uint8_t* iface, int32_t join) {
    (void)fd; (void)group; (void)iface; (void)join;
    errno = ENOPROTOOPT; return -1;
}
#else
static inline int32_t kama_multicast_v4(ptrdiff_t fd, const uint8_t* group, const uint8_t* iface, int32_t join) {
    struct ip_mreq m;
    memset(&m, 0, sizeof m);
    memcpy(&m.imr_multiaddr, group, 4);
    memcpy(&m.imr_interface, iface, 4);
    if (setsockopt((kama__sock)fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                   (const char*)&m, (socklen_t)sizeof m) != 0) return kama__sock_fail();
    return 0;
}
#endif
// Linux spells the V6 pair IPV6_ADD/DROP_MEMBERSHIP (and aliases JOIN/LEAVE); macOS and Winsock spell JOIN/LEAVE.
#if !defined(IPV6_JOIN_GROUP)
#  define IPV6_JOIN_GROUP  IPV6_ADD_MEMBERSHIP
#  define IPV6_LEAVE_GROUP IPV6_DROP_MEMBERSHIP
#endif
static inline int32_t kama_multicast_v6(ptrdiff_t fd, const uint8_t* group, uint32_t index, int32_t join) {
    struct ipv6_mreq m;
    memset(&m, 0, sizeof m);
    memcpy(&m.ipv6mr_multiaddr, group, 16);
    m.ipv6mr_interface = index;
    if (setsockopt((kama__sock)fd, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                   (const char*)&m, (socklen_t)sizeof m) != 0) return kama__sock_fail();
    return 0;
}
// The outgoing interface for this socket's multicast sends. Without it the group routes by the table, so a host
// with a default route sends a loopback-bound socket's datagram out a real interface and fails (measured on
// macOS: EADDRNOTAVAIL), and a multi-homed host picks for the caller.
static inline int32_t kama_multicast_if_v4(ptrdiff_t fd, const uint8_t* iface) {
    struct in_addr a;
    memcpy(&a, iface, 4);
    if (setsockopt((kama__sock)fd, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&a, (socklen_t)sizeof a) != 0)
        return kama__sock_fail();
    return 0;
}
static inline int32_t kama_multicast_if_v6(ptrdiff_t fd, uint32_t index) {
    unsigned int v = (unsigned int)index;
    if (setsockopt((kama__sock)fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char*)&v, (socklen_t)sizeof v) != 0)
        return kama__sock_fail();
    return 0;
}
// Loopback and hop limit of multicast sends, by the socket's family. The V6 options take an int everywhere. The
// V4 ones are the historical `u_char`: macOS and Linux accept a byte or an int (measured), OpenBSD accepts only
// the byte, and Winsock documents a DWORD — so a byte on POSIX and a DWORD on Windows.
static inline int32_t kama_multicast_opt(ptrdiff_t fd, int32_t hops, uint32_t value) {
    int rc;
    if (kama__sock_family(fd) == 6) {
        int v = (int)value;
        rc = setsockopt((kama__sock)fd, IPPROTO_IPV6, hops ? IPV6_MULTICAST_HOPS : IPV6_MULTICAST_LOOP,
                        (const char*)&v, (socklen_t)sizeof v);
    } else {
#if defined(_WIN32)
        DWORD v = (DWORD)value;
#else
        unsigned char v = (unsigned char)value;
#endif
        rc = setsockopt((kama__sock)fd, IPPROTO_IP, hops ? IP_MULTICAST_TTL : IP_MULTICAST_LOOP,
                        (const char*)&v, (socklen_t)sizeof v);
    }
    if (rc != 0) return kama__sock_fail();
    return 0;
}
// An interface's index from its name (`lo0`, `eth0`; on Windows the NDIS name, e.g. `ethernet_32769`, not the
// friendly "Ethernet"). 0 with errno ENOENT when there is no such interface — 0 is never a real index.
static inline uint32_t kama_interface_index(const char* name) {
    unsigned int i = (name && *name) ? if_nametoindex(name) : 0u;
    if (i == 0) kama__os_fail(0, ENOENT);
    return (uint32_t)i;
}

// ---- name resolution (DNS) -------------------------------------------------
// ⚠️ The ONE seam in this file that is written once instead of twice, and the exception is deliberate:
// `getaddrinfo` is the same standardized call with the same semantics on Winsock and on POSIX (that is why
// it replaced `gethostbyname` everywhere), and the two branches above have already pulled the header that
// declares it — <ws2tcpip.h> and <netdb.h>. Copying an identical body would only give it somewhere to
// drift. Everything else here differs per platform, which is why everything else is duplicated.
//
// Resolves a host NAME to its addresses of BOTH families, each as the socket-address calls above take one:
// `outFamily[i]` (4 or 6), sixteen bytes at `outIps + 16*i`, `outScope[i]`. They come back in the RESOLVER's
// order, which is the answer to a question the caller did not ask twice: the system resolver already applies
// RFC 6724 destination-address selection (which is why `localhost` is `::1` FIRST on macOS), and re-sorting
// or filtering them here would throw that away. Returns the count written (1..max), or -1 with errno set.
//
// It BLOCKS, possibly for seconds, and there is no portable timeout — `getaddrinfo` takes none, and the
// asynchronous spellings (getaddrinfo_a, GetAddrInfoEx) share no interface. A program that cannot afford
// the stall resolves on another isolate.
static inline int32_t kama__eai_errno(int rc) {
    // EAI_* codes are their own space, so map them onto the errno set `lastError()` classifies. "No such
    // name" is NotFound (the name does not exist — the same shape as a missing file), a temporary resolver
    // failure is TimedOut (a retry may work; WouldBlock would be a lie about a blocking call), and anything
    // else is HostUnreachable rather than an `Other` carrying a code from the wrong number space.
    if (rc == EAI_NONAME) return (int32_t)ENOENT;
#if defined(EAI_NODATA)
    if (rc == EAI_NODATA) return (int32_t)ENOENT;
#endif
    if (rc == EAI_AGAIN)  return (int32_t)ETIMEDOUT;
    if (rc == EAI_MEMORY) return (int32_t)ENOMEM;
#if defined(EAI_SYSTEM)
    if (rc == EAI_SYSTEM) return (int32_t)errno;   // getaddrinfo already set it
#endif
    return (int32_t)EHOSTUNREACH;
}
static inline int32_t kama_resolve_host(const char* host, int32_t* outFamily, uint8_t* outIps, uint32_t* outScope, int32_t max) {
    struct addrinfo hints;
    struct addrinfo* res = (struct addrinfo*)0;
    struct addrinfo* it;
    int32_t n = 0;
    int rc;
    if (!host || !*host || !outFamily || !outIps || !outScope || max <= 0) { kama__os_fail(0, ENOENT); return -1; }
    // ⚠️ `getaddrinfo` is a WINSOCK call, so it needs WSAStartup like every socket here does — and this
    // is the one entry point that reaches it without creating a socket first. Without this, a resolve
    // performed before the program's first `bind`/`connect` failed with WSANOTINITIALISED (carried out
    // as HostUnreachable), while the same call AFTER one succeeded: `tests/net_resolve` scored 111 of
    // 127 on Windows, missing exactly the `resolve("localhost")` that ran before its listener bound.
    // No-op on POSIX, where kama_net_init() returns 0.
    if (kama_net_init() != 0) return -1;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;   // one entry per address, not one per (address, socket type)
    rc = getaddrinfo(host, (const char*)0, &hints, &res);
    if (rc != 0) { errno = kama__eai_errno(rc); return -1; }
    for (it = res; it && n < max; it = it->ai_next) {
        struct sockaddr_storage ss;
        uint16_t port;
        if (!it->ai_addr || (it->ai_family != AF_INET && it->ai_family != AF_INET6)) continue;
        if ((size_t)it->ai_addrlen > sizeof ss) continue;
        memset(&ss, 0, sizeof ss);
        memcpy(&ss, it->ai_addr, (size_t)it->ai_addrlen);
        kama__sa_read(&ss, &outFamily[n], outIps + 16 * n, &port, &outScope[n]);
        n++;
    }
    freeaddrinfo(res);
    // A success that yielded nothing is a failure to the caller, not an empty list: every answer was of a
    // family no socket here speaks.
    if (n == 0) { errno = EHOSTUNREACH; return -1; }
    return n;
}

#endif  // KAMA_OS_H
