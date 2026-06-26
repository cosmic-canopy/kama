#include <stdint.h>
int main(void){
  double pi=0.0, sign=1.0;
  for(uint64_t i=0;i<20000000ULL;i++){ uint64_t d=2*i+1; pi += sign/(double)d; sign=-sign; }
  double scaled=pi*4000000000.0; uint64_t check=(uint64_t)scaled; return (int)(check%256);
}
