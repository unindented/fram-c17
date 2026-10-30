#ifndef FRAM_CMD_BUILD_H
#define FRAM_CMD_BUILD_H

#include <stdbool.h>
#include <stddef.h>

#include "app/exit_code.h"
#include "shared/arena.h"

struct JobArenas;
struct Log;
struct Os;

/** Caller-selected knobs for one build invocation. */
struct BuildOptions {
  /**
   * Worker threads used for parallel probing, for generating derivatives, and for rendering album
   * pages. `0` means detect the CPU count. The build runs at most four per online CPU, and a phase
   * runs at most one per job.
   */
  ptrdiff_t worker_count;

  /** Whether to print phase progress and status messages to `stderr`. */
  bool is_verbose;

  /**
   * Whether to run every phase but change nothing under `output_dir`, printing to `stdout` what the
   * build would write, keep, and remove.
   */
  bool is_dry_run;

  /** Whether to ignore the build cache for this build, then write it afresh. */
  bool is_forced;
};

/**
 * @brief Runs the `build` command for the `fram.toml` project in the current directory.
 *
 * This is a thin boundary over `cmd_build_execute`. It resolves the worker count, reserves the
 * build's memory in one block, the log and a perm and a scratch for the main thread and for each
 * worker, for fewer workers when an address-space limit refuses the block, runs
 * `cmd_build_execute`, and releases the block after the build. On failure it prints the build's log
 * to `stderr` exactly once, every error on its own line. Progress lines emitted under verbose mode
 * are status, not diagnostics. If the build runs out of memory, the program's `os_oom` prints the
 * log instead.
 *
 * @param options Build knobs such as worker count and verbosity. Must not be `NULL`.
 * @param os      Host the build reads, writes, and prints its status lines and log on. Must not be
 *                `NULL`.
 * @return `EXIT_CODE_OK` on a complete build, or `EXIT_CODE_FAILURE` on any error.
 */
enum ExitCode cmd_build_run(const struct BuildOptions* options, struct Os* os)
    __attribute__((nonnull(1, 2)));

/**
 * @brief Runs the build pipeline, collecting every error into a log.
 *
 * Performs no error printing itself so callers can inspect the log directly. `cmd_build_run` is the
 * boundary that prints it. Until the first write, the build keeps going after an error wherever the
 * next phase still has something to check, so one run reports every bad config key and every
 * path-limit failure in the album skeleton, or, once the skeleton is planned, every bad config key,
 * every source that cannot be probed, and every output collision, in that order, sources in walk
 * order. A skeleton error stops the build before the probe, because the later phases read the
 * skeleton. It stops before writing anything if any error was recorded. A dry run runs the same
 * phases, changes nothing under `output_dir`, and prints to `stdout` what it would have written,
 * kept, and removed.
 *
 * The main thread's data that outlives a phase, from the configuration and the source paths to the
 * manifest, goes in `perm`, and its temporaries and a parallel phase's job logs in `scratch`. Each
 * worker allocates only from its own `worker_arenas` entry. A build whose data outgrows an arena
 * ends in `os_oom`.
 *
 * @param options       Build knobs. Its worker count must be positive. Must not be `NULL`.
 * @param worker_arenas One perm and one scratch per worker, `options->worker_count` of them. Must
 *                      not be `NULL`.
 * @param os            Host every phase reads from and writes to, and status lines print on. Must
 *                      not be `NULL`.
 * @param perm          The main thread's perm. The build uses it up, and nothing it allocates
 *                      outlives the call.
 * @param scratch       The main thread's scratch.
 * @param log           Log that receives every error. Its arena must not overlap the other arenas,
 *                      because the phases copy arenas by value while they log. Left empty on
 *                      success. Must not be `NULL`.
 * @return `0` on a complete build, or `-1` on any error, with every error in `log`.
 */
int cmd_build_execute(const struct BuildOptions* options,
                      struct JobArenas* worker_arenas,
                      struct Os* os,
                      struct Arena perm,
                      struct Arena scratch,
                      struct Log* log) __attribute__((nonnull(1, 2, 3, 6)));

#endif
