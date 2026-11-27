#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static void
fail(char *message)
{
  printf("symlinktest: FAIL: %s\n", message);
  exit(1);
}

static void
cleanup(void)
{
  unlink("sym_target");
  unlink("sym_link");
  unlink("sym_link2");
  unlink("broken_link");
  unlink("cycle_a");
  unlink("cycle_b");
  unlink("created_target");
  unlink("create_alias");
}

static void
test_create_target(void)
{
  int fd = open("sym_target", O_CREATE | O_RDWR);
  if (fd < 0)
    fail("cannot create target file");

  if (write(fd, "hello", 5) != 5)
    fail("cannot write target file");

  close(fd);
  printf("TEST 1 PASSED: regular file created\n");
}

static void
test_create_symlink(void)
{
  if (symlink("sym_target", "sym_link") < 0)
    fail("symlink creation failed");

  printf("TEST 2 PASSED: symbolic link created\n");
}

static void
test_follow_symlink(void)
{
  char buf[128];
  struct stat st;

  int fd = open("sym_link", O_RDONLY);
  if (fd < 0)
    fail("cannot open symbolic link");

  memset(buf, 0, sizeof(buf));
  int n = read(fd, buf, sizeof(buf));
  if (n != 5 || memcmp(buf, "hello", 5) != 0)
    fail("symbolic link did not resolve correctly");

  if (fstat(fd, &st) < 0 || st.type != T_FILE)
    fail("opened inode is not the target file");

  close(fd);
  printf("TEST 3 PASSED: symbolic link followed successfully\n");
}

static void
test_nofollow(void)
{
  char buf[128];
  struct stat st;

  int fd = open("sym_link", O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    fail("cannot open symbolic link with O_NOFOLLOW");

  if (fstat(fd, &st) < 0 || st.type != T_SYMLINK)
    fail("O_NOFOLLOW did not return symlink inode");

  memset(buf, 0, sizeof(buf));
  int n = read(fd, buf, sizeof(buf));
  if (n != strlen("sym_target") + 1 || strcmp(buf, "sym_target") != 0)
    fail("incorrect symbolic link contents");

  close(fd);
  printf("TEST 4 PASSED: O_NOFOLLOW works\n");
}

static void
test_chained_symlinks(void)
{
  char buf[128];

  if (symlink("sym_link", "sym_link2") < 0)
    fail("cannot create second symbolic link");

  int fd = open("sym_link2", O_RDONLY);
  if (fd < 0)
    fail("cannot follow chained symbolic links");

  memset(buf, 0, sizeof(buf));
  int n = read(fd, buf, sizeof(buf));
  if (n != 5 || memcmp(buf, "hello", 5) != 0)
    fail("chained symbolic link returned incorrect data");

  close(fd);
  printf("TEST 5 PASSED: chained symbolic links work\n");
}

static void
test_dangling_symlink(void)
{
  if (symlink("missing_target", "broken_link") < 0)
    fail("cannot create dangling symbolic link");

  int fd = open("broken_link", O_RDONLY);
  if (fd >= 0) {
    close(fd);
    fail("opening dangling symbolic link should fail");
  }

  printf("TEST 6 PASSED: dangling symbolic link handled\n");
}

static void
test_circular_symlinks(void)
{
  if (symlink("cycle_b", "cycle_a") < 0)
    fail("cannot create cycle_a");

  if (symlink("cycle_a", "cycle_b") < 0)
    fail("cannot create cycle_b");

  int fd = open("cycle_a", O_RDONLY);
  if (fd >= 0) {
    close(fd);
    fail("circular symbolic link should fail");
  }

  printf("TEST 7 PASSED: circular links detected\n");
}

static void
test_create_through_symlink(void)
{
  char buf[128];

  if (symlink("created_target", "create_alias") < 0)
    fail("cannot create creation alias");

  int fd = open("create_alias", O_CREATE | O_RDWR);
  if (fd < 0)
    fail("cannot create target through symbolic link");

  if (write(fd, "ok", 2) != 2)
    fail("cannot write through creation alias");

  close(fd);

  fd = open("created_target", O_RDONLY);
  if (fd < 0)
    fail("created target does not exist");

  memset(buf, 0, sizeof(buf));
  int n = read(fd, buf, sizeof(buf));
  if (n != 2 || memcmp(buf, "ok", 2) != 0)
    fail("incorrect data in newly created target");

  close(fd);
  printf("TEST 8 PASSED: O_CREATE through symbolic link works\n");
}

static void
test_unlink_symlink(void)
{
  if (unlink("sym_link") < 0)
    fail("cannot unlink symbolic link");

  int fd = open("sym_target", O_RDONLY);
  if (fd < 0)
    fail("unlinking symbolic link deleted target");

  close(fd);
  printf("TEST 9 PASSED: unlink affects symbolic link only\n");
}

int
main(void)
{
  cleanup();

  test_create_target();
  test_create_symlink();
  test_follow_symlink();
  test_nofollow();
  test_chained_symlinks();
  test_dangling_symlink();
  test_circular_symlinks();
  test_create_through_symlink();
  test_unlink_symlink();

  cleanup();
  printf("\nAll symbolic-link tests passed!\n");
  exit(0);
}