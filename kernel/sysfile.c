//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->nlink >= NLINK_MAX) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  // dp may have been unlinked while we resolved it; linking into an
  // orphaned directory leaks ip (itrunc discards the record without
  // dropping ip->nlink).  create() has the same guard.
  if (dp->nlink == 0) {
    iunlockput(dp);
    goto bad;
  }
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if (argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (ip->type == T_DIR && !isdirempty(ip)) {
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if (dp->nlink == 0) {
    iunlockput(dp);
    return 0;
  }

  // a new directory's ".." would push dp->nlink past its maximum
  if (type == T_DIR && dp->nlink >= NLINK_MAX) {
    iunlockput(dp);
    return 0;
  }

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) { // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if (type == T_DIR) {
    // now that success is guaranteed:
    dp->nlink++; // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, p->sz, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, p->sz, fdarray + sizeof(fd0), (char *)&fd1,
              sizeof(fd1)) < 0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// Memory map a file into the process's virtual address space.
uint64
sys_mmap(void)
{
  uint64 addr;
  uint64 length;
  uint64 offset;

  int prot;
  int flags;
  int fd;

  struct file *f;
  struct proc *p = myproc();
  struct vm_area_struct *vm = 0;

  // read system call arguments
  argaddr(0, &addr);
  argaddr(1, &length);
  argint(2, &prot);
  argint(3, &flags);
  argaddr(5, &offset);

  // validate
  if (addr != 0 || length == 0 || offset != 0)
    return -1;
  if (prot != PROT_READ &&
      prot != PROT_WRITE &&
      prot != (PROT_READ | PROT_WRITE))
    return -1;
  if (flags != MAP_SHARED && flags != MAP_PRIVATE)
    return -1;

  // get the file descriptor and corresponding file
  if (argfd(4, &fd, &f) < 0)
    return -1;

  // only regular files can be memory mapped
  if (f->type != FD_INODE || !f->readable)
    return -1;

  ilock(f->ip);
  int regular_file = (f->ip->type == T_FILE);
  iunlock(f->ip);

  if (!regular_file)
    return -1;

  // shared writable mapping requires a writable file
  if (flags == MAP_SHARED &&
      (prot & PROT_WRITE) &&
      !f->writable)
    return -1;

  // calculate page-aligned mapping size
  if (length > TRAPFRAME)
    return -1;

  uint64 map_len = PGROUNDUP(length);

  if (map_len < length)
    return -1;

  // allocate virtual address space from top to bottom
  uint64 cur_max = p->cur_max;

  if (cur_max < map_len)
    return -1;

  uint64 start_addr = cur_max - map_len;

  // do not overlap existing heap memory
  if (start_addr < PGROUNDUP(p->sz))
    return -1;

  // find an available VMA entry
  for (int i = 0; i < NVMA; i++) {
    if (p->vma[i].valid == 0) {
      vm = &p->vma[i];
      break;
    }
  }

  if (vm == 0)
    return -1;

  // initialize the VMA
  vm->valid = 1;
  vm->start_ad = start_addr;
  vm->end_ad = cur_max;
  vm->len = length;
  vm->offset = 0;
  vm->prot = prot;
  vm->flags = flags;
  vm->fd = fd;
  // keep the file alive even if the user closes its fd
  vm->file = filedup(f);
  // update the next available mmap address
  p->cur_max = start_addr;
  // return the starting virtual address
  return start_addr;
}

// Unmap a previously mapped memory region.
uint64
sys_munmap(void) {
  uint64 addr;
  uint64 length;
  struct proc *p = myproc();
  struct vm_area_struct *vm = 0;
  // 1. Read system call arguments.
  argaddr(0, &addr);
  argaddr(1, &length);
  // 2. Validate arguments.
  if (length == 0 || addr % PGSIZE != 0) return -1;
  if (addr + length < addr || addr + length >= MAXVA) return -1;
  uint64 start = addr;
  uint64 end = PGROUNDUP(addr + length);
  if (end <= start) return -1;

  // 3. Find the VMA containing the requested range.
  for (int i = 0; i < NVMA; i++) {
    if (p->vma[i].valid &&
        start >= p->vma[i].start_ad &&
        end <= p->vma[i].end_ad) {

      vm = &p->vma[i];
      break;
    }
  }
  if (vm == 0) return -1;
  // 4. Only allow unmapping from the beginning,
  //    the end, or the entire VMA.
  if (start != vm->start_ad && end != vm->end_ad) return -1;

  // 5. Write modified pages back for MAP_SHARED.
  if (vm->flags == MAP_SHARED) {
    struct inode *ip = vm->file->ip;
    for (uint64 va = start; va < end; va += PGSIZE) {
      pte_t *pte = walk(p->pagetable, va, 0);
      // Skip pages that have never been allocated.
      if (pte == 0 || (*pte & PTE_V) == 0) continue;
      uint64 relative_off = va - vm->start_ad;
      uint64 file_off = vm->offset + relative_off;
      // Do not write beyond the mapping length.
      if (relative_off >= vm->len) continue;
      uint64 bytes = vm->len - relative_off;
      if (bytes > PGSIZE) bytes = PGSIZE;
      // Begin one filesystem transaction per page.
      begin_op();
      ilock(ip);
      // Do not extend the file beyond its existing size.
      if (file_off < ip->size) {
        uint available = ip->size - (uint)file_off;
        if (bytes > available) bytes = available;
        if (bytes > 0) {
          uint64 pa = PTE2PA(*pte);
          writei(ip, 0, pa, (uint)file_off, (uint)bytes);
        }
      }
      iunlock(ip);
      end_op();
    }
  }

  // 6. Remove the specified mappings.
  uint64 npages = (end - start) / PGSIZE;
  uvmunmap(p->pagetable, start, npages, 1);
  // 7. Update VMA metadata.
  // Case A: Entire VMA is removed.
  if (start == vm->start_ad && end == vm->end_ad) {
    struct file *f = vm->file;
    if (p->cur_max == vm->start_ad) p->cur_max = vm->end_ad;
    memset(vm, 0, sizeof(*vm));
    fileclose(f);
  }

  // Case B: Beginning of VMA is removed.
  else if (start == vm->start_ad) {
    uint64 removed = end - start;
    vm->offset += removed;
    if (vm->len > removed) vm->len -= removed;
    else vm->len = 0;
    vm->start_ad = end;
    // Reclaim address space if this was the
    // lowest allocated VMA.
    if (p->cur_max == start) p->cur_max = end;
  }
  // Case C: End of VMA is removed.
  else if (end == vm->end_ad) {
    uint64 remaining = start - vm->start_ad;
    if (vm->len > remaining) vm->len = remaining;
    vm->end_ad = start;
  }
  return 0;
}

// Clean up all memory mapped regions of a process.
void
mmap_cleanup(struct proc *p)
{
  for (int i = 0; i < NVMA; i++) {
    struct vm_area_struct *vm = &p->vma[i];
    if (!vm->valid) continue;
    // Write modified pages back for MAP_SHARED.
    if (vm->flags == MAP_SHARED) {
      struct inode *ip = vm->file->ip;
      for (uint64 va = vm->start_ad; va < vm->end_ad; va += PGSIZE) {
        pte_t *pte = walk(p->pagetable, va, 0);

        // Skip pages that have not been allocated.
        if (pte == 0 || !(*pte & PTE_V)) continue;
        uint64 relative_off = va - vm->start_ad;
        if (relative_off >= vm->len) continue;
        uint64 file_off = vm->offset + relative_off;
        uint64 bytes = vm->len - relative_off;

        if (bytes > PGSIZE) bytes = PGSIZE;
        begin_op();
        ilock(ip);

        // Never extend the file during mmap cleanup.
        if (file_off < ip->size) {
          uint available = ip->size - (uint)file_off;
          if (bytes > available) bytes = available;
          if (bytes > 0) {
            uint64 pa = PTE2PA(*pte);
            writei(ip, 0, pa, (uint)file_off, (uint)bytes);
          }
        }
        iunlock(ip);
        end_op();
      }
    }

    // Unmap all pages belonging to this VMA.
    if (p->pagetable) {
      uint64 npages = (vm->end_ad - vm->start_ad) / PGSIZE;
      uvmunmap(p->pagetable,
               vm->start_ad,
               npages,
               1);
    }
    // Release the mapped file reference.
    struct file *f = vm->file;
    memset(vm, 0, sizeof(*vm));
    if (f) fileclose(f);
  }
  // Restore the initial mmap allocation boundary.
  p->cur_max = TRAPFRAME;
}