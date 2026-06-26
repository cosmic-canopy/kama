package main
import "os"
type Shape interface{ area() int64 }
type Circle struct{ r int64 }
type Square struct{ s int64 }
func (c Circle) area() int64 { return c.r*c.r }
func (q Square) area() int64 { return q.s*q.s }
func measure(sh Shape) int64 { return sh.area() }
func main(){ var c Shape = Circle{3}; var q Shape = Square{4}; var sum uint64
  for i:=uint64(0);i<8000000;i++ { if i%2==0 { sum+=uint64(measure(c)) } else { sum+=uint64(measure(q)) } }
  os.Exit(int(sum%256)) }
