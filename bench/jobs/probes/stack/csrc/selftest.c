// Probe 8: can a pool worker run one job on a stack of its stated size, through the switch KR-3 already owns?
#include "kama_fiber.h"
#include "selftest.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif
static inline int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static kama_fiber* root;
typedef struct { long depth; long got; } job;
static long deep(long n) { volatile char pad[512]; pad[0] = (char)n; return n == 0 ? 0 : deep(n - 1) + 1 + (pad[0] - (char)n); }
static void onStack(void* arg) { job* j = (job*)arg; j->got = deep(j->depth); for (;;) kama_fiber_switch(kama_fiber_current(), root); }
static void quick(void* arg) { job* j = (job*)arg; for (;;) { j->got++; kama_fiber_switch(kama_fiber_current(), root); } }
static void onceThenBack(void* arg) { job* j = (job*)arg; j->got++; kama_fiber_switch(kama_fiber_current(), root); }
static int64_t cmp;
int fiber_selftest(void) {
    root = kama_fiber_root();
    // Deeper than a pool thread's stated 8 MiB (100 000 frames of over 512 B is over 50 MiB), on a 64 MiB stack.
    job j = { 100000, 0 };
    kama_fiber* f = kama_fiber_new(onStack, &j, (size_t)64 << 20);
    kama_fiber_switch(root, f);
    printf("deep on 64 MiB: got=%ld\n", j.got);
    kama_fiber_free(f);
    // What a stated-stack job costs: a stack made and freed per job, and one kept and reused.
    enum { N = 2000 };
    job q = { 0, 0 };
    int64_t a[7], b[7];
    for (int r = 0; r < 7; ++r) {
        int64_t t0 = now_ns();
        for (int i = 0; i < N; ++i) { kama_fiber* x = kama_fiber_new(onceThenBack, &q, (size_t)64 << 20); kama_fiber_switch(root, x); kama_fiber_free(x); }
        a[r] = (now_ns() - t0) / N;
        kama_fiber* k = kama_fiber_new(quick, &q, (size_t)64 << 20);
        int64_t t1 = now_ns();
        for (int i = 0; i < N * 100; ++i) kama_fiber_switch(root, k);
        b[r] = (now_ns() - t1) / (N * 100);
        kama_fiber_free(k);
    }
    int64_t amin = a[0], bmin = b[0];
    for (int r = 1; r < 7; ++r) { if (a[r] < amin) amin = a[r]; if (b[r] < bmin) bmin = b[r]; }
    printf("per job, stack made and freed: %lld ns; stack kept (a switch there and back): %lld ns; got=%ld\n", (long long)amin, (long long)bmin, q.got);
#if !defined(_WIN32)
    // Outgrowing it: the handler the fiber stacks already have must speak.
    fflush(stdout);
    pid_t c = fork();
    if (c == 0) { job over = { 100000000, 0 }; kama_fiber* o = kama_fiber_new(onStack, &over, (size_t)256 << 10); kama_fiber_switch(kama_fiber_root(), o); _exit(0); }
    int st = 0; waitpid(c, &st, 0);
    printf("overflow child: signalled=%d sig=%d\n", WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
    return 0;
}
