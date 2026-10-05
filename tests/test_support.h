#ifndef FRAM_TEST_SUPPORT_H
#define FRAM_TEST_SUPPORT_H

#include <stddef.h>
#include <stdio.h>

/**
 * Result a test wrapper returns when its own setup or teardown fails rather than the call it wraps.
 * It is none of the `0`, `1` and `2` exit codes and neither the `0` nor the `-1` an action returns,
 * so a plumbing failure cannot pass for a result of the call under test.
 */
enum { TEST_PLUMBING_FAILED = 255 };

/**
 * @brief Creates a temporary fixture root.
 *
 * The returned pointer aliases the caller's writable template and must not be freed.
 *
 * @param root_dir Writable `mkdtemp` template. Receives the created directory path. Must not be
 *                 `NULL`.
 * @return `root_dir` on success, or `NULL` after recording a test-plumbing failure.
 */
const char* init_fixture_dir(char root_dir[static 1]);

/**
 * @brief Removes a fixture root and everything below it.
 *
 * Walking the tree keeps cleanup complete without a per-test inventory of the paths a fixture
 * writes or the code under test creates, and it needs no caller to classify a path as a file or a
 * directory. Each removal is checked, so a fixture a test leaves behind fails that test rather than
 * accumulating under `/tmp`.
 *
 * @param root_dir Fixture root directory to remove. Must not be `NULL`.
 */
void remove_fixture_tree(const char* root_dir);

/**
 * @brief Writes one file relative to a fixture root, creating parent directories as needed.
 *
 * @param root_dir      Fixture root directory. Must not be `NULL`.
 * @param relative_path Relative fixture path below `root_dir`. Must not be `NULL`.
 * @param contents      Terminated text to write. Must not be `NULL`.
 * @return `0` on success, or `-1` on test-plumbing failure.
 */
int write_fixture_file(const char* root_dir, const char* relative_path, const char* contents);

/**
 * @brief Changes into a directory, saving the working directory to return to.
 *
 * The saved directory is held open rather than recorded as a path, so no path length bounds it.
 *
 * @param dir              Directory to change into. Must not be `NULL`.
 * @param saved_dir_fd_out Receives a descriptor of the previous working directory on success, for
 *                         `working_dir_leave`. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with the working
 *         directory unchanged.
 */
int working_dir_enter(const char* dir, int* saved_dir_fd_out);

/**
 * @brief Returns to the working directory `working_dir_enter` saved, and closes its descriptor.
 *
 * @param saved_dir_fd Descriptor `working_dir_enter` wrote. Closed on return.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
int working_dir_leave(int saved_dir_fd);

/** A standard stream redirected by `capture_begin` or `capture_begin_unwritable`. */
struct StreamCapture {
  /** Redirected stream, `stdout` or `stderr`. */
  FILE* stream;

  /** Duplicate of the stream's original descriptor, which `capture_end` restores and closes. */
  int saved_fd;

  /** Anonymous file that receives the stream, or `NULL` when the stream is unwritable. */
  FILE* capture;
};

/**
 * @brief Redirects a standard stream into an anonymous file until `capture_end`.
 *
 * Flushes the stream first, so only text written after the call is captured.
 *
 * @param stream      Stream to redirect, `stdout` or `stderr`. Must not be `NULL`.
 * @param capture_out Receives the redirection state on success. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with the stream left
 *         unredirected.
 */
int capture_begin(FILE* stream, struct StreamCapture* capture_out);

/**
 * @brief Redirects a standard stream to a read-only descriptor until `capture_end`.
 *
 * Every write that reaches the descriptor fails with `EBADF`, which makes a stream write failure
 * deterministic. `capture_end` discards the text the failed writes leave buffered, so it never
 * reaches the restored stream.
 *
 * @param stream      Stream to redirect, `stdout` or `stderr`. Must not be `NULL`.
 * @param capture_out Receives the redirection state on success. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure, with the stream left
 *         unredirected.
 */
int capture_begin_unwritable(FILE* stream, struct StreamCapture* capture_out);

/**
 * @brief Restores a redirected stream and reads what it captured.
 *
 * Flushes the stream into the capture, restores the original descriptor, and clears the stream's
 * error indicator, which a write-failure path under test may have set.
 *
 * @param capture      State `capture_begin` or `capture_begin_unwritable` wrote. Must not be
 *                     `NULL`.
 * @param text_out     Buffer that receives the terminated captured text. Unused, and may be `NULL`,
 *                     for a capture `capture_begin_unwritable` began.
 * @param text_out_len Size of `text_out` in bytes. Must be non-zero when `text_out` is used.
 * @return `0` on success, or `-1` after recording a test-plumbing failure. The stream is restored
 *         either way.
 */
int capture_end(struct StreamCapture* capture, char* text_out, size_t text_out_len);

/**
 * @brief Reads an anonymous capture stream.
 *
 * @param capture      Stream to rewind and read. Must not be `NULL`.
 * @param text_out     Buffer that receives terminated captured text. Must not be `NULL`.
 * @param text_out_len Size of `text_out` in bytes. Must be non-zero.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
int read_capture(FILE* capture, char* text_out, size_t text_out_len);

#endif
