#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
static void* idle(void* a) { (void)a; pause(); return 0; }
int main(int argc, char** argv) {
    if (argc > 1) { pthread_t t; pthread_create(&t, 0, idle, 0); usleep(10000); }
    int r = unshare(CLONE_NEWUSER);
    printf("%s: unshare(CLONE_NEWUSER) = %d (%s)\n", argc > 1 ? "with one other thread" : "single-threaded", r, r ? strerror(errno) : "ok");
    return 0;
}
