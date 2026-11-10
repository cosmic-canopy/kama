//go:build !windows

package main

import (
	"syscall"
	"time"
)

var base = time.Now()

// nowNs is nanoseconds since the program started, on the monotonic clock.
func nowNs() int64 { return time.Since(base).Nanoseconds() }

// nap holds this thread in the OS for ms milliseconds, as a read that blocks does: a system call, which Go's scheduler sees
// and works around. time.Sleep would park only the goroutine and hold no thread.
func nap(ms int) {
	ts := syscall.NsecToTimespec(int64(ms) * 1e6)
	syscall.Nanosleep(&ts, nil)
}
