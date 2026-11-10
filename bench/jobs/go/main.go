// The Go arm of KR-3's pool benchmark: a goroutine per chunk and a WaitGroup, the idiom, on Go's one scheduler. The shapes and
// the usage are jobs.kama's. Go keeps spinning threads between bursts, and what they burn shows in the run's CPU.
// BENCH_CALLERS=2 runs the loop on two goroutines at once; BENCH_BLOCKERS=b adds a goroutine whose hand-outs of b goroutines
// each hold a thread in the OS for BENCH_BLOCK_MS, until the caller is done.
package main

import (
	"fmt"
	"os"
	"slices"
	"strconv"
	"sync"
	"sync/atomic"
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
	slot := int64(setting("BENCH_SLOT_US", 1389)) * 1000
	rounds := setting("BENCH_ROUNDS", 0)
	callers := 1
	if setting("BENCH_CALLERS", 1) == 2 {
		callers = 2
	}
	blockers := setting("BENCH_BLOCKERS", 0)
	k := min(workers, elems)
	chunk := (elems + k - 1) / k
	// One caller: `calls` barriers over its own `xs`, each one's time in `t`.
	drive := func(xs []uint32, t []int64) {
		start := nowNs()
		var next int64
		for c := 0; c < calls; c++ {
			if shape == "frame" {
				next += slot
				for nowNs()-start < next {
				}
			}
			t0 := nowNs()
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
			t[c] = nowNs() - t0
		}
	}
	// The runtime starts threads as goroutines want them, so a hand-out runs once before the clock does, as pool.c's
	// `pool_start` does.
	var warm sync.WaitGroup
	for range k {
		warm.Add(1)
		go warm.Done()
	}
	warm.Wait()
	xs := make([][]uint32, callers)
	t := make([]int64, calls*callers)
	for i := range xs {
		xs[i] = make([]uint32, elems)
	}
	if callers == 2 {
		var wg sync.WaitGroup
		for i := range xs {
			wg.Add(1)
			go func() {
				defer wg.Done()
				drive(xs[i], t[i*calls:(i+1)*calls])
			}()
		}
		wg.Wait()
	} else if blockers > 0 {
		var done atomic.Bool
		ms := setting("BENCH_BLOCK_MS", 2)
		stopped := make(chan struct{})
		go func() {
			for !done.Load() {
				var wg sync.WaitGroup
				for range blockers {
					wg.Add(1)
					go func() {
						defer wg.Done()
						nap(ms)
					}()
				}
				wg.Wait()
			}
			close(stopped)
		}()
		drive(xs[0], t)
		done.Store(true)
		<-stopped
	} else {
		drive(xs[0], t)
	}
	var total, check int64
	for _, ns := range t {
		total += ns
	}
	for _, x := range xs {
		for _, e := range x {
			check += int64(e)
		}
	}
	slices.Sort(t)
	n := len(t)
	fmt.Printf("%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n", shape, workers, calls, elems, total, t[n/2], t[n-1-n/100], check)
}
