// `pthread_setattr_default_np` is a GNU extension, so glibc needs `_GNU_SOURCE` to declare it.
#define _GNU_SOURCE

#include <acutest.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/pool.h"
#include "shared/arena.h"
#include "test_support.h"

/** Number of jobs `test_worker_index_is_stable_and_bounded` runs. */
enum { WORKER_JOB_COUNT = 64 };

/**
 * Per-job record of which worker ran each job and on which thread. Each job writes only its own
 * slot, and `pool_run` joins every worker before the test reads them.
 */
struct WorkerRecord {
  /** Worker index each job received, indexed by job. */
  ptrdiff_t workers[WORKER_JOB_COUNT];

  /** Thread each job ran on, indexed by job. */
  pthread_t threads[WORKER_JOB_COUNT];
};

/**
 * These counters are shared by pool callback tests. Every test loads `count` with
 * `memory_order_relaxed` from the main thread and then asserts an exact total. That is correct only
 * because `pool_run` joins every worker before returning, and `pthread_join` publishes each
 * worker's writes. The relaxed load reads already-synchronized memory. Do not "fix" this to
 * `seq_cst`, and do not copy the relaxed pattern to a counter read without an intervening join.
 */
struct PoolCounter {
  /** Number of callback invocations observed. */
  atomic_ptrdiff_t count;

  /** Index that `fail_one_job` should reject. */
  ptrdiff_t index_fail;
};

/**
 * @brief Counts a job invocation without failing.
 *
 * @param index    Zero-based job index. Unused.
 * @param worker   Index of the worker running the job. Unused.
 * @param userdata Pointer to the shared `PoolCounter`.
 * @return `0` always.
 */
static int count_job(ptrdiff_t index, ptrdiff_t worker, void* userdata) {
  (void)index;
  (void)worker;
  struct PoolCounter* counter = userdata;
  atomic_fetch_add_explicit(&counter->count, 1, memory_order_relaxed);
  return 0;
}

/**
 * @brief Counts a job invocation and fails on the configured index.
 *
 * @param index    Zero-based job index.
 * @param worker   Index of the worker running the job. Unused.
 * @param userdata Pointer to the shared `PoolCounter`.
 * @return `-1` for the configured failing index, or `0` otherwise.
 */
static int fail_one_job(ptrdiff_t index, ptrdiff_t worker, void* userdata) {
  (void)worker;
  struct PoolCounter* counter = userdata;
  atomic_fetch_add_explicit(&counter->count, 1, memory_order_relaxed);
  return index == counter->index_fail ? -1 : 0;
}

/**
 * @brief Records the worker index and thread a job ran on, in the job's own slot.
 *
 * @param index    Zero-based job index, below `WORKER_JOB_COUNT`.
 * @param worker   Index of the worker running the job.
 * @param userdata Pointer to the shared `WorkerRecord`.
 * @return `0` always.
 */
static int record_worker_job(ptrdiff_t index, ptrdiff_t worker, void* userdata) {
  struct WorkerRecord* record = userdata;
  record->workers[index] = worker;
  record->threads[index] = pthread_self();
  return 0;
}

// glibc lets a test change the default thread attributes, so only there can a test make
// `pthread_create` fail on demand.
#ifdef __GLIBC__
/**
 * @brief Returns at once. The thread `thread_create_fail_begin` tries to start runs it.
 *
 * @param arg Unused.
 * @return `NULL`.
 */
static void* thread_return(void* arg) {
  (void)arg;
  return NULL;
}

/**
 * @brief Makes every later `pthread_create` with default attributes fail.
 *
 * Raises the default stack size past any address space, so no thread stack can be mapped. The
 * caller restores the defaults it saved with `pthread_setattr_default_np`.
 *
 * @return The error `pthread_create` now returns, or `0` when the defaults could not be changed or
 *         a thread still started.
 */
static int thread_create_fail_begin(void) {
  pthread_attr_t failing;
  if (pthread_getattr_default_np(&failing) != 0) {
    return 0;
  }
  const bool is_set = pthread_attr_setstacksize(&failing, SIZE_MAX / 4) == 0 &&
                      pthread_setattr_default_np(&failing) == 0;
  (void)pthread_attr_destroy(&failing);
  if (!is_set) {
    return 0;
  }
  pthread_t thread;
  const int error_number = pthread_create(&thread, NULL, thread_return, NULL);
  if (error_number == 0) {
    (void)pthread_join(thread, NULL);
  }
  return error_number;
}
#endif

// The default worker count is always at least one, so a pool can always make progress.
static void test_default_worker_count_is_at_least_one(void) {
  TEST_CHECK(pool_resolve_worker_count() >= 1);
}

// A successful run dispatches every job index.
static void test_runs_all_jobs(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 0;
  TEST_CHECK(pool_run(32, 4, count_job, &counter, test_arena()) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 32);
}

// Zero jobs complete without starting any workers.
static void test_zero_jobs_starts_no_workers(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 0;
  TEST_CHECK(pool_run(0, 4, count_job, &counter, test_arena()) == 0);
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
  TEST_CHECK(pool_run(3, 0, count_job, &counter, test_arena()) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 3);
}

// `pool_run` clamps a worker count above the job count, and every job index still runs. The first
// call cannot see the clamp, since at most `job_count` threads run a job either way. The second
// call is what makes the clamp observable. The clamp happens before `pool_run` allocates the worker
// array, so an absurd request costs two threads and succeeds. Without the clamp, `pool_run` tries
// to allocate one worker record per requested worker from its scratch arena, and the run ends in
// `os_oom`, which fails the test.
static void test_worker_count_clamped_to_job_count(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 0;
  TEST_CHECK(pool_run(2, 100, count_job, &counter, test_arena()) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 2);

  atomic_store_explicit(&counter.count, 0, memory_order_relaxed);
  TEST_CHECK(pool_run(2, PTRDIFF_MAX / 1024, count_job, &counter, test_arena()) == 0);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 2);
}

// Every worker index is below the requested worker count, and one index always belongs to one
// thread, so per-worker state indexed by it is never touched by two threads at once.
static void test_worker_index_is_stable_and_bounded(void) {
  enum { WORKER_COUNT = 4 };
  struct WorkerRecord record = {0};
  TEST_CHECK(pool_run(WORKER_JOB_COUNT, WORKER_COUNT, record_worker_job, &record, test_arena()) ==
             0);
  for (ptrdiff_t i = 0; i < WORKER_JOB_COUNT; i++) {
    TEST_CHECK(record.workers[i] >= 0 && record.workers[i] < WORKER_COUNT);
    for (ptrdiff_t j = i + 1; j < WORKER_JOB_COUNT; j++) {
      if (record.workers[i] == record.workers[j]) {
        TEST_CHECK(pthread_equal(record.threads[i], record.threads[j]) != 0);
      }
    }
  }
}

// A worker count above the job count is clamped before any worker index is handed out, so two jobs
// with a hundred requested workers see indexes below two.
static void test_worker_index_respects_job_count_clamp(void) {
  struct WorkerRecord record = {0};
  TEST_CHECK(pool_run(2, 100, record_worker_job, &record, test_arena()) == 0);
  TEST_CHECK(record.workers[0] < 2 && record.workers[1] < 2);
}

// One failing job fails the whole run, and every remaining index still runs. Workers deliberately
// never observe `has_failed` (`src/runtime/pool.c`), so the count is the full job count, not
// however many workers claimed before the failure.
static void test_propagates_job_failure(void) {
  struct PoolCounter counter;
  atomic_init(&counter.count, 0);
  counter.index_fail = 7;
  TEST_CHECK(pool_run(16, 3, fail_one_job, &counter, test_arena()) == -1);
  TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 16);
}

#ifdef __GLIBC__
// A pool that can start no worker runs no job and fails with the error of its first
// `pthread_create` in `errno`, the only account of the failure its caller gets.
static void test_reports_the_thread_creation_error(void) {
  pthread_attr_t saved;
  TEST_ASSERT(pthread_getattr_default_np(&saved) == 0);
  const int error_number = thread_create_fail_begin();
  if (TEST_CHECK(error_number != 0)) {
    struct PoolCounter counter;
    atomic_init(&counter.count, 0);
    counter.index_fail = 0;
    errno = 0;
    TEST_CHECK(pool_run(4, 2, count_job, &counter, test_arena()) == -1);
    TEST_CHECK(errno == error_number);
    TEST_CHECK(atomic_load_explicit(&counter.count, memory_order_relaxed) == 0);
  }
  (void)pthread_setattr_default_np(&saved);
  (void)pthread_attr_destroy(&saved);
}
#endif

TEST_LIST = {
    {"default worker count is at least one", test_default_worker_count_is_at_least_one},
    {"runs all jobs", test_runs_all_jobs},
    {"zero jobs starts no workers", test_zero_jobs_starts_no_workers},
    {"zero workers promoted to one", test_zero_workers_promoted_to_one},
    {"worker count clamped to job count", test_worker_count_clamped_to_job_count},
    {"worker index is stable and bounded", test_worker_index_is_stable_and_bounded},
    {"worker index respects job count clamp", test_worker_index_respects_job_count_clamp},
    {"propagates job failure", test_propagates_job_failure},
#ifdef __GLIBC__
    {"reports the thread creation error", test_reports_the_thread_creation_error},
#endif
    {NULL, NULL},
};
