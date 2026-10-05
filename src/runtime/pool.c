#include "runtime/pool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

/** Shared worker-pool state for one `pool_run` call. */
struct PoolState {
  /** Total number of job indexes available. */
  size_t job_count;

  /** User callback invoked for each claimed index. */
  PoolJobFn job_fn;

  /** User data passed through to `job_fn`. */
  void* userdata;

  /** Next job index to claim atomically. */
  atomic_size_t index_next;

  /**
   * Whether any job callback failed. Workers deliberately do not observe this, so every job runs
   * and the run collects every error.
   */
  atomic_bool has_failed;
};

/**
 * @brief Claims job indexes until none remain, running the job callback for each.
 *
 * Serves as the worker thread entry point. The worker claims indexes atomically and records any job
 * failure in the shared state.
 *
 * @param arg Pointer to the shared `struct PoolState` for this run. Must not be `NULL`.
 * @return `NULL` always. Results are reported through the shared state.
 */
static void* pool_run_worker(void* arg) __attribute__((nonnull(1)));

size_t pool_resolve_worker_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  // `sysconf` returns `-1` when the count is indeterminate, so reject anything below `1` before the
  // cast. A negative `long` converts to a huge `size_t`.
  if (n < 1) {
    return 1;
  }
  return (size_t)n;
}

int pool_run(size_t job_count, size_t worker_count, PoolJobFn job_fn, void* userdata) {
  if (job_count == 0) {
    return 0;
  }
  if (worker_count == 0) {
    worker_count = 1;
  }
  if (worker_count > job_count) {
    worker_count = job_count;
  }

  // `calloc` fails on product overflow rather than wrapping, so no separate check is needed.
  pthread_t* threads = calloc(worker_count, sizeof(*threads));
  if (threads == NULL) {
    return -1;
  }
  struct PoolState state = {.job_count = job_count, .job_fn = job_fn, .userdata = userdata};
  // Initialize both atomics explicitly, before any worker exists. The initializer above zeroes
  // them, but a zeroed object is not necessarily an initialized atomic, because the implementation
  // may need to set up an embedded lock. Operating on an uninitialized atomic is undefined.
  atomic_init(&state.index_next, 0);
  atomic_init(&state.has_failed, false);

  size_t started = 0;
  // A failed `pthread_create` is not fatal. Workers claim indexes dynamically, so those that did
  // start still run every remaining job. Only a pool of zero workers fails the run.
  for (; started < worker_count; started++) {
    if (pthread_create(&threads[started], NULL, pool_run_worker, &state) != 0) {
      break;
    }
  }
  // This function must join every started worker before returning, because `state` lives on this
  // frame.
  for (size_t i = 0; i < started; i++) {
    (void)pthread_join(threads[i], NULL);
  }

  free(threads);
  if (started == 0) {
    return -1;
  }
  return atomic_load(&state.has_failed) ? -1 : 0;
}

static void* pool_run_worker(void* arg) {
  struct PoolState* state = arg;
  // Relaxed ordering is sufficient for both atomics. `fetch_add` hands out each index exactly once
  // whatever the ordering, and nothing publishes data through `index_next`. `has_failed` needs no
  // release either. `pool_run` joins every worker before loading it, and `pthread_join` is the
  // happens-before edge that publishes each job's writes to the main thread.
  for (;;) {
    const size_t index = atomic_fetch_add_explicit(&state->index_next, 1, memory_order_relaxed);
    if (index >= state->job_count) {
      break;
    }
    if (state->job_fn(index, state->userdata) != 0) {
      atomic_store_explicit(&state->has_failed, true, memory_order_relaxed);
    }
  }
  return NULL;
}
