#ifndef FRAM_POOL_H
#define FRAM_POOL_H

#include <stddef.h>

#include "shared/arena.h"

/**
 * @brief Callback run once per job index by `pool_run`.
 *
 * @param index    Zero-based job index to process.
 * @param worker   Zero-based index of the worker running the job, below the clamped worker count.
 *                 Every job one worker runs sees the same value, and no two workers share one, so
 *                 the job can use per-worker state without locking.
 * @param userdata Opaque pointer passed through from `pool_run`.
 * @return `0` on success, or `-1` to mark the overall run as failed.
 */
typedef int (*PoolJobFn)(ptrdiff_t index, ptrdiff_t worker, void* userdata);

/**
 * @brief Resolves the worker count to use when the caller has not chosen one.
 *
 * Queries the host's online CPU count, so resolve it once and reuse it rather than calling per run.
 *
 * @return Number of online processors, or `1` when the count is unavailable.
 */
ptrdiff_t pool_resolve_worker_count(void);

/**
 * @brief Runs `job_fn` once for each job index across a bounded set of worker threads.
 *
 * Every job index in `[0, job_count)` runs even if some jobs fail, so the run collects every error.
 * Each worker takes the next index not yet taken, so the jobs one worker runs come in index order.
 * The run clamps the worker count to at least `1` and at most `job_count`. The run may create fewer
 * workers than requested. The run still completes every job across whichever workers started
 * successfully. Worker indexes stay below the clamped count, so a caller that sized per-worker
 * state for `worker_count` workers covers every index.
 *
 * `pool_run` joins every worker it started before it returns. Once it returns, the caller may read
 * whatever the jobs wrote with no further synchronization. `job.c` reads its jobs' logs
 * unsynchronized after the run, and `cmd_build.c` reasons from it about `stderr` interleaving.
 * Detaching the workers, or returning before the join, would make those claims false while this
 * contract still looked satisfied.
 *
 * @param job_count    Number of job indexes to process. Must not be negative.
 * @param worker_count Requested worker threads. A count below `1` is treated as `1`.
 * @param job_fn       Callback invoked for each index, concurrently from up to `worker_count`
 *                     threads, so it must be thread-safe. Must not be `NULL`.
 * @param userdata     Opaque pointer forwarded to every `job_fn` call. Every worker receives the
 *                     same pointer, so any mutation through it is the caller's to make safe. The
 *                     parallel phases partition by index, one result slot per job, and by worker,
 *                     one set of arenas per worker.
 * @param scratch      Arena for the run's thread bookkeeping, released when the run returns.
 * @return `0` when every job succeeded, or `-1` if any job failed or no worker could be created.
 *         When no worker could be created, no job ran and `errno` holds the error the first
 *         `pthread_create` returned.
 */
int pool_run(ptrdiff_t job_count,
             ptrdiff_t worker_count,
             PoolJobFn job_fn,
             void* userdata,
             struct Arena scratch) __attribute__((nonnull(3)));

#endif
