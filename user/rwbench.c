#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "user/user.h"

#define NWORKER 6
#define RUN_TICKS 100
#define START_DELAY 20
#define DATA_ITEMS 256
#define WORK_ITERS 4096
#define FS_RUN_TICKS 100
#define FS_FILE_BLOCKS 8

static int data[DATA_ITEMS];
static volatile int sink;
static int bench_lock;
static int bench_start;
static int bench_stop;
static int bench_writes_per_ten;
static int bench_exclusive;
static int fs_start;
static int fs_stop;

static void
initdata(void)
{
  for(int i = 0; i < DATA_ITEMS; i++)
    data[i] = i * 17 + 3;
}

static void
work(int salt)
{
  int acc = sink;

  for(int i = 0; i < WORK_ITERS; i++)
    acc += data[(i + salt) & (DATA_ITEMS - 1)];
  sink = acc;
}

static void
make_fs_file(void)
{
  int fd;
  char block[BSIZE];

  unlink("rwbenchfile");
  fd = open("rwbenchfile", O_CREATE|O_WRONLY);
  if(fd < 0){
    printf("rwbench: create rwbenchfile failed\n");
    exit(1);
  }
  for(int i = 0; i < sizeof(block); i++)
    block[i] = i;
  for(int i = 0; i < FS_FILE_BLOCKS; i++){
    if(write(fd, block, sizeof(block)) != sizeof(block)){
      printf("rwbench: write rwbenchfile failed\n");
      exit(1);
    }
  }
  close(fd);
}

static void
worker(int fd, int id)
{
  int rounds = 0;
  int failed = 0;

  while(uptime() < bench_start)
    pause(1);

  while(uptime() < bench_stop){
    int writeop = (rounds % 10) < bench_writes_per_ten;
    int rc;

    if(bench_exclusive || writeop)
      rc = rwlock_wrlock(bench_lock);
    else
      rc = rwlock_rdlock(bench_lock);
    if(rc < 0){
      failed = 1;
      break;
    }

    work(rounds + id);

    if(rwlock_unlock(bench_lock) < 0){
      failed = 1;
      break;
    }
    rounds++;
  }

  if(failed)
    rounds = -1;
  if(write(fd, &rounds, sizeof(rounds)) != sizeof(rounds))
    exit(1);
  exit(0);
}

static int
run_once(char *name, int writes_per_ten, int exclusive, int *totalp, int *ticksp)
{
  int p[2];
  int pid;
  int status;
  int rounds;
  int failed = 0;

  bench_lock = rwlock_alloc();
  if(bench_lock < 0)
    return -1;
  bench_writes_per_ten = writes_per_ten;
  bench_exclusive = exclusive;
  bench_start = uptime() + START_DELAY;
  bench_stop = bench_start + RUN_TICKS;

  if(pipe(p) < 0)
    return -1;

  for(int i = 0; i < NWORKER; i++){
    pid = fork();
    if(pid < 0){
      failed = 1;
      break;
    }
    if(pid == 0){
      close(p[0]);
      worker(p[1], i);
    }
  }
  close(p[1]);

  *totalp = 0;
  for(int i = 0; i < NWORKER; i++){
    if(read(p[0], &rounds, sizeof(rounds)) != sizeof(rounds)){
      failed = 1;
      break;
    }
    if(rounds < 0)
      failed = 1;
    else
      *totalp += rounds;
  }
  close(p[0]);

  for(int i = 0; i < NWORKER; i++){
    if(wait(&status) < 0 || status != 0)
      failed = 1;
  }

  *ticksp = uptime() - bench_start;
  if(rwlock_free(bench_lock) < 0)
    failed = 1;
  if(failed || *ticksp <= 0){
    printf("rwbench: %s failed\n", name);
    return -1;
  }
  return 0;
}

static void
fs_worker(int pipefd, int id)
{
  int fd;
  int rounds = 0;
  int failed = 0;
  char buf[BSIZE];
  struct stat st;

  fd = open("rwbenchfile", O_RDONLY);
  if(fd < 0)
    exit(1);

  while(uptime() < fs_start)
    pause(1);

  while(uptime() < fs_stop){
    int n;

    if(fstat(fd, &st) < 0){
      failed = 1;
      break;
    }
    n = read(fd, buf, sizeof(buf));
    if(n < 0){
      failed = 1;
      break;
    }
    if(n == 0){
      close(fd);
      fd = open("rwbenchfile", O_RDONLY);
      if(fd < 0){
        failed = 1;
        break;
      }
      continue;
    }
    sink += buf[(rounds + id) & (sizeof(buf) - 1)] + st.size;
    rounds++;
  }

  close(fd);
  if(failed)
    rounds = -1;
  if(write(pipefd, &rounds, sizeof(rounds)) != sizeof(rounds))
    exit(1);
  exit(0);
}

static void
measure_fs(void)
{
  int p[2];
  int pid;
  int status;
  int rounds;
  int total = 0;
  int failed = 0;
  int ticks;

  make_fs_file();
  fs_start = uptime() + START_DELAY;
  fs_stop = fs_start + FS_RUN_TICKS;

  if(pipe(p) < 0)
    exit(1);
  for(int i = 0; i < NWORKER; i++){
    pid = fork();
    if(pid < 0){
      failed = 1;
      break;
    }
    if(pid == 0){
      close(p[0]);
      fs_worker(p[1], i);
    }
  }
  close(p[1]);

  for(int i = 0; i < NWORKER; i++){
    if(read(p[0], &rounds, sizeof(rounds)) != sizeof(rounds)){
      failed = 1;
      break;
    }
    if(rounds < 0)
      failed = 1;
    else
      total += rounds;
  }
  close(p[0]);

  for(int i = 0; i < NWORKER; i++){
    if(wait(&status) < 0 || status != 0)
      failed = 1;
  }

  ticks = uptime() - fs_start;
  unlink("rwbenchfile");
  if(failed || ticks <= 0){
    printf("rwbench: fsread failed\n");
    exit(1);
  }
  printf("rwbench: fsread     workers %d rounds %d ticks %d ops_per_tick_x100 %d\n",
         NWORKER, total, ticks, total * 100 / ticks);
}

static void
measure(char *name, int writes_per_ten)
{
  int exclusive_total;
  int exclusive_ticks;
  int rw_total;
  int rw_ticks;
  int exclusive_x100;
  int rw_x100;

  if(run_once(name, writes_per_ten, 1, &exclusive_total, &exclusive_ticks) < 0)
    exit(1);
  if(run_once(name, writes_per_ten, 0, &rw_total, &rw_ticks) < 0)
    exit(1);

  exclusive_x100 = exclusive_total * 100 / exclusive_ticks;
  rw_x100 = rw_total * 100 / rw_ticks;
  printf("rwbench: %-10s exclusive_rounds %d exclusive_ticks %d exclusive_ops_per_tick_x100 %d rw_rounds %d rw_ticks %d rw_ops_per_tick_x100 %d speedup_x100 %d\n",
         name,
         exclusive_total,
         exclusive_ticks,
         exclusive_x100,
         rw_total,
         rw_ticks,
         rw_x100,
         rw_x100 * 100 / exclusive_x100);
}

int
main(void)
{
  initdata();
  printf("rwbench: workers %d run_ticks %d work_iters %d\n",
         NWORKER, RUN_TICKS, WORK_ITERS);
  printf("rwbench: exclusive uses wrlock for every operation\n");
  printf("rwbench: rw uses rdlock for read operations and wrlock for write operations\n");
  printf("rwbench: writes_per_ten is the number of write operations in each 10-round cycle\n");
  printf("rwbench: higher ops_per_tick_x100 is better\n");
  measure("readonly", 0);
  measure("readmostly", 1);
  measure("balanced", 5);
  measure("writemostly", 9);
  measure("writeonly", 10);
  measure_fs();
  printf("rwbench: PASS\n");
  exit(0);
}
