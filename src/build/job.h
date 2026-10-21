#ifndef FRAM_JOB_H
#define FRAM_JOB_H

#include <stdbool.h>
#include <stddef.h>

#include "shared/arena.h"
#include "shared/str.h"

struct Log;
struct Os;

/**
 * The two arenas one pool worker owns for a whole build. No other worker allocates from them, so a
 * job needs no lock, and no arena is ever shared between two jobs running at once.
 */
struct JobArenas {
  /**
   * Region for what a job produces that must outlive it, such as output the main thread reads after
   * the phase. It persists across every job and phase the worker runs, so a job that commits here
   * leaves its data for the main thread and for later jobs to read. A job that does not commit
   * works in a copy, which leaves the region as it found it.
   */
  struct Arena perm;

  /**
   * Region for a job's temporaries. A job works in a copy, so nothing it allocates here survives.
   */
  struct Arena scratch;
};

/** Most failed jobs one parallel phase prints before summarizing the rest as a count. */
enum { JOB_ERROR_REPORT_COUNT_MAX = 20 };

/**
 * Bytes from which a job gives its source's pages back once it is done with it, as `malloc` returns
 * a large buffer, while a smaller source's pages stay for the worker's next job to reuse. It is
 * declared with the worker arenas whose reuse it is about, for every job that reads a source.
 */
enum { JOB_SOURCE_DISCARD_LEN_MIN = 16 * 1024 * 1024 };

/**
 * @brief Callback run once per job index by `job_run`.
 *
 * @param index    Zero-based job index to process.
 * @param arenas   Arenas of the worker running the job, which only this job uses while it runs.
 *                 Must not be `NULL`.
 * @param log      Log of this job alone, which receives every error the job finds. Its arena is
 *                 neither of `arenas`. Must not be `NULL`.
 * @param userdata Opaque pointer passed through from `job_run`. Every worker receives the same
 *                 pointer, so any mutation through it is the caller's to make safe.
 * @return `0` on success, or `-1` to mark the phase as failed.
 */
typedef int (*JobFn)(ptrdiff_t index, struct JobArenas* arenas, struct Log* log, void* userdata);

/**
 * @brief Runs one parallel phase, reports progress, and collects its diagnostics.
 *
 * Gives each job a log of its own, runs every index through the worker pool, and then appends the
 * failed jobs' logs to `log` in job order, so the report is the same however the jobs were
 * scheduled. A job whose messages repeat an earlier job's is counted but not printed again, because
 * one systematic failure, such as a template that cannot parse, fails every job alike. Each job may
 * name itself at the end of each of its lines, after `attribution`, so the comparison stops there:
 * `x (while rendering 'a')` repeats `x (while rendering 'b')`. At most `JOB_ERROR_REPORT_COUNT_MAX`
 * jobs are printed, followed by a count of the remaining failures. Every job's errors count in
 * `log->errors`, printed or not.
 *
 * @param job_count     Number of job indexes to run. Must not be negative. `0` runs nothing and
 *                      succeeds.
 * @param worker_count  Requested worker threads, as for `pool_run`. Must be positive.
 * @param worker_arenas One `JobArenas` per requested worker, `worker_count` of them. Each job gets
 *                      the entry for the worker running it. Must not be `NULL`.
 * @param job_fn        Callback invoked for each index, concurrently from several threads, so it
 *                      must be thread-safe. Must not be `NULL`.
 * @param userdata      Opaque pointer forwarded to every `job_fn` call.
 * @param phase_label   Phase name leading each progress line, and named when no worker can start or
 *                      the run fails without a diagnostic.
 * @param attribution   Text that starts the job's own name at the end of a line, such as
 *                      ` (while rendering '`, or empty when a job's lines carry none. A line
 *                      compares only up to its last `attribution`.
 * @param is_verbose    Whether each finished job prints a progress line to `stderr`.
 * @param os            Host the progress line is printed on, by every worker at once. Must not be
 *                      `NULL`.
 * @param scratch       Arena for the jobs' logs and the run's thread bookkeeping, released when the
 *                      run returns. One share of it per worker holds the logs of that worker's
 *                      jobs. Must not overlap any of `worker_arenas` or `log`'s arena.
 * @param log           Log that receives the collected errors. Must not be `NULL`.
 * @return `0` when every job succeeded, or `-1` when a job failed or the worker pool could not
 *         start.
 */
int job_run(ptrdiff_t job_count,
            ptrdiff_t worker_count,
            struct JobArenas* worker_arenas,
            JobFn job_fn,
            void* userdata,
            struct Str phase_label,
            struct Str attribution,
            bool is_verbose,
            struct Os* os,
            struct Arena scratch,
            struct Log* log) __attribute__((nonnull(3, 4, 9, 11)));

#endif
