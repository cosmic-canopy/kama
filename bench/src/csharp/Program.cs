class P {
  static ulong Fib(ulong n){ if(n<2) return n; return Fib(n-1)+Fib(n-2); }
  static ulong Clen(ulong n){ ulong s=0; while(n!=1){ if(n%2==0) n/=2; else n=3*n+1; s++; } return s; }
  abstract class Shape { public abstract long Area(); }
  class Circle : Shape { long r; public Circle(long r){this.r=r;} public override long Area()=>r*r; }
  class Square : Shape { long s; public Square(long s){this.s=s;} public override long Area()=>s*s; }
  static long Measure(Shape sh)=>sh.Area();
  static long Add1(long x)=>x+1;
  static long Mul3(long x)=>x*3;
  static long Apply(System.Func<long,long> op, long x)=>op(x);
  static int Main(string[] args){
    string w = args.Length>0?args[0]:"fib";
    ulong sum=0;
    if(w=="fib"){ for(ulong i=0;i<32;i++) sum+=Fib(i); }
    else if(w=="pi"){ double pi=0,sign=1; for(ulong i=0;i<20000000;i++){ ulong d=2*i+1; pi+=sign/(double)d; sign=-sign; } double scaled=pi*4000000000.0; sum=(ulong)scaled; }
    else if(w=="collatz"){ for(ulong i=1;i<700000;i++) sum+=Clen(i); }
    else if(w=="dispatch"){ const int N=512; Shape[] shapes=new Shape[N]; for(int j=0;j<N;j++) shapes[j]=(j%2==0)?(Shape)new Circle(3):new Square(4); for(ulong i=0;i<8000000;i++) sum+=(ulong)shapes[(int)(i%(ulong)N)].Area(); }
    else if(w=="alloc"){ for(int iter=0;iter<2000;iter++){ var xs=new System.Collections.Generic.List<int>(); for(int j=1;j<=1000;j++) xs.Add(j); ulong s=0; foreach(var v in xs) s+=(ulong)v; sum+=s; } }
    else if(w=="fnptr"){ System.Func<long,long> a=Add1, b=Mul3; for(ulong i=0;i<8000000;i++){ if(i%2==0) sum+=(ulong)Apply(a,(long)i); else sum+=(ulong)Apply(b,(long)i); } }
    else if(w=="map"){ const long N=100000, PASSES=10; var m=new System.Collections.Generic.Dictionary<int,long>(); for(long i=0;i<N;i++) m[(int)i]=i*2; for(long p=0;p<PASSES;p++) for(long i=0;i<N;i++){ int k=(int)((i*2654435761L)%N); sum+=(ulong)m[k]; } }
    return (int)(sum%256);
  }
}
