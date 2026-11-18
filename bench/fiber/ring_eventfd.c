// Does a DEFER_TASKRUN ring's registered eventfd report each RECV's completion to another thread while the ring's
// owner sleeps elsewhere (a window package's wait)? kama's flags; two bytes 60 ms apart; the owner re-arms after each.
// KR-412's probe, independent of kama. Linux only:  cc -O1 -pthread -o /tmp/rp ring_eventfd.c && /tmp/rp 0  (or 1, 2)
// Mode 0 is kama's ring flags watched through the eventfd, mode 1 drops DEFER_TASKRUN, mode 2 keeps kama's flags and
// watches the socket itself. On Linux 6.18 (WSL, 2026-11-18) mode 0 reported byte 1 20–34 ms late and byte 2 only at
// the owner's 1 s bound; modes 1 and 2 reported both within 1 ms (ROADMAP_DETAIL `s5-ios`, KR-412).
#define _GNU_SOURCE
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

static long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }
static int ringFd, efd, cli, srv, mode;
static unsigned *sqHead, *sqTail, *sqMask, *sqArray, *cqHead, *cqTail, *cqMask, sqLocal;
static struct io_uring_sqe* sqes; static struct io_uring_cqe* cqes;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t c = PTHREAD_COND_INITIALIZER; static int posted;
static uint8_t buf[1];

static int enter(unsigned submit) {
    __atomic_store_n(sqTail, sqLocal, __ATOMIC_RELEASE);
    return (int)syscall(__NR_io_uring_enter, ringFd, submit, 0, IORING_ENTER_GETEVENTS, NULL, 0);
}
static void recvSqe(void) {
    unsigned i = sqLocal & *sqMask; struct io_uring_sqe* e = &sqes[i]; memset(e, 0, sizeof *e);
    e->opcode = IORING_OP_RECV; e->fd = srv; e->addr = (uint64_t)(uintptr_t)buf; e->len = 1; e->user_data = 7;
    sqArray[i] = i; sqLocal++;
}
static int reap(void) {
    int n = 0; unsigned h = *cqHead;
    while (h != __atomic_load_n(cqTail, __ATOMIC_ACQUIRE)) { struct io_uring_cqe* q = &cqes[h & *cqMask]; printf("  [owner] t=%ld reaped res=%d byte=%d\n", ms(), q->res, buf[0]); h++; n++; }
    __atomic_store_n(cqHead, h, __ATOMIC_RELEASE); return n;
}
static int wq;   // the watcher's epoll set: the eventfd (modes 0, 1), or the socket itself (mode 2)
// Mode 2 (KR-412's option a): the owner arms the socket in the watcher's set, one shot, with each RECV it submits.
static void armSocket(int op) { struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT }; epoll_ctl(wq, op, srv, &ev); }
static void* watcher(void* a) {
    (void)a;
    for (;;) {
        struct epoll_event got; int n = epoll_wait(wq, &got, 1, 2000); uint64_t v = 0;
        if (n <= 0) { printf("  [watch] t=%ld epoll timeout\n", ms()); continue; }
        if (mode == 2) printf("  [watch] t=%ld socket readable\n", ms());
        else { (void)!read(efd, &v, sizeof v); printf("  [watch] t=%ld eventfd=%llu\n", ms(), (unsigned long long)v); }
        pthread_mutex_lock(&m); posted = 1; pthread_cond_signal(&c); pthread_mutex_unlock(&m);
        usleep(5000);   // the loop's pass before watching again
    }
    return NULL;
}
static void* writer(void* a) {
    (void)a; uint8_t b = 1;
    usleep(60000); (void)!write(cli, &b, 1); printf("  [write] t=%ld byte 1\n", ms());
    usleep(60000); b = 2; (void)!write(cli, &b, 1); printf("  [write] t=%ld byte 2\n", ms());
    return NULL;
}
int main(int argc, char** argv) {
    // 0 kama's flags, watched through the eventfd; 1 without DEFER_TASKRUN (and SINGLE_ISSUER); 2 kama's flags, the
    // socket watched itself (KR-412's option a)
    mode = argc > 1 ? atoi(argv[1]) : 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    struct io_uring_params p; memset(&p, 0, sizeof p);
    p.flags = mode == 1 ? IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE
                        : IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = 128;
    ringFd = (int)syscall(__NR_io_uring_setup, 64, &p);
    if (ringFd < 0) { perror("setup"); return 1; }
    size_t sl = p.sq_off.array + p.sq_entries * sizeof(unsigned), cl = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t len = sl > cl ? sl : cl;
    uint8_t* r = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringFd, IORING_OFF_SQ_RING);
    sqes = mmap(NULL, p.sq_entries * sizeof *sqes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ringFd, IORING_OFF_SQES);
    sqHead = (unsigned*)(r + p.sq_off.head); sqTail = (unsigned*)(r + p.sq_off.tail); sqMask = (unsigned*)(r + p.sq_off.ring_mask);
    sqArray = (unsigned*)(r + p.sq_off.array); sqLocal = *sqTail;
    cqHead = (unsigned*)(r + p.cq_off.head); cqTail = (unsigned*)(r + p.cq_off.tail); cqMask = (unsigned*)(r + p.cq_off.ring_mask);
    cqes = (struct io_uring_cqe*)(r + p.cq_off.cqes);
    efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (syscall(__NR_io_uring_register, ringFd, IORING_REGISTER_EVENTFD, &efd, 1) != 0) { perror("register"); return 1; }
    int l = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a; bind(l, (void*)&a, sizeof a); listen(l, 1); getsockname(l, (void*)&a, &al);
    cli = socket(AF_INET, SOCK_STREAM, 0); connect(cli, (void*)&a, sizeof a); srv = accept(l, NULL, NULL);
    printf("mode %d (%s), t0=%ld\n", mode,
           mode == 0 ? "kama's flags, the eventfd" : mode == 1 ? "no DEFER_TASKRUN, the eventfd" : "kama's flags, the socket", ms());
    wq = epoll_create1(0);
    if (mode == 2) armSocket(EPOLL_CTL_ADD);
    else { struct epoll_event ev = { .events = EPOLLIN }; epoll_ctl(wq, EPOLL_CTL_ADD, efd, &ev); }
    pthread_t w, wr; pthread_create(&w, NULL, watcher, NULL);
    recvSqe(); enter(1);
    pthread_create(&wr, NULL, writer, NULL);
    for (int got = 0; got < 2;) {
        pthread_mutex_lock(&m);
        struct timespec at; clock_gettime(CLOCK_REALTIME, &at); at.tv_sec += 1;   // the package's wait, bounded at 1 s
        while (!posted) if (pthread_cond_timedwait(&c, &m, &at) != 0) break;
        const int was = posted; posted = 0; pthread_mutex_unlock(&m);
        printf("  [owner] t=%ld wait returned (%s)\n", ms(), was ? "posted" : "timeout");
        enter(0);
        int n = reap(); got += n;
        if (n && got < 2) { recvSqe(); enter(1); if (mode == 2) armSocket(EPOLL_CTL_MOD); }
    }
    return 0;
}
