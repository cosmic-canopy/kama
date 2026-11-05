#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
int main(void){ cpu_set_t s; sched_getaffinity(0,sizeof s,&s); printf("kama cpuCount() today (sysconf) = %ld, affinity allows = %d\n", sysconf(_SC_NPROCESSORS_ONLN), CPU_COUNT(&s)); return 0; }
