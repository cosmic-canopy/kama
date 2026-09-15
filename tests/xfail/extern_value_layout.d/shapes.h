#ifndef KAMA_TEST_SHAPES_H
#define KAMA_TEST_SHAPES_H
typedef struct Sample { short level; float gain; } Sample;
static inline int sample_level(Sample s) { return s.level; }
#endif
