/* A header BESIDE the .c, so the fixture also proves `csources` puts that directory on the
   include path — for the C itself and for the kama file that externs it. */
#ifndef KAMA_TEST_ADDER_H
#define KAMA_TEST_ADDER_H
int kama_test_add(int a, int b);
#endif
