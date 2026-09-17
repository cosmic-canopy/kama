/* Both platform files define `kama_test_gated_side`, so compiling both is a duplicate symbol and compiling
   neither an undefined one: the link itself proves exactly one entry survived its gate. */
#ifndef KAMA_TEST_GATED_H
#define KAMA_TEST_GATED_H
int kama_test_gated_side(void);
int kama_test_gated_include(void);
#endif
