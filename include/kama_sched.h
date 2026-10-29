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
//   kqueue   EV_CLEAR on reading, registered once, when the socket meets the scheduler; writing is asked for only
//            while a writer waits (one-shot), and that change rides the next wait's changelist, so a park costs no
//            system call either. No queued change outlives the wait that asked for it (see RELEASE).
//   Poller   everything else — AFD on Windows, poll(2) on another POSIX — through the public `Poller`: a socket is
//            in it only while a fiber waits on it, so a level-triggered backend never reports one nobody reads.
//   io_uring Linux 6.1 on, chosen when the scheduler is made, with epoll as the fallback (KR-253,
//            docs/design/io_uring.md): no registration at all. A fiber that waits submits its wait to the ring, and
//            the scheduler's one `io_uring_enter` a turn submits every wait of the turn and harvests what completed.
//            Every submission belongs to a parked fiber that waits for its own completion (decision 10), so a
//            completion never names freed memory, and a scheduler ends with nothing in flight.
//
// TIMERS are a min-heap of parked fibers on their deadlines (decision 11). A fiber woken before its deadline leaves
// the heap at once, so the heap never holds a fiber that is not waiting.
//
// LIFETIMES. A fiber's control block is counted: its owner (a joiner, or the scheduler for a detached one) holds one
// reference, and a remote wake holds another until this isolate has taken it off the remote queue — so a wake that
// arrives as its fiber ends never reaches freed memory. Those references are released only on this isolate's thread,
// which keeps a fiber's stack on its own thread's idle list. The scheduler itself is held by its thread, by every
// fiber it has made and by every socket registration, so a remote wake or a release can always reach it.
//
// RELEASE (decision 13, as corrected 2026-10-26). A socket can move to another isolate (it is `Sendable`), and nothing
// runs at the hand-off. So the isolate that next uses or drops a socket registered here takes it off this queue
// itself — an epoll or kqueue delete is a system call any thread may make — and pushes the record onto `freed`, which
// this scheduler empties at the top of its next turn: a batch of events it is reading may still name the record.
// Once this scheduler has ended, `freed` holds a mark, and the releaser frees the record itself. That needs no change
// queued here to name a record outside the wait that queued it, which is why kqueue registers reading at once.

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
  // The ring arm needs 6.1's headers (IORING_SETUP_DEFER_TASKRUN is their newest name it uses); older ones build epoll
  // alone. Whether the running kernel allows the ring is a probe, when a scheduler is made (decision 6).
  #if defined(__has_include)
    #if __has_include(<linux/io_uring.h>)
      #include <linux/io_uring.h>
      #if defined(IORING_SETUP_DEFER_TASKRUN)
        #define KAMA__SCHED_URING 1
        #include <sys/mman.h>
      #endif
    #endif
  #endif
#elif (defined(__APPLE__) || defined(__FreeBSD__)) && !defined(KAMA__SCHED_GENERIC)
  #define KAMA__SCHED_KQUEUE 1
#else
  #define KAMA__SCHED_POLLER 1      /* KAMA__SCHED_GENERIC forces this arm elsewhere, so the tests reach it too */
#endif

// Decision 12: Go checks its global queue every 61 scheduler ticks; this starts there.
#define KAMA_SCHED_POLL_EVERY 61
#define KAMA__SCHED_EVENTS 128
// The ring's queues. A full submission queue is submitted at once, without waiting. A completion queue that overflows
// is kept by the kernel (IORING_FEAT_NODROP) and handed over at the next wait, so these sizes cost speed when exceeded,
// never a completion. The M4's measurement saw about 40 completions a wait (io_uring.md §6). 80 KiB per scheduler.
#define KAMA__SCHED_SQ 256
#define KAMA__SCHED_CQ 4096

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
    int32_t drained;                // edge backends: the last call in a direction came back short (1 read, 2 write)
    int32_t closed;                 // edge backends: a close or an error was reported for a direction, once and for good
    int32_t armed;                  // the Poller arm: the interest the Poller holds now (0: not in it)
    int32_t inflight;               // the ring arm: its submissions not yet completed, which a release must find 0
    struct kama_sched_io* freedNext; // released by another isolate: the scheduler's `freed` list
} kama_sched_io;
#define KAMA__SCHED_GONE ((kama_sched_io*)(uintptr_t)1)   // `freed` once the scheduler has ended

struct kama_sched {
    kama_fiber* head;               // the ready queue, FIFO, through kama_fiber.next
    kama_fiber* tail;
    kama_fiber** heap;              // the timer heap, on kama_fiber.deadline
    int32_t heapN, heapCap;
    int32_t live;                   // fibers made here that have not ended
    uint32_t ticks;                 // runs since the last poll
    int32_t refs;                   // the thread, and each fiber control block not yet freed
    kama_fiber* remote;             // woken from another isolate: a stack, pushed with CAS, taken whole
    kama_sched_io* freed;           // registrations another isolate released: a stack, pushed with CAS, freed here
    void* poller;                   // kama_poller_create: the OS queue, and the waker registered with it
    void* waker;                    // token 0; a socket's token is its kama_sched_io
#if defined(KAMA__SCHED_EPOLL)
    struct epoll_event ev[KAMA__SCHED_EVENTS];
    int noPwait2;                   // the kernel predates epoll_pwait2 (5.11): millisecond waits
#if defined(KAMA__SCHED_URING)
    // The ring arm, when the probe found it (`sqes` set); epoll otherwise. The Poller above stays made either way: it
    // holds the waker, whose eventfd the ring watches instead of the epoll set.
    struct io_uring_sqe* sqes;      // NULL: this scheduler runs epoll
    struct io_uring_cqe* cqes;
    unsigned *sqHead, *sqTail, *sqMask, *sqArray, *cqHead, *cqTail, *cqMask;
    unsigned sqLocal;               // the next submission's slot, ahead of the kernel's tail until the next enter
    int ringFd, ringIx;             // the ring's descriptor, and its registered index (decision 5)
    void* ringMap; size_t ringLen, sqesLen;
#endif
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
#if defined(KAMA__SCHED_URING)
    if (s->sqes) {                                    // first: closing it ends the waker's poll, the one left in flight
        munmap(s->sqes, s->sqesLen); munmap(s->ringMap, s->ringLen); close(s->ringFd);
    }
#endif
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

#if defined(KAMA__SCHED_URING)
// The ring arm's setup and probe (decisions 6 and 7): 0 with the ring mapped and registered, -1 to run epoll. Any
// refusal is "unavailable": ENOSYS (Podman's seccomp, or no io_uring), EPERM (Docker's seccomp, or
// kernel.io_uring_disabled), EACCES (an SELinux policy), EINVAL (a kernel before 6.1), or an operation the probe lacks.
// KAMA_IO_URING=0 skips it, read here, once per scheduler.
static inline int kama__sched_ringOpen(kama_sched* s) {
    const char* off = getenv("KAMA_IO_URING");
    if (off && off[0] == '0' && off[1] == 0) return -1;
    struct io_uring_params p; memset(&p, 0, sizeof p);
    p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_SUBMIT_ALL | IORING_SETUP_CQSIZE;
    p.cq_entries = KAMA__SCHED_CQ;
    const int fd = (int)syscall(__NR_io_uring_setup, KAMA__SCHED_SQ, &p);
    if (fd < 0) return -1;
    const unsigned feats = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP | IORING_FEAT_EXT_ARG;
    _Alignas(struct io_uring_probe) unsigned char pb[sizeof(struct io_uring_probe) + 32 * sizeof(struct io_uring_probe_op)];
    memset(pb, 0, sizeof pb);
    struct io_uring_probe* pr = (struct io_uring_probe*)(void*)pb;
    static const uint8_t need[] = { IORING_OP_POLL_ADD, IORING_OP_ASYNC_CANCEL, IORING_OP_RECV, IORING_OP_SEND };
    int ok = (p.features & feats) == feats && syscall(__NR_io_uring_register, fd, IORING_REGISTER_PROBE, pr, 32) == 0;
    for (size_t i = 0; ok && i < sizeof need; i++)
        ok = need[i] <= pr->last_op && (pr->ops[need[i]].flags & IO_URING_OP_SUPPORTED);
    const size_t sqLen = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    const size_t cqLen = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    const size_t len = sqLen > cqLen ? sqLen : cqLen, sqesLen = p.sq_entries * sizeof(struct io_uring_sqe);
    char* ring = ok ? (char*)mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQ_RING)
                    : (char*)MAP_FAILED;
    void* sqes = ring != MAP_FAILED
        ? mmap(NULL, sqesLen, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, IORING_OFF_SQES) : MAP_FAILED;
    // The ring's own descriptor, registered (5.18): each io_uring_enter then skips the descriptor lookup.
    struct io_uring_rsrc_update up; memset(&up, 0, sizeof up); up.offset = -1U; up.data = (uint64_t)fd;
    if (sqes == MAP_FAILED || syscall(__NR_io_uring_register, fd, IORING_REGISTER_RING_FDS, &up, 1) != 1) {
        if (sqes != MAP_FAILED) munmap(sqes, sqesLen);
        if (ring != MAP_FAILED) munmap(ring, len);
        close(fd);
        return -1;
    }
    s->ringFd = fd; s->ringIx = (int)up.offset; s->ringMap = ring; s->ringLen = len; s->sqesLen = sqesLen;
    s->sqes = (struct io_uring_sqe*)sqes;
    s->cqes = (struct io_uring_cqe*)(void*)(ring + p.cq_off.cqes);
    s->sqHead = (unsigned*)(void*)(ring + p.sq_off.head); s->sqTail = (unsigned*)(void*)(ring + p.sq_off.tail);
    s->sqMask = (unsigned*)(void*)(ring + p.sq_off.ring_mask); s->sqArray = (unsigned*)(void*)(ring + p.sq_off.array);
    s->cqHead = (unsigned*)(void*)(ring + p.cq_off.head); s->cqTail = (unsigned*)(void*)(ring + p.cq_off.tail);
    s->cqMask = (unsigned*)(void*)(ring + p.cq_off.ring_mask);
    s->sqLocal = *s->sqTail;
    return 0;
}
static inline void kama__sched_ringWaker(kama_sched* s);
#endif

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
#if defined(KAMA__SCHED_URING)
    if (kama__sched_ringOpen(s) == 0) kama__sched_ringWaker(s);
#endif
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

// Whether `s` runs the ring arm: built with it, and its probe passed when `s` was made.
static inline int kama__sched_ring(const kama_sched* s) {
#if defined(KAMA__SCHED_URING)
    return s->sqes != NULL;
#else
    (void)s; return 0;
#endif
}

#if defined(KAMA__SCHED_URING)
// ---- the ring arm (KR-253) --------------------------------------------------------------------------------------
// A submission's record, on the stack of the fiber that waits for it (decision 10): its completion's user_data is this
// record's address, and the fiber leaves its wait only once that completion has arrived. Two user_data values name no
// record: 0, a cancel's own completion, and the waker's poll.
typedef struct kama__ring_op { kama_fiber* f; int32_t res; int32_t done; } kama__ring_op;
#define KAMA__RING_WAKER 1u

// Make ready every fiber whose submission completed, and keep the waker's poll armed.
static inline void kama__sched_ringReap(kama_sched* s) {
    unsigned head = *s->cqHead;
    const unsigned tail = __atomic_load_n(s->cqTail, __ATOMIC_ACQUIRE);
    int rearm = 0;
    for (; head != tail; head++) {
        const struct io_uring_cqe* c = &s->cqes[head & *s->cqMask];
        if (c->user_data == KAMA__RING_WAKER) {       // clear, then drain, as the epoll arm does
            kama__waker* w = (kama__waker*)s->waker; uint64_t v;
            kama__waker_seen(w);
            while (read(w->efd, &v, sizeof v) == (long)sizeof v) { }
            if (!(c->flags & IORING_CQE_F_MORE)) rearm = 1;   // the kernel ended the multishot poll
            continue;
        }
        if (!c->user_data) continue;                  // a cancel's own completion
        kama__ring_op* op = (kama__ring_op*)(uintptr_t)c->user_data;
        op->res = c->res; op->done = 1;
        kama_sched_unpark(op->f);
    }
    __atomic_store_n(s->cqHead, head, __ATOMIC_RELEASE);
    if (rearm) kama__sched_ringWaker(s);
}
// Submit what was queued, wait up to `timeoutNs` for a completion (-1: until one comes; 0: not at all), and reap.
static inline void kama__sched_ringEnter(kama_sched* s, int64_t timeoutNs) {
    __atomic_store_n(s->sqTail, s->sqLocal, __ATOMIC_RELEASE);
    struct __kernel_timespec ts; struct io_uring_getevents_arg arg; memset(&arg, 0, sizeof arg);
    if (timeoutNs > 0) {                              // a nanosecond timeout (decision 11), as epoll_pwait2's
        ts.tv_sec = timeoutNs / 1000000000; ts.tv_nsec = timeoutNs % 1000000000;
        arg.ts = (uint64_t)(uintptr_t)&ts;
    }
    const unsigned pending = s->sqLocal - __atomic_load_n(s->sqHead, __ATOMIC_ACQUIRE);
    const long r = syscall(__NR_io_uring_enter, s->ringIx, pending, timeoutNs != 0 ? 1u : 0u,
                           IORING_ENTER_GETEVENTS | IORING_ENTER_REGISTERED_RING | IORING_ENTER_EXT_ARG, &arg, sizeof arg);
    // A signal, the timeout, or completions the kernel holds back until these are reaped (EBUSY): reap, and the loop
    // goes round. Anything else is a defect here, not a state to wait out.
    if (r < 0 && errno != EINTR && errno != ETIME && errno != EBUSY && errno != EAGAIN) {
        static const char msg[] = "fiber scheduler: io_uring_enter failed";
        kama_panic(kama_string_lit(msg, sizeof msg - 1));
    }
    kama__sched_ringReap(s);
}
// The next submission's slot, zeroed. A full queue is submitted first, without waiting.
static inline struct io_uring_sqe* kama__sched_sqe(kama_sched* s) {
    while (s->sqLocal - __atomic_load_n(s->sqHead, __ATOMIC_ACQUIRE) > *s->sqMask) kama__sched_ringEnter(s, 0);
    const unsigned i = s->sqLocal & *s->sqMask;
    struct io_uring_sqe* e = &s->sqes[i];
    memset(e, 0, sizeof *e);
    s->sqArray[i] = i;
    s->sqLocal++;
    return e;
}
// The waker's eventfd, watched by one multishot poll for the scheduler's life (decision 9).
static inline void kama__sched_ringWaker(kama_sched* s) {
    struct io_uring_sqe* e = kama__sched_sqe(s);
    e->opcode = IORING_OP_POLL_ADD; e->fd = ((kama__waker*)s->waker)->efd;
    e->poll32_events = EPOLLIN; e->len = IORING_POLL_ADD_MULTI; e->user_data = KAMA__RING_WAKER;
}
// Park until `op` has completed (decision 10). A deadline that passes first submits a cancel and keeps waiting, since
// the kernel may hold the fiber's buffer until the completion arrives. That completion carries -ECANCELED, or the
// result the kernel finished first, and either is the caller's.
static inline void kama__sched_ringPark(kama_sched* s, kama__ring_op* op, uint64_t deadline) {
    while (!op->done) {
        if (kama_sched_parkUntil(deadline) || op->done) continue;   // woken: by the completion, or a hint
        struct io_uring_sqe* e = kama__sched_sqe(s);
        e->opcode = IORING_OP_ASYNC_CANCEL; e->addr = (uint64_t)(uintptr_t)op;   // user_data 0: see the reap
        deadline = 0;
    }
}
// The ring arm's wait for readiness (decision 9): a one-shot poll on the socket, parked on until it completes. 1 when
// it may be ready (a hint, as epoll's: the retry looks), 0 when the deadline cancelled it.
static inline int kama__sched_ringPoll(kama_sched_io* io, int32_t interest, uint64_t deadline) {
    kama_sched* s = io->s;
    kama__ring_op op; op.f = kama_fiber_current(); op.res = 0; op.done = 0;
    struct io_uring_sqe* e = kama__sched_sqe(s);
    e->opcode = IORING_OP_POLL_ADD; e->fd = (int)io->fd;
    e->poll32_events = (interest & 1) ? EPOLLIN | EPOLLRDHUP : EPOLLOUT;   // a hang-up and an error come regardless
    e->user_data = (uint64_t)(uintptr_t)&op;
    io->inflight++;
    kama__sched_ringPark(s, &op, deadline);
    io->inflight--;
    if (op.res == -ECANCELED) return 0;
    if (op.res > 0) {                                 // a close or an error, kept as the epoll arm keeps it
        if (op.res & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) io->closed |= 1;
        if (op.res & (EPOLLHUP | EPOLLERR)) io->closed |= 2;
    }
    return 1;
}
#endif

// A socket released with a submission in flight would let the kernel complete into a record that is gone. The
// language rules it out (a parked fiber borrows its socket, so it cannot close or move), and a debug build checks.
static inline void kama__sched_ioIdle(const kama_sched_io* io) {
#ifndef NDEBUG
    static const char msg[] = "fiber scheduler: a socket released while the kernel holds an operation on it";
    if (io->inflight) kama_panic(kama_string_lit(msg, sizeof msg - 1));
#else
    (void)io;
#endif
}

// ---- I/O --------------------------------------------------------------------------------------------------------
static inline void kama__sched_ioReady(kama_sched_io* io, int32_t bits) {
    if (bits & 1) { if (io->reader) { kama_fiber* f = io->reader; io->reader = NULL; kama_sched_unpark(f); } else io->ready |= 1; }
    if (bits & 2) { if (io->writer) { kama_fiber* f = io->writer; io->writer = NULL; kama_sched_unpark(f); } else io->ready |= 2; }
}

#if defined(KAMA__SCHED_KQUEUE)
// Drop every queued change that names `io`.
static inline void kama__sched_purge(kama_sched* s, kama_sched_io* io) {
    int32_t k = 0;
    for (int32_t i = 0; i < s->nChanges; i++) if (s->changes[i].udata != (void*)io) s->changes[k++] = s->changes[i];
    s->nChanges = k;
}
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
    if (!kama__sched_ring(s) && epoll_ctl(((kama__poller*)s->poller)->epfd, EPOLL_CTL_ADD, (int)fd, &ev) != 0) {
        const int e = errno; kama_free(io, sizeof *io, _Alignof(kama_sched_io)); errno = e; return NULL;
    }
#elif defined(KAMA__SCHED_KQUEUE)
    // At once, not on the next wait's changelist: a socket may move to another isolate before that wait, and a
    // change still queued here would then register a record the other isolate has already released.
    struct kevent ch; EV_SET(&ch, (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, io);
    if (kevent(((kama__poller*)s->poller)->kq, &ch, 1, NULL, 0, NULL) != 0) {
        const int e = errno; kama_free(io, sizeof *io, _Alignof(kama_sched_io)); errno = e; return NULL;
    }
#endif
    __atomic_add_fetch(&s->refs, 1, __ATOMIC_RELAXED);
    return io;
}
// Take `io` off the scheduler, before its descriptor closes (decision 13: epoll watches the open file description, so
// a descriptor dup'd elsewhere would keep a closed one's registration alive). A fiber still waiting on it is woken,
// and its retry meets the closed socket.
static inline void kama_sched_io_close(kama_sched_io* io) {
    kama_sched* s = io->s;
    kama__sched_ioIdle(io);
    kama_fiber* r = io->reader; kama_fiber* w = io->writer;
    io->reader = io->writer = NULL;
#if defined(KAMA__SCHED_EPOLL)
    struct epoll_event ev; memset(&ev, 0, sizeof ev);
    if (!kama__sched_ring(s)) (void)epoll_ctl(((kama__poller*)s->poller)->epfd, EPOLL_CTL_DEL, (int)io->fd, &ev);
#elif defined(KAMA__SCHED_KQUEUE)
    kama__sched_purge(s, io);                         // a change not yet applied would name freed memory
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
    kama__sched_unref(s);
}

// After an I/O call on `io` reported WouldBlock: park until it may be ready for `interest` (1 read, 2 write), or until
// `deadline` (0 for none). 1 when it may be ready — a hint: retry the call — and 0 when the deadline passed.
// A stream call in direction `interest` came back short: the socket's buffer was drained (a read) or filled (a write)
// just now, so every readiness reported for that direction is spent (no poll can have run since the call: this fiber
// has not switched), and an edge-triggered queue reports the next change. The next call in that direction parks first
// (kama_sched_io_drained) instead of making a system call that could only fail with EAGAIN: tokio's rule, tokio#5866.
// Not once a close or an error was reported: the queue reports that only once, often with the last data, so the next
// call must make its system call to meet it, or it would park for an edge that never comes (tokio keeps its closed
// bits for the same reason). The Poller arm watches a socket only while a fiber waits on it: a short call tells it
// nothing.
static inline void kama_sched_io_short(kama_sched_io* io, int32_t interest) {
#if !defined(KAMA__SCHED_POLLER)
    if (io->closed & interest) return;
    io->drained |= interest;
    io->ready &= ~interest;
#else
    (void)io; (void)interest;
#endif
}
// Whether the next call in direction `interest` parks first, because the last one came back short. Once: it clears.
static inline int kama_sched_io_drained(kama_sched_io* io, int32_t interest) {
    if (!(io->drained & interest)) return 0;
    io->drained &= ~interest;
    return 1;
}
static inline int kama_sched_io_wait(kama_sched_io* io, int32_t interest, uint64_t deadline) {
    static const char msg[] = "fiber scheduler: an I/O wait outside a fiber";
    kama_fiber* f = kama__sched_self(msg, sizeof msg - 1);
#if defined(KAMA__SCHED_URING)
    if (kama__sched_ring(io->s)) return kama__sched_ringPoll(io, interest, deadline);
#endif
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
#if defined(KAMA__SCHED_KQUEUE)
    if (interest & 2) kama__sched_purge(io->s, io);  // woken before a wait applied it: no change outlives its wait
#elif defined(KAMA__SCHED_POLLER)
    kama__sched_arm(io);
#endif
    return woke;
}

// Release `io` from ANY isolate (decision 13, as corrected): on its own, close it; from another, take the socket off
// its scheduler's queue from here and hand the record back, for its scheduler to free after the batch of events it
// may be reading (see RELEASE). The socket's descriptor must still be open: it closes after this.
static inline void kama_sched_io_release(kama_sched_io* io) {
    kama_sched* s = io->s;
    if ((void*)s == kama__fibers.sched) { kama_sched_io_close(io); return; }
    kama__sched_ioIdle(io);
#if defined(KAMA__SCHED_EPOLL)
    struct epoll_event ev; memset(&ev, 0, sizeof ev);
    if (!kama__sched_ring(s)) (void)epoll_ctl(((kama__poller*)s->poller)->epfd, EPOLL_CTL_DEL, (int)io->fd, &ev);
#elif defined(KAMA__SCHED_KQUEUE)
    struct kevent del[2], res[2];
    EV_SET(&del[0], (uintptr_t)io->fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    EV_SET(&del[1], (uintptr_t)io->fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    (void)kevent(((kama__poller*)s->poller)->kq, del, 2, res, 2, NULL);
#endif
    // The Poller arm holds a socket only while one of its own fibers waits on it, and none can: the socket is here.
    kama_sched_io* head = __atomic_load_n(&s->freed, __ATOMIC_ACQUIRE);
    do {
        if (head == KAMA__SCHED_GONE) {               // it has ended: no batch of its can name the record
            kama_free(io, sizeof *io, _Alignof(kama_sched_io));
            kama__sched_unref(s);
            return;
        }
        io->freedNext = head;
    } while (!__atomic_compare_exchange_n(&s->freed, &head, io, 1, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE));
}
// Free what other isolates released; `gone` marks the list so a later release frees its own.
static inline void kama__sched_drainFreed(kama_sched* s, int gone) {
    if (!gone && !__atomic_load_n(&s->freed, __ATOMIC_RELAXED)) return;
    kama_sched_io* io = __atomic_exchange_n(&s->freed, gone ? KAMA__SCHED_GONE : (kama_sched_io*)NULL, __ATOMIC_ACQ_REL);
    int32_t n = 0;
    while (io && io != KAMA__SCHED_GONE) {
        kama_sched_io* next = io->freedNext;
        kama_free(io, sizeof *io, _Alignof(kama_sched_io));
        n++;
        io = next;
    }
    if (n) __atomic_sub_fetch(&s->refs, n, __ATOMIC_ACQ_REL);   // the thread's own reference outlives this
}

// Wait for I/O, a remote wake or `timeoutNs` (-1: until something comes), and make ready what came.
static inline void kama__sched_wait(kama_sched* s, int64_t timeoutNs) {
#if defined(KAMA__SCHED_EPOLL)
#if defined(KAMA__SCHED_URING)
    if (kama__sched_ring(s)) { kama__sched_ringEnter(s, timeoutNs); return; }
#endif
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
        if (ev & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) io->closed |= 1;   // reported once: kama_sched_io_short
        if (ev & (EPOLLHUP | EPOLLERR)) io->closed |= 2;
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
        if (k->flags & (EV_EOF | EV_ERROR)) io->closed |= bits;   // reported once: kama_sched_io_short
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
    kama__sched_drainFreed(s, 0);
    kama__sched_drainRemote(s);
    kama__sched_fireTimers(s, s->heapN ? kama_now_mono_ns() : 0);
    if (rootParked && !kama__fibers.root.parked) return;   // the root's own wake came, and is never queued: no wait
    kama_fiber* f = kama__sched_pop(s);
    if (f) {
        if (++s->ticks >= KAMA_SCHED_POLL_EVERY) { s->ticks = 0; kama__sched_wait(s, 0); }
        // The next fiber's saved registers (kama__fiber_swap's 160 bytes) sit on its own stack, gone cold while every
        // other ready fiber ran: fetch them now, so they arrive while this one runs rather than when that one starts.
        // Measured (KR-251): 1.7% less CPU per request at 10,000 echo connections, none at 1,000, where they stay cached.
        const kama_fiber* n = s->head;
        if (n) {
            const char* sp = (const char*)n->sp;
            __builtin_prefetch(sp); __builtin_prefetch(sp + 64); __builtin_prefetch(sp + 128);
        }
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
    kama__sched_drainFreed(s, 1);
#if defined(KAMA__SCHED_URING)
    if (s->sqes) {                                    // registered for this thread, so undone on it; the ring itself
        struct io_uring_rsrc_update up; memset(&up, 0, sizeof up);   // closes with the scheduler's last reference
        up.offset = (unsigned)s->ringIx;
        (void)syscall(__NR_io_uring_register, s->ringFd, IORING_UNREGISTER_RING_FDS, &up, 1);
    }
#endif
    kama__fibers.sched = NULL; kama__fibers.root.sched = NULL; kama__fibers.schedEnd = NULL;
    kama__sched_unref(s);
}

#endif /* !__EMSCRIPTEN__ && !KAMA_FIBER_NONE */

// The scheduler's half of a socket's wait (kama_os.h, KR-3 step 5), in a program that runs fibers. A target with no
// fiber switch has no scheduler, so nothing registers there and every socket keeps the thread's path.
#if defined(KAMA__OS_FIBERS)
#if !defined(KAMA_FIBER_NONE)
static inline int kama__net_here(void) { return kama__fibers.sched != NULL; }
static inline int kama__net_mine(void* io) { return (void*)((kama_sched_io*)io)->s == kama__fibers.sched; }
static inline void* kama__net_open(ptrdiff_t fd) { return kama_sched_io_open(fd); }
static inline int32_t kama__net_park(void* io, int32_t interest, uint64_t by) {
    return kama_sched_io_wait((kama_sched_io*)io, interest, by);
}
static inline void kama__net_drop(void* io) { kama_sched_io_release((kama_sched_io*)io); }
static inline void kama__net_short(void* io, int32_t interest) { kama_sched_io_short((kama_sched_io*)io, interest); }
static inline int kama__net_drained(void* io, int32_t interest) { return kama_sched_io_drained((kama_sched_io*)io, interest); }
#else
static inline int kama__net_here(void) { return 0; }
static inline int kama__net_mine(void* io) { (void)io; return 1; }
static inline void* kama__net_open(ptrdiff_t fd) { (void)fd; return NULL; }
static inline int32_t kama__net_park(void* io, int32_t interest, uint64_t by) { (void)io; (void)interest; (void)by; return 0; }
static inline void kama__net_drop(void* io) { (void)io; }
static inline void kama__net_short(void* io, int32_t interest) { (void)io; (void)interest; }
static inline int kama__net_drained(void* io, int32_t interest) { (void)io; (void)interest; return 0; }
#endif
#elif defined(KAMA_FIBERS) && defined(KAMA_OS_H) && !defined(__EMSCRIPTEN__)
#error "kama_os.h was read before KAMA_FIBERS was defined: its sockets would never park (define it before any kama header)"
#endif
// ...and of a sleep (kama_time.h): on an isolate with a scheduler it parks the running fiber, the root included.
#if defined(KAMA_FIBERS) && !defined(__EMSCRIPTEN__)
#if !defined(KAMA_FIBER_NONE)
static inline int kama__time_park(uint64_t ns) {
    if (!kama__fibers.sched) return 0;
    kama_sched_sleep(ns);
    return 1;
}
#else
static inline int kama__time_park(uint64_t ns) { (void)ns; return 0; }
#endif
#endif
#define KAMA__SCHED_BODY 1   /* the body above is complete: the halves below may use it */
#endif /* KAMA_SCHED_H */

// The scheduler's half of a channel's wait (kama_channel.h, KR-3 step 6). It is outside the include guard because this
// header does not include kama_channel.h: the channel includes this header at its end, and when this header was read
// first — a C test, the header-order guard — that second read is the one that defines it. It waits for the body above
// to be complete: this header includes kama_os.h, which includes this header again at its end, and that nested read
// comes before the body. Parking allocates nothing here; the waiter is on the parked fiber's stack.
#if defined(KAMA__SCHED_BODY)
#if defined(KAMA__CHANNEL_FIBERS) && !defined(KAMA__SCHED_CHANNEL)
#define KAMA__SCHED_CHANNEL 1
#if !defined(KAMA_FIBER_NONE)
static inline int kama__channel_here(void) { return kama__fibers.sched != NULL; }
static inline void* kama__channel_self(void) { return kama_fiber_current(); }
static inline uint64_t kama__channel_by(int64_t nanos) {
    const uint64_t now = kama_now_mono_ns();
    if (nanos <= 0) return now;
    return (uint64_t)nanos > UINT64_MAX - now ? UINT64_MAX : now + (uint64_t)nanos;
}
static inline int kama__channel_switch(uint64_t by) { return kama_sched_parkUntil(by); }
static inline void kama__channel_wake(void* fiber) { kama_sched_wake((kama_fiber*)fiber); }
#else
static inline int kama__channel_here(void) { return 0; }
static inline void* kama__channel_self(void) { return NULL; }
static inline uint64_t kama__channel_by(int64_t nanos) { (void)nanos; return 0; }
static inline int kama__channel_switch(uint64_t by) { (void)by; return 0; }
static inline void kama__channel_wake(void* fiber) { (void)fiber; }
#endif
#elif defined(KAMA_FIBERS) && defined(KAMA_CHANNEL_H) && !defined(KAMA__CHANNEL_FIBERS) && !defined(__EMSCRIPTEN__)
#error "kama_channel.h was read before KAMA_FIBERS was defined: its channels would never park (define it before any kama header)"
#endif
#endif

// The scheduler's half of an isolate's join (kama_isolate.h, KR-3 step 6), outside the include guard and waiting for the
// body, for the reasons the channel's does. The joiner publishes itself and then looks at `done`; the isolate's thread sets `done` and then takes
// the joiner. All four are sequentially consistent, so at least one side sees the other: the joiner sees the end and
// never parks, or the thread sees the joiner and wakes it. A wake that lands after the joiner saw the end is a stale
// one, which every park tolerates, and the joiner is alive through it because the join's `pthread_join` waits for the
// thread to exit.
#if defined(KAMA__SCHED_BODY)
#if defined(KAMA__ISOLATE_FIBERS) && !defined(KAMA__SCHED_ISOLATE)
#define KAMA__SCHED_ISOLATE 1
#if !defined(KAMA_FIBER_NONE)
static inline void kama__isolate_park(kama__isolate* c) {
    if (!kama__fibers.sched) return;
    kama_fiber* me = kama_fiber_current();
    for (;;) {
        __atomic_store_n(&c->joiner, (void*)me, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&c->done, __ATOMIC_SEQ_CST)) return;
        kama_sched_park();
    }
}
static inline void kama__isolate_ended(kama__isolate* c) {
    __atomic_store_n(&c->done, 1, __ATOMIC_SEQ_CST);
    void* j = __atomic_exchange_n(&c->joiner, (void*)NULL, __ATOMIC_SEQ_CST);
    if (j) kama_sched_wake((kama_fiber*)j);
}
#else
static inline void kama__isolate_park(kama__isolate* c) { (void)c; }
static inline void kama__isolate_ended(kama__isolate* c) { (void)c; }
#endif
#elif defined(KAMA_FIBERS) && defined(KAMA_ISOLATE_H) && !defined(KAMA__ISOLATE_FIBERS) && !defined(__EMSCRIPTEN__)
#error "kama_isolate.h was read before KAMA_FIBERS was defined: its joins would never park (define it before any kama header)"
#endif
#endif
