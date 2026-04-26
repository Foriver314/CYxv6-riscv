#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define TICK_TARGET 30
#define NSIZES 3

static uint64 sizes[NSIZES] = {
  64 * 1024,
  SUPERPGSIZE,
  8 * 1024 * 1024,
};

static void
warmup(char *buf, uint64 size)
{
  for(uint64 i = 0; i < size; i += PGSIZE)
    buf[i] = (char)(i / PGSIZE);
  buf[size - 1] = 1;
}

static int
run_round(char *buf, uint64 size, int dirty)
{
  int pid;
  int status;

  pid = fork();
  if(pid < 0)
    return -1;
  if(pid == 0){
    if(dirty){
      for(uint64 i = 0; i < size; i += PGSIZE)
        buf[i]++;
      buf[size - 1]++;
    }
    exit(0);
  }
  if(wait(&status) < 0)
    return -1;
  if(status != 0)
    return -1;
  return 0;
}

static int
measure(char *buf, uint64 size, int dirty, int *roundsp)
{
  int rounds;
  int t0;
  int t1;

  if(run_round(buf, size, dirty) < 0)
    return -1;

  rounds = 0;
  t0 = uptime();
  do {
    if(run_round(buf, size, dirty) < 0)
      return -1;
    rounds++;
    t1 = uptime();
  } while(t1 - t0 < TICK_TARGET);

  *roundsp = rounds;
  return t1 - t0;
}

int
main(void)
{
  char *buf;
  uint64 max = sizes[NSIZES - 1];

  buf = sbrk(max);
  if(buf == SBRK_ERROR){
    printf("cowbench: sbrk failed\n");
    exit(1);
  }
  warmup(buf, max);

  printf("cowbench: ticks are from uptime(), lower is better\n");
  printf("cowbench: size_KiB share_ticks share_rounds share_tpf_x1000 dirty_ticks dirty_rounds dirty_tpf_x1000 speedup_x100\n");
  printf("cowbench: -------- ---------- ------------ ---------------- ----------- ------------ ---------------- -------------\n");
  for(int i = 0; i < NSIZES; i++){
    uint64 size = sizes[i];
    int share_rounds;
    int dirty_rounds;
    int share_ticks;
    int dirty_ticks;

    warmup(buf, size);
    share_ticks = measure(buf, size, 0, &share_rounds);
    if(share_ticks < 0){
      printf("cowbench: share run failed at %d bytes\n", (int)size);
      exit(1);
    }

    warmup(buf, size);
    dirty_ticks = measure(buf, size, 1, &dirty_rounds);
    if(dirty_ticks < 0){
      printf("cowbench: dirty run failed at %d bytes\n", (int)size);
      exit(1);
    }

    printf("cowbench: %8d %10d %12d %16d %11d %12d %16d %13d\n",
           (int)(size / 1024),
           share_ticks,
           share_rounds,
           share_ticks * 1000 / share_rounds,
           dirty_ticks,
           dirty_rounds,
           dirty_ticks * 1000 / dirty_rounds,
           share_rounds * 100 / dirty_rounds);
  }

  exit(0);
}
