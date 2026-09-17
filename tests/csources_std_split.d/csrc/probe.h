/* Included by BOTH translation units: `gnu.c` (a `csources` entry, compiled as GNU C) and kama's own
   generated C (through `extern "probe.h"`, compiled as ISO C11). `__STRICT_ANSI__` is what tells the
   two modes apart, so each TU reports the mode it was actually compiled in. */
#ifndef KAMA_TEST_PROBE_H
#define KAMA_TEST_PROBE_H
#ifdef __STRICT_ANSI__
#define KAMA_TEST_THIS_TU_IS_ISO 1
#else
#define KAMA_TEST_THIS_TU_IS_ISO 0
#endif
static inline int kama_test_kama_tu_is_iso(void) { return KAMA_TEST_THIS_TU_IS_ISO; }
int kama_test_csources_tu_is_iso(void);
#endif
