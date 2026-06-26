class P {
  static ulong Fib(ulong n){ if(n<2) return n; return Fib(n-1)+Fib(n-2); }
  static ulong Clen(ulong n){ ulong s=0; while(n!=1){ if(n%2==0) n/=2; else n=3*n+1; s++; } return s; }
  abstract class Shape { public abstract long Area(); }
  class Circle : Shape { long r; public Circle(long r){this.r=r;} public override long Area()=>r*r; }
  class Square : Shape { long s; public Square(long s){this.s=s;} public override long Area()=>s*s; }
  static long Measure(Shape sh)=>sh.Area();
  static int Main(string[] args){
    string w = args.Length>0?args[0]:"fib";
    ulong sum=0;
    if(w=="fib"){ for(ulong i=0;i<32;i++) sum+=Fib(i); }
    else if(w=="pi"){ double pi=0,sign=1; for(ulong i=0;i<20000000;i++){ ulong d=2*i+1; pi+=sign/(double)d; sign=-sign; } double scaled=pi*4000000000.0; sum=(ulong)scaled; }
    else if(w=="collatz"){ for(ulong i=1;i<700000;i++) sum+=Clen(i); }
    else if(w=="dispatch"){ Shape c=new Circle(3), q=new Square(4); for(ulong i=0;i<8000000;i++){ if(i%2==0) sum+=(ulong)Measure(c); else sum+=(ulong)Measure(q); } }
    return (int)(sum%256);
  }
}
