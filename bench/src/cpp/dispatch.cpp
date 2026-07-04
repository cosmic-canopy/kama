// dispatch — genuine dynamic dispatch over a heap-owned heterogeneous vector<unique_ptr<Shape>>,
// built at runtime so the concrete type is not knowable at the call site (no devirtualization).
#include <cstdint>
#include <vector>
#include <memory>
struct Shape { virtual int64_t area(){return 0;} virtual ~Shape(){} };
struct Circle : Shape { int64_t r; Circle(int64_t r):r(r){} int64_t area() override {return r*r;} };
struct Square : Shape { int64_t s; Square(int64_t s):s(s){} int64_t area() override {return s*s;} };
int main(){
  const int N=512;
  std::vector<std::unique_ptr<Shape>> shapes;
  shapes.reserve(N);
  for(int j=0;j<N;j++){ if(j%2==0) shapes.push_back(std::make_unique<Circle>(3)); else shapes.push_back(std::make_unique<Square>(4)); }
  uint64_t sum=0;
  for(uint64_t i=0;i<8000000;i++){ sum+=(uint64_t)shapes[i%N]->area(); }
  return (int)(sum%256);
}
