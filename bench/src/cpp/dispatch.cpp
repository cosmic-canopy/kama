#include <cstdint>
struct Shape { virtual int64_t area(){return 0;} virtual ~Shape(){} };
struct Circle : Shape { int64_t r; Circle(int64_t r):r(r){} int64_t area() override {return r*r;} };
struct Square : Shape { int64_t s; Square(int64_t s):s(s){} int64_t area() override {return s*s;} };
static int64_t measure(Shape& sh){ return sh.area(); }
int main(){
  Circle c(3); Square q(4); uint64_t sum=0;
  for(uint64_t i=0;i<8000000;i++){ if((i%2)==0) sum+=(uint64_t)measure(c); else sum+=(uint64_t)measure(q); }
  return (int)(sum%256);
}
