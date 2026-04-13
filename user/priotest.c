// user/priotest.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define WORK_AMOUNT 5000000000L

void cpu_work(void) {
  volatile long i;
  for(i = 0; i < WORK_AMOUNT; i++)
    ;
}

int main(void) {
  int priorities[3] = {1, 5, 9};
  int pids[3];
  int runtimes[3];
  int total = 0;
  int i;

  printf("Priority Scheduling Test\n");
  printf("=========================\n");
  printf("Lower priority value = higher priority\n\n");

  // fork three children
  for(i = 0; i < 3; i++) {
    int pid = fork();
    if(pid < 0) {
      printf("fork failed\n");
      exit(1);
    }
    if(pid == 0) {
      set_priority(priorities[i]);
      printf("child %d: set priority=%d\n", getpid(), priorities[i]);
      cpu_work();
      int rt = get_runtime(getpid());
      exit(rt);        // pass runtime to parent via exit status
    }
    pids[i] = pid;     // parent records each child's PID
  }

  // parent: collect results — match by PID to preserve priority order
  for(i = 0; i < 3; i++) {
    int status;
    int wpid = wait(&status);   // wpid: PID of the child that just finished
    int j;
    for(j = 0; j < 3; j++) {
      if(pids[j] == wpid) {     // match finished child to its priority slot
        runtimes[j] = status;
        total += status;
        break;
      }
    }
  }

  // parent prints all results — no interleaving
  printf("--- Results ---\n");
  for(i = 0; i < 3; i++) {
    int pct = (total > 0) ? (runtimes[i] * 100 / total) : 0;
    printf("priority=%d  PID=%d  runtime=%d ticks  CPU=%d%%\n",
           priorities[i], pids[i], runtimes[i], pct);
  }
  printf("total: %d ticks\n\n", total);

  if(runtimes[0] > runtimes[2])
    printf("PASS: priority=1 got more CPU than priority=9\n");
  else
    printf("FAIL: priority scheduling not working\n");

  printf("\nDone.\n");
  exit(0);
}
