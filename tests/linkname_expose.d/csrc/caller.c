#include "caller.h"
extern int host_tick(void);
int kama_test_call_tick(void) { return host_tick() + 1; }
