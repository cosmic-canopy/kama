#ifndef KAMA_POOL_H
#define KAMA_POOL_H
// kama_pool.h — the job pool's seam (KR-3 step 2; ROADMAP_DETAIL `s6-pools`, decisions 4, 6 and 14). The pool itself is kama,
// lib/std/concurrent/jobs.kama; this is what kama cannot say there:
//   - the process's one pool. A module `static` is per thread, so the root is a C global, defined once by the emitter
//     (emitRuntimeSlotDefinitions) and set by a compare-exchange in kama, so two first hand-outs start one pool;
//   - a worker's start: detached and never joined (a process ends with its workers asleep, which returning from `main`
//     and `exit` both do at once, measured on every leg by bench/jobs/probes/pool_seam/), on an isolate's stack with its
//     overflow handler armed, and with every asynchronous signal blocked, so a signal a program expects on its own
//     thread does not land in a worker. The synchronous ones stay open: blocked, a worker's stack overflow lost kama's
//     message (a bare segmentation fault on Linux, a worker that never came back on macOS);
//   - a fork's child, which has none of the parent's workers: its pool goes back to unstarted, and its first hand-out
//     starts one of its own. `Atomic.wait`'s table resets itself (kama_wait.h);
//   - a pause for a spin, the clock the spin reads, and whether the caller is a fiber.
// Hosted only: on bare metal a `parallel_for` runs every chunk on its caller, and this is never read.
#include "kama_runtime.h"   /* kama_alloc_zeroed, kama_panic */
#include "kama_isolate.h"   /* <pthread.h>, KAMA_ISOLATE_STACK, and kama_fiber.h's kama__stack_isolate */
#include "kama_wait.h"      /* KAMA__WAIT_PAUSE, kama_now_mono_ns, and on an isolate that runs fibers kama__park_here */
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <signal.h>
#endif

// NULL until a pool starts, 1 while one starts, then the pool (jobs.kama's `JobPool`).
extern void* kama__pool_root;
static inline void* kama_pool_root_cell(void) { return &kama__pool_root; }

// The pool's own memory, zeroed: every field of it starts at zero, which is each one's empty state.
static inline void* kama_pool_block(size_t n, size_t align) {
    void* p = kama_alloc_zeroed(n, align);
    if (!p) kama_panic(kama_string_lit("out of memory", 13));
    return p;
}

static inline void kama_pool_pause(void) { KAMA__WAIT_PAUSE(); }
static inline uint64_t kama_pool_now(void) { return kama_now_mono_ns(); }

// On an isolate that runs fibers a hand-out's caller is a fiber, the root included: it takes no chunks and parks at the
// barrier, since a chunk would stall every other fiber on its thread (decision 6).
static inline int32_t kama_pool_on_fiber(void) {
#if defined(KAMA__PARK_FIBERS)
    return kama__park_here();
#else
    return 0;
#endif
}

typedef struct { void (*entry)(void*); void* arg; } kama__pool_go;
static inline void* kama__pool_go_run(void* p) {
    const kama__pool_go go = *(kama__pool_go*)p;
    kama_free(p, sizeof go, _Alignof(kama__pool_go));
#if !defined(__EMSCRIPTEN__)
    kama__stack_isolate(KAMA_ISOLATE_STACK);   /* as an isolate's thread arms it (kama_isolate.h) */
#endif
    go.entry(go.arg);
    return NULL;
}

// Start a worker running `entry(arg)`. It is never joined. Panics if the thread cannot start, as a `spawn` does.
static inline void kama_pool_spawn(void (*entry)(void*), void* arg) {
    static const char msg[] = "the job pool could not start a thread";
    kama__pool_go* go = (kama__pool_go*)kama_pool_block(sizeof *go, _Alignof(kama__pool_go));
    go->entry = entry; go->arg = arg;
    pthread_attr_t a;
    if (pthread_attr_init(&a) != 0) kama_panic(kama_string_lit(msg, sizeof msg - 1));
    (void)pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
#if !defined(__EMSCRIPTEN__)
    (void)pthread_attr_setstacksize(&a, KAMA_ISOLATE_STACK);
#endif
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    sigset_t async, was;
    sigfillset(&async);
    sigdelset(&async, SIGSEGV); sigdelset(&async, SIGBUS); sigdelset(&async, SIGFPE);
    sigdelset(&async, SIGILL); sigdelset(&async, SIGTRAP);
    (void)pthread_sigmask(SIG_SETMASK, &async, &was);
#endif
    pthread_t t;
    const int rc = pthread_create(&t, &a, kama__pool_go_run, go);
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    (void)pthread_sigmask(SIG_SETMASK, &was, NULL);
#endif
    pthread_attr_destroy(&a);
    if (rc != 0) kama_panic(kama_string_lit(msg, sizeof msg - 1));
}

// By the pool's start: a fork's child begins with no pool. Registered once; a child inherits the registration.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
static inline void kama__pool_afterFork(void) { kama__pool_root = NULL; }
static inline void kama__pool_arm(void) { (void)pthread_atfork(NULL, NULL, kama__pool_afterFork); }
#endif
static inline void kama_pool_forks(void) {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    (void)pthread_once(&once, kama__pool_arm);
#endif
}

#endif
