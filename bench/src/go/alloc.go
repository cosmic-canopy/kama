package main
import "os"
func main(){
    var total uint64
    for iter:=0; iter<2000; iter++ {
        xs := []int32{}
        for j:=int32(1); j<=1000; j++ { xs = append(xs, j) }
        var s uint64
        for _, v := range xs { s += uint64(v) }
        total += s
    }
    os.Exit(int(total % 256))
}
