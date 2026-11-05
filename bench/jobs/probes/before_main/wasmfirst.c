// What a thread start costs on wasm, under node: first, second, then ten at once until all are running.
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }
static atomic_int running; static atomic_int stop;
static void* one(void* a){ (void)a; return 0; }
static void* park(void* a){ (void)a; atomic_fetch_add(&running,1); while(!atomic_load(&stop)){} return 0; }
int main(void){ pthread_t t; double a=now(); pthread_create(&t,0,one,0); pthread_join(t,0); double b=now(); pthread_create(&t,0,one,0); pthread_join(t,0); double c=now();
  printf("wasm: first create+join %.2f ms, second %.3f ms\n", b-a, c-b);
  pthread_t ts[10]; double d=now(); for(int i=0;i<10;i++) pthread_create(&ts[i],0,park,0); double e=now(); while(atomic_load(&running)<10){} double f=now(); atomic_store(&stop,1); for(int i=0;i<10;i++) pthread_join(ts[i],0);
  printf("wasm: 10 more: create returns %.2f ms, all running %.2f ms\n", e-d, f-d); return 0; }
