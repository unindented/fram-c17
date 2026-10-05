#ifndef FRAM_CMD_BUILD_H
#define FRAM_CMD_BUILD_H

#include <stdbool.h>
#include <stddef.h>

#include "app/exit_code.h"

struct StringBuffer;

/** Caller-selected knobs for one build invocation. */
struct BuildOptions {
  /**
   * Worker threads used for parallel probing, for generating derivatives, and for rendering album
   * pages. `0` means detect the CPU count.
   */
  size_t worker_count;

  /** Whether to print phase progress and status messages to `stderr`. */
  bool is_verbose;
};

/**
 * @brief Runs the `build` command for the `fram.toml` project in the current directory.
 *
 * This is a thin boundary over `cmd_build_execute`. On failure it prints the single collected
 * diagnostic to `stderr` exactly once. Progress lines emitted under verbose mode are status, not
 * diagnostics.
 *
 * @param options Build knobs such as worker count and verbosity. Must not be `NULL`.
 * @return `EXIT_CODE_OK` on a complete build, or `EXIT_CODE_FAILURE` on any error.
 */
enum ExitCode cmd_build_run(const struct BuildOptions* options) __attribute__((nonnull(1)));

/**
 * @brief Runs the build pipeline, collecting the failure diagnostic into a growable buffer.
 *
 * Performs no error printing itself so callers can inspect the diagnostic directly. `cmd_build_run`
 * is the boundary that prints it. A single failing phase appends one message. Multiple media probe,
 * derivative, or page render failures append one line each, so nothing is truncated.
 *
 * @param options   Build knobs such as worker count and verbosity. Must not be `NULL`.
 * @param error_out Growable buffer that receives the diagnostic. Must be initialized. Left empty on
 *                  success. Must not be `NULL`.
 * @return `0` on a complete build, or `-1` on any error, with a message appended to `error_out`.
 */
int cmd_build_execute(const struct BuildOptions* options, struct StringBuffer* error_out)
    __attribute__((nonnull(1, 2)));

#endif
