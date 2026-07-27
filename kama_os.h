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
// The readiness poller uses select() (see kama_poller_wait — WSAPoll mis-handles a connecting socket).
// FD_SETSIZE caps how many sockets fit one fd_set; raise it from the default 64 for the server selector.
// MUST be defined before <winsock2.h>.
#ifndef FD_SETSIZE
#  define FD_SETSIZE 1024
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
// Same shape + bit convention as the POSIX branch; a heap WSAPOLLFD[] behind a Ptr stores each fd +
// requested events (WSAPOLLFD is just a convenient {SOCKET, events, revents} record here). The wait()
// itself uses select(), NOT WSAPoll: WSAPoll mis-handles a *connecting* socket — it can return a socket
// as ready with revents==0 (or never signal a refused connect), so a caller resolving a non-blocking
// connect (poll for writable, then read SO_ERROR) reads SO_ERROR while the handshake is still in flight
// and mis-reports it as connected. select() reports a connecting socket only on genuine resolution —
// writefds on success, exceptfds on failure — which is exactly the "writable == connect resolved"
// contract std::net::Poller advertises. (Bounded by FD_SETSIZE, raised above.)
typedef struct kama__poller { WSAPOLLFD* fds; int len; int cap; } kama__poller;
static inline void* kama_poller_create(void) {
    kama__poller* p = (kama__poller*)malloc(sizeof *p);
    if (!p) { errno = ENOMEM; return NULL; }
    p->fds = NULL; p->len = 0; p->cap = 0; return p;
}
static inline void kama_poller_add(void* ph, ptrdiff_t fd, int32_t interest) {
    kama__poller* p = (kama__poller*)ph;
    SHORT ev = 0;
    if (interest & 1) ev = (SHORT)(ev | POLLRDNORM);
    if (interest & 2) ev = (SHORT)(ev | POLLWRNORM);
    for (int i = 0; i < p->len; i++) if (p->fds[i].fd == (SOCKET)fd) { p->fds[i].events = ev; p->fds[i].revents = 0; return; }
    if (p->len == p->cap) {
        int nc = p->cap ? p->cap * 2 : 8;
        WSAPOLLFD* nf = (WSAPOLLFD*)realloc(p->fds, (size_t)nc * sizeof(WSAPOLLFD));
        if (!nf) return; p->fds = nf; p->cap = nc;
    }
    p->fds[p->len].fd = (SOCKET)fd; p->fds[p->len].events = ev; p->fds[p->len].revents = 0; p->len++;
}
static inline void kama_poller_remove(void* ph, ptrdiff_t fd) {
    kama__poller* p = (kama__poller*)ph;
    for (int i = 0; i < p->len; i++) if (p->fds[i].fd == (SOCKET)fd) { p->fds[i] = p->fds[p->len - 1]; p->len--; return; }
}
static inline int32_t kama_poller_wait(void* ph, int32_t timeoutMs) {
    kama__poller* p = (kama__poller*)ph;
    fd_set rd, wr, ex;
    FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
    for (int i = 0; i < p->len; i++) {
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
    for (int i = 0; i < p->len; i++) {
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
    for (int i = 0; i < p->len; i++) if (p->fds[i].fd == (SOCKET)fd) {
        int bits = 0;
        if (p->fds[i].revents & (POLLRDNORM | POLLHUP | POLLERR)) bits |= 1;
        if (p->fds[i].revents & POLLWRNORM) bits |= 2;
        return (int32_t)bits;
    }
    return 0;
}
static inline void kama_poller_free(void* ph) {
    kama__poller* p = (kama__poller*)ph;
    if (p) { free(p->fds); free(p); }
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
#include <arpa/inet.h>    // inet_addr
#include <poll.h>         // poll, struct pollfd, POLLIN/POLLOUT
#include <sys/wait.h>     // waitpid, WIFEXITED/WEXITSTATUS/WIFSIGNALED/WTERMSIG, WNOHANG  (std::process)
#include <signal.h>       // kill, SIGKILL, SIGTERM  (std::process)

// NOT <unistd.h>: on macOS its `write`/`read` carry a `__DARWIN_ALIAS_C` asm label, and because
// kama_runtime.h already declared+used `write` (its bounds-trap, block scope) BEFORE this header is
// reached, clang errors "cannot apply asm label to function after its first use." Declare the handful we
// need directly — they link to the same libc symbols — matching kama_runtime.h's own block-scoped-extern
// style (and keeping these decls out of user code).
extern long read(int, void*, size_t);
extern long write(int, const void*, size_t);
extern int  close(int);
extern int  unlink(const char*);
// std::process (fork/exec/pipe): hand-declared for the same reason as read/write above. These have no
// asm-label alias on macOS, so redeclaring is safe even if a socket header transitively pulls <unistd.h>.
extern int   pipe(int[2]);
extern int   dup2(int, int);
extern int   chdir(const char*);
extern int   execvp(const char*, char* const[]);
extern int   fork(void);
extern void  _exit(int);
extern int   kill(int, int);
extern int   fcntl(int, int, ...);
extern char* strdup(const char*);
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

// ---- process (std::process; POSIX fork/exec) -------------------------------
// A child's argv/envp are built HERE as strdup'd `char*[]` — owned independently of the kama `string` RAII,
// so they survive across fork even after the parent frees the source strings. kama holds only the opaque
// vector `Ptr`, `int32` fds/pid, and the `int` wait-status folded to scalar accessors (like `struct stat`).
static inline void* kama_argv_new(int32_t n) {
    return calloc((size_t)n + 1, sizeof(char*));           // n slots + NULL terminator, zeroed
}
static inline void kama_argv_set(void* v, int32_t i, const char* s) {
    ((char**)v)[i] = strdup(s ? s : "");                   // own a copy (survives kama-string drop across fork)
}
static inline void kama_argv_free(void* v) {
    if (!v) return;
    for (char** p = (char**)v; *p; ++p) free(*p);
    free(v);
}
// Build the child's envp: start from the parent `environ` (unless `clear`), then apply each "KEY=VALUE"
// override — REPLACING an inherited entry with the same KEY (getenv semantics: a plain append wouldn't
// override, since libc returns the first match), else appending. `overrides` is a NULL-terminated char*[]
// of "KEY=VALUE". Returns a fresh strdup'd char*[] (free with kama_argv_free). Done in the PARENT (malloc
// is safe there); the child only assigns the result to `environ` then execs.
static inline void* kama_envp_build(void* overridesV, int32_t clear) {
    char** ov = (char**)overridesV;
    int nov = 0; if (ov) while (ov[nov]) nov++;
    int nbase = 0; if (!clear && environ) while (environ[nbase]) nbase++;
    char** out = (char**)calloc((size_t)nbase + (size_t)nov + 1, sizeof(char*));
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
        if (!overridden) out[k++] = strdup(e);
    }
    for (int j = 0; j < nov; j++) out[k++] = strdup(ov[j]);
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
// no lock it must release, so this is safe. Each std-fd arg is a real fd to dup2 (a pipe end or /dev/null),
// or -1 to leave the inherited fd untouched. Returns 0 (pid in *outPid) or -1 (fork failed).
static inline int32_t kama_proc_spawn(void* argv, void* envp, const char* cwd,
                                      int32_t inFd, int32_t outFd, int32_t errFd, int32_t* outPid) {
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
    *outPid = (int32_t)pid;                                // ---- parent ----
    return 0;
}
// waitpid folded to scalar. Returns >0 (reaped; status in *outStatus), 0 (WNOHANG: still running), -1 (err).
static inline int32_t kama_waitpid(int32_t pid, int32_t* outStatus, int32_t flags) {
    int st = 0;
    int r = (int)waitpid((int)pid, &st, (int)flags);
    if (r > 0) *outStatus = (int32_t)st;
    return (int32_t)r;
}
static inline int32_t kama_proc_exited(int32_t st)      { return WIFEXITED(st)   ? 1 : 0; }
static inline int32_t kama_proc_exit_code(int32_t st)   { return (int32_t)WEXITSTATUS(st); }
static inline int32_t kama_proc_signaled(int32_t st)    { return WIFSIGNALED(st) ? 1 : 0; }
static inline int32_t kama_proc_term_signal(int32_t st) { return (int32_t)WTERMSIG(st); }
static inline int32_t kama_kill(int32_t pid, int32_t sig) { return (int32_t)kill((int)pid, (int)sig); }
static inline int32_t kama_WNOHANG(void) { return (int32_t)WNOHANG; }
static inline int32_t kama_SIGKILL(void) { return (int32_t)SIGKILL; }
static inline int32_t kama_SIGTERM(void) { return (int32_t)SIGTERM; }

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

// ---- readiness poller (poll(2)) --------------------------------------------
// A heap `struct pollfd[]` cursor stays OPAQUE to kama (behind a `Ptr`). interest bits: 1=read, 2=write.
// ready bits: 1=readable (incl. hangup/error so the caller reads EOF/err), 2=writable (a connect resolved).
typedef struct kama__poller { struct pollfd* fds; int len; int cap; } kama__poller;
static inline void* kama_poller_create(void) {
    kama__poller* p = (kama__poller*)malloc(sizeof *p);
    if (!p) { errno = ENOMEM; return NULL; }
    p->fds = NULL; p->len = 0; p->cap = 0; return p;
}
static inline void kama_poller_add(void* ph, ptrdiff_t fd, int32_t interest) {
    kama__poller* p = (kama__poller*)ph;
    short ev = 0;
    if (interest & 1) ev = (short)(ev | POLLIN);
    if (interest & 2) ev = (short)(ev | POLLOUT);
    for (int i = 0; i < p->len; i++) if (p->fds[i].fd == (int)fd) { p->fds[i].events = ev; p->fds[i].revents = 0; return; }
    if (p->len == p->cap) {
        int nc = p->cap ? p->cap * 2 : 8;
        struct pollfd* nf = (struct pollfd*)realloc(p->fds, (size_t)nc * sizeof(struct pollfd));
        if (!nf) return; p->fds = nf; p->cap = nc;
    }
    p->fds[p->len].fd = (int)fd; p->fds[p->len].events = ev; p->fds[p->len].revents = 0; p->len++;
}
static inline void kama_poller_remove(void* ph, ptrdiff_t fd) {
    kama__poller* p = (kama__poller*)ph;
    for (int i = 0; i < p->len; i++) if (p->fds[i].fd == (int)fd) { p->fds[i] = p->fds[p->len - 1]; p->len--; return; }
}
static inline int32_t kama_poller_wait(void* ph, int32_t timeoutMs) {
    kama__poller* p = (kama__poller*)ph;
    return (int32_t)poll(p->fds, (nfds_t)p->len, (int)timeoutMs);   // -1 errno; 0 timeout; >0 count
}
static inline int32_t kama_poller_ready(void* ph, ptrdiff_t fd) {
    kama__poller* p = (kama__poller*)ph;
    for (int i = 0; i < p->len; i++) if (p->fds[i].fd == (int)fd) {
        int bits = 0;
        if (p->fds[i].revents & (POLLIN | POLLHUP | POLLERR)) bits |= 1;
        if (p->fds[i].revents & POLLOUT) bits |= 2;
        return (int32_t)bits;
    }
    return 0;
}
static inline void kama_poller_free(void* ph) {
    kama__poller* p = (kama__poller*)ph;
    if (p) { free(p->fds); free(p); }
}

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
#endif  // KAMA_OS_H
