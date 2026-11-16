#ifndef GREET_H
#define GREET_H
#include <stdint.h>
// Two functions kama calls as C: Swift's on an Apple target (`@c`, SE-0495), and greet.c's everywhere else.
int32_t greet_add(int32_t a, int32_t b);
int32_t greet_count(const char* s);
#endif
