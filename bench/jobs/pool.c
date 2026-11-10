// pool.c — KR-3's job pool, as a prototype of the hand-out path, measured against `parallel_for` and Rayon (jobs.kama,
// rust/, go/, omp.c). Not the runtime: it exists to find out what a barrier can cost before the design is ruled.
//
// The shape: every record lives on the forker's stack, so a hand-out allocates nothing. A fork on a worker pushes its chunks on
// that worker's fixed deque, as Rayon's do; a hand-out from a thread outside the pool (a frame loop, an isolate) puts one group
// on a list under a short lock, whose chunks are claimed by a fetch_add, and works its own chunks first. The forker waits for the chunks others took, then returns, and
// its frame takes the records with it, so nothing may touch one after the last chunk's count: that decrement carries the waiter's
// flag in the same word, and the wake after it names an address and reads nothing.
//
// Knobs, all from the environment, since one binary keeps the arms honest:
//   POOL_SPIN_NS   how long an idle worker spins before it sleeps (0: it sleeps at once)
//   POOL_WAKE      room (the default): a group can use as many workers as it has chunks for others, and no more than there are.
//                  The caller wakes all of them itself when that is POOL_WAKE_CAP (3) or fewer, and two otherwise; a worker that
//                  pins a group with chunks left wakes up to two more, until as many as it can use have been woken. No wake the
//                  group cannot use, and one wave when it needs few | tree: the caller wakes two, and a worker that took a chunk and
//                  sees more wakes two more, so the wakes double | chain: as tree, one at a time | all: one wake of every sleeper |
//                  one: the caller wakes one per chunk
//   POOL_HELP      own: a waiting caller outside the pool only waits | any: it takes other work meanwhile | nested: as any, but
//                  only at a wait inside one of its chunks (KR-363's A) | tree: as nested, and only work its own hand-outs led to,
//                  whoever handed it out (A′). A worker always takes any work | isolate: as tree, and a worker too takes
//                  only its own tree's work while it waits inside a chunk; an idle worker still takes any | isolate-all: as
//                  isolate, and a caller outside the pool takes its own tree's work at its top-level wait too |
//                  isolate-parks: as isolate, and a waiting worker takes another tree's work too if it never waits in the
//                  OS (`pool_run_parks`), which cannot hang it; a caller outside the pool still takes only its own |
//                  isolate-workers: a worker waiting inside a chunk takes only its own tree's work, and a caller outside the
//                  pool only waits, as with own: what kama does since `0.9.904`.
//   POOL_EXEMPT    parks: under any isolate mode, a waiting worker also takes another tree's work if it never waits in the OS.
//   POOL_STEP      none | run: while two callers outside the pool are in a hand-out, the workers working are one fewer (C) |
//                  caller: every caller outside the pool past the first counts as a core taken, from its first hand-out until it
//                  leaves, so the workers working are that many fewer.
//   BENCH_ROOT     worker: the nested shape starts on a worker, as Rayon's `install` does | caller: on the caller, as kama does.
//
// Usage: BENCH_SHAPE=tight|frame|nested BENCH_WORKERS=<k> BENCH_POOL=<threads> BENCH_CALLS=<n> BENCH_ELEMS=<n> BENCH_ROUNDS=<n> ./pool
//        BENCH_CALLERS=2, BENCH_BLOCKERS=<b> BENCH_BLOCK_MS=<ms>: jobs.kama's two callers and its chunks held in the OS. The nested
//        shape takes BENCH_BLOCKERS too: a caller beside it hands out chunks that sleep, which A can pick up and A′ cannot;
//        and BENCH_CALLERS=2, two callers each running it at once.
// Prints one TSV row: shape, workers, calls, elems, total ns in the barriers, p50 ns, p99 ns, checksum.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
extern int __ulock_wait(uint32_t op, void* addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t op, void* addr, uint64_t wake_value);
#define KW_COMPARE_AND_WAIT 1u
#define KW_WAKE_ALL 0x100u
#define KW_NO_ERRNO 0x01000000u
static void kwait(atomic_uint* a, uint32_t v) { __ulock_wait(KW_COMPARE_AND_WAIT | KW_NO_ERRNO, a, v, 0); }
static void kwake(atomic_uint* a, int all) { __ulock_wake(KW_COMPARE_AND_WAIT | KW_NO_ERRNO | (all ? KW_WAKE_ALL : 0), a, 0); }
#elif defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
#include <math.h>
static void kwait(atomic_uint* a, uint32_t v) { emscripten_futex_wait((volatile void*)a, v, INFINITY); }
static void kwake(atomic_uint* a, int all) { emscripten_futex_wake((volatile void*)a, all ? INT32_MAX : 1); }
#elif defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#include <synchapi.h>
static void kwait(atomic_uint* a, uint32_t v) { WaitOnAddress((volatile void*)a, &v, sizeof v, INFINITE); }
static void kwake(atomic_uint* a, int all) { if (all) WakeByAddressAll((void*)a); else WakeByAddressSingle((void*)a); }
#else
#include <linux/futex.h>
#include <limits.h>
#include <sys/syscall.h>
#include <unistd.h>
static void kwait(atomic_uint* a, uint32_t v) { syscall(SYS_futex, a, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0); }
static void kwake(atomic_uint* a, int all) { syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, NULL, NULL, 0); }
#endif

#if defined(__aarch64__)
static inline void relax(void) { __asm__ __volatile__("isb"); }
#elif defined(__x86_64__)
static inline void relax(void) { __asm__ __volatile__("pause"); }
#else
static inline void relax(void) { }
#endif

static inline int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

// ── The pool ────────────────────────────────────────────────────────────────────────────────────────────────────────

#define WAITER 0x80000000u   // in `remaining`: its owner sleeps on it, so the last chunk's decrement knows to wake
#define LINE 128             // Apple silicon's cache line, and two of everyone else's

// One chunk of a fork, on the forker's stack. Whoever takes it reads it and runs it; the forker cannot return until its
// `remaining` reaches zero, so the record outlives every thread holding it.
typedef struct task {
    void (*run)(void* ctx, int32_t i);
    void* ctx;
    atomic_uint* remaining;
    uintptr_t tree;               // POOL_HELP=tree: the caller outside the pool whose hand-out this chunk descends from
    int32_t i;
    int32_t parks;                // the chunk may wait in the OS, as the compiler's walk would know (POOL_HELP=isolate-parks)
} task;

// A worker's fixed deque (Chase-Lev, on Le et al.'s orders for weak memory, publishing by release stores rather than a release
// fence, since TSan models no fence and would otherwise call every steal a race). It holds pointers to records on stacks, so it
// never grows and a fork never allocates; a full deque runs the chunk at once, which fork-join always allows.
#define DEQUE_N 1024
typedef struct deque {
    _Alignas(LINE) atomic_llong top;
    _Alignas(LINE) atomic_llong bottom;
    _Alignas(LINE) _Atomic(task*) buf[DEQUE_N];
    _Atomic(uintptr_t) tree[DEQUE_N];   // each cell's `task.tree`, readable before a steal claims it, when the record may be gone
} deque;

static int track;   // POOL_HELP=nested, tree or isolate: a chunk's start and end keep `at`
static int exempt;  // POOL_HELP=isolate-parks: a worker waiting inside a chunk takes another tree's work too, if it never waits in the OS
static _Thread_local deque* self;  // NULL on a thread outside the pool

static inline int deque_push(deque* d, task* t) {
    long long b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
    long long tp = atomic_load_explicit(&d->top, memory_order_acquire);
    if (b - tp >= DEQUE_N) return 0;
    if (track) atomic_store_explicit(&d->tree[b & (DEQUE_N - 1)], t->tree | (uintptr_t)(t->parks != 0), memory_order_relaxed);   // published by the store below
    atomic_store_explicit(&d->buf[b & (DEQUE_N - 1)], t, memory_order_release);   // a thief's acquire of the slot sees the record
    atomic_store_explicit(&d->bottom, b + 1, memory_order_release);
    return 1;
}

static inline task* deque_pop(deque* d) {
    long long b = atomic_load_explicit(&d->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&d->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    long long tp = atomic_load_explicit(&d->top, memory_order_relaxed);
    task* x = NULL;
    if (tp <= b) {
        x = atomic_load_explicit(&d->buf[b & (DEQUE_N - 1)], memory_order_relaxed);
        if (tp == b) {
            if (!atomic_compare_exchange_strong_explicit(&d->top, &tp, tp + 1, memory_order_seq_cst, memory_order_relaxed)) x = NULL;
            atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
        }
    } else {
        atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
    }
    return x;
}

// `only`, when not 0, takes the oldest chunk only if it descends from that caller's hand-out. The cell cannot be refilled while
// `top` is still `tp`, so a tag read before the claim is the claimed chunk's.
static inline task* deque_steal(deque* d, uintptr_t only) {
    long long tp = atomic_load_explicit(&d->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    long long b = atomic_load_explicit(&d->bottom, memory_order_acquire);
    if (tp >= b) return NULL;
    task* x = atomic_load_explicit(&d->buf[tp & (DEQUE_N - 1)], memory_order_acquire);
    if (only) {   // a tree is an aligned address, so its low bit carries `parks`
        uintptr_t tag = atomic_load_explicit(&d->tree[tp & (DEQUE_N - 1)], memory_order_relaxed);
        if ((tag & ~(uintptr_t)1) != only && (!exempt || !self || (tag & 1))) return NULL;
    }
    if (!atomic_compare_exchange_strong_explicit(&d->top, &tp, tp + 1, memory_order_seq_cst, memory_order_relaxed)) return NULL;
    return x;
}

// A hand-out from a thread outside the pool: every chunk of it, by index, on one list under a short lock.
typedef struct group {
    void (*run)(void* ctx, int32_t i);
    void* ctx;
    int32_t count;
    atomic_int next;              // the next chunk to hand out: a claim is one fetch_add
    atomic_uint remaining;        // chunks not finished, plus one per thread holding a pin, plus WAITER
    int32_t pinned;               // threads that have pinned it: under the lock
    int32_t woken;                // POOL_WAKE=room: wakes made for it, under the lock but the caller's first
    uintptr_t tree;               // as `task.tree`
    int32_t parks;                // as `task.parks`
    struct group* prev;
    struct group* after;
} group;

static struct {
    _Alignas(LINE) atomic_int lock;
    _Atomic(group*) head;         // written under the lock; read without it, so an idle spin costs no write
    group* tail;
    _Alignas(LINE) atomic_uint epoch;   // what a sleeping worker sleeps on
    _Alignas(LINE) atomic_int sleepers;
    _Alignas(LINE) atomic_int stop;
} P;

static deque* deques;              // one per worker, made up front
static int nworkers;
static _Thread_local uint32_t seed;
// When `track`: how many chunks this thread is inside (a wait at 0 is a caller's top-level one), and the `tree` of the one it
// runs. Padded a line each side: macOS gives each thread its `_Thread_local`s in a small malloc'd block, 16 bytes from the next
// thread's, so two writes a chunk to an unpadded record took the nested shape from 1.7 ms to 3.4 (writes once a steal, as `seed`
// is, measured no different padded).
static _Thread_local struct { char before[LINE]; int depth; uintptr_t tree; char after[LINE]; } at;
static inline uintptr_t me(void) { return (uintptr_t)&at; }   // a thread's own id: the address of its copy

static int64_t spin_ns = 50000;
static enum { WAKE_ALL, WAKE_ONE, WAKE_CHAIN, WAKE_TREE, WAKE_ROOM } wake_kind = WAKE_ROOM;
static int32_t wake_cap = 3;
static enum { HELP_OWN, HELP_ANY, HELP_NESTED, HELP_OWN_TREE, HELP_ISOLATE, HELP_ISOLATE_ALL, HELP_ISOLATE_PARKS, HELP_ISOLATE_WORKERS } help_kind = HELP_OWN;
static enum { STEP_NONE, STEP_RUN, STEP_CALLER } step_kind = STEP_NONE;

// POOL_STEP: the callers outside the pool counted against the workers, and the workers holding one of the cores left to them.
// A worker takes a core before it looks for work and gives it back after; a worker that cannot take one sleeps. Best effort:
// the caller takes its own chunks whatever the workers do, so a worker aside costs speed and never progress.
static atomic_int outside;
static atomic_int working;
static _Thread_local int counted;       // this thread is in `outside`

static inline int room(void) {
    int extra = atomic_load_explicit(&outside, memory_order_relaxed) - 1;
    return nworkers - (extra > 0 ? extra : 0);
}
static inline int core_take(void) {
    if (step_kind == STEP_NONE) return 1;
    int w = atomic_load_explicit(&working, memory_order_relaxed);
    while (w < room())
        if (atomic_compare_exchange_weak_explicit(&working, &w, w + 1, memory_order_relaxed, memory_order_relaxed)) return 1;
    return 0;
}
static inline void core_give(void) { if (step_kind != STEP_NONE) atomic_fetch_sub_explicit(&working, 1, memory_order_relaxed); }
static inline int core_free(void) { return step_kind == STEP_NONE || atomic_load_explicit(&working, memory_order_relaxed) < room(); }

// Test first: a bare test-and-set from ten spinning threads is ten writes to one line per pass.
static inline void lock_take(void) {
    for (;;) {
        if (!atomic_exchange_explicit(&P.lock, 1, memory_order_acquire)) return;
        while (atomic_load_explicit(&P.lock, memory_order_relaxed)) relax();
    }
}
static inline void lock_give(void) { atomic_store_explicit(&P.lock, 0, memory_order_release); }

// Tell a sleeper there is work. The caller has already made the work visible. The RMW is the full barrier between that and
// our read of `sleepers` (the sleeper's half is `sleepers` up, then a look for work), and it fails the compare of any worker
// about to sleep on the old epoch. A release store then a load would not do: x86 may reorder those.
static inline void wake_some(int kind, int32_t n) {
    atomic_fetch_add_explicit(&P.epoch, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&P.sleepers, memory_order_seq_cst) <= 0) return;
    if (kind == WAKE_ALL) kwake(&P.epoch, 1);
    else if (kind == WAKE_CHAIN) kwake(&P.epoch, 0);
    else if (kind == WAKE_TREE) { kwake(&P.epoch, 0); if (n > 1) kwake(&P.epoch, 0); }
    else if (kind == WAKE_ROOM) {   // the waker has already counted them, and a wake with nobody asleep is a system call for nothing
        const int32_t asleep = atomic_load_explicit(&P.sleepers, memory_order_relaxed);
        for (int32_t i = 0; i < n && i < asleep; ++i) kwake(&P.epoch, 0);
    }
    else for (int32_t i = 0; i < n; ++i) kwake(&P.epoch, 0);
}

static inline void finish_chunk(void (*run)(void*, int32_t), void* ctx, int32_t i, atomic_uint* remaining, uintptr_t tr) {
    if (track) {
        uintptr_t was = at.tree;
        at.tree = tr; at.depth++;
        run(ctx, i);
        at.depth--; at.tree = was;
    } else {
        run(ctx, i);
    }
    uint32_t was = atomic_fetch_sub_explicit(remaining, 1, memory_order_acq_rel);
    if (was == (WAITER | 1u)) kwake(remaining, 1);   // an address, nothing read: the frame may already be gone
}

// A group whose chunks are all out leaves the list before the last one runs, and its count cannot reach zero until that one has
// finished, so no thread can find a group whose frame is gone. That is what lets the record live on a stack.
static inline void group_unlist(group* g) {
    lock_take();
    if (g->prev) g->prev->after = g->after; else atomic_store_explicit(&P.head, g->after, memory_order_relaxed);
    if (g->after) g->after->prev = g->prev; else P.tail = g->prev;
    lock_give();
}

static inline void group_chunks(group* g) {
    for (;;) {
        int32_t i = atomic_fetch_add_explicit(&g->next, 1, memory_order_relaxed);
        if (i >= g->count) return;
        if (i == g->count - 1) group_unlist(g);
        finish_chunk(g->run, g->ctx, i, &g->remaining, g->tree);
    }
}

// Pin the oldest group under the lock, then claim its chunks with no lock at all, so a hand-out takes this lock once per thread
// rather than once per chunk. The pin is one more count in `remaining`, taken while the group is listed and so alive; dropping it
// is the same decrement a chunk's end is, with the same wake.
// `only`, when not 0, pins the oldest group that descends from that caller's hand-out, past any that do not.
static inline int run_listed(uintptr_t only) {
    if (!atomic_load_explicit(&P.head, memory_order_relaxed)) return 0;
    lock_take();
    group* g = atomic_load_explicit(&P.head, memory_order_relaxed);
    if (only) while (g && g->tree != only && (!exempt || !self || g->parks)) g = g->after;
    if (!g || g->pinned >= g->count - 1) { lock_give(); return 0; }   // the caller is one, so count - 1 others is all it can use
    g->pinned++;
    atomic_fetch_add_explicit(&g->remaining, 1, memory_order_relaxed);
    int32_t more = 0;
    const int32_t usable = g->count - 1 < nworkers ? g->count - 1 : nworkers;   // pins it can take, and workers there are
    if (wake_kind == WAKE_ROOM && g->woken < usable && atomic_load_explicit(&g->next, memory_order_relaxed) < g->count) {
        more = usable - g->woken;
        if (more > 2) more = 2;
        g->woken += more;
    }
    lock_give();
    if (more > 0 && atomic_load_explicit(&P.sleepers, memory_order_relaxed) > 0) wake_some(WAKE_ROOM, more);
    // The fan-out is best effort, since the caller takes whatever nobody else does, so a relaxed look spares this a write to the line
    // sleepers sleep on. Only the caller's own wake, in pool_run, has to be the full barrier.
    if ((wake_kind == WAKE_CHAIN || wake_kind == WAKE_TREE) && atomic_load_explicit(&P.sleepers, memory_order_relaxed) > 0
        && atomic_load_explicit(&g->next, memory_order_relaxed) < g->count)
        wake_some(wake_kind, g->count - atomic_load_explicit(&g->next, memory_order_relaxed));
    group_chunks(g);
    uint32_t was = atomic_fetch_sub_explicit(&g->remaining, 1, memory_order_acq_rel);
    if (was == (WAITER | 1u)) kwake(&g->remaining, 1);
    return 1;
}

// One pass over the other workers' deques, from a random one. Finding one with more left wakes the next, as Go's scheduler
// does, so one wake from the forker fans out without the forker paying for all of them.
static inline int run_stolen(uintptr_t only) {
    if (nworkers == 0) return 0;
    seed = seed * 1664525u + 1013904223u;
    int from = (int)(seed >> 16) % nworkers;
    for (int k = 0; k < nworkers; ++k) {
        deque* d = &deques[(from + k) % nworkers];
        if (d == self) continue;
        task* t = deque_steal(d, only);
        if (t) {
            if (atomic_load_explicit(&P.sleepers, memory_order_relaxed) > 0
                && atomic_load_explicit(&d->bottom, memory_order_relaxed) > atomic_load_explicit(&d->top, memory_order_relaxed))
                wake_some(WAKE_CHAIN, 1);
            finish_chunk(t->run, t->ctx, t->i, t->remaining, t->tree);
            return 1;
        }
    }
    return 0;
}

static inline int work_visible(void) {
    if (atomic_load_explicit(&P.head, memory_order_seq_cst)) return 1;
    for (int k = 0; k < nworkers; ++k)
        if (atomic_load_explicit(&deques[k].bottom, memory_order_seq_cst) > atomic_load_explicit(&deques[k].top, memory_order_seq_cst)) return 1;
    return 0;
}

static void* worker(void* arg) {
    self = (deque*)arg;
    seed = (uint32_t)(uintptr_t)arg;
    while (!atomic_load_explicit(&P.stop, memory_order_relaxed)) {
        // A worker holds one of the cores left to it while it works and while it looks for work, since a look spends a core
        // too, as Go caps its spinning threads; without one it sleeps at once.
        if (core_take()) {
            for (;;) {
                task* t = deque_pop(self);
                if (t) { finish_chunk(t->run, t->ctx, t->i, t->remaining, t->tree); continue; }
                if (run_listed(0) || run_stolen(0)) continue;
                int hit = 0;
                if (spin_ns > 0) {
                    int64_t until = now_ns() + spin_ns;
                    do {
                        for (int n = 0; n < 32; ++n) relax();
                        hit = work_visible();
                    } while (!hit && !atomic_load_explicit(&P.stop, memory_order_relaxed) && now_ns() < until);
                }
                if (!hit) break;
            }
            core_give();
        }
        uint32_t e = atomic_load_explicit(&P.epoch, memory_order_seq_cst);
        atomic_fetch_add_explicit(&P.sleepers, 1, memory_order_seq_cst);
        if (!(work_visible() && core_free()) && !atomic_load(&P.stop)) kwait(&P.epoch, e);
        atomic_fetch_sub_explicit(&P.sleepers, 1, memory_order_seq_cst);
    }
    return NULL;
}

static pthread_t* threads;

static void pool_start(int total) {
    nworkers = total > 1 ? total - 1 : 0;   // the caller is one of the `total` that run
    threads = calloc((size_t)(nworkers ? nworkers : 1), sizeof *threads);
    if (nworkers) {
#if defined(_WIN32)
        deques = _aligned_malloc((size_t)nworkers * sizeof *deques, LINE);
#else
        if (posix_memalign((void**)&deques, LINE, (size_t)nworkers * sizeof *deques) != 0) abort();
#endif
        memset(deques, 0, (size_t)nworkers * sizeof *deques);
    }
    for (int i = 0; i < nworkers; ++i) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, (size_t)8 << 20);
        pthread_create(&threads[i], &a, worker, &deques[i]);
        pthread_attr_destroy(&a);
    }
}

static void pool_stop(void) {
    atomic_store(&P.stop, 1);
    atomic_fetch_add(&P.epoch, 1);
    kwake(&P.epoch, 1);
    for (int i = 0; i < nworkers; ++i) pthread_join(threads[i], NULL);
    free(threads);
#if defined(_WIN32)
    _aligned_free(deques);
#else
    free(deques);
#endif
}

// Wait for what others took. A worker keeps taking work meanwhile, or it would idle with work about; a thread outside the
// pool waits, or helps as POOL_HELP says, `only` its own tree's work when not 0. Either way it sleeps once there is nothing it
// could do.
static inline void barrier_wait(atomic_uint* remaining, int helps, uintptr_t only) {
    int64_t until = 0;
    for (;;) {
        uint32_t v = atomic_load_explicit(remaining, memory_order_acquire);
        if ((v & ~WAITER) == 0) return;
        if (helps) {
            task* t = self ? deque_pop(self) : NULL;
            if (t) { finish_chunk(t->run, t->ctx, t->i, t->remaining, t->tree); until = 0; continue; }
            if (run_listed(only) || run_stolen(only)) { until = 0; continue; }
        }
        if (!until) until = now_ns() + spin_ns;
        if (now_ns() < until) { relax(); continue; }
        uint32_t w = atomic_fetch_or_explicit(remaining, WAITER, memory_order_acq_rel) | WAITER;
        if ((w & ~WAITER) == 0) return;
        kwait(remaining, w);
    }
}

static void pool_leave(void);

// The hand-out and its barrier: everything on this frame, no allocation.
static void pool_run_parks(void (*run)(void*, int32_t), void* ctx, int32_t k, int32_t parks) {
    if (k <= 1 || nworkers == 0) {
        for (int32_t i = 0; i < k; ++i) run(ctx, i);
        return;
    }
    if (self) {
        // On a worker: chunk 0 here, the rest on this worker's deque, a fence and a look for sleepers. No lock, no system call
        // unless someone sleeps.
        atomic_uint remaining;
        atomic_init(&remaining, (uint32_t)k);
        task rec[k];
        const uintptr_t tr = track ? at.tree : 0;
        for (int32_t i = k - 1; i > 0; --i) {
            rec[i].run = run; rec[i].ctx = ctx; rec[i].remaining = &remaining; rec[i].tree = tr; rec[i].i = i; rec[i].parks = parks;
            if (!deque_push(self, &rec[i])) finish_chunk(run, ctx, i, &remaining, tr);
        }
        atomic_thread_fence(memory_order_seq_cst);
        if (atomic_load_explicit(&P.sleepers, memory_order_relaxed) > 0) wake_some(WAKE_CHAIN, 1);   // a fork: one is enough to start
        finish_chunk(run, ctx, 0, &remaining, tr);
        barrier_wait(&remaining, 1, help_kind >= HELP_ISOLATE ? tr : 0);
        return;
    }
    // A caller outside the pool counts against the workers (POOL_STEP=run) while it is in its outermost hand-out, or (=caller)
    // from its first one until `pool_leave`.
    int counting = step_kind != STEP_NONE && !counted;
    if (counting) { counted = 1; atomic_fetch_add_explicit(&outside, 1, memory_order_relaxed); }
    group g;
    g.run = run; g.ctx = ctx; g.count = k; g.pinned = 0;
    g.tree = at.depth > 0 ? at.tree : me();   // inside a chunk, the hand-out belongs to that chunk's tree
    g.parks = parks;
    int32_t wakes = k - 1;
    if (wake_kind == WAKE_ROOM) {
        wakes = k - 1 < nworkers ? k - 1 : nworkers;   // all it can use, if that is few; two to start the fan-out if not
        if (wakes > wake_cap) wakes = 2;
    }
    g.woken = wakes;   // read here, not after the listing: a worker that pins it may write it then
    atomic_init(&g.next, 0);
    atomic_init(&g.remaining, (uint32_t)k);
    g.after = NULL;
    lock_take();
    g.prev = P.tail;
    if (P.tail) P.tail->after = &g; else atomic_store_explicit(&P.head, &g, memory_order_relaxed);
    P.tail = &g;
    lock_give();
    wake_some(wake_kind, wakes);

    // Our own chunks first: nothing can wait on a chunk nobody took, so this alone rules out a deadlock under nesting.
    group_chunks(&g);
    int helps = help_kind == HELP_ANY || help_kind == HELP_ISOLATE_ALL
        || (help_kind >= HELP_NESTED && help_kind != HELP_ISOLATE_WORKERS && at.depth > 0);
    barrier_wait(&g.remaining, helps, help_kind >= HELP_OWN_TREE ? g.tree : 0);
    if (counting && step_kind == STEP_RUN) pool_leave();
}

// A caller outside the pool is done with it: the cores it was counted as taking go back to the workers.
static void pool_leave(void) {
    if (!counted) return;
    counted = 0;
    atomic_fetch_sub_explicit(&outside, 1, memory_order_relaxed);
    if (atomic_load_explicit(&P.sleepers, memory_order_seq_cst) > 0 && work_visible()) wake_some(WAKE_CHAIN, 1);
}

static void pool_run(void (*run)(void*, int32_t), void* ctx, int32_t k) { pool_run_parks(run, ctx, k, 0); }

// Run `run` once on a worker and wait, as Rayon's `install` does: how a recursion gets onto a deque from outside the pool.
static void pool_on_worker(void (*run)(void*, int32_t), void* ctx) {
    if (nworkers == 0) { run(ctx, 0); return; }
    group g;
    g.run = run; g.ctx = ctx; g.count = 2; g.pinned = 0;   // no caller's half here: one thread pins, and takes the one chunk
    g.tree = me();
    g.parks = 0;
    g.woken = 1;
    atomic_init(&g.next, 1);
    atomic_init(&g.remaining, 1u);
    g.after = NULL;
    lock_take();
    g.prev = P.tail;
    if (P.tail) P.tail->after = &g; else atomic_store_explicit(&P.head, &g, memory_order_relaxed);
    P.tail = &g;
    lock_give();
    wake_some(WAKE_CHAIN, 1);
    barrier_wait(&g.remaining, 0, 0);
}

// ── The work ────────────────────────────────────────────────────────────────────────────────────────────────────────

typedef struct { uint32_t* xs; int32_t len; int32_t chunk; int32_t rounds; } each_ctx;

// xorshift, `rounds` times an element: a cost per element no compiler folds, the same in every arm, so `check` must agree.
static void each(void* c, int32_t i) {
    each_ctx* e = (each_ctx*)c;
    int32_t from = i * e->chunk;
    int32_t to = from + e->chunk;
    if (to > e->len) to = e->len;
    for (int32_t n = from; n < to; ++n) {
        uint32_t x = e->xs[n] + 1u;
        for (int32_t r = 0; r < e->rounds; ++r) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
        e->xs[n] = x;
    }
}

static inline void barrier(uint32_t* xs, int32_t len, int32_t k, int32_t rounds) {
    if (k > len) k = len;
    each_ctx c = { xs, len, (len + k - 1) / k, rounds };
    pool_run(each, &c, k);
}

// Recursive fork-join, each fork two chunks: the shape a central list pays for and per-worker deques do not.
typedef struct { int32_t n; int64_t r[2]; } fork_ctx;
static int64_t fib(int32_t n);
static void fork_arm(void* c, int32_t i) { fork_ctx* f = (fork_ctx*)c; f->r[i] = fib(f->n - 1 - i); }
static int64_t fib_seq(int32_t n) { return n < 2 ? n : fib_seq(n - 1) + fib_seq(n - 2); }
static int32_t leaf = 10;
static int64_t fib(int32_t n) {
    if (n < leaf) return fib_seq(n);
    fork_ctx f;
    f.n = n; f.r[0] = f.r[1] = 0;
    pool_run(fork_arm, &f, 2);
    return f.r[0] + f.r[1];
}
typedef struct { int32_t n; int64_t r; } root_ctx;
static void fib_root(void* c, int32_t i) { (void)i; root_ctx* q = (root_ctx*)c; q->r = fib(q->n); }

// The BENCH_BLOCKERS shape: chunks that each sleep in the OS, as a read that blocks does, handed out until the caller is done.
static void nap(void* c, int32_t i) {
    (void)i;
    struct timespec d = { 0, (long)*(int32_t*)c * 1000000L };
    nanosleep(&d, NULL);
}
typedef struct { int32_t k; int32_t ms; atomic_int* done; } block_arg;
static void* block(void* p) {
    block_arg* a = (block_arg*)p;
    while (!atomic_load_explicit(a->done, memory_order_acquire)) pool_run_parks(nap, &a->ms, a->k, 1);
    pool_leave();
    return NULL;
}

// One caller of the nested shape: `calls` runs of fib(`n`), each one's time in `t`, started on the caller or on a worker.
typedef struct { int64_t* t; int calls, n, on_caller; int64_t check; } fib_arg;
static void* fib_drive(void* p) {
    fib_arg* a = (fib_arg*)p;
    for (int c = 0; c < a->calls; ++c) {
        root_ctx q = { a->n, 0 };
        a->t[c] = now_ns();
        if (a->on_caller) q.r = fib(a->n);
        else pool_on_worker(fib_root, &q);
        a->t[c] = now_ns() - a->t[c];
        a->check += q.r;
    }
    pool_leave();
    return NULL;
}

// One caller: `calls` barriers over its own `xs`, each one's time in `t`.
typedef struct { uint32_t* xs; int64_t* t; int calls, elems, workers, rounds, frame; int64_t slot; } drive_arg;
static void* drive(void* p) {
    drive_arg* a = (drive_arg*)p;
    int64_t next = now_ns();
    for (int c = 0; c < a->calls; ++c) {
        if (a->frame) { next += a->slot; while (now_ns() < next) relax(); }   // the caller's serial work, between phases
        int64_t t0 = now_ns();
        barrier(a->xs, a->elems, a->workers, a->rounds);
        a->t[c] = now_ns() - t0;
    }
    pool_leave();
    return NULL;
}

static int setting(const char* name, int dflt) { const char* v = getenv(name); return v && *v ? atoi(v) : dflt; }
static int cmp64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

int main(void) {
    const char* shape = getenv("BENCH_SHAPE") ? getenv("BENCH_SHAPE") : "tight";
    int workers = setting("BENCH_WORKERS", 4);
    int pool = setting("BENCH_POOL", workers);
    int calls = setting("BENCH_CALLS", 2000);
    int elems = setting("BENCH_ELEMS", 1024);
    int slot_us = setting("BENCH_SLOT_US", 1389);   // 12 barriers in a 60 Hz frame
    int rounds = setting("BENCH_ROUNDS", 0);
    int callers = setting("BENCH_CALLERS", 1) == 2 ? 2 : 1;
    int blockers = setting("BENCH_BLOCKERS", 0);
    leaf = setting("BENCH_LEAF", 10);
    spin_ns = setting("POOL_SPIN_NS", 50000);
    const char* wake = getenv("POOL_WAKE");
    if (wake && !strcmp(wake, "all")) wake_kind = WAKE_ALL;
    if (wake && !strcmp(wake, "one")) wake_kind = WAKE_ONE;
    if (wake && !strcmp(wake, "chain")) wake_kind = WAKE_CHAIN;
    if (wake && !strcmp(wake, "tree")) wake_kind = WAKE_TREE;
    wake_cap = setting("POOL_WAKE_CAP", 3);
    const char* help = getenv("POOL_HELP");
    if (help && !strcmp(help, "any")) help_kind = HELP_ANY;
    if (help && !strcmp(help, "nested")) help_kind = HELP_NESTED;
    if (help && !strcmp(help, "tree")) help_kind = HELP_OWN_TREE;
    if (help && !strcmp(help, "isolate")) help_kind = HELP_ISOLATE;
    if (help && !strcmp(help, "isolate-all")) help_kind = HELP_ISOLATE_ALL;
    if (help && !strcmp(help, "isolate-parks")) help_kind = HELP_ISOLATE_PARKS;
    if (help && !strcmp(help, "isolate-workers")) help_kind = HELP_ISOLATE_WORKERS;
    const char* ex = getenv("POOL_EXEMPT");
    exempt = help_kind == HELP_ISOLATE_PARKS || (ex && !strcmp(ex, "parks"));
    track = help_kind >= HELP_NESTED;
    const char* step = getenv("POOL_STEP");
    if (step && !strcmp(step, "run")) step_kind = STEP_RUN;
    if (step && !strcmp(step, "caller")) step_kind = STEP_CALLER;
    const char* rooted = getenv("BENCH_ROOT");
    int on_caller = rooted && !strcmp(rooted, "caller");

    pool_start(pool);
    int n = calls * callers;   // samples
    int64_t* t = calloc((size_t)n, sizeof *t);
    int64_t total = 0, check = 0;
    if (!strcmp(shape, "nested")) {
        atomic_int done;
        atomic_init(&done, 0);
        block_arg b = { blockers, setting("BENCH_BLOCK_MS", 2), &done };
        pthread_t th;
        if (blockers > 0) pthread_create(&th, NULL, block, &b);
        fib_arg f[2];
        for (int i = 0; i < callers; ++i) f[i] = (fib_arg){ t + (size_t)i * calls, calls, elems, on_caller, 0 };
        if (callers == 2) {
            pthread_t two[2];
            for (int i = 0; i < 2; ++i) pthread_create(&two[i], NULL, fib_drive, &f[i]);
            for (int i = 0; i < 2; ++i) pthread_join(two[i], NULL);
        } else {
            fib_drive(&f[0]);
        }
        for (int i = 0; i < callers; ++i) check += f[i].check;
        if (blockers > 0) {
            atomic_store_explicit(&done, 1, memory_order_release);
            pthread_join(th, NULL);
        }
    } else {
        drive_arg d[2];
        for (int i = 0; i < callers; ++i)
            d[i] = (drive_arg){ calloc((size_t)elems, sizeof(uint32_t)), t + (size_t)i * calls, calls, elems, workers, rounds,
                                !strcmp(shape, "frame"), (int64_t)slot_us * 1000 };
        if (callers == 2) {
            pthread_t th[2];
            for (int i = 0; i < 2; ++i) pthread_create(&th[i], NULL, drive, &d[i]);
            for (int i = 0; i < 2; ++i) pthread_join(th[i], NULL);
        } else if (blockers > 0) {
            atomic_int done;
            atomic_init(&done, 0);
            block_arg b = { blockers, setting("BENCH_BLOCK_MS", 2), &done };
            pthread_t th;
            pthread_create(&th, NULL, block, &b);
            drive(&d[0]);
            atomic_store_explicit(&done, 1, memory_order_release);
            pthread_join(th, NULL);
        } else {
            drive(&d[0]);
        }
        for (int i = 0; i < callers; ++i) {
            for (int e = 0; e < elems; ++e) check += d[i].xs[e];
            free(d[i].xs);
        }
    }
    for (int i = 0; i < n; ++i) total += t[i];
    qsort(t, (size_t)n, sizeof *t, cmp64);
    printf("%s\t%d\t%d\t%d\t%lld\t%lld\t%lld\t%lld\n", shape, workers, calls, elems, (long long)total,
           (long long)t[n / 2], (long long)t[n - 1 - n / 100], (long long)check);
    pool_stop();
    return 0;
}
