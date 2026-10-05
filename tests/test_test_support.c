#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

// A fixture file lands below the root with its parent directories created and its text intact, and
// removing the tree deletes the root and everything below it.
static void test_fixture_tree_write_and_remove(void) {
  char root_dir_template[] = "/tmp/fram-test-support.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
    return;
  }
  const int write_rc = write_fixture_file(root_dir, "nested/deeper/file.txt", "fixture text");
  char fixture_path[PATH_MAX];
  const int fixture_path_len =
      snprintf(fixture_path, sizeof(fixture_path), "%s/nested/deeper/file.txt", root_dir);
  const bool is_path_complete =
      fixture_path_len > 0 && (size_t)fixture_path_len < sizeof(fixture_path);
  FILE* fixture = is_path_complete ? fopen(fixture_path, "rb") : NULL;
  const bool is_opened = fixture != NULL;
  char text[64] = "";
  int read_rc = -1;
  int close_rc = -1;
  if (fixture != NULL) {
    read_rc = read_capture(fixture, text, sizeof(text));
    close_rc = fclose(fixture);
  }
  // The tree is removed before any assertion, so a failed one cannot leave it behind.
  remove_fixture_tree(root_dir);
  errno = 0;
  const int access_rc = access(root_dir, F_OK);
  const int access_errno = errno;

  TEST_CHECK(root_dir == root_dir_template);
  TEST_CHECK(write_rc == 0);
  TEST_CHECK(is_path_complete);
  TEST_CHECK(is_opened);
  TEST_CHECK(read_rc == 0);
  TEST_CHECK(close_rc == 0);
  TEST_CHECK(strcmp(text, "fixture text") == 0);
  TEST_CHECK(access_rc == -1 && access_errno == ENOENT);
}

// Entering a directory makes relative paths resolve below it, and leaving restores the working
// directory the test started in.
static void test_working_dir_enter_and_leave(void) {
  char root_dir_template[] = "/tmp/fram-test-support.XXXXXX";
  const char* root_dir = init_fixture_dir(root_dir_template);
  if (root_dir == NULL) {
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
  TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0);

  TEST_CHECK(strcmp(text, "captured\n") == 0);
  TEST_CHECK(is_same_file(STDERR_FILENO, &stderr_before));
}

// A stream buffered rather than flushed at the time the capture ends is still captured whole, since
// `capture_end` flushes before restoring. `stdout` is fully buffered when redirected to a file.
static void test_capture_flushes_buffered_text(void) {
  struct StreamCapture capture;
  TEST_ASSERT(capture_begin(stdout, &capture) == 0);
  TEST_CHECK(fputs("buffered", stdout) >= 0);
  char text[64];
  TEST_CHECK(capture_end(&capture, text, sizeof(text)) == 0);

  TEST_CHECK(strcmp(text, "buffered") == 0);
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
    {"fixture tree write and remove", test_fixture_tree_write_and_remove},
    {"working dir enter and leave", test_working_dir_enter_and_leave},
    {"capture reads stream text and restores", test_capture_reads_stream_text_and_restores},
    {"capture flushes buffered text", test_capture_flushes_buffered_text},
    {"capture reads empty text", test_capture_reads_empty_text},
    {"unwritable capture fails writes and discards them",
     test_unwritable_capture_fails_writes_and_discards_them},
    {NULL, NULL},
};
