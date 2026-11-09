// The C half of tests/csources_threaded.d: a count every thread adds to, and one per thread. An atomic and a thread-local
// are each enough for a wasm object compiled without `-pthread` to be refused by a threaded link.
#include "count.h"
static int32_t all;
static _Thread_local int32_t mine;
int32_t count_bump(void) { mine++; return __atomic_add_fetch(&all, 1, __ATOMIC_SEQ_CST); }
int32_t count_total(void) { return __atomic_load_n(&all, __ATOMIC_SEQ_CST); }
int32_t count_mine(void) { return mine; }
