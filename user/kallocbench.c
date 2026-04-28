#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define NWORKER 8
#define SMALL_PAGES 16
#define SMALL_TICKS 40
#define COW_ROUNDS 20
#define COW_SIZE SUPERPGSIZE

static void
touch(char *p, int size, int stride)
{
  for(int i = 0; i < size; i += stride)
    p[i]++;
  p[size - 1]++;
}

static int
small_once(void)
{
  char *p;

  p = sbrk(SMALL_PAGES * PGSIZE);
  if(p == SBRK_ERROR)
    return -1;
  touch(p, SMALL_PAGES * PGSIZE, PGSIZE);
  if(sbrk(-SMALL_PAGES * PGSIZE) == SBRK_ERROR)
    return -1;
  return 0;
}

static void
small_worker(int fd)
{
  int t0;
  int rounds;

  rounds = 0;
  t0 = uptime();
  while(uptime() - t0 < SMALL_TICKS){
    if(small_once() < 0)
      exit(1);
    rounds++;
  }
  if(write(fd, &rounds, sizeof(rounds)) != sizeof(rounds))
    exit(1);
  exit(0);
}

static void
cow_worker(void)
{
  char *p;
  int pid;
  int status;

  p = sbrk(COW_SIZE);
  if(p == SBRK_ERROR)
    exit(1);
  touch(p, COW_SIZE, PGSIZE);

  for(int i = 0; i < COW_ROUNDS; i++){
    pid = fork();
    if(pid < 0)
      exit(1);
    if(pid == 0){
      touch(p, COW_SIZE, PGSIZE);
      exit(0);
    }
    if(wait(&status) < 0 || status != 0)
      exit(1);
  }
  exit(0);
}

static void
measure_small(void)
{
  int p[2];
  int pid;
  int status;
  int rounds;
  int total;
  int t0;
  int ticks;
  int failed;

  if(pipe(p) < 0){
    printf("kallocbench: small pipe failed\n");
    exit(1);
  }

  failed = 0;
  t0 = uptime();
  for(int i = 0; i < NWORKER; i++){
    pid = fork();
    if(pid < 0){
      failed = 1;
      break;
    }
    if(pid == 0){
      close(p[0]);
      small_worker(p[1]);
    }
  }
  close(p[1]);

  total = 0;
  for(int i = 0; i < NWORKER; i++){
    if(read(p[0], &rounds, sizeof(rounds)) != sizeof(rounds)){
      failed = 1;
      break;
    }
    total += rounds;
  }
  close(p[0]);

  for(int i = 0; i < NWORKER; i++){
    if(wait(&status) < 0 || status != 0)
      failed = 1;
  }
  ticks = uptime() - t0;

  if(failed || ticks <= 0){
    printf("kallocbench: small failed\n");
    exit(1);
  }
  printf("kallocbench: small total_rounds %d ticks %d ops_per_tick_x100 %d\n",
         total, ticks, total * 100 / ticks);
}

static int
run_workers(void (*fn)(void))
{
  int pid;
  int status;
  int failed;

  failed = 0;
  for(int i = 0; i < NWORKER; i++){
    pid = fork();
    if(pid < 0){
      failed = 1;
      break;
    }
    if(pid == 0)
      fn();
  }

  for(int i = 0; i < NWORKER; i++){
    if(wait(&status) < 0 || status != 0)
      failed = 1;
  }
  return failed ? -1 : 0;
}

static void
measure(char *name, void (*fn)(void))
{
  int t0;
  int ticks;

  t0 = uptime();
  if(run_workers(fn) < 0){
    printf("kallocbench: %s failed\n", name);
    exit(1);
  }
  ticks = uptime() - t0;
  printf("kallocbench: %s ticks %d\n", name, ticks);
}

int
main(void)
{
  printf("kallocbench: workers %d\n", NWORKER);
  printf("kallocbench: small_pages %d small_ticks %d\n", SMALL_PAGES, SMALL_TICKS);
  measure_small();
  measure("cow", cow_worker);
  printf("kallocbench: PASS\n");
  exit(0);
}
