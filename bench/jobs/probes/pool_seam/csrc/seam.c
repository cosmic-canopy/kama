// The C half of the pool seam's probe (KR-3 step 2, before commit 3): workers started here and never joined, each running
// kama (src/main.kama) that sleeps in `Atomic.wait`, as the pool's workers will. PROBE picks what is asked:
//   0  return from `main` with every worker asleep          5  the same, with a block nothing reaches: LeakSanitizer is live
//   1  `exit(7)` with every worker asleep                   2  a signal `main` blocks: which thread runs its handler?
//   3  a stack overflow on a worker                         4  `fork`: do the child's workers answer?
//   6  `fork` while another thread holds the bucket lock of `Atomic.wait`'s table that the child then notifies through
// MASKED: 0 starts workers with the starter's signal mask, 1 with every asynchronous signal blocked (the recommendation),
// 2 with the synchronous ones blocked too. Mode 6 reads it as "hold the lock". node hands an emscripten program no
// environment, so a wasm build takes the mode as -DFORCE_MODE=<m> -DFORCE_N=<workers> on --cc.
#define _GNU_SOURCE 1
#include "kama_isolate.h"
#include "kama_wait.h"
#include "seam.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern void probe_worker(int32_t index);   // src/main.kama, exposed
extern void probe_recurse(int32_t depth);

static uint32_t cell, bell;
static int arrived, acked, overflowMode;
void* seam_cell(void) { return &cell; }
void* seam_bell(void) { return &bell; }
void seam_arrive(void) { __atomic_add_fetch(&arrived, 1, __ATOMIC_ACQ_REL); }
void seam_ack(void) { __atomic_add_fetch(&acked, 1, __ATOMIC_ACQ_REL); }

static void* run(void* p) {
#if !defined(__EMSCRIPTEN__)
    kama__stack_isolate(KAMA_ISOLATE_STACK);   // the overflow handler covers this stack, as an isolate's start does
#endif
    if (overflowMode) probe_recurse(0);
    probe_worker((int32_t)(intptr_t)p);
    return NULL;
}

// Start `n` detached workers. `masked` 1 blocks every asynchronous signal around `pthread_create`, so each worker
// inherits that mask, and leaves the synchronous ones open; 2 blocks those too.
int32_t seam_start(int32_t n, int32_t masked, int32_t overflow) {
    sigset_t all, old;
    overflowMode = overflow;
    if (masked) {
        sigfillset(&all);
        if (masked == 1) {
            sigdelset(&all, SIGSEGV); sigdelset(&all, SIGBUS); sigdelset(&all, SIGFPE); sigdelset(&all, SIGILL);
            sigdelset(&all, SIGTRAP);
        }
        pthread_sigmask(SIG_SETMASK, &all, &old);
    }
    for (int32_t i = 0; i < n; ++i) {
        pthread_attr_t a; pthread_t t;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
#if !defined(__EMSCRIPTEN__)
        pthread_attr_setstacksize(&a, KAMA_ISOLATE_STACK);
#endif
        if (pthread_create(&t, &a, run, (void*)(intptr_t)i) != 0) { fprintf(stderr, "pthread_create failed\n"); exit(90); }
        pthread_attr_destroy(&a);
    }
    if (masked) pthread_sigmask(SIG_SETMASK, &old, NULL);
    return n;
}

static void sleepMs(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static void waitArrived(int n) { while (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) < n) sleepMs(1); sleepMs(20); }

#if !defined(__EMSCRIPTEN__)
static pthread_t mainThread;
static volatile sig_atomic_t onMain, onWorker;
static void onUsr1(int s) { (void)s; if (pthread_equal(pthread_self(), mainThread)) onMain++; else onWorker++; }

// Ring the bell: each worker asleep on it acks once. The acks seen within `ms`.
static int ring(int ms) {
    __atomic_store_n(&bell, 1u, __ATOMIC_RELEASE);
    kama_atomic_notify(&bell, 1);
    for (int k = 0; k < ms && __atomic_load_n(&acked, __ATOMIC_ACQUIRE) == 0; ++k) sleepMs(1);
    sleepMs(20);
    return __atomic_load_n(&acked, __ATOMIC_ACQUIRE);
}

// Hold the bell's bucket lock, as a waiter does for a moment while it queues, for a second.
static void* holdBucket(void* p) {
    kama__wait_bucket* b = kama__wait_bucket_of(&bell);
    kama__wait_lock(b, 1);
    __atomic_store_n((int*)p, 1, __ATOMIC_RELEASE);
    sleepMs(1000);
    kama__wait_lock(b, 0);
    return NULL;
}
#endif

int32_t seam_probe(int32_t mode, int32_t n, int32_t masked) {
#if defined(FORCE_MODE)
    mode = FORCE_MODE; n = FORCE_N;
#endif
    if (mode == 0) {
        seam_start(n, masked, 0); waitArrived(n);
        printf("%d asleep, returning\n", __atomic_load_n(&arrived, __ATOMIC_ACQUIRE)); fflush(stdout);
        return 0;
    }
    if (mode == 1) {
        seam_start(n, masked, 0); waitArrived(n);
        printf("%d asleep, exit(7)\n", __atomic_load_n(&arrived, __ATOMIC_ACQUIRE)); fflush(stdout);
        exit(7);
    }
    if (mode == 5) {
        seam_start(n, masked, 0); waitArrived(n);
        static void* volatile lost; lost = malloc(4096); memset(lost, 1, 4096); lost = 0;
        return 0;
    }
#if !defined(__EMSCRIPTEN__)
    mainThread = pthread_self();
    if (mode == 2) {
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = onUsr1; sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        seam_start(n, masked, 0); waitArrived(n);
        sigset_t u; sigemptyset(&u); sigaddset(&u, SIGUSR1);
        for (int r = 0; r < 50; ++r) {
            pthread_sigmask(SIG_BLOCK, &u, NULL);
            kill(getpid(), SIGUSR1);
            sleepMs(2);
            pthread_sigmask(SIG_UNBLOCK, &u, NULL);
            sleepMs(1);
        }
        printf("signals masked=%d: handler on main %d, on a worker %d\n", masked, (int)onMain, (int)onWorker);
        return 0;
    }
    if (mode == 3) {
        seam_start(1, masked, 1); sleepMs(3000);
        printf("overflow masked=%d: still running 3 s later\n", masked);
        return 1;
    }
    if (mode == 4) {
        seam_start(n, masked, 0); waitArrived(n);
        pid_t c = fork();
        if (c == 0) {
            int a = ring(300);
            printf("child: %d workers counted, %d acked the bell\n", __atomic_load_n(&arrived, __ATOMIC_ACQUIRE), a);
            fflush(stdout); _exit(0);
        }
        int st; waitpid(c, &st, 0);
        printf("parent: %d workers counted, %d acked the bell\n", __atomic_load_n(&arrived, __ATOMIC_ACQUIRE), ring(300));
        return 0;
    }
    if (mode == 6) {
        seam_start(n, 1, 0); waitArrived(n);   // asleep on the bell, so its bucket's count is above zero
        int held = 0; pthread_t h;
        if (masked) { pthread_create(&h, NULL, holdBucket, &held); while (!__atomic_load_n(&held, __ATOMIC_ACQUIRE)) sleepMs(1); }
        pid_t c = fork();
        if (c == 0) {
            alarm(3);
            kama_atomic_notify(&bell, 1);
            printf("child: the notify returned\n"); fflush(stdout); _exit(0);
        }
        int st; waitpid(c, &st, 0);
        if (WIFSIGNALED(st)) printf("child (lock held=%d): killed by signal %d, the notify never returned\n", masked, WTERMSIG(st));
        if (masked) pthread_join(h, NULL);
        return 0;
    }
#endif
    return 99;
}
