// The C half of tests/fiber_sched.d (KR-3 step 2): the scheduler in include/kama_sched.h, driven directly until
// `spawn fiber` lands. Each check is a numbered return; 0 is all of them. Compiled twice into one program: as it is,
// and by schedtest_poller.c with KAMA__SCHED_GENERIC, so the arm Windows and poll(2) take is tested on every host.
#include "kama_sched.h"
#include "schedtest.h"
#include <stdio.h>
#if !defined(_WIN32)
#include <sys/socket.h>
#endif

static int failed;
#define CHECK(n, cond) do { if (!(cond) && !failed) failed = (n); } while (0)

// 1. park and unpark: two fibers hand a turn back and forth. A wake reaches only a parked fiber, so each looks at the
// turn before it parks, which is the shape every waiter has.
static kama_fiber* pingF; static kama_fiber* pongF; static int pings, turn;
static void ping(void* a) {
    (void)a;
    for (int i = 0; i < 1000; i++) {
        while (turn != 0) kama_sched_park();
        pings++; turn = 1; kama_sched_unpark(pongF);
    }
}
static void pong(void* a) {
    (void)a;
    for (int i = 0; i < 1000; i++) {
        while (turn != 1) kama_sched_park();
        turn = 0; kama_sched_unpark(pingF);
    }
}

// 2. yield is round-robin.
static char order[64]; static int orderN;
static void yielder(void* a) { for (int i = 0; i < 3; i++) { order[orderN++] = (char)(intptr_t)a; kama_sched_yield(); } }

// 3. sleep: three fibers wake in deadline order, none early.
static int woke[3]; static int wokeN; static uint64_t t0; static int early;
static void sleeper(void* a) {
    const int ms = (int)(intptr_t)a;
    kama_sched_sleep((uint64_t)ms * 1000000ull);
    if (kama_now_mono_ns() - t0 < (uint64_t)ms * 1000000ull) early = 1;
    woke[wokeN++] = ms;
}

// 4. join yields the child's work.
static int childOut;
static void child(void* a) { kama_sched_sleep(1000000); childOut = (int)(intptr_t)a * 2; }
static int joined;
static void parent(void* a) { (void)a; kama_fiber* c = kama_sched_spawn(child, (void*)(intptr_t)21, 0); kama_sched_join(c); joined = childOut; }

#if !defined(_WIN32)
// 5. a reader parks on a socket and a writer wakes it; a writer parks on a full buffer and the reader drains it.
static int sv[2]; static kama_sched_io* rio; static kama_sched_io* wio;
static int readGot; static int readWaits;
static void reader(void* a) {
    (void)a; char buf[16];
    for (;;) {
        ssize_t n = recv(sv[0], buf, sizeof buf, 0);
        if (n > 0) { readGot = buf[0]; return; }
        if (n < 0 && errno == EAGAIN) { readWaits++; kama_sched_io_wait(rio, 1, 0); continue; }
        readGot = -1; return;
    }
}
static void writer(void* a) { (void)a; kama_sched_sleep(5000000); char c = 42; (void)send(sv[1], &c, 1, 0); }
static long filled, drained; static int writeWaits;
static void filler(void* a) {
    (void)a; static char chunk[4096]; long goal = 4l << 20;
    while (filled < goal) {
        size_t want = goal - filled < (long)sizeof chunk ? (size_t)(goal - filled) : sizeof chunk;
        ssize_t n = send(sv[1], chunk, want, 0);
        if (n > 0) { filled += n; continue; }
        if (errno == EAGAIN) { writeWaits++; kama_sched_io_wait(wio, 2, 0); continue; }
        return;
    }
    shutdown(sv[1], SHUT_WR);
}
static void drainer(void* a) {
    (void)a; static char buf[65536];
    for (;;) {
        ssize_t n = recv(sv[0], buf, sizeof buf, 0);
        if (n > 0) { drained += n; continue; }
        if (n == 0) return;
        if (errno == EAGAIN) { kama_sched_io_wait(rio, 1, 0); continue; }
        return;
    }
}
// 6. a read with a deadline and no data times out, not early.
static int timedOut; static int tooEarly;
static void deadliner(void* a) {
    (void)a; char c;
    const uint64_t start = kama_now_mono_ns(), deadline = start + 20000000ull;
    for (;;) {
        if (recv(sv[0], &c, 1, 0) > 0) return;
        if (!kama_sched_io_wait(rio, 1, deadline)) { timedOut = 1; tooEarly = kama_now_mono_ns() < deadline; return; }
    }
}
// 7. a remote wake from another thread ends a park with no deadline.
static kama_fiber* waitingF; static volatile int remoteDone; static int remoteSeen;
static void* remoteThread(void* a) {
    (void)a;
    while (!__atomic_load_n(&waitingF, __ATOMIC_ACQUIRE)) { }
    kama_sleep_ns(10000000);
    __atomic_store_n(&remoteDone, 1, __ATOMIC_RELEASE);
    kama_sched_wake(waitingF);
    return NULL;
}
static void remoteWaiter(void* a) {
    (void)a;
    __atomic_store_n(&waitingF, kama_fiber_current(), __ATOMIC_RELEASE);
    while (!__atomic_load_n(&remoteDone, __ATOMIC_ACQUIRE)) kama_sched_park();
    remoteSeen = 1;
}
// 8. I/O is polled every KAMA_SCHED_POLL_EVERY runs, even with a fiber always ready.
static volatile int readerRan; static long spins;
static void busy(void* a) { (void)a; while (!readerRan && spins < 1000000) { spins++; kama_sched_yield(); } }
static void starved(void* a) { (void)a; char c; while (recv(sv[0], &c, 1, 0) <= 0) kama_sched_io_wait(rio, 1, 0); readerRan = 1; }
static void* lateWriter(void* a) { (void)a; kama_sleep_ns(5000000); char c = 7; (void)send(sv[1], &c, 1, 0); return NULL; }
#endif

// 9. ten thousand fibers, each sleeping a moment, all end and are freed.
static int many;
static void brief(void* a) { kama_sched_sleep((uint64_t)((intptr_t)a % 7) * 100000ull); many++; }

// 11–14. the root parks: a join, a `scope`'s wait (on the root and on a fiber), a timed sleep, each running the loop
// inside the park until it is woken (decision 1).
static int scoped, inner, nested;
static void counted(void* a) { kama_sched_sleep((uint64_t)((intptr_t)a % 5) * 100000ull); scoped++; }
static void leaf(void* a) { (void)a; kama_sched_yield(); inner++; }
static void nest(void* a) {
    (void)a;
    kama_fiber_scope in = { 0, 0 };
    for (int i = 0; i < 3; i++) kama_sched_scopeSpawn(&in, leaf, 0, 0);
    kama_sched_scopeWait(&in);
    nested = inner == 3 && !in.head;
}

int fiber_sched_selftest(void) {
    kama_sched_current();
    pingF = kama_sched_spawn(ping, 0, 0); pongF = kama_sched_spawn(pong, 0, 0);
    kama_sched_detach(pingF); kama_sched_detach(pongF);
    kama_sched_run();
    CHECK(1, pings == 1000);

    for (int i = 0; i < 3; i++) kama_sched_detach(kama_sched_spawn(yielder, (void*)(intptr_t)('a' + i), 0));
    kama_sched_run();
    CHECK(2, orderN == 9 && memcmp(order, "abcabcabc", 9) == 0);

    t0 = kama_now_mono_ns();
    kama_sched_detach(kama_sched_spawn(sleeper, (void*)(intptr_t)30, 0));
    kama_sched_detach(kama_sched_spawn(sleeper, (void*)(intptr_t)10, 0));
    kama_sched_detach(kama_sched_spawn(sleeper, (void*)(intptr_t)20, 0));
    kama_sched_run();
    CHECK(3, wokeN == 3 && woke[0] == 10 && woke[1] == 20 && woke[2] == 30 && !early);

    kama_sched_detach(kama_sched_spawn(parent, 0, 0));
    kama_sched_run();
    CHECK(4, joined == 42);

#if !defined(_WIN32)
    CHECK(50, socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    for (int i = 0; i < 2; i++) { int fl = fcntl(sv[i], F_GETFL); fcntl(sv[i], F_SETFL, fl | O_NONBLOCK); }
    rio = kama_sched_io_open(sv[0]); wio = kama_sched_io_open(sv[1]);
    CHECK(51, rio && wio);
    kama_sched_detach(kama_sched_spawn(reader, 0, 0));
    kama_sched_detach(kama_sched_spawn(writer, 0, 0));
    kama_sched_run();
    CHECK(5, readGot == 42 && readWaits >= 1);

    kama_sched_detach(kama_sched_spawn(deadliner, 0, 0));
    kama_sched_run();
    CHECK(6, timedOut && !tooEarly);

    pthread_t th;
    CHECK(70, pthread_create(&th, NULL, remoteThread, NULL) == 0);
    kama_sched_detach(kama_sched_spawn(remoteWaiter, 0, 0));
    kama_sched_run();
    pthread_join(th, NULL);
    CHECK(7, remoteSeen);

    pthread_t lw;
    CHECK(80, pthread_create(&lw, NULL, lateWriter, NULL) == 0);
    kama_sched_detach(kama_sched_spawn(starved, 0, 0));
    kama_sched_detach(kama_sched_spawn(busy, 0, 0));
    kama_sched_run();
    pthread_join(lw, NULL);
    CHECK(8, readerRan && spins < 1000000);

    { char junk[64]; while (recv(sv[0], junk, sizeof junk, 0) > 0) { } }   // what the checks above left unread
    kama_sched_detach(kama_sched_spawn(filler, 0, 0));
    kama_sched_detach(kama_sched_spawn(drainer, 0, 0));
    kama_sched_run();
    CHECK(10, filled == 4l << 20 && drained == filled && writeWaits >= 1);
    kama_sched_io_close(rio); kama_sched_io_close(wio);
    close(sv[0]); close(sv[1]);
#endif

    childOut = 0;
    kama_fiber* c = kama_sched_spawn(child, (void*)(intptr_t)7, 0);
    kama_sched_join(c);                                       // on the root: the park runs `child`
    CHECK(11, childOut == 14);

    kama_fiber_scope sc = { 0, 0 };
    for (int i = 0; i < 100; i++) kama_sched_scopeSpawn(&sc, counted, (void*)(intptr_t)i, 64 << 10);
    kama_sched_scopeWait(&sc);
    CHECK(12, scoped == 100 && !sc.head && !sc.waiter);

    const uint64_t t1 = kama_now_mono_ns();
    kama_sched_sleep(2000000);                                // a timed park on the root, nothing else live
    CHECK(13, kama_now_mono_ns() - t1 >= 2000000);

    kama_fiber_scope outer = { 0, 0 };
    kama_sched_scopeSpawn(&outer, nest, 0, 0);                // a scope inside a fiber, inside a scope on the root
    kama_sched_scopeWait(&outer);
    CHECK(14, nested);

    for (int i = 0; i < 10000; i++) kama_sched_detach(kama_sched_spawn(brief, (void*)(intptr_t)i, 64 << 10));
    kama_sched_run();
    CHECK(9, many == 10000);
    kama_sched_destroy();
    return failed;
}
