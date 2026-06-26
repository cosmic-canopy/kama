#include <stdint.h>
static uint64_t fib(uint64_t n){ if(n<2) return n; return fib(n-1)+fib(n-2); }
int main(void){ uint64_t s=0; for(uint64_t i=0;i<32;i++) s+=fib(i); return (int)(s%256); }
