// The C half of tests/fiber_switch.d (KR-3 step 1): include/kama_fiber.h driven directly, with no `spawn fiber` yet.
// The overflow check runs in a child process, which must die of SIGABRT having said what overflowed; Windows has no
// fork, and its handler is covered by tests/trap/isolate_stack_overflow instead.
#include "kama_fiber.h"
#include "selftest.h"
#include <stdio.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

static kama_fiber* root;
static long counter;
static void pingpong(void* arg) { long n = (long)(intptr_t)arg; for (long i = 0; i < n; i++) { counter++; kama_fiber_switch(kama_fiber_current(), root); } }
static long deep(long n) { volatile char pad[512]; pad[0] = (char)n; return n == 0 ? 0 : deep(n - 1) + 1 + (pad[0] - (char)n); }
static long deepGot;
static void goDeep(void* arg) { deepGot = deep((long)(intptr_t)arg); }
static int rr[100];
static kama_fiber* fs[100];
static void roundRobin(void* arg) { int i = (int)(intptr_t)arg; for (int k = 0; k < 10; k++) { rr[i]++; kama_fiber_switch(fs[i], root); } }
static double fsum;
static void floaty(void* arg) { (void)arg; double a = 1.5; for (int i = 0; i < 1000; i++) { a = a * 1.0001 + 0.5; kama_fiber_switch(kama_fiber_current(), root); } fsum = a; }
#if defined(__aarch64__)
static void clobber(void* arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("mov x19, #1\n mov x20, #2\n mov x21, #3\n mov x22, #4\n mov x23, #5\n mov x24, #6\n mov x25, #7\n mov x26, #8\n mov x27, #9\n mov x28, #10\n fmov d8, #1.0\n fmov d9, #2.0\n fmov d10, #3.0\n fmov d11, #4.0\n fmov d12, #5.0\n fmov d13, #6.0\n fmov d14, #7.0\n fmov d15, #8.0"
                         ::: "x19","x20","x21","x22","x23","x24","x25","x26","x27","x28","d8","d9","d10","d11","d12","d13","d14","d15");
        kama_fiber_switch(kama_fiber_current(), root);
    }
}
static int regsKept(kama_fiber* f) {
    register uint64_t a __asm__("x19") = 0x1919; register uint64_t b __asm__("x28") = 0x2828;
    register double c __asm__("d8") = 8.25; register double d __asm__("d15") = 15.75;
    __asm__ volatile("" : "+r"(a), "+r"(b), "+w"(c), "+w"(d));
    kama_fiber_switch(root, f);
    __asm__ volatile("" : "+r"(a), "+r"(b), "+w"(c), "+w"(d));
    return a == 0x1919 && b == 0x2828 && c == 8.25 && d == 15.75;
}
#elif defined(__x86_64__)
static void clobber(void* arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("movq $1, %%rbx\n movq $2, %%r12\n movq $3, %%r13\n movq $4, %%r14\n movq $5, %%r15" ::: "rbx","r12","r13","r14","r15");
        kama_fiber_switch(kama_fiber_current(), root);
    }
}
static int regsKept(kama_fiber* f) {
    register uint64_t a __asm__("rbx") = 0x1919; register uint64_t b __asm__("r12") = 0x1212;
    register uint64_t c __asm__("r13") = 0x1313; register uint64_t d __asm__("r15") = 0x1515;
    __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d));
    kama_fiber_switch(root, f);
    __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d));
    return a == 0x1919 && b == 0x1212 && c == 0x1313 && d == 0x1515;
}
#endif
static void overflow(void* arg) { (void)arg; deep(100000000); }

int fiber_selftest(void) {
    root = kama_fiber_root();
    kama_fiber* p = kama_fiber_new(pingpong, (void*)(intptr_t)100000, 0);
    while (!p->done) kama_fiber_switch(root, p);
    if (counter != 100000) return 1;
    kama_fiber_free(p);
    kama_fiber* d = kama_fiber_new(goDeep, (void*)(intptr_t)300, 0);   // ~300 frames of 560 B, inside 256 KiB
    kama_fiber_switch(root, d);
    if (!d->done || deepGot != 300) return 2;
#if !defined(_WIN32)
    char* lo = d->lo;
    kama_fiber_free(d);
    kama_fiber* again = kama_fiber_new(goDeep, (void*)(intptr_t)10, 0);
    if (again->lo != lo) return 3;                                      // the idle stack was reused
    kama_fiber_switch(root, again);
    kama_fiber_free(again);
#else
    kama_fiber_free(d);
#endif
    for (int i = 0; i < 100; i++) fs[i] = kama_fiber_new(roundRobin, (void*)(intptr_t)i, 64 << 10);
    for (int k = 0; k < 11; k++) for (int i = 0; i < 100; i++) if (!fs[i]->done) kama_fiber_switch(root, fs[i]);
    for (int i = 0; i < 100; i++) { if (rr[i] != 10 || !fs[i]->done) return 4; kama_fiber_free(fs[i]); }
    kama_fiber* fl = kama_fiber_new(floaty, 0, 0);
    while (!fl->done) kama_fiber_switch(root, fl);
    double a = 1.5; for (int i = 0; i < 1000; i++) a = a * 1.0001 + 0.5;
    if (fsum != a) return 5;
    kama_fiber_free(fl);
#if defined(__aarch64__) || defined(__x86_64__)
    kama_fiber* c = kama_fiber_new(clobber, 0, 0);
    for (int i = 0; i < 100; i++) if (!regsKept(c)) return 6;
#endif
#if !defined(_WIN32)
    int fds[2];
    if (pipe(fds) != 0) return 7;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 2); close(fds[0]);
        kama_fiber* f = kama_fiber_new(overflow, 0, 0);
        kama_fiber_switch(kama_fiber_root(), f);
        _exit(8);
    }
    close(fds[1]);
    char buf[512]; size_t got = 0; ssize_t n;
    while (got < sizeof buf - 1 && (n = read(fds[0], buf + got, sizeof buf - 1 - got)) > 0) got += (size_t)n;
    buf[got] = 0;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGABRT) return 10;
    if (!strstr(buf, "kama: fiber stack overflow (256 KiB; `spawn fiber(stack: …)` to raise it)")) return 11;
#endif
    return 0;
}
