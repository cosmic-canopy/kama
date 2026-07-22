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
  struct V4 { public float x,y,z,w; public V4(float x,float y,float z,float w){this.x=x;this.y=y;this.z=z;this.w=w;} }
  struct M4 { public V4 c0,c1,c2,c3; public M4(V4 a,V4 b,V4 c,V4 d){c0=a;c1=b;c2=c;c3=d;} }
  static V4 V4add(V4 a,V4 b)=>new V4(a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w);
  static V4 V4sub(V4 a,V4 b)=>new V4(a.x-b.x,a.y-b.y,a.z-b.z,a.w-b.w);
  static V4 V4scale(V4 a,float s)=>new V4(a.x*s,a.y*s,a.z*s,a.w*s);
  static float V4dot(V4 a,V4 b)=>a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
  static V4 M4transform(M4 m,V4 v)=>new V4(
    m.c0.x*v.x+m.c1.x*v.y+m.c2.x*v.z+m.c3.x*v.w,
    m.c0.y*v.x+m.c1.y*v.y+m.c2.y*v.z+m.c3.y*v.w,
    m.c0.z*v.x+m.c1.z*v.y+m.c2.z*v.z+m.c3.z*v.w,
    m.c0.w*v.x+m.c1.w*v.y+m.c2.w*v.z+m.c3.w*v.w);
  static M4 M4mul(M4 a,M4 b)=>new M4(M4transform(a,b.c0),M4transform(a,b.c1),M4transform(a,b.c2),M4transform(a,b.c3));
  static V4 QuatMul(V4 a,V4 b)=>new V4(   // Hamilton product (x,y,z,w)
    a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
    a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
    a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
    a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z);
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
    else if(w=="math"){ M4 mat=new M4(new V4(1,1,0,0),new V4(0,1,1,0),new V4(0,0,1,1),new V4(1,0,0,1)); for(ulong i=0;i<2000000;i++){ float s=(float)(i%8); V4 a=new V4(s,s+1,s+2,s+3), b=new V4(s+2,s+3,s+4,s+5); V4 c=V4add(a,b); V4 e=V4scale(c,3.0f); V4 f=V4sub(e,b); float dp=V4dot(a,b); V4 mv=M4transform(mat,a); M4 mm=M4mul(mat,mat); V4 q1=new V4(s,s+1,s+2,s+3), q2=new V4(s+1,s,s+3,s+2); V4 qq=QuatMul(q1,q2); float qdot=qq.x*qq.x+qq.y*qq.y+qq.z*qq.z+qq.w*qq.w; float acc=(f.x+f.y+f.z+f.w)+dp+(mv.x+mv.y+mv.z+mv.w)+(mm.c0.x+mm.c1.y+mm.c2.z+mm.c3.w)+qdot; sum+=(ulong)acc; } }
    return (int)(sum%256);
  }
}
