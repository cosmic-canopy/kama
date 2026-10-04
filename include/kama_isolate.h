#ifndef KAMA_ISOLATE_H
#define KAMA_ISOLATE_H

// The native isolate seam — spawn a top-level fn on a fresh OS thread with a MOVED-in argument
// bundle, and join it. Shared-nothing by construction: the entry takes ownership of the bundle, so
// no memory is aliased across the thread boundary (the whole M2 concurrency model in one header).
//
// A dedicated pay-for-what-you-use header (like kama_os.h / kama_gpu.h), pulled in only by a program
// that `extern "kama_isolate.h";`'s — NOT folded into kama_runtime.h, which must stay usable on the
// freestanding/MCU path where <pthread.h> does not exist. (And pthread_t is not void* on glibc, so
// the block-scoped-extern trick kama_runtime.h uses elsewhere would be unsafe here.)

#include <pthread.h>
#include <limits.h>        /* PTHREAD_STACK_MIN */
#include "kama_runtime.h"   /* kama_panic, kama_string_lit */

typedef pthread_t kama_isolate_t;

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

// Start `entry` on a new thread with `arg` (the moved bundle pointer), with a `stack`-byte stack when `sized`. A
// size is rounded up to a multiple of 64 KiB, the largest page any target uses (macOS refuses a size that is not
// a page multiple), and never below the platform's own thread minimum — 128 KiB on Linux arm64, where glibc's
// `PTHREAD_STACK_MIN` is larger than one 64 KiB unit and a smaller size is refused. Panics on failure — spawn is
// not a recoverable condition in the M2 surface (no channel to report back over yet).
static inline kama_isolate_t kama__isolate_start(void* (*entry)(void*), void* arg, int sized, size_t stack) {
    static const char big[] = "isolate spawn failed: the stack size was refused";
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) kama_panic(kama_string_lit("isolate spawn failed", 20));
    if (sized) {
        const size_t unit = (size_t)64 << 10;
        size_t least = unit;
#if defined(PTHREAD_STACK_MIN)
        if ((size_t)PTHREAD_STACK_MIN > least) least = (size_t)PTHREAD_STACK_MIN;   /* a sysconf call on newer glibc */
#endif
        size_t want = stack < least ? least : stack;
        if (want > (size_t)-1 - (unit - 1) || pthread_attr_setstacksize(&attr, (want + unit - 1) & ~(unit - 1)) != 0) {
            pthread_attr_destroy(&attr);
            kama_panic(kama_string_lit(big, sizeof big - 1));
        }
    }
    pthread_t t;
    const int rc = pthread_create(&t, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) kama_panic(kama_string_lit("isolate spawn failed", 20));
    return t;
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

// Block until the isolate finishes. Its result is dropped inside the trampoline, so there is nothing
// to hand back (M2 has no channels); the join is purely the RAII/structured-lifetime barrier.
static inline void kama_isolate_join(kama_isolate_t h) { pthread_join(h, NULL); }

// Heap-BOXED handle, for the RAII `Isolate` type (`Isolate h = isolate worker(...);`). kama holds the box
// as an opaque `UnsafePtr` (void*) — portable, since a bare kama_isolate_t is not always pointer-sized. spawn_boxed
// allocates the box and spawns; join_boxed joins the thread and frees the box. The handle owns the join, so
// dropping it (scope exit) joins — a forgotten join can't orphan the thread.
static inline void* kama__isolate_box(kama_isolate_t t) {
    kama_isolate_t* box = (kama_isolate_t*)kama_alloc(sizeof(kama_isolate_t), _Alignof(kama_isolate_t));
    if (!box) kama_panic(kama_string_lit("isolate spawn failed", 20));
    *box = t;
    return box;
}
static inline void* kama_isolate_spawn_boxed(void* (*entry)(void*), void* arg) {
    return kama__isolate_box(kama_isolate_spawn(entry, arg));
}
static inline void* kama_isolate_spawn_boxed_sized(void* (*entry)(void*), void* arg, size_t stack) {
    return kama__isolate_box(kama_isolate_spawn_sized(entry, arg, stack));
}
static inline void kama_isolate_join_boxed(void* h) {
    kama_isolate_t* box = (kama_isolate_t*)h;
    kama_isolate_join(*box);
    kama_free(box, sizeof(kama_isolate_t), _Alignof(kama_isolate_t));
}

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

#endif
