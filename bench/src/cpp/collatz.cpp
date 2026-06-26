#include <stdint.h>
static uint64_t clen(uint64_t n){ uint64_t s=0; while(n!=1){ if((n%2)==0) n=n/2; else n=3*n+1; s++; } return s; }
int main(void){ uint64_t s=0; for(uint64_t i=1;i<700000;i++) s+=clen(i); return (int)(s%256); }
