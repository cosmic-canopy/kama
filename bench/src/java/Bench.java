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

  // math workload: Vec4/Mat4/Quat as float[4] / float[4][4], field-by-field ops (mirrors std::math).
  static float[] v4add(float[] a, float[] b){ return new float[]{a[0]+b[0],a[1]+b[1],a[2]+b[2],a[3]+b[3]}; }
  static float[] v4sub(float[] a, float[] b){ return new float[]{a[0]-b[0],a[1]-b[1],a[2]-b[2],a[3]-b[3]}; }
  static float[] v4scale(float[] a, float s){ return new float[]{a[0]*s,a[1]*s,a[2]*s,a[3]*s}; }
  static float v4dot(float[] a, float[] b){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3]; }
  static float[] m4transform(float[][] m, float[] v){ return new float[]{
    m[0][0]*v[0]+m[1][0]*v[1]+m[2][0]*v[2]+m[3][0]*v[3],
    m[0][1]*v[0]+m[1][1]*v[1]+m[2][1]*v[2]+m[3][1]*v[3],
    m[0][2]*v[0]+m[1][2]*v[1]+m[2][2]*v[2]+m[3][2]*v[3],
    m[0][3]*v[0]+m[1][3]*v[1]+m[2][3]*v[2]+m[3][3]*v[3] }; }
  static float[][] m4mul(float[][] a, float[][] b){ return new float[][]{
    m4transform(a,b[0]), m4transform(a,b[1]), m4transform(a,b[2]), m4transform(a,b[3]) }; }
  static float[] quatMul(float[] a, float[] b){ return new float[]{   // Hamilton product (x,y,z,w)
    a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
    a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
    a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
    a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2] }; }

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
    else if(w.equals("math")){ float[][] mat={{1,1,0,0},{0,1,1,0},{0,0,1,1},{1,0,0,1}}; for(long i=0;i<2000000L;i++){ float s=(float)(i%8); float[] a={s,s+1,s+2,s+3}, b={s+2,s+3,s+4,s+5}; float[] c=v4add(a,b); float[] e=v4scale(c,3.0f); float[] f=v4sub(e,b); float dp=v4dot(a,b); float[] mv=m4transform(mat,a); float[][] mm=m4mul(mat,mat); float[] q1={s,s+1,s+2,s+3}, q2={s+1,s,s+3,s+2}; float[] qq=quatMul(q1,q2); float qdot=qq[0]*qq[0]+qq[1]*qq[1]+qq[2]*qq[2]+qq[3]*qq[3]; float acc=(f[0]+f[1]+f[2]+f[3])+dp+(mv[0]+mv[1]+mv[2]+mv[3])+(mm[0][0]+mm[1][1]+mm[2][2]+mm[3][3])+qdot; sum+=(long)acc; } }
    System.exit((int)(sum%256));
  }
}
