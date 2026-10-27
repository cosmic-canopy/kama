#ifndef KAMA_ISOLATE_H
#define KAMA_ISOLATE_H

// The native isolate seam — spawn a top-level fn on a fresh OS thread with a MOVED-in argument
// bundle, and join it. Shared-nothing by construction: the entry takes ownership of the bundle, so
// no memory is aliased across the thread boundary (the whole M2 concurrency model in one header).
//
// A dedicated pay-for-what-you-use header (like kama_os.h / kama_app.h), pulled in only by a program
// that `extern "kama_isolate.h";`'s — NOT folded into kama_runtime.h, which must stay usable on the
// freestanding/MCU path where <pthread.h> does not exist. (And pthread_t is not void* on glibc, so
// the block-scoped-extern trick kama_runtime.h uses elsewhere would be unsafe here.)

#include <pthread.h>
#include <limits.h>        /* PTHREAD_STACK_MIN */
#include "kama_runtime.h"   /* kama_panic, kama_string_lit */
#include "kama_fiber.h"     /* the stack-overflow handler an isolate's thread arms as it starts */

// A spawned isolate: its thread, what the thread runs, and how its end reaches a fiber parked in its join (KR-3 step 6).
// The spawn allocates it and the join frees it, so its address is the handle — a held `Isolate` keeps it as its
// `UnsafePtr` — and the thread reads it to its last instruction, which is safe because the join frees it only once
// `pthread_join` has seen the thread exit.
typedef struct kama__isolate {
    pthread_t th;
    void*   (*entry)(void*);
    void*     arg;
    size_t    stack;
    int32_t   done;      /* the entry has returned and its scheduler is gone: set by the thread, read by a parked join */
    void*     joiner;    /* the fiber parked in the join, or NULL */
} kama__isolate;
typedef kama__isolate* kama_isolate_t;

// The fiber half (KR-3 step 6, fibers.md decision 8): in a program that runs fibers, a join on an isolate with a
// scheduler parks the joining fiber — the root included, whose park runs the others — until the isolate has ended,
// instead of blocking the thread in `pthread_join`. The scheduler's half is defined in kama_sched.h, which this header
// includes at its end.
#if defined(KAMA_FIBERS) && !defined(__EMSCRIPTEN__)
#define KAMA__ISOLATE_FIBERS 1
static inline void kama__isolate_park(kama__isolate* c);    // on an isolate with a scheduler, park until `c` has ended
static inline void kama__isolate_ended(kama__isolate* c);   // `c`'s thread is done: wake a fiber parked in its join
#endif

// An isolate's stack is STATED, not the OS's. Left to `pthread_create(&t, NULL, …)` it was 512 KiB on
// macOS, 8 MiB with glibc and 128 KiB with musl — three answers to "how deep may this recursion go", so a
// program that worked on its main thread crashed in an isolate on two of them. It is the main thread's own
// size. Natively that is 8 MiB on a 64-bit target (Linux's and macOS's main thread; kama links Windows' up from
// 1 MiB to match) and 2 MiB on a 32-bit one, whose address space would not hold many 8 MiB reservations — and it
// is RESERVED, not committed: an isolate that never recurses deeply touches a few pages of it. On wasm it is
// whatever the main thread has, which is what an unsized pthread gets there (emscripten's `STACK_SIZE`, with
// `DEFAULT_PTHREAD_STACK_SIZE` unset): kama states 256 KiB, and a project that raises it in `emSettings` raises
// both. It stays small because every stack is carved out of the module's one fixed linear memory.
#if !defined(__EMSCRIPTEN__)
#if UINTPTR_MAX > 0xFFFFFFFFu
#define KAMA_ISOLATE_STACK ((size_t)8 << 20)
#else
#define KAMA_ISOLATE_STACK ((size_t)2 << 20)
#endif
#endif

// What a new isolate's thread is handed: its entry and bundle, and the stack it was given, which an overflow names.
// The thread arms the overflow handler (kama_fiber.h) before it runs any kama, so a recursion that outgrows the
// stack says "isolate stack overflow (8 MiB; `spawn(stack: …)` to raise it)" rather than dying of a bare SIGSEGV.
// wasm's stacks are emscripten's, so there the entry is started as it is.
#if !defined(__EMSCRIPTEN__)
static void* kama__isolate_run(void* p) {
    kama__isolate* c = (kama__isolate*)p;
    kama__stack_isolate(c->stack);
    void* r = c->entry(c->arg);
    if (kama__fibers.schedEnd) kama__fibers.schedEnd();   // an isolate that ran fibers frees its scheduler and poller
#if defined(KAMA__ISOLATE_FIBERS)
    kama__isolate_ended(c);
#endif
    return r;
}
#endif

// Start `entry` on a new thread with `arg` (the moved bundle pointer), with a `stack`-byte stack when `sized`. A
// size is rounded up to a multiple of 64 KiB, the largest page any target uses (macOS refuses a size that is not
// a page multiple), and never below the platform's own thread minimum — 128 KiB on Linux arm64, where glibc's
// `PTHREAD_STACK_MIN` is larger than one 64 KiB unit and a smaller size is refused. Panics on failure — spawn is
// not a recoverable condition in the M2 surface (no channel to report back over yet).
static inline kama_isolate_t kama__isolate_start(void* (*entry)(void*), void* arg, int sized, size_t stack) {
    static const char big[] = "isolate spawn failed: the stack size was refused";
    size_t given = stack;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) kama_panic(kama_string_lit("isolate spawn failed", 20));
    if (sized) {
        const size_t unit = (size_t)64 << 10;
        size_t least = unit;
#if defined(PTHREAD_STACK_MIN)
        if ((size_t)PTHREAD_STACK_MIN > least) least = (size_t)PTHREAD_STACK_MIN;   /* a sysconf call on newer glibc */
#endif
        size_t want = stack < least ? least : stack;
        given = (want + unit - 1) & ~(unit - 1);
        if (want > (size_t)-1 - (unit - 1) || pthread_attr_setstacksize(&attr, given) != 0) {
            pthread_attr_destroy(&attr);
            kama_panic(kama_string_lit(big, sizeof big - 1));
        }
    }
    kama__isolate* c = (kama__isolate*)kama_alloc(sizeof *c, _Alignof(kama__isolate));
    if (!c) { pthread_attr_destroy(&attr); kama_panic(kama_string_lit("isolate spawn failed", 20)); }
    c->entry = entry; c->arg = arg; c->stack = given; c->done = 0; c->joiner = NULL;
#if defined(__EMSCRIPTEN__)
    const int rc = pthread_create(&c->th, &attr, entry, arg);
#else
    const int rc = pthread_create(&c->th, &attr, kama__isolate_run, c);
#endif
    pthread_attr_destroy(&attr);
    if (rc != 0) { kama_free(c, sizeof *c, _Alignof(kama__isolate)); kama_panic(kama_string_lit("isolate spawn failed", 20)); }
    return c;
}

// The stated size (above). Its own entry rather than a reserved value of `stack`, so no size a program can write —
// a `stack:` expression that comes to 0 at run time included — means "the default".
static inline kama_isolate_t kama_isolate_spawn(void* (*entry)(void*), void* arg) {
#if defined(__EMSCRIPTEN__)
    return kama__isolate_start(entry, arg, 0, 0);
#else
    return kama__isolate_start(entry, arg, 1, KAMA_ISOLATE_STACK);
#endif
}
// `spawn(stack: n)` — what the spawn site asked for.
static inline kama_isolate_t kama_isolate_spawn_sized(void* (*entry)(void*), void* arg, size_t stack) {
    return kama__isolate_start(entry, arg, 1, stack);
}

// Wait until the isolate finishes, and free its handle. Its result is dropped inside the trampoline, so there is
// nothing to hand back; the join is the RAII/structured-lifetime barrier. On an isolate that runs fibers the joining
// fiber parks until the isolate has ended, and the `pthread_join` after it is then the thread's last instructions.
static inline void kama_isolate_join(kama_isolate_t h) {
#if defined(KAMA__ISOLATE_FIBERS)
    kama__isolate_park(h);
#endif
    pthread_join(h->th, NULL);
    kama_free(h, sizeof *h, _Alignof(kama__isolate));
}

// The handle of the RAII `Isolate` type (`Isolate h = spawn worker(...);`), which kama holds as an opaque `UnsafePtr`.
// A handle is already a pointer to what the join frees, so it is its own box. The `Isolate` owns the join, so dropping
// it (scope exit) joins — a forgotten join can't orphan the thread.
static inline void* kama_isolate_spawn_boxed(void* (*entry)(void*), void* arg) { return kama_isolate_spawn(entry, arg); }
static inline void* kama_isolate_spawn_boxed_sized(void* (*entry)(void*), void* arg, size_t stack) {
    return kama_isolate_spawn_sized(entry, arg, stack);
}
static inline void kama_isolate_join_boxed(void* h) { kama_isolate_join((kama_isolate_t)h); }

// The default worker count for `parallel_for` (M6.3) when no build-time override is set: the machine's
// logical-core count. Because a parallel_for's slices are disjoint and joined at a barrier, this only
// affects speed, never the result — the call site caps it at the collection length. Reached from kama as
// `std::concurrent::cpuCount()`, which is what a `parallel_for (…, workers: cpuCount())` calls.
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
static inline int kama_parfor_workers(void) { int n = emscripten_num_logical_cores(); return n > 0 ? n : 1; }
#elif defined(_WIN32)
// mingw-w64 ships <unistd.h> but no sysconf, so the POSIX branch below did not merely misreport the core
// count on Windows — it failed to COMPILE, and took every fixture that reaches this header with it
// (isolate, channel, atomic, parfor, scope, shared, ecs: 31 of the 48 failures on the Windows leg).
//
// winpthreads answers this itself, and <pthread.h> is already included above — which is the whole reason
// to prefer it over GetActiveProcessorCount. That would mean either <windows.h>, which cannot be included
// here (a TU that also uses kama_os.h needs <winsock2.h> to come FIRST — see the note at the top of that
// header), or a hand-declared prototype that clang warns about the moment windows.h declares it too, and
// a warning is a failed build in this suite.
static inline int kama_parfor_workers(void) { int n = pthread_num_processors_np(); return n > 0 ? n : 1; }
#else
#include <unistd.h>
static inline int kama_parfor_workers(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

#if defined(KAMA__ISOLATE_FIBERS)
#include "kama_sched.h"
#endif
#endif
