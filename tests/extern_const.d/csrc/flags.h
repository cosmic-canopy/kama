#ifndef KAMA_TEST_EXTERNCONST_FLAGS_H
#define KAMA_TEST_EXTERNCONST_FLAGS_H
#include <stdint.h>
/* The ONE place these values are stated — the two shapes a C API spells a named constant in. */
typedef uint64_t Usage;
static const Usage USAGE_COPY_DST = 0x08;   /* a typed constant, OR'd as bit flags (webgpu.h's shape) */
static const Usage USAGE_UNIFORM  = 0x40;
#define KEY_ESCAPE 256                      /* a macro (GLFW's shape): its type is its literal's, `int` */
#define GAIN 1.5                            /* ...`double` */
int32_t take_usage(uint64_t u);
#endif
