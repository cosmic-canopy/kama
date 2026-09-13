#include "pair.h"
int32_t takes_pair(Pair p) { return p.a * 10 + p.b; }
Pair    make_pair(int32_t a) { Pair p = { a, a + 1 }; return p; }
int32_t host_calls(void)     { Pair p = { 3, 4 }; return sum_pair(p); }
