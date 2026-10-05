#include <acutest.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/pool.h"

/**
 * These counters are shared by pool callback tests. Every test loads `count` with
 * `memory_order_relaxed` from the main thread and then asserts an exact total. That is correct only
 * because `pool_run` joins every worker before returning, and `pthread_join` publishes each
 * worker's writes. The relaxed load reads already-synchronized memory. Do not "fix" this to
 * `seq_cst`, and do not copy the relaxed pattern to a counter read without an intervening join.
 */
struct PoolCounter {
  /** Number of callback invocations observed. */
  atomic_size_t count;

  /** Index that `fail_one_job` should reject. */
  size_t index_fail;
};

/**
 * @brief Counts a job invocation without failing.
 *
 * @param index    Zero-based job index. Unused.
 * @param userdata Pointer to the shared `PoolCounter`.
 * @return `0` always.
 */
static int count_job(size_t index, void* userdata) {
  (void)index;
  struct PoolCounter* counter = userdata;
  atomic_fetch_add_explicit(&counter->count, 1, memory_order_relaxed);
  return 0;
}

/**
 * @brief Counts a job invocation and fails on the configured index.
 *
 * @param index    Zero-based job index.
 * @param userdata Pointer to the shared `PoolCounter`.
 * @return `-1` for the configured failing index, or `0` otherwise.
 */
static int fail_one_job(size_t index, void* userdata) {
  struct PoolCounter* counter = userdata;
  atomic_fetch_add_explicit(&counter->count, 1, memory_order_relaxed);
  return index == counter->index_fail ? -1 : 0;
}

// The default worker count is always at least one, so a pool can always make progress.
static void test_default_worker_count_is_at_least_one(void) {
  TEST_CHECK(pool_resolve_worker_count() >= 1);
}

// A successful run dispatches every job index.
static void test_runs_all_jobs(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 0;
  TEST_CHECK(pool_run(32, 4, count_job, &counter) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 32);
}

// Zero jobs complete without starting any workers.
static void test_zero_jobs_starts_no_workers(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 0;
  TEST_CHECK(pool_run(0, 4, count_job, &counter) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 0);
}

// `pool_run` promotes a zero worker count to one worker, which still runs every job. The `== 0`
// return pins the promotion. Without it `worker_count` stays 0, `pool_run` creates no worker, and
// `pool_run` reports `-1`. The promoted *value* is deliberately not asserted. Promoting to 2
// instead of 1 is behaviorally identical here. The only observable measure would be counting
// distinct `pthread_self()` values, which is racy: one worker can claim every index before a second
// one starts, so the check would pass for a mutant most of the time.
static void test_zero_workers_promoted_to_one(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 0;
  TEST_CHECK(pool_run(3, 0, count_job, &counter) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 3);
}

// `pool_run` clamps a worker count above the job count, and every job index still runs. The second
// call is what makes the clamp observable at all. The clamp happens before `pool_run` allocates the
// worker array, so an absurd request costs two threads and succeeds. Without the clamp, `pool_run`
// tries to allocate one thread handle per requested worker and the run fails. The first call cannot
// see the clamp, since at most `job_count` threads run a job either way.
static void test_worker_count_clamped_to_job_count(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 0;
  TEST_CHECK(pool_run(2, 100, count_job, &counter) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 2);

  atomic_store_explicit(&counter.count, 0, memory_order_relaxed);
  TEST_CHECK(pool_run(2, SIZE_MAX / 1024, count_job, &counter) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 2);
}

// One failing job fails the whole run, and every remaining index still runs. Workers deliberately
// never observe `has_failed` (`src/runtime/pool.c`), so the count is the full job count, not
// however many workers claimed before the failure.
static void test_propagates_job_failure(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 7;
  TEST_CHECK(pool_run(16, 3, fail_one_job, &counter) == -1);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 16);
}

TEST_LIST = {
    {"default worker count is at least one", test_default_worker_count_is_at_least_one},
    {"runs all jobs", test_runs_all_jobs},
    {"zero jobs starts no workers", test_zero_jobs_starts_no_workers},
    {"zero workers promoted to one", test_zero_workers_promoted_to_one},
    {"worker count clamped to job count", test_worker_count_clamped_to_job_count},
    {"propagates job failure", test_propagates_job_failure},
    {NULL, NULL},
};
