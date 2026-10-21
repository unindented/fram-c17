#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "runtime/fs.h"
#include "runtime/os.h"
#include "shared/arena.h"
#include "shared/log.h"
#include "shared/str.h"
#include "test_support.h"

/**
 * @brief Reports whether a descriptor refers to the same open file it did when `before` was filled.
 *
 * @param fd     Descriptor to inspect.
 * @param before Stat buffer filled from `fd` before a redirection.
 * @return `true` when `fd` names the same device and inode as `before`, or `false` otherwise.
 */
static bool is_same_file(int fd, const struct stat* before) {
  struct stat after;
  return fstat(fd, &after) == 0 && after.st_dev == before->st_dev && after.st_ino == before->st_ino;
}

/**
 * @brief Runs out of memory. Run through `test_is_oom`.
 *
 * @param userdata Unused.
 */
static void run_out_of_memory(void* userdata) {
  (void)userdata;
  os_oom();
}

/**
 * @brief Returns without running out of memory. Run through `test_is_oom`.
 *
 * @param userdata Unused.
 */
static void return_normally(void* userdata) {
  (void)userdata;
}

// `test_is_oom` reports a child that ends in `os_oom`, and not one that returns.
static void test_detects_only_an_out_of_memory_exit(void) {
  TEST_CHECK(test_is_oom(run_out_of_memory, NULL));
  TEST_CHECK(!test_is_oom(return_normally, NULL));
}

// An `os_oom` that no `test_is_oom` expects exits with status `1`, the one acutest counts as a
// failed test.
static void test_unexpected_oom_exits_with_failure_status(void) {
  (void)fflush(stdout);
  (void)fflush(stderr);
  const pid_t pid = fork();
  TEST_ASSERT(pid >= 0);
  if (pid == 0) {
    // The child's report would read as a failure of this test, so it goes nowhere.
    const int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      (void)dup2(null_fd, STDERR_FILENO);
    }
    os_oom();
  }
  int status = 0;
  TEST_CHECK(waitpid(pid, &status, 0) == pid);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

// `test_rng` starts from `TEST_RNG_SEED_DEFAULT` when `FRAM_TEST_SEED` is unset or empty, and from
// the decimal seed it names otherwise, so a seed a failing check printed replays the failure.
static void test_rng_starts_from_the_named_seed(void) {
  TEST_ASSERT(unsetenv("FRAM_TEST_SEED") == 0);
  TEST_CHECK(test_rng().state == TEST_RNG_SEED_DEFAULT);
  TEST_ASSERT(setenv("FRAM_TEST_SEED", "18446744073709551615", 1) == 0);
  TEST_CHECK(test_rng().state == UINT64_MAX);
  TEST_ASSERT(setenv("FRAM_TEST_SEED", "", 1) == 0);
  TEST_CHECK(test_rng().state == TEST_RNG_SEED_DEFAULT);
  TEST_CHECK(unsetenv("FRAM_TEST_SEED") == 0);
}

// A `FRAM_TEST_SEED` that `test_rng_seed_parse` rejects ends the process with status `1`, the one
// acutest counts as a failed test, after one line naming the variable and the value. The child sets
// the variable, so the rest of the suite never sees it.
static void test_rng_exits_on_a_malformed_seed(void) {
  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stderr, &capture) == 0);
  (void)fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    if (setenv("FRAM_TEST_SEED", "abc", 1) == 0) {
      (void)test_rng();
    }
    _Exit(0);
  }
  int status = 0;
  const bool is_waited = pid >= 0 && waitpid(pid, &status, 0) == pid;
  char text[128];
  const int capture_rc = capture_end(&capture, text, sizeof(text));

  TEST_CHECK(is_waited);
  TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1);
  // `capture_end` writes `text` only when it succeeds.
  if (TEST_CHECK(capture_rc == 0)) {
    TEST_CHECK(strcmp(text,
                      "environment variable 'FRAM_TEST_SEED' must be an unsigned 64-bit decimal "
                      "integer: 'abc'\n") == 0);
  }
}

// `test_rng_seed_parse` reads a whole unsigned decimal `uint64_t`, and gives the default for an
// unset or empty value.
static void test_rng_seed_parse_reads_a_decimal_seed(void) {
  uint64_t seed = 1;
  TEST_CHECK(test_rng_seed_parse((struct Str){0}, &seed) == 0 && seed == TEST_RNG_SEED_DEFAULT);
  TEST_CHECK(test_rng_seed_parse(S(""), &seed) == 0 && seed == TEST_RNG_SEED_DEFAULT);
  TEST_CHECK(test_rng_seed_parse(S("0"), &seed) == 0 && seed == 0);
  TEST_CHECK(test_rng_seed_parse(S("0042"), &seed) == 0 && seed == 42);
  TEST_CHECK(test_rng_seed_parse(S("18446744073709551615"), &seed) == 0 && seed == UINT64_MAX);
}

// `test_rng_seed_parse` rejects a sign, a prefix, whitespace, trailing text and a value past
// `UINT64_MAX`, and leaves the seed untouched.
static void test_rng_seed_parse_rejects_a_malformed_seed(void) {
  static const struct Str malformed[] = {
      STR_INIT("-1"),
      STR_INIT("+1"),
      STR_INIT("0x1f"),
      STR_INIT(" 1"),
      STR_INIT("1 "),
      STR_INIT("12abc"),
      STR_INIT("abc"),
      STR_INIT("18446744073709551616"),
      STR_INIT("99999999999999999999"),
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(malformed) / sizeof(malformed[0])); i++) {
    uint64_t seed = 1;
    TEST_CHECK_(test_rng_seed_parse(malformed[i], &seed) == -1 && seed == 1, "'%.*s' is rejected",
                STR_FMT(malformed[i]));
  }
}

// Two generators from one seed draw the same values, and the step is the pinned one: from state
// `0`, the second value is the high half of its multiplier.
static void test_rng_repeats_its_sequence(void) {
  struct TestRng zero = {0};
  TEST_CHECK(test_rng_next(&zero) == 0);
  TEST_CHECK(test_rng_next(&zero) == UINT32_C(0x3243f6a8));
  struct TestRng first = {TEST_RNG_SEED_DEFAULT};
  struct TestRng second = first;
  for (ptrdiff_t i = 0; i < 64; i++) {
    TEST_CHECK(test_rng_next(&first) == test_rng_next(&second));
  }
}

// `test_rng_below` stays below its bound and, over many draws, reaches every value below it.
static void test_rng_below_covers_its_range(void) {
  enum { BOUND = 7, DRAW_COUNT = 1000 };
  struct TestRng rng = {TEST_RNG_SEED_DEFAULT};
  bool is_drawn[BOUND] = {false};
  for (ptrdiff_t i = 0; i < DRAW_COUNT; i++) {
    const ptrdiff_t value = test_rng_below(&rng, BOUND);
    if (!TEST_CHECK(0 <= value && value < BOUND)) {
      return;
    }
    is_drawn[value] = true;
  }
  for (ptrdiff_t i = 0; i < BOUND; i++) {
    TEST_CHECK_(is_drawn[i], "%td is drawn", i);
  }
  TEST_CHECK(test_rng_below(&rng, 1) == 0);
  const ptrdiff_t value = test_rng_below(&rng, UINT32_MAX);
  TEST_CHECK(0 <= value && value < (ptrdiff_t)UINT32_MAX);
}

// A fixture file lands below the root with its parent directories created and its text intact, and
// removing the tree deletes the root and everything below it.
static void test_fixture_tree_write_and_remove(void) {
  enum { FIXTURE_LEN_MAX = 64 };
  char root_dir[] = "/tmp/fram-test-support.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  const int write_rc = write_fixture_file(root_dir, "nested/deeper/file.txt", "fixture text");
  struct Arena arena = test_arena();
  const struct Str fixture_path =
      str_concat(str_from_cstr(root_dir), S("/nested/deeper/file.txt"), &arena);
  struct Log log = test_log();
  struct Str text = {0};
  const int read_rc = os_read(os_posix(), fixture_path, FIXTURE_LEN_MAX, &arena, &text,
                              S("failed to read fixture"), &log);
  // The tree is removed before any assertion, so a failed one cannot leave it behind.
  remove_fixture_tree(root_dir);
  errno = 0;
  const int access_rc = access(root_dir, F_OK);
  const int access_errno = errno;

  TEST_CHECK(write_rc == 0);
  TEST_CHECK(read_rc == 0 && test_log_is_empty(&log));
  TEST_CHECK(str_eq(text, S("fixture text")));
  TEST_CHECK(access_rc == -1 && access_errno == ENOENT);
}

// Entering a directory makes relative paths resolve below it, and leaving restores the working
// directory the test started in.
static void test_working_dir_enter_and_leave(void) {
  char root_dir[] = "/tmp/fram-test-support.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }
  const int write_rc = write_fixture_file(root_dir, "marker", "");
  char working_dir_before[PATH_MAX];
  const bool has_dir_before = getcwd(working_dir_before, sizeof(working_dir_before)) != NULL;

  int saved_dir_fd = -1;
  const int enter_rc = working_dir_enter(root_dir, &saved_dir_fd);
  int access_rc = -1;
  int leave_rc = -1;
  if (enter_rc == 0) {
    access_rc = access("marker", F_OK);
    leave_rc = working_dir_leave(saved_dir_fd);
  }
  char working_dir_after[PATH_MAX];
  const bool has_dir_after = getcwd(working_dir_after, sizeof(working_dir_after)) != NULL;
  // The tree is removed before any assertion, so a failed one cannot leave it behind.
  remove_fixture_tree(root_dir);

  TEST_CHECK(write_rc == 0);
  TEST_CHECK(enter_rc == 0);
  TEST_CHECK(access_rc == 0);
  TEST_CHECK(leave_rc == 0);
  TEST_CHECK(has_dir_before && has_dir_after && strcmp(working_dir_after, working_dir_before) == 0);
}

// A capture receives what the stream writes while redirected, and the stream writes to its original
// descriptor again once the capture ends.
static void test_capture_reads_stream_text_and_restores(void) {
  struct stat stderr_before;
  TEST_ASSERT(fstat(STDERR_FILENO, &stderr_before) == 0);

  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stderr, &capture) == 0);
  TEST_CHECK(fputs("captured\n", stderr) >= 0);
  char text[64];
  if (TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0)) {
    TEST_CHECK(strcmp(text, "captured\n") == 0);
  }
  TEST_CHECK(is_same_file(STDERR_FILENO, &stderr_before));
}

// A stream buffered rather than flushed at the time the capture ends is still captured whole, since
// `capture_end` flushes before restoring. `stdout` is fully buffered when redirected to a file.
static void test_capture_flushes_buffered_text(void) {
  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stdout, &capture) == 0);
  TEST_CHECK(fputs("buffered", stdout) >= 0);
  char text[64];
  if (TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0)) {
    TEST_CHECK(strcmp(text, "buffered") == 0);
  }
}

// A capture that ends with nothing written reads an empty string.
static void test_capture_reads_empty_text(void) {
  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stderr, &capture) == 0);
  char text[8] = "stale";
  TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0);

  TEST_CHECK(text[0] == '\0');
}

// An unwritable stream fails its flush with `EBADF`, and the text the failure left buffered is
// discarded rather than reaching the restored descriptor. The unwritable capture runs inside an
// ordinary one, so any text that leaked through would land in the outer capture.
static void test_unwritable_capture_fails_writes_and_discards_them(void) {
  struct stat stdout_before;
  TEST_ASSERT(fstat(STDOUT_FILENO, &stdout_before) == 0);

  struct StreamCapture outer;
  TEST_ASSERT(capture_begin(stdout, &outer) == 0);
  struct StreamCapture unwritable;
  TEST_CHECK(capture_begin_unwritable(stdout, &unwritable) == 0);
  TEST_CHECK(fputs("lost", stdout) >= 0);
  errno = 0;
  TEST_CHECK(fflush(stdout) == EOF);
  TEST_CHECK(errno == EBADF);
  TEST_CHECK(capture_end(&unwritable, NULL, 0) == 0);
  TEST_CHECK(ferror(stdout) == 0);
  char text[64];
  TEST_CHECK(capture_end(&outer, text, sizeof(text)) == 0);

  TEST_CHECK(text[0] == '\0');
  TEST_CHECK(is_same_file(STDOUT_FILENO, &stdout_before));
}

TEST_LIST = {
    {"detects only an out of memory exit", test_detects_only_an_out_of_memory_exit},
    {"unexpected oom exits with failure status", test_unexpected_oom_exits_with_failure_status},
    {"rng starts from the named seed", test_rng_starts_from_the_named_seed},
    {"rng exits on a malformed seed", test_rng_exits_on_a_malformed_seed},
    {"rng seed parse reads a decimal seed", test_rng_seed_parse_reads_a_decimal_seed},
    {"rng seed parse rejects a malformed seed", test_rng_seed_parse_rejects_a_malformed_seed},
    {"rng repeats its sequence", test_rng_repeats_its_sequence},
    {"rng below covers its range", test_rng_below_covers_its_range},
    {"fixture tree write and remove", test_fixture_tree_write_and_remove},
    {"working dir enter and leave", test_working_dir_enter_and_leave},
    {"capture reads stream text and restores", test_capture_reads_stream_text_and_restores},
    {"capture flushes buffered text", test_capture_flushes_buffered_text},
    {"capture reads empty text", test_capture_reads_empty_text},
    {"unwritable capture fails writes and discards them",
     test_unwritable_capture_fails_writes_and_discards_them},
    {NULL, NULL},
};
