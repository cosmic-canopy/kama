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
#  error "kama_os.h: Windows branch not yet implemented (plan Step 4) — std::fs/std::net are POSIX-only for now"
#else

#include <errno.h>
#include <string.h>       // memset, strlen
#include <fcntl.h>        // open, O_*
#include <unistd.h>       // read, write, close, unlink
#include <sys/stat.h>     // fstat, stat, S_ISDIR
#include <dirent.h>       // opendir, readdir, closedir
#include <sys/socket.h>   // socket, bind, listen, accept, connect, setsockopt, send, recv
#include <netinet/in.h>   // sockaddr_in, htons, htonl, INADDR_ANY
#include <arpa/inet.h>    // inet_addr

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

#endif  // !_WIN32
#endif  // KAMA_OS_H
