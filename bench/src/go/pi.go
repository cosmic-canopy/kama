package main
import "os"
func main(){ pi:=0.0; sign:=1.0
  for i:=uint64(0);i<20000000;i++ { d:=2*i+1; pi += sign/float64(d); sign=-sign }
  scaled:=pi*4000000000.0; check:=uint64(scaled); os.Exit(int(check%256)) }
