// user/runtimetest.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// CPU-bound job: iterate n-times
void cpu_work(long n) {
  volatile long i;
  for(i = 0; i < n; i++)
    ;
}

int main(void) {
  printf("Runtime Measurement Test\n");
  printf("=========================\n");

  int pid1 = fork();
  if(pid1 == 0) {
    // child 1: less work
    cpu_work(1000000000L);
    int mypid = getpid();
    printf("Child 1 (PID=%d): runtime = %d ticks\n", 
        mypid, get_runtime(mypid));
    exit(0);
  }

  int pid2 = fork();
  if(pid2 == 0) {
    // child 2: more work
    cpu_work(3000000000L);
    int mypid = getpid();
    printf("Child 2 (PID=%d): runtime = %d ticks\n", 
        mypid, get_runtime(mypid));
    exit(0);
  }

  // parent: wait children
  int status;
  wait(&status);
  wait(&status);

  exit(0);
}
