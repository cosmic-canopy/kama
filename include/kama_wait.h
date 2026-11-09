#ifndef KAMA_WAIT_H
#define KAMA_WAIT_H

// kama_wait.h — `Atomic.wait`, `waitUntil`, `notifyOne` and `notifyAll` (KR-3 step 2; ROADMAP_DETAIL, "Step 2 begins",
// W1–W6 as ruled 2026-11-07).
//
// A waiter never sleeps on the cell. It queues in one table keyed by the cell's address and sleeps on a word of its own,
// which is what lets one mechanism serve every width of cell (W2), a fiber (W4: the record holds the fiber, which
// parks), and macOS before 14.4 (each thread asleep on its own condvar), with no private symbol (decision 14). Go's
// semaphore table, WebKit's ParkingLot and Rust's `parking_lot` are the same shape.
//
//   wait    Load the cell; if it already differs, that is the answer. A thread then polls it for up to
//           KAMA__WAIT_POLL_NS (W6), and a fiber never does, since what changes the cell may be a sibling that cannot
//           run meanwhile. Then, under the bucket's lock: count itself in, look at the cell again, and queue a record
//           on its own stack; then sleep (a thread) or park (a fiber) until a notify takes it off. It returns only on a
//           value other than the one it waited on, or at the deadline.
//   notify  A fence, then the bucket's count: zero means nobody waits in this bucket, and that is all an idle notify
//           costs (a hash, a fence and a load: 1.1 ns on the M4). Otherwise it takes the matching records off under the
//           lock. A fiber is woken under the lock, which keeps it alive through the wake, as the channel's are; a thread
//           after the lock is let go.
//
// The count and the second look are a Dekker pair: a waiter counts itself in and then loads the cell, both seq-cst; a
// notifier's store comes before its fence and its read of the count. At least one sees the other, so no wake is lost.
//
// A thread's record lives on its stack. A notifier takes it off under the lock and only then sets `woken` and wakes the
// word, and the waiter leaves only once `woken` is set: one whose deadline passed but which finds itself already taken
// off waits for that, rather than leave a record the notifier is about to write. The OS wake after the store reads no
// memory at the address (a futex, os_sync, WaitOnAddress and Atomics.notify all key on it), so a late one costs at most
// a spurious wake, which every wait here loops over. The condvar a thread sleeps on below macOS 14.4 is signalled under
// the bucket's lock instead, and its waiter takes that lock once before it leaves, so nothing touches the condvar after.
//
// By target:
//   threads              the table. A thread sleeps on os_sync_wait_on_address (macOS 14.4 and iOS 17.4, weak-linked;
//                        below, its own condvar), a private futex (Linux, Android), WaitOnAddress (Windows, looked
//                        up once from its API set) or emscripten_futex_wait (wasm with threads, where a browser's main
//                        thread, which may not block, spins instead).
//   wasm without threads nothing else runs, so a wait that nothing could end panics, and one with a deadline spins to it,
//                        as `sleep` does there.
//   bare metal           an `@interrupt` handler is the other party, so a wait polls the cell (KR-233 makes it WFE/SEV).
//
// The table is 256 buckets of one cache line (two on glibc's arm64, whose mutex is 48 B), zeroed static storage, which
// is every lock's unlocked state here: os_unfair_lock, SRWLOCK and glibc's, musl's and emscripten's pthread mutex. The
// emitter defines it once, in a program that includes this header (emitRuntimeSlotDefinitions).

#include <stdint.h>
#include <stddef.h>

// The fiber half (W4): on an isolate that runs fibers a wait parks the fiber, the root included. The scheduler's half is
// kama_sched.h's, shared with the channel's waits and included at the end of this file. Declared before kama_time.h,
// which under KAMA_FIBERS reads kama_sched.h itself, and its `#error` must find these already asked for.
#if defined(KAMA_FIBERS) && !defined(__EMSCRIPTEN__)
#define KAMA__PARK_FIBERS 1
static inline int      kama__park_here(void);
static inline void*    kama__park_self(void);
static inline int      kama__park_switch(uint64_t by);
static inline void     kama__park_wake(void* fiber);
#endif

#include "kama_runtime.h"    /* kama_panic, kama_string_lit */
#include "kama_atomic.h"     /* kama_atomic_load */
#include "kama_time.h"       /* kama_now_mono_ns: the clock an `Instant` holds, and the scheduler's */

#define KAMA__WAIT_POLL  0   /* bare metal */
#define KAMA__WAIT_ALONE 1   /* wasm without threads */
#define KAMA__WAIT_TABLE 2
#if defined(KAMA_TARGET_EMBEDDED)
  #define KAMA__WAIT_HOW KAMA__WAIT_POLL
#elif defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  #define KAMA__WAIT_HOW KAMA__WAIT_ALONE
#else
  #define KAMA__WAIT_HOW KAMA__WAIT_TABLE
#endif

#define KAMA__WAIT_NONE UINT64_MAX   /* no deadline */

#if defined(__aarch64__)
  #define KAMA__WAIT_PAUSE() __asm__ __volatile__("isb" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
  #define KAMA__WAIT_PAUSE() __builtin_ia32_pause()
#elif defined(__arm__)
  #define KAMA__WAIT_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
  #define KAMA__WAIT_PAUSE() ((void)0)
#endif

// Load the cell into `seen` (seq-cst, so the value seen is ordered after whatever store made it) and compare it bitwise
// with `old`, as `compareExchange` does: `-0.0` and `+0.0` are different values.
static inline int kama__wait_changed(void* cell, const void* old, void* seen, size_t width) {
    kama_atomic_load(cell, seen, width, __ATOMIC_SEQ_CST);
    switch (width) {   /* a cell is one machine word, so no memcmp: bare metal may have no libc to call */
        case 1:  return *(const uint8_t*)seen  != *(const uint8_t*)old;
        case 2:  return *(const uint16_t*)seen != *(const uint16_t*)old;
        case 4:  return *(const uint32_t*)seen != *(const uint32_t*)old;
        default: return *(const uint64_t*)seen != *(const uint64_t*)old;
    }
}

#if KAMA__WAIT_HOW == KAMA__WAIT_TABLE
#include "kama_locked.h"     /* kama__xlock and kama__locked_x: each bucket's lock */
#include "kama_isolate.h"    /* kama_parfor_workers: whether another CPU could change the cell while this one polls */
#if defined(__APPLE__)
  #include <pthread.h>
  #if __has_include(<os/os_sync_wait_on_address.h>)
    #include <os/os_sync_wait_on_address.h>
    #include <os/clock.h>
    #define KAMA__WAIT_OS_SYNC 1
  #endif
#elif defined(__linux__)
  #include <sys/syscall.h>
  extern long syscall(long, ...);     /* as kama_isolate.h declares it: <unistd.h> may hide it under -std=c11 */
  #define KAMA__FUTEX_WAIT_PRIVATE 128 /* FUTEX_WAIT | FUTEX_PRIVATE_FLAG: a stable kernel ABI constant */
  #define KAMA__FUTEX_WAKE_PRIVATE 129 /* FUTEX_WAKE | FUTEX_PRIVATE_FLAG */
#elif defined(__EMSCRIPTEN__)
  #include <emscripten/threading.h>
#endif

// W6: how long a thread polls the cell before it queues. Measured 2026-11-07 on the M4 (bench/jobs/wait.c, `spin<ns>`):
// polls of 1, 2 and 4 µs all hand off in about 60 ns, against libc++'s 68, and 1 µs burns the least CPU on a wait that
// goes on to sleep (1.7 µs beyond none, against libc++'s 9.3). Read the clock every 16 polls.
#define KAMA__WAIT_POLL_NS 1000u
#define KAMA__WAIT_BUCKETS 256

typedef struct kama__wait_rec {
    struct kama__wait_rec* next;
    struct kama__wait_rec* prev;
    const void*            addr;     /* the cell waited on */
    void*                  fiber;    /* the parked fiber, or NULL for a thread */
    uint32_t               woken;    /* a thread sleeps on this word; set once a notify took the record off */
    int32_t                linked;   /* queued in its bucket (the bucket's lock) */
#if defined(__APPLE__)
    pthread_mutex_t        m;        /* below macOS 14.4 only: the thread's own condvar */
    pthread_cond_t         c;
#endif
} kama__wait_rec;

typedef struct {
    _Alignas(64) kama__xlock lock;
    uint32_t                 count;  /* records queued here (atomic): a notify that reads zero takes no lock */
    kama__wait_rec*          head;
    kama__wait_rec*          tail;
} kama__wait_bucket;

extern kama__wait_bucket kama__wait_table[KAMA__WAIT_BUCKETS];

static inline kama__wait_bucket* kama__wait_bucket_of(const void* cell) {
    uint64_t x = (uint64_t)(uintptr_t)cell;
    x ^= x >> 17;
    x *= 0x9E3779B97F4A7C15ull;
    return &kama__wait_table[x >> 56];
}
static inline void kama__wait_lock(kama__wait_bucket* b, int on) { kama__locked_x(&b->lock, on); }

static inline void kama__wait_link(kama__wait_bucket* b, kama__wait_rec* w) {
    w->next = NULL; w->prev = b->tail; w->linked = 1;
    if (b->tail) b->tail->next = w; else b->head = w;
    b->tail = w;
}
static inline void kama__wait_unlink(kama__wait_bucket* b, kama__wait_rec* w) {
    if (w->prev) w->prev->next = w->next; else b->head = w->next;
    if (w->next) w->next->prev = w->prev; else b->tail = w->prev;
    w->next = w->prev = NULL; w->linked = 0;
    __atomic_sub_fetch(&b->count, 1, __ATOMIC_SEQ_CST);
}

// Below macOS 14.4 there is no public wait on an address, so a thread sleeps on its own condvar (decision 14).
static inline int kama__wait_cv(void) {
#if defined(KAMA__WAIT_OS_SYNC)
    if (__builtin_available(macOS 14.4, iOS 17.4, *)) return 0;
    return 1;
#elif defined(__APPLE__)
    return 1;
#else
    return 0;
#endif
}

#if defined(_WIN32)
// WaitOnAddress and WakeByAddressSingle are the API set api-ms-win-core-synch-l1-2-0's (Windows 8 on). mingw-w64's kernel32
// leaves them out and zig's mingw-w64 has no synchronization.lib, so they are looked up once, as libc++ and Microsoft's STL
// look them up, and no Windows toolchain needs a library for them.
static inline void* kama__wait_win(const char* name, void** cache) {
    void* f = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
    if (!f) {
        HMODULE m = GetModuleHandleW(L"api-ms-win-core-synch-l1-2-0.dll");
        f = m ? (void*)GetProcAddress(m, name) : NULL;
        if (!f) {
            static const char msg[] = "Atomic.wait: this Windows has no WaitOnAddress (Windows 8 and later have it)";
            kama_panic(kama_string_lit(msg, sizeof msg - 1));
        }
        __atomic_store_n(cache, f, __ATOMIC_RELEASE);
    }
    return f;
}
#endif

// Sleep on `word` while it holds 0, for at most `ns` (KAMA__WAIT_NONE: no limit). It may return early; the caller looks.
static inline void kama__wait_os(uint32_t* word, uint64_t ns) {
#if defined(KAMA__WAIT_OS_SYNC)
    if (__builtin_available(macOS 14.4, iOS 17.4, *)) {
        if (ns == KAMA__WAIT_NONE) (void)os_sync_wait_on_address(word, 0, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        else (void)os_sync_wait_on_address_with_timeout(word, 0, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                         OS_CLOCK_MACH_ABSOLUTE_TIME, ns);
    }
#elif defined(__linux__)
    struct timespec t, *tp = NULL;
    if (ns != KAMA__WAIT_NONE) { t.tv_sec = (time_t)(ns / 1000000000u); t.tv_nsec = (long)(ns % 1000000000u); tp = &t; }
    (void)syscall(__NR_futex, word, KAMA__FUTEX_WAIT_PRIVATE, 0u, tp, NULL, 0);
#elif defined(_WIN32)
    static void* fn;
    int (__stdcall* waitOnAddress)(volatile void*, void*, size_t, unsigned long) =
        (int (__stdcall*)(volatile void*, void*, size_t, unsigned long))kama__wait_win("WaitOnAddress", &fn);
    uint32_t zero = 0;
    unsigned long ms = 0xFFFFFFFFul;   /* INFINITE */
    if (ns != KAMA__WAIT_NONE) { const uint64_t m = (ns + 999999u) / 1000000u; ms = m < 0xFFFFFFFEu ? (unsigned long)m : 0xFFFFFFFEul; }
    (void)waitOnAddress(word, &zero, sizeof zero, ms);
#elif defined(__EMSCRIPTEN__)
    (void)emscripten_futex_wait(word, 0, ns == KAMA__WAIT_NONE ? __builtin_inf() : (double)ns / 1e6);
#else
    (void)word; (void)ns;
#endif
}
static inline void kama__wait_os_wake(uint32_t* word) {
#if defined(KAMA__WAIT_OS_SYNC)
    if (__builtin_available(macOS 14.4, iOS 17.4, *)) (void)os_sync_wake_by_address_any(word, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
#elif defined(__linux__)
    (void)syscall(__NR_futex, word, KAMA__FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
#elif defined(_WIN32)
    static void* fn;
    ((void (__stdcall*)(void*))kama__wait_win("WakeByAddressSingle", &fn))(word);
#elif defined(__EMSCRIPTEN__)
    (void)emscripten_futex_wake(word, 1);
#else
    (void)word;
#endif
}

// A thread's sleep until its record is taken off and woken (0), or until `by` passes (1).
static inline int kama__wait_sleep(kama__wait_rec* w, int cv, uint64_t by) {
#if defined(__APPLE__)
    if (cv) {
        int late = 0;
        pthread_mutex_lock(&w->m);
        while (!__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE)) {
            if (by == KAMA__WAIT_NONE) { pthread_cond_wait(&w->c, &w->m); continue; }
            const uint64_t now = kama_now_mono_ns();
            if (now >= by) { late = 1; break; }
            struct timespec t;
            t.tv_sec = (time_t)((by - now) / 1000000000u); t.tv_nsec = (long)((by - now) % 1000000000u);
            (void)pthread_cond_timedwait_relative_np(&w->c, &w->m, &t);
        }
        pthread_mutex_unlock(&w->m);
        return late;
    }
#else
    (void)cv;
#endif
    for (;;) {
        if (__atomic_load_n(&w->woken, __ATOMIC_ACQUIRE)) return 0;
        uint64_t left = KAMA__WAIT_NONE;
        if (by != KAMA__WAIT_NONE) {
            const uint64_t now = kama_now_mono_ns();
            if (now >= by) return 1;
            left = by - now;
        }
        kama__wait_os(&w->woken, left);
    }
}

// Whether a thread polls before it queues: only where another CPU could change the cell meanwhile. Read once per unit.
static inline int kama__wait_polls(void) {
    static int cpus;
    int n = __atomic_load_n(&cpus, __ATOMIC_RELAXED);
    if (n == 0) { n = kama_parfor_workers(); __atomic_store_n(&cpus, n, __ATOMIC_RELAXED); }
    return n > 1;
}
static inline int kama__wait_polled(void* cell, const void* old, void* seen, size_t width) {
    if (!kama__wait_polls()) return 0;
    const uint64_t t0 = kama_now_mono_ns();
    for (unsigned k = 1;; ++k) {
        KAMA__WAIT_PAUSE();
        if (kama__wait_changed(cell, old, seen, width)) return 1;
        if ((k & 15u) == 0 && kama_now_mono_ns() - t0 >= KAMA__WAIT_POLL_NS) return 0;
    }
}

// A fork's child has only the thread that forked, and a bucket lock another thread held at that moment would stay held
// in it forever: the child's first wait or notify in that bucket never returned (bench/jobs/probes/pool_seam/, mode 6,
// on Linux and macOS). So the child starts with the table empty and every lock open, which is its zeroed state: the
// records queued there belong to threads the child does not have. Armed by the first wait that queues, which is the
// first moment any thread can hold a bucket's lock; once per unit, and a second arming only repeats the reset.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
static inline void kama__wait_afterFork(void) { __builtin_memset(kama__wait_table, 0, sizeof kama__wait_table); }
static inline void kama__wait_arm(void) { (void)pthread_atfork(NULL, NULL, kama__wait_afterFork); }
#endif
static inline void kama__wait_forks(void) {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)pthread_once(&once, kama__wait_arm);
#endif
}

static inline int32_t kama__wait_queued(void* cell, const void* old, void* seen, size_t width, uint64_t by) {
    kama__wait_forks();
    void* fiber = NULL;
#if defined(KAMA__PARK_FIBERS)
    if (kama__park_here()) fiber = kama__park_self();
#endif
    if (!fiber && kama__wait_polled(cell, old, seen, width)) return 0;
    kama__wait_bucket* b = kama__wait_bucket_of(cell);
    kama__wait_rec w;
    w.addr = cell; w.fiber = fiber; w.linked = 0;
    const int cv = !fiber && kama__wait_cv();
#if defined(__APPLE__)
    if (cv) { pthread_mutex_init(&w.m, NULL); pthread_cond_init(&w.c, NULL); }
#endif
    int32_t r;
    for (;;) {
        kama__wait_lock(b, 1);
        __atomic_add_fetch(&b->count, 1, __ATOMIC_SEQ_CST);   /* before the look: the Dekker pair with notify's fence */
        if (kama__wait_changed(cell, old, seen, width)) { __atomic_sub_fetch(&b->count, 1, __ATOMIC_SEQ_CST); kama__wait_lock(b, 0); r = 0; break; }
        if (by != KAMA__WAIT_NONE && kama_now_mono_ns() >= by) { __atomic_sub_fetch(&b->count, 1, __ATOMIC_SEQ_CST); kama__wait_lock(b, 0); r = 1; break; }
        __atomic_store_n(&w.woken, 0, __ATOMIC_RELAXED);
        kama__wait_link(b, &w);
        kama__wait_lock(b, 0);
        if (fiber) {
#if defined(KAMA__PARK_FIBERS)
            (void)kama__park_switch(by == KAMA__WAIT_NONE ? 0 : by);
            kama__wait_lock(b, 1);
            if (w.linked) kama__wait_unlink(b, &w);   /* nobody took it off: the deadline, or a stale wake */
            kama__wait_lock(b, 0);
#endif
        } else if (kama__wait_sleep(&w, cv, by)) {
            kama__wait_lock(b, 1);
            const int mine = w.linked;
            if (mine) kama__wait_unlink(b, &w);
            kama__wait_lock(b, 0);
            if (!mine) (void)kama__wait_sleep(&w, cv, KAMA__WAIT_NONE);   /* taken off: `woken` is about to be set */
        }
        if (cv) { kama__wait_lock(b, 1); kama__wait_lock(b, 0); }   /* the notifier is done with the condvar */
        if (kama__wait_changed(cell, old, seen, width)) { r = 0; break; }   /* the deadline is the loop's top's to judge */
    }
#if defined(__APPLE__)
    if (cv) { pthread_cond_destroy(&w.c); pthread_mutex_destroy(&w.m); }
#endif
    return r;
}
#endif /* KAMA__WAIT_TABLE */

// `Atomic.waitUntil`: wait while the `width`-byte cell holds the bytes at `old`, until `by` (a kama_now_mono_ns time;
// KAMA__WAIT_NONE for none). 0 when it loaded another value, 1 when the deadline passed first; `seen` holds the last
// value loaded either way.
static inline int32_t kama_atomic_wait_until(void* cell, const void* old, void* seen, size_t width, uint64_t by) {
    if (kama__wait_changed(cell, old, seen, width)) return 0;
#if KAMA__WAIT_HOW == KAMA__WAIT_TABLE
    return kama__wait_queued(cell, old, seen, width, by);
#elif KAMA__WAIT_HOW == KAMA__WAIT_ALONE
    if (by == KAMA__WAIT_NONE) {
        static const char msg[] = "Atomic.wait: nothing in this program can change the cell — a wasm build with no "
                                  "threads runs nothing else, so this wait would never end";
        kama_panic(kama_string_lit(msg, sizeof msg - 1));
    }
    while (kama_now_mono_ns() < by) { }
    return kama__wait_changed(cell, old, seen, width) ? 0 : 1;
#else
    for (;;) {
        KAMA__WAIT_PAUSE();
        if (kama__wait_changed(cell, old, seen, width)) return 0;
        if (by != KAMA__WAIT_NONE && kama_now_mono_ns() >= by) return 1;
    }
#endif
}

// `Atomic.wait`: `waitUntil` with no deadline. Its own entry so that on bare metal a wait with no deadline never names
// the clock, which a board supplies there (KR-341): a board that declares none still links a program that waits.
static inline void kama_atomic_wait(void* cell, const void* old, void* seen, size_t width) {
#if KAMA__WAIT_HOW == KAMA__WAIT_POLL
    while (!kama__wait_changed(cell, old, seen, width)) KAMA__WAIT_PAUSE();
#else
    (void)kama_atomic_wait_until(cell, old, seen, width, KAMA__WAIT_NONE);
#endif
}

// `notifyOne` (all = 0) and `notifyAll` (all = 1): wake waiters on `cell`. A store to the cell comes first.
static inline void kama_atomic_notify(void* cell, int32_t all) {
#if KAMA__WAIT_HOW == KAMA__WAIT_TABLE
    kama__wait_bucket* b = kama__wait_bucket_of(cell);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);   /* the store before this notify is ordered before the count's read */
    if (__atomic_load_n(&b->count, __ATOMIC_RELAXED) == 0) return;
    const int cv = kama__wait_cv();
    kama__wait_rec* woke = NULL;
    kama__wait_lock(b, 1);
    for (kama__wait_rec* w = b->head; w; ) {
        kama__wait_rec* const n = w->next;
        if (w->addr == cell) {
            kama__wait_unlink(b, w);
#if defined(KAMA__PARK_FIBERS)
            if (w->fiber) kama__park_wake(w->fiber);
            else
#endif
            if (cv) {
#if defined(__APPLE__)
                pthread_mutex_lock(&w->m);
                __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
                pthread_cond_signal(&w->c);
                pthread_mutex_unlock(&w->m);
#endif
            } else { w->next = woke; woke = w; }
            if (!all) break;
        }
        w = n;
    }
    kama__wait_lock(b, 0);
    while (woke) {
        /* Read everything first: once `woken` is set the waiter may leave, and its record is on its stack. */
        kama__wait_rec* const n = woke->next;
        uint32_t* const word = &woke->woken;
        __atomic_store_n(word, 1, __ATOMIC_RELEASE);
        kama__wait_os_wake(word);
        woke = n;
    }
#else
    (void)cell; (void)all;
#endif
}

#if defined(KAMA__PARK_FIBERS)
#include "kama_sched.h"
#endif
#endif
