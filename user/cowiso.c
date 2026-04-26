#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

#define SMALLPAGES 16
#define NMARKS 3

static int
run_region(char *name, char *region, uint64 size)
{
  int p[2];
  int pid;
  int status;
  int marks[NMARKS] = {0, size / 2, size - 1};
  char before[NMARKS];
  char childvals[NMARKS];

  for(int i = 0; i < NMARKS; i++){
    before[i] = region[marks[i]];
  }

  if(pipe(p) < 0){
    printf("cowiso: pipe failed for %s\n", name);
    return -1;
  }

  pid = fork();
  if(pid < 0){
    printf("cowiso: fork failed for %s\n", name);
    close(p[0]);
    close(p[1]);
    return -1;
  }

  if(pid == 0){
    close(p[0]);
    for(int i = 0; i < NMARKS; i++){
      region[marks[i]] = 'a' + i;
      childvals[i] = region[marks[i]];
    }
    if(write(p[1], childvals, sizeof(childvals)) != sizeof(childvals))
      exit(2);
    close(p[1]);
    exit(0);
  }

  close(p[1]);
  if(read(p[0], childvals, sizeof(childvals)) != sizeof(childvals)){
    printf("cowiso: short read for %s\n", name);
    close(p[0]);
    wait(&status);
    return -1;
  }
  close(p[0]);

  if(wait(&status) < 0 || status != 0){
    printf("cowiso: child failed for %s\n", name);
    return -1;
  }

  for(int i = 0; i < NMARKS; i++){
    if(region[marks[i]] != before[i]){
      printf("cowiso: parent changed in %s at %d\n", name, marks[i]);
      return -1;
    }
    if(childvals[i] != 'a' + i){
      printf("cowiso: child value mismatch in %s at %d\n", name, marks[i]);
      return -1;
    }
  }

  printf("cowiso: %s PASS\n", name);
  return 0;
}

int
main(void)
{
  char *small;
  char *base;
  char *pad;
  char *super;
  int ok = 1;
  int offset;

  small = sbrk(SMALLPAGES * PGSIZE);
  if(small == SBRK_ERROR){
    printf("cowiso: small sbrk failed\n");
    exit(1);
  }
  for(int i = 0; i < SMALLPAGES * PGSIZE; i += PGSIZE)
    small[i] = 's';
  small[SMALLPAGES * PGSIZE - 1] = 'S';

  if(run_region("small-page", small, SMALLPAGES * PGSIZE) < 0)
    ok = 0;

  base = sbrk(0);
  offset = (int)((uint64)base % SUPERPGSIZE);
  if(offset != 0){
    pad = sbrk(SUPERPGSIZE - offset);
    if(pad == SBRK_ERROR){
      printf("cowiso: superpage align failed\n");
      exit(1);
    }
    for(int i = 0; i < SUPERPGSIZE - offset; i += PGSIZE)
      pad[i] = 'p';
  }

  super = sbrk(SUPERPGSIZE);
  if(super == SBRK_ERROR){
    printf("cowiso: superpage alloc failed\n");
    exit(1);
  }
  for(int i = 0; i < SUPERPGSIZE; i += PGSIZE)
    super[i] = 'u';
  super[SUPERPGSIZE / 2] = 'm';
  super[SUPERPGSIZE - 1] = 'U';

  if(((uint64)super % SUPERPGSIZE) != 0)
    printf("cowiso: superpage region not aligned, running fallback check\n");
  if(run_region("superpage-candidate", super, SUPERPGSIZE) < 0)
    ok = 0;

  if(!ok)
    exit(1);
  printf("cowiso: PASS\n");
  exit(0);
}
