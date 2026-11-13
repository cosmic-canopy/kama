// The C half of tests/csources_em_settings.d: an atomic and a thread-local, either enough for a wasm object compiled
// without shared memory to be refused by a link that has it.
#include "count.h"
static _Atomic int32_t all;
static _Thread_local int32_t mine;
int32_t count_bump(void) { mine++; return ++all; }
int32_t count_mine(void) { return mine; }
