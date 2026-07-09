// bench/src/java/Bench.java — Java (OpenJDK HotSpot JIT) port of the C# Program.cs.
// One class, all six workloads, selected by args[0]; exits with the checksum (sum % 256).
// This is the same standard HotSpot JVM that runs a long-running app like Minecraft (Java
// Edition) — not a GraalVM native-image. Signed 64-bit `long` matches C#'s `ulong` here
// because every workload's running sum stays well under 2^63, so `% 256` agrees, and `pi`
// is pure IEEE-754 double.
import java.util.ArrayList;
import java.util.HashMap;
import java.util.function.LongUnaryOperator;

public class Bench {
  static long fib(long n){ if(n<2) return n; return fib(n-1)+fib(n-2); }
  static long clen(long n){ long s=0; while(n!=1){ if(n%2==0) n/=2; else n=3*n+1; s++; } return s; }

  abstract static class Shape { abstract long area(); }
  static class Circle extends Shape { long r; Circle(long r){this.r=r;} long area(){ return r*r; } }
  static class Square extends Shape { long s; Square(long s){this.s=s;} long area(){ return s*s; } }
  static long measure(Shape sh){ return sh.area(); }

  static long add1(long x){ return x+1; }
  static long mul3(long x){ return x*3; }
  static long apply(LongUnaryOperator op, long x){ return op.applyAsLong(x); }

  public static void main(String[] args){
    String w = args.length>0 ? args[0] : "fib";
    long sum=0;
    if(w.equals("fib")){ for(long i=0;i<32;i++) sum+=fib(i); }
    else if(w.equals("pi")){ double pi=0,sign=1; for(long i=0;i<20000000L;i++){ long d=2*i+1; pi+=sign/(double)d; sign=-sign; } sum=(long)(pi*4000000000.0); }
    else if(w.equals("collatz")){ for(long i=1;i<700000;i++) sum+=clen(i); }
    else if(w.equals("dispatch")){ final int N=512; Shape[] shapes=new Shape[N]; for(int j=0;j<N;j++) shapes[j]=(j%2==0)?new Circle(3):new Square(4); for(long i=0;i<8000000;i++) sum+=shapes[(int)(i%N)].area(); }
    else if(w.equals("alloc")){ for(int iter=0;iter<2000;iter++){ ArrayList<Integer> xs=new ArrayList<>(); for(int j=1;j<=1000;j++) xs.add(j); long s=0; for(int v: xs) s+=v; sum+=s; } }
    else if(w.equals("fnptr")){ LongUnaryOperator a=Bench::add1, b=Bench::mul3; for(long i=0;i<8000000;i++){ if(i%2==0) sum+=apply(a,i); else sum+=apply(b,i); } }
    else if(w.equals("map")){ final long N=100000, PASSES=10; HashMap<Integer,Long> m=new HashMap<>(); for(long i=0;i<N;i++) m.put((int)i, i*2); for(long p=0;p<PASSES;p++) for(long i=0;i<N;i++){ int k=(int)((i*2654435761L)%N); sum+=m.get(k); } }
    System.exit((int)(sum%256));
  }
}
