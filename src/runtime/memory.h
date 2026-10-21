#ifndef FRAM_MEMORY_H
#define FRAM_MEMORY_H

#include "shared/str.h"

struct Log;
struct Os;

/**
 * @brief Names the log that the out-of-memory report prints first.
 *
 * A command sets its own log before its work starts and clears it once the log is printed. The
 * report reads the log from whichever thread runs out of memory. That is safe because the main
 * thread does not write its log while worker threads run: a parallel phase appends its jobs' logs
 * only after every worker is joined.
 *
 * @param log The command's log, or `NULL` to clear it.
 */
void memory_oom_set_log(const struct Log* log);

/**
 * @brief Names the file the calling thread works on, and the log its job writes.
 *
 * Each thread has its own value, so a worker can name its job without a lock. A job sets it when it
 * starts. A value from an earlier job stays until the next one replaces it.
 *
 * @param file_path File the thread works on, or empty when it works on no single file. Must stay
 *                  valid until the next call on this thread.
 * @param log       Log of the thread's current job, or `NULL` when the thread writes the command's
 *                  log itself.
 */
void memory_oom_set_job(struct Str file_path, const struct Log* log);

/**
 * @brief Prints the out-of-memory report to standard error.
 *
 * Prints the command's log, then the calling thread's job log, then `fram: out of memory`, followed
 * by ` (in '<file>')` when the thread named a file. It allocates nothing, so it works with every
 * arena full. The program's `os_oom` calls it.
 *
 * @param os Host to print on. Must not be `NULL`.
 */
void memory_oom_report(struct Os* os) __attribute__((nonnull(1)));

#endif
