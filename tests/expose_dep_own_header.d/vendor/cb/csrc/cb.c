/* The package's own C. Its `expose fn`s' prototypes, and the `cb_Pair` one takes, come from the header kama writes
   for this package, by the name a build of the package alone gives it. */
#include "cb.h"
#include "cb.kama.h"

/* The program's `expose fn two` is `hdrapp_two`. This package's header leaves it out; declared there, this would not
   compile ("static declaration follows non-static declaration"). */
static double hdrapp_two(double x) { return x; }

int32_t cb_c_run(void)
{
    cb_Pair p = { 20, (int32_t)hdrapp_two(8.0) };
    return cb_tick() + cb_hooks_tock() + cb_sum(p);   /* 4 + 8 + 28 */
}
