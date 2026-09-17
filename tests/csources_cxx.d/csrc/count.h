/* Shared by a C file, a C++ file and kama's own C: the C++ side is reached through `extern "C"`. */
#ifndef KAMA_TEST_COUNT_H
#define KAMA_TEST_COUNT_H
#ifdef __cplusplus
extern "C" {
#endif
int kama_test_count_c(void);
int kama_test_count_cxx(void);
#ifdef __cplusplus
}
#endif
#endif
