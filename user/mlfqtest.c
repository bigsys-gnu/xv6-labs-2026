// user/mlfqtest.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define WORK       3000000000L   // CPU-bound work amount
#define SHORT_WORK  200000000L   // mixed process work amount

// pure CPU-bound work
void cpu_work(long n) {
  volatile long i;
  for(i = 0; i < n; i++)
    ;
}

int main(void) {
  int pids[3];
  int i;

  printf("MLFQ Scheduler Test\n");
  printf("====================\n");
  printf("Must run with: make qemu CPUS=1\n\n");

  int test_start = uptime();
  printf("Test started at tick=%d\n\n", test_start);

  // Process 1: pure CPU-bound -- expected to reach Q2
  if((pids[0] = fork()) == 0) {
    int mypid = getpid();
    int start = uptime();
    int q_start = get_queue_level(mypid);

    printf("[CPU-bound ] PID=%d  started  tick=%d  queue=%d\n",
           mypid, start, q_start);

    cpu_work(WORK);

    int end   = uptime();
    int q_end = get_queue_level(mypid);
    printf("[CPU-bound ] PID=%d  finished tick=%d  queue=%d  "
           "duration=%d  (expected queue: 2)\n",
           mypid, end, q_end, end - start);
    exit(0);
  }

  // Process 2: interactive -- pauses frequently, expected to stay at Q0
  if((pids[1] = fork()) == 0) {
    int mypid = getpid();
    int start = uptime();
    int q_start = get_queue_level(mypid);

    printf("[Interactive] PID=%d  started  tick=%d  queue=%d\n",
           mypid, start, q_start);

    int j;
    for(j = 0; j < 10; j++) {
      pause(1);    // voluntary yield -> keeps queue_level (Rule 5)
    }

    int end   = uptime();
    int q_end = get_queue_level(mypid);
    printf("[Interactive] PID=%d  finished tick=%d  queue=%d  "
           "duration=%d  (expected queue: 0)\n",
           mypid, end, q_end, end - start);
    exit(0);
  }

  // Process 3: mixed -- some CPU work, some pause
  if((pids[2] = fork()) == 0) {
    int mypid = getpid();
    int start = uptime();
    int q_start = get_queue_level(mypid);

    printf("[Mixed     ] PID=%d  started  tick=%d  queue=%d\n",
           mypid, start, q_start);

    int j;
    for(j = 0; j < 5; j++) {
      cpu_work(SHORT_WORK);   // some CPU work
      pause(1);               // then voluntary yield
    }

    int end   = uptime();
    int q_end = get_queue_level(mypid);
    printf("[Mixed     ] PID=%d  finished tick=%d  queue=%d  "
           "duration=%d  (expected queue: 0 or 1)\n",
           mypid, end, q_end, end - start);
    exit(0);
  }

  // parent: wait for all children
  for(i = 0; i < 3; i++)
    wait(0);

  int test_end = uptime();
  printf("\nTest finished at tick=%d  total=%d ticks\n",
         test_end, test_end - test_start);
  printf("\n--- Summary ---\n");
  printf("Higher-priority processes should finish earlier.\n");
  printf("CPU-bound should reach queue=2.\n");
  printf("Interactive should stay at queue=0.\n");
  exit(0);
}
