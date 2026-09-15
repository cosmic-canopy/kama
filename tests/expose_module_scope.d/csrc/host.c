#include "host.h"
/* The host sees each module's `tick` under its own symbol — the module path joined with `_`. */
extern int exposemods_alpha_tick(void);
extern int exposemods_beta_tick(void);
int host_sum(void) { return exposemods_alpha_tick() + exposemods_beta_tick(); }
