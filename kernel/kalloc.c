// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// Reference count for each physical memory page.
#define NREFPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
static int refcnt[NREFPAGES];
static uint
paindex(void *pa)
{
  uint64 addr = (uint64)pa;

  if (addr < KERNBASE || addr >= PHYSTOP)
    panic("paindex");

  return (addr - KERNBASE) / PGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    refcnt[paindex(p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) panic("kfree");
  acquire(&kmem.lock);
  uint idx = paindex(pa);
  if (refcnt[idx] < 1) panic("kfree ref");
  // Decrease the reference count
  refcnt[idx]--;
  // Other processes still use this page
  if (refcnt[idx] > 0) {
    release(&kmem.lock);
    return;
  }
  // No references remain. Free the page
  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    uint idx = paindex(r);
    if (refcnt[idx] != 0) panic("kalloc ref");
    //newly allocated page has one reference
    refcnt[idx] = 1;
  }
  release(&kmem.lock);
  if (r) memset((char *)r, 5, PGSIZE);
  return (void *)r;
}

// Increase the reference count of a physical page.
void
kref(void *pa)
{
  acquire(&kmem.lock);
  uint idx = paindex(pa);
  if (refcnt[idx] < 1) panic("kref");
  refcnt[idx]++;
  release(&kmem.lock);
}