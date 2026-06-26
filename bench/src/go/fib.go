package main
import "os"
func fib(n uint64) uint64 { if n<2 {return n}; return fib(n-1)+fib(n-2) }
func main(){ var s uint64; for i:=uint64(0);i<32;i++ { s+=fib(i) }; os.Exit(int(s%256)) }
