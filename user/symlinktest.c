#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define PGSIZE 4096
#define MAP_FAILED ((void *)-1)

static char page_data[2 * PGSIZE];
static char copy_data[2 * PGSIZE];
static int failures;

static void
check(const char *name, int ok)
{
  if (ok)
    printf("symlinktest: %s: PASS\n", name);
  else {
    printf("symlinktest: %s: FAIL\n", name);
    failures++;
  }
}

static int
write_file(const char *path, const char *data, int n)
{
  int fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
  int ok = fd >= 0;

  if (ok)
    ok = write(fd, data, n) == n;
  if (fd >= 0)
    close(fd);
  return ok;
}

static int
read_file(const char *path, char *data, int n)
{
  int fd = open(path, O_RDONLY);
  int got = -1;

  if (fd >= 0) {
    got = read(fd, data, n);
    close(fd);
  }
  return got;
}

static void
cleanup(void)
{
  unlink("sl-target");
  unlink("sl-link");
  unlink("sl-copy");
  unlink("sl-small");
  unlink("sl-pages");
  unlink("sl-dangling");
  unlink("sl-loop-a");
  unlink("sl-loop-b");
}

static void
test_create_and_follow(void)
{
  int fd;
  int ok;
  struct stat st;
  char buf[16];

  ok = write_file("sl-target", "original", 8) &&
       symlink("sl-target", "sl-link") == 0;
  fd = open("sl-link", O_RDONLY);
  if (fd < 0) {
    ok = 0;
  } else {
    int n = read(fd, buf, sizeof(buf));
    ok = ok && n == 8 && memcmp(buf, "original", 8) == 0;
    close(fd);
  }
  fd = open("sl-link", O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    ok = 0;
  } else {
    int r = fstat(fd, &st);
    ok = ok && r == 0 && st.type == T_SYMLINK &&
         st.size == strlen("sl-target");
    close(fd);
  }
  check("create, follow, and inspect link", ok);
}

static void
test_write_copy_and_truncate(void)
{
  int fd;
  int ok;
  char buf[32];

  fd = open("sl-link", O_WRONLY | O_TRUNC);
  ok = fd >= 0;
  if (ok)
    ok = write(fd, "rewritten", 9) == 9;
  if (fd >= 0)
    close(fd);

  ok = ok && read_file("sl-target", buf, sizeof(buf)) == 9 &&
       memcmp(buf, "rewritten", 9) == 0;
  ok = ok && read_file("sl-link", buf, sizeof(buf)) == 9 &&
       memcmp(buf, "rewritten", 9) == 0;

  fd = open("sl-link", O_RDONLY);
  if (fd < 0)
    ok = 0;
  else {
    int n = read(fd, copy_data, sizeof(copy_data));
    close(fd);
    ok = ok && write_file("sl-copy", copy_data, n) &&
         read_file("sl-copy", buf, sizeof(buf)) == 9 &&
         memcmp(buf, "rewritten", 9) == 0;
  }
  check("write, truncate, read, and copy through link", ok);
}

static void
test_unlink_and_dangling(void)
{
  int fd;
  int ok;

  ok = unlink("sl-target") == 0;
  fd = open("sl-link", O_RDONLY);
  ok = ok && fd < 0;
  if (fd >= 0)
    close(fd);
  fd = open("sl-link", O_RDONLY | O_NOFOLLOW);
  ok = ok && fd >= 0;
  if (fd >= 0)
    close(fd);
  ok = ok && symlink("sl-never-created", "sl-dangling") == 0;
  fd = open("sl-dangling", O_RDONLY);
  ok = ok && fd < 0;
  if (fd >= 0)
    close(fd);
  check("unlink target and reject dangling link", ok);
}

static void
test_loops(void)
{
  int fd;
  int ok = symlink("sl-loop-b", "sl-loop-a") == 0 &&
           symlink("sl-loop-a", "sl-loop-b") == 0;

  fd = open("sl-loop-a", O_RDONLY);
  ok = ok && fd < 0;
  if (fd >= 0)
    close(fd);
  check("reject symlink loops", ok);
}

static void
test_mmap_small_and_pages(void)
{
  int fd;
  int i;
  int ok;
  char *mapped;

  for (i = 0; i < 2 * PGSIZE; i++)
    page_data[i] = i < PGSIZE ? 'A' : 'B';
  ok = write_file("sl-pages", page_data, sizeof(page_data));
  fd = open("sl-pages", O_RDONLY);
  if (fd < 0)
    ok = 0;
  mapped = fd < 0 ? MAP_FAILED : mmap(0, 1, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED)
    ok = 0;
  else {
    ok = ok && mapped[0] == 'A';
    ok = ok && munmap(mapped, 1) == 0;
  }
  if (fd >= 0)
    close(fd);

  fd = open("sl-link", O_RDONLY);
  if (fd >= 0)
    close(fd);
  unlink("sl-link");
  ok = ok && symlink("sl-pages", "sl-link") == 0;
  fd = open("sl-link", O_RDWR);
  if (fd < 0)
    ok = 0;
  mapped = fd < 0 ? MAP_FAILED :
           mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED)
    ok = 0;
  else {
    ok = ok && mapped[0] == 'A' && mapped[PGSIZE] == 'B';
    mapped[10] = 'X';
    mapped[PGSIZE + 20] = 'Y';
    ok = ok && munmap(mapped, 2 * PGSIZE) == 0;
  }
  if (fd >= 0)
    close(fd);

  ok = ok && read_file("sl-pages", copy_data, sizeof(copy_data)) ==
           sizeof(copy_data) &&
       copy_data[10] == 'X' && copy_data[PGSIZE + 20] == 'Y';
  check("small and multi-page mmap through link", ok);
}

static void
test_private_mmap_and_zero_tail(void)
{
  int fd;
  int ok;
  char *mapped;

  unlink("sl-link");
  ok = write_file("sl-small", "small", 5) &&
       symlink("sl-small", "sl-link") == 0;
  fd = open("sl-link", O_RDONLY);
  if (fd < 0)
    ok = 0;
  mapped = fd < 0 ? MAP_FAILED :
           mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED)
    ok = 0;
  else {
    ok = ok && mapped[0] == 's' && mapped[4] == 'l' &&
         mapped[PGSIZE] == 0;
    mapped[0] = 'P';
    ok = ok && munmap(mapped, 2 * PGSIZE) == 0;
  }
  if (fd >= 0)
    close(fd);
  ok = ok && read_file("sl-small", copy_data, sizeof(copy_data)) == 5 &&
       copy_data[0] == 's';
  check("private mmap isolation and zero-filled tail", ok);
}

int
main(void)
{
  cleanup();
  test_create_and_follow();
  test_write_copy_and_truncate();
  test_unlink_and_dangling();
  test_loops();
  test_mmap_small_and_pages();
  test_private_mmap_and_zero_tail();
  cleanup();
  printf("symlinktest: %d failures\n", failures);
  exit(failures != 0);
}
