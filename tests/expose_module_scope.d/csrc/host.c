#include "host.h"
/* The host sees each module's `tick` under its own symbol — the module path joined with `_` — and declares
   neither by hand: `exposemods.h` is the header kama generates for the project, on this file's include path. */
#include "exposemods.h"
int host_sum(void) { return exposemods_alpha_tick() + exposemods_beta_tick(); }
