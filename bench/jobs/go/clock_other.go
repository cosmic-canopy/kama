//go:build !windows

package main

import "time"

var base = time.Now()

// nowNs is nanoseconds since the program started, on the monotonic clock.
func nowNs() int64 { return time.Since(base).Nanoseconds() }
