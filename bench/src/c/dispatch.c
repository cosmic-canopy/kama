#include <stdint.h>
typedef struct Shape Shape;
typedef struct { int64_t (*area)(Shape*); } VT;
struct Shape { const VT* vt; int64_t v; };
static int64_t carea(Shape* s){ return s->v*s->v; }
static const VT cvt = { carea };
static int64_t measure(Shape* s){ return s->vt->area(s); }
int main(void){
  Shape c = { &cvt, 3 }, q = { &cvt, 4 };
  uint64_t sum=0;
  for(uint64_t i=0;i<8000000;i++){ if((i%2)==0) sum+=(uint64_t)measure(&c); else sum+=(uint64_t)measure(&q); }
  return (int)(sum%256);
}
