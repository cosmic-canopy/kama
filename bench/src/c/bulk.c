#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// C has no growable array: the one every C program writes — realloc doubling, then memcpy.
typedef struct { uint8_t* p; size_t len, cap; } Buf;
static void append(Buf* b, const uint8_t* s, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4;
        while (nc < b->len + n) nc *= 2;
        b->p = (uint8_t*)realloc(b->p, nc); b->cap = nc;
    }
    memcpy(b->p + b->len, s, n); b->len += n;
}
static size_t chunk(size_t k) { switch (k % 5) { case 0: return 7; case 1: return 64; case 2: return 1000; case 3: return 4096; default: return 65536; } }
int main(void) {
    enum { SRC = 65536, TARGET = 4194304 };
    uint8_t* src = (uint8_t*)malloc(SRC);
    for (size_t i = 0; i < SRC; i++) src[i] = (uint8_t)(i * 7 + (i >> 8));
    uint8_t* dst = (uint8_t*)calloc(TARGET, 1);
    uint64_t total = 0;
    for (size_t r = 0; r < 64; r++) {
        Buf buf = {0};
        size_t k = r, off = r * 13;
        while (buf.len < TARGET) {
            size_t c = chunk(k++);
            if (c > TARGET - buf.len) c = TARGET - buf.len;
            off = (off + 4099) % (SRC - c + 1);
            append(&buf, src + off, c);
        }
        memcpy(dst, buf.p, TARGET);
        uint64_t s = 0; for (size_t i = r; i < TARGET; i += 4093) s += dst[i];
        total += s;
        free(buf.p);
    }
    free(src); free(dst);
    return (int)(total % 256);
}
