// main.go — the Go arm of KR-243: the standard library's `net` and `net/http`, a goroutine per connection.
//
// Usage: server <echo|http> <port>        GOMAXPROCS (environment) sets how many cores the runtime uses
package main

import (
	"fmt"
	"net"
	"net/http"
	"os"
)

var body = []byte("Hello, World!")

func echo(c net.Conn) {
	defer c.Close()
	buf := make([]byte, 4096)
	for {
		n, err := c.Read(buf)
		if n > 0 {
			if _, werr := c.Write(buf[:n]); werr != nil {
				return
			}
		}
		if err != nil {
			return
		}
	}
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: server <echo|http> <port>")
		os.Exit(64)
	}
	addr := "127.0.0.1:" + os.Args[2]
	switch os.Args[1] {
	case "echo":
		ln, err := net.Listen("tcp", addr)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		for {
			c, err := ln.Accept()
			if err != nil {
				continue
			}
			go echo(c)
		}
	case "http":
		h := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "text/plain")
			w.Header().Set("Content-Length", "13")
			w.Write(body)
		})
		if err := http.ListenAndServe(addr, h); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	default:
		fmt.Fprintf(os.Stderr, "server: unknown mode %q\n", os.Args[1])
		os.Exit(64)
	}
}
