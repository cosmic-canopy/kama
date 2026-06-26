package main
import "os"
func clen(n uint64) uint64 { var s uint64; for n!=1 { if n%2==0 {n=n/2} else {n=3*n+1}; s++ }; return s }
func main(){ var s uint64; for i:=uint64(1);i<700000;i++ { s+=clen(i) }; os.Exit(int(s%256)) }
