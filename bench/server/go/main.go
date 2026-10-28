// main.go — the Go arm of KR-243: the standard library's `net` and `net/http`, a goroutine per connection.
//
// Usage: server <echo|http|http-raw|skew> <port>        GOMAXPROCS (environment) sets how many cores the runtime uses
//   http      `net/http`, the full stack
//   http-raw  `net` and the C and kama arms' own minimal parse: find where each request ends, write a fixed reply
//   skew      each 64-byte request answered after the rounds of mixing it asks for (client.c says how)
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
)

var body = []byte("Hello, World!")
var reply = []byte("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nHello, World!")

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

// Answers each complete request (`\r\n\r\n` ends one), however the reads split it, until the client closes.
func httpRaw(c net.Conn) {
	defer c.Close()
	buf := make([]byte, 4096)
	state := 0 // how much of "\r\n\r\n" the bytes so far end with
	for {
		n, err := c.Read(buf)
		for _, b := range buf[:n] {
			switch {
			case b == '\r':
				if state == 2 {
					state = 3
				} else {
					state = 1
				}
			case b == '\n' && (state == 1 || state == 3):
				state++
			default:
				state = 0
			}
			if state == 4 {
				state = 0
				if _, werr := c.Write(reply); werr != nil {
					return
				}
			}
		}
		if err != nil {
			return
		}
	}
}

// The skew burst, the same in every arm: `rounds` passes of FNV-1a over the request's 64 bytes.
func mix(b []byte, rounds uint32) uint32 {
	h := uint32(2166136261)
	for r := uint32(0); r < rounds; r++ {
		for _, x := range b[:64] {
			h = (h ^ uint32(x)) * 16777619
		}
	}
	return h
}

func skew(c net.Conn) {
	defer c.Close()
	buf := make([]byte, 64)
	for {
		if _, err := io.ReadFull(c, buf); err != nil {
			return
		}
		h := mix(buf, uint32(buf[0])|uint32(buf[1])<<8|uint32(buf[2])<<16|uint32(buf[3])<<24)
		buf[4], buf[5], buf[6], buf[7] = byte(h), byte(h>>8), byte(h>>16), byte(h>>24)
		if _, err := c.Write(buf); err != nil {
			return
		}
	}
}

func serve(addr string, handle func(net.Conn)) {
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
		go handle(c)
	}
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: server <echo|http|http-raw|skew> <port>")
		os.Exit(64)
	}
	addr := "127.0.0.1:" + os.Args[2]
	switch os.Args[1] {
	case "echo":
		serve(addr, echo)
	case "http-raw":
		serve(addr, httpRaw)
	case "skew":
		serve(addr, skew)
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
