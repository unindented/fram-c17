#include <acutest.h>
#include <string.h>

#include "core/error.h"

// Only two of `error`'s three exported functions are tested directly here, which reads as a
// coverage hole and is not one. `record_error` in `src/app/cli.c` calls `error_report_va`, and
// `src/app/test_cli.c` exercises it through `cli_parse`. `expected_errno_reason` in
// `src/runtime/test_fs.c` derives its expectation from `strerror`, so every `fs` reason assertion
// also pins `error_system_message`'s success branch against the running libc, for `ENOENT`,
// `ENOTDIR`, `EACCES` and `EISDIR`. The `strerror_r`-failed branch is reachable only from an
// `errno` the system cannot describe, so this file pins it below.

// A reported error formats the message and returns the conventional failure value.
static void test_report_error_formats_and_returns_failure(void) {
  char err[64];
  TEST_CHECK(error_report(err, sizeof(err), "failed to read config: %s ('%s')", "no such file",
                          "fram.toml") == -1);
  TEST_CHECK(strcmp(err, "failed to read config: no such file ('fram.toml')") == 0);
}

// A message too long for its buffer keeps its leading reason and ends in `...`, so a value cut by
// the buffer cannot be read as a whole one. Several diagnostics truncate by construction. A
// limit-exceeded message naming an over-long value can never fit that value. The marker is what
// separates those from a message that happened to fit exactly.
static void test_report_error_marks_truncated_message(void) {
  char err[32];
  TEST_CHECK(error_report(err, sizeof(err), "failed to write output: '%s'",
                          "a-path-far-longer-than-the-buffer-allows") == -1);
  TEST_CHECK(strlen(err) == sizeof(err) - 1);
  static const char head[] = "failed to write output: '";
  TEST_CHECK(strncmp(err, head, sizeof(head) - 1) == 0);
  TEST_CHECK(strcmp(err + sizeof(err) - 4, "...") == 0);
}

// A message that fits exactly, filling the buffer with no room to spare, is left unmarked. This is
// the boundary the truncation check has to get right: `vsnprintf` returning exactly the buffer's
// capacity means one byte too many, while one less means a perfect fit.
static void test_report_error_leaves_exact_fit_unmarked(void) {
  // 24 characters plus the terminator.
  char err[25];
  TEST_CHECK(error_report(err, sizeof(err), "%s", "failed to write output::") == -1);
  TEST_CHECK(strcmp(err, "failed to write output::") == 0);
}

// One byte too many is marked, which is the other side of that boundary and the case that separates
// `>=` from `>`. Without it a weakened check hands back `failed to write output::` for a message
// that really lost its last byte, byte-identical to the exact fit asserted above. The 25-character
// argument is one byte too many for the 25-byte buffer, so the pair reads as the two sides of one
// comparison.
static void test_report_error_marks_one_byte_over(void) {
  char err[25];
  TEST_CHECK(error_report(err, sizeof(err), "%s", "failed to write output:::") == -1);
  TEST_CHECK(strcmp(err, "failed to write outpu...") == 0);
  TEST_CHECK(strlen(err) == sizeof(err) - 1);
}

// The function writes the marker only when there is room for it, so a buffer smaller than `...`
// truncates silently rather than overrunning to say it truncated. This is the other arm of the
// guard that `test_report_error_accepts_null_buffer` reaches with `NULL`. Below
// `sizeof(TRUNCATION_MARKER)` there is nowhere to put the marker. At exactly that size the marker
// displaces the message entirely. Each case fills a wider buffer with a sentinel and checks that
// every byte from `err_len` onward survives. The sentinel proves those later bytes survived. A
// caller with a short buffer cannot check that itself.
static void test_report_error_marks_only_when_marker_fits(void) {
  static const struct {
    size_t err_len;
    const char* expected;
  } cases[] = {
      {1, ""},  // room for the terminator and nothing else
      {2, "f"},
      {3, "fa"},   // one byte short of the marker
      {4, "..."},  // exactly the marker, so no message survives
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char err[8];
    memset(err, '#', sizeof(err));
    TEST_CHECK(error_report(err, cases[i].err_len, "failed to write output: '%s'", "a-long-path") ==
               -1);
    TEST_CHECK(strcmp(err, cases[i].expected) == 0);
    for (size_t j = cases[i].err_len; j < sizeof(err); j++) {
      TEST_CHECK(err[j] == '#');
    }
  }
}

// `error_report` accepts a `NULL` buffer with a zero length and writes nothing. That is what call
// sites that want no message pass. Every `fs_write_file(..., NULL, 0)` unit-test fixture write
// reaches this on a failure. `vsnprintf` tolerates the pair by contract. What needs pinning is the
// truncation marker: the `err_len >= sizeof(TRUNCATION_MARKER)` guard is the only thing between an
// over-long message and a `memcpy` through a null pointer. Nothing here can observe that directly.
// The assertion is the return value, and `-fsanitize=undefined` in the debug build catches a
// regression by trapping the null-pointer arithmetic before the write happens.
static void test_report_error_accepts_null_buffer(void) {
  TEST_CHECK(error_report(NULL, 0, "failed to write output: '%s'",
                          "a-path-far-longer-than-any-buffer") == -1);
}

// An `errno` the system cannot describe still yields a terminated diagnostic naming the number,
// rather than whatever `strerror_r` left in the buffer on its failing call. `999999` is out of
// range on every target. The expected text came from a run.
static void test_system_message_falls_back_for_unknown_errno(void) {
  char message[64];
  TEST_CHECK(
      strcmp(error_system_message(message, sizeof(message), 999999), "system error 999999") == 0);
}

TEST_LIST = {
    {"report error formats and returns failure", test_report_error_formats_and_returns_failure},
    {"report error marks truncated message", test_report_error_marks_truncated_message},
    {"report error leaves exact fit unmarked", test_report_error_leaves_exact_fit_unmarked},
    {"report error marks one byte over", test_report_error_marks_one_byte_over},
    {"report error marks only when marker fits", test_report_error_marks_only_when_marker_fits},
    {"report error accepts null buffer", test_report_error_accepts_null_buffer},
    {"system message falls back for unknown errno",
     test_system_message_falls_back_for_unknown_errno},
    {NULL, NULL},
};
