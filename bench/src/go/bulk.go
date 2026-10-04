package main

import "os"

func chunk(k int) int {
	switch k % 5 {
	case 0:
		return 7
	case 1:
		return 64
	case 2:
		return 1000
	case 3:
		return 4096
	}
	return 65536
}

func main() {
	const SRC, TARGET = 65536, 4194304
	src := make([]byte, SRC)
	for i := 0; i < SRC; i++ {
		src[i] = byte(i*7 + (i >> 8))
	}
	dst := make([]byte, TARGET)
	var total uint64
	for r := 0; r < 64; r++ {
		var buf []byte
		k, off := r, r*13
		for len(buf) < TARGET {
			c := chunk(k)
			k++
			if c > TARGET-len(buf) {
				c = TARGET - len(buf)
			}
			off = (off + 4099) % (SRC - c + 1)
			buf = append(buf, src[off:off+c]...)
		}
		copy(dst, buf)
		var s uint64
		for i := r; i < TARGET; i += 4093 {
			s += uint64(dst[i])
		}
		total += s
	}
	os.Exit(int(total % 256))
}
