// `TEST_NO_MAIN` tells acutest not to define `main` in this file. Each test executable defines
// `main` and the acutest run state in its own translation unit. This file has only acutest's
// declarations, such as `acutest_check_`, `acutest_message_` and `acutest_abort_`. The linker
// connects them to the definitions in the test executable.
//
// A helper here can therefore call `TEST_CHECK`. Acutest reports a failure at this file and line,
// and the test that called the helper then fails. No helper here calls `TEST_ASSERT`, because a
// test may call one while its fixture exists, and the abort would skip the test's cleanup.
#define TEST_NO_MAIN

#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "test_support.h"

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/path.h"
#include "shared/arena.h"
#include "shared/buf.h"
#include "shared/log.h"
#include "shared/str.h"

/**
 * Bytes of stack memory `write_fixture_file` builds its path in, twice over: once counted and once
 * terminated. A fixture path is a temporary root plus a short relative path, far below this.
 */
enum { FIXTURE_PATH_MEMORY_SIZE = 4096 };

/**
 * Whether this process is a child `test_is_oom` started, which expects `os_oom`. Set in the child
 * before `fn` runs, so before any thread `fn` starts, and never cleared, because the child ends
 * when `fn` returns.
 */
static bool test_oom_is_expected;

_Noreturn void os_oom(void) {
  if (test_oom_is_expected) {
    _Exit(TEST_OOM_EXIT_STATUS);
  }
  // acutest counts only status `1` as a failed test, so this fails the test that ran out.
  (void)fputs("unexpected out of memory\n", stderr);
  _Exit(1);
}

struct Arena test_arena(void) {
  struct Arena arena;
  if (arena_reserve(TEST_ARENA_LEN, &arena) != 0) {
    os_oom();
  }
  return arena;
}

struct Log test_log(void) {
  struct Arena block = test_arena();
  struct Arena* arena = arena_new(&block, 1, struct Arena);
  *arena = block;
  return (struct Log){.buf = {.arena = arena}};
}

bool test_log_is(const struct Log* log, const char* format, ...) {
  // The first pass measures the expected text and the second writes it, as `buf_printf` does.
  // Starting the list twice needs no `va_copy`, and forwarding it to `buf_vprintf` instead makes
  // Clang's analyzer report a false `va_end` on AArch64 Linux.
  va_list args;
  va_start(args, format);
  const int expected_len = vsnprintf(NULL, 0, format, args);
  va_end(args);
  if (!TEST_CHECK(expected_len >= 0)) {
    return false;
  }
  const struct Arena region = test_arena();
  struct Arena arena = region;
  char* expected = arena_new(&arena, (ptrdiff_t)expected_len + 1, char);
  va_start(args, format);
  (void)vsnprintf(expected, (size_t)expected_len + 1, format, args);
  va_end(args);
  const struct Str text = buf_str(&log->buf);
  const struct Str expected_text = {expected, expected_len};
  const bool is_equal = str_eq(text, expected_text);
  TEST_CHECK_(is_equal, "log holds the expected text");
  TEST_MSG("expected: '%.*s'", STR_FMT(expected_text));
  TEST_MSG("actual:   '%.*s'", STR_FMT(text));
  arena_release(region);
  return is_equal;
}

bool test_log_is_one(const struct Log* log, const char* message) {
  return test_log_is(log, "%s\n", message) && TEST_CHECK(log->errors == 1);
}

bool test_log_is_empty(const struct Log* log) {
  const struct Str text = buf_str(&log->buf);
  TEST_CHECK_(text.len == 0, "log is empty");
  TEST_MSG("actual: '%.*s'", STR_FMT(text));
  return text.len == 0;
}

bool test_is_oom(void (*fn)(void* userdata), void* userdata) {
  // Flush first, so output buffered before the fork is not written once by each process.
  (void)fflush(stdout);
  (void)fflush(stderr);
  const pid_t pid = fork();
  TEST_CHECK(pid >= 0);
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    test_oom_is_expected = true;
    fn(userdata);
    _Exit(0);
  }
  int status = 0;
  TEST_CHECK(waitpid(pid, &status, 0) == pid);
  return WIFEXITED(status) && WEXITSTATUS(status) == TEST_OOM_EXIT_STATUS;
}

struct TestRng test_rng(void) {
  const char* seed = getenv("FRAM_TEST_SEED");
  struct Str seed_text = {0};
  if (seed != NULL) {
    seed_text = str_from_cstr(seed);
  }
  struct TestRng rng;
  if (test_rng_seed_parse(seed_text, &rng.state) != 0) {
    // acutest counts only status `1` as a failed test, as for an unexpected `os_oom`.
    (void)fprintf(stderr,
                  "environment variable 'FRAM_TEST_SEED' must be an unsigned 64-bit decimal "
                  "integer: '%s'\n",
                  seed);
    _Exit(1);
  }
  return rng;
}

int test_rng_seed_parse(struct Str text, uint64_t* seed_out) {
  if (text.len == 0) {
    *seed_out = TEST_RNG_SEED_DEFAULT;
    return 0;
  }
  uint64_t seed = 0;
  for (ptrdiff_t i = 0; i < text.len; i++) {
    const char c = text.data[i];
    if (c < '0' || c > '9') {
      return -1;
    }
    const uint64_t digit = (uint64_t)(c - '0');
    if (seed > (UINT64_MAX - digit) / 10) {
      return -1;
    }
    seed = seed * 10 + digit;
  }
  *seed_out = seed;
  return 0;
}

uint32_t test_rng_next(struct TestRng* rng) {
  rng->state = rng->state * UINT64_C(0x3243f6a8885a308d) + 1;
  return (uint32_t)(rng->state >> 32);
}

ptrdiff_t test_rng_below(struct TestRng* rng, ptrdiff_t bound) {
  return (ptrdiff_t)(((uint64_t)test_rng_next(rng) * (uint64_t)bound) >> 32);
}

int init_fixture_dir(char root_dir[static 1]) {
  return TEST_CHECK(mkdtemp(root_dir) != NULL) ? 0 : -1;
}

/**
 * @brief Removes one entry a fixture-tree walk visits.
 *
 * @param path      Path of the visited entry.
 * @param st        Stat buffer `nftw` filled. Unused.
 * @param type_flag Entry type `nftw` determined.
 * @param ftw       Traversal state `nftw` maintains. Unused.
 * @return `0` to continue the walk.
 */
static int remove_fixture_tree_entry(const char* path,
                                     const struct stat* st,
                                     int type_flag,
                                     struct FTW* ftw) {
  (void)st;
  (void)ftw;
  TEST_CHECK((type_flag == FTW_DP ? rmdir(path) : unlink(path)) == 0);
  return 0;
}

void remove_fixture_tree(const char* root_dir) {
  enum { FIXTURE_TREE_FD_MAX = 8 };
  TEST_CHECK(nftw(root_dir, remove_fixture_tree_entry, FIXTURE_TREE_FD_MAX, FTW_DEPTH | FTW_PHYS) ==
             0);
}

int write_fixture_file(const char* root_dir, const char* relative_path, const char* contents) {
  // This writes through the system directly rather than through `os_write`, because every unit-test
  // executable links this file, and one that links the in-memory host has no `os_posix` and an
  // `os_write` that leaves no file on disk.
  _Alignas(max_align_t) char memory[FIXTURE_PATH_MEMORY_SIZE];
  struct Arena arena = {.beg = memory, .end = memory + sizeof(memory)};
  const struct Str fixture_path =
      path_join(str_from_cstr(root_dir), str_from_cstr(relative_path), &arena);
  char* path = str_cstr(fixture_path, &arena);
  for (char* slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
    *slash = '\0';
    const int mkdir_rc = mkdir(path, 0775);
    const int mkdir_errno = errno;
    if (!TEST_CHECK_(mkdir_rc == 0 || mkdir_errno == EEXIST, "fixture '%s' has its directories",
                     relative_path)) {
      TEST_MSG("cannot create directory: %s ('%s')", strerror(mkdir_errno), path);
      return -1;
    }
    *slash = '/';
  }
  // ISO C does not require `fwrite` to set `errno`, so it is cleared first, and a failure that left
  // it at `0` still names the file.
  errno = 0;
  FILE* fp = fopen(path, "wb");
  const size_t len = strlen(contents);
  const bool is_written = fp != NULL && fwrite(contents, 1, len, fp) == len;
  const int write_errno = errno;
  const bool is_closed = fp != NULL && fclose(fp) == 0;
  if (!TEST_CHECK_(is_written && is_closed, "fixture '%s' is written", relative_path)) {
    TEST_MSG("cannot write: %s ('%s')", strerror(is_written ? errno : write_errno), path);
    return -1;
  }
  return 0;
}

int working_dir_enter(const char* dir, int* saved_dir_fd_out) {
  const int saved_dir_fd = open(".", O_RDONLY | O_DIRECTORY);
  TEST_CHECK(saved_dir_fd >= 0);
  if (saved_dir_fd < 0) {
    return -1;
  }
  const int chdir_rc = chdir(dir);
  TEST_CHECK(chdir_rc == 0);
  if (chdir_rc != 0) {
    TEST_CHECK(close(saved_dir_fd) == 0);
    return -1;
  }
  *saved_dir_fd_out = saved_dir_fd;
  return 0;
}

int working_dir_leave(int saved_dir_fd) {
  const int fchdir_rc = fchdir(saved_dir_fd);
  TEST_CHECK(fchdir_rc == 0);
  const int close_rc = close(saved_dir_fd);
  TEST_CHECK(close_rc == 0);
  return fchdir_rc == 0 && close_rc == 0 ? 0 : -1;
}

char* save_env(const char* name, struct Arena* arena) {
  const char* value = getenv(name);
  return value == NULL ? NULL : str_cstr(str_from_cstr(value), arena);
}

void restore_env(const char* name, const char* saved) {
  if (saved == NULL) {
    (void)unsetenv(name);
  } else {
    (void)setenv(name, saved, 1);
  }
}

/**
 * @brief Points a flushed standard stream's descriptor at another descriptor.
 *
 * Leaves `capture_out->capture` `NULL` for the caller to set.
 *
 * @param stream      Stream to redirect. Must not be `NULL`.
 * @param target_fd   Descriptor the stream writes to until `capture_end`.
 * @param capture_out Receives the redirection state on success. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int capture_begin_redirect(FILE* stream, int target_fd, struct StreamCapture* capture_out) {
  const int stream_fd = fileno(stream);
  const int saved_fd = dup(stream_fd);
  TEST_CHECK(saved_fd >= 0);
  if (saved_fd < 0) {
    return -1;
  }
  const int redirect_rc = dup2(target_fd, stream_fd);
  TEST_CHECK(redirect_rc == stream_fd);
  if (redirect_rc != stream_fd) {
    TEST_CHECK(close(saved_fd) == 0);
    return -1;
  }
  capture_out->stream = stream;
  capture_out->saved_fd = saved_fd;
  capture_out->capture = NULL;
  return 0;
}

/**
 * @brief Flushes a standard stream before its descriptor is redirected.
 *
 * @param stream Stream to flush. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int capture_begin_flush(FILE* stream) {
  const int flush_rc = fflush(stream);
  TEST_CHECK(flush_rc == 0);
  if (flush_rc != 0) {
    clearerr(stream);
    return -1;
  }
  return 0;
}

int capture_begin(FILE* stream, struct StreamCapture* capture_out) {
  if (capture_begin_flush(stream) != 0) {
    return -1;
  }
  FILE* capture = tmpfile();
  TEST_CHECK(capture != NULL);
  if (capture == NULL) {
    return -1;
  }
  if (capture_begin_redirect(stream, fileno(capture), capture_out) != 0) {
    const int close_rc = fclose(capture);
    TEST_CHECK(close_rc == 0);
    return -1;
  }
  capture_out->capture = capture;
  return 0;
}

int capture_begin_unwritable(FILE* stream, struct StreamCapture* capture_out) {
  if (capture_begin_flush(stream) != 0) {
    return -1;
  }
  const int unwritable_fd = open("/dev/null", O_RDONLY);
  TEST_CHECK(unwritable_fd >= 0);
  if (unwritable_fd < 0) {
    return -1;
  }
  // The stream's descriptor is now a duplicate, so this one is no longer needed either way.
  const int rc = capture_begin_redirect(stream, unwritable_fd, capture_out);
  TEST_CHECK(close(unwritable_fd) == 0);
  return rc;
}

/**
 * @brief Flushes what a redirected stream holds into the descriptor it writes to.
 *
 * An unwritable stream still holds the text its failed stdio writes left buffered. That text is
 * flushed into `/dev/null` instead, or the first successful flush after the restore would print it.
 *
 * @param capture State `capture_begin` or `capture_begin_unwritable` wrote. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int capture_end_flush(const struct StreamCapture* capture) {
  if (capture->capture == NULL) {
    const int sink_fd = open("/dev/null", O_WRONLY);
    TEST_CHECK(sink_fd >= 0);
    if (sink_fd < 0) {
      return -1;
    }
    const int stream_fd = fileno(capture->stream);
    const int redirect_rc = dup2(sink_fd, stream_fd);
    TEST_CHECK(redirect_rc == stream_fd);
    const int close_rc = close(sink_fd);
    TEST_CHECK(close_rc == 0);
    if (redirect_rc != stream_fd || close_rc != 0) {
      return -1;
    }
    clearerr(capture->stream);
  }
  const int flush_rc = fflush(capture->stream);
  TEST_CHECK(flush_rc == 0);
  return flush_rc == 0 ? 0 : -1;
}

/**
 * @brief Reads what an anonymous capture file received.
 *
 * @param capture      File to rewind and read. Must not be `NULL`.
 * @param text_out     Buffer that receives the terminated captured text. Must not be `NULL`.
 * @param text_out_len Size of `text_out` in bytes. Must be positive.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int capture_end_read(FILE* capture, char* text_out, ptrdiff_t text_out_len) {
  if (!TEST_CHECK(text_out_len > 0)) {
    return -1;
  }
  if (fseek(capture, 0, SEEK_SET) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  const ptrdiff_t text_len = (ptrdiff_t)fread(text_out, 1, (size_t)(text_out_len - 1), capture);
  text_out[text_len] = '\0';
  if (text_len == text_out_len - 1) {
    const int trailing = fgetc(capture);
    TEST_CHECK(trailing == EOF);
    if (trailing != EOF) {
      return -1;
    }
  }
  if (ferror(capture) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  return 0;
}

int capture_end(struct StreamCapture* capture, char* text_out, ptrdiff_t text_out_len) {
  bool has_failed = capture_end_flush(capture) != 0;

  const int stream_fd = fileno(capture->stream);
  const int restore_rc = dup2(capture->saved_fd, stream_fd);
  TEST_CHECK(restore_rc == stream_fd);
  const int close_rc = close(capture->saved_fd);
  TEST_CHECK(close_rc == 0);
  clearerr(capture->stream);
  has_failed = has_failed || restore_rc != stream_fd || close_rc != 0;

  if (capture->capture != NULL) {
    has_failed = capture_end_read(capture->capture, text_out, text_out_len) != 0 || has_failed;
    const int fclose_rc = fclose(capture->capture);
    TEST_CHECK(fclose_rc == 0);
    has_failed = has_failed || fclose_rc != 0;
  }
  return has_failed ? -1 : 0;
}

struct Str str_from_bytes(const unsigned char* bytes, ptrdiff_t bytes_len) {
  return (struct Str){(const char*)bytes, bytes_len};
}
