// The C half of tests/fiber_sched.d (KR-3 step 2): the scheduler in include/kama_sched.h, driven directly until
// `spawn fiber` lands. Each check is a numbered return; 0 is all of them. Compiled twice into one program: as it is,
// and by schedtest_poller.c with KAMA__SCHED_GENERIC, so the arm Windows and poll(2) take is tested on every host.
// Checks 15–22 drive a socket's wait (kama_os.h, KR-3 step 5) the way std's socket calls do, in a program that runs
// fibers — which defines KAMA_FIBERS before any kama header, as the emitter does for one; checks 30 and 32, a stream's
// short read. Checks 23–29 drive a channel's wait (kama_channel.h) and an isolate's join (kama_isolate.h), KR-3 step 6;
// both are read AFTER the scheduler here, the order no emitted program has, so linking this proves the scheduler's half
// of those seams is defined either way.
#define KAMA_FIBERS 1
#include "kama_sched.h"
#include "kama_channel.h"
#include "kama_isolate.h"
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

// 15–22: a socket's wait, as std's calls run it — kama_net_ready before the call, kama_net_retry after it fails.
static int np[2];                                    // a fresh blocking pair per check
static void* nio; static int32_t nmode; static int nread, nwaits;
static int pairUp(void) {
    if (np[0] > 0) { kama_net_close(np[0], NULL); kama_net_close(np[1], NULL); }
    nio = NULL; nmode = 0; nread = 0; nwaits = 0;
    return socketpair(AF_UNIX, SOCK_STREAM, 0, np) == 0;
}
static int nonBlocking(int fd) { return (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0; }
// The socket a scheduler holds, asked without changing it: epoll's MOD and kqueue's delete both say ENOENT for one
// it does not.
static int heldBy(kama_sched* s, int fd) {
#if defined(KAMA__SCHED_EPOLL)
    if (kama__sched_ring(s)) return 0;               // the ring arm registers nothing with the OS (KR-253: every wait is a submission)
    struct epoll_event ev; memset(&ev, 0, sizeof ev); ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    return !(epoll_ctl(((kama__poller*)s->poller)->epfd, EPOLL_CTL_MOD, fd, &ev) != 0 && errno == ENOENT);
#elif defined(KAMA__SCHED_KQUEUE)
    struct kevent d, r; EV_SET(&d, (uintptr_t)fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    return !(kevent(((kama__poller*)s->poller)->kq, &d, 1, &r, 1, NULL) == 1 && (r.flags & EV_ERROR) && r.data == ENOENT);
#else
    (void)s; (void)fd; return 0;                     // the Poller arm holds a socket only while a fiber waits on it
#endif
}
// 15. a read on a fiber: the socket is made non-blocking as kama's own and registered, the read parks, and a write
// from another fiber wakes it.
static void netReader(void* a) {
    (void)a; char c;
    if (kama_net_ready(np[0], &nio, &nmode, 0) != 0) { nread = -1; return; }
    for (;;) {
        ssize_t n = recv(np[0], &c, 1, KAMA__NET_DONTWAIT(nmode));
        if (n > 0) { nread = c; return; }
        nwaits++;
        if (!kama_net_retry(np[0], nio, nmode, 1, 0)) { nread = -2; return; }
    }
}
static void netWriter(void* a) { (void)a; kama_sched_sleep(3000000); char c = 42; (void)send(np[1], &c, 1, 0); }
// 16. a socket the program made non-blocking answers WouldBlock on a fiber: it is not registered and never waits.
static int wouldBlock, unregistered;
static void netProgramNb(void* a) {
    (void)a; char c;
    if (kama_net_set_mode(np[0], nio, &nmode, 1) != 0) return;
    if (kama_net_ready(np[0], &nio, &nmode, 0) != 0) return;
    unregistered = nio == NULL;
    if (recv(np[0], &c, 1, KAMA__NET_DONTWAIT(nmode)) < 0 && !kama_net_retry(np[0], nio, nmode, 1, 0)) wouldBlock = errno == EAGAIN;
}
// 17. a read deadline on a fiber times out, not early; one already passed fails the call before it is made.
static int netTimedOut, netEarly, netExpired;
static void netDeadline(void* a) {
    (void)a; char c;
    const uint64_t by = kama_now_mono_ns() + 20000000ull;
    if (kama_net_ready(np[0], &nio, &nmode, by) != 0) return;
    for (;;) {
        if (recv(np[0], &c, 1, KAMA__NET_DONTWAIT(nmode)) > 0) return;
        if (!kama_net_retry(np[0], nio, nmode, 1, by)) { netTimedOut = errno == ETIMEDOUT; netEarly = kama_now_mono_ns() < by; break; }
    }
    netExpired = kama_net_ready(np[0], &nio, &nmode, by) == -1 && errno == ETIMEDOUT;
}
// 18. on a thread, which runs no scheduler: a deadline waits in poll(2) and times out; a byte before it is read; and
// with no deadline the socket is blocking again, as before fibers.
static int thTimedOut, thEarly, thGot, thBlocking;
static void* netThread(void* a) {
    (void)a; char c; void* io = NULL; int32_t mode = KAMA_NET_DEADLINE;
    uint64_t by = kama_now_mono_ns() + 20000000ull;
    if (kama_net_ready(np[0], &io, &mode, by) != 0 || io || !(mode & KAMA_NET_KAMA_NB)) return NULL;
    for (;;) {
        if (recv(np[0], &c, 1, 0) > 0) return NULL;
        if (!kama_net_retry(np[0], io, mode, 1, by)) { thTimedOut = errno == ETIMEDOUT; thEarly = kama_now_mono_ns() < by; break; }
    }
    by = kama_now_mono_ns() + 2000000000ull;
    c = 9; (void)send(np[1], &c, 1, 0);
    for (;;) {
        if (recv(np[0], &c, 1, 0) > 0) { thGot = c; break; }
        if (!kama_net_retry(np[0], io, mode, 1, by)) break;
    }
    mode &= ~KAMA_NET_DEADLINE;
    thBlocking = kama_net_ready(np[0], &io, &mode, 0) == 0 && mode == 0 && !nonBlocking(np[0]);
    return NULL;
}
// 19. a socket registered here and then used on another isolate: that isolate takes it off this queue itself and
// hands the record back, which the next turn here frees.
static int relHeld, relBlocking;
static void* netElsewhere(void* a) {
    (void)a;
    relBlocking = kama_net_ready(np[0], &nio, &nmode, 0) == 0 && !nio && nmode == 0 && !nonBlocking(np[0]);
    return NULL;
}
static void noop(void* a) { (void)a; }
// 20. a registration whose scheduler has ended: the releaser frees it, and the scheduler with it.
static void* goneIo; static int32_t goneRefs;
static void* goneHome(void* a) {
    (void)a; int32_t mode = 0;
    kama_sched* s = kama_sched_current();                // this thread runs a scheduler, and registers on it
    if (kama_net_ready(np[1], &goneIo, &mode, 0) != 0) return NULL;
    kama_sched_destroy();                                // it ends; the registration still holds it
    goneRefs = __atomic_load_n(&s->refs, __ATOMIC_ACQUIRE);
    return NULL;
}
// 21. kqueue queues no change that outlives its wait: none after registering, none after a write wait that timed out
// before any wait applied it.
static int noChanges;
static void netWriteTimeout(void* a) {
    (void)a;
    kama_sched* s = (kama_sched*)kama__fibers.sched;
    if (kama_net_ready(np[1], &nio, &nmode, 0) != 0) return;
#if defined(KAMA__SCHED_KQUEUE)
    const int fresh = s->nChanges == 0;
    (void)kama_sched_io_wait((kama_sched_io*)nio, 2, kama_now_mono_ns() + 1);
    noChanges = fresh && s->nChanges == 0;
#else
    (void)s; noChanges = 1;
#endif
}
// 22. a connect on a fiber resolves as a blocking one would: to a listener, connected; to a closed port, refused.
static int conOk, conRefused;
static void netConnect(void* a) {
    struct sockaddr_in* to = (struct sockaddr_in*)a;
    for (int pass = 0; pass < 2; pass++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0); void* io = NULL; int32_t mode = 0;
        if (fd < 0 || kama_net_ready(fd, &io, &mode, 0) != 0) return;
        int rc = connect(fd, (struct sockaddr*)&to[pass], sizeof to[pass]);
        if (rc != 0) rc = kama_net_connected(fd, io, mode, kama_now_mono_ns() + 2000000000ull);
        if (pass == 0) conOk = rc == 0;
        else conRefused = rc != 0 && errno == ECONNREFUSED;
        kama_net_close(fd, io);
    }
}
// 30. a stream read that came back short (KR-251): on an edge backend the next read parks first, so it never meets
// EAGAIN (the Poller arm learns nothing from a short read, and meets it once); a byte that arrives after the short read
// and before that park still wakes it; the park keeps the read's deadline; and a socket the program made non-blocking
// answers WouldBlock instead of parking.
#if defined(KAMA__SCHED_POLLER)
enum { EDGE = 0 };
#else
enum { EDGE = 1 };
#endif
static int shortFirst, shortAgain, shortEagains, shortLate, shortTimedOut, shortNb;
static ssize_t shortRead(char* buf) {   // one read of up to 8 bytes, recorded as std's stream read records it
    ssize_t n = recv(np[0], buf, 8, KAMA__NET_DONTWAIT(nmode));
    if (n > 0) kama_net_short(nio, 1, n, 8);
    return n;
}
static void netShortReader(void* a) {
    (void)a; char buf[8];
    if (kama_net_ready(np[0], &nio, &nmode, 0) != 0) return;
    shortFirst = shortRead(buf) == 1;                          // the driver's one byte, of eight asked
    if (kama_net_drained(nio, nmode, 1, 0) != 0) return;       // parks: the writer's byte wakes it
    for (;;) {
        if (shortRead(buf) > 0) { shortAgain = buf[0] == 'b'; break; }
        shortEagains++;
        if (!kama_net_retry(np[0], nio, nmode, 1, 0)) return;
    }
    (void)send(np[1], "c", 1, 0);                              // arrives while this fiber runs: no poll has seen it
    if (kama_net_drained(nio, nmode, 1, kama_now_mono_ns() + 2000000000ull) != 0) return;
    shortLate = shortRead(buf) == 1 && buf[0] == 'c';
    const uint64_t by = kama_now_mono_ns() + 20000000ull;      // nothing more comes
    const int32_t rc = kama_net_drained(nio, nmode, 1, by);
    shortTimedOut = EDGE ? rc == -1 && errno == ETIMEDOUT && kama_now_mono_ns() >= by : rc == 0;
    kama_net_short(nio, 1, 1, 8);
    if (kama_net_set_mode(np[0], nio, &nmode, 1) != 0) return;
    shortNb = kama_net_drained(nio, nmode, 1, 0) == 0 && recv(np[0], buf, 8, KAMA__NET_DONTWAIT(nmode)) < 0
           && !kama_net_retry(np[0], nio, nmode, 1, 0) && errno == EAGAIN;
}
static void netShortWriter(void* a) { (void)a; (void)send(np[1], "b", 1, 0); }
// 32. a short read from a peer that had already closed: an edge-triggered queue reports the close once, with the last
// data, so the read after it must meet the end rather than park for an edge that will never come (the hang TSan's
// timing found in fiber_tcp_bulk; tokio keeps its closed bits for the same reason).
static int eofShort, eofSeen;
static void netShortEof(void* a) {
    (void)a; char buf[8];
    if (kama_net_ready(np[0], &nio, &nmode, 0) != 0) return;
    kama_sched_sleep(1000000);                                 // a poll runs: it reports the byte and the close at once
    eofShort = shortRead(buf) == 1;
    if (kama_net_drained(nio, nmode, 1, kama_now_mono_ns() + 2000000000ull) != 0) return;   // the hang: 2 s, then fails
    eofSeen = recv(np[0], buf, 8, KAMA__NET_DONTWAIT(nmode)) == 0;
}
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

// 23–28: a channel's wait on a fiber, through the calls std's Sender and Receiver make. A channel of ints with one sender
// and one receiver claimed, as `Channel.sender()` and `.receiver()` leave one, and its end: the factory and whatever
// endpoints are left let go, the last of them freeing the queue.
static void* chanOf(size_t cap) {
    void* ch = kama_channel_new(sizeof(int), cap);
    kama_channel_add_sender(ch); kama_channel_add_receiver(ch);
    return ch;
}
static void chanEnd(void* ch, int senders, int receivers) {
    int last = kama_channel_close_factory(ch);
    while (senders-- > 0) last = kama_channel_close_sender(ch);
    while (receivers-- > 0) last = kama_channel_close_receiver(ch);
    if (last) { int v; while (kama_channel_try_pop(ch, &v)) { } kama_channel_free((kama_channel_t*)ch); }
}
// 23. two fibers on one isolate over two rendezvous channels: every receive and every send parks, and the other runs.
static void* chA; static void* chB; static int pingSum, pongN;
static void chanPing(void* a) {
    (void)a;
    for (int i = 1; i <= 1000; i++) {
        int r = 0;
        if (kama_channel_send(chA, &i) != 0 || kama_channel_recv(chB, &r) != 0 || r != i + 1) break;
        pingSum += r;
    }
    (void)kama_channel_close_sender(chA);                     // the answerer hears the close and ends
}
static void chanPong(void* a) {
    (void)a;
    int v;
    while (kama_channel_recv(chA, &v) == 0) { v++; if (kama_channel_send(chB, &v) != 0) return; pongN++; }
}
// 24. a send from another thread wakes a fiber parked in a receive, and the isolate's other fiber runs meanwhile.
static void* chR; static int remoteGot; static long chanTicks;
static void* chanRemoteSender(void* a) { (void)a; kama_sleep_ns(10000000); int v = 24; (void)kama_channel_send(chR, &v); return NULL; }
static void chanRemoteWaiter(void* a) { (void)a; int v = 0; if (kama_channel_recv(chR, &v) == 0) remoteGot = v; }
static void chanTicker(void* a) { (void)a; while (!remoteGot && chanTicks < 100000000) { chanTicks++; kama_sched_yield(); } }
// 25. a full ring parks its sender until the receiver, a fiber on the same isolate, takes one.
static void* chF; static int fedSum, fedInOrder = 1;
static void chanFeeder(void* a) {
    (void)a;
    for (int i = 1; i <= 100; i++) if (kama_channel_send(chF, &i) != 0) return;
    (void)kama_channel_close_sender(chF);
}
static void chanEater(void* a) {
    (void)a;
    int v, want = 1;
    while (kama_channel_recv(chF, &v) == 0) { if (v != want++) fedInOrder = 0; fedSum += v; kama_sched_yield(); }
}
// 26. a timed receive parks with a deadline: it times out no earlier than asked, then receives what a sibling sends late,
// and a third fiber keeps running the whole time.
static void* chT; static int timedOutOk, timedGot, timedTicks;
static void chanTimed(void* a) {
    (void)a;
    int v = 0;
    const uint64_t t = kama_now_mono_ns();
    int r = kama_channel_recv_timeout(chT, &v, 20000000);
    timedOutOk = r == 1 && kama_now_mono_ns() - t >= 20000000;
    r = kama_channel_recv_timeout(chT, &v, 5000000000LL);
    if (r == 0) timedGot = v;
}
// On Windows a fiber's timer resolves to the ~16 ms system tick until step 8, so the late send waits long enough to
// hold several of the ticker's sleeps there, and a few are all the check needs.
static void chanLate(void* a) { (void)a; kama_sched_sleep(100000000); int v = 26; (void)kama_channel_send(chT, &v); }
static void chanTimedTicker(void* a) { (void)a; while (!timedGot) { timedTicks++; kama_sched_sleep(1000000); } }
// 27. the last sender's close wakes every fiber parked in a receive.
static void* chC; static int closedSeen;
static void chanClosedWaiter(void* a) { (void)a; int v; if (kama_channel_recv(chC, &v) == -1) closedSeen++; }
static void chanCloser(void* a) { (void)a; kama_sched_sleep(5000000); (void)kama_channel_close_sender(chC); }
// 28. a receive on another thread wakes a fiber parked in a send on a full ring, and the last receiver's close wakes it
// for good: its send answers -1.
static void* chS; static int stuffed, undelivered;
static void* chanSlowEater(void* a) {
    (void)a;
    int v;
    for (int i = 0; i < 3; i++) { kama_sleep_ns(5000000); (void)kama_channel_recv(chS, &v); }
    kama_sleep_ns(5000000);
    (void)kama_channel_close_receiver(chS);
    return NULL;
}
static void chanStuffer(void* a) {
    (void)a;
    for (int i = 0; i < 10; i++) { if (kama_channel_send(chS, &i) != 0) { undelivered = 1; return; } stuffed++; }
}
// 29. a fiber's join of an isolate parks it until the isolate's thread has ended, and the core runs a sibling meanwhile.
static int isoRan, isoJoined, isoTicks;
static void* isoChild(void* a) { (void)a; kama_sleep_ns(100000000); isoRan = 1; return NULL; }   // long, as 26's
static void isoJoiner(void* a) { kama_isolate_join((kama_isolate_t)a); isoJoined = isoRan; }
static void isoTicker(void* a) { (void)a; while (!isoJoined) { isoTicks++; kama_sched_sleep(1000000); } }

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

    kama_sched* self = kama_sched_current();
    const int32_t baseRefs = self->refs;
    CHECK(150, pairUp());
    kama_sched_detach(kama_sched_spawn(netReader, 0, 0));
    kama_sched_detach(kama_sched_spawn(netWriter, 0, 0));
    kama_sched_run();
    CHECK(15, nread == 42 && nwaits >= 1 && nio && nmode == KAMA_NET_KAMA_NB && nonBlocking(np[0]) && self->refs == baseRefs + 1);
    kama_net_release(nio); nio = NULL;
    CHECK(151, self->refs == baseRefs && !heldBy(self, np[0]));

    CHECK(160, pairUp());
    kama_sched_detach(kama_sched_spawn(netProgramNb, 0, 0));
    kama_sched_run();
    CHECK(16, wouldBlock && unregistered && nmode == KAMA_NET_PROGRAM_NB);

    CHECK(170, pairUp());
    kama_sched_detach(kama_sched_spawn(netDeadline, 0, 0));
    kama_sched_run();
    CHECK(17, netTimedOut && !netEarly && netExpired);
    kama_net_release(nio); nio = NULL;

    CHECK(180, pairUp());
    pthread_t nt;
    CHECK(181, pthread_create(&nt, NULL, netThread, NULL) == 0);
    pthread_join(nt, NULL);
    CHECK(18, thTimedOut && !thEarly && thGot == 9 && thBlocking);

    CHECK(190, pairUp());
    CHECK(191, kama_net_ready(np[0], &nio, &nmode, 0) == 0 && nio);   // on the root: registered here
#if !defined(KAMA__SCHED_POLLER)
    if (!kama__sched_ring(self)) CHECK(192, heldBy(self, np[0]));   // an arm that registers with the OS: it did
#endif
    pthread_t el;
    CHECK(193, pthread_create(&el, NULL, netElsewhere, NULL) == 0);
    pthread_join(el, NULL);
    relHeld = heldBy(self, np[0]);
    const int handedBack = __atomic_load_n(&self->freed, __ATOMIC_ACQUIRE) != NULL;
    kama_sched_detach(kama_sched_spawn(noop, 0, 0));
    kama_sched_run();                                        // the next turn frees what was handed back
    CHECK(19, relBlocking && !relHeld && handedBack && !self->freed && self->refs == baseRefs);

    CHECK(200, pairUp());
    pthread_t gh;
    CHECK(201, pthread_create(&gh, NULL, goneHome, NULL) == 0);
    pthread_join(gh, NULL);
    CHECK(202, goneIo && goneRefs == 1);
    kama_net_release(goneIo);                                // frees the record and the scheduler it held
    CHECK(20, 1);

    CHECK(210, pairUp());
    kama_sched_detach(kama_sched_spawn(netWriteTimeout, 0, 0));
    kama_sched_run();
    CHECK(21, noChanges);
    kama_net_release(nio); nio = NULL;

    {
        struct sockaddr_in to[2]; memset(to, 0, sizeof to);
        int ls = socket(AF_INET, SOCK_STREAM, 0), dead = socket(AF_INET, SOCK_STREAM, 0);
        socklen_t l = sizeof to[0];
        to[0].sin_family = to[1].sin_family = AF_INET;
        to[0].sin_addr.s_addr = to[1].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(220, bind(ls, (struct sockaddr*)&to[0], l) == 0 && listen(ls, 4) == 0 && getsockname(ls, (struct sockaddr*)&to[0], &l) == 0);
        l = sizeof to[1];
        CHECK(221, bind(dead, (struct sockaddr*)&to[1], l) == 0 && getsockname(dead, (struct sockaddr*)&to[1], &l) == 0);
        close(dead);                                         // a port nothing listens on
        kama_sched_detach(kama_sched_spawn(netConnect, to, 0));
        kama_sched_run();
        CHECK(22, conOk && conRefused);
        close(ls);
    }

    CHECK(31, pairUp() && send(np[1], "a", 1, 0) == 1);
    kama_sched_detach(kama_sched_spawn(netShortReader, 0, 0));
    kama_sched_detach(kama_sched_spawn(netShortWriter, 0, 0));
    kama_sched_run();
    CHECK(30, shortFirst && shortAgain && shortEagains == !EDGE && shortLate && shortTimedOut && shortNb);
    kama_net_release(nio); nio = NULL;

    CHECK(33, pairUp() && send(np[1], "z", 1, 0) == 1 && shutdown(np[1], SHUT_WR) == 0);
    kama_sched_detach(kama_sched_spawn(netShortEof, 0, 0));
    kama_sched_run();
    CHECK(32, eofShort && eofSeen);
    kama_net_release(nio); nio = NULL;
    kama_net_close(np[0], NULL); kama_net_close(np[1], NULL); np[0] = np[1] = 0;
#endif

    chA = chanOf(0); chB = chanOf(0);
    kama_sched_detach(kama_sched_spawn(chanPong, 0, 0));
    kama_sched_detach(kama_sched_spawn(chanPing, 0, 0));
    kama_sched_run();
    CHECK(23, pingSum == 501500 && pongN == 1000);
    chanEnd(chA, 0, 1); chanEnd(chB, 1, 1);

    chR = chanOf(1);
    pthread_t rs;
    CHECK(240, pthread_create(&rs, NULL, chanRemoteSender, NULL) == 0);
    kama_sched_detach(kama_sched_spawn(chanRemoteWaiter, 0, 0));
    kama_sched_detach(kama_sched_spawn(chanTicker, 0, 0));
    kama_sched_run();
    pthread_join(rs, NULL);
    CHECK(24, remoteGot == 24 && chanTicks > 0);
    chanEnd(chR, 1, 1);

    chF = chanOf(1);
    kama_sched_detach(kama_sched_spawn(chanFeeder, 0, 0));
    kama_sched_detach(kama_sched_spawn(chanEater, 0, 0));
    kama_sched_run();
    CHECK(25, fedSum == 5050 && fedInOrder);
    chanEnd(chF, 0, 1);

    chT = chanOf(1);
    kama_sched_detach(kama_sched_spawn(chanTimed, 0, 0));
    kama_sched_detach(kama_sched_spawn(chanLate, 0, 0));
    kama_sched_detach(kama_sched_spawn(chanTimedTicker, 0, 0));
    kama_sched_run();
    CHECK(26, timedOutOk && timedGot == 26 && timedTicks >= 3);
    chanEnd(chT, 1, 1);

    chC = chanOf(1);
    for (int i = 0; i < 3; i++) kama_sched_detach(kama_sched_spawn(chanClosedWaiter, 0, 0));
    kama_sched_detach(kama_sched_spawn(chanCloser, 0, 0));
    kama_sched_run();
    CHECK(27, closedSeen == 3);
    chanEnd(chC, 0, 1);

    chS = chanOf(1);
    pthread_t se;
    CHECK(280, pthread_create(&se, NULL, chanSlowEater, NULL) == 0);
    kama_sched_detach(kama_sched_spawn(chanStuffer, 0, 0));
    kama_sched_run();
    pthread_join(se, NULL);
    CHECK(28, stuffed >= 3 && undelivered);
    chanEnd(chS, 1, 0);

    kama_isolate_t ih = kama_isolate_spawn(isoChild, NULL);
    kama_sched_detach(kama_sched_spawn(isoJoiner, ih, 0));
    kama_sched_detach(kama_sched_spawn(isoTicker, 0, 0));
    kama_sched_run();
    CHECK(29, isoJoined && isoTicks >= 3);

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
