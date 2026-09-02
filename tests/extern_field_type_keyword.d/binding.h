/* A C struct with a field named `type` — the shape webgpu.h uses in four structs. `bufKind` reads
   `b->type` on the C side, which is what proves kama emitted the LITERAL field name: a consistently
   wrong-but-self-consistent name would satisfy a kama-only round-trip and fail here. */
#include <stdint.h>
typedef struct { int32_t type; int32_t hasDynamicOffset; } BufBinding;
static int32_t bufKind(const BufBinding* b) { return b->type; }
