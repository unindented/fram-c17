// `TEST_NO_MAIN` tells acutest not to define `main` in this file. Each test executable defines
// `main` and the acutest run state in its own translation unit. This file has only the declarations
// of `acutest_check_` and `acutest_abort_`. The linker connects both declarations to the
// definitions in the test executable.
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
#include <fcntl.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/path.h"
#include "runtime/fs.h"
#include "shared/arena.h"

const char* init_fixture_dir(char root_dir[static 1]) {
  char* created_root_dir = mkdtemp(root_dir);
  TEST_CHECK(created_root_dir != NULL);
  if (created_root_dir == NULL) {
    return NULL;
  }
  return created_root_dir;
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
  struct Arena arena;
  arena_init(&arena);
  char* fixture_path = path_join(root_dir, relative_path, &arena);
  const int rc =
      fixture_path == NULL ? -1 : fs_write_file(fixture_path, contents, strlen(contents), NULL, 0);
  arena_free(&arena);
  return rc;
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
 * An unwritable stream still holds the text its failed writes left buffered. That text is flushed
 * into `/dev/null` instead, or the first successful flush after the restore would print it.
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

int capture_end(struct StreamCapture* capture, char* text_out, size_t text_out_len) {
  bool has_failed = capture_end_flush(capture) != 0;

  const int stream_fd = fileno(capture->stream);
  const int restore_rc = dup2(capture->saved_fd, stream_fd);
  TEST_CHECK(restore_rc == stream_fd);
  const int close_rc = close(capture->saved_fd);
  TEST_CHECK(close_rc == 0);
  clearerr(capture->stream);
  has_failed = has_failed || restore_rc != stream_fd || close_rc != 0;

  if (capture->capture != NULL) {
    has_failed = read_capture(capture->capture, text_out, text_out_len) != 0 || has_failed;
    const int fclose_rc = fclose(capture->capture);
    TEST_CHECK(fclose_rc == 0);
    has_failed = has_failed || fclose_rc != 0;
  }
  return has_failed ? -1 : 0;
}

int read_capture(FILE* capture, char* text_out, size_t text_out_len) {
  if (!TEST_CHECK(text_out_len > 0)) {
    return -1;
  }
  if (fseek(capture, 0, SEEK_SET) != 0) {
    TEST_CHECK(false);
    return -1;
  }
  const size_t text_len = fread(text_out, 1, text_out_len - 1, capture);
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
