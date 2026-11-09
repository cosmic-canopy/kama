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

#if defined(KAMA_TARGET_EMBEDDED)
// Bare metal (KR-341): one core is one isolate, since a module `static` is one plain C global there. The compiler
// refuses every form that would start a second (`spawn`, `parallel_spawn`, `parallel_for`) and `Isolate` is gated to
// hosted targets, so all this header keeps is `cpuCount()`, which is 1, and none of the thread machinery below.
static inline int kama_parfor_workers(void) { return 1; }
#else

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

// A `parallel_for`'s brace: join every chunk it started. A name of its own, so the job walk can tell a nested
// `parallel_for`, whose waiter will take work once a pool runs the chunks, from a task's join (KR-3 decision 5).
static inline void kama_isolate_join_chunks(kama_isolate_t* h, int n) {
    for (int j = 0; j < n; ++j) kama_isolate_join(h[j]);
}

// The handle of the RAII `Isolate` type (`Isolate h = spawn worker(...);`), which kama holds as an opaque `UnsafePtr`.
// A handle is already a pointer to what the join frees, so it is its own box. The `Isolate` owns the join, so dropping
// it (scope exit) joins — a forgotten join can't orphan the thread.
static inline void* kama_isolate_spawn_boxed(void* (*entry)(void*), void* arg) { return kama_isolate_spawn(entry, arg); }
static inline void* kama_isolate_spawn_boxed_sized(void* (*entry)(void*), void* arg, size_t stack) {
    return kama_isolate_spawn_sized(entry, arg, stack);
}
static inline void kama_isolate_join_boxed(void* h) { kama_isolate_join((kama_isolate_t)h); }

// The default worker count for `parallel_for` (M6.3) when no build-time override is set: the CPUs this
// process may use, which is not always the machine's count (below). Because a parallel_for's slices are disjoint and joined at a barrier, this only
// affects speed, never the result — the call site caps it at the collection length. Reached from kama as
// `std::concurrent::cpuCount()`, which is what a `parallel_for (…, chunks: cpuCount())` calls.
#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>
static inline int kama_parfor_workers(void) { int n = emscripten_num_logical_cores(); return n > 0 ? n : 1; }
#elif defined(_WIN32)
// How many CPUs this process may use: its affinity mask, and a job object's hard CPU rate cap, which is what a
// Windows container's `--cpus` sets. Measured on 20 CPUs: under an affinity of 2, winpthreads'
// `pthread_num_processors_np` (the mask's popcount) said 2, Go 2 and Rust's `available_parallelism` 20; under a cap
// of 2 CPUs' worth, all three said 20. A cap rounds up, as Linux's quota does. Only the process's own job is read:
// a cap on an enclosing job binds too, and Windows does not name it.
//
// Past 64 CPUs (more than one processor group, reasoned from the documentation; no box here has one) the mask is
// the primary group's, and it reads 0 once the process's threads span groups, as they may from Windows 11 on.
// `pthread_num_processors_np` answered 1 then; this answers every processor instead.
//
// Declared rather than included: headers are emitted in name order, and <windows.h> belongs to kama_os.h (as in
// kama_fiber.h). Each matches its <windows.h> declaration, so a unit that sees both compiles: the job's class is an
// enum there, which is an `unsigned int` here since no value is negative.
__declspec(dllimport) void* __stdcall GetCurrentProcess(void);
__declspec(dllimport) int __stdcall GetProcessAffinityMask(void* process, kama__win_size* mask, kama__win_size* system);
__declspec(dllimport) unsigned long __stdcall GetActiveProcessorCount(unsigned short group);
__declspec(dllimport) int __stdcall QueryInformationJobObject(void* job, unsigned int cls, void* info, unsigned long size,
                                                              unsigned long* got);
static inline int kama__usableCpus(void) {
    long all = (long)GetActiveProcessorCount(0xffff);               /* ALL_PROCESSOR_GROUPS */
    long n = all;
    kama__win_size mask = 0, sys = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &mask, &sys) && mask) n = __builtin_popcountll(mask);
    struct { unsigned long flags; unsigned long rate; } cap = {0, 0};   /* JOBOBJECT_CPU_RATE_CONTROL_INFORMATION */
    if (QueryInformationJobObject(0, 15 /* JobObjectCpuRateControlInformation */, &cap, sizeof cap, 0) && (cap.flags & 1)) {
        unsigned long rate = (cap.flags & 4) ? cap.rate                /* HARD_CAP: a share of every processor, in 1/10000 */
                           : (cap.flags & 0x10) ? (cap.rate >> 16) : 0; /* MIN_MAX_RATE: its MaxRate, the same unit */
        long quota = rate ? (long)((rate * (unsigned long long)all + 9999) / 10000) : 0;
        if (quota > 0 && quota < n) n = quota;
    }
    return n > 0 ? (int)n : 1;
}
#elif defined(__linux__)
// How many CPUs this process may use, which is not how many the machine has: `sysconf` sees neither an affinity mask nor a
// cgroup's quota, so under `taskset -c 0,1` on 6 CPUs it said 6 where Rust's `available_parallelism` and Go's `GOMAXPROCS`
// say 2, and a pool sized by it on a large host in a small container spins against the quota. A quota rounds up, as Go's and
// Java's do, since 1.5 CPUs can run two threads. Raw `open` and `read`, since `cpuCount()` may sit in `@noheap` code and
// `fopen` allocates.
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
extern long syscall(long, ...);   // <unistd.h> declares it only under a feature set this TU may have read it without

static inline int kama__readSmall(const char* path, char* into, int cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    long got = read(fd, into, (size_t)cap - 1);
    close(fd);
    if (got <= 0) return 0;
    into[got] = 0;
    return 1;
}
static inline long kama__quotaCpus(long quota, long period) {
    return (quota > 0 && period > 0) ? (quota + period - 1) / period : 0;
}
// The quota set on one cgroup directory: v2's `cpu.max` ("max 100000", "200000 100000") or v1's two files. 0 = none, or no file.
static inline long kama__cgroupDirCpus(const char* dir, int v1) {
    char path[4200], text[64];
    if (!v1) {
        snprintf(path, sizeof path, "%s/cpu.max", dir);
        if (!kama__readSmall(path, text, sizeof text) || text[0] == 'm') return 0;
        char* rest;
        long quota = strtol(text, &rest, 10);
        return kama__quotaCpus(quota, strtol(rest, NULL, 10));
    }
    snprintf(path, sizeof path, "%s/cpu.cfs_quota_us", dir);
    if (!kama__readSmall(path, text, sizeof text)) return 0;
    long quota = strtol(text, NULL, 10);
    snprintf(path, sizeof path, "%s/cpu.cfs_period_us", dir);
    if (!kama__readSmall(path, text, sizeof text)) return 0;
    return kama__quotaCpus(quota, strtol(text, NULL, 10));
}
// The tightest quota from `base` + `cg` up through its ancestors, since a parent's limit binds its children. In a container
// `cg` can name a host path the mount does not hold: those reads fail, and the mount's root answers.
static inline long kama__cgroupWalkCpus(const char* base, const char* cg, int len, int v1) {
    char dir[4096];
    long best = 0;
    for (;;) {
        snprintf(dir, sizeof dir, "%s%.*s", base, len, cg);
        long here = kama__cgroupDirCpus(dir, v1);
        if (here > 0 && (best == 0 || here < best)) best = here;
        if (len == 0) return best;
        while (len > 0 && cg[--len] != '/') { }
    }
}
// `selfCgroup` is /proc/self/cgroup, one `id:controllers:path` per line: `0::/path` on v2, `4:cpu,cpuacct:/path` on v1; `root`
// is /sys/fs/cgroup. Parameters only so a fixture can hand it a tree. 0 = no quota anywhere.
static inline long kama__cgroupCpus(const char* selfCgroup, const char* root) {
    char text[4096], base[4096];
    if (!kama__readSmall(selfCgroup, text, sizeof text)) return 0;
    long best = 0;
    for (char* line = text; line && *line; ) {
        char* end = strchr(line, '\n');
        char* c1 = strchr(line, ':');
        char* c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (c2 && (!end || c2 < end)) {
            const char* cg = c2 + 1;
            int len = (int)(end ? end - cg : (long)strlen(cg));
            if (len == 1 && cg[0] == '/') len = 0;
            int ctl = (int)(c2 - c1 - 1);
            long here = 0;
            if (ctl == 0) {
                here = kama__cgroupWalkCpus(root, cg, len, 0);
            } else if ((ctl >= 3 && !strncmp(c1 + 1, "cpu,", 4)) || (ctl == 3 && !strncmp(c1 + 1, "cpu", 3))
                       || (ctl > 4 && !strncmp(c2 - 4, ",cpu", 4)) || (ctl > 5 && strstr(c1 + 1, ",cpu,") && strstr(c1 + 1, ",cpu,") < c2)) {
                snprintf(base, sizeof base, "%s/cpu,cpuacct", root);
                here = kama__cgroupWalkCpus(base, cg, len, 1);
                long named;
                snprintf(base, sizeof base, "%s/cpu", root);
                named = kama__cgroupWalkCpus(base, cg, len, 1);
                if (named > 0 && (here == 0 || named < here)) here = named;
            }
            if (here > 0 && (best == 0 || here < best)) best = here;
        }
        line = end ? end + 1 : NULL;
    }
    return best;
}
static inline int kama__affinityCpus(void) {
    unsigned long mask[128];   // 8,192 CPUs, the kernel's largest
    long bytes = syscall(__NR_sched_getaffinity, 0, sizeof mask, mask);
    if (bytes <= 0) return 0;
    int n = 0;
    for (long i = 0; i < bytes / (long)sizeof(unsigned long); ++i) n += __builtin_popcountl(mask[i]);
    return n;
}
static inline int kama__usableCpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    long allowed = kama__affinityCpus();
    if (allowed > 0 && (n <= 0 || allowed < n)) n = allowed;
    long quota = kama__cgroupCpus("/proc/self/cgroup", "/sys/fs/cgroup");
    if (quota > 0 && (n <= 0 || quota < n)) n = quota;
    return n > 0 ? (int)n : 1;
}
#else
#include <unistd.h>
static inline int kama_parfor_workers(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif
#if !defined(__EMSCRIPTEN__) && (defined(_WIN32) || defined(__linux__))
// Read once: racing first callers compute one answer, and a pool is sized from it once.
static inline int kama_parfor_workers(void) {
    static int known;
    int n = __atomic_load_n(&known, __ATOMIC_RELAXED);
    if (n == 0) { n = kama__usableCpus(); __atomic_store_n(&known, n, __ATOMIC_RELAXED); }
    return n;
}
#endif

#if defined(KAMA__ISOLATE_FIBERS)
#include "kama_sched.h"
#endif
#endif  // KAMA_TARGET_EMBEDDED
#endif
