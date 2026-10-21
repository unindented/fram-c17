#include "runtime/pool.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#include "shared/arena.h"

/** Shared worker-pool state for one `pool_run` call. */
struct PoolState {
  /** Total number of job indexes available. */
  ptrdiff_t job_count;

  /** User callback invoked for each claimed index. */
  PoolJobFn job_fn;

  /** User data passed through to `job_fn`. */
  void* userdata;

  /** Next job index to claim atomically. */
  atomic_ptrdiff_t index_next;

  /**
   * Whether any job callback failed. Workers deliberately do not observe this, so every job runs
   * and the run collects every error.
   */
  atomic_bool has_failed;
};

/** One worker thread and the identity its jobs see. */
struct PoolWorker {
  /** Thread running `pool_run_worker`. Written by `pthread_create`. */
  pthread_t thread;

  /** Shared state of the run. */
  struct PoolState* state;

  /** Index passed to every job this worker runs. */
  ptrdiff_t index;
};

/**
 * @brief Claims job indexes until none remain, running the job callback for each.
 *
 * Serves as the worker thread entry point. The worker claims indexes atomically and records any job
 * failure in the shared state.
 *
 * @param arg Pointer to this worker's `struct PoolWorker`. Must not be `NULL`.
 * @return `NULL` always. Results are reported through the shared state.
 */
static void* pool_run_worker(void* arg) __attribute__((nonnull(1)));

ptrdiff_t pool_resolve_worker_count(void) {
  const long n = sysconf(_SC_NPROCESSORS_ONLN);
  // `sysconf` returns `-1` when the count is indeterminate.
  return n < 1 ? 1 : (ptrdiff_t)n;
}

int pool_run(ptrdiff_t job_count,
             ptrdiff_t worker_count,
             PoolJobFn job_fn,
             void* userdata,
             struct Arena scratch) {
  assert(job_count >= 0);
  if (job_count == 0) {
    return 0;
  }
  if (worker_count <= 0) {
    worker_count = 1;
  }
  if (worker_count > job_count) {
    worker_count = job_count;
  }

  struct PoolWorker* workers = arena_new(&scratch, worker_count, struct PoolWorker);
  struct PoolState state = {.job_count = job_count, .job_fn = job_fn, .userdata = userdata};
  // Initialize both atomics explicitly, before any worker exists. The initializer above zeroes
  // them, but a zeroed object is not necessarily an initialized atomic, because the implementation
  // may need to set up an embedded lock. Operating on an uninitialized atomic is undefined.
  atomic_init(&state.index_next, 0);
  atomic_init(&state.has_failed, false);

  ptrdiff_t started = 0;
  int create_error = 0;
  // A failed `pthread_create` is not fatal. Workers claim indexes dynamically, so those that did
  // start still run every remaining job. Only a pool of zero workers fails the run, and its error
  // is the one the first `pthread_create` returned, since no later one is attempted.
  for (; started < worker_count; started++) {
    workers[started].state = &state;
    workers[started].index = started;
    create_error =
        pthread_create(&workers[started].thread, NULL, pool_run_worker, &workers[started]);
    if (create_error != 0) {
      break;
    }
  }
  // This function must join every started worker before returning, because `state` lives on this
  // frame and `workers` in this call's copy of the scratch arena.
  for (ptrdiff_t i = 0; i < started; i++) {
    (void)pthread_join(workers[i].thread, NULL);
  }

  if (started == 0) {
    errno = create_error;
    return -1;
  }
  return atomic_load(&state.has_failed) ? -1 : 0;
}

static void* pool_run_worker(void* arg) {
  const struct PoolWorker* worker = arg;
  struct PoolState* state = worker->state;
  // Relaxed ordering is sufficient for both atomics. `fetch_add` hands out each index exactly once
  // whatever the ordering, and nothing publishes data through `index_next`. `has_failed` needs no
  // release either. `pool_run` joins every worker before loading it, and `pthread_join` is the
  // happens-before edge that publishes each job's writes to the main thread.
  for (;;) {
    const ptrdiff_t index = atomic_fetch_add_explicit(&state->index_next, 1, memory_order_relaxed);
    if (index >= state->job_count) {
      break;
    }
    if (state->job_fn(index, worker->index, state->userdata) != 0) {
      atomic_store_explicit(&state->has_failed, true, memory_order_relaxed);
    }
  }
  return NULL;
}
