#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "rwlock.h"

void
initrwlock(struct rwlock *rw, char *name)
{
  initlock(&rw->lk, "rwlock");
  rw->name = name;
  rw->readers = 0;
  rw->writer = 0;
  rw->waiting_readers = 0;
  rw->waiting_writers = 0;
  rw->writer_pid = 0;
}

static void
acquireread_common(struct rwlock *rw, int interruptible, int *failed)
{
  acquire(&rw->lk);
  rw->waiting_readers++;
  while(rw->writer || rw->waiting_writers > 0){
    if(interruptible && myproc() && killed(myproc())){
      rw->waiting_readers--;
      wakeup(rw);
      release(&rw->lk);
      *failed = 1;
      return;
    }
    sleep(rw, &rw->lk);
  }
  rw->waiting_readers--;
  rw->readers++;
  release(&rw->lk);
  *failed = 0;
}

int
acquireread(struct rwlock *rw)
{
  int failed;

  acquireread_common(rw, 1, &failed);
  return failed ? -1 : 0;
}

void
acquireread_blocking(struct rwlock *rw)
{
  int failed;

  acquireread_common(rw, 0, &failed);
}

void
releaseread(struct rwlock *rw)
{
  acquire(&rw->lk);
  if(rw->readers <= 0)
    panic("releaseread");
  rw->readers--;
  if(rw->readers == 0)
    wakeup(rw);
  release(&rw->lk);
}

static void
acquirewrite_common(struct rwlock *rw, int interruptible, int *failed)
{
  struct proc *p;

  acquire(&rw->lk);
  rw->waiting_writers++;
  while(rw->writer || rw->readers > 0){
    if(interruptible && myproc() && killed(myproc())){
      rw->waiting_writers--;
      wakeup(rw);
      release(&rw->lk);
      *failed = 1;
      return;
    }
    sleep(rw, &rw->lk);
  }
  rw->waiting_writers--;
  rw->writer = 1;
  p = myproc();
  rw->writer_pid = p ? p->pid : 0;
  release(&rw->lk);
  *failed = 0;
}

int
acquirewrite(struct rwlock *rw)
{
  int failed;

  acquirewrite_common(rw, 1, &failed);
  return failed ? -1 : 0;
}

void
acquirewrite_blocking(struct rwlock *rw)
{
  int failed;

  acquirewrite_common(rw, 0, &failed);
}

void
releasewrite(struct rwlock *rw)
{
  acquire(&rw->lk);
  if(!rw->writer)
    panic("releasewrite");
  rw->writer = 0;
  rw->writer_pid = 0;
  wakeup(rw);
  release(&rw->lk);
}

int
holdingread(struct rwlock *rw)
{
  int r;

  acquire(&rw->lk);
  r = rw->readers > 0;
  release(&rw->lk);
  return r;
}

int
holdingwrite(struct rwlock *rw)
{
  int r;
  struct proc *p = myproc();

  acquire(&rw->lk);
  r = p && rw->writer && rw->writer_pid == p->pid;
  release(&rw->lk);
  return r;
}

int
rwlock_isidle(struct rwlock *rw)
{
  int r;

  acquire(&rw->lk);
  r = rw->readers == 0 && !rw->writer &&
      rw->waiting_readers == 0 && rw->waiting_writers == 0;
  release(&rw->lk);
  return r;
}
