/* The two things a kama program cannot see without the seam under test: the process umask, and a path's
   mode bits. Set the first, read the second. */
#ifndef KAMA_TEST_UMASK_PROBE_H
#define KAMA_TEST_UMASK_PROBE_H
#include <stdint.h>
int32_t kama_test_set_umask(int32_t mask);
int32_t kama_test_mode_of(const char* path);
#endif
