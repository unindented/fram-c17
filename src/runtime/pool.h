#ifndef FRAM_POOL_H
#define FRAM_POOL_H

#include <stddef.h>

/**
 * @brief Callback run once per job index by `pool_run`.
 *
 * @param index    Zero-based job index to process.
 * @param userdata Opaque pointer passed through from `pool_run`.
 * @return `0` on success, or `-1` to mark the overall run as failed.
 */
typedef int (*PoolJobFn)(size_t index, void* userdata);

/**
 * @brief Resolves the worker count to use when the caller has not chosen one.
 *
 * Queries the host's online CPU count, so resolve it once and reuse it rather than calling per run.
 *
 * @return Number of online processors, or `1` when the count is unavailable.
 */
size_t pool_resolve_worker_count(void);

/**
 * @brief Runs `job_fn` once for each job index across a bounded set of worker threads.
 *
 * Every job index in `[0, job_count)` runs even if some jobs fail, so the run collects every error.
 * The run clamps the worker count to at least `1` and at most `job_count`. The run may create fewer
 * workers than requested. The run still completes every job across whichever workers started
 * successfully.
 *
 * `pool_run` joins every worker it started before it returns. Once it returns, the caller may read
 * whatever the jobs wrote with no further synchronization. `job.c` reads its error slots
 * unsynchronized after the run, and `cmd_build.c` reasons from it about `stderr` interleaving.
 * Detaching the workers, or returning before the join, would make those claims false while this
 * contract still looked satisfied.
 *
 * @param job_count    Number of job indexes to process.
 * @param worker_count Requested worker threads. `0` is treated as `1`.
 * @param job_fn       Callback invoked for each index, concurrently from up to `worker_count`
 *                     threads, so it must be thread-safe. Must not be `NULL`.
 * @param userdata     Opaque pointer forwarded to every `job_fn` call. Every worker receives the
 *                     same pointer, so any mutation through it is the caller's to make safe. The
 *                     parallel phases partition by index, one result slot per job.
 * @return `0` when every job succeeded, or `-1` if any job failed or no worker could be created.
 */
int pool_run(size_t job_count, size_t worker_count, PoolJobFn job_fn, void* userdata)
    __attribute__((nonnull(3)));

#endif
