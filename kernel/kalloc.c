// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers.
//
// The allocator is a binary buddy allocator over 4096-byte base pages.
// kalloc()/kfree() keep the traditional xv6 one-page interface, while
// kalloc_order()/kfree_order() can allocate aligned runs for future
// superpage mappings. Reference counts are tracked per base page so a
// later COW implementation can share and release ordinary pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define KMEM_NPAGE ((PHYSTOP - KERNBASE) / PGSIZE)
#define BUDDY_MAX_ORDER 15
#define REF_NLOCK 64
#define KCACHE_MAX 32
#define KCACHE_REFILL 16

#define PAGE_ALLOCATED 0
#define PAGE_FREE 1
#define PAGE_FREEING 2
#define PAGE_CACHE 3

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct page {
  int ref;
  uchar order;
  uchar is_free;
  uchar usable;
};

struct kcache {
  struct spinlock lock;
  struct run *freelist;
  int count;
};

struct {
  struct spinlock buddy_lock;
  struct spinlock ref_lock[REF_NLOCK];
  struct kcache cache[NCPU];
  struct run *freelist[BUDDY_MAX_ORDER + 1];
} kmem;

static struct page pages[KMEM_NPAGE];

static int
pa_index(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

static uint64
index_pa(int idx)
{
  return KERNBASE + (uint64)idx * PGSIZE;
}

static int
valid_order(int order)
{
  return order >= 0 && order <= BUDDY_MAX_ORDER;
}

static int
valid_page(uint64 pa)
{
  if(pa % PGSIZE)
    return 0;
  if(pa < KERNBASE || pa >= PHYSTOP)
    return 0;
  return pages[pa_index(pa)].usable;
}

static struct spinlock*
page_ref_lock(int idx)
{
  return &kmem.ref_lock[idx % REF_NLOCK];
}

static struct kcache*
lock_current_cache(void)
{
  struct kcache *c;

  push_off();
  c = &kmem.cache[cpuid()];
  acquire(&c->lock);
  pop_off();
  return c;
}

static void
push_block(int idx, int order)
{
  struct run *r;

  pages[idx].order = order;
  pages[idx].is_free = PAGE_FREE;
  r = (struct run*)index_pa(idx);
  r->next = kmem.freelist[order];
  kmem.freelist[order] = r;
}

static void
remove_block(int idx, int order)
{
  struct run **p;
  struct run *r;

  p = &kmem.freelist[order];
  r = (struct run*)index_pa(idx);
  while(*p){
    if(*p == r){
      *p = r->next;
      pages[idx].is_free = PAGE_ALLOCATED;
      return;
    }
    p = &(*p)->next;
  }
  panic("buddy remove");
}

static void
free_block(int idx, int order)
{
  int buddy;

  while(order < BUDDY_MAX_ORDER){
    buddy = idx ^ (1 << order);
    if(buddy < 0 || buddy >= KMEM_NPAGE)
      break;
    if(!pages[buddy].usable || pages[buddy].is_free != PAGE_FREE ||
       pages[buddy].order != order)
      break;

    remove_block(buddy, order);
    if(buddy < idx)
      idx = buddy;
    order++;
  }
  push_block(idx, order);
}

static void *
alloc_block(int order)
{
  int o;
  int idx;
  struct run *r;

  if(!valid_order(order))
    panic("kalloc_order");

  for(o = order; o <= BUDDY_MAX_ORDER; o++){
    r = kmem.freelist[o];
    if(r)
      break;
  }
  if(o > BUDDY_MAX_ORDER)
    return 0;

  idx = pa_index((uint64)r);
  kmem.freelist[o] = r->next;
  pages[idx].is_free = PAGE_ALLOCATED;

  while(o > order){
    o--;
    push_block(idx + (1 << o), o);
  }

  pages[idx].order = order;
  pages[idx].is_free = PAGE_ALLOCATED;
  return (void*)index_pa(idx);
}

static void
init_allocated_block(int idx, int order)
{
  int i;
  int npage;

  npage = 1 << order;
  for(i = 0; i < npage; i++){
    pages[idx + i].ref = 1;
    pages[idx + i].is_free = PAGE_ALLOCATED;
    pages[idx + i].order = order;
  }
}

static void
freerange(void *pa_start, void *pa_end)
{
  char *p;

  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    int idx = pa_index((uint64)p);
    pages[idx].usable = 1;
    pages[idx].ref = 0;
    pages[idx].order = 0;
    pages[idx].is_free = PAGE_ALLOCATED;
    free_block(idx, 0);
  }
}

static void
free_to_buddy(int idx, int order)
{
  acquire(&kmem.buddy_lock);
  if(!pages[idx].usable || pages[idx].is_free != PAGE_FREEING ||
     pages[idx].order != order)
    panic("buddy free");
  free_block(idx, order);
  release(&kmem.buddy_lock);
}

static void
cache_push_locked(struct kcache *c, int idx)
{
  struct run *r;

  r = (struct run*)index_pa(idx);
  r->next = c->freelist;
  c->freelist = r;
  c->count++;
}

static void *
cache_pop(void)
{
  struct kcache *c;
  struct spinlock *lk;
  struct run *r;
  int idx;

  c = lock_current_cache();
  r = c->freelist;
  if(r){
    c->freelist = r->next;
    c->count--;
  }
  release(&c->lock);

  if(r == 0)
    return 0;

  idx = pa_index((uint64)r);
  lk = page_ref_lock(idx);
  acquire(lk);
  if(pages[idx].is_free != PAGE_CACHE || pages[idx].order != 0 ||
     pages[idx].ref != 0)
    panic("kcache pop");
  pages[idx].is_free = PAGE_ALLOCATED;
  pages[idx].ref = 1;
  pages[idx].order = 0;
  release(lk);

  return (void*)r;
}

static int
cache_push(void *pa)
{
  struct kcache *c;
  struct spinlock *lk;
  int idx;

  idx = pa_index((uint64)pa);
  c = lock_current_cache();
  if(c->count >= KCACHE_MAX){
    release(&c->lock);
    return 0;
  }

  lk = page_ref_lock(idx);
  acquire(lk);
  if(pages[idx].is_free != PAGE_FREEING || pages[idx].order != 0 ||
     pages[idx].ref != 0)
    panic("kcache push");
  pages[idx].is_free = PAGE_CACHE;
  cache_push_locked(c, idx);
  release(lk);
  release(&c->lock);
  return 1;
}

static void
drain_cache(struct kcache *c)
{
  struct spinlock *lk;
  struct run *r;
  struct run *next;
  int idx;

  acquire(&c->lock);
  r = c->freelist;
  c->freelist = 0;
  c->count = 0;
  release(&c->lock);

  while(r){
    next = r->next;
    idx = pa_index((uint64)r);
    lk = page_ref_lock(idx);
    acquire(lk);
    if(pages[idx].is_free != PAGE_CACHE || pages[idx].order != 0 ||
       pages[idx].ref != 0)
      panic("kcache drain");
    pages[idx].is_free = PAGE_FREEING;
    release(lk);
    free_to_buddy(idx, 0);
    r = next;
  }
}

static void
drain_all_caches(void)
{
  int i;

  for(i = 0; i < NCPU; i++)
    drain_cache(&kmem.cache[i]);
}

static void
refill_cache(void)
{
  struct kcache *c;
  void *pa[KCACHE_REFILL];
  int i;
  int idx;
  int n;

  n = 0;
  acquire(&kmem.buddy_lock);
  for(i = 0; i < KCACHE_REFILL; i++){
    pa[n] = alloc_block(0);
    if(pa[n] == 0)
      break;
    idx = pa_index((uint64)pa[n]);
    pages[idx].ref = 0;
    pages[idx].order = 0;
    pages[idx].is_free = PAGE_CACHE;
    n++;
  }
  release(&kmem.buddy_lock);

  if(n == 0)
    return;

  c = lock_current_cache();
  for(i = 0; i < n && c->count < KCACHE_MAX; i++)
    cache_push_locked(c, pa_index((uint64)pa[i]));
  release(&c->lock);

  for(; i < n; i++){
    idx = pa_index((uint64)pa[i]);
    acquire(page_ref_lock(idx));
    if(pages[idx].is_free != PAGE_CACHE || pages[idx].order != 0 ||
       pages[idx].ref != 0)
      panic("kcache refill");
    pages[idx].is_free = PAGE_FREEING;
    release(page_ref_lock(idx));
    free_to_buddy(idx, 0);
  }
}

static void *
alloc_from_buddy(int order)
{
  void *pa;
  int idx;

  acquire(&kmem.buddy_lock);
  pa = alloc_block(order);
  if(pa){
    idx = pa_index((uint64)pa);
    init_allocated_block(idx, order);
  }
  release(&kmem.buddy_lock);
  return pa;
}

void
kinit()
{
  int i;

  initlock(&kmem.buddy_lock, "kmembuddy");
  for(i = 0; i < REF_NLOCK; i++)
    initlock(&kmem.ref_lock[i], "kmemref");
  for(i = 0; i < NCPU; i++){
    initlock(&kmem.cache[i].lock, "kmemcache");
    kmem.cache[i].freelist = 0;
    kmem.cache[i].count = 0;
  }

  acquire(&kmem.buddy_lock);
  freerange(end, (void*)PHYSTOP);
  release(&kmem.buddy_lock);
}

void *
kalloc_order(int order)
{
  void *pa;

  if(!valid_order(order))
    panic("kalloc_order");

  if(order == 0){
    pa = cache_pop();
    if(pa == 0){
      refill_cache();
      pa = cache_pop();
    }
    if(pa == 0){
      drain_all_caches();
      pa = alloc_from_buddy(0);
    }
  } else {
    pa = alloc_from_buddy(order);
    if(pa == 0){
      drain_all_caches();
      pa = alloc_from_buddy(order);
    }
  }

  if(pa)
    memset(pa, 5, PGSIZE << order); // fill with junk
  return pa;
}

void
kfree_order(void *pa, int order)
{
  struct spinlock *lk;
  int idx;
  int i;
  int npage;
  int ref;

  if(!valid_order(order) || ((uint64)pa % (PGSIZE << order)) != 0)
    panic("kfree_order");
  if(!valid_page((uint64)pa))
    panic("kfree_order");

  idx = pa_index((uint64)pa);
  npage = 1 << order;
  if(idx + npage > KMEM_NPAGE)
    panic("kfree_order");

  lk = page_ref_lock(idx);
  acquire(lk);
  if(pages[idx].is_free != PAGE_ALLOCATED || pages[idx].order != order)
    panic("kfree_order");
  ref = pages[idx].ref;
  for(i = 0; i < npage; i++){
    if(!pages[idx + i].usable || pages[idx + i].is_free != PAGE_ALLOCATED ||
       pages[idx + i].order != order || pages[idx + i].ref != ref || ref < 1)
      panic("kfree_order");
    pages[idx + i].ref--;
  }
  if(ref > 1){
    release(lk);
    return;
  }
  for(i = 0; i < npage; i++)
    pages[idx + i].is_free = PAGE_FREEING;
  release(lk);

  memset(pa, 1, PGSIZE << order);
  if(order == 0 && cache_push(pa))
    return;
  free_to_buddy(idx, order);
}

// Increment the reference count of one base page.
// Intended for future COW mappings.
void
kaddref_order(void *pa, int order)
{
  struct spinlock *lk;
  int idx;
  int i;
  int npage;
  int ref;

  if(!valid_order(order) || ((uint64)pa % (PGSIZE << order)) != 0)
    panic("kaddref_order");
  if(!valid_page((uint64)pa))
    panic("kaddref_order");

  idx = pa_index((uint64)pa);
  npage = 1 << order;
  if(idx + npage > KMEM_NPAGE)
    panic("kaddref_order");

  lk = page_ref_lock(idx);
  acquire(lk);
  if(pages[idx].is_free != PAGE_ALLOCATED || pages[idx].order != order)
    panic("kaddref_order");
  ref = pages[idx].ref;
  for(i = 0; i < npage; i++){
    if(!pages[idx + i].usable || pages[idx + i].is_free != PAGE_ALLOCATED ||
       pages[idx + i].order != order || pages[idx + i].ref != ref || ref < 1)
      panic("kaddref_order");
    pages[idx + i].ref++;
  }
  release(lk);
}

// Split allocator metadata for a private allocated order block into
// smaller allocated blocks. Future COW code must break sharing before
// demoting a shared superpage.
void
ksplit_order(void *pa, int old_order, int new_order)
{
  struct spinlock *lk;
  int idx;
  int i;
  int npage;
  int ref;

  if(!valid_order(old_order) || !valid_order(new_order) ||
     new_order >= old_order ||
     ((uint64)pa % (PGSIZE << old_order)) != 0)
    panic("ksplit_order");
  if(!valid_page((uint64)pa))
    panic("ksplit_order");

  idx = pa_index((uint64)pa);
  npage = 1 << old_order;
  if(idx + npage > KMEM_NPAGE)
    panic("ksplit_order");

  lk = page_ref_lock(idx);
  acquire(lk);
  if(pages[idx].is_free != PAGE_ALLOCATED || pages[idx].order != old_order)
    panic("ksplit_order");
  ref = pages[idx].ref;
  if(ref != 1)
    panic("ksplit_order: shared");
  for(i = 0; i < npage; i++){
    if(!pages[idx + i].usable || pages[idx + i].is_free != PAGE_ALLOCATED ||
       pages[idx + i].order != old_order || pages[idx + i].ref != ref ||
       ref < 1)
      panic("ksplit_order");
  }
  for(i = 0; i < npage; i++)
    pages[idx + i].order = new_order;
  release(lk);
}

void
kaddref(void *pa)
{
  kaddref_order(pa, 0);
}

int
kgetref(void *pa)
{
  struct spinlock *lk;
  int ref;
  int idx;

  if(!valid_page((uint64)pa))
    panic("kgetref");

  idx = pa_index((uint64)pa);
  lk = page_ref_lock(idx);
  acquire(lk);
  ref = pages[idx].ref;
  release(lk);
  return ref;
}

// Free the 4096-byte page of physical memory pointed at by pa.
void
kfree(void *pa)
{
  kfree_order(pa, 0);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  return kalloc_order(0);
}
