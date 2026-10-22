// poller.c — what one park costs a fiber scheduler, under each way of registering a socket.
//
// KR-3 decision 6 (docs/design/fibers.md). A scheduler parks a fiber on a socket only after the I/O call reported
// WouldBlock, and wakes it when the kernel says the socket is ready. The public `Poller` is level-triggered (ruled
// 2026-10-21): it reports a socket on every wait while the socket stays ready, so a scheduler on it has to switch a
// socket off when nobody is parked on it and on again at the next park. Edge-triggered and one-shot registration
// trade that bookkeeping for something else. This program runs the park protocol under each arm, with no fibers (a
// task is a struct; the cost of a stack switch is a separate question), counts every system call the server thread
// makes, and times the server thread's CPU.
//
// Usage: poller <arm> <workload> <conns> <requests>      one tab-separated line on stdout; `poller -h` prints the header
//   arm       epoll (Linux):  lt  et  oneshot
//             kqueue (macOS): lt  clear  clear-read  dispatch  dispatch-now
//   workload  pingpong  one 64-byte request per round trip, so a task parks once per request
//             pipeline  16 requests written at once and a yield after each, so a socket stays readable while its
//                       task is not parked: the case where the arms differ in what an unwanted report costs
// VERIFY=1 also prints every counter, setup and client threads included, on stderr, to compare with `strace -c -f`.
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/epoll.h>
#define BACKEND "epoll"
#else
#include <sys/event.h>
#include <pthread/qos.h>
#define BACKEND "kqueue"
#endif

enum { REQ = 64, BATCH = 16, MAXEV = 1024, CLIENTS = 3 };
enum { LT, ET, ONESHOT, CLEAR, DISPATCH, DISPATCH_NOW, CLEAR_READ };

// Each thread counts its own calls. `ctl` is a registration call (epoll_ctl, or a kevent() that only applies changes);
// `changes` is a kevent change record, which rides in the next wait's call unless the arm applies it at once.
typedef struct { uint64_t waits, ctl, changes, reads, writes, again, events, idle; } counts;
static _Thread_local counts C;
static counts clientSum;
static pthread_mutex_t sumLock = PTHREAD_MUTEX_INITIALIZER;

static void die(const char* what) { perror(what); exit(2); }
static uint64_t nowNs(clockid_t clk) { struct timespec t; clock_gettime(clk, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static ssize_t rd(int fd, void* b, size_t n) { C.reads++; ssize_t r = read(fd, b, n); if (r < 0 && errno == EAGAIN) C.again++; return r; }
static ssize_t wr(int fd, const void* b, size_t n) { C.writes++; return write(fd, b, n); }
static void nonBlocking(int fd) { if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) die("fcntl"); }

// ---- the server: one task per connection, a ready queue, and the arm's park/report rules -------------------------
typedef struct { int fd; int have; bool parked; bool armed; } task;
static task* T; static int NT; static int arm;
static int* rq; static int rqHead, rqTail, rqLen;      // a task is queued at most once, so NT slots suffice
static void push(task* t) { rq[rqTail] = (int)(t - T); rqTail = (rqTail + 1) % NT; rqLen++; }
static task* pop(void) { task* t = &T[rq[rqHead]]; rqHead = (rqHead + 1) % NT; rqLen--; return t; }

#ifdef __linux__
static int ep;
static struct epoll_event evs[MAXEV];
static void ctl(int op, task* t, uint32_t ev) {
    C.ctl++;
    struct epoll_event e; memset(&e, 0, sizeof e); e.events = ev; e.data.u64 = (uint64_t)(t - T);
    if (epoll_ctl(ep, op, t->fd, &e) != 0) die("epoll_ctl");
}
static void backendOpen(void) { if ((ep = epoll_create1(EPOLL_CLOEXEC)) < 0) die("epoll_create1"); }
static void watch(task* t) {
    switch (arm) {
    case LT:      ctl(EPOLL_CTL_ADD, t, EPOLLIN | EPOLLRDHUP); break;
    case ET:      ctl(EPOLL_CTL_ADD, t, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET); break;   // both directions, once (mio)
    case ONESHOT: ctl(EPOLL_CTL_ADD, t, EPOLLIN | EPOLLRDHUP | EPOLLONESHOT); break;
    }
    t->armed = true;
}
static void park(task* t) {
    t->parked = true;
    if (t->armed) return;
    ctl(EPOLL_CTL_MOD, t, arm == LT ? EPOLLIN | EPOLLRDHUP : EPOLLIN | EPOLLRDHUP | EPOLLONESHOT);
    t->armed = true;
}
static void reported(task* t, bool readable) {
    if (arm == ONESHOT) t->armed = false;                 // the kernel disarmed it when it reported it
    if (t->parked && readable) { t->parked = false; push(t); return; }
    C.idle++;                                             // a report nobody was waiting for
    if (arm == LT) { ctl(EPOLL_CTL_MOD, t, 0); t->armed = false; }   // or every wait reports it again
}
static void serverWait(int timeoutMs) {
    C.waits++;
    int n = epoll_wait(ep, evs, MAXEV, timeoutMs);
    if (n < 0) { if (errno == EINTR) return; die("epoll_wait"); }
    C.events += (uint64_t)n;
    for (int i = 0; i < n; i++)
        reported(&T[evs[i].data.u64], (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0);
}
#else
static int kq;
static struct kevent chg[MAXEV]; static int nchg;
static struct kevent evs[MAXEV];
static void flush(void) {
    if (nchg == 0) return;
    C.ctl++;
    if (kevent(kq, chg, nchg, NULL, 0, NULL) < 0) die("kevent(changes)");
    nchg = 0;
}
static void change(task* t, int16_t filter, uint16_t flags) {
    C.changes++;
    EV_SET(&chg[nchg], (uintptr_t)t->fd, filter, flags, 0, 0, (void*)(intptr_t)(t - T)); nchg++;
    if (arm == DISPATCH_NOW || nchg == MAXEV) flush();    // DISPATCH_NOW: one kevent() per change, as kama__kq_change does
}
static void backendOpen(void) { if ((kq = kqueue()) < 0) die("kqueue"); }
static void watch(task* t) {
    switch (arm) {
    case LT:       change(t, EVFILT_READ, EV_ADD); break;
    case CLEAR:    change(t, EVFILT_READ, EV_ADD | EV_CLEAR); change(t, EVFILT_WRITE, EV_ADD | EV_CLEAR); break;   // Go, mio
    case CLEAR_READ: change(t, EVFILT_READ, EV_ADD | EV_CLEAR); break;   // WRITE only while a writer waits
    case DISPATCH: case DISPATCH_NOW: change(t, EVFILT_READ, EV_ADD | EV_DISPATCH); break;
    }
    t->armed = true;
}
static void park(task* t) {
    t->parked = true;
    if (t->armed) return;
    change(t, EVFILT_READ, EV_ENABLE);
    t->armed = true;
}
static void reported(task* t, bool readable) {
    if (arm == DISPATCH || arm == DISPATCH_NOW) t->armed = false;   // EV_DISPATCH disabled it as it reported it
    if (t->parked && readable) { t->parked = false; push(t); return; }
    C.idle++;
    if (arm == LT) { change(t, EVFILT_READ, EV_DISABLE); t->armed = false; }
}
static void serverWait(int timeoutMs) {
    C.waits++;
    struct timespec zero = { 0, 0 };
    int n = kevent(kq, chg, nchg, evs, MAXEV, timeoutMs == 0 ? &zero : NULL);   // pending changes ride in this call
    nchg = 0;
    if (n < 0) { if (errno == EINTR) return; die("kevent(wait)"); }
    C.events += (uint64_t)n;
    for (int i = 0; i < n; i++) {
        if (evs[i].flags & EV_ERROR) { errno = (int)evs[i].data; die("kevent change"); }
        reported(&T[(intptr_t)evs[i].udata], evs[i].filter == EVFILT_READ);
    }
}
#endif

static uint64_t served, total; static bool pipelined;
static void run(task* t) {
    char buf[REQ];
    for (;;) {
        ssize_t r = rd(t->fd, buf, (size_t)(REQ - t->have));   // exactly one request, so the next stays in the socket
        if (r < 0) { if (errno == EAGAIN) { park(t); return; } die("read"); }
        if (r == 0) { fprintf(stderr, "read: a client closed early\n"); exit(2); }
        t->have += (int)r;
        if (t->have < REQ) continue;
        t->have = 0;
        if (wr(t->fd, buf, REQ) != REQ) die("write: a 64-byte reply did not fit, which this model does not handle");
        served++;
        if (pipelined) { push(t); return; }               // yield: the next request is probably buffered already
    }
}

// ---- the clients: closed loop, one round in flight per connection, plain level-triggered readiness --------------
typedef struct { int* fds; int n; uint64_t rounds; size_t bytes; } client;
static void* clientMain(void* arg) {
    client* c = (client*)arg;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    static const char req[REQ * BATCH];
    char buf[REQ * BATCH];
    size_t* need = calloc((size_t)c->n, sizeof *need); uint64_t* left = calloc((size_t)c->n, sizeof *left);
    if (!need || !left) die("calloc");
#ifdef __linux__
    int q = epoll_create1(EPOLL_CLOEXEC); if (q < 0) die("epoll_create1");
    struct epoll_event* ev = calloc(MAXEV, sizeof *ev);
#else
    int q = kqueue(); if (q < 0) die("kqueue");
    struct kevent* ev = calloc(MAXEV, sizeof *ev);
#endif
    if (!ev) die("calloc");
    for (int i = 0; i < c->n; i++) {
#ifdef __linux__
        C.ctl++;
        struct epoll_event e; memset(&e, 0, sizeof e); e.events = EPOLLIN; e.data.u64 = (uint64_t)i;
        if (epoll_ctl(q, EPOLL_CTL_ADD, c->fds[i], &e) != 0) die("epoll_ctl(client)");
#else
        C.ctl++; C.changes++;
        struct kevent k; EV_SET(&k, (uintptr_t)c->fds[i], EVFILT_READ, EV_ADD, 0, 0, (void*)(intptr_t)i);
        if (kevent(q, &k, 1, NULL, 0, NULL) < 0) die("kevent(client)");
#endif
        need[i] = c->bytes; left[i] = c->rounds;
        if (wr(c->fds[i], req, c->bytes) != (ssize_t)c->bytes) die("write(client)");
    }
    int live = c->n;
    while (live > 0) {
        C.waits++;
#ifdef __linux__
        int n = epoll_wait(q, ev, MAXEV, -1);
#else
        int n = kevent(q, NULL, 0, ev, MAXEV, NULL);
#endif
        if (n < 0) { if (errno == EINTR) continue; die("client wait"); }
        for (int k = 0; k < n; k++) {
#ifdef __linux__
            int i = (int)ev[k].data.u64;
#else
            int i = (int)(intptr_t)ev[k].udata;
#endif
            ssize_t r = rd(c->fds[i], buf, need[i]);
            if (r < 0) { if (errno == EAGAIN) continue; die("read(client)"); }
            if (r == 0) { fprintf(stderr, "client: the server closed early\n"); exit(2); }
            need[i] -= (size_t)r;
            if (need[i] > 0) continue;
            if (--left[i] == 0) { live--; continue; }
            need[i] = c->bytes;
            if (wr(c->fds[i], req, c->bytes) != (ssize_t)c->bytes) die("write(client)");
        }
    }
    close(q); free(ev); free(need); free(left);
    pthread_mutex_lock(&sumLock);
    clientSum.waits += C.waits; clientSum.ctl += C.ctl; clientSum.changes += C.changes;
    clientSum.reads += C.reads; clientSum.writes += C.writes; clientSum.again += C.again;
    pthread_mutex_unlock(&sumLock);
    return NULL;
}

static void printCounts(const char* who, const counts* k) {
    fprintf(stderr, "%-8s waits=%llu ctl=%llu changes=%llu reads=%llu writes=%llu again=%llu\n", who,
            (unsigned long long)k->waits, (unsigned long long)k->ctl, (unsigned long long)k->changes,
            (unsigned long long)k->reads, (unsigned long long)k->writes, (unsigned long long)k->again);
}

int main(int argc, char** argv) {
    const char* header = "backend\tarm\tworkload\tconns\trequests\twall_ns\tcpu_ns\twaits\tctl\tchanges\treads\twrites\tagain\tevents\tidle";
    if (argc == 2 && strcmp(argv[1], "-h") == 0) { puts(header); return 0; }
    if (argc != 5) { fprintf(stderr, "usage: poller <arm> <pingpong|pipeline> <conns> <requests>\n%s\n", header); return 2; }
    const char* arms[] = { "lt", "et", "oneshot", "clear", "dispatch", "dispatch-now", "clear-read" };
    arm = -1;
    for (int i = 0; i < 7; i++) if (strcmp(argv[1], arms[i]) == 0) arm = i;
#ifdef __linux__
    if (arm != LT && arm != ET && arm != ONESHOT) { fprintf(stderr, "epoll arms: lt et oneshot\n"); return 2; }
#else
    if (arm != LT && arm != CLEAR && arm != CLEAR_READ && arm != DISPATCH && arm != DISPATCH_NOW) { fprintf(stderr, "kqueue arms: lt clear clear-read dispatch dispatch-now\n"); return 2; }
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    pipelined = strcmp(argv[2], "pipeline") == 0;
    if (!pipelined && strcmp(argv[2], "pingpong") != 0) { fprintf(stderr, "workload: pingpong or pipeline\n"); return 2; }
    NT = atoi(argv[3]);
    const uint64_t asked = strtoull(argv[4], NULL, 10), per = pipelined ? BATCH : 1;
    if (NT < CLIENTS) { fprintf(stderr, "conns: at least %d\n", CLIENTS); return 2; }
    uint64_t rounds = asked / ((uint64_t)NT * per); if (rounds == 0) rounds = 1;
    total = rounds * (uint64_t)NT * per;
    signal(SIGPIPE, SIG_IGN);

    struct rlimit lim; getrlimit(RLIMIT_NOFILE, &lim);
    rlim_t want = (rlim_t)NT * 2 + 64;
    if (lim.rlim_cur < want) { lim.rlim_cur = lim.rlim_max < want ? lim.rlim_max : want; setrlimit(RLIMIT_NOFILE, &lim); }

    // Setup, untimed: NT loopback connections, accepted one at a time so the backlog never fills.
    int ls = socket(AF_INET, SOCK_STREAM, 0); if (ls < 0) die("socket");
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr*)&a, sizeof a) != 0 || listen(ls, 128) != 0 || getsockname(ls, (struct sockaddr*)&a, &al) != 0) die("listen");
    T = calloc((size_t)NT, sizeof *T); rq = calloc((size_t)NT, sizeof *rq);
    int* cfd = calloc((size_t)NT, sizeof *cfd);
    if (!T || !rq || !cfd) die("calloc");
    const int one = 1; const struct linger rst = { 1, 0 };   // close with a reset: no TIME_WAIT to exhaust the ports
    for (int i = 0; i < NT; i++) {
        int c = socket(AF_INET, SOCK_STREAM, 0); if (c < 0) die("socket(client)");
        if (connect(c, (struct sockaddr*)&a, sizeof a) != 0) die("connect");
        int s = accept(ls, NULL, NULL); if (s < 0) die("accept");
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(c, SOL_SOCKET, SO_LINGER, &rst, sizeof rst);
        nonBlocking(c); nonBlocking(s);
        cfd[i] = c; T[i].fd = s;
    }
    close(ls);
    backendOpen();
    for (int i = 0; i < NT; i++) watch(&T[i]);
#ifndef __linux__
    flush();
#endif
    counts setup = C; memset(&C, 0, sizeof C);

    client cl[CLIENTS]; pthread_t th[CLIENTS];
    for (int k = 0; k < CLIENTS; k++) {
        cl[k].n = NT / CLIENTS + (k < NT % CLIENTS);
        cl[k].fds = cfd + (k * (NT / CLIENTS) + (k < NT % CLIENTS ? k : NT % CLIENTS));
        cl[k].rounds = rounds; cl[k].bytes = (size_t)(REQ * per);
    }
    const uint64_t w0 = nowNs(CLOCK_MONOTONIC), c0 = nowNs(CLOCK_THREAD_CPUTIME_ID);
    for (int k = 0; k < CLIENTS; k++) if (pthread_create(&th[k], NULL, clientMain, &cl[k]) != 0) die("pthread_create");
    for (int i = 0; i < NT; i++) push(&T[i]);             // every task starts with a read, which parks it
    while (served < total) {
        for (int k = rqLen; k > 0 && served < total; k--) run(pop());
        if (served < total) serverWait(rqLen > 0 ? 0 : -1);
    }
    const uint64_t c1 = nowNs(CLOCK_THREAD_CPUTIME_ID), w1 = nowNs(CLOCK_MONOTONIC);
    for (int k = 0; k < CLIENTS; k++) pthread_join(th[k], NULL);

    printf("%s\t%s\t%s\t%d\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n", BACKEND, argv[1], argv[2], NT,
           (unsigned long long)total, (unsigned long long)(w1 - w0), (unsigned long long)(c1 - c0),
           (unsigned long long)C.waits, (unsigned long long)C.ctl, (unsigned long long)C.changes, (unsigned long long)C.reads,
           (unsigned long long)C.writes, (unsigned long long)C.again, (unsigned long long)C.events, (unsigned long long)C.idle);
    if (getenv("VERIFY")) { printCounts("setup", &setup); printCounts("server", &C); printCounts("clients", &clientSum); }
    for (int i = 0; i < NT; i++) { close(cfd[i]); close(T[i].fd); }
    return 0;
}
