// clockcost.c — what a kernel clock read costs here, over a system call that reads no clock (Linux). Prints whole ns.
//
// run.sh measures this before a server run and refuses one where it is more than 200 ns. Loopback TCP reads the clock
// several times a packet, so where the hypervisor traps the kernel's counter reads, a run measures the trap rather
// than the servers: podman's libkrun VM did when it ran nested (2026-10-27, about 1,000 ns here against about 30).
// The vDSO's clock_gettime reads the counter in user space and is not this; a real system call is.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }

int main(void) {
    enum { N = 20000, TRIES = 9 };
    double best = 1e18;
    for (int k = 0; k < TRIES; ++k) {                  // interleaved, and the least: drift hits both alike
        struct timespec t;
        double a = now();
        for (int i = 0; i < N; ++i) syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &t);
        double b = now();
        for (int i = 0; i < N; ++i) syscall(SYS_getppid);
        double c = now();
        double over = ((b - a) - (c - b)) / N;
        if (over < best) best = over;
    }
    printf("%.0f\n", best < 0 ? 0 : best);
    return 0;
}
