// Tests for Task 3: Memory-Mapped Files.

#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PGSIZE 4096

static char buf[64];

static void
fail(char *msg)
{
  printf("mmaptest: FAIL: %s\n", msg);
  exit(1);
}

static void
makefile(char *name, int size, char value)
{
  int fd, n;

  unlink(name);
  fd = open(name, O_CREATE | O_RDWR);
  if (fd < 0)
    fail("cannot create test file");

  for (int i = 0; i < sizeof(buf); i++)
    buf[i] = value;

  while (size > 0) {
    n = size < sizeof(buf) ? size : sizeof(buf);
    if (write(fd, buf, n) != n)
      fail("cannot initialize test file");
    size -= n;
  }
  close(fd);
}

static void
checkbyte(char *name, int offset, char expected)
{
  int fd, n;
  char value;

  fd = open(name, O_RDONLY);
  if (fd < 0)
    fail("cannot reopen test file");

  while (offset > 0) {
    n = offset < sizeof(buf) ? offset : sizeof(buf);
    if (read(fd, buf, n) != n)
      fail("cannot read test file");
    offset -= n;
  }

  if (read(fd, &value, 1) != 1 || value != expected)
    fail("file contents do not match");
  close(fd);
}

static void
test_shared_and_partial_unmap(void)
{
  int fd;
  char *p;

  printf("mmaptest: shared mapping and partial munmap\n");
  makefile("mmapfile", 4 * PGSIZE, 'a');

  fd = open("mmapfile", O_RDWR);
  if (fd < 0)
    fail("open failed");

  // Map three pages of a four-page file. Pages load when first accessed.
  p = (char *)mmap(0, 3 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == (char *)-1)
    fail("mmap failed");
  close(fd); // The mapping must retain its file reference.

  // Touch pages in a different order to exercise lazy page loading.
  if (p[2 * PGSIZE] != 'a' || p[0] != 'a' || p[PGSIZE] != 'a')
    fail("mapped file contents are incorrect");

  p[0] = 'A';
  p[PGSIZE] = 'B';
  p[2 * PGSIZE] = 'C';

  if (munmap(p, PGSIZE) < 0)
    fail("munmap of mapping beginning failed");
  if (p[PGSIZE] != 'B')
    fail("remaining mapping after prefix unmap failed");

  if (munmap(p + 2 * PGSIZE, PGSIZE) < 0)
    fail("munmap of mapping end failed");
  if (p[PGSIZE] != 'B')
    fail("middle mapping was affected");

  if (munmap(p + PGSIZE, PGSIZE) < 0)
    fail("munmap of remaining page failed");

  checkbyte("mmapfile", 0, 'A');
  checkbyte("mmapfile", PGSIZE, 'B');
  checkbyte("mmapfile", 2 * PGSIZE, 'C');
  checkbyte("mmapfile", 3 * PGSIZE, 'a');
  unlink("mmapfile");
}

static void
test_private(void)
{
  int fd;
  char *p;

  printf("mmaptest: private mapping\n");
  makefile("mmappriv", PGSIZE, 'b');

  fd = open("mmappriv", O_RDONLY);
  if (fd < 0)
    fail("open private file failed");

  p = (char *)mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == (char *)-1)
    fail("private mmap failed");

  p[10] = 'P';
  if (munmap(p, PGSIZE) < 0)
    fail("private munmap failed");
  close(fd);

  checkbyte("mmappriv", 10, 'b');
  unlink("mmappriv");
}

static void
test_fork_and_exit(void)
{
  int fd, pid, status;
  char *p;

  printf("mmaptest: fork and exit cleanup\n");
  makefile("mmapfork", PGSIZE, 'c');

  fd = open("mmapfork", O_RDWR);
  if (fd < 0)
    fail("open fork file failed");

  p = (char *)mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == (char *)-1)
    fail("fork-test mmap failed");
  close(fd);

  pid = fork();
  if (pid < 0)
    fail("fork failed");
  if (pid == 0) {
    if (p[20] != 'c')
      exit(1);
    p[20] = 'F';
    exit(0); // Exit must write back without an explicit munmap.
  }

  if (wait(&status) != pid || status != 0)
    fail("child could not use inherited mapping");
  if (munmap(p, PGSIZE) < 0)
    fail("parent munmap failed");

  checkbyte("mmapfork", 20, 'F');
  unlink("mmapfork");
}

static void
test_readonly(void)
{
  int fd, pid, status;
  char *p;

  printf("mmaptest: read-only protection\n");
  makefile("mmapro", PGSIZE, 'r');

  fd = open("mmapro", O_RDONLY);
  if (fd < 0)
    fail("open read-only file failed");

  p = (char *)mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == (char *)-1)
    fail("read-only mmap failed");
  if (p[0] != 'r')
    fail("read from read-only mapping failed");

  pid = fork();
  if (pid < 0)
    fail("fork in protection test failed");
  if (pid == 0) {
    p[0] = 'X';
    exit(0); // This should not be reached.
  }

  if (wait(&status) != pid || status != -1)
    fail("write to PROT_READ mapping was not rejected");
  if (munmap(p, PGSIZE) < 0)
    fail("read-only munmap failed");

  close(fd);
  unlink("mmapro");
}

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  printf("mmaptest: starting\n");
  test_shared_and_partial_unmap();
  test_private();
  test_fork_and_exit();
  test_readonly();
  printf("mmaptest: all tests passed\n");
  exit(0);
}
