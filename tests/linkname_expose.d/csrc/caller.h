#ifndef KAMA_TEST_LINK_CALLER_H
#define KAMA_TEST_LINK_CALLER_H
/* The host side: it knows the kama module only by the symbol `host_tick`, which kama exports under
   `@linkName` — the kama function is called `tick`. */
int kama_test_call_tick(void);
#endif
