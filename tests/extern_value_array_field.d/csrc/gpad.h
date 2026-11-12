#include <stdint.h>
typedef struct gpad { int32_t n; float v[4]; } gpad;
static inline float gpad_sum(const gpad* p) { return p->v[0] + p->v[1] + p->v[2] + p->v[3]; }
