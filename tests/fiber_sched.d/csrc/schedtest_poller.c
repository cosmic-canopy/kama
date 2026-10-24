// schedtest.c again, on the Poller arm: the one Windows (AFD) and a POSIX host with neither epoll nor kqueue take.
#define KAMA__SCHED_GENERIC 1
#define fiber_sched_selftest fiber_sched_selftest_poller
#include "schedtest.c"
