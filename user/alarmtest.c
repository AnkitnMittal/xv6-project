#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

volatile int alarm_count;
volatile int handler_running;
volatile int nested_alarm;
int failures;

void
print_handler(void)
{
  alarm_count++;
  printf("alarm!\n");
  sigreturn();
}

void
count_handler(void)
{
  alarm_count++;
  sigreturn();
}

void
slow_handler(void)
{
  if (handler_running)
    nested_alarm = 1;

  handler_running = 1;
  alarm_count++;

  for (volatile uint64 i = 0; i < 50000000; i++)
    ;

  handler_running = 0;
  sigreturn();
}

/* Test 0: Check whether an alarm is delivered */
void
test0(void)
{
  printf("test0: basic alarm\n");
  alarm_count = 0;

  sigalarm(2, print_handler);

  for (volatile uint64 i = 0; i < 100000000 && alarm_count == 0; i++)
    ;

  sigalarm(0, 0);

  if (alarm_count > 0)
    printf("test0 passed\n");
  else {
    printf("test0 failed\n");
    failures++;
  }
}

/* Test 1: Check repeated alarm delivery */
void
test1(void)
{
  printf("test1: repeated alarms\n");
  alarm_count = 0;

  sigalarm(2, count_handler);

  for (volatile uint64 i = 0; i < 300000000 && alarm_count < 5; i++)
    ;

  sigalarm(0, 0);

  if (alarm_count >= 5)
    printf("test1 passed\n");
  else {
    printf("test1 failed: alarms=%d\n", alarm_count);
    failures++;
  }
}

/* Test 2: Check that handlers are not nested */
void
test2(void)
{
  printf("test2: nested alarm protection\n");
  alarm_count = 0;
  handler_running = 0;
  nested_alarm = 0;

  sigalarm(1, slow_handler);

  for (volatile uint64 i = 0; i < 100000000 && alarm_count == 0; i++)
    ;

  sigalarm(0, 0);

  if (alarm_count > 0 && !nested_alarm && !handler_running)
    printf("test2 passed\n");
  else {
    printf("test2 failed\n");
    failures++;
  }
}

/* Test 3: Check that alarms stop after disabling them */
void
test3(void)
{
  printf("test3: disable alarm\n");
  alarm_count = 0;

  sigalarm(2, count_handler);

  for (volatile uint64 i = 0; i < 150000000 && alarm_count < 2; i++)
    ;

  sigalarm(0, 0);
  int count_after_disable = alarm_count;

  for (volatile uint64 i = 0; i < 50000000; i++)
    ;

  if (count_after_disable >= 2 && alarm_count == count_after_disable)
    printf("test3 passed\n");
  else {
    printf("test3 failed: before=%d, after=%d\n", count_after_disable,
           alarm_count);
    failures++;
  }
}

int
main(void)
{
  test0();
  test1();
  test2();
  test3();

  if (failures == 0)
    printf("alarmtest: all tests passed\n");
  else
    printf("alarmtest: %d test(s) failed\n", failures);

  exit(failures == 0 ? 0 : 1);
}
