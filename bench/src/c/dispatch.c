// dispatch — genuine dynamic dispatch: a heap array of base pointers to a heterogeneous mix
// of concrete types, so the concrete target is not statically knowable at the call site
// (no devirtualization). Every call is a real indirect vtable call.
#include <stdint.h>
#include <stdlib.h>
typedef struct Shape Shape;
typedef struct { int64_t (*area)(Shape*); } VT;
struct Shape { const VT* vt; int64_t v; };
static int64_t circle_area(Shape* s){ return s->v*s->v; }
static int64_t square_area(Shape* s){ return s->v*s->v; }
static const VT circle_vt = { circle_area };
static const VT square_vt = { square_area };
#define N 512
int main(void){
  Shape** shapes = (Shape**)malloc(N*sizeof(Shape*));
  for(int j=0;j<N;j++){
    Shape* sh = (Shape*)malloc(sizeof(Shape));
    if(j%2==0){ sh->vt=&circle_vt; sh->v=3; } else { sh->vt=&square_vt; sh->v=4; }
    shapes[j]=sh;
  }
  uint64_t sum=0;
  for(uint64_t i=0;i<8000000;i++){ Shape* sh=shapes[i%N]; sum+=(uint64_t)sh->vt->area(sh); }
  int rc=(int)(sum%256);
  for(int j=0;j<N;j++) free(shapes[j]);
  free(shapes);
  return rc;
}
