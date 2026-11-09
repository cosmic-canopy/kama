// A fork's child after the job pool has started (KR-3 step 2; bench/jobs/probes/pool_seam/ modes 4 and 6). The child has only
// the thread that forked, so:
//   - its pool is unstarted again: the parent's root would post work to workers it does not have. Its first `parallel_for`
//     starts a pool of its own, and finishes (forks_child, in kama);
//   - `Atomic.wait`'s table is empty and every lock in it open: here a thread of the parent waits on `bell`, so its bucket
//     counts one waiter, and another thread holds that bucket's lock at the fork. Until `0.9.889` the child's notify on
//     `bell` took the lock forever, and the alarm below ended the child.
// No fork on Windows or wasm: 0 there, which the kama side reads as a pass.
#define _GNU_SOURCE 1
#include "forks.h"
#include "kama_wait.h"
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern void* kama__pool_root;
extern int32_t forks_child(void);   // src/main.kama

static uint32_t bell;
static int held, waiting;
static void nap(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

static void* waiter(void* p) {
    (void)p;
    uint32_t zero = 0, seen;
    __atomic_store_n(&waiting, 1, __ATOMIC_RELEASE);
    kama_atomic_wait(&bell, &zero, &seen, 4);
    return NULL;
}
static void* holder(void* p) {
    (void)p;
    kama__wait_bucket* b = kama__wait_bucket_of(&bell);
    kama__wait_lock(b, 1);
    __atomic_store_n(&held, 1, __ATOMIC_RELEASE);
    nap(300);
    kama__wait_lock(b, 0);
    return NULL;
}

int32_t forks_check(void) {
    if ((uintptr_t)__atomic_load_n(&kama__pool_root, __ATOMIC_ACQUIRE) <= 1) return 10;   // main ran a parallel_for first
    pthread_t w, h;
    pthread_create(&w, NULL, waiter, NULL);
    while (!__atomic_load_n(&waiting, __ATOMIC_ACQUIRE)) nap(1);
    while (__atomic_load_n(&kama__wait_bucket_of(&bell)->count, __ATOMIC_ACQUIRE) == 0) nap(1);   // queued, asleep
    pthread_create(&h, NULL, holder, NULL);
    while (!__atomic_load_n(&held, __ATOMIC_ACQUIRE)) nap(1);
    const pid_t c = fork();
    if (c == 0) {
        alarm(10);
        if (__atomic_load_n(&kama__pool_root, __ATOMIC_ACQUIRE) != NULL) _exit(11);
        kama_atomic_notify(&bell, 1);
#if defined(__SANITIZE_THREAD__)
        _exit(0);   // TSan refuses a thread started after a threaded process forks: the child's own pool is not its to test
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
        _exit(0);
#endif
#endif
        _exit(forks_child() == 42 ? 0 : 12);
    }
    int st = 0;
    waitpid(c, &st, 0);
    pthread_join(h, NULL);
    __atomic_store_n(&bell, 1u, __ATOMIC_SEQ_CST);
    kama_atomic_notify(&bell, 1);
    pthread_join(w, NULL);
    if (WIFSIGNALED(st)) return 13;
    return WEXITSTATUS(st);
}
#else
int32_t forks_check(void) { return 0; }
#endif
