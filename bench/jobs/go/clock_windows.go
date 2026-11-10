// Go's monotonic clock on Windows advances with the timer interrupt, so a barrier of a few microseconds read as 0 or as a whole
// tick (p50 0 µs and p99 1,093 µs on a 1 ms phase). Rust's Instant and the C arms' clock_gettime read QueryPerformanceCounter
// there, and so does this.
package main

import (
	"syscall"
	"unsafe"
)

var (
	kernel32   = syscall.NewLazyDLL("kernel32.dll")
	qpc        = kernel32.NewProc("QueryPerformanceCounter")
	qpf        = kernel32.NewProc("QueryPerformanceFrequency")
	sleep      = kernel32.NewProc("Sleep")
	freq, base int64
)

func init() {
	qpf.Call(uintptr(unsafe.Pointer(&freq)))
	qpc.Call(uintptr(unsafe.Pointer(&base)))
}

// nowNs is nanoseconds since the program started. Counted from `base`, so `* 1e9` cannot overflow inside a run.
func nowNs() int64 {
	var c int64
	qpc.Call(uintptr(unsafe.Pointer(&c)))
	return (c - base) * 1e9 / freq
}

// nap holds this thread in the OS for ms milliseconds, as a read that blocks does: a system call, which Go's scheduler sees
// and works around. time.Sleep would park only the goroutine and hold no thread.
func nap(ms int) { sleep.Call(uintptr(ms)) }
