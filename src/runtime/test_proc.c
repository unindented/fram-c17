#include <acutest.h>
#include <string.h>

#include "core/error.h"
#include "runtime/proc.h"

// Arguments are passed directly without shell interpretation, so metacharacters remain literal.
static void test_run_captures_literal_argument(void) {
  const char* const argv[] = {"/bin/echo", "name with spaces;$(false)", NULL};
  char output[128];
  TEST_CHECK(proc_run(argv, output, sizeof(output), NULL, 0) == 0);
  TEST_CHECK(strcmp(output, "name with spaces;$(false)\n") == 0);
}

// A nonzero exit reports both its status and the child's standard-error tail.
static void test_run_reports_failed_exit(void) {
  const char* const argv[] = {"/bin/sh", "-c", "printf 'specific failure' >&2; exit 7", NULL};
  char reason[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(proc_run(argv, NULL, 0, reason, sizeof(reason)) == -1);
  TEST_CHECK(strstr(reason, "status 7") != NULL);
  TEST_CHECK(strstr(reason, "specific failure") != NULL);
}

// Standard error longer than the reason keeps its newest bytes, so the child's final line, which
// usually names the cause, survives and the dropped front is marked.
static void test_run_reports_newest_stderr_when_it_overflows(void) {
  const char* const argv[] = {"/bin/sh", "-c",
                              "i=0; while [ $i -lt 40 ]; do echo \"warning: line $i\" >&2; "
                              "i=$((i+1)); done; echo 'FATAL: the actual cause' >&2; exit 1",
                              NULL};
  char reason[64] = "";
  TEST_CHECK(proc_run(argv, NULL, 0, reason, sizeof(reason)) == -1);
  const char* expected = "process exited with status 1: ...ine 39\nFATAL: the actual cause";
  TEST_CHECK(strcmp(reason, expected) == 0);
  TEST_MSG("reason: '%s'", reason);
}

// A missing executable receives the actionable program-not-found diagnostic rather than a raw
// `posix_spawn` error number.
static void test_run_reports_missing_program(void) {
  const char* const argv[] = {"fram-definitely-not-a-program", NULL};
  char reason[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(proc_run(argv, NULL, 0, reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(reason, "program not found ('fram-definitely-not-a-program')") == 0);
}

// More than one pipe capacity is written to both streams. Reading one stream to EOF before the
// other would deadlock here; polling and draining them together completes and retains all stdout.
static void test_run_drains_stdout_and_stderr_together(void) {
  const char* const argv[] = {
      "/bin/sh", "-c", "i=0; while [ $i -lt 40000 ]; do printf o; printf e >&2; i=$((i+1)); done",
      NULL};
  static char output[40001];
  char reason[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(proc_run(argv, output, sizeof(output), reason, sizeof(reason)) == 0);
  TEST_CHECK(strlen(output) == 40000);
  TEST_CHECK(output[0] == 'o' && output[39999] == 'o');
}

// Successful output that does not fit is rejected after the pipes are fully drained, and the
// retained prefix is still terminated for diagnostics.
static void test_run_rejects_truncated_output(void) {
  const char* const argv[] = {"/bin/echo", "abcdef", NULL};
  char output[4];
  char reason[ERROR_MESSAGE_SIZE] = "";
  TEST_CHECK(proc_run(argv, output, sizeof(output), reason, sizeof(reason)) == -1);
  TEST_CHECK(strcmp(output, "abc") == 0);
  TEST_CHECK(strstr(reason, "exceeds 3-byte buffer") != NULL);
}

TEST_LIST = {
    {"run captures literal argument", test_run_captures_literal_argument},
    {"run reports failed exit", test_run_reports_failed_exit},
    {"run reports newest stderr when it overflows",
     test_run_reports_newest_stderr_when_it_overflows},
    {"run reports missing program", test_run_reports_missing_program},
    {"run drains stdout and stderr together", test_run_drains_stdout_and_stderr_together},
    {"run rejects truncated output", test_run_rejects_truncated_output},
    {NULL, NULL},
};
