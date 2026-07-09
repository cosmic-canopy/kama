package main
import "os"
func main() {
    const N int64 = 100000
    const PASSES int64 = 10
    m := make(map[int32]int64, N*2)
    for i := int64(0); i < N; i++ { m[int32(i)] = i * 2 }
    var sum uint64 = 0
    for p := int64(0); p < PASSES; p++ {
        for i := int64(0); i < N; i++ {
            k := int32((i * 2654435761) % N)
            sum += uint64(m[k])
        }
    }
    os.Exit(int(sum % 256))
}
