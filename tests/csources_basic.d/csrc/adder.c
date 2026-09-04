/* The project's OWN C, compiled by `kama build` from the manifest's `csources` — no out-of-band
   Makefile. Deliberately trivial: what is under test is that it is compiled and linked at all. */
#include "adder.h"

int kama_test_add(int a, int b) { return a + b; }
