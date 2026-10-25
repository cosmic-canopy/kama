#ifndef KAMA_SCHED_H
#define KAMA_SCHED_H

// The fiber scheduler (KR-3, docs/design/fibers.md §8 step 2): one per isolate, over the fiber seam (kama_fiber.h) and
// the readiness poller (kama_os.h). An isolate that runs fibers runs them all on its own thread, so nothing here is
// shared between isolates except the remote queue, which another isolate uses to wake one of this isolate's fibers.
//
// Pay for what you use, as kama_fiber.h: a program that `extern "kama_sched.h";`s pulls it in, and a program that only
// uses sockets or isolates never compiles it.
//
// THE LOOP runs on the thread's own context (the root fiber, fiber 0): take the next ready fiber, switch to it, and
// when it switches back — it parked, yielded or ended — take the next. When nothing is ready it waits for I/O, a timer
// or a remote wake, for no longer than the earliest timer. The root parks too (a `scope`'s closing brace, a join), and
// a park on the root RUNS the loop, inside the park, until something wakes it: the root is never queued, so a
// `scope` runs the same C whether it is entered on the thread's own context or on a fiber (decision 1). Cooperative (decision 12): a fiber runs until it parks or
// yields, and I/O is polled every KAMA_SCHED_POLL_EVERY runs even while fibers are ready, so two fibers that keep
// each other ready cannot starve a socket.
//
// A WAKE IS A HINT. A parked fiber can be woken early by anything that held it, so every wait is a loop: retry the
// I/O, look at the condition again, park again.
//
// REGISTRATION follows decision 6, per backend, and it is internal: the public `Poller` stays level-triggered.
//   epoll    edge-triggered, registered once for reading and writing when the socket meets the scheduler, so a park
//            costs no system call. Readiness that arrives while no fiber waits is kept on the registration.
//   kqueue   EV_CLEAR on reading, registered once; writing is asked for only while a writer waits (one-shot). Every
//            change rides the next wait's changelist, so a park costs no system call either.
//   Poller   everything else — AFD on Windows, poll(2) on another POSIX — through the public `Poller`: a socket is
//            in it only while a fiber waits on it, so a level-triggered backend never reports one nobody reads.
//
// TIMERS are a min-heap of parked fibers on their deadlines (decision 11). A fiber woken before its deadline leaves
// the heap at once, so the heap never holds a fiber that is not waiting.
//
// LIFETIMES. A fiber's control block is counted: its owner (a joiner, or the scheduler for a detached one) holds one
// reference, and a remote wake holds another until this isolate has taken it off the remote queue — so a wake that
// arrives as its fiber ends never reaches freed memory. Those references are released only on this isolate's thread,
// which keeps a fiber's stack on its own thread's idle list. The scheduler itself is held by its thread and by every
// fiber it has made, so a remote wake can always reach it.

#include "kama_runtime.h"
#include "kama_os.h"
#include "kama_fiber.h"
#include "kama_time.h"

#if !defined(__EMSCRIPTEN__) && !defined(KAMA_FIBER_NONE)
#if defined(__linux__)
  #include <sys/syscall.h>   /* SYS_epoll_pwait2 */
#endif

#if defined(__linux__) && !defined(KAMA__SCHED_GENERIC)
  #define KAMA__SCHED_EPOLL 1
#elif (defined(__APPLE__) || defined(__FreeBSD__)) && !defined(KAMA__SCHED_GENERIC)
  #define KAMA__SCHED_KQUEUE 1
#else
  #define KAMA__SCHED_POLLER 1      /* KAMA__SCHED_GENERIC forces this arm elsewhere, so the tests reach it too */
#endif

// Decision 12: Go checks its global queue every 61 scheduler ticks; this starts there.
#define KAMA_SCHED_POLL_EVERY 61
#define KAMA__SCHED_EVENTS 128

typedef struct kama_sched kama_sched;
static inline void kama_sched_destroy(void);
// A `scope`'s fiber children (decision 10): the ones spawned into it that have not ended, a list each unlinks itself
// from as it ends, so a scope that spawns in a loop holds only its live ones.
typedef struct kama_fiber_scope { kama_fiber* head; kama_fiber* waiter; } kama_fiber_scope;
// One socket's registration with this isolate's scheduler: made once, closed before the socket is.
typedef struct kama_sched_io {
    ptrdiff_t fd;
    kama_sched* s;
    kama_fiber* reader;             // the fiber waiting to read, or NULL
    kama_fiber* writer;             // ...to write
    int32_t ready;                  // edge backends: readiness reported while no fiber waited (1 read, 2 write)
    int32_t armed;                  // the Poller arm: the interest the Poller holds now (0: not in it)
} kama_sched_io;

struct kama_sched {
    kama_fiber* head;               // the ready queue, FIFO, through kama_fiber.next
    kama_fiber* tail;
    kama_fiber** heap;              // the timer heap, on kama_fiber.deadline
    int32_t heapN, heapCap;
    int32_t live;                   // fibers made here that have not ended
    uint32_t ticks;                 // runs since the last poll
    int32_t refs;                   // the thread, and each fiber control block not yet freed
    kama_fiber* remote;             // woken from another isolate: a stack, pushed with CAS, taken whole
    void* poller;                   // kama_poller_create: the OS queue, and the waker registered with it
    void* waker;                    // token 0; a socket's token is its kama_sched_io
#if defined(KAMA__SCHED_EPOLL)
    struct epoll_event ev[KAMA__SCHED_EVENTS];
    int noPwait2;                   // the kernel predates epoll_pwait2 (5.11): millisecond waits
#elif defined(KAMA__SCHED_KQUEUE)
    struct kevent ev[KAMA__SCHED_EVENTS];
    struct kevent* changes;         // the next wait's changelist
    int32_t nChanges, capChanges;
#else
    void* events;                   // kama_poll_events_new
#endif
};

// ---- the scheduler ----------------------------------------------------------------------------------------------
static inline void kama__sched_unref(kama_sched* s) {
    if (__atomic_sub_fetch(&s->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    kama__waker_release(s->waker);
    kama_poller_free(s->poller);
#if defined(KAMA__SCHED_KQUEUE)
    if (s->changes) kama_free(s->changes, (size_t)s->capChanges * sizeof *s->changes, _Alignof(struct kevent));
#elif defined(KAMA__SCHED_POLLER)
    kama_poll_events_free(s->events);
#endif
    if (s->heap) kama_free(s->heap, (size_t)s->heapCap * sizeof *s->heap, _Alignof(kama_fiber*));
    kama_free(s, sizeof *s, _Alignof(kama_sched));
}

// This isolate's scheduler, made the first time it is asked for.
static inline kama_sched* kama_sched_current(void) {
    kama_fiber_thread* t = &kama__fibers;
    if (t->sched) return (kama_sched*)t->sched;
    kama_fiber_root();
    kama_sched* s = (kama_sched*)kama_alloc(sizeof *s, _Alignof(kama_sched));
    if (!s) kama_panic(kama_string_lit("fiber scheduler: out of memory", 30));
    memset(s, 0, sizeof *s);
    s->refs = 1;
    s->poller = kama_poller_create();
    if (s->poller) s->waker = kama_poller_waker(s->poller, 0);
#if defined(KAMA__SCHED_POLLER)
    if (s->waker) s->events = kama_poll_events_new(KAMA__SCHED_EVENTS);
    if (!s->events) s->waker = NULL;
#endif
    if (!s->waker) kama_panic(kama_string_lit("fiber scheduler: the OS readiness queue could not be made", 57));
    t->sched = s;
    t->root.sched = s; t->root.heapIx = -1; t->root.refs = 1;   // the root parks here too, and is never freed
    t->schedEnd = kama_sched_destroy;
    return s;
}

// The fiber's last reference is gone: its stack goes back to this thread's idle list, and it lets go of its scheduler.
static inline void kama__sched_drop(kama_fiber* f) {
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) != 0) return;
    kama_sched* s = (kama_sched*)f->sched;
    kama_fiber_free(f);
    kama__sched_unref(s);
}

static inline void kama__sched_push(kama_sched* s, kama_fiber* f) {
    f->next = NULL;
    if (s->tail) s->tail->next = f; else s->head = f;
    s->tail = f;
}
static inline kama_fiber* kama__sched_pop(kama_sched* s) {
    kama_fiber* f = s->head;
    if (f) { s->head = f->next; if (!s->head) s->tail = NULL; f->next = NULL; }
    return f;
}

// ---- timers: a binary min-heap; each fiber knows its slot, so leaving it early is O(log n) ----------------------
static inline void kama__sched_heapPut(kama_sched* s, int32_t i, kama_fiber* f) { s->heap[i] = f; f->heapIx = i; }
static inline void kama__sched_heapUp(kama_sched* s, int32_t i) {
    kama_fiber* f = s->heap[i];
    while (i > 0) {
        int32_t p = (i - 1) / 2;
        if (s->heap[p]->deadline <= f->deadline) break;
        kama__sched_heapPut(s, i, s->heap[p]); i = p;
    }
    kama__sched_heapPut(s, i, f);
}
static inline void kama__sched_heapDown(kama_sched* s, int32_t i) {
    kama_fiber* f = s->heap[i];
    for (;;) {
        int32_t c = 2 * i + 1;
        if (c >= s->heapN) break;
        if (c + 1 < s->heapN && s->heap[c + 1]->deadline < s->heap[c]->deadline) c++;
        if (f->deadline <= s->heap[c]->deadline) break;
        kama__sched_heapPut(s, i, s->heap[c]); i = c;
    }
    kama__sched_heapPut(s, i, f);
}
static inline void kama__sched_timerAdd(kama_sched* s, kama_fiber* f) {
    if (s->heapN == s->heapCap) {
        int32_t nc = s->heapCap ? s->heapCap * 2 : 64;
        kama_fiber** h = (kama_fiber**)kama_alloc((size_t)nc * sizeof *h, _Alignof(kama_fiber*));
        if (!h) kama_panic(kama_string_lit("fiber scheduler: out of memory", 30));
        if (s->heapN) kama_copy(h, s->heap, (size_t)s->heapN * sizeof *h);
        if (s->heap) kama_free(s->heap, (size_t)s->heapCap * sizeof *h, _Alignof(kama_fiber*));
        s->heap = h; s->heapCap = nc;
    }
    s->heap[s->heapN] = f; f->heapIx = s->heapN++;
    kama__sched_heapUp(s, f->heapIx);
}
static inline void kama__sched_timerDel(kama_sched* s, kama_fiber* f) {
    int32_t i = f->heapIx;
    if (i < 0) return;
    f->heapIx = -1;
    kama_fiber* last = s->heap[--s->heapN];
    if (i < s->heapN) { kama__sched_heapPut(s, i, last); kama__sched_heapUp(s, i); kama__sched_heapDown(s, last->heapIx); }
}
static inline void kama__sched_fireTimers(kama_sched* s, uint64_t now) {
    while (s->heapN && s->heap[0]->deadline <= now) {
        kama_fiber* f = s->heap[0];
        kama__sched_timerDel(s, f);
        f->timedOut = 1; f->parked = 0;
        if (f != &kama__fibers.root) kama__sched_push(s, f);   // the root's park sees `parked` clear and returns
    }
}

// ---- parking ----------------------------------------------------------------------------------------------------
// Make `f`, parked on THIS isolate's scheduler, ready. One that is not parked is left alone: a wake is a hint.
static inline void kama_sched_unpark(kama_fiber* f) {
    if (!f->parked) return;
    f->parked = 0;
    kama_sched* s = (kama_sched*)f->sched;
    if (f->heapIx >= 0) kama__sched_timerDel(s, f);
    if (f != &kama__fibers.root) kama__sched_push(s, f);
}

// The running fiber, the root included, on this isolate's scheduler (made on the root's first park).
static inline kama_fiber* kama__sched_self(const char* where, size_t n) {
    kama_fiber* f = kama_fiber_current();
    if (f == &kama__fibers.root) (void)kama_sched_current();
    if (!f->sched) kama_panic(kama_string_lit(where, n));
    return f;
}
static inline void kama__sched_turn(kama_sched* s);
// The park rule's run-time depth (KR-3 step 4, fibers.md decision 3). The compiler refuses a park inside a lasting
// access to `Shared` storage and inside an `@onPanic` region, so the fiber giving up the thread holds neither — which is
// why a switch saves no state. A debug build checks it at both ways a fiber gives the thread up.
static inline void kama__sched_parkClean(void) {
#ifndef NDEBUG
    static const char acc[] = "fiber scheduler: a park inside an open access to a `Shared` value (the park rule missed it)";
    if (kama_accesses.head) kama_panic(kama_string_lit(acc, sizeof acc - 1));
#if defined(KAMA_ONPANIC)
    static const char rec[] = "fiber scheduler: a park inside an `@onPanic` region (the park rule missed it)";
    if (kama_recover_top) kama_panic(kama_string_lit(rec, sizeof rec - 1));
#endif
#endif
}
// Park the running fiber until something unparks it, or until `deadline` (a kama_now_mono_ns time; 0 for none).
// 1 when it was woken, 0 when the deadline passed. On the root, the park runs the loop until then.
static inline int kama_sched_parkUntil(uint64_t deadline) {
    static const char msg[] = "fiber scheduler: a park outside a fiber";
    kama_fiber* f = kama__sched_self(msg, sizeof msg - 1);
    kama_sched* s = (kama_sched*)f->sched;
    kama__sched_parkClean();
    f->parked = 1; f->timedOut = 0;
    if (deadline) { f->deadline = deadline; kama__sched_timerAdd(s, f); }
    if (f == &kama__fibers.root) { while (f->parked) kama__sched_turn(s); }
    else kama_fiber_switch(f, &kama__fibers.root);
    return !f->timedOut;
}
static inline void kama_sched_park(void) { (void)kama_sched_parkUntil(0); }
// Let every other ready fiber run once, then carry on.
static inline void kama_sched_yield(void) {
    static const char msg[] = "fiber scheduler: a yield outside a fiber";
    kama_fiber* f = kama__sched_self(msg, sizeof msg - 1);
    if (f == &kama__fibers.root) kama_panic(kama_string_lit(msg, sizeof msg - 1));
    kama__sched_parkClean();
    kama__sched_push((kama_sched*)f->sched, f);
    kama_fiber_switch(f, &kama__fibers.root);
}
// At least `ns` nanoseconds.
static inline void kama_sched_sleep(uint64_t ns) {
    const uint64_t until = kama_now_mono_ns() + ns;
    while (kama_sched_parkUntil(until) && kama_now_mono_ns() < until) { }
}

// Wake `f` from ANY isolate. On its own isolate it is an unpark; from another, `f` goes onto its scheduler's remote
// queue with a reference of its own, and that scheduler's waker ends its wait. A second wake while one is in flight is
// the same wake. The caller must hold `f` alive while it calls — a waiter list does, by taking it off under its lock.
static inline void kama_sched_wake(kama_fiber* f) {
    kama_sched* s = (kama_sched*)f->sched;
    if ((void*)s == kama__fibers.sched) { kama_sched_unpark(f); return; }
    int32_t idle = 0;
    if (!__atomic_compare_exchange_n(&f->remoteQueued, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    __atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);
    kama_fiber* head = __atomic_load_n(&s->remote, __ATOMIC_RELAXED);
    do { f->remoteNext = head; } while (!__atomic_compare_exchange_n(&s->remote, &head, f, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    kama__waker_wake(s->waker);
}
static inline void kama__sched_drainRemote(kama_sched* s) {
    kama_fiber* f = __atomic_exchange_n(&s->remote, (kama_fiber*)NULL, __ATOMIC_ACQUIRE);
    kama_fiber* fifo = NULL;
    while (f) { kama_fiber* n = f->remoteNext; f->remoteNext = fifo; fifo = f; f = n; }
    while (fifo) {
        kama_fiber* n = fifo->remoteNext;
        fifo->remoteNext = NULL;
        __atomic_store_n(&fifo->remoteQueued, 0, __ATOMIC_RELEASE);
        kama_sched_unpark(fifo);
        kama__sched_drop(fifo);
        fifo = n;
    }
}

// ---- I/O --------------------------------------------------------------------------------------------------------
static inline void kama__sched_ioReady(kama_sched_io* io, int32_t bits) {
    if (bits & 1) { if (io->reader) { kama_fiber* f = io->reader; io->reader = NULL; kama_sched_unpark(f); } else io->ready |= 1; }
    if (bits & 2) { if (io->writer) { kama_fiber* f = io->writer; io->writer = NULL; kama_sched_unpark(f); } else io->ready |= 2; }
}

#if defined(KAMA__SCHED_KQUEUE)
static inline void kama__sched_change(kama_sched* s, ptrdiff_t fd, int16_t filter, uint16_t flags, void* io) {
    if (s->nChanges == s->capChanges) {
        int32_t nc = s->capChanges ? s->capChanges * 2 : 32;
        struct kevent* c = (struct kevent*)kama_alloc((size_t)nc * sizeof *c, _Alignof(struct kevent));
        if (!c) kama_panic(kama_string_lit("fiber scheduler: out of memory", 30));
        if (s->nChanges) kama_copy(c, s->changes, (size_t)s->nChanges * sizeof *c);
        if (s->changes) kama_free(s->changes, (size_t)s->capChanges * sizeof *c, _Alignof(struct kevent));
        s->changes = c; s->capChanges = nc;
    }
    EV_SET(&s->changes[s->nChanges], (uintptr_t)fd, filter, flags, 0, 0, io);
    s->nChanges++;
}
#endif
#if defined(KAMA__SCHED_POLLER)
// The Poller holds a socket only while a fiber waits on it, for what the waiting fibers want.
static inline void kama__sched_arm(kama_sched_io* io) {
    const int32_t want = (io->reader ? 1 : 0) | (io->writer ? 2 : 0);
    if (want == io->armed) return;
    if (want == 0) { (void)kama_poller_remove(io->s->poller, io->fd); io->armed = 0; return; }
    if (kama_poller_add(io->s->poller, io->fd, want, (uint64_t)(uintptr_t)io) == 0) io->armed = want;
    else { io->armed = 0; kama__sched_ioReady(io, want); }   // it cannot be watched: the retry meets the error
}
#endif

// Register `fd` with this isolate's scheduler. NULL when the OS refuses it (errno set): a regular file, which no
// readiness queue watches (those go to the offload pool, decision 8).
static inline kama_sched_io* kama_sched_io_open(ptrdiff_t fd) {
    kama_sched* s = kama_sched_current();
    kama_sched_io* io = (kama_sched_io*)kama_alloc(sizeof *io, _Alignof(kama_sched_io));
    if (!io) { errno = ENOMEM; return NULL; }
    memset(io, 0, sizeof *io);
    io->fd = fd; io->s = s;
#if defined(KAMA__SCHED_EPOLL)
    struct epoll_event ev; memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    ev.data.ptr = io;
    if (epoll_ctl(((kama__poller*)s->poller)->epfd, EPOLL_CTL_ADD, (int)fd, &ev) != 0) {
        const int e = errno; kama_free(io, sizeof *io, _Alignof(kama_sched_io)); errno = e; return NULL;
    }
#elif defined(KAMA__SCHED_KQUEUE)
    kama__sched_change(s, fd, EVFILT_READ, EV_ADD | EV_CLEAR, io);
#endif
    return io;
}
// Take `io` off the scheduler, before its descriptor closes (decision 13: epoll watches the open file description, so
// a descriptor dup'd elsewhere would keep a closed one's registration alive). A fiber still waiting on it is woken,
// and its retry meets the closed socket.
static inline void kama_sched_io_close(kama_sched_io* io) {
    kama_sched* s = io->s;
    kama_fiber* r = io->reader; kama_fiber* w = io->writer;
    io->reader = io->writer = NULL;
#if defined(KAMA__SCHED_EPOLL)
    struct epoll_event ev; memset(&ev, 0, sizeof ev);
    (void)epoll_ctl(((kama__poller*)s->poller)->epfd, EPOLL_CTL_DEL, (int)io->fd, &ev);
#elif defined(KAMA__SCHED_KQUEUE)
    int32_t k = 0;                                    // a change not yet applied would name freed memory
    for (int32_t i = 0; i < s->nChanges; i++) if (s->changes[i].udata != (void*)io) s->changes[k++] = s->changes[i];
    s->nChanges = k;
    struct kevent del[2];
    EV_SET(&del[0], (uintptr_t)io->fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    EV_SET(&del[1], (uintptr_t)io->fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    struct kevent res[2];
    (void)kevent(((kama__poller*)s->poller)->kq, del, 2, res, 2, NULL);   // ENOENT for one never added: fine
#else
    if (io->armed) (void)kama_poller_remove(s->poller, io->fd);
#endif
    kama_free(io, sizeof *io, _Alignof(kama_sched_io));
    if (r) kama_sched_unpark(r);
    if (w) kama_sched_unpark(w);
}

// After an I/O call on `io` reported WouldBlock: park until it may be ready for `interest` (1 read, 2 write), or until
// `deadline` (0 for none). 1 when it may be ready — a hint: retry the call — and 0 when the deadline passed.
static inline int kama_sched_io_wait(kama_sched_io* io, int32_t interest, uint64_t deadline) {
    static const char msg[] = "fiber scheduler: an I/O wait outside a fiber";
    kama_fiber* f = kama__sched_self(msg, sizeof msg - 1);
#if !defined(KAMA__SCHED_POLLER)
    if (io->ready & interest) { io->ready &= ~interest; return 1; }   // it came while nobody waited
#endif
    if (interest & 1) io->reader = f; else io->writer = f;
#if defined(KAMA__SCHED_KQUEUE)
    if (interest & 2) kama__sched_change(io->s, io->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, io);
#elif defined(KAMA__SCHED_POLLER)
    kama__sched_arm(io);
#endif
    const int woke = kama_sched_parkUntil(deadline);
    if (io->reader == f) io->reader = NULL;          // timed out, or woken by something else: not waiting now
    if (io->writer == f) io->writer = NULL;
#if defined(KAMA__SCHED_POLLER)
    kama__sched_arm(io);
#endif
    return woke;
}

// Wait for I/O, a remote wake or `timeoutNs` (-1: until something comes), and make ready what came.
static inline void kama__sched_wait(kama_sched* s, int64_t timeoutNs) {
#if defined(KAMA__SCHED_EPOLL)
    kama__poller* p = (kama__poller*)s->poller;
    int n = -1;
#if defined(SYS_epoll_pwait2)
    // A nanosecond timeout (decision 11): epoll_wait's is in milliseconds, which a timer heap rounds up a whole one.
    if (!s->noPwait2 && timeoutNs >= 0) {
        struct timespec ts; ts.tv_sec = (time_t)(timeoutNs / 1000000000); ts.tv_nsec = (long)(timeoutNs % 1000000000);
        n = (int)syscall(SYS_epoll_pwait2, p->epfd, s->ev, KAMA__SCHED_EVENTS, &ts, NULL, 0);
        if (n < 0 && errno == ENOSYS) s->noPwait2 = 1;
    }
    if (s->noPwait2 || timeoutNs < 0)
#endif
    {
        int timeoutMs = timeoutNs < 0 ? -1 : (int)((timeoutNs + 999999) / 1000000);   // rounded up: never early
        n = epoll_wait(p->epfd, s->ev, KAMA__SCHED_EVENTS, timeoutMs);
    }
    for (int i = 0; i < n; i++) {
        kama_sched_io* io = (kama_sched_io*)s->ev[i].data.ptr;
        if (!io) {                                    // the waker (token 0): clear, then drain, as kama_poller_wait does
            kama__waker* w = (kama__waker*)s->waker; uint64_t v;
            kama__waker_seen(w);
            while (read(w->efd, &v, sizeof v) == (long)sizeof v) { }
            continue;
        }
        const uint32_t ev = s->ev[i].events;
        int32_t bits = 0;
        if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) bits |= 1;
        if (ev & (EPOLLOUT | EPOLLHUP | EPOLLERR)) bits |= 2;
        kama__sched_ioReady(io, bits);
    }
#elif defined(KAMA__SCHED_KQUEUE)
    kama__poller* p = (kama__poller*)s->poller;
    struct timespec ts; struct timespec* pts = NULL;
    if (timeoutNs >= 0) { ts.tv_sec = (time_t)(timeoutNs / 1000000000); ts.tv_nsec = (long)(timeoutNs % 1000000000); pts = &ts; }
    int n = kevent(p->kq, s->changes, s->nChanges, s->ev, KAMA__SCHED_EVENTS, pts);
    s->nChanges = 0;                                  // applied, whatever the wait then returned
    for (int i = 0; i < n; i++) {
        const struct kevent* k = &s->ev[i];
        if (k->filter == EVFILT_USER) { kama__waker_seen((kama__waker*)s->waker); continue; }
        kama_sched_io* io = (kama_sched_io*)k->udata;
        if (!io) continue;
        int32_t bits = k->filter == EVFILT_READ ? 1 : k->filter == EVFILT_WRITE ? 2 : 0;
        if (k->flags & EV_ERROR) bits = 3;            // a change was refused: the retry meets the reason
        if (k->flags & EV_EOF) bits |= 1;
        kama__sched_ioReady(io, bits);
    }
#else
    int32_t timeoutMs = timeoutNs < 0 ? -1 : (int32_t)((timeoutNs + 999999) / 1000000);
    int32_t n = kama_poller_wait(s->poller, s->events, timeoutMs);
    for (int32_t i = 0; i < n; i++) {
        const uint64_t token = kama_poll_events_token(s->events, i);
        if (token == 0) continue;                    // the waker, already cleared and drained by the Poller
        kama_sched_io* io = (kama_sched_io*)(uintptr_t)token;
        kama__sched_ioReady(io, kama_poll_events_bits(s->events, i));
        kama__sched_arm(io);
    }
#endif
}

// ---- fibers -----------------------------------------------------------------------------------------------------
// A fiber that will run `entry(arg)` on this isolate's scheduler, ready now. Join it or detach it.
static inline kama_fiber* kama_sched_spawn(void (*entry)(void*), void* arg, size_t stack) {
    kama_sched* s = kama_sched_current();
    kama_fiber* f = kama_fiber_new(entry, arg, stack);
    f->sched = s; f->refs = 1; f->heapIx = -1; f->parked = 0; f->timedOut = 0; f->detached = 0;
    f->remoteQueued = 0; f->joiner = NULL; f->remoteNext = NULL; f->deadline = 0;
    f->scope = NULL; f->scopePrev = NULL; f->scopeNext = NULL;
    __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
    s->live++;
    kama__sched_push(s, f);
    return f;
}
// Park until `f` has ended, then free it. From a fiber of the same isolate, or its root.
static inline void kama_sched_join(kama_fiber* f) {
    static const char msg[] = "fiber scheduler: a join outside a fiber";
    kama_fiber* me = kama__sched_self(msg, sizeof msg - 1);
    while (!f->done) { f->joiner = me; kama_sched_park(); }
    f->joiner = NULL;
    kama__sched_drop(f);
}
// Nobody will join `f`: the scheduler frees it when it ends.
static inline void kama_sched_detach(kama_fiber* f) {
    f->detached = 1;
    if (f->done) kama__sched_drop(f);
}

// One turn of the loop, on the root: the next ready fiber runs until it switches back, or with none ready, a wait for
// I/O, a timer or a remote wake. A fiber that ended leaves its scope's list, and wakes its scope's waiter when it was
// the last, and its joiner.
static inline void kama__sched_turn(kama_sched* s) {
    const int8_t rootParked = kama__fibers.root.parked;
    kama__sched_drainRemote(s);
    kama__sched_fireTimers(s, s->heapN ? kama_now_mono_ns() : 0);
    if (rootParked && !kama__fibers.root.parked) return;   // the root's own wake came, and is never queued: no wait
    kama_fiber* f = kama__sched_pop(s);
    if (f) {
        if (++s->ticks >= KAMA_SCHED_POLL_EVERY) { s->ticks = 0; kama__sched_wait(s, 0); }
        kama_fiber_switch(&kama__fibers.root, f);
        if (f->done) {
            s->live--;
            kama_fiber_scope* sc = (kama_fiber_scope*)f->scope;
            if (sc) {
                if (f->scopePrev) f->scopePrev->scopeNext = f->scopeNext; else sc->head = f->scopeNext;
                if (f->scopeNext) f->scopeNext->scopePrev = f->scopePrev;
                f->scope = NULL;
                if (!sc->head && sc->waiter) kama_sched_unpark(sc->waiter);
            }
            if (f->joiner) kama_sched_unpark(f->joiner);
            if (f->detached) kama__sched_drop(f);
        }
        return;
    }
    int64_t timeout = -1;
    if (s->heapN) {
        const uint64_t now = kama_now_mono_ns(), at = s->heap[0]->deadline;
        timeout = at > now ? (int64_t)(at - now) : 0;
    }
    kama__sched_wait(s, timeout);
}
// Run this isolate's fibers until every one has ended. On the thread's own context, not on a fiber.
static inline void kama_sched_run(void) {
    kama_sched* s = kama_sched_current();
    if (kama_fiber_current() != &kama__fibers.root) kama_panic(kama_string_lit("fiber scheduler: run from inside a fiber", 40));
    while (s->live) kama__sched_turn(s);
}

// ---- scopes -----------------------------------------------------------------------------------------------------
// A fiber spawned into `sc`: the scope never joins its children one by one, so the scheduler frees each as it ends.
static inline void kama_sched_scopeSpawn(kama_fiber_scope* sc, void (*entry)(void*), void* arg, size_t stack) {
    kama_fiber* f = kama_sched_spawn(entry, arg, stack);
    f->detached = 1;
    f->scope = sc; f->scopeNext = sc->head;
    if (sc->head) sc->head->scopePrev = f;
    sc->head = f;
}
// A `scope`'s closing brace: park until every fiber spawned into it has ended — on a fiber, or on the root, whose park
// runs them.
static inline void kama_sched_scopeWait(kama_fiber_scope* sc) {
    kama_fiber* me = kama_fiber_current();
    while (sc->head) { sc->waiter = me; kama_sched_park(); }
    sc->waiter = NULL;
}
// Let go of this isolate's scheduler once its fibers are done; it is freed when the last of them is.
static inline void kama_sched_destroy(void) {
    kama_sched* s = (kama_sched*)kama__fibers.sched;
    if (!s) return;
    kama__sched_drainRemote(s);
    kama__fibers.sched = NULL; kama__fibers.root.sched = NULL; kama__fibers.schedEnd = NULL;
    kama__sched_unref(s);
}

#endif /* !__EMSCRIPTEN__ && !KAMA_FIBER_NONE */
#endif /* KAMA_SCHED_H */
