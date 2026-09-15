#ifndef KAMA_TEST_BOUNDENUM_LEVEL_H
#define KAMA_TEST_BOUNDENUM_LEVEL_H
#include <stdint.h>
/* The ONE place these values are stated; kama names the constants. */
typedef enum Mode { MODE_OFF = 0, MODE_ON = 9 } Mode;
int32_t mode_raw(Mode m);
int32_t host_run(void);
#endif
