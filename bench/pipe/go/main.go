// The Go arm of KR-264's pipe benchmark, run at GOMAXPROCS=1 as kama's fibers share one isolate. The workloads and the
// usage are pipe.kama's, except that `net.Pipe` has no capacity: it hands bytes straight from a writer to a reader, and
// a write returns only once they are all taken, so `ops`, which needs a buffer, does not exist here.
package main

import (
	"fmt"
	"io"
	"net"
	"os"
	"runtime"
	"strconv"
	"time"
)

func setting(name string, dflt int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return dflt
}

func pingpong(count, bytes int) int64 {
	a, b := net.Pipe()
	done := make(chan struct{})
	go func() {
		buf := make([]byte, bytes)
		for i := 0; i < count; i++ {
			if _, err := io.ReadFull(b, buf); err != nil {
				panic(err)
			}
			if _, err := b.Write(buf); err != nil {
				panic(err)
			}
		}
		close(done)
	}()
	buf := make([]byte, bytes)
	t0 := time.Now()
	for i := 0; i < count; i++ {
		if _, err := a.Write(buf); err != nil {
			panic(err)
		}
		if _, err := io.ReadFull(a, buf); err != nil {
			panic(err)
		}
	}
	ns := time.Since(t0).Nanoseconds()
	<-done
	return ns
}

func bulk(count, bytes int) int64 {
	a, b := net.Pipe()
	done := make(chan struct{})
	buf := make([]byte, bytes)
	t0 := time.Now()
	go func() {
		into := make([]byte, bytes)
		for {
			if _, err := b.Read(into); err != nil {
				if err == io.EOF {
					close(done)
					return
				}
				panic(err)
			}
		}
	}()
	for i := 0; i < count; i++ {
		if _, err := a.Write(buf); err != nil {
			panic(err)
		}
	}
	a.Close()
	<-done
	return time.Since(t0).Nanoseconds()
}

func main() {
	runtime.GOMAXPROCS(1)
	workload := os.Getenv("BENCH_WORKLOAD")
	count := setting("BENCH_COUNT", 1000000)
	bytes := setting("BENCH_BYTES", 64)
	capacity := setting("BENCH_CAPACITY", 8192)
	var ns int64
	switch workload {
	case "pingpong":
		ns = pingpong(count, bytes)
	case "bulk":
		ns = bulk(count, bytes)
	default:
		fmt.Fprintln(os.Stderr, "pipe: BENCH_WORKLOAD is pingpong or bulk")
		os.Exit(2)
	}
	fmt.Printf("%s\t%d\t%d\t%d\t%d\n", workload, count, bytes, capacity, ns)
}
