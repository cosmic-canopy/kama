# A bytearray grows in place; `+=` of a memoryview slice and a slice assignment are each one memcpy.
import sys
def chunk(k): return (7, 64, 1000, 4096, 65536)[k % 5]
SRC, TARGET = 65536, 4194304
src = bytearray((i * 7 + (i >> 8)) & 0xff for i in range(SRC))
view = memoryview(src)
dst = bytearray(TARGET)
total = 0
for r in range(64):
    buf = bytearray()
    k, off = r, r * 13
    while len(buf) < TARGET:
        c = chunk(k); k += 1
        if c > TARGET - len(buf): c = TARGET - len(buf)
        off = (off + 4099) % (SRC - c + 1)
        buf += view[off:off + c]
    dst[:] = buf
    total += sum(dst[r::4093])
sys.exit(total % 256)
