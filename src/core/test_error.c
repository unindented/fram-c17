#include <acutest.h>
#include <string.h>

#include "core/error.h"

// The `os` contract tests in `src/runtime/test_os.c` and the POSIX tests in `src/runtime/test_fs.c`
// derive their expectations from `strerror`, so their failure assertions also pin
// `error_system_message`'s success branch against the running libc for every `errno` they assert.
// The `strerror_r`-failed branch is reachable only from an `errno` the system cannot describe, so
// this file pins it below.

// An `errno` the system cannot describe still yields a terminated diagnostic naming the number,
// rather than whatever `strerror_r` left in the buffer on its failing call. `999999` is out of
// range on every target. The expected text came from a run.
static void test_system_message_falls_back_for_unknown_errno(void) {
  char message[64];
  TEST_CHECK(
      strcmp(error_system_message(message, sizeof(message), 999999), "system error 999999") == 0);
}

TEST_LIST = {
    {"system message falls back for unknown errno",
     test_system_message_falls_back_for_unknown_errno},
    {NULL, NULL},
};
