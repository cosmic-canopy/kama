#ifndef KAMA_TEST_LINK_ADDER_H
#define KAMA_TEST_LINK_ADDER_H
/* Two C symbols a kama file cannot spell as-is: one collides with a kama KEYWORD (`match` is not a C
   keyword, so C is happy to define it), the other is simply not the name the kama side wants. */
int match(int a, int b);
int kama_test_add(int a, int b);
#endif
