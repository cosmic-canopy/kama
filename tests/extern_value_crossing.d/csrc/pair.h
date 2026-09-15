/* The ONE place Pair's layout is stated. kama binds it with `type extern value` and the host includes it,
   so a Pair crossing by value means the same bytes on both sides, in both directions (KR-45). */
#ifndef KAMA_TEST_PAIR_H
#define KAMA_TEST_PAIR_H
#include <stdint.h>
typedef struct Pair { int32_t a; int32_t b; } Pair;
int32_t takes_pair(Pair p);    /* C, called by kama */
Pair    make_pair(int32_t a);  /* C, called by kama, returned by value */
int32_t externcross_sum_pair(Pair p);   /* kama (`expose fn sum_pair` in project `externcross`), called by C */
int32_t host_calls(void);      /* C, calls back into kama */
#endif
