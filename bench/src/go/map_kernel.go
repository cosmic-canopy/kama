// Equal-workload kernel — see bench/src/c/map_kernel.c. Same open-addressing map, same hash, same
// fixed prealloc, no stdlib map (Go's builtin `map` is measured in the `map` row instead).
package main

import "os"

type IntMap struct {
	keys []int32
	vals []int64
	used []uint8
	cap  int32
}

func newIntMap(cap int32) *IntMap {
	return &IntMap{keys: make([]int32, cap), vals: make([]int64, cap), used: make([]uint8, cap), cap: cap}
}

func (m *IntMap) slotFor(k int32) int32 {
	x := uint64(uint32(k)) * 2654435761 // hash
	return int32(x % uint64(m.cap))
}

func (m *IntMap) put(k int32, v int64) {
	i := m.slotFor(k)
	for m.used[i] != 0 {
		if m.keys[i] == k {
			m.vals[i] = v
			return
		}
		i++
		if i >= m.cap {
			i = 0
		}
	}
	m.keys[i] = k
	m.vals[i] = v
	m.used[i] = 1
}

func (m *IntMap) get(k int32) int64 {
	i := m.slotFor(k)
	for m.used[i] != 0 {
		if m.keys[i] == k {
			return m.vals[i]
		}
		i++
		if i >= m.cap {
			i = 0
		}
	}
	return 0
}

func main() {
	const N int64 = 100000
	const PASSES int64 = 10
	m := newIntMap(262144) // > N / 0.75, power of 2
	for i := int64(0); i < N; i++ {
		m.put(int32(i), i*2)
	}
	var sum uint64 = 0
	for p := int64(0); p < PASSES; p++ {
		for i := int64(0); i < N; i++ {
			k := int32((i * 2654435761) % N)
			sum += uint64(m.get(k))
		}
	}
	os.Exit(int(sum % 256))
}
