// omp.c — the OpenMP arm of KR-3's pool benchmark, with gcc's libgomp: a team that stays alive between regions, which is the
// fastest barrier in common use, and what its workers burn to be that fast (OMP_WAIT_POLICY, GOMP_SPINCOUNT). The shapes and
// the usage are jobs.kama's. Built only where the compiler has OpenMP: Apple's clang does not, so this arm runs in the Linux
// image. BENCH_CALLERS=2 runs the loop on two threads, each of which libgomp gives a team of its own; BENCH_BLOCKERS=b adds a
// thread whose regions of b iterations each sleep BENCH_BLOCK_MS in the OS, until the caller is done.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int setting(const char* name, int dflt) { const char* v = getenv(name); return v && *v ? atoi(v) : dflt; }
static int cmp64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return x < y ? -1 : x > y;
}

// One caller: `calls` barriers over its own `xs`, each one's time in `t`.
typedef struct { uint32_t* xs; int64_t* t; int calls, elems, workers, rounds, frame; int64_t slot; } drive_arg;
static void* drive(void* p) {
    drive_arg* a = (drive_arg*)p;
    uint32_t* xs = a->xs;
    int elems = a->elems, rounds = a->rounds;
    // libgomp starts a caller's team at its first region, so one runs before the clock does, as pool.c's `pool_start` does.
#pragma omp parallel num_threads(a->workers)
    { }
    int64_t next = now_ns();
    for (int c = 0; c < a->calls; ++c) {
        if (a->frame) { next += a->slot; while (now_ns() < next) { } }
        int64_t t0 = now_ns();
#pragma omp parallel for num_threads(a->workers) schedule(static)
        for (int i = 0; i < elems; ++i) {
            uint32_t x = xs[i] + 1u;
            for (int r = 0; r < rounds; ++r) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
            xs[i] = x;
        }
        a->t[c] = now_ns() - t0;
    }
    return NULL;
}

typedef struct { int k; int ms; atomic_int* done; } block_arg;
static void* block(void* p) {
    block_arg* a = (block_arg*)p;
    struct timespec d = { 0, (long)a->ms * 1000000L };
    while (!atomic_load_explicit(a->done, memory_order_acquire)) {
#pragma omp parallel for num_threads(a->k) schedule(static, 1)
        for (int i = 0; i < a->k; ++i) nanosleep(&d, NULL);
    }
    return NULL;
}

int main(void) {
    const char* shape = getenv("BENCH_SHAPE") ? getenv("BENCH_SHAPE") : "tight";
    int workers = setting("BENCH_WORKERS", 4);
    int calls = setting("BENCH_CALLS", 2000);
    int elems = setting("BENCH_ELEMS", 1024);
    int callers = setting("BENCH_CALLERS", 1) == 2 ? 2 : 1;
    int blockers = setting("BENCH_BLOCKERS", 0);
    int n = calls * callers;   // samples
    int64_t* t = calloc((size_t)n, sizeof *t);
    drive_arg d[2];
    for (int i = 0; i < callers; ++i)
        d[i] = (drive_arg){ calloc((size_t)elems, sizeof(uint32_t)), t + (size_t)i * calls, calls, elems, workers,
                            setting("BENCH_ROUNDS", 0), !strcmp(shape, "frame"), (int64_t)setting("BENCH_SLOT_US", 1389) * 1000 };
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
    int64_t total = 0, check = 0;
    for (int i = 0; i < n; ++i) total += t[i];
    for (int i = 0; i < callers; ++i)
        for (int e = 0; e < elems; ++e) check += d[i].xs[e];
    qsort(t, (size_t)n, sizeof *t, cmp64);
    printf("%s\t%d\t%d\t%d\t%lld\t%lld\t%lld\t%lld\n", shape, workers, calls, elems, (long long)total,
           (long long)t[n / 2], (long long)t[n - 1 - n / 100], (long long)check);
    return 0;
}
