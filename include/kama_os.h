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
static inline int32_t kama_ENOENT(void)       { return (int32_t)ENOENT; }
static inline int32_t kama_EACCES(void)       { return (int32_t)EACCES; }
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

static inline void kama__capture_wsa(void) {
    int e = WSAGetLastError();
    switch (e) {
        case WSAEWOULDBLOCK:  errno = EAGAIN;        break;
        case WSAECONNREFUSED: errno = ECONNREFUSED;  break;
        case WSAECONNRESET:   errno = ECONNRESET;    break;
        case WSAEADDRINUSE:   errno = EADDRINUSE;    break;
        case WSAEINTR:        errno = EINTR;         break;
        case WSAEACCES:       errno = EACCES;        break;
        case WSAEINPROGRESS:  errno = EINPROGRESS;   break;
        case WSAEALREADY:     errno = EINPROGRESS;   break;   // non-blocking connect already in flight
        case WSAETIMEDOUT:    errno = ETIMEDOUT;     break;
        case WSAEHOSTUNREACH: errno = EHOSTUNREACH;  break;
        case WSAENETUNREACH:  errno = ENETUNREACH;   break;
        case WSAEMSGSIZE:     errno = EMSGSIZE;      break;
        case WSAEINVAL:       errno = EINVAL;        break;
        default:              errno = e;             break;   // carried through as IoError::Other(code)
    }
}

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
    if (n <= 0) { errno = EINVAL; return NULL; }
    wchar_t* w = (wchar_t*)kama__sized_alloc((size_t)n * sizeof(wchar_t));   // released by kama__wfree
    if (!w) { errno = ENOMEM; return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, w, n);
    return w;
}
// The read direction, as a fresh UTF-8 string from `kama_alloc(n, 1)` — `n` is `*outLen + 1` — so it may become a
// kama_string's buffer directly (cap = n). ⚠️ NTFS permits a lone surrogate in a name; it comes back as
// U+FFFD and cannot be re-opened — the same limit Rust's `to_str()` has, and not worth an OsString.
static inline char* kama__utf8(const wchar_t* w, size_t* outLen) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);   // includes the NUL
    if (n <= 0) { errno = EINVAL; return NULL; }
    char* s = (char*)kama_alloc((size_t)n, 1);
    if (!s) { errno = ENOMEM; return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    if (outLen) *outLen = (size_t)n - 1;
    return s;
}
// A PATH, long-path aware. Past 248 characters (MAX_PATH minus the 12 CreateDirectoryW reserves for an 8.3
// name — the threshold Rust and Go use) the path is made absolute and normalized by GetFullPathNameW
// (`/` -> `\`, `..` collapsed — which Win32 already does lexically, so nothing changes meaning) and given
// the `\\?\` prefix (`\\?\UNC\` for a share). Every W call here honours that prefix with LongPathsEnabled=0
// (probed: docs/platforms/windows.md), so no registry setting is asked of the user. A shorter path passes
// through untouched, so the common case is exactly what it was.
static inline wchar_t* kama__wpath(const char* utf8) {
    wchar_t* w = kama__wide(utf8, -1);
    if (!w) return NULL;
    if (wcslen(w) < 248 || (w[0] == L'\\' && w[1] == L'\\' && w[2] == L'?' && w[3] == L'\\')) return w;
    DWORD full = GetFullPathNameW(w, 0, NULL, NULL);                        // required size, incl. NUL
    wchar_t* v = full ? (wchar_t*)kama__sized_alloc(((size_t)full + 8) * sizeof(wchar_t)) : NULL;   // + `\\?\UNC\`
    if (!v) { kama__sized_free(w); errno = full ? ENOMEM : ENOENT; return NULL; }
    DWORD got = GetFullPathNameW(w, full, v + 4, NULL);                     // excludes the NUL on success
    kama__sized_free(w);
    if (got == 0 || got >= full) { kama__sized_free(v); errno = ENOENT; return NULL; }
    if (v[4] == L'\\' && v[5] == L'\\') {                                    // \\srv\share\x -> \\?\UNC\srv\share\x
        memmove(v + 8, v + 6, ((size_t)got - 2 + 1) * sizeof(wchar_t));
        memcpy(v, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
    } else {
        memcpy(v, L"\\\\?\\", 4 * sizeof(wchar_t));
    }
    return v;
}
// Releasing may clobber errno; the wrappers below free their wide copy AFTER the call whose errno they return.
// Every wide path/string above is a sized block, so this is the one release for all of them.
static inline void kama__wfree(void* p) { int e = errno; kama__sized_free(p); errno = e; }

// ---- files (wide CRT low-level I/O) -----------------------------------------
// _O_BINARY is essential: Windows text mode would translate CRLF/^Z and corrupt binary data. The `_w*` CRT
// family is preferred over raw Win32 for the file calls because it sets `errno` itself, so kama_last_error()
// stays the single error channel with no GetLastError mapping.
static inline int32_t kama__wopen(const char* path, int flags) {
    wchar_t* w = kama__wpath(path); if (!w) return -1;
    int fd = _wopen(w, flags, _S_IREAD | _S_IWRITE); kama__wfree(w); return (int32_t)fd;
}
static inline int32_t   kama_open_read(const char* path)   { return kama__wopen(path, _O_RDONLY | _O_BINARY); }
static inline int32_t   kama_open_create(const char* path) { return kama__wopen(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY); }
static inline int32_t   kama_open_append(const char* path) { return kama__wopen(path, _O_WRONLY | _O_CREAT | _O_APPEND | _O_BINARY); }
static inline ptrdiff_t kama_read(int32_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)_read((int)fd, buf, (unsigned int)n); }
static inline ptrdiff_t kama_write(int32_t fd, const uint8_t* buf, size_t n) { return (ptrdiff_t)_write((int)fd, buf, (unsigned int)n); }
static inline int32_t   kama_close_fd(int32_t fd) { return (int32_t)_close((int)fd); }
static inline int32_t   kama_unlink(const char* path) {
    wchar_t* w = kama__wpath(path); if (!w) return -1;
    int r = _wunlink(w); kama__wfree(w); return (int32_t)r;
}

// `struct stat` stays opaque: one call folds every fact kama's `Metadata` carries into scalar out-params.
// mtime is NANOSECONDS from the UNIX epoch, so it is a `std::time::Timestamp` with no conversion at the
// kama end — `_stat64` carries whole seconds, which is the resolution Windows reports here.
// `readOnly` is the write PERMISSION BIT, not an access check: it says what the file's mode records, and
// says nothing about this process (root ignores it; an ACL can deny a writable-looking file).
static inline int32_t kama_fstat_meta(int32_t fd, uint64_t* outSize, int32_t* outIsDir,
                                      int64_t* outMtimeNs, int32_t* outReadOnly) {
    struct _stat64 st; if (_fstat64((int)fd, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = (st.st_mode & _S_IFDIR) ? 1 : 0;
    *outMtimeNs = (int64_t)st.st_mtime * 1000000000ll;
    *outReadOnly = (st.st_mode & _S_IWRITE) ? 0 : 1;
    return 0;
}
static inline int32_t kama_path_meta(const char* path, uint64_t* outSize, int32_t* outIsDir,
                                     int64_t* outMtimeNs, int32_t* outReadOnly) {
    wchar_t* w = kama__wpath(path); if (!w) return -1;
    struct _stat64 st; int r = _wstat64(w, &st); kama__wfree(w);
    if (r != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = (st.st_mode & _S_IFDIR) ? 1 : 0;
    *outMtimeNs = (int64_t)st.st_mtime * 1000000000ll;
    *outReadOnly = (st.st_mode & _S_IWRITE) ? 0 : 1;
    return 0;
}
// Directory creation, rename and existence. `_wmkdir` takes no mode on Windows; `MoveFileExW` with
// REPLACE_EXISTING is what makes rename overwrite as POSIX's does (plain MoveFileW fails on an existing
// destination, which would have made the same kama call behave differently per platform).
static inline int32_t kama_mkdir(const char* path) {
    wchar_t* w = kama__wpath(path); if (!w) return -1;
    int r = _wmkdir(w); kama__wfree(w); return (int32_t)r;
}
static inline int32_t kama_rmdir(const char* path) {
    wchar_t* w = kama__wpath(path); if (!w) return -1;
    int r = _wrmdir(w); kama__wfree(w); return (int32_t)r;
}
static inline DWORD kama__attrs(const char* path) {
    wchar_t* w = kama__wpath(path); if (!w) return INVALID_FILE_ATTRIBUTES;
    DWORD a = GetFileAttributesW(w); kama__wfree(w); return a;
}
// Is this path a symlink (a reparse point here), WITHOUT following it? The distinction only matters to a
// recursive delete, which must not walk through a link and empty a directory somewhere else.
static inline int32_t kama_is_symlink(const char* path) {
    DWORD a = kama__attrs(path);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
}
static inline int32_t kama_rename(const char* from, const char* to) {
    wchar_t* wf = kama__wpath(from); if (!wf) return -1;
    wchar_t* wt = kama__wpath(to);   if (!wt) { kama__wfree(wf); return -1; }
    BOOL ok = MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    DWORD e = ok ? 0 : GetLastError();
    kama__wfree(wf); kama__wfree(wt);
    if (!ok) { errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES; return -1; }
    return 0;
}
static inline int32_t kama_exists(const char* path) {
    return kama__attrs(path) == INVALID_FILE_ATTRIBUTES ? 0 : 1;
}

// ---- directory iteration (Win32 FindFirstFileW) ----------------------------
// DIR* analogue: a heap cursor holding the search handle + the pending entry (FindFirstFile already
// returns the first match). Empty string = end-of-directory; kama filters "." / ".." itself.
typedef struct kama__dir { HANDLE h; WIN32_FIND_DATAW kama_data; int pending; } kama__dir;
static inline void* kama_diropen(const char* path) {
    size_t n = strlen(path);
    char* pattern = (char*)kama__sized_alloc(n + 3);                         // "<path>\*"
    if (!pattern) { errno = ENOMEM; return NULL; }
    memcpy(pattern, path, n); pattern[n] = '\\'; pattern[n + 1] = '*'; pattern[n + 2] = '\0';
    wchar_t* w = kama__wpath(pattern);                                       // GetFullPathNameW keeps a trailing `*`
    kama__wfree(pattern);
    if (!w) return NULL;
    kama__dir* d = (kama__dir*)kama_alloc(sizeof *d, _Alignof(kama__dir));
    if (!d) { kama__wfree(w); errno = ENOMEM; return NULL; }
    d->h = FindFirstFileW(w, &d->kama_data); kama__wfree(w);
    if (d->h == INVALID_HANDLE_VALUE) { kama_free(d, sizeof *d, _Alignof(kama__dir)); errno = ENOENT; return NULL; }
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

// The two pieces the shared address calls (below the platform split) need from each platform: the native
// handle type, and turning a failed call's error into errno.
typedef SOCKET kama__sock;
static inline int32_t kama__sock_fail(void) { kama__capture_wsa(); return -1; }

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
    if (soerr != 0) {
        switch (soerr) {
            case WSAECONNREFUSED: errno = ECONNREFUSED;  break;
            case WSAECONNRESET:   errno = ECONNRESET;    break;
            case WSAETIMEDOUT:    errno = ETIMEDOUT;     break;
            case WSAEHOSTUNREACH: errno = EHOSTUNREACH;  break;
            case WSAENETUNREACH:  errno = ENETUNREACH;   break;
            default:              errno = soerr;         break;
        }
        return -1;
    }
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
    if (!p) { errno = ENOMEM; return NULL; }
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
    if (!cmdline) { errno = ENOMEM; return -1; }
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
        errno = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES;
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
    if (r != WAIT_OBJECT_0) { errno = EINVAL; return -1; }
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) { errno = EINVAL; return -1; }
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
    if (!TerminateProcess((HANDLE)handle, 1)) { errno = EINVAL; return -1; }
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
    if (!th) { errno = EAGAIN; return -1; }   // nothing drained yet: co.buf is still NULL
    kama__cap_thread(&co);                                 // drain stdout on this thread
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (co.err || ce.err) {
        if (co.buf) kama_free(co.buf, co.kama_cap, 1);
        if (ce.buf) kama_free(ce.buf, ce.kama_cap, 1);
        errno = EIO; return -1;
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
extern char** environ;

// ---- errno / last-error ----------------------------------------------------
// A stable accessor + constant accessors, so kama never hardcodes per-OS errno numbers. (The Windows
// branch will map WSAGetLastError() codes onto these same POSIX values.)
static inline int32_t kama_last_error(void)   { return (int32_t)errno; }
static inline int32_t kama_ENOENT(void)       { return (int32_t)ENOENT; }
static inline int32_t kama_EACCES(void)       { return (int32_t)EACCES; }
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
static inline int32_t   kama_open_read(const char* path)   { return (int32_t)open(path, O_RDONLY); }
static inline int32_t   kama_open_create(const char* path) { return (int32_t)open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
static inline int32_t   kama_open_append(const char* path) { return (int32_t)open(path, O_WRONLY | O_CREAT | O_APPEND, 0644); }
static inline ptrdiff_t kama_read(int32_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)read((int)fd, buf, n); }
static inline ptrdiff_t kama_write(int32_t fd, const uint8_t* buf, size_t n) { return (ptrdiff_t)write((int)fd, buf, n); }
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
// `readOnly` is the owner's write PERMISSION BIT, not an access check for this process (root ignores it,
// and an ACL can deny a file whose mode looks writable) — `access(W_OK)` would answer a different question.
static inline int32_t kama_fstat_meta(int32_t fd, uint64_t* outSize, int32_t* outIsDir,
                                      int64_t* outMtimeNs, int32_t* outReadOnly) {
    struct stat st; if (fstat((int)fd, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = S_ISDIR(st.st_mode) ? 1 : 0;
    *outMtimeNs = (int64_t)st.st_mtime * 1000000000ll + KAMA_ST_MTIME_NSEC(st);
    *outReadOnly = (st.st_mode & S_IWUSR) ? 0 : 1;
    return 0;
}
static inline int32_t kama_path_meta(const char* path, uint64_t* outSize, int32_t* outIsDir,
                                     int64_t* outMtimeNs, int32_t* outReadOnly) {
    struct stat st; if (stat(path, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = S_ISDIR(st.st_mode) ? 1 : 0;
    *outMtimeNs = (int64_t)st.st_mtime * 1000000000ll + KAMA_ST_MTIME_NSEC(st);
    *outReadOnly = (st.st_mode & S_IWUSR) ? 0 : 1;
    return 0;
}
// Directory creation, rename and existence. 0777 is the POSIX default — the process umask narrows it,
// which is the one place a mode belongs. `rename` already replaces an existing destination here; the
// Windows branch has to ask for that explicitly to match.
static inline int32_t kama_mkdir(const char* path) { return (int32_t)mkdir(path, 0777); }
static inline int32_t kama_rmdir(const char* path) { return (int32_t)rmdir(path); }
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
static inline ptrdiff_t kama_socket_tcp(int32_t family) { return (ptrdiff_t)socket(family == 6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0); }
static inline int32_t   kama_set_reuseaddr(ptrdiff_t fd) {
    int one = 1; return (int32_t)setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof one);
}
static inline int32_t   kama_listen(ptrdiff_t fd, int32_t backlog) { return (int32_t)listen((int)fd, (int)backlog); }
static inline ptrdiff_t kama_accept(ptrdiff_t fd)                  { return (ptrdiff_t)accept((int)fd, (struct sockaddr*)0, (socklen_t*)0); }
static inline ptrdiff_t kama_recv(ptrdiff_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)recv((int)fd, buf, n, 0); }
static inline ptrdiff_t kama_send(ptrdiff_t fd, const uint8_t* buf, size_t n)  { return (ptrdiff_t)send((int)fd, buf, n, 0); }
static inline int32_t   kama_close_socket(ptrdiff_t fd) { return (int32_t)close((int)fd); }

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

// See the Winsock twin: the native handle, and errno on failure (already set here).
typedef int kama__sock;
static inline int32_t kama__sock_fail(void) { return -1; }

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
    if (connect((kama__sock)fd, (struct sockaddr*)&ss, len) != 0) return kama__sock_fail();
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
    if (i == 0) errno = ENOENT;
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
    if (!host || !*host || !outFamily || !outIps || !outScope || max <= 0) { errno = ENOENT; return -1; }
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
