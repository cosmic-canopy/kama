// omp.c — the OpenMP arm of KR-3's pool benchmark, with gcc's libgomp: a team that stays alive between regions, which is the
// fastest barrier in common use, and what its workers burn to be that fast (OMP_WAIT_POLICY, GOMP_SPINCOUNT). The shapes and
// the usage are jobs.kama's. Built only where the compiler has OpenMP: Apple's clang does not, so this arm runs in the Linux
// image.
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

int main(void) {
    const char* shape = getenv("BENCH_SHAPE") ? getenv("BENCH_SHAPE") : "tight";
    int workers = setting("BENCH_WORKERS", 4);
    int calls = setting("BENCH_CALLS", 2000);
    int elems = setting("BENCH_ELEMS", 1024);
    int64_t slot = (int64_t)setting("BENCH_SLOT_US", 1389) * 1000;
    int frame = !strcmp(shape, "frame");
    int rounds = setting("BENCH_ROUNDS", 0);
    uint32_t* xs = calloc((size_t)elems, sizeof *xs);
    int64_t* t = calloc((size_t)calls, sizeof *t);
    int64_t total = 0, check = 0, next = now_ns();
    for (int c = 0; c < calls; ++c) {
        if (frame) { next += slot; while (now_ns() < next) { } }
        int64_t t0 = now_ns();
#pragma omp parallel for num_threads(workers) schedule(static)
        for (int i = 0; i < elems; ++i) {
            uint32_t x = xs[i] + 1u;
            for (int r = 0; r < rounds; ++r) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
            xs[i] = x;
        }
        t[c] = now_ns() - t0;
        total += t[c];
    }
    for (int i = 0; i < elems; ++i) check += xs[i];
    qsort(t, (size_t)calls, sizeof *t, cmp64);
    printf("%s\t%d\t%d\t%d\t%lld\t%lld\t%lld\t%lld\n", shape, workers, calls, elems, (long long)total,
           (long long)t[calls / 2], (long long)t[calls - 1 - calls / 100], (long long)check);
    return 0;
}
