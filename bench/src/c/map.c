#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// Minimal open-addressing (linear-probe) int32 -> int64 map: the idiomatic C answer (no stdlib hashmap).
typedef struct { int32_t *keys; int64_t *vals; uint8_t *used; int32_t cap; } Map;
static void mput(Map *m, int32_t k, int64_t v) {
    uint64_t x = (uint64_t)(uint32_t)k * 2654435761u;                 // hash
    int32_t i = (int32_t)(x % (uint64_t)m->cap);
    while (m->used[i]) { if (m->keys[i] == k) { m->vals[i] = v; return; } i++; if (i >= m->cap) i = 0; }
    m->keys[i] = k; m->vals[i] = v; m->used[i] = 1;
}
static int64_t mget(Map *m, int32_t k) {
    uint64_t x = (uint64_t)(uint32_t)k * 2654435761u;
    int32_t i = (int32_t)(x % (uint64_t)m->cap);
    while (m->used[i]) { if (m->keys[i] == k) return m->vals[i]; i++; if (i >= m->cap) i = 0; }
    return 0;
}
int main(void) {
    const long long N = 100000, PASSES = 10;
    Map m; m.cap = 262144;                                            // > N / 0.75, power of 2
    m.keys = malloc(sizeof(int32_t) * m.cap);
    m.vals = malloc(sizeof(int64_t) * m.cap);
    m.used = calloc(m.cap, 1);
    for (long long i = 0; i < N; i++) mput(&m, (int32_t)i, i * 2);
    uint64_t sum = 0;
    for (long long p = 0; p < PASSES; p++)
        for (long long i = 0; i < N; i++) {
            int32_t k = (int32_t)((i * 2654435761LL) % N);
            sum += (uint64_t)mget(&m, k);
        }
    return (int)(sum % 256);
}
