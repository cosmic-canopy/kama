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
// Windows branch: TODO (plan Step 4) — Winsock (`WSAStartup`/`SOCKET`/`closesocket`/`WSAGetLastError`) +
// CRT (`_open`/`_stat`/`FindFirstFile`). The POSIX branch below also serves iOS/Android/BSD and (via
// emscripten's POSIX shims) the wasm target's virtual FS.

#include "kama_runtime.h"

#if defined(_WIN32)

// ============================ Windows (Winsock + CRT) ============================
// Same function set and EXACT signatures as the POSIX branch below — the kama modules are identical on
// both platforms; only this header differs. Toolchain is mingw-w64 UCRT + clang (see release.yml). Winsock
// headers MUST precede <windows.h>; WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in the old winsock.h.
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>     // socket, bind, listen, accept, connect, send, recv, WSAStartup, SOCKET
#include <ws2tcpip.h>     // (numeric-host helpers)
#include <windows.h>      // FindFirstFileA / HANDLE / MAX_PATH
#include <io.h>           // _open, _read, _write, _close, _unlink
#include <fcntl.h>        // _O_*
#include <sys/stat.h>     // _stat64, _S_IFDIR
#include <errno.h>        // ENOENT, ECONNREFUSED, ... (UCRT defines the POSIX supplemental codes)
#include <string.h>       // memcpy, strlen
#include <stdlib.h>       // malloc, free (dir cursor)

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
        default:              errno = e;             break;   // carried through as IoError::Other(code)
    }
}

// ---- files (CRT low-level I/O) ---------------------------------------------
// _O_BINARY is essential: Windows text mode would translate CRLF/^Z and corrupt binary data.
static inline int32_t   kama_open_read(const char* path)   { return (int32_t)_open(path, _O_RDONLY | _O_BINARY); }
static inline int32_t   kama_open_create(const char* path) { return (int32_t)_open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE); }
static inline ptrdiff_t kama_read(int32_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)_read((int)fd, buf, (unsigned int)n); }
static inline ptrdiff_t kama_write(int32_t fd, const uint8_t* buf, size_t n) { return (ptrdiff_t)_write((int)fd, buf, (unsigned int)n); }
static inline int32_t   kama_close_fd(int32_t fd) { return (int32_t)_close((int)fd); }
static inline int32_t   kama_unlink(const char* path) { return (int32_t)_unlink(path); }

static inline int32_t kama_fstat_size(int32_t fd, uint64_t* outSize, int32_t* outIsDir) {
    struct _stat64 st; if (_fstat64((int)fd, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = (st.st_mode & _S_IFDIR) ? 1 : 0; return 0;
}
static inline int32_t kama_path_stat(const char* path, uint64_t* outSize, int32_t* outIsDir) {
    struct _stat64 st; if (_stat64(path, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = (st.st_mode & _S_IFDIR) ? 1 : 0; return 0;
}

// ---- directory iteration (Win32 FindFirstFile) -----------------------------
// DIR* analogue: a heap cursor holding the search handle + the pending entry (FindFirstFile already
// returns the first match). Empty string = end-of-directory; kama filters "." / ".." itself.
typedef struct kama__dir { HANDLE h; WIN32_FIND_DATAA data; int pending; } kama__dir;
static inline void* kama_diropen(const char* path) {
    char pattern[MAX_PATH];
    size_t n = strlen(path);
    if (n + 3 >= sizeof pattern) { errno = ENOMEM; return NULL; }   // long-path support: deferred
    memcpy(pattern, path, n);
    pattern[n] = '\\'; pattern[n + 1] = '*'; pattern[n + 2] = '\0';  // "<path>\*"
    kama__dir* d = (kama__dir*)malloc(sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    d->h = FindFirstFileA(pattern, &d->data);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); errno = ENOENT; return NULL; }
    d->pending = 1;
    return d;
}
static inline int32_t kama_dirclose(void* dirp) {
    kama__dir* d = (kama__dir*)dirp;
    BOOL ok = FindClose(d->h); free(d); return ok ? 0 : -1;
}
static inline kama_string kama_dirnext(void* dirp) {
    kama__dir* d = (kama__dir*)dirp;
    if (!d->pending && !FindNextFileA(d->h, &d->data)) {
        kama_string r; r.data = NULL; r.len = 0; r.cap = 0; return r;   // end of directory
    }
    d->pending = 0;
    return kama_string_from_raw((const uint8_t*)d->data.cFileName, 0, (int32_t)strlen(d->data.cFileName));
}

// ---- TCP sockets (Winsock2) ------------------------------------------------
// A SOCKET is UINT_PTR; INVALID_SOCKET reinterpreted as ptrdiff_t is -1, preserving `< 0 == error`.
static inline int32_t kama_net_init(void) {
    static int done = 0;
    if (!done) { WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { kama__capture_wsa(); return -1; } done = 1; }
    return 0;
}
static inline ptrdiff_t kama_socket_tcp(void) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)s;
}
static inline int32_t kama_set_reuseaddr(ptrdiff_t fd) {
    BOOL one = 1; return (int32_t)setsockopt((SOCKET)fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, (int)sizeof one);
}
static inline int32_t kama_bind_inet(ptrdiff_t fd, const char* host, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    if (bind((SOCKET)fd, (struct sockaddr*)&a, (int)sizeof a) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}
static inline int32_t kama_connect_inet(ptrdiff_t fd, const char* host, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = inet_addr((host && *host) ? host : "127.0.0.1");
    if (connect((SOCKET)fd, (struct sockaddr*)&a, (int)sizeof a) != 0) { kama__capture_wsa(); return -1; }
    return 0;
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
static inline ptrdiff_t kama_socket_udp(void) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)s;
}
static inline ptrdiff_t kama_sendto_inet(ptrdiff_t fd, const uint8_t* buf, size_t n, const char* host, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = inet_addr((host && *host) ? host : "127.0.0.1");
    int r = sendto((SOCKET)fd, (const char*)buf, (int)n, 0, (struct sockaddr*)&a, (int)sizeof a);
    if (r == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
    return (ptrdiff_t)r;
}
static inline ptrdiff_t kama_recvfrom_inet(ptrdiff_t fd, uint8_t* buf, size_t n, uint32_t* outIp, uint16_t* outPort) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    int alen = (int)sizeof a;
    int r = recvfrom((SOCKET)fd, (char*)buf, (int)n, 0, (struct sockaddr*)&a, &alen);
    if (r == SOCKET_ERROR) { kama__capture_wsa(); return -1; }
    if (outIp) *outIp = (uint32_t)ntohl(a.sin_addr.s_addr); if (outPort) *outPort = ntohs(a.sin_port);
    return (ptrdiff_t)r;
}

// ---- socket address / options ----------------------------------------------
// `outIp` is HOST-order (ntohl'd) so kama extracts octets with fixed shifts, endianness-independent.
static inline int32_t kama_getsockname(ptrdiff_t fd, uint32_t* outIp, uint16_t* outPort) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    int alen = (int)sizeof a;
    if (getsockname((SOCKET)fd, (struct sockaddr*)&a, &alen) != 0) { kama__capture_wsa(); return -1; }
    if (outIp) *outIp = (uint32_t)ntohl(a.sin_addr.s_addr); if (outPort) *outPort = ntohs(a.sin_port);
    return 0;
}
static inline int32_t kama_set_nonblocking(ptrdiff_t fd, int32_t on) {
    u_long mode = on ? 1u : 0u;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}
static inline int32_t kama_set_ttl(ptrdiff_t fd, uint32_t ttl) {
    DWORD v = (DWORD)ttl;
    if (setsockopt((SOCKET)fd, IPPROTO_IP, IP_TTL, (const char*)&v, (int)sizeof v) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}
static inline int32_t kama_set_broadcast(ptrdiff_t fd, int32_t on) {
    BOOL v = on ? 1 : 0;
    if (setsockopt((SOCKET)fd, SOL_SOCKET, SO_BROADCAST, (const char*)&v, (int)sizeof v) != 0) { kama__capture_wsa(); return -1; }
    return 0;
}

#else

#include <errno.h>
#include <string.h>       // memset, strlen
#include <fcntl.h>        // open, O_*
#include <sys/stat.h>     // fstat, stat, S_ISDIR
#include <dirent.h>       // opendir, readdir, closedir
#include <sys/socket.h>   // socket, bind, listen, accept, connect, setsockopt, send, recv
#include <netinet/in.h>   // sockaddr_in, htons, htonl, INADDR_ANY
#include <arpa/inet.h>    // inet_addr

// NOT <unistd.h>: on macOS its `write`/`read` carry a `__DARWIN_ALIAS_C` asm label, and because
// kama_runtime.h already declared+used `write` (its bounds-trap, block scope) BEFORE this header is
// reached, clang errors "cannot apply asm label to function after its first use." Declare the handful we
// need directly — they link to the same libc symbols — matching kama_runtime.h's own block-scoped-extern
// style (and keeping these decls out of user code).
extern long read(int, void*, size_t);
extern long write(int, const void*, size_t);
extern int  close(int);
extern int  unlink(const char*);

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

// ---- files -----------------------------------------------------------------
static inline int32_t   kama_open_read(const char* path)   { return (int32_t)open(path, O_RDONLY); }
static inline int32_t   kama_open_create(const char* path) { return (int32_t)open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
static inline ptrdiff_t kama_read(int32_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)read((int)fd, buf, n); }
static inline ptrdiff_t kama_write(int32_t fd, const uint8_t* buf, size_t n) { return (ptrdiff_t)write((int)fd, buf, n); }
static inline int32_t   kama_close_fd(int32_t fd) { return (int32_t)close((int)fd); }
static inline int32_t   kama_unlink(const char* path) { return (int32_t)unlink(path); }

// `struct stat` stays opaque: fold size + S_ISDIR into scalar out-params. 0 = ok, -1 = error (errno set).
static inline int32_t kama_fstat_size(int32_t fd, uint64_t* outSize, int32_t* outIsDir) {
    struct stat st; if (fstat((int)fd, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = S_ISDIR(st.st_mode) ? 1 : 0; return 0;
}
static inline int32_t kama_path_stat(const char* path, uint64_t* outSize, int32_t* outIsDir) {
    struct stat st; if (stat(path, &st) != 0) return -1;
    *outSize = (uint64_t)st.st_size; *outIsDir = S_ISDIR(st.st_mode) ? 1 : 0; return 0;
}

// Directory iteration: `DIR*` stays opaque (void*); each entry's `d_name` (inline char[]) is copied into
// a kama string via the runtime's kama_string_from_raw. An empty string signals end-of-directory (kama
// filters "." / ".." itself). NULL dirp = open failed (errno set).
static inline void*       kama_diropen(const char* path)  { return (void*)opendir(path); }
static inline int32_t     kama_dirclose(void* dirp)       { return (int32_t)closedir((DIR*)dirp); }
static inline kama_string kama_dirnext(void* dirp) {
    struct dirent* e = readdir((DIR*)dirp);
    if (!e) { kama_string r; r.data = NULL; r.len = 0; r.cap = 0; return r; }
    return kama_string_from_raw((const uint8_t*)e->d_name, 0, (int32_t)strlen(e->d_name));
}

// ---- TCP sockets -----------------------------------------------------------
// The `sockaddr_in` fill (family/htons/inet_addr/zero-init) and the `(struct sockaddr*)` cast are all done
// here — kama passes only fd:isize, host:cstr, port:uint16. Handles are `ptrdiff_t` (isize).
static inline int32_t   kama_net_init(void) { return 0; }   // POSIX: nothing to init (Windows: WSAStartup)
static inline ptrdiff_t kama_socket_tcp(void) { return (ptrdiff_t)socket(AF_INET, SOCK_STREAM, 0); }
static inline int32_t   kama_set_reuseaddr(ptrdiff_t fd) {
    int one = 1; return (int32_t)setsockopt((int)fd, SOL_SOCKET, SO_REUSEADDR, &one, (socklen_t)sizeof one);
}
static inline int32_t kama_bind_inet(ptrdiff_t fd, const char* host, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = (host && *host) ? inet_addr(host) : htonl(INADDR_ANY);
    return (int32_t)bind((int)fd, (struct sockaddr*)&a, (socklen_t)sizeof a);
}
static inline int32_t kama_connect_inet(ptrdiff_t fd, const char* host, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = inet_addr((host && *host) ? host : "127.0.0.1");
    return (int32_t)connect((int)fd, (struct sockaddr*)&a, (socklen_t)sizeof a);
}
static inline int32_t   kama_listen(ptrdiff_t fd, int32_t backlog) { return (int32_t)listen((int)fd, (int)backlog); }
static inline ptrdiff_t kama_accept(ptrdiff_t fd)                  { return (ptrdiff_t)accept((int)fd, (struct sockaddr*)0, (socklen_t*)0); }
static inline ptrdiff_t kama_recv(ptrdiff_t fd, uint8_t* buf, size_t n)        { return (ptrdiff_t)recv((int)fd, buf, n, 0); }
static inline ptrdiff_t kama_send(ptrdiff_t fd, const uint8_t* buf, size_t n)  { return (ptrdiff_t)send((int)fd, buf, n, 0); }
static inline int32_t   kama_close_socket(ptrdiff_t fd) { return (int32_t)close((int)fd); }

// ---- UDP datagrams ---------------------------------------------------------
// Same conventions as TCP: `sockaddr_in` fill stays in C, kama passes fd:isize + host:cstr + port:uint16.
// `recvfrom` hands the sender back as scalar out-params (IPv4 as a big-endian uint32 + host-order port) —
// never a struct — exactly like kama_fstat_size folds `struct stat`. On error return -1 (errno set).
static inline ptrdiff_t kama_socket_udp(void) { return (ptrdiff_t)socket(AF_INET, SOCK_DGRAM, 0); }
static inline ptrdiff_t kama_sendto_inet(ptrdiff_t fd, const uint8_t* buf, size_t n, const char* host, uint16_t port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    a.sin_addr.s_addr = inet_addr((host && *host) ? host : "127.0.0.1");
    return (ptrdiff_t)sendto((int)fd, buf, n, 0, (struct sockaddr*)&a, (socklen_t)sizeof a);
}
static inline ptrdiff_t kama_recvfrom_inet(ptrdiff_t fd, uint8_t* buf, size_t n, uint32_t* outIp, uint16_t* outPort) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    socklen_t alen = (socklen_t)sizeof a;
    ptrdiff_t r = (ptrdiff_t)recvfrom((int)fd, buf, n, 0, (struct sockaddr*)&a, &alen);
    if (r >= 0) { if (outIp) *outIp = (uint32_t)ntohl(a.sin_addr.s_addr); if (outPort) *outPort = ntohs(a.sin_port); }
    return r;
}

// ---- socket address / options ----------------------------------------------
// `outIp` is HOST-order (ntohl'd) so kama extracts octets with fixed shifts (a=>>24 … d=&0xff),
// endianness-independent. `outPort` is host order too.
static inline int32_t kama_getsockname(ptrdiff_t fd, uint32_t* outIp, uint16_t* outPort) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    socklen_t alen = (socklen_t)sizeof a;
    if (getsockname((int)fd, (struct sockaddr*)&a, &alen) != 0) return -1;
    if (outIp) *outIp = (uint32_t)ntohl(a.sin_addr.s_addr); if (outPort) *outPort = ntohs(a.sin_port);
    return 0;
}
static inline int32_t kama_set_nonblocking(ptrdiff_t fd, int32_t on) {
    int fl = fcntl((int)fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (on) fl |= O_NONBLOCK; else fl &= ~O_NONBLOCK;
    return (int32_t)fcntl((int)fd, F_SETFL, fl);
}
static inline int32_t kama_set_ttl(ptrdiff_t fd, uint32_t ttl) {
    int v = (int)ttl; return (int32_t)setsockopt((int)fd, IPPROTO_IP, IP_TTL, &v, (socklen_t)sizeof v);
}
static inline int32_t kama_set_broadcast(ptrdiff_t fd, int32_t on) {
    int v = on ? 1 : 0; return (int32_t)setsockopt((int)fd, SOL_SOCKET, SO_BROADCAST, &v, (socklen_t)sizeof v);
}

#endif  // !_WIN32
#endif  // KAMA_OS_H
