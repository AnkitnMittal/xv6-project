#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PGSIZE 4096
#define MAP_FAILED ((void *)-1)

static char filedata[2 * PGSIZE];
static char checkdata[2 * PGSIZE];
static int failures;

static void
report(const char *name, int ok)
{
  if (ok) {
    printf("%s: PASS\n", name);
  } else {
    printf("%s: FAIL\n", name);
    failures++;
  }
}

// Create or replace a file whose pages contain distinct byte patterns.
static int
make_file(const char *name, int pages, char first, char second)
{
  int fd;
  int i;
  int len = pages * PGSIZE;

  for (i = 0; i < len; i++)
    filedata[i] = (i < PGSIZE) ? first : second;

  fd = open(name, O_CREATE | O_RDWR | O_TRUNC);
  if (fd < 0) {
    printf("mmaptest: cannot create %s\n", name);
    return -1;
  }

  if (write(fd, filedata, len) != len) {
    printf("mmaptest: cannot write %s\n", name);
    close(fd);
    return -1;
  }

  return fd;
}

// Verify one byte in a file by reading from its beginning.
static int
file_byte_is(const char *name, int offset, char expected)
{
  int fd;
  int n;
  int ok;

  if (offset < 0 || offset >= 2 * PGSIZE)
    return 0;

  fd = open(name, O_RDONLY);
  if (fd < 0)
    return 0;

  n = read(fd, checkdata, offset + 1);
  close(fd);

  ok = (n == offset + 1 && checkdata[offset] == expected);
  return ok;
}

static void
test_basic_read(void)
{
  int fd = make_file("mmapread", 1, 'A', 'B');
  char *p;
  int ok = 0;

  if (fd < 0) {
    report("basic file read", 0);
    return;
  }

  p = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p != MAP_FAILED) {
    // First access should cause a lazy page fault and load the file.
    ok = (p[0] == 'A' && p[1] == 'A');
    if (munmap(p, PGSIZE) < 0)
      ok = 0;
  }

  close(fd);
  report("basic file read / lazy loading", ok);
}

static void
test_shared_writeback(void)
{
  int fd = make_file("mmapshared", 1, 'A', 'B');
  char *p;
  int ok = 0;

  if (fd < 0) {
    report("MAP_SHARED writeback", 0);
    return;
  }

  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED) {
    p[0] = 'S';
    p[100] = 'X';
    if (munmap(p, PGSIZE) == 0)
      ok = file_byte_is("mmapshared", 0, 'S') &&
           file_byte_is("mmapshared", 100, 'X');
  }

  close(fd);
  report("MAP_SHARED writeback", ok);
}

static void
test_private_mapping(void)
{
  int fd = make_file("mmapprivate", 1, 'A', 'B');
  char *p;
  int ok = 0;

  if (fd < 0) {
    report("MAP_PRIVATE isolation", 0);
    return;
  }

  // A writable private mapping is allowed on a read-only descriptor.
  close(fd);
  fd = open("mmapprivate", O_RDONLY);
  if (fd < 0) {
    report("MAP_PRIVATE isolation", 0);
    return;
  }

  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p != MAP_FAILED) {
    p[0] = 'P';
    p[1] = 'Q';
    if (munmap(p, PGSIZE) == 0)
      ok = file_byte_is("mmapprivate", 0, 'A') &&
           file_byte_is("mmapprivate", 1, 'A');
  }

  close(fd);
  report("MAP_PRIVATE does not write back", ok);
}

static void
test_mapping_longer_than_file(void)
{
  int fd = make_file("mmapshort", 1, 'L', 'L');
  char *p;
  int ok = 0;

  if (fd < 0) {
    report("mapping longer than file", 0);
    return;
  }

  p = mmap(0, 2 * PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p != MAP_FAILED) {
    // The bytes beyond EOF should be zero-filled.
    ok = (p[0] == 'L' && p[PGSIZE] == 0 && p[PGSIZE + 100] == 0);
    if (munmap(p, 2 * PGSIZE) < 0)
      ok = 0;
  }

  close(fd);
  report("mapping longer than file (zero-filled tail)", ok);
}

static void
test_partial_unmap_start(void)
{
  int fd = make_file("mmapfront", 2, 'A', 'B');
  char *p;
  int ok = 0;

  if (fd < 0) {
    report("partial unmap from beginning", 0);
    return;
  }

  p = mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED) {
    // Fault in both pages before removing the first page.
    if (p[0] == 'A' && p[PGSIZE] == 'B' &&
        munmap(p, PGSIZE) == 0) {
      // The second page remains mapped and must retain file offset PGSIZE.
      if (p[PGSIZE] == 'B') {
        p[PGSIZE + 10] = 'Z';
        if (munmap(p + PGSIZE, PGSIZE) == 0)
          ok = file_byte_is("mmapfront", PGSIZE + 10, 'Z');
      }
    }
  }

  close(fd);
  report("partial unmap from beginning", ok);
}

static void
test_partial_unmap_end(void)
{
  int fd = make_file("mmapback", 2, 'A', 'B');
  char *p;
  int ok = 0;

  if (fd < 0) {
    report("partial unmap from end", 0);
    return;
  }

  p = mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED) {
    if (p[0] == 'A' && p[PGSIZE] == 'B' &&
        munmap(p + PGSIZE, PGSIZE) == 0) {
      // The first page remains mapped.
      p[10] = 'E';
      p[11] = 'F';
      if (munmap(p, PGSIZE) == 0)
        ok = file_byte_is("mmapback", 10, 'E') &&
             file_byte_is("mmapback", 11, 'F');
    }
  }

  close(fd);
  report("partial unmap from end", ok);
}

static void
test_fork_shared(void)
{
  int fd = make_file("mmapforks", 1, 'A', 'A');
  char *p;
  int pid;
  int ok = 0;

  if (fd < 0) {
    report("fork MAP_SHARED", 0);
    return;
  }

  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) {
    close(fd);
    report("fork MAP_SHARED", 0);
    return;
  }

  // Make sure the page is resident before fork.
  if (p[0] != 'A') {
    munmap(p, PGSIZE);
    close(fd);
    report("fork MAP_SHARED", 0);
    return;
  }

  pid = fork();
  if (pid == 0) {
    p[0] = 'C';
    exit(0);
  } else if (pid > 0) {
    wait(0);
    // Parent should see the child's write through the shared physical page.
    ok = (p[0] == 'C');
    if (munmap(p, PGSIZE) < 0)
      ok = 0;
    if (ok)
      ok = file_byte_is("mmapforks", 0, 'C');
  }

  close(fd);
  report("fork MAP_SHARED", ok);
}

static void
test_fork_private(void)
{
  int fd = make_file("mmapforkp", 1, 'Q', 'Q');
  char *p;
  int pid;
  int ok = 0;

  if (fd < 0) {
    report("fork MAP_PRIVATE", 0);
    return;
  }

  close(fd);
  fd = open("mmapforkp", O_RDONLY);
  if (fd < 0) {
    report("fork MAP_PRIVATE", 0);
    return;
  }

  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) {
    close(fd);
    report("fork MAP_PRIVATE", 0);
    return;
  }

  if (p[0] != 'Q') {
    munmap(p, PGSIZE);
    close(fd);
    report("fork MAP_PRIVATE", 0);
    return;
  }

  pid = fork();
  if (pid == 0) {
    p[0] = 'C';
    exit(0);
  } else if (pid > 0) {
    wait(0);
    // Parent's private page must remain unchanged.
    ok = (p[0] == 'Q');
    if (munmap(p, PGSIZE) < 0)
      ok = 0;
    if (ok)
      ok = file_byte_is("mmapforkp", 0, 'Q');
  }

  close(fd);
  report("fork MAP_PRIVATE", ok);
}

static void
test_invalid_shared_write(void)
{
  int fd = make_file("mmapro", 1, 'R', 'R');
  void *p;
  int ok = 0;

  if (fd < 0) {
    report("reject shared writable mapping of read-only fd", 0);
    return;
  }

  close(fd);
  fd = open("mmapro", O_RDONLY);
  if (fd >= 0) {
    p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ok = (p == MAP_FAILED);
    if (!ok)
      munmap(p, PGSIZE);
    close(fd);
  }

  report("reject shared writable mapping of read-only fd", ok);
}

int
main(void)
{
  printf("\n=== mmap/munmap tests starting ===\n");

  test_basic_read();
  test_shared_writeback();
  test_private_mapping();
  test_mapping_longer_than_file();
  test_partial_unmap_start();
  test_partial_unmap_end();
  test_fork_shared();
  test_fork_private();
  test_invalid_shared_write();

  if (failures == 0) {
    printf("=== ALL MMAP TESTS PASSED ===\n");
    exit(0);
  }

  printf("=== MMAP TESTS FAILED: %d ===\n", failures);
  exit(1);
}
