// The C half of tests/fiber_sched.d: drives include/kama_sched.h directly. Each returns 0, or the failed check.
int fiber_sched_selftest(void);         // this host's own arm
int fiber_sched_selftest_poller(void);  // the Poller arm, forced
