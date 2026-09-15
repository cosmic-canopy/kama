#ifndef KAMA_TEST_EXTERNENUM_FMT_H
#define KAMA_TEST_EXTERNENUM_FMT_H
#include <stdint.h>
/* The ONE place these values are stated. kama names the constants and never writes a number. */
typedef enum Format { FORMAT_UNDEFINED = 0, FORMAT_RGBA8 = 18, FORMAT_BGRA8 = 27 } Format;
typedef struct Surface { int32_t width; Format format; } Surface;
int32_t fmt_bits(Format f);
int32_t surface_score(Surface s);
int32_t host_calls(void);
#endif
