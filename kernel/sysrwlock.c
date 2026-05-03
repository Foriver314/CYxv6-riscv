#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "rwlock.h"

#define NRWLOCK 32

struct user_rwlock {
  int used;
  int refs;
  struct rwlock lock;
  struct spinlock meta;
  struct proc *writer;
  struct proc *readers[NPROC];
  int read_counts[NPROC];
};

struct {
  struct spinlock lock;
  struct user_rwlock rwlocks[NRWLOCK];
} rwtable;

static void
clear_owners(struct user_rwlock *u)
{
  u->writer = 0;
  for(int i = 0; i < NPROC; i++){
    u->readers[i] = 0;
    u->read_counts[i] = 0;
  }
}

static int
reader_slot(struct user_rwlock *u, struct proc *p)
{
  for(int i = 0; i < NPROC; i++){
    if(u->readers[i] == p)
      return i;
  }
  return -1;
}

static int
empty_reader_slot(struct user_rwlock *u)
{
  for(int i = 0; i < NPROC; i++){
    if(u->readers[i] == 0)
      return i;
  }
  return -1;
}

static int
is_held(struct user_rwlock *u)
{
  if(u->writer)
    return 1;
  for(int i = 0; i < NPROC; i++){
    if(u->readers[i])
      return 1;
  }
  return 0;
}

static struct user_rwlock*
rwlock_get(int id)
{
  struct user_rwlock *u;

  if(id < 0 || id >= NRWLOCK)
    return 0;

  acquire(&rwtable.lock);
  u = &rwtable.rwlocks[id];
  if(!u->used){
    release(&rwtable.lock);
    return 0;
  }
  u->refs++;
  release(&rwtable.lock);
  return u;
}

static void
rwlock_put(struct user_rwlock *u)
{
  acquire(&rwtable.lock);
  u->refs--;
  release(&rwtable.lock);
}

void
rwlockinit(void)
{
  initlock(&rwtable.lock, "rwtable");
  for(int i = 0; i < NRWLOCK; i++){
    rwtable.rwlocks[i].used = 0;
    rwtable.rwlocks[i].refs = 0;
    initrwlock(&rwtable.rwlocks[i].lock, "urwlock");
    initlock(&rwtable.rwlocks[i].meta, "urwmeta");
    clear_owners(&rwtable.rwlocks[i]);
  }
}

void
rwlock_proc_cleanup(struct proc *p)
{
  struct user_rwlock *u;
  int count;

  for(int i = 0; i < NRWLOCK; i++){
    u = &rwtable.rwlocks[i];

    acquire(&u->meta);
    if(!u->used){
      release(&u->meta);
      continue;
    }
    if(u->writer == p){
      u->writer = 0;
      release(&u->meta);
      releasewrite(&u->lock);
      continue;
    }
    int slot = reader_slot(u, p);
    if(slot >= 0){
      count = u->read_counts[slot];
      u->readers[slot] = 0;
      u->read_counts[slot] = 0;
      release(&u->meta);
      for(int j = 0; j < count; j++)
        releaseread(&u->lock);
      continue;
    }
    release(&u->meta);
  }
}

uint64
sys_rwlock_alloc(void)
{
  struct user_rwlock *u;

  acquire(&rwtable.lock);
  for(int i = 0; i < NRWLOCK; i++){
    u = &rwtable.rwlocks[i];
    if(!u->used && u->refs == 0){
      acquire(&u->meta);
      initrwlock(&u->lock, "urwlock");
      clear_owners(u);
      u->used = 1;
      release(&u->meta);
      release(&rwtable.lock);
      return i;
    }
  }
  release(&rwtable.lock);
  return -1;
}

uint64
sys_rwlock_rdlock(void)
{
  int id;
  int slot;
  struct proc *p = myproc();
  struct user_rwlock *u;

  argint(0, &id);
  u = rwlock_get(id);
  if(u == 0)
    return -1;

  acquire(&u->meta);
  if(u->writer == p || reader_slot(u, p) >= 0){
    release(&u->meta);
    rwlock_put(u);
    return -1;
  }
  release(&u->meta);

  if(acquireread(&u->lock) < 0){
    rwlock_put(u);
    return -1;
  }

  acquire(&u->meta);
  slot = empty_reader_slot(u);
  if(slot < 0){
    release(&u->meta);
    releaseread(&u->lock);
    rwlock_put(u);
    return -1;
  }
  u->readers[slot] = p;
  u->read_counts[slot] = 1;
  release(&u->meta);

  rwlock_put(u);
  return 0;
}

uint64
sys_rwlock_wrlock(void)
{
  int id;
  struct proc *p = myproc();
  struct user_rwlock *u;

  argint(0, &id);
  u = rwlock_get(id);
  if(u == 0)
    return -1;

  acquire(&u->meta);
  if(u->writer == p || reader_slot(u, p) >= 0){
    release(&u->meta);
    rwlock_put(u);
    return -1;
  }
  release(&u->meta);

  if(acquirewrite(&u->lock) < 0){
    rwlock_put(u);
    return -1;
  }

  acquire(&u->meta);
  u->writer = p;
  release(&u->meta);

  rwlock_put(u);
  return 0;
}

uint64
sys_rwlock_unlock(void)
{
  int id;
  int slot;
  int count = 0;
  int mode = 0;
  struct proc *p = myproc();
  struct user_rwlock *u;

  argint(0, &id);
  u = rwlock_get(id);
  if(u == 0)
    return -1;

  acquire(&u->meta);
  if(u->writer == p){
    u->writer = 0;
    mode = 2;
  } else if((slot = reader_slot(u, p)) >= 0){
    count = u->read_counts[slot];
    u->readers[slot] = 0;
    u->read_counts[slot] = 0;
    mode = 1;
  }
  release(&u->meta);

  if(mode == 2){
    releasewrite(&u->lock);
  } else if(mode == 1){
    for(int i = 0; i < count; i++)
      releaseread(&u->lock);
  } else {
    rwlock_put(u);
    return -1;
  }

  rwlock_put(u);
  return 0;
}

uint64
sys_rwlock_free(void)
{
  int id;
  int ok = 0;
  struct user_rwlock *u;

  argint(0, &id);
  u = rwlock_get(id);
  if(u == 0)
    return -1;

  acquire(&rwtable.lock);
  acquire(&u->meta);
  if(u->used && u->refs == 1 && !is_held(u) && rwlock_isidle(&u->lock)){
    u->used = 0;
    clear_owners(u);
    ok = 1;
  }
  release(&u->meta);
  release(&rwtable.lock);

  rwlock_put(u);
  return ok ? 0 : -1;
}
