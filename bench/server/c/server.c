// server.c — the C arm of KR-243, the ceiling: one event loop per thread, nothing between the loop and the socket.
//
// Usage: server <echo|http|skew> <port> <threads>
//   echo  writes back whatever it reads
//   http  answers every complete HTTP/1.1 request in its buffer with `200` and `Hello, World!`, keep-alive
//   skew  answers each 64-byte request after the rounds of mixing it asks for (client.c says how), the mix in bytes 4-7
//
// Linux: each thread has its own listener on the port (SO_REUSEPORT), so the kernel spreads connections — the
// thread-per-core shape the kama fiber arm has. macOS balances no SO_REUSEPORT group, so there the threads share one
// listener, each loop accepting from it.
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif

enum { BUF = 4096 };
static const char RESP[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, World!";
enum { ECHO, HTTP, SKEW, REQ = 64 };
static int g_mode, g_port;

// The skew burst, the same in every arm: `rounds` passes of FNV-1a over the request's 64 bytes. Each step depends on
// the one before, so no compiler can vectorize it away, and its result goes back in the reply.
static uint32_t mix(const unsigned char* b, uint32_t rounds) {
    uint32_t h = 2166136261u;
    for (uint32_t r = 0; r < rounds; ++r)
        for (int i = 0; i < REQ; ++i) h = (h ^ b[i]) * 16777619u;
    return h;
}

typedef struct { int fd; size_t in; char buf[BUF]; } conn;

static int listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef __linux__
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port); a.sin_addr.s_addr = htonl(0x7f000001u);
    if (bind(fd, (struct sockaddr*)&a, sizeof a) != 0 || listen(fd, 4096) != 0) { perror("server: listen"); exit(1); }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static void watch(int q, int fd, void* tag) {
#ifdef __linux__
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = tag };
    epoll_ctl(q, EPOLL_CTL_ADD, fd, &ev);
#else
    struct kevent ev; EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, tag);
    kevent(q, &ev, 1, NULL, 0, NULL);
#endif
}

static int write_all(int fd, const char* p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return -1;   // a 64-byte echo or a 76-byte reply never fills a socket buffer the client drains
    }
    return 0;
}

// 0 to keep the connection, -1 to close it.
static int serve(conn* c) {
    for (;;) {
        ssize_t n = read(c->fd, c->buf + c->in, BUF - 1 - c->in);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return errno == EAGAIN ? 0 : -1; }
        if (g_mode == ECHO) { if (write_all(c->fd, c->buf, (size_t)n) != 0) return -1; continue; }
        c->in += (size_t)n;
        if (g_mode == SKEW) {
            for (; c->in >= REQ; c->in -= REQ, memmove(c->buf, c->buf + REQ, c->in)) {
                unsigned char* b = (unsigned char*)c->buf;
                uint32_t h = mix(b, (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
                for (int i = 0; i < 4; ++i) b[4 + i] = (unsigned char)(h >> (8 * i));
                if (write_all(c->fd, c->buf, REQ) != 0) return -1;
            }
            continue;
        }
        c->buf[c->in] = 0;
        char* start = c->buf;
        for (char* end; (end = strstr(start, "\r\n\r\n")) != NULL; start = end + 4)
            if (write_all(c->fd, RESP, sizeof RESP - 1) != 0) return -1;
        size_t left = c->in - (size_t)(start - c->buf);
        memmove(c->buf, start, left); c->in = left;
        if (c->in >= BUF - 1) return -1;
    }
}

static int g_shared = -1;
static void* loop(void* arg) {
    (void)arg;
    int lfd = g_shared >= 0 ? g_shared : listener();
#ifdef __linux__
    int q = epoll_create1(0);
#else
    int q = kqueue();
#endif
    watch(q, lfd, NULL);
    enum { MAXEV = 256 };
    for (;;) {
#ifdef __linux__
        struct epoll_event evs[MAXEV];
        int n = epoll_wait(q, evs, MAXEV, -1);
#else
        struct kevent evs[MAXEV];
        int n = kevent(q, NULL, 0, evs, MAXEV, NULL);
#endif
        for (int i = 0; i < n; ++i) {
#ifdef __linux__
            conn* c = evs[i].data.ptr;
#else
            conn* c = evs[i].udata;
#endif
            if (!c) {
                for (;;) {
                    int fd = accept(lfd, NULL, NULL);
                    if (fd < 0) break;
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                    conn* nc = calloc(1, sizeof(conn)); nc->fd = fd;
                    watch(q, fd, nc);
                }
                continue;
            }
            if (serve(c) != 0) { close(c->fd); free(c); }
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "usage: server <echo|http|skew> <port> <threads>\n"); return 64; }
    g_mode = strcmp(argv[1], "http") == 0 ? HTTP : strcmp(argv[1], "skew") == 0 ? SKEW : ECHO;
    g_port = atoi(argv[2]);
    int threads = atoi(argv[3]);
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) == 0) {
        r.rlim_cur = r.rlim_max;
#ifdef __APPLE__
        if (r.rlim_cur > 1048576) r.rlim_cur = 1048576;   // macOS refuses an infinite soft limit
#endif
        setrlimit(RLIMIT_NOFILE, &r);
    }
#ifndef __linux__
    g_shared = listener();
#endif
    pthread_t th;
    for (int t = 1; t < threads; ++t) pthread_create(&th, NULL, loop, NULL);
    loop(NULL);
    return 0;
}
