// The Go arm of KR-3's pool benchmark: a goroutine per chunk and a WaitGroup, the idiom, on Go's one scheduler. The shapes and
// the usage are jobs.kama's. Go keeps spinning threads between bursts, and what they burn shows in the run's CPU.
package main

import (
	"fmt"
	"os"
	"slices"
	"strconv"
	"sync"
	"time"
)

func setting(name string, dflt int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return dflt
}

func main() {
	shape := os.Getenv("BENCH_SHAPE")
	if shape == "" {
		shape = "tight"
	}
	workers := setting("BENCH_WORKERS", 4)
	calls := setting("BENCH_CALLS", 2000)
	elems := setting("BENCH_ELEMS", 1024)
	slot := time.Duration(setting("BENCH_SLOT_US", 1389)) * time.Microsecond
	rounds := setting("BENCH_ROUNDS", 0)
	xs := make([]uint32, elems)
	t := make([]int64, calls)
	k := min(workers, elems)
	chunk := (elems + k - 1) / k
	var total int64
	start := time.Now()
	var next time.Duration
	for c := 0; c < calls; c++ {
		if shape == "frame" {
			next += slot
			for time.Since(start) < next {
			}
		}
		t0 := time.Now()
		var wg sync.WaitGroup
		for from := 0; from < elems; from += chunk {
			part := xs[from:min(from+chunk, elems)]
			wg.Add(1)
			go func() {
				defer wg.Done()
				for i := range part {
					x := part[i] + 1
					for r := 0; r < rounds; r++ {
						x ^= x << 13
						x ^= x >> 17
						x ^= x << 5
					}
					part[i] = x
				}
			}()
		}
		wg.Wait()
		t[c] = time.Since(t0).Nanoseconds()
		total += t[c]
	}
	var check int64
	for _, x := range xs {
		check += int64(x)
	}
	slices.Sort(t)
	fmt.Printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", shape, workers, calls, elems, total, t[calls/2], t[calls-1-calls/100], check)
}
