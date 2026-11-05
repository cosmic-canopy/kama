// A thread started before fork does not exist in the child.
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
static void* idle(void* a) { (void)a; pause(); return 0; }
static int threads(void) { FILE* f = fopen("/proc/self/status","r"); char l[256]; int n=-1; while (fgets(l,256,f)) if (!strncmp(l,"Threads:",8)) sscanf(l+8,"%d",&n); fclose(f); return n; }
int main(void) { pthread_t t[4]; for (int i=0;i<4;i++) pthread_create(&t[i],0,idle,0); usleep(10000);
  printf("parent threads=%d\n", threads()); fflush(stdout);
  pid_t p=fork(); if(!p){ printf("child after fork threads=%d\n", threads()); fflush(stdout); _exit(0);} waitpid(p,0,0); return 0; }
