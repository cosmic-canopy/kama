// uring.c — what a request costs a fiber scheduler's core on epoll, and on io_uring under each ring shape.
//
// KR-253's measurement before any kama code (docs/benchmarks/uring-*.tsv). It models the epoll arm: a socket is registered
// once, edge-triggered, a read that would block parks its task, and a read that came back short of its buffer parks the
// next one first, since the socket is drained (tokio's rule, kama since 0.9.752). This program runs that protocol and
// the ring shapes the design weighs, with no fibers (a task is a struct; a stack switch is a separate cost), counts
// every system call the server thread makes, and times the server thread's CPU, plus any io-wq worker the kernel ran
// for it, so work moved off the thread is not lost from the count.
//
// Usage: uring <arm> <conns> <requests> <reply-bytes>      one tab-separated line on stdout; `uring -h` prints the header
//   arm  et              epoll, edge-triggered, registered once; the read/write system calls (kama today: the reference)
//        ring-poll       (a) a multishot poll per socket through the ring stands in for epoll; reads and writes stay calls
//        ring-op         (b) the read system call first; a read that would block, or follows a short one, is a RECV into
//                        the task's own buffer, and its completion carries the data; the reply is a write system call
//        ring-send       (b') as ring-op, and the reply is a SEND too: the task parks until it completes
//        ring-op-pf, ring-send-pf   the same, each RECV marked POLL_FIRST (see submitRecv)
//        ring-multishot  (c) one multishot RECV per socket over a provided buffer ring; each completion is copied into the
//                        task's buffer, as `read(into:)` would have to; the reply is a write system call
// Every request is 64 bytes; the reply is <reply-bytes> (64 for echo, 4096 for the raw-HTTP shape). The ring is set up
// as the design rules: SINGLE_ISSUER | DEFER_TASKRUN, its descriptor registered, no fixed files.
// VERIFY=1 also prints the server's counters on stderr, to compare with `strace -c -f`.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum { REQ = 64, BUF = 4096, MAXREPLY = 4096, MAXEV = 1024, CLIENTS = 3 };
enum { ET, RING_POLL, RING_OP, RING_SEND, RING_MULTISHOT };
enum { SQ_ENTRIES = 4096, CQ_ENTRIES = 32768, NBUF = 4096, BGID = 1 };

// The server thread's calls. `waits` is an epoll_wait or an io_uring_enter; `ctl` a registration call (epoll_ctl, or a
// ring registration); `flushes` the io_uring_enter calls made only because the submission queue filled.
typedef struct { uint64_t waits, ctl, reads, writes, again, sqes, cqes, idle, flushes; } counts;
static counts C;

static void die(const char* what) { perror(what); exit(2); }
static uint64_t nowNs(clockid_t clk) { struct timespec t; clock_gettime(clk, &t); return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec; }
static void nonBlocking(int fd) { if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) die("fcntl"); }

// ---- tasks: one per connection, a ready queue --------------------------------------------------------------------
typedef struct {
    int fd;
    int have;          // request bytes read so far
    int sent;          // reply bytes written so far (ring-send)
    bool drained;      // the last read came back short of its buffer: the next one parks first
    bool parked;       // waiting on the kernel (a readiness report, or an operation's completion)
    bool sending;      // ring-send: a SEND is in flight
    bool armed;        // ring-poll, ring-multishot: the multishot request is live
    char buf[BUF];
} task;
static task* T; static int NT; static int arm; static int reply;
static int* rq; static int rqHead, rqTail, rqLen;      // a task is queued at most once, so NT slots suffice
static void push(task* t) { rq[rqTail] = (int)(t - T); rqTail = (rqTail + 1) % NT; rqLen++; }
static task* pop(void) { task* t = &T[rq[rqHead]]; rqHead = (rqHead + 1) % NT; rqLen--; return t; }
static char out[MAXREPLY];

// ---- epoll ---------------------------------------------------------------------------------------------------------
static int ep;
static struct epoll_event evs[MAXEV];

// ---- the ring, by its system calls: no liburing (decision 6) --------------------------------------------------------
static int ringFd, ringIndex;                          // the ring's descriptor, and its registered index
static int* rearm; static int nRearm;                  // tasks whose multishot request the kernel ended
static unsigned *sqHead, *sqTail, *sqMask, *sqArray, *cqHead, *cqTail, *cqMask;
static struct io_uring_sqe* sqes; static struct io_uring_cqe* cqes;
static unsigned sqLocalTail, pending;                  // SQEs written but not yet submitted
static struct io_uring_buf_ring* br; static char* bufPool; static uint16_t brTail;

static int enter(unsigned submit, unsigned minComplete, unsigned flags) {
    return (int)syscall(__NR_io_uring_enter, ringIndex, submit, minComplete, flags | IORING_ENTER_REGISTERED_RING, NULL, 0);
}
static void ringOpen(void) {
    struct io_uring_params p; memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_CQSIZE | IORING_SETUP_SUBMIT_ALL;
    p.cq_entries = CQ_ENTRIES;
    int fd = (int)syscall(__NR_io_uring_setup, SQ_ENTRIES, &p);
    if (fd < 0) die("io_uring_setup (seccomp and SELinux both refuse it in a default container)");
    if (!(p.features & IORING_FEAT_SINGLE_MMAP)) { fprintf(stderr, "io_uring: no SINGLE_MMAP\n"); exit(2); }
    size_t sqSize = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cqSize = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    size_t ringSize = sqSize > cqSize ? sqSize : cqSize;
    char* ring = mmap(NULL, ringSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    if (ring == MAP_FAILED) die("mmap(ring)");
    sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES);
    if (sqes == MAP_FAILED) die("mmap(sqes)");
    sqHead = (unsigned*)(ring + p.sq_off.head); sqTail = (unsigned*)(ring + p.sq_off.tail);
    sqMask = (unsigned*)(ring + p.sq_off.ring_mask); sqArray = (unsigned*)(ring + p.sq_off.array);
    cqHead = (unsigned*)(ring + p.cq_off.head); cqTail = (unsigned*)(ring + p.cq_off.tail);
    cqMask = (unsigned*)(ring + p.cq_off.ring_mask); cqes = (struct io_uring_cqe*)(ring + p.cq_off.cqes);
    sqLocalTail = *sqTail;
    // The ring's own descriptor, registered (5.18): io_uring_enter then skips the descriptor lookup.
    struct io_uring_rsrc_update up; memset(&up, 0, sizeof up); up.offset = -1U; up.data = (uint64_t)fd;
    C.ctl++;
    if (syscall(__NR_io_uring_register, fd, IORING_REGISTER_RING_FDS, &up, 1) != 1) die("IORING_REGISTER_RING_FDS");
    ringIndex = (int)up.offset; ringFd = fd;
    if (arm == RING_MULTISHOT) {
        // A provided buffer ring (5.19): NBUF buffers of BUF bytes, shared by every socket, each handed back after its copy.
        br = mmap(NULL, NBUF * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        bufPool = mmap(NULL, (size_t)NBUF * BUF, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (br == MAP_FAILED || bufPool == MAP_FAILED) die("mmap(buffer ring)");
        struct io_uring_buf_reg reg; memset(&reg, 0, sizeof reg);
        reg.ring_addr = (uint64_t)(uintptr_t)br; reg.ring_entries = NBUF; reg.bgid = BGID;
        C.ctl++;
        if (syscall(__NR_io_uring_register, ringFd, IORING_REGISTER_PBUF_RING, &reg, 1) != 0)
            die("IORING_REGISTER_PBUF_RING");
        for (int i = 0; i < NBUF; i++) {
            struct io_uring_buf* b = &br->bufs[(brTail + i) & (NBUF - 1)];
            b->addr = (uint64_t)(uintptr_t)(bufPool + (size_t)i * BUF); b->len = BUF; b->bid = (uint16_t)i;
        }
        brTail = (uint16_t)(brTail + NBUF);
        __atomic_store_n(&br->tail, brTail, __ATOMIC_RELEASE);
    }
}
static void recycle(uint16_t bid) {
    struct io_uring_buf* b = &br->bufs[brTail & (NBUF - 1)];
    b->addr = (uint64_t)(uintptr_t)(bufPool + (size_t)bid * BUF); b->len = BUF; b->bid = bid;
    brTail++;
    __atomic_store_n(&br->tail, brTail, __ATOMIC_RELEASE);
}
static struct io_uring_sqe* sqe(void) {
    if (sqLocalTail - __atomic_load_n(sqHead, __ATOMIC_ACQUIRE) >= SQ_ENTRIES) {
        C.flushes++; C.waits++;                           // the queue is full: submit now, without waiting
        __atomic_store_n(sqTail, sqLocalTail, __ATOMIC_RELEASE);
        if (enter(pending, 0, IORING_ENTER_GETEVENTS) < 0) die("io_uring_enter(flush)");
        pending = 0;
    }
    unsigned idx = sqLocalTail & *sqMask;
    struct io_uring_sqe* e = &sqes[idx]; memset(e, 0, sizeof *e);
    sqArray[idx] = idx; sqLocalTail++; pending++; C.sqes++;
    return e;
}
enum { TAG_POLL = 1, TAG_RECV = 2, TAG_SEND = 3 };
static uint64_t tag(task* t, int kind) { return ((uint64_t)(t - T) << 2) | (uint64_t)kind; }
static void submitPoll(task* t) {
    struct io_uring_sqe* e = sqe(); e->opcode = IORING_OP_POLL_ADD; e->fd = t->fd;
    e->poll32_events = POLLIN | POLLRDHUP; e->len = IORING_POLL_ADD_MULTI; e->user_data = tag(t, TAG_POLL); t->armed = true;
}
// Every RECV is submitted for a socket known to be empty (a read just failed with EAGAIN, or came back short). The -pf
// arms say so to the kernel (POLL_FIRST, 5.19), which then arms its poll at once instead of trying the receive first:
// the kernel's side of the short-read rule. Whether that pays is measured, not assumed.
static bool pollFirst;
static void submitRecv(task* t) {
    struct io_uring_sqe* e = sqe(); e->opcode = IORING_OP_RECV; e->fd = t->fd;
    if (pollFirst) e->ioprio = IORING_RECVSEND_POLL_FIRST;
    e->addr = (uint64_t)(uintptr_t)(t->buf + t->have); e->len = (unsigned)(BUF - t->have); e->user_data = tag(t, TAG_RECV);
}
static void submitMultishot(task* t) {
    struct io_uring_sqe* e = sqe(); e->opcode = IORING_OP_RECV; e->fd = t->fd;
    e->ioprio = IORING_RECV_MULTISHOT; e->flags = IOSQE_BUFFER_SELECT; e->buf_group = BGID; e->user_data = tag(t, TAG_RECV);
    t->armed = true;
}
static void submitSend(task* t) {
    struct io_uring_sqe* e = sqe(); e->opcode = IORING_OP_SEND; e->fd = t->fd;
    e->addr = (uint64_t)(uintptr_t)(out + t->sent); e->len = (unsigned)(reply - t->sent); e->user_data = tag(t, TAG_SEND);
    t->sending = true;
}

// ---- the server: what a task does when it runs -----------------------------------------------------------------
static uint64_t served, total;
static void replied(task* t) { t->have = 0; served++; }
static void writeReply(task* t) {
    C.writes++;
    if (write(t->fd, out, (size_t)reply) != reply) die("write: a reply did not fit the socket buffer, which this model does not handle");
    replied(t);
}
// A request is complete in t->buf: answer it, the arm's way. Returns false when the task parked on its reply.
static bool answer(task* t) {
    if (arm != RING_SEND) { writeReply(t); return true; }
    t->sent = 0; submitSend(t); t->parked = true; return false;
}
static void run(task* t) {
    for (;;) {
        if (t->have == REQ && !answer(t)) return;
        if (arm == RING_MULTISHOT) { t->parked = true; return; }   // the next request arrives by itself
        if (t->drained) {                                  // the last read came back short: park without asking
            t->parked = true;
            if (arm == RING_OP || arm == RING_SEND) submitRecv(t);
            return;
        }
        C.reads++;
        ssize_t r = read(t->fd, t->buf + t->have, (size_t)(BUF - t->have));
        if (r < 0) {
            if (errno != EAGAIN) die("read");
            C.again++; t->parked = true;
            if (arm == RING_OP || arm == RING_SEND) submitRecv(t);
            return;
        }
        if (r == 0) { fprintf(stderr, "read: a client closed early\n"); exit(2); }
        t->have += (int)r;
        t->drained = r < BUF - (t->have - (int)r);         // short of what it asked for
        if (t->have > REQ) { fprintf(stderr, "read: more than one request in flight, which this model does not send\n"); exit(2); }
    }
}

// A report or a completion for a task.
static void wake(task* t) { if (t->parked) { t->parked = false; push(t); } else C.idle++; }
static void reap(void) {
    unsigned head = *cqHead, tail = __atomic_load_n(cqTail, __ATOMIC_ACQUIRE);
    for (; head != tail; head++) {
        struct io_uring_cqe* c = &cqes[head & *cqMask];
        C.cqes++;
        task* t = &T[c->user_data >> 2];
        int kind = (int)(c->user_data & 3);
        if (kind == TAG_POLL) {
            if (!(c->flags & IORING_CQE_F_MORE)) { t->armed = false; rearm[nRearm++] = (int)(t - T); }
            if (c->res < 0) { errno = -c->res; die("poll completion"); }
            t->drained = false; wake(t);
        } else if (kind == TAG_SEND) {
            if (c->res < 0) { errno = -c->res; die("send completion"); }
            t->sent += c->res;
            if (t->sent < reply) { submitSend(t); continue; }      // a short send: the rest, still parked
            t->sending = false; replied(t); wake(t);
        } else if (arm == RING_MULTISHOT) {
            if (!(c->flags & IORING_CQE_F_MORE)) { t->armed = false; rearm[nRearm++] = (int)(t - T); }
            if (c->res == -ENOBUFS) { C.idle++; continue; }           // re-armed below, once buffers are back
            if (c->res < 0) { errno = -c->res; die("multishot recv completion"); }
            if (c->res == 0) { fprintf(stderr, "recv: a client closed early\n"); exit(2); }
            uint16_t bid = (uint16_t)(c->flags >> IORING_CQE_BUFFER_SHIFT);
            if (t->have + c->res > REQ) { fprintf(stderr, "recv: more than one request in flight\n"); exit(2); }
            memcpy(t->buf + t->have, bufPool + (size_t)bid * BUF, (size_t)c->res);   // into the caller's buffer
            recycle(bid);
            t->have += c->res; wake(t);
        } else {                                           // a RECV into the task's buffer: the data is here
            if (c->res < 0) { errno = -c->res; die("recv completion"); }
            if (c->res == 0) { fprintf(stderr, "recv: a client closed early\n"); exit(2); }
            int asked = BUF - t->have;
            t->have += c->res; t->drained = c->res < asked; wake(t);
        }
    }
    __atomic_store_n(cqHead, head, __ATOMIC_RELEASE);
    while (nRearm > 0) {                                  // a multishot request the kernel ended is armed again
        task* t = &T[rearm[--nRearm]];
        if (arm == RING_POLL) submitPoll(t); else submitMultishot(t);
    }
}
static void serverWait(bool block) {
    C.waits++;
    if (arm == ET) {
        int n = epoll_wait(ep, evs, MAXEV, block ? -1 : 0);
        if (n < 0) { if (errno == EINTR) return; die("epoll_wait"); }
        for (int i = 0; i < n; i++) {
            task* t = &T[evs[i].data.u64];
            if (!(evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))) { C.idle++; continue; }   // writable only
            t->drained = false; wake(t);
        }
        return;
    }
    __atomic_store_n(sqTail, sqLocalTail, __ATOMIC_RELEASE);
    int r = enter(pending, block ? 1 : 0, IORING_ENTER_GETEVENTS);
    if (r < 0) { if (errno == EINTR) return; die("io_uring_enter"); }
    pending = 0;
    reap();
}

// ---- the clients: closed loop, one request in flight per connection, plain level-triggered readiness -------------
typedef struct { int* fds; int n; uint64_t rounds; } client;
static void* clientMain(void* arg) {
    client* c = (client*)arg;
    static const char req[REQ];
    char buf[MAXREPLY];
    size_t* need = calloc((size_t)c->n, sizeof *need); uint64_t* left = calloc((size_t)c->n, sizeof *left);
    struct epoll_event* ev = calloc(MAXEV, sizeof *ev);
    if (!need || !left || !ev) die("calloc");
    int q = epoll_create1(EPOLL_CLOEXEC); if (q < 0) die("epoll_create1");
    for (int i = 0; i < c->n; i++) {
        struct epoll_event e; memset(&e, 0, sizeof e); e.events = EPOLLIN; e.data.u64 = (uint64_t)i;
        if (epoll_ctl(q, EPOLL_CTL_ADD, c->fds[i], &e) != 0) die("epoll_ctl(client)");
        need[i] = (size_t)reply; left[i] = c->rounds;
        if (write(c->fds[i], req, REQ) != REQ) die("write(client)");
    }
    int live = c->n;
    while (live > 0) {
        int n = epoll_wait(q, ev, MAXEV, -1);
        if (n < 0) { if (errno == EINTR) continue; die("client wait"); }
        for (int k = 0; k < n; k++) {
            int i = (int)ev[k].data.u64;
            ssize_t r = read(c->fds[i], buf, need[i]);
            if (r < 0) { if (errno == EAGAIN) continue; die("read(client)"); }
            if (r == 0) { fprintf(stderr, "client: the server closed early\n"); exit(2); }
            need[i] -= (size_t)r;
            if (need[i] > 0) continue;
            if (--left[i] == 0) { live--; continue; }
            need[i] = (size_t)reply;
            if (write(c->fds[i], req, REQ) != REQ) die("write(client)");
        }
    }
    close(q); free(ev); free(need); free(left);
    return NULL;
}

// CPU the kernel spent in io-wq workers (iou-wrk-*) or a polling thread (iou-sqp-*) of this process: work the ring moved
// off the server thread, which CLOCK_THREAD_CPUTIME_ID would not see.
static uint64_t ringThreadsNs(void) {
    DIR* d = opendir("/proc/self/task"); if (!d) return 0;
    uint64_t ticks = 0; struct dirent* e; char path[320], comm[32], stat[1024];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        FILE* f = fopen(path, "r"); if (!f) continue;
        bool ring = fgets(comm, sizeof comm, f) && strncmp(comm, "iou-", 4) == 0; fclose(f);
        if (!ring) continue;
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        f = fopen(path, "r"); if (!f) continue;
        if (fgets(stat, sizeof stat, f)) {
            char* p = strrchr(stat, ')'); unsigned long long ut = 0, st = 0;
            if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) ticks += ut + st;
        }
        fclose(f);
    }
    closedir(d);
    return ticks * (1000000000u / (uint64_t)sysconf(_SC_CLK_TCK));
}

int main(int argc, char** argv) {
    const char* header = "backend\tarm\tconns\treply\trequests\twall_ns\tcpu_ns\twq_ns\twaits\tctl\treads\twrites\tagain\tsqes\tcqes\tidle\tflushes";
    if (argc == 2 && strcmp(argv[1], "-h") == 0) { puts(header); return 0; }
    if (argc != 5) { fprintf(stderr, "usage: uring <arm> <conns> <requests> <reply-bytes>\n%s\n", header); return 2; }
    const char* arms[] = { "et", "ring-poll", "ring-op", "ring-send", "ring-multishot" };
    char base[32]; snprintf(base, sizeof base, "%s", argv[1]);
    size_t bl = strlen(base);
    if (bl > 3 && strcmp(base + bl - 3, "-pf") == 0) { pollFirst = true; base[bl - 3] = 0; }
    arm = -1;
    for (int i = 0; i < 5; i++) if (strcmp(base, arms[i]) == 0) arm = i;
    if (arm < 0 || (pollFirst && arm != RING_OP && arm != RING_SEND)) {
        fprintf(stderr, "arms: et ring-poll ring-op ring-op-pf ring-send ring-send-pf ring-multishot\n"); return 2;
    }
    NT = atoi(argv[2]);
    const uint64_t asked = strtoull(argv[3], NULL, 10);
    reply = atoi(argv[4]);
    if (NT < CLIENTS) { fprintf(stderr, "conns: at least %d\n", CLIENTS); return 2; }
    if (reply < 1 || reply > MAXREPLY) { fprintf(stderr, "reply-bytes: 1..%d\n", MAXREPLY); return 2; }
    uint64_t rounds = asked / (uint64_t)NT; if (rounds == 0) rounds = 1;
    total = rounds * (uint64_t)NT;
    signal(SIGPIPE, SIG_IGN);

    struct rlimit lim; getrlimit(RLIMIT_NOFILE, &lim);
    rlim_t want = (rlim_t)NT * 2 + 64;
    if (lim.rlim_cur < want) { lim.rlim_cur = lim.rlim_max < want ? lim.rlim_max : want; setrlimit(RLIMIT_NOFILE, &lim); }

    // Setup, untimed: NT loopback connections, accepted one at a time so the backlog never fills.
    int ls = socket(AF_INET, SOCK_STREAM, 0); if (ls < 0) die("socket");
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr*)&a, sizeof a) != 0 || listen(ls, 128) != 0 || getsockname(ls, (struct sockaddr*)&a, &al) != 0) die("listen");
    T = calloc((size_t)NT, sizeof *T); rq = calloc((size_t)NT, sizeof *rq); rearm = calloc((size_t)NT, sizeof *rearm);
    int* cfd = calloc((size_t)NT, sizeof *cfd);
    if (!T || !rq || !rearm || !cfd) die("calloc");
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
    memset(out, 'x', sizeof out);
    if (arm == ET) {
        if ((ep = epoll_create1(EPOLL_CLOEXEC)) < 0) die("epoll_create1");
        for (int i = 0; i < NT; i++) {                      // both directions, once (decision 6 of fibers.md)
            C.ctl++;
            struct epoll_event e; memset(&e, 0, sizeof e);
            e.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET; e.data.u64 = (uint64_t)i;
            if (epoll_ctl(ep, EPOLL_CTL_ADD, T[i].fd, &e) != 0) die("epoll_ctl");
        }
    } else {
        ringOpen();
        if (arm == RING_POLL) for (int i = 0; i < NT; i++) submitPoll(&T[i]);
        if (arm == RING_MULTISHOT) for (int i = 0; i < NT; i++) submitMultishot(&T[i]);
        serverWait(false);                                 // the registrations go in before the clock starts
    }
    counts setup = C; memset(&C, 0, sizeof C);

    client cl[CLIENTS]; pthread_t th[CLIENTS];
    for (int k = 0; k < CLIENTS; k++) {
        cl[k].n = NT / CLIENTS + (k < NT % CLIENTS);
        cl[k].fds = cfd + (k * (NT / CLIENTS) + (k < NT % CLIENTS ? k : NT % CLIENTS));
        cl[k].rounds = rounds;
    }
    const uint64_t wq0 = ringThreadsNs();
    const uint64_t w0 = nowNs(CLOCK_MONOTONIC), c0 = nowNs(CLOCK_THREAD_CPUTIME_ID);
    for (int k = 0; k < CLIENTS; k++) if (pthread_create(&th[k], NULL, clientMain, &cl[k]) != 0) die("pthread_create");
    for (int i = 0; i < NT; i++) { if (arm == RING_MULTISHOT) T[i].parked = true; else push(&T[i]); }   // every task starts with a read
    while (served < total) {
        for (int k = rqLen; k > 0 && served < total; k--) run(pop());
        if (served < total) serverWait(rqLen == 0);
    }
    const uint64_t c1 = nowNs(CLOCK_THREAD_CPUTIME_ID), w1 = nowNs(CLOCK_MONOTONIC);
    const uint64_t wq1 = ringThreadsNs();
    for (int k = 0; k < CLIENTS; k++) pthread_join(th[k], NULL);

    printf("%s\t%s\t%d\t%d\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
           arm == ET ? "epoll" : "io_uring", argv[1], NT, reply, (unsigned long long)total,
           (unsigned long long)(w1 - w0), (unsigned long long)(c1 - c0), (unsigned long long)(wq1 - wq0),
           (unsigned long long)C.waits, (unsigned long long)C.ctl, (unsigned long long)C.reads, (unsigned long long)C.writes,
           (unsigned long long)C.again, (unsigned long long)C.sqes, (unsigned long long)C.cqes, (unsigned long long)C.idle,
           (unsigned long long)C.flushes);
    if (getenv("VERIFY")) {
        fprintf(stderr, "setup  waits=%llu ctl=%llu sqes=%llu\n", (unsigned long long)setup.waits, (unsigned long long)setup.ctl, (unsigned long long)setup.sqes);
        fprintf(stderr, "server waits=%llu reads=%llu writes=%llu again=%llu sqes=%llu cqes=%llu idle=%llu flushes=%llu\n",
                (unsigned long long)C.waits, (unsigned long long)C.reads, (unsigned long long)C.writes, (unsigned long long)C.again,
                (unsigned long long)C.sqes, (unsigned long long)C.cqes, (unsigned long long)C.idle, (unsigned long long)C.flushes);
    }
    for (int i = 0; i < NT; i++) { close(cfd[i]); close(T[i].fd); }
    return 0;
}
