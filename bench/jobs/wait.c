// wait.c — KR-3 step 2's `Atomic.wait` and `notify`, two designs measured before the surface is ruled. Not the runtime.
//
// The arms, all from WAIT_ARM:
//   cell      the OS waits on the cell itself (os_sync_wait_on_address on Apple, a futex on Linux, WaitOnAddress on Windows),
//             and every notify is the OS's wake: a bare futex, as a hand-rolled one is written.
//   cell-count the same, with a waiter count per hashed bucket, so a notify that reads zero makes no system call: what
//             libstdc++ and libc++ do for a cell the OS can wait on.
//   table     every waiter queues in one table keyed by the cell's address and sleeps on its own thread's word, which the OS
//             waits on; a notify reads the bucket's waiter count and returns at once when it is zero: Go's semaphore table,
//             WebKit's ParkingLot, Rust's parking_lot.
//   table-cv  the table, each thread asleep on its own pthread condvar instead: the fallback where the OS has no public wait
//             on an address (macOS before 14.4). Not on Windows.
//   spin<S>[+<Y>]  the table, after polling the cell for S ns and then yielding for Y ns more: `spin1000` is the ruled
//             `Atomic.wait` (W6), `spin4000+60000` the shape libc++ documents. wait_cxx.cpp is libc++ itself.
//
// The shapes, from WAIT_SHAPE:
//   pingpong  two threads hand a count back and forth over two cells, each waiting for the other: a round trip is two sleeps
//             and two wakes without a spin, and no sleep at all with one, so it measures what a hand-off costs.
//   slow      the same, with the ponger busy for 50 µs before each answer: every wait is a long one, so the CPU column shows
//             what a spin burns before it gives up and sleeps.
//   idle      one thread notifies a cell nobody waits on: what a notify costs a pool's hot path when every worker is busy.
//
// Usage: WAIT_ARM=cell|cell-count|table|table-cv|spin<S>[+<Y>] WAIT_SHAPE=pingpong|slow|idle WAIT_N=<ops> ./wait
// Prints one TSV row: arm, shape, ops, ns per op, CPU ns per op (both threads, user + system). Run the arms interleaved,
// never one after another (this box drifts).
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
static void os_wait(_Atomic uint32_t* a, uint32_t v) { os_sync_wait_on_address((void*)a, v, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE); }
static void os_wake(_Atomic uint32_t* a, int all) {
    if (all) os_sync_wake_by_address_all((void*)a, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    else os_sync_wake_by_address_any((void*)a, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}
#elif defined(_WIN32)
#include <windows.h>   // link with -lsynchronization
static void os_wait(_Atomic uint32_t* a, uint32_t v) { WaitOnAddress((volatile void*)a, &v, sizeof v, INFINITE); }
static void os_wake(_Atomic uint32_t* a, int all) { if (all) WakeByAddressAll((void*)a); else WakeByAddressSingle((void*)a); }
#else
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
static void os_wait(_Atomic uint32_t* a, uint32_t v) { syscall(SYS_futex, a, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0); }
static void os_wake(_Atomic uint32_t* a, int all) { syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, NULL, NULL, 0); }
#endif

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec; }
static uint64_t cpu_ns(void) {
#if defined(_WIN32)
    FILETIME c, e, k, u;   // 100 ns ticks
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100u;
#else
    struct rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (uint64_t)(r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000000000u + (uint64_t)(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1000u;
#endif
}
#if defined(__aarch64__)
#define SPIN_HINT() __asm__ __volatile__("isb")
#elif defined(__x86_64__) || defined(__i386__)
#define SPIN_HINT() __builtin_ia32_pause()
#else
#define SPIN_HINT() ((void)0)
#endif

// ---- cell ----------------------------------------------------------------------------------------------------------
static void cell_wait(_Atomic uint32_t* c, uint32_t old) { while (atomic_load(c) == old) os_wait(c, old); }
static void cell_notify(_Atomic uint32_t* c, int all) { os_wake(c, all); }

// ---- cell-count ----------------------------------------------------------------------------------------------------
typedef struct { _Atomic uint32_t n; char pad[60]; } counter;
static counter counts[256];
static _Atomic uint32_t* count_of(const void* a) {
    uint64_t x = (uint64_t)(uintptr_t)a;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ull;
    return &counts[x >> 56].n;
}
static void cell_count_wait(_Atomic uint32_t* c, uint32_t old) {
    _Atomic uint32_t* k = count_of(c);
    while (atomic_load(c) == old) {
        atomic_fetch_add(k, 1);   // seq-cst, before the OS's look at the cell: paired with the fence below
        if (atomic_load(c) == old) os_wait(c, old);
        atomic_fetch_sub(k, 1);
    }
}
static void cell_count_notify(_Atomic uint32_t* c, int all) {
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(count_of(c), memory_order_relaxed)) os_wake(c, all);
}

// ---- table ---------------------------------------------------------------------------------------------------------
// A waiter is its thread's own record, so a late wake reaches a live one and costs at most a spurious wake.
typedef struct waiter {
    struct waiter* next;
    const void* addr;
    _Atomic uint32_t woken;
    int cvReady;
    pthread_mutex_t m;
    pthread_cond_t c;
} waiter;
typedef struct {
    atomic_flag lock;
    _Atomic uint32_t count;   // waiters queued here: a notify that reads zero takes no lock and makes no system call
    waiter* head;
    waiter* tail;
    char pad[64 - sizeof(atomic_flag) - sizeof(uint32_t) - 2 * sizeof(void*)];
} bucket;
#define NBUCKET 256
static bucket table[NBUCKET];
static _Thread_local waiter self;
static int use_cv;

static bucket* bucket_of(const void* a) {
    uint64_t x = (uint64_t)(uintptr_t)a;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ull;
    return &table[x >> 56];
}
static void lock(bucket* b) { while (atomic_flag_test_and_set_explicit(&b->lock, memory_order_acquire)) { } }
static void unlock(bucket* b) { atomic_flag_clear_explicit(&b->lock, memory_order_release); }

static void park(waiter* w) {
    if (!use_cv) { while (!atomic_load_explicit(&w->woken, memory_order_acquire)) os_wait(&w->woken, 0); return; }
    pthread_mutex_lock(&w->m);
    while (!atomic_load_explicit(&w->woken, memory_order_acquire)) pthread_cond_wait(&w->c, &w->m);
    pthread_mutex_unlock(&w->m);
}
static void unpark(waiter* w) {
    if (!use_cv) { atomic_store_explicit(&w->woken, 1, memory_order_release); os_wake(&w->woken, 0); return; }
    pthread_mutex_lock(&w->m);
    atomic_store_explicit(&w->woken, 1, memory_order_release);
    pthread_cond_signal(&w->c);
    pthread_mutex_unlock(&w->m);
}

// The spin before the table (W6): poll the cell, reading the clock every 16 polls, for `spin_ns`, then yield for `yield_ns`.
static uint64_t spin_ns, yield_ns;
static int spun(_Atomic uint32_t* c, uint32_t old) {
    if (!spin_ns && !yield_ns) return 0;
    const uint64_t t0 = now_ns();
    for (unsigned k = 1;; ++k) {
        if (atomic_load_explicit(c, memory_order_relaxed) != old) return 1;
        SPIN_HINT();
        if ((k & 15) == 0) {
            const uint64_t e = now_ns() - t0;
            if (e >= spin_ns + yield_ns) return 0;
            if (e >= spin_ns) sched_yield();
        }
    }
}

static void table_wait(_Atomic uint32_t* c, uint32_t old) {
    if (spun(c, old)) return;
    waiter* w = &self;
    if (use_cv && !w->cvReady) { pthread_mutex_init(&w->m, NULL); pthread_cond_init(&w->c, NULL); w->cvReady = 1; }
    bucket* b = bucket_of(c);
    while (atomic_load(c) == old) {
        lock(b);
        atomic_fetch_add(&b->count, 1);   // seq-cst, before the look below: paired with the fence in table_notify
        if (atomic_load(c) != old) { atomic_fetch_sub(&b->count, 1); unlock(b); return; }
        w->addr = c; w->next = NULL;
        atomic_store_explicit(&w->woken, 0, memory_order_relaxed);
        if (b->tail) b->tail->next = w; else b->head = w;
        b->tail = w;
        unlock(b);
        park(w);
    }
}
static void table_notify(_Atomic uint32_t* c, int all) {
    bucket* b = bucket_of(c);
    atomic_thread_fence(memory_order_seq_cst);   // the store before this notify is ordered before the count's read
    if (atomic_load_explicit(&b->count, memory_order_relaxed) == 0) return;
    waiter* woke = NULL;
    lock(b);
    waiter* prev = NULL;
    for (waiter* w = b->head; w; ) {
        waiter* n = w->next;
        if (w->addr == c) {
            if (prev) prev->next = n; else b->head = n;
            if (b->tail == w) b->tail = prev;
            atomic_fetch_sub(&b->count, 1);
            w->next = woke; woke = w;
            if (!all) break;
        } else prev = w;
        w = n;
    }
    unlock(b);
    while (woke) { waiter* n = woke->next; unpark(woke); woke = n; }
}

// ---- the shapes ----------------------------------------------------------------------------------------------------
static void (*wait_fn)(_Atomic uint32_t*, uint32_t);
static void (*notify_fn)(_Atomic uint32_t*, int);
static _Atomic uint32_t ping, pong;
static long n_ops;
static int slow;

static void* ponger(void* arg) {
    (void)arg;
    for (uint32_t i = 0; i < (uint32_t)n_ops; ++i) {
        wait_fn(&ping, i);
        if (slow) { const uint64_t t = now_ns(); while (now_ns() - t < 50000) { } }
        atomic_store(&pong, i + 1);
        notify_fn(&pong, 0);
    }
    return NULL;
}

int main(void) {
    const char* arm = getenv("WAIT_ARM") ? getenv("WAIT_ARM") : "cell";
    const char* shape = getenv("WAIT_SHAPE") ? getenv("WAIT_SHAPE") : "pingpong";
    n_ops = getenv("WAIT_N") ? atol(getenv("WAIT_N")) : 20000;
    if (!strcmp(arm, "cell")) { wait_fn = cell_wait; notify_fn = cell_notify; }
    else if (!strcmp(arm, "cell-count")) { wait_fn = cell_count_wait; notify_fn = cell_count_notify; }
    else if (!strcmp(arm, "table")) { wait_fn = table_wait; notify_fn = table_notify; }
    else if (!strcmp(arm, "table-cv")) { wait_fn = table_wait; notify_fn = table_notify; use_cv = 1; }
    else if (!strncmp(arm, "spin", 4)) {
        wait_fn = table_wait; notify_fn = table_notify;
        spin_ns = strtoull(arm + 4, NULL, 10);
        const char* y = strchr(arm, '+');
        yield_ns = y ? strtoull(y + 1, NULL, 10) : 0;
    }
    else { fprintf(stderr, "wait: unknown WAIT_ARM %s\n", arm); return 2; }

    slow = !strcmp(shape, "slow");
    uint64_t t0, t1;
    const uint64_t c0 = cpu_ns();
    if (!strcmp(shape, "pingpong") || slow) {
        pthread_t th;
        pthread_create(&th, NULL, ponger, NULL);
        t0 = now_ns();
        for (uint32_t i = 0; i < (uint32_t)n_ops; ++i) {
            atomic_store(&ping, i + 1);
            notify_fn(&ping, 0);
            wait_fn(&pong, i);
        }
        t1 = now_ns();
        pthread_join(th, NULL);
    } else if (!strcmp(shape, "idle")) {
        t0 = now_ns();
        for (long i = 0; i < n_ops; ++i) { atomic_store(&ping, (uint32_t)i); notify_fn(&ping, 0); }
        t1 = now_ns();
    } else { fprintf(stderr, "wait: unknown WAIT_SHAPE %s\n", shape); return 2; }
    const uint64_t c1 = cpu_ns();
    printf("%s\t%s\t%ld\t%.1f\t%.1f\n", arm, shape, n_ops, (double)(t1 - t0) / (double)n_ops, (double)(c1 - c0) / (double)n_ops);
    return 0;
}
