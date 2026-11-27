#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "stat.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[]; // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t)kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext - KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP - (uint64)etext,
         PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if (mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if (va >= MAXVA)
    panic("walk");

  for (int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if (*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if (!alloc || (pagetable = (pde_t *)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if (va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    return 0;
  if ((*pte & PTE_V) == 0)
    return 0;
  if ((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if ((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if (size == 0)
    panic("mappages: size");

  a = va;
  last = va + size - PGSIZE;
  for (;;) {
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if (a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t)kalloc();
  if (pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if ((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for (a = va; a < va + npages * PGSIZE; a += PGSIZE) {
    if ((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;
    if ((*pte & PTE_V) == 0) // has physical page been allocated?
      continue;
    if (do_free) {
      uint64 pa = PTE2PA(*pte);
      kfree((void *)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if (newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for (a = oldsz; a < newsz; a += PGSIZE) {
    mem = kalloc();
    if (mem == 0) {
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if (mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R | PTE_U | xperm) !=
        0) {
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if (newsz >= oldsz)
    return oldsz;

  if (PGROUNDUP(newsz) < PGROUNDUP(oldsz)) {
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) && (pte & (PTE_R | PTE_W | PTE_X)) == 0) {
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if (pte & PTE_V) {
      panic("freewalk: leaf");
    }
  }
  kfree((void *)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if (sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for (i = 0; i < sz; i += PGSIZE) {
    if ((pte = walk(old, i, 0)) == 0)
      continue; // page table entry hasn't been allocated
    if ((*pte & PTE_V) == 0)
      continue; // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if ((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char *)pa, PGSIZE);
    if (mappages(new, i, PGSIZE, (uint64)mem, flags) != 0) {
      kfree(mem);
      goto err;
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 psz, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;
  struct proc *p = myproc();

  while (len > 0) {
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      int r = 0;
      if (pagetable == p->pagetable)
        r = mmap_pagefault(p, va0, 1);

      if (r < 0)
        return -1;

      if (r == 0) {
        pa0 = vmfault(pagetable, psz, va0, 0);
        if (pa0 == 0)
          return -1;
      } else {
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0)
          return -1;
      }
    }

    pte = walk(pagetable, va0, 0);

    // Forbid copyout over read-only user pages.
    if ((*pte & PTE_W) == 0)
      return -1;

    n = PGSIZE - (dstva - va0);
    if (n > len)
      n = len;

    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }

  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  struct proc *p = myproc();

  while (len > 0) {
    va0 = PGROUNDDOWN(srcva);

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      int r = 0;
      if (pagetable == p->pagetable)
        r = mmap_pagefault(p, va0, 0);

      if (r < 0)
        return -1;

      if (r == 0) {
        pa0 = vmfault(pagetable, psz, va0, 1);
        if (pa0 == 0)
          return -1;
      } else {
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0)
          return -1;
      }
    }

    n = PGSIZE - (srcva - va0);
    if (n > len)
      n = len;

    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }

  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, uint64 psz, char *dst, uint64 srcva,
          uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;
  struct proc *proc = myproc();

  while (got_null == 0 && max > 0) {
    va0 = PGROUNDDOWN(srcva);

    pa0 = walkaddr(pagetable, va0);
    if (pa0 == 0) {
      int r = 0;
      if (pagetable == proc->pagetable)
        r = mmap_pagefault(proc, va0, 0);

      if (r < 0)
        return -1;

      if (r == 0) {
        pa0 = vmfault(pagetable, psz, va0, 1);
        if (pa0 == 0)
          return -1;
      } else {
        pa0 = walkaddr(pagetable, va0);
        if (pa0 == 0)
          return -1;
      }
    }

    n = PGSIZE - (srcva - va0);
    if (n > max)
      n = max;

    char *p = (char *)(pa0 + (srcva - va0));

    while (n > 0) {
      if (*p == '\0') {
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }

      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }

  return got_null ? 0 : -1;
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 psz, uint64 va, int read)
{
  uint64 mem;

  if (va >= psz)
    return 0;
  va = PGROUNDDOWN(va);
  if (ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64)kalloc();
  if (mem == 0)
    return 0;
  memset((void *)mem, 0, PGSIZE);
  if (mappages(pagetable, va, PGSIZE, mem, PTE_W | PTE_U | PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V) {
    return 1;
  }
  return 0;
}

static struct mmap_area *
mmap_find_area(struct proc *p, uint64 va)
{
  for (int i = 0; i < NMMAP; i++) {
    struct mmap_area *a = &p->mmap_areas[i];

    if (!a->valid)
      continue;

    if (va >= a->addr && va < a->addr + a->len)
      return a;
  }

  return 0;
}

uint64
mmap_min_addr(struct proc *p)
{
  uint64 limit = TRAPFRAME;

  for (int i = 0; i < NMMAP; i++) {
    struct mmap_area *a = &p->mmap_areas[i];

    if (a->valid && a->addr < limit)
      limit = a->addr;
  }

  return limit;
}

static uint64
mmap_find_addr(struct proc *p, uint64 len)
{
  uint64 size = PGROUNDUP(len);
  uint64 top = TRAPFRAME;

  for (;;) {
    if (top < size)
      return 0;

    uint64 start = top - size;

    if (start < PGROUNDUP(p->sz))
      return 0;

    int overlap = 0;

    for (int i = 0; i < NMMAP; i++) {
      struct mmap_area *a = &p->mmap_areas[i];

      if (!a->valid)
        continue;

      uint64 vstart = a->addr;
      uint64 vend = a->addr + PGROUNDUP(a->len);

      if (start < vend && start + size > vstart) {
        top = vstart;
        overlap = 1;
        break;
      }
    }

    if (!overlap)
      return start;
  }
}

uint64
mmap_create(struct proc *p, struct file *f, uint64 len, int prot, int flags)
{
  struct mmap_area *a = 0;

  if (len == 0 || len > TRAPFRAME)
    return (uint64)-1;

  if (prot == 0 || (prot & ~(PROT_READ | PROT_WRITE)))
    return (uint64)-1;

  if (flags != MAP_SHARED && flags != MAP_PRIVATE)
    return (uint64)-1;

  if (f == 0 || f->type != FD_INODE || f->ip == 0)
    return (uint64)-1;

  if (!f->readable)
    return (uint64)-1;

  if (flags == MAP_SHARED && (prot & PROT_WRITE) && !f->writable)
    return (uint64)-1;

  ilock(f->ip);
  short type = f->ip->type;
  iunlock(f->ip);

  if (type != T_FILE)
    return (uint64)-1;

  for (int i = 0; i < NMMAP; i++) {
    if (!p->mmap_areas[i].valid) {
      a = &p->mmap_areas[i];
      break;
    }
  }

  if (a == 0)
    return (uint64)-1;

  uint64 addr = mmap_find_addr(p, len);

  if (addr == 0)
    return (uint64)-1;

  memset(a, 0, sizeof(*a));

  a->valid = 1;
  a->addr = addr;
  a->len = len;
  a->offset = 0;
  a->prot = prot;
  a->flags = flags;
  a->file = filedup(f);

  return addr;
}

int
mmap_pagefault(struct proc *p, uint64 va, int is_write)
{
  struct mmap_area *a = mmap_find_area(p, va);

  if (a == 0)
    return 0;

  if (is_write && !(a->prot & PROT_WRITE))
    return -1;

  uint64 pageva = PGROUNDDOWN(va);
  pte_t *pte = walk(p->pagetable, pageva, 0);

  if (pte && (*pte & PTE_V))
    return -1;

  char *mem = kalloc();

  if (mem == 0)
    return -1;

  memset(mem, 0, PGSIZE);

  uint64 page_offset = pageva - a->addr;
  uint64 file_offset = a->offset + page_offset;
  uint64 remaining = a->len - page_offset;
  uint n = remaining > PGSIZE ? PGSIZE : remaining;

  ilock(a->file->ip);

  if (file_offset < a->file->ip->size) {
    uint available = a->file->ip->size - file_offset;

    if (n > available)
      n = available;

    if (n > 0) {
      int r = readi(a->file->ip, 0, (uint64)mem, (uint)file_offset, n);

      if (r < 0) {
        iunlock(a->file->ip);
        kfree(mem);
        return -1;
      }
    }
  }

  iunlock(a->file->ip);

  int perm = PTE_U | PTE_R;

  if (a->prot & PROT_WRITE)
    perm |= PTE_W;

  if (mappages(p->pagetable, pageva, PGSIZE, (uint64)mem, perm) != 0) {
    kfree(mem);
    return -1;
  }

  return 1;
}

static int
mmap_writeback_page(struct mmap_area *a, uint64 va, uint64 pa)
{
  uint64 page_offset = va - a->addr;

  if (page_offset >= a->len)
    return 0;

  uint n = a->len - page_offset;

  if (n > PGSIZE)
    n = PGSIZE;

  uint64 file_offset = a->offset + page_offset;

  begin_op();
  ilock(a->file->ip);

  if (file_offset >= a->file->ip->size) {
    n = 0;
  } else {
    uint available = a->file->ip->size - file_offset;

    if (n > available)
      n = available;
  }

  int written = 0;

  if (n > 0)
    written = writei(a->file->ip, 0, pa, (uint)file_offset, n);

  iunlock(a->file->ip);
  end_op();

  return written == n ? 0 : -1;
}

int
mmap_unmap(struct proc *p, uint64 addr, uint64 len)
{
  if (len == 0 || addr >= TRAPFRAME)
    return -1;

  if (addr % PGSIZE != 0)
    return -1;

  if (len > TRAPFRAME - addr)
    return -1;

  uint64 end = PGROUNDUP(addr + len);

  for (int i = 0; i < NMMAP; i++) {
    struct mmap_area *a = &p->mmap_areas[i];

    if (!a->valid)
      continue;

    uint64 vstart = a->addr;
    uint64 vend = PGROUNDUP(a->addr + a->len);

    if (addr < vstart || addr >= vend)
      continue;

    if (end > vend)
      return -1;

    int at_start = (addr == vstart);
    int at_end = (end == vend);

    if (!at_start && !at_end)
      return -1;

    int write_error = 0;

    if (a->flags == MAP_SHARED && (a->prot & PROT_WRITE)) {
      for (uint64 va = addr; va < end; va += PGSIZE) {
        pte_t *pte = walk(p->pagetable, va, 0);

        if (pte == 0 || !(*pte & PTE_V))
          continue;

        uint64 pa = PTE2PA(*pte);

        if (mmap_writeback_page(a, va, pa) < 0)
          write_error = 1;
      }
    }

    for (uint64 va = addr; va < end; va += PGSIZE) {
      pte_t *pte = walk(p->pagetable, va, 0);

      if (pte && (*pte & PTE_V))
        uvmunmap(p->pagetable, va, 1, 1);
    }

    sfence_vma();

    if (at_start && at_end) {
      struct file *f = a->file;

      memset(a, 0, sizeof(*a));
      fileclose(f);
    } else if (at_start) {
      uint64 removed = end - a->addr;

      a->addr = end;
      a->offset += removed;
      a->len -= removed;
    } else {
      a->len = addr - a->addr;
    }

    return write_error ? -1 : 0;
  }

  return 0;
}

void
mmap_cleanup(struct proc *p)
{
  for (int i = 0; i < NMMAP; i++) {
    struct mmap_area *a = &p->mmap_areas[i];

    if (!a->valid)
      continue;

    uint64 addr = a->addr;
    uint64 len = a->len;

    mmap_unmap(p, addr, len);
  }
}