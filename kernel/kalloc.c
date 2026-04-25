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

struct {
  struct spinlock lock;
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

static void
push_block(int idx, int order)
{
  struct run *r;

  pages[idx].order = order;
  pages[idx].is_free = 1;
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
      pages[idx].is_free = 0;
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
    if(!pages[buddy].usable || !pages[buddy].is_free || pages[buddy].order != order)
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
  pages[idx].is_free = 0;

  while(o > order){
    o--;
    push_block(idx + (1 << o), o);
  }

  pages[idx].order = order;
  pages[idx].is_free = 0;
  return (void*)index_pa(idx);
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
    pages[idx].is_free = 0;
    free_block(idx, 0);
  }
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  acquire(&kmem.lock);
  freerange(end, (void*)PHYSTOP);
  release(&kmem.lock);
}

void *
kalloc_order(int order)
{
  void *pa;
  int idx;
  int i;
  int npage;

  acquire(&kmem.lock);
  pa = alloc_block(order);
  if(pa){
    idx = pa_index((uint64)pa);
    npage = 1 << order;
    for(i = 0; i < npage; i++){
      pages[idx + i].ref = 1;
      pages[idx + i].is_free = 0;
      pages[idx + i].order = order;
    }
  }
  release(&kmem.lock);

  if(pa)
    memset(pa, 5, PGSIZE << order); // fill with junk
  return pa;
}

void
kfree_order(void *pa, int order)
{
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

  acquire(&kmem.lock);
  if(pages[idx].is_free || pages[idx].order != order)
    panic("kfree_order");
  ref = pages[idx].ref;
  for(i = 0; i < npage; i++){
    if(!pages[idx + i].usable || pages[idx + i].order != order ||
       pages[idx + i].ref != ref || ref < 1)
      panic("kfree_order");
    pages[idx + i].ref--;
  }
  if(ref > 1){
    release(&kmem.lock);
    return;
  }
  memset(pa, 1, PGSIZE << order);
  pages[idx].order = order;
  free_block(idx, order);
  release(&kmem.lock);
}

// Increment the reference count of one base page.
// Intended for future COW mappings.
void
kaddref_order(void *pa, int order)
{
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

  acquire(&kmem.lock);
  if(pages[idx].is_free || pages[idx].order != order)
    panic("kaddref_order");
  ref = pages[idx].ref;
  for(i = 0; i < npage; i++){
    if(!pages[idx + i].usable || pages[idx + i].order != order ||
       pages[idx + i].ref != ref || ref < 1)
      panic("kaddref_order");
    pages[idx + i].ref++;
  }
  release(&kmem.lock);
}

void
kaddref(void *pa)
{
  kaddref_order(pa, 0);
}

int
kgetref(void *pa)
{
  int ref;

  if(!valid_page((uint64)pa))
    panic("kgetref");

  acquire(&kmem.lock);
  ref = pages[pa_index((uint64)pa)].ref;
  release(&kmem.lock);
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
