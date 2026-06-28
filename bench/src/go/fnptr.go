package main
import "os"
type Op func(int64) int64
func add1(x int64) int64 { return x+1 }
func mul3(x int64) int64 { return x*3 }
func apply(op Op, x int64) int64 { return op(x) }
func main(){ var a Op = add1; var b Op = mul3; var sum uint64
  for i:=uint64(0);i<8000000;i++ { if i%2==0 { sum+=uint64(apply(a,int64(i))) } else { sum+=uint64(apply(b,int64(i))) } }
  os.Exit(int(sum%256)) }
