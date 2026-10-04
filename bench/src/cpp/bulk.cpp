#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <vector>
static size_t chunk(size_t k) { switch (k % 5) { case 0: return 7; case 1: return 64; case 2: return 1000; case 3: return 4096; default: return 65536; } }
int main() {
    const size_t SRC = 65536, TARGET = 4194304;
    std::vector<uint8_t> src(SRC);
    for (size_t i = 0; i < SRC; i++) src[i] = (uint8_t)(i * 7 + (i >> 8));
    std::vector<uint8_t> dst(TARGET);
    uint64_t total = 0;
    for (size_t r = 0; r < 64; r++) {
        std::vector<uint8_t> buf;
        size_t k = r, off = r * 13;
        while (buf.size() < TARGET) {
            size_t c = chunk(k++);
            if (c > TARGET - buf.size()) c = TARGET - buf.size();
            off = (off + 4099) % (SRC - c + 1);
            buf.insert(buf.end(), src.begin() + off, src.begin() + off + c);
        }
        std::copy(buf.begin(), buf.end(), dst.begin());
        uint64_t s = 0; for (size_t i = r; i < TARGET; i += 4093) s += dst[i];
        total += s;
    }
    return (int)(total % 256);
}
