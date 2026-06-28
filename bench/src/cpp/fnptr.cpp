#include <cstdint>
typedef int64_t (*Op)(int64_t);
static int64_t add1(int64_t x){ return x+1; }
static int64_t mul3(int64_t x){ return x*3; }
static int64_t apply(Op op, int64_t x){ return op(x); }
int main(){
  Op a=add1, b=mul3; uint64_t sum=0;
  for(uint64_t i=0;i<8000000;i++){ if((i%2)==0) sum+=(uint64_t)apply(a,(int64_t)i); else sum+=(uint64_t)apply(b,(int64_t)i); }
  return (int)(sum%256);
}
