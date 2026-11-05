package main
import ("fmt";"os";"runtime";"strings")
func threads() string { b, _ := os.ReadFile("/proc/self/status"); for _, l := range strings.Split(string(b), "\n") { if strings.HasPrefix(l, "Threads:") { return strings.TrimSpace(l[8:]) } }; return "?" }
func main() { fmt.Println("go: threads at main =", threads(), " GOMAXPROCS =", runtime.GOMAXPROCS(0), " NumCPU =", runtime.NumCPU()) }
